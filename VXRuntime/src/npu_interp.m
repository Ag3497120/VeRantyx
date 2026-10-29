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
// 120 Hz, half the GPU's work of every refresh; VXRT_NPU_RACE_HZ sets another rate). The flow is made every 3rd real
// frame (every 2nd, or every one, as the scene moves faster) on the Neural Engine, which VideoToolbox helps with the GPU
// (in the background, newest pair first; a flow up to 6 frames old and its motion map go on with B); each prediction costs only a GPU warp at half resolution (2x2 blocks): the motion map,
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
#if VX_XROS
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif
#include "npu_interp.h"
#include "npu_proto.h"
#include "warp_core.h"

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
// (and, with the game's own motion vectors: frame pixels per motion vector unit, which way depth is nearer)
typedef struct { float w, h, sx, sy, t, thr, fb, fbk; uint32_t idx_bits, kw, kh, mvmode; float msx, msy; uint32_t rev, pad; } npu_race_params;

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
    "struct R { float w, h, sx, sy, t, thr, fb, fbk; uint idx_bits, kw, kh, mvmode; float msx, msy; uint rev, pad; };\n"
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
    // (in front: the fastest, or with the game's depth the nearest)
    "    float pr = e.mvmode ? (e.rev ? v.w - 1 : 2 - v.w) * 8191 : length(v.xy) * 4;\n"
    "    uint mq = min(uint(max(pr, 0.0)), (1u << (32 - e.idx_bits)) - 1);\n"
    "    atomic_fetch_max_explicit(&key[uint(t.y) * e.kw + uint(t.x)], (mq << e.idx_bits) | (g.y * e.kw + g.x + 1), memory_order_relaxed);\n"
    "}\n"
    // the HUD apart: how much of the final frame F equals the scene before the UI S (FSR2's output), on a 4-pixel grid:
    // cnt[0] all, [1] within 2/255, [2] within 8/255
    "kernel void npu_hud_diff(texture2d<float> f [[texture(0)]], texture2d<float> sc [[texture(1)]], device atomic_uint *cnt [[buffer(0)]],\n"
    "                         uint2 g [[thread_position_in_grid]]) {\n"
    "    uint2 q = g * 4;\n"
    "    if (q.x >= f.get_width() || q.y >= f.get_height()) return;\n"
    "    float3 d = abs(f.read(q).rgb - sc.read(q).rgb);\n"
    "    float m = max(d.x, max(d.y, d.z));\n"
    "    atomic_fetch_add_explicit(&cnt[0], 1, memory_order_relaxed);\n"
    "    if (m <= 2.0 / 255) atomic_fetch_add_explicit(&cnt[1], 1, memory_order_relaxed);\n"
    "    if (m <= 8.0 / 255) atomic_fetch_add_explicit(&cnt[2], 1, memory_order_relaxed);\n"
    "}\n"
    // the predicted scene (warped S) with the latest UI on it, unwarped: the UI is F - S (zero where there is none); a
    // pixel the UI covers nearly opaquely (a large difference) is F itself
    "fragment float4 npu_hud_fs(V in [[stage_in]], texture2d<float> wp [[texture(0)]], texture2d<float> f [[texture(1)]],\n"
    "                           texture2d<float> sc [[texture(2)]]) {\n"
    "    uint2 p = min(uint2(in.uv * float2(f.get_width(), f.get_height())), uint2(f.get_width(), f.get_height()) - 1);\n"
    "    float3 fc = f.read(p).rgb, ui = fc - sc.read(p).rgb, w = wp.read(p).rgb;\n"
    "    float m = max(abs(ui.x), max(abs(ui.y), abs(ui.z)));\n"
    "    float3 o = m <= 2.0 / 255 ? w : saturate(w + ui);\n"
    "    return float4(mix(o, fc, smoothstep(0.25, 0.5, m)), 1);\n"
    "}\n"
    // the depth as R32Float, from a depth texture or a one-channel float one
    "kernel void npu_depth_from_depth(depth2d<float> d [[texture(0)]], texture2d<float, access::write> o [[texture(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x < o.get_width() && g.y < o.get_height()) o.write(float4(d.read(g)), g);\n"
    "}\n"
    "kernel void npu_depth_from_color(texture2d<float> d [[texture(0)]], texture2d<float, access::write> o [[texture(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x < o.get_width() && g.y < o.get_height()) o.write(float4(d.read(g).r), g);\n"
    "}\n"
    // the motion vectors' unit and signs: for each of 8 candidates (unit: a frame's width/height per unit, or frame pixels
    // per render pixel; each axis either sign) and no motion (8), how well B matches A where the vectors point, at
    // sample points 16 frame pixels apart; out[point * 9 + candidate]
    // (out[point * 10 + 9]: the depth there, to tell which way it goes)
    "kernel void npu_mv_calib(texture2d<float> b [[texture(0)]], texture2d<float> a [[texture(1)]], texture2d<float> mv [[texture(2)]],\n"
    "                         texture2d<float> dp [[texture(3)]],\n"
    "                         device float *out [[buffer(0)]], constant R &e [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    uint gw = uint(e.w) / 16, gh = uint(e.h) / 16;\n"
    "    if (g.x >= gw || g.y >= gh) return;\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    constexpr sampler sn(filter::nearest, address::clamp_to_edge);\n"
    "    float2 sz = float2(e.w, e.h), p = float2(g) * 16 + 8, v = mv.sample(sn, p / sz, level(0)).xy;\n"
    "    float2 rp = sz / float2(mv.get_width(), mv.get_height());\n"
    "    for (uint c = 0; c < 9; c++) {\n"
    "        float2 k = c == 8 ? 0 : (c & 4 ? rp : sz) * float2(c & 1 ? -1 : 1, c & 2 ? -1 : 1);\n"
    "        float2 off = v * k;\n"
    "        float err = 0;\n"
    "        for (int y = -2; y <= 2; y += 2) for (int x = -2; x <= 2; x += 2) {\n"
    "            float2 q = p + float2(x, y);\n"
    "            err += dot(abs(b.sample(s, q / sz, level(0)).rgb - a.sample(s, (q + off) / sz, level(0)).rgb), 1);\n"
    "        }\n"
    "        out[(g.y * gw + g.x) * 10 + c] = all(isfinite(off)) ? err : 1e9;\n"
    "    }\n"
    "    out[(g.y * gw + g.x) * 10 + 9] = dp.sample(sn, p / sz, level(0)).r;\n"
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
    // the game's motion vectors and depth (R32Float) with each frame, when the D3D12 front end found them
    id<MTLTexture> smv[NSRC], sdp[NSRC];
    uint8_t scene[NSRC];
    uint32_t sconst[NSRC][32];     // (and FSR2's constants with them: the projection; shas_const[slot] when found)
    uint8_t shas_const[NSRC];
    id<MTLBuffer> rkeys;
    npu_race_params RP;
    // FSR2's constants as read (fsr: 1 when plausible): tan(vertical FOV/2), device-to-view depth, motion vector scale
    uint8_t fsr[NSRC];
    float fsr_tan[NSRC], fsr_d2v[NSRC][4], fsr_mvs[NSRC][2];
    double ctime[NSRC];            // (each frame's content time: when the game presented it, after any pacing wait)
    vxw_params WP;                 // (the warp core's parameters of the prediction shown last; WA: its A)
    int WA;
    int helper;                    // the optical flow's helper started (race: only when no motion vectors come)
    id<MTLTexture> scol[NSRC];     // the HUD apart: each frame's scene before the UI (FSR2's output), shud[slot] when taken
    uint8_t shud[NSRC];
    id<MTLTexture> hwarp;          // (the warped scene, before the UI goes on it)
    uint32_t hud_n;
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
    id<MTLComputePipelineState> yuv_ps, flow_in_ps, splat_ps, mm_ps, rsplat_ps, resolve_ps, depth_d_ps, depth_c_ps, calib_ps, hud_diff_ps;
    id<MTLRenderPipelineState> hud_ps[2];          // (the HUD's composite: the layer's format, BGRA8 for recordings)
    volatile int race_hud;         // ("hud" in the switch file: the scene warped without the UI, the UI composited on top)
    volatile float hud_eq;         // (the share of the final frame equal to FSR2's output, last measured; -1 none)
    vxw_core *wc;                  // race with the game's motion vectors: the warp core (warp_core.m)
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
    volatile int race_noflow, race_nowarp, race_nomv;
    volatile int switch_on;        // (the switch file's state: the D3D12 front end looks for nothing while off)
    volatile float race_real_cap;
    float real_fps;                // (VXRT_NPU_REAL_FPS: race paced)
    volatile float race_fps;       // (the same, or the switch file's "fpsN")  // (race: real frames shown at most this often, the rest only counted: a slow game simulated)
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
    if (!*path) { g.switch_on = 1; return 1; }
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
                    g.race_noflow = g.race_nowarp = g.race_nomv = 0;
                    int hud = 0;
                    g.race_real_cap = 0;
                    g.race_fps = g.real_fps;
                    while (fscanf(f, "%31s", opt) == 1) {
                        if (!strcmp(opt, "noflow")) g.race_noflow = 1;
                        else if (!strcmp(opt, "nowarp")) g.race_nowarp = 1;
                        else if (!strcmp(opt, "nomv")) g.race_nomv = 1;  // (the optical flow even where the game's vectors are)
                        else if (!strcmp(opt, "hud")) hud = 1;           // (the scene warped without the UI: FSR2's output)
                        else if (!strncmp(opt, "real", 4)) g.race_real_cap = (float)atof(opt + 4);   // ("real15": low fps simulated)
                        else if (!strncmp(opt, "fps", 3)) g.race_fps = (float)atof(opt + 3);        // ("fps20": paced, as VXRT_NPU_REAL_FPS)
                        else if (atof(opt) != race_hz(-1)) race_hz((float)atof(opt));
                    }
                    g.race_hud = hud || (getenv("VXRT_NPU_RACE_HUD") && atoi(getenv("VXRT_NPU_RACE_HUD")));
                }
                if (!strcmp(word, "interpolate") && g.interp_ok) mode = NPU_MODE_INTERPOLATE, race = 0;
            }
            fclose(f);
        }
        if (now != on || mode != g.mode || race != g.race)
            vx_log("NPU mode: %s (%s)", now ? (race ? "race" : mode == NPU_MODE_FLOW ? "extrapolate" : "interpolate") : "switched off", path);
        on = now;
        g.switch_on = now;
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
    // (VXRT_NPU_REAL_FPS=N, race: the game's real frames paced at about N a second, on the refresh grid; predictions fill
    //  every refresh between them)
    const char *cap = getenv("VXRT_NPU_REAL_FPS");
    if (g.race && cap && atof(cap) > 0) g.race_fps = g.real_fps = (float)atof(cap);
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
    S->RP.sx = S->E.sx; S->RP.sy = S->E.sy;
    vx_log("NPU mode: %ux%u extrapolated from optical flow of %ux%u frames (%ux%u vectors, %.2f pixels a unit; started in %.0f ms)",
           S->w, S->h, S->tw, S->th, r->flow_w, r->flow_h, r->flow_scale, r->start_ms);
    __atomic_store_n(&S->ready, 1, __ATOMIC_RELEASE);
}

