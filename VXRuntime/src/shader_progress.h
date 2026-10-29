// Compilation telemetry for the game's D3D11 path; compiler work stays on Metal's own queues.
#pragma once
#import <Metal/Metal.h>
#include <stdint.h>
#include "shader_cache.h"

void vx_shader_translation_finished(uint64_t ns, int success);
void vx_shader_wait_finished(uint64_t ns);
NSDictionary *vx_shader_progress_snapshot(void);
void vx_shader_progress_flush(void);
id<MTLLibrary> vx_metal_library(id<MTLDevice> device, NSString *source, NSError **error);
id<MTLLibrary> vx_metal_library_entries(id<MTLDevice> device, NSString *source, NSArray<NSString *> *entries, NSError **error);
void vx_metal_library_async(id<MTLDevice> device, NSString *source, MTLNewLibraryCompletionHandler done);
id<MTLRenderPipelineState> vx_metal_render_pipeline(id<MTLDevice> device, MTLRenderPipelineDescriptor *desc, NSError **error);
id<MTLComputePipelineState> vx_metal_compute_pipeline(id<MTLDevice> device, id<MTLFunction> function, NSError **error);
id<MTLComputePipelineState> vx_metal_compute_descriptor(id<MTLDevice> device, MTLComputePipelineDescriptor *desc, NSError **error);
