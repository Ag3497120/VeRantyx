// kernel32's interrupt-time clocks (realtimeapiset.h), which Chromium (the Windows Steam client's steamwebhelper)
// reads through api-ms-win-core-realtime-l1-1-1: the time since the machine started, in 100 ns units. "Unbiased"
// leaves out the time the machine was asleep, like macOS's mach_absolute_time (which QueryPerformanceCounter in
// src/kernel32.c also counts); the plain interrupt time includes it, like mach_continuous_time. The Precise forms read
// the clock now; the others may be a timer tick (about 15.6 ms on Windows) old, which reading it now also satisfies.
#include <mach/mach_time.h>
#include "vx.h"

static uint64_t to_100ns(uint64_t ticks) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (uint64_t)((__uint128_t)ticks * tb.numer / tb.denom / 100);
}
static WINAPI void QueryUnbiasedInterruptTimePrecise(uint64_t *t) { *t = to_100ns(mach_absolute_time()); }
static WINAPI void QueryInterruptTimePrecise(uint64_t *t) { *t = to_100ns(mach_continuous_time()); }
static WINAPI void QueryInterruptTime(uint64_t *t) { *t = to_100ns(mach_continuous_time()); }
static WINAPI BOOL QueryUnbiasedInterruptTime(uint64_t *t) {
    if (!t) { vx_set_last_error(87 /* ERROR_INVALID_PARAMETER */); return 0; }
    *t = to_100ns(mach_absolute_time());
    return 1;
}

static const vx_export kernel32[] = {
    VX_EXPORT(QueryUnbiasedInterruptTimePrecise), VX_EXPORT(QueryInterruptTimePrecise), VX_EXPORT(QueryInterruptTime),
    VX_EXPORT(QueryUnbiasedInterruptTime),
};

void vx_ktime_init(void) {
    vx_register_module("kernel32.dll", kernel32, sizeof kernel32 / sizeof kernel32[0]);
    vx_register_module("kernelbase.dll", kernel32, sizeof kernel32 / sizeof kernel32[0]);
}

#ifdef __aarch64__
#include "thunks/ktime.c.inc"
#endif
