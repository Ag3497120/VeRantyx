// Windows and drawing: what user32.c, gdi.c and cocoa.m share.
//
// AppKit only works on the macOS main thread, while Windows programs create windows on whatever thread runs
// their message loop. So the program runs on a thread of its own (its "main" thread too), the macOS main thread
// runs AppKit, and every AppKit call goes there synchronously. Events from AppKit become Windows messages in the
// message queue of the thread that created the window. Each window keeps its client area as a BGRA bitmap that
// GDI draws into and the window's view displays.
#ifndef VX_UI_H
#define VX_UI_H
#include <pthread.h>
#include <CoreGraphics/CoreGraphics.h>
#include "vx.h"

typedef uint64_t WPARAM;
typedef int64_t  LPARAM;
typedef int64_t  LRESULT;
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef LRESULT (WINAPI *WNDPROC)(HANDLE, uint32_t, WPARAM, LPARAM);

typedef struct vx_class vx_class;
typedef struct vx_queue vx_queue;

// Text drawn with GDI (TextOut, ExtTextOut, DrawText) and where it is on a surface (a window's client area or a
// bitmap): what Command Clips (control.c) report of a window without its pixels. Painting over a place forgets
// the text there; a blit copies it along with the pixels. Guarded by vx_text_lock.
typedef struct { RECT r; char *s; } vx_text;                // r: the text's box in the surface's coordinates; s: UTF-8
typedef struct { vx_text *v; int n, cap; } vx_texts;
extern pthread_mutex_t vx_text_lock;
void vx_texts_free(vx_texts *t);

// A GDI region (region.c): rectangles that do not overlap, in bands sorted top to bottom, left to right (RGNDATA's form).
typedef struct vx_region { int n, cap; RECT *r; } vx_region;
vx_region *vx_region_new(void);
void vx_region_free(vx_region *g);
void vx_region_copy(vx_region *dst, const vx_region *src);
void vx_region_set_rect(vx_region *g, LONG l, LONG t, LONG r, LONG b);
int  vx_region_combine(vx_region *out, const vx_region *a, const vx_region *b, int mode);   // RGN_AND.. RGN_DIFF; the type
void vx_region_offset(vx_region *g, int dx, int dy);
RECT vx_region_box(const vx_region *g);
int  vx_region_type(const vx_region *g);                     // NULLREGION (1), SIMPLEREGION (2), COMPLEXREGION (3)
HANDLE vx_gdi_region_new(vx_region *r);                      // gdi.c: a region object holding r (NULL if none could be made)
vx_region *vx_gdi_region(HANDLE h);                          // gdi.c: a region object's region, NULL if h is none

typedef struct vx_window {
    HANDLE   hwnd;
    WNDPROC  proc;
    int      unicode;               // created through a W class: messages carry UTF-16 text
    vx_class *cls;
    DWORD    style, exstyle;
    HANDLE   parent, instance;
    int64_t  id, userdata;
    char    *title;                 // UTF-8
    RECT     rect;                  // the window rectangle (frame included), screen coordinates
    int      client_w, client_h;
    DWORD    tid;                   // the thread that owns it (runs its window procedure)
    vx_queue *queue;
    void    *native, *delegate;     // cocoa: the NSWindow and its delegate (retained), NULL for child windows
    int      center;                // CW_USEDEFAULT position: centered on screen
    // The client area. Guarded by `lock` (GDI draws from the program's threads, the view reads on the main thread).
    pthread_mutex_t lock;
    uint32_t *pixels;
    int      bw, bh;
    CGContextRef ctx;
    int      redraw_pending;        // a display request is already queued on the main thread
    int      visible, invalid, erase, destroyed, minimized, enabled;
    uint8_t *extra;                 // cbWndExtra bytes
    int      extra_size;
    vx_texts texts;                 // what GDI drew there as text (control.c)
    // What the screen shows of the client area (present.c, cocoa.m), guarded by `lock`. GDI draws into `pixels`; a
    // finished picture (the DC given back, EndPaint, UpdateLayeredWindow) is copied to `front`, which the main thread
    // hands to Core Animation: a half-drawn frame is never shown.
    uint32_t *front;                // the committed picture, fw x fh, BGRA (premultiplied when front_alpha)
    int      fw, fh, front_alpha;
    RECT     dirty;                 // drawn since the last commit (device coordinates), when `changed`
    int      changed, paint_depth;  // paint_depth: inside BeginPaint (a DC given back there does not commit)
    uint64_t committed, presented;  // pictures committed / handed to Core Animation
    int      drawn;                 // GDI has drawn into the client area (a child window that never has shows what is beneath it)
    double   changed_at;            // when the first uncommitted drawing happened (seconds, monotonic)
    int      fallback_pending;      // a late commit of drawing nobody committed is scheduled
    double   shown_at;              // cocoa: when the main thread last handed its pictures to Core Animation
    void    *layer;                 // cocoa: a child window's CALayer in its parent's layer (retained), else NULL
    vx_region *update;              // the update region (client coordinates); NULL with `invalid`: the whole client area
    int      layered;               // WS_EX_LAYERED set up by 1: SetLayeredWindowAttributes, 2: UpdateLayeredWindow
    uint8_t  layer_alpha;           // constant alpha (LWA_ALPHA, or the BLENDFUNCTION's SourceConstantAlpha)
    uint32_t layer_key;             // color key (COLORREF)
    DWORD    layer_flags;           // LWA_* (layered 1) or ULW_* (layered 2), and AC_SRC_ALPHA << 8 for per-pixel alpha
    int      frameless;             // its WM_NCCALCSIZE made the whole window its client area: a Mac window without a title bar
} vx_window;

