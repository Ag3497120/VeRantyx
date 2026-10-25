// The input method (imm32) between the Mac's text input and Windows programs, from Microsoft's documentation of the
// Input Method Manager and the WM_IME_* messages.
//
// The Mac's input method (Japanese, Chinese, Korean, dead keys) composes text in the window that has the keyboard; a
// Windows program sees what an IME on Windows shows it: the key as VK_PROCESSKEY (user32.c), WM_IME_STARTCOMPOSITION,
// WM_IME_COMPOSITION with the composition (GCS_COMPSTR, its attributes, clauses and cursor) as it changes, the result
// (GCS_RESULTSTR) when it is committed, WM_IME_ENDCOMPOSITION; ImmGetCompositionString reads them. A program that does
// not handle these hands them to DefWindowProc, which turns the result into WM_IME_CHAR and that into WM_CHAR.
//
// Only a program that asks for an input context (ImmGetContext, as Chromium does for its text fields) gets
// compositions, and only in windows it has not taken the context from (ImmAssociateContext(h, NULL)): others get the
// Mac's keys and characters as before (a game with an input method left on would otherwise lose its keys to it).
// There is one input context for the process. Where the program shows the composition (ImmSetCompositionWindow,
// ImmSetCandidateWindow) is where the Mac puts its candidate list.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"

enum { WM_IME_STARTCOMPOSITION = 0x010D, WM_IME_ENDCOMPOSITION = 0x010E, WM_IME_COMPOSITION = 0x010F, WM_IME_SETCONTEXT = 0x0281,
       WM_IME_NOTIFY = 0x0282, WM_IME_CHAR = 0x0286 };
enum { GCS_COMPREADSTR = 0x1, GCS_COMPREADATTR = 0x2, GCS_COMPREADCLAUSE = 0x4, GCS_COMPSTR = 0x8, GCS_COMPATTR = 0x10,
       GCS_COMPCLAUSE = 0x20, GCS_CURSORPOS = 0x80, GCS_DELTASTART = 0x100, GCS_RESULTREADSTR = 0x200,
       GCS_RESULTREADCLAUSE = 0x400, GCS_RESULTSTR = 0x800, GCS_RESULTCLAUSE = 0x1000 };
#define IMM_ERROR_NODATA (-1)
#define IMM_ERROR_GENERAL (-2)
#define ATTR_INPUT 0
#define NI_COMPOSITIONSTR 0x15
#define CPS_COMPLETE 1
#define CPS_CANCEL 4
#define IACE_CHILDREN 0x1
#define IACE_DEFAULT 0x10
#define CFS_DEFAULT 0
#define CFS_RECT 1
#define CFS_POINT 2
#define CFS_CANDIDATEPOS 0x40
#define CFS_EXCLUDE 0x80

#define MAX_TEXT 512
static struct {
    int asked;                                  // the program asked for an input context
    int composing;
    HANDLE target;                              // the window the composition is in
    uint16_t comp[MAX_TEXT]; int comp_len, cursor;
    uint16_t result[MAX_TEXT]; int result_len;
    HANDLE form_hwnd; POINT form_at; RECT form_area; DWORD form_style;   // ImmSetCompositionWindow (client coordinates)
    HANDLE cand_hwnd; POINT cand_at; RECT cand_area; DWORD cand_style;   // ImmSetCandidateWindow
    int open;
    HANDLE off[256]; int noff;                  // windows without an input context
} g;
static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static int g_context;                           // the process's input context: its address is the HIMC
#define HIMC_VALUE ((HANDLE)&g_context)

HANDLE vx_keyboard_target(vx_window *w);        // user32.c
static int disabled(HANDLE h) {
    for (int i = 0; i < g.noff; i++) if (g.off[i] == h) return 1;
    return 0;
}
static void set_disabled(HANDLE h, int off) {
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < g.noff && g.off[i] != h) i++;
    if (off && i == g.noff && g.noff < 256) g.off[g.noff++] = h;
    if (!off && i < g.noff) g.off[i] = g.off[--g.noff];
    pthread_mutex_unlock(&g_lock);
}

