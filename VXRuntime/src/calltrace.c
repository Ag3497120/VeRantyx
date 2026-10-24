// VXRT_TRACE_CALLS=N (VXRT_TRACE_CALLS_FOR=<program.exe> limits it to one program): every call from Windows code into this runtime's functions is logged with its first four
// arguments, and its return value and last error, up to N times per function (200 for N < 10). Imports and
// GetProcAddress results go through a trampoline (context.S); functions that capture or switch stacks (setjmp,
// unwinding, fibers, thread and process exits) are left alone. A debugging aid: the return addresses it swaps
// are not visible to exception unwinding.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "vx.h"

#define MAX_TRACED 8192
typedef struct { const char *dll, *name; void *target; int calls; } traced;
static traced g_traced[MAX_TRACED];
static int g_count, g_limit = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
extern uint8_t vx_trace_stubs[];
void vx_trace_return(void);

typedef struct { uint64_t ret; int index; uint64_t start; } frame;
static __thread frame t_frames[1024];
static __thread int t_depth;

int vx_trace_calls_on(void) {
    if (g_limit < 0) {
        const char *e = getenv("VXRT_TRACE_CALLS");
        int n = e ? atoi(e) : 0;
        g_limit = n <= 0 ? 0 : n < 10 ? 200 : n;
        // VXRT_TRACE_CALLS_FOR=<program.exe>: only that program (a launcher's game, not the launcher and SteamVR
        // started with the same environment)
        const char *only = getenv("VXRT_TRACE_CALLS_FOR");
        if (g_limit && only && *only) {
            const char *b = vx_proc.argv && vx_proc.argv[0] ? vx_proc.argv[0] : "";
            for (const char *c = b; *c; c++) if (*c == '\\' || *c == '/') b = c + 1;
            if (strcasecmp(b, only)) g_limit = 0;
        }
    }
    return g_limit > 0;
}
static int ring_on(void);
static int left_alone(const char *name) {
    // (kept in a ring, the waits are what a hung thread's last call shows: only their logging is too much)
    if (ring_on() && (!strcmp(name, "WaitForSingleObject") || !strcmp(name, "Sleep") || !strcmp(name, "EnterCriticalSection") ||
                      !strcmp(name, "AcquireSRWLockExclusive") || !strcmp(name, "AcquireSRWLockShared"))) return 0;
    static const char *const prefixes[] = {
        "Rtl", "_setjmp", "setjmp", "longjmp", "_longjmp", "__C_specific_handler", "__CxxFrameHandler", "_CxxThrowException",
        "__chkstk", "_chkstk", "RaiseException", "ExitThread", "ExitProcess", "TerminateThread", "TerminateProcess",
        "FreeLibraryAndExitThread", "SwitchToFiber", "CreateFiber", "ConvertThreadToFiber", "ConvertFiberToThread", "DeleteFiber",
        "_local_unwind", "__std_terminate", "terminate", "abort", "_exit", "exit", "_cexit", "__CxxThrow", "_purecall",
        "GetLastError", "SetLastError", "TlsGetValue", "FlsGetValue", "EnterCriticalSection", "LeaveCriticalSection",
        "Interlocked", "AcquireSRW", "ReleaseSRW", "TryAcquireSRW", "TryEnterCriticalSection", "QueryPerformanceCounter",
        "GetTickCount", "GetCurrentThreadId", "GetCurrentProcess", "GetCurrentThread", "HeapAlloc", "HeapFree", "HeapReAlloc",
        "HeapSize", "malloc", "free", "realloc", "calloc", "_malloc", "_free", "memcpy", "memset", "memmove", "memcmp", "str", "wcs",
        "__acrt", "__stdio", "_errno", "__p_", "_lock", "_unlock", "WaitForSingleObject", "Sleep",
    };
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return 1;
    return 0;
}
// The trampoline for a function (the function itself when it is left alone or the table is full).
void *vx_trace_wrap(const char *dll, const char *name, void *fn) {
#ifdef __aarch64__
    (void)dll; (void)name; return fn;          // (ARM64: not yet)
#endif
    if (!fn || !name || !vx_trace_calls_on() || left_alone(name)) return fn;
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < g_count && g_traced[i].target != fn) i++;
    if (i == g_count && g_count < MAX_TRACED) { g_traced[i] = (traced){ dll, strdup(name), fn, 0 }; g_count++; }
    pthread_mutex_unlock(&g_lock);
    return i < MAX_TRACED ? vx_trace_stubs + 16 * i : fn;
}
// VXRT_TRACE_CALLS_RING=1: the calls are not logged as they happen but kept, the last RING_SIZE of each thread, and
// printed when that thread faults (VXRT_TRACE_SEH=1) or ends the process (TerminateProcess on itself); =2 prints
// every thread's of the last 300 ms, in time order. A run keeps close to its untraced timing, which logging every
// call changes enough to hide a race (Steam's main thread losing its data to a new thread's check: src/sync.c).
#define RING_SIZE 2048
typedef struct { uint64_t start, a[4], from, result; int index; DWORD err; int returned; } ring_entry;
typedef struct { unsigned next; DWORD tid; ring_entry e[RING_SIZE]; } ring;   // (outlives its thread: never freed)
static __thread ring *t_ring;
static ring *g_rings[1024];                                                    // every thread's (VXRT_TRACE_CALLS_RING=2)
static int g_ring_count;
static int ring_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("VXRT_TRACE_CALLS_RING"); on = e && (*e == '1' || *e == '2'); }
    return on;
}
static ring_entry *ring_add(void) {
    if (!t_ring) {
        t_ring = calloc(1, sizeof *t_ring);
        if (!t_ring) return NULL;
        t_ring->tid = vx_current_tid();
        int k = __atomic_fetch_add(&g_ring_count, 1, __ATOMIC_ACQ_REL);
        if (k < 1024) __atomic_store_n(&g_rings[k], t_ring, __ATOMIC_RELEASE);
    }
    return &t_ring->e[t_ring->next++ % RING_SIZE];
}
static void ring_line(DWORD tid, const ring_entry *e) {
    const traced *t = &g_traced[e->index];
    vx_log("  %llu.%06llu [%u] %s!%s(%#llx, %#llx, %#llx, %#llx) from %p%s%#llx (last error %u)",
           (unsigned long long)(e->start / 1000000000), (unsigned long long)(e->start / 1000 % 1000000), tid, t->dll, t->name,
           (unsigned long long)e->a[0], (unsigned long long)e->a[1], (unsigned long long)e->a[2], (unsigned long long)e->a[3],
           (void *)e->from, e->returned ? " = " : " (no return yet) ", (unsigned long long)e->result, e->err);
}
static void ring_dump_all(uint64_t ms) {
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), from = now - ms * 1000000ull;
    int n = g_ring_count < 1024 ? g_ring_count : 1024;
    unsigned *pos = calloc((size_t)n + 1, sizeof *pos), *end = calloc((size_t)n + 1, sizeof *end);
    for (int i = 0; i < n; i++) {
        ring *r = __atomic_load_n(&g_rings[i], __ATOMIC_ACQUIRE);
        unsigned next = r ? r->next : 0, have = next < RING_SIZE ? next : RING_SIZE;
        pos[i] = next - have; end[i] = next;
        while (pos[i] != end[i] && r->e[pos[i] % RING_SIZE].start < from) pos[i]++;
    }
    for (;;) {
        int best = -1; uint64_t bt = ~0ull;
        for (int i = 0; i < n; i++)
            if (pos[i] != end[i] && g_rings[i]->e[pos[i] % RING_SIZE].start < bt) { bt = g_rings[i]->e[pos[i] % RING_SIZE].start; best = i; }
        if (best < 0) break;
        ring_line(g_rings[best]->tid, &g_rings[best]->e[pos[best]++ % RING_SIZE]);
    }
    free(pos); free(end);
}
// VXRT_TRACE_CALLS_RING_AT=<seconds>: at that time after the start, each thread's last calls (a call that has not
// returned is where the thread waits): what a hung program is doing.
static void *ring_dump_later(void *arg) {
    sleep((unsigned)(uintptr_t)arg);
    int n = g_ring_count < 1024 ? g_ring_count : 1024;
    vx_log("each thread's last calls into the runtime, %u s after the start:", (unsigned)(uintptr_t)arg);
    for (int i = 0; i < n; i++) {
        ring *r = __atomic_load_n(&g_rings[i], __ATOMIC_ACQUIRE);
        unsigned next = r ? r->next : 0, have = next < 12 ? next : 12;
        for (unsigned k = next - have; k != next; k++) ring_line(r->tid, &r->e[k % RING_SIZE]);
    }
    return NULL;
}
__attribute__((constructor)) static void ring_dump_at(void) {
    const char *e = getenv("VXRT_TRACE_CALLS_RING_AT");
    if (!e || atoi(e) <= 0) return;
    pthread_t th; pthread_create(&th, NULL, ring_dump_later, (void *)(uintptr_t)atoi(e)); pthread_detach(th);
}
void vx_trace_ring_dump(const char *why) {
    const char *e = getenv("VXRT_TRACE_CALLS_RING");
    if (e && *e == '2') { vx_log("[%u] every thread's calls into the runtime in the 300 ms before %s:", vx_current_tid(), why); ring_dump_all(300); return; }
    if (!t_ring) return;
    unsigned n = t_ring->next < RING_SIZE ? t_ring->next : RING_SIZE;
    vx_log("[%u] the last %u calls into the runtime before %s:", vx_current_tid(), n, why);
    for (unsigned k = t_ring->next - n; k != t_ring->next; k++) ring_line(t_ring->tid, &t_ring->e[k % RING_SIZE]);
}
// From vx_trace_enter: args points at the return address; rcx, rdx, r8, r9 were pushed just below it.
WINAPI void *vx_trace_called(int index, uint64_t *ret) {
    traced *t = &g_traced[index];
    int n = __sync_fetch_and_add(&t->calls, 1);
    if (n < g_limit && ring_on()) {
        ring_entry *e = ring_add();
        if (e) *e = (ring_entry){ clock_gettime_nsec_np(CLOCK_UPTIME_RAW), { ret[-1], ret[-2], ret[-3], ret[-4] }, *ret, 0, index, 0, 0 };
        if (e && t_depth < 1024) {
            t_frames[t_depth++] = (frame){ *ret, index, (uint64_t)(uintptr_t)e };
            *ret = (uint64_t)(uintptr_t)vx_trace_return;
        }
    } else if (n < g_limit) {
        vx_log("[%u] %s!%s(%#llx, %#llx, %#llx, %#llx) from %p", vx_current_tid(), t->dll, t->name,
               (unsigned long long)ret[-1], (unsigned long long)ret[-2], (unsigned long long)ret[-3], (unsigned long long)ret[-4], (void *)*ret);
        if (t_depth < 1024) {
            t_frames[t_depth++] = (frame){ *ret, index, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) };
            *ret = (uint64_t)(uintptr_t)vx_trace_return;
        }
    }
    return t->target;
}
WINAPI uint64_t vx_trace_returned(uint64_t result) {
    frame f = t_frames[--t_depth];
    DWORD err = vx_get_last_error();
    if (ring_on()) {                                   // (start holds the ring entry; it may have been reused: same call?)
        ring_entry *e = (ring_entry *)(uintptr_t)f.start;
        if (e->from == f.ret && e->index == f.index) { e->result = result; e->err = err; e->returned = 1; }
        return f.ret;
    }
    vx_log("[%u]   %s = %#llx (last error %u) %llu us", vx_current_tid(), g_traced[f.index].name, (unsigned long long)result, err,
           (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - f.start) / 1000));
    return f.ret;
}

