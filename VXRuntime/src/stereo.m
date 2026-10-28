// Stereo 3D from flat games: depth-image-based rendering (DIBR) at present time. See stereo.h.
//
// VXRT_STEREO=1 turns it on (off: nothing here runs). Knobs, read once:
//   VXRT_STEREO_SEP=2.0      disparity at infinity between the eyes, % of the width (2 % of a 3 m screen ~ the IPD)
//   VXRT_STEREO_POP=0.4      the largest pop-out (in front of the screen), as a fraction of SEP
//   VXRT_STEREO_CONV=1.0     the screen plane's distance, relative to the median depth of the picture's centre
//   VXRT_STEREO_WINDOW=sbs   what the Mac window shows: sbs (both eyes squeezed side by side), anaglyph (red/cyan),
//                            left, right, flat (the game's picture)
//   VXRT_STEREO_STREAM=<file>|1   both eyes side by side (full width each, BGRA8) into the Mac encoder's shared frame
//                            file (1: $VERANTYX_SHARED_FRAME or ~/Verantyx_VR_Drive/vr_stereo_frame.dat), with
//                            captureSourceMode 3 ("a flat game in stereo": the headset shows it on a virtual screen)
//   VXRT_STEREO_SHOT=<dir>   writing a count N to <dir>/now: the next N frames' eyes as <dir>/<n>-sbs.png and
//                            <n>-anaglyph.png, a line each in <dir>/shots.txt; while <dir>/pause exists (looked
//                            for every 30 frames) the pass is off as with VXRT_STEREO=0 (a before/after in one run)
//   VXRT_STEREO_STATS=300    every so many frames, the stereo pass's GPU time in the log
//
// The scene's depth: among the depth attachments the frame's render passes drew into with depth testing, those of
// the back buffer's aspect (shadow maps, cube faces, a mirror's reflection view drop out), the one with the most
// depth-tested draws. Which one that is is decided at each present for the next frame; that texture is copied at the
// end of every pass that wrote it (a placed resource may be aliased by something else later in the frame), into a
// buffer the stereo pass reads at present (GPU-ordered on the queue). A frame in which it was not written (a loading
// screen, a menu) is shown flat: both eyes the game's picture.
//
// Disparity without the projection matrix: with reversed Z and an infinite far plane d = n/z, so inverse depth is
// d itself (standard Z: 1 - d, ~n/z for far >> near), and disparity(z) = S * (1 - z_c/z) = S * (1 - d/d_c), d_c the
// inverse depth on the screen plane (the median of the centre, smoothed over frames). Far: +S (the right eye's
// image to the right: behind the screen), clamped to -POP*S in front.
//
// The pass (one command buffer, its GPU time measured): 1. st_conv: d_c (median of 16x16 samples of the centre);
// 2. st_inv: the depth (at the upscaler's render size) to the picture's size, edge-aware: bilinear within a
// surface, across a depth edge the sample whose colour matches the pixel's (joint bilateral, picking not blending,
// so no pixels float between the depths); 3. st_eye: each eye by a backward parallax search per row (a row segment
// in threadgroup memory): every pair of neighbouring source pixels maps to a span of the eye's row; of the spans
// that cover the output pixel the nearest surface wins (interpolated between the two), and a pixel only covered by
// a disocclusion gap (a span stretched past 2 px: foreground edge next to background) takes the gap's background-side
// pixel; 4. st_window (what the window shows), st_pack (the eyes side by side for the encoder/snapshots).
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "stereo.h"

void vx_log(const char *fmt, ...);

