// The warp core (see warp_core.h): Metal and Foundation only, for macOS and visionOS.
#import <Metal/Metal.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "warp_core.h"

// (struct W: vxw_params, then the output's size, B's block grid (the map), the output's block grid (the keys) and the
//  key's index bits)
typedef struct { vxw_params p; uint32_t ow, oh, mw, mh, kw, kh, bits, flags2; } vxw_gpu_params;
enum { VXW2_DEPTH_OUT = 1 };

static const char *g_vxw_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct W { uint w, h, flags; float t, sx, sy, tx, ty, za, zb; float rot[9]; float pos[3];\n"
    "           float eye, S, pop, zc, conv, thr; float dt[4]; float dnear;\n"
    "           uint ow, oh, mw, mh, kw, kh, bits, flags2; };\n"
    // the distance of a depth value (FSR2: view depth = z_a / (d - z_b), its sign the view space's handedness)
    "static float vxw_dist(float d, constant W &p) {\n"
    "    if (p.flags & 16) d = 1 - d;\n"
    "    float z = abs(p.za / (d - p.zb));\n"
    "    return isfinite(z) ? max(z, 1e-6f) : 1e30f;\n"            // (d == z_b: infinitely far)
    "}\n"
    "static uint2 vxw_at(float2 uv, uint2 size) { return min(uint2(max(uv, 0.0f) * float2(size)), size - 1); }\n"
    // 1: each block of B: where it lands, how near it is; xy: its displacement (frame pixels), z: its distance
    "kernel void vxw_map(texture2d<float> b [[texture(0)]], texture2d<float> a [[texture(1)]], texture2d<float> mv [[texture(2)]],\n"
    "                    texture2d<float> dp [[texture(3)]], texture2d<float, access::write> map [[texture(4)]],\n"
    "                    device atomic_uint *key [[buffer(0)]], constant W &p [[buffer(1)]], device const float *st [[buffer(2)]],\n"
    "                    device atomic_uint *mx [[buffer(3)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x >= p.mw || g.y >= p.mh) return;\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    int2 hi = int2(p.w, p.h) - 1, p0 = int2(g) * 2;\n"
    "    float2 sz = float2(p.w, p.h), osz = float2(p.ow, p.oh), pc = float2(p0) + 1, uv = pc / sz;\n"
    "    float2 m = -mv.read(vxw_at(uv, uint2(mv.get_width(), mv.get_height()))).xy * float2(p.sx, p.sy);\n"
    "    if (!all(isfinite(m)) || length(m) > 0.5f * max(sz.x, sz.y)) m = 0;\n"
    // (no motion of its own where A and B are the same around the block, or where B matches A unmoved clearly better
    //  than moved: the HUD and subtitles over a moving scene; a pose and the eye still move it)
    "    bool still = false;\n"
    "    if ((p.flags & 4) && length(m) > 0) {\n"
    "        still = true;\n"
    "        for (int y = -1; y <= 2 && still; y++) for (int x = -1; x <= 2; x++) {\n"
    "            uint2 q = uint2(clamp(p0 + int2(x, y), int2(0), hi));\n"
    "            if (any(abs(b.read(q).rgb - a.read(q).rgb) > p.thr)) { still = false; break; }\n"
    "        }\n"
    "        if (!still && length(m) > 1) {\n"
    "            float e0 = 0, e1 = 0;\n"
    "            for (int y = 0; y <= 1; y++) for (int x = 0; x <= 1; x++) {\n"
    "                uint2 q = uint2(min(p0 + int2(x, y), hi));\n"
    "                float3 c = b.read(q).rgb;\n"
    "                e0 += dot(abs(c - a.read(q).rgb), 1);\n"
    "                e1 += dot(abs(c - a.sample(s, (float2(q) + 0.5f - m) / sz, level(0)).rgb), 1);\n"
    "            }\n"
    "            still = e0 < 0.5f * e1 && e1 > 0.45f;\n"
    "        }\n"
    "        if (still) m = 0;\n"
    "    }\n"
    "    float z = vxw_dist(dp.read(vxw_at(uv, uint2(dp.get_width(), dp.get_height()))).r, p);\n"
    // (q: where it lands, in output pixels; the target's frustum: dt (left, right, up, down tangents) or B's own)
    "    float2 q = (pc + p.t * m) * osz / sz;\n"
    "    if (p.flags & 1) {\n"                                      // (the pose: unprojected, moved, projected)
    "        float2 qb = pc + p.t * m;\n"
    "        float2 ndc = float2(qb.x / sz.x * 2 - 1, 1 - qb.y / sz.y * 2);\n"
    "        float zz = min(z, 1e6f);\n"
    "        float3 X = float3(ndc.x * p.tx * zz, ndc.y * p.ty * zz, zz);\n"
    "        float3 Y = float3(p.rot[0] * X.x + p.rot[1] * X.y + p.rot[2] * X.z + p.pos[0],\n"
    "                          p.rot[3] * X.x + p.rot[4] * X.y + p.rot[5] * X.z + p.pos[1],\n"
    "                          p.rot[6] * X.x + p.rot[7] * X.y + p.rot[8] * X.z + p.pos[2]);\n"
    "        if (Y.z <= 1e-5f) { map.write(float4(0, 0, z, 0), g); return; }\n"   // (behind the new view: lands nowhere)
    "        float4 f = p.dt[0] + p.dt[1] > 0 ? float4(p.dt[0], p.dt[1], p.dt[2], p.dt[3]) : float4(p.tx, p.tx, p.ty, p.ty);\n"
    "        q = float2((Y.x / Y.z + f.x) / (f.x + f.y) * osz.x, (f.z - Y.y / Y.z) / (f.z + f.w) * osz.y);\n"
    "        if (z < 1e6f) z = Y.z;\n"
    "    }\n"
    "    if (p.flags & 2) {\n"                                      // (the eye: stereo.m's disparity, linear in inverse depth)
    "        float iv = p.za / z, ic = (p.flags & 8) ? max(st[0] / ((1 + p.pop) * p.conv), 1e-7f) : p.za / max(p.zc, 1e-6f);\n"
    "        q.x += p.eye * clamp(p.S * (1 - iv / ic), -2 * p.pop * p.S, p.S) * osz.x / sz.x;\n"
    "    }\n"
    "    map.write(float4(q * sz / osz - pc, z, 1), g);\n"     // (the displacement in B's pixels)
    "    atomic_fetch_max_explicit(mx, uint(min(length(m), 4095.0f) * 16), memory_order_relaxed);\n"
    "    int2 tb = int2(floor(q * 0.5f));\n"
    "    if (any(tb < 0) || tb.x >= int(p.kw) || tb.y >= int(p.kh)) return;\n"
    // (in front: the nearest, by the log of the inverse distance in the key's high bits)
    "    float top = float((1u << (32 - p.bits)) - 1);\n"
    "    float pr = clamp((log2(1 / z) + 40) * (top / 64), 0.0f, top - 1);\n"
    // (flat, an overlay is in front of everything: nothing moving covers the HUD)
    "    if (still && !(p.flags & 3)) pr = top;\n"
    "    atomic_fetch_max_explicit(&key[uint(tb.y) * p.kw + uint(tb.x)], (uint(pr) << p.bits) | (g.y * p.mw + g.x + 1), memory_order_relaxed);\n"
    "}\n"
    // 2: each output block's displacement: the block that won it, or (a hole) the farthest landed in the nearest ring
    "kernel void vxw_resolve(texture2d<float> map [[texture(0)]], texture2d<float, access::write> disp [[texture(1)]],\n"
    "                        device const uint *key [[buffer(0)]], constant W &p [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x >= p.kw || g.y >= p.kh) return;\n"
    "    uint mask = (1u << p.bits) - 1, k = key[g.y * p.kw + g.x];\n"
    "    if (!k) {\n"
    "        uint best = 0xffffffffu;\n"
    "        for (int r = 1; r <= 16 && best == 0xffffffffu; r *= 2)\n"
    "            for (int d = 0; d < 9; d++) {\n"
    "                if (d == 4) continue;\n"
    "                int2 q = int2(g) + int2(d % 3 - 1, d / 3 - 1) * r;\n"
    "                if (any(q < 0) || q.x >= int(p.kw) || q.y >= int(p.kh)) continue;\n"
    "                uint k2 = key[uint(q.y) * p.kw + uint(q.x)];\n"
    "                if (k2 && (k2 >> p.bits) < (best >> p.bits)) best = k2;\n"
    "            }\n"
    "        k = best == 0xffffffffu ? 0 : best;\n"
    "    }\n"
    "    float2 r = float2(p.w, p.h) / float2(p.ow, p.oh), gc = (float2(g) * 2 + 1) * r;\n"   // (the block's centre in B)
    "    uint2 own = min(uint2(gc * 0.5f), uint2(p.mw, p.mh) - 1);\n"
    "    uint2 src = k ? uint2(((k & mask) - 1) % p.mw, ((k & mask) - 1) / p.mw) : own;\n"
    "    float4 v = map.read(src);\n"
    "    float2 dsp = v.w > 0 && all(isfinite(v.xy)) ? v.xy : float2(0);\n"
    // (a hole: the background's displacement, unless B there shows something nearer - what moved away and uncovered
    //  it, which would show twice: then the background block's own pixels, stretched over the hole, as stereo.m's gaps)
    "    if (k && !key[g.y * p.kw + g.x]) {\n"
    "        float2 sc = gc - dsp;\n"
    "        uint2 sb = uint2(clamp(int2(floor(sc * 0.5f)), int2(0), int2(p.mw, p.mh) - 1));\n"
    "        float4 o = map.read(sb);\n"
    "        if (o.z < 0.9f * v.z) dsp = gc - (float2(src) * 2 + 1);\n"
    "    }\n"
    "    disp.write(float4(dsp, isfinite(v.z) ? v.z : 1e30f, 0), g);\n"   // (z: the distance there, for the depth out)
    "}\n"
    // 3: each output pixel: B sampled its block's displacement back
    // (the depth out: reversed infinite, dnear / distance; dnear 0: B's own depth value moved with it)
    "static float vxw_out_depth(float z, constant W &p) { return p.dnear > 0 ? saturate(p.dnear / z) : (p.za / z + p.zb); }\n"
    "kernel void vxw_gather(texture2d<float> b [[texture(0)]], texture2d<float> disp [[texture(1)]], texture2d<float, access::write> o [[texture(2)]],\n"
    "                       texture2d<float, access::write> od [[texture(3)]], constant W &p [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "    if (g.x >= p.ow || g.y >= p.oh) return;\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    float4 v = disp.read(min(g / 2, uint2(p.kw, p.kh) - 1));\n"
    "    float2 pb = (float2(g) + 0.5f) * float2(p.w, p.h) / float2(p.ow, p.oh);\n"
    "    o.write(float4(b.sample(s, (pb - v.xy) / float2(p.w, p.h), level(0)).rgb, 1), g);\n"
    "    if (p.flags2 & 1) od.write(float4(vxw_out_depth(v.z, p)), g);\n"
    "}\n"
    "struct VO { float4 pos [[position]]; float2 uv; };\n"
    "vertex VO vxw_vs(uint id [[vertex_id]]) {\n"
    "    float2 q = float2((id << 1) & 2, id & 2);\n"
    "    VO o; o.pos = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = q; return o;\n"
    "}\n"
    "fragment float4 vxw_gather_fs(VO in [[stage_in]], texture2d<float> b [[texture(0)]], texture2d<float> disp [[texture(1)]],\n"
    "                              constant W &p [[buffer(1)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    uint2 g = min(uint2(in.uv * float2(p.ow, p.oh)), uint2(p.ow, p.oh) - 1);\n"
    "    float2 v = disp.read(min(g / 2, uint2(p.kw, p.kh) - 1)).xy;\n"
    "    return float4(b.sample(s, in.uv - v / float2(p.w, p.h)).rgb, 1);\n"
    "}\n"
    // (with a depth attachment: the depth out as the fragment's depth)
    "struct FD { float4 c [[color(0)]]; float d [[depth(any)]]; };\n"
    "fragment FD vxw_gather_depth_fs(VO in [[stage_in]], texture2d<float> b [[texture(0)]], texture2d<float> disp [[texture(1)]],\n"
    "                                constant W &p [[buffer(1)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    uint2 g = min(uint2(in.uv * float2(p.ow, p.oh)), uint2(p.ow, p.oh) - 1);\n"
    "    float4 v = disp.read(min(g / 2, uint2(p.kw, p.kh) - 1));\n"
    "    FD o; o.c = float4(b.sample(s, in.uv - v.xy / float2(p.w, p.h)).rgb, 1); o.d = vxw_out_depth(v.z, p); return o;\n"
    "}\n"
    // the stereo screen plane: the pct-th of 16x16 samples of inverse depth (z_a / z), smoothed in st[0]; st[1] this frame's
    "kernel void vxw_conv(texture2d<float> dp [[texture(0)]], constant W &p [[buffer(1)]], device float *st [[buffer(2)]],\n"
    "                     constant float &pct [[buffer(3)]], uint i [[thread_index_in_threadgroup]]) {\n"
    "    threadgroup float v[256];\n"
    "    uint2 ds = uint2(dp.get_width(), dp.get_height());\n"
    "    uint2 q = min(uint2(((i & 15) * 2 + 1) * ds.x / 32, ((i >> 4) * 2 + 1) * ds.y / 32), ds - 1);\n"
    "    v[i] = p.za / vxw_dist(dp.read(q).r, p);\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    for (uint k = 2; k <= 256; k <<= 1)\n"
    "        for (uint j = k >> 1; j > 0; j >>= 1) {\n"
    "            uint l = i ^ j;\n"
    "            if (l > i) {\n"
    "                float x = v[i], y = v[l];\n"
    "                if ((x > y) == ((i & k) == 0)) { v[i] = y; v[l] = x; }\n"
    "            }\n"
    "            threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "        }\n"
    "    if (i == 0) {\n"
    "        float m = v[min(255u, uint(pct * 255 + 0.5f))];\n"
    "        if (m > 1e-6f) st[0] = st[0] > 0 ? mix(st[0], m, 0.2f) : m;\n"
    "        st[1] = m;\n"
    "    }\n"
    "}\n";

