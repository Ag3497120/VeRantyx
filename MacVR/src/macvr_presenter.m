#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "macvr.h"
#include "warp_core.h"

// ============================================================================
// Shared Frame Layout (matches MacEncoder/HardwareEncoder.swift)
// ============================================================================
// Header (64 bytes):
//   offset 0:  int32 latestIndex (0 or 1)
//   offset 16: int32 captureSourceMode (2 = full VR with 90Hz Vision Pro reprojection, 3 = flat stereo screen)
// Slot (starts at 64 for slot 0, 64 + kSlotSize for slot 1):
//   offset +0:  uint32 sequence
//   offset +4:  uint32 width  (2 * eye_w)
//   offset +8:  uint32 height (eye_h)
//   offset +12: uint32 format (87 = DXGI_FORMAT_B8G8R8A8_UNORM)
//   offset +16: float  transform[16] (column-major renderHeadTransform)
//   offset +80: double timestamp     (visionTimestamp)
//   offset +88: uint8  pixels[width * height * 4]
static const size_t kHeaderBytes = 64;
static const size_t kSlotHeaderBytes = 88;
static const size_t kMaxPixelBytes = 4096ULL * 4096ULL * 8ULL;
static const size_t kSlotStride = kSlotHeaderBytes + kMaxPixelBytes;
static const size_t kSharedHandsTailBytes = 4096;
static const size_t kTotalSharedBytes = kHeaderBytes + 2 * kSlotStride + kSharedHandsTailBytes;

static int g_frame_fd = -1;
static uint8_t *g_frame_map = NULL;
static uint32_t g_frame_seq = 0;
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;

static uint8_t *macvr_ensure_shared_frame_locked(const char *override_path) {
    if (g_frame_map && !override_path) return g_frame_map;
    const char *path = override_path ? override_path : getenv("VERANTYX_SHARED_FRAME");
    if (!path || !*path) path = "/tmp/verantyx_vr_frame";

    int fd = open(path, O_RDWR | O_CREAT, 0666);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < kTotalSharedBytes) {
        if (ftruncate(fd, (off_t)kTotalSharedBytes) != 0) {
            close(fd);
            return NULL;
        }
    }
    void *ptr = mmap(NULL, kTotalSharedBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    if (override_path && g_frame_map) {
        munmap(g_frame_map, kTotalSharedBytes);
        if (g_frame_fd >= 0) close(g_frame_fd);
    }
    g_frame_fd = fd;
    g_frame_map = (uint8_t *)ptr;
    macvr_log("Mapped shared VR frame buffer at %s (%zu MB)", path, kTotalSharedBytes >> 20);
    return g_frame_map;
}

static void macvr_publish_stereo_frame(const void *bgra_bytes,
                                       uint32_t width,
                                       uint32_t height,
                                       size_t row_bytes,
                                       const float render_head_col[16],
                                       double vision_ts,
                                       int capture_mode,
                                       const char *override_path) {
    size_t dst_row_bytes = (size_t)width * 4;
    size_t total_pixel_bytes = dst_row_bytes * (size_t)height;
    if (total_pixel_bytes > kMaxPixelBytes) return;

    pthread_mutex_lock(&g_frame_lock);
    uint8_t *base = macvr_ensure_shared_frame_locked(override_path);
    if (!base) {
        pthread_mutex_unlock(&g_frame_lock);
        return;
    }
    int32_t cur_idx = 0;
    memcpy(&cur_idx, base, sizeof(int32_t));
    int32_t next_idx = (cur_idx == 0) ? 1 : 0;
    uint8_t *slot = base + kHeaderBytes + (size_t)next_idx * kSlotStride;

    uint32_t seq = ++g_frame_seq;
    uint32_t fmt = 87; // DXGI_FORMAT_B8G8R8A8_UNORM
    memcpy(slot + 0, &seq, 4);
    memcpy(slot + 4, &width, 4);
    memcpy(slot + 8, &height, 4);
    memcpy(slot + 12, &fmt, 4);
    memcpy(slot + 16, render_head_col, 64);
    memcpy(slot + 80, &vision_ts, 8);

    uint8_t *dst_pixels = slot + kSlotHeaderBytes;
    if (row_bytes == dst_row_bytes) {
        memcpy(dst_pixels, bgra_bytes, total_pixel_bytes);
    } else {
        const uint8_t *src = (const uint8_t *)bgra_bytes;
        for (uint32_t y = 0; y < height; y++) {
            memcpy(dst_pixels + (size_t)y * dst_row_bytes, src + (size_t)y * row_bytes, dst_row_bytes);
        }
    }

    int32_t mode = capture_mode;
    memcpy(base + 16, &mode, sizeof(int32_t));
    atomic_thread_fence(memory_order_release);
    memcpy(base + 0, &next_idx, sizeof(int32_t));
    pthread_mutex_unlock(&g_frame_lock);
}

