// Threads and kernel objects: threads, events, mutexes, semaphores, waits, critical sections, SRW locks,
// condition variables, one-time initialization.
//
// Kernel objects live in this process (there is no server): a handle indexes a table of reference-counted
// objects, and every state change wakes all waiters through one condition variable; each waiter then re-checks
// what it waits for. Simple and correct; per-object wait queues are a later optimisation.
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vx.h"

#define INFINITE      0xffffffffu
#define WAIT_TIMEOUT  0x102u
#define WAIT_FAILED   0xffffffffu
#define STILL_ACTIVE  259u
#define CREATE_SUSPENDED 4u

enum { OBJ_THREAD = 1, OBJ_EVENT, OBJ_MUTEX, OBJ_SEMAPHORE };
typedef struct {
    int type, refs;
    int done, started; DWORD suspended, exit_code, tid; pthread_t pthread;     // thread
    DWORD (WINAPI *start)(void *); void *arg;
    int manual, signaled;                                                     // event
    DWORD owner, recursion;                                                   // mutex
    LONG count, max;                                                          // semaphore
} vx_object;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_changed = PTHREAD_COND_INITIALIZER;
#define HANDLE_BASE 0x100
#define MAX_HANDLES 8192             // handles 0x100..0x80fc; file handles start at 0x10000
static vx_object *g_handles[MAX_HANDLES];
static vx_object g_never;            // what the pseudo handles of the current process/thread wait on

DWORD vx_current_tid(void) { uint64_t t; pthread_threadid_np(NULL, &t); return (DWORD)t; }

// All of the following run with g_lock held.
static HANDLE new_handle(vx_object *o) {
    for (int i = 0; i < MAX_HANDLES; i++)
        if (!g_handles[i]) { g_handles[i] = o; o->refs++; return (HANDLE)(intptr_t)(HANDLE_BASE + i * 4); }
    return NULL;
}
static int handle_index(HANDLE h) {
    intptr_t v = (intptr_t)h - HANDLE_BASE;
    return v >= 0 && v % 4 == 0 && v / 4 < MAX_HANDLES ? (int)(v / 4) : -1;
}
static vx_object *lookup(HANDLE h) { int i = handle_index(h); return i >= 0 ? g_handles[i] : NULL; }
static void unref(vx_object *o) { if (--o->refs == 0) free(o); }
static vx_object *new_object(int type) { vx_object *o = calloc(1, sizeof *o); o->type = type; return o; }

// ---- threads -----------------------------------------------------------------------------------------------------
static __thread vx_object *t_self;          // this thread's object (created on demand for threads not made here)
static __thread jmp_buf *t_exit_jump;       // where ExitThread leaves the thread's start routine
static __thread DWORD t_exit_code;

static vx_object *self_object(void) {
    if (!t_self) {
        t_self = new_object(OBJ_THREAD);
        t_self->refs = 1; t_self->started = 1; t_self->tid = vx_current_tid(); t_self->pthread = pthread_self();
    }
    return t_self;
}