static const char *g_stereo_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct SP { uint w, h, dw, dh; uint dfmt, reversed, layout, encode; float S, pop, conv, pct; float smooth, p0, p1, p2; };\n"
    "#define TG 256\n"
    "#define RMAX 160\n"
    "static float inv_of(device const uchar *dep, constant SP &p, uint2 q) {\n"
    "    uint i = q.y * p.dw + q.x;\n"
    "    float d = p.dfmt == 1 ? float(((device const ushort *)dep)[i]) / 65535.0f : ((device const float *)dep)[i];\n"
    "    d = clamp(d, 0.0f, 1.0f);\n"
    "    return p.reversed ? d : 1.0f - d;\n"
    "}\n"
    // 1: the screen plane's inverse depth, the pct-th of 16x16 samples over the centre (half the width and height),
    //    sorted in threadgroup memory; smoothed over frames in state[0] (state[1]: this frame's)
    "kernel void st_conv(device const uchar *dep [[buffer(0)]], constant SP &p [[buffer(1)]], device float *state [[buffer(2)]],\n"
    "                    uint i [[thread_index_in_threadgroup]]) {\n"
    "    threadgroup float v[256];\n"
    "    uint gx = i & 15, gy = i >> 4;\n"
    "    uint2 q = uint2(p.dw / 4 + (gx * 2 + 1) * p.dw / 64, p.dh / 4 + (gy * 2 + 1) * p.dh / 64);\n"
    "    v[i] = inv_of(dep, p, min(q, uint2(p.dw - 1, p.dh - 1)));\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    for (uint k = 2; k <= 256; k <<= 1)\n"
    "        for (uint j = k >> 1; j > 0; j >>= 1) {\n"
    "            uint l = i ^ j;\n"
    "            if (l > i) {\n"
    "                float a = v[i], b = v[l];\n"
    "                if ((a > b) == ((i & k) == 0)) { v[i] = b; v[l] = a; }\n"
    "            }\n"
    "            threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "        }\n"
    "    if (i == 0) {\n"
    "        float m = v[min(255u, uint(p.pct * 255.0f + 0.5f))];\n"
    "        float prev = state[0];\n"
    "        if (m > 1e-6f) state[0] = prev > 0 ? mix(prev, m, p.smooth) : m;\n"
    "        state[1] = m;\n"
    "    }\n"
    "}\n"
    // 2: inverse depth at the picture's size, edge-aware
    "kernel void st_inv(device const uchar *dep [[buffer(0)]], constant SP &p [[buffer(1)]],\n"
    "                   texture2d<float, access::read> col [[texture(0)]], texture2d<float, access::write> inv [[texture(1)]],\n"
    "                   uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x >= p.w || g.y >= p.h) return;\n"
    "    float2 q = (float2(g) + 0.5f) * float2(p.dw, p.dh) / float2(p.w, p.h) - 0.5f;\n"
    "    int2 i0 = int2(floor(q));\n"
    "    float2 f = q - float2(i0);\n"
    "    int2 lim = int2(p.dw - 1, p.dh - 1);\n"
    "    int2 c[4] = { clamp(i0, int2(0), lim), clamp(i0 + int2(1, 0), int2(0), lim), clamp(i0 + int2(0, 1), int2(0), lim),\n"
    "                  clamp(i0 + int2(1, 1), int2(0), lim) };\n"
    "    float wb[4] = { (1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y };\n"
    "    float d[4], lo = 2, hi = -1, r = 0;\n"
    "    for (int k = 0; k < 4; k++) { d[k] = inv_of(dep, p, uint2(c[k])); lo = min(lo, d[k]); hi = max(hi, d[k]); r += wb[k] * d[k]; }\n"
    "    if (hi - lo > 0.04f * hi) {\n"
    "        float3 cp = col.read(g).rgb;\n"
    "        float best = -1;\n"
    "        for (int k = 0; k < 4; k++) {\n"
    "            float2 pf = (float2(c[k]) + 0.5f) * float2(p.w, p.h) / float2(p.dw, p.dh) - 0.5f;\n"
    "            uint2 pc = uint2(clamp(int2(rint(pf)), int2(0), int2(p.w - 1, p.h - 1)));\n"
    "            float3 dc = col.read(pc).rgb - cp;\n"
    "            float wgt = (wb[k] + 0.05f) * exp(-dot(dc, dc) * 30.0f);\n"
    "            if (wgt > best) { best = wgt; r = d[k]; }\n"
    "        }\n"
    "    }\n"
    "    inv.write(float4(r), g);\n"
    "}\n"
    // 3: the eyes (z 0 left, 1 right), threadgroups of TG pixels of a row
    "kernel void st_eye(texture2d<float, access::read> col [[texture(0)]], texture2d<float, access::read> inv [[texture(1)]],\n"
    "                   texture2d<float, access::write> outL [[texture(2)]], texture2d<float, access::write> outR [[texture(3)]],\n"
    "                   constant SP &p [[buffer(1)]], device const float *state [[buffer(2)]],\n"
    "                   uint3 g [[thread_position_in_grid]], uint3 tg [[threadgroup_position_in_grid]],\n"
    "                   uint t [[thread_index_in_threadgroup]]) {\n"
    "    threadgroup float T[TG + 2 * RMAX + 2];\n"
    "    threadgroup float I[TG + 2 * RMAX + 2];\n"
    "    float s = g.z == 0 ? -0.5f : 0.5f;\n"
    "    float inv_c = max(state[0] / p.conv, 1e-7f);\n"
    "    int R = min(int(ceil(p.S * 0.5f)) + 2, RMAX);\n"
    "    int W = int(p.w), y = int(min(g.y, p.h - 1));\n"
    "    int x0 = int(tg.x) * TG - R, n = TG + 2 * R + 1;\n"
    "    for (int k = int(t); k < n; k += TG) {\n"
    "        int xs = clamp(x0 + k, 0, W - 1);\n"
    "        float v = inv.read(uint2(xs, y)).r;\n"
    "        T[k] = float(x0 + k) + s * clamp(p.S * (1.0f - v / inv_c), -p.pop * p.S, p.S);\n"
    "        I[k] = v;\n"
    "    }\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    if (g.x >= p.w || g.y >= p.h) return;\n"
    "    float x = float(g.x);\n"
    "    float best = -1, a = 0, gbest = -1;\n"
    "    int bx = -1, gx = -1;\n"
    "    int lo = max(int(t), -x0), hi = min(int(t) + 2 * R, W - 2 - x0);\n"
    "    for (int k = lo; k <= hi; k++) {\n"
    "        float t0 = T[k], t1 = T[k + 1];\n"
    "        if (!(t0 <= x && x < t1)) continue;\n"
    "        float i0 = I[k], i1 = I[k + 1];\n"
    "        if (t1 - t0 <= 2.0f) {\n"
    "            float aa = (x - t0) / (t1 - t0), iv = mix(i0, i1, aa);\n"
    "            if (iv > best) { best = iv; a = aa; bx = x0 + k; }\n"
    "        } else {\n"
    "            float fi = max(i0, i1);\n"
    "            if (fi > gbest) { gbest = fi; gx = x0 + k + (i0 < i1 ? 0 : 1); }\n"
    "        }\n"
    "    }\n"
    "    float4 c;\n"
    "    if (bx >= 0) c = mix(col.read(uint2(bx, y)), col.read(uint2(min(bx + 1, W - 1), y)), a);\n"
    "    else if (gx >= 0) c = col.read(uint2(gx, y));\n"
    "    else c = col.read(uint2(g.x, y));\n"
    "    if (g.z == 0) outL.write(c, g.xy); else outR.write(c, g.xy);\n"
    "}\n"
    // 4: the window's picture: 1 both eyes squeezed side by side, 2 red/cyan anaglyph, 3 left, 4 right
    "kernel void st_window(texture2d<float, access::read> L [[texture(0)]], texture2d<float, access::read> Rt [[texture(1)]],\n"
    "                      texture2d<float, access::write> o [[texture(2)]], constant SP &p [[buffer(1)]],\n"
    "                      uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x >= p.w || g.y >= p.h) return;\n"
    "    float4 c;\n"
    "    if (p.layout == 1) {\n"
    "        uint h2 = p.w / 2, x = g.x < h2 ? g.x : g.x - h2;\n"
    "        uint2 a = uint2(min(2 * x, p.w - 1), g.y), b = uint2(min(2 * x + 1, p.w - 1), g.y);\n"
    "        c = g.x < h2 ? 0.5f * (L.read(a) + L.read(b)) : 0.5f * (Rt.read(a) + Rt.read(b));\n"
    "    } else if (p.layout == 2) {\n"
    "        float4 l = L.read(g), r = Rt.read(g);\n"
    "        c = float4(l.r, r.g, r.b, 1);\n"
    "    } else c = p.layout == 4 ? Rt.read(g) : L.read(g);\n"
    "    o.write(float4(c.rgb, 1), g);\n"
    "}\n"
    // 5: both eyes side by side as BGRA8 (sRGB-encoded when the picture reads as linear: sRGB or float formats)
    "kernel void st_pack(texture2d<float, access::read> L [[texture(0)]], texture2d<float, access::read> Rt [[texture(1)]],\n"
    "                    device uint *out [[buffer(0)]], constant SP &p [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x >= 2 * p.w || g.y >= p.h) return;\n"
    "    bool r = g.x >= p.w;\n"
    "    uint2 q = uint2(r ? g.x - p.w : g.x, g.y);\n"
    "    float3 c = saturate((r ? Rt.read(q) : L.read(q)).rgb);\n"
    "    if (p.encode) c = select(1.055f * pow(c, 1.0f / 2.4f) - 0.055f, c * 12.92f, c <= 0.0031308f);\n"
    "    uint3 b = uint3(saturate(c) * 255.0f + 0.5f);\n"
    "    out[g.y * 2 * p.w + g.x] = b.z | (b.y << 8) | (b.x << 16) | 0xff000000u;\n"
    "}\n";

