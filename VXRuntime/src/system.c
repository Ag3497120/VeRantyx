// System information and services that games look up with GetProcAddress: processor topology, the thread pool
// (work items, timers, waits), extended file information, toolhelp snapshots (none), process tokens, locale
// names, special folders, and the ntdll functions behind them.
#include "vx.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define STATUS_NOT_IMPLEMENTED 0xC0000002u
#define ERROR_INSUFFICIENT_BUFFER 122
#define INVALID_HANDLE ((HANDLE)(intptr_t)-1)

static int sysctl_int(const char *name, int fallback) {
    int v = 0; size_t n = sizeof v;
    return sysctlbyname(name, &v, &n, NULL, 0) == 0 && v > 0 ? v : fallback;
}
static int cpu_count(void) { return sysctl_int("hw.logicalcpu", (int)sysconf(_SC_NPROCESSORS_ONLN)); }

// ---- processor topology: one package, one NUMA node, a core per logical processor (Apple silicon has no SMT) ----
// SYSTEM_LOGICAL_PROCESSOR_INFORMATION (32 bytes): mask, relationship, then 16 bytes (core flags, node number,
// or a CACHE_DESCRIPTOR: level, associativity, line size, size, type).
static WINAPI BOOL GetLogicalProcessorInformation(uint8_t *buf, DWORD *len) {
    int n = cpu_count();
    DWORD need = (DWORD)(n + 2 + 2) * 32;
    if (!len) { vx_set_last_error(87); return 0; }
    if (!buf || *len < need) { *len = need; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memset(buf, 0, need);
    uint64_t all = n >= 64 ? ~0ull : (1ull << n) - 1;
    uint8_t *p = buf;
    for (int i = 0; i < n; i++, p += 32) { *(uint64_t *)p = 1ull << i; *(DWORD *)(p + 8) = 0; }   // RelationProcessorCore
    *(uint64_t *)p = all; *(DWORD *)(p + 8) = 3; p += 32;                                            // RelationProcessorPackage
    *(uint64_t *)p = all; *(DWORD *)(p + 8) = 1; p += 32;                                            // RelationNumaNode (node 0)
    // Caches: L1 data per core is represented once for all; L2 shared (RelationCache).
    *(uint64_t *)p = all; *(DWORD *)(p + 8) = 2; p[16] = 1; p[17] = 8; *(uint16_t *)(p + 18) = 128;
    *(DWORD *)(p + 20) = (DWORD)sysctl_int("hw.perflevel0.l1dcachesize", 65536); *(DWORD *)(p + 24) = 2; p += 32;   // CacheData
    *(uint64_t *)p = all; *(DWORD *)(p + 8) = 2; p[16] = 2; p[17] = 16; *(uint16_t *)(p + 18) = 128;
    *(DWORD *)(p + 20) = (DWORD)sysctl_int("hw.perflevel0.l2cachesize", 4 << 20); *(DWORD *)(p + 24) = 0;   // CacheUnified
    *len = need;
    return 1;
}
// SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX: relationship, size, then per kind: PROCESSOR_RELATIONSHIP (flags,
// efficiency class, 20 reserved, group count, GROUP_AFFINITY[] at 24), NUMA_NODE_RELATIONSHIP (node number,
// 20 reserved, GROUP_AFFINITY at 24), GROUP_RELATIONSHIP (max and active group counts, 20 reserved,
// PROCESSOR_GROUP_INFO at 24: max and active processor counts, 38 reserved, active mask).
enum { REL_CORE = 0, REL_NUMA = 1, REL_CACHE = 2, REL_PACKAGE = 3, REL_GROUP = 4, REL_ALL = 0xffff };
static WINAPI BOOL GetLogicalProcessorInformationEx(DWORD rel, uint8_t *buf, DWORD *len) {
    int n = cpu_count();
    uint64_t all = n >= 64 ? ~0ull : (1ull << n) - 1;
    uint8_t tmp[64 * 48 + 4 * 80];
    memset(tmp, 0, sizeof tmp);
    uint8_t *p = tmp;
    if (rel == REL_CORE || rel == REL_ALL)
        for (int i = 0; i < n && i < 64; i++, p += 48) {
            *(DWORD *)p = REL_CORE; *(DWORD *)(p + 4) = 48;
            p[9] = (uint8_t)(i < sysctl_int("hw.perflevel0.logicalcpu", n) ? 1 : 0);   // performance cores rank higher
            *(uint16_t *)(p + 30) = 1; *(uint64_t *)(p + 32) = 1ull << i;
        }
    if (rel == REL_PACKAGE || rel == REL_ALL) { *(DWORD *)p = REL_PACKAGE; *(DWORD *)(p + 4) = 48; *(uint16_t *)(p + 30) = 1; *(uint64_t *)(p + 32) = all; p += 48; }
    if (rel == REL_NUMA || rel == REL_ALL) { *(DWORD *)p = REL_NUMA; *(DWORD *)(p + 4) = 48; *(uint64_t *)(p + 32) = all; p += 48; }
    if (rel == REL_GROUP || rel == REL_ALL) {
        *(DWORD *)p = REL_GROUP; *(DWORD *)(p + 4) = 80;
        *(uint16_t *)(p + 8) = 1; *(uint16_t *)(p + 10) = 1;
        p[32] = (uint8_t)n; p[33] = (uint8_t)n; *(uint64_t *)(p + 72) = all;
        p += 80;
    }
    DWORD need = (DWORD)(p - tmp);
    if (!len) { vx_set_last_error(87); return 0; }
    if (!buf || *len < need) { *len = need; vx_set_last_error(ERROR_INSUFFICIENT_BUFFER); return 0; }
    memcpy(buf, tmp, need);
    *len = need;
    return 1;
}
static WINAPI DWORD GetActiveProcessorCount(uint16_t group) { (void)group; return (DWORD)cpu_count(); }
static WINAPI DWORD GetMaximumProcessorCount(uint16_t group) { (void)group; return (DWORD)cpu_count(); }
static WINAPI uint16_t GetActiveProcessorGroupCount(void) { return 1; }
static WINAPI uint16_t GetMaximumProcessorGroupCount(void) { return 1; }
static WINAPI DWORD SetThreadIdealProcessor(HANDLE t, DWORD p) { (void)t; (void)p; return 0; }
static WINAPI BOOL SetThreadIdealProcessorEx(HANDLE t, void *p, void *prev) { (void)t; (void)p; if (prev) memset(prev, 0, 4); return 1; }
static WINAPI BOOL GetThreadIdealProcessorEx(HANDLE t, void *p) { (void)t; memset(p, 0, 4); return 1; }
static WINAPI BOOL SetThreadGroupAffinity(HANDLE t, const void *a, void *prev) { (void)t; (void)a; if (prev) memset(prev, 0, 16); return 1; }
static WINAPI BOOL GetThreadGroupAffinity(HANDLE t, uint8_t *a) {
    (void)t; memset(a, 0, 16);
    int n = cpu_count();
    *(uint64_t *)a = n >= 64 ? ~0ull : (1ull << n) - 1;
    return 1;
}
static WINAPI void FlushProcessWriteBuffers(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// ---- the thread pool: workers are Windows threads; timers and waits run their callbacks on them too ---------------
typedef struct tp_item {
    int kind;                                    // 1 work, 2 timer, 3 wait
    void *callback, *context;
    int pending, running, closed;                // queued submissions, callbacks in progress
    // timers
    uint64_t due_ns; DWORD period; int armed;
    // waits
    HANDLE handle; uint64_t wait_deadline_ns; int has_deadline;
    struct tp_item *next_timer;
    // When closed: the event to set once its callbacks have completed (UnregisterWaitEx's and DeleteTimerQueueTimer's
    // CompletionEvent), and whether the context goes with it (theirs: a callback may still be running with it).
    HANDLE done_event; int free_context;
} tp_item;
typedef struct tp_job { tp_item *item; DWORD wait_result; struct tp_job *next; } tp_job;
static pthread_mutex_t g_tp_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tp_work = PTHREAD_COND_INITIALIZER, g_tp_done = PTHREAD_COND_INITIALIZER, g_tp_timers = PTHREAD_COND_INITIALIZER;
static tp_job *g_tp_head, *g_tp_tail;
static tp_item *g_tp_timer_list;
static int g_tp_started;
static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static void enqueue(tp_item *it, DWORD result) {               // with the lock held
    tp_job *j = calloc(1, sizeof *j);
    j->item = it; j->wait_result = result;
    if (g_tp_tail) g_tp_tail->next = j; else g_tp_head = j;
    g_tp_tail = j;
    it->pending++;
    pthread_cond_signal(&g_tp_work);
}
// A closed item whose callbacks have all completed: its completion event set, then it is freed.
static void tp_finish(tp_item *it) {
    if (it->done_event) {
        static BOOL (WINAPI *set)(HANDLE);
        if (!set) set = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "SetEvent");
        if (set) set(it->done_event);
    }
    if (it->free_context) free(it->context);
    free(it);
}
static void *tp_worker(void *arg) {
    (void)arg;
    vx_thread_attach();
    pthread_mutex_lock(&g_tp_lock);
    for (;;) {
        while (!g_tp_head) pthread_cond_wait(&g_tp_work, &g_tp_lock);
        tp_job *j = g_tp_head;
        g_tp_head = j->next;
        if (!g_tp_head) g_tp_tail = NULL;
        tp_item *it = j->item;
        it->pending--; it->running++;
        pthread_mutex_unlock(&g_tp_lock);
        // PTP_WORK_CALLBACK / PTP_TIMER_CALLBACK (instance, context, object); PTP_WAIT_CALLBACK adds the result.
        if (it->kind == 3) ((void (WINAPI *)(void *, void *, void *, DWORD))it->callback)(NULL, it->context, it, j->wait_result);
        else ((void (WINAPI *)(void *, void *, void *))it->callback)(NULL, it->context, it);
        free(j);
        pthread_mutex_lock(&g_tp_lock);
        it->running--;
        int finished = it->closed && !it->pending && !it->running;
        pthread_cond_broadcast(&g_tp_done);
        if (finished) { pthread_mutex_unlock(&g_tp_lock); tp_finish(it); pthread_mutex_lock(&g_tp_lock); }
    }
    return NULL;
}
// The timer thread fires due timers and wait deadlines; waits themselves poll their handles here too.
static DWORD (WINAPI *g_wait_one)(HANDLE, DWORD);
static void *tp_timer_thread(void *arg) {
    (void)arg;
    vx_thread_attach();
    pthread_mutex_lock(&g_tp_lock);
    for (;;) {
        uint64_t now = now_ns(), next = now + 1000000000ull;
        for (tp_item *t = g_tp_timer_list; t; t = t->next_timer) {
            if (!t->armed || t->closed) continue;
            if (t->kind == 3) {
                pthread_mutex_unlock(&g_tp_lock);
                DWORD r = g_wait_one ? g_wait_one(t->handle, 0) : 0x102;
                pthread_mutex_lock(&g_tp_lock);
                if (!t->armed) continue;
                if (r == 0) { t->armed = 0; enqueue(t, 0); continue; }
                if (t->has_deadline && now >= t->wait_deadline_ns) { t->armed = 0; enqueue(t, 0x102); continue; }   // WAIT_TIMEOUT
                if (next > now + 2000000ull) next = now + 2000000ull;   // handles are polled every 2 ms
                continue;
            }
            if (t->due_ns <= now) {
                enqueue(t, 0);
                if (t->period) t->due_ns = now + (uint64_t)t->period * 1000000ull; else t->armed = 0;
            }
            if (t->armed && t->due_ns < next) next = t->due_ns;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t wait = next > now ? next - now : 0;
        ts.tv_sec += (time_t)(wait / 1000000000ull); ts.tv_nsec += (long)(wait % 1000000000ull);
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_tp_timers, &g_tp_lock, &ts);
    }
    return NULL;
}
static void tp_start(void) {                                      // with the lock held
    if (g_tp_started) return;
    g_tp_started = 1;
    g_wait_one = (DWORD (WINAPI *)(HANDLE, DWORD))vx_find_export("kernel32.dll", "WaitForSingleObject");
    int n = cpu_count();
    for (int i = 0; i < n + 1; i++) {
        pthread_t t; pthread_attr_t a;
        pthread_attr_init(&a); pthread_attr_setstacksize(&a, 4 << 20); pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &a, i == n ? tp_timer_thread : tp_worker, NULL);
        pthread_attr_destroy(&a);
    }
}
static tp_item *tp_new(int kind, void *cb, void *ctx) {
    tp_item *it = calloc(1, sizeof *it);
    it->kind = kind; it->callback = cb; it->context = ctx;
    pthread_mutex_lock(&g_tp_lock);
    tp_start();
    if (kind != 1) { it->next_timer = g_tp_timer_list; g_tp_timer_list = it; }
    pthread_mutex_unlock(&g_tp_lock);
    return it;
}
static void tp_cancel_queued(tp_item *it) {                     // with the lock held: drop queued submissions
    for (tp_job **j = &g_tp_head; *j;) {
        if ((*j)->item == it) { tp_job *d = *j; *j = d->next; free(d); it->pending--; } else j = &(*j)->next;
    }
    g_tp_tail = NULL;
    for (tp_job *j = g_tp_head; j; j = j->next) g_tp_tail = j;
}
static void tp_wait_callbacks(tp_item *it, BOOL cancel) {
    pthread_mutex_lock(&g_tp_lock);
    if (cancel) tp_cancel_queued(it);
    while (it->pending || it->running) pthread_cond_wait(&g_tp_done, &g_tp_lock);
    pthread_mutex_unlock(&g_tp_lock);
}
// Closes an item: no more callbacks start (queued ones are dropped with `cancel`); `done` is set and the context freed
// (with `free_context`) once those running have returned - at once when none is.
static void tp_close_ex(tp_item *it, BOOL cancel, HANDLE done, int free_context) {
    pthread_mutex_lock(&g_tp_lock);
    it->armed = 0; it->closed = 1; it->done_event = done; it->free_context = free_context;
    if (cancel) tp_cancel_queued(it);
    for (tp_item **t = &g_tp_timer_list; *t; t = &(*t)->next_timer) if (*t == it) { *t = it->next_timer; break; }
    int idle = !it->pending && !it->running;
    pthread_mutex_unlock(&g_tp_lock);
    if (idle) tp_finish(it);
}
static void tp_close(tp_item *it) { tp_close_ex(it, 0, NULL, 0); }
static WINAPI void *CreateThreadpoolWork(void *cb, void *ctx, void *env) { (void)env; return tp_new(1, cb, ctx); }
static WINAPI void SubmitThreadpoolWork(tp_item *w) { pthread_mutex_lock(&g_tp_lock); enqueue(w, 0); pthread_mutex_unlock(&g_tp_lock); }
static WINAPI void WaitForThreadpoolWorkCallbacks(tp_item *w, BOOL cancel) { tp_wait_callbacks(w, cancel); }
static WINAPI void CloseThreadpoolWork(tp_item *w) { tp_close(w); }
// TrySubmitThreadpoolCallback: a one-off work item (PTP_SIMPLE_CALLBACK: instance, context).
typedef struct { void *cb, *ctx; } simple_job;
static WINAPI void run_simple(void *instance, void *ctx, void *work) {
    simple_job *s = ctx;
    ((void (WINAPI *)(void *, void *))s->cb)(instance, s->ctx);
    free(s);
    tp_close(work);
}
static WINAPI BOOL TrySubmitThreadpoolCallback(void *cb, void *ctx, void *env) {
    (void)env;
    simple_job *s = malloc(sizeof *s);
    s->cb = cb; s->ctx = ctx;
    SubmitThreadpoolWork(tp_new(1, (void *)run_simple, s));
    return 1;
}
static WINAPI void *CreateThreadpoolTimer(void *cb, void *ctx, void *env) { (void)env; return tp_new(2, cb, ctx); }
// The due time: a FILETIME, negative for "from now" in 100 ns units, else absolute (taken as now); NULL cancels.
static WINAPI void SetThreadpoolTimer(tp_item *t, const int64_t *due, DWORD period, DWORD window) {
    (void)window;
    pthread_mutex_lock(&g_tp_lock);
    if (!due) t->armed = 0;
    else {
        t->due_ns = now_ns() + (*due < 0 ? (uint64_t)(-*due) * 100ull : 0);
        t->period = period; t->armed = 1;
    }
    pthread_cond_signal(&g_tp_timers);
    pthread_mutex_unlock(&g_tp_lock);
}
static WINAPI BOOL IsThreadpoolTimerSet(tp_item *t) { return t->armed; }
static WINAPI void WaitForThreadpoolTimerCallbacks(tp_item *t, BOOL cancel) { if (cancel) SetThreadpoolTimer(t, NULL, 0, 0); tp_wait_callbacks(t, cancel); }
static WINAPI void CloseThreadpoolTimer(tp_item *t) { tp_close(t); }
static WINAPI void *CreateThreadpoolWait(void *cb, void *ctx, void *env) { (void)env; return tp_new(3, cb, ctx); }
static WINAPI void SetThreadpoolWait(tp_item *w, HANDLE h, const int64_t *timeout) {
    pthread_mutex_lock(&g_tp_lock);
    w->handle = h; w->armed = h != NULL;
    w->has_deadline = timeout != NULL;
    if (timeout) w->wait_deadline_ns = now_ns() + (*timeout < 0 ? (uint64_t)(-*timeout) * 100ull : 0);
    pthread_cond_signal(&g_tp_timers);
    pthread_mutex_unlock(&g_tp_lock);
}
static WINAPI void WaitForThreadpoolWaitCallbacks(tp_item *w, BOOL cancel) { if (cancel) SetThreadpoolWait(w, NULL, NULL); tp_wait_callbacks(w, cancel); }
static WINAPI void CloseThreadpoolWait(tp_item *w) { tp_close(w); }
static WINAPI void FreeLibraryWhenCallbackReturns(void *instance, void *module) { (void)instance; (void)module; }
static WINAPI void SetEventWhenCallbackReturns(void *instance, HANDLE e) {
    (void)instance;
    BOOL (WINAPI *set)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "SetEvent");
    if (set) set(e);
}
static WINAPI void *CreateThreadpool(void *reserved) { (void)reserved; static int pool; return &pool; }   // the one pool
static WINAPI void CloseThreadpool(void *pool) { (void)pool; }
static WINAPI void SetThreadpoolThreadMaximum(void *pool, DWORD n) { (void)pool; (void)n; }
static WINAPI BOOL SetThreadpoolThreadMinimum(void *pool, DWORD n) { (void)pool; (void)n; return 1; }