static void *thread_main(void *p) {
    vx_object *t = p;
    t_self = t;
    vx_thread_attach();
    pthread_mutex_lock(&g_lock);              // also waits until CreateThread has filled in the object
    while (t->suspended) pthread_cond_wait(&g_changed, &g_lock);
    t->started = 1;
    pthread_mutex_unlock(&g_lock);
    vx_call_tls_callbacks(2 /* DLL_THREAD_ATTACH */);
    jmp_buf exit_jump;
    t_exit_jump = &exit_jump;
    DWORD code;
    if (!setjmp(exit_jump)) code = t->start(t->arg);
    else code = t_exit_code;
    t_exit_jump = NULL;
    vx_call_tls_callbacks(3 /* DLL_THREAD_DETACH */);
    pthread_mutex_lock(&g_lock);
    t->done = 1; t->exit_code = code;
    t_self = NULL;
    unref(t);
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static WINAPI HANDLE CreateThread(void *sa, size_t stack, DWORD (WINAPI *start)(void *), void *arg, DWORD flags, DWORD *tid) {
    (void)sa;
    vx_object *t = new_object(OBJ_THREAD);
    t->refs = 1;                               // held by the running thread
    t->start = start; t->arg = arg;
    t->suspended = flags & CREATE_SUSPENDED ? 1 : 0;
    // The thread's stack also carries this runtime's frames (exception dispatch keeps CONTEXTs on it): 2 MB at least.
    size_t size = stack > (2u << 20) ? stack : (2u << 20);
    size = (size + 16383) & ~(size_t)16383;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, size);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_mutex_lock(&g_lock);
    HANDLE h = new_handle(t);
    if (!h || pthread_create(&t->pthread, &a, thread_main, t)) {
        if (h) g_handles[handle_index(h)] = NULL;
        pthread_mutex_unlock(&g_lock);
        free(t); pthread_attr_destroy(&a);
        vx_set_last_error(8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return NULL;
    }
    uint64_t id; pthread_threadid_np(t->pthread, &id);
    t->tid = (DWORD)id;
    pthread_mutex_unlock(&g_lock);
    pthread_attr_destroy(&a);
    if (tid) *tid = t->tid;
    return h;
}

static WINAPI void ExitThread(DWORD code) {
    if (t_exit_jump) { t_exit_code = code; longjmp(*t_exit_jump, 1); }
    fflush(stdout);
    pthread_exit(NULL);                        // the main thread: the process lives on while other threads run
}

static WINAPI uintptr_t _beginthreadex(void *sa, unsigned stack, DWORD (WINAPI *start)(void *), void *arg, unsigned flags, unsigned *tid) {
    DWORD id = 0;
    HANDLE h = CreateThread(sa, stack, start, arg, flags, &id);
    if (tid) *tid = id;
    return (uintptr_t)h;
}
static WINAPI void _endthreadex(unsigned code) { ExitThread(code); }

static vx_object *thread_object(HANDLE h) {       // with g_lock held
    if ((intptr_t)h == -2) return self_object();
    vx_object *o = lookup(h);
    return o && o->type == OBJ_THREAD ? o : NULL;
}

static WINAPI BOOL GetExitCodeThread(HANDLE h, DWORD *code) {
    pthread_mutex_lock(&g_lock);
    vx_object *t = thread_object(h);
    if (t) *code = t->done ? t->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&g_lock);
    if (!t) vx_set_last_error(6 /* ERROR_INVALID_HANDLE */);
    return t != NULL;
}
static WINAPI DWORD GetThreadId(HANDLE h) {
    pthread_mutex_lock(&g_lock);
    vx_object *t = thread_object(h);
    DWORD id = t ? t->tid : 0;
    pthread_mutex_unlock(&g_lock);
    return id;
}
static WINAPI DWORD ResumeThread(HANDLE h) {
    pthread_mutex_lock(&g_lock);
    vx_object *t = thread_object(h);
    DWORD prev = t ? t->suspended : (DWORD)-1;
    if (t && t->suspended) t->suspended--;
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
    return prev;
}
// Only a thread that has not started yet can be suspended for now (macOS can suspend a Mach thread, but its
// state while stopped in this runtime's code is not a Windows context yet).
static WINAPI DWORD SuspendThread(HANDLE h) {
    pthread_mutex_lock(&g_lock);
    vx_object *t = thread_object(h);
    DWORD prev = (DWORD)-1;
    if (t && !t->started) prev = t->suspended++;
    pthread_mutex_unlock(&g_lock);
    if (prev == (DWORD)-1) vx_set_last_error(50 /* ERROR_NOT_SUPPORTED */);
    return prev;
}
static WINAPI BOOL GetThreadContext(HANDLE h, void *ctx) { (void)h; (void)ctx; vx_set_last_error(50); return 0; }
static WINAPI BOOL SetThreadContext(HANDLE h, const void *ctx) { (void)h; (void)ctx; vx_set_last_error(50); return 0; }
static WINAPI int GetThreadPriority(HANDLE h) { (void)h; return 0; }
static WINAPI BOOL SetThreadPriority(HANDLE h, int p) { (void)h; (void)p; return 1; }
static WINAPI LONG SetThreadDescription(HANDLE h, const WCHAR *d) { (void)h; (void)d; return 0; }
static WINAPI BOOL SwitchToThread(void) { sched_yield(); return 1; }