typedef struct { uint32_t w, h, dw, dh, dfmt, reversed, layout, encode; float S, pop, conv, pct, smooth, p0, p1, p2; } stereo_params;

// ---- configuration ----
static struct {
    float sep, pop, conv;
    int window;                       // 0 flat, 1 sbs, 2 anaglyph, 3 left, 4 right
    char stream[1024], shot[1024];
    int stats;
} g_cfg;
static int g_on = -1;
static float env_float(const char *name, float dflt, float lo, float hi) {
    const char *e = getenv(name);
    if (!e || !*e) return dflt;
    char *end = NULL;
    float v = strtof(e, &end);
    return end != e && isfinite(v) && v >= lo && v <= hi ? v : dflt;
}
int vx_stereo_on(void) {
    if (g_on >= 0) return g_on;
    const char *e = getenv("VXRT_STEREO");
    int on = e && *e && *e != '0';
    if (on) {
        g_cfg.sep = env_float("VXRT_STEREO_SEP", 2.0f, 0.0f, 10.0f) / 100.0f;
        g_cfg.pop = env_float("VXRT_STEREO_POP", 0.4f, 0.0f, 1.0f);
        g_cfg.conv = env_float("VXRT_STEREO_CONV", 1.0f, 0.05f, 20.0f);
        const char *w = getenv("VXRT_STEREO_WINDOW");
        g_cfg.window = !w || !*w || !strcmp(w, "sbs") ? 1 : !strcmp(w, "anaglyph") ? 2 : !strcmp(w, "left") ? 3 :
                       !strcmp(w, "right") ? 4 : 0;
        const char *s = getenv("VXRT_STEREO_STREAM");
        if (s && !strcmp(s, "1")) {
            const char *f = getenv("VERANTYX_SHARED_FRAME"), *home = getenv("HOME");
            if (f && *f) snprintf(g_cfg.stream, sizeof g_cfg.stream, "%s", f);
            else snprintf(g_cfg.stream, sizeof g_cfg.stream, "%s/Verantyx_VR_Drive/vr_stereo_frame.dat", home ? home : "/tmp");
        } else if (s && *s && strcmp(s, "0")) snprintf(g_cfg.stream, sizeof g_cfg.stream, "%s", s);
        const char *d = getenv("VXRT_STEREO_SHOT");
        if (d && *d) snprintf(g_cfg.shot, sizeof g_cfg.shot, "%s", d);
        g_cfg.stats = (int)env_float("VXRT_STEREO_STATS", 300, 0, 1e6f);
        vx_log("stereo: on, separation %.2f%% of the width, pop-out %.2f, convergence %.2f, window %s%s%s", g_cfg.sep * 100,
               g_cfg.pop, g_cfg.conv, (const char *[]){ "flat", "sbs", "anaglyph", "left", "right" }[g_cfg.window],
               g_cfg.stream[0] ? ", streamed to " : "", g_cfg.stream[0] ? g_cfg.stream : "");
    }
    g_on = on;
    return on;
}

