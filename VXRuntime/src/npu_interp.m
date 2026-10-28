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
// VXRT_NPU_INTERP=race: the display's refresh drives the window (asynchronous spacewarp's idea). A real frame is
// presented the moment the GPU has it, as without NPU mode; a display link (CADisplayLink) wakes at each refresh and,
// when no real frame has come by a margin before the next one, presents a frame predicted to that time: B, the newest
// real frame, moved on by t = (now - when B was presented) / the frames' interval of the motion from the optical flow
// of A -> B. t is at most VXRT_NPU_RACE_MAX (default 1.25; past it the window keeps what it shows), and a refresh is
// left alone when the scene moved less than VXRT_NPU_RACE_MIN_PX (default 0.3) pixels since the last frame shown.
// Predictions come at most every other refresh of a 100 Hz+ display (even steps of 2 refreshes, 60 a second at
// 120 Hz, half the GPU's work of every refresh; VXRT_NPU_RACE_HZ sets another rate). The flow is made once per real
// frame on the Neural Engine (in the background, newest pair first; until it comes, a flow up to 3 frames old and its
// motion map go on with B); each prediction costs only a GPU warp at half resolution (2x2 blocks): the motion map,
// once per flow, at B's blocks, with no motion where A and B are equal or where B matches A better unmoved than moved
// (HUD and subtitles over a moving scene, and their rims), and blocks whose forward and backward flows disagree
// (occlusions) left out; then a splat (fastest wins), each block's motion (holes, from disocclusions and the blocks
// left out, take the slowest motion that landed nearby: the background's, so errors stay local), and the frame
// sampled from B with it at full resolution. A game at the refresh rate gets no predicted frames: each refresh has a
// real one. VXRT_NPU_RACE_MARGIN_MS (default 4) is how long before the next refresh a refresh's prediction is decided.
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
#include <pthread.h>
#import <AppKit/AppKit.h>
#include "npu_interp.h"
#include "npu_proto.h"

extern char **environ;

enum { NSRC = NPU_MAX_SLOTS, NDST = 3, MAXT = NPU_MAX_TILES, MAX_TILE_PX = 1280 * 720, OVERLAP = 32, FLOW_W = 640 };
enum { KIND_REAL, KIND_INTERP, KIND_EXTRAP, KIND_RACE };

