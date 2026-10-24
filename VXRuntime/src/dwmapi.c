// dwmapi.dll, the Desktop Window Manager's API, for windows whose frames macOS draws. Chromium (the Windows Steam
// client's steamwebhelper) delay-loads it and ends when it cannot be loaded. From Microsoft's documentation of each
// function, for this runtime's windows:
//   - composition is always on (as on every Windows since 8, and as macOS always composites);
//   - DwmDefWindowProc handles nothing: hit testing in the frame is the program's own (and the Mac's, outside it);
//   - DwmExtendFrameIntoClientArea and the settable window attributes are accepted and change nothing: the frame
//     is the Mac's, which does not take Windows' frame styles (the attributes Windows 11 added fail, as on the
//     Windows 10 22H2 the runtime reports);
//   - DwmGetWindowAttribute answers what is known: the frame's bounds (the window rectangle), not cloaked, no
//     Windows non-client rendering;
//   - DwmGetCompositionTimingInfo gives the main display's refresh rate and the refresh count and time of the last
//     vertical blank as that rate computes them on QueryPerformanceCounter's clock, not measured blanks.
#include <mach/mach_time.h>
#include <string.h>
#include <CoreGraphics/CoreGraphics.h>
#include "ui.h"

typedef int32_t HRESULT;
#define S_OK 0
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_HANDLE ((HRESULT)0x80070006)
enum { DWMWA_NCRENDERING_ENABLED = 1, DWMWA_NCRENDERING_POLICY, DWMWA_TRANSITIONS_FORCEDISABLED, DWMWA_ALLOW_NCPAINT,
       DWMWA_CAPTION_BUTTON_BOUNDS, DWMWA_NONCLIENT_RTL_LAYOUT, DWMWA_FORCE_ICONIC_REPRESENTATION, DWMWA_FLIP3D_POLICY,
       DWMWA_EXTENDED_FRAME_BOUNDS, DWMWA_HAS_ICONIC_BITMAP, DWMWA_DISALLOW_PEEK, DWMWA_EXCLUDED_FROM_PEEK, DWMWA_CLOAK,
       DWMWA_CLOAKED, DWMWA_FREEZE_REPRESENTATION, DWMWA_PASSIVE_UPDATE_MODE, DWMWA_USE_HOSTBACKDROPBRUSH,
       DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1 = 19, DWMWA_USE_IMMERSIVE_DARK_MODE = 20 };

static BOOL (WINAPI *rt_GetWindowRect)(HANDLE, RECT *);

static WINAPI HRESULT DwmIsCompositionEnabled(BOOL *enabled) {
    if (!enabled) return E_INVALIDARG;
    *enabled = 1;
    return S_OK;
}
static WINAPI BOOL DwmDefWindowProc(HANDLE hwnd, uint32_t msg, WPARAM wp, LPARAM lp, LRESULT *result) {
    (void)hwnd; (void)msg; (void)wp; (void)lp;
    if (result) *result = 0;
    return 0;
}
static WINAPI HRESULT DwmExtendFrameIntoClientArea(HANDLE hwnd, const void *margins) {
    if (!vx_window_get(hwnd)) return E_HANDLE;
    return margins ? S_OK : E_INVALIDARG;
}
static WINAPI HRESULT DwmGetWindowAttribute(HANDLE hwnd, DWORD attribute, void *value, DWORD size) {
    if (!vx_window_get(hwnd)) return E_HANDLE;
    if (!value) return E_INVALIDARG;
    switch (attribute) {
    case DWMWA_NCRENDERING_ENABLED: case DWMWA_CLOAKED:          // BOOL / DWORD: no, not cloaked
        if (size != 4) return E_INVALIDARG;
        *(DWORD *)value = 0;
        return S_OK;
    case DWMWA_EXTENDED_FRAME_BOUNDS:                            // RECT in screen coordinates
        if (size != sizeof(RECT)) return E_INVALIDARG;
        return rt_GetWindowRect && rt_GetWindowRect(hwnd, value) ? S_OK : E_HANDLE;
    default:
        return E_INVALIDARG;
    }
}
static WINAPI HRESULT DwmSetWindowAttribute(HANDLE hwnd, DWORD attribute, const void *value, DWORD size) {
    if (!vx_window_get(hwnd)) return E_HANDLE;
    if (!value) return E_INVALIDARG;
    switch (attribute) {
    case DWMWA_NCRENDERING_POLICY: case DWMWA_TRANSITIONS_FORCEDISABLED: case DWMWA_ALLOW_NCPAINT:
    case DWMWA_NONCLIENT_RTL_LAYOUT: case DWMWA_FORCE_ICONIC_REPRESENTATION: case DWMWA_FLIP3D_POLICY:
    case DWMWA_HAS_ICONIC_BITMAP: case DWMWA_DISALLOW_PEEK: case DWMWA_EXCLUDED_FROM_PEEK: case DWMWA_CLOAK:
    case DWMWA_FREEZE_REPRESENTATION: case DWMWA_USE_IMMERSIVE_DARK_MODE_BEFORE_20H1: case DWMWA_USE_IMMERSIVE_DARK_MODE:
        return size == 4 ? S_OK : E_INVALIDARG;                  // (all of these take a BOOL or an enum)
    default:
        return E_INVALIDARG;
    }
}