static uint64_t cpu_mask(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n >= 64 ? ~0ull : (1ull << n) - 1; }
static WINAPI BOOL GetProcessAffinityMask(HANDLE p, uint64_t *pm, uint64_t *sm) { (void)p; *pm = *sm = cpu_mask(); return 1; }
static WINAPI BOOL SetProcessAffinityMask(HANDLE p, uint64_t m) { (void)p; (void)m; return 1; }
static WINAPI uint64_t SetThreadAffinityMask(HANDLE h, uint64_t m) { (void)h; (void)m; return cpu_mask(); }

static WINAPI HANDLE OpenProcess(DWORD access, BOOL inherit, DWORD pid) {
    (void)access; (void)inherit;
    if (pid == (DWORD)getpid()) return (HANDLE)(intptr_t)-1;
    vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */);
    return NULL;
}

// ---- events, mutexes, semaphores ----------------------------------------------------------------------------------
static HANDLE create(vx_object *o) {
    pthread_mutex_lock(&g_lock);
    HANDLE h = new_handle(o);
    pthread_mutex_unlock(&g_lock);
    if (!h) { free(o); vx_set_last_error(4 /* ERROR_TOO_MANY_OPEN_FILES */); return NULL; }
    vx_set_last_error(0);                       // names are ignored: every object is new
    return h;
}
static HANDLE create_event(BOOL manual, BOOL initial) {
    vx_object *o = new_object(OBJ_EVENT); o->manual = manual; o->signaled = initial; return create(o);
}
static WINAPI HANDLE CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name) { (void)sa; (void)name; return create_event(manual, initial); }
static WINAPI HANDLE CreateEventW(void *sa, BOOL manual, BOOL initial, const WCHAR *name) { (void)sa; (void)name; return create_event(manual, initial); }
static WINAPI HANDLE CreateEventExW(void *sa, const WCHAR *name, DWORD flags, DWORD access) {
    (void)sa; (void)name; (void)access;
    return create_event(flags & 1 /* CREATE_EVENT_MANUAL_RESET */, (flags & 2) != 0 /* CREATE_EVENT_INITIAL_SET */);
}
static HANDLE create_mutex(BOOL owned) {
    vx_object *o = new_object(OBJ_MUTEX);
    if (owned) { o->owner = vx_current_tid(); o->recursion = 1; }
    return create(o);
}
static WINAPI HANDLE CreateMutexA(void *sa, BOOL owned, const char *name) { (void)sa; (void)name; return create_mutex(owned); }
static WINAPI HANDLE CreateMutexW(void *sa, BOOL owned, const WCHAR *name) { (void)sa; (void)name; return create_mutex(owned); }
static HANDLE create_semaphore(LONG initial, LONG max) {
    if (max <= 0 || initial < 0 || initial > max) { vx_set_last_error(87); return NULL; }
    vx_object *o = new_object(OBJ_SEMAPHORE); o->count = initial; o->max = max; return create(o);
}
static WINAPI HANDLE CreateSemaphoreA(void *sa, LONG initial, LONG max, const char *name) { (void)sa; (void)name; return create_semaphore(initial, max); }
static WINAPI HANDLE CreateSemaphoreW(void *sa, LONG initial, LONG max, const WCHAR *name) { (void)sa; (void)name; return create_semaphore(initial, max); }
static WINAPI HANDLE CreateSemaphoreExW(void *sa, LONG initial, LONG max, const WCHAR *name, DWORD flags, DWORD access) {
    (void)sa; (void)name; (void)flags; (void)access; return create_semaphore(initial, max);
}

