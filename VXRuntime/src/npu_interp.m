// NPU mode for flat presents (VXRT_NPU_INTERP=1): every frame a program presents in its window is followed, on
// screen, by a frame VideoToolbox's VTLowLatencyFrameInterpolation (macOS 26, on the Neural Engine) makes between
// it and the next one, so the window shows twice the program's frame rate.
//
// That processor can only make frames between two it has (phases strictly inside (0, 1)), so frame N is shown half a
// frame interval late: when N has been drawn, the midpoint of N-1 and N is shown, and N half the interval after it.
// Per present:
//   1. (the presenting thread) a copy of the back buffer, and from it the picture in 420v (the one format the
//      processor takes), tile by tile (it takes at most ~0.92 Mpx a frame), into IOSurfaces it shares; committed with
//      the program's own work. Nothing here waits for the display or the Neural Engine.
//   2. (a serial queue, when the GPU is done) the request to the interpolator process (tools/vxrt_npu.m: the processor
//      reports itself unsupported under Rosetta), waited for at most half a frame interval; then the middle frame is
//      presented at once and the real one at a time half an interval later (presentDrawable:atTime:). Late or failed:
//      the real frame alone, at once.
// The interpolated frame goes back to RGB in the shader that draws it, blending the tiles where they overlap.
//
// VXRT_NPU_INTERP=extrapolate: no frame is held back. Each frame B is shown as soon as the GPU has it; the Neural
// Engine's optical flow (VideoToolbox, VTOpticalFlow, from a 640-pixel-wide copy of the previous frame A and B) gives
// the motion at B's pixels, and half a frame interval after B a frame predicted from B is shown: B's pixels moved on
// by half their motion (forward warped: each pixel to one place, the fastest wins where several land; a place none
// lands on takes B sampled half a motion back). Pixels equal in A and B (the HUD, subtitles, anything still) stay.
//
// VXRT_NPU_INTERP_SNAPSHOT=dir (VXRT_NPU_INTERP_SNAPSHOT_EVERY=n, default 120): every n-th interpolated frame, with the
// two real frames around it, as PNGs in dir (to compare them).
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOSurface/IOSurface.h>
#import <ImageIO/ImageIO.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <libgen.h>
#include <math.h>
#include <os/lock.h>
#include "npu_interp.h"
#include "npu_proto.h"

extern char **environ;

enum { NSRC = NPU_MAX_SLOTS, NDST = 3, MAXT = NPU_MAX_TILES, MAX_TILE_PX = 1280 * 720, OVERLAP = 32, FLOW_W = 640 };
enum { KIND_REAL, KIND_INTERP, KIND_EXTRAP };

// (shared with the shaders: struct P)
typedef struct { float iw, ih; uint32_t nt, linear; float tile[MAXT][4]; float ramp[MAXT][4]; } npu_params;
// (struct E) the frame's size, frame pixels per flow unit, how far on (0.5: half an interval), the largest difference
// that counts as unchanged, the bits of a splat key that hold the pixel's index
typedef struct { float w, h, sx, sy, k, thr; uint32_t idx_bits, pad; } npu_extrap_params;