struct vxw_core {
    __strong id<MTLDevice> dev;
    __strong id<MTLComputePipelineState> map, resolve, gather, conv;
    __strong id<MTLFunction> vs, fs, fsd;
    __strong id<MTLRenderPipelineState> draw[4];
    MTLPixelFormat draw_fmt[4][2];
    __strong id<MTLDepthStencilState> dss;
    __strong id<MTLLibrary> lib;
    __strong id<MTLTexture> dummy;     // (the depth out's binding when there is none)
    // per frame size (B's: the map) and output size (the keys, the displacements)
    uint32_t w, h, ow, oh, mw, mh, kw, kh, bits;
    __strong id<MTLTexture> mapt, disp;
    __strong id<MTLBuffer> keys, mx, state;
    _Atomic float last_motion;
};

vxw_core *vxw_create(void *mtl_device) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)mtl_device;
    if (!dev) return NULL;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(g_vxw_msl) options:nil error:&err];
    if (!lib) { NSLog(@"vxw: shaders: %@", err.localizedDescription); return NULL; }
    vxw_core *c = (vxw_core *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->dev = dev;
    c->lib = lib;
    c->map = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"vxw_map"] error:&err];
    c->resolve = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"vxw_resolve"] error:&err];
    c->gather = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"vxw_gather"] error:&err];
    c->conv = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"vxw_conv"] error:&err];
    c->vs = [lib newFunctionWithName:@"vxw_vs"];
    c->fs = [lib newFunctionWithName:@"vxw_gather_fs"];
    c->fsd = [lib newFunctionWithName:@"vxw_gather_depth_fs"];
    c->state = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = YES;
    c->dss = [dev newDepthStencilStateWithDescriptor:dd];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModePrivate;
    c->dummy = [dev newTextureWithDescriptor:td];
    if (!c->map || !c->resolve || !c->gather || !c->conv || !c->vs || !c->fs || !c->fsd || !c->state || !c->dss || !c->dummy) {
        NSLog(@"vxw: pipelines: %@", err.localizedDescription);
        vxw_destroy(c);
        return NULL;
    }
    memset(c->state.contents, 0, 16);
    atomic_store(&c->last_motion, -1.0f);
    return c;
}

