// The notification area (shell32's Shell_NotifyIcon): each icon a program puts there is an NSStatusItem in the Mac's
// menu bar. A program started hidden (Steam with -silent) lives only there; this is how the user opens or quits it.
//
// The icon's HICON is the item's picture (icons.c keeps its pixels, larger sizes too for Retina), the tip its tooltip,
// NIS_HIDDEN hides it. A click on it reaches the owner window as the program's callback message, in the form of the
// version it asked for (NIM_SETVERSION), as Microsoft documents for NOTIFYICONDATA:
//   before version 4: wParam = the icon's id, lParam = the mouse message;
//   version 4:        wParam = the click's screen point (x low, y high), lParam = the message (low) | the id (high);
//   left click:  WM_LBUTTONDOWN, WM_LBUTTONUP, then (version 3 and later) NIN_SELECT; a double click WM_LBUTTONDBLCLK,
//   right click: WM_RBUTTONDOWN, WM_RBUTTONUP, then (version 3 and later) WM_CONTEXTMENU. On the Mac a right click is a
//   two-finger click or Control and a click.
// The program then shows its own menu with TrackPopupMenu (menus.m). Its icons go with NIM_DELETE, with the owner window
// (DestroyWindow, user32.c) and with the process. Balloons (NIF_INFO) are written to the log only: macOS notifications
// need an app bundle with the user's permission, which the runtime is not.
#import <Cocoa/Cocoa.h>
#include "ui.h"

typedef int32_t WINBOOL;                        // (Objective-C has a BOOL of its own, one byte)
enum { NIM_ADD = 0, NIM_MODIFY = 1, NIM_DELETE = 2, NIM_SETFOCUS = 3, NIM_SETVERSION = 4 };
enum { NIF_MESSAGE = 0x1, NIF_ICON = 0x2, NIF_TIP = 0x4, NIF_STATE = 0x8, NIF_INFO = 0x10, NIF_GUID = 0x20, NIF_SHOWTIP = 0x80 };
enum { NIS_HIDDEN = 0x1 };
enum { NIN_SELECT = 0x400, NIN_KEYSELECT = 0x401, WM_CONTEXTMENU = 0x7b };
enum { ERROR_INVALID_PARAMETER = 87, ERROR_INVALID_WINDOW_HANDLE = 1400, ERROR_NO_MORE_ITEMS = 259 };
#define E_FAIL ((int32_t)0x80004005)
#define E_INVALIDARG ((int32_t)0x80070057)

// NOTIFYICONDATAW/A (x64): cbSize 0, hWnd 8, uID 16, uFlags 20, uCallbackMessage 24, hIcon 32, szTip 40, then these; the
// structure of each Windows version ends where the next field begins (MinGW-w64's shellapi.h, ABI facts).
typedef struct { uint32_t state, state_mask, info, version, info_title, guid, v2_size, v3_size; int wide; } layout;
static const layout W = { 296, 300, 304, 816, 820, 952, 952, 968, 1 };
static const layout A = { 168, 172, 176, 432, 436, 504, 504, 520, 0 };

#define MAX_TRAY 64
typedef struct {
    int used, serial;                           // serial: the status button's tag (slots are reused)
    HANDLE hwnd; uint32_t id;
    int has_guid; uint8_t guid[16];
    int has_callback; uint32_t callback, version;
    char tip[512];                              // UTF-8
    int showtip, hidden;
    HANDLE icon;
    int image_w, image_h;                       // the picture shown (0: the stand-in, the runtime's own icon)
    void *item;                                 // the NSStatusItem (retained); touched on the main thread only
} tray_icon;
static tray_icon g_tray[MAX_TRAY];
static int g_serial;
static pthread_mutex_t g_tray_lock = PTHREAD_MUTEX_INITIALIZER;