static const char *g_npu_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct P { float iw, ih; uint nt, linear; float4 tile[4]; float4 ramp[4]; };\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V npu_vs(uint id [[vertex_id]]) {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    V o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "static float3 to_gamma(float3 c) { c = saturate(c); return select(1.055 * pow(c, 1 / 2.4) - 0.055, c * 12.92, c <= 0.0031308); }\n"
    "static float3 to_linear(float3 c) { return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045); }\n"
    "fragment float4 npu_real_fs(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    return float4(t.sample(s, in.uv).rgb, 1);\n"
    "}\n"
    // BT.709, video range, both ways
    "kernel void npu_to_yuv(texture2d<float> src [[texture(0)]], texture2d<float, access::write> oy [[texture(1)]],\n"
    "                       texture2d<float, access::write> oc [[texture(2)]], constant P &p [[buffer(0)]],\n"
    "                       constant uint &ti [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x * 2 >= oy.get_width() || g.y * 2 >= oy.get_height()) return;\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    float2 o = p.tile[ti].xy;\n"
    "    float3 sum = 0;\n"
    "    for (uint k = 0; k < 4; k++) {\n"
    "        uint2 l = g * 2 + uint2(k & 1, k >> 1);\n"
    "        float3 c = saturate(src.sample(s, (o + float2(l) + 0.5) / float2(p.iw, p.ih), level(0)).rgb);\n"
    "        if (p.linear) c = to_gamma(c);\n"
    "        oy.write(float4(16.0 / 255 + dot(c, float3(0.2126, 0.7152, 0.0722)) * 219.0 / 255), l);\n"
    "        sum += c;\n"
    "    }\n"
    "    float3 c = sum / 4;\n"
    "    float y = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
    "    oc.write(float4(128.0 / 255 + (c.z - y) / 1.8556 * 224.0 / 255, 128.0 / 255 + (c.x - y) / 1.5748 * 224.0 / 255, 0, 0), g);\n"
    "}\n"
    // each tile weighs 1 inside and falls to 0 across its overlap with a neighbour (ramp: left, right, top, bottom)
    "fragment float4 npu_interp_fs(V in [[stage_in]], constant P &p [[buffer(0)]],\n"
    "                              array<texture2d<float>, 4> ty [[texture(0)]], array<texture2d<float>, 4> tc [[texture(4)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    float2 q = in.uv * float2(p.iw, p.ih);\n"
    "    float3 acc = 0; float wsum = 0;\n"
    "    for (uint i = 0; i < p.nt; i++) {\n"
    "        float4 r = p.tile[i], rp = p.ramp[i];\n"
    "        float2 l = q - r.xy;\n"
    "        if (any(l < 0) || any(l > r.zw)) continue;\n"
    "        float w = 1;\n"
    "        if (rp.x > 0) w = min(w, l.x / rp.x);\n"
    "        if (rp.y > 0) w = min(w, (r.z - l.x) / rp.y);\n"
    "        if (rp.z > 0) w = min(w, l.y / rp.z);\n"
    "        if (rp.w > 0) w = min(w, (r.w - l.y) / rp.w);\n"
    "        w = max(w, 1e-4);\n"
    "        float2 uv = l / r.zw;\n"
    "        acc += w * float3(ty[i].sample(s, uv).r, tc[i].sample(s, uv).rg);\n"
    "        wsum += w;\n"
    "    }\n"
    "    float3 v = acc / max(wsum, 1e-6);\n"
    "    float y = (v.x - 16.0 / 255) * (255.0 / 219), cb = (v.y - 128.0 / 255) * (255.0 / 224), cr = (v.z - 128.0 / 255) * (255.0 / 224);\n"
    "    float3 rgb = saturate(float3(y + 1.5748 * cr, y - 0.1873 * cb - 0.4681 * cr, y + 1.8556 * cb));\n"
    "    return float4(p.linear ? to_linear(rgb) : rgb, 1);\n"
    "}\n"
    // extrapolation: the optical flow's input (RGBA half float, a 4-tap box filter), the splat, the predicted frame
    "struct E { float w, h, sx, sy, k, thr; uint idx_bits, pad; };\n"
    "kernel void npu_to_flow(texture2d<float> src [[texture(0)]], texture2d<float, access::write> o [[texture(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x >= o.get_width() || g.y >= o.get_height()) return;\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    float2 sz = float2(o.get_width(), o.get_height()), c = (float2(g) + 0.5) / sz, d = 0.25 / sz;\n"
    "    float3 v = src.sample(s, c - d, level(0)).rgb + src.sample(s, c + d, level(0)).rgb +\n"
    "               src.sample(s, c + float2(d.x, -d.y), level(0)).rgb + src.sample(s, c + float2(-d.x, d.y), level(0)).rgb;\n"
    "    o.write(float4(saturate(v / 4), 1), g);\n"
    "}\n"
    "static float2 motion(texture2d<float> fl, float2 uv, constant E &e) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    return -fl.sample(s, uv, level(0)).xy * float2(e.sx, e.sy);\n"   // (the backward flow points from B to A)
    "}\n"
    // unchanged: no channel of the 3x3 around it differs by more than thr between A and B (a lone pixel that happens
    // to match would tear out of a moving surface)
    "static bool still(texture2d<float> b, texture2d<float> a, uint2 p, constant E &e) {\n"
    "    int2 hi = int2(e.w, e.h) - 1;\n"
    "    for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) {\n"
    "        uint2 q = uint2(clamp(int2(p) + int2(x, y), int2(0), hi));\n"
    "        if (any(abs(b.read(q).rgb - a.read(q).rgb) > e.thr)) return false;\n"
    "    }\n"
    "    return true;\n"
    "}\n"
    "kernel void npu_splat(texture2d<float> b [[texture(0)]], texture2d<float> a [[texture(1)]], texture2d<float> fl [[texture(2)]],\n"
    "                      device atomic_uint *key [[buffer(0)]], constant E &e [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    uint w = uint(e.w), h = uint(e.h);\n"
    "    if (g.x >= w || g.y >= h) return;\n"
    "    float2 m = 0;\n"
    "    if (!still(b, a, g, e)) m = motion(fl, (float2(g) + 0.5) / float2(e.w, e.h), e);\n"
    "    int2 t = int2(round(float2(g) + e.k * m));\n"
    "    if (any(t < 0) || t.x >= int(w) || t.y >= int(h)) return;\n"
    "    uint mq = min(uint(length(m) * 4), (1u << (32 - e.idx_bits)) - 1);\n"
    "    atomic_fetch_max_explicit(&key[uint(t.y) * w + uint(t.x)], (mq << e.idx_bits) | (g.y * w + g.x + 1), memory_order_relaxed);\n"
    "}\n"
    "fragment float4 npu_extrap_fs(V in [[stage_in]], texture2d<float> b [[texture(0)]], texture2d<float> a [[texture(1)]],\n"
    "                              texture2d<float> fl [[texture(2)]], device const uint *key [[buffer(0)]], constant E &e [[buffer(1)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    uint w = uint(e.w);\n"
    "    uint2 p = min(uint2(in.uv * float2(e.w, e.h)), uint2(w - 1, uint(e.h) - 1));\n"
    "    if (still(b, a, p, e)) return float4(b.read(p).rgb, 1);\n"
    // the pixel that landed here decides the motion (the fastest: in front); its colour is B sampled that motion back
    // (bilinear: no stair steps from the rounding of where it landed)
    "    uint k = key[p.y * w + p.x];\n"
    "    if (k) {\n"
    "        uint i = (k & ((1u << e.idx_bits) - 1)) - 1;\n"
    "        uint2 src = uint2(i % w, i / w);\n"
    "        float2 m = still(b, a, src, e) ? float2(0) : motion(fl, (float2(src) + 0.5) / float2(e.w, e.h), e);\n"
    "        return float4(b.sample(s, in.uv - e.k * m / float2(e.w, e.h)).rgb, 1);\n"
    "    }\n"
    "    return float4(b.sample(s, in.uv - e.k * motion(fl, in.uv, e) / float2(e.w, e.h)).rgb, 1);\n"
    "}\n";

// The buffers for one back buffer size: the copies of the frames (RGB, the program's format), the 420v tiles the
// interpolator reads (src) and writes (dst), and that process. A new size makes a new set.
@interface VXNpuSet : NSObject {
@public
    uint32_t w, h, tw, th, nt, mode;
    MTLPixelFormat format;
    npu_params P;
    id<MTLTexture> copy[NSRC];
    id<MTLTexture> sy[NSRC][MAXT], sc[NSRC][MAXT], dy[NDST][MAXT], dc[NDST][MAXT];
    // extrapolation: the flow's input frames, its forward and backward flows, the splat's keys; the frames drawn
    id<MTLTexture> fin[NSRC], ffwd[NDST], fbwd[NDST];
    id<MTLBuffer> keys;
    npu_extrap_params E;
    uint32_t ex_prev, ex_dst;
    IOSurfaceRef surfaces[2 * NSRC * MAXT];
    int sock;
    pid_t pid;
    volatile int ready;            // the helper's sessions run and the 420v textures exist
}
@end
@implementation VXNpuSet
- (void)dealloc {
    for (int i = 0; i < 2 * NSRC * MAXT; i++) if (surfaces[i]) CFRelease(surfaces[i]);
    if (sock >= 0) close(sock);    // (the helper ends when its socket closes)
}
@end

static struct {
    int state;                     // 0 not decided, 1 on, -1 off
    int mode;                      // NPU_MODE_INTERPOLATE, or NPU_MODE_FLOW (extrapolation); a set keeps the one it began with
    int interp_ok, flow_ok;        // what VideoToolbox supports here
    char helper[1024];
    os_unfair_lock lock;           // set
    VXNpuSet *set;
    __unsafe_unretained CAMetalLayer *layer;
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;     // the display's
    id<MTLRenderPipelineState> real_ps, interp_ps, real_png_ps, interp_png_ps, extrap_ps, extrap_png_ps;
    id<MTLComputePipelineState> yuv_ps, flow_in_ps, splat_ps;
    dispatch_queue_t q, init_q;
    uint32_t next_slot;            // (the presenting thread's)
    volatile int pending;          // frames committed and not yet shown
    // the display queue's
    VXNpuSet *shown_set;
    int prev;                      // the previous frame's slot, -1 for none
    int prev_yuv;
    double prev_t, interval, last_at;
    double lag;                    // from an immediate present to the frame on screen (measured; at first 1/60 s)
    uint32_t seq, next_dst;
    int busy;                      // a request that missed its time is still running
    // statistics (every 5 s)
    double stat_t0;
    uint32_t frames, real, interp, missed, skipped, dropped, failed;
    volatile int presented;
    float ms[512], late[512], gpu_ms[512], ks[512];
    uint32_t nms, nlate, nk;
    volatile uint32_t ngpu;
    const char *snap_dir;
    int snap_every;
} g = { .lock = OS_UNFAIR_LOCK_INIT, .prev = -1, .lag = 1.0 / 60 };

static double now_s(void) { return CACurrentMediaTime(); }

// VXRT_NPU_INTERP_SWITCH=file: on only while that file exists (looked at twice a second), to compare in one scene;
// a file that starts with "extrapolate" or "interpolate" also picks the mode
static int switched_on(void) {
    static const char *path;
    static double next;
    static int on = 1;
    if (!path && !(path = getenv("VXRT_NPU_INTERP_SWITCH"))) path = "";
    if (!*path) return 1;
    double t = CACurrentMediaTime();
    if (t >= next) {
        next = t + 0.5;
        int now = 0, mode = g.mode;
        FILE *f = fopen(path, "r");
        if (f) {
            char word[32] = "";
            now = 1;
            if (fscanf(f, "%31s", word) == 1) {
                if (!strcmp(word, "extrapolate") && g.flow_ok) mode = NPU_MODE_FLOW;
                if (!strcmp(word, "interpolate") && g.interp_ok) mode = NPU_MODE_INTERPOLATE;
            }
            fclose(f);
        }
        if (now != on || mode != g.mode)
            vx_log("NPU mode: %s (%s)", now ? (mode == NPU_MODE_FLOW ? "extrapolate" : "interpolate") : "switched off", path);
        on = now;
        g.mode = mode;
    }
    return on;
}

int npu_interp_on(void *layer) {
    if (g.state) return g.state > 0 && switched_on();
    const char *e = getenv("VXRT_NPU_INTERP");
    if (!e || !*e || !strcmp(e, "0")) { g.state = -1; return 0; }
    g.mode = !strcmp(e, "extrapolate") ? NPU_MODE_FLOW : NPU_MODE_INTERPOLATE;
    char exe[1024]; uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n)) { g.state = -1; return 0; }
    snprintf(g.helper, sizeof g.helper, "%s/vxrt-npu", dirname(exe));
    for (int k = 0; k < 2; k++) {
        pid_t pid; int status = -1;
        char *argv[] = { g.helper, "--check", k ? "flow" : NULL, NULL };
        if (posix_spawn(&pid, g.helper, NULL, NULL, argv, environ) == 0) waitpid(pid, &status, 0);
        if (k) g.flow_ok = status == 0; else g.interp_ok = status == 0;
    }
    if (!(g.mode == NPU_MODE_FLOW ? g.flow_ok : g.interp_ok)) {
        vx_log("NPU mode: off (%s: %s)", g.helper, access(g.helper, X_OK) ? "not found" : "VideoToolbox's frame interpolation or optical flow is not supported on this Mac (macOS 26 and Apple silicon needed)");
        g.state = -1;
        return 0;
    }
    g.state = 1;
    g.snap_dir = getenv("VXRT_NPU_INTERP_SNAPSHOT");
    g.snap_every = getenv("VXRT_NPU_INTERP_SNAPSHOT_EVERY") ? atoi(getenv("VXRT_NPU_INTERP_SNAPSHOT_EVERY")) : 120;
    if (g.snap_every < 1) g.snap_every = 120;
    vx_log(g.mode == NPU_MODE_FLOW ? "NPU mode: on (extrapolate): each frame is shown at once, then one predicted from it with the Neural Engine's optical flow (VideoToolbox)"
                                   : "NPU mode: on (interpolate): each frame shown in the window is followed by one the Neural Engine interpolates (VideoToolbox)");
    return switched_on();
}

