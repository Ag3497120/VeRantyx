// Vulkan commands Metal has no single command for, done with small shaders of our own: scaled blits, clears of
// attachments inside a render pass and of whole images outside one, buffer fills with a 32-bit pattern, query copies.
#include "vk_impl.h"
#include "vk_encode.h"
#include <stdlib.h>
#include <string.h>

#define TRACE(...) do { if (vk_trace()) vx_log("vulkan: " __VA_ARGS__); } while (0)

static NSString *const k_internal_source =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct BlitParams { float4 src_rect; float depth; uint layer; uint slice; float pad; };\n"
     "struct VOut { float4 pos [[position]]; float2 uv; };\n"
     "struct VOutL { float4 pos [[position]]; float2 uv; uint layer [[render_target_array_index]]; };\n"
     "vertex VOut vk_quad_vs(uint vid [[vertex_id]], constant BlitParams& p [[buffer(0)]]) {\n"
     "    float2 xy = float2(float((vid << 1) & 2), float(vid & 2));\n"
     "    VOut o; o.pos = float4(xy * float2(2.0, -2.0) + float2(-1.0, 1.0), p.depth, 1.0);\n"
     "    o.uv = mix(p.src_rect.xy, p.src_rect.zw, xy); return o; }\n"
     "vertex VOutL vk_quad_layer_vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant BlitParams& p [[buffer(0)]]) {\n"
     "    float2 xy = float2(float((vid << 1) & 2), float(vid & 2));\n"
     "    VOutL o; o.pos = float4(xy * float2(2.0, -2.0) + float2(-1.0, 1.0), p.depth, 1.0);\n"
     "    o.uv = mix(p.src_rect.xy, p.src_rect.zw, xy); o.layer = p.layer + iid; return o; }\n"
     "fragment float4 vk_blit_float(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    return t.sample(s, in.uv, level(0)); }\n"
     "fragment uint4 vk_blit_uint(VOut in [[stage_in]], texture2d<uint> t [[texture(0)]]) {\n"
     "    return t.read(uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1)))); }\n"
     "fragment int4 vk_blit_sint(VOut in [[stage_in]], texture2d<int> t [[texture(0)]]) {\n"
     "    return t.read(uint2(min(in.uv * float2(t.get_width(), t.get_height()), float2(t.get_width() - 1, t.get_height() - 1)))); }\n"
     "struct DepthOut { float d [[depth(any)]]; };\n"
     "fragment DepthOut vk_blit_depth(VOut in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    DepthOut o; o.d = t.sample(s, in.uv); return o; }\n"
     "kernel void vk_fill(device uint *dst [[buffer(0)]], constant uint2 &p [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
     "    if (i < p.y) dst[i] = p.x; }\n"
     "struct QueryCopy { uint count, flags; ulong stride; };\n"
     "kernel void vk_copy_queries(device uchar *dst [[buffer(0)]], const device ulong *src [[buffer(1)]],\n"
     "                            constant QueryCopy &p [[buffer(2)]], const device uchar *avail [[buffer(3)]], uint i [[thread_position_in_grid]]) {\n"
     "    if (i >= p.count) return;\n"
     "    device uchar *d = dst + i * p.stride;\n"
     "    bool a = avail[i] != 0;\n"
     "    if (p.flags & 1u) { *(device ulong *)d = src[i]; if (p.flags & 4u) *(device ulong *)(d + 8) = a ? 1ul : 0ul; }\n"
     "    else { *(device uint *)d = uint(src[i]); if (p.flags & 4u) *(device uint *)(d + 4) = a ? 1u : 0u; } }\n";

void vk_internal_init(VkDevice dev) {
    NSError *e = nil;
    dev->internal = [dev->mtl newLibraryWithSource:k_internal_source options:nil error:&e];
    if (!dev->internal) vx_log("vulkan: internal shaders did not compile: %s", e.localizedDescription.UTF8String);
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = d.magFilter = MTLSamplerMinMagFilterLinear;
    d.sAddressMode = d.tAddressMode = MTLSamplerAddressModeClampToEdge;
    dev->internal_linear = [dev->mtl newSamplerStateWithDescriptor:d];
    d.minFilter = d.magFilter = MTLSamplerMinMagFilterNearest;
    dev->internal_nearest = [dev->mtl newSamplerStateWithDescriptor:d];
}