// Timer queues, registered waits and waitable timers, on the pool's timers and waits. WAITORTIMERCALLBACK takes
// (parameter, timed out or fired).
typedef struct { tp_item *item; void *callback, *param; DWORD flags; HANDLE object; DWORD ms; } tp_legacy;
static WINAPI void legacy_timer(void *instance, void *ctx, void *timer) {
    (void)instance; (void)timer;
    tp_legacy *l = ctx;
    ((void (WINAPI *)(void *, uint8_t))l->callback)(l->param, 1);
}
static WINAPI HANDLE CreateTimerQueue(void) { static int queue; return &queue; }
static WINAPI BOOL DeleteTimerQueueEx(HANDLE q, HANDLE done) { (void)q; (void)done; return 1; }
static WINAPI BOOL DeleteTimerQueue(HANDLE q) { (void)q; return 1; }
static WINAPI BOOL CreateTimerQueueTimer(HANDLE *out, HANDLE q, void *cb, void *param, DWORD due, DWORD period, DWORD flags) {
    (void)q;
    tp_legacy *l = calloc(1, sizeof *l);
    l->callback = cb; l->param = param; l->flags = flags;
    l->item = CreateThreadpoolTimer((void *)legacy_timer, l, NULL);
    int64_t when = -(int64_t)due * 10000;
    SetThreadpoolTimer(l->item, &when, period, 0);
    *out = l;
    return 1;
}
static WINAPI BOOL ChangeTimerQueueTimer(HANDLE q, tp_legacy *l, DWORD due, DWORD period) {
    (void)q;
    int64_t when = -(int64_t)due * 10000;
    SetThreadpoolTimer(l->item, &when, period, 0);
    return 1;
}
// CompletionEvent (`done`): INVALID_HANDLE_VALUE waits for the callbacks running; an event is set once they have
// returned (at once when none is); NULL neither. The callbacks' context (l) goes with the item, not before.
static WINAPI BOOL DeleteTimerQueueTimer(HANDLE q, tp_legacy *l, HANDLE done) {
    (void)q;
    if (!l) return 1;
    SetThreadpoolTimer(l->item, NULL, 0, 0);
    if (done == INVALID_HANDLE) WaitForThreadpoolTimerCallbacks(l->item, 1);   // wait for running callbacks
    tp_close_ex(l->item, 1, done == INVALID_HANDLE ? NULL : done, 1);
    return 1;
}
// RegisterWaitForSingleObject: the callback runs when the object is signalled (or the wait times out), and again
// for each later signal unless WT_EXECUTEONLYONCE.
static WINAPI void legacy_wait(void *instance, void *ctx, void *wait, DWORD result) {
    (void)instance;
    tp_legacy *l = ctx;
    ((void (WINAPI *)(void *, uint8_t))l->callback)(l->param, result == 0x102);
    if (!(l->flags & 8)) {
        int64_t t = -(int64_t)l->ms * 10000;
        SetThreadpoolWait(wait, l->object, l->ms == 0xffffffffu ? NULL : &t);
    }
}
static WINAPI BOOL RegisterWaitForSingleObject(HANDLE *out, HANDLE object, void *cb, void *param, DWORD ms, DWORD flags) {
    tp_legacy *l = calloc(1, sizeof *l);
    l->callback = cb; l->param = param; l->flags = flags; l->object = object; l->ms = ms;
    l->item = CreateThreadpoolWait((void *)legacy_wait, l, NULL);
    int64_t t = -(int64_t)ms * 10000;
    SetThreadpoolWait(l->item, object, ms == 0xffffffffu ? NULL : &t);
    *out = l;
    return 1;
}
// CompletionEvent as DeleteTimerQueueTimer's. Chromium's ObjectWatcher passes an event and waits on it: it was
// never set, so the thread that stopped watching (a socket's, after its connect or accept event) waited forever.
static WINAPI BOOL UnregisterWaitEx(tp_legacy *l, HANDLE done) {
    if (!l) { vx_set_last_error(6 /* ERROR_INVALID_HANDLE */); return 0; }
    SetThreadpoolWait(l->item, NULL, NULL);
    if (done == INVALID_HANDLE) WaitForThreadpoolWaitCallbacks(l->item, 1);
    tp_close_ex(l->item, 1, done == INVALID_HANDLE ? NULL : done, 1);
    return 1;
}
static WINAPI BOOL UnregisterWait(tp_legacy *l) { return UnregisterWaitEx(l, NULL); }
// Waitable timers: an event the pool's timer sets when due (and resets on SetWaitableTimer for auto-reset ones).
// A named timer is shared between processes: its event is a named one, and its due time lives in shared memory under
// the name, so any process can push it back. Elite Dangerous's WatchDog64.exe creates "EliteDangerousHeartbeatTimer"
// and ends the game when it fires; the game opens it and keeps setting it later (until then every game was ended a
// minute after it started). A process's pool timer, when it fires, looks at the shared due time first and waits again
// if someone has moved it.
typedef struct { volatile int64_t due; volatile int32_t period; int32_t manual; int32_t creator; } wtimer_shared;   // due: CLOCK_REALTIME ns, 0 none
typedef struct wtimer { HANDLE event; tp_item *item; wtimer_shared *shared; struct wtimer *next; } wtimer;
static wtimer *g_wtimers;
static int64_t realtime_ns(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec; }
static WINAPI void wtimer_fire(void *instance, void *ctx, void *timer) {
    (void)instance; (void)timer;
    wtimer *w = ctx;
    if (w->shared) {
        int64_t due = w->shared->due, now = realtime_ns();
        if (!due) return;                                                  // cancelled (by any process)
        if (due > now + 1000000) {                                         // pushed back: wait for the new time
            int64_t rel = -(due - now) / 100;
            SetThreadpoolTimer(w->item, &rel, 0, 0);
            return;
        }
        int32_t period = w->shared->period;
        if (getenv("VXRT_TRACE_TIMERS")) vx_log("timer: a shared waitable timer fired (%lld ms late)", (long long)((now - due) / 1000000));
        __sync_bool_compare_and_swap(&w->shared->due, due, period ? now + (int64_t)period * 1000000 : 0);
        if (period) { int64_t rel = -(int64_t)period * 10000; SetThreadpoolTimer(w->item, &rel, 0, 0); }
    }
    BOOL (WINAPI *set)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "SetEvent");
    set(w->event);
}
// The shared part of a named timer: /vxwt-<hash of the name>; created (create) or only opened.
static wtimer_shared *wtimer_shared_for(const char *name, int create, int manual, int *existed) {
    uint32_t h = 2166136261u;
    for (const char *c = name; *c; c++) h = (h ^ (uint8_t)(*c >= 'A' && *c <= 'Z' ? *c + 32 : *c)) * 16777619u;
    char shm[32];
    snprintf(shm, sizeof shm, "/vxwt-%08x", h);
    int fd = shm_open(shm, O_RDWR, 0600);
    *existed = fd >= 0;
    if (fd < 0 && create) fd = shm_open(shm, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return NULL;
    if (!*existed && ftruncate(fd, sizeof(wtimer_shared))) { close(fd); return NULL; }
    wtimer_shared *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (s == MAP_FAILED) return NULL;
    // The memory outlives the processes: one whose creator has gone (an earlier run's) starts over, as a new object
    // would on Windows, instead of firing at once on its old due time.
    if (*existed && create && s->creator && kill(s->creator, 0) && errno == ESRCH) { *existed = 0; s->due = 0; s->period = 0; }
    if (!*existed) { s->manual = manual; s->creator = getpid(); }
    return s;
}
static HANDLE wtimer_make(BOOL manual, const char *name, int open_only) {
    wtimer_shared *shared = NULL;
    int existed = 0;
    HANDLE event = NULL;
    if (name && *name) {
        shared = wtimer_shared_for(name, !open_only, manual, &existed);
        if (!shared) { vx_set_last_error(2); return NULL; }                 // ERROR_FILE_NOT_FOUND (opening one nobody made)
        char event_name[300];
        snprintf(event_name, sizeof event_name, "vxwt:%s", name);
        HANDLE (WINAPI *create)(void *, BOOL, BOOL, const char *) = (HANDLE (WINAPI *)(void *, BOOL, BOOL, const char *))vx_find_export("kernel32.dll", "CreateEventA");
        event = create(NULL, shared->manual, 0, event_name);
        if (getenv("VXRT_TRACE_TIMERS")) vx_log("timer: %s waitable timer \"%s\"%s", open_only ? "opened" : "created", name, existed ? " (it existed)" : "");
    } else {
        HANDLE (WINAPI *create)(void *, BOOL, BOOL, const WCHAR *) = (HANDLE (WINAPI *)(void *, BOOL, BOOL, const WCHAR *))vx_find_export("kernel32.dll", "CreateEventW");
        event = create(NULL, manual, 0, NULL);
    }
    wtimer *w = calloc(1, sizeof *w);
    w->event = event;
    w->shared = shared;
    w->item = CreateThreadpoolTimer((void *)wtimer_fire, w, NULL);
    pthread_mutex_lock(&g_tp_lock);
    w->next = g_wtimers; g_wtimers = w;
    pthread_mutex_unlock(&g_tp_lock);
    vx_set_last_error(existed && !open_only ? 183 /* ERROR_ALREADY_EXISTS */ : 0);
    if (shared && shared->due) {                                           // set by another process: fire here too, on time
        int64_t rel = -(shared->due - realtime_ns()) / 100;
        if (rel > 0) rel = 0;
        SetThreadpoolTimer(w->item, &rel, 0, 0);
    }
    return w->event;
}
static wtimer *wtimer_of(HANDLE h) {
    pthread_mutex_lock(&g_tp_lock);
    wtimer *w = g_wtimers;
    while (w && w->event != h) w = w->next;
    pthread_mutex_unlock(&g_tp_lock);
    return w;
}
static WINAPI HANDLE CreateWaitableTimerW(void *sa, BOOL manual, const WCHAR *name) {
    (void)sa; char *a = vx_utf8_dup(name); HANDLE h = wtimer_make(manual, a, 0); free(a); return h;
}
static WINAPI HANDLE CreateWaitableTimerA(void *sa, BOOL manual, const char *name) { (void)sa; return wtimer_make(manual, name, 0); }
static WINAPI HANDLE CreateWaitableTimerExW(void *sa, const WCHAR *name, DWORD flags, DWORD access) {
    (void)sa; (void)access; char *a = vx_utf8_dup(name); HANDLE h = wtimer_make(flags & 1, a, 0); free(a); return h;
}
static WINAPI HANDLE OpenWaitableTimerA(DWORD access, BOOL inherit, const char *name) { (void)access; (void)inherit; return wtimer_make(0, name, 1); }
static WINAPI HANDLE OpenWaitableTimerW(DWORD access, BOOL inherit, const WCHAR *name) {
    (void)access; (void)inherit; char *a = vx_utf8_dup(name); HANDLE h = wtimer_make(0, a, 1); free(a); return h;
}
// The due time: negative is relative (100 ns units), positive absolute (a FILETIME; taken against the system clock).
static WINAPI BOOL SetWaitableTimer(HANDLE h, const int64_t *due, LONG period, void *routine, void *arg, BOOL resume) {
    (void)routine; (void)arg; (void)resume;
    wtimer *w = wtimer_of(h);
    if (!w) { vx_set_last_error(6); return 0; }
    BOOL (WINAPI *reset)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "ResetEvent");
    reset(h);
    int64_t rel = *due;
    if (rel > 0) {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        int64_t now = ((int64_t)ts.tv_sec + 11644473600ll) * 10000000ll + ts.tv_nsec / 100;
        rel = now - rel;                                          // in the past: fires at once
        if (rel > 0) rel = 0;
    }
    if (w->shared) {
        w->shared->period = period; w->shared->due = realtime_ns() + (-rel) * 100;
        if (getenv("VXRT_TRACE_TIMERS")) { static int n; if (++n <= 20 || n % 100 == 0) vx_log("timer: shared waitable timer set #%d, due in %lld ms, period %d ms (asked %lld)", n, (long long)(-rel / 10000), (int)period, (long long)*due); }
    }
    SetThreadpoolTimer(w->item, &rel, w->shared ? 0 : (DWORD)period, 0);   // (a shared one repeats in wtimer_fire)
    return 1;
}
static WINAPI BOOL SetWaitableTimerEx(HANDLE h, const int64_t *due, LONG period, void *routine, void *arg, void *reason, DWORD delay) {
    (void)reason; (void)delay;
    return SetWaitableTimer(h, due, period, routine, arg, 0);
}
static WINAPI BOOL CancelWaitableTimer(HANDLE h) {
    wtimer *w = wtimer_of(h);
    if (!w) return 0;
    if (w->shared && getenv("VXRT_TRACE_TIMERS")) vx_log("timer: shared waitable timer cancelled");
    if (w->shared) w->shared->due = 0;
    SetThreadpoolTimer(w->item, NULL, 0, 0);
    return 1;
}

