// user32's clipboard on the Mac's pasteboard, from Microsoft's documentation of the clipboard functions.
//
// Text is the Mac's: what a program puts there as CF_UNICODETEXT (or CF_TEXT, CF_OEMTEXT, converted) Mac apps can
// paste, and what Mac apps copied a program reads in all three forms (Windows synthesizes the other two the same way).
// Other formats stay in this process, readable by it until the clipboard is emptied: Mac apps have no use for a
// Windows program's private formats, and other Windows programs here run in processes of their own.
// One window or thread has the clipboard open at a time (ERROR_ACCESS_DENIED for another thread meanwhile);
// EmptyClipboard makes the opener's window the owner. The memory GetClipboardData hands out belongs to the clipboard,
// as on Windows: the program must not free it, and it lasts until the clipboard is emptied or the format read again.
// AddClipboardFormatListener windows get WM_CLIPBOARDUPDATE when the pasteboard changes (it is looked at 3 times a second).
// VXRT_PASTEBOARD=<name>: a pasteboard of that name instead of the general one (the tests use their own).
#import <AppKit/AppKit.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "ui.h"

typedef int32_t WINBOOL;                        // (Objective-C has a BOOL of its own, one byte)

enum { CF_TEXT = 1, CF_OEMTEXT = 7, CF_UNICODETEXT = 13 };
enum { ERROR_ACCESS_DENIED = 5, ERROR_NOT_ENOUGH_MEMORY = 8, ERROR_INVALID_PARAMETER = 87, ERROR_INVALID_WINDOW_HANDLE = 1400,
       ERROR_CLIPBOARD_NOT_OPEN = 1418, ERROR_INVALID_FLAGS = 1004 };
#define WM_CLIPBOARDUPDATE 0x031D
#define GMEM_MOVEABLE 0x2

static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static struct {
    int open; DWORD tid; HANDLE opener;
    HANDLE owner; NSInteger owned_change;          // this process emptied it last, at that pasteboard change count
    int owned;
    struct { DWORD fmt; void *mem; } local[64]; int nlocal;   // formats only this process reads
    struct { DWORD fmt; void *mem; } given[8]; int ngiven;    // text GetClipboardData made
    HANDLE listeners[32]; int nlisteners;
    int watching;
} g;

static void *(WINAPI *rt_GlobalAlloc)(DWORD, size_t);
static void *(WINAPI *rt_GlobalFree)(void *);
static size_t (WINAPI *rt_GlobalSize)(void *);
static int (WINAPI *rt_MultiByteToWideChar)(unsigned, DWORD, const char *, int, WCHAR *, int);
static int (WINAPI *rt_WideCharToMultiByte)(unsigned, DWORD, const WCHAR *, int, char *, int, const char *, WINBOOL *);

static void resolve(void) {                         // (kernel32's functions, registered after this file's)
    if (rt_GlobalAlloc) return;
    rt_GlobalFree = (void *(WINAPI *)(void *))vx_find_export("kernel32.dll", "GlobalFree");
    rt_GlobalSize = (size_t (WINAPI *)(void *))vx_find_export("kernel32.dll", "GlobalSize");
    rt_MultiByteToWideChar = (int (WINAPI *)(unsigned, DWORD, const char *, int, WCHAR *, int))vx_find_export("kernel32.dll", "MultiByteToWideChar");
    rt_WideCharToMultiByte = (int (WINAPI *)(unsigned, DWORD, const WCHAR *, int, char *, int, const char *, WINBOOL *))vx_find_export("kernel32.dll", "WideCharToMultiByte");
    __atomic_store_n(&rt_GlobalAlloc, (void *(WINAPI *)(DWORD, size_t))vx_find_export("kernel32.dll", "GlobalAlloc"), __ATOMIC_RELEASE);
}
static NSPasteboard *pasteboard(void) {
    const char *name = getenv("VXRT_PASTEBOARD");
    return name && *name ? [NSPasteboard pasteboardWithName:[NSString stringWithUTF8String:name]] : NSPasteboard.generalPasteboard;
}
static int is_text(DWORD fmt) { return fmt == CF_TEXT || fmt == CF_OEMTEXT || fmt == CF_UNICODETEXT; }
static int opened_here(void) {
    resolve();
    if (g.open && g.tid == vx_current_tid()) return 1;
    vx_set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
    return 0;
}
static int has_text(void) {
    @autoreleasepool { return [pasteboard() availableTypeFromArray:@[ NSPasteboardTypeString ]] != nil; }
}
static void forget_local(void) {
    for (int i = 0; i < g.nlocal; i++) if (g.local[i].mem) rt_GlobalFree(g.local[i].mem);
    for (int i = 0; i < g.ngiven; i++) rt_GlobalFree(g.given[i].mem);
    g.nlocal = g.ngiven = 0;
}