// user32.c
vx_window *vx_window_get(HANDLE hwnd);                     // NULL if hwnd is not a live window
void vx_window_post(HANDLE hwnd, uint32_t msg, WPARAM wp, LPARAM lp);   // from any thread
void vx_window_post_all(uint32_t msg, WPARAM wp, LPARAM lp);                       // to every top-level window
void vx_window_resized(vx_window *w, int client_w, int client_h);      // from cocoa, main thread
void vx_window_moved(vx_window *w, RECT frame);
void vx_window_activated(vx_window *w, int active);
void   vx_window_shown_inactive(vx_window *w);              // shown without the Mac's focus: active for the game with VXRT_STAY_ACTIVE
void vx_window_minimized(vx_window *w, int minimized);
void vx_key_event(vx_window *w, int mac_keycode, int down, int repeat, int alt);
// A key with the characters it makes (TranslateMessage posts them as WM_CHAR); ime: the input method took the key
// (WM_KEYDOWN carries VK_PROCESSKEY, its WM_IME_* messages come from ime.c). Keys go to the focus window (user32.c).
void vx_key_event_chars(vx_window *w, int mac_keycode, int down, int repeat, int alt, const uint16_t *chars, int n, int ime);
void vx_char_event(vx_window *w, const uint16_t *chars, int n);
void vx_mouse_input(vx_window *top, uint32_t msg, WPARAM keys, int x, int y);   // at a screen point: to the window under it
void vx_mouse_wheel(vx_window *top, int delta, WPARAM keys, int x, int y, int horizontal);   // to the focus window
void vx_mouse_exited(vx_window *top);                    // the pointer left the Mac window (WM_MOUSELEAVE)
void vx_mouse_button(int vk, int down);                   // VK_LBUTTON etc. state for GetKeyState
void vx_raw_mouse(int dx, int dy, uint16_t button_flags, int16_t wheel);   // WM_INPUT for raw-input programs
LRESULT vx_send_message(HANDLE hwnd, uint32_t msg, WPARAM wp, LPARAM lp);
int vx_virtual_screen(RECT *r);                           // main display, Windows coordinates
#define VX_HWND_DESKTOP ((HANDLE)(intptr_t)0x10010)        // GetDesktopWindow(): the screen's window (user32.c, gdi.c)