// The interpolation grid: the fewest tiles (up to 2 x 2, overlapping by OVERLAP pixels, width a multiple of 16) that
// hold w x h; a picture larger than four tiles is interpolated scaled down.
static void layout(VXNpuSet *S) {
    static const uint32_t grids[][2] = { { 1, 1 }, { 2, 1 }, { 1, 2 }, { 2, 2 } };
    if (S->mode == NPU_MODE_FLOW) {                                 // (the flow's input: 640 wide, or the frame's width)
        S->tw = S->w < FLOW_W ? (S->w + 15) & ~15u : FLOW_W;
        S->th = ((uint32_t)((float)S->h * S->tw / S->w) + 1) & ~1u;
        S->nt = 1;
        return;
    }
    float scale = 1;
    for (int pass = 0; pass < 2; pass++) {
        float iw = floorf(S->w * scale), ih = floorf(S->h * scale);
        for (int k = 0; k < 4; k++) {
            uint32_t nx = grids[k][0], ny = grids[k][1];
            uint32_t tw = ((uint32_t)ceilf((iw + OVERLAP * (nx - 1)) / nx) + 15) & ~15u;
            uint32_t th = ((uint32_t)ceilf((ih + OVERLAP * (ny - 1)) / ny) + 15) & ~15u;
            if (tw < 64) tw = 64;
            if (th < 64) th = 64;
            if (tw * th > MAX_TILE_PX) continue;
            S->tw = tw; S->th = th; S->nt = nx * ny;
            S->P.iw = iw; S->P.ih = ih;
            float x0[2], y0[2];
            for (uint32_t i = 0; i < nx; i++) x0[i] = nx > 1 ? roundf(i * (iw - tw) / (nx - 1)) : 0;
            for (uint32_t j = 0; j < ny; j++) y0[j] = ny > 1 ? roundf(j * (ih - th) / (ny - 1)) : 0;
            float ox = nx > 1 ? x0[0] + tw - x0[1] : 0, oy = ny > 1 ? y0[0] + th - y0[1] : 0;   // the overlaps
            for (uint32_t j = 0; j < ny; j++)
                for (uint32_t i = 0; i < nx; i++) {
                    uint32_t t = j * nx + i;
                    S->P.tile[t][0] = x0[i]; S->P.tile[t][1] = y0[j]; S->P.tile[t][2] = tw; S->P.tile[t][3] = th;
                    S->P.ramp[t][0] = i > 0 ? ox : 0; S->P.ramp[t][1] = i + 1 < nx ? ox : 0;
                    S->P.ramp[t][2] = j > 0 ? oy : 0; S->P.ramp[t][3] = j + 1 < ny ? oy : 0;
                }
            S->P.nt = S->nt;
            return;
        }
        scale = sqrtf(4.0f * (MAX_TILE_PX * 0.85f) / ((float)S->w * S->h));
    }
}

