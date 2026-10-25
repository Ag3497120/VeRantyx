// The AppKit side of windows: the macOS main thread runs NSApplication; Windows windows are NSWindows whose
// content view shows the window's client-area bitmap and turns keyboard and mouse events into Windows messages.
//
// The content view hosts a Core Animation layer whose contents are the window's committed picture (present.c), one
// image pixel to a point, scaled to the display's backing pixels by the GPU; child windows are sublayers at their
// places, above it in the order they were made. Pictures are handed over on the main thread at most once in 8 ms
// and without implicit animations, so what the screen shows is always a whole picture.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include <dlfcn.h>
#include "ui.h"

// ---- the main thread ----------------------------------------------------------------------------------------------
static pthread_mutex_t g_ui_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ui_cond = PTHREAD_COND_INITIALIZER;
static int g_ui_wanted, g_ui_ready;

typedef struct { int (*program)(void *); void *arg; } program_start;

static void *program_thread(void *p) {
    program_start *s = p;
    int rc = s->program(s->arg);
    fflush(stdout); fflush(stderr);
    _exit(rc);
}

// Starts AppKit (once) and waits until the main thread runs it.
static void ui_ensure(void) {
    pthread_mutex_lock(&g_ui_lock);
    if (!g_ui_wanted) { g_ui_wanted = 1; pthread_cond_broadcast(&g_ui_cond); }
    while (!g_ui_ready) pthread_cond_wait(&g_ui_cond, &g_ui_lock);
    pthread_mutex_unlock(&g_ui_lock);
}

void vx_cocoa_ensure(void) { ui_ensure(); }

static void on_main(dispatch_block_t b) {
    ui_ensure();
    if ([NSThread isMainThread]) b(); else dispatch_sync(dispatch_get_main_queue(), b);
}

static int activation_allowed(void) { const char *e = getenv("VXRT_NO_ACTIVATE"); return !(e && *e == '1'); }

@interface VXAppDelegate : NSObject <NSApplicationDelegate>
@end
@implementation VXAppDelegate
// Quit (Cmd+Q) asks every window to close, like Alt+F4 on all of them; the program decides.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)app {
    for (NSWindow *w in app.windows) [w performClose:nil];
    return NSTerminateCancel;
}
@end

int vx_ui_run_main(int (*program)(void *), void *arg, size_t stack) {
    static program_start start;
    start.program = program; start.arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (stack + 16383) & ~(size_t)16383);
    pthread_t t;
    if (pthread_create(&t, &a, program_thread, &start)) { vx_log("cannot start the program thread"); return 1; }
    // Until a window is wanted the main thread serves the main queue (in 50 ms slices) without starting AppKit: the
    // Game Controller framework tells about controllers there, so XInput sees them in a program without windows too.
    pthread_mutex_lock(&g_ui_lock);
    while (!g_ui_wanted) {
        pthread_mutex_unlock(&g_ui_lock);
        @autoreleasepool { CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false); }
        pthread_mutex_lock(&g_ui_lock);
    }
    pthread_mutex_unlock(&g_ui_lock);
    @autoreleasepool {
        [NSApplication sharedApplication];
        // No Dock icon, menu bar or activation until the program shows something (become_regular): a Windows
        // process whose windows are all hidden has no taskbar button (Steam started with -silent).
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        static VXAppDelegate *delegate;
        delegate = [VXAppDelegate new];
        NSApp.delegate = delegate;
        NSMenu *bar = [NSMenu new], *app_menu = [NSMenu new];
        NSMenuItem *app_item = [NSMenuItem new];
        [bar addItem:app_item];
        [app_menu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
        app_item.submenu = app_menu;
        NSApp.mainMenu = bar;
    }
    pthread_mutex_lock(&g_ui_lock);
    g_ui_ready = 1;
    pthread_cond_broadcast(&g_ui_cond);
    pthread_mutex_unlock(&g_ui_lock);
    [NSApp run];
    return 0;
}

// ---- coordinates ------------------------------------------------------------------------------------------------------
// Windows screen coordinates: origin at the top left of the main display, y down. Cocoa: bottom left, y up.
static CGFloat main_height(void) { return NSScreen.screens.count ? NSScreen.screens[0].frame.size.height : 1080; }
static NSRect cocoa_rect(RECT r) {
    return NSMakeRect(r.left, main_height() - r.bottom, r.right - r.left, r.bottom - r.top);
}
static RECT windows_rect(NSRect f) {
    RECT r = { (LONG)f.origin.x, (LONG)(main_height() - f.origin.y - f.size.height), 0, 0 };
    r.right = r.left + (LONG)f.size.width; r.bottom = r.top + (LONG)f.size.height;
    return r;
}

