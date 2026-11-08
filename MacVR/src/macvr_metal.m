#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "macvr.h"

// ============================================================================
// Logging
// ============================================================================
static FILE *g_log_file = NULL;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void macvr_log(const char *fmt, ...) {
    pthread_mutex_lock(&g_log_lock);
    static int init_done = 0;
    if (!init_done) {
        init_done = 1;
        const char *p = getenv("VERANTYX_MACVR_LOG");
        if (!p || !*p) p = "/tmp/verantyx_macvr.log";
        g_log_file = fopen(p, "a");
    }
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[MacVR] %s\n", buf);
    if (g_log_file) {
        fprintf(g_log_file, "[%.3f] %s\n", CACurrentMediaTime(), buf);
        fflush(g_log_file);
    }
    pthread_mutex_unlock(&g_log_lock);
}

static IMP macvr_swizzle(Class cls, SEL sel, IMP newImp) {
    if (!cls) return NULL;
    Method m = class_getInstanceMethod(cls, sel);
    if (!m) return NULL;
    IMP oldImp = method_getImplementation(m);
    if (oldImp == newImp) return oldImp;
    if (!class_addMethod(cls, sel, newImp, method_getTypeEncoding(m))) {
        method_setImplementation(m, newImp);
    }
    return oldImp;
}

static IMP macvr_swizzle_class(Class cls, SEL sel, IMP newImp) {
    if (!cls) return NULL;
    Method m = class_getClassMethod(cls, sel);
    if (!m) return NULL;
    Class meta = objc_getMetaClass(class_getName(cls));
    return macvr_swizzle(meta, sel, newImp);
}

// ============================================================================
// FNV-1a 64-bit Hash & Shader / Pipeline Tagging (from vxpipe.m)
// ============================================================================
uint64_t macvr_fnv1a64(const void *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ p[i]) * 0x100000001b3ULL;
    }
    return h;
}

static const void *kHashKey = &kHashKey;
static const void *kUIVSKey = &kUIVSKey;
static const void *kUISepKey = &kUISepKey;
static const void *kUITwinKey = &kUITwinKey;
static const void *kUIRecKey = &kUIRecKey;
static const void *kCamPassKey = &kCamPassKey;
static const void *kEncVMKey = &kEncVMKey;
static const void *kEncActionKey = &kEncActionKey;

// Cyberpunk 2077 2.3.2 & 2.5.0 UI layer vertex shaders:
// 9659c847a5663c68 (97% of HUD draws: text, bars, brackets, flat/tilted widgets)
// dd2ffa0fee62e619 (remaining 3%: filled backgrounds)
static int is_ui_vertex_hash(NSString *h) {
    if (!h) return 0;
    return [h isEqualToString:@"9659c847a5663c68"] || [h isEqualToString:@"dd2ffa0fee62e619"];
}

// ============================================================================
// Shared GPU Buffer Address Tracker (for Top-Level Argument Buffer -> CBV)
// ============================================================================
typedef struct {
    uint64_t gpu_addr;
    uint64_t length;
    __unsafe_unretained id<MTLBuffer> buf;
} MacVRBufRange;

static MacVRBufRange *g_buf_ranges = NULL;
static size_t g_num_buf_ranges = 0;
static size_t g_cap_buf_ranges = 0;
static pthread_mutex_t g_buf_lock = PTHREAD_MUTEX_INITIALIZER;
static const void *kBufTrackerKey = &kBufTrackerKey;

@interface MacVRBufTracker : NSObject
@property (nonatomic, assign) uint64_t gpuAddr;
@end

@implementation MacVRBufTracker
- (void)dealloc {
    pthread_mutex_lock(&g_buf_lock);
    for (size_t i = 0; i < g_num_buf_ranges; i++) {
        if (g_buf_ranges[i].gpu_addr == _gpuAddr) {
            memmove(g_buf_ranges + i, g_buf_ranges + i + 1, (g_num_buf_ranges - i - 1) * sizeof(MacVRBufRange));
            g_num_buf_ranges--;
            break;
        }
    }
    pthread_mutex_unlock(&g_buf_lock);
}
@end

static void macvr_track_buffer(id bufObj) {
    if (!bufObj) return;
    id<MTLBuffer> b = bufObj;
    if (b.storageMode != MTLStorageModeShared || b.length < 64) return;
    uint64_t addr = b.gpuAddress;
    if (!addr) return;
    if (objc_getAssociatedObject(b, kBufTrackerKey)) return;

    MacVRBufTracker *tracker = [MacVRBufTracker new];
    tracker.gpuAddr = addr;
    objc_setAssociatedObject(b, kBufTrackerKey, tracker, OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    pthread_mutex_lock(&g_buf_lock);
    if (g_num_buf_ranges == g_cap_buf_ranges) {
        g_cap_buf_ranges = g_cap_buf_ranges ? g_cap_buf_ranges * 2 : 4096;
        g_buf_ranges = (MacVRBufRange *)realloc(g_buf_ranges, g_cap_buf_ranges * sizeof(MacVRBufRange));
    }
    size_t lo = 0, hi = g_num_buf_ranges;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_buf_ranges[mid].gpu_addr < addr) lo = mid + 1;
        else hi = mid;
    }
    memmove(g_buf_ranges + lo + 1, g_buf_ranges + lo, (g_num_buf_ranges - lo) * sizeof(MacVRBufRange));
    g_buf_ranges[lo].gpu_addr = addr;
    g_buf_ranges[lo].length = b.length;
    g_buf_ranges[lo].buf = b;
    g_num_buf_ranges++;
    pthread_mutex_unlock(&g_buf_lock);
}

void macvr_test_register_buffer(id mtlBuffer) {
    macvr_track_buffer(mtlBuffer);
}

static const uint8_t *macvr_buf_at_locked(uint64_t gpu_addr, uint64_t *out_avail, uint64_t *out_base) {
    size_t lo = 0, hi = g_num_buf_ranges;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_buf_ranges[mid].gpu_addr <= gpu_addr) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return NULL;
    MacVRBufRange *r = &g_buf_ranges[lo - 1];
    if (gpu_addr >= r->gpu_addr + r->length) return NULL;
    *out_avail = (r->gpu_addr + r->length) - gpu_addr;
    *out_base = r->gpu_addr;
    const void *contents = [r->buf contents];
    if (!contents) return NULL;
    return (const uint8_t *)contents + (gpu_addr - r->gpu_addr);
}

