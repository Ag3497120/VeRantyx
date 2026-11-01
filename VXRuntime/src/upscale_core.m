// The upscale core (see upscale_core.h): MetalFX's temporal or spatial scaler in place of a game's upscaler.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <TargetConditionals.h>
#include <stdio.h>
#include "upscale_core.h"

#if TARGET_OS_VISION
#define VXU_HAS_TEMPORAL 0   // (API_UNAVAILABLE(visionos) in the visionOS 26.0 SDK)
#else
#define VXU_HAS_TEMPORAL 1
#endif

static const char *vxu_src =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "kernel void vxu_copy(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],\n"
    "                     constant uint2 &size [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {\n"
    "    if (p.x >= size.x || p.y >= size.y) return;\n"
    "    dst.write(src.read(p), p);\n"
    "}\n";

static struct {
    __strong id<MTLDevice> dev;
    __strong id<MTLComputePipelineState> copy;
    __strong id scaler;                       // id<MTLFXTemporalScaler> or id<MTLFXSpatialScaler>
    __strong id<MTLTexture> out, color, depth;  // the scaler's output; colour and depth copies when needed
    __strong id<MTLBuffer> depth_buf;
    int mode, ae, hdr, color_copy, depth_copy;
    MTLPixelFormat cf, df, mf;
    NSUInteger iw, ih, ow, oh;
    int failed;                               // (this key could not be made: not again every frame)
    int encoded;
} U;

int vxu_supported(void *device, int mode) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)device;
    if (!dev) return 0;
#if VXU_HAS_TEMPORAL
    if (mode == VXU_TEMPORAL) return [MTLFXTemporalScalerDescriptor supportsDevice:dev] ? 1 : 0;
#endif
    if (mode == VXU_SPATIAL) return [MTLFXSpatialScalerDescriptor supportsDevice:dev] ? 1 : 0;
    return 0;
}

static id<MTLComputePipelineState> copy_pipeline(id<MTLDevice> dev) {
    if (U.copy && U.dev == dev) return U.copy;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(vxu_src) options:nil error:&err];
    id<MTLFunction> fn = [lib newFunctionWithName:@"vxu_copy"];
    U.copy = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
    U.dev = dev;
    return U.copy;
}

static void encode_copy(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, uint32_t w, uint32_t h) {
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    uint32_t size[2] = { w, h };
    [e setComputePipelineState:U.copy];
    [e setTexture:src atIndex:0];
    [e setTexture:dst atIndex:1];
    [e setBytes:size length:sizeof size atIndex:0];
    [e dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e endEncoding];
}

static int is_depth_stencil(MTLPixelFormat f) {
    return f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatX32_Stencil8;
}