int vx_ime_active(vx_window *w) {
    pthread_mutex_lock(&g_lock);
    int on = g.asked && !disabled(vx_keyboard_target(w));
    pthread_mutex_unlock(&g_lock);
    return on;
}
static void start(HANDLE target) {             // g_lock held
    if (g.composing) return;
    g.composing = 1; g.target = target;
    vx_window_post(target, WM_IME_STARTCOMPOSITION, 0, 0);
}
static void end(void) {                         // g_lock held
    if (!g.composing) return;
    vx_window_post(g.target, WM_IME_ENDCOMPOSITION, 0, 0);
    g.composing = 0; g.comp_len = 0; g.cursor = 0;
}
void vx_ime_compose(vx_window *w, const uint16_t *s, int n, int cursor) {
    HANDLE target = vx_keyboard_target(w);
    pthread_mutex_lock(&g_lock);
    if (n <= 0) {                               // the composition was emptied: it ends without a result
        if (g.composing) { g.comp_len = 0; vx_window_post(g.target, WM_IME_COMPOSITION, 0, 0); end(); }
        pthread_mutex_unlock(&g_lock);
        return;
    }
    start(target);
    g.comp_len = n < MAX_TEXT ? n : MAX_TEXT;
    memcpy(g.comp, s, (size_t)g.comp_len * 2);
    g.cursor = cursor < 0 ? 0 : cursor > g.comp_len ? g.comp_len : cursor;
    vx_window_post(g.target, WM_IME_COMPOSITION, g.comp[g.comp_len - 1],
                   GCS_COMPSTR | GCS_COMPATTR | GCS_COMPCLAUSE | GCS_CURSORPOS | GCS_DELTASTART);
    pthread_mutex_unlock(&g_lock);
}
void vx_ime_commit(vx_window *w, const uint16_t *s, int n) {
    HANDLE target = vx_keyboard_target(w);
    pthread_mutex_lock(&g_lock);
    start(target);
    g.result_len = n < 0 ? 0 : n < MAX_TEXT ? n : MAX_TEXT;
    memcpy(g.result, s, (size_t)g.result_len * 2);
    g.comp_len = 0; g.cursor = 0;
    vx_window_post(g.target, WM_IME_COMPOSITION, g.result_len ? g.result[0] : 0, g.result_len ? GCS_RESULTSTR | GCS_RESULTCLAUSE : 0);
    end();
    pthread_mutex_unlock(&g_lock);
}
// Where the composition shows: the candidate window's place if the program gave one, else the composition window's,
// else the window's client area's top-left corner; in screen coordinates.
void vx_ime_caret(vx_window *w, RECT *r) {
    pthread_mutex_lock(&g_lock);
    HANDLE h = g.cand_hwnd ? g.cand_hwnd : g.form_hwnd;
    POINT at = g.cand_hwnd ? g.cand_at : g.form_at;
    RECT area = g.cand_hwnd ? g.cand_area : g.form_area;
    DWORD style = g.cand_hwnd ? g.cand_style : g.form_style;
    pthread_mutex_unlock(&g_lock);
    vx_window *in = h ? vx_window_get(h) : NULL;
    if (!in) { in = w; at = (POINT){ 0, 0 }; style = CFS_DEFAULT; }
    POINT o; vx_window_screen_origin(in, &o);
    if (style & (CFS_EXCLUDE | CFS_RECT)) *r = (RECT){ o.x + area.left, o.y + area.top, o.x + area.right, o.y + area.bottom };
    else *r = (RECT){ o.x + at.x, o.y + at.y, o.x + at.x + 1, o.y + at.y + 20 };
}

// DefWindowProc's part: a result becomes WM_IME_CHAR, a WM_IME_CHAR becomes WM_CHAR; the rest needs nothing.
LRESULT vx_ime_default(HANDLE hwnd, uint32_t msg, WPARAM wp, LPARAM lp, int unicode) {
    (void)unicode;
    if (msg == WM_IME_COMPOSITION && (lp & GCS_RESULTSTR)) {
        uint16_t r[MAX_TEXT]; int n;
        pthread_mutex_lock(&g_lock);
        n = g.result_len; memcpy(r, g.result, (size_t)n * 2);
        pthread_mutex_unlock(&g_lock);
        for (int i = 0; i < n; i++) vx_window_post(hwnd, WM_IME_CHAR, r[i], 1);
    } else if (msg == WM_IME_CHAR) {
        vx_window_post(hwnd, 0x0102 /* WM_CHAR */, wp, lp);
    }
    return 0;
}

