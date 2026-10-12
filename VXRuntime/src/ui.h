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
} vx_window;

// user32.c
vx_window *vx_window_get(HANDLE hwnd);                     // NULL if hwnd is not a live window
void vx_window_post(HANDLE hwnd, uint32_t msg, WPARAM wp, LPARAM lp);   // from any thread
void vx_window_resized(vx_window *w, int client_w, int client_h);      // from cocoa, main thread
void vx_window_moved(vx_window *w, RECT frame);
void vx_window_activated(vx_window *w, int active);
void vx_window_minimized(vx_window *w, int minimized);
void vx_key_event(vx_window *w, int mac_keycode, int down, int repeat, int alt);
void vx_char_event(vx_window *w, const uint16_t *chars, int n);
void vx_mouse_button(int vk, int down);                   // VK_LBUTTON etc. state for GetKeyState
void vx_request_redraw(vx_window *w);                     // after drawing into the client area
LRESULT vx_send_message(HANDLE hwnd, uint32_t msg, WPARAM wp, LPARAM lp);
int vx_virtual_screen(RECT *r);                           // main display, Windows coordinates

// gdi.c
HANDLE vx_dc_for_window(vx_window *w);
void   vx_dc_release(HANDLE dc);
void   vx_fill_client(vx_window *w, HANDLE brush);        // WM_ERASEBKGND's default
void   vx_gdi_init(void);
uint32_t vx_sys_color(int index);                         // COLORREF of a COLOR_* index

// cocoa.m (each returns after the main thread has done it)
void vx_cocoa_create(vx_window *w);
void vx_cocoa_destroy(vx_window *w);
void vx_cocoa_show(vx_window *w, int cmd);                // SW_* command
void vx_cocoa_set_title(vx_window *w);
void vx_cocoa_set_frame(vx_window *w);                    // from w->rect
void vx_cocoa_set_style(vx_window *w);                    // from w->style
void vx_cocoa_redraw(vx_window *w);                       // asynchronous
void vx_cocoa_activate(vx_window *w);
int  vx_cocoa_message_box(const char *text, const char *caption, unsigned type);
void vx_cocoa_beep(void);
int  vx_cocoa_title_height(void);
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
#define MAKELPARAM(lo, hi) ((LPARAM)(uint32_t)(((uint16_t)(lo)) | ((uint32_t)(uint16_t)(hi) << 16)))
#endif
