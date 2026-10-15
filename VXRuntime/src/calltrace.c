// VXRT_TRACE_CALLS=N: every call from Windows code into this runtime's functions is logged with its first four
// arguments, and its return value and last error, up to N times per function (200 for N < 10). Imports and
// GetProcAddress results go through a trampoline (context.S); functions that capture or switch stacks (setjmp,
// unwinding, fibers, thread and process exits) are left alone. A debugging aid: the return addresses it swaps
// are not visible to exception unwinding.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
    }
    return g_limit > 0;
}
static int left_alone(const char *name) {
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
// From vx_trace_enter: args points at the return address; rcx, rdx, r8, r9 were pushed just below it.
WINAPI void *vx_trace_called(int index, uint64_t *ret) {
    traced *t = &g_traced[index];
    int n = __sync_fetch_and_add(&t->calls, 1);
    if (n < g_limit) {
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
