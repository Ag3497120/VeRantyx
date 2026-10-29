// upscale_core (MetalFX in place of FSR2) on synthetic frames shaped as Cyberpunk's: render-size content at the top
// left of display-size textures (colour RGBA16Float or RG11B10Float, depth Depth32Float_Stencil8, motion RG16Float),
// a jittered horizontal ramp drawn each frame; the output must be the ramp at display size within a tolerance, in a
// game-owned output texture of another format (RGBA16Float and RG11B10Float), and the GPU time per frame is printed.
// The jitter is FSR2's convention (the image moved by +jitter pixels, y down) passed as it is: a fine diagonal
// pattern then comes back within 0.05 at 2x (VXU_JSX/VXU_JSY=-1 turn a sign: 0.09-0.24).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "upscale_core.h"

static const char *src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void draw(texture2d<float, access::write> c [[texture(0)]], texture2d<float, access::write> m [[texture(1)]],\n"
    "                 constant float4 &k [[buffer(0)]], uint2 p [[thread_position_in_grid]]) {\n"
    "    if (p.x >= uint(k.z) || p.y >= uint(k.w)) return;\n"
    "    float x = (float(p.x) + 0.5 - k.x) / k.z;\n"                // (the jittered sample's place, 0..1)
    "    float y = (float(p.y) + 0.5 - k.y) / k.w;\n"
    "    c.write(float4(x, y, 0.5 + 0.5 * sin(6.2831853 * (x * k.z + y * k.w) / 3.5), 1), p);\n"
    "    m.write(float4(0), p);\n"
    "}\n"
    "kernel void depthfill(texture2d<float, access::write> d [[texture(0)]], uint2 p [[thread_position_in_grid]]) { d.write(float4(0.5), p); }\n";

static float half_f(uint16_t h) {
    uint32_t e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : ldexpf((float)(m | 1024), (int)e - 25);
    return h & 0x8000 ? -v : v;
}

static int run(id<MTLDevice> dev, int mode, MTLPixelFormat cf, MTLPixelFormat of, uint32_t rw, uint32_t rh, uint32_t W, uint32_t H) {
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(src) options:nil error:&err];
    id<MTLComputePipelineState> draw = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"draw"] error:&err];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:cf width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> color = [dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatRG16Float;
    id<MTLTexture> mv = [dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatDepth32Float_Stencil8; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> depth = [dev newTextureWithDescriptor:td];
    td.pixelFormat = of; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> out = [dev newTextureWithDescriptor:td];
    // (depth 0.5 everywhere: a render pass clear)
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = 0.5;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    rp.stencilAttachment.texture = depth; rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.storeAction = MTLStoreActionStore;
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    [cb commit];
    double gpu = 0; int frames = 160, rc = 0;
    for (int i = 0; i < frames; i++) {
        float js = getenv("VXU_JS") ? (float)atof(getenv("VXU_JS")) : 1.0f;
        float jx = -0.5f, jy = -0.5f;                              // (Halton 2, 3 - 0.5, as FSR2 and MetalFX's samples)
        for (int b = 2; b <= 3; b++) { float r = 0, fr = 1; for (int n = i % 32 + 1; n; n /= b) { fr /= b; r += fr * (n % b); } if (b == 2) jx += r; else jy += r; }
        if (i <= 100) cb = [q commandBuffer];   // (frames 100 on in one command buffer: their GPU time together)
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        float k[4] = { jx, jy, (float)rw, (float)rh };
        [e setComputePipelineState:draw]; [e setTexture:color atIndex:0]; [e setTexture:mv atIndex:1]; [e setBytes:k length:16 atIndex:0];
        [e dispatchThreads:MTLSizeMake(rw, rh, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [e endEncoding];
        vxu_frame f = { (__bridge void *)color, (__bridge void *)depth, (__bridge void *)mv, (__bridge void *)out, rw, rh, W, H,
                        jx * (getenv("VXU_JSX") ? (float)atof(getenv("VXU_JSX")) : js), jy * (getenv("VXU_JSY") ? (float)atof(getenv("VXU_JSY")) : js), (float)rw, (float)rh, 1.0f, i == 0, 1, 0, 1, getenv("VXU_SYNC") != NULL };
        char why[512]; int note;
        if ((i < 100 || !getenv("VXU_BASE")) && vxu_encode((__bridge void *)cb, mode, &f, why, sizeof why, &note)) { printf("  encode failed: %s\n", why); return 1; }
        if (note && i == 0) printf("  %s\n", why);
        if (i < 100) { [cb commit]; [cb waitUntilCompleted]; if (cb.error) { printf("  GPU error: %s\n", cb.error.localizedDescription.UTF8String); return 1; } }
    }
    [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { printf("  GPU error: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
    gpu = cb.GPUEndTime - cb.GPUStartTime;
    // (read back through a shared RGBA16Float copy)
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:H mipmapped:NO];
    td.storageMode = MTLStorageModeShared; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> rb = [dev newTextureWithDescriptor:td];
    cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    id<MTLComputePipelineState> cp = [dev newComputePipelineStateWithFunction:[[dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\nkernel void c(texture2d<float, access::read> s [[texture(0)]], texture2d<float, access::write> d [[texture(1)]], uint2 p [[thread_position_in_grid]]) { d.write(s.read(p), p); }\n"
        options:nil error:&err] newFunctionWithName:@"c"] error:&err];
    [e setComputePipelineState:cp]; [e setTexture:out atIndex:0]; [e setTexture:rb atIndex:1];
    [e dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    uint16_t *px = malloc((size_t)W * H * 8);
    [rb getBytes:px bytesPerRow:W * 8 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    double err_x = 0, err_y = 0, err_b = 0; int n = 0;
    for (uint32_t y = H / 8; y < H * 7 / 8; y += 7)
        for (uint32_t x = W / 8; x < W * 7 / 8; x += 7) {
            const uint16_t *v = px + 4 * ((size_t)y * W + x);
            err_x += fabs(half_f(v[0]) - ((float)x + 0.5f) / (float)W);
            err_y += fabs(half_f(v[1]) - ((float)y + 0.5f) / (float)H);
            { float xs = ((float)x + 0.5f) / (float)W * (float)rw, ys = ((float)y + 0.5f) / (float)H * (float)rh;
              err_b += fabs(half_f(v[2]) - (0.5f + 0.5f * sinf(6.2831853f * (xs + ys) / 3.5f))); }
            n++;
        }
    err_x /= n; err_y /= n; err_b /= n;
    // (the fine pattern: 0.016 with the jitter as FSR2 gives it, 0.09-0.24 with either sign turned: the jitter convention)
    int ok = err_x < 0.02 && err_y < 0.02 && (mode != VXU_TEMPORAL || W != 2 * rw || err_b < 0.05);
    printf("  %s colour %lu -> output %lu, %ux%u in %ux%u -> %ux%u: mean error x %.4f y %.4f, fine pattern %.4f, GPU %.2f ms/frame: %s\n",
           mode == VXU_TEMPORAL ? "temporal" : "spatial", (unsigned long)cf, (unsigned long)of, rw, rh, W, H, W, H, err_x, err_y, err_b,
           gpu * 1e3 / (frames - 100), ok ? "ok" : "FAILED");
    free(px);
    if (!ok) rc = 1;
    return rc;
}

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("upscale_core_test: no Metal device, skipped\n"); return 0; }
        int fail = 0;
        printf("upscale_core_test (%s): temporal %s, spatial %s\n", dev.name.UTF8String,
               vxu_supported((__bridge void *)dev, VXU_TEMPORAL) ? "supported" : "not supported",
               vxu_supported((__bridge void *)dev, VXU_SPATIAL) ? "supported" : "not supported");
        if (vxu_supported((__bridge void *)dev, VXU_TEMPORAL)) {
            fail |= run(dev, VXU_TEMPORAL, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float, 756, 491, 1512, 982);
            fail |= run(dev, VXU_TEMPORAL, MTLPixelFormatRG11B10Float, MTLPixelFormatRG11B10Float, 756, 491, 1512, 982);
            fail |= run(dev, VXU_TEMPORAL, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float, 1008, 655, 1512, 982);
        }
        if (vxu_supported((__bridge void *)dev, VXU_SPATIAL))
            fail |= run(dev, VXU_SPATIAL, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float, 756, 491, 1512, 982);
        printf("upscale_core_test: %s\n", fail ? "FAILED" : "all ok");
        return fail;
    }
}
