// visionOS (VISIONOS_PORT.md): the windows of src/cocoa.m on UIKit, in the VX Runtime app's one flat window. The app
// gives its root view (vxrt_xros_attach); that view is the program's display (its size in points is the screen
// size the program sees). Each top-level Windows window is a subview at its rectangle (UIKit's y runs down, as
// Windows' does); a GDI window's picture is its layer's contents as on the Mac (present.c commits them), and a swap
// chain's CAMetalLayer is a sublayer filling the window. Touches are the left mouse button.
//
// Not here (stubs, logged): message boxes (the first button, as VXRT_MESSAGEBOX=auto), window snapshots, window
// dragging and the input method; the notification area, menus and the Windows clipboard are left to the stand-ins
// in desktop.c and user32.c (src/xros_stubs.c).
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>
#include <time.h>
#include "ui.h"

int vx_xros_display_w = 1280, vx_xros_display_h = 720, vx_xros_display_hz = 90;
double vx_xros_pointer_x, vx_xros_pointer_y, vx_xros_last_input;

// ---- the main thread: the app's ---------------------------------------------------------------------------------------
static pthread_mutex_t g_ui_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ui_cond = PTHREAD_COND_INITIALIZER;
static int g_ui_ready;
static UIView *g_display;                     // (the app's root view)
static CGFloat g_scale = 2;

void vxrt_xros_attach(UIView *display) {     // (the app, main thread: once its view has its size)
    g_display = display;
    CGSize s = display.bounds.size;
    g_scale = display.traitCollection.displayScale > 0 ? display.traitCollection.displayScale : 2;
    if (s.width >= 64 && s.height >= 64) { vx_xros_display_w = (int)s.width; vx_xros_display_h = (int)s.height; }
    pthread_mutex_lock(&g_ui_lock);
    g_ui_ready = 1;
    pthread_cond_broadcast(&g_ui_cond);
    pthread_mutex_unlock(&g_ui_lock);
}
static void ui_ensure(void) {
    pthread_mutex_lock(&g_ui_lock);
    while (!g_ui_ready) pthread_cond_wait(&g_ui_cond, &g_ui_lock);
    pthread_mutex_unlock(&g_ui_lock);
}
void vx_cocoa_ensure(void) { ui_ensure(); }
static void on_main(dispatch_block_t b) {
    ui_ensure();
    if ([NSThread isMainThread]) b(); else dispatch_sync(dispatch_get_main_queue(), b);
}

typedef struct { int (*program)(void *); void *arg; } program_start;
void vxrt_xros_program_exited(int rc);       // (the app: written to its log before the process ends)
static void *program_thread(void *p) {
    program_start *s = p;
    int rc = s->program(s->arg);
    fflush(stdout); fflush(stderr);
    vxrt_xros_program_exited(rc);
    _exit(rc);
}
// The loader's thread (the app started it): the program on a thread of its own with the stack it asks for; this one
// waits. (On the Mac the main thread runs AppKit instead.)
int vx_ui_run_main(int (*program)(void *), void *arg, size_t stack) {
    static program_start start;
    start.program = program; start.arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, (stack + 16383) & ~(size_t)16383);
    pthread_t t;
    if (pthread_create(&t, &a, program_thread, &start)) { vx_log("cannot start the program thread"); return 1; }
    pthread_join(t, NULL);
    return 0;
}

