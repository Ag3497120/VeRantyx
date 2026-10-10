// KLVR_TRACE_CALLS=1: every served interface gets a vtable of trampolines that
// count calls per slot, log the first call of each slot by symbol name, and
// dump per-slot counts every 5 s. Debug aid for "what is the game polling".
// The trampolines are Apple arm64 assembly; elsewhere (the Linux host tests)
// trace_wrap hands the object back untouched.
#include "klvr_internal.h"
#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

#define KLVR_TR_MAX 2048
extern "C" {
void *klvr_tr_real[KLVR_TR_MAX];
static const char *g_tr_iface[KLVR_TR_MAX];
static uint32_t g_tr_slot[KLVR_TR_MAX];
static std::atomic<uint64_t> g_tr_count[KLVR_TR_MAX];
static uint64_t g_tr_last[KLVR_TR_MAX];
void *klvr_tr_hit(uint64_t i);
extern const char klvr_tr_stubs[];
}

__asm__(
    ".text\n.p2align 3\n"
    ".globl _klvr_tr_stubs\n_klvr_tr_stubs:\n"
    ".set i, 0\n"
    ".rept 2048\n"
    "  movz x16, #i\n  b _klvr_tr_common\n"
    "  .set i, i+1\n"
    ".endr\n"
    "_klvr_tr_common:\n"
    "  stp x29, x30, [sp, #-224]!\n  mov x29, sp\n"
    "  stp x0, x1, [sp, #16]\n  stp x2, x3, [sp, #32]\n  stp x4, x5, [sp, #48]\n"
    "  stp x6, x7, [sp, #64]\n  str x8, [sp, #80]\n"
    "  stp q0, q1, [sp, #96]\n  stp q2, q3, [sp, #128]\n  stp q4, q5, [sp, #160]\n  stp q6, q7, [sp, #192]\n"
    "  mov x0, x16\n  bl _klvr_tr_hit\n  mov x16, x0\n"
    "  ldp x0, x1, [sp, #16]\n  ldp x2, x3, [sp, #32]\n  ldp x4, x5, [sp, #48]\n"
    "  ldp x6, x7, [sp, #64]\n  ldr x8, [sp, #80]\n"
    "  ldp q0, q1, [sp, #96]\n  ldp q2, q3, [sp, #128]\n  ldp q4, q5, [sp, #160]\n  ldp q6, q7, [sp, #192]\n"
    "  ldp x29, x30, [sp], #224\n  br x16\n");

static const char *sym(void *p) {
    Dl_info d;
    return dladdr(p, &d) && d.dli_sname ? d.dli_sname : "?";
}

static double now_s() {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static std::atomic<uint32_t> g_tr_n;

extern "C" void *klvr_tr_hit(uint64_t i) {
    if (g_tr_count[i].fetch_add(1) == 0)
        fprintf(stderr, "  [openvr-trace] %.3f first call %s slot %u %s\n", now_s(), g_tr_iface[i], g_tr_slot[i],
                sym(klvr_tr_real[i]));
    return klvr_tr_real[i];
}

static void dumper() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        uint32_t n = g_tr_n.load();
        fprintf(stderr, "  [openvr-trace] --- calls in the last 5 s ---\n");
        for (uint32_t i = 0; i < n; i++) {
            uint64_t c = g_tr_count[i].load(), d = c - g_tr_last[i];
            g_tr_last[i] = c;
            if (d) fprintf(stderr, "  [openvr-trace]   %6llu  %s slot %u %s\n", (unsigned long long)d, g_tr_iface[i],
                           g_tr_slot[i], sym(klvr_tr_real[i]));
        }
    }
}

namespace klvr {
void *trace_wrap(void *obj, const char *version, uint32_t slots) {
    static bool on = getenv("KLVR_TRACE_CALLS") && getenv("KLVR_TRACE_CALLS")[0] == '1';
    if (!on || !obj) return obj;
    static std::mutex mu;
    static std::once_flag once;
    std::call_once(once, [] { std::thread(dumper).detach(); });
    std::lock_guard<std::mutex> l(mu);
    uint32_t base = g_tr_n.load();
    if (base + slots > KLVR_TR_MAX) return obj;
    const void *const *real = *(const void *const **)obj;
    const void **vt = (const void **)calloc(slots, sizeof(void *));
    for (uint32_t s = 0; s < slots; s++) {
        klvr_tr_real[base + s] = (void *)real[s];
        g_tr_iface[base + s] = strdup(version);
        g_tr_slot[base + s] = s;
        vt[s] = klvr_tr_stubs + 8 * (base + s);
    }
    g_tr_n.store(base + slots);
    void **o = (void **)calloc(1, sizeof(void *));
    *o = (void *)vt;
    return o;
}
} // namespace klvr
#else
void *klvr::trace_wrap(void *obj, const char *, uint32_t) { return obj; }
#endif