// Runs `change` on the object of type `type` behind h, with the lock held, and wakes the waiters.
static BOOL modify(HANDLE h, int type, int (*change)(vx_object *, intptr_t), intptr_t arg) {
    pthread_mutex_lock(&g_lock);
    vx_object *o = lookup(h);
    int r = o && o->type == type ? change(o, arg) : 6 /* ERROR_INVALID_HANDLE */;
    if (!r) pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
    if (r) vx_set_last_error((DWORD)r);
    return r == 0;
}
static int set_event(vx_object *o, intptr_t v) { o->signaled = (int)v; return 0; }
static WINAPI BOOL SetEvent(HANDLE h) { return modify(h, OBJ_EVENT, set_event, 1); }
static WINAPI BOOL ResetEvent(HANDLE h) { return modify(h, OBJ_EVENT, set_event, 0); }
static int release_mutex(vx_object *o, intptr_t tid) {
    if (o->owner != (DWORD)tid) return 288;     // ERROR_NOT_OWNER
    if (--o->recursion == 0) o->owner = 0;
    return 0;
}
static WINAPI BOOL ReleaseMutex(HANDLE h) { return modify(h, OBJ_MUTEX, release_mutex, vx_current_tid()); }

static WINAPI BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *previous) {
    pthread_mutex_lock(&g_lock);
    vx_object *o = lookup(h);
    DWORD err = !o || o->type != OBJ_SEMAPHORE ? 6 : n <= 0 || o->count + n > o->max ? 298 /* ERROR_TOO_MANY_POSTS */ : 0;
    if (!err) {
        if (previous) *previous = o->count;
        o->count += n;
        pthread_cond_broadcast(&g_changed);
    }
    pthread_mutex_unlock(&g_lock);
    if (err) vx_set_last_error(err);
    return !err;
}

// ---- waiting ---------------------------------------------------------------------------------------------------------------
static int is_signaled(const vx_object *o, DWORD tid) {
    switch (o->type) {
    case OBJ_THREAD:    return o->done;
    case OBJ_EVENT:     return o->signaled;
    case OBJ_MUTEX:     return !o->owner || o->owner == tid;
    case OBJ_SEMAPHORE: return o->count > 0;
    }
    return 0;
}
static void acquire(vx_object *o, DWORD tid) {
    if (o->type == OBJ_EVENT && !o->manual) o->signaled = 0;
    else if (o->type == OBJ_MUTEX) { o->owner = tid; o->recursion++; }
    else if (o->type == OBJ_SEMAPHORE) o->count--;
}

static void deadline_after(struct timespec *ts, DWORD ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts->tv_nsec >= 1000000000) { ts->tv_sec++; ts->tv_nsec -= 1000000000; }
}

static WINAPI DWORD WaitForMultipleObjects(DWORD n, const HANDLE *hs, BOOL all, DWORD ms) {
    if (!n || n > 64) { vx_set_last_error(87); return WAIT_FAILED; }
    DWORD tid = vx_current_tid();
    struct timespec deadline;
    if (ms != INFINITE) deadline_after(&deadline, ms);
    vx_object *objs[64];
    pthread_mutex_lock(&g_lock);
    for (DWORD i = 0; i < n; i++) {
        objs[i] = lookup(hs[i]);
        if (!objs[i] && ((intptr_t)hs[i] == -1 || (intptr_t)hs[i] == -2)) objs[i] = &g_never;
        if (!objs[i]) { pthread_mutex_unlock(&g_lock); vx_set_last_error(6); return WAIT_FAILED; }
    }
    DWORD result;
    for (;;) {
        if (all) {
            DWORD i = 0;
            while (i < n && is_signaled(objs[i], tid)) i++;
            if (i == n) { for (i = 0; i < n; i++) acquire(objs[i], tid); result = 0; break; }
        } else {
            DWORD i = 0;
            while (i < n && !is_signaled(objs[i], tid)) i++;
            if (i < n) { acquire(objs[i], tid); result = i; break; }
        }
        if (ms == 0) { result = WAIT_TIMEOUT; break; }
        if (ms == INFINITE) pthread_cond_wait(&g_changed, &g_lock);
        else if (pthread_cond_timedwait(&g_changed, &g_lock, &deadline) == ETIMEDOUT) { result = WAIT_TIMEOUT; break; }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}
static WINAPI DWORD WaitForMultipleObjectsEx(DWORD n, const HANDLE *hs, BOOL all, DWORD ms, BOOL alertable) {
    (void)alertable; return WaitForMultipleObjects(n, hs, all, ms);
}
static WINAPI DWORD WaitForSingleObject(HANDLE h, DWORD ms) { return WaitForMultipleObjects(1, &h, 0, ms); }
static WINAPI DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable) { (void)alertable; return WaitForMultipleObjects(1, &h, 0, ms); }
static WINAPI DWORD SleepEx(DWORD ms, BOOL alertable) {
    (void)alertable;
    if (ms) usleep((useconds_t)ms * 1000); else sched_yield();
    return 0;
}