// ---- the window's view ----------------------------------------------------------------------------------------------
@interface VXWinView : UIView {
@public
    vx_window *w;
}
@end
@implementation VXWinView
- (POINT)screen:(UITouch *)t {
    CGPoint p = [t locationInView:self];
    POINT o; vx_window_screen_origin(w, &o);
    vx_xros_pointer_x = o.x + p.x; vx_xros_pointer_y = o.y + p.y;
    vx_xros_last_input = CFAbsoluteTimeGetCurrent();
    return (POINT){ o.x + (LONG)p.x, o.y + (LONG)p.y };
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e {
    POINT p = [self screen:touches.anyObject];
    vx_mouse_button(1, 1);
    vx_mouse_input(w, WM_LBUTTONDOWN, 0x01, p.x, p.y);
}
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e {
    POINT p = [self screen:touches.anyObject];
    vx_mouse_input(w, WM_MOUSEMOVE, 0x01, p.x, p.y);
}
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e {
    POINT p = [self screen:touches.anyObject];
    vx_mouse_button(1, 0);
    vx_mouse_input(w, WM_LBUTTONUP, 0, p.x, p.y);
}
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e { [self touchesEnded:touches withEvent:e]; }
- (void)layoutSubviews {
    [super layoutSubviews];
    for (CALayer *l in self.layer.sublayers)
        if ([l isKindOfClass:[CAMetalLayer class]]) {
            CAMetalLayer *ml = (CAMetalLayer *)l;
            ml.frame = self.bounds;
            ml.drawableSize = CGSizeMake(self.bounds.size.width * g_scale, self.bounds.size.height * g_scale);
        }
}
@end
static VXWinView *native(vx_window *w) { return (__bridge VXWinView *)w->native; }
static CAMetalLayer *metal_of(VXWinView *v) {
    for (CALayer *l in v.layer.sublayers) if ([l isKindOfClass:[CAMetalLayer class]]) return (CAMetalLayer *)l;
    return nil;
}

// ---- pictures (present.c commits them), as cocoa.m --------------------------------------------------------------------
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + t.tv_nsec / 1e9; }
#define MIN_PRESENT_INTERVAL 0.008
static void setup_layer(CALayer *l) {
    l.contentsGravity = kCAGravityResize;
    l.magnificationFilter = kCAFilterNearest;
    l.contentsScale = 1;
    l.opaque = YES;
    l.anchorPoint = CGPointZero;
}
static CALayer *root_layer(vx_window *w) { return w->native && !metal_of(native(w)) ? native(w).layer : nil; }
static void present_layer(CALayer *layer, vx_window *w, int depth) {
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
    for (int i = 0; i < n; i++) {
        vx_window *c = kids[i];
        CALayer *cl = (__bridge CALayer *)c->layer;
        if (!cl) { cl = [CALayer layer]; setup_layer(cl); c->layer = (__bridge_retained void *)cl; }
        pthread_mutex_lock(&c->lock);
        RECT r = c->rect;
        pthread_mutex_unlock(&c->lock);
        cl.frame = CGRectMake(r.left, r.top, r.right - r.left, r.bottom - r.top);
        present_layer(cl, c, depth + 1);
        [subs addObject:cl];
    }
    if ((layer.sublayers.count || subs.count) && ![layer.sublayers isEqualToArray:subs]) layer.sublayers = subs;
}
static void present_window(vx_window *w) {
    __atomic_store_n(&w->redraw_pending, 0, __ATOMIC_RELEASE);
    w->shown_at = now_s();
    CALayer *root = root_layer(w);
    if (!root) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    present_layer(root, w, 0);
    [CATransaction commit];
}
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
void vx_cocoa_present_now(void) {
    pthread_mutex_lock(&g_ui_lock);
    int ready = g_ui_ready;
    pthread_mutex_unlock(&g_ui_lock);
    if (!ready) return;
    on_main(^{
        for (UIView *v in g_display.subviews)
            if ([v isKindOfClass:[VXWinView class]]) { vx_window *w = ((VXWinView *)v)->w; if (w && w->native) present_window(w); }
        [CATransaction flush];
    });
}
double vx_cocoa_refresh_period(void) { return 1.0 / (vx_xros_display_hz >= 24 ? vx_xros_display_hz : 90); }
void vx_cocoa_window_gone(vx_window *w) {
    pthread_mutex_lock(&g_ui_lock);
    int ready = g_ui_ready;
    pthread_mutex_unlock(&g_ui_lock);
    if (!ready) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!w->layer) return;
        CALayer *l = (__bridge_transfer CALayer *)w->layer;
        w->layer = NULL;
        [CATransaction begin]; [CATransaction setDisableActions:YES];
        [l removeFromSuperlayer];
        [CATransaction commit];
    });
}
void vx_cocoa_set_layered(vx_window *w) {
    if (!w->native) return;
    DWORD ex = w->exstyle, flags = w->layer_flags;
    int mode = w->layered;
    uint8_t alpha = w->layer_alpha;
    on_main(^{
        if (!w->native) return;
        int layered = (ex & WS_EX_LAYERED) != 0;
        native(w).alpha = !layered ? 1 : !mode ? 0 : (flags & 2) ? alpha / 255.0 : 1;
        native(w).userInteractionEnabled = !(layered && (ex & WS_EX_TRANSPARENT));
    });
}
int vx_cocoa_snapshot_view(vx_window *w, const char *png, char *info, size_t cap) {
    snprintf(info, cap, "window snapshots are not made on visionOS");
    return 0;
}

// ---- operations requested by user32 -------------------------------------------------------------------------------------
int vx_cocoa_title_height(void) { return 0; }           // (no title bars: the app's window has the system's)
static CGRect view_rect(RECT r) { return CGRectMake(r.left, r.top, r.right - r.left, r.bottom - r.top); }