static int read_all(int fd, void *p, size_t n) {
    for (size_t got = 0; got < n;) {
        ssize_t r = read(fd, (char *)p + got, n - got);
        if (r <= 0) return 0;
        got += (size_t)r;
    }
    return 1;
}

// (the init queue) textures on the flow's buffers, and the splat's keys
static void flow_textures(VXNpuSet *S, const npu_init_reply *r) {
    uint32_t n = NSRC + 2 * NDST;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    for (uint32_t k = 0; k < n; k++)
        if (!(S->surfaces[k] = IOSurfaceLookup(r->ids[k]))) { vx_log("NPU mode: buffer %u not found", r->ids[k]); return; }
#pragma clang diagnostic pop
    for (uint32_t k = 0; k < n; k++) {
        int in = k < NSRC;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:in ? MTLPixelFormatRGBA16Float : MTLPixelFormatRG16Float
                                                                                      width:in ? S->tw : r->flow_w height:in ? S->th : r->flow_h mipmapped:NO];
        td.usage = in ? MTLTextureUsageShaderWrite : MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> t = [g.dev newTextureWithDescriptor:td iosurface:S->surfaces[k] plane:0];
        if (!t) { vx_log("NPU mode: no texture on the optical flow's buffers"); return; }
        if (in) S->fin[k] = t;
        else if (k < NSRC + NDST) S->ffwd[k - NSRC] = t;
        else S->fbwd[k - NSRC - NDST] = t;
    }
    uint32_t px = S->w * S->h, bits = 1;
    while (bits < 24 && (1u << bits) <= px) bits++;
    S->keys = [g.dev newBufferWithLength:(NSUInteger)px * 4 options:MTLResourceStorageModePrivate];
    if (!S->keys || (1u << bits) <= px) { vx_log("NPU mode: frame too large to extrapolate"); return; }
    S->E = (npu_extrap_params){ (float)S->w, (float)S->h, r->flow_scale * S->w / S->tw, r->flow_scale * S->h / S->th, 0.5f, 2.0f / 255, bits, 0 };
    vx_log("NPU mode: %ux%u extrapolated from optical flow of %ux%u frames (%ux%u vectors, %.2f pixels a unit; started in %.0f ms)",
           S->w, S->h, S->tw, S->th, r->flow_w, r->flow_h, r->flow_scale, r->start_ms);
    __atomic_store_n(&S->ready, 1, __ATOMIC_RELEASE);
}

