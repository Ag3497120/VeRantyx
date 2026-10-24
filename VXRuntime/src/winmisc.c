// Smaller user32 functions Chromium (the Windows Steam client's steamwebhelper) imports and the runtime lacked; a
// missing one ends the program when its delay-load stub cannot find it. From Microsoft's documentation of each:
//   GetWindowRgn/GetWindowRgnBox  windows here are their rectangles: SetWindowRgn (src/desktop.c) does not shape the
//                        Mac window, so no window has a region (ERROR, as for a window without one)
//   IsChild              the chain of parents from a child window (WS_CHILD) up to its top-level window
//   InflateRect, EqualRect, CharUpperW, CharNextW   (UTF-16; CharNext steps over a low surrogate after a high one)
//   GetLastInputInfo     the time of the Mac's last keyboard or mouse input, on GetTickCount's clock
//   BeginDeferWindowPos, DeferWindowPos, EndDeferWindowPos   the moves collected, then made with SetWindowPos
//   NotifyWinEvent       accessibility events: SetWinEventHook (src/client.c) delivers none, so nobody listens
#include <pthread.h>
#include <stdlib.h>
#include <wctype.h>
#include <xlocale.h>
#include <CoreGraphics/CoreGraphics.h>
#include "ui.h"

enum { ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_INVALID_PARAMETER = 87, ERROR_INVALID_WINDOW_HANDLE = 1400, ERROR_INVALID_HANDLE = 6 };
#define WS_CHILD 0x40000000u

static BOOL (WINAPI *rt_SetWindowPos)(HANDLE, HANDLE, int, int, int, int, uint32_t);
static DWORD (WINAPI *rt_GetTickCount)(void);