// ---- the scene's depth ----
typedef struct { void *tex; uint32_t w, h, draws, writes, funcs; } stereo_cand;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static stereo_cand g_cand[16];
static int g_ncand;
static void *g_pick;                                  // (identity only, not retained: the depth whose copies are kept)
static uint32_t g_pick_w, g_pick_h, g_pick_funcs;
static MTLPixelFormat g_pick_fmt;
static id<MTLBuffer> g_hold;                          // (the copies of g_pick, each pass that wrote it)
static int g_copies;                                  // (this frame's)
static uint32_t g_copy_funcs;
static _Atomic uint32_t g_back_w, g_back_h;
static _Atomic int g_paused;

void vx_stereo_depth_pass(void *cbp, void *texp, uint32_t draws, uint32_t writes, uint32_t funcs) {
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)cbp;
    id<MTLTexture> t = (__bridge id<MTLTexture>)texp;
    uint32_t bw = atomic_load(&g_back_w), bh = atomic_load(&g_back_h);
    if (atomic_load(&g_paused) || !draws || !t || !bw || !bh || t.sampleCount > 1 || t.textureType != MTLTextureType2D) return;
    MTLPixelFormat f = t.pixelFormat;
    if (f != MTLPixelFormatDepth16Unorm && f != MTLPixelFormatDepth32Float && f != MTLPixelFormatDepth32Float_Stencil8) return;
    uint32_t w = (uint32_t)t.width, h = (uint32_t)t.height;
    double a = (double)w / h, ab = (double)bw / bh;
    if (w < 128 || fabs(a / ab - 1.0) > 0.03) return;
    pthread_mutex_lock(&g_lock);
    stereo_cand *c = NULL;
    for (int k = 0; k < g_ncand; k++) if (g_cand[k].tex == texp) c = &g_cand[k];
    if (!c && g_ncand < 16) { c = &g_cand[g_ncand++]; *c = (stereo_cand){ texp, w, h, 0, 0, 0 }; }
    if (c) { c->draws += draws; c->writes += writes; c->funcs |= funcs; }
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
            g_copy_funcs |= funcs;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

