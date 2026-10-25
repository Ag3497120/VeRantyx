// winmm (timers; audio and joysticks report no devices until they are implemented).
#include <mach/mach_time.h>
#include "vx.h"

static WINAPI DWORD timeGetTime(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (DWORD)(mach_absolute_time() * tb.numer / tb.denom / 1000000);
}
static WINAPI uint32_t timeBeginPeriod(uint32_t ms) { (void)ms; return 0; }     // TIMERR_NOERROR: timers are already precise
static WINAPI uint32_t timeEndPeriod(uint32_t ms) { (void)ms; return 0; }
static WINAPI uint32_t timeGetDevCaps(uint32_t *caps, uint32_t size) { if (size >= 8) { caps[0] = 1; caps[1] = 1000000; } return 0; }
#define MMSYSERR_NODRIVER 6
#define JOYERR_UNPLUGGED 167
static WINAPI uint32_t waveOutGetNumDevs(void) { return 0; }
static WINAPI uint32_t waveOutOpen(void *h, uint32_t dev, const void *fmt, uintptr_t cb, uintptr_t inst, DWORD flags) { (void)h; (void)dev; (void)fmt; (void)cb; (void)inst; (void)flags; return MMSYSERR_NODRIVER; }
static WINAPI uint32_t waveInGetNumDevs(void) { return 0; }
static WINAPI uint32_t midiOutGetNumDevs(void) { return 0; }
static WINAPI uint32_t joyGetNumDevs(void) { return 16; }
static WINAPI uint32_t joyGetPosEx(uint32_t id, void *info) { (void)id; (void)info; return JOYERR_UNPLUGGED; }
static WINAPI uint32_t joyGetPos(uint32_t id, void *info) { (void)id; (void)info; return JOYERR_UNPLUGGED; }
static WINAPI uint32_t joyGetDevCapsA(uintptr_t id, void *caps, uint32_t size) { (void)id; (void)caps; (void)size; return JOYERR_UNPLUGGED; }
static WINAPI uint32_t joyGetDevCapsW(uintptr_t id, void *caps, uint32_t size) { (void)id; (void)caps; (void)size; return JOYERR_UNPLUGGED; }
static WINAPI BOOL PlaySoundA(const char *s, HANDLE m, DWORD f) { (void)s; (void)m; (void)f; return 0; }
static WINAPI BOOL PlaySoundW(const WCHAR *s, HANDLE m, DWORD f) { (void)s; (void)m; (void)f; return 0; }

static const vx_export winmm[] = {
    VX_EXPORT(timeGetTime), VX_EXPORT(timeBeginPeriod), VX_EXPORT(timeEndPeriod), VX_EXPORT(timeGetDevCaps),
    VX_EXPORT(waveOutGetNumDevs), VX_EXPORT(waveOutOpen), VX_EXPORT(waveInGetNumDevs), VX_EXPORT(midiOutGetNumDevs),
    VX_EXPORT(joyGetNumDevs), VX_EXPORT(joyGetPosEx), VX_EXPORT(joyGetPos), VX_EXPORT(joyGetDevCapsA), VX_EXPORT(joyGetDevCapsW),
    VX_EXPORT(PlaySoundA), VX_EXPORT(PlaySoundW),
};

// (imm32: src/ime.c)

void vx_mm_init(void) {
    vx_register_module("winmm.dll", winmm, sizeof winmm / sizeof winmm[0]);
}

#ifdef __aarch64__
#include "thunks/mm.c.inc"
#endif