// ============================================================================
// Camera & Projection Extraction (from vxpipe.m lines 805-858)
// ============================================================================
static pthread_mutex_t g_cam_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_game_cam[16];
static float g_game_tan[2];
static uint64_t g_game_cam_seq = 0;
static atomic_uint g_frame_counter = 0;
static atomic_uint g_cam_frame = 0;

uint64_t macvr_game_camera(float m[16]) {
    pthread_mutex_lock(&g_cam_lock);
    uint64_t seq = g_game_cam_seq;
    if (seq && m) memcpy(m, g_game_cam, sizeof(g_game_cam));
    pthread_mutex_unlock(&g_cam_lock);
    return seq;
}

int macvr_game_tan(float *tx, float *ty) {
    pthread_mutex_lock(&g_cam_lock);
    int ok = g_game_tan[0] > 0.0f;
    if (ok) {
        if (tx) *tx = g_game_tan[0];
        if (ty) *ty = g_game_tan[1];
    }
    pthread_mutex_unlock(&g_cam_lock);
    return ok;
}

static int macvr_cam_parse(const uint8_t *tl, size_t n, float m[16], float *otx, float *oty) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *v = getenv("VERANTYX_GAME_CAM");
        enabled = !(v && strcmp(v, "0") == 0);
    }
    if (!enabled || !tl || n < 8) return 0;

    static unsigned qw = 0, tb = 24, mb = 320;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("VERANTYX_CAM_PATH");
        if (e) sscanf(e, "%u:%u:%u", &qw, &tb, &mb);
    });
    if (n < (size_t)(qw + 1) * 8) return 0;

    int ok = 0;
    if (otx) *otx = 0.0f;
    if (oty) *oty = 0.0f;

    pthread_mutex_lock(&g_buf_lock);
    uint64_t v = 0;
    memcpy(&v, tl + qw * 8, 8);
    uint64_t av = 0, base = 0;
    const uint8_t *d = macvr_buf_at_locked(v, &av, &base);
    if (d && av >= tb + 8) {
        uint64_t v2 = 0;
        memcpy(&v2, d + tb, 8);
        const uint8_t *c = macvr_buf_at_locked(v2, &av, &base);
        if (c && av >= mb + 64) {
            memcpy(m, c + mb, 64);
            ok = 1;
        }
        if (c && av >= 192 && mb == 320 && otx && oty) {
            float proj[16];
            memcpy(proj, c + 128, sizeof(proj));
            float p0 = proj[0], p5 = proj[5];
            if (p0 > 0.2f && p0 < 5.0f && p5 > 0.2f && p5 < 8.0f) {
                *otx = 1.0f / p0;
                *oty = 1.0f / p5;
            }
        }
    }
    pthread_mutex_unlock(&g_buf_lock);

    if (ok) {
        for (int r = 0; r < 3 && ok; r++) {
            float l = m[r*4]*m[r*4] + m[r*4+1]*m[r*4+1] + m[r*4+2]*m[r*4+2];
            if (!(fabsf(l - 1.0f) < 0.02f)) ok = 0;
        }
        if (ok && fabsf(m[0]*m[4] + m[1]*m[5] + m[2]*m[6]) > 0.02f) ok = 0;
        if (ok && fabsf(m[4]*m[8] + m[5]*m[9] + m[6]*m[10]) > 0.02f) ok = 0;
        if (ok && fabsf(m[15] - 1.0f) > 1e-3f) ok = 0;
    }
    return ok;
}

static void macvr_cam_commit(const float m[16], float tx, float ty) {
    pthread_mutex_lock(&g_cam_lock);
    memcpy(g_game_cam, m, 64);
    g_game_cam_seq++;
    uint64_t seq = g_game_cam_seq;
    float prev_tx = g_game_tan[0];
    if (tx > 0.0f) {
        g_game_tan[0] = tx;
        g_game_tan[1] = ty;
    }
    pthread_mutex_unlock(&g_cam_lock);

    if (seq == 1) {
        macvr_log("Camera matrix locked from G-buffer constants (forward=%.3f %.3f %.3f)", m[8], m[9], m[10]);
    }
    if (tx > 0.0f && fabsf(prev_tx - tx) > 0.01f) {
        macvr_log("Projection tangents updated: hfov=%.1f deg, vfov=%.1f deg",
                  2.0f * atanf(tx) * 57.29578f, 2.0f * atanf(ty) * 57.29578f);
    }
}

int macvr_test_parse_camera_from_tlab(const void *tlab, size_t len, float m[16], float *tx, float *ty) {
    float ltx = 0.0f, lty = 0.0f;
    int ok = macvr_cam_parse((const uint8_t *)tlab, len, m, &ltx, &lty);
    if (ok) {
        macvr_cam_commit(m, ltx, lty);
        if (tx) *tx = ltx;
        if (ty) *ty = lty;
    }
    return ok;
}

// ============================================================================
// Foreground Viewmodel State & World-Anchored UI State
// ============================================================================
@interface MacVREncState : NSObject {
@public
    int vp_fg;
    int had_fg;
    int draw_idx;
    int cam_ok;
    float cam_m[16];
    float cam_tx;
    float cam_ty;
    id<MTLBuffer> original_tlab;
    NSUInteger offset_tlab;
    NSData *bytes_tlab;
}
@end
@implementation MacVREncState
@end

static atomic_uint g_vm_last_frame = 0;
int macvr_viewmodel_active(void) {
    unsigned last = atomic_load(&g_vm_last_frame);
    unsigned now = atomic_load(&g_frame_counter);
    return last && (now - last <= 2);
}

static void mat4_mul_row(float out[16], const float a[16], const float b[16]) {
    float t[16];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            t[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] + a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
        }
    }
    memcpy(out, t, sizeof(t));
}

// ============================================================================
// HUD Separation & Menu Detection (from vxmtl.m lines 455-645)
// ============================================================================
static id<MTLTexture> g_hud_tex = nil;
static atomic_int g_hud_Suppress = 1;
static atomic_uint g_hud_frame = 0;
static atomic_uint g_hud_taken = 0;
static atomic_int g_ui_menu = 0;
static unsigned g_ui_frame = 0, g_ui_Total = 0, g_ui_11 = 0, g_ui_39 = 0;

void macvr_hud_suppress_set(int on) {
    atomic_store(&g_hud_Suppress, on);
}

int macvr_ui_menu(void) {
    return atomic_load(&g_ui_menu);
}

