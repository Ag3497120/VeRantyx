// The immersive presenter (see xros_vr.h; VISIONOS_PORT.md, "VR presentation"). visionOS only.
//
// Knobs (read once):
//   VXRT_VR_TAN_Y=0.7          the game's tan(vertical FOV / 2) when no FSR2 constants say it (tan_x from the aspect)
//   VXRT_VR_NEAR=0.1, VXRT_VR_FAR=100, VXRT_VR_DEPTH=std|rev   the game's depth without FSR2's constants: standard
//                              (near 0, far 1) or reversed infinite (near / distance), its planes in game units
//   VXRT_VR_SCALE=1            metres per game unit (the stereo's and the head translation's scale)
//   VXRT_VR_PREDICT=1          with the game's motion vectors: each display frame predicted to its time (the NPU
//                              race's predictions); 0: the newest real frame only, reprojected to the head
//   VXRT_VR_REAL_HZ=0          a fixed real rate for the game (sync-interval presents wait for that many compositor
//                              frames: 22.5 at 90 Hz = every 4th); 0: the game's own sync interval
//   VXRT_VR_DEPTH_OUT=1        depth to the compositor (its per-pixel late reprojection); 0: none (rotation only)
//
// The model: the game's camera is the head as it was when the frame was presented (the game's own camera is moved by
// the mouse or a controller; the head only looks around within the frame). At each compositor frame, each eye is
// that picture moved from the camera it was rendered with to the eye (rotation, the eye's offset, its frustum), by
// the game's depth: head rotation is reprojected, the two eyes get their parallax, and where the game gave motion
// vectors the picture is predicted on to the frame's display time. Without depth the picture is at VXRT_VR_FAR.
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xros_vr.h"
#include "warp_core.h"
#include "npu_interp.h"

void vx_log(const char *fmt, ...);
void vx_xros_pace_tick(void);                     // (src/xros_ui.m: a compositor frame for sync-interval presents)

static int g_on;
void vx_xros_vr_enable(int on) {
    g_on = on;
    const char *n = getenv("VXRT_NPU_INTERP");
    if (on) vx_log("VR: immersive presentation on (CompositorServices; the window is only a fallback); NPU race %s: %s", n && !strcmp(n, "race") ? "on" : "off",
                   n && !strcmp(n, "race") ? "each compositor frame predicted from the game's motion vectors (when it has them) and reprojected to the head" : "real frames reprojected to the head");
}
int vx_xros_vr_on(void) { return g_on; }

static float envf(const char *n, float d) { const char *e = getenv(n); return e && *e ? (float)atof(e) : d; }
static struct { float tan_y, nearp, farp, scale, real_hz; int depth_std, predict, depth_out; } C;
static void config_once(void) {
    C.tan_y = envf("VXRT_VR_TAN_Y", 0.7f);
    C.nearp = envf("VXRT_VR_NEAR", 0.1f);
    C.farp = envf("VXRT_VR_FAR", 100.0f);
    C.scale = envf("VXRT_VR_SCALE", 1.0f);
    C.real_hz = envf("VXRT_VR_REAL_HZ", 0);
    const char *d = getenv("VXRT_VR_DEPTH");
    C.depth_std = !d || strcmp(d, "rev");
    C.predict = envf("VXRT_VR_PREDICT", 1) != 0;
    C.depth_out = envf("VXRT_VR_DEPTH_OUT", 1) != 0;
    vx_log("VR: game projection without FSR2: tan(vFOV/2) %.3f, depth %s near %g far %g; scale %g m/unit; prediction %s; depth to the compositor %s",
           C.tan_y, C.depth_std ? "standard" : "reversed infinite", C.nearp, C.farp, C.scale, C.predict ? "on" : "off", C.depth_out ? "on" : "off");
}
static void config(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, config_once); }