// ============================================================================
// Floating Stereo HUD Panel Shader (from VXpro/vr/xros_vr.m lines 2939-2998)
// ============================================================================
static NSString *const kMacVRHudShaderSrc =
    @"#include <metal_stdlib>\n"
    @"using namespace metal;\n"
    @"struct HudUniforms {\n"
    @"    float4 uv_scale_bias; // xy = scale, zw = bias\n"
    @"};\n"
    @"struct VSOut {\n"
    @"    float4 pos [[position]];\n"
    @"    float2 uv;\n"
    @"};\n"
    @"vertex VSOut macvr_hud_vs(uint vid [[vertex_id]]) {\n"
    @"    float2 p = float2((vid << 1) & 2, vid & 2);\n"
    @"    VSOut o;\n"
    @"    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    @"    o.uv = float2(p.x, 1.0 - p.y);\n"
    @"    return o;\n"
    @"}\n"
    @"fragment float4 macvr_hud_fs(VSOut in [[stage_in]],\n"
    @"                             texture2d<float> hud [[texture(0)]],\n"
    @"                             constant HudUniforms &u [[buffer(0)]]) {\n"
    @"    constexpr sampler s(filter::linear, address::clamp_to_zero);\n"
    @"    float2 h = in.uv * u.uv_scale_bias.xy + u.uv_scale_bias.zw;\n"
    @"    if (any(h < 0.0) || any(h > 1.0)) discard_fragment();\n"
    @"    float4 c = hud.sample(s, h);\n"
    @"    if (c.a < 1.0 / 255.0) discard_fragment();\n"
    @"    // Convert linear sampled sRGB HUD into sRGB-encoded BGRA8 target with premultiplied alpha\n"
    @"    float3 srgb = pow(max(c.rgb, 0.0), 1.0 / 2.2);\n"
    @"    return float4(srgb, c.a);\n"
    @"}\n";

// ============================================================================
// Presenter State
// ============================================================================
static vxw_core *g_warp = NULL;
static id<MTLDevice> g_dev = nil;
static id<MTLRenderPipelineState> g_hud_pipe = nil;
static id<MTLBuffer> g_depth_buf = nil;
static id<MTLTexture> g_depth_tex = nil;
static NSUInteger g_depth_w = 0, g_depth_h = 0;
static atomic_uint g_depth_captured_frame = 0;
static atomic_uint g_present_frame = 0;

static uint32_t g_eye_w = 1920;
static uint32_t g_eye_h = 1824;
static id<MTLTexture> g_eye_tex[2] = {nil, nil};
static id<MTLTexture> g_sbs_tex = nil;
#define kReadbackRing 3
static id<MTLBuffer> g_readback_buf[kReadbackRing] = {nil, nil, nil};
static size_t g_readback_row_bytes = 0;
static unsigned g_readback_idx = 0;

static void macvr_ensure_resources(id<MTLDevice> dev) {
    if (!dev) return;
    if (g_dev == dev && g_warp && g_eye_tex[0] && g_sbs_tex) return;
    g_dev = dev;

    const char *ew = getenv("VERANTYX_EYE_W");
    const char *eh = getenv("VERANTYX_EYE_H");
    if (ew && atoi(ew) >= 640 && atoi(ew) <= 2048) g_eye_w = (uint32_t)atoi(ew);
    if (eh && atoi(eh) >= 640 && atoi(eh) <= 2048) g_eye_h = (uint32_t)atoi(eh);

    if (g_warp) vxw_destroy(g_warp);
    g_warp = vxw_create((__bridge void *)dev);

    // Build floating HUD pipeline
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kMacVRHudShaderSrc options:nil error:&err];
    if (lib) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"macvr_hud_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"macvr_hud_fs"];
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
        ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
        ca.blendingEnabled = YES;
        ca.sourceRGBBlendFactor = MTLBlendFactorOne;
        ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
        ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        g_hud_pipe = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    }

    for (int e = 0; e < 2; e++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:g_eye_w
                                                                                     height:g_eye_h
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        g_eye_tex[e] = [dev newTextureWithDescriptor:td];
    }

    uint32_t sbs_w = g_eye_w * 2;
    MTLTextureDescriptor *sbs_td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:sbs_w
                                                                                     height:g_eye_h
                                                                                  mipmapped:NO];
    sbs_td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    sbs_td.storageMode = MTLStorageModePrivate;
    g_sbs_tex = [dev newTextureWithDescriptor:sbs_td];

    g_readback_row_bytes = ((size_t)sbs_w * 4 + 255) & ~(size_t)255;
    size_t buf_len = g_readback_row_bytes * (size_t)g_eye_h;
    for (int i = 0; i < kReadbackRing; i++) {
        g_readback_buf[i] = [dev newBufferWithLength:buf_len options:MTLResourceStorageModeShared];
    }
    macvr_log("MacVR presenter resources ready on %s: per-eye=%ux%u, sbs=%ux%u",
              dev.name.UTF8String, g_eye_w, g_eye_h, sbs_w, g_eye_h);
}

