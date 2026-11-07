#pragma once
#include "../NativeARM/vrruntime/vr_runtime.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cmath>
#include <ctime>
#include <vector>
struct SharedBridge {
 bool originProvisional=false;bool livePoseObserved=false; unsigned originSamples=0; XrPosef originPrevious{}; bool originReady=false; float originYaw=0, originX=0, originY=0, originZ=0;
 uint8_t* map=nullptr; vrrt::DoubleBufferHeader* header=nullptr; uint32_t sequence=0;
 vrrt::SharedFrame frame{}; vrrt::FrameProjection projection{}; std::vector<uint8_t> pixels, scratch;
 bool open(){const char* path=getenv("VERANTYX_SHARED_FRAME");if(!path)return false;int fd=::open(path,O_RDWR|O_CREAT,0644);if(fd<0)return false;struct stat st{};if(fstat(fd,&st)||((size_t)st.st_size<vrrt::kSharedMapSize&&ftruncate(fd,vrrt::kSharedMapSize))){close(fd);return false;}void* p=mmap(nullptr,vrrt::kSharedMapSize,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);close(fd);if(p==MAP_FAILED)return false;map=(uint8_t*)p;header=(vrrt::DoubleBufferHeader*)p;__atomic_store_n(&header->captureSourceMode,0,__ATOMIC_RELEASE);return true;}
 ~SharedBridge(){if(map)munmap(map,vrrt::kSharedMapSize);}
 static void matrix(const XrPosef& p,float* m){auto q=p.orientation;float x=q.x,y=q.y,z=q.z,w=q.w;memset(m,0,64);m[0]=1-2*(y*y+z*z);m[1]=2*(x*y+z*w);m[2]=2*(x*z-y*w);m[4]=2*(x*y-z*w);m[5]=1-2*(x*x+z*z);m[6]=2*(y*z+x*w);m[8]=2*(x*z+y*w);m[9]=2*(y*z-x*w);m[10]=1-2*(x*x+y*y);m[12]=p.position.x;m[13]=p.position.y;m[14]=p.position.z;m[15]=1;}
 void tracking(const XrView* v,bool valid,const vrrt::SharedHands& controls){vrrt::SharedHands hands=controls;XrPosef head=v[0].pose;head.position={(v[0].pose.position.x+v[1].pose.position.x)/2,(v[0].pose.position.y+v[1].pose.position.y)/2,(v[0].pose.position.z+v[1].pose.position.z)/2};matrix(head,hands.headTransform);
 const float dist=head.position.x*head.position.x+head.position.y*head.position.y+head.position.z*head.position.z;
 const auto orientation=head.orientation;
 const bool live=std::isfinite(dist)&&(dist>0.000001f||orientation.x*orientation.x+orientation.y*orientation.y+orientation.z*orientation.z>0.0001f);
 if(valid&&(!originReady||(originProvisional&&live))){
  // A neutral but valid runtime pose must not block game startup. Rebase once
  // when live tracking arrives, preserving the default camera/menu location.
  originYaw=atan2f(hands.headTransform[8],hands.headTransform[10]);
  originX=head.position.x;originY=head.position.y-1.6f;originZ=head.position.z;
  originReady=true;originProvisional=!live;originSamples=1;
  printf("Tracking origin: %s, game head height 1.6m\n",originProvisional?"provisional":"live");
 }
 if(originReady){float c=cosf(originYaw),s=sinf(originYaw);for(int col=0;col<3;col++){float x=hands.headTransform[col*4],z=hands.headTransform[col*4+2];hands.headTransform[col*4]=c*x-s*z;hands.headTransform[col*4+2]=s*x+c*z;}float x=head.position.x-originX,z=head.position.z-originZ;hands.headTransform[12]=c*x-s*z;hands.headTransform[13]=head.position.y-originY;hands.headTransform[14]=s*x+c*z;}
 if(originReady){float c=cosf(originYaw),s=sinf(originYaw);for(int h=0;h<2;h++){float* m=h?hands.rightTransform:hands.leftTransform;bool tracked=h?hands.rightTracked:hands.leftTracked;if(!tracked)continue;for(int col=0;col<3;col++){float x=m[col*4],z=m[col*4+2];m[col*4]=c*x-s*z;m[col*4+2]=s*x+c*z;}float x=m[12]-originX,z=m[14]-originZ;m[12]=c*x-s*z;m[13]-=originY;m[14]=s*x+c*z;for(float* velocity:{h?hands.rightVelocity:hands.leftVelocity,h?hands.rightAngularVelocity:hands.leftAngularVelocity}){float x=velocity[0],z=velocity[2];velocity[0]=c*x-s*z;velocity[2]=s*x+c*z;}}}
 hands.headTracked=valid;struct timespec ts{};clock_gettime(CLOCK_MONOTONIC,&ts);hands.visionTimestamp=ts.tv_sec+ts.tv_nsec*1e-9;for(int e=0;e<2;e++){float* t=e?hands.rightTangents:hands.leftTangents;t[0]=tan(v[e].fov.angleLeft);t[1]=tan(v[e].fov.angleRight);t[2]=tan(v[e].fov.angleDown);t[3]=tan(v[e].fov.angleUp);}int32_t seq=__atomic_load_n(&header->poseSequence,__ATOMIC_RELAXED);seq=(seq+1)|1;__atomic_store_n(&header->poseSequence,seq,__ATOMIC_RELEASE);memcpy(map+vrrt::kHandsOffset,&hands,sizeof(hands));__atomic_store_n(&header->poseSequence,seq+1,__ATOMIC_RELEASE);}
 bool read(){if(__atomic_load_n(&header->captureSourceMode,__ATOMIC_ACQUIRE)!=2)return false;auto index=uint32_t(__atomic_load_n(&header->latestReadyIndex,__ATOMIC_ACQUIRE))&1;auto* f=(vrrt::SharedFrame*)(map+sizeof(vrrt::DoubleBufferHeader)+vrrt::kFrameBlockSize*index);vrrt::SharedFrame candidate;memcpy(&candidate,f,sizeof(candidate));if(!candidate.sequenceNumber||candidate.sequenceNumber==sequence)return false;size_t bytes=size_t(candidate.width)*candidate.height*4;if(!candidate.width||candidate.width%2||!candidate.height||bytes>vrrt::kMaxPixelBytes||(candidate.format!=28&&candidate.format!=87))return false;scratch.resize(bytes);memcpy(scratch.data(),(uint8_t*)f+sizeof(*f),bytes);vrrt::FrameProjection proj{};memcpy(&proj,map+vrrt::kProjectionOffset+index*sizeof(proj),sizeof(proj));__atomic_thread_fence(__ATOMIC_ACQUIRE);if(index!=(uint32_t(__atomic_load_n(&header->latestReadyIndex,__ATOMIC_ACQUIRE))&1)||candidate.sequenceNumber!=f->sequenceNumber)return false;pixels.swap(scratch);frame=candidate;projection=proj;sequence=candidate.sequenceNumber;return true;}
 XrPosef renderPose(int eye,float ipd)const{float world[16];memcpy(world,frame.renderHeadTransform,sizeof(world));
 if(originReady){float c=cosf(originYaw),s=sinf(originYaw);for(int col=0;col<4;col++){float x=world[col*4],z=world[col*4+2];world[col*4]=c*x+s*z;world[col*4+2]=-s*x+c*z;}world[12]+=originX;world[13]+=originY;world[14]+=originZ;}
 const float* m=world;XrPosef p{};p.orientation.w=sqrtf(fmaxf(0,1+m[0]+m[5]+m[10]))/2;p.orientation.x=copysignf(sqrtf(fmaxf(0,1+m[0]-m[5]-m[10]))/2,m[6]-m[9]);p.orientation.y=copysignf(sqrtf(fmaxf(0,1-m[0]+m[5]-m[10]))/2,m[8]-m[2]);p.orientation.z=copysignf(sqrtf(fmaxf(0,1-m[0]-m[5]+m[10]))/2,m[1]-m[4]);float side=(eye?1.f:-1.f)*ipd/2;p.position={m[12]+m[0]*side,m[13]+m[1]*side,m[14]+m[2]*side};return p;}
};