// (shared with the shaders: struct P)
typedef struct { float iw, ih; uint32_t nt, linear; float tile[MAXT][4]; float ramp[MAXT][4]; } npu_params;
// (struct E) the frame's size, frame pixels per flow unit, how far on (0.5: half an interval), the largest difference
// that counts as unchanged, the bits of a splat key that hold the pixel's index
typedef struct { float w, h, sx, sy, k, thr; uint32_t idx_bits, pad; } npu_extrap_params;
// (struct R) race: the frame's size, frame pixels per flow unit, how far on, the still threshold, the forward-backward
// tolerance (pixels + a share of the motion), the splat key's index bits, the half-resolution key grid
typedef struct { float w, h, sx, sy, t, thr, fb, fbk; uint32_t idx_bits, kw, kh, pad; } npu_race_params;

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
    "}\n"
    // race, at half resolution (each of these a 2x2 block of the frame): B's motion map (xy: frame pixels an interval,
    // z: still, w: the flows agree), the splat, the motion each block is drawn with (holes filled), then the frame
    "struct R { float w, h, sx, sy, t, thr, fb, fbk; uint idx_bits, kw, kh, pad; };\n"
    "kernel void npu_race_mm(texture2d<float> b [[texture(0)]], texture2d<float> a [[texture(1)]], texture2d<float> fb [[texture(2)]],\n"
    "                        texture2d<float> ff [[texture(3)]], texture2d<float, access::write> o [[texture(4)]],\n"
    "                        constant R &e [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x >= e.kw || g.y >= e.kh) return;\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    int2 hi = int2(e.w, e.h) - 1, p0 = int2(g) * 2;\n"
    // still: no channel of the 4x4 around the block differs between A and B (a lone pixel that happens to match would
    // tear out of a moving surface)
    "    bool still = true;\n"
    "    for (int y = -1; y <= 2 && still; y++) for (int x = -1; x <= 2; x++) {\n"
    "        uint2 q = uint2(clamp(p0 + int2(x, y), int2(0), hi));\n"
    "        if (any(abs(b.read(q).rgb - a.read(q).rgb) > e.thr)) { still = false; break; }\n"
    "    }\n"
    "    if (still) { o.write(float4(0, 0, 1, 1), g); return; }\n"
    "    float2 sz = float2(e.w, e.h), uv = (float2(p0) + 1) / sz, k = float2(e.sx, e.sy);\n"
    "    float2 m = -fb.sample(s, uv, level(0)).xy * k;\n"                // (the backward flow points from B to A)
    "    if (any(isnan(m)) || any(isinf(m)) || length(m) > 0.5 * max(e.w, e.h)) { o.write(float4(0, 0, 0, 0), g); return; }\n"
    // an overlay over a moving scene (HUD, subtitles): B matches A where it is better than where the motion says
    // (z 0.7: held only where neighbours say so too, a single dark block that matches by chance is not)
    "    if (length(m) > 1) {\n"
    "        float e0 = 0, e1 = 0;\n"
    "        for (int y = 0; y <= 1; y++) for (int x = 0; x <= 1; x++) {\n"
    "            uint2 q = uint2(min(p0 + int2(x, y), hi));\n"
    "            float3 c = b.read(q).rgb;\n"
    "            e0 += dot(abs(c - a.read(q).rgb), 1);\n"
    "            e1 += dot(abs(c - a.sample(s, (float2(q) + 0.5 - m) / sz, level(0)).rgb), 1);\n"
    "        }\n"
    "        if (e0 < 0.5 * e1 && e1 > 0.45) { o.write(float4(m, 0.7, 1), g); return; }\n"
    "    }\n"
    // where it was in A, and the forward flow there: back where it started, or the flows disagree (something covered
    // it in one of the frames); a pixel that came in over the frame's edge has nothing to compare with
    "    float2 ua = uv - m / sz;\n"
    "    float ok = 1;\n"
    "    if (all(ua >= 0) && all(ua <= 1)) {\n"
    "        float2 f = ff.sample(s, ua, level(0)).xy * k;\n"
    "        ok = all(isfinite(f)) && length(f - m) <= e.fb + e.fbk * length(m) ? 1 : 0;\n"
    "    }\n"
    "    o.write(float4(m, 0, ok), g);\n"
    "}\n"
    // still, or the edge of something still (an overlay's antialiased rim, which would otherwise move off as a ghost)
    "static bool held(texture2d<float> mm, uint2 p) {\n"
    "    uint2 hi = uint2(mm.get_width(), mm.get_height()) - 1;\n"
    "    float z = mm.read(p).z;\n"
    "    if (z > 0.9) return true;\n"
    "    float4 n = float4(mm.read(uint2(max(int(p.x) - 1, 0), p.y)).z, mm.read(uint2(min(p.x + 1, hi.x), p.y)).z,\n"
    "                      mm.read(uint2(p.x, max(int(p.y) - 1, 0))).z, mm.read(uint2(p.x, min(p.y + 1, hi.y))).z);\n"
    "    if (z > 0.5) return dot(float4(n > 0.5), 1) >= 2;\n"          // (an overlay block among others)
    "    return dot(float4(n > 0.5), 1) >= 3;\n"                        // (the rim of something held)
    "}\n"
    "kernel void npu_race_splat(texture2d<float> mm [[texture(0)]], device atomic_uint *key [[buffer(0)]], constant R &e [[buffer(1)]],\n"
    "                           uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x >= e.kw || g.y >= e.kh) return;\n"
    "    float4 v = mm.read(g);\n"
    "    if (v.w < 0.5 || v.z > 0.9 || held(mm, g)) return;\n"    // (held: drawn as B; the flows disagree: it lands nowhere, filled as a hole is)
    "    int2 t = int2(round(float2(g) + e.t * v.xy / 2));\n"
    "    if (any(t < 0) || t.x >= int(e.kw) || t.y >= int(e.kh)) return;\n"
    "    uint mq = min(uint(length(v.xy) * 4), (1u << (32 - e.idx_bits)) - 1);\n"
    "    atomic_fetch_max_explicit(&key[uint(t.y) * e.kw + uint(t.x)], (mq << e.idx_bits) | (g.y * e.kw + g.x + 1), memory_order_relaxed);\n"
    "}\n"
    // each block's motion: the block that landed on it; a hole (something uncovered) the slowest motion that landed in the
    // nearest ring around it (the background's); held blocks none
    "kernel void npu_race_resolve(texture2d<float> mm [[texture(0)]], texture2d<float, access::write> o [[texture(1)]],\n"
    "                             device const uint *key [[buffer(0)]], constant R &e [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x >= e.kw || g.y >= e.kh) return;\n"
    "    if (held(mm, g)) { o.write(float4(0, 0, 1, 0), g); return; }\n"
    "    int2 kp = int2(g);\n"
    "    uint mask = (1u << e.idx_bits) - 1, k = key[g.y * e.kw + g.x];\n"
    "    if (!k) {\n"
    "        uint best = 0xffffffffu;\n"
    "        for (int r = 1; r <= 16 && best == 0xffffffffu; r *= 2)\n"
    "            for (int d = 0; d < 9; d++) {\n"
    "                if (d == 4) continue;\n"
    "                int2 q = kp + int2(d % 3 - 1, d / 3 - 1) * r;\n"
    "                if (any(q < 0) || q.x >= int(e.kw) || q.y >= int(e.kh)) continue;\n"
    "                uint k2 = key[uint(q.y) * e.kw + uint(q.x)];\n"
    "                if (k2 && (k2 >> e.idx_bits) < (best >> e.idx_bits)) best = k2;\n"
    "            }\n"
    "        k = best == 0xffffffffu ? 0 : best;\n"
    "    }\n"
    // (nothing landed near: its own flow, however unsure; never a zero motion standing still in a moving scene)
    "    float2 m = k ? mm.read(uint2(((k & mask) - 1) % e.kw, ((k & mask) - 1) / e.kw)).xy : mm.read(g).xy;\n"
    "    if (!all(isfinite(m))) m = 0;\n"
    "    o.write(float4(m, 0, 0), g);\n"
    "}\n"
    "fragment float4 npu_race_fs(V in [[stage_in]], texture2d<float> b [[texture(0)]], texture2d<float> rm [[texture(1)]],\n"
    "                            constant R &e [[buffer(1)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    uint2 p = min(uint2(in.uv * float2(e.w, e.h)), uint2(e.w, e.h) - 1);\n"
    "    float4 v = rm.read(min(p / 2, uint2(e.kw, e.kh) - 1));\n"
    "    if (v.z > 0.5) return float4(b.read(p).rgb, 1);\n"              // (held: the HUD, B's own pixels)
    "    return float4(b.sample(s, in.uv - e.t * v.xy / float2(e.w, e.h)).rgb, 1);\n"
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
    // race (half resolution): B's motion map, the splat's keys, the motion each block is drawn with
    id<MTLTexture> mm, rm;
    id<MTLBuffer> rkeys;
    npu_race_params RP;
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
    volatile int race;             // (NPU_MODE_FLOW) presents driven by the display (VXRT_NPU_INTERP=race)
    int interp_ok, flow_ok;        // what VideoToolbox supports here
    char helper[1024];
    os_unfair_lock lock;           // set
    VXNpuSet *set;
    __unsafe_unretained CAMetalLayer *layer;
    id<MTLDevice> dev;
    id<MTLCommandQueue> queue;     // the display's
    id<MTLRenderPipelineState> real_ps, interp_ps, real_png_ps, interp_png_ps, extrap_ps, extrap_png_ps, race_ps, race_png_ps;
    id<MTLComputePipelineState> yuv_ps, flow_in_ps, splat_ps, mm_ps, rsplat_ps, resolve_ps;
    dispatch_queue_t q, init_q, flow_q;
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
    volatile int race_flow;        // race's flow queue is using the helper
    volatile int race_noflow, race_nowarp;
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
static void race_stop(void);
static float race_hz(float hz);

