#include "mvr_geometry.hpp"
#include <iostream>
#include <random>
#include <cstring>
using namespace mvr::geometry;
static uint64_t checks=0;
static void check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
static bool near(double a,double b,double eps=1e-10){return std::abs(a-b)<=eps;}
static double tx(const Fov& f,double u){return std::tan(f.left)+(std::tan(f.right)-std::tan(f.left))*u;}
static double ty(const Fov& f,double v){return std::tan(f.up)-(std::tan(f.up)-std::tan(f.down))*v;}
template<class F>void rejected(F f){bool caught=false;try{f();}catch(const std::invalid_argument&){caught=true;}check(caught,"invalid request accepted");}
int main(){try{
 std::array<Fov,2> native{{{-.94,.76,.88,-.84},{-.76,.94,.88,-.84}}};Size size{2064,2208};CropOptions c;c.enabled=true;
 FoveationOptions f;f.enabled=true;
 auto p=makePlan(native,size,c,f,1,1);
 check(p.eyes[0].render.size.width==1744&&p.eyes[0].render.size.height==1984,"crop alignment");
 check(p.features==3,"independent feature flags");
 for(unsigned eye=0;eye<2;eye++){
  auto r=p.eyes[eye].render;check(r.sourceRect.width==r.size.width&&r.sourceRect.height==r.size.height,"rect size");
  if(eye==0)check(r.fov.right==native[eye].right,"left-eye nasal plane moved");
  else check(r.fov.left==native[eye].left,"right-eye nasal plane moved");
  for(unsigned i=0;i<r.size.width;i++)check(near(tx(r.fov,(i+.5)/r.size.width),tx(native[eye],(r.sourceRect.x+i+.5)/size.width)),"retained X ray moved");
  for(unsigned i=0;i<r.size.height;i++)check(near(ty(r.fov,(i+.5)/r.size.height),ty(native[eye],(r.sourceRect.y+i+.5)/size.height)),"retained Y ray moved");
  auto r2=crop(native[eye],size,eye,c);check(r2.fov.left==r.fov.left&&r2.fov.up==r.fov.up,"same immutable input not idempotent");
 }
 // OFF must retain byte-equivalent source FOV fields and dimensions, even for
 // dimensions that aren't codec aligned. Caller retains original OFF path.
 auto off=makePlan(native,{2063,2207},CropOptions{},FoveationOptions{},2,2);
 for(unsigned e=0;e<2;e++){
  check(std::memcmp(&off.eyes[e].render.fov,&native[e],sizeof(Fov))==0,"OFF FOV changed");
  check(off.eyes[e].render.size.width==2063&&off.eyes[e].render.size.height==2207,"OFF size changed");
  check(off.eyes[e].x.identity()&&off.eyes[e].y.identity(),"OFF foveation changed");
 }
 check(off.features==0,"OFF flags");
 auto cropOnly=makePlan(native,size,c,FoveationOptions{},3,1);check(cropOnly.features==1&&cropOnly.eyes[0].x.identity(),"crop-only dependency");
 auto foveationOnly=makePlan(native,size,CropOptions{},f,4,1);check(foveationOnly.features==2&&foveationOnly.eyes[0].render.size.width==2064,"foveation changed rendering");
 // The external Mac bridge has a32-pixel SOURCE-height allocation grid.
 // Cropping on that grid must keep every retained row on its original ray.
 auto bridgeCrop=c;bridgeCrop.alignmentX=bridgeCrop.alignmentY=32;
 for(auto baseline:{Size{1664,1792},Size{1888,2048},Size{2048,2208},Size{2064,2208}}){
  for(unsigned eye=0;eye<2;eye++){
   auto r=crop(native[eye],baseline,eye,bridgeCrop);
   check(r.size.width%32==0&&r.size.height%32==0,"native bridge source allocation grid");
   if(baseline.height==2048)check(r.size.height==1824,"16-aligned source silently truncated by bridge");
   if(baseline.width==2064)check(r.size.width==1728,"16-aligned width silently truncated by bridge");
   for(unsigned x=0;x<r.size.width;x++)check(near(tx(r.fov,(x+.5)/r.size.width),tx(native[eye],(r.sourceRect.x+x+.5)/baseline.width)),"32-aligned crop stretches retained column");
   for(unsigned y=0;y<r.size.height;y++)check(near(ty(r.fov,(y+.5)/r.size.height),ty(native[eye],(r.sourceRect.y+y+.5)/baseline.height)),"32-aligned crop stretches retained row");
  }
 }
 std::mt19937 rng(0x4d5652);
 for(unsigned iteration=0;iteration<300;iteration++){
  uint32_t n=16*(2+rng()%500);double centre=.12+(rng()%7600)/10000.,fraction=.2+(rng()%7000)/10000.,ratio=1.2+(rng()%3800)/1000.;
  auto a=foveate(n,centre,fraction,ratio);validate(a);
  check(a.packedPixels<=a.sourcePixels,"packing enlarged image");check(a.packedPixels%16==0,"packed alignment");
  double prev=-1;
  for(unsigned i=0;i<=1000;i++){
   double s=n*i/1000.;auto q=sourceToPacked(a,s);check(q>=prev,"mapping not monotonic");prev=q;
   check(near(packedToSource(a,q),s,1e-9),"inverse mapping failure");
  }
  for(uint32_t s=a.sourceBegin;s<a.sourceEnd;s++){
   auto q=sourceToPacked(a,s+.5);check(near(q,std::floor(q)+.5),"centre resampled to fractional texel");
   check(near(sourceToPacked(a,s+1.)-sourceToPacked(a,s),1.),"central density changed");
  }
  check(near(sourceToPacked(a,0),0)&&near(sourceToPacked(a,n),a.packedPixels),"edge endpoint mismatch");
  check(near(packedToSource(a,a.packedBegin),a.sourceBegin)&&near(packedToSource(a,a.packedEnd),a.sourceEnd),"piecewise seam mismatch");
 }
 // Shader uniforms must reproduce the host's exact integer geometry.
 check(p.gpu.eyes[0].x.sourcePixels==p.eyes[0].x.sourcePixels,"GPU source size");
 check(p.gpu.eyes[0].x.packedBegin==p.eyes[0].x.packedBegin,"GPU packing boundary");
 check(p.gpu.eyes[1].y.reserved0==0&&p.gpu.eyes[1].y.reserved1==0,"GPU padding uninitialized");
 Negotiation n;Epoch e{1,1,123},stale{1,2,123};
 check(!n.activate(e)&&!n.acceptsFrame(e),"activation without ack");n.offer(e);
 check(!n.acknowledge(stale),"stale ack accepted");check(n.acknowledge(e),"matching ack rejected");
 check(!n.acceptsFrame(e),"frame before activation");check(n.activate(e)&&n.acceptsFrame(e),"activation failed");
 check(!n.acceptsFrame({2,1,123})&&!n.acceptsFrame({1,1,124}),"mismatched geometry accepted");
 rejected([&]{n.offer(stale);});n.disconnect();check(!n.acceptsFrame(e),"frame accepted after disconnect");
 rejected([&]{n.offer(e);});n.offer({2,1,234});check(!n.acknowledge(e),"prior session ack accepted");
 rejected([&]{crop({0,.8,.8,-.8},size,0,c);});
 rejected([&]{auto x=c;x.horizontal=0;crop(native[0],size,0,x);});
 rejected([&]{auto x=c;x.alignmentX=0;crop(native[0],size,0,x);});
 rejected([&]{foveate(1024,.5,.6,0);});rejected([&]{foveate(1024,std::numeric_limits<double>::quiet_NaN());});
 rejected([&]{makePlan(native,size,c,f,0,1);});
 std::cout<<"PASS "<<checks<<" checks; crop "<<p.eyes[0].render.size.width<<'x'<<p.eyes[0].render.size.height<<" per eye; packed "<<p.eyes[0].x.packedPixels<<'x'<<p.eyes[0].y.packedPixels<<" per eye\n";
 return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