// (the init queue) the helper process, its sessions, and textures on its buffers' planes
static void set_start(VXNpuSet *S) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return;
    int one = 1;
    setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);   // (none of the program's descriptors)
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addinherit_np(&fa, 1);
    posix_spawn_file_actions_addinherit_np(&fa, 2);
    posix_spawn_file_actions_adddup2(&fa, sv[1], NPU_FD);
    char *argv[] = { g.helper, NULL };
    int rc = posix_spawn(&S->pid, g.helper, &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(sv[1]);
    if (rc) { close(sv[0]); vx_log("NPU mode: %s did not start (%d)", g.helper, rc); return; }
    S->sock = sv[0];
    npu_init_req q = { NPU_OP_INIT, S->tw, S->th, S->nt, NSRC, NDST, S->mode };
    npu_init_reply r = { .status = -1 };
    if (write(S->sock, &q, sizeof q) != sizeof q || !read_all(S->sock, &r, sizeof r) || r.status) {
        vx_log("NPU mode: the interpolator did not start (%d); the frames are shown as they are", r.status);
        return;
    }
    if (S->mode == NPU_MODE_FLOW) { flow_textures(S, &r); return; }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    for (uint32_t k = 0; k < (NSRC + NDST) * S->nt; k++)
        if (!(S->surfaces[k] = IOSurfaceLookup(r.ids[k]))) { vx_log("NPU mode: buffer %u not found", r.ids[k]); return; }
#pragma clang diagnostic pop
    for (uint32_t k = 0; k < (NSRC + NDST) * S->nt; k++) {
        int dst = k >= NSRC * S->nt;
        uint32_t slot = (dst ? k - NSRC * S->nt : k) / S->nt, t = k % S->nt;
        MTLTextureDescriptor *yd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:S->tw height:S->th mipmapped:NO];
        MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:S->tw / 2 height:S->th / 2 mipmapped:NO];
        yd.usage = cd.usage = dst ? MTLTextureUsageShaderRead : MTLTextureUsageShaderWrite;
        yd.storageMode = cd.storageMode = MTLStorageModeShared;
        id<MTLTexture> y = [g.dev newTextureWithDescriptor:yd iosurface:S->surfaces[k] plane:0];
        id<MTLTexture> c = [g.dev newTextureWithDescriptor:cd iosurface:S->surfaces[k] plane:1];
        if (!y || !c) { vx_log("NPU mode: no texture on the interpolator's buffers"); return; }
        if (dst) { S->dy[slot][t] = y; S->dc[slot][t] = c; }
        else { S->sy[slot][t] = y; S->sc[slot][t] = c; }
    }
    vx_log("NPU mode: %ux%u interpolated in %u tile%s of %ux%u%s (sessions started in %.0f ms)", S->w, S->h, S->nt, S->nt > 1 ? "s" : "",
           S->tw, S->th, S->P.iw < S->w ? " (scaled down)" : "", r.start_ms);
    __atomic_store_n(&S->ready, 1, __ATOMIC_RELEASE);
}

static int setup(CAMetalLayer *layer, id<MTLDevice> dev) {
    if (g.dev) return 1;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(g_npu_msl) options:nil error:&err];
    if (!lib) { vx_log("NPU mode: the shaders did not compile: %s", err.localizedDescription.UTF8String); g.state = -1; return 0; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"npu_vs"];
    pd.colorAttachments[0].pixelFormat = layer.pixelFormat;
    pd.fragmentFunction = [lib newFunctionWithName:@"npu_real_fs"];
    g.real_ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.fragmentFunction = [lib newFunctionWithName:@"npu_interp_fs"];
    g.interp_ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;   // (for the snapshots)
    g.interp_png_ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.fragmentFunction = [lib newFunctionWithName:@"npu_real_fs"];
    g.real_png_ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.fragmentFunction = [lib newFunctionWithName:@"npu_extrap_fs"];
    g.extrap_png_ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.colorAttachments[0].pixelFormat = layer.pixelFormat;
    g.extrap_ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    g.flow_in_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_to_flow"] error:&err];
    g.splat_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_splat"] error:&err];
    g.yuv_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_to_yuv"] error:&err];
    if (!g.real_ps || !g.interp_ps || !g.yuv_ps || !g.extrap_ps || !g.flow_in_ps || !g.splat_ps) { vx_log("NPU mode: no pipeline: %s", err.localizedDescription.UTF8String); g.state = -1; return 0; }
    g.queue = [dev newCommandQueue];
    g.q = dispatch_queue_create("vxrt.npu.display", DISPATCH_QUEUE_SERIAL);
    g.init_q = dispatch_queue_create("vxrt.npu.init", DISPATCH_QUEUE_SERIAL);
    g.dev = dev;
    return 1;
}

static void draw(id<MTLCommandBuffer> cb, id<MTLTexture> target, int kind, VXNpuSet *S, uint32_t slot, int png) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (kind == KIND_EXTRAP) {                                     // (after splat(): S->ex_prev, S->ex_dst)
        [enc setRenderPipelineState:png ? g.extrap_png_ps : g.extrap_ps];
        [enc setFragmentTexture:S->copy[slot] atIndex:0];
        [enc setFragmentTexture:S->copy[S->ex_prev] atIndex:1];
        [enc setFragmentTexture:S->fbwd[S->ex_dst] atIndex:2];
        [enc setFragmentBuffer:S->keys offset:0 atIndex:0];
        [enc setFragmentBytes:&S->E length:sizeof S->E atIndex:1];
    } else if (kind == KIND_INTERP) {
        [enc setRenderPipelineState:png ? g.interp_png_ps : g.interp_ps];
        npu_params P = S->P;
        P.linear = !png && (g.layer.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB || g.layer.pixelFormat == MTLPixelFormatRGBA16Float);
        [enc setFragmentBytes:&P length:sizeof P atIndex:0];
        for (uint32_t t = 0; t < MAXT; t++) {
            uint32_t u = t < S->nt ? t : 0;
            [enc setFragmentTexture:S->dy[slot][u] atIndex:t];
            [enc setFragmentTexture:S->dc[slot][u] atIndex:4 + t];
        }
    } else {
        [enc setRenderPipelineState:png ? g.real_png_ps : g.real_ps];
        [enc setFragmentTexture:S->copy[slot] atIndex:0];
    }
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [enc endEncoding];
}

// VXRT_NPU_INTERP_RECORD=dir: while the file dir/rec exists, every frame shown (at half size) as dir/fNNNNN.bgra
// (raw BGRA) and dir/fNNNNN.txt (kind, the time asked for, the time it was on screen), to make a video of what the
// window showed without a screen recording. Up to VXRT_NPU_INTERP_RECORD_FRAMES (default 240) frames, then dir/rec goes.
static const char *const kind_names[] = { "real", "interp", "extrap" };