// ---- the main thread ------------------------------------------------------------------------------------------------------------
// Blocks go through the main run loop in its common modes, so they run while a menu is tracked or a modal panel is up.
static void on_main(void (^b)(void), int wait) {
    vx_cocoa_ensure();
    if ([NSThread isMainThread]) { b(); return; }
    dispatch_semaphore_t done = wait ? dispatch_semaphore_create(0) : nil;
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{ @autoreleasepool { b(); } if (done) dispatch_semaphore_signal(done); });
    CFRunLoopWakeUp(CFRunLoopGetMain());
    if (done) dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
}
// Text as a string: UTF-8 (the ANSI code page here), else read as Windows-1252 (an A string from elsewhere); never nil.
static NSString *ns_text(const char *s) {
    return [NSString stringWithUTF8String:s] ?: [[NSString alloc] initWithBytes:s length:strlen(s) encoding:NSWindowsCP1252StringEncoding] ?: @"";
}
static CGFloat main_height(void) { return NSScreen.screens.count ? NSScreen.screens[0].frame.size.height : 1080; }
static RECT windows_rect(NSRect f) {            // Cocoa screen coordinates to Windows' (y down from the main display's top)
    RECT r = { (LONG)f.origin.x, (LONG)(main_height() - f.origin.y - f.size.height), 0, 0 };
    r.right = r.left + (LONG)f.size.width; r.bottom = r.top + (LONG)f.size.height;
    return r;
}
// The status button's window while the item is in the menu bar: the menu bar places a new item some 0.5 s after it is
// made (its window is off every screen until then), and keeps off it items there is no room for (the notch).
static NSWindow *placed(NSStatusItem *item) {
    NSWindow *w = item.visible ? item.button.window : nil;
    return w && w.screen && w.frame.size.height > 0 ? w : nil;
}

static const char *g_click_as;                 // Command Clips: the kind of click a performClick stands for
static void deliver(int serial, const char *how, POINT at);

@interface VXTrayTarget : NSObject
- (void)clicked:(NSStatusBarButton *)sender;
@end
@implementation VXTrayTarget
- (void)clicked:(NSStatusBarButton *)sender {
    NSEvent *e = NSApp.currentEvent;
    const char *how = g_click_as;
    if (!how) {
        int right = e.type == NSEventTypeRightMouseUp || e.type == NSEventTypeRightMouseDown || (e.modifierFlags & NSEventModifierFlagControl);
        how = right ? "right" : e.clickCount == 2 ? "double" : "left";
    }
    NSPoint m = NSEvent.mouseLocation;
    NSWindow *w = sender.window.screen && sender.window.frame.size.height > 0 ? sender.window : nil;
    if (g_click_as && w) m = NSMakePoint(NSMidX(w.frame), NSMidY(w.frame));   // (no mouse moved there: the icon's middle)
    deliver((int)sender.tag, how, (POINT){ (LONG)m.x, (LONG)(main_height() - m.y) });
}
@end
static VXTrayTarget *g_target;

// The picture: every size of the icon as a representation of one image of the menu bar's icon size, so AppKit draws the
// one that suits the display; the runtime's own icon when the program gave none with pixels.
static NSImage *status_image(vx_icon_image *v, int n) {
    CGFloat side = NSStatusBar.systemStatusBar.thickness - 4;
    if (side < 16) side = 16;
    if (side > 20) side = 20;
    NSImage *img = [[NSImage alloc] initWithSize:NSMakeSize(side, side)];
    for (int i = 0; i < n; i++) {
        CFDataRef data = CFDataCreate(NULL, (const UInt8 *)v[i].px, (CFIndex)v[i].w * v[i].h * 4);
        CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGImageRef cg = CGImageCreate((size_t)v[i].w, (size_t)v[i].h, 8, 32, (size_t)v[i].w * 4, cs, kCGImageAlphaFirst | kCGBitmapByteOrder32Little,
                                      dp, NULL, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs); CGDataProviderRelease(dp); CFRelease(data);
        if (!cg) continue;
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
        CGImageRelease(cg);
        rep.size = NSMakeSize(side, side);
        [img addRepresentation:rep];
    }
    if (!n) {
        NSImage *app = [NSApp.applicationIconImage copy];
        app.size = NSMakeSize(side, side);
        return app;
    }
    return img;
}