static NSWindowStyleMask style_mask(DWORD style) {
    if ((style & WS_POPUP) && (style & WS_CAPTION) != WS_CAPTION) return NSWindowStyleMaskBorderless;
    NSWindowStyleMask m = 0;
    if ((style & WS_CAPTION) == WS_CAPTION) m |= NSWindowStyleMaskTitled;
    if (style & WS_SYSMENU) m |= NSWindowStyleMaskClosable;
    if (style & WS_MINIMIZEBOX) m |= NSWindowStyleMaskMiniaturizable;
    if (style & WS_THICKFRAME) m |= NSWindowStyleMaskResizable;
    return m ? m : NSWindowStyleMaskBorderless;
}

// The title bar height of this macOS version (28 points before macOS 26, 32 since), asked of AppKit once.
int vx_cocoa_title_height(void) {
    static int h;
    if (!h) {
        NSRect c = NSMakeRect(0, 0, 100, 100);
        NSRect f = [NSWindow frameRectForContentRect:c styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable];
        h = (int)(f.size.height - c.size.height);
        if (h <= 0) h = 28;
    }
    return h;
}

// ---- the view ------------------------------------------------------------------------------------------------------------
@interface VXView : NSView {
@public
    vx_window *w;
    NSTrackingArea *tracking;
    CGFloat wheel;                         // accumulated precise scrolling, in WHEEL_DELTA units
    CGFloat raw_x, raw_y;                  // fractions of raw movement not reported yet
}
@end

@implementation VXView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { return YES; }

- (void)updateTrackingAreas {
    if (tracking) [self removeTrackingArea:tracking];
    tracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                            options:NSTrackingMouseMoved | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                              owner:self userInfo:nil];
    [self addTrackingArea:tracking];
    [super updateTrackingAreas];
}

// MK_* flags for the mouse messages' wParam.
static WPARAM mouse_keys(NSEvent *e) {
    NSUInteger b = NSEvent.pressedMouseButtons, f = e.modifierFlags;
    WPARAM k = 0;
    if (b & 1) k |= 0x01;
    if (b & 2) k |= 0x02;
    if (f & NSEventModifierFlagShift) k |= 0x04;
    if (f & NSEventModifierFlagControl) k |= 0x08;
    if (b & 4) k |= 0x10;
    return k;
}
- (LPARAM)pos:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    return MAKELPARAM((int)p.x, (int)p.y);
}
- (void)button:(NSEvent *)e message:(uint32_t)msg vk:(int)vk down:(int)down {
    vx_mouse_button(vk, down);
    // RI_MOUSE_*_BUTTON_DOWN/UP: left 0x1/0x2, right 0x4/0x8, middle 0x10/0x20.
    int shift = vk == 1 ? 0 : vk == 2 ? 2 : 4;
    vx_raw_mouse(0, 0, (uint16_t)((down ? 1 : 2) << shift), 0);
    if (down && e.clickCount == 2 && msg == WM_LBUTTONDOWN) msg = WM_LBUTTONDBLCLK;
    vx_window_post(w->hwnd, msg, mouse_keys(e), [self pos:e]);
}
- (void)mouseDown:(NSEvent *)e        { [self button:e message:WM_LBUTTONDOWN vk:1 down:1]; }
- (void)mouseUp:(NSEvent *)e          { [self button:e message:WM_LBUTTONUP vk:1 down:0]; }
- (void)rightMouseDown:(NSEvent *)e   { [self button:e message:WM_RBUTTONDOWN vk:2 down:1]; }
- (void)rightMouseUp:(NSEvent *)e     { [self button:e message:WM_RBUTTONUP vk:2 down:0]; }
- (void)otherMouseDown:(NSEvent *)e   { if (e.buttonNumber == 2) [self button:e message:WM_MBUTTONDOWN vk:4 down:1]; }
- (void)otherMouseUp:(NSEvent *)e     { if (e.buttonNumber == 2) [self button:e message:WM_MBUTTONUP vk:4 down:0]; }
- (void)mouseMoved:(NSEvent *)e {
    // Raw movement: whole-pixel deltas, the remainder carried to the next event.
    raw_x += e.deltaX; raw_y += e.deltaY;
    int dx = (int)raw_x, dy = (int)raw_y;
    raw_x -= dx; raw_y -= dy;
    vx_raw_mouse(dx, dy, 0, 0);
    vx_window_post(w->hwnd, WM_MOUSEMOVE, mouse_keys(e), [self pos:e]);
}
- (void)mouseDragged:(NSEvent *)e     { [self mouseMoved:e]; }
- (void)rightMouseDragged:(NSEvent *)e { [self mouseMoved:e]; }
- (void)otherMouseDragged:(NSEvent *)e { [self mouseMoved:e]; }
- (void)scrollWheel:(NSEvent *)e {
    // WHEEL_DELTA (120) per notch; trackpads deliver fractions, accumulated here.
    wheel += e.hasPreciseScrollingDeltas ? e.scrollingDeltaY * 120.0 / 40.0 : e.scrollingDeltaY * 120.0;
    int delta = (int)wheel;
    if (!delta) return;
    wheel -= delta;
    vx_raw_mouse(0, 0, 0x400 /* RI_MOUSE_WHEEL */, (int16_t)delta);
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSRect screen = [self.window convertRectToScreen:[self convertRect:NSMakeRect(p.x, p.y, 0, 0) toView:nil]];
    vx_window_post(w->hwnd, WM_MOUSEWHEEL, (WPARAM)(uint16_t)mouse_keys(e) | ((WPARAM)(uint16_t)(int16_t)delta << 16),
                   MAKELPARAM((int)screen.origin.x, (int)(main_height() - screen.origin.y)));
}