// user32.c, for control.c (Command Clips)
char  *vx_ui_state(int texts);                            // the windows (and their text) as JSON (malloc'd)
int    vx_ui_snapshot(HANDLE hwnd, const char *png);      // the client area's pixels into a PNG file
int    vx_ui_snapshot_view(HANDLE hwnd, const char *png, char *info, size_t cap);   // what its Mac window shows (present.c)
int    vx_ui_idle(void);                                  // every thread with windows waits for messages, nothing to paint
HANDLE vx_ui_window(HANDLE hwnd);                         // hwnd if live; 0: the active window, else the first visible one
int    vx_ui_find_text(HANDLE hwnd, const char *needle, HANDLE *in, POINT *at);   // the middle of drawn text with needle
void   vx_ui_click(HANDLE hwnd, int x, int y);            // a left click at client coordinates
void   vx_ui_click_hold(HANDLE hwnd, int x, int y, int hold_ms);   // the same, the button held down hold_ms
void   vx_ui_key(HANDLE hwnd, int vk);                    // WM_KEYDOWN, WM_KEYUP (to the focus window inside hwnd)
void   vx_ui_keys(HANDLE hwnd, const int *vks, int n);    // keys pressed in order, let go in reverse (Ctrl+V)
void   vx_ui_wheel(HANDLE hwnd, int x, int y, int delta); // WM_MOUSEWHEEL with the pointer at client x, y
void   vx_ui_type(HANDLE hwnd, const char *utf8);         // WM_CHAR for each UTF-16 unit

// user32.c, for present.c and cocoa.m
vx_window *vx_window_top(vx_window *w);                   // the top-level window w is in (w itself if it is one)
int  vx_window_children(vx_window *parent, vx_window **out, int cap, int visible_only);   // bottom to top
int  vx_window_all(vx_window **out, int cap);             // every live window
void vx_window_screen_origin(vx_window *w, POINT *o);     // where its client area is on the screen
int  vx_window_wants_paint(vx_window *w);                 // gets WM_PAINT when invalid (not an UpdateLayeredWindow one)
void vx_window_wake(vx_window *w);                        // its thread's message wait looks again (WM_PAINT)

// gdi.c
HANDLE vx_dc_for_window(vx_window *w);
void   vx_dc_release(HANDLE dc);
int    vx_fill_client(vx_window *w, HANDLE dc, HANDLE brush);   // WM_ERASEBKGND's default, on its DC
void   vx_gdi_init(void);
uint32_t vx_sys_color(int index);                         // COLORREF of a COLOR_* index

// gdi.c, for present.c: a DC's surface, to move pixels without Core Graphics. Pixels are 32-bit BGRA rows of w pixels;
// row y (device coordinates, y down) is px + (bottom_up ? h - 1 - y : y) * w. A window's surface is locked from
// vx_surface_lock to vx_surface_unlock.
typedef struct vx_surface {
    uint32_t *px;                   // NULL: a bitmap of another depth (bits, bpp, palette describe it)
    int w, h, bottom_up, bpp;
    uint8_t *bits; const uint32_t *palette; size_t stride;
    vx_window *win;                 // a window's client area, else NULL
    int ox, oy;                     // device = logical + (ox, oy): the viewport origin less the window origin
    const RECT *clip; int nclip;    // drawing stays inside these device rectangles (nclip < 0: no clip region)
    int stretch_mode;               // SetStretchBltMode's
    int locked;
} vx_surface;
int  vx_surface_lock(HANDLE dc, vx_surface *s);           // 0: no such DC, or one with nothing to draw on (the screen)
void vx_surface_unlock(vx_surface *s, const RECT *drawn); // drawn: the device rectangle changed (NULL: nothing)
int  vx_bitmap_surface(HANDLE bitmap, vx_surface *s);     // a bitmap's pixels (not locked; 0 if not a bitmap)
HANDLE vx_dc_bitmap(HANDLE dc);                           // the bitmap selected into a memory DC
int  vx_dc_brush_bgrx(HANDLE dc, uint32_t *bgrx);         // the DC's brush as a pixel (0: a null brush)
int  vx_dc_set_clip(HANDLE dc, const vx_region *r);       // the DC's clip region (device coordinates; NULL: none)
vx_window *vx_dc_window(HANDLE dc);                       // a window DC's window
int  vx_dc_valid(HANDLE dc);                              // a DC (the screen's too, which has no surface)
void vx_gdi_copy_texts(HANDLE dst, int xd, int yd, int wd, int hd, HANDLE src, int xs, int ys, int ws, int hs);
void vx_gdi_forget_texts(HANDLE dc, RECT r);              // r (device coordinates) painted over

