// D3D11's persistent Metal IR and GPU pipeline cache. All files are disposable.
#pragma once
#import <Metal/Metal.h>

id<MTLLibrary> vx_shader_cache_library(id<MTLDevice> device, NSString *source, NSArray<NSString *> *entries, NSError **error);
void vx_shader_cache_library_async(id<MTLDevice> device, NSString *source, NSArray<NSString *> *entries, MTLNewLibraryCompletionHandler done);
id<MTLFunction> vx_metal_function(id<MTLLibrary> library, NSString *name);
id<MTLRenderPipelineState> vx_shader_cache_render(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, NSError **error);
id<MTLComputePipelineState> vx_shader_cache_compute(id<MTLDevice> device, MTLComputePipelineDescriptor *desc, NSError **error);
id<MTLComputePipelineState> vx_shader_cache_compute_function(id<MTLDevice> device, id<MTLFunction> function, NSError **error);
NSDictionary *vx_shader_cache_snapshot(void);
void vx_shader_cache_flush(void);