static FILE *record(id<MTLCommandBuffer> cb, int kind, VXNpuSet *S, uint32_t slot, double at) {
    static const char *dir;
    static int max = -1, n;
    static double next_check;
    static int on;
    if (max < 0) { dir = getenv("VXRT_NPU_INTERP_RECORD"); max = getenv("VXRT_NPU_INTERP_RECORD_FRAMES") ? atoi(getenv("VXRT_NPU_INTERP_RECORD_FRAMES")) : 240; }
    if (!dir || !g.interp_png_ps) return NULL;
    char path[1200];
    snprintf(path, sizeof path, "%s/rec", dir);
    if (now_s() >= next_check) { next_check = now_s() + 0.25; on = access(path, F_OK) == 0; }
    if (!on) return NULL;
    if (n >= max) { unlink(path); on = 0; n = 0; vx_log("NPU mode: recorded %d frames in %s", max, dir); return NULL; }
    uint32_t w = S->w / 2, h = S->h / 2, k = (uint32_t)n++;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [g.dev newTextureWithDescriptor:td];
    id<MTLBuffer> out = [g.dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
    if (!target || !out) return NULL;
    draw(cb, target, kind, S, slot, 1);
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:out destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [blit endEncoding];
    char raw[1200];
    snprintf(raw, sizeof raw, "%s/f%05u_%ux%u.bgra", dir, k, w, h);
    NSString *rawpath = @(raw);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        FILE *f = fopen(rawpath.UTF8String, "wb");
        if (f) { fwrite(out.contents, 1, out.length, f); fclose(f); }
    }];
    snprintf(path, sizeof path, "%s/f%05u.txt", dir, k);
    FILE *f = fopen(path, "w");
    if (f) fprintf(f, "%s %.6f %.6f", kind_names[kind], now_s(), at);
    return f;
}

// (the display queue) a frame into a drawable, presented at `at` (0: at once)
// (the extrapolated frame's first pass, into S->keys: where each of B's pixels lands)
static void splat(id<MTLCommandBuffer> cb, VXNpuSet *S, uint32_t slot) {
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:S->keys range:NSMakeRange(0, S->keys.length) value:0];
    [blit endEncoding];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:g.splat_ps];
    [enc setTexture:S->copy[slot] atIndex:0];
    [enc setTexture:S->copy[S->ex_prev] atIndex:1];
    [enc setTexture:S->fbwd[S->ex_dst] atIndex:2];
    [enc setBuffer:S->keys offset:0 atIndex:0];
    [enc setBytes:&S->E length:sizeof S->E atIndex:1];
    [enc dispatchThreads:MTLSizeMake(S->w, S->h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];
}

static void show(int kind, VXNpuSet *S, uint32_t slot, double at) {
    @autoreleasepool {
        id<CAMetalDrawable> d = [g.layer nextDrawable];
        if (!d) return;
        id<MTLCommandBuffer> cb = [g.queue commandBuffer];
        if (kind == KIND_EXTRAP) {
            splat(cb, S, slot);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {      // (the GPU's time for the splat and the frame)
                uint32_t i = __atomic_fetch_add(&g.ngpu, 1, __ATOMIC_RELAXED);
                if (i < 512) g.gpu_ms[i] = (float)((done.GPUEndTime - done.GPUStartTime) * 1000);
            }];
        }
        draw(cb, d.texture, kind, S, slot, 0);
        FILE *rec = record(cb, kind, S, slot, at);
        double asked = now_s();
        int immediate = !(at > asked + 0.001);
        [d addPresentedHandler:^(id<MTLDrawable> done) {
            if (done.presentedTime > 0) __atomic_add_fetch(&g.presented, 1, __ATOMIC_RELAXED);
            if (immediate && done.presentedTime > asked && done.presentedTime < asked + 0.1)
                g.lag = g.lag * 0.9 + (done.presentedTime - asked) * 0.1;   // (a benign race: one writer)
            if (rec) { fprintf(rec, " %.6f\n", done.presentedTime); fclose(rec); }
        }];
        if (at > now_s() + 0.001) [cb presentDrawable:d atTime:at];
        else [cb presentDrawable:d];
        [cb commit];
        double t = immediate ? asked + g.lag : at;                  // (when it will be on screen)
        if (t > g.last_at) g.last_at = t;
        if (kind != KIND_REAL) g.interp++; else g.real++;
    }
}

// VXRT_NPU_INTERP_SNAPSHOT: the real frames before and after an interpolated one and that one (extrapolated: the two
// real frames it comes from, then it), as PNGs
static void snapshot(VXNpuSet *S, uint32_t prev, uint32_t dst, uint32_t cur) {
    static uint32_t n;
    if (!g.snap_dir || !g.interp_png_ps || ++n % (uint32_t)g.snap_every) return;
    @autoreleasepool {
        uint32_t w = S->w, h = S->h;
        int ex = S->mode == NPU_MODE_FLOW;
        const char *names[3] = { "a_real", ex ? "b_real" : "b_interp", ex ? "c_extrap" : "c_real" };
        for (int k = 0; k < 3; k++) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> target = [g.dev newTextureWithDescriptor:td];
            id<MTLBuffer> out = [g.dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [g.queue commandBuffer];
            if (ex) { if (k == 2) splat(cb, S, cur); draw(cb, target, k == 2 ? KIND_EXTRAP : KIND_REAL, S, k == 0 ? prev : cur, 1); }
            else draw(cb, target, k == 1 ? KIND_INTERP : KIND_REAL, S, k == 0 ? prev : k == 1 ? dst : cur, 1);
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                         toBuffer:out destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
            [blit endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef ctx = CGBitmapContextCreate(out.contents, w, h, 8, w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
            CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
            char path[1200];
            snprintf(path, sizeof path, "%s/npu_%05u_%s.png", g.snap_dir, n, names[k]);
            CGImageDestinationRef f = img ? CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(path)], CFSTR("public.png"), 1, NULL) : NULL;
            if (f) { CGImageDestinationAddImage(f, img, NULL); CGImageDestinationFinalize(f); CFRelease(f); }
            if (img) CGImageRelease(img);
            if (ctx) CGContextRelease(ctx);
            CGColorSpaceRelease(cs);
        }
    }
}

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static float pct(float *v, uint32_t n, float p) { if (!n) return 0; qsort(v, n, sizeof *v, cmpf); return v[(uint32_t)((n - 1) * p + 0.5f)]; }