static WINAPI WINBOOL OpenClipboard(HANDLE hwnd) {
    if (hwnd && !vx_window_get(hwnd)) { vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    pthread_mutex_lock(&g_lock);
    WINBOOL ok = !g.open || g.tid == vx_current_tid();
    if (ok) { g.open = 1; g.tid = vx_current_tid(); g.opener = hwnd; }
    pthread_mutex_unlock(&g_lock);
    if (!ok) vx_set_last_error(ERROR_ACCESS_DENIED);
    return ok;
}
static WINAPI WINBOOL CloseClipboard(void) {
    pthread_mutex_lock(&g_lock);
    WINBOOL ok = opened_here();
    if (ok) { g.open = 0; g.opener = NULL; }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
static WINAPI WINBOOL EmptyClipboard(void) {
    pthread_mutex_lock(&g_lock);
    WINBOOL ok = opened_here();
    if (ok) {
        forget_local();
        @autoreleasepool { NSPasteboard *pb = pasteboard(); [pb clearContents]; g.owned_change = pb.changeCount; }
        g.owner = g.opener; g.owned = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}
// Text to the pasteboard (UTF-16 up to its terminating null, within the memory object's size).
static int put_text(const WCHAR *s, size_t cap_chars) {
    size_t n = 0;
    while (n < cap_chars && s[n]) n++;
    @autoreleasepool {
        NSPasteboard *pb = pasteboard();
        if (!g.owned || pb.changeCount != g.owned_change) [pb clearContents];
        WINBOOL ok = [pb setString:[NSString stringWithCharacters:s length:n] forType:NSPasteboardTypeString];
        g.owned_change = pb.changeCount; g.owned = 1; g.owner = g.opener;
        return ok;
    }
}
static WINAPI HANDLE SetClipboardData(DWORD fmt, HANDLE mem) {
    pthread_mutex_lock(&g_lock);
    HANDLE r = NULL;
    if (!opened_here()) goto done;
    if (!fmt) { vx_set_last_error(ERROR_INVALID_PARAMETER); goto done; }
    if (is_text(fmt) && mem) {
        size_t size = rt_GlobalSize(mem);
        if (fmt == CF_UNICODETEXT) { if (!put_text(mem, size / 2)) goto done; }
        else {
            size_t n = 0;
            while (n < size && ((const char *)mem)[n]) n++;
            WCHAR *w = calloc(n + 1, sizeof *w);
            int k = n ? rt_MultiByteToWideChar(fmt == CF_OEMTEXT ? 1 /* CP_OEMCP */ : 0 /* CP_ACP */, 0, mem, (int)n, w, (int)n) : 0;
            w[k > 0 ? k : 0] = 0;
            int ok = put_text(w, n + 1);
            free(w);
            if (!ok) goto done;
        }
        // (the clipboard owns the memory now, and the program may still read it until CloseClipboard: it is kept with
        // this process's formats until the clipboard is emptied, the text itself being on the pasteboard)
    }
    int i = 0;
    while (i < g.nlocal && g.local[i].fmt != fmt) i++;
    if (i == g.nlocal) { if (g.nlocal == 64) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); goto done; } g.nlocal++; }
    else if (g.local[i].mem && g.local[i].mem != mem) rt_GlobalFree(g.local[i].mem);
    g.local[i].fmt = fmt; g.local[i].mem = mem;               // (NULL: rendered on request, which is not supported)
    if (!g.owned) { g.owner = g.opener; g.owned = 1; }
    r = mem;
done:
    pthread_mutex_unlock(&g_lock);
    return r;
}
static WINAPI HANDLE GetClipboardData(DWORD fmt) {
    pthread_mutex_lock(&g_lock);
    void *r = NULL;
    if (!opened_here()) goto done;
    if (is_text(fmt)) {
        @autoreleasepool {
            NSString *s = [pasteboard() stringForType:NSPasteboardTypeString];
            if (!s) goto done;
            NSUInteger n = s.length;
            WCHAR *w = malloc((n + 1) * sizeof *w);
            [s getCharacters:w range:NSMakeRange(0, n)];
            w[n] = 0;
            if (fmt == CF_UNICODETEXT) {
                r = rt_GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof *w);
                if (r) memcpy(r, w, (n + 1) * sizeof *w);
            } else {
                unsigned cp = fmt == CF_OEMTEXT ? 1 : 0;
                int need = rt_WideCharToMultiByte(cp, 0, w, (int)n + 1, NULL, 0, NULL, NULL);
                r = need > 0 ? rt_GlobalAlloc(GMEM_MOVEABLE, (size_t)need) : NULL;
                if (r) rt_WideCharToMultiByte(cp, 0, w, (int)n + 1, r, need, NULL, NULL);
            }
            free(w);
        }
        if (!r) { vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY); goto done; }
        int i = 0;
        while (i < g.ngiven && g.given[i].fmt != fmt) i++;
        if (i < g.ngiven) rt_GlobalFree(g.given[i].mem);
        else if (g.ngiven < 8) g.ngiven++;
        else { rt_GlobalFree(g.given[0].mem); i = 0; }
        g.given[i].fmt = fmt; g.given[i].mem = r;
        goto done;
    }
    for (int i = 0; i < g.nlocal; i++) if (g.local[i].fmt == fmt) r = g.local[i].mem;
    if (!r) vx_set_last_error(0);