void *macvr_hud_take(void) {
    unsigned f = atomic_load(&g_hud_frame);
    if (!f || atomic_exchange(&g_hud_taken, f) == f) return NULL;
    return (__bridge void *)g_hud_tex;
}

@interface MacVREncAction : NSObject
@property (nonatomic, strong) id<MTLCommandBuffer> cb;
@property (nonatomic, strong) id<MTLTexture> color;
@property (nonatomic, strong) id<MTLTexture> depth;
@property (nonatomic, assign) int loadAction;
@property (nonatomic, assign) int ui;
@property (nonatomic, assign) int ui_hud;
@property (nonatomic, assign) NSUInteger slice;
@property (nonatomic, assign) unsigned draws;
@end
@implementation MacVREncAction
@end

static macvr_pass_fn g_pass_hook = NULL;
static macvr_present_fn g_present_hook = NULL;

void macvr_set_pass_hook(macvr_pass_fn fn) { g_pass_hook = fn; }
void macvr_set_present_hook(macvr_present_fn fn) { g_present_hook = fn; }

// ============================================================================
// Method Swizzle Implementations
// ============================================================================
typedef id (*next_drawable_imp)(id, SEL);
static next_drawable_imp o_next_drawable = NULL;

typedef id (*render_enc_imp)(id, SEL, MTLRenderPassDescriptor *);
static render_enc_imp o_render_enc = NULL;

typedef void (*present_imp)(id, SEL, id);
typedef void (*present_at_imp)(id, SEL, id, CFTimeInterval);
static present_imp o_present = NULL;
static present_at_imp o_present_at = NULL, o_present_after = NULL;

typedef void (*end_enc_imp)(id, SEL);
static end_enc_imp o_end_enc = NULL;

typedef void (*set_rps_imp)(id, SEL, id);
static set_rps_imp o_set_rps = NULL;

typedef void (*set_vp_imp)(id, SEL, MTLViewport);
typedef void (*set_vps_imp)(id, SEL, const MTLViewport *, NSUInteger);
static set_vp_imp o_set_vp = NULL;
static set_vps_imp o_set_vps = NULL;

typedef void (*set_buf_imp)(id, SEL, id, NSUInteger, NSUInteger);
typedef void (*set_off_imp)(id, SEL, NSUInteger, NSUInteger);
typedef void (*set_bytes_imp)(id, SEL, const void *, NSUInteger, NSUInteger);
typedef void (*set_buf_stride_imp)(id, SEL, id, NSUInteger, NSUInteger, NSUInteger);
typedef void (*set_off_stride_imp)(id, SEL, NSUInteger, NSUInteger, NSUInteger);
typedef void (*set_bytes_stride_imp)(id, SEL, const void *, NSUInteger, NSUInteger, NSUInteger);
static set_buf_imp o_svb = NULL;
static set_off_imp o_svo = NULL;
static set_bytes_imp o_svy = NULL;
static set_buf_stride_imp o_svbs = NULL;
static set_off_stride_imp o_svos = NULL;
static set_bytes_stride_imp o_svys = NULL;

typedef void (*draw_prim_imp)(id, SEL, MTLPrimitiveType, NSUInteger, NSUInteger);
typedef void (*draw_prim_inst_imp)(id, SEL, MTLPrimitiveType, NSUInteger, NSUInteger, NSUInteger);
typedef void (*draw_prim_base_imp)(id, SEL, MTLPrimitiveType, NSUInteger, NSUInteger, NSUInteger, NSUInteger);
typedef void (*draw_idx_imp)(id, SEL, MTLPrimitiveType, NSUInteger, MTLIndexType, id, NSUInteger);
typedef void (*draw_idx_inst_imp)(id, SEL, MTLPrimitiveType, NSUInteger, MTLIndexType, id, NSUInteger, NSUInteger);
typedef void (*draw_idx_base_imp)(id, SEL, MTLPrimitiveType, NSUInteger, MTLIndexType, id, NSUInteger, NSUInteger, NSInteger, NSUInteger);
static draw_prim_imp o_dp = NULL;
static draw_prim_inst_imp o_dpi = NULL;
static draw_prim_base_imp o_dpib = NULL;
static draw_idx_imp o_dx = NULL;
static draw_idx_inst_imp o_dxi = NULL;
static draw_idx_base_imp o_dxib = NULL;