// ---- files ------------------------------------------------------------------------------------------------------------------
// FILE_BASIC_INFO (times, attributes), FILE_STANDARD_INFO (allocation, size, links, delete pending, directory).
static WINAPI BOOL GetFileInformationByHandleEx(HANDLE h, int cls, uint8_t *out, DWORD size) {
    int fd = vx_handle_fd(h);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { vx_set_last_error(6); return 0; }
    if (cls == 0 && size >= 40) {
        memset(out, 0, 40);
        uint64_t t = ((uint64_t)st.st_mtimespec.tv_sec + 11644473600ull) * 10000000ull;
        for (int i = 0; i < 4; i++) memcpy(out + 8 * i, &t, 8);
        *(DWORD *)(out + 32) = S_ISDIR(st.st_mode) ? 0x10 : 0x80;
        return 1;
    }
    if (cls == 1 && size >= 24) {
        memset(out, 0, 24);
        *(uint64_t *)out = (uint64_t)st.st_blocks * 512; *(uint64_t *)(out + 8) = (uint64_t)st.st_size;
        *(DWORD *)(out + 16) = (DWORD)st.st_nlink; out[21] = S_ISDIR(st.st_mode) ? 1 : 0;
        return 1;
    }
    vx_set_last_error(87);
    return 0;
}
// (SetFileInformationByHandle: src/filestate.c)
static WINAPI DWORD GetLongPathNameW(const WCHAR *in, WCHAR *out, DWORD cap) {
    size_t n = vx_wcslen(in);
    if (!out || cap <= n) return (DWORD)n + 1;
    memmove(out, in, (n + 1) * sizeof(WCHAR));
    return (DWORD)n;
}
static WINAPI DWORD GetLongPathNameA(const char *in, char *out, DWORD cap) {
    size_t n = strlen(in);
    if (!out || cap <= n) return (DWORD)n + 1;
    memmove(out, in, n + 1);
    return (DWORD)n;
}
static WINAPI BOOL CreateSymbolicLinkW(const WCHAR *link, const WCHAR *target, DWORD flags) { (void)link; (void)target; (void)flags; vx_set_last_error(1314); return 0; }
static WINAPI BOOL AreFileApisANSI(void) { return 1; }