// (the game thread, a new set) race's half-resolution buffers of the optical flow's path and its parameters; the game's
// motion vectors need none of these (the warp core keeps its own), nor the flow's helper
static void race_buffers(VXNpuSet *S) {
    uint32_t kw = (S->w + 1) / 2, kh = (S->h + 1) / 2, kbits = 1;
    while ((1u << kbits) <= kw * kh) kbits++;
    const char *fb = getenv("VXRT_NPU_RACE_FB");
    S->RP = (npu_race_params){ (float)S->w, (float)S->h, 0, 0, 0, 2.0f / 255, fb ? (float)atof(fb) : 3.0f, 0.25f, kbits, kw, kh, 0, 0, 0,
                                 !getenv("VXRT_NPU_RACE_DEPTH_REVERSED") || atoi(getenv("VXRT_NPU_RACE_DEPTH_REVERSED")), 0 };
    MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:kw height:kh mipmapped:NO];
    md.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    md.storageMode = MTLStorageModePrivate;
    S->mm = [g.dev newTextureWithDescriptor:md];
    S->rm = [g.dev newTextureWithDescriptor:md];
    S->rkeys = [g.dev newBufferWithLength:(NSUInteger)kw * kh * 4 options:MTLResourceStorageModePrivate];
    if (!S->mm || !S->rm || !S->rkeys) { vx_log("NPU mode: no buffers for race"); S->mm = nil; }
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
    g.depth_d_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_depth_from_depth"] error:&err];
    g.depth_c_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_depth_from_color"] error:&err];
    g.calib_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_mv_calib"] error:&err];
    g.hud_diff_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_hud_diff"] error:&err];
    {
        MTLRenderPipelineDescriptor *hd = [MTLRenderPipelineDescriptor new];
        hd.vertexFunction = [lib newFunctionWithName:@"npu_vs"];
        hd.fragmentFunction = [lib newFunctionWithName:@"npu_hud_fs"];
        hd.colorAttachments[0].pixelFormat = layer.pixelFormat;
        g.hud_ps[0] = [dev newRenderPipelineStateWithDescriptor:hd error:&err];
        hd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        g.hud_ps[1] = [dev newRenderPipelineStateWithDescriptor:hd error:&err];
    }
    g.hud_eq = -1;
    g.flow_in_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_to_flow"] error:&err];
    g.splat_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_splat"] error:&err];
    g.yuv_ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"npu_to_yuv"] error:&err];
    if (!g.real_ps || !g.interp_ps || !g.yuv_ps || !g.extrap_ps || !g.flow_in_ps || !g.splat_ps || !g.race_ps || !g.mm_ps || !g.rsplat_ps || !g.resolve_ps || !g.depth_d_ps || !g.depth_c_ps || !g.calib_ps) { vx_log("NPU mode: no pipeline: %s", err.localizedDescription.UTF8String); g.state = -1; return 0; }
    if (g.race && !(g.wc = vxw_create((__bridge void *)dev))) vx_log("NPU mode (race): the warp core did not compile: no predictions from the game's motion vectors");
    g.queue = [dev newCommandQueue];
    g.q = dispatch_queue_create("vxrt.npu.display", DISPATCH_QUEUE_SERIAL);
    g.init_q = dispatch_queue_create("vxrt.npu.init", DISPATCH_QUEUE_SERIAL);
    g.flow_q = dispatch_queue_create("vxrt.npu.flow", DISPATCH_QUEUE_SERIAL);
    g.dev = dev;
    return 1;
}