static void macvr_draw_floating_hud(id<MTLCommandBuffer> cb,
                                    id<MTLTexture> eyeTarget,
                                    id<MTLTexture> hudTex,
                                    int eye,
                                    const float eye_tan[4],
                                    float ipd) {
    if (!cb || !eyeTarget || !hudTex || !g_hud_pipe) return;
    // Floating stereo HUD panel at z_hud = 1.5m, half-tan = tan(28 deg) = 0.5317 (from VXpro xros_vr.m)
    float tx_half = 0.5317f;
    float ty_half = tx_half * ((float)hudTex.height / (float)(hudTex.width ? hudTex.width : 1));
    float z_hud = 1.50f;
    float ex = ((eye == 0) ? -0.5f : 0.5f) * ipd;

    float tl = eye_tan[0], tr = eye_tan[1], tu = eye_tan[2], td = eye_tan[3];
    // For u in [0,1], tan_x(u) = -tl + u * (tl + tr), tan_y(v) = tu - v * (tu + td)
    // Panel u_hud = ((tan_x(u) + ex / z_hud) / tx_half) * 0.5 + 0.5
    float sx = (tl + tr) / (2.0f * tx_half);
    float bx = (-tl + ex / z_hud) / (2.0f * tx_half) + 0.5f;
    float sy = (tu + td) / (2.0f * ty_half);
    float by = (-td) / (2.0f * ty_half) + 0.5f;
    float uniforms[4] = { sx, sy, bx, by };

    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = eyeTarget;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:g_hud_pipe];
    [enc setFragmentTexture:hudTex atIndex:0];
    [enc setFragmentBytes:uniforms length:sizeof(uniforms) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

// ============================================================================
// Pass Hook: Capture Scene Reversed-Z Depth Buffer
// ============================================================================
static void macvr_on_pass(id cbObj, id colorObj, id depthObj, int loadAction) {
    id<MTLCommandBuffer> cb = cbObj;
    id<MTLTexture> dt = depthObj;
    if (!cb || !dt) return;

    // Capture the primary scene depth buffer (cleared at start of G-buffer, >= 640x360)
    MTLPixelFormat fmt = dt.pixelFormat;
    if (fmt != MTLPixelFormatDepth32Float_Stencil8 && fmt != MTLPixelFormatDepth32Float) return;
    if (dt.width < 640 || dt.height < 360 || dt.textureType != MTLTextureType2D) return;
    if (loadAction != (int)MTLLoadActionClear) return;

    unsigned pf = atomic_load(&g_present_frame) + 1;
    if (atomic_exchange(&g_depth_captured_frame, pf) == pf) return;

    NSUInteger w = dt.width, h = dt.height;
    NSUInteger row_bytes = ((w * 4) + 255) & ~(NSUInteger)255;
    if (!g_depth_buf || g_depth_w != w || g_depth_h != h) {
        g_depth_w = w;
        g_depth_h = h;
        g_depth_buf = [cb.device newBufferWithLength:row_bytes * h options:MTLResourceStorageModePrivate];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float
                                                                                      width:w
                                                                                     height:h
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        g_depth_tex = [g_depth_buf newTextureWithDescriptor:td offset:0 bytesPerRow:row_bytes];
        macvr_log("Scene depth capture buffer allocated: %lux%lu (fmt=%lu)", (unsigned long)w, (unsigned long)h, (unsigned long)fmt);
    }
    if (!g_depth_buf) return;

    MTLBlitOption opt = (fmt == MTLPixelFormatDepth32Float_Stencil8) ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:dt
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(w, h, 1)
               toBuffer:g_depth_buf
      destinationOffset:0
 destinationBytesPerRow:row_bytes
destinationBytesPerImage:row_bytes * h
                options:opt];
    [bl endEncoding];
}