static int switched_on(void) {
    static const char *path;
    static double next;
    static int on = 1;
    if (!path && !(path = getenv("VXRT_NPU_INTERP_SWITCH"))) path = "";
    if (!*path) return 1;
    double t = CACurrentMediaTime();
    if (t >= next) {
        next = t + 0.5;
        int now = 0, mode = g.mode, race = g.race;
        FILE *f = fopen(path, "r");
        if (f) {
            char word[32] = "";
            now = 1;
            if (fscanf(f, "%31s", word) == 1) {
                if (!strcmp(word, "extrapolate") && g.flow_ok) mode = NPU_MODE_FLOW, race = 0;
                if (!strcmp(word, "race") && g.flow_ok) {
                    mode = NPU_MODE_FLOW, race = 1;
                    // ("race 60": predictions' rate; "noflow", "nowarp": no optical flow / no predictions shown, to
                    //  measure what each costs)
                    char opt[32];
                    g.race_noflow = g.race_nowarp = 0;
                    while (fscanf(f, "%31s", opt) == 1) {
                        if (!strcmp(opt, "noflow")) g.race_noflow = 1;
                        else if (!strcmp(opt, "nowarp")) g.race_nowarp = 1;
                        else if (atof(opt) != race_hz(-1)) race_hz((float)atof(opt));
                    }
                }
                if (!strcmp(word, "interpolate") && g.interp_ok) mode = NPU_MODE_INTERPOLATE, race = 0;
            }
            fclose(f);
        }
        if (now != on || mode != g.mode || race != g.race)
            vx_log("NPU mode: %s (%s)", now ? (race ? "race" : mode == NPU_MODE_FLOW ? "extrapolate" : "interpolate") : "switched off", path);
        on = now;
        g.mode = mode;
        g.race = race;
        if (!on || !race) race_stop();
    }
    return on;
}

int npu_interp_on(void *layer) {
    if (g.state) return g.state > 0 && switched_on();
    const char *e = getenv("VXRT_NPU_INTERP");
    if (!e || !*e || !strcmp(e, "0")) { g.state = -1; return 0; }
    g.race = !strcmp(e, "race");
    g.mode = !strcmp(e, "extrapolate") || g.race ? NPU_MODE_FLOW : NPU_MODE_INTERPOLATE;
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
    vx_log(g.race ? "NPU mode: on (race): each display refresh shows the newest real frame, or one predicted to its time from the Neural Engine's optical flow (VideoToolbox)" :
           g.mode == NPU_MODE_FLOW ? "NPU mode: on (extrapolate): each frame is shown at once, then one predicted from it with the Neural Engine's optical flow (VideoToolbox)"
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
    uint32_t kw = (S->w + 1) / 2, kh = (S->h + 1) / 2, kbits = 1;
    while ((1u << kbits) <= kw * kh) kbits++;
    MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:kw height:kh mipmapped:NO];
    md.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    md.storageMode = MTLStorageModePrivate;
    S->mm = [g.dev newTextureWithDescriptor:md];
    S->rm = [g.dev newTextureWithDescriptor:md];
    S->rkeys = [g.dev newBufferWithLength:(NSUInteger)kw * kh * 4 options:MTLResourceStorageModePrivate];
    if (!S->mm || !S->rm || !S->rkeys) { vx_log("NPU mode: no buffers for race"); return; }
    const char *fb = getenv("VXRT_NPU_RACE_FB");
    S->RP = (npu_race_params){ (float)S->w, (float)S->h, S->E.sx, S->E.sy, 0, 2.0f / 255, fb ? (float)atof(fb) : 3.0f, 0.25f, kbits, kw, kh, 0 };
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
    pd.fragmentFunction = [lib newFunctionWithName:@"npu_race_fs"];
    g.race_png_ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.colorAttachments[0].pixelFormat = layer.pixelFormat;
    g.race_ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.fragmentFunction = [lib newFunctionWithName:@"npu_extrap_fs"];
    g.extrap_ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    g.mm_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_race_mm"] error:&err];
    g.rsplat_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_race_splat"] error:&err];
    g.resolve_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_race_resolve"] error:&err];
    g.flow_in_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_to_flow"] error:&err];
    g.splat_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_splat"] error:&err];
    g.yuv_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_to_yuv"] error:&err];
    if (!g.real_ps || !g.interp_ps || !g.yuv_ps || !g.extrap_ps || !g.flow_in_ps || !g.splat_ps || !g.race_ps || !g.mm_ps || !g.rsplat_ps || !g.resolve_ps) { vx_log("NPU mode: no pipeline: %s", err.localizedDescription.UTF8String); g.state = -1; return 0; }
    g.queue = [dev newCommandQueue];
    g.q = dispatch_queue_create("vxrt.npu.display", DISPATCH_QUEUE_SERIAL);
    g.init_q = dispatch_queue_create("vxrt.npu.init", DISPATCH_QUEUE_SERIAL);
    g.flow_q = dispatch_queue_create("vxrt.npu.flow", DISPATCH_QUEUE_SERIAL);
    g.dev = dev;
    return 1;
}

