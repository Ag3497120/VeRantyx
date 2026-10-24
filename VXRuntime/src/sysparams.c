// SystemParametersInfo's fonts and non-client metrics (SPI_GETNONCLIENTMETRICS, SPI_GETICONTITLELOGFONT,
// SPI_GETICONMETRICS) and SystemParametersInfoForDpi, which answers the same three for a DPI. Programs take their UI
// fonts from these: Chromium (the Windows Steam client's steamwebhelper) takes its default font from
// NONCLIENTMETRICS.lfMessageFont, and with the structure left unfilled it derived a 5-pixel font with no face over and
// over until its thread ran out of stack.
//
// The fonts: one face, 9 points (lfHeight -12 at 96 DPI, scaled for another DPI), regular weight, as Windows 10 sets
// every one of them; the face is Arial, which the Mac has and which this runtime's GDI and DirectWrite (the Mac's
// installed fonts) both find (Windows' own Segoe UI is not on a Mac). The sizes in pixels are GetSystemMetricsForDpi's
// (iScrollWidth is SM_CXVSCROLL, and so on), so the two never disagree.
#include <string.h>
#include "vx.h"

enum { SPI_GETICONTITLELOGFONT = 0x1f, SPI_GETNONCLIENTMETRICS = 0x29, SPI_GETICONMETRICS = 0x2d };
enum { SM_CXVSCROLL = 2, SM_CYHSCROLL = 3, SM_CXSIZE = 30, SM_CYSIZE = 31, SM_CXSIZEFRAME = 32, SM_CXSMSIZE = 52,
       SM_CYSMSIZE = 53, SM_CXMENUSIZE = 54, SM_CYMENUSIZE = 55, SM_CXICONSPACING = 38, SM_CYICONSPACING = 39,
       SM_CXPADDEDBORDER = 92 };
enum { ERROR_INVALID_PARAMETER = 87 };

static int (WINAPI *rt_GetSystemMetricsForDpi)(int, uint32_t);
static int metric(int i, uint32_t dpi) { return rt_GetSystemMetricsForDpi ? rt_GetSystemMetricsForDpi(i, dpi) : 0; }

// A LOGFONTA (60 bytes) or LOGFONTW (92) of the UI font at a DPI.
static size_t logfont(uint8_t *lf, int wide, uint32_t dpi) {
    size_t n = wide ? 92 : 60;
    memset(lf, 0, n);
    *(LONG *)lf = -(LONG)((12 * (int64_t)dpi + 48) / 96);             // 9 points
    *(LONG *)(lf + 16) = 400;                                          // FW_NORMAL
    lf[23] = 1;                                                        // DEFAULT_CHARSET
    lf[26] = 5;                                                        // CLEARTYPE_QUALITY
    static const char face[] = "Arial";
    for (size_t i = 0; i < sizeof face; i++) { if (wide) ((WCHAR *)(lf + 28))[i] = (WCHAR)face[i]; else lf[28 + i] = (uint8_t)face[i]; }
    return n;
}

// 1 or 0 for the actions here (FALSE with ERROR_INVALID_PARAMETER for a structure of the wrong size), -1 for others.
int vx_sysparams(uint32_t action, uint32_t param, void *data, int wide, uint32_t dpi) {
    (void)param;
    if (action != SPI_GETICONTITLELOGFONT && action != SPI_GETNONCLIENTMETRICS && action != SPI_GETICONMETRICS) return -1;
    if (!data) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    uint8_t *d = data;
    size_t lf = wide ? 92 : 60;
    if (action == SPI_GETICONTITLELOGFONT) { logfont(d, wide, dpi); return 1; }
    uint32_t size = *(uint32_t *)d;
    if (action == SPI_GETICONMETRICS) {                                // cbSize, iHorzSpacing, iVertSpacing, iTitleWrap, lfFont
        if (size != 16 + lf) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
        *(int *)(d + 4) = metric(SM_CXICONSPACING, dpi); *(int *)(d + 8) = metric(SM_CYICONSPACING, dpi); *(int *)(d + 12) = 1;
        logfont(d + 16, wide, dpi);
        return 1;
    }
    // NONCLIENTMETRICS: cbSize, iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight, lfCaptionFont,
    // iSmCaptionWidth, iSmCaptionHeight, lfSmCaptionFont, iMenuWidth, iMenuHeight, lfMenuFont, lfStatusFont,
    // lfMessageFont, and since Windows Vista iPaddedBorderWidth (a structure without it is accepted too).
    size_t full = 24 + lf + 8 + lf + 8 + 3 * lf + 4;
    if (size != full && size != full - 4) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    int *head = (int *)(d + 4);
    head[0] = metric(SM_CXSIZEFRAME, dpi); head[1] = metric(SM_CXVSCROLL, dpi); head[2] = metric(SM_CYHSCROLL, dpi);
    head[3] = metric(SM_CXSIZE, dpi); head[4] = metric(SM_CYSIZE, dpi);
    uint8_t *p = d + 24;
    p += logfont(p, wide, dpi);
    ((int *)p)[0] = metric(SM_CXSMSIZE, dpi); ((int *)p)[1] = metric(SM_CYSMSIZE, dpi); p += 8;
    p += logfont(p, wide, dpi);
    ((int *)p)[0] = metric(SM_CXMENUSIZE, dpi); ((int *)p)[1] = metric(SM_CYMENUSIZE, dpi); p += 8;
    for (int i = 0; i < 3; i++) p += logfont(p, wide, dpi);
    if (size == full) *(int *)p = metric(SM_CXPADDEDBORDER, dpi);
    return 1;
}

// Windows 10 1607 and later: only the three actions above, for a DPI.
static WINAPI BOOL SystemParametersInfoForDpi(uint32_t action, uint32_t param, void *data, uint32_t flags, uint32_t dpi) {
    (void)flags;
    int r = vx_sysparams(action, param, data, 1, dpi);
    if (r < 0) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    return r;
}

static const vx_export user32[] = { VX_EXPORT(SystemParametersInfoForDpi) };

void vx_sysparams_init(void) {
    rt_GetSystemMetricsForDpi = (int (WINAPI *)(int, uint32_t))vx_find_export("user32.dll", "GetSystemMetricsForDpi");
    vx_register_module("user32.dll", user32, 1);
}

#ifdef __aarch64__
#include "thunks/sysparams.c.inc"
#endif