// ============================================================================
// Present Hook: Stereo Depth Warp + Floating HUD + Shared Frame Output
// ============================================================================
static void macvr_encode_and_publish(id<MTLCommandBuffer> cb,
                                     id<MTLTexture> colorTex,
                                     id<MTLTexture> depthTex,
                                     id<MTLTexture> hudTex,
                                     int vr_mode,
                                     const char *override_frame_path) {
    if (!cb || !colorTex) return;
    macvr_ensure_resources(cb.device);
    if (!g_warp || !g_eye_tex[0] || !g_eye_tex[1] || !g_sbs_tex) return;

    float eye_tangents[8] = {
        1.000f, 0.839f, 0.839f, 1.000f,
        0.839f, 1.000f, 0.839f, 1.000f
    };
    float ipd = 0.063f;
    macvr_read_head_tracking(NULL, NULL, eye_tangents, &ipd, NULL);

    float render_head[16];
    double vision_ts = 0.0;
    float res_yaw = 0.0f, res_pitch = 0.0f, res_roll = 0.0f;
    int driving = 0;
    macvr_input_step_camera(vr_mode, macvr_ui_menu(), render_head, &vision_ts, &res_yaw, &res_pitch, &res_roll, &driving);

    // Use game's real projection tangents when available, else fallback to 100 deg HFOV
    float gtx = 0.0f, gty = 0.0f;
    if (!macvr_game_tan(&gtx, &gty) || gtx <= 0.1f || gty <= 0.1f) {
        float aspect = (float)colorTex.width / (float)(colorTex.height ? colorTex.height : 1);
        gtx = tanf(50.0f * (float)M_PI / 180.0f); // 100 deg HFOV
        gty = gtx / aspect;
    }

    // Build 3x3 view-space rotation matrix for residual yaw/pitch/roll
    float cy = cosf(res_yaw), sy = sinf(res_yaw);
    float cp = cosf(res_pitch), sp = sinf(res_pitch);
    float cr = cosf(res_roll), sr = sinf(res_roll);
    float rot_mat[9] = {
        cy * cr + sy * sp * sr,  -cy * sr + sy * sp * cr,  sy * cp,
        cp * sr,                  cp * cr,                -sp,
       -sy * cr + cy * sp * sr,   sy * sr + cy * sp * cr,  cy * cp
    };

    for (int e = 0; e < 2; e++) {
        vxw_params wp;
        memset(&wp, 0, sizeof(wp));
        wp.w = (uint32_t)colorTex.width;
        wp.h = (uint32_t)colorTex.height;
        const float *et = &eye_tangents[e * 4];
        if (vr_mode && depthTex) {
            wp.flags = VXW_POSE;
            wp.tan_x = gtx;
            wp.tan_y = gty;
            wp.z_a   = 0.10f; // REDengine reversed-Z near plane
            wp.z_b   = 0.0f;  // infinite reversed-Z
            memcpy(wp.rot, rot_mat, sizeof(rot_mat));
            // Target camera from B's camera: X' = rot * X + pos, so for eye at +ex in view space, pos[0] = -ex
            wp.pos[0] = -(((e == 0) ? -0.5f : 0.5f) * ipd);
            wp.pos[1] = 0.0f;
            wp.pos[2] = 0.0f;
            wp.dst_tan[0] = et[0];
            wp.dst_tan[1] = et[1];
            wp.dst_tan[2] = et[2];
            wp.dst_tan[3] = et[3];
        } else {
            // Virtual cinema screen mode (menus / cutscenes / loading): unwarped identity copy
            wp.flags = 0;
            wp.tan_x = gtx;
            wp.tan_y = gty;
            wp.z_a   = 0.10f;
            wp.z_b   = 0.0f;
            wp.rot[0] = 1.0f; wp.rot[4] = 1.0f; wp.rot[8] = 1.0f;
        }
        vxw_encode_eye(g_warp,
                       (__bridge void *)cb,
                       &wp,
                       (__bridge void *)colorTex,
                       NULL,
                       NULL,
                       (__bridge void *)(vr_mode ? depthTex : nil),
                       (__bridge void *)g_eye_tex[e],
                       NULL);
        if (vr_mode && hudTex) {
            macvr_draw_floating_hud(cb, g_eye_tex[e], hudTex, e, et, ipd);
        }
    }

    // Pack Left & Right eyes side-by-side into g_sbs_tex and copy to ring buffer
    unsigned ring_slot = (g_readback_idx++) % kReadbackRing;
    id<MTLBuffer> rbuf = g_readback_buf[ring_slot];
    uint32_t eye_w = g_eye_w, eye_h = g_eye_h;
    uint32_t sbs_w = eye_w * 2;
    size_t row_bytes = g_readback_row_bytes;

    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    for (int e = 0; e < 2; e++) {
        [bl copyFromTexture:g_eye_tex[e]
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(eye_w, eye_h, 1)
                  toTexture:g_sbs_tex
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake((NSUInteger)e * eye_w, 0, 0)];
    }
    [bl copyFromTexture:g_sbs_tex
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(sbs_w, eye_h, 1)
               toBuffer:rbuf
      destinationOffset:0
 destinationBytesPerRow:row_bytes
destinationBytesPerImage:row_bytes * eye_h];
    [bl endEncoding];

    int capture_mode = vr_mode ? 2 : 3;
    NSString *overrideStr = override_frame_path ? @(override_frame_path) : nil;
    struct { float m[16]; } head_box;
    memcpy(head_box.m, render_head, sizeof(head_box.m));

    [cb addCompletedHandler:^(id<MTLCommandBuffer> _Nonnull completed) {
        (void)completed;
        macvr_publish_stereo_frame(rbuf.contents,
                                   sbs_w,
                                   eye_h,
                                   row_bytes,
                                   head_box.m,
                                   vision_ts,
                                   capture_mode,
                                   overrideStr ? overrideStr.UTF8String : NULL);
    }];
}