static void draw(id<MTLCommandBuffer> cb, id<MTLTexture> target, int kind, VXNpuSet *S, uint32_t slot, int png) {
    if (kind == KIND_RACE && S->RP.mvmode && g.race_hud && S->shud[slot] && g.hud_eq >= 0.5f && g.hud_ps[png ? 1 : 0]) {
        // (the HUD apart: the scene before the UI warped alone (no held blocks: nothing guesses where the HUD is), then
        //  B's UI, unwarped, on it)
        if (!S->hwarp) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:S->w height:S->h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            td.storageMode = MTLStorageModePrivate;
            S->hwarp = [g.dev newTextureWithDescriptor:td];
        }
        vxw_params P = S->WP;
        P.flags &= ~(uint32_t)VXW_STILL;
        if (S->hwarp && vxw_encode(g.wc, (__bridge void *)cb, &P, (__bridge void *)S->scol[slot], NULL, (__bridge void *)S->smv[slot],
                                   (__bridge void *)S->sdp[slot], (__bridge void *)S->hwarp)) {
            MTLRenderPassDescriptor *hp = [MTLRenderPassDescriptor renderPassDescriptor];
            hp.colorAttachments[0].texture = target;
            hp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            hp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> he = [cb renderCommandEncoderWithDescriptor:hp];
            [he setRenderPipelineState:g.hud_ps[png ? 1 : 0]];
            [he setFragmentTexture:S->hwarp atIndex:0];
            [he setFragmentTexture:S->copy[slot] atIndex:1];
            [he setFragmentTexture:S->scol[slot] atIndex:2];
            [he drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [he endEncoding];
            return;
        }
    }
    if (kind == KIND_RACE && S->RP.mvmode) {                       // (the game's motion vectors: the warp core, S->WP)
        if (vxw_encode(g.wc, (__bridge void *)cb, &S->WP, (__bridge void *)S->copy[slot], S->WA >= 0 ? (__bridge void *)S->copy[S->WA] : NULL,
                       (__bridge void *)S->smv[slot], (__bridge void *)S->sdp[slot], (__bridge void *)target)) return;
        kind = KIND_REAL;                                          // (it could not: B as it is)
    }
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
    // (a real frame with the game's motion vectors and depth: those too, as they are (render size): fNNNNN_mv_WxH.rg16f
    //  or .rg32f, fNNNNN_depth_WxH.r32f - the data to train motion extrapolation or inpainting on)
    id<MTLBuffer> mvb = nil, dpb = nil;
    NSString *mvpath = nil, *dppath = nil;
    id<MTLTexture> smv = slot < NSRC ? S->smv[slot] : nil, sdp = slot < NSRC ? S->sdp[slot] : nil;
    if (kind == KIND_REAL && slot < NSRC && S->scene[slot] && smv && sdp) {
        uint32_t bpp = smv.pixelFormat == MTLPixelFormatRG32Float ? 8 : 4, mw = (uint32_t)smv.width, mh = (uint32_t)smv.height;
        mvb = [g.dev newBufferWithLength:(NSUInteger)mw * mh * bpp options:MTLResourceStorageModeShared];
        dpb = [g.dev newBufferWithLength:(NSUInteger)sdp.width * sdp.height * 4 options:MTLResourceStorageModeShared];
        if (mvb && dpb) {
            id<MTLBlitCommandEncoder> b2 = [cb blitCommandEncoder];
            [b2 copyFromTexture:smv sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(mw, mh, 1)
                       toBuffer:mvb destinationOffset:0 destinationBytesPerRow:mw * bpp destinationBytesPerImage:(NSUInteger)mw * mh * bpp];
            [b2 copyFromTexture:sdp sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(sdp.width, sdp.height, 1)
                       toBuffer:dpb destinationOffset:0 destinationBytesPerRow:sdp.width * 4 destinationBytesPerImage:sdp.width * sdp.height * 4];
            [b2 endEncoding];
            mvpath = [NSString stringWithFormat:@"%s/f%05u_mv_%ux%u.%s", dir, k, mw, mh, bpp == 8 ? "rg32f" : "rg16f"];
            dppath = [NSString stringWithFormat:@"%s/f%05u_depth_%lux%lu.r32f", dir, k, (unsigned long)sdp.width, (unsigned long)sdp.height];
        }
    }
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        FILE *f = fopen(rawpath.UTF8String, "wb");
        if (f) { fwrite(out.contents, 1, out.length, f); fclose(f); }
        if (mvpath && (f = fopen(mvpath.UTF8String, "wb"))) { fwrite(mvb.contents, 1, mvb.length, f); fclose(f); }
        if (dppath && (f = fopen(dppath.UTF8String, "wb"))) { fwrite(dpb.contents, 1, dpb.length, f); fclose(f); }
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
    if (S->RP.mvmode || !S->mm) return;                            // (the warp core does its own)
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
    double interval, period;       // between real frames shown (ready times), between refreshes
    double ginterval, glast_t;     // between the game's frames (all, shown or not: the game's motion vectors' interval)
    double shown_at, shown_t;      // when the last frame (real or not) was presented, and how far on from B
    int want_a, want_b, run_a, run_b, flow_busy;   // the flow's pairs (waiting, running)
    int flow_dst, flow_b; uint32_t flow_seq;       // the newest flow done: its dst slot, its pair's B, a count
    uint32_t want_bseq, flow_bseq;                 // which B (b_seq) the waiting pair and the newest flow end at
    // the game's motion vectors: which slots have them, whether their unit is known (then no flow), its votes
    uint32_t scene;
    int mv_ok, mm_mv, calib_busy, calib_frames, calib_done;   // (mv_ok: 2 FSR2's scale, 1 the vote's)
    uint32_t votes[9], moving;
    uint32_t mvgen;
    float flow_px;                                 // its motion, frame pixels an interval (98th percentile)
    float mv_px;                                   // the game's vectors' largest motion (of the last motion map made)
    uint32_t mm_b_seq, mm_flow_seq;                // what the motion map was made from
    // statistics (every 5 s, the tick's thread; frames, superseded: the handlers')
    uint32_t frames, real, gen, still, held, evened, stale, nflow, ngpu, nlat, nt, npres, capped;
    double even;                   // no prediction this many refreshes before a real frame is due
    float flow_ms[512], gpu_ms[512], lat[512], ts[512];
    double pres[1024];
    double stat_t0;
    dispatch_semaphore_t sem;
    CADisplayLink *link;
    CFRunLoopRef loop;
    __unsafe_unretained CAMetalLayer *layer;
    double margin, max_t, min_px;
    float hz;
    // paced (VXRT_NPU_REAL_FPS=N): the game's frames started every k refreshes (k = the refresh rate / N, rounded), each
    // shown when the display reaches its content time + D (D: the latency envelope, whole refreshes), predictions between
    int paced;
    float cap;
    double pace_next, pace_i;      // (the game thread's) the next frame's start, the frames' interval
    double next_content;           // (the game thread's) the content time of the frame it is making now
    double link_ts;                // the display link's time of the refresh being shown (its vsync)
    double rec_content;            // (the content time of the frame being presented, for recordings)
    double present_at;             // (paced: the vsync a present is scheduled for, presentDrawable:atTime:; 0 at once)
    double shown_T;                // (paced: the vsync the last present was for)
    double vsync;                  // a refresh's time (the link's), to start frames on the refresh grid
    double dl, d;                  // the latency (content time to ready, p90 of the last 32), D
    float lring[32]; uint32_t nl;
    double d_down;                 // (when D last stepped down)
    // the frames ready and not yet due, oldest first (with D longer than a frame interval several wait at once)
    int pq[4], pq_in[4], pq_scene[4], npq;
    double pq_c[4];
    int pend;                      // (bit() of the queue's slots for race_slot: -1, kept for the old masks)
    double ca, cb;                 // A's and B's content times
    int b_shown;                   // B itself (t 0) shown
    uint32_t pend_dropped, nlat2, late_real;
    float lat2[512];
    double noscene_t;              // since when no frame came with the game's motion vectors
} R = { .lock = OS_UNFAIR_LOCK_INIT, .present = PTHREAD_MUTEX_INITIALIZER, .last = -1, .a = -1, .b = -1, .want_a = -1,
        .want_b = -1, .run_a = -1, .run_b = -1, .flow_dst = -1, .flow_b = -1, .period = 1.0 / 60, .pend = -1 };

static uint32_t bit(int slot) { return slot >= 0 ? 1u << slot : 0; }

static void race_reset(VXNpuSet *S) {                              // (R.lock held) a new set: nothing of the old one
    R.set = S;
    R.inflight = R.has_in = 0;
    R.last = R.a = R.b = R.want_a = R.want_b = R.pend = -1;
    R.npq = 0;
    R.b_shown = 0; R.noscene_t = 0;
    R.interval = 0;
    R.flow_dst = R.flow_b = -1;
    R.flow_seq = R.mm_flow_seq = R.mm_b_seq = 0;
    R.scene = 0; R.mv_ok = 0; R.mm_mv = -1; R.calib_frames = 0; R.calib_done = 0; R.moving = 0;
    memset(R.votes, 0, sizeof R.votes);
    R.b_seq = 1;
}

// The scene's motion vectors and depth from the D3D12 front end (the upscaler's inputs; the last dispatch of a frame
// that read both): taken with the frame at its present, copied into the slot's textures (depth as R32Float).
static struct {
    os_unfair_lock lock;
    __strong id<MTLTexture> mv, dp;
    uint64_t cs, cand[16];
    int off, told, ctold, prio, ncand;
    uint32_t consts[32];
    int has_consts;
    __strong id<MTLTexture> col;   // (the HUD apart: FSR2's output, the scene before the UI)
    uint64_t col_cs;
    int col_told;
} SC = { .lock = OS_UNFAIR_LOCK_INIT };

int npu_interp_wants_scene_color(uint32_t *w, uint32_t *h) {
    VXNpuSet *S = g.set;
    if (!g.race_hud || !npu_interp_wants_scene() || !S) return 0;
    *w = S->w; *h = S->h;
    return 1;
}

void npu_interp_scene_color(void *texture, uint64_t cs) {
    id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
    os_unfair_lock_lock(&SC.lock);
    SC.col = t; SC.col_cs = cs;
    int tell = !SC.col_told++;
    os_unfair_lock_unlock(&SC.lock);
    if (tell) vx_log("NPU mode (race, hud): the scene before the UI: FSR2's output %lux%lu format %lu from cs %016llx",
                     (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long long)cs);
}

int npu_interp_wants_scene(void) {
    static int off = -1;                                           // (VXRT_NPU_RACE_MV=0: the optical flow only)
    if (off < 0) SC.off = off = getenv("VXRT_NPU_RACE_MV") && !atoi(getenv("VXRT_NPU_RACE_MV"));
    return g.state > 0 && g.race && !off && !g.race_nomv && g.switch_on;
}