// ---- toolhelp: no snapshots (crash reporters fall back), processes and modules -----------------------------------------
static WINAPI HANDLE CreateToolhelp32Snapshot(DWORD flags, DWORD pid) { (void)flags; (void)pid; vx_set_last_error(50); return INVALID_HANDLE; }
static WINAPI BOOL toolhelp_none(HANDLE h, void *entry) { (void)h; (void)entry; vx_set_last_error(18); return 0; }

// ---- processes: no packages, exception policy ---------------------------------------------------------------------------
static WINAPI LONG GetCurrentPackageId(DWORD *len, void *buf) { (void)len; (void)buf; return 15700; }   // APPMODEL_ERROR_NO_PACKAGE
static WINAPI LONG GetCurrentPackageFullName(DWORD *len, void *buf) { (void)len; (void)buf; return 15700; }
static DWORD g_exception_policy;
static WINAPI BOOL GetProcessUserModeExceptionPolicy(DWORD *f) { *f = g_exception_policy; return 1; }
static WINAPI BOOL SetProcessUserModeExceptionPolicy(DWORD f) { g_exception_policy = f; return 1; }

// ---- locales by name ------------------------------------------------------------------------------------------------------
static WINAPI int GetUserDefaultLocaleName(WCHAR *out, int cap) { return (int)vx_return_string_w("en-US", out, (DWORD)cap) + 1; }
static WINAPI int GetSystemDefaultLocaleName(WCHAR *out, int cap) { return GetUserDefaultLocaleName(out, cap); }
static WINAPI int LCIDToLocaleName(DWORD lcid, WCHAR *out, int cap, DWORD flags) {
    (void)flags;
    const vx_locale *l = vx_locale_by_lcid(lcid);
    if (!l) { vx_set_last_error(87); return 0; }
    return (int)vx_return_string_w(l->name, out, (DWORD)cap) + 1;
}
static WINAPI DWORD LocaleNameToLCID(const WCHAR *name, DWORD flags) {
    (void)flags;
    char *a = vx_utf8_dup(name);
    const vx_locale *l = vx_locale_by_name(a);
    if (getenv("VXRT_TRACE_LOCALE")) vx_log("locale: LocaleNameToLCID(%s) = %#x", a ? a : "NULL", l ? (unsigned)l->lcid : 0u);
    free(a);
    if (!l) { vx_set_last_error(87); return 0; }
    return l->lcid;
}
static WINAPI int GetLocaleInfoEx(const WCHAR *name, DWORD type, WCHAR *out, int cap) {
    char *a = vx_utf8_dup(name);
    const vx_locale *l = vx_locale_by_name(a);
    if (getenv("VXRT_TRACE_LOCALE")) vx_log("locale: GetLocaleInfoEx(%s, %#x)%s", a ? a : "NULL", (unsigned)type, l ? "" : ": unknown");
    free(a);
    if (!l) { vx_set_last_error(87); return 0; }
    int (WINAPI *info)(DWORD, DWORD, WCHAR *, int) = (int (WINAPI *)(DWORD, DWORD, WCHAR *, int))vx_find_export("kernel32.dll", "GetLocaleInfoW");
    if (info) return info(l->lcid, type, out, cap);
    return (int)vx_return_string_w("", out, (DWORD)cap) + 1;
}