// Brings the status item of slot i up to date (creating it); main thread. `pictures` is taken (freed here).
static void update_item(int i, int serial, vx_icon_image *pictures, int npictures, int new_picture) {
    pthread_mutex_lock(&g_tray_lock);
    tray_icon c = g_tray[i];
    pthread_mutex_unlock(&g_tray_lock);
    if (!c.used || c.serial != serial) { vx_icon_images_free(pictures, npictures); return; }   // (deleted meanwhile)
    NSStatusItem *item = (__bridge NSStatusItem *)c.item;
    if (!item) {
        if (!g_target) g_target = [VXTrayTarget new];
        item = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
        item.button.target = g_target;
        item.button.action = @selector(clicked:);
        item.button.tag = serial;
        [item.button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
        item.button.imageScaling = NSImageScaleProportionallyUpOrDown;
        pthread_mutex_lock(&g_tray_lock);
        int kept = g_tray[i].used && g_tray[i].serial == serial;
        if (kept) g_tray[i].item = (__bridge_retained void *)item;
        pthread_mutex_unlock(&g_tray_lock);
        if (!kept) { [NSStatusBar.systemStatusBar removeStatusItem:item]; vx_icon_images_free(pictures, npictures); return; }   // (deleted meanwhile)
        new_picture = 1;
    }
    if (new_picture) item.button.image = status_image(pictures, npictures);
    vx_icon_images_free(pictures, npictures);
    NSString *tip = c.tip[0] && (c.version < 4 || c.showtip) ? ns_text(c.tip) : nil;
    item.button.toolTip = tip;                  // (version 4 shows the standard tip only with NIF_SHOWTIP)
    item.visible = !c.hidden;
}
static void remove_item(void *p) {
    if (!p) return;
    on_main(^{ NSStatusItem *item = (__bridge_transfer NSStatusItem *)p; [NSStatusBar.systemStatusBar removeStatusItem:item]; }, 0);
}

// ---- the program's side -----------------------------------------------------------------------------------------------------------
static int find_icon(HANDLE hwnd, uint32_t id, const uint8_t *guid) {   // (g_tray_lock held)
    for (int i = 0; i < MAX_TRAY; i++) {
        tray_icon *t = &g_tray[i];
        if (!t->used) continue;
        if (guid ? t->has_guid && !memcmp(t->guid, guid, 16) : t->hwnd == hwnd && t->id == id) return i;
    }
    return -1;
}
static void copy_text(char *out, size_t cap, const uint8_t *s, size_t max_units, int wide) {
    if (wide) {
        size_t n = 0;
        while (n < max_units && ((const WCHAR *)s)[n]) n++;
        size_t k = vx_utf16_to_utf8((const WCHAR *)s, n, out, cap - 1);
        out[k < cap - 1 ? k : cap - 1] = 0;
    } else {
        size_t n = 0;
        while (n < max_units && n < cap - 1 && s[n]) n++;
        memcpy(out, s, n); out[n] = 0;
    }
}
// The fields uFlags names, from a NOTIFYICONDATA of `size` bytes. Returns whether the picture changed. (g_tray_lock held)
static int apply(tray_icon *t, const uint8_t *d, uint32_t size, uint32_t flags, const layout *L) {
    int v2 = size >= L->v2_size, picture = 0;
    if (flags & NIF_MESSAGE) { t->has_callback = 1; t->callback = *(const uint32_t *)(d + 24); }
    if (flags & NIF_ICON) { t->icon = *(HANDLE const *)(d + 32); picture = 1; }
    if (flags & NIF_TIP) copy_text(t->tip, sizeof t->tip, d + 40, v2 ? 128 : 64, L->wide);
    t->showtip = (flags & NIF_SHOWTIP) != 0;
    if ((flags & NIF_STATE) && v2) {
        uint32_t state = *(const uint32_t *)(d + L->state), mask = *(const uint32_t *)(d + L->state_mask);
        if (mask & NIS_HIDDEN) t->hidden = (state & NIS_HIDDEN) != 0;
    }
    if ((flags & NIF_INFO) && v2) {
        char title[256], text[1024];
        copy_text(title, sizeof title, d + L->info_title, 64, L->wide);
        copy_text(text, sizeof text, d + L->info, 256, L->wide);
        if (text[0]) vx_log("notification area: balloon \"%s\": %s (not shown: macOS notifications need an app bundle)", title, text);
    }
    return picture;
}

static WINBOOL notify_icon(DWORD msg, const uint8_t *d, const layout *L) {
    if (!d || *(const uint32_t *)d < 40 + (L->wide ? 128 : 64)) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    uint32_t size = *(const uint32_t *)d, flags = *(const uint32_t *)(d + 20), id = *(const uint32_t *)(d + 16);
    HANDLE hwnd = *(HANDLE const *)(d + 8);
    const uint8_t *guid = (flags & NIF_GUID) && size >= L->v3_size ? d + L->guid : NULL;
    pthread_mutex_lock(&g_tray_lock);
    int i = find_icon(hwnd, id, guid), picture = 0, serial = 0, ok = 1;
    void *gone = NULL;
    switch (msg) {
    case NIM_ADD:
        if (i >= 0 || !vx_window_get(hwnd)) { ok = 0; vx_set_last_error(i >= 0 ? ERROR_INVALID_PARAMETER : ERROR_INVALID_WINDOW_HANDLE); break; }
        for (i = 0; i < MAX_TRAY && g_tray[i].used; i++) {}
        if (i == MAX_TRAY) { ok = 0; vx_set_last_error(ERROR_NO_MORE_ITEMS); break; }
        g_tray[i] = (tray_icon){ .used = 1, .serial = ++g_serial, .hwnd = hwnd, .id = id };
        if (guid) { g_tray[i].has_guid = 1; memcpy(g_tray[i].guid, guid, 16); }
        apply(&g_tray[i], d, size, flags, L);
        picture = 1;
        break;
    case NIM_MODIFY:
        if (i < 0) { ok = 0; break; }
        if (!guid) g_tray[i].hwnd = hwnd;
        picture = apply(&g_tray[i], d, size, flags, L);
        break;
    case NIM_DELETE:
        if (i < 0) { ok = 0; break; }
        gone = g_tray[i].item;
        memset(&g_tray[i], 0, sizeof g_tray[i]);
        break;
    case NIM_SETVERSION: {
        uint32_t v = size >= L->v2_size ? *(const uint32_t *)(d + L->version) : 0;
        if (i < 0 || v > 4) { ok = 0; vx_set_last_error(ERROR_INVALID_PARAMETER); break; }
        g_tray[i].version = v;
        break;
    }
    case NIM_SETFOCUS:                          // (the menu bar keeps no keyboard focus to give back)
        ok = i >= 0;
        break;
    default:
        ok = 0; vx_set_last_error(ERROR_INVALID_PARAMETER);
    }
    if (ok && i >= 0 && msg != NIM_DELETE) serial = g_tray[i].serial;
    HANDLE icon = ok && i >= 0 ? g_tray[i].icon : NULL;
    pthread_mutex_unlock(&g_tray_lock);
    if (gone) remove_item(gone);
    if (ok && (msg == NIM_ADD || msg == NIM_MODIFY)) {
        vx_icon_image pictures[8];
        int n = picture ? vx_icon_images(icon, pictures, 8) : 0;
        pthread_mutex_lock(&g_tray_lock);
        if (picture && g_tray[i].serial == serial) { g_tray[i].image_w = n ? pictures[0].w : 0; g_tray[i].image_h = n ? pictures[0].h : 0; }
        pthread_mutex_unlock(&g_tray_lock);
        int slot = i;
        vx_icon_image *taken = n ? malloc(sizeof pictures) : NULL;
        if (n) memcpy(taken, pictures, sizeof pictures);
        on_main(^{ update_item(slot, serial, taken, n, picture); free(taken); }, 1);
    }
    return ok;
}
static WINAPI WINBOOL Shell_NotifyIconW(DWORD msg, const void *data) { return notify_icon(msg, data, &W); }
static WINAPI WINBOOL Shell_NotifyIconA(DWORD msg, const void *data) { return notify_icon(msg, data, &A); }

// NOTIFYICONIDENTIFIER: cbSize 0, hWnd 8, uID 16, guidItem 20 (40 bytes). The icon by its GUID if one is given.
static WINAPI int32_t Shell_NotifyIconGetRect(const uint8_t *ident, RECT *out) {
    static const uint8_t zero[16];
    if (!ident || !out || *(const uint32_t *)ident < 36) return E_INVALIDARG;
    const uint8_t *guid = memcmp(ident + 20, zero, 16) ? ident + 20 : NULL;
    pthread_mutex_lock(&g_tray_lock);
    int i = find_icon(*(HANDLE const *)(ident + 8), *(const uint32_t *)(ident + 16), guid);
    int serial = i >= 0 ? g_tray[i].serial : 0;
    pthread_mutex_unlock(&g_tray_lock);
    if (i < 0) return E_FAIL;
    // A new item is placed within about half a second (placed()); asked right after NIM_ADD, this waits for it.
    __block RECT r; __block int found = 0, gone = 0;
    for (int tries = 0; !found && !gone && tries < 40; tries++) {
        if (tries) usleep(25000);
        on_main(^{
            pthread_mutex_lock(&g_tray_lock);
            NSStatusItem *item = g_tray[i].serial == serial ? (__bridge NSStatusItem *)g_tray[i].item : nil;
            pthread_mutex_unlock(&g_tray_lock);
            NSWindow *w = placed(item);
            if (w) { r = windows_rect(w.frame); found = 1; }
            gone = !item || !item.visible;
        }, 1);
    }
    if (!found) return E_FAIL;                  // (hidden, or no room for it in the menu bar)
    *out = r;
    return 0;
}

// A click (or the keyboard's choice) on the icon of `serial`, as the owner window's callback message(s).
static void deliver(int serial, const char *how, POINT at) {
    pthread_mutex_lock(&g_tray_lock);
    tray_icon t = { 0 };
    for (int i = 0; i < MAX_TRAY; i++) if (g_tray[i].used && g_tray[i].serial == serial) t = g_tray[i];
    pthread_mutex_unlock(&g_tray_lock);
    if (!t.used || !t.has_callback) return;
    uint32_t msgs[4]; int n = 0;
    int v3 = t.version >= 3;
    if (!strcmp(how, "left")) { msgs[n++] = WM_LBUTTONDOWN; msgs[n++] = WM_LBUTTONUP; if (v3) msgs[n++] = NIN_SELECT; }
    else if (!strcmp(how, "double")) { msgs[n++] = WM_LBUTTONDBLCLK; msgs[n++] = WM_LBUTTONUP; }
    else if (!strcmp(how, "right")) { msgs[n++] = WM_RBUTTONDOWN; msgs[n++] = WM_RBUTTONUP; if (v3) msgs[n++] = WM_CONTEXTMENU; }
    else if (!strcmp(how, "key")) { if (v3) msgs[n++] = NIN_KEYSELECT; else { msgs[n++] = WM_RBUTTONDOWN; msgs[n++] = WM_RBUTTONUP; } }
    else if (!strcmp(how, "menu-key")) { if (v3) msgs[n++] = WM_CONTEXTMENU; else { msgs[n++] = WM_RBUTTONDOWN; msgs[n++] = WM_RBUTTONUP; } }
    for (int k = 0; k < n; k++) {
        if (t.version >= 4)
            vx_window_post(t.hwnd, t.callback, (WPARAM)(uint32_t)MAKELPARAM(at.x, at.y), MAKELPARAM(msgs[k], t.id));
        else
            vx_window_post(t.hwnd, t.callback, t.id, msgs[k]);
    }
}

// ---- the owner window goes (user32.c's DestroyWindow) ------------------------------------------------------------------------
void vx_tray_window_destroyed(HANDLE hwnd) {
    void *gone[MAX_TRAY]; int n = 0;
    pthread_mutex_lock(&g_tray_lock);
    for (int i = 0; i < MAX_TRAY; i++)
        if (g_tray[i].used && g_tray[i].hwnd == hwnd) { gone[n++] = g_tray[i].item; memset(&g_tray[i], 0, sizeof g_tray[i]); }
    pthread_mutex_unlock(&g_tray_lock);
    for (int i = 0; i < n; i++) remove_item(gone[i]);
}

// ---- Command Clips (control.c) ---------------------------------------------------------------------------------------------------
static char *json(id obj) {
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
    char *s = malloc(d.length + 1);
    memcpy(s, d.bytes, d.length); s[d.length] = 0;
    return s;
}
char *vx_tray_state(void) {
    @autoreleasepool {
        __block NSMutableArray *icons = [NSMutableArray array];
        void (^collect)(void) = ^{
            pthread_mutex_lock(&g_tray_lock);
            for (int i = 0; i < MAX_TRAY; i++) {
                tray_icon *t = &g_tray[i];
                if (!t->used) continue;
                NSMutableDictionary *d = [@{ @"index": @(icons.count), @"hwnd": @((long long)(intptr_t)t->hwnd), @"id": @(t->id),
                    @"callback": t->has_callback ? @(t->callback) : [NSNull null], @"version": @(t->version),
                    @"tip": ns_text(t->tip), @"hidden": @(t->hidden != 0),
                    @"image": t->image_w ? @[ @(t->image_w), @(t->image_h) ] : [NSNull null] } mutableCopy];
                if (t->has_guid) {
                    const uint8_t *g = t->guid;
                    d[@"guid"] = [NSString stringWithFormat:@"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", *(const uint32_t *)g, *(const uint16_t *)(g + 4),
                                  *(const uint16_t *)(g + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]];
                }
                NSWindow *w = placed((__bridge NSStatusItem *)t->item);
                if (w) { RECT r = windows_rect(w.frame); d[@"rect"] = @[ @(r.left), @(r.top), @(r.right), @(r.bottom) ]; }
                d[@"shown"] = @(w != nil);              // (in the menu bar now: see placed())
                [icons addObject:d];
            }
            pthread_mutex_unlock(&g_tray_lock);
        };
        on_main(collect, 1);
        return json(@{ @"ok": @YES, @"icons": icons });
    }
}
// index: the n-th icon there is. A mouse click goes through the status button itself (performClick), as a real one would.
int vx_tray_click(int index, const char *how, char *why, size_t cap) {
    static const char *kinds[] = { "left", "right", "double", "key", "menu-key" };
    const char *kind = NULL;
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) if (!strcmp(how, kinds[k])) kind = kinds[k];
    if (!kind) { snprintf(why, cap, "tray-click <index> left|right|double|key|menu-key"); return 0; }
    pthread_mutex_lock(&g_tray_lock);
    int serial = 0, seen = 0, slot = -1;
    for (int i = 0; i < MAX_TRAY && slot < 0; i++) if (g_tray[i].used && seen++ == index) { slot = i; serial = g_tray[i].serial; }
    pthread_mutex_unlock(&g_tray_lock);
    if (slot < 0) { snprintf(why, cap, "no such notification-area icon"); return 0; }
    __block int clicked = 0;
    if (!strcmp(kind, "key") || !strcmp(kind, "menu-key")) {
        __block POINT at = { 0, 0 };                              // (the keyboard's anchor: the icon's top left corner)
        on_main(^{
            pthread_mutex_lock(&g_tray_lock);
            NSStatusItem *item = g_tray[slot].serial == serial ? (__bridge NSStatusItem *)g_tray[slot].item : nil;
            pthread_mutex_unlock(&g_tray_lock);
            NSWindow *w = placed(item);
            if (w) { RECT r = windows_rect(w.frame); at = (POINT){ r.left, r.top }; }
        }, 1);
        deliver(serial, kind, at);
        return 1;
    }
    on_main(^{
        pthread_mutex_lock(&g_tray_lock);
        NSStatusItem *item = g_tray[slot].serial == serial ? (__bridge NSStatusItem *)g_tray[slot].item : nil;
        pthread_mutex_unlock(&g_tray_lock);
        if (!item.visible) return;
        g_click_as = kind;
        [item.button performClick:nil];
        g_click_as = NULL;
        clicked = 1;
    }, 1);
    if (!clicked) snprintf(why, cap, "the icon is hidden or not shown");
    return clicked;
}
// The n-th icon's picture as the status item has it, drawn at twice the menu bar's size into a PNG file.
int vx_tray_snapshot(int index, const char *path, char *why, size_t cap) {
    pthread_mutex_lock(&g_tray_lock);
    int serial = 0, seen = 0, slot = -1;
    for (int i = 0; i < MAX_TRAY && slot < 0; i++) if (g_tray[i].used && seen++ == index) { slot = i; serial = g_tray[i].serial; }
    pthread_mutex_unlock(&g_tray_lock);
    if (slot < 0) { snprintf(why, cap, "no such notification-area icon"); return 0; }
    __block int ok = 0;
    char *p = strdup(path);
    on_main(^{
        pthread_mutex_lock(&g_tray_lock);
        NSStatusItem *item = g_tray[slot].serial == serial ? (__bridge NSStatusItem *)g_tray[slot].item : nil;
        pthread_mutex_unlock(&g_tray_lock);
        NSImage *img = item.button.image;
        if (!img) return;
        NSInteger w = (NSInteger)(img.size.width * 2), h = (NSInteger)(img.size.height * 2);
        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:w pixelsHigh:h bitsPerSample:8 samplesPerPixel:4
                                     hasAlpha:YES isPlanar:NO colorSpaceName:NSDeviceRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        rep.size = img.size;
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [img drawInRect:NSMakeRect(0, 0, img.size.width, img.size.height)];
        [NSGraphicsContext restoreGraphicsState];
        ok = [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(p) atomically:YES];
    }, 1);
    free(p);
    if (!ok) snprintf(why, cap, "tray-snapshot <index> <path>: not written");
    return ok;
}

static const vx_export shell32[] = { VX_EXPORT(Shell_NotifyIconW), VX_EXPORT(Shell_NotifyIconA), VX_EXPORT(Shell_NotifyIconGetRect) };
void vx_icons_init(void);                       // icons.c
void vx_menus_init(void);                       // menus.m
void vx_tray_init(void) {
    vx_icons_init();
    vx_menus_init();
    vx_register_module("shell32.dll", shell32, sizeof shell32 / sizeof shell32[0]);
}

#ifdef __aarch64__
#include "thunks/tray.m.inc"
#endif
