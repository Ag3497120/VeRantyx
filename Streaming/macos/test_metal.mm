#import "MetalPacker.hpp"
#import <Foundation/Foundation.h>
#include <iostream>
#include "../protocol/mvr_geometry_wire.hpp"
#include <vector>
#include <stdexcept>
#include <algorithm>
using namespace mvr::geometry;
static size_t assertions=0;
static void check(bool x,const char* message){++assertions;if(!x)throw std::runtime_error(message);}
static unsigned char pattern(unsigned x,unsigned y,unsigned eye,unsigned c){
 if(c==0)return eye?231:17; // B: any eye mixing is immediately visible
 if(c==1)return (x*19+y*7+11)%256;
 if(c==2)return (x*5+y*23+31)%256;
 return 255;
}
static CVPixelBufferRef inputFor(const Plan& p){
 unsigned w=p.eyes[0].render.size.width,h=p.eyes[0].render.size.height;
 NSDictionary* d=@{(id)kCVPixelBufferMetalCompatibilityKey:@YES,(id)kCVPixelBufferIOSurfacePropertiesKey:@{}};
 CVPixelBufferRef b=nullptr;check(CVPixelBufferCreate(kCFAllocatorDefault,w*2,h,kCVPixelFormatType_32BGRA,(__bridge CFDictionaryRef)d,&b)==kCVReturnSuccess,"input allocation");
 CVPixelBufferLockBaseAddress(b,0);auto* bytes=(unsigned char*)CVPixelBufferGetBaseAddress(b);size_t stride=CVPixelBufferGetBytesPerRow(b);
 for(unsigned eye=0;eye<2;eye++)for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++)for(unsigned c=0;c<4;c++)bytes[y*stride+(eye*w+x)*4+c]=pattern(x+p.eyes[eye].render.sourceRect.x,y+p.eyes[eye].render.sourceRect.y,eye,c);
 CVPixelBufferUnlockBaseAddress(b,0);return b;
}
static void testPlan(MVRMetalPacker* packer,bool cropOn,bool foveateOn){
 CropOptions c;c.enabled=cropOn;FoveationOptions f;f.enabled=foveateOn;
 std::array<Fov,2> native={Fov{-.9,.7,.8,-.75},Fov{-.7,.9,.8,-.75}};
 auto p=makePlan(native,{320,256},c,f,17,3);
 // Exercise the actual serialized descriptor -> matching ACK/COMMIT -> GPU
 // plan path, not only identical C++ objects on both sides.
 mvr::wire::Descriptor descriptor{{320,256},native,p};mvr::wire::Sender sender;mvr::wire::Receiver receiver;
 auto offer=sender.offer(descriptor);auto ack=receiver.acceptOffer(offer);auto commit=sender.acceptAck(ack);
 check(receiver.acceptCommit(commit),"receiver commits matching wire geometry");sender.commitSent();
 check(sender.acceptsFrame(sender.epoch())&&receiver.acceptsFrame(sender.epoch()),"both ends accept same frame epoch");
 check(receiver.active()!=nullptr,"active receiver descriptor exists");p=receiver.active()->plan;
 CVPixelBufferRef b=inputFor(p);NSError* error=nil;id<MTLSharedEvent> completed=[packer.device newSharedEvent];MVRPackedFrame* frame=[packer packCompletedInput:b plan:p signalEvent:completed value:1 error:&error];
 CVPixelBufferRelease(b);if(!frame)throw std::runtime_error(error.description.UTF8String);
 // Synchronous completion is ONLY for this verification executable.
 id<MTLCommandQueue> downstream=[packer.device newCommandQueue];id<MTLCommandBuffer> consumer=[downstream commandBuffer];[consumer encodeWaitForEvent:completed value:1];[consumer commit];[consumer waitUntilCompleted];check(completed.signaledValue>=1,"downstream GPU waited for pack completion");check(frame.commandBuffer.status!=MTLCommandBufferStatusError,"pack GPU has no error after downstream event wait");
 auto result=frame.pixelBuffer;check(CVPixelBufferGetWidth(result)==p.eyes[0].x.packedPixels*2,"packed width");check(CVPixelBufferGetHeight(result)==p.eyes[0].y.packedPixels,"packed height");
 CVPixelBufferLockBaseAddress(result,kCVPixelBufferLock_ReadOnly);
 auto* v=(unsigned char*)CVPixelBufferGetBaseAddress(result);auto stride=CVPixelBufferGetBytesPerRow(result);unsigned ew=p.eyes[0].x.packedPixels;
 for(unsigned eye=0;eye<2;eye++){
  const auto& e=p.eyes[eye];
  for(unsigned y=0;y<e.y.packedPixels;y++)for(unsigned x=0;x<e.x.packedPixels;x++){
   const auto* pixel=v+y*stride+(eye*ew+x)*4;check(pixel[0]==(eye?231:17),"no left/right eye bleed");check(pixel[3]==255,"alpha retained");
   if(x>=e.x.packedBegin&&x<e.x.packedEnd&&y>=e.y.packedBegin&&y<e.y.packedEnd){
    unsigned sx=e.x.sourceBegin+x-e.x.packedBegin+e.render.sourceRect.x;
    unsigned sy=e.y.sourceBegin+y-e.y.packedBegin+e.render.sourceRect.y;
    for(unsigned ch=1;ch<3;ch++)check(std::abs(int(pixel[ch])-int(pattern(sx,sy,eye,ch)))<=1,"central texels preserved");
   }
  }
 }
 CVPixelBufferUnlockBaseAddress(result,kCVPixelBufferLock_ReadOnly);
 std::cout<<"PASS crop="<<cropOn<<" foveation="<<foveateOn<<" render="<<p.eyes[0].render.size.width<<"x"<<p.eyes[0].render.size.height<<" video="<<ew<<"x"<<p.eyes[0].y.packedPixels<<" gpu_ms="<<(frame.commandBuffer.GPUEndTime-frame.commandBuffer.GPUStartTime)*1000<<"\n";
}
static void rejectInvalid(MVRMetalPacker* packer){
 CropOptions c;FoveationOptions f;f.enabled=true;auto p=makePlan({Fov{-.9,.7,.8,-.75},Fov{-.7,.9,.8,-.75}},{320,256},c,f,17,4);
 auto b=inputFor(p);NSError* error=nil;p.gpu.eyes[0].x.packedPixels+=1;
 check([packer packCompletedInput:b plan:p error:&error]==nil,"reject altered GPU descriptor");check(error!=nil,"error available");CVPixelBufferRelease(b);
}
static void testPool(MVRMetalPacker* packer){
 CropOptions c;FoveationOptions f;f.enabled=true;auto p=makePlan({Fov{-.9,.7,.8,-.75},Fov{-.7,.9,.8,-.75}},{320,256},c,f,21,1);
 auto b=inputFor(p);
 @autoreleasepool {
  NSMutableArray<MVRPackedFrame*>* held=[NSMutableArray new];
  for(unsigned i=0;i<3;i++){@autoreleasepool{NSError* e=nil;auto frame=[packer packCompletedInput:b plan:p error:&e];check(frame!=nil,"three bounded frames allowed");[frame.commandBuffer waitUntilCompleted];[held addObject:frame];}}
  NSError* err=nil;check([packer packCompletedInput:b plan:p error:&err]==nil,"fourth outstanding frame rejected");
  [held removeAllObjects];
 }
 @autoreleasepool{NSError* err=nil;auto frame=[packer packCompletedInput:b plan:p error:&err];check(frame!=nil,"pool recovers after frame release");[frame.commandBuffer waitUntilCompleted];}
 CVPixelBufferRelease(b);
 std::cout<<"PASS bounded pool and recovery\n";
}
static void testProducer(MVRMetalPacker* packer){
 CropOptions c;FoveationOptions f;f.enabled=true;auto p=makePlan({Fov{-.9,.7,.8,-.75},Fov{-.7,.9,.8,-.75}},{320,256},c,f,22,1);
 auto b=inputFor(p);auto dev=packer.device;
 CVMetalTextureCacheRef cache=nullptr;CVMetalTextureRef cv=nullptr;
 check(CVMetalTextureCacheCreate(kCFAllocatorDefault,nullptr,dev,nullptr,&cache)==0,"producer cache");
 check(CVMetalTextureCacheCreateTextureFromImage(kCFAllocatorDefault,cache,b,nullptr,MTLPixelFormatBGRA8Unorm,640,256,0,&cv)==0,"producer texture");
 auto buffer=[dev newBufferWithLength:640*256*4 options:MTLResourceStorageModeShared];auto* bytes=(unsigned char*)buffer.contents;
 for(size_t i=0;i<640*256;i++){bytes[i*4]=9;bytes[i*4+1]=11;bytes[i*4+2]=13;bytes[i*4+3]=255;}
 auto event=[dev newSharedEvent];NSError* err=nil;
 check([packer pack:b plan:p producerEvent:nil value:1 error:&err]==nil,"missing producer dependency rejected");
 // Submit consumer BEFORE producer; without an event this can read old pixels.
 auto frame=[packer pack:b plan:p producerEvent:event value:1 error:&err];if(!frame)throw std::runtime_error(err.description.UTF8String);
 auto queue=[dev newCommandQueue];auto command=[queue commandBuffer];auto blit=[command blitCommandEncoder];
 [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:640*4 sourceBytesPerImage:640*256*4 sourceSize:MTLSizeMake(640,256,1) toTexture:CVMetalTextureGetTexture(cv) destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
 [blit endEncoding];[command encodeSignalEvent:event value:1];[command commit];[frame.commandBuffer waitUntilCompleted];
 check(frame.commandBuffer.status==MTLCommandBufferStatusCompleted,"cross-queue source ready");
 CVPixelBufferLockBaseAddress(frame.pixelBuffer,kCVPixelBufferLock_ReadOnly);auto* out=(unsigned char*)CVPixelBufferGetBaseAddress(frame.pixelBuffer);check(out[0]==9&&out[1]==11&&out[2]==13&&out[3]==255,"consumer observed GPU producer pixels");CVPixelBufferUnlockBaseAddress(frame.pixelBuffer,kCVPixelBufferLock_ReadOnly);
 CFRelease(cv);CFRelease(cache);CVPixelBufferRelease(b);
 std::cout<<"PASS GPU producer/consumer event across separate queues\n";
}
static void benchmark(MVRMetalPacker* packer,bool cropOn){
 CropOptions c;c.enabled=cropOn;FoveationOptions f;f.enabled=true;auto p=makePlan({Fov{-.9,.7,.8,-.75},Fov{-.7,.9,.8,-.75}},{2064,2208},c,f,23+cropOn,1);
 auto b=inputFor(p);std::vector<double> times;
 for(unsigned i=0;i<11;i++){@autoreleasepool{NSError* e=nil;auto frame=[packer packCompletedInput:b plan:p error:&e];if(!frame)throw std::runtime_error(e.description.UTF8String);[frame.commandBuffer waitUntilCompleted];check(frame.commandBuffer.status==MTLCommandBufferStatusCompleted,"full-size pack complete");if(i>=3)times.push_back((frame.commandBuffer.GPUEndTime-frame.commandBuffer.GPUStartTime)*1000);}}
 CVPixelBufferRelease(b);std::sort(times.begin(),times.end());
 std::cout<<"GPU_ONLY crop="<<cropOn<<" render="<<p.eyes[0].render.size.width<<"x"<<p.eyes[0].render.size.height<<" video="<<p.eyes[0].x.packedPixels<<"x"<<p.eyes[0].y.packedPixels<<" median_ms="<<times[times.size()/2]<<" max_ms="<<times.back()<<" measured_frames="<<times.size()<<" (not streaming latency)\n";
}
int main(int argc,char** argv){@autoreleasepool{try{
 if(argc!=2&&argc!=3)throw std::runtime_error("usage: test_metal path/to/foveated_pack.metal");
 NSError* error=nil;NSString* source=[NSString stringWithContentsOfFile:[NSString stringWithUTF8String:argv[1]] encoding:NSUTF8StringEncoding error:&error];
 id<MTLDevice> device=MTLCreateSystemDefaultDevice();check(device!=nil,"Metal device available");
 MVRMetalPacker* packer=[[MVRMetalPacker alloc] initWithDevice:device shaderSource:source error:&error];if(!packer)throw std::runtime_error(error.description.UTF8String);
 std::cout<<"Metal device: "<<device.name.UTF8String<<"\n";
 @autoreleasepool{testPlan(packer,false,false);}@autoreleasepool{testPlan(packer,true,false);}@autoreleasepool{testPlan(packer,false,true);}@autoreleasepool{testPlan(packer,true,true);}@autoreleasepool{rejectInvalid(packer);}@autoreleasepool{testPool(packer);}@autoreleasepool{testProducer(packer);}
 if(argc==3){@autoreleasepool{benchmark(packer,false);}@autoreleasepool{benchmark(packer,true);}}
 std::cout<<"PASS assertions="<<assertions<<"; native Metal only; no encoder, game or Quest test\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}}}