static WINAPI int GetWindowRgn(HANDLE hwnd, HANDLE rgn) {
    (void)rgn;
    if (!vx_window_get(hwnd)) vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
    return 0;                                                       // ERROR: no window region
}
static WINAPI int GetWindowRgnBox(HANDLE hwnd, RECT *r) {
    (void)r;
    if (!vx_window_get(hwnd)) vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
    return 0;
}
static WINAPI BOOL IsChild(HANDLE parent, HANDLE hwnd) {
    vx_window *w = vx_window_get(hwnd);
    for (int depth = 0; w && (w->style & WS_CHILD) && w->parent && depth < 4096; depth++) {
        if (w->parent == parent) return 1;
        w = vx_window_get(w->parent);
    }
    return 0;
}
static WINAPI BOOL InflateRect(RECT *r, int dx, int dy) {
    if (!r) return 0;
    r->left -= dx; r->right += dx; r->top -= dy; r->bottom += dy;
    return 1;
}
static WINAPI BOOL EqualRect(const RECT *a, const RECT *b) {
    if (!a || !b) return 0;
    return a->left == b->left && a->top == b->top && a->right == b->right && a->bottom == b->bottom;
}
// A string converted in place, or (a pointer below 64K) one character in the low word. Unicode's case mapping (a
// UTF-8 locale's: the C locale's towupper knows ASCII only), character by character as Windows does.
static locale_t g_unicode;
static pthread_once_t g_unicode_once = PTHREAD_ONCE_INIT;
static void make_unicode(void) { g_unicode = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", NULL); }
static WCHAR upper(WCHAR c) {
    pthread_once(&g_unicode_once, make_unicode);
    if (c >= 0xd800 && c <= 0xdfff) return c;
    wint_t u = g_unicode ? towupper_l(c, g_unicode) : towupper(c);
    return u <= 0xffff ? (WCHAR)u : c;
}
static WINAPI WCHAR *CharUpperW(WCHAR *s) {
    if ((uintptr_t)s < 0x10000) return (WCHAR *)(uintptr_t)upper((WCHAR)(uintptr_t)s);
    for (WCHAR *p = s; *p; p++) *p = upper(*p);
    return s;
}
static WINAPI WCHAR *CharNextW(const WCHAR *s) {
    if (!*s) return (WCHAR *)s;
    if (s[0] >= 0xd800 && s[0] < 0xdc00 && s[1] >= 0xdc00 && s[1] < 0xe000) return (WCHAR *)s + 2;
    return (WCHAR *)s + 1;
}
// LASTINPUTINFO: cbSize (8), dwTime.
static WINAPI BOOL GetLastInputInfo(uint32_t *info) {
    if (!info || info[0] != 8) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    double idle = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
    if (!(idle >= 0)) idle = 0;
    info[1] = rt_GetTickCount() - (DWORD)(idle * 1000);
    return 1;
}

// ---- deferred window positions -----------------------------------------------------------------------------------------------
typedef struct { HANDLE hwnd, after; int x, y, cx, cy; uint32_t flags; } deferred;
typedef struct { uint32_t magic; int n, cap; deferred *v; } defer_list;
#define DEFER_MAGIC 0x44504f53u
static WINAPI HANDLE BeginDeferWindowPos(int count) {
    if (count < 0) { vx_set_last_error(ERROR_INVALID_PARAMETER); return NULL; }
    defer_list *l = calloc(1, sizeof *l);
    if (!l) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    l->magic = DEFER_MAGIC;
    return l;
}
static void defer_free(defer_list *l) { l->magic = 0; free(l->v); free(l); }
// On a failure the whole set is dropped (the handle is gone), as Windows does.
static WINAPI HANDLE DeferWindowPos(HANDLE h, HANDLE hwnd, HANDLE after, int x, int y, int cx, int cy, uint32_t flags) {
    defer_list *l = h;
    if (!l || l->magic != DEFER_MAGIC) { vx_set_last_error(ERROR_INVALID_HANDLE); return NULL; }
    if (!vx_window_get(hwnd)) { defer_free(l); vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return NULL; }
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        deferred *v = realloc(l->v, (size_t)cap * sizeof *v);
        if (!v) { defer_free(l); vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        l->v = v; l->cap = cap;
    }
    l->v[l->n++] = (deferred){ hwnd, after, x, y, cx, cy, flags };
    return l;
}
static WINAPI BOOL EndDeferWindowPos(HANDLE h) {
    defer_list *l = h;
    if (!l || l->magic != DEFER_MAGIC) { vx_set_last_error(ERROR_INVALID_HANDLE); return 0; }
    BOOL ok = 1;
    for (int i = 0; i < l->n; i++)
        if (!rt_SetWindowPos(l->v[i].hwnd, l->v[i].after, l->v[i].x, l->v[i].y, l->v[i].cx, l->v[i].cy, l->v[i].flags)) ok = 0;
    defer_free(l);
    return ok;
}

static WINAPI void NotifyWinEvent(DWORD event, HANDLE hwnd, LONG object, LONG child) { (void)event; (void)hwnd; (void)object; (void)child; }

static const vx_export user32[] = {
    VX_EXPORT(GetWindowRgn), VX_EXPORT(GetWindowRgnBox), VX_EXPORT(IsChild), VX_EXPORT(InflateRect), VX_EXPORT(EqualRect),
    VX_EXPORT(CharUpperW), VX_EXPORT(CharNextW), VX_EXPORT(GetLastInputInfo), VX_EXPORT(BeginDeferWindowPos),
    VX_EXPORT(DeferWindowPos), VX_EXPORT(EndDeferWindowPos), VX_EXPORT(NotifyWinEvent),
};

void vx_winmisc_init(void) {
    rt_SetWindowPos = (BOOL (WINAPI *)(HANDLE, HANDLE, int, int, int, int, uint32_t))vx_find_export("user32.dll", "SetWindowPos");
    rt_GetTickCount = (DWORD (WINAPI *)(void))vx_find_export("kernel32.dll", "GetTickCount");
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
}

#ifdef __aarch64__
#include "thunks/winmisc.c.inc"
#endif