// ---- shell folders: the user's profile, application data, documents ------------------------------------------------------
const char *vx_special_folder_path(int csidl) {
    switch (csidl & 0xff) {
    case 0x05: return "C:\\users\\vxrt\\Documents";                   // CSIDL_PERSONAL
    case 0x1a: return "C:\\users\\vxrt\\AppData\\Roaming";            // CSIDL_APPDATA
    case 0x1c: return "C:\\users\\vxrt\\AppData\\Local";              // CSIDL_LOCAL_APPDATA
    case 0x23: return "C:\\ProgramData";                              // CSIDL_COMMON_APPDATA
    case 0x28: return "C:\\users\\vxrt";                              // CSIDL_PROFILE
    case 0x24: return "C:\\windows";                                  // CSIDL_WINDOWS
    case 0x25: return "C:\\windows\\system32";                        // CSIDL_SYSTEM
    case 0x26: return "C:\\Program Files";                            // CSIDL_PROGRAM_FILES
    case 0x2a: return "C:\\Program Files (x86)";
    case 0x10: return "C:\\users\\vxrt\\Desktop";                     // CSIDL_DESKTOPDIRECTORY
    case 0x2e: return "C:\\users\\Public\\Documents";                 // CSIDL_COMMON_DOCUMENTS
    default: return NULL;
    }
}
static void make_folder(const char *win) {
    char unix_path[1024];
    if (!vx_unix_path(win, unix_path, sizeof unix_path)) return;
    for (char *c = unix_path + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(unix_path, 0777); *c = '/'; }
    mkdir(unix_path, 0777);
}
static WINAPI LONG SHGetFolderPathW(void *wnd, int csidl, HANDLE token, DWORD flags, WCHAR *out) {
    (void)wnd; (void)token; (void)flags;
    const char *p = vx_special_folder_path(csidl);
    if (!p) return (LONG)0x80070057;
    make_folder(p);
    vx_return_string_w(p, out, 260);
    return 0;
}
static WINAPI LONG SHGetFolderPathA(void *wnd, int csidl, HANDLE token, DWORD flags, char *out) {
    (void)wnd; (void)token; (void)flags;
    const char *p = vx_special_folder_path(csidl);
    if (!p) return (LONG)0x80070057;
    make_folder(p);
    vx_return_string_a(p, out, 260);
    return 0;
}
// Known folders (Vista's GUIDs for the same places); the path is CoTaskMemAlloc'd (malloc, as com.c's).
static WINAPI LONG SHGetKnownFolderPath(const uint8_t *id, DWORD flags, HANDLE token, WCHAR **out) {
    (void)flags; (void)token;
    static const struct { uint32_t d1; uint16_t d2, d3; int csidl; } known[] = {
        { 0x3eb685db, 0x65f9, 0x4cf6, 0x1a }, { 0xf1b32785, 0x6fba, 0x4fcf, 0x1c }, { 0xfdd39ad0, 0x238f, 0x46af, 0x05 },
        { 0x62ab5d82, 0xfdc1, 0x4dc3, 0x23 }, { 0x5e6c858f, 0x0e22, 0x4760, 0x28 }, { 0xf38bf404, 0x1d43, 0x42f2, 0x24 },
        { 0x1ac14e77, 0x02e7, 0x4e5d, 0x25 }, { 0x905e63b6, 0xc1bf, 0x494e, 0x26 }, { 0x7c5a40ef, 0xa0fb, 0x4bfc, 0x2a },
        { 0xb4bfcc3a, 0xdb2c, 0x424c, 0x10 }, { 0xed4824af, 0xdce4, 0x45a8, 0x2e },
    };
    uint32_t d1; uint16_t d2, d3;
    memcpy(&d1, id, 4); memcpy(&d2, id + 4, 2); memcpy(&d3, id + 6, 2);
    const char *p = NULL;
    for (size_t i = 0; i < sizeof known / sizeof known[0] && !p; i++)
        if (known[i].d1 == d1 && known[i].d2 == d2 && known[i].d3 == d3) p = vx_special_folder_path(known[i].csidl);
    if (d1 == 0xa520a1a4 && d2 == 0x1780) p = "C:\\users\\vxrt\\AppData\\LocalLow";
    if (d1 == 0x4c5c32ff && d2 == 0xbb9d) p = "C:\\users\\vxrt\\Saved Games";
    if (out) *out = NULL;
    if (!p) return (LONG)0x80070002;                               // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
    make_folder(p);
    WCHAR *w = vx_utf16_dup(p);
    if (out) *out = w; else free(w);
    return 0;
}
static WINAPI BOOL SHGetSpecialFolderPathW(void *wnd, WCHAR *out, int csidl, BOOL create) { (void)create; return SHGetFolderPathW(wnd, csidl, NULL, 0, out) == 0; }
static WINAPI BOOL SHGetSpecialFolderPathA(void *wnd, char *out, int csidl, BOOL create) { (void)create; return SHGetFolderPathA(wnd, csidl, NULL, 0, out) == 0; }

