#import "MetalPacker.hpp"
#import <Foundation/Foundation.h>
#include <mutex>
#include <memory>
#include <atomic>

static void fail(NSError** out,NSString* msg,NSInteger code=1) {
 if(out)*out=[NSError errorWithDomain:@"MVRFoveatedTransport" code:code userInfo:@{NSLocalizedDescriptionKey:msg}];
}
@interface MVRPackedFrame () {
 CVPixelBufferRef _buffer;
 CVMetalTextureRef _sourceTexture;
 CVMetalTextureRef _destinationTexture;
 CVPixelBufferRef _input;
 std::shared_ptr<std::atomic<unsigned>> _leases;
}
@property(nonatomic, strong, readwrite) id<MTLCommandBuffer> commandBuffer;
- (instancetype)initWithBuffer:(CVPixelBufferRef)b input:(CVPixelBufferRef)i source:(CVMetalTextureRef)s destination:(CVMetalTextureRef)d command:(id<MTLCommandBuffer>)command leases:(std::shared_ptr<std::atomic<unsigned>>)leases;
@end
@implementation MVRPackedFrame
- (instancetype)initWithBuffer:(CVPixelBufferRef)b input:(CVPixelBufferRef)i source:(CVMetalTextureRef)s destination:(CVMetalTextureRef)d command:(id<MTLCommandBuffer>)command leases:(std::shared_ptr<std::atomic<unsigned>>)leases {
 if((self=[super init])){_buffer=CVPixelBufferRetain(b);_input=CVPixelBufferRetain(i);_sourceTexture=s;CFRetain(s);_destinationTexture=d;CFRetain(d);self.commandBuffer=command;_leases=leases;++*_leases;}return self;
}
- (CVPixelBufferRef)pixelBuffer{return _buffer;}
- (void)dealloc {if(_leases)--*_leases;if(_buffer)CVPixelBufferRelease(_buffer);if(_input)CVPixelBufferRelease(_input);if(_sourceTexture)CFRelease(_sourceTexture);if(_destinationTexture)CFRelease(_destinationTexture);}
@end