static NSMutableDictionary *pipeline_cache(VkDevice dev) {
    static NSMutableDictionary *caches;
    if (!caches) caches = [NSMutableDictionary new];
    NSValue *key = [NSValue valueWithPointer:dev];
    NSMutableDictionary *c = caches[key];
    if (!c) { c = [NSMutableDictionary new]; caches[key] = c; }
    return c;
}

id<MTLComputePipelineState> vk_internal_compute(VkDevice dev, NSString *name) {
    pthread_mutex_lock(&dev->internal_lock);
    NSMutableDictionary *cache = pipeline_cache(dev);
    id<MTLComputePipelineState> p = cache[name];
    if (!p && dev->internal) {
        NSError *e = nil;
        p = [dev->mtl newComputePipelineStateWithFunction:[dev->internal newFunctionWithName:name] error:&e];
        if (p) cache[name] = p;
        else vx_log("vulkan: internal %s: %s", name.UTF8String, e.localizedDescription.UTF8String);
    }
    pthread_mutex_unlock(&dev->internal_lock);
    return p;
}

id<MTLRenderPipelineState> vk_internal_render(VkDevice dev, NSString *vs, NSString *fs, MTLPixelFormat color, MTLPixelFormat depth,
                                              MTLPixelFormat stencil, NSUInteger samples, int write_depth) {
    NSString *key = [NSString stringWithFormat:@"%@/%@/%lu/%lu/%lu/%lu/%d", vs, fs, (unsigned long)color, (unsigned long)depth,
                     (unsigned long)stencil, (unsigned long)samples, write_depth];
    pthread_mutex_lock(&dev->internal_lock);
    NSMutableDictionary *cache = pipeline_cache(dev);
    id<MTLRenderPipelineState> p = cache[key];
    if (!p && dev->internal) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [dev->internal newFunctionWithName:vs];
        d.fragmentFunction = fs ? [dev->internal newFunctionWithName:fs] : nil;
        d.colorAttachments[0].pixelFormat = color;
        d.depthAttachmentPixelFormat = depth;
        d.stencilAttachmentPixelFormat = stencil;
        d.rasterSampleCount = samples ? samples : 1;
        if ([vs isEqualToString:@"vk_quad_layer_vs"]) d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        NSError *e = nil;
        p = [dev->mtl newRenderPipelineStateWithDescriptor:d error:&e];
        if (p) cache[key] = p;
        else vx_log("vulkan: internal pipeline %s: %s", key.UTF8String, e.localizedDescription.UTF8String);
    }
    pthread_mutex_unlock(&dev->internal_lock);
    return p;
}

// Each helper ends whatever encoder is open and runs its own on the queue's fence chain.
static void end_any(vk_enc *e) { vk_end_encoder(e); }

typedef struct { float src_rect[4]; float depth; uint32_t layer, slice; float pad; } blit_params;

static id<MTLRenderCommandEncoder> one_off_render(vk_enc *e, MTLRenderPassDescriptor *d) {
    end_any(e);
    id<MTLRenderCommandEncoder> r = [e->cb renderCommandEncoderWithDescriptor:d];
    [r waitForFence:e->fence beforeStages:MTLRenderStageVertex];
    return r;
}
static void one_off_end(vk_enc *e, id<MTLRenderCommandEncoder> r) {
    [r updateFence:e->fence afterStages:MTLRenderStageFragment];
    [r endEncoding];
}