// ---- handles -----------------------------------------------------------------------------------------------------------------
static WINAPI BOOL CloseHandle(HANDLE h) {
    intptr_t v = (intptr_t)h;
    if (v == -1 || v == -2) return 1;
    pthread_mutex_lock(&g_lock);
    int i = handle_index(h);
    vx_object *o = i >= 0 ? g_handles[i] : NULL;
    if (o) { g_handles[i] = NULL; unref(o); }
    pthread_mutex_unlock(&g_lock);
    if (o) return 1;
    int fd = vx_handle_fd(h);
    if (fd > 2) { close(fd); return 1; }
    if (fd >= 0) return 1;
    vx_set_last_error(6);
    return 0;
}

static WINAPI BOOL DuplicateHandle(HANDLE src_process, HANDLE src, HANDLE dst_process, HANDLE *dst, DWORD access, BOOL inherit, DWORD options) {
    (void)src_process; (void)dst_process; (void)access; (void)inherit;
    if ((intptr_t)src == -1) { *dst = src; return 1; }
    pthread_mutex_lock(&g_lock);
    vx_object *o = (intptr_t)src == -2 ? self_object() : lookup(src);
    HANDLE h = o ? new_handle(o) : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!o) {
        int fd = vx_handle_fd(src);
        if (fd < 0) { vx_set_last_error(6); return 0; }
        h = (HANDLE)(intptr_t)(0x10000 + dup(fd));
    }
    *dst = h;
    if (options & 1 /* DUPLICATE_CLOSE_SOURCE */) CloseHandle(src);
    return h != NULL;
}

// ---- critical sections and SRW locks -------------------------------------------------------------------------------------
// A CRITICAL_SECTION (40 bytes, in the program's memory) holds a pointer to a recursive pthread mutex.
static pthread_mutex_t *cs_mutex(void *cs) {
    pthread_mutex_t **slot = (pthread_mutex_t **)cs;
    if (!*slot) {
        pthread_mutex_t *m = malloc(sizeof *m);
        pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
        if (!__sync_bool_compare_and_swap(slot, NULL, m)) { pthread_mutex_destroy(m); free(m); }
    }
    return *slot;
}
static WINAPI void InitializeCriticalSection(void *cs) { memset(cs, 0, 40); cs_mutex(cs); }
static WINAPI BOOL InitializeCriticalSectionAndSpinCount(void *cs, DWORD spin) { (void)spin; InitializeCriticalSection(cs); return 1; }
static WINAPI BOOL InitializeCriticalSectionEx(void *cs, DWORD spin, DWORD flags) { (void)spin; (void)flags; InitializeCriticalSection(cs); return 1; }
static WINAPI void EnterCriticalSection(void *cs) { pthread_mutex_lock(cs_mutex(cs)); }
static WINAPI BOOL TryEnterCriticalSection(void *cs) { return pthread_mutex_trylock(cs_mutex(cs)) == 0; }
static WINAPI void LeaveCriticalSection(void *cs) { pthread_mutex_unlock(cs_mutex(cs)); }
static WINAPI void DeleteCriticalSection(void *cs) {
    pthread_mutex_t **slot = (pthread_mutex_t **)cs;
    if (*slot) { pthread_mutex_destroy(*slot); free(*slot); *slot = NULL; }
}
// SRW lock: one pointer, holding a pthread rwlock.
static pthread_rwlock_t *srw(void *l) {
    pthread_rwlock_t **slot = (pthread_rwlock_t **)l;
    if (!*slot) {
        pthread_rwlock_t *r = malloc(sizeof *r); pthread_rwlock_init(r, NULL);
        if (!__sync_bool_compare_and_swap(slot, NULL, r)) { pthread_rwlock_destroy(r); free(r); }
    }
    return *slot;
}
static WINAPI void InitializeSRWLock(void *l) { *(void **)l = NULL; }
static WINAPI void AcquireSRWLockExclusive(void *l) { pthread_rwlock_wrlock(srw(l)); }
static WINAPI void ReleaseSRWLockExclusive(void *l) { pthread_rwlock_unlock(srw(l)); }
static WINAPI void AcquireSRWLockShared(void *l) { pthread_rwlock_rdlock(srw(l)); }
static WINAPI void ReleaseSRWLockShared(void *l) { pthread_rwlock_unlock(srw(l)); }
static WINAPI BOOL TryAcquireSRWLockExclusive(void *l) { return pthread_rwlock_trywrlock(srw(l)) == 0; }
static WINAPI BOOL TryAcquireSRWLockShared(void *l) { return pthread_rwlock_tryrdlock(srw(l)) == 0; }