void vx_cocoa_create(vx_window *w) {
    on_main(^{
        VXWinView *v = [[VXWinView alloc] initWithFrame:view_rect(w->rect)];
        v->w = w;
        v.multipleTouchEnabled = NO;
        v.backgroundColor = UIColor.blackColor;
        setup_layer(v.layer);
        v.layer.anchorPoint = CGPointMake(0.5, 0.5);          // (a view's own layer keeps UIKit's anchor)
        v.hidden = YES;
        if (w->parent || (w->style & WS_CHILD)) { /* (child windows are sublayers of their top-level window's view) */ }
        [g_display addSubview:v];
        w->native = (__bridge_retained void *)v;
        CGSize s = v.bounds.size;
        vx_window_resized(w, (int)s.width, (int)s.height);
    });
}
void vx_cocoa_destroy(vx_window *w) {
    if (!w->native) return;
    on_main(^{
        VXWinView *v = (__bridge_transfer VXWinView *)w->native;
        w->native = w->delegate = NULL;
        v->w = NULL;
        [v removeFromSuperview];
    });
}
void vx_cocoa_show(vx_window *w, int cmd) {
    if (!w->native) return;
    on_main(^{
        VXWinView *v = native(w);
        switch (cmd) {
        case 0: case 2: case 6: case 7: case 11: v.hidden = YES; break;           // SW_HIDE and the minimized forms
        case 3:                                                                     // SW_MAXIMIZE: the whole display
            v.frame = g_display.bounds; v.hidden = NO; [g_display bringSubviewToFront:v];
            vx_window_resized(w, (int)v.bounds.size.width, (int)v.bounds.size.height);
            vx_window_activated(w, 1);
            break;
        default: v.hidden = NO; [g_display bringSubviewToFront:v]; vx_window_activated(w, 1);
        }
    });
}
void vx_cocoa_activate(vx_window *w) {
    if (!w->visible || !w->native) return;
    on_main(^{ [g_display bringSubviewToFront:native(w)]; vx_window_activated(w, 1); });
}
void vx_cocoa_drag(vx_window *w) { }
void vx_cocoa_ime_cancel(vx_window *w) { }
void vx_cocoa_set_title(vx_window *w) { }
void vx_cocoa_set_frame(vx_window *w) {
    if (!w->native) return;
    RECT r = w->rect;
    on_main(^{
        VXWinView *v = native(w);
        v.frame = view_rect(r);
        [v setNeedsLayout]; [v layoutIfNeeded];
        vx_window_resized(w, (int)v.bounds.size.width, (int)v.bounds.size.height);
    });
}
void vx_cocoa_set_style(vx_window *w) { vx_cocoa_set_frame(w); }
int vx_cocoa_message_box(const char *text, const char *caption, unsigned type) {
    static const int first[] = { 1, 1, 3, 6, 6, 4, 2 };
    vx_log("MessageBox (visionOS: answered with its first button): %s: %s", caption ? caption : "", text ? text : "");
    return first[(type & 0xf) > 6 ? 0 : (type & 0xf)];
}
void vx_cocoa_beep(void) { }

// A CAMetalLayer filling the window's view (Direct3D and Vulkan swap chains present into it).
void *vx_cocoa_metal_layer(vx_window *w, void *device, int pixel_format) {
    if (!w->native) return NULL;
    __block void *result = NULL;
    on_main(^{
        VXWinView *v = native(w);
        CAMetalLayer *ml = metal_of(v);
        if (!ml) { ml = [CAMetalLayer layer]; [v.layer addSublayer:ml]; }
        ml.device = (__bridge id<MTLDevice>)device;
        ml.pixelFormat = (MTLPixelFormat)pixel_format;
        ml.framebufferOnly = YES;
        ml.contentsScale = g_scale;
        ml.frame = v.bounds;
        ml.drawableSize = CGSizeMake(v.bounds.size.width * g_scale, v.bounds.size.height * g_scale);
        v.layer.contents = nil;
        result = (__bridge void *)ml;
    });
    return result;
}
void *vx_cocoa_metal_layer_bgra(vx_window *w, void *device) { return vx_cocoa_metal_layer(w, device, (int)MTLPixelFormatBGRA8Unorm); }

// ---- the clipboard's text (Command Clips) -------------------------------------------------------------------------------
int vx_ui_clipboard_get(char *out, size_t cap) {
    __block NSString *s = nil;
    on_main(^{ s = UIPasteboard.generalPasteboard.string; });
    if (!s || !cap) return 0;
    snprintf(out, cap, "%s", s.UTF8String ?: "");
    return 1;
}
int vx_ui_clipboard_set(const char *utf8) {
    NSString *s = [NSString stringWithUTF8String:utf8] ?: @"";
    on_main(^{ UIPasteboard.generalPasteboard.string = s; });
    return 1;
}