// ---- imm32 ---------------------------------------------------------------------------------------------------------------------
static WINAPI HANDLE ImmGetContext(HANDLE hwnd) {
    pthread_mutex_lock(&g_lock);
    g.asked = 1;
    HANDLE c = hwnd && disabled(hwnd) ? NULL : HIMC_VALUE;
    pthread_mutex_unlock(&g_lock);
    return c;
}
static WINAPI BOOL ImmReleaseContext(HANDLE hwnd, HANDLE himc) { (void)hwnd; return himc == HIMC_VALUE; }
static WINAPI HANDLE ImmAssociateContext(HANDLE hwnd, HANDLE himc) {
    if (!vx_window_get(hwnd)) return NULL;
    pthread_mutex_lock(&g_lock);
    HANDLE was = disabled(hwnd) ? NULL : HIMC_VALUE;
    pthread_mutex_unlock(&g_lock);
    set_disabled(hwnd, himc == NULL);
    return was;
}
static void associate_tree(HANDLE hwnd, int off) {
    vx_window *all[1024];
    int n = vx_window_all(all, 1024);
    vx_window *top = vx_window_get(hwnd);
    for (int i = 0; i < n; i++) {
        vx_window *w = all[i];
        for (int d = 0; w && d < 64 && (w->style & WS_CHILD); d++) {
            w = vx_window_get(w->parent);
            if (w == top) { set_disabled(all[i]->hwnd, off); break; }
        }
    }
}
// ImmAssociateContextEx: himc NULL and no flags takes the context away; IACE_DEFAULT gives the default one back;
// IACE_CHILDREN does it for the child windows too.
static WINAPI BOOL ImmAssociateContextEx(HANDLE hwnd, HANDLE himc, DWORD flags) {
    if (!vx_window_get(hwnd)) return 0;
    int off = !himc && !(flags & IACE_DEFAULT);
    set_disabled(hwnd, off);
    if (flags & IACE_CHILDREN) associate_tree(hwnd, off);
    return 1;
}
// ImmGetCompositionString: the bytes copied, or (no buffer, or a size of 0) the bytes it needs; IMM_ERROR_NODATA for
// what there is none of. GCS_CURSORPOS and GCS_DELTASTART answer a position, not bytes.
static LONG composition(HANDLE himc, DWORD index, void *buf, DWORD size, int wide) {
    if (himc != HIMC_VALUE) return IMM_ERROR_GENERAL;
    pthread_mutex_lock(&g_lock);
    const uint16_t *text = NULL; int n = 0; LONG r = IMM_ERROR_NODATA;
    uint8_t attrs[MAX_TEXT]; DWORD clauses[2];
    switch (index) {
    case GCS_COMPSTR: text = g.comp; n = g.comp_len; break;
    case GCS_RESULTSTR: text = g.result; n = g.result_len; break;
    case GCS_CURSORPOS: r = g.cursor; goto done;
    case GCS_DELTASTART: r = 0; goto done;
    case GCS_COMPATTR:                           // one attribute per character (per byte in the A form): all being typed
        n = g.comp_len;
        if (!wide) { char tmp[MAX_TEXT * 3]; n = (int)vx_utf16_to_utf8(g.comp, (size_t)n, tmp, sizeof tmp); }
        memset(attrs, ATTR_INPUT, sizeof attrs);
        r = n;
        if (buf && size) { if (size < (DWORD)n) { r = IMM_ERROR_GENERAL; goto done; } memcpy(buf, attrs, (size_t)n); }
        goto done;
    case GCS_COMPCLAUSE: case GCS_RESULTCLAUSE: {   // one clause: offsets 0 and the length
        int len = index == GCS_COMPCLAUSE ? g.comp_len : g.result_len;
        if (!wide) { char tmp[MAX_TEXT * 3]; len = (int)vx_utf16_to_utf8(index == GCS_COMPCLAUSE ? g.comp : g.result, (size_t)len, tmp, sizeof tmp); }
        if (!len) goto done;
        clauses[0] = 0; clauses[1] = (DWORD)len; r = sizeof clauses;
        if (buf && size) { if (size < sizeof clauses) { r = IMM_ERROR_GENERAL; goto done; } memcpy(buf, clauses, sizeof clauses); }
        goto done;
    }
    default: goto done;                          // the reading strings: none kept
    }
    if (wide) {
        r = n * 2;
        if (buf && size) { if (size < (DWORD)r) r = (LONG)(size & ~1u); memcpy(buf, text, (size_t)r); }
    } else {
        char tmp[MAX_TEXT * 3];
        r = (LONG)vx_utf16_to_utf8(text, (size_t)n, tmp, sizeof tmp);
        if (buf && size) { if (size < (DWORD)r) r = (LONG)size; memcpy(buf, tmp, (size_t)r); }
    }
done:
    pthread_mutex_unlock(&g_lock);
    return r;
}
static WINAPI LONG ImmGetCompositionStringW(HANDLE himc, DWORD index, void *buf, DWORD size) { return composition(himc, index, buf, size, 1); }
static WINAPI LONG ImmGetCompositionStringA(HANDLE himc, DWORD index, void *buf, DWORD size) { return composition(himc, index, buf, size, 0); }
// ImmSetCompositionString: the composition is the Mac's input method's to change; a program cannot here.
static WINAPI BOOL ImmSetCompositionStringW(HANDLE himc, DWORD index, const void *a, DWORD an, const void *b, DWORD bn) {
    (void)himc; (void)index; (void)a; (void)an; (void)b; (void)bn;
    return 0;
}
void vx_cocoa_ime_cancel(vx_window *w);         // cocoa.m: the Mac's input method drops its composition
// ImmNotifyIME(NI_COMPOSITIONSTR): CPS_COMPLETE commits the composition as it is, CPS_CANCEL drops it (and the Mac's
// input method is told to drop its own).
static WINAPI BOOL ImmNotifyIME(HANDLE himc, DWORD action, DWORD index, DWORD value) {
    (void)value;
    if (himc != HIMC_VALUE) return 0;
    if (action != NI_COMPOSITIONSTR || (index != CPS_COMPLETE && index != CPS_CANCEL)) return 1;
    pthread_mutex_lock(&g_lock);
    int composing = g.composing; HANDLE target = g.target;
    uint16_t comp[MAX_TEXT]; int n = g.comp_len;
    memcpy(comp, g.comp, (size_t)n * 2);
    pthread_mutex_unlock(&g_lock);
    if (!composing) return 1;
    vx_window *w = vx_window_get(target);
    if (w) vx_cocoa_ime_cancel(vx_window_top(w));
    pthread_mutex_lock(&g_lock);
    if (index == CPS_COMPLETE && n) {
        g.result_len = n; memcpy(g.result, comp, (size_t)n * 2); g.comp_len = 0;
        vx_window_post(g.target, WM_IME_COMPOSITION, comp[0], GCS_RESULTSTR | GCS_RESULTCLAUSE);
    } else {
        g.comp_len = 0;
        vx_window_post(g.target, WM_IME_COMPOSITION, 0, 0);
    }
    end();
    pthread_mutex_unlock(&g_lock);
    return 1;
}
// COMPOSITIONFORM: style, current position, area; CANDIDATEFORM: index, style, position, area. Both in the client
// coordinates of the window with the focus.
typedef struct { DWORD style; POINT at; RECT area; } COMPOSITIONFORM;
typedef struct { DWORD index, style; POINT at; RECT area; } CANDIDATEFORM;
HANDLE vx_focus_window(void);                   // user32.c
static WINAPI BOOL ImmSetCompositionWindow(HANDLE himc, const COMPOSITIONFORM *f) {
    if (himc != HIMC_VALUE || !f) return 0;
    pthread_mutex_lock(&g_lock);
    g.form_hwnd = vx_focus_window(); g.form_style = f->style; g.form_at = f->at; g.form_area = f->area;
    pthread_mutex_unlock(&g_lock);
    return 1;
}
static WINAPI BOOL ImmGetCompositionWindow(HANDLE himc, COMPOSITIONFORM *f) {
    if (himc != HIMC_VALUE || !f) return 0;
    pthread_mutex_lock(&g_lock);
    f->style = g.form_style; f->at = g.form_at; f->area = g.form_area;
    pthread_mutex_unlock(&g_lock);
    return 1;
}
static WINAPI BOOL ImmSetCandidateWindow(HANDLE himc, const CANDIDATEFORM *f) {
    if (himc != HIMC_VALUE || !f || f->index > 3) return 0;
    if (f->index) return 1;                      // (one candidate list on the Mac)
    pthread_mutex_lock(&g_lock);
    g.cand_hwnd = vx_focus_window(); g.cand_style = f->style; g.cand_at = f->at; g.cand_area = f->area;
    pthread_mutex_unlock(&g_lock);
    return 1;
}
static WINAPI BOOL ImmGetOpenStatus(HANDLE himc) { return himc == HIMC_VALUE && g.open; }
static WINAPI BOOL ImmSetOpenStatus(HANDLE himc, BOOL open) { if (himc != HIMC_VALUE) return 0; g.open = open != 0; return 1; }
static WINAPI BOOL ImmIsIME(HANDLE hkl) { (void)hkl; return 0; }   // (the keyboard layouts are no IME layouts; the Mac's is apart)
static WINAPI HANDLE ImmGetDefaultIMEWnd(HANDLE hwnd) { (void)hwnd; return NULL; }   // (no IME window here)
static WINAPI BOOL ImmDisableIME(DWORD tid) { (void)tid; return 1; }
// ImmGetVirtualKey: the key behind the last VK_PROCESSKEY (the input method took it).
static volatile int g_processed_vk = 0xE5;
void vx_ime_key(int vk) { g_processed_vk = vk; }
static WINAPI unsigned ImmGetVirtualKey(HANDLE hwnd) { return vx_window_get(hwnd) ? (unsigned)g_processed_vk : 0; }