static void draw(id<MTLCommandBuffer> cb, id<MTLTexture> target, int kind, VXNpuSet *S, uint32_t slot, int png) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    if (kind == KIND_RACE) {                                       // (after race_splat(): slot is B, S->RP.t)
        [enc setRenderPipelineState:png ? g.race_png_ps : g.race_ps];
        [enc setFragmentTexture:S->copy[slot] atIndex:0];
        [enc setFragmentTexture:S->rm atIndex:1];
        [enc setFragmentBytes:&S->RP length:sizeof S->RP atIndex:1];
    } else if (kind == KIND_EXTRAP) {                              // (after splat(): S->ex_prev, S->ex_dst)
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
static const char *const kind_names[] = { "real", "interp", "extrap", "race" };

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

// (race: the half-resolution splat of B's motion map, S->RP.t of an interval on, into S->rkeys, and from it each
// block's motion, into S->rm)
static void race_splat(id<MTLCommandBuffer> cb, VXNpuSet *S) {
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:S->rkeys range:NSMakeRange(0, S->rkeys.length) value:0];
    [blit endEncoding];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:g.rsplat_ps];
    [enc setTexture:S->mm atIndex:0];
    [enc setBuffer:S->rkeys offset:0 atIndex:0];
    [enc setBytes:&S->RP length:sizeof S->RP atIndex:1];
    [enc dispatchThreads:MTLSizeMake(S->RP.kw, S->RP.kh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];
    enc = [cb computeCommandEncoder];                              // (after the splat's atomics)
    [enc setComputePipelineState:g.resolve_ps];
    [enc setTexture:S->mm atIndex:0];
    [enc setTexture:S->rm atIndex:1];
    [enc setBuffer:S->rkeys offset:0 atIndex:0];
    [enc setBytes:&S->RP length:sizeof S->RP atIndex:1];
    [enc dispatchThreads:MTLSizeMake(S->RP.kw, S->RP.kh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];
}

// VXRT_NPU_INTERP_SNAPSHOT: the real frames before and after an interpolated one and that one (extrapolated: the two
// real frames it comes from, then it), as PNGs
static void snapshot(VXNpuSet *S, uint32_t prev, uint32_t dst, uint32_t cur) {
    static uint32_t n;
    if (!g.snap_dir || !g.interp_png_ps || ++n % (uint32_t)g.snap_every) return;
    @autoreleasepool {
        uint32_t w = S->w, h = S->h;
        int ex = S->mode == NPU_MODE_FLOW;
        int race = ex && g.race;
        const char *names[3] = { "a_real", ex ? "b_real" : "b_interp", race ? "c_race" : ex ? "c_extrap" : "c_real" };
        for (int k = 0; k < 3; k++) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
            id<MTLTexture> target = [g.dev newTextureWithDescriptor:td];
            id<MTLBuffer> out = [g.dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [g.queue commandBuffer];
            if (race) { if (k == 2) race_splat(cb, S); draw(cb, target, k == 2 ? KIND_RACE : KIND_REAL, S, k == 0 ? prev : cur, 1); }
            else if (ex) { if (k == 2) splat(cb, S, cur); draw(cb, target, k == 2 ? KIND_EXTRAP : KIND_REAL, S, k == 0 ? prev : cur, 1); }
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
    if (has_in && g.prev >= 0 && g.prev_yuv && g.interval > 0 && !newer && !g.busy && !g.race_flow) {
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

// ---- race (VXRT_NPU_INTERP=race): the display's refresh drives the window ----
// Real frames are presented the moment the GPU has them (on the display queue, as without NPU mode: nothing added);
// a display link's thread fills the refreshes no real frame reaches with a predicted one. (A CAMetalDisplayLink was
// tried first: its frames reached the screen 41 ms after they were ready, 19 ms later than a present at once, since
// it schedules each 5 refreshes ahead; and a layer with one throws in nextDrawable.)
// Slots: the game thread takes one no one uses (in flight, the frames the prediction reads, the flow's pairs); the
// flow queue makes the newest pair's optical flow. All under R.lock (short); presents under R.present (ordered: a
// real frame is never committed before a prediction made from older ones).
static struct {
    os_unfair_lock lock;
    pthread_mutex_t present;
    VXNpuSet *set;
    uint32_t inflight, has_in;     // slot masks: copied and not yet handled; has its flow input
    int last; double last_t;       // the newest frame handled (slot, when it was ready)
    int a, b; double tb;           // the frame shown last (B), its predecessor (A), when B was presented
    uint32_t b_seq;                // counts the B's shown
    double interval, period;       // between real frames (ready times), between refreshes
    double shown_at, shown_t;      // when the last frame (real or not) was presented, and how far on from B
    int want_a, want_b, run_a, run_b, flow_busy;   // the flow's pairs (waiting, running)
    int flow_dst, flow_b; uint32_t flow_seq;       // the newest flow done: its dst slot, its pair's B, a count
    uint32_t want_bseq, flow_bseq;                 // which B (b_seq) the waiting pair and the newest flow end at
    float flow_px;                                 // its motion, frame pixels an interval (98th percentile)
    uint32_t mm_b_seq, mm_flow_seq;                // what the motion map was made from
    // statistics (every 5 s, the tick's thread; frames, superseded: the handlers')
    uint32_t frames, real, gen, still, held, evened, stale, nflow, ngpu, nlat, nt, npres;
    int even;
    float flow_ms[512], gpu_ms[512], lat[512], ts[512];
    double pres[1024];
    double stat_t0;
    dispatch_semaphore_t sem;
    CADisplayLink *link;
    CFRunLoopRef loop;
    __unsafe_unretained CAMetalLayer *layer;
    double margin, max_t, min_px;
    float hz;
} R = { .lock = OS_UNFAIR_LOCK_INIT, .present = PTHREAD_MUTEX_INITIALIZER, .last = -1, .a = -1, .b = -1, .want_a = -1,
        .want_b = -1, .run_a = -1, .run_b = -1, .flow_dst = -1, .flow_b = -1, .period = 1.0 / 60 };

static uint32_t bit(int slot) { return slot >= 0 ? 1u << slot : 0; }

static void race_reset(VXNpuSet *S) {                              // (R.lock held) a new set: nothing of the old one
    R.set = S;
    R.inflight = R.has_in = 0;
    R.last = R.a = R.b = R.want_a = R.want_b = -1;
    R.interval = 0;
    R.flow_dst = R.flow_b = -1;
    R.flow_seq = R.mm_flow_seq = R.mm_b_seq = 0;
    R.b_seq = 1;
}

// (the game thread) a slot for the next frame, -1 for none free
static int race_slot(VXNpuSet *S) {
    os_unfair_lock_lock(&R.lock);
    if (R.set != S) race_reset(S);
    uint32_t used = R.inflight | bit(R.last) | bit(R.a) | bit(R.b) | bit(R.want_a) | bit(R.want_b) | bit(R.run_a) | bit(R.run_b);
    int slot = -1;
    for (uint32_t k = 0; k < NSRC && slot < 0; k++) {
        uint32_t s = (g.next_slot + k) % NSRC;
        if (!(used & bit((int)s))) slot = (int)s;
    }
    if (slot >= 0) { g.next_slot = (uint32_t)slot + 1; R.inflight |= bit(slot); }
    os_unfair_lock_unlock(&R.lock);
    return slot;
}

static float half_to_float(uint16_t h) {
    uint32_t e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? INFINITY : ldexpf((float)(m | 1024), (int)e - 25);
    return h & 0x8000 ? -v : v;
}

// (the flow queue) how far B's pixels move in an interval: the 98th percentile of the backward flow's vectors, in frame
// pixels (to skip predicting a scene that does not move)
static float flow_motion(VXNpuSet *S, uint32_t d) {
    IOSurfaceRef sf = S->surfaces[NSRC + NDST + d];
    if (!sf || IOSurfaceLock(sf, kIOSurfaceLockReadOnly, NULL)) return 1e9f;
    const uint8_t *base = IOSurfaceGetBaseAddress(sf);
    size_t stride = IOSurfaceGetBytesPerRow(sf), w = IOSurfaceGetWidth(sf), h = IOSurfaceGetHeight(sf);
    uint32_t hist[257] = { 0 }, n = 0;
    for (size_t y = 0; y < h; y += 2)
        for (size_t x = 0; x < w; x += 2) {
            const uint16_t *v = (const uint16_t *)(base + y * stride) + 2 * x;
            float mx = half_to_float(v[0]) * S->E.sx, my = half_to_float(v[1]) * S->E.sy, l = sqrtf(mx * mx + my * my);
            hist[l < 256 ? (int)l : 256]++;
            n++;
        }
    IOSurfaceUnlock(sf, kIOSurfaceLockReadOnly, NULL);
    uint32_t acc = 0;
    for (int k = 0; k <= 256; k++) if ((acc += hist[k]) >= n * 49 / 50) return (float)k + 1;
    return 257;
}

// (R.lock held) the waiting pair's optical flow, unless one is running; the helper answers in order, a reply for
// another request (extrapolate mode's, late) is skipped
static void flow_kick(VXNpuSet *S) {
    if (R.flow_busy || R.want_b < 0 || !S->ready || S->sock < 0 || !g.race) return;
    int a = R.want_a, b = R.want_b;
    uint32_t bseq = R.want_bseq;
    R.want_a = R.want_b = -1;
    R.run_a = a; R.run_b = b; R.flow_busy = 1; g.race_flow = 1;
    uint32_t d = (uint32_t)(R.flow_dst + 1) % NDST;
    dispatch_async(g.flow_q, ^{
        npu_proc_req q = { NPU_OP_PROC, __atomic_add_fetch(&g.seq, 1, __ATOMIC_RELAXED), (uint32_t)a, (uint32_t)b, d };
        double t0 = now_s();
        npu_proc_reply r = { 0, -1, 0 };
        int ok = write(S->sock, &q, sizeof q) == sizeof q;
        while (ok) {
            struct pollfd pf = { S->sock, POLLIN, 0 };
            if (poll(&pf, 1, 1000) != 1 || !read_all(S->sock, &r, sizeof r)) { ok = 0; break; }
            if (r.seq == q.seq) break;
        }
        float px = ok && r.status == 0 ? flow_motion(S, d) : 0;
        os_unfair_lock_lock(&R.lock);
        R.flow_busy = 0; g.race_flow = 0;
        R.run_a = R.run_b = -1;
        if (R.set == S && ok && r.status == 0) {
            R.flow_dst = (int)d; R.flow_b = b; R.flow_seq++; R.flow_px = px; R.flow_bseq = bseq;
            if (R.nflow < 512) R.flow_ms[R.nflow++] = (float)((now_s() - t0) * 1000);
        }
        if (R.set == S) flow_kick(S);
        os_unfair_lock_unlock(&R.lock);
    });
}

// (R.present held) a frame into a drawable, presented at once: a real one (slot), or B (slot) predicted S->RP.t on
static int race_present(int kind, VXNpuSet *S, int slot, int a, uint32_t d, int new_mm, double ready) {
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [g.layer nextDrawable];
        if (!drawable) return 0;
        id<MTLCommandBuffer> cb = [g.queue commandBuffer];
        if (kind == KIND_RACE) {
            if (new_mm) {                                          // (once per flow)
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:g.mm_ps];
                [enc setTexture:S->copy[slot] atIndex:0];
                [enc setTexture:S->copy[a] atIndex:1];
                [enc setTexture:S->fbwd[d] atIndex:2];
                [enc setTexture:S->ffwd[d] atIndex:3];
                [enc setTexture:S->mm atIndex:4];
                [enc setBytes:&S->RP length:sizeof S->RP atIndex:0];
                [enc dispatchThreads:MTLSizeMake(S->RP.kw, S->RP.kh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
                [enc endEncoding];
            }
            race_splat(cb, S);
            [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                uint32_t i = __atomic_fetch_add(&R.ngpu, 1, __ATOMIC_RELAXED);
                if (i < 512) R.gpu_ms[i] = (float)((done.GPUEndTime - done.GPUStartTime) * 1000);
            }];
        }
        draw(cb, drawable.texture, kind, S, (uint32_t)slot, 0);
        FILE *rec = record(cb, kind, S, (uint32_t)slot, 0);
        uint32_t pi = __atomic_fetch_add(&R.npres, 1, __ATOMIC_RELAXED);
        [drawable addPresentedHandler:^(id<MTLDrawable> done) {
            double pt = done.presentedTime;
            if (pi < 1024) R.pres[pi] = pt;
            if (kind == KIND_REAL && pt > 0) {
                uint32_t i = __atomic_fetch_add(&R.nlat, 1, __ATOMIC_RELAXED);
                if (i < 512) R.lat[i] = (float)((pt - ready) * 1000);
            }
            if (rec) { fprintf(rec, " %.6f\n", pt); fclose(rec); }
        }];
        [cb presentDrawable:drawable];
        [cb commit];
    }
    return 1;
}

// (the display queue) a frame the GPU has copied: presented at once, B from now on
static void handle_race(VXNpuSet *S, uint32_t slot, int has_in, double t) {
    g.busy = 0;                                                    // (the flow queue skips a late extrapolate reply)
    pthread_mutex_lock(&R.present);
    os_unfair_lock_lock(&R.lock);
    if (R.set != S) race_reset(S);
    R.inflight &= ~bit((int)slot);
    R.frames++;
    int prev = R.last;
    double dt = t - R.last_t;
    if (prev >= 0 && dt > 0.002 && dt < 0.25) R.interval = R.interval ? R.interval * 0.8 + dt * 0.2 : dt;
    else if (dt >= 0.25) prev = -1;                                // (a pause, or the mode was off: no motion to go on)
    R.last = (int)slot; R.last_t = t;
    R.has_in = has_in ? R.has_in | bit((int)slot) : R.has_in & ~bit((int)slot);
    R.a = prev; R.b = (int)slot; R.b_seq++;
    // (a game at the refresh rate gets no predicted frames: no flow wanted)
    // (and not every frame's: a flow takes 20-50 ms of the Neural Engine and some of the GPU; a frame or two older is
    //  as good while the motion is slow)
    uint32_t every = R.flow_px > 8 ? 1 : R.flow_px < 1 ? 3 : 2;
    if (prev >= 0 && has_in && (R.has_in & bit(prev)) && R.interval > R.period * 1.2 && !g.race_noflow &&
        (!R.flow_seq || R.b_seq - R.flow_bseq >= every)) {
        R.want_a = prev; R.want_b = (int)slot; R.want_bseq = R.b_seq;
        flow_kick(S);
    }
    os_unfair_lock_unlock(&R.lock);
    if (race_present(KIND_REAL, S, (int)slot, -1, 0, 0, t)) {
        double now = now_s();
        R.tb = R.shown_at = now; R.shown_t = 0;
        R.real++;
    }
    pthread_mutex_unlock(&R.present);
    dispatch_semaphore_signal(R.sem);
}

static void race_stats(double now) {
    if (!R.stat_t0) { R.stat_t0 = now; return; }
    double dt = now - R.stat_t0;
    if (dt < 5) return;
    os_unfair_lock_lock(&R.lock);
    uint32_t frames = R.frames;
    R.frames = 0;
    float fl[512]; uint32_t nfl = R.nflow;
    memcpy(fl, R.flow_ms, nfl * sizeof *fl);
    R.nflow = 0;
    os_unfair_lock_unlock(&R.lock);
    if (dt <= 6) {
        // the window's steps: presentedTime of each frame on screen, in refreshes (1: every refresh had a new frame)
        uint32_t hist[7] = { 0 }, n = R.npres < 1024 ? R.npres : 1024, shown = 0, gone = 0;
        double prev = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (R.pres[i] <= 0) { gone++; continue; }
            if (prev > 0) {
                int k = (int)lround((R.pres[i] - prev) / R.period);
                hist[k < 0 ? 0 : k > 6 ? 6 : k]++;
            }
            prev = R.pres[i];
            shown++;
        }
        uint32_t ngpu = __atomic_load_n(&R.ngpu, __ATOMIC_ACQUIRE), nlat = __atomic_load_n(&R.nlat, __ATOMIC_ACQUIRE);
        if (ngpu > 512) ngpu = 512;
        if (nlat > 512) nlat = 512;
        float gpu = pct(R.gpu_ms, ngpu, 0.5f);
        vx_log("NPU mode (race): game %.1f fps, window %.1f fps on screen (%u real + %u predicted presented = %.0f%% predicted, "
               "%u not shown: replaced in the same refresh); refreshes without a new frame: %u (the scene still), %u (nothing to "
               "predict from), %u (a real frame due: even steps); %.1f Hz refresh; steps on screen in refreshes 1:%u 2:%u 3:%u 4:%u 5:%u 6+:%u (%u steps); optical "
               "flow %.1f ms median (%u), motion %.1f px an interval, %u%% of the predicted frames on the previous pair's flow; "
               "prediction on the GPU %.2f ms median, %.2f ms p95 (%.1f%% of the GPU's time), t %.2f median; real frames on "
               "screen %.1f ms after they were ready (median, %.1f ms p95)",
               frames / dt, shown / dt, R.real, R.gen, R.real + R.gen ? 100.0 * R.gen / (R.real + R.gen) : 0, gone, R.still,
               R.held, R.evened, 1 / R.period, hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], shown ? shown - 1 : 0,
               pct(fl, nfl, 0.5f), nfl, R.flow_px, R.gen ? 100 * R.stale / R.gen : 0, gpu, pct(R.gpu_ms, ngpu, 0.95f),
               100.0 * gpu * R.gen / 1000 / dt, pct(R.ts, R.nt, 0.5f), pct(R.lat, nlat, 0.5f), pct(R.lat, nlat, 0.95f));
    }
    R.stat_t0 = now;
    R.real = R.gen = R.still = R.held = R.evened = R.stale = R.nt = 0;
    __atomic_store_n(&R.ngpu, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&R.nlat, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&R.npres, 0, __ATOMIC_RELEASE);
}

// (the link's thread) one refresh: unless a real frame comes in time, one predicted from B to the time it will be shown
static void race_tick(double tick, double period) {
    if (period > 0.002 && period < 0.05) R.period = period;
    if (!g.race || g.state <= 0) return;
    double frame_due = tick + R.period - R.margin;                // (decided late: a real frame may still come)
    for (;;) {
        double now = now_s();
        if (now >= frame_due || R.shown_at >= tick) break;
        dispatch_semaphore_wait(R.sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)((frame_due - now) * 1e9)));
    }
    pthread_mutex_lock(&R.present);
    double now = now_s();
    int done = 0, snap_a = 0, snap_b = 0;
    VXNpuSet *snap_s = nil;
    // (the predicted frames' rate: VXRT_NPU_RACE_HZ, else half a 100 Hz+ display's: even 2-refresh steps, half the GPU)
    double gap = R.hz > 0 ? 1 / R.hz : R.period < 0.01 ? 2 * R.period : R.period;
    if (R.shown_at < tick && now - R.shown_at >= gap - 0.002) {
        os_unfair_lock_lock(&R.lock);
        VXNpuSet *S = R.set;
        int ok = S && S->mm && R.b >= 0 && R.a >= 0 && R.flow_seq && R.interval > 0 &&
                 R.b_seq - R.flow_bseq <= 3;                       // (a flow up to 3 frames old: it may take longer than a frame)
        // how far on: B was presented at tb (and will be on screen as long after it as this one)
        double t = ok ? (now - R.tb) / R.interval : 0;
        // (even steps: no prediction a refresh before the next real frame is due; it would leave a 1-refresh step)
        double due = R.last_t + R.interval - now;
        if (!ok || t <= 0 || t > R.max_t + 1e-3 || g.race_nowarp) { R.held++; os_unfair_lock_unlock(&R.lock); }
        else if (R.even && due > 0 && due < R.period) { R.evened++; os_unfair_lock_unlock(&R.lock); }
        else if ((t - R.shown_t) * R.flow_px < R.min_px) { R.still++; os_unfair_lock_unlock(&R.lock); }
        else {
            int slot = R.b, a = R.a, fresh = R.flow_bseq == R.b_seq;
            uint32_t d = (uint32_t)R.flow_dst;
            // (once per flow: a previous pair's flow goes on with the motion map made with it, from A's pixels)
            int new_mm = R.mm_flow_seq != R.flow_seq;
            R.mm_b_seq = R.b_seq; R.mm_flow_seq = R.flow_seq;
            os_unfair_lock_unlock(&R.lock);
            S->RP.t = (float)t;
            if (race_present(KIND_RACE, S, slot, a, d, new_mm, 0)) {
                R.shown_at = now; R.shown_t = t;
                R.gen++;
                if (!fresh) R.stale++;
                if (R.nt < 512) R.ts[R.nt++] = (float)t;
                done = 1;
                snap_s = S; snap_a = a; snap_b = slot;
            }
        }
    }
    pthread_mutex_unlock(&R.present);
    if (done) snapshot(snap_s, (uint32_t)snap_a, 0, (uint32_t)snap_b);   // (not holding up real frames)
    race_stats(now_s());
}