// present.c: from GDI's pixels to the screen
void vx_present_changed(vx_window *w, const RECT *r);     // (w->lock held) GDI drew into r of the client area (NULL: all)
void vx_present_commit(vx_window *w);                     // what was drawn is a finished picture: show it
void vx_present_dc_released(vx_window *w);                // a DC of the window was given back
void vx_window_invalidate(vx_window *w, const RECT *r, int erase);   // into the update region (r NULL: the client area)
void vx_window_validate_all(vx_window *w);
LRESULT vx_default_paint(HANDLE hwnd);                    // DefWindowProc's WM_PAINT: BeginPaint, EndPaint
CGImageRef vx_window_front_image(vx_window *w, int *alpha);   // the committed picture (main thread), NULL if none
int  vx_png_write(CGImageRef img, const char *png);
void vx_present_window_gone(vx_window *w);                // destroyed: its buffers go
void vx_present_exstyle(vx_window *w, DWORD before);      // WS_EX_LAYERED / WS_EX_TRANSPARENT changed

// BITMAPINFOHEADER: size, width, height (negative: top-down), planes, bit count, compression, image size, ...
typedef struct { DWORD size; LONG width, height; uint16_t planes, bpp; DWORD compression, image_size; LONG xppm, yppm; DWORD used, important; } vx_bmih;
// gdi.c: a DIB of any common depth as top-down BGRA (malloc'd; 32 bpp keeps its fourth byte if keep_alpha, else 0xff)
uint32_t *vx_dib_to_bgra(const vx_bmih *bi, const void *bits, const uint32_t *palette, int keep_alpha, int *w, int *h);

// gdi.c, for icons.c: bitmaps as plain pixels (top-down BGRA; see there)
uint32_t *vx_gdi_bitmap_bgra(HANDLE bitmap, int *w, int *h, int *bpp);   // malloc'd; NULL if not a bitmap
HANDLE vx_gdi_bitmap_from_bgra(int w, int h, const uint32_t *px);
HANDLE vx_gdi_bitmap_mono(int w, int h, const uint8_t *rows);
int    vx_gdi_draw_bgra(HANDLE dc, int x, int y, int w, int h, const uint32_t *px, int pw, int ph, int blend);

// icons.c: icons and cursors with their pictures (straight-alpha BGRA, top-down)
typedef struct { int w, h; uint32_t *px; } vx_icon_image;
int  vx_icon_images(HANDLE icon, vx_icon_image *out, int cap);   // its picture, then larger sizes of it; 0: no pixels
void vx_icon_images_free(vx_icon_image *v, int n);

// menus.m and tray.m, for user32.c and control.c (Command Clips)
char *vx_menu_state(void);                                  // the popup menu open now, as JSON (malloc'd)
int   vx_menu_pick(const char *text, int wait_ms, char *why, size_t cap);   // as a click on its first enabled item with text
void  vx_tray_window_destroyed(HANDLE hwnd);                // the window's notification-area icons go with it
char *vx_tray_state(void);                                  // the notification-area icons, as JSON (malloc'd)
int   vx_tray_click(int index, const char *how, char *why, size_t cap);   // as a click on one (left, right, double, key, menu-key)
int   vx_tray_snapshot(int index, const char *png, char *why, size_t cap);   // its picture in the menu bar, into a PNG file

// cocoa.m (each returns after the main thread has done it)
void vx_cocoa_ensure(void);                               // AppKit runs on the main thread (starts it if needed)
void vx_cocoa_create(vx_window *w);
void vx_cocoa_destroy(vx_window *w);
void vx_cocoa_show(vx_window *w, int cmd);                // SW_* command
void vx_cocoa_set_title(vx_window *w);
void vx_cocoa_set_frame(vx_window *w);                    // from w->rect
void vx_cocoa_set_style(vx_window *w);                    // from w->style
void vx_cocoa_redraw(vx_window *w);                       // asynchronous: the window's committed pictures shown
void vx_cocoa_present_now(void);                          // every window's committed picture shown, and on screen
void vx_cocoa_set_layered(vx_window *w);                  // from w->exstyle and w->layered / layer_* (transparency)
int  vx_cocoa_snapshot_view(vx_window *w, const char *png, char *info, size_t cap);   // what the Mac window shows
double vx_cocoa_refresh_period(void);                     // the main display's frame time (seconds)
void vx_cocoa_window_gone(vx_window *w);                  // a child window's layer goes (asynchronous)
void vx_cocoa_activate(vx_window *w);
void vx_cocoa_drag(vx_window *w);                         // the window follows the mouse while its button is down (HTCAPTION)