static void stats(double t) {
    if (!g.stat_t0) { g.stat_t0 = t; return; }
    double dt = t - g.stat_t0;
    if (dt < 5) return;
    int presented = __atomic_exchange_n(&g.presented, 0, __ATOMIC_RELAXED);
    uint32_t ngpu = __atomic_exchange_n(&g.ngpu, 0, __ATOMIC_RELAXED);
    if (ngpu > 512) ngpu = 512;
    if (dt <= 6) {                                                 // (longer: the mode was off meanwhile)
        int ex = g.shown_set && g.shown_set->mode == NPU_MODE_FLOW;
        vx_log("NPU mode: game %.1f fps, window %.1f fps (%u real + %u %s in %.1f s, %.1f fps on screen); "
               "%s %.1f ms median, %.1f ms p95%s; real frames on screen %.1f ms after they were ready (median, %.1f ms p95; "
               "an immediate present takes %.1f ms); "
               "%u late, %u skipped, %u failed, %u frames dropped",
               g.frames / dt, (g.real + g.interp) / dt, g.real, g.interp, ex ? "extrapolated" : "interpolated", dt, presented / dt,
               ex ? "optical flow" : "interpolation", pct(g.ms, g.nms, 0.5f), pct(g.ms, g.nms, 0.95f),
               ex ? [NSString stringWithFormat:@", warp on the GPU %.1f ms median, predicted %.2f of an interval on (median)",
                                               pct(g.gpu_ms, ngpu, 0.5f), pct(g.ks, g.nk, 0.5f)].UTF8String : "",
               pct(g.late, g.nlate, 0.5f), pct(g.late, g.nlate, 0.95f), g.lag * 1000, g.missed, g.skipped, g.failed, g.dropped);
    }
    g.stat_t0 = t;
    g.frames = g.real = g.interp = g.missed = g.skipped = g.failed = g.dropped = 0;
    g.nms = g.nlate = g.nk = 0;
}

static void handle_extrap(VXNpuSet *S, uint32_t slot, int has_in, double t);

// (the display queue) a frame the GPU has copied (and converted, when has_yuv)
static void handle(VXNpuSet *S, uint32_t slot, int has_yuv, double t) {
    if (S->mode == NPU_MODE_FLOW) { handle_extrap(S, slot, has_yuv, t); return; }
    g.frames++;
    if (S != g.shown_set) { g.shown_set = S; g.prev = -1; g.busy = 0; g.interval = 0; }
    if (g.prev >= 0) {
        double dt = t - g.prev_t;
        if (dt > 0.002 && dt < 0.25) g.interval = g.interval ? g.interval * 0.8 + dt * 0.2 : dt;
        else if (dt >= 0.25) g.prev = -1;                          // (a pause, or the mode was off: no midpoint to make)
    }
    if (g.busy) {                                                  // the late request's reply, if it came
        struct pollfd pf = { S->sock, POLLIN, 0 };
        npu_proc_reply r;
        if (poll(&pf, 1, 0) == 1 && read_all(S->sock, &r, sizeof r)) g.busy = 0;
    }
    int newer = __atomic_load_n(&g.pending, __ATOMIC_ACQUIRE) > 1;
    int shown = 0;
    if (has_yuv && g.prev >= 0 && g.prev_yuv && g.interval > 0 && !newer && !g.busy) {
        uint32_t dst = g.next_dst++ % NDST;
        npu_proc_req q = { NPU_OP_PROC, ++g.seq, (uint32_t)g.prev, slot, dst };
        double half = g.interval / 2, t0 = now_s();
        if (write(S->sock, &q, sizeof q) == sizeof q) {
            // (the reply in time: before half an interval has gone since the frame was ready)
            int wait_ms = (int)((t + half - t0) * 1000);
            struct pollfd pf = { S->sock, POLLIN, 0 };
            npu_proc_reply r = { 0, -1, 0 };
            if (wait_ms > 0 && poll(&pf, 1, wait_ms) == 1 && read_all(S->sock, &r, sizeof r)) {
                if (r.status == 0) {
                    if (g.nms < 512) g.ms[g.nms++] = (float)((now_s() - t0) * 1000);
                    // (both at set times, the first as soon as a present can be on screen: even steps)
                    double soon = now_s() + g.lag;
                    double at = g.last_at + half * 0.75 > soon ? g.last_at + half * 0.75 : soon;
                    show(KIND_INTERP, S, dst, at);
                    show(KIND_REAL, S, slot, at + half);
                    if (g.nlate < 512) g.late[g.nlate++] = (float)((at + half - t) * 1000);
                    snapshot(S, (uint32_t)g.prev, dst, slot);
                    shown = 1;
                } else g.failed++;
            } else { g.missed++; g.busy = 1; }
        } else g.failed++;
    } else if (has_yuv && g.prev >= 0) g.skipped++;
    if (!shown) {
        double now = now_s();
        double at = g.last_at + 1.0 / 240 > now ? g.last_at + 1.0 / 240 : now;
        show(KIND_REAL, S, slot, at);
        double on_screen = at > now ? at : now + g.lag;
        if (g.nlate < 512) g.late[g.nlate++] = (float)((on_screen - t) * 1000);
    }
    g.prev = (int)slot; g.prev_yuv = has_yuv; g.prev_t = t;
    __atomic_sub_fetch(&g.pending, 1, __ATOMIC_RELEASE);
    stats(t);
}