@interface VXNpuTick : NSObject
@end
@implementation VXNpuTick
- (void)tick:(CADisplayLink *)link {
    @autoreleasepool { race_tick(now_s(), link.targetTimestamp - link.timestamp); }
}
@end

static float race_hz(float hz) {
    if (hz >= 0) R.hz = hz;
    return R.hz;
}

// (the game thread) the display link goes, and its thread (waited for: its tick may be running)
static void race_stop(void) {
    if (!R.loop) return;
    CFRunLoopRef loop = R.loop;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    CFRunLoopPerformBlock(loop, kCFRunLoopDefaultMode, ^{
        [R.link invalidate];
        R.link = nil;
        CFRunLoopStop(CFRunLoopGetCurrent());
        dispatch_semaphore_signal(done);
    });
    CFRunLoopWakeUp(loop);
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 1000000000));
    R.loop = NULL;
    R.layer = nil;
    vx_log("NPU mode (race): display link off");
}

// (the game thread) a display link (CADisplayLink, which runs a ProMotion display at 120 Hz where a CVDisplayLink
// ticks at 60) for the screen the window is on, on a thread of its own
static void race_start(CAMetalLayer *layer) {
    if (R.layer == layer && R.loop) return;
    race_stop();
    if (!R.sem) {
        R.sem = dispatch_semaphore_create(0);
        const char *m = getenv("VXRT_NPU_RACE_MARGIN_MS"), *x = getenv("VXRT_NPU_RACE_MAX"), *hz = getenv("VXRT_NPU_RACE_HZ"),
                   *px = getenv("VXRT_NPU_RACE_MIN_PX");
        R.margin = (m ? atof(m) : 4) / 1000;
        R.max_t = x ? atof(x) : 1.25;
        R.min_px = px ? atof(px) : 0.3;
        R.even = !getenv("VXRT_NPU_RACE_EVEN") || atoi(getenv("VXRT_NPU_RACE_EVEN"));
        if (hz && !R.hz) R.hz = (float)atof(hz);
    }
    NSScreen *screen = nil;
    for (NSWindow *w in [NSApp windows])                           // (reading: no change to the window)
        if (w.contentView.layer == layer || w.contentView.layer == layer.superlayer) screen = w.screen;
    if (!screen) screen = [NSScreen mainScreen];
    float hz = (float)screen.maximumFramesPerSecond;               // (real frames show at the display's rate; R.hz caps predictions)
    dispatch_semaphore_t started = dispatch_semaphore_create(0);
    NSThread *th = [[NSThread alloc] initWithBlock:^{
        VXNpuTick *target = [VXNpuTick new];
        CADisplayLink *link = [screen displayLinkWithTarget:target selector:@selector(tick:)];
        link.preferredFrameRateRange = CAFrameRateRangeMake(hz, hz, hz);
        [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
        R.link = link;
        R.loop = CFRunLoopGetCurrent();
        dispatch_semaphore_signal(started);
        CFRunLoopRun();
        (void)target;
    }];
    th.name = @"vxrt.npu.race";
    th.qualityOfService = NSQualityOfServiceUserInteractive;
    [th start];
    dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
    R.layer = layer;
    vx_log("NPU mode (race): display link on (%s, %.0f Hz; decided %.1f ms before the next refresh, at most %.2f of an "
           "interval on, a frame when the scene moved %.1f px)", screen.localizedName.UTF8String ?: "?", hz, R.margin * 1000,
           R.max_t, R.min_px);
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
    int race = g.race && S->mode == NPU_MODE_FLOW;
    if (race) race_start(layer);
    // (more frames waiting than the slots allow: this one is not shown)
    int rslot = race ? race_slot(S) : -1;
    if (race ? rslot < 0 : __atomic_load_n(&g.pending, __ATOMIC_ACQUIRE) >= 5) {
        dispatch_async(g.q, ^{ g.dropped++; });
        if (waitable) [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { (void)done; vx_release_semaphore(waitable, 1); }];
        [cb commit];
        return;
    }
    uint32_t slot = race ? (uint32_t)rslot : g.next_slot++ % NSRC;
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
    if (!race) __atomic_add_fetch(&g.pending, 1, __ATOMIC_ACQ_REL);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        if (waitable) vx_release_semaphore(waitable, 1);
        double t = now_s();
        dispatch_async(g.q, ^{ if (race) handle_race(S, slot, has_yuv, t); else handle(S, slot, has_yuv, t); });
    }];
    [cb commit];
}