// ---- condition variables ------------------------------------------------------------------------------------------------------
// A CONDITION_VARIABLE (one pointer) holds a wake counter. A sleeper reads it while still holding its lock, so a
// wake that comes after the caller checked its predicate always changes the counter and is never missed. Every
// wake wakes all sleepers (Windows allows spurious wakeups).
static pthread_mutex_t g_cv_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv_changed = PTHREAD_COND_INITIALIZER;

static int cv_sleep(uint64_t *cv, uint64_t seen, DWORD ms) {
    struct timespec deadline;
    if (ms != INFINITE) deadline_after(&deadline, ms);
    int woken = 1;
    pthread_mutex_lock(&g_cv_lock);
    while (__atomic_load_n(cv, __ATOMIC_ACQUIRE) == seen) {
        if (ms == INFINITE) pthread_cond_wait(&g_cv_changed, &g_cv_lock);
        else if (ms == 0 || pthread_cond_timedwait(&g_cv_changed, &g_cv_lock, &deadline) == ETIMEDOUT) { woken = 0; break; }
    }
    pthread_mutex_unlock(&g_cv_lock);
    if (!woken) vx_set_last_error(1460 /* ERROR_TIMEOUT */);
    return woken;
}
static WINAPI void InitializeConditionVariable(uint64_t *cv) { *cv = 0; }
static WINAPI void WakeAllConditionVariable(uint64_t *cv) {
    pthread_mutex_lock(&g_cv_lock);
    __atomic_add_fetch(cv, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_cv_changed);
    pthread_mutex_unlock(&g_cv_lock);
}
static WINAPI void WakeConditionVariable(uint64_t *cv) { WakeAllConditionVariable(cv); }
static WINAPI BOOL SleepConditionVariableCS(uint64_t *cv, void *cs, DWORD ms) {
    uint64_t seen = __atomic_load_n(cv, __ATOMIC_ACQUIRE);
    LeaveCriticalSection(cs);
    int woken = cv_sleep(cv, seen, ms);
    EnterCriticalSection(cs);
    return woken;
}
static WINAPI BOOL SleepConditionVariableSRW(uint64_t *cv, void *l, DWORD ms, DWORD flags) {
    int shared = flags & 1;                    // CONDITION_VARIABLE_LOCKMODE_SHARED
    uint64_t seen = __atomic_load_n(cv, __ATOMIC_ACQUIRE);
    pthread_rwlock_unlock(srw(l));
    int woken = cv_sleep(cv, seen, ms);
    if (shared) AcquireSRWLockShared(l); else AcquireSRWLockExclusive(l);
    return woken;
}

