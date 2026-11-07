#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include "../shared_bridge.h"
int main(){
 SharedBridge bridge;bridge.map=(uint8_t*)mmap(nullptr,vrrt::kSharedMapSize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(bridge.map!=MAP_FAILED);bridge.header=(vrrt::DoubleBufferHeader*)bridge.map;
 XrView v[2]{};for(auto& eye:v){eye.pose.orientation.w=1;eye.fov={-.7f,.7f,.7f,-.7f};}v[0].pose.position.x=-.032f;v[1].pose.position.x=.032f;vrrt::SharedHands control{};
 bridge.tracking(v,true,control);assert(bridge.originReady&&bridge.originProvisional);assert(bridge.header->poseSequence==2);
 vrrt::SharedHands h{};memcpy(&h,bridge.map+vrrt::kHandsOffset,sizeof h);assert(fabs(h.headTransform[13]-1.6)<.001);
 for(auto& eye:v){eye.pose.position.x+=3;eye.pose.position.y=1.75;eye.pose.position.z=2;}
 bridge.tracking(v,true,control);assert(!bridge.originProvisional);memcpy(&h,bridge.map+vrrt::kHandsOffset,sizeof h);assert(fabs(h.headTransform[12])<.001&&fabs(h.headTransform[13]-1.6)<.001&&fabs(h.headTransform[14])<.001);
 for(auto& eye:v)eye.pose.position.x+=.1;
 bridge.tracking(v,true,control);memcpy(&h,bridge.map+vrrt::kHandsOffset,sizeof h);assert(fabs(h.headTransform[12]-.1)<.001);
 puts("Neutral startup, one-time live rebase, correct height and subsequent head motion passed.");
}