// FSR2's constants. Layouts:
//   1. FSR 2.1/2.2 (ffx_fsr2 cbFSR2): render size, max render size, display size, input colour size, luma mip size, mip
//      level, frame index, device-to-view depth x4 [12], jitter [16], motion vector scale [18], downscale, jitter
//      cancellation, pre-exposure x2, tan(half the vertical FOV) [26], ...
//   2. FSR 2.0 (as Cyberpunk 2077 has it, seen in its constant buffer): render size, display size, luma mip size [4],
//      mip level, frame index, 1/display size [8], jitter [10], device-to-view [12] (FLT_EPSILON, -1, the near plane,
//      FLT_EPSILON: reversed infinite depth, distance = near / d), depth clip / lock status / reactive mask scales
//      [16-21], motion vector scale [22], downscale [24], pre-exposure, tan(half the vertical FOV) [27], ...
// d2v comes out as warp_core wants it: [0] z_b, [1] z_a (distance = [1] / (d - [0])), [2] tan_x, [3] tan_y.
static int fsr2_parse(const uint32_t *w, float *tan_half_fov_y, float d2v[4], float mvs[2], uint32_t disp[2]) {
    const float *f = (const float *)w;
    // 2.0: the luma mip's size small, 1/display size and the downscale factor consistent
    if (w[2] >= w[0] && w[3] >= w[1] && w[4] < w[2] && w[5] < w[3] && fabsf(f[8] * w[2] - 1) < 1e-3f && fabsf(f[9] * w[3] - 1) < 1e-3f &&
        fabsf(f[24] - (float)w[0] / w[2]) < 0.02f && f[27] > 0.05f && f[27] < 5) {
        float near = f[14] > 1e-4f && f[14] < 10 ? f[14] : 1;
        *tan_half_fov_y = f[27];
        d2v[0] = 0; d2v[1] = near; d2v[2] = f[27] * w[2] / w[3]; d2v[3] = f[27];
        mvs[0] = f[22]; mvs[1] = f[23];
        disp[0] = w[2]; disp[1] = w[3];
        return 2;
    }
    // 2.1/2.2
    float t = f[26];
    if (t > 0.05f && t < 5.0f && isfinite(f[12]) && isfinite(f[13]) && f[14] > 0.01f && f[15] > 0.01f && w[4] >= w[0] && w[5] >= w[1]) {
        *tan_half_fov_y = t;
        memcpy(d2v, f + 12, 16);
        mvs[0] = f[18]; mvs[1] = f[19];
        disp[0] = w[4]; disp[1] = w[5];
        return 1;
    }
    return 0;
}

// Which of a frame's candidates is FSR2's own input: several dispatches read a two-channel float texture and a depth of
// the same size (FSR2's first pass reads the game's motion vectors and depth; its later passes read its dilated copies,
// R32Float depth; other effects (motion blur, TAA-like passes) read them too, without FSR2's constants). Priority: FSR2's
// constants found (2) + a depth texture rather than a colour one (1); the first of the highest priority in a frame wins.
static int scene_prio(id<MTLTexture> dp, const uint32_t *consts) {
    MTLPixelFormat f = dp.pixelFormat;
    int depth = f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatDepth16Unorm;
    return (consts ? 2 : 0) + depth;
}

void npu_interp_scene(void *mv_, void *dp_, uint64_t cs, const uint32_t *consts) {
    id<MTLTexture> mv = (__bridge id<MTLTexture>)mv_, dp = (__bridge id<MTLTexture>)dp_;
    int prio = scene_prio(dp, consts);
    os_unfair_lock_lock(&SC.lock);
    // (each distinct candidate logged once: the shader, the sizes and formats, its priority)
    int seen = 0;
    for (int k = 0; k < SC.ncand; k++) if (SC.cand[k] == cs) seen = 1;
    if (!seen && SC.ncand < 16) SC.cand[SC.ncand++] = cs;
    int took = 0;
    if (!SC.mv || prio > SC.prio) {
        SC.mv = mv; SC.dp = dp; SC.cs = cs; SC.prio = prio;
        SC.has_consts = consts != NULL;
        if (consts) memcpy(SC.consts, consts, sizeof SC.consts);
        took = 1;
    }
    int tell = took && !SC.told++, ctell = took && consts && !SC.ctold++;
    os_unfair_lock_unlock(&SC.lock);
    if (!seen)
        vx_log("NPU mode (race): candidate for the scene's motion vectors and depth: cs %016llx, %lux%lu format %lu, depth %lux%lu "
               "format %lu, FSR2 constants %s (priority %d)", (unsigned long long)cs, (unsigned long)mv.width, (unsigned long)mv.height,
               (unsigned long)mv.pixelFormat, (unsigned long)dp.width, (unsigned long)dp.height, (unsigned long)dp.pixelFormat,
               consts ? "found" : "not found", prio);
    if (ctell) {
        char hex[32 * 9 + 1] = "";
        for (int k = 0; k < 32; k++) snprintf(hex + 9 * k, 10, "%08x ", consts[k]);
        float th, d2v[4], mvs[2]; uint32_t disp[2];
        int lay = fsr2_parse(consts, &th, d2v, mvs, disp);
        vx_log("NPU mode (race): FSR2's constants with them: %s", hex);
        if (lay) vx_log("NPU mode (race): read as FSR %s: display %ux%u, tan(vertical FOV/2) %.4f (%.1f degrees), device-to-view "
                        "depth %g %g %g %g, motion vector scale %g %g", lay == 1 ? "2.1/2.2" : "2.0 (Cyberpunk 2077's)", disp[0], disp[1], th,
                        2 * atanf(th) * 57.29578f, d2v[0], d2v[1], d2v[2], d2v[3], mvs[0], mvs[1]);
        else vx_log("NPU mode (race): those constants are not laid out as FSR 2.0-2.2's (no plausible field of view)");
    } else if (!consts && !SC.ctold && tell) vx_log("NPU mode (race): no FSR2 constants found with them (root constants, root CBVs, CBV tables)");
    if (tell)
        vx_log("NPU mode (race): the scene's motion vectors (%lux%lu, format %lu) and depth (%lux%lu, format %lu) from the "
               "dispatch of cs %016llx", (unsigned long)mv.width, (unsigned long)mv.height, (unsigned long)mv.pixelFormat,
               (unsigned long)dp.width, (unsigned long)dp.height, (unsigned long)dp.pixelFormat, (unsigned long long)cs);
}

// (the game thread, in the present's command buffer) this frame's motion vectors and depth into the slot's textures
static int scene_capture(id<MTLCommandBuffer> cb, VXNpuSet *S, uint32_t slot) {
    os_unfair_lock_lock(&SC.lock);
    id<MTLTexture> mv = SC.mv, dp = SC.dp, col = SC.col;
    SC.mv = SC.dp = SC.col = nil;
    SC.prio = -1;
    S->shas_const[slot] = (uint8_t)SC.has_consts;
    memcpy(S->sconst[slot], SC.consts, sizeof SC.consts);
    os_unfair_lock_unlock(&SC.lock);
    S->scene[slot] = 0;
    S->shud[slot] = 0;
    if (col && g.race_hud && col.width == S->w && col.height == S->h) {      // (the HUD apart: the scene before the UI)
        id<MTLTexture> sc = S->scol[slot];
        if (!sc || sc.pixelFormat != col.pixelFormat) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:col.pixelFormat width:S->w height:S->h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            sc = S->scol[slot] = [g.dev newTextureWithDescriptor:td];
        }
        if (sc) {
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:col sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(S->w, S->h, 1)
                        toTexture:sc destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            S->shud[slot] = 1;
            if (S->hud_n++ % 300 == 0) {                           // (every 300 frames: how much of F is S)
                id<MTLBuffer> cnt = [g.dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
                memset(cnt.contents, 0, 16);
                id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:g.hud_diff_ps];
                [enc setTexture:S->copy[slot] atIndex:0];
                [enc setTexture:sc atIndex:1];
                [enc setBuffer:cnt offset:0 atIndex:0];
                [enc dispatchThreads:MTLSizeMake((S->w + 3) / 4, (S->h + 3) / 4, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
                [enc endEncoding];
                MTLPixelFormat ff = S->copy[slot].pixelFormat, sf = sc.pixelFormat;
                [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                    (void)done;
                    const uint32_t *k = cnt.contents;
                    g.hud_eq = k[0] ? (float)k[1] / k[0] : -1;
                    vx_log("NPU mode (race, hud): the final frame (format %lu) equals FSR2's output (format %lu) at %.1f%% of %u points "
                           "(within 2/255; %.1f%% within 8/255): %s", (unsigned long)ff, (unsigned long)sf, 100.0 * g.hud_eq, k[0],
                           k[0] ? 100.0 * k[2] / k[0] : 0.0, g.hud_eq >= 0.5f ? "the UI is the difference, the scene warped alone" :
                           "something more than the UI is drawn after FSR2 (post-processing, another colour space): the HUD not apart");
                }];
            }
        }
    }
    uint32_t disp[2];
    S->fsr[slot] = S->shas_const[slot] && fsr2_parse(S->sconst[slot], &S->fsr_tan[slot], S->fsr_d2v[slot], S->fsr_mvs[slot], disp) &&
                   isfinite(S->fsr_mvs[slot][0]) && isfinite(S->fsr_mvs[slot][1]) && S->fsr_mvs[slot][0] != 0 && S->fsr_mvs[slot][1] != 0;
    if (!mv || !dp) return 0;
    id<MTLTexture> c = S->smv[slot], cd = S->sdp[slot];
    if (!c || c.width != mv.width || c.height != mv.height || c.pixelFormat != mv.pixelFormat || !cd || cd.width != dp.width || cd.height != dp.height) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mv.pixelFormat width:mv.width height:mv.height mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        S->smv[slot] = [g.dev newTextureWithDescriptor:td];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:dp.width height:dp.height mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        td.storageMode = MTLStorageModePrivate;
        S->sdp[slot] = [g.dev newTextureWithDescriptor:td];
        if (!S->smv[slot] || !S->sdp[slot]) return 0;
    }
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:mv sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(mv.width, mv.height, 1)
                toTexture:S->smv[slot] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    MTLPixelFormat f = dp.pixelFormat;
    int is_depth = f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatDepth16Unorm;
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:is_depth ? g.depth_d_ps : g.depth_c_ps];
    [enc setTexture:dp atIndex:0];
    [enc setTexture:S->sdp[slot] atIndex:1];
    [enc dispatchThreads:MTLSizeMake(dp.width, dp.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];
    S->scene[slot] = 1;
    return 1;
}

