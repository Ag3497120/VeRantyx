// Smaller user32 functions Chromium (the Windows Steam client's steamwebhelper) imports and the runtime lacked; a
// missing one ends the program when its delay-load stub cannot find it. From Microsoft's documentation of each:
//   GetWindowRgn/GetWindowRgnBox  windows here are their rectangles: SetWindowRgn (src/desktop.c) does not shape the
//                        Mac window, so no window has a region (ERROR, as for a window without one)
//   IsChild              the chain of parents from a child window (WS_CHILD) up to its top-level window
//   InflateRect, EqualRect, CharUpperW, CharNextW   (UTF-16; CharNext steps over a low surrogate after a high one)
//   GetLastInputInfo     the time of the Mac's last keyboard or mouse input, on GetTickCount's clock
//   BeginDeferWindowPos, DeferWindowPos, EndDeferWindowPos   the moves collected, then made with SetWindowPos
//   NotifyWinEvent       accessibility events: SetWinEventHook (src/client.c) delivers none, so nobody listens
//   ShutdownBlockReasonCreate / Destroy   the reason a window gives for holding up a shutdown, kept per window and
//                        only from the window's own thread (ERROR_ACCESS_DENIED otherwise); macOS asks programs about
//                        logging out its own way, so nothing here shows it
//   InitializeTouchInjection / InjectTouchInput   synthesized touch input: the arguments are checked as documented, but
//                        there is no touch input here to inject into (ERROR_NOT_SUPPORTED); the Windows Steam client's
//                        steamui.dll imports them
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
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

// ---- shutdown block reasons -------------------------------------------------------------------------------------------------
enum { ERROR_ACCESS_DENIED = 5, ERROR_NOT_SUPPORTED = 50 };
static struct { HANDLE hwnd; WCHAR *reason; } g_block[64];
static pthread_mutex_t g_block_lock = PTHREAD_MUTEX_INITIALIZER;
static vx_window *own_window(HANDLE hwnd) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) { vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return NULL; }
    if (w->tid != vx_current_tid()) { vx_set_last_error(ERROR_ACCESS_DENIED); return NULL; }
    return w;
}
static WINAPI BOOL ShutdownBlockReasonDestroy(HANDLE hwnd);
static WINAPI BOOL ShutdownBlockReasonCreate(HANDLE hwnd, const WCHAR *reason) {
    if (!own_window(hwnd)) return 0;
    if (!reason) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    size_t n = 0; while (reason[n]) n++;
    WCHAR *copy = malloc((n + 1) * sizeof *copy);
    if (!copy) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    memcpy(copy, reason, (n + 1) * sizeof *copy);
    pthread_mutex_lock(&g_block_lock);
    int slot = -1;
    for (int i = 0; i < 64; i++) {
        if (g_block[i].hwnd == hwnd) { slot = i; break; }
        if (slot < 0 && (!g_block[i].hwnd || !vx_window_get(g_block[i].hwnd))) slot = i;   // (a free one, or a gone window's)
    }
    if (slot >= 0) { free(g_block[slot].reason); g_block[slot].hwnd = hwnd; g_block[slot].reason = copy; }
    pthread_mutex_unlock(&g_block_lock);
    if (slot < 0) { free(copy); vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    return 1;
}
static WINAPI BOOL ShutdownBlockReasonDestroy(HANDLE hwnd) {
    if (!own_window(hwnd)) return 0;
    pthread_mutex_lock(&g_block_lock);
    for (int i = 0; i < 64; i++)
        if (g_block[i].hwnd == hwnd) { free(g_block[i].reason); g_block[i].hwnd = NULL; g_block[i].reason = NULL; }
    pthread_mutex_unlock(&g_block_lock);
    return 1;
}
// ShutdownBlockReasonQuery: the reason with its terminating null; no buffer: the size it needs, in characters.
static WINAPI BOOL ShutdownBlockReasonQuery(HANDLE hwnd, WCHAR *out, DWORD *cch) {
    if (!vx_window_get(hwnd)) { vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    if (!cch) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    BOOL ok = 0;
    pthread_mutex_lock(&g_block_lock);
    for (int i = 0; i < 64 && !ok; i++) {
        if (g_block[i].hwnd != hwnd) continue;
        DWORD n = 0; while (g_block[i].reason[n]) n++;
        if (!out) { *cch = n + 1; ok = 1; }
        else if (*cch > n) { memcpy(out, g_block[i].reason, (n + 1) * sizeof *out); ok = 1; }
        else { *cch = n + 1; vx_set_last_error(122 /* ERROR_INSUFFICIENT_BUFFER */); pthread_mutex_unlock(&g_block_lock); return 0; }
    }
    pthread_mutex_unlock(&g_block_lock);
    if (!ok) vx_set_last_error(1168 /* ERROR_NOT_FOUND */);
    return ok;
}

// ---- touch injection --------------------------------------------------------------------------------------------------------
// InitializeTouchInjection: maxCount 1..MAX_TOUCH_COUNT (256), dwMode TOUCH_FEEDBACK_DEFAULT (1), _INDIRECT (2) or _NONE (3).
static WINAPI BOOL InitializeTouchInjection(uint32_t max_count, DWORD mode) {
    if (!max_count || max_count > 256 || mode < 1 || mode > 3) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    vx_set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}
static WINAPI BOOL InjectTouchInput(uint32_t count, const void *contacts) {
    if (!count || count > 256 || !contacts) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    vx_set_last_error(ERROR_NOT_SUPPORTED);
    return 0;
}

static const vx_export user32[] = {
    VX_EXPORT(GetWindowRgn), VX_EXPORT(GetWindowRgnBox), VX_EXPORT(IsChild), VX_EXPORT(InflateRect), VX_EXPORT(EqualRect),
    VX_EXPORT(CharUpperW), VX_EXPORT(CharNextW), VX_EXPORT(GetLastInputInfo), VX_EXPORT(BeginDeferWindowPos),
    VX_EXPORT(DeferWindowPos), VX_EXPORT(EndDeferWindowPos), VX_EXPORT(NotifyWinEvent),
    VX_EXPORT(ShutdownBlockReasonCreate), VX_EXPORT(ShutdownBlockReasonDestroy), VX_EXPORT(ShutdownBlockReasonQuery),
    VX_EXPORT(InitializeTouchInjection), VX_EXPORT(InjectTouchInput),
};

void vx_winmisc_init(void) {
    rt_SetWindowPos = (BOOL (WINAPI *)(HANDLE, HANDLE, int, int, int, int, uint32_t))vx_find_export("user32.dll", "SetWindowPos");
    rt_GetTickCount = (DWORD (WINAPI *)(void))vx_find_export("kernel32.dll", "GetTickCount");
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
}

#ifdef __aarch64__
#include "thunks/winmisc.c.inc"
#endif