// The scaler for this frame's key, made when the key changes; colour and depth copies tried when the formats as they
// are cannot make one.
static int make_scaler(id<MTLDevice> dev, int mode, const vxu_frame *f, id<MTLTexture> color, id<MTLTexture> depth,
                       id<MTLTexture> motion, char *why, size_t why_len, int *note) {
    MTLPixelFormat cf = color.pixelFormat, df = depth ? depth.pixelFormat : MTLPixelFormatInvalid, mf = motion ? motion.pixelFormat : MTLPixelFormatInvalid;
    if (U.scaler && U.mode == mode && U.cf == cf && U.df == df && U.mf == mf && U.iw == color.width && U.ih == color.height &&
        U.ow == f->out_w && U.oh == f->out_h && U.ae == f->auto_exposure && U.hdr == f->hdr) return 1;
    if (U.failed && U.mode == mode && U.cf == cf && U.df == df && U.mf == mf && U.iw == color.width && U.ih == color.height &&
        U.ow == f->out_w && U.oh == f->out_h && U.ae == f->auto_exposure && U.hdr == f->hdr) {
        snprintf(why, why_len, "no scaler for these formats and sizes (tried before)");
        return 0;
    }
    U.scaler = nil; U.out = nil; U.color = nil; U.depth = nil; U.depth_buf = nil;
    U.mode = mode; U.cf = cf; U.df = df; U.mf = mf; U.iw = color.width; U.ih = color.height; U.ow = f->out_w; U.oh = f->out_h;
    U.ae = f->auto_exposure; U.hdr = f->hdr; U.failed = 1;
    float scale = (float)f->out_w / (float)(f->content_w ? f->content_w : 1);
    id scaler = nil;
    int cc = 0, dc = 0;
    for (int attempt = 0; attempt < 4 && !scaler; attempt++) {
        cc = attempt & 1; dc = (attempt >> 1) & 1;
        if (dc && !is_depth_stencil(df)) continue;
        if (cc && cf == MTLPixelFormatRGBA16Float) continue;
        MTLPixelFormat ucf = cc ? MTLPixelFormatRGBA16Float : cf, udf = dc ? MTLPixelFormatDepth32Float : df;
        (void)udf;
#if VXU_HAS_TEMPORAL
        if (mode == VXU_TEMPORAL) {
            MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
            d.colorTextureFormat = ucf; d.depthTextureFormat = udf; d.motionTextureFormat = mf;
            d.outputTextureFormat = MTLPixelFormatRGBA16Float;
            d.inputWidth = color.width; d.inputHeight = color.height;
            d.outputWidth = f->out_w; d.outputHeight = f->out_h;
            d.autoExposureEnabled = f->auto_exposure ? YES : NO;
            d.requiresSynchronousInitialization = f->sync_init ? YES : NO;
            float maxs = [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:dev];
            float mins = [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:dev];
            d.inputContentPropertiesEnabled = YES;
            d.inputContentMinScale = mins;
            d.inputContentMaxScale = maxs;
            if (scale > maxs + 1e-3f || scale < mins - 1e-3f) {
                snprintf(why, why_len, "MetalFX's temporal scaler takes %.2f-%.2fx on this GPU, %.2fx asked", mins, maxs, scale);
                return 0;
            }
            scaler = [d newTemporalScalerWithDevice:dev];
        }
#endif
        if (mode == VXU_SPATIAL) {
            if (dc) continue;
            MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
            d.colorTextureFormat = ucf; d.outputTextureFormat = MTLPixelFormatRGBA16Float;
            d.inputWidth = color.width; d.inputHeight = color.height;
            d.outputWidth = f->out_w; d.outputHeight = f->out_h;
            d.colorProcessingMode = f->hdr ? MTLFXSpatialScalerColorProcessingModeHDR : MTLFXSpatialScalerColorProcessingModePerceptual;
            scaler = [d newSpatialScalerWithDevice:dev];
        }
    }
    if (!scaler) {
        snprintf(why, why_len, "MetalFX made no %s scaler for colour format %lu, depth %lu, motion %lu, %lux%lu -> %ux%u",
                 mode == VXU_TEMPORAL ? "temporal" : "spatial", (unsigned long)cf, (unsigned long)df, (unsigned long)mf,
                 (unsigned long)color.width, (unsigned long)color.height, f->out_w, f->out_h);
        return 0;
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:f->out_w height:f->out_h mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = [(id<MTLFXSpatialScalerBase>)scaler outputTextureUsage] | MTLTextureUsageShaderRead;
    U.out = [dev newTextureWithDescriptor:td];
    if (cc) {
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:color.width height:color.height mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        U.color = [dev newTextureWithDescriptor:td];
    }
    if (dc) {
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:depth.width height:depth.height mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead;
        U.depth = [dev newTextureWithDescriptor:td];
        U.depth_buf = [dev newBufferWithLength:depth.width * depth.height * 4 options:MTLResourceStorageModePrivate];
    }
    if (!U.out || (cc && !U.color) || (dc && (!U.depth || !U.depth_buf))) { snprintf(why, why_len, "no memory for the scaler's textures"); return 0; }
    U.scaler = scaler; U.color_copy = cc; U.depth_copy = dc; U.failed = 0;
    *note = 1;
    snprintf(why, why_len, "MetalFX %s scaler: colour %lu%s, depth %lu%s, motion %lu, %lux%lu (content %ux%u) -> %ux%u, %.2fx%s",
             mode == VXU_TEMPORAL ? "temporal" : "spatial", (unsigned long)cf, cc ? " (copied to RGBA16Float)" : "",
             (unsigned long)df, dc ? " (copied to Depth32Float)" : "", (unsigned long)mf, (unsigned long)color.width,
             (unsigned long)color.height, f->content_w, f->content_h, f->out_w, f->out_h, scale,
             mode == VXU_TEMPORAL ? (f->auto_exposure ? ", auto exposure" : ", exposure 1") : (f->hdr ? ", HDR" : ", perceptual"));
    return 1;
}

int vxu_encode(void *command_buffer, int mode, const vxu_frame *f, char *why, size_t why_len, int *note) {
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
    id<MTLTexture> color = (__bridge id<MTLTexture>)f->color, depth = (__bridge id<MTLTexture>)f->depth;
    id<MTLTexture> motion = (__bridge id<MTLTexture>)f->motion, output = (__bridge id<MTLTexture>)f->output;
    *note = 0;
    why[0] = 0;
    if (!cb || !color || !output || (mode == VXU_TEMPORAL && (!depth || !motion))) { snprintf(why, why_len, "inputs missing"); return -1; }
#if !VXU_HAS_TEMPORAL
    if (mode == VXU_TEMPORAL) { snprintf(why, why_len, "no temporal scaler on this platform"); return -1; }
#endif
    if (!f->content_w || !f->content_h || f->content_w > color.width || f->content_h > color.height ||
        !f->out_w || !f->out_h || f->out_w > output.width || f->out_h > output.height) {
        snprintf(why, why_len, "sizes do not fit: content %ux%u in %lux%lu, display %ux%u in %lux%lu", f->content_w, f->content_h,
                 (unsigned long)color.width, (unsigned long)color.height, f->out_w, f->out_h, (unsigned long)output.width, (unsigned long)output.height);
        return -1;
    }
    id<MTLDevice> dev = cb.device;
    if (!copy_pipeline(dev)) { snprintf(why, why_len, "the copy kernel did not compile"); return -1; }
    int made = 0;
    if (!make_scaler(dev, mode, f, color, depth, motion, why, why_len, &made)) return -1;
    if (made) *note = 1;
    int reset = f->reset || made;
    (void)reset;
    if (U.color_copy) { encode_copy(cb, color, U.color, (uint32_t)color.width, (uint32_t)color.height); color = U.color; }
    if (U.depth_copy) {
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        NSUInteger w = depth.width, h = depth.height;
        [b copyFromTexture:depth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                  toBuffer:U.depth_buf destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4
                   options:MTLBlitOptionDepthFromDepthStencil];
        [b copyFromBuffer:U.depth_buf sourceOffset:0 sourceBytesPerRow:w * 4 sourceBytesPerImage:w * h * 4 sourceSize:MTLSizeMake(w, h, 1)
                toTexture:U.depth destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
        depth = U.depth;
    }
#if VXU_HAS_TEMPORAL
    if (mode == VXU_TEMPORAL) {
        id<MTLFXTemporalScaler> s = U.scaler;
        s.colorTexture = color; s.depthTexture = depth; s.motionTexture = motion; s.outputTexture = U.out;
        s.inputContentWidth = f->content_w; s.inputContentHeight = f->content_h;
        s.jitterOffsetX = f->jitter_x; s.jitterOffsetY = f->jitter_y;
        s.motionVectorScaleX = f->mv_scale_x; s.motionVectorScaleY = f->mv_scale_y;
        s.preExposure = f->pre_exposure > 0 ? f->pre_exposure : 1.0f;
        s.depthReversed = f->depth_reversed ? YES : NO;
        s.reset = reset ? YES : NO;
        [s encodeToCommandBuffer:cb];
    }
#endif
    if (mode == VXU_SPATIAL) {
        id<MTLFXSpatialScaler> s = U.scaler;
        s.colorTexture = color; s.outputTexture = U.out;
        s.inputContentWidth = f->content_w; s.inputContentHeight = f->content_h;
        [s encodeToCommandBuffer:cb];
    }
    encode_copy(cb, U.out, output, f->out_w, f->out_h);
    if (!U.encoded++ && !made) { *note = 1; snprintf(why, why_len, "first frame encoded"); }
    return 0;
}