// (the display queue) the motion vectors' unit and signs, from frames A and B: each sample point votes for the candidate
// that matches B to A best, where something moves (no motion is clearly worse); decided after enough votes. With FSR2's
// constants the scale is known at once (theirs: UV units, A's pixel = B's + mv x scale x frame size); the vote then only
// checks it (and wins if it clearly disagrees). The depth at the sample points tells which way it goes (logged once).
static void mv_calibrate(VXNpuSet *S, int a, int b) {
    uint32_t gw = S->w / 16, gh = S->h / 16;
    id<MTLBuffer> out = [g.dev newBufferWithLength:(NSUInteger)gw * gh * 10 * 4 options:MTLResourceStorageModeShared];
    id<MTLTexture> mvb = S->smv[b], dpb = S->sdp[b];
    if (!out || !mvb || !dpb) return;
    id<MTLCommandBuffer> cb = [g.queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:g.calib_ps];
    [enc setTexture:S->copy[b] atIndex:0];
    [enc setTexture:S->copy[a] atIndex:1];
    [enc setTexture:mvb atIndex:2];
    [enc setTexture:dpb atIndex:3];
    [enc setBuffer:out offset:0 atIndex:0];
    [enc setBytes:&S->RP length:sizeof S->RP atIndex:1];
    [enc dispatchThreads:MTLSizeMake(gw, gh, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [enc endEncoding];
    R.calib_busy = 1;
    float rw = (float)mvb.width, rh = (float)mvb.height;
    int fsr = S->fsr[b];
    float cmsx = fsr ? S->fsr_mvs[b][0] * S->w : 0, cmsy = fsr ? S->fsr_mvs[b][1] * S->h : 0, d2v0 = fsr ? S->fsr_d2v[b][0] : NAN;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        const float *e = out.contents;
        // (the depth: how much of it is exactly 0, exactly 1, and its median: reversed Z has the sky at 0)
        static int dtold;
        if (!dtold++) {
            uint32_t n = gw * gh, z0 = 0, z1 = 0;
            float *dv = malloc(n * sizeof *dv);
            for (uint32_t i = 0; i < n; i++) { float d = e[i * 10 + 9]; z0 += d <= 0; z1 += d >= 1; if (dv) dv[i] = d; }
            float med = dv ? pct(dv, n, 0.5f) : NAN, p10 = dv ? pct(dv, n, 0.1f) : NAN, p90 = dv ? pct(dv, n, 0.9f) : NAN;
            free(dv);
            vx_log("NPU mode (race): the depth at %u points: %.0f%% exactly 0, %.0f%% exactly 1, p10 %.4g median %.4g p90 %.4g; FSR2's "
                   "device-to-view %s; the warp takes it as %s", n, 100.0 * z0 / n, 100.0 * z1 / n, p10, med, p90,
                   isfinite(d2v0) ? (d2v0 < 0.5f ? "says reversed (near 1, far 0)" : "says standard (near 0, far 1)") : "not known",
                   isfinite(d2v0) ? "FSR2 says" : S->RP.rev ? "reversed (VXRT_NPU_RACE_DEPTH_REVERSED=0 for standard)" : "standard");
        }
        os_unfair_lock_lock(&R.lock);
        for (uint32_t i = 0; i < gw * gh; i++, e += 10) {
            int best = 0;
            for (int c = 1; c < 8; c++) if (e[c] < e[best]) best = c;
            if (!(e[8] > 1.5f * e[best] && e[8] > 0.3f)) continue;  // (nothing moves here, or nothing matches)
            R.votes[best]++; R.moving++;
        }
        R.calib_frames++;
        R.calib_busy = 0;
        int best = 0;
        for (int c = 1; c < 8; c++) if (R.votes[c] > R.votes[best]) best = c;
        if (R.set == S && R.moving >= 400 && R.votes[best] >= R.moving * 6 / 10) {
            float kx = best & 4 ? (float)S->w / rw : (float)S->w, ky = best & 4 ? (float)S->h / rh : (float)S->h;
            kx = best & 1 ? -kx : kx;
            ky = best & 2 ? -ky : ky;
            const char *unit = best & 4 ? "render pixels" : "frame widths/heights";
            if (fsr) {
                int agree = kx * cmsx > 0 && ky * cmsy > 0 && fabsf(kx / cmsx - 1) < 0.15f && fabsf(ky / cmsy - 1) < 0.15f;
                vx_log("NPU mode (race): the motion vectors' vote (%s, x %s, y %s: %g, %g frame pixels a unit; %u of %u moving sample "
                       "points over %d frames) %s FSR2's scale (%g, %g)", unit, best & 1 ? "negated" : "as is", best & 2 ? "negated" : "as is",
                       kx, ky, R.votes[best], R.moving, R.calib_frames, agree ? "agrees with" : "DISAGREES with: the vote is used, not", cmsx, cmsy);
                if (!agree) { S->RP.msx = kx; S->RP.msy = ky; R.mv_ok = 1; }
            } else {
                S->RP.msx = kx; S->RP.msy = ky;
                R.mv_ok = 1;
                vx_log("NPU mode (race): the motion vectors are in %s, x %s, y %s (%u of %u moving sample points over %d frames "
                       "agree): predictions from them, no optical flow", unit, best & 1 ? "negated" : "as is", best & 2 ? "negated" : "as is",
                       R.votes[best], R.moving, R.calib_frames);
            }
            R.calib_done = 1;
        } else if (R.calib_frames % 50 == 0)
            vx_log("NPU mode (race): the motion vectors' unit not %s yet (%u moving sample points, best %u)", fsr ? "checked" : "decided",
                   R.moving, R.votes[best]);
        os_unfair_lock_unlock(&R.lock);
    }];
    [cb commit];
}