- (void)keyDown:(NSEvent *)e {
    int alt = (e.modifierFlags & NSEventModifierFlagOption) != 0;
    vx_key_event(w, e.keyCode, 1, e.ARepeat, alt);
    if (e.modifierFlags & NSEventModifierFlagCommand) return;      // Cmd+key belongs to the Mac (menus)
    NSString *s = e.characters;
    uint16_t buf[16]; int n = 0;
    for (NSUInteger i = 0; i < s.length && n < 16; i++) {
        unichar c = [s characterAtIndex:i];
        if (c >= 0xf700 && c <= 0xf8ff) continue;                   // arrows, function keys: no character
        if (c == 0x7f) c = 0x08;                                     // the Mac's Delete is Backspace
        if (c == 0x03) c = 0x0d;                                     // keypad Enter
        buf[n++] = c;
    }
    if (n) vx_char_event(w, buf, n);
}
- (void)keyUp:(NSEvent *)e { vx_key_event(w, e.keyCode, 0, 0, (e.modifierFlags & NSEventModifierFlagOption) != 0); }
- (void)flagsChanged:(NSEvent *)e {
    NSEventModifierFlags f = e.modifierFlags;
    int down;
    switch (e.keyCode) {
    case 56: case 60: down = (f & NSEventModifierFlagShift) != 0; break;
    case 59: case 62: down = (f & NSEventModifierFlagControl) != 0; break;
    case 58: case 61: down = (f & NSEventModifierFlagOption) != 0; break;
    case 55: case 54: down = (f & NSEventModifierFlagCommand) != 0; break;
    case 57:          down = (f & NSEventModifierFlagCapsLock) != 0; break;
    default: return;
    }
    vx_key_event(w, e.keyCode, down, 0, 0);
}
@end

// ---- the window delegate ------------------------------------------------------------------------------------------------
@interface VXWindowDelegate : NSObject <NSWindowDelegate> {
@public
    vx_window *w;
}
@end

@implementation VXWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)sender { vx_window_post(w->hwnd, WM_CLOSE, 0, 0); return NO; }
- (void)windowDidResize:(NSNotification *)n {
    NSWindow *win = n.object;
    if ([win.contentView.layer isKindOfClass:[CAMetalLayer class]]) {
        CAMetalLayer *ml = (CAMetalLayer *)win.contentView.layer;
        CGFloat scale = win.backingScaleFactor;
        ml.contentsScale = scale;
        ml.drawableSize = CGSizeMake(win.contentView.bounds.size.width * scale, win.contentView.bounds.size.height * scale);
    }
    vx_window_moved(w, windows_rect(win.frame));
    NSSize s = win.contentView.frame.size;
    vx_window_resized(w, (int)s.width, (int)s.height);
    vx_cocoa_redraw(w);
}
- (void)windowDidMove:(NSNotification *)n { vx_window_moved(w, windows_rect(((NSWindow *)n.object).frame)); }
- (void)windowDidBecomeKey:(NSNotification *)n { vx_window_activated(w, 1); }
- (void)windowDidResignKey:(NSNotification *)n { vx_window_activated(w, 0); }
- (void)windowDidMiniaturize:(NSNotification *)n { vx_window_minimized(w, 1); }
- (void)windowDidDeminiaturize:(NSNotification *)n { vx_window_minimized(w, 0); }
@end