@interface MVRMetalPacker () {
 CVMetalTextureCacheRef _cache;
 CVPixelBufferPoolRef _pool;
 size_t _width,_height;
 std::mutex _lock;
 std::shared_ptr<std::atomic<unsigned>> _leases;
}
- (MVRPackedFrame*)packInternal:(CVPixelBufferRef)input plan:(const mvr::geometry::Plan&)p producerEvent:(id<MTLEvent>)event value:(uint64_t)value completion:(void(^)(MVRPackedFrame*))completion signalEvent:(id<MTLSharedEvent>)signal signalValue:(uint64_t)signalValue error:(NSError**)error;
@property(nonatomic, strong, readwrite) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLComputePipelineState> pipeline;
@property(nonatomic, strong) id<MTLCommandQueue> queue;
@end
@implementation MVRMetalPacker
- (instancetype)initWithDevice:(id<MTLDevice>)d shaderSource:(NSString*)source error:(NSError**)error {
 if(!(self=[super init]))return nil;
 if(!d||!source){fail(error,@"Missing Metal device or shader source");return nil;}
 self.device=d;_leases=std::make_shared<std::atomic<unsigned>>(0);
 MTLCompileOptions* opts=[MTLCompileOptions new];if(@available(macOS 15.0,*)){opts.mathMode=MTLMathModeSafe;}
 id<MTLLibrary> library=[d newLibraryWithSource:source options:opts error:error];if(!library)return nil;
 id<MTLFunction> fn=[library newFunctionWithName:@"mvr_foveated_pack"];
 if(!fn){fail(error,@"Pack kernel missing");return nil;}
 self.pipeline=[d newComputePipelineStateWithFunction:fn error:error];if(!self.pipeline)return nil;
 self.queue=[d newCommandQueue];
 CVReturn r=CVMetalTextureCacheCreate(kCFAllocatorDefault,NULL,d,NULL,&_cache);
 if(r||!self.queue){fail(error,@"Metal queue/texture cache creation failed",r);return nil;}
 return self;
}
- (MVRPackedFrame*)packCompletedInput:(CVPixelBufferRef)input plan:(const mvr::geometry::Plan&)p error:(NSError**)error {
 return [self packInternal:input plan:p producerEvent:nil value:0 completion:nil signalEvent:nil signalValue:0 error:error];
}
- (MVRPackedFrame*)packCompletedInput:(CVPixelBufferRef)input plan:(const mvr::geometry::Plan&)p completion:(void(^)(MVRPackedFrame*))completion error:(NSError**)error {
 return [self packInternal:input plan:p producerEvent:nil value:0 completion:completion signalEvent:nil signalValue:0 error:error];
}
- (MVRPackedFrame*)pack:(CVPixelBufferRef)input plan:(const mvr::geometry::Plan&)p producerEvent:(id<MTLEvent>)event value:(uint64_t)value error:(NSError**)error {
 if(!event||(event.device&&event.device.registryID!=self.device.registryID)){fail(error,@"Missing or foreign GPU producer event");return nil;}
 return [self packInternal:input plan:p producerEvent:event value:value completion:nil signalEvent:nil signalValue:0 error:error];
}
- (MVRPackedFrame*)packCompletedInput:(CVPixelBufferRef)input plan:(const mvr::geometry::Plan&)p signalEvent:(id<MTLSharedEvent>)event value:(uint64_t)value error:(NSError**)error {
 if(!event||!value||(event.device&&event.device.registryID!=self.device.registryID)){fail(error,@"Missing or foreign completion event");return nil;}
 return [self packInternal:input plan:p producerEvent:nil value:0 completion:nil signalEvent:event signalValue:value error:error];
}
- (MVRPackedFrame*)packInternal:(CVPixelBufferRef)input plan:(const mvr::geometry::Plan&)p producerEvent:(id<MTLEvent>)event value:(uint64_t)value completion:(void(^)(MVRPackedFrame*))completion signalEvent:(id<MTLSharedEvent>)signal signalValue:(uint64_t)signalValue error:(NSError**)error {
 std::lock_guard<std::mutex> guard(_lock);
 try {
  for(auto& e:p.eyes){mvr::geometry::validate(e.x);mvr::geometry::validate(e.y);
   mvr::geometry::require(e.x.sourcePixels<=8192&&e.x.packedPixels<=8192&&e.y.sourcePixels<=8192&&e.y.packedPixels<=8192,"unsupported texture dimensions");
   mvr::geometry::require(e.x.packedPixels<=e.x.sourcePixels&&e.y.packedPixels<=e.y.sourcePixels,"packing expands source");
  }
 }catch(const std::exception& e){fail(error,[NSString stringWithUTF8String:e.what()]);return nil;}
 if(_leases->load()>=3){fail(error,@"Three outstanding frames; release completed work instead of queueing more");return nil;}
 const auto& e=p.eyes[0];const auto& r=p.eyes[1];
 if(!input||CVPixelBufferGetPixelFormatType(input)!=kCVPixelFormatType_32BGRA||CVPixelBufferIsPlanar(input)){
  fail(error,@"Prototype requires nonplanar 32BGRA input; NV12/HDR paths need explicit format handling");return nil;}
 if(e.x.sourcePixels!=r.x.sourcePixels||e.y.sourcePixels!=r.y.sourcePixels||e.x.packedPixels!=r.x.packedPixels||e.y.packedPixels!=r.y.packedPixels||
 CVPixelBufferGetWidth(input)!=size_t(e.x.sourcePixels)*2||CVPixelBufferGetHeight(input)!=e.y.sourcePixels){fail(error,@"SBS input or per-eye geometry mismatch");return nil;}
 auto expected=p.gpu;expected.eyes[0]={mvr::geometry::shader(e.x),mvr::geometry::shader(e.y)};expected.eyes[1]={mvr::geometry::shader(r.x),mvr::geometry::shader(r.y)};
 if(memcmp(&expected,&p.gpu,sizeof expected)){fail(error,@"GPU descriptor differs from validated plan");return nil;}
 size_t w=size_t(e.x.packedPixels)*2,h=e.y.packedPixels;
 if(!_pool||w!=_width||h!=_height){
  if(_pool){CVPixelBufferPoolRelease(_pool);_pool=NULL;}
  NSDictionary* attrs=@{(id)kCVPixelBufferWidthKey:@(w),(id)kCVPixelBufferHeightKey:@(h),(id)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferMetalCompatibilityKey:@YES,(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
  CVReturn result=CVPixelBufferPoolCreate(kCFAllocatorDefault,NULL,(__bridge CFDictionaryRef)attrs,&_pool);
  if(result){fail(error,@"Pixel buffer pool creation failed",result);return nil;}_width=w;_height=h;
 }
 CVMetalTextureCacheFlush(_cache,0);
 CVPixelBufferRef output=NULL;
 NSDictionary* limits=@{(id)kCVPixelBufferPoolAllocationThresholdKey:@3};
 CVReturn result=CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(kCFAllocatorDefault,_pool,(__bridge CFDictionaryRef)limits,&output);
 if(result){fail(error,@"Output pool busy: do not accumulate unbounded frames",result);return nil;}
 // Copy colour metadata only. A full-size clean aperture/display dimension
 // attachment is invalid for the smaller nonlinearly packed raster.
 const CFStringRef colourKeys[]={kCVImageBufferColorPrimariesKey,kCVImageBufferTransferFunctionKey,kCVImageBufferYCbCrMatrixKey,kCVImageBufferCGColorSpaceKey};
 for(auto key:colourKeys){CVAttachmentMode mode;auto attachment=CVBufferCopyAttachment(input,key,&mode);if(attachment){CVBufferSetAttachment(output,key,attachment,mode);CFRelease(attachment);}}
 CVMetalTextureRef source=NULL,destination=NULL;
 result=CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,_cache,input,NULL,MTLPixelFormatBGRA8Unorm,size_t(e.x.sourcePixels)*2,e.y.sourcePixels,0,&source);
 if(!result)result=CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,_cache,output,NULL,MTLPixelFormatBGRA8Unorm,w,h,0,&destination);
 if(result||!source||!destination){if(source)CFRelease(source);if(destination)CFRelease(destination);CVPixelBufferRelease(output);fail(error,@"CVPixelBuffer Metal texture mapping failed",result);return nil;}
 id<MTLCommandBuffer> command=[self.queue commandBuffer];
 if(event)[command encodeWaitForEvent:event value:value];
 id<MTLComputeCommandEncoder> enc=[command computeCommandEncoder];
 if(!enc){CFRelease(source);CFRelease(destination);CVPixelBufferRelease(output);fail(error,@"Command encoder creation failed");return nil;}
 command.label=@"MVR foveated transport pack";
 [enc setComputePipelineState:self.pipeline];[enc setTexture:CVMetalTextureGetTexture(source) atIndex:0];[enc setTexture:CVMetalTextureGetTexture(destination) atIndex:1];
 [enc setBytes:&p.gpu length:sizeof p.gpu atIndex:0];
 [enc dispatchThreads:MTLSizeMake(w,h,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)];[enc endEncoding];
 MVRPackedFrame* frame=[[MVRPackedFrame alloc] initWithBuffer:output input:input source:source destination:destination command:command leases:_leases];
 CFRelease(source);CFRelease(destination);CVPixelBufferRelease(output);
 // Retain pixel buffers/Metal views until GPU completion even if the caller
 // releases its frame immediately. Caller owns the returned frame afterwards.
 __block MVRPackedFrame* pending=frame;
 [command addCompletedHandler:^(id<MTLCommandBuffer>){if(completion)completion(pending);pending=nil;}];
 if(signal)[command encodeSignalEvent:signal value:signalValue];
 [command commit];return frame;
}
- (void)dealloc {if(_cache)CFRelease(_cache);if(_pool)CVPixelBufferPoolRelease(_pool);}
@end