void vxw_destroy(vxw_core *c) {
    if (!c) return;
    c->dev = nil; c->lib = nil; c->map = c->resolve = c->gather = c->conv = nil; c->vs = c->fs = c->fsd = nil;
    for (int k = 0; k < 4; k++) c->draw[k] = nil;
    c->dss = nil; c->dummy = nil;
    c->mapt = c->disp = nil; c->keys = c->mx = c->state = nil;
    free(c);
}

static int sized(vxw_core *c, uint32_t w, uint32_t h, uint32_t ow, uint32_t oh) {
    if (c->w == w && c->h == h && c->ow == ow && c->oh == oh && c->keys) return 1;
    uint32_t mw = (w + 1) / 2, mh = (h + 1) / 2, kw = (ow + 1) / 2, kh = (oh + 1) / 2, bits = 1;
    while (bits < 26 && (1u << bits) <= mw * mh) bits++;
    if ((1u << bits) <= mw * mh || kw * kh == 0) return 0;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:mw height:mh mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    td.storageMode = MTLStorageModePrivate;
    c->mapt = [c->dev newTextureWithDescriptor:td];
    td.width = kw; td.height = kh;
    c->disp = [c->dev newTextureWithDescriptor:td];
    c->keys = [c->dev newBufferWithLength:(NSUInteger)kw * kh * 4 options:MTLResourceStorageModePrivate];
    c->mx = [c->dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    if (!c->mapt || !c->disp || !c->keys || !c->mx) { c->keys = nil; return 0; }
    c->w = w; c->h = h; c->ow = ow; c->oh = oh; c->mw = mw; c->mh = mh; c->kw = kw; c->kh = kh; c->bits = bits;
    return 1;
}

static id<MTLRenderPipelineState> draw_pipeline(vxw_core *c, MTLPixelFormat f, MTLPixelFormat df) {
    for (int k = 0; k < 4; k++) if (c->draw[k] && c->draw_fmt[k][0] == f && c->draw_fmt[k][1] == df) return c->draw[k];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = c->vs;
    pd.fragmentFunction = df != MTLPixelFormatInvalid ? c->fsd : c->fs;
    pd.colorAttachments[0].pixelFormat = f;
    pd.depthAttachmentPixelFormat = df;
    NSError *err = nil;
    id<MTLRenderPipelineState> ps = [c->dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!ps) return nil;
    int slot = 0;
    for (int k = 0; k < 4; k++) if (!c->draw[k]) { slot = k; break; }
    c->draw[slot] = ps; c->draw_fmt[slot][0] = f; c->draw_fmt[slot][1] = df;
    return ps;
}

int vxw_encode(vxw_core *c, void *command_buffer, const vxw_params *p, void *b_, void *a_, void *mv_, void *depth_, void *out_) {
    return vxw_encode_eye(c, command_buffer, p, b_, a_, mv_, depth_, out_, NULL);
}

int vxw_encode_eye(vxw_core *c, void *command_buffer, const vxw_params *p, void *b_, void *a_, void *mv_, void *depth_, void *out_,
                   void *out_depth_) {
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
    id<MTLTexture> b = (__bridge id<MTLTexture>)b_, a = (__bridge id<MTLTexture>)a_, mv = (__bridge id<MTLTexture>)mv_,
                   depth = (__bridge id<MTLTexture>)depth_, out = (__bridge id<MTLTexture>)out_, od = (__bridge id<MTLTexture>)out_depth_;
    if (!c || !cb || !p || !b || !mv || !depth || !out || !p->w || !p->h) return 0;
    uint32_t ow = (uint32_t)out.width, oh = (uint32_t)out.height;
    if (od && (od.width != ow || od.height != oh)) return 0;
    if (!sized(c, p->w, p->h, ow, oh)) return 0;
    MTLPixelFormat df = od ? od.pixelFormat : MTLPixelFormatInvalid;
    int od_depth = df == MTLPixelFormatDepth32Float || df == MTLPixelFormatDepth16Unorm || df == MTLPixelFormatDepth32Float_Stencil8;
    vxw_gpu_params P = { *p, ow, oh, c->mw, c->mh, c->kw, c->kh, c->bits, od ? VXW2_DEPTH_OUT : 0 };
    if (!a) { a = b; P.p.flags &= ~(uint32_t)VXW_STILL; }
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit fillBuffer:c->keys range:NSMakeRange(0, c->keys.length) value:0];
    [blit fillBuffer:c->mx range:NSMakeRange(0, 4) value:0];
    [blit endEncoding];
    MTLSize tg = MTLSizeMake(16, 16, 1);
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:c->map];
    [enc setTexture:b atIndex:0];
    [enc setTexture:a atIndex:1];
    [enc setTexture:mv atIndex:2];
    [enc setTexture:depth atIndex:3];
    [enc setTexture:c->mapt atIndex:4];
    [enc setBuffer:c->keys offset:0 atIndex:0];
    [enc setBytes:&P length:sizeof P atIndex:1];
    [enc setBuffer:c->state offset:0 atIndex:2];
    [enc setBuffer:c->mx offset:0 atIndex:3];
    [enc dispatchThreads:MTLSizeMake(c->mw, c->mh, 1) threadsPerThreadgroup:tg];
    [enc endEncoding];
    enc = [cb computeCommandEncoder];                              // (after the map's atomics)
    [enc setComputePipelineState:c->resolve];
    [enc setTexture:c->mapt atIndex:0];
    [enc setTexture:c->disp atIndex:1];
    [enc setBuffer:c->keys offset:0 atIndex:0];
    [enc setBytes:&P length:sizeof P atIndex:1];
    [enc dispatchThreads:MTLSizeMake(c->kw, c->kh, 1) threadsPerThreadgroup:tg];
    if ((out.usage & MTLTextureUsageShaderWrite) && (!od || (!od_depth && (od.usage & MTLTextureUsageShaderWrite)))) {
        [enc memoryBarrierWithScope:MTLBarrierScopeTextures];
        [enc setComputePipelineState:c->gather];
        [enc setTexture:b atIndex:0];
        [enc setTexture:c->disp atIndex:1];
        [enc setTexture:out atIndex:2];
        [enc setTexture:od ? od : c->dummy atIndex:3];
        [enc dispatchThreads:MTLSizeMake(ow, oh, 1) threadsPerThreadgroup:tg];
        [enc endEncoding];
    } else {
        [enc endEncoding];
        if (od && !od_depth) return 0;                             // (a colour depth-out needs shader writes)
        id<MTLRenderPipelineState> ps = draw_pipeline(c, out.pixelFormat, od ? df : MTLPixelFormatInvalid);
        if (!ps) return 0;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = out;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (od) {
            rp.depthAttachment.texture = od;
            rp.depthAttachment.loadAction = MTLLoadActionDontCare;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
        }
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        [re setRenderPipelineState:ps];
        if (od) [re setDepthStencilState:c->dss];
        [re setFragmentTexture:b atIndex:0];
        [re setFragmentTexture:c->disp atIndex:1];
        [re setFragmentBytes:&P length:sizeof P atIndex:1];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [re endEncoding];
    }
    id<MTLBuffer> mx = c->mx;
    vxw_core *core = c;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        atomic_store(&core->last_motion, *(volatile uint32_t *)mx.contents / 16.0f);
    }];
    return 1;
}