// ---- the encoder's shared frame file (MacEncoder/HardwareEncoder.swift, NativeARM/vrruntime/vr_runtime.h) ----
#pragma pack(push, 1)
typedef struct { uint32_t sequence, width, height, format; float transform[16]; double timestamp; } stereo_shared_frame;
#pragma pack(pop)
typedef struct { volatile int32_t latest, pending, request, ack, mode, pose; uint32_t padding[10]; } stereo_shared_header;
enum { SHARED_PIXELS = 4096u * 4096u * 8u, SHARED_BLOCK = 88 + SHARED_PIXELS, SHARED_SIZE = 64 + 2 * SHARED_BLOCK + 312 + 1024,
       MODE_FLAT_STEREO = 3 };
static uint8_t *g_map;
static uint32_t g_stream_seq;
static int stream_map(void) {
    static int tried;
    if (g_map || tried) return g_map != NULL;
    tried = 1;
    int fd = open(g_cfg.stream, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { vx_log("stereo: cannot open the shared frame file %s", g_cfg.stream); return 0; }
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < SHARED_SIZE && ftruncate(fd, SHARED_SIZE) != 0) vx_log("stereo: cannot size %s", g_cfg.stream);
    void *m = mmap(NULL, SHARED_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { vx_log("stereo: cannot map %s", g_cfg.stream); return 0; }
    g_map = m;
    vx_log("stereo: streaming to %s (start MacEncoder/HardwareEncoder with VERANTYX_SHARED_FRAME set to it)", g_cfg.stream);
    return 1;
}
// (both eyes side by side, `w` x `h` each, BGRA8: into the free slot, then published)
static void stream_publish(const void *pixels, uint32_t w, uint32_t h) {
    if (!stream_map()) return;
    stereo_shared_header *hd = (stereo_shared_header *)g_map;
    uint32_t idx = ((uint32_t)__atomic_load_n(&hd->latest, __ATOMIC_ACQUIRE) & 1u) ^ 1u;
    stereo_shared_frame *f = (stereo_shared_frame *)(g_map + 64 + (size_t)SHARED_BLOCK * idx);
    memcpy((uint8_t *)f + 88, pixels, (size_t)w * 2 * h * 4);
    uint32_t seq = ++g_stream_seq;
    if (!seq) seq = ++g_stream_seq;
    f->sequence = seq; f->width = 2 * w; f->height = h; f->format = 87;      // DXGI_FORMAT_B8G8R8A8_UNORM
    memset(f->transform, 0, sizeof f->transform);
    f->transform[0] = f->transform[5] = f->transform[10] = f->transform[15] = 1;
    f->timestamp = 0;                                                         // (no head pose: the headset's screen is world-locked)
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&hd->latest, (int32_t)idx, __ATOMIC_RELEASE);
    __atomic_store_n(&hd->mode, MODE_FLAT_STEREO, __ATOMIC_RELEASE);
}