static double refresh_hz(void) {
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    double hz = m ? CGDisplayModeGetRefreshRate(m) : 60;
    if (m) CGDisplayModeRelease(m);
    return hz > 1 ? hz : 60;                                     // (a display that does not say: 60, as user32.c takes it)
}
// DWM_TIMING_INFO (packed to 1 byte, 292 bytes): cbSize, rateRefresh {num, den}, qpcRefreshPeriod, rateCompose,
// qpcVBlank, cRefresh, cDXRefresh, qpcCompose, cFrame, ... QPC_TIME is QueryPerformanceCounter's (nanoseconds here).
static WINAPI HRESULT DwmGetCompositionTimingInfo(HANDLE hwnd, uint8_t *info) {
    if (hwnd || !info || *(uint32_t *)info != 292) return E_INVALIDARG;   // (Windows 8.1 and later: for the desktop only)
    memset(info + 4, 0, 288);
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    uint64_t now = mach_absolute_time() * tb.numer / tb.denom;
    double hz = refresh_hz();
    uint32_t rate = (uint32_t)(hz * 1000 + 0.5);
    uint64_t period = (uint64_t)(1e9 / hz + 0.5);
    memcpy(info + 4, &rate, 4); *(uint32_t *)(info + 8) = 1000;  // rateRefresh
    memcpy(info + 12, &period, 8);                               // qpcRefreshPeriod
    memcpy(info + 20, &rate, 4); *(uint32_t *)(info + 24) = 1000;   // rateCompose: composition follows the display
    uint64_t count = now / period, vblank = count * period;
    memcpy(info + 28, &vblank, 8);                               // qpcVBlank
    memcpy(info + 36, &count, 8);                                // cRefresh
    memcpy(info + 48, &vblank, 8);                               // qpcCompose
    memcpy(info + 56, &count, 8);                                // cFrame
    return S_OK;
}

static const vx_export dwmapi[] = {
    VX_EXPORT(DwmIsCompositionEnabled), VX_EXPORT(DwmDefWindowProc), VX_EXPORT(DwmExtendFrameIntoClientArea),
    VX_EXPORT(DwmGetWindowAttribute), VX_EXPORT(DwmSetWindowAttribute), VX_EXPORT(DwmGetCompositionTimingInfo),
};

void vx_dwmapi_init(void) {
    rt_GetWindowRect = (BOOL (WINAPI *)(HANDLE, RECT *))vx_find_export("user32.dll", "GetWindowRect");
    vx_register_module("dwmapi.dll", dwmapi, sizeof dwmapi / sizeof dwmapi[0]);
}

#ifdef __aarch64__
#include "thunks/dwmapi.c.inc"
#endif
