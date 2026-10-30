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
#import <GameController/GameController.h>
#include <time.h>
#include "ui.h"

int vx_xros_display_w = 1280, vx_xros_display_h = 720, vx_xros_display_hz = 90;
double vx_xros_pointer_x, vx_xros_pointer_y, vx_xros_last_input;

// ---- the main thread: the app's ---------------------------------------------------------------------------------------
static pthread_mutex_t g_ui_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ui_cond = PTHREAD_COND_INITIALIZER;
static int g_ui_ready;
static UIView *g_display;                     // (the app's root view)
static vx_window *g_key_window;               // (the top-level window last shown or activated: keyboard and mouse go there)
static void input_start(void);
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
    input_start();
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
        if (g_key_window == w) g_key_window = NULL;
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
            vx_window_activated(w, 1); g_key_window = w;
            break;
        default: v.hidden = NO; [g_display bringSubviewToFront:v]; vx_window_activated(w, 1); g_key_window = w;
        }
    });
}
void vx_cocoa_activate(vx_window *w) {
    if (!w->visible || !w->native) return;
    on_main(^{ [g_display bringSubviewToFront:native(w)]; vx_window_activated(w, 1); g_key_window = w; });
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

// ---- present pacing -------------------------------------------------------------------------------------------------
// visionOS has no CAMetalLayer.displaySyncEnabled, and nextDrawable did not block: Present(1) ran 1141 times a second
// on the headset. A display link (90 Hz there) on a thread of its own counts refreshes; a present with sync interval
// n waits until n refreshes have passed since the last one (as a flip-model present waits for its vblanks), and one
// that is late does not wait. VXRT_XROS_PACE=0: no pacing.
static pthread_mutex_t g_pace_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pace_cond = PTHREAD_COND_INITIALIZER;
static uint64_t g_ticks, g_last_present_tick;
static double g_vr_tick_time;
float vx_xros_vr_real_hz(void);   // (src/xros_vr.m: VXRT_VR_REAL_HZ, a fixed real rate in VR)
int vx_xros_vr_on(void);
@interface VXPaceTarget : NSObject
@end
@implementation VXPaceTarget
- (void)tick:(CADisplayLink *)l {
    pthread_mutex_lock(&g_pace_lock);
    if (CACurrentMediaTime() - g_vr_tick_time > 0.1) {      // (the immersive presenter's frames pace instead while they come)
        g_ticks++;
        pthread_cond_broadcast(&g_pace_cond);
    }
    pthread_mutex_unlock(&g_pace_lock);
}
@end
// The immersive presenter's compositor frames (src/xros_vr.m): in VR, presents are paced by those, not the display link.
void vx_xros_pace_tick(void) {
    pthread_mutex_lock(&g_pace_lock);
    g_vr_tick_time = CACurrentMediaTime();
    g_ticks++;
    pthread_cond_broadcast(&g_pace_cond);
    pthread_mutex_unlock(&g_pace_lock);
}
static void *pace_thread(void *arg) {
    @autoreleasepool {
        VXPaceTarget *t = [VXPaceTarget new];
        CADisplayLink *link = [CADisplayLink displayLinkWithTarget:t selector:@selector(tick:)];
        int hz = vx_xros_display_hz >= 24 ? vx_xros_display_hz : 90;
        link.preferredFrameRateRange = CAFrameRateRangeMake(hz, hz, hz);
        [link addToRunLoop:NSRunLoop.currentRunLoop forMode:NSRunLoopCommonModes];
        vx_log("present pacing: display link at %d Hz for sync-interval presents", hz);
        for (;;) [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:NSDate.distantFuture];
    }
    return NULL;
}
static void pace_start(void) { pthread_t t; pthread_create(&t, NULL, pace_thread, NULL); pthread_detach(t); }
void vx_xros_vsync_wait(unsigned sync) {
    static int on = -1;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (on < 0) on = !getenv("VXRT_XROS_PACE") || atoi(getenv("VXRT_XROS_PACE")) != 0;
    if (!on || !sync) return;
    static unsigned min_ticks = ~0u;
    if (min_ticks == ~0u) {                                   // (VR with VXRT_VR_REAL_HZ: at most that many real frames a second)
        float hz = vx_xros_vr_on() ? vx_xros_vr_real_hz() : 0;
        int dhz = vx_xros_display_hz >= 24 ? vx_xros_display_hz : 90;
        min_ticks = hz > 0 ? (unsigned)lroundf(dhz / hz) : 0;
        if (min_ticks) vx_log("present pacing: VR real rate %.1f a second: every %u compositor frames", hz, min_ticks);
    }
    if (sync < min_ticks) sync = min_ticks;
    pthread_once(&once, pace_start);
    pthread_mutex_lock(&g_pace_lock);
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 100 * 1000000L; if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    while (g_ticks < g_last_present_tick + sync)
        if (pthread_cond_timedwait(&g_pace_cond, &g_pace_lock, &deadline)) break;   // (no ticks for 100 ms: the window is away)
    g_last_present_tick = g_ticks;
    pthread_mutex_unlock(&g_pace_lock);
}

// ---- keyboard and mouse ---------------------------------------------------------------------------------------------
// A Bluetooth keyboard and mouse (or the Mac's, through the headset's keyboard sharing) through the Game Controller
// framework: GCKeyboard's keys (HID usages) as the Mac key codes user32.c's table takes (WM_KEYDOWN, WM_CHAR for
// letters, digits and space, raw keyboard input), GCMouse's moves as raw input (WM_INPUT, what games' cameras read)
// and the pointer, its buttons as the mouse buttons at the pointer, its wheel as raw wheel input. Game controllers
// are XInput's already (src/input.m). Touches stay the left button (VXWinView).
static const uint8_t g_hid_to_mac[256] = {
    [0x04] = 0x00, [0x05] = 0x0B, [0x06] = 0x08, [0x07] = 0x02, [0x08] = 0x0E, [0x09] = 0x03, [0x0A] = 0x05, [0x0B] = 0x04,   // a-h
    [0x0C] = 0x22, [0x0D] = 0x26, [0x0E] = 0x28, [0x0F] = 0x25, [0x10] = 0x2E, [0x11] = 0x2D, [0x12] = 0x1F, [0x13] = 0x23,   // i-p
    [0x14] = 0x0C, [0x15] = 0x0F, [0x16] = 0x01, [0x17] = 0x11, [0x18] = 0x20, [0x19] = 0x09, [0x1A] = 0x0D, [0x1B] = 0x07,   // q-x
    [0x1C] = 0x10, [0x1D] = 0x06,                                                                                            // y z
    [0x1E] = 0x12, [0x1F] = 0x13, [0x20] = 0x14, [0x21] = 0x15, [0x22] = 0x17, [0x23] = 0x16, [0x24] = 0x1A, [0x25] = 0x1C,   // 1-8
    [0x26] = 0x19, [0x27] = 0x1D,                                                                                            // 9 0
    [0x28] = 0x24, [0x29] = 0x35, [0x2A] = 0x33, [0x2B] = 0x30, [0x2C] = 0x31, [0x2D] = 0x1B, [0x2E] = 0x18, [0x2F] = 0x21,   // return esc bs tab space - = [
    [0x30] = 0x1E, [0x31] = 0x2A, [0x33] = 0x29, [0x34] = 0x27, [0x35] = 0x32, [0x36] = 0x2B, [0x37] = 0x2F, [0x38] = 0x2C,   // ] \ ; ' ` , . /
    [0x39] = 0x39,                                                                                                           // caps lock
    [0x3A] = 0x7A, [0x3B] = 0x78, [0x3C] = 0x63, [0x3D] = 0x76, [0x3E] = 0x60, [0x3F] = 0x61, [0x40] = 0x62, [0x41] = 0x64,   // F1-F8
    [0x42] = 0x65, [0x43] = 0x6D, [0x44] = 0x67, [0x45] = 0x6F,                                                              // F9-F12
    [0x4A] = 0x73, [0x4B] = 0x74, [0x4C] = 0x75, [0x4D] = 0x77, [0x4E] = 0x79, [0x4F] = 0x7C, [0x50] = 0x7B, [0x51] = 0x7D,   // home pgup del end pgdn right left down
    [0x52] = 0x7E,                                                                                                           // up
    [0xE0] = 0x3B, [0xE1] = 0x38, [0xE2] = 0x3A, [0xE3] = 0x37, [0xE4] = 0x3E, [0xE5] = 0x3C, [0xE6] = 0x3D, [0xE7] = 0x36,   // ctrl shift alt cmd, right ones
};
static int g_shift, g_alt;
static unsigned g_keys_seen, g_mouse_moves;
static void key_changed(GCKeyCode usage, BOOL down) {
    if (usage == 0xE1 || usage == 0xE5) g_shift = down;
    if (usage == 0xE2 || usage == 0xE6) g_alt = down;
    if (usage > 0xFF || (!g_hid_to_mac[usage] && usage != 0x04)) return;
    int code = g_hid_to_mac[usage];
    uint16_t ch = 0;
    if (usage >= 0x04 && usage <= 0x1D) ch = (uint16_t)((g_shift ? 'A' : 'a') + (usage - 0x04));
    else if (usage >= 0x1E && usage <= 0x26 && !g_shift) ch = (uint16_t)('1' + (usage - 0x1E));
    else if (usage == 0x27 && !g_shift) ch = '0';
    else if (usage == 0x2C) ch = ' ';
    else if (usage == 0x28) ch = '\r';
    if (g_keys_seen++ < 8) vx_log("input: key (HID %#x) %s -> Mac code %#x, window %p", (unsigned)usage, down ? "down" : "up", code, g_key_window);
    if (!g_key_window) return;
    if (down && ch) vx_key_event_chars(g_key_window, code, 1, 0, g_alt, &ch, 1, 0);
    else vx_key_event(g_key_window, code, down, 0, g_alt);
}
static void mouse_button(int which, BOOL down) {
    static const struct { int vk; uint32_t down, up; WPARAM mk; } b[3] = {
        { 1, WM_LBUTTONDOWN, WM_LBUTTONUP, 0x01 }, { 2, WM_RBUTTONDOWN, WM_RBUTTONUP, 0x02 }, { 4, WM_MBUTTONDOWN, WM_MBUTTONUP, 0x10 } };
    vx_mouse_button(b[which].vk, down);
    vx_raw_mouse(0, 0, (uint16_t)((down ? 1 : 2) << (2 * which)), 0);
    if (g_key_window) vx_mouse_input(g_key_window, down ? b[which].down : b[which].up, down ? b[which].mk : 0,
                                     (int)vx_xros_pointer_x, (int)vx_xros_pointer_y);
}
static void mouse_attach(GCMouse *m) {
    GCMouseInput *in = m.mouseInput;
    vx_log("input: mouse connected (%s)", m.vendorName.UTF8String ?: "?");
    in.mouseMovedHandler = ^(GCMouseInput *mi, float dx, float dy) {
        (void)mi;
        vx_raw_mouse((int)lroundf(dx), (int)lroundf(-dy), 0, 0);       // (GCMouse's y runs up; raw input's down)
        vx_xros_pointer_x = fmin(fmax(vx_xros_pointer_x + dx, 0), vx_xros_display_w - 1);
        vx_xros_pointer_y = fmin(fmax(vx_xros_pointer_y - dy, 0), vx_xros_display_h - 1);
        vx_xros_last_input = CFAbsoluteTimeGetCurrent();
        if (g_mouse_moves++ < 3) vx_log("input: mouse move %.1f %.1f", dx, dy);
        if (g_key_window) vx_mouse_input(g_key_window, WM_MOUSEMOVE, 0, (int)vx_xros_pointer_x, (int)vx_xros_pointer_y);
    };
    in.leftButton.pressedChangedHandler = ^(GCControllerButtonInput *bi, float v, BOOL p) { (void)bi; (void)v; mouse_button(0, p); };
    in.rightButton.pressedChangedHandler = ^(GCControllerButtonInput *bi, float v, BOOL p) { (void)bi; (void)v; mouse_button(1, p); };
    in.middleButton.pressedChangedHandler = ^(GCControllerButtonInput *bi, float v, BOOL p) { (void)bi; (void)v; mouse_button(2, p); };
    in.scroll.yAxis.valueChangedHandler = ^(GCControllerAxisInput *ax, float v) {
        (void)ax;
        if (v != 0) vx_raw_mouse(0, 0, 0x400 /* RI_MOUSE_WHEEL */, (int16_t)lroundf(v * 120));
    };
}
static void keyboard_attach(GCKeyboard *k) {
    vx_log("input: keyboard connected (%s)", k.vendorName.UTF8String ?: "?");
    k.keyboardInput.keyChangedHandler = ^(GCKeyboardInput *ki, GCControllerButtonInput *b, GCKeyCode code, BOOL pressed) {
        (void)ki; (void)b; key_changed(code, pressed);
    };
}
static void input_start(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:GCKeyboardDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *n) { keyboard_attach(n.object); }];
        [nc addObserverForName:GCMouseDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *n) { mouse_attach(n.object); }];
        if (GCKeyboard.coalescedKeyboard) keyboard_attach(GCKeyboard.coalescedKeyboard);
        for (GCMouse *m in GCMouse.mice) mouse_attach(m);
        vx_log("input: keyboard %s, %lu mice, %lu controllers at start", GCKeyboard.coalescedKeyboard ? "yes" : "no",
               (unsigned long)GCMouse.mice.count, (unsigned long)GCController.controllers.count);
    });
}