// ---- one-time initialization ----------------------------------------------------------------------------------------------------
static pthread_mutex_t g_once_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
typedef BOOL (WINAPI *init_once_fn)(void *once, void *param, void **context);
static WINAPI void InitOnceInitialize(uint64_t *once) { *once = 0; }
static WINAPI BOOL InitOnceExecuteOnce(uint64_t *once, init_once_fn fn, void *param, void **context) {
    if (__atomic_load_n(once, __ATOMIC_ACQUIRE) == 2) return 1;
    pthread_mutex_lock(&g_once_lock);
    BOOL ok = 1;
    if (*once != 2) {
        ok = fn(once, param, context);
        if (ok) __atomic_store_n(once, 2, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_once_lock);
    return ok;
}

static const vx_export sync_k32[] = {
    VX_EXPORT(CreateThread), VX_EXPORT(ExitThread), VX_EXPORT(GetExitCodeThread), VX_EXPORT(GetThreadId),
    VX_EXPORT(ResumeThread), VX_EXPORT(SuspendThread), VX_EXPORT(GetThreadContext), VX_EXPORT(SetThreadContext),
    VX_EXPORT(GetThreadPriority), VX_EXPORT(SetThreadPriority), VX_EXPORT(SetThreadDescription), VX_EXPORT(SwitchToThread),
    VX_EXPORT(GetProcessAffinityMask), VX_EXPORT(SetProcessAffinityMask), VX_EXPORT(SetThreadAffinityMask), VX_EXPORT(OpenProcess),
    VX_EXPORT(CreateEventA), VX_EXPORT(CreateEventW), VX_EXPORT(CreateEventExW), VX_EXPORT(SetEvent), VX_EXPORT(ResetEvent),
    VX_EXPORT(CreateMutexA), VX_EXPORT(CreateMutexW), VX_EXPORT(ReleaseMutex),
    VX_EXPORT(CreateSemaphoreA), VX_EXPORT(CreateSemaphoreW), VX_EXPORT(CreateSemaphoreExW), VX_EXPORT(ReleaseSemaphore),
    VX_EXPORT(WaitForSingleObject), VX_EXPORT(WaitForSingleObjectEx), VX_EXPORT(WaitForMultipleObjects),
    VX_EXPORT(WaitForMultipleObjectsEx), VX_EXPORT(SleepEx), VX_EXPORT(CloseHandle), VX_EXPORT(DuplicateHandle),
    VX_EXPORT(InitializeCriticalSection), VX_EXPORT(InitializeCriticalSectionAndSpinCount), VX_EXPORT(InitializeCriticalSectionEx),
    VX_EXPORT(EnterCriticalSection), VX_EXPORT(TryEnterCriticalSection), VX_EXPORT(LeaveCriticalSection), VX_EXPORT(DeleteCriticalSection),
    VX_EXPORT(InitializeSRWLock), VX_EXPORT(AcquireSRWLockExclusive), VX_EXPORT(ReleaseSRWLockExclusive),
    VX_EXPORT(AcquireSRWLockShared), VX_EXPORT(ReleaseSRWLockShared),
    VX_EXPORT(TryAcquireSRWLockExclusive), VX_EXPORT(TryAcquireSRWLockShared),
    VX_EXPORT(InitializeConditionVariable), VX_EXPORT(WakeConditionVariable), VX_EXPORT(WakeAllConditionVariable),
    VX_EXPORT(SleepConditionVariableCS), VX_EXPORT(SleepConditionVariableSRW),
    VX_EXPORT(InitOnceInitialize), VX_EXPORT(InitOnceExecuteOnce),
};
static const vx_export sync_crt[] = { VX_EXPORT(_beginthreadex), VX_EXPORT(_endthreadex) };

void vx_sync_init(void) {
    vx_register_module("kernel32.dll", sync_k32, sizeof sync_k32 / sizeof sync_k32[0]);
    vx_register_module("kernelbase.dll", sync_k32, sizeof sync_k32 / sizeof sync_k32[0]);
    vx_register_module("ucrtbase.dll", sync_crt, sizeof sync_crt / sizeof sync_crt[0]);
    vx_register_module("msvcrt.dll", sync_crt, sizeof sync_crt / sizeof sync_crt[0]);
}
