#pragma once
#import <Metal/Metal.h>
#import <CoreVideo/CoreVideo.h>
#include "../geometry/mvr_geometry.hpp"
#include <string>

// Standalone native integration component. Does not install hooks or alter
// VideoToolbox properties. Source images MUST already match the render plan.
@interface MVRPackedFrame : NSObject
@property(nonatomic, readonly) CVPixelBufferRef pixelBuffer;
@property(nonatomic, strong, readonly) id<MTLCommandBuffer> commandBuffer;
@end

@interface MVRMetalPacker : NSObject
@property(nonatomic, strong, readonly) id<MTLDevice> device;
- (instancetype)initWithDevice:(id<MTLDevice>)device
                  shaderSource:(NSString*)source error:(NSError**)error;
// Caller waits for completion via command-buffer event/handler before passing
// the buffer to an encoder. No synchronous GPU waits or CPU readback here.
// Explicit CPU-written or externally GPU-completed source. The caller must
// not use this overload for a still-pending producer.
- (MVRPackedFrame*)packCompletedInput:(CVPixelBufferRef)input
                   plan:(const mvr::geometry::Plan&)plan error:(NSError**)error;
// Completion is registered before commit; callers must not attach a handler
// to a command buffer which may already have completed.
- (MVRPackedFrame*)packCompletedInput:(CVPixelBufferRef)input plan:(const mvr::geometry::Plan&)plan
                        completion:(void(^)(MVRPackedFrame*))completion error:(NSError**)error;
// Signal completion on the GPU, allowing a downstream queue to encode a
// wait without a CPU stall. The caller retains the frame through consumption.
- (MVRPackedFrame*)packCompletedInput:(CVPixelBufferRef)input plan:(const mvr::geometry::Plan&)plan
                         signalEvent:(id<MTLSharedEvent>)event value:(uint64_t)value error:(NSError**)error;
// Normal GPU-producer path: wait on the producer event WITHOUT blocking CPU.
- (MVRPackedFrame*)pack:(CVPixelBufferRef)input plan:(const mvr::geometry::Plan&)plan
         producerEvent:(id<MTLEvent>)event value:(uint64_t)value error:(NSError**)error;
@end