// (the game thread) a slot for the next frame, -1 for none free
static int race_slot(VXNpuSet *S) {
    os_unfair_lock_lock(&R.lock);
    if (R.set != S) race_reset(S);
    uint32_t used = R.inflight | bit(R.last) | bit(R.a) | bit(R.b) | bit(R.want_a) | bit(R.want_b) | bit(R.run_a) | bit(R.run_b);
    for (int k = 0; k < R.npq; k++) used |= bit(R.pq[k]);
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

// (R.lock not held) the warp core's parameters for B (slot) predicted t of the game's frame intervals on: its motion
// vectors' scale (FSR2's, or the vote's), its projection (FSR2's, or depth in near-plane units), held blocks against A
static void warp_params(VXNpuSet *S, int slot, int a, float t) {
    vxw_params P;
    memset(&P, 0, sizeof P);
    P.w = S->w; P.h = S->h;
    P.flags = a >= 0 ? VXW_STILL : 0;
    P.t = t;
    if (R.mv_ok == 2 && S->fsr[slot]) { P.mv_sx = S->fsr_mvs[slot][0] * S->w; P.mv_sy = S->fsr_mvs[slot][1] * S->h; }
    else { P.mv_sx = S->RP.msx; P.mv_sy = S->RP.msy; }
    if (!S->fsr[slot] || !vxw_projection_from_fsr2(&P, S->fsr_tan[slot], S->fsr_d2v[slot])) {
        P.z_a = 1; P.z_b = 0;
        if (!S->RP.rev) P.flags |= VXW_DEPTH_STD;
    }
    P.thr = 2.0f / 255;
    S->WP = P;
    S->WA = a;
    S->RP.mvmode = 1;
}

// (R.present held) a frame into a drawable, presented at once: a real one (slot), or B (slot) predicted S->RP.t on
// (the game's motion vectors: S->WP, the warp core; else the optical flow's motion map d)
static int race_present(int kind, VXNpuSet *S, int slot, int a, uint32_t d, int new_mm, int mv, double ready) {
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [g.layer nextDrawable];
        if (!drawable) return 0;
        id<MTLCommandBuffer> cb = [g.queue commandBuffer];
        if (kind == KIND_RACE) {
            if (!mv) S->RP.mvmode = 0;
            if (!mv && new_mm) {                                   // (once per flow)
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
        double content = R.rec_content;                            // (the content time it shows: recorded as a 5th field)
        [drawable addPresentedHandler:^(id<MTLDrawable> done) {
            double pt = done.presentedTime;
            if (pi < 1024) R.pres[pi] = pt;
            if (kind == KIND_REAL && pt > 0) {
                uint32_t i = __atomic_fetch_add(&R.nlat, 1, __ATOMIC_RELAXED);
                if (i < 512) R.lat[i] = (float)((pt - ready) * 1000);
            }
            if (rec) { fprintf(rec, " %.6f %.6f\n", pt, content); fclose(rec); }
        }];
        if (R.present_at > 0) [cb presentDrawable:drawable atTime:R.present_at];
        else [cb presentDrawable:drawable];
        [cb commit];
    }
    return 1;
}

// (R.lock held) the optical flow's helper, the first time a race set has gone 3 s without the game's motion vectors
// (menus, a game without FSR2) and none have been usable: with them there is no flow, and no helper process at all
static void race_helper_check(VXNpuSet *S, int has_scene, double t) {
    static int flow = -1;
    if (flow < 0) flow = !getenv("VXRT_NPU_RACE_FLOW") || atoi(getenv("VXRT_NPU_RACE_FLOW"));
    if (has_scene) { R.noscene_t = 0; return; }
    if (!R.noscene_t) R.noscene_t = t;
    if (S->helper || R.mv_ok || !flow || t - R.noscene_t < 3) return;
    S->helper = 1;
    vx_log("NPU mode (race): no motion vectors from the game for 3 s: the optical flow's helper starts");
    VXNpuSet *s = S;
    dispatch_async(g.init_q, ^{ set_start(s); });
}

// (R.lock held) B from now on: slot, made at content time c
static void race_new_b(VXNpuSet *S, int slot, int has_in, int has_scene, double c) {
    int prev = R.last;
    double dt = c - R.last_t;
    if (prev >= 0 && dt > 0.002 && dt < 0.25) R.interval = R.interval ? R.interval * 0.8 + dt * 0.2 : dt;
    else if (dt >= 0.25) prev = -1;                                // (a pause, or the mode was off: no motion to go on)
    R.last = slot; R.last_t = c;
    R.has_in = has_in ? R.has_in | bit(slot) : R.has_in & ~bit(slot);
    R.a = prev; R.b = slot; R.b_seq++;
    R.ca = R.cb; R.cb = c;
    R.scene = has_scene ? R.scene | bit(slot) : R.scene & ~bit(slot);
    if (R.mv_ok && S->helper && !R.flow_busy && S->sock >= 0 && R.want_b < 0) {   // (motion vectors work: no flow from now on)
        __atomic_store_n(&S->ready, 0, __ATOMIC_RELEASE);
        close(S->sock);
        S->sock = -1;
        vx_log("NPU mode (race): the game's motion vectors work: the optical flow's helper stops");
    }
    if (has_scene && S->fsr[slot] && !R.mv_ok && g.wc) {
        R.mv_ok = 2;
        vx_log("NPU mode (race): predictions from the game's motion vectors with FSR2's scale (%g, %g frame pixels a unit), no "
               "optical flow; the vote checks it while the scene moves", S->fsr_mvs[slot][0] * S->w, S->fsr_mvs[slot][1] * S->h);
    }
    if (has_scene && prev >= 0 && (R.scene & bit(prev)) && !R.calib_done && !R.calib_busy) mv_calibrate(S, prev, slot);
    race_helper_check(S, has_scene, c);
    // (a game at the refresh rate gets no predicted frames: no flow wanted)
    // (and not every frame's: a flow takes 20-50 ms of the Neural Engine and some of the GPU; a frame or two older is
    //  as good while the motion is slow)
    // (measured, 31-35 fps real: the flow every 2nd frame cost 6.5% of the game's frame rate)
    uint32_t every = R.flow_px > 16 ? 1 : R.flow_px > 6 ? 2 : 3;
    if (prev >= 0 && has_in && (R.has_in & bit(prev)) && R.interval > R.period * 1.2 && !g.race_noflow && !(R.mv_ok && has_scene) &&
        (!R.flow_seq || R.b_seq - R.flow_bseq >= every)) {
        R.want_a = prev; R.want_b = slot; R.want_bseq = R.b_seq;
        flow_kick(S);
    }
}

// (the display queue) a frame the GPU has copied: presented at once, B from now on; paced (VXRT_NPU_REAL_FPS): kept
// until its time, the display link's tick shows it
static void handle_race(VXNpuSet *S, uint32_t slot, int has_in, int has_scene, double t) {
    g.busy = 0;                                                    // (the flow queue skips a late extrapolate reply)
    pthread_mutex_lock(&R.present);
    os_unfair_lock_lock(&R.lock);
    if (R.set != S) race_reset(S);
    R.inflight &= ~bit((int)slot);
    R.frames++;
    double gdt = t - R.glast_t;
    if (gdt > 0.002 && gdt < 0.25) R.ginterval = R.ginterval ? R.ginterval * 0.8 + gdt * 0.2 : gdt;
    R.glast_t = t;
    if (R.paced) {
        // (the latency from the content time to ready: its envelope (up at once, down slowly) sets how far behind the
        //  display shows the content, so every real frame is in by its time)
        double lat = t - S->ctime[slot];
        if (lat > 0 && lat < 0.5) {
            R.lring[R.nl++ % 32] = (float)lat;
            uint32_t n = R.nl < 32 ? R.nl : 32;
            float v[32];
            memcpy(v, R.lring, n * sizeof *v);
            R.dl = pct(v, n, 0.9f);                                // (a rare slow frame is shown late, warped on: not D)
        }
        if (R.nlat2 < 512) R.lat2[R.nlat2++] = (float)(lat * 1000);
        if (R.npq == 4) {                                          // (full: the oldest goes, never shown)
            memmove(R.pq, R.pq + 1, 3 * sizeof *R.pq); memmove(R.pq_c, R.pq_c + 1, 3 * sizeof *R.pq_c);
            memmove(R.pq_in, R.pq_in + 1, 3 * sizeof *R.pq_in); memmove(R.pq_scene, R.pq_scene + 1, 3 * sizeof *R.pq_scene);
            R.npq = 3; R.pend_dropped++;
        }
        R.pq[R.npq] = (int)slot; R.pq_c[R.npq] = S->ctime[slot]; R.pq_in[R.npq] = has_in; R.pq_scene[R.npq] = has_scene; R.npq++;
        os_unfair_lock_unlock(&R.lock);
        pthread_mutex_unlock(&R.present);
        dispatch_semaphore_signal(R.sem);
        return;
    }
    R.npq = 0;                                                     // (paced just turned off: those waiting are dropped)
    // (a slow game simulated, "real15" in the switch file: a frame sooner than that after the last one shown is
    //  counted and not shown)
    float cap = g.race_real_cap;
    if (cap > 0 && R.last >= 0 && t - R.last_t < 1.0 / cap - 0.003 && t - R.last_t > 0) {
        R.capped++;
        os_unfair_lock_unlock(&R.lock);
        pthread_mutex_unlock(&R.present);
        return;
    }
    race_new_b(S, (int)slot, has_in, has_scene, t);
    os_unfair_lock_unlock(&R.lock);
    R.rec_content = t;
    if (race_present(KIND_REAL, S, (int)slot, -1, 0, 0, 0, t)) {
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
               "screen %.1f ms after they were ready (median, %.1f ms p95); %u predicted from the game's motion vectors%s; "
               "%u real frames not shown (shown at most %.0f a second)",
               frames / dt, shown / dt, R.real, R.gen, R.real + R.gen ? 100.0 * R.gen / (R.real + R.gen) : 0, gone, R.still,
               R.held, R.evened, 1 / R.period, hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], shown ? shown - 1 : 0,
               pct(fl, nfl, 0.5f), nfl, R.flow_px, R.gen ? 100 * R.stale / R.gen : 0, gpu, pct(R.gpu_ms, ngpu, 0.95f),
               100.0 * gpu * R.gen / 1000 / dt, pct(R.ts, R.nt, 0.5f), pct(R.lat, nlat, 0.5f), pct(R.lat, nlat, 0.95f), R.mvgen,
               R.mv_ok ? "" : R.scene ? " (their unit not known yet)" : " (none seen)", R.capped, g.race_real_cap);
        if (R.paced) {
            uint32_t n2 = R.nlat2 < 512 ? R.nlat2 : 512;
            vx_log("NPU mode (race, paced): real frames every %.0f refreshes (%.1f fps asked, %.1f fps on the grid); shown %.1f ms "
                   "after their content time (D, %.0f refreshes); content to ready %.1f ms median, %.1f ms p95, %.1f ms max (%u); "
                   "%u frames replaced before their time, %u shown late as they are",
                   R.pace_i / R.period, R.cap, R.pace_i > 0 ? 1 / R.pace_i : 0, R.d * 1000, R.d / R.period, pct(R.lat2, n2, 0.5f),
                   pct(R.lat2, n2, 0.95f), pct(R.lat2, n2, 1.0f), n2, R.pend_dropped, R.late_real);
        }
    }
    R.stat_t0 = now;
    R.real = R.gen = R.still = R.held = R.evened = R.stale = R.nt = R.mvgen = R.capped = 0;
    R.nlat2 = R.pend_dropped = R.late_real = 0;
    __atomic_store_n(&R.ngpu, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&R.nlat, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&R.npres, 0, __ATOMIC_RELEASE);
}