// ---- snapshots ----
static void write_png(const char *path, void *bgra, uint32_t w, uint32_t h) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(bgra, w, h, 8, (size_t)w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    NSURL *url = [NSURL fileURLWithPath:@(path)];
    CGImageDestinationRef dst = img ? CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) { CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
    if (img) CGImageRelease(img);
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}
static void shot_write(uint8_t *sbs, uint32_t w, uint32_t h, int n, const char *line) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%04d-sbs.png", g_cfg.shot, n);
    write_png(path, sbs, 2 * w, h);
    uint8_t *ana = malloc((size_t)w * h * 4);
    if (ana) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint8_t *l = sbs + ((size_t)y * 2 * w + x) * 4, *r = l + (size_t)w * 4, *o = ana + ((size_t)y * w + x) * 4;
                o[0] = r[0]; o[1] = r[1]; o[2] = l[2]; o[3] = 255;                 // (BGRA: red from the left eye)
            }
        snprintf(path, sizeof path, "%s/%04d-anaglyph.png", g_cfg.shot, n);
        write_png(path, ana, w, h);
        free(ana);
    }
    snprintf(path, sizeof path, "%s/shots.txt", g_cfg.shot);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%04d %s\n", n, line); fclose(f); }
    free(sbs);
}

// ---- the pass ----
static id<MTLDevice> g_dev;
static id<MTLComputePipelineState> g_p_conv, g_p_inv, g_p_eye, g_p_window, g_p_pack;
static id<MTLTexture> g_eye[2], g_inv, g_win;
static id<MTLBuffer> g_state, g_pack[3];
static _Atomic int g_pack_busy[3];
static uint32_t g_tw, g_th;
static int g_frame, g_shot_left, g_shot_n;
// (stats: since the last report)
static pthread_mutex_t g_stat_lock = PTHREAD_MUTEX_INITIALIZER;
static double g_gpu_sum, g_gpu_max;
static int g_gpu_n, g_stereo_n, g_flat_n, g_stream_n, g_stream_skip;