#ifdef __aarch64__
// ARM64: the thunks call these (src/arm64.h), the same lines as above. Functions counted by name.
#include "arm64.h"
int vx_tracing;
__attribute__((constructor)) static void trace_init(void) { vx_tracing = vx_trace_calls_on(); }
static __thread int t_skip[64], t_level;                             // (calls past the limit: their results unlogged)
static int count_call(const char *name) {
    enum { SLOTS = 8192 };
    static struct { const char *name; int calls; } slots[SLOTS];
    uint32_t h = (uint32_t)(((uintptr_t)name >> 3) * 0x9E3779B1u) & (SLOTS - 1);
    for (int n = 0; n < SLOTS; n++, h = (h + 1) & (SLOTS - 1)) {
        const char *s = __atomic_load_n(&slots[h].name, __ATOMIC_ACQUIRE);
        if (!s) { const char *none = NULL; if (!__atomic_compare_exchange_n(&slots[h].name, &none, name, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && none != name) continue; s = name; }
        if (s == name) return __atomic_fetch_add(&slots[h].calls, 1, __ATOMIC_RELAXED);
    }
    return g_limit;
}
void vx_trace_in(const char *name, const x86_ctx *c) {
    int skip = left_alone(name) || count_call(name) >= g_limit;
    if (t_level < 64) t_skip[t_level] = skip;
    t_level++;
    if (skip) return;
    vx_log("[%u] %s(%#llx, %#llx, %#llx, %#llx) from %#llx", vx_current_tid(), name, (unsigned long long)c->gpr[1], (unsigned long long)c->gpr[2],
           (unsigned long long)c->gpr[8], (unsigned long long)c->gpr[9], (unsigned long long)*(const uint64_t *)(uintptr_t)c->gpr[4]);
}
void vx_trace_out(const char *name, const x86_ctx *c) {
    if (t_level > 0) t_level--;
    if (t_level < 64 && t_skip[t_level]) return;
    vx_log("[%u]   %s = %#llx (last error %u)", vx_current_tid(), name, (unsigned long long)c->gpr[0], vx_get_last_error());
}
#include "thunks/calltrace.c.inc"
#endif