static void macvr_on_present(id cbObj, id drawableObj) {
    id<MTLCommandBuffer> cb = cbObj;
    id<CAMetalDrawable> drawable = drawableObj;
    if (!cb || !drawable || !drawable.texture) return;

    unsigned pf = atomic_fetch_add(&g_present_frame, 1) + 1;
    unsigned df = atomic_load(&g_depth_captured_frame);
    int has_scene_depth = (df != 0 && pf - df <= 2 && g_depth_tex != nil);
    int in_menu = macvr_ui_menu();
    int vr_mode = (has_scene_depth && !in_menu);

    macvr_hud_suppress_set(vr_mode);
    id<MTLTexture> hudTex = (__bridge id<MTLTexture>)macvr_hud_take();

    macvr_encode_and_publish(cb, drawable.texture, has_scene_depth ? g_depth_tex : nil, hudTex, vr_mode, NULL);

    if (pf == 1 || pf % 300 == 0) {
        macvr_log("Presented frame #%u (vr_mode=%d, depth=%dx%d, hud=%s, menu=%d)",
                  pf, vr_mode, (int)g_depth_w, (int)g_depth_h, hudTex ? "yes" : "no", in_menu);
    }
}

void macvr_presenter_init(void) {
    macvr_set_pass_hook(macvr_on_pass);
    macvr_set_present_hook(macvr_on_present);
    macvr_log("MacVR stereo presenter initialized.");
}

// ============================================================================
// Synthetic Integration Test Helper (for macvr_test)
// ============================================================================
int macvr_test_present_synthetic_frame(id mtlDevice, const char *sharedFramePath, int vrMode) {
    id<MTLDevice> dev = mtlDevice;
    if (!dev) return 0;
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];

    MTLTextureDescriptor *ctd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1280 height:720 mipmapped:NO];
    ctd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> color = [dev newTextureWithDescriptor:ctd];

    MTLTextureDescriptor *dtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:1280 height:720 mipmapped:NO];
    dtd.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> depth = [dev newTextureWithDescriptor:dtd];

    MTLTextureDescriptor *htd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:1280 height:720 mipmapped:NO];
    htd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> hud = [dev newTextureWithDescriptor:htd];

    // Clear color & HUD to known non-zero values
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = color;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.5, 0.8, 1.0);
    id<MTLRenderCommandEncoder> ce = [cb renderCommandEncoderWithDescriptor:rp];
    [ce endEncoding];

    MTLRenderPassDescriptor *hrp = [MTLRenderPassDescriptor renderPassDescriptor];
    hrp.colorAttachments[0].texture = hud;
    hrp.colorAttachments[0].loadAction = MTLLoadActionClear;
    hrp.colorAttachments[0].storeAction = MTLStoreActionStore;
    hrp.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.8, 0.1, 0.75);
    id<MTLRenderCommandEncoder> hce = [cb renderCommandEncoderWithDescriptor:hrp];
    [hce endEncoding];

    macvr_encode_and_publish(cb, color, depth, hud, vrMode, sharedFramePath);
    [cb commit];
    [cb waitUntilCompleted];
    return 1;
}