// ---- operations requested by user32 -------------------------------------------------------------------------------------
static NSWindow *native(vx_window *w) { return (__bridge NSWindow *)w->native; }

// ---- pictures on the screen (present.c commits them) -----------------------------------------------------------------------
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
#define MIN_PRESENT_INTERVAL 0.008
static void setup_layer(CALayer *l) {
    l.contentsGravity = kCAGravityResize;                  // (the picture is the layer's size, but for a moment of resizing)
    l.magnificationFilter = kCAFilterNearest;              // one GDI pixel a point: whole device pixels on a Retina display
    l.contentsScale = 1;
    l.geometryFlipped = YES;                               // (y down, as AppKit makes the layer of a flipped view)
    l.opaque = YES;
    l.anchorPoint = CGPointZero;
}
// The window's own layer: the GDI one, unless a Direct3D or Vulkan swap chain made it a CAMetalLayer.
static CALayer *root_layer(vx_window *w) {
    CALayer *l = native(w).contentView.layer;
    return l && ![l isKindOfClass:[CAMetalLayer class]] ? l : nil;
}
// Main thread: w's committed picture into its layer, its visible children into sublayers at their places (y measured
// from the top of the parent's client area), stale ones dropped.
static void present_layer(CALayer *layer, vx_window *w, int depth) {
    // A child window GDI never drew into shows its parent's picture beneath it, as a child that paints nothing does on
    // Windows (Chromium's "Chrome Legacy Window" covers the whole page and never paints).
    pthread_mutex_lock(&w->lock);
    int drawn = w->drawn;
    pthread_mutex_unlock(&w->lock);
    if (depth > 0 && !drawn) { layer.contents = nil; layer.opaque = NO; }
    else if (w->committed != w->presented || !layer.contents) {
        if (depth > 0) layer.opaque = YES;
        int alpha = 0;
        CGImageRef img = vx_window_front_image(w, &alpha);
        if (img) { layer.contents = (__bridge id)img; CGImageRelease(img); }
    }
    vx_window *kids[256];
    int n = depth < 16 ? vx_window_children(w, kids, 256, 1) : 0;
    NSMutableArray *subs = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
    CGFloat height = layer.bounds.size.height;
    for (int i = 0; i < n; i++) {
        vx_window *c = kids[i];
        CALayer *cl = (__bridge CALayer *)c->layer;
        if (!cl) { cl = [CALayer layer]; setup_layer(cl); c->layer = (__bridge_retained void *)cl; }
        pthread_mutex_lock(&c->lock);
        RECT r = c->rect;
        pthread_mutex_unlock(&c->lock);
        CGFloat y = layer.geometryFlipped ? r.top : height - r.bottom;
        cl.frame = CGRectMake(r.left, y, r.right - r.left, r.bottom - r.top);
        present_layer(cl, c, depth + 1);
        [subs addObject:cl];
    }
    if ((layer.sublayers.count || subs.count) && ![layer.sublayers isEqualToArray:subs]) layer.sublayers = subs;
}
static void present_window(vx_window *w) {
    __atomic_store_n(&w->redraw_pending, 0, __ATOMIC_RELEASE);
    w->shown_at = now_s();
    CALayer *root = w->native ? root_layer(w) : nil;
    if (!root) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];                 // (no cross-fade between pictures)
    present_layer(root, w, 0);
    [CATransaction commit];
}
// Asynchronous: w's top-level window shows the committed pictures, at most once in MIN_PRESENT_INTERVAL.
void vx_cocoa_redraw(vx_window *w) {
    w = vx_window_top(w);
    if (!w || !w->native || __atomic_exchange_n(&w->redraw_pending, 1, __ATOMIC_ACQ_REL)) return;
    double wait = w->shown_at + MIN_PRESENT_INTERVAL - now_s();
    dispatch_block_t b = ^{
        if (w->native && !w->destroyed) present_window(w);
        else __atomic_store_n(&w->redraw_pending, 0, __ATOMIC_RELEASE);
    };
    if (wait > 0) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(wait * 1e9)), dispatch_get_main_queue(), b);
    else dispatch_async(dispatch_get_main_queue(), b);
}
// Every window's committed pictures handed over now, and the transaction sent to the window server (DwmFlush).
void vx_cocoa_present_now(void) {
    pthread_mutex_lock(&g_ui_lock);
    int ready = g_ui_ready;
    pthread_mutex_unlock(&g_ui_lock);
    if (!ready) return;
    on_main(^{
        for (NSWindow *win in NSApp.windows)
            if ([win.contentView isKindOfClass:[VXView class]]) { vx_window *w = ((VXView *)win.contentView)->w; if (w && w->native) present_window(w); }
        [CATransaction flush];
    });
}
double vx_cocoa_refresh_period(void) {
    static double period;
    if (!period) {
        CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
        double hz = m ? CGDisplayModeGetRefreshRate(m) : 0;
        if (m) CGDisplayModeRelease(m);
        period = 1.0 / (hz >= 24 ? hz : 60);
    }
    return period;
}
void vx_cocoa_window_gone(vx_window *w) {
    pthread_mutex_lock(&g_ui_lock);
    int ready = g_ui_ready;
    pthread_mutex_unlock(&g_ui_lock);
    if (!ready) return;
    dispatch_async(dispatch_get_main_queue(), ^{             // (on the main thread, which may be making the layer now)
        if (!w->layer) return;
        CALayer *l = (__bridge_transfer CALayer *)w->layer;
        w->layer = NULL;
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        [l removeFromSuperlayer];
        [CATransaction commit];
    });
}
// WS_EX_LAYERED: transparent where the picture is (per-pixel alpha or a color key), the constant alpha as the window's,
// invisible until SetLayeredWindowAttributes or UpdateLayeredWindow; WS_EX_TRANSPARENT as well: clicks go through.
static void apply_layered(vx_window *w, DWORD ex, int mode, DWORD flags, uint8_t alpha) {
    NSWindow *win = native(w);
    CALayer *root = root_layer(w);
    int layered = (ex & WS_EX_LAYERED) != 0;
    int clear = layered && (mode == 2 ? !(flags & 4) && ((flags & 1) || ((flags & 2) && (flags & 0x100))) : mode == 1 && (flags & 1));
    win.opaque = !clear;
    win.backgroundColor = clear ? NSColor.clearColor : NSColor.blackColor;
    win.hasShadow = !clear;
    if (root) { root.opaque = !clear; root.backgroundColor = clear ? NULL : CGColorGetConstantColor(kCGColorBlack); }
    win.alphaValue = !layered ? 1 : !mode ? 0 : (flags & 2) ? alpha / 255.0 : 1;
    win.ignoresMouseEvents = layered && (ex & WS_EX_TRANSPARENT);
}
void vx_cocoa_set_layered(vx_window *w) {
    if (!w->native) return;
    DWORD ex = w->exstyle, flags = w->layer_flags;
    int mode = w->layered;
    uint8_t alpha = w->layer_alpha;
    on_main(^{ if (w->native) apply_layered(w, ex, mode, flags, alpha); });
}
// What the window server has of one of this program's windows (its own windows need no screen-recording
// permission), looked up at run time: the SDK no longer declares CGWindowListCreateImage.
typedef CGImageRef (*window_image_fn)(CGRect, uint32_t, uint32_t, uint32_t);
static CGImageRef window_server_image(uint32_t number) {
    static window_image_fn fn;
    static int looked;
    if (!looked) { fn = (window_image_fn)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage"); looked = 1; }
    // kCGWindowListOptionIncludingWindow; kCGWindowImageBoundsIgnoreFraming | kCGWindowImageBestResolution
    return fn ? fn(CGRectNull, 8, number, 1 | 8) : NULL;
}
// An image in sRGB, the color space the pictures are tagged with (the window server's are in the display's).
static CGImageRef in_srgb(CGImageRef img) {
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(NULL, w, h, 8, 0, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!c) return NULL;
    CGContextSetInterpolationQuality(c, kCGInterpolationNone);
    CGContextSetBlendMode(c, kCGBlendModeCopy);
    CGContextDrawImage(c, CGRectMake(0, 0, w, h), img);
    CGImageRef out = CGBitmapContextCreateImage(c);
    CGContextRelease(c);
    return out;
}
// A layer tree rendered by Core Animation at a scale. The layers are geometry-flipped (y down, as AppKit makes a
// flipped view's layer), which renderInContext draws upside down into a bitmap: the context is flipped to match.
static CGImageRef layer_image(CALayer *layer, CGFloat scale) {
    CGSize size = layer.bounds.size;
    size_t pw = (size_t)ceil(size.width * scale), ph = (size_t)ceil(size.height * scale);
    if (!pw || !ph) return NULL;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef c = CGBitmapContextCreate(NULL, pw, ph, 8, 0, cs, kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!c) return NULL;
    CGContextSetInterpolationQuality(c, kCGInterpolationNone);
    if (layer.geometryFlipped) { CGContextTranslateCTM(c, 0, ph); CGContextScaleCTM(c, 1, -1); }
    CGContextScaleCTM(c, scale, scale);
    [layer renderInContext:c];
    CGImageRef img = CGBitmapContextCreateImage(c);
    CGContextRelease(c);
    return img;
}
// What the Mac shows of a window's client area (a child window's too), as a PNG file at the window's backing scale:
// the window server's pixels of the window when it can be asked (in sRGB again, and with the window's alpha), else
// Core Animation's rendering of its layers.
// info: JSON fields (where the pixels came from, scale, size in pixels, the window's alpha, whether it is opaque).
int vx_cocoa_snapshot_view(vx_window *w, const char *png, char *info, size_t cap) {
    pthread_mutex_lock(&g_ui_lock);
    int ready = g_ui_ready;
    pthread_mutex_unlock(&g_ui_lock);
    vx_window *top = vx_window_top(w);
    if (!ready || !top || !top->native) return 0;
    __block uint32_t number = 0;
    __block CGFloat scale = 1, alpha = 1, frame_w = 0;
    __block BOOL opaque = YES, visible = NO;
    __block CGRect crop = CGRectZero;                      // points, from the top left of the window's frame
    __block CGImageRef fallback = NULL;
    on_main(^{
        if (!top->native) return;
        present_window(top);
        [CATransaction flush];
        NSWindow *win = native(top);
        number = (uint32_t)win.windowNumber; scale = win.backingScaleFactor; alpha = win.alphaValue; opaque = win.opaque;
        visible = win.isVisible;
        NSRect f = win.frame, c = win.contentView.frame;
        frame_w = f.size.width;
        POINT o = { 0, 0 }, at = { 0, 0 };
        if (w != top) { vx_window_screen_origin(top, &o); vx_window_screen_origin(w, &at); }
        crop = CGRectMake(c.origin.x + at.x - o.x, f.size.height - (c.origin.y + c.size.height) + at.y - o.y, w->client_w, w->client_h);
        CALayer *layer = w == top ? root_layer(top) : (__bridge CALayer *)w->layer;
        if (layer) fallback = layer_image(layer, scale);
    });
    const char *source = "layers";
    CGImageRef img = NULL;
    if (visible && number) {
        usleep((useconds_t)(3 * vx_cocoa_refresh_period() * 1e6));   // (the window server shows the transaction a frame later)
        CGImageRef full = window_server_image(number);
        if (full && frame_w > 0) {
            CGFloat s = CGImageGetWidth(full) / frame_w;
            CGImageRef part = CGImageCreateWithImageInRect(full, CGRectMake(crop.origin.x * s, crop.origin.y * s, crop.size.width * s, crop.size.height * s));
            img = part ? in_srgb(part) : NULL;
            if (part) CGImageRelease(part);
            if (img) { source = "window-server"; scale = s; }
        }
        if (full) CGImageRelease(full);
    }
    if (!img) { img = fallback; fallback = NULL; }
    if (fallback) CGImageRelease(fallback);
    int ok = img && vx_png_write(img, png);
    if (info && cap) snprintf(info, cap, "\"source\":\"%s\",\"scale\":%g,\"width\":%zu,\"height\":%zu,\"window_alpha\":%.3f,\"opaque\":%s",
                              source, (double)scale, img ? CGImageGetWidth(img) : 0, img ? CGImageGetHeight(img) : 0, (double)alpha, opaque ? "true" : "false");
    if (img) CGImageRelease(img);
    return ok;
}

// The first window shown (or message box): the program becomes an ordinary app with a Dock icon and the menu bar, and
// takes the Mac's focus unless VXRT_NO_ACTIVATE. Main thread.
static void become_regular(void) {
    if (NSApp.activationPolicy == NSApplicationActivationPolicyRegular) return;
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    if (activation_allowed()) [NSApp activateIgnoringOtherApps:YES];
}

void vx_cocoa_create(vx_window *w) {
    on_main(^{
        NSWindowStyleMask mask = style_mask(w->style);
        NSRect frame = cocoa_rect(w->rect);
        NSRect content = [NSWindow contentRectForFrameRect:frame styleMask:mask];
        NSWindow *win = [[NSWindow alloc] initWithContentRect:content styleMask:mask backing:NSBackingStoreBuffered defer:NO];
        win.releasedWhenClosed = NO;
        win.acceptsMouseMovedEvents = YES;
        win.title = [NSString stringWithUTF8String:w->title ? w->title : ""] ?: @"";
        VXView *view = [[VXView alloc] initWithFrame:NSMakeRect(0, 0, content.size.width, content.size.height)];
        view->w = w;
        CALayer *root = [CALayer layer];
        setup_layer(root);
        root.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
        view.layer = root;                                     // (set before wantsLayer: a layer-hosting view)
        view.wantsLayer = YES;
        win.contentView = view;
        [win makeFirstResponder:view];
        VXWindowDelegate *d = [VXWindowDelegate new];
        d->w = w;
        win.delegate = d;
        if (w->center) {                                       // (an owned window, a dialog: over its owner, on its screen)
            vx_window *owner = w->parent ? vx_window_get(w->parent) : NULL;
            NSWindow *ow = owner && owner->native ? (__bridge NSWindow *)owner->native : nil;
            if (ow) { NSRect o = ow.frame, f = win.frame; [win setFrameOrigin:NSMakePoint(NSMidX(o) - f.size.width / 2, NSMidY(o) - f.size.height / 2)]; }
            else [win center];
        }
        w->native = (__bridge_retained void *)win;
        w->delegate = (__bridge_retained void *)d;
        pthread_mutex_lock(&w->lock);
        w->rect = windows_rect(win.frame);
        pthread_mutex_unlock(&w->lock);
        NSSize s = win.contentView.frame.size;
        vx_window_resized(w, (int)s.width, (int)s.height);
        if (w->exstyle & WS_EX_LAYERED) apply_layered(w, w->exstyle, w->layered, w->layer_flags, w->layer_alpha);
    });
}

void vx_cocoa_destroy(vx_window *w) {
    if (!w->native) return;
    on_main(^{
        NSWindow *win = (__bridge_transfer NSWindow *)w->native;
        VXWindowDelegate *d = (__bridge_transfer VXWindowDelegate *)w->delegate;
        // VXRT_SNAPSHOT=file.png: the view draws itself into that file before the window goes (for tests).
        const char *snap = getenv("VXRT_SNAPSHOT");
        if (snap && *snap) {
            present_window(w);
            CALayer *layer = root_layer(w);
            CGImageRef img = layer ? layer_image(layer, win.backingScaleFactor) : NULL;
            if (img) { vx_png_write(img, snap); CGImageRelease(img); }
        }
        (void)d;
        w->native = w->delegate = NULL;
        win.delegate = nil;
        [win orderOut:nil];
        [win close];
    });
}

void vx_cocoa_show(vx_window *w, int cmd) {
    if (!w->native) return;
    on_main(^{
        NSWindow *win = native(w);
        if (cmd) become_regular();
        switch (cmd) {
        case 0: [win orderOut:nil]; break;                                       // SW_HIDE
        case 2: case 6: case 7: case 11: [win miniaturize:nil]; break;           // the minimized forms
        case 3: if (!win.isZoomed) [win zoom:nil]; [win makeKeyAndOrderFront:nil]; break;   // SW_MAXIMIZE
        case 4: case 8: [win orderFront:nil]; break;                             // without activation
        default:
            if (win.isMiniaturized) [win deminiaturize:nil];
            if (activation_allowed()) { [win makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; }
            else { [win orderFront:nil]; vx_window_shown_inactive(w); }
        }
    });
}

void vx_cocoa_activate(vx_window *w) {
    if (!w->visible) return;                     // (SetForegroundWindow does not show a hidden window: Steam's -silent)
    if (w->native && !activation_allowed()) { vx_window_shown_inactive(w); return; }
    if (!w->native || !activation_allowed()) return;
    on_main(^{ [native(w) makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES]; });
}

void vx_cocoa_set_title(vx_window *w) {
    if (!w->native) return;
    char *t = strdup(w->title ? w->title : "");
    on_main(^{ native(w).title = [NSString stringWithUTF8String:t] ?: @""; });
    free(t);
}

void vx_cocoa_set_frame(vx_window *w) {
    if (!w->native) return;
    RECT r = w->rect;
    on_main(^{ [native(w) setFrame:cocoa_rect(r) display:YES]; });
}

void vx_cocoa_set_style(vx_window *w) {
    if (!w->native) return;
    DWORD style = w->style;
    RECT r = w->rect;
    on_main(^{
        NSWindow *win = native(w);
        win.styleMask = style_mask(style);
        [win setFrame:cocoa_rect(r) display:YES];
        [win makeFirstResponder:win.contentView];
    });
}


// MessageBox: an NSAlert with the buttons of the MB_* type; returns the ID* of the button pressed.
static const struct { const char *labels[3]; int ids[3]; } message_box_kinds[] = {
    { { "OK" }, { 1 } }, { { "OK", "Cancel" }, { 1, 2 } }, { { "Abort", "Retry", "Ignore" }, { 3, 4, 5 } },
    { { "Yes", "No", "Cancel" }, { 6, 7, 2 } }, { { "Yes", "No" }, { 6, 7 } }, { { "Retry", "Cancel" }, { 4, 2 } },
    { { "Cancel", "Try Again", "Continue" }, { 2, 10, 11 } },
};
int vx_cocoa_message_box(const char *text, const char *caption, unsigned type) {
    __block int result = 1;
    // VXRT_MESSAGEBOX=auto: the box's first button, pressed at once (unattended runs; the text is in the log already)
    const char *automatic = getenv("VXRT_MESSAGEBOX");
    if (automatic && !strcmp(automatic, "auto")) return message_box_kinds[(type & 0xf) > 6 ? 0 : (type & 0xf)].ids[0];
    char *t = strdup(text ? text : ""), *c = strdup(caption ? caption : "");
    on_main(^{
        NSAlert *a = [NSAlert new];
        a.messageText = [NSString stringWithUTF8String:c] ?: @"";
        a.informativeText = [NSString stringWithUTF8String:t] ?: @"";
        unsigned icon = type & 0xf0;
        a.alertStyle = icon == 0x10 ? NSAlertStyleCritical : icon == 0x30 ? NSAlertStyleWarning : NSAlertStyleInformational;
        #define kinds message_box_kinds
        unsigned k = type & 0xf;
        if (k > 6) k = 0;
        int n = 0;
        for (; n < 3 && kinds[k].labels[n]; n++) [a addButtonWithTitle:[NSString stringWithUTF8String:kinds[k].labels[n]]];
        become_regular();
        if (activation_allowed()) [NSApp activateIgnoringOtherApps:YES];
        NSModalResponse r = [a runModal];
        int index = (int)(r - NSAlertFirstButtonReturn);
        result = index >= 0 && index < n ? kinds[k].ids[index] : kinds[k].ids[0];
        #undef kinds
    });
    free(t); free(c);
    return result;
}

// A CAMetalLayer as the content of the window's view (Direct3D swap chains present into it); the view keeps
// delivering input. Returns the layer (owned by the view).
void *vx_cocoa_metal_layer(vx_window *w, void *device, int pixel_format) {
    if (!w->native) return NULL;
    __block void *result = NULL;
    on_main(^{
        NSWindow *win = native(w);
        NSView *v = win.contentView;
        CAMetalLayer *ml = [v.layer isKindOfClass:[CAMetalLayer class]] ? (CAMetalLayer *)v.layer : [CAMetalLayer layer];
        ml.device = (__bridge id<MTLDevice>)device;
        ml.pixelFormat = (MTLPixelFormat)pixel_format;
        ml.framebufferOnly = YES;
        CGFloat scale = win.backingScaleFactor;
        ml.contentsScale = scale;
        ml.drawableSize = CGSizeMake(v.bounds.size.width * scale, v.bounds.size.height * scale);
        if (v.layer != ml) { v.layer = ml; v.wantsLayer = YES; }
        result = (__bridge void *)ml;
    });
    return result;
}

void *vx_cocoa_metal_layer_bgra(vx_window *w, void *device) {
    return vx_cocoa_metal_layer(w, device, (int)MTLPixelFormatBGRA8Unorm);
}

void vx_cocoa_beep(void) { ui_ensure(); dispatch_async(dispatch_get_main_queue(), ^{ NSBeep(); }); }

#ifdef __aarch64__
#include "thunks/cocoa.m.inc"
#endif