static void macvr_bind_viewmodel(id<MTLRenderCommandEncoder> enc, MacVREncState *vm) {
    if (!vm || !vm->vp_fg || !o_svb) return;
    const uint8_t *tl = NULL;
    size_t n = 0;
    if (vm->bytes_tlab) {
        tl = (const uint8_t *)vm->bytes_tlab.bytes;
        n = vm->bytes_tlab.length;
    } else if (vm->original_tlab && vm->original_tlab.storageMode == MTLStorageModeShared && vm->offset_tlab < vm->original_tlab.length) {
        tl = (const uint8_t *)vm->original_tlab.contents + vm->offset_tlab;
        n = vm->original_tlab.length - vm->offset_tlab;
    }
    if (!tl || n < 8) return;

    float g_delta[3] = {0}, g_rot[3] = {0}, o_delta[3] = {0}, o_rot[3] = {0};
    int off_ok = 0;
    int side = macvr_viewmodel_poses(g_delta, g_rot, o_delta, o_rot, &off_ok);
    if (!side) return;
    vm->draw_idx++;

    id<MTLBuffer> shadow = [[enc device] newBufferWithLength:16384 options:MTLResourceStorageModeShared];
    if (!shadow) return;
    uint8_t *dst = (uint8_t *)shadow.contents;
    uint8_t *table = dst + 4096;
    uint8_t *constants = dst + 8192;
    memset(dst, 0, 16384);
    memcpy(dst, tl, n < 4096 ? n : 4096);

    uint64_t addr = 0, av = 0, base = 0;
    memcpy(&addr, tl, 8);
    pthread_mutex_lock(&g_buf_lock);
    const uint8_t *d = macvr_buf_at_locked(addr, &av, &base);
    int ok = (d && av >= 32);
    size_t tbl_len = (ok && av < 4096) ? (size_t)av : 4096;
    if (ok) {
        memcpy(table, d, tbl_len);
        memcpy(&addr, d + 24, 8);
        const uint8_t *c = macvr_buf_at_locked(addr, &av, &base);
        ok = (c && av >= 512);
        if (ok) memcpy(constants, c, av < 2048 ? (size_t)av : 2048);
    }
    pthread_mutex_unlock(&g_buf_lock);
    if (!ok) return;

    float p[16], v[16];
    memcpy(p, constants + 128, 64);
    memcpy(v, constants + 320, 64);
    for (int r = 0; r < 3; r++) {
        float l = v[r*4+0]*v[r*4+0] + v[r*4+1]*v[r*4+1] + v[r*4+2]*v[r*4+2];
        if (!(fabsf(l - 1.0f) < 0.02f)) return;
    }
    if (fabsf(v[15] - 1.0f) > 0.001f || !isfinite(p[0]) || fabsf(p[0]) < 0.2f) return;

    float yaw = fmaxf(-0.75f, fminf(0.75f, g_rot[0]));
    float pitch = fmaxf(-0.55f, fminf(0.55f, -g_rot[1]));
    float roll = fmaxf(-0.55f, fminf(0.55f, -g_rot[2]));
    float sh_delta[3] = {
        fmaxf(-0.24f, fminf(0.24f, g_delta[0])),
        fmaxf(-0.20f, fminf(0.20f, g_delta[1])),
        fmaxf(-0.16f, fminf(0.18f, g_delta[2]))
    };
    float cy = cosf(yaw), sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cr = cosf(roll), sr = sinf(roll);
    float ry[16] = {cy,0,sy,0, 0,1,0,0, -sy,0,cy,0, 0,0,0,1};
    float rx[16] = {1,0,0,0, 0,cp,-sp,0, 0,sp,cp,0, 0,0,0,1};
    float rz[16] = {cr,-sr,0,0, sr,cr,0,0, 0,0,1,0, 0,0,0,1};
    float m[16], tmp[16];
    float pivot[3] = {side < 0 ? -0.14f : 0.14f, -0.16f, 0.28f};
    mat4_mul_row(tmp, rx, ry);
    mat4_mul_row(m, rz, tmp);
    for (int r = 0; r < 3; r++) {
        m[r*4+3] = pivot[r] + sh_delta[r] - (m[r*4+0]*pivot[0] + m[r*4+1]*pivot[1] + m[r*4+2]*pivot[2]);
    }
    for (int i = 0; i < 2; i++) {
        memcpy(v, constants + 320 + i*64, 64);
        mat4_mul_row(v, m, v);
        memcpy(constants + 320 + i*64, v, 64);
        mat4_mul_row(tmp, p, v);
        memcpy(constants + (i ? 448 : 0), tmp, 64);
        memcpy(constants + (i ? 256 : 192), tmp, 64);
    }
    addr = shadow.gpuAddress + 4096;
    memcpy(dst, &addr, 8);
    addr = shadow.gpuAddress + 8192;
    memcpy(table + 24, &addr, 8);
    [enc useResource:shadow usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
    o_svb(enc, @selector(setVertexBuffer:offset:atIndex:), shadow, 0, 2);
    atomic_store(&g_vm_last_frame, atomic_load(&g_frame_counter));
}

static void macvr_note_viewport(id enc, MTLViewport v) {
    MacVREncState *vm = objc_getAssociatedObject(enc, kEncVMKey);
    if (!vm) return;
    int fg = fabs(v.zfar - v.znear) <= 0.25 && ((v.znear >= 0.8 && v.znear <= 1.0) || (v.zfar >= 0.8 && v.zfar <= 1.0 && v.znear > 0.01));
    if (fg) vm->had_fg = 1;
    if (fg != vm->vp_fg) {
        vm->vp_fg = fg;
        if (fg) vm->draw_idx = 0;
        if (vm->original_tlab && o_svb) {
            o_svb(enc, @selector(setVertexBuffer:offset:atIndex:), vm->original_tlab, vm->offset_tlab, 2);
        } else if (vm->bytes_tlab && o_svy) {
            o_svy(enc, @selector(setVertexBytes:length:atIndex:), vm->bytes_tlab.bytes, vm->bytes_tlab.length, 2);
        }
        macvr_bind_viewmodel(enc, vm);
    }
}

static void macvr_note_tlab(id enc, id buf, NSUInteger off, NSUInteger idx, const void *bytes, NSUInteger len, int offonly) {
    if (idx != 2) return;
    if (objc_getAssociatedObject(enc, kCamPassKey)) {
        objc_setAssociatedObject(enc, kCamPassKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        MacVREncState *cvm = objc_getAssociatedObject(enc, kEncVMKey);
        float m[16], tx = 0.0f, ty = 0.0f;
        int got = 0;
        if (bytes) {
            got = macvr_cam_parse((const uint8_t *)bytes, len, m, &tx, &ty);
        } else if (buf && !offonly) {
            id<MTLBuffer> b = buf;
            if (b.storageMode == MTLStorageModeShared && off + 8 <= b.length) {
                got = macvr_cam_parse((const uint8_t *)b.contents + off, b.length - off, m, &tx, &ty);
            }
        }
        if (got) {
            if (cvm) {
                memcpy(cvm->cam_m, m, 64);
                cvm->cam_tx = tx;
                cvm->cam_ty = ty;
                cvm->cam_ok = 1;
            } else {
                unsigned fr = atomic_load(&g_frame_counter);
                if (atomic_exchange(&g_cam_frame, fr) != fr) {
                    macvr_cam_commit(m, tx, ty);
                }
            }
        }
    }
    MacVREncState *vm = objc_getAssociatedObject(enc, kEncVMKey);
    if (vm) {
        if (bytes) {
            vm->bytes_tlab = [NSData dataWithBytes:bytes length:len];
            vm->original_tlab = nil;
        } else if (offonly) {
            vm->offset_tlab = off;
        } else {
            vm->original_tlab = buf;
            vm->offset_tlab = off;
            vm->bytes_tlab = nil;
        }
        macvr_bind_viewmodel(enc, vm);
    }
}

static void h_set_vp(id self, SEL s, MTLViewport v) {
    o_set_vp(self, s, v);
    macvr_note_viewport(self, v);
}

static void h_set_vps(id self, SEL s, const MTLViewport *v, NSUInteger n) {
    o_set_vps(self, s, v, n);
    if (v && n) macvr_note_viewport(self, v[0]);
}

static void h_svb(id self, SEL s, id b, NSUInteger o, NSUInteger i) {
    o_svb(self, s, b, o, i);
    macvr_note_tlab(self, b, o, i, NULL, 0, 0);
}

static void h_svo(id self, SEL s, NSUInteger o, NSUInteger i) {
    MacVREncState *vm = (i == 2) ? objc_getAssociatedObject(self, kEncVMKey) : nil;
    if (vm && vm->original_tlab) {
        o_svb(self, @selector(setVertexBuffer:offset:atIndex:), vm->original_tlab, o, i);
    } else {
        o_svo(self, s, o, i);
    }
    macvr_note_tlab(self, nil, o, i, NULL, 0, 1);
}

static void h_svy(id self, SEL s, const void *b, NSUInteger l, NSUInteger i) {
    o_svy(self, s, b, l, i);
    macvr_note_tlab(self, nil, 0, i, b, l, 0);
}

static void h_svbs(id self, SEL s, id b, NSUInteger o, NSUInteger st, NSUInteger i) {
    o_svbs(self, s, b, o, st, i);
    macvr_note_tlab(self, b, o, i, NULL, 0, 0);
}

static void h_svos(id self, SEL s, NSUInteger o, NSUInteger st, NSUInteger i) {
    MacVREncState *vm = (i == 2) ? objc_getAssociatedObject(self, kEncVMKey) : nil;
    if (vm && vm->original_tlab && o_svbs) {
        o_svbs(self, @selector(setVertexBuffer:offset:attributeStride:atIndex:), vm->original_tlab, o, st, i);
    } else {
        o_svos(self, s, o, st, i);
    }
    macvr_note_tlab(self, nil, o, i, NULL, 0, 1);
}

static void h_svys(id self, SEL s, const void *b, NSUInteger l, NSUInteger st, NSUInteger i) {
    o_svys(self, s, b, l, st, i);
    macvr_note_tlab(self, nil, 0, i, b, l, 0);
}

static void macvr_count_draw(id enc) {
    MacVREncAction *act = objc_getAssociatedObject(enc, kEncActionKey);
    if (act) act.draws++;
}

static void h_dp(id self, SEL s, MTLPrimitiveType t, NSUInteger v, NSUInteger c) { macvr_count_draw(self); o_dp(self, s, t, v, c); }
static void h_dpi(id self, SEL s, MTLPrimitiveType t, NSUInteger v, NSUInteger c, NSUInteger i) { macvr_count_draw(self); o_dpi(self, s, t, v, c, i); }
static void h_dpib(id self, SEL s, MTLPrimitiveType t, NSUInteger v, NSUInteger c, NSUInteger i, NSUInteger bi) { macvr_count_draw(self); o_dpib(self, s, t, v, c, i, bi); }
static void h_dx(id self, SEL s, MTLPrimitiveType t, NSUInteger c, MTLIndexType it, id ib, NSUInteger io) { macvr_count_draw(self); o_dx(self, s, t, c, it, ib, io); }
static void h_dxi(id self, SEL s, MTLPrimitiveType t, NSUInteger c, MTLIndexType it, id ib, NSUInteger io, NSUInteger i) { macvr_count_draw(self); o_dxi(self, s, t, c, it, ib, io, i); }
static void h_dxib(id self, SEL s, MTLPrimitiveType t, NSUInteger c, MTLIndexType it, id ib, NSUInteger io, NSUInteger i, NSInteger bv, NSUInteger bi) { macvr_count_draw(self); o_dxib(self, s, t, c, it, ib, io, i, bv, bi); }

static void h_set_rps(id self, SEL s, id rps) {
    o_set_rps(self, s, rps);
    if (rps && objc_getAssociatedObject(rps, kUIVSKey)) {
        MacVREncAction *act = objc_getAssociatedObject(self, kEncActionKey);
        if (act && act.ui == 1) act.ui = 2;
    }
}

static void macvr_ui_end(MacVREncAction *act) {
    id<MTLCommandBuffer> cb = act.cb;
    id<MTLTexture> src = act.color;
    if (!cb || !src) return;
    unsigned f = atomic_load(&g_frame_counter);
    if (f != g_ui_frame) {
        if (g_ui_Total) {
            int menu = (g_ui_11 >= 4 && g_ui_39 == 0) || (g_ui_39 > 1600) || (g_ui_Total > 2500);
            atomic_store(&g_ui_menu, menu);
        } else {
            atomic_store(&g_ui_menu, 0);
        }
        g_ui_frame = f;
        g_ui_Total = g_ui_11 = g_ui_39 = 0;
    }
    g_ui_Total += act.draws;
    if (src.pixelFormat == MTLPixelFormatRG11B10Float) g_ui_11 += act.draws;
    else if (src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB) g_ui_39 += act.draws;

    if (!act.ui_hud) return;
    static int hud_enabled = -1;
    if (hud_enabled < 0) {
        const char *v = getenv("VERANTYX_HUD_LAYER");
        hud_enabled = !(v && strcmp(v, "0") == 0);
    }
    if (!hud_enabled) return;

    NSUInteger w = src.width, h = src.height;
    if (!g_hud_tex || g_hud_tex.width != w || g_hud_tex.height != h || g_hud_tex.pixelFormat != src.pixelFormat) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        g_hud_tex = [cb.device newTextureWithDescriptor:td];
        macvr_log("HUD offscreen layer captured: %lux%lu fmt=%lu", (unsigned long)w, (unsigned long)h, (unsigned long)src.pixelFormat);
    }
    if (!g_hud_tex) return;

    int first_in_frame = (atomic_exchange(&g_hud_frame, f) != f);
    if (first_in_frame) {
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:src sourceSlice:act.slice sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(w, h, 1) toTexture:g_hud_tex destinationSlice:0 destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
        [bl endEncoding];
    }
    if (atomic_load(&g_hud_Suppress) && !(src.usage & MTLTextureUsageShaderWrite)) {
        MTLRenderPassDescriptor *cl = [MTLRenderPassDescriptor renderPassDescriptor];
        cl.colorAttachments[0].texture = src;
        cl.colorAttachments[0].slice = act.slice;
        cl.colorAttachments[0].loadAction = MTLLoadActionClear;
        cl.colorAttachments[0].storeAction = MTLStoreActionStore;
        cl.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLRenderCommandEncoder> ce = o_render_enc ? o_render_enc(cb, @selector(renderCommandEncoderWithDescriptor:), cl) : [cb renderCommandEncoderWithDescriptor:cl];
        if (o_end_enc) o_end_enc(ce, @selector(endEncoding));
        else [ce endEncoding];
    }
}

static void h_end_enc(id self, SEL s) {
    o_end_enc(self, s);
    MacVREncState *vm = objc_getAssociatedObject(self, kEncVMKey);
    if (vm) {
        if (vm->cam_ok && !vm->had_fg) {
            unsigned fr = atomic_load(&g_frame_counter);
            if (atomic_exchange(&g_cam_frame, fr) != fr) {
                macvr_cam_commit(vm->cam_m, vm->cam_tx, vm->cam_ty);
            }
        } else if (vm->cam_ok && atomic_load(&g_frame_counter) - atomic_load(&g_cam_frame) > 3) {
            atomic_store(&g_cam_frame, atomic_load(&g_frame_counter));
            macvr_cam_commit(vm->cam_m, 0.0f, 0.0f);
        }
        objc_setAssociatedObject(self, kEncVMKey, nil, OBJC_ASSOCIATION_ASSIGN);
    }
    objc_setAssociatedObject(self, kCamPassKey, nil, OBJC_ASSOCIATION_ASSIGN);

    MacVREncAction *act = objc_getAssociatedObject(self, kEncActionKey);
    if (act) {
        objc_setAssociatedObject(self, kEncActionKey, nil, OBJC_ASSOCIATION_ASSIGN);
        if (act.ui == 2) macvr_ui_end(act);
        if (g_pass_hook && (act.color || act.depth)) {
            g_pass_hook(act.cb, act.color, act.depth, act.loadAction);
        }
    }
}

// ============================================================================
// Device Pipeline & Buffer Hooks
// ============================================================================
typedef id (*new_lib_data_imp)(id, SEL, dispatch_data_t, NSError **) __attribute__((ns_returns_retained));
typedef id (*new_fn_name_imp)(id, SEL, NSString *) __attribute__((ns_returns_retained));
typedef id (*new_fn_const_imp)(id, SEL, NSString *, MTLFunctionConstantValues *, NSError **) __attribute__((ns_returns_retained));
static new_lib_data_imp o_new_lib_data = NULL;
static new_fn_name_imp o_new_fn_name = NULL;
static new_fn_const_imp o_new_fn_const = NULL;

__attribute__((ns_returns_retained)) static id h_new_fn_name(id self, SEL s, NSString *name) {
    id fn = o_new_fn_name(self, s, name);
    NSString *h = objc_getAssociatedObject(self, kHashKey);
    if (fn && h) objc_setAssociatedObject(fn, kHashKey, h, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return fn;
}

__attribute__((ns_returns_retained)) static id h_new_fn_const(id self, SEL s, NSString *name, MTLFunctionConstantValues *cv, NSError **err) {
    id fn = o_new_fn_const(self, s, name, cv, err);
    NSString *h = objc_getAssociatedObject(self, kHashKey);
    if (fn && h) objc_setAssociatedObject(fn, kHashKey, h, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return fn;
}

__attribute__((ns_returns_retained)) static id h_new_lib_data(id self, SEL s, dispatch_data_t data, NSError **err) {
    id lib = o_new_lib_data(self, s, data, err);
    if (lib && data) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            Class lc = object_getClass(lib);
            o_new_fn_name = (new_fn_name_imp)macvr_swizzle(lc, @selector(newFunctionWithName:), (IMP)h_new_fn_name);
            o_new_fn_const = (new_fn_const_imp)macvr_swizzle(lc, @selector(newFunctionWithName:constantValues:error:), (IMP)h_new_fn_const);
        });
        __block uint64_t h = 0xcbf29ce484222325ULL;
        dispatch_data_apply(data, ^bool(dispatch_data_t region, size_t offset, const void *buffer, size_t size) {
            const uint8_t *p = (const uint8_t *)buffer;
            for (size_t i = 0; i < size; i++) h = (h ^ p[i]) * 0x100000001b3ULL;
            return true;
        });
        NSString *hs = [NSString stringWithFormat:@"%016llx", (unsigned long long)h];
        objc_setAssociatedObject(lib, kHashKey, hs, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return lib;
}

typedef id (*new_rp_imp)(id, SEL, MTLRenderPipelineDescriptor *, NSError **) __attribute__((ns_returns_retained));
typedef id (*new_rp_opt_imp)(id, SEL, MTLRenderPipelineDescriptor *, MTLPipelineOption, MTLAutoreleasedRenderPipelineReflection *, NSError **) __attribute__((ns_returns_retained));
typedef void (*new_rp_cb_imp)(id, SEL, MTLRenderPipelineDescriptor *, MTLNewRenderPipelineStateCompletionHandler);
typedef void (*new_rp_opt_cb_imp)(id, SEL, MTLRenderPipelineDescriptor *, MTLPipelineOption, MTLNewRenderPipelineStateWithReflectionCompletionHandler);
static new_rp_imp o_new_rp = NULL;
static new_rp_opt_imp o_new_rp_opt = NULL;
static new_rp_cb_imp o_new_rp_cb = NULL;
static new_rp_opt_cb_imp o_new_rp_opt_cb = NULL;

static void macvr_tag_render_pipeline(id rps, MTLRenderPipelineDescriptor *desc) {
    if (!rps || !desc) return;
    id<MTLFunction> vf = desc.vertexFunction;
    NSString *vh = vf ? objc_getAssociatedObject(vf, kHashKey) : nil;
    if (is_ui_vertex_hash(vh)) {
        objc_setAssociatedObject(rps, kUIVSKey, @1, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
}

__attribute__((ns_returns_retained)) static id h_new_rp(id self, SEL s, MTLRenderPipelineDescriptor *d, NSError **e) {
    id p = o_new_rp(self, s, d, e);
    macvr_tag_render_pipeline(p, d);
    return p;
}

__attribute__((ns_returns_retained)) static id h_new_rp_opt(id self, SEL s, MTLRenderPipelineDescriptor *d, MTLPipelineOption opt, MTLAutoreleasedRenderPipelineReflection *r, NSError **e) {
    id p = o_new_rp_opt(self, s, d, opt, r, e);
    macvr_tag_render_pipeline(p, d);
    return p;
}

static void h_new_rp_cb(id self, SEL s, MTLRenderPipelineDescriptor *d0, MTLNewRenderPipelineStateCompletionHandler cb) {
    MTLRenderPipelineDescriptor *d = [d0 copy];
    o_new_rp_cb(self, s, d0, ^(id<MTLRenderPipelineState> p, NSError *e) {
        macvr_tag_render_pipeline(p, d);
        if (cb) cb(p, e);
    });
}

static void h_new_rp_opt_cb(id self, SEL s, MTLRenderPipelineDescriptor *d0, MTLPipelineOption opt, MTLNewRenderPipelineStateWithReflectionCompletionHandler cb) {
    MTLRenderPipelineDescriptor *d = [d0 copy];
    o_new_rp_opt_cb(self, s, d0, opt, ^(id<MTLRenderPipelineState> p, MTLRenderPipelineReflection *r, NSError *e) {
        macvr_tag_render_pipeline(p, d);
        if (cb) cb(p, r, e);
    });
}

typedef id (*new_buf_len_imp)(id, SEL, NSUInteger, MTLResourceOptions) __attribute__((ns_returns_retained));
typedef id (*new_buf_bytes_imp)(id, SEL, const void *, NSUInteger, MTLResourceOptions) __attribute__((ns_returns_retained));
typedef id (*new_buf_no_copy_imp)(id, SEL, void *, NSUInteger, MTLResourceOptions, void (^)(void *, NSUInteger)) __attribute__((ns_returns_retained));
typedef id (*new_heap_imp)(id, SEL, MTLHeapDescriptor *) __attribute__((ns_returns_retained));
typedef id (*heap_buf_len_imp)(id, SEL, NSUInteger, MTLResourceOptions) __attribute__((ns_returns_retained));
typedef id (*heap_buf_off_imp)(id, SEL, NSUInteger, MTLResourceOptions, NSUInteger) __attribute__((ns_returns_retained));
static new_buf_len_imp o_buf_len = NULL;
static new_buf_bytes_imp o_buf_bytes = NULL;
static new_buf_no_copy_imp o_buf_nocopy = NULL;
static new_heap_imp o_new_heap = NULL;
static heap_buf_len_imp o_heap_buf_len = NULL;
static heap_buf_off_imp o_heap_buf_off = NULL;

__attribute__((ns_returns_retained)) static id h_buf_len(id self, SEL s, NSUInteger l, MTLResourceOptions o) {
    id b = o_buf_len(self, s, l, o);
    macvr_track_buffer(b);
    return b;
}

__attribute__((ns_returns_retained)) static id h_buf_bytes(id self, SEL s, const void *p, NSUInteger l, MTLResourceOptions o) {
    id b = o_buf_bytes(self, s, p, l, o);
    macvr_track_buffer(b);
    return b;
}

__attribute__((ns_returns_retained)) static id h_buf_nocopy(id self, SEL s, void *p, NSUInteger l, MTLResourceOptions o, void (^d)(void *, NSUInteger)) {
    id b = o_buf_nocopy(self, s, p, l, o, d);
    macvr_track_buffer(b);
    return b;
}

__attribute__((ns_returns_retained)) static id h_heap_buf_len(id self, SEL s, NSUInteger l, MTLResourceOptions o) {
    id b = o_heap_buf_len(self, s, l, o);
    macvr_track_buffer(b);
    return b;
}

__attribute__((ns_returns_retained)) static id h_heap_buf_off(id self, SEL s, NSUInteger l, MTLResourceOptions o, NSUInteger off) {
    id b = o_heap_buf_off(self, s, l, o, off);
    macvr_track_buffer(b);
    return b;
}

__attribute__((ns_returns_retained)) static id h_new_heap(id self, SEL s, MTLHeapDescriptor *d) {
    id h = o_new_heap(self, s, d);
    if (h) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            Class hc = object_getClass(h);
            o_heap_buf_len = (heap_buf_len_imp)macvr_swizzle(hc, @selector(newBufferWithLength:options:), (IMP)h_heap_buf_len);
            o_heap_buf_off = (heap_buf_off_imp)macvr_swizzle(hc, @selector(newBufferWithLength:options:offset:), (IMP)h_heap_buf_off);
        });
    }
    return h;
}

static void macvr_hook_device(id<MTLDevice> dev) {
    if (!dev) return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class dc = object_getClass(dev);
        o_buf_len = (new_buf_len_imp)macvr_swizzle(dc, @selector(newBufferWithLength:options:), (IMP)h_buf_len);
        o_buf_bytes = (new_buf_bytes_imp)macvr_swizzle(dc, @selector(newBufferWithBytes:length:options:), (IMP)h_buf_bytes);
        o_buf_nocopy = (new_buf_no_copy_imp)macvr_swizzle(dc, @selector(newBufferWithBytesNoCopy:length:options:deallocator:), (IMP)h_buf_nocopy);
        o_new_heap = (new_heap_imp)macvr_swizzle(dc, @selector(newHeapWithDescriptor:), (IMP)h_new_heap);
        o_new_lib_data = (new_lib_data_imp)macvr_swizzle(dc, @selector(newLibraryWithData:error:), (IMP)h_new_lib_data);
        o_new_rp = (new_rp_imp)macvr_swizzle(dc, @selector(newRenderPipelineStateWithDescriptor:error:), (IMP)h_new_rp);
        o_new_rp_opt = (new_rp_opt_imp)macvr_swizzle(dc, @selector(newRenderPipelineStateWithDescriptor:options:reflection:error:), (IMP)h_new_rp_opt);
        o_new_rp_cb = (new_rp_cb_imp)macvr_swizzle(dc, @selector(newRenderPipelineStateWithDescriptor:completionHandler:), (IMP)h_new_rp_cb);
        o_new_rp_opt_cb = (new_rp_opt_cb_imp)macvr_swizzle(dc, @selector(newRenderPipelineStateWithDescriptor:options:completionHandler:), (IMP)h_new_rp_opt_cb);
        macvr_log("Hooked MTLDevice class: %s (%s)", class_getName(dc), dev.name.UTF8String);
    });
}

