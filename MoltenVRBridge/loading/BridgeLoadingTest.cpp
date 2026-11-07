#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <fstream>
#include <cassert>
#include <chrono>
#include "../shared_bridge.h"
#include "../loading_bridge.h"
int main(int argc,char** argv){
 assert(argc==3);char folder[]="/tmp/alyx-loading-bridge-test.XXXXXX";assert(mkdtemp(folder));
 std::string state=std::string(folder)+"/state.json",stage=std::string(folder)+"/stage.json";
 setenv("VERANTYX_LOADING_LIBRARY",argv[1],1);setenv("VERANTYX_NATIVE_GAME_DIR",argv[2],1);setenv("VERANTYX_NATIVE_LOADING_FILE",state.c_str(),1);setenv("VERANTYX_NATIVE_STAGE_FILE",stage.c_str(),1);
 std::ofstream(state)<<"{\"type\":\"begin_loading\",\"mapName\":\"a2_quarantine_entrance\",\"chapter\":\"HALF-LIFE: ALYX\",\"loadingText\":\"LOADING\",\"visible\":true}";
 SharedBridge game;game.map=(uint8_t*)mmap(nullptr,vrrt::kSharedMapSize,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);assert(game.map!=MAP_FAILED);game.originReady=true;
 vrrt::SharedHands hands{};hands.headTransform[0]=hands.headTransform[5]=hands.headTransform[10]=hands.headTransform[15]=1;hands.headTransform[13]=1.65;
 for(float* f:{hands.leftTangents,hands.rightTangents}){f[0]=f[2]=-1;f[1]=f[3]=1;}
 LoadingBridge loading(1728,1984);assert(loading.context);
 auto now=[](){timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);return ts.tv_sec+ts.tv_nsec*1e-9;};double start=now();bool active=false,changed=false;
 while(!active&&now()-start<90){hands.visionTimestamp=now();memcpy(game.map+vrrt::kHandsOffset,&hands,sizeof hands);active=loading.update(game,now(),changed);usleep(25000);}
 assert(active&&changed);assert(loading.image.frame.width==3456&&loading.image.frame.height==1984&&loading.image.frame.format==87);assert(fabs(loading.image.frame.renderHeadTransform[13]-1.65)<0.001);
 size_t orange=0;for(size_t i=0;i<loading.image.pixels.size();i+=4){int b=loading.image.pixels[i],g=loading.image.pixels[i+1],r=loading.image.pixels[i+2];if(r>90&&r>g+12&&g>b+12)orange++;}assert(orange>100);
 unlink(state.c_str());usleep(150000);assert(!loading.update(game,now(),changed));rmdir(folder);
 puts("C++ bridge -> Swift renderer: native map pixels, C ABI, pose metadata and game-frame handoff passed.");
}