// Command Clips (control.c): compose, then commit, in hwnd's focus, as the Mac's input method would.
int vx_ui_ime(HANDLE hwnd, const char *compose, const char *result) {
    vx_window *w = vx_window_get(hwnd);
    if (!w) return 0;
    uint16_t c[MAX_TEXT], r[MAX_TEXT];
    int nc = (int)vx_utf8_to_utf16(compose, strlen(compose), c, MAX_TEXT), nr = (int)vx_utf8_to_utf16(result, strlen(result), r, MAX_TEXT);
    vx_ime_compose(w, c, nc, nc);
    vx_ime_commit(w, r, nr);
    return 1;
}

static const vx_export imm32[] = {
    VX_EXPORT(ImmGetContext), VX_EXPORT(ImmReleaseContext), VX_EXPORT(ImmAssociateContext), VX_EXPORT(ImmAssociateContextEx),
    VX_EXPORT(ImmGetCompositionStringW), VX_EXPORT(ImmGetCompositionStringA), VX_EXPORT(ImmSetCompositionStringW),
    VX_EXPORT(ImmNotifyIME), VX_EXPORT(ImmSetCompositionWindow), VX_EXPORT(ImmGetCompositionWindow), VX_EXPORT(ImmSetCandidateWindow),
    VX_EXPORT(ImmGetOpenStatus), VX_EXPORT(ImmSetOpenStatus), VX_EXPORT(ImmIsIME), VX_EXPORT(ImmGetDefaultIMEWnd), VX_EXPORT(ImmDisableIME), VX_EXPORT(ImmGetVirtualKey),
};
void vx_ime_init(void) {
    vx_register_module("imm32.dll", imm32, sizeof imm32 / sizeof imm32[0]);
}

#ifdef __aarch64__
#include "thunks/ime.c.inc"
#endif