done:
    pthread_mutex_unlock(&g_lock);
    return r;
}
static WINAPI WINBOOL IsClipboardFormatAvailable(DWORD fmt) {
    if (is_text(fmt)) return has_text();
    pthread_mutex_lock(&g_lock);
    WINBOOL found = 0;
    for (int i = 0; i < g.nlocal && !found; i++) found = g.local[i].fmt == fmt;
    pthread_mutex_unlock(&g_lock);
    return found;
}
// The formats there are: text in its three forms when the pasteboard has text, then this process's own.
static int formats(DWORD *out, int cap) {
    int n = 0;
    if (has_text()) { static const DWORD t[3] = { CF_UNICODETEXT, CF_TEXT, CF_OEMTEXT }; for (int i = 0; i < 3 && n < cap; i++) out[n++] = t[i]; }
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g.nlocal && n < cap; i++) if (!is_text(g.local[i].fmt)) out[n++] = g.local[i].fmt;
    pthread_mutex_unlock(&g_lock);
    return n;
}
static WINAPI int CountClipboardFormats(void) { DWORD f[70]; return formats(f, 70); }
static WINAPI DWORD EnumClipboardFormats(DWORD fmt) {
    pthread_mutex_lock(&g_lock);
    int ok = opened_here();
    pthread_mutex_unlock(&g_lock);
    if (!ok) return 0;
    DWORD f[70]; int n = formats(f, 70), i = 0;
    if (fmt) { while (i < n && f[i] != fmt) i++; i++; }
    vx_set_last_error(0);
    return i < n ? f[i] : 0;
}
static WINAPI int GetPriorityClipboardFormat(const DWORD *list, int n) {
    DWORD f[70]; int have = formats(f, 70);
    if (!have) return 0;
    for (int i = 0; list && i < n; i++) for (int k = 0; k < have; k++) if (f[k] == list[i]) return (int)list[i];
    return -1;
}
static WINAPI DWORD GetClipboardSequenceNumber(void) { @autoreleasepool { return (DWORD)pasteboard().changeCount; } }
static WINAPI HANDLE GetClipboardOwner(void) {
    pthread_mutex_lock(&g_lock);
    HANDLE h = NULL;
    @autoreleasepool { if (g.owned && pasteboard().changeCount == g.owned_change) h = g.owner; }
    pthread_mutex_unlock(&g_lock);
    return h && vx_window_get(h) ? h : NULL;
}
static WINAPI HANDLE GetOpenClipboardWindow(void) { return g.open ? g.opener : NULL; }