void vk_internal_blit(vk_enc *e, VkImage src, VkImage dst, const VkImageBlit2 *rg, VkFilter filter) {
    if (!src->mtl || !dst->mtl) return;
    if (src->type == VK_IMAGE_TYPE_3D || dst->type == VK_IMAGE_TYPE_3D) { TRACE("3D image blit not done"); return; }
    int32_t sx0 = rg->srcOffsets[0].x, sy0 = rg->srcOffsets[0].y, sx1 = rg->srcOffsets[1].x, sy1 = rg->srcOffsets[1].y;
    int32_t dx0 = rg->dstOffsets[0].x, dy0 = rg->dstOffsets[0].y, dx1 = rg->dstOffsets[1].x, dy1 = rg->dstOffsets[1].y;
    uint32_t layers = rg->srcSubresource.layerCount;
    uint32_t sw = src->extent.width >> rg->srcSubresource.mipLevel, sh = src->extent.height >> rg->srcSubresource.mipLevel;
    if (!sw) sw = 1;
    if (!sh) sh = 1;
    // A copy when nothing is scaled or flipped and the formats match.
    if (sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 && sx1 > sx0 && sy1 > sy0 && src->mtl.pixelFormat == dst->mtl.pixelFormat &&
        src->samples == dst->samples) {
        end_any(e);
        id<MTLBlitCommandEncoder> b = [e->cb blitCommandEncoder];
        [b waitForFence:e->fence];
        for (uint32_t l = 0; l < layers; l++)
            [b copyFromTexture:src->mtl sourceSlice:rg->srcSubresource.baseArrayLayer + l sourceLevel:rg->srcSubresource.mipLevel
                  sourceOrigin:MTLOriginMake(sx0, sy0, 0) sourceSize:MTLSizeMake(sx1 - sx0, sy1 - sy0, 1) toTexture:dst->mtl
              destinationSlice:rg->dstSubresource.baseArrayLayer + l destinationLevel:rg->dstSubresource.mipLevel
             destinationOrigin:MTLOriginMake(dx0, dy0, 0)];
        [b updateFence:e->fence];
        [b endEncoding];
        return;
    }
    const vk_format_info *sf = src->fmt, *df = dst->fmt;
    int depth = (df->aspects & VKF_DEPTH) != 0;
    NSString *fs = depth ? @"vk_blit_depth" : df->numeric == VKF_UINT ? @"vk_blit_uint" : df->numeric == VKF_SINT ? @"vk_blit_sint" : @"vk_blit_float";
    if ((sf->aspects & VKF_DEPTH) != depth) { TRACE("blit between depth and color images not done"); return; }
    for (uint32_t l = 0; l < layers; l++) {
        id<MTLTexture> sv = [src->mtl newTextureViewWithPixelFormat:src->mtl.pixelFormat textureType:MTLTextureType2D
                              levels:NSMakeRange(rg->srcSubresource.mipLevel, 1) slices:NSMakeRange(rg->srcSubresource.baseArrayLayer + l, 1)];
        id<MTLTexture> dv = [dst->mtl newTextureViewWithPixelFormat:dst->mtl.pixelFormat textureType:MTLTextureType2D
                              levels:NSMakeRange(rg->dstSubresource.mipLevel, 1) slices:NSMakeRange(rg->dstSubresource.baseArrayLayer + l, 1)];
        if (!sv || !dv) continue;
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        if (depth) { d.depthAttachment.texture = dv; d.depthAttachment.loadAction = MTLLoadActionLoad; d.depthAttachment.storeAction = MTLStoreActionStore; }
        else { d.colorAttachments[0].texture = dv; d.colorAttachments[0].loadAction = MTLLoadActionLoad; d.colorAttachments[0].storeAction = MTLStoreActionStore; }
        id<MTLRenderPipelineState> ps = vk_internal_render(e->dev, @"vk_quad_vs", fs, depth ? MTLPixelFormatInvalid : dv.pixelFormat,
                                                           depth ? dv.pixelFormat : MTLPixelFormatInvalid, MTLPixelFormatInvalid, 1, depth);
        if (!ps) continue;
        id<MTLRenderCommandEncoder> r = one_off_render(e, d);
        [r setRenderPipelineState:ps];
        if (depth) {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            dd.depthCompareFunction = MTLCompareFunctionAlways;
            dd.depthWriteEnabled = YES;
            [r setDepthStencilState:[e->dev->mtl newDepthStencilStateWithDescriptor:dd]];
        }
        // (the destination rectangle as the viewport; flips by the source rectangle's direction)
        int32_t vx = dx0 < dx1 ? dx0 : dx1, vy = dy0 < dy1 ? dy0 : dy1;
        uint32_t vw = (uint32_t)abs(dx1 - dx0), vh = (uint32_t)abs(dy1 - dy0);
        [r setViewport:(MTLViewport){ vx, vy, vw, vh, 0, 1 }];
        blit_params p = { { (float)sx0 / sw, (float)sy0 / sh, (float)sx1 / sw, (float)sy1 / sh }, 0, 0, 0, 0 };
        if (dx1 < dx0) { float t = p.src_rect[0]; p.src_rect[0] = p.src_rect[2]; p.src_rect[2] = t; }
        if (dy1 < dy0) { float t = p.src_rect[1]; p.src_rect[1] = p.src_rect[3]; p.src_rect[3] = t; }
        [r setVertexBytes:&p length:sizeof p atIndex:0];
        [r setFragmentTexture:sv atIndex:0];
        [r setFragmentSamplerState:filter == VK_FILTER_LINEAR && !depth ? e->dev->internal_linear : e->dev->internal_nearest atIndex:0];
        [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        one_off_end(e, r);
    }
}

void vk_internal_clear_image(vk_enc *e, VkImage img, int depth, const VkClearValue *v, const VkImageSubresourceRange *range) {
    if (!img->mtl || img->fmt->compressed) return;
    uint32_t levels = range->levelCount == VK_REMAINING_MIP_LEVELS ? img->mips - range->baseMipLevel : range->levelCount;
    uint32_t layers = range->layerCount == VK_REMAINING_ARRAY_LAYERS ? img->layers - range->baseArrayLayer : range->layerCount;
    if (img->type == VK_IMAGE_TYPE_3D) layers = 1;
    MTLTextureType tt = img->samples > 1 ? MTLTextureType2DMultisample : img->type == VK_IMAGE_TYPE_3D ? MTLTextureType3D : MTLTextureType2D;
    for (uint32_t m = 0; m < levels; m++)
        for (uint32_t l = 0; l < layers; l++) {
            id<MTLTexture> view = [img->mtl newTextureViewWithPixelFormat:img->mtl.pixelFormat textureType:tt
                                    levels:NSMakeRange(range->baseMipLevel + m, 1) slices:NSMakeRange(img->type == VK_IMAGE_TYPE_3D ? 0 : range->baseArrayLayer + l, 1)];
            if (!view) continue;
            MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
            if (depth) {
                if ((range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) && (img->fmt->aspects & VKF_DEPTH)) {
                    d.depthAttachment.texture = view; d.depthAttachment.loadAction = MTLLoadActionClear;
                    d.depthAttachment.clearDepth = v->depthStencil.depth; d.depthAttachment.storeAction = MTLStoreActionStore;
                }
                if ((range->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && (img->fmt->aspects & VKF_STENCIL)) {
                    d.stencilAttachment.texture = view; d.stencilAttachment.loadAction = MTLLoadActionClear;
                    d.stencilAttachment.clearStencil = v->depthStencil.stencil; d.stencilAttachment.storeAction = MTLStoreActionStore;
                }
            } else {
                d.colorAttachments[0].texture = view;
                d.colorAttachments[0].loadAction = MTLLoadActionClear;
                d.colorAttachments[0].storeAction = MTLStoreActionStore;
                const vk_format_info *f = img->fmt;
                d.colorAttachments[0].clearColor = f->numeric == VKF_UINT ?
                    MTLClearColorMake(v->color.uint32[0], v->color.uint32[1], v->color.uint32[2], v->color.uint32[3]) : f->numeric == VKF_SINT ?
                    MTLClearColorMake(v->color.int32[0], v->color.int32[1], v->color.int32[2], v->color.int32[3]) :
                    MTLClearColorMake(v->color.float32[0], v->color.float32[1], v->color.float32[2], v->color.float32[3]);
            }
            if (img->type == VK_IMAGE_TYPE_3D) d.renderTargetArrayLength = view.depth;
            id<MTLRenderCommandEncoder> r = one_off_render(e, d);
            one_off_end(e, r);
        }
}

void vk_internal_fill(vk_enc *e, VkBuffer b, uint64_t offset, uint64_t size, uint32_t data) {
    if (!b->mtl || !size) return;
    end_any(e);
    uint8_t byte = data & 0xff;
    if (data == byte * 0x01010101u) {
        id<MTLBlitCommandEncoder> bl = [e->cb blitCommandEncoder];
        [bl waitForFence:e->fence];
        [bl fillBuffer:b->mtl range:NSMakeRange(b->offset + offset, size) value:byte];
        [bl updateFence:e->fence];
        [bl endEncoding];
        return;
    }
    id<MTLComputePipelineState> ps = vk_internal_compute(e->dev, @"vk_fill");
    if (!ps) return;
    id<MTLComputeCommandEncoder> c = [e->cb computeCommandEncoder];
    [c waitForFence:e->fence];
    [c setComputePipelineState:ps];
    [c setBuffer:b->mtl offset:b->offset + offset atIndex:0];
    uint32_t p[2] = { data, (uint32_t)(size / 4) };
    [c setBytes:p length:sizeof p atIndex:1];
    [c dispatchThreads:MTLSizeMake(p[1], 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [c updateFence:e->fence];
    [c endEncoding];
}

void vk_internal_copy_queries(vk_enc *e, VkQueryPool pool, uint32_t first, uint32_t count, VkBuffer dst, uint64_t offset,
                              uint64_t stride, VkQueryResultFlags flags) {
    struct { VkQueryPool pool; uint32_t first, count; VkBuffer dst; uint64_t offset, stride; VkQueryResultFlags flags; } cc =
        { pool, first, count, dst, offset, stride, flags }, *c = &cc;
    if (!c->dst->mtl || !c->count) return;
    id<MTLComputePipelineState> ps = vk_internal_compute(e->dev, @"vk_copy_queries");
    if (!ps) return;
    end_any(e);
    // (availability: the queries this command buffer ends are available by the time this runs, in order)
    uint8_t avail[4096];
    uint32_t n = c->count < 4096 ? c->count : 4096;
    for (uint32_t i = 0; i < n; i++) avail[i] = c->first + i < c->pool->count && c->pool->available[c->first + i] ? 1 : 0;
    id<MTLComputeCommandEncoder> ce = [e->cb computeCommandEncoder];
    [ce waitForFence:e->fence];
    [ce setComputePipelineState:ps];
    [ce setBuffer:c->dst->mtl offset:c->dst->offset + c->offset atIndex:0];
    [ce setBuffer:c->pool->results offset:c->first * 8 atIndex:1];
    struct { uint32_t count, flags; uint64_t stride; } p = { n, c->flags, c->stride };
    [ce setBytes:&p length:sizeof p atIndex:2];
    [ce setBytes:avail length:(n + 15) & ~15u atIndex:3];
    [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [ce updateFence:e->fence];
    [ce endEncoding];
}

// Clears inside the current render pass: a quad per rectangle with a pipeline for the pass's formats, writing only the
// cleared attachments (its MSL made for those formats).
static void clear_attachments_impl(vk_enc *e, id<MTLRenderCommandEncoder> r, const uint32_t *formats, uint32_t color_count,
                                        MTLPixelFormat depth_fmt, MTLPixelFormat stencil_fmt, uint32_t samples, uint32_t width, uint32_t height,
                                        uint32_t na, const VkClearAttachment *atts, uint32_t nr, const VkClearRect *rects, int layered) {
    VkDevice dev = e->dev;
    uint32_t mask = 0;
    int clear_depth = 0, clear_stencil = 0;
    float colors[8][4] = { { 0 } };
    float depth = 0;
    uint32_t stencil = 0;
    for (uint32_t i = 0; i < na; i++) {
        if (atts[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
            uint32_t c = atts[i].colorAttachment;
            if (c < color_count && formats[c]) { mask |= 1u << c; memcpy(colors[c], atts[i].clearValue.color.float32, 16); }
        }
        if (atts[i].aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) { clear_depth = depth_fmt != MTLPixelFormatInvalid; depth = atts[i].clearValue.depthStencil.depth; }
        if (atts[i].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) { clear_stencil = stencil_fmt != MTLPixelFormatInvalid; stencil = atts[i].clearValue.depthStencil.stencil; }
    }
    if (!mask && !clear_depth && !clear_stencil) return;
    // The pipeline: outputs typed by each attachment's format class, written only where cleared.
    NSMutableString *key = [NSMutableString stringWithFormat:@"clear/%u/%lu/%lu/%u/%d/%d/%d", mask, (unsigned long)depth_fmt,
                            (unsigned long)stencil_fmt, samples, clear_depth, clear_stencil, layered];
    for (uint32_t i = 0; i < color_count; i++) [key appendFormat:@"/%u", formats[i]];
    pthread_mutex_lock(&dev->internal_lock);
    NSMutableDictionary *cache = pipeline_cache(dev);
    id<MTLRenderPipelineState> ps = cache[key];
    pthread_mutex_unlock(&dev->internal_lock);
    if (!ps) {
        NSMutableString *src = [NSMutableString stringWithString:@"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct P { float4 c[8]; float depth; uint layer; };\n"
            "struct V { float4 pos [[position]]; uint layer [[render_target_array_index]]; };\n"
            "vertex V vs(uint vid [[vertex_id]], uint iid [[instance_id]], constant P& p [[buffer(0)]]) {\n"
            "  float2 xy = float2(float((vid << 1) & 2), float(vid & 2));\n"
            "  V o; o.pos = float4(xy * float2(2.0, -2.0) + float2(-1.0, 1.0), p.depth, 1.0); o.layer = p.layer + iid; return o; }\n"
            ];
        uint32_t outputs = 0;
        for (uint32_t i = 0; i < color_count; i++) if (formats[i]) outputs++;
        if (outputs) [src appendString:@"struct O {\n"];
        for (uint32_t i = 0; i < color_count; i++) {
            if (!formats[i]) continue;
            const vk_format_info *f = vk_format((VkFormat)formats[i]);
            const char *t = f && f->numeric == VKF_UINT ? "uint4" : f && f->numeric == VKF_SINT ? "int4" : "float4";
            [src appendFormat:@"  %s c%u [[color(%u)]];\n", t, i, i];
        }
        // (no color attachment: a fragment function without outputs, the depth/stencil state does the clear)
        [src appendString:outputs ? @"};\nfragment O fs(V in [[stage_in]], constant P& p [[buffer(0)]]) { O o;\n" :
                                    @"fragment void fs(V in [[stage_in]], constant P& p [[buffer(0)]]) {\n"];
        for (uint32_t i = 0; i < color_count; i++) {
            if (!formats[i]) continue;
            const vk_format_info *f = vk_format((VkFormat)formats[i]);
            const char *t = f && f->numeric == VKF_UINT ? "uint4" : f && f->numeric == VKF_SINT ? "int4" : "float4";
            [src appendFormat:@"  o.c%u = %s(as_type<%s>(p.c[%u]));\n", i, t, f && f->numeric != VKF_FLOAT ? t : "float4", i];
        }
        [src appendString:outputs ? @"  return o; }\n" : @"}\n"];
        NSError *err = nil;
        id<MTLLibrary> lib = [dev->mtl newLibraryWithSource:src options:nil error:&err];
        if (!lib) { vx_log("vulkan: clear shader: %s", err.localizedDescription.UTF8String); return; }
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vs"];
        d.fragmentFunction = [lib newFunctionWithName:@"fs"];
        for (uint32_t i = 0; i < color_count; i++) {
            if (!formats[i]) continue;
            const vk_format_info *f = vk_format((VkFormat)formats[i]);
            d.colorAttachments[i].pixelFormat = f ? f->mtl : MTLPixelFormatInvalid;
            d.colorAttachments[i].writeMask = (mask & (1u << i)) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        }
        d.depthAttachmentPixelFormat = depth_fmt;
        d.stencilAttachmentPixelFormat = stencil_fmt;
        d.rasterSampleCount = samples ? samples : 1;
        d.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        ps = [dev->mtl newRenderPipelineStateWithDescriptor:d error:&err];
        if (!ps) { vx_log("vulkan: clear pipeline: %s", err.localizedDescription.UTF8String); return; }
        pthread_mutex_lock(&dev->internal_lock);
        cache[key] = ps;
        pthread_mutex_unlock(&dev->internal_lock);
    }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    dd.depthWriteEnabled = clear_depth;
    if (clear_stencil) {
        MTLStencilDescriptor *s = [MTLStencilDescriptor new];
        s.stencilCompareFunction = MTLCompareFunctionAlways;
        s.depthStencilPassOperation = MTLStencilOperationReplace;
        s.writeMask = 0xff;
        dd.frontFaceStencil = s;
        dd.backFaceStencil = s;
    }
    [r setRenderPipelineState:ps];
    [r setDepthStencilState:[dev->mtl newDepthStencilStateWithDescriptor:dd]];
    [r setStencilReferenceValue:stencil];
    [r setCullMode:MTLCullModeNone];
    [r setTriangleFillMode:MTLTriangleFillModeFill];
    [r setDepthBias:0 slopeScale:0 clamp:0];
    for (uint32_t i = 0; i < nr; i++) {
        const VkClearRect *cr = &rects[i];
        struct { float c[8][4]; float depth; uint32_t layer; uint32_t pad[2]; } p;
        memcpy(p.c, colors, sizeof p.c);
        p.depth = depth;
        p.layer = layered ? cr->baseArrayLayer : 0;
        int64_t x = cr->rect.offset.x, y = cr->rect.offset.y, w = cr->rect.extent.width, h = cr->rect.extent.height;
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > width) w = width - x;
        if (y + h > height) h = height - y;
        if (w <= 0 || h <= 0) continue;
        [r setViewport:(MTLViewport){ x, y, w, h, 0, 1 }];
        [r setScissorRect:(MTLScissorRect){ (NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h }];
        [r setVertexBytes:&p length:sizeof p atIndex:0];
        [r setFragmentBytes:&p length:sizeof p atIndex:0];
        [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:layered ? cr->layerCount : 1];
    }
}

void vk_internal_clear_attachments(vk_enc *e, uint32_t na, const VkClearAttachment *atts, uint32_t nr, const VkClearRect *rects) {
    if (!e->render) return;
    uint32_t formats[8] = { 0 };
    for (uint32_t i = 0; i < e->pass.color_count; i++)
        if (e->pass.colors[i].view) formats[i] = (uint32_t)e->pass.colors[i].view->format;
    MTLPixelFormat depth = MTLPixelFormatInvalid, stencil = MTLPixelFormatInvalid;
    if (e->pass.depth.view && e->pass.depth.view->mtl) {
        if (e->pass.has_depth) depth = e->pass.depth.view->mtl.pixelFormat;
        if (e->pass.has_stencil) stencil = e->pass.depth.view->mtl.pixelFormat;
    }
    clear_attachments_impl(e, e->render, formats, e->pass.color_count, depth, stencil, e->pass_samples, e->pass_width, e->pass_height,
                           na, atts, nr, rects, e->pass.layers > 1);
}

// Present into a layer whose format differs from the image's: a full-size draw.
VkResult vk_present_blit(VkQueue q, id<MTLCommandBuffer> cb, VkImage img, id<MTLTexture> target);
VkResult vk_present_blit(VkQueue q, id<MTLCommandBuffer> cb, VkImage img, id<MTLTexture> target) {
    VkDevice dev = q->device;
    id<MTLRenderPipelineState> ps = vk_internal_render(dev, @"vk_quad_vs", @"vk_blit_float", target.pixelFormat, MTLPixelFormatInvalid,
                                                       MTLPixelFormatInvalid, 1, 0);
    if (!ps) return VK_ERROR_INITIALIZATION_FAILED;
    if (!q->last_fence) q->last_fence = [dev->mtl newFence];
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = target;
    d.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> r = [cb renderCommandEncoderWithDescriptor:d];
    [r waitForFence:q->last_fence beforeStages:MTLRenderStageVertex];
    [r setRenderPipelineState:ps];
    blit_params p = { { 0, 0, 1, 1 }, 0, 0, 0, 0 };
    [r setVertexBytes:&p length:sizeof p atIndex:0];
    [r setFragmentTexture:img->mtl atIndex:0];
    [r setFragmentSamplerState:dev->internal_linear atIndex:0];
    [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [r updateFence:q->last_fence afterStages:MTLRenderStageFragment];
    [r endEncoding];
    return VK_SUCCESS;
}

#ifdef __aarch64__
#include "thunks/vk_internal.m.inc"
#endif