static int make_pipelines(id<MTLDevice> dev) {
    if (g_dev == dev && g_p_pack) return 1;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(g_stereo_msl) options:nil error:&err];
    if (!lib) { vx_log("stereo: shaders: %s", err.localizedDescription.UTF8String); return 0; }
    id<MTLComputePipelineState> (^make)(NSString *) = ^id<MTLComputePipelineState>(NSString *name) {
        NSError *e = nil;
        id<MTLComputePipelineState> p = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&e];
        if (!p) vx_log("stereo: %s: %s", name.UTF8String, e.localizedDescription.UTF8String);
        return p;
    };
    g_p_conv = make(@"st_conv"); g_p_inv = make(@"st_inv"); g_p_eye = make(@"st_eye");
    g_p_window = make(@"st_window"); g_p_pack = make(@"st_pack");
    g_state = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    if (!g_p_conv || !g_p_inv || !g_p_eye || !g_p_window || !g_p_pack || !g_state) { g_p_pack = nil; return 0; }
    memset(g_state.contents, 0, 16);
    g_dev = dev;
    return 1;
}
static int make_targets(uint32_t w, uint32_t h) {
    if (g_tw == w && g_th == h && g_win) return 1;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModePrivate;
    g_eye[0] = [g_dev newTextureWithDescriptor:td];
    g_eye[1] = [g_dev newTextureWithDescriptor:td];
    g_win = [g_dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatR32Float;
    g_inv = [g_dev newTextureWithDescriptor:td];
    for (int k = 0; k < 3; k++) g_pack[k] = [g_dev newBufferWithLength:(NSUInteger)w * 2 * h * 4 options:MTLResourceStorageModeShared];
    if (!g_eye[0] || !g_eye[1] || !g_win || !g_inv || !g_pack[0] || !g_pack[1] || !g_pack[2]) { g_win = nil; return 0; }
    g_tw = w; g_th = h;
    return 1;
}
static int linear_format(MTLPixelFormat f) {
    return f == MTLPixelFormatRGBA8Unorm_sRGB || f == MTLPixelFormatBGRA8Unorm_sRGB || f == MTLPixelFormatRGBA16Float ||
           f == MTLPixelFormatRGBA32Float || f == MTLPixelFormatRG11B10Float || f == MTLPixelFormatRGB9E5Float;
}

void *vx_stereo_present(void *queuep, void *backp, uint32_t w, uint32_t h) {
    id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)queuep;
    id<MTLTexture> back = (__bridge id<MTLTexture>)backp;
    if (!q || !back || !w || !h || w > 4096 || h > 4096) return NULL;
    atomic_store(&g_back_w, w);
    atomic_store(&g_back_h, h);
    static int s_presents;
    if (g_cfg.shot[0] && s_presents++ % 30 == 0) {
        char path[1200];
        snprintf(path, sizeof path, "%s/pause", g_cfg.shot);
        int paused = access(path, F_OK) == 0;
        if (paused != atomic_load(&g_paused)) vx_log("stereo: %s", paused ? "paused (the window shows the game's picture)" : "resumed");
        atomic_store(&g_paused, paused);
    }
    if (atomic_load(&g_paused)) {
        pthread_mutex_lock(&g_lock);
        g_ncand = 0; g_copies = 0; g_copy_funcs = 0; g_pick = NULL;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    // the frame's scene depth, and the next frame's choice
    pthread_mutex_lock(&g_lock);
    // (one frame without a copy keeps the last one: with VXRT_D3D12_PARALLEL_REPLAY a frame's passes can be encoded
    //  after its present is, their copies counted for the next frame)
    static int s_grace;
    int valid = g_hold && (g_copies > 0 || s_grace);
    s_grace = g_copies > 0;
    id<MTLBuffer> hold = g_hold;
    uint32_t dw = g_pick_w, dh = g_pick_h, dfuncs = g_copy_funcs;
    MTLPixelFormat dfmt = g_pick_fmt;
    stereo_cand *win = NULL, *cur = NULL;
    for (int k = 0; k < g_ncand; k++) {
        if (!win || g_cand[k].draws > win->draws) win = &g_cand[k];
        if (g_cand[k].tex == g_pick) cur = &g_cand[k];
    }
    // (a new choice only when it is clearly ahead: two depths drawn about as much would take turns)
    if (win && win != cur && (!cur || win->draws > cur->draws + cur->draws / 4)) {
        g_pick = win->tex; g_pick_w = win->w; g_pick_h = win->h; g_pick_funcs = win->funcs;
        g_pick_fmt = ((__bridge id<MTLTexture>)win->tex).pixelFormat;
        vx_log("stereo: the scene's depth is %ux%u fmt %lu (%u depth-tested draws, %u writing, compare funcs %#x; %d candidates)",
               win->w, win->h, (unsigned long)g_pick_fmt, win->draws, win->writes, win->funcs, g_ncand);
    } else if (!win) g_pick = NULL;
    g_ncand = 0; g_copies = 0; g_copy_funcs = 0;
    pthread_mutex_unlock(&g_lock);

    if (!make_pipelines(q.device) || !make_targets(w, h)) return NULL;
    g_frame++;
    // reversed Z: GREATER / GREATER_EQUAL on the depth writes (LESS / LESS_EQUAL: standard; neither: reversed, as
    // modern engines draw)
    uint32_t rev = (dfuncs & ((1u << 5) | (1u << 7))) || !(dfuncs & ((1u << 2) | (1u << 4)));
    float S = fminf(g_cfg.sep * (float)w, 2.0f * (160 - 3));
    stereo_params p = { w, h, dw, dh, dfmt == MTLPixelFormatDepth16Unorm, rev, (uint32_t)g_cfg.window, (uint32_t)linear_format(back.pixelFormat),
                        S, g_cfg.pop, g_cfg.conv, 0.5f, 0.2f, 0, 0, 0 };
    if (!valid || S <= 0) valid = 0;
    // snapshots asked for: <shot>/now
    if (g_cfg.shot[0] && !g_shot_left) {
        char path[1200];
        snprintf(path, sizeof path, "%s/now", g_cfg.shot);
        FILE *f = fopen(path, "r");
        if (f) {
            int n = 0;
            if (fscanf(f, "%d", &n) != 1 || n < 1) n = 1;
            fclose(f);
            unlink(path);
            g_shot_left = n;
            vx_log("stereo: the next %d frames' eyes go to %s", n, g_cfg.shot);
        }
    }
    int shot = g_shot_left > 0 ? ++g_shot_n : 0;
    if (g_shot_left > 0) g_shot_left--;
    int pack = -1;
    if (g_cfg.stream[0] || shot)
        for (int k = 0; k < 3 && pack < 0; k++) {
            int expect = 0;
            if (atomic_compare_exchange_strong(&g_pack_busy[k], &expect, 1)) pack = k;
        }

    id<MTLCommandBuffer> cb = [q commandBuffer];
    if (!cb) { if (pack >= 0) atomic_store(&g_pack_busy[pack], 0); return NULL; }
    cb.label = @"stereo";
    id<MTLTexture> L = back, R = back;
    MTLSize tg16 = MTLSizeMake(16, 16, 1), grid = MTLSizeMake(w, h, 1);
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setBytes:&p length:sizeof p atIndex:1];
    if (valid) {
        [e setComputePipelineState:g_p_conv];
        [e setBuffer:hold offset:0 atIndex:0];
        [e setBuffer:g_state offset:0 atIndex:2];
        [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [e setComputePipelineState:g_p_inv];
        [e setTexture:back atIndex:0];
        [e setTexture:g_inv atIndex:1];
        [e dispatchThreads:grid threadsPerThreadgroup:tg16];
        [e setComputePipelineState:g_p_eye];
        [e setTexture:back atIndex:0];
        [e setTexture:g_inv atIndex:1];
        [e setTexture:g_eye[0] atIndex:2];
        [e setTexture:g_eye[1] atIndex:3];
        [e dispatchThreadgroups:MTLSizeMake((w + 255) / 256, h, 2) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        L = g_eye[0]; R = g_eye[1];
    }
    id<MTLTexture> show = nil;
    if (g_cfg.window) {
        [e setComputePipelineState:g_p_window];
        [e setTexture:L atIndex:0];
        [e setTexture:R atIndex:1];
        [e setTexture:g_win atIndex:2];
        [e dispatchThreads:grid threadsPerThreadgroup:tg16];
        show = g_win;
    }
    if (pack >= 0) {
        [e setComputePipelineState:g_p_pack];
        [e setTexture:L atIndex:0];
        [e setTexture:R atIndex:1];
        [e setBuffer:g_pack[pack] offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(2 * w, h, 1) threadsPerThreadgroup:tg16];
    }
    [e endEncoding];
    int frame = g_frame, stream = g_cfg.stream[0] != 0, stats = g_cfg.stats;
    id<MTLBuffer> packed = pack >= 0 ? g_pack[pack] : nil, state = g_state;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        double ms = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
        if (packed) {
            if (stream) stream_publish(packed.contents, w, h);
            if (shot) {
                uint8_t *copy = malloc((size_t)w * 2 * h * 4);
                char line[256];
                const float *st = state.contents;
                snprintf(line, sizeof line, "frame %d %s back %ux%u fmt %lu depth %ux%u fmt %lu reversed %u inv_c %.5f (this frame %.5f) S %.1f px gpu %.3f ms",
                         frame, valid ? "stereo" : "flat", w, h, (unsigned long)back.pixelFormat, dw, dh, (unsigned long)dfmt, rev,
                         st[0], st[1], S, ms);
                char *text = strdup(line);
                if (copy && text) {
                    memcpy(copy, packed.contents, (size_t)w * 2 * h * 4);
                    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ shot_write(copy, w, h, shot, text); free(text); });
                } else { free(copy); free(text); }
            }
            atomic_store(&g_pack_busy[pack], 0);
        }
        pthread_mutex_lock(&g_stat_lock);
        if (done.status == MTLCommandBufferStatusCompleted) {
            g_gpu_sum += ms; g_gpu_n++;
            if (ms > g_gpu_max) g_gpu_max = ms;
        }
        if (valid) g_stereo_n++; else g_flat_n++;
        if (packed && stream) g_stream_n++;
        if (stats && g_stereo_n + g_flat_n >= stats) {
            const float *st = state.contents;
            vx_log("stereo: %d frames (%d stereo, %d flat), stereo pass GPU %.3f ms average, %.3f max; %ux%u, depth %ux%u, screen plane inv depth %.5f; %d streamed, %d not (encoder slots busy)",
                   g_stereo_n + g_flat_n, g_stereo_n, g_flat_n, g_gpu_n ? g_gpu_sum / g_gpu_n : 0.0, g_gpu_max, w, h, dw, dh, st[0],
                   g_stream_n, g_stream_skip);
            g_gpu_sum = g_gpu_max = 0; g_gpu_n = g_stereo_n = g_flat_n = g_stream_n = g_stream_skip = 0;
        }
        pthread_mutex_unlock(&g_stat_lock);
    }];
    if (stream && pack < 0) { pthread_mutex_lock(&g_stat_lock); g_stream_skip++; pthread_mutex_unlock(&g_stat_lock); }
    [cb commit];
    return (__bridge void *)show;
}