// (the link's thread, paced) one refresh, on screen at T = tick + period: it shows the content of time T - D: the
// newest frame due by then (itself, when its content time is that), else B predicted to it from the game's motion
// vectors (or the optical flow). Every refresh the window gets a frame whose content time is one refresh on: even steps,
// whatever the GPU's time of each real frame (as long as it stays under D).
static void race_tick_paced(double tick) {
    double per = R.period;
    if (R.link_ts > tick - 2 * per && R.link_ts <= tick + 0.001) tick = R.link_ts;   // (the refresh's own time: the grid)
    // (made a refresh ahead and scheduled for its vsync: the warp's work waits behind the game's on the GPU, and a present
    //  decided a few ms before its vsync missed it about half the time at El Coyote - a refresh late, a judder step)
    double T = tick + 2 * per;
    R.vsync = tick;
    // (D: the latency envelope and the decision's margin, rounded up to whole refreshes: content times start on the
    //  refresh grid, so a real frame comes due exactly on a refresh and is shown as it is)
    double d = ceil((R.dl + R.margin + 0.001) / per - 0.05) * per;
    if (d < per) d = per;
    if (d >= R.d || !R.d) R.d = d;                                 // (up at once; down a refresh at a time, every 0.5 s)
    else if (d < R.d - 0.5 * per && tick - R.d_down > 0.5) { R.d -= per; R.d_down = tick; }
    d = R.d;
    double tau = T - d, decide = tick + 0.001;
    for (;;) {                                                     // (until the frame due is in, or the decision's time)
        double now = now_s();
        os_unfair_lock_lock(&R.lock);
        int in = R.npq > 0 && R.pq_c[0] <= tau + 0.25 * per;
        os_unfair_lock_unlock(&R.lock);
        if (in || now >= decide) break;
        dispatch_semaphore_wait(R.sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)((decide - now) * 1e9)));
    }
    pthread_mutex_lock(&R.present);
    double now = now_s();
    // (paced: every refresh up to 100 Hz (Vision Pro's 90), every other one above (120 Hz Macs reached 60 even steps,
    //  not 120: run20); "race N" or VXRT_NPU_RACE_HZ sets another rate)
    double gap = R.hz > 0 ? 1 / R.hz : per < 0.01 ? 2 * per : per;
    int kind = -1, slot = -1, a = -1, mv = 0, fresh = 0, new_mm = 0;
    uint32_t d_flow = 0;
    float t = 0;
    VXNpuSet *S = nil;
    os_unfair_lock_lock(&R.lock);
    S = R.set;
    int due = -1;                                                  // (the newest frame due: B from now on; older ones skipped)
    for (int k = 0; k < R.npq; k++) if (R.pq_c[k] <= tau + 0.25 * per) due = k;
    if (S && due >= 0) {
        R.pend_dropped += (uint32_t)due;
        race_new_b(S, R.pq[due], R.pq_in[due], R.pq_scene[due], R.pq_c[due]);
        R.b_shown = 0;
        int n = R.npq - due - 1;
        memmove(R.pq, R.pq + due + 1, n * sizeof *R.pq); memmove(R.pq_c, R.pq_c + due + 1, n * sizeof *R.pq_c);
        memmove(R.pq_in, R.pq_in + due + 1, n * sizeof *R.pq_in); memmove(R.pq_scene, R.pq_scene + due + 1, n * sizeof *R.pq_scene);
        R.npq = n;
    }
    if (S && R.b >= 0 && T - R.shown_T >= gap - 0.5 * per) {       // (one present per target vsync, whatever the tick's jitter)
        double ival = R.a >= 0 && R.cb - R.ca > 0.002 ? R.cb - R.ca : R.pace_i;
        t = ival > 0 ? (float)((tau - R.cb) / ival) : 0;
        mv = R.mv_ok && (R.scene & bit(R.b)) && !SC.off && !g.race_nomv && g.wc;
        int flow_ok = !mv && S->ready && S->mm && R.a >= 0 && R.flow_seq && R.b_seq - R.flow_bseq <= 6;
        if (!R.b_shown && t * ival < 0.5 * per) kind = KIND_REAL;  // (B's own time: B as it is)
        else if (!R.b_shown && t > R.max_t) { kind = KIND_REAL; R.late_real++; }   // (too late to predict to: B late)
        else if (t <= 0 || t > R.max_t + 1e-3 || g.race_nowarp || (!mv && !flow_ok)) R.held++;
        else {
            float px = mv ? (R.mm_b_seq != R.b_seq ? 1e9f : vxw_last_motion(g.wc)) : R.flow_px;
            if ((t - R.shown_t) * px < R.min_px && R.b_shown) R.still++;
            else {
                kind = KIND_RACE;
                fresh = mv || R.flow_bseq == R.b_seq;
                d_flow = (uint32_t)R.flow_dst;
                new_mm = !mv && (R.mm_flow_seq != R.flow_seq || R.mm_mv != 0);
                R.mm_b_seq = R.b_seq; R.mm_flow_seq = R.flow_seq; R.mm_mv = mv;
                if (mv) R.mvgen++;
            }
        }
        slot = R.b; a = R.a;
    }
    os_unfair_lock_unlock(&R.lock);
    int done = 0;
    R.present_at = T - 0.002;
    if (kind >= 0) R.rec_content = R.cb + t * (R.a >= 0 && R.cb - R.ca > 0.002 ? R.cb - R.ca : R.pace_i);
    if (kind == KIND_REAL) {
        if (race_present(KIND_REAL, S, slot, -1, 0, 0, 0, now)) {
            R.tb = R.shown_at = now; R.shown_t = 0; R.b_shown = 1; R.shown_T = T;
            R.real++;
        }
    } else if (kind == KIND_RACE) {
        S->RP.t = t;
        if (mv) warp_params(S, slot, a, t);
        if (race_present(KIND_RACE, S, slot, a, d_flow, new_mm, mv, 0)) {
            R.shown_at = now; R.shown_t = t; R.b_shown = 1; R.shown_T = T;
            R.gen++;
            if (!fresh) R.stale++;
            if (R.nt < 512) R.ts[R.nt++] = t;
            done = 1;
        }
    }
    R.present_at = 0;
    pthread_mutex_unlock(&R.present);
    if (done && a >= 0) snapshot(S, (uint32_t)a, 0, (uint32_t)slot);   // (A: the snapshot's first picture)
    race_stats(now_s());
}