void vxw_encode_conv(vxw_core *c, void *command_buffer, const vxw_params *p, void *depth_, float pct) {
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
    id<MTLTexture> depth = (__bridge id<MTLTexture>)depth_;
    if (!c || !cb || !p || !depth) return;
    vxw_gpu_params P = { *p, c->ow, c->oh, c->mw, c->mh, c->kw, c->kh, c->bits, 0 };
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:c->conv];
    [enc setTexture:depth atIndex:0];
    [enc setBytes:&P length:sizeof P atIndex:1];
    [enc setBuffer:c->state offset:0 atIndex:2];
    [enc setBytes:&pct length:sizeof pct atIndex:3];
    [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    [enc endEncoding];
}

float vxw_last_motion(vxw_core *c) { return c ? atomic_load(&c->last_motion) : -1; }

int vxw_projection_from_fsr2(vxw_params *p, float tan_half_fov_y, const float d2v[4]) {
    if (!p || !(tan_half_fov_y > 0.05f && tan_half_fov_y < 5) || !isfinite(d2v[0]) || !isfinite(d2v[1]) || d2v[1] == 0 ||
        !(d2v[2] > 0.01f && d2v[2] < 20) || !(d2v[3] > 0.01f && d2v[3] < 20)) return 0;
    p->z_b = d2v[0];
    p->z_a = d2v[1];
    p->tan_x = d2v[2];
    p->tan_y = d2v[3];
    return 1;
}