// ============================================================================
// Command Buffer & CAMetalLayer Hooks
// ============================================================================
static id h_render_enc(id self, SEL s, MTLRenderPassDescriptor *desc) {
    id enc = o_render_enc(self, s, desc);
    if (!enc || !desc) return enc;

    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class rc = object_getClass(enc);
        o_end_enc = (end_enc_imp)macvr_swizzle(rc, @selector(endEncoding), (IMP)h_end_enc);
        o_set_rps = (set_rps_imp)macvr_swizzle(rc, @selector(setRenderPipelineState:), (IMP)h_set_rps);
        o_set_vp = (set_vp_imp)macvr_swizzle(rc, @selector(setViewport:), (IMP)h_set_vp);
        o_set_vps = (set_vps_imp)macvr_swizzle(rc, @selector(setViewports:count:), (IMP)h_set_vps);
        o_svb = (set_buf_imp)macvr_swizzle(rc, @selector(setVertexBuffer:offset:atIndex:), (IMP)h_svb);
        o_svo = (set_off_imp)macvr_swizzle(rc, @selector(setVertexBufferOffset:atIndex:), (IMP)h_svo);
        o_svy = (set_bytes_imp)macvr_swizzle(rc, @selector(setVertexBytes:length:atIndex:), (IMP)h_svy);
        o_svbs = (set_buf_stride_imp)macvr_swizzle(rc, @selector(setVertexBuffer:offset:attributeStride:atIndex:), (IMP)h_svbs);
        o_svos = (set_off_stride_imp)macvr_swizzle(rc, @selector(setVertexBufferOffset:attributeStride:atIndex:), (IMP)h_svos);
        o_svys = (set_bytes_stride_imp)macvr_swizzle(rc, @selector(setVertexBytes:length:attributeStride:atIndex:), (IMP)h_svys);
        o_dp = (draw_prim_imp)macvr_swizzle(rc, @selector(drawPrimitives:vertexStart:vertexCount:), (IMP)h_dp);
        o_dpi = (draw_prim_inst_imp)macvr_swizzle(rc, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:), (IMP)h_dpi);
        o_dpib = (draw_prim_base_imp)macvr_swizzle(rc, @selector(drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:), (IMP)h_dpib);
        o_dx = (draw_idx_imp)macvr_swizzle(rc, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:), (IMP)h_dx);
        o_dxi = (draw_idx_inst_imp)macvr_swizzle(rc, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:), (IMP)h_dxi);
        o_dxib = (draw_idx_base_imp)macvr_swizzle(rc, @selector(drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:), (IMP)h_dxib);
        macvr_log("Hooked MTLRenderCommandEncoder class: %s", class_getName(rc));
    });

    // Note G-buffer pass (depth + >= 3 color attachments) for camera extraction
    if (desc.depthAttachment.texture &&
        desc.colorAttachments[0].texture &&
        desc.colorAttachments[1].texture &&
        desc.colorAttachments[2].texture) {
        if (atomic_load(&g_cam_frame) != atomic_load(&g_frame_counter)) {
            objc_setAssociatedObject(enc, kCamPassKey, @((int)desc.depthAttachment.loadAction + 1), OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
    }
    if (desc.depthAttachment.texture && desc.depthAttachment.texture.width >= 1024) {
        objc_setAssociatedObject(enc, kEncVMKey, [MacVREncState new], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }

    id<MTLTexture> c0 = desc.colorAttachments[0].texture;
    id<MTLTexture> dt = desc.depthAttachment.texture;
    int ui_cand = (c0 && !dt && !desc.colorAttachments[1].texture && desc.colorAttachments[0].level == 0 && c0.width >= 640 && c0.height >= 360);
    if ((g_pass_hook && (c0 || dt)) || ui_cand) {
        MacVREncAction *act = [MacVREncAction new];
        act.cb = self;
        act.color = c0;
        act.depth = dt;
        act.loadAction = dt ? (int)desc.depthAttachment.loadAction : -1;
        act.ui = ui_cand ? 1 : 0;
        act.ui_hud = (ui_cand && c0.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB && c0.mipmapLevelCount > 1);
        act.slice = desc.colorAttachments[0].slice;
        objc_setAssociatedObject(enc, kEncActionKey, act, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return enc;
}

static void h_present(id self, SEL s, id drawable) {
    if (g_present_hook) g_present_hook(self, drawable);
    o_present(self, s, drawable);
}

static void h_present_at(id self, SEL s, id drawable, CFTimeInterval t) {
    if (g_present_hook) g_present_hook(self, drawable);
    o_present_at(self, s, drawable, t);
}

static void h_present_after(id self, SEL s, id drawable, CFTimeInterval t) {
    if (g_present_hook) g_present_hook(self, drawable);
    o_present_after(self, s, drawable, t);
}

static id h_next_drawable(CAMetalLayer *self, SEL s) {
    if (self.framebufferOnly) self.framebufferOnly = NO;
    macvr_hook_device(self.device);
    atomic_fetch_add(&g_frame_counter, 1);

    id<CAMetalDrawable> d = o_next_drawable(self, s);
    if (d && self.device) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            id<MTLCommandQueue> q = [self.device newCommandQueue];
            id<MTLCommandBuffer> cb = [q commandBuffer];
            if (cb) {
                Class cbc = object_getClass(cb);
                o_render_enc = (render_enc_imp)macvr_swizzle(cbc, @selector(renderCommandEncoderWithDescriptor:), (IMP)h_render_enc);
                o_present = (present_imp)macvr_swizzle(cbc, @selector(presentDrawable:), (IMP)h_present);
                o_present_at = (present_at_imp)macvr_swizzle(cbc, @selector(presentDrawable:atTime:), (IMP)h_present_at);
                o_present_after = (present_at_imp)macvr_swizzle(cbc, @selector(presentDrawable:afterMinimumDuration:), (IMP)h_present_after);
                macvr_log("Hooked MTLCommandBuffer class: %s", class_getName(cbc));
            }
        });
    }
    return d;
}

// ============================================================================
// MetalFX Frame Interpolator Suppression (for Cyberpunk 2.5.0, from vxmtlfx.m)
// ============================================================================
static BOOL h_fi_supports_device(id self, SEL s, id<MTLDevice> dev) {
    static int logged = 0;
    if (!logged++) {
        macvr_log("MetalFX: %s +supportsDevice: -> NO (suppressing synthetic frame interpolation for VR head-tracking)", class_getName(self));
    }
    return NO;
}

__attribute__((constructor))
static void macvr_init(void) {
    @autoreleasepool {
        macvr_log("Initializing VeRantyx MacVR runtime dylib...");
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (dev) macvr_hook_device(dev);

        o_next_drawable = (next_drawable_imp)macvr_swizzle([CAMetalLayer class], @selector(nextDrawable), (IMP)h_next_drawable);

        // Suppress MetalFX Frame Interpolation in Cyberpunk 2.5.0 unless VERANTYX_FRAMEGEN=1
        const char *fg = getenv("VERANTYX_FRAMEGEN");
        if (!fg || strcmp(fg, "1") != 0) {
            const char *fi_classes[] = { "MTLFXFrameInterpolatorDescriptor", "MTL4FXFrameInterpolatorDescriptor" };
            for (size_t i = 0; i < 2; i++) {
                Class fc = objc_getClass(fi_classes[i]);
                if (fc) macvr_swizzle_class(fc, @selector(supportsDevice:), (IMP)h_fi_supports_device);
            }
        }

        macvr_input_init();
        macvr_presenter_init();
    }
}
