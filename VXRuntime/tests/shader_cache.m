#import <Foundation/Foundation.h>
#include <assert.h>
#include <stdarg.h>
#include <time.h>
#include "../src/vx.h"
#include "../src/shader_progress.h"

vx_process vx_proc;
void vx_log(const char *format, ...) { (void)format; }

static void report(uint64_t start) {
    vx_shader_progress_flush();
    NSMutableDictionary *result = [vx_shader_cache_snapshot() mutableCopy];
    result[@"elapsed_ms"] = @((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - start) / 1000000.0);
    NSData *json = [NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingSortedKeys error:nil];
    puts([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
}

static void draw(id<MTLDevice> device, id<MTLRenderPipelineState> pipeline, MTLPixelFormat format) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    assert(target);
    float vertices[] = { -1, -1, 3, -1, -1, 3 };
    id<MTLBuffer> buffer = [device newBufferWithBytes:vertices length:sizeof vertices options:MTLResourceStorageModeShared];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> command = [[device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline]; [encoder setVertexBuffer:buffer offset:0 atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [encoder endEncoding];
    [command commit]; [command waitUntilCompleted];
    assert(command.status == MTLCommandBufferStatusCompleted);
    uint8_t pixels[64];
    [target getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    for (int i = 0; i < 16; i++) {
        assert(abs(pixels[4 * i + (format == MTLPixelFormatBGRA8Unorm ? 2 : 0)] - 64) <= 1);
        assert(abs(pixels[4 * i + 1] - 128) <= 1);
        assert(abs(pixels[4 * i + (format == MTLPixelFormatBGRA8Unorm ? 0 : 2)] - 191) <= 1);
        assert(pixels[4 * i + 3] == 255);
    }
}

int main(int argc, char **argv) {
    @autoreleasepool {
        uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice(); assert(device);
        if (argc > 1 && !strcmp(argv[1], "failure")) {
            NSString *bad = @"#include <metal_stdlib>\nusing namespace metal; kernel void vx_main() { undefined_identifier; }";
            NSError *error = nil;
            assert(!vx_metal_library(device, bad, &error) && error);
            dispatch_group_t failure = dispatch_group_create(); dispatch_group_enter(failure);
            vx_metal_library_async(device, bad, ^(id<MTLLibrary> library, NSError *e) {
                assert(!library && e); dispatch_group_leave(failure);
            });
            dispatch_group_wait(failure, DISPATCH_TIME_FOREVER);
            report(start);
            return 0;
        }
        BOOL changed = argc > 1 && !strcmp(argv[1], "changed");
        NSString *cs = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal; kernel void vx_main(device uint *v [[buffer(0)]], uint i [[thread_position_in_grid]]) { v[i] = i + %u; }", changed ? 100u : 42u];
        NSString *vs = @"#include <metal_stdlib>\nusing namespace metal; struct In { float2 p [[attribute(0)]]; }; struct Out { float4 p [[position]]; }; vertex Out vx_main(In v [[stage_in]]) { Out o; o.p = float4(v.p,0,1); return o; }";
        NSString *ps = @"#include <metal_stdlib>\nusing namespace metal; fragment float4 vx_main() { return float4(0.25,0.5,0.75,1); }";
        dispatch_group_t requests = dispatch_group_create();
        for (int i = 0; i < 32; i++) {
            dispatch_group_enter(requests);
            vx_metal_library_async(device, cs, ^(id<MTLLibrary> lib, NSError *error) {
                assert(lib && !error); dispatch_group_leave(requests);
            });
        }
        // A synchronous caller may arrive before the asynchronous owner has completed.
        NSError *error = nil;
        id<MTLLibrary> cl = vx_metal_library(device, cs, &error); assert(cl && !error);
        dispatch_group_wait(requests, DISPATCH_TIME_FOREVER);
        id<MTLLibrary> vl = vx_metal_library(device, vs, &error); if (!vl) NSLog(@"%@", error); assert(vl && !error);
        id<MTLLibrary> pl = vx_metal_library(device, ps, &error); assert(pl && !error);
        id<MTLComputePipelineState> cp = vx_metal_compute_pipeline(device, vx_metal_function(cl, @"vx_main"), &error); assert(cp && !error);
        id<MTLBuffer> output = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command = [[device newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:cp]; [encoder setBuffer:output offset:0 atIndex:0];
        [encoder dispatchThreads:MTLSizeMake(4,1,1) threadsPerThreadgroup:MTLSizeMake(4,1,1)];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        assert(command.status == MTLCommandBufferStatusCompleted);
        for (int i = 0; i < 4; i++) assert(((uint32_t *)output.contents)[i] == i + (changed ? 100u : 42u));

        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vx_metal_function(vl, @"vx_main"); d.fragmentFunction = vx_metal_function(pl, @"vx_main");
        MTLVertexDescriptor *vertex = [MTLVertexDescriptor vertexDescriptor];
        vertex.attributes[0].format = MTLVertexFormatFloat2; vertex.attributes[0].bufferIndex = 1; vertex.layouts[1].stride = 8;
        d.vertexDescriptor = vertex; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> rp = vx_metal_render_pipeline(device, d, &error); assert(rp && !error);
        assert(!d.binaryArchives.count); // Wrappers must not mutate the caller's descriptor.
        draw(device, rp, MTLPixelFormatRGBA8Unorm);
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        rp = vx_metal_render_pipeline(device, d, &error); assert(rp && !error); draw(device, rp, MTLPixelFormatRGBA8Unorm);
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        rp = vx_metal_render_pipeline(device, d, &error); assert(rp && !error); draw(device, rp, MTLPixelFormatBGRA8Unorm);
        vertex.layouts[1].stride = 16; d.vertexDescriptor = vertex;
        assert(vx_metal_render_pipeline(device, d, &error) && !error);
        d.rasterSampleCount = 4;
        assert(vx_metal_render_pipeline(device, d, &error) && !error);
        report(start);
    }
}