// (the link's thread) one refresh: unless a real frame comes in time, one predicted from B to the time it will be shown
static void race_tick(double tick, double period) {
    if (period > 0.002 && period < 0.05) R.period = period;
    if (!g.race || g.state <= 0) return;
    if (R.paced) { race_tick_paced(tick); return; }
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
        // (the game's motion vectors for B: always B's own; else the optical flow, a few frames old at most: one every
        //  3rd frame, taking 2)
        int mv = S && R.mv_ok && R.b >= 0 && (R.scene & bit(R.b)) && !SC.off && !g.race_nomv && g.wc;
        int ok = S && R.b >= 0 && R.a >= 0 && R.interval > 0 && (mv || (S->ready && S->mm && R.flow_seq && R.b_seq - R.flow_bseq <= 6));
        // how far on: B was presented at tb (and will be on screen as long after it as this one)
        double t = ok ? (now - R.tb) / R.interval : 0;
        // (the game's vectors are a game frame's motion: with frames not shown, B is several of those old)
        double tm = mv && R.ginterval > 0 ? (now - R.tb) / R.ginterval : t;
        // (even steps: no prediction VXRT_NPU_RACE_EVEN (1.5) refreshes before the next real frame is due; it would leave a
        //  1-refresh step)
        double due = R.last_t + R.interval - now;
        // (the game's vectors: B's motion map not made yet counts as moving; else its largest motion)
        if (mv) R.mv_px = R.mm_b_seq != R.b_seq || R.mm_mv != 1 ? 1e9f : vxw_last_motion(g.wc);
        if (!ok || t <= 0 || t > R.max_t + 1e-3 || g.race_nowarp) { R.held++; os_unfair_lock_unlock(&R.lock); }
        else if (due > 0 && due < R.even * R.period) { R.evened++; os_unfair_lock_unlock(&R.lock); }
        else if ((t - R.shown_t) * (mv ? R.mv_px : R.flow_px) < R.min_px) { R.still++; os_unfair_lock_unlock(&R.lock); }
        else {
            int slot = R.b, a = R.a, fresh = mv || R.flow_bseq == R.b_seq;
            uint32_t d = (uint32_t)R.flow_dst;
            // (once per flow: a previous pair's flow goes on with the motion map made with it, from A's pixels; the
            //  game's vectors once per B)
            int new_mm = mv ? R.mm_b_seq != R.b_seq || R.mm_mv != 1 : R.mm_flow_seq != R.flow_seq || R.mm_mv != 0;
            R.mm_b_seq = R.b_seq; R.mm_flow_seq = R.flow_seq; R.mm_mv = mv;
            if (mv) R.mvgen++;
            os_unfair_lock_unlock(&R.lock);
            S->RP.t = (float)tm;
            R.rec_content = R.cb + tm * (mv && R.ginterval > 0 ? R.ginterval : R.interval);
            if (mv) warp_params(S, slot, a, (float)tm);
            if (race_present(KIND_RACE, S, slot, a, d, new_mm, mv, 0)) {
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
    if (done && snap_a >= 0) snapshot(snap_s, (uint32_t)snap_a, 0, (uint32_t)snap_b);   // (not holding up real frames)
    race_stats(now_s());
}

@interface VXNpuTick : NSObject
@end
@implementation VXNpuTick
- (void)tick:(CADisplayLink *)link {
    @autoreleasepool { R.link_ts = link.timestamp; race_tick(now_s(), link.targetTimestamp - link.timestamp); }
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
        R.even = getenv("VXRT_NPU_RACE_EVEN") ? atof(getenv("VXRT_NPU_RACE_EVEN")) : 1.5;
        if (hz && !R.hz) R.hz = (float)atof(hz);
    }
#if VX_XROS
    float hz = 90;                                                 // (visionOS: no screens to ask; Vision Pro's rate)
#define VX_NPU_SCREEN_NAME "Vision Pro"
#else
    NSScreen *screen = nil;
    for (NSWindow *w in [NSApp windows])                           // (reading: no change to the window)
        if (w.contentView.layer == layer || w.contentView.layer == layer.superlayer) screen = w.screen;
    if (!screen) screen = [NSScreen mainScreen];
    float hz = (float)screen.maximumFramesPerSecond;               // (real frames show at the display's rate; R.hz caps predictions)
#define VX_NPU_SCREEN_NAME (screen.localizedName.UTF8String ?: "?")
#endif
    const char *dhz = getenv("VXRT_NPU_RACE_DISPLAY_HZ");          // (another refresh rate asked of the display link: 90, as Vision Pro's)
    if (dhz && atof(dhz) > 0) hz = (float)atof(dhz);
    dispatch_semaphore_t started = dispatch_semaphore_create(0);
    NSThread *th = [[NSThread alloc] initWithBlock:^{
        VXNpuTick *target = [VXNpuTick new];
#if VX_XROS
        CADisplayLink *link = [CADisplayLink displayLinkWithTarget:target selector:@selector(tick:)];
#else
        CADisplayLink *link = [screen displayLinkWithTarget:target selector:@selector(tick:)];
#endif
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
           "interval on, a frame when the scene moved %.1f px)", VX_NPU_SCREEN_NAME, hz, R.margin * 1000,
           R.max_t, R.min_px);
}

// (the game thread, paced) waits until this frame's start on the refresh grid, every k refreshes (the refresh rate /
// VXRT_NPU_REAL_FPS, rounded); returns that time, the frame's content time. A game slower than that runs as it can,
// its frames' starts put back on the grid when one is late by a whole interval.
static double race_pace(void) {
    double per = R.period, now = now_s();
    int k = (int)lround(1.0 / (R.cap * per));
    if (k < 1) k = 1;
    double ival = k * per;
    if (fabs(R.pace_i - ival) > 1e-4) R.pace_next = 0;
    R.pace_i = ival;
    if (!R.pace_next || now > R.pace_next + ival) {
        double vs = R.vsync > 0 && R.vsync <= now ? R.vsync : now;
        R.pace_next = vs + ceil((now - vs) / per) * per;
    }
    double c = R.pace_next;
    while ((now = now_s()) < c - 0.0002) {
        double wait = c - now;
        struct timespec ts = { 0, (long)(wait * 1e9) };
        nanosleep(&ts, NULL);
    }
    R.pace_next = c + ival;
    return c;
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
#if !VX_XROS   // (visionOS: the system paces CAMetalLayer)
    layer.displaySyncEnabled = YES;
#endif
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
        if (g.race && S->mode == NPU_MODE_FLOW) race_buffers(S);  // (race: the flow's helper only if no motion vectors come)
        else dispatch_async(g.init_q, ^{ set_start(s); });
    }
    int race = g.race && S->mode == NPU_MODE_FLOW;
    if (race) race_start(layer);
    if (race && R.cap != g.race_fps) {                             // (paced on, off, or another rate)
        R.cap = g.race_fps;
        R.paced = R.cap > 0;
        R.pace_next = 0;
        if (R.paced) vx_log("NPU mode (race): the game's frames paced at %.1f a second (on the refresh grid), shown at their content time", R.cap);
        else vx_log("NPU mode (race): not paced");
    }
    // (paced: this frame's content time is when its start was let go, at the end of the previous present)
    double content = race && R.paced && R.next_content > 0 ? R.next_content : now_s();
    // (more frames waiting than the slots allow: this one is not shown)
    int rslot = race ? race_slot(S) : -1;
    if (race ? rslot < 0 : __atomic_load_n(&g.pending, __ATOMIC_ACQUIRE) >= 5) {
        dispatch_async(g.q, ^{ g.dropped++; });
        if (waitable) [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { (void)done; vx_release_semaphore(waitable, 1); }];
        [cb commit];
        R.next_content = race && R.paced ? race_pace() : 0;
        return;
    }
    uint32_t slot = race ? (uint32_t)rslot : g.next_slot++ % NSRC;
    S->ctime[slot] = content;
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                toTexture:S->copy[slot] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
    int has_yuv = __atomic_load_n(&S->ready, __ATOMIC_ACQUIRE);
    int has_scene = race && !g.race_nomv ? scene_capture(cb, S, slot) : 0;
    if (has_scene && R.mv_ok) has_yuv = 0;                         // (the game's motion vectors: no flow, no flow input)
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
        dispatch_async(g.q, ^{ if (race) handle_race(S, slot, has_yuv, has_scene, t); else handle(S, slot, has_yuv, t); });
    }];
    [cb commit];
    // (paced: the game's next frame starts on the refresh grid; this present's work is committed first)
    R.next_content = race && R.paced ? race_pace() : 0;
}