// ---- ntdll ---------------------------------------------------------------------------------------------------------------
static WINAPI DWORD NtQuerySystemInformation(int cls, void *out, DWORD size, DWORD *ret) { (void)cls; (void)out; (void)size; if (ret) *ret = 0; return STATUS_NOT_IMPLEMENTED; }
static WINAPI DWORD NtQueryInformationProcess(HANDLE p, int cls, void *out, DWORD size, DWORD *ret) { (void)p; (void)cls; (void)out; (void)size; if (ret) *ret = 0; return STATUS_NOT_IMPLEMENTED; }
static WINAPI DWORD NtQueryInformationThread(HANDLE t, int cls, void *out, DWORD size, DWORD *ret) { (void)t; (void)cls; (void)out; (void)size; if (ret) *ret = 0; return STATUS_NOT_IMPLEMENTED; }
int vx_reg_close(void *hkey);
static WINAPI DWORD NtClose(HANDLE h) {
    if (vx_reg_close(h)) return 0;
    BOOL (WINAPI *close_handle)(HANDLE) = (BOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "CloseHandle");
    return close_handle && close_handle(h) ? 0 : 0xC0000008u;      // STATUS_INVALID_HANDLE
}
typedef struct { uint16_t length, max; WCHAR *buffer; } UNICODE_STRING;
static WINAPI void RtlInitUnicodeString(UNICODE_STRING *u, const WCHAR *s) {
    size_t n = s ? vx_wcslen(s) * 2 : 0;
    u->length = (uint16_t)n; u->max = (uint16_t)(s ? n + 2 : 0); u->buffer = (WCHAR *)s;
}
static WINAPI void RtlFreeUnicodeString(UNICODE_STRING *u) { if (u) { free(u->buffer); u->buffer = NULL; u->length = u->max = 0; } }
static WINAPI BOOL RtlFreeHeap(HANDLE heap, DWORD flags, void *p) {
    BOOL (WINAPI *heap_free)(HANDLE, DWORD, void *) = (BOOL (WINAPI *)(HANDLE, DWORD, void *))vx_find_export("kernel32.dll", "HeapFree");
    return heap_free ? heap_free(heap, flags, p) : 0;
}
static WINAPI void *RtlAllocateHeap(HANDLE heap, DWORD flags, size_t n) {
    void *(WINAPI *heap_alloc)(HANDLE, DWORD, size_t) = (void *(WINAPI *)(HANDLE, DWORD, size_t))vx_find_export("kernel32.dll", "HeapAlloc");
    return heap_alloc ? heap_alloc(heap, flags, n) : NULL;
}
static WINAPI DWORD NtQueryVirtualMemory(HANDLE p, const void *addr, int cls, void *out, size_t n, size_t *ret) {
    (void)p;
    if (cls != 0) { if (ret) *ret = 0; return STATUS_NOT_IMPLEMENTED; }    // MemoryBasicInformation only
    size_t (WINAPI *query)(const void *, void *, size_t) = (size_t (WINAPI *)(const void *, void *, size_t))vx_find_export("kernel32.dll", "VirtualQuery");
    size_t r = query(addr, out, n);
    if (ret) *ret = r;
    return r ? 0 : 0xC000000Du;                                       // STATUS_INVALID_PARAMETER
}
static WINAPI DWORD LdrLockLoaderLock(DWORD flags, DWORD *state, uintptr_t *cookie) { (void)flags; if (state) *state = 1; if (cookie) *cookie = 1; return 0; }
static WINAPI DWORD LdrUnlockLoaderLock(DWORD flags, uintptr_t cookie) { (void)flags; (void)cookie; return 0; }
void *vx_virtual_alloc_below(size_t size, uintptr_t limit, DWORD protect);   // kernel32.c
// ZeroBits asks for an address with that many high zero bits counted from bit 31 (1: below 2 GB), or, when it is
// larger than 32, is a mask the address must fit in.
static WINAPI DWORD NtAllocateVirtualMemory(HANDLE p, void **addr, uintptr_t zero, size_t *size, DWORD type, DWORD protect) {
    (void)p;
    if (zero && !*addr && (type & 0x2000 /* MEM_RESERVE */)) {
        uintptr_t limit = zero < 32 ? (uintptr_t)1 << (32 - zero) : zero + 1;
        void *r = vx_virtual_alloc_below(*size, limit, protect);
        if (!r) return 0xC0000017u;
        *addr = r;
        return 0;
    }
    void *(WINAPI *valloc)(void *, size_t, DWORD, DWORD) = (void *(WINAPI *)(void *, size_t, DWORD, DWORD))vx_find_export("kernel32.dll", "VirtualAlloc");
    void *r = valloc ? valloc(*addr, *size, type, protect) : NULL;
    if (!r) return 0xC0000017u;                                       // STATUS_NO_MEMORY
    *addr = r;
    return 0;
}
// FlsGetValue2 (Windows 11): FlsGetValue without setting the last error.
static WINAPI void *FlsGetValue2(DWORD i) {
    void *(WINAPI *get)(DWORD) = (void *(WINAPI *)(DWORD))vx_find_export("kernel32.dll", "FlsGetValue");
    return get ? get(i) : NULL;
}