static void *watch(void *arg) {
    (void)arg;
    NSInteger last;
    @autoreleasepool { last = pasteboard().changeCount; }
    for (;;) {
        usleep(330000);
        NSInteger now;
        @autoreleasepool { now = pasteboard().changeCount; }
        if (now == last) continue;
        last = now;
        HANDLE to[32]; int n;
        pthread_mutex_lock(&g_lock);
        n = g.nlisteners; memcpy(to, g.listeners, sizeof to);
        pthread_mutex_unlock(&g_lock);
        for (int i = 0; i < n; i++) vx_window_post(to[i], WM_CLIPBOARDUPDATE, 0, 0);
    }
    return NULL;
}
static WINAPI WINBOOL AddClipboardFormatListener(HANDLE hwnd) {
    if (!vx_window_get(hwnd)) { vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < g.nlisteners && g.listeners[i] != hwnd) i++;
    WINBOOL ok = i < g.nlisteners || g.nlisteners < 32;
    if (ok && i == g.nlisteners) g.listeners[g.nlisteners++] = hwnd;
    if (ok && !g.watching) { pthread_t t; if (!pthread_create(&t, NULL, watch, NULL)) { pthread_detach(t); g.watching = 1; } }
    pthread_mutex_unlock(&g_lock);
    if (!ok) vx_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    return ok;
}
static WINAPI WINBOOL RemoveClipboardFormatListener(HANDLE hwnd) {
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < g.nlisteners && g.listeners[i] != hwnd) i++;
    WINBOOL ok = i < g.nlisteners;
    if (ok) g.listeners[i] = g.listeners[--g.nlisteners];
    pthread_mutex_unlock(&g_lock);
    if (!ok) vx_set_last_error(ERROR_INVALID_PARAMETER);
    return ok;
}

// RegisterClipboardFormat: one number (0xC000-0xFFFF) per name, names compared without regard to case; the same name
// gives the same number again. GetClipboardFormatName: the name of a registered one (not of the predefined formats).
static struct { char *name; } g_names[512]; static int g_nnames;
static DWORD register_format(const char *name) {
    if (!name || !*name) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    pthread_mutex_lock(&g_lock);
    int i = 0;
    while (i < g_nnames && strcasecmp(g_names[i].name, name)) i++;
    if (i == g_nnames && g_nnames < 512) g_names[g_nnames++].name = strdup(name);
    pthread_mutex_unlock(&g_lock);
    return i < 512 ? 0xC000u + (DWORD)i : 0;
}
static WINAPI DWORD RegisterClipboardFormatA(const char *name) { return register_format(name); }
static WINAPI DWORD RegisterClipboardFormatW(const WCHAR *name) { char *a = name ? vx_utf8_dup(name) : NULL; DWORD r = register_format(a); free(a); return r; }
static const char *format_name(DWORD fmt) {
    int i = (int)fmt - 0xC000;
    return i >= 0 && i < g_nnames ? g_names[i].name : NULL;
}
static WINAPI int GetClipboardFormatNameA(DWORD fmt, char *out, int cap) {
    const char *n = format_name(fmt);
    if (!n || !out || cap <= 0) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    snprintf(out, (size_t)cap, "%s", n);
    return (int)strlen(out);
}
static WINAPI int GetClipboardFormatNameW(DWORD fmt, WCHAR *out, int cap) {
    const char *n = format_name(fmt);
    if (!n || !out || cap <= 0) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    size_t k = vx_utf8_to_utf16(n, strlen(n), out, (size_t)cap - 1);
    out[k] = 0;
    return (int)k;
}

// Command Clips: the pasteboard's text, as another program would see or leave it.
int vx_ui_clipboard_get(char *out, size_t cap) {
    @autoreleasepool {
        NSString *s = [pasteboard() stringForType:NSPasteboardTypeString];
        if (!s || !cap) return 0;
        snprintf(out, cap, "%s", s.UTF8String ?: "");
        return 1;
    }
}
int vx_ui_clipboard_set(const char *utf8) {
    @autoreleasepool {
        NSPasteboard *pb = pasteboard();
        [pb clearContents];
        return [pb setString:[NSString stringWithUTF8String:utf8] ?: @"" forType:NSPasteboardTypeString];
    }
}

static const vx_export user32[] = {
    VX_EXPORT(OpenClipboard), VX_EXPORT(CloseClipboard), VX_EXPORT(EmptyClipboard), VX_EXPORT(SetClipboardData),
    VX_EXPORT(GetClipboardData), VX_EXPORT(IsClipboardFormatAvailable), VX_EXPORT(CountClipboardFormats),
    VX_EXPORT(EnumClipboardFormats), VX_EXPORT(GetPriorityClipboardFormat), VX_EXPORT(GetClipboardSequenceNumber),
    VX_EXPORT(GetClipboardOwner), VX_EXPORT(GetOpenClipboardWindow), VX_EXPORT(AddClipboardFormatListener),
    VX_EXPORT(RemoveClipboardFormatListener), VX_EXPORT(RegisterClipboardFormatA), VX_EXPORT(RegisterClipboardFormatW),
    VX_EXPORT(GetClipboardFormatNameA), VX_EXPORT(GetClipboardFormatNameW),
};
void vx_clipboard_init(void) {
    vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]);
}

#ifdef __aarch64__
#include "thunks/clipboard.m.inc"
#endif