// (the display queue) extrapolation: B (slot) at once; the flow from A (g.prev) to B, and in time for half an interval
// after B, the frame predicted from them
static void handle_extrap(VXNpuSet *S, uint32_t slot, int has_in, double t) {
    g.frames++;
    if (S != g.shown_set) { g.shown_set = S; g.prev = -1; g.busy = 0; g.interval = 0; }
    if (g.prev >= 0) {
        double dt = t - g.prev_t;
        if (dt > 0.002 && dt < 0.25) g.interval = g.interval ? g.interval * 0.8 + dt * 0.2 : dt;
        else if (dt >= 0.25) g.prev = -1;                          // (a pause, or the mode was off: no motion to go on)
    }
    if (g.busy) {
        struct pollfd pf = { S->sock, POLLIN, 0 };
        npu_proc_reply r;
        if (poll(&pf, 1, 0) == 1 && read_all(S->sock, &r, sizeof r)) g.busy = 0;
    }
    double now = now_s();
    double at_b = g.last_at + 1.0 / 240 > now ? g.last_at + 1.0 / 240 : now;
    show(KIND_REAL, S, slot, at_b);
    if (at_b <= now) at_b = now + g.lag;                           // (when B is on screen)
    if (g.nlate < 512) g.late[g.nlate++] = (float)((at_b - t) * 1000);
    int newer = __atomic_load_n(&g.pending, __ATOMIC_ACQUIRE) > 1;
    if (has_in && g.prev >= 0 && g.prev_yuv && g.interval > 0 && !newer && !g.busy) {
        uint32_t dst = g.next_dst++ % NDST;
        npu_proc_req q = { NPU_OP_PROC, ++g.seq, (uint32_t)g.prev, slot, dst };
        double half = g.interval / 2, t0 = now_s();
        if (write(S->sock, &q, sizeof q) == sizeof q) {
            // The predicted frame is due half an interval after B; a flow that comes later (it takes ~20 ms, more than
            // half of a 30 fps interval) still makes a frame, predicted as far on as it will be shown, up to 0.8 of an
            // interval, so the motion stays right where the pacing is not even.
            int wait_ms = (int)((at_b + g.interval * 0.8 - 0.004 - t0) * 1000);
            struct pollfd pf = { S->sock, POLLIN, 0 };
            npu_proc_reply r = { 0, -1, 0 };
            if (wait_ms > 0 && poll(&pf, 1, wait_ms) == 1 && read_all(S->sock, &r, sizeof r)) {
                if (r.status == 0) {
                    double done = now_s(), at = at_b + half > done + 0.004 ? at_b + half : done + 0.004;
                    if (g.nms < 512) g.ms[g.nms++] = (float)((done - t0) * 1000);
                    S->ex_prev = (uint32_t)g.prev; S->ex_dst = dst;
                    S->E.k = (float)((at - at_b) / g.interval);
                    show(KIND_EXTRAP, S, slot, at);
                    if (g.nk < 512) g.ks[g.nk++] = S->E.k;
                    snapshot(S, (uint32_t)g.prev, dst, slot);
                } else g.failed++;
            } else { g.missed++; g.busy = 1; }
        } else g.failed++;
    } else if (has_in && g.prev >= 0) g.skipped++;
    g.prev = (int)slot; g.prev_yuv = has_in; g.prev_t = t;
    __atomic_sub_fetch(&g.pending, 1, __ATOMIC_RELEASE);
    stats(t);
}

void npu_interp_frame(void *layer_, void *cb_, void *texture, uint32_t w, uint32_t h, HANDLE waitable) {
    CAMetalLayer *layer = (__bridge CAMetalLayer *)layer_;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cb_;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    if (!g.dev && !setup(layer, cb.device)) {
        if (waitable) [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { (void)done; vx_release_semaphore(waitable, 1); }];
        [cb commit];
        return;
    }
    if (w > tex.width) w = (uint32_t)tex.width;
    if (h > tex.height) h = (uint32_t)tex.height;
    layer.displaySyncEnabled = YES;
    VXNpuSet *S;
    os_unfair_lock_lock(&g.lock);
    S = g.set;
    os_unfair_lock_unlock(&g.lock);
    if (!S || S->w != w || S->h != h || S->format != tex.pixelFormat || g.layer != layer || S->mode != (uint32_t)g.mode) {
        S = [VXNpuSet new];
        S->w = w; S->h = h; S->format = tex.pixelFormat; S->sock = -1; S->mode = (uint32_t)g.mode;
        layout(S);
        for (int i = 0; i < NSRC; i++) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:tex.pixelFormat width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            S->copy[i] = [g.dev newTextureWithDescriptor:td];
        }
        os_unfair_lock_lock(&g.lock);
        g.set = S;
        g.layer = layer;
        os_unfair_lock_unlock(&g.lock);
        VXNpuSet *s = S;
        dispatch_async(g.init_q, ^{ set_start(s); });
    }
    // (more frames waiting than the slots allow: this one is not shown)
    if (__atomic_load_n(&g.pending, __ATOMIC_ACQUIRE) >= NSRC - 1) {
        dispatch_async(g.q, ^{ g.dropped++; });
        if (waitable) [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { (void)done; vx_release_semaphore(waitable, 1); }];
        [cb commit];
        return;
    }
    uint32_t slot = g.next_slot++ % NSRC;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                toTexture:S->copy[slot] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    int has_yuv = __atomic_load_n(&S->ready, __ATOMIC_ACQUIRE);
    if (has_yuv && S->mode == NPU_MODE_FLOW) {                      // (the flow's input)
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:g.flow_in_ps];
        [enc setTexture:S->copy[slot] atIndex:0];
        [enc setTexture:S->fin[slot] atIndex:1];
        [enc dispatchThreads:MTLSizeMake(S->tw, S->th, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [enc endEncoding];
    } else if (has_yuv) {
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:g.yuv_ps];
        npu_params P = S->P;
        P.linear = layer.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB || layer.pixelFormat == MTLPixelFormatRGBA16Float;
        [enc setBytes:&P length:sizeof P atIndex:0];
        [enc setTexture:S->copy[slot] atIndex:0];
        for (uint32_t t = 0; t < S->nt; t++) {
            [enc setBytes:&t length:sizeof t atIndex:1];
            [enc setTexture:S->sy[slot][t] atIndex:1];
            [enc setTexture:S->sc[slot][t] atIndex:2];
            [enc dispatchThreads:MTLSizeMake(S->tw / 2, S->th / 2, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        }
        [enc endEncoding];
    }
    __atomic_add_fetch(&g.pending, 1, __ATOMIC_ACQ_REL);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        if (waitable) vx_release_semaphore(waitable, 1);
        double t = now_s();
        dispatch_async(g.q, ^{ handle(S, slot, has_yuv, t); });
    }];
    [cb commit];
}