// ---- slots ----
typedef struct {
    __strong id<MTLTexture> color, view, depth, mv;   // (view: the colour as the warp samples it, sRGB-decoded for 8-bit unorm)
    uint32_t w, h, has_depth, has_mv, has_proj, flags;
    float tan_x, tan_y, z_a, z_b, mv_sx, mv_sy;
    float head[16];                  // (origin from the camera it is shown from: the head at its present)
    int head_valid;                  // (0: presented before the first compositor frame; it takes the next frame's head)
    double content_time, interval;
    uint64_t serial;
    int writing, readers;
} vr_slot;
static vr_slot g_slot[3];
static int g_newest = -1, g_held = -1;
static uint64_t g_serial;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static float g_head[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
static double g_last_submit, g_interval = 1.0 / 30;
static double g_present_time;
static int g_head_valid;
// (stats, reset each report)
static unsigned g_frames, g_submits, g_draws, g_noframe, g_depth_frames, g_mv_frames;
static double g_gpu_ms, g_age_ms; static unsigned g_gpu_n;
static float g_tan_seen[4];

// ---- the scene's depth (as stereo.m: the depth of the back buffer's aspect with the most depth-tested draws; copied
// at the end of every pass that wrote it, since a placed resource may be aliased later in the frame) ----
typedef struct { void *tex; uint32_t w, h, draws; } depth_cand;
static depth_cand g_cand[16];
static int g_ncand, g_copies;
static void *g_pick; static uint32_t g_pick_w, g_pick_h; static MTLPixelFormat g_pick_fmt;
static id<MTLBuffer> g_hold;
static uint32_t g_back_w, g_back_h;

void vx_xros_vr_depth_pass(void *cbp, void *texp, uint32_t draws, uint32_t writes, uint32_t funcs) {
    (void)funcs;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cbp;
    id<MTLTexture> t = (__bridge id<MTLTexture>)texp;
    if (!g_on || !draws || !t || !g_back_w || t.sampleCount > 1 || t.textureType != MTLTextureType2D) return;
    MTLPixelFormat f = t.pixelFormat;
    if (f != MTLPixelFormatDepth16Unorm && f != MTLPixelFormatDepth32Float && f != MTLPixelFormatDepth32Float_Stencil8) return;
    uint32_t w = (uint32_t)t.width, h = (uint32_t)t.height;
    if (w < 128 || fabs(((double)w / h) / ((double)g_back_w / g_back_h) - 1.0) > 0.03) return;
    pthread_mutex_lock(&g_lock);
    depth_cand *c = NULL;
    for (int k = 0; k < g_ncand; k++) if (g_cand[k].tex == texp) c = &g_cand[k];
    if (!c && g_ncand < 16) { c = &g_cand[g_ncand++]; *c = (depth_cand){ texp, w, h, 0 }; }
    if (c) c->draws += draws;
    if (texp == g_pick && writes && w == g_pick_w && h == g_pick_h && f == g_pick_fmt) {
        uint32_t bpp = f == MTLPixelFormatDepth16Unorm ? 2 : 4;
        NSUInteger need = (NSUInteger)w * h * bpp;
        if (!g_hold || g_hold.length != need) g_hold = [cb.device newBufferWithLength:need options:MTLResourceStorageModePrivate];
        if (g_hold) {
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                      toBuffer:g_hold destinationOffset:0 destinationBytesPerRow:(NSUInteger)w * bpp destinationBytesPerImage:need
                       options:f == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
            [b endEncoding];
            g_copies++;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static id<MTLTexture> slot_tex(id<MTLDevice> dev, id<MTLTexture> t, MTLPixelFormat f, uint32_t w, uint32_t h) {
    if (t && t.pixelFormat == f && t.width == w && t.height == h) return t;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    td.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:td];
}
// (an 8-bit unorm back buffer holds sRGB-encoded values: sampled through an sRGB view, so the sRGB drawable shows them
//  as they are; VXRT_VR_SRGB=0: as they are, unconverted)
static MTLPixelFormat srgb_of(MTLPixelFormat f) {
    static int on = -1;
    if (on < 0) on = envf("VXRT_VR_SRGB", 1) != 0;
    if (!on) return MTLPixelFormatInvalid;
    return f == MTLPixelFormatRGBA8Unorm ? MTLPixelFormatRGBA8Unorm_sRGB : f == MTLPixelFormatBGRA8Unorm ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatInvalid;
}

void vx_xros_vr_submit(void *cbp, void *backp, uint32_t w, uint32_t h, void (*release)(void *), void *arg) {
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cbp;
    id<MTLTexture> back = (__bridge id<MTLTexture>)backp;
    id<MTLDevice> dev = cb.device;
    config();
    double now = CACurrentMediaTime();
    pthread_mutex_lock(&g_lock);
    if (g_last_submit > 0) { double d = now - g_last_submit; if (d > 0 && d < 0.5) g_interval = g_interval * 0.8 + d * 0.2; }
    g_last_submit = now;
    g_back_w = w; g_back_h = h;
    int s = -1;
    for (int k = 0; k < 3 && s < 0; k++) if (k != g_held && k != g_newest && !g_slot[k].writing && !g_slot[k].readers) s = k;
    // (the scene's depth this frame, and the pick for the next)
    int had_depth = g_copies > 0 && g_hold;
    id<MTLBuffer> hold = g_hold; uint32_t dw = g_pick_w, dh = g_pick_h; MTLPixelFormat df = g_pick_fmt;
    int best = -1;
    for (int k = 0; k < g_ncand; k++) if (best < 0 || g_cand[k].draws > g_cand[best].draws) best = k;
    void *np = best >= 0 ? g_cand[best].tex : NULL;
    if (np != g_pick) {
        g_pick = np;
        if (np) {
            id<MTLTexture> t = (__bridge id<MTLTexture>)np;
            g_pick_w = (uint32_t)t.width; g_pick_h = (uint32_t)t.height; g_pick_fmt = t.pixelFormat;
            vx_log("VR: the scene's depth: %ux%u format %lu (%u depth-tested draws)", g_pick_w, g_pick_h, (unsigned long)g_pick_fmt, g_cand[best].draws);
        }
    }
    g_ncand = 0; g_copies = 0;
    if (had_depth) g_hold = nil;                     // (the slot keeps reading this one; the next frame copies into a new one)
    if (s >= 0) g_slot[s].writing = 1;
    float head[16]; memcpy(head, g_head, sizeof head);
    int head_valid = g_head_valid;
    double interval = g_interval;
    pthread_mutex_unlock(&g_lock);
    // the frame's scene from the D3D12 layer (FSR2's inputs: motion vectors, depth, constants), taken every frame
    void *mvp = NULL, *dpp = NULL; uint32_t consts[32];
    int scene = npu_interp_take_scene(&mvp, &dpp, consts);
    id<MTLTexture> smv = scene ? (__bridge_transfer id<MTLTexture>)mvp : nil, sdp = scene ? (__bridge_transfer id<MTLTexture>)dpp : nil;
    if (s < 0) {                                      // (all three in use: this frame is not shown; the game is faster than the GPU)
        if (release) [cb addCompletedHandler:^(id<MTLCommandBuffer> d) { (void)d; release(arg); }];
        return;
    }
    vr_slot *S = &g_slot[s];
    id<MTLTexture> was = S->color;
    S->color = slot_tex(dev, S->color, back.pixelFormat, w, h);
    if (S->color != was || !S->view) {
        MTLPixelFormat sf = srgb_of(back.pixelFormat);
        S->view = sf != MTLPixelFormatInvalid ? [S->color newTextureViewWithPixelFormat:sf] : S->color;
    }
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:back sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
             toTexture:S->color destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    S->w = w; S->h = h;
    S->has_depth = S->has_mv = S->has_proj = 0;
    float th = 0, d2v[4], mvs[2]; uint32_t disp[2];
    if (scene & 2) S->has_proj = npu_fsr2_parse(consts, &th, d2v, mvs, disp) != 0;
    if (scene && sdp && smv) {                         // (FSR2's depth and motion vectors: render size)
        MTLPixelFormat f = sdp.pixelFormat;
        uint32_t sw = (uint32_t)sdp.width, sh = (uint32_t)sdp.height, bpp = f == MTLPixelFormatDepth16Unorm ? 2 : 4;
        MTLPixelFormat to = f == MTLPixelFormatDepth16Unorm ? MTLPixelFormatR16Unorm : MTLPixelFormatR32Float;
        S->depth = slot_tex(dev, S->depth, to, sw, sh);
        if (f == MTLPixelFormatR32Float) {
            [b copyFromTexture:sdp sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(sw, sh, 1)
                     toTexture:S->depth destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            S->has_depth = 1;
        } else if (f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatDepth16Unorm) {
            id<MTLBuffer> tmp = [dev newBufferWithLength:(NSUInteger)sw * sh * bpp options:MTLResourceStorageModePrivate];
            if (tmp) {
                [b copyFromTexture:sdp sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(sw, sh, 1)
                          toBuffer:tmp destinationOffset:0 destinationBytesPerRow:(NSUInteger)sw * bpp destinationBytesPerImage:(NSUInteger)sw * sh * bpp
                           options:f == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
                [b copyFromBuffer:tmp sourceOffset:0 sourceBytesPerRow:(NSUInteger)sw * bpp sourceBytesPerImage:(NSUInteger)sw * sh * bpp
                       sourceSize:MTLSizeMake(sw, sh, 1) toTexture:S->depth destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
                S->has_depth = 1;
            }
        }
        S->mv = slot_tex(dev, S->mv, smv.pixelFormat, (uint32_t)smv.width, (uint32_t)smv.height);
        [b copyFromTexture:smv sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(smv.width, smv.height, 1)
                 toTexture:S->mv destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        S->has_mv = S->has_proj;                       // (their scale comes with FSR2's constants)
    }
    if (!S->has_depth && had_depth && hold) {          // (the depth pass's copy)
        MTLPixelFormat to = df == MTLPixelFormatDepth16Unorm ? MTLPixelFormatR16Unorm : MTLPixelFormatR32Float;
        uint32_t bpp = df == MTLPixelFormatDepth16Unorm ? 2 : 4;
        S->depth = slot_tex(dev, S->depth, to, dw, dh);
        [b copyFromBuffer:hold sourceOffset:0 sourceBytesPerRow:(NSUInteger)dw * bpp sourceBytesPerImage:(NSUInteger)dw * dh * bpp
               sourceSize:MTLSizeMake(dw, dh, 1) toTexture:S->depth destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        S->has_depth = 1;
    }
    [b endEncoding];
    // the projection: FSR2's, else the knobs'
    vxw_params p = { 0 };
    if (S->has_proj && vxw_projection_from_fsr2(&p, th, d2v)) {
        S->tan_x = p.tan_x; S->tan_y = p.tan_y; S->z_a = p.z_a; S->z_b = p.z_b; S->flags = 0;
        S->mv_sx = mvs[0] * w; S->mv_sy = mvs[1] * h;
    } else {
        S->has_proj = 0; S->has_mv = 0;
        S->tan_y = C.tan_y; S->tan_x = C.tan_y * (float)w / (float)h;
        if (C.depth_std) { S->z_a = C.farp * C.nearp / (C.farp - C.nearp); S->z_b = -C.nearp / (C.farp - C.nearp); S->flags = VXW_DEPTH_STD; }
        else { S->z_a = C.nearp; S->z_b = 0; S->flags = 0; }
    }
    memcpy(S->head, head, sizeof head);
    S->head_valid = head_valid;
    S->content_time = now; S->interval = interval;
    uint64_t serial = ++g_serial;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> d) {
        (void)d;
        pthread_mutex_lock(&g_lock);
        S->writing = 0; S->serial = serial;
        if (g_newest < 0 || g_slot[g_newest].serial < serial) g_newest = s;
        g_submits++;
        if (S->has_depth) g_depth_frames++;
        if (S->has_mv) g_mv_frames++;
        pthread_mutex_unlock(&g_lock);
        if (release) release(arg);
    }];
    static int told;
    if (!told++) vx_log("VR: first frame %ux%u format %lu into slot %d (depth %s, motion vectors %s, FSR2 projection %s)", w, h,
                        (unsigned long)back.pixelFormat, s, S->has_depth ? "yes" : "not yet", S->has_mv ? "yes" : "no", S->has_proj ? "yes" : "no");
}

// ---- the compositor loop ----
static vxw_core *g_core;
static int g_frame_slot = -1;
static id<MTLTexture> g_zero_mv, g_far_depth;
static double g_report;

int vx_xros_vr_begin_frame(const float m[16], double presentation_time) {
    config();
    vx_xros_pace_tick();
    pthread_mutex_lock(&g_lock);
    memcpy(g_head, m, sizeof g_head);
    g_head_valid = 1;
    for (int k = 0; k < 3; k++) if (!g_slot[k].writing && !g_slot[k].head_valid) { memcpy(g_slot[k].head, m, sizeof g_head); g_slot[k].head_valid = 1; }
    g_present_time = presentation_time;
    g_frames++;
    if (g_newest >= 0) g_held = g_newest;
    int have = g_held >= 0;
    if (have) g_slot[g_held].readers++;              // (until this frame's command buffer completes: vx_xros_vr_end_frame)
    g_frame_slot = g_held;
    if (have) g_age_ms += (presentation_time - g_slot[g_held].content_time) * 1e3; else g_noframe++;
    pthread_mutex_unlock(&g_lock);
    return have;
}

static void mul4(const float *a, const float *b, float *o) {     // (column-major: o = a * b)
    for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) {
        float v = 0;
        for (int k = 0; k < 4; k++) v += a[k * 4 + r] * b[c * 4 + k];
        o[c * 4 + r] = v;
    }
}

int vx_xros_vr_draw_eye(void *cbp, int eye, const float view_from_origin[16], const float tan[4], float near_m, void *outp, void *odp) {
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cbp;
    pthread_mutex_lock(&g_lock);
    int s = g_frame_slot;
    double pt = g_present_time;
    if (eye >= 0 && eye < 2) memcpy(&g_tan_seen[0], tan, 4 * sizeof *tan);
    pthread_mutex_unlock(&g_lock);
    if (s < 0) return 0;
    vr_slot *S = &g_slot[s];
    id<MTLDevice> dev = cb.device;
    if (!g_core) {
        g_core = vxw_create((__bridge void *)dev);
        float zero[2] = { 0, 0 };
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Float width:1 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        g_zero_mv = [dev newTextureWithDescriptor:td];
        [g_zero_mv replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:zero bytesPerRow:8];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:1 height:1 mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        g_far_depth = [dev newTextureWithDescriptor:td];
        vx_log("VR: warp core %s", g_core ? "ready" : "did not compile");
    }
    if (!g_core) return 0;
    // the eye from the camera the frame was shown from (both ARKit's right-up-back), then in the warp's right-up-forward
    float M[16];
    mul4(view_from_origin, S->head, M);
    vxw_params p = { 0 };
    p.w = S->w; p.h = S->h;
    p.flags = VXW_POSE | S->flags;
    p.tan_x = S->tan_x; p.tan_y = S->tan_y; p.z_a = S->z_a; p.z_b = S->z_b;
    static const float sg[3] = { 1, 1, -1 };
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) p.rot[r * 3 + c] = sg[r] * M[c * 4 + r] * sg[c];
        p.pos[r] = sg[r] * M[12 + r] / C.scale;
    }
    memcpy(p.dst_tan, tan, sizeof p.dst_tan);
    p.dst_near = C.depth_out && odp ? near_m / C.scale : 0;
    id<MTLTexture> depth = S->has_depth ? S->depth : g_far_depth;
    if (!S->has_depth) {                                // (no depth: everything at the far plane)
        float far1 = S->flags & VXW_DEPTH_STD ? 1.0f : S->z_a / C.farp;
        [g_far_depth replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&far1 bytesPerRow:4];
    }
    if (S->has_mv && C.predict && S->interval > 0) {
        float t = (float)((pt - S->content_time) / S->interval);
        p.t = t < 0 ? 0 : t > 2 ? 2 : t;
        p.mv_sx = S->mv_sx; p.mv_sy = S->mv_sy;
    }
    id<MTLTexture> mv = S->has_mv ? S->mv : g_zero_mv;
    int ok = vxw_encode_eye(g_core, cbp, &p, (__bridge void *)S->view, NULL, (__bridge void *)mv, (__bridge void *)depth, outp,
                            C.depth_out ? odp : NULL);
    pthread_mutex_lock(&g_lock);
    if (ok) g_draws++;
    pthread_mutex_unlock(&g_lock);
    static int told;
    if (!told++) vx_log("VR: first eye: %s, frame %ux%u -> %lux%lu, frustum %.3f %.3f %.3f %.3f, game tan %.3f %.3f, depth %s, pos %.4f %.4f %.4f",
                        ok ? "warped" : "NOT encoded", S->w, S->h, (unsigned long)((__bridge id<MTLTexture>)outp).width,
                        (unsigned long)((__bridge id<MTLTexture>)outp).height, tan[0], tan[1], tan[2], tan[3], p.tan_x, p.tan_y,
                        S->has_depth ? "the game's" : "none (far)", p.pos[0], p.pos[1], p.pos[2]);
    return ok;
}

void vx_xros_vr_end_frame(void *cbp) {
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cbp;
    pthread_mutex_lock(&g_lock);
    int s = g_frame_slot;
    g_frame_slot = -1;
    if (!cb && s >= 0) g_slot[s].readers--;           // (a frame the compositor cancelled)
    pthread_mutex_unlock(&g_lock);
    if (!cb) return;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> d) {
        pthread_mutex_lock(&g_lock);
        if (s >= 0) g_slot[s].readers--;
        if (d.GPUEndTime > d.GPUStartTime) { g_gpu_ms += (d.GPUEndTime - d.GPUStartTime) * 1e3; g_gpu_n++; }
        pthread_mutex_unlock(&g_lock);
    }];
}

void vx_xros_vr_stats(char *out, unsigned size) {
    double now = CACurrentMediaTime();
    pthread_mutex_lock(&g_lock);
    double dt = g_report > 0 ? now - g_report : 1;
    g_report = now;
    unsigned have = g_frames - g_noframe;
    snprintf(out, size, "\"compositor_hz\":%.1f,\"game_fps\":%.1f,\"eyes_warped_per_s\":%.1f,\"no_frame\":%u,\"warp_gpu_ms\":%.2f,"
             "\"frame_age_ms\":%.1f,\"game_interval_ms\":%.1f,\"depth_frames\":%u,\"mv_frames\":%u,\"tan\":[%.3f,%.3f,%.3f,%.3f]",
             g_frames / dt, g_submits / dt, g_draws / dt, g_noframe, g_gpu_n ? g_gpu_ms / g_gpu_n : 0, have ? g_age_ms / have : 0,
             g_interval * 1e3, g_depth_frames, g_mv_frames, g_tan_seen[0], g_tan_seen[1], g_tan_seen[2], g_tan_seen[3]);
    g_frames = g_submits = g_draws = g_noframe = g_depth_frames = g_mv_frames = 0;
    g_gpu_ms = g_age_ms = 0; g_gpu_n = 0;
    pthread_mutex_unlock(&g_lock);
}

float vx_xros_vr_real_hz(void) { config(); return C.real_hz; }