// ime.c: the input method (imm32) between the Mac's text input and Windows programs
LRESULT vx_ime_default(HANDLE hwnd, uint32_t msg, WPARAM wp, LPARAM lp, int unicode);   // DefWindowProc's WM_IME_*
int  vx_ime_active(vx_window *w);                         // the Mac's input method composes for this window's focus
void vx_ime_compose(vx_window *w, const uint16_t *s, int n, int cursor);   // the composition so far (marked text)
void vx_ime_commit(vx_window *w, const uint16_t *s, int n);                 // the composition's result
void vx_ime_caret(vx_window *w, RECT *screen);             // where the program shows the composition (for the Mac's candidates)
int  vx_ui_ime(HANDLE hwnd, const char *compose, const char *result);       // Command Clips: compose, then commit
void vx_ime_key(int vk);                                  // the key the input method took (ImmGetVirtualKey)
void vx_cocoa_ime_cancel(vx_window *top);                 // cocoa.m: the Mac's input method drops its composition
HANDLE vx_keyboard_target(vx_window *w);                  // user32.c: the focus window if it is w or inside it, else w
HANDLE vx_focus_window(void);                             // user32.c: GetFocus's

// clipboard.m
int  vx_ui_clipboard_get(char *out, size_t cap);          // Command Clips: the clipboard's text (UTF-8); 0 if none
int  vx_ui_clipboard_set(const char *utf8);
int  vx_cocoa_message_box(const char *text, const char *caption, unsigned type);
void vx_cocoa_beep(void);
int  vx_cocoa_title_height(void);
void *vx_cocoa_metal_layer(vx_window *w, void *mtl_device, int mtl_pixel_format);   // the view's CAMetalLayer
void *vx_cocoa_metal_layer_bgra(vx_window *w, void *mtl_device);                    // Vulkan's BGRA8 WSI layer
// Runs program(arg) on a thread of its own with the given stack size and AppKit on the calling (main) thread
// once the program asks for it. The process ends when program returns.
int  vx_ui_run_main(int (*program)(void *), void *arg, size_t stack);

// Message and style numbers used across the files.
enum {
    WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003, WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006,
    WM_SETFOCUS = 0x0007, WM_KILLFOCUS = 0x0008, WM_ENABLE = 0x000A, WM_SETTEXT = 0x000C, WM_GETTEXT = 0x000D,
    WM_GETTEXTLENGTH = 0x000E, WM_PAINT = 0x000F, WM_CLOSE = 0x0010, WM_QUIT = 0x0012, WM_ERASEBKGND = 0x0014,
    WM_SHOWWINDOW = 0x0018, WM_ACTIVATEAPP = 0x001C, WM_SETCURSOR = 0x0020, WM_GETMINMAXINFO = 0x0024,
    WM_WINDOWPOSCHANGED = 0x0047, WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082, WM_NCHITTEST = 0x0084,
    WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104, WM_SYSKEYUP = 0x0105,
    WM_SYSCHAR = 0x0106, WM_SYSCOMMAND = 0x0112, WM_TIMER = 0x0113,
    WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_LBUTTONDBLCLK = 0x0203,
    WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205, WM_MBUTTONDOWN = 0x0207, WM_MBUTTONUP = 0x0208,
    WM_MOUSEWHEEL = 0x020A, WM_SIZING = 0x0214, WM_USER = 0x0400,
};
#define WS_POPUP       0x80000000u
#define WS_CHILD       0x40000000u
#define WS_MINIMIZE    0x20000000u
#define WS_VISIBLE     0x10000000u
#define WS_DISABLED    0x08000000u
#define WS_MAXIMIZE    0x01000000u
#define WS_CAPTION     0x00C00000u
#define WS_SYSMENU     0x00080000u
#define WS_THICKFRAME  0x00040000u
#define WS_MINIMIZEBOX 0x00020000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_EX_TRANSPARENT 0x00000020u
#define WS_EX_LAYERED  0x00080000u
#define MAKELPARAM(lo, hi) ((LPARAM)(uint32_t)(((uint16_t)(lo)) | ((uint32_t)(uint16_t)(hi) << 16)))
#endif