static const vx_export k32[] = {
    VX_EXPORT(GetLogicalProcessorInformation), VX_EXPORT(GetLogicalProcessorInformationEx), VX_EXPORT(GetActiveProcessorCount),
    VX_EXPORT(GetMaximumProcessorCount), VX_EXPORT(GetActiveProcessorGroupCount), VX_EXPORT(GetMaximumProcessorGroupCount),
    VX_EXPORT(SetThreadIdealProcessor), VX_EXPORT(SetThreadIdealProcessorEx), VX_EXPORT(GetThreadIdealProcessorEx),
    VX_EXPORT(SetThreadGroupAffinity), VX_EXPORT(GetThreadGroupAffinity), VX_EXPORT(FlushProcessWriteBuffers),
    VX_EXPORT(CreateThreadpoolWork), VX_EXPORT(SubmitThreadpoolWork), VX_EXPORT(WaitForThreadpoolWorkCallbacks),
    VX_EXPORT(CloseThreadpoolWork), VX_EXPORT(TrySubmitThreadpoolCallback), VX_EXPORT(CreateThreadpoolTimer),
    VX_EXPORT(SetThreadpoolTimer), VX_EXPORT(IsThreadpoolTimerSet), VX_EXPORT(WaitForThreadpoolTimerCallbacks),
    VX_EXPORT(CloseThreadpoolTimer), VX_EXPORT(CreateThreadpoolWait), VX_EXPORT(SetThreadpoolWait),
    VX_EXPORT(WaitForThreadpoolWaitCallbacks), VX_EXPORT(CloseThreadpoolWait), VX_EXPORT(FreeLibraryWhenCallbackReturns),
    VX_EXPORT(SetEventWhenCallbackReturns), VX_EXPORT(CreateThreadpool), VX_EXPORT(CloseThreadpool),
    VX_EXPORT(SetThreadpoolThreadMaximum), VX_EXPORT(SetThreadpoolThreadMinimum),
    VX_EXPORT(CreateTimerQueue), VX_EXPORT(DeleteTimerQueue), VX_EXPORT(DeleteTimerQueueEx), VX_EXPORT(CreateTimerQueueTimer),
    VX_EXPORT(ChangeTimerQueueTimer), VX_EXPORT(DeleteTimerQueueTimer), VX_EXPORT(RegisterWaitForSingleObject),
    VX_EXPORT(UnregisterWait), VX_EXPORT(UnregisterWaitEx), VX_EXPORT(CreateWaitableTimerW), VX_EXPORT(OpenWaitableTimerA), VX_EXPORT(OpenWaitableTimerW), VX_EXPORT(CreateWaitableTimerA),
    VX_EXPORT(CreateWaitableTimerExW), VX_EXPORT(SetWaitableTimer), VX_EXPORT(SetWaitableTimerEx), VX_EXPORT(CancelWaitableTimer),
    VX_EXPORT(GetFileInformationByHandleEx), VX_EXPORT(GetLongPathNameW),
    VX_EXPORT(GetLongPathNameA), VX_EXPORT(CreateSymbolicLinkW), VX_EXPORT(AreFileApisANSI),
    VX_EXPORT(CreateToolhelp32Snapshot), { "Thread32First", (void *)toolhelp_none }, { "Thread32Next", (void *)toolhelp_none },
    { "Module32First", (void *)toolhelp_none }, { "Module32Next", (void *)toolhelp_none }, { "Module32FirstW", (void *)toolhelp_none },
    { "Module32NextW", (void *)toolhelp_none }, { "Process32First", (void *)toolhelp_none }, { "Process32Next", (void *)toolhelp_none },
    { "Process32FirstW", (void *)toolhelp_none }, { "Process32NextW", (void *)toolhelp_none },
    VX_EXPORT(GetCurrentPackageId), VX_EXPORT(GetCurrentPackageFullName),
    VX_EXPORT(GetProcessUserModeExceptionPolicy), VX_EXPORT(SetProcessUserModeExceptionPolicy),
    VX_EXPORT(GetUserDefaultLocaleName), VX_EXPORT(GetSystemDefaultLocaleName), VX_EXPORT(LCIDToLocaleName),
    VX_EXPORT(LocaleNameToLCID), VX_EXPORT(GetLocaleInfoEx), VX_EXPORT(FlsGetValue2),
};
static const vx_export shell[] = {
    VX_EXPORT(SHGetFolderPathW), VX_EXPORT(SHGetFolderPathA), VX_EXPORT(SHGetKnownFolderPath), VX_EXPORT(SHGetSpecialFolderPathW), VX_EXPORT(SHGetSpecialFolderPathA),
};
static const vx_export ntdll[] = {
    VX_EXPORT(NtQuerySystemInformation), VX_EXPORT(NtQueryInformationProcess), VX_EXPORT(NtQueryInformationThread),
    VX_EXPORT(NtClose), VX_EXPORT(RtlInitUnicodeString), VX_EXPORT(RtlFreeUnicodeString), VX_EXPORT(RtlFreeHeap), VX_EXPORT(RtlAllocateHeap), VX_EXPORT(LdrLockLoaderLock),
    VX_EXPORT(LdrUnlockLoaderLock), VX_EXPORT(NtAllocateVirtualMemory), VX_EXPORT(NtQueryVirtualMemory),
};
void vx_system_init(void) {
    vx_register_module("kernel32.dll", k32, sizeof k32 / sizeof k32[0]);
    vx_register_module("shell32.dll", shell, sizeof shell / sizeof shell[0]);
    vx_register_module("ntdll.dll", ntdll, sizeof ntdll / sizeof ntdll[0]);
}

#ifdef __aarch64__
#include "thunks/system.c.inc"
#endif
