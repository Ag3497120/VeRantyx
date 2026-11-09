#ifndef MVR_GEOMETRY_HPP
#define MVR_GEOMETRY_HPP
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

// Self-contained C++17 geometry. Pixel coordinates denote texel EDGES; centres
// are i + .5. FOVs contain OpenXR radians. Image y=0 is TOP, never bottom.
namespace mvr { namespace geometry {
struct Size { uint32_t width{}, height{}; };
struct Fov { double left{}, right{}, up{}, down{}; };
struct Rect { uint32_t x{}, y{}, width{}, height{}; }; // y measured from top
struct CropOptions { bool enabled=false; double horizontal=.85, vertical=.90; uint32_t alignmentX=16, alignmentY=16; };
struct CropResult { Fov fov; Size size; Rect sourceRect; };
inline void require(bool ok, const char* message) { if(!ok) throw std::invalid_argument(message); }
inline uint32_t alignDown(uint32_t x,uint32_t a) { require(a!=0,"zero alignment"); return x-x%a; }
inline uint32_t alignUp(uint32_t x,uint32_t a) { require(a!=0,"zero alignment"); uint64_t v=(uint64_t(x)+a-1)/a*a; require(v<=UINT32_MAX,"alignment overflow");return uint32_t(v); }
inline void validate(const Fov& f) {
 constexpr double limit=1.57079632679489661923;
 require(std::isfinite(f.left)&&std::isfinite(f.right)&&std::isfinite(f.up)&&std::isfinite(f.down),"nonfinite FOV");
 require(f.left>-limit&&f.left<0&&f.right>0&&f.right<limit&&f.down>-limit&&f.down<0&&f.up>0&&f.up<limit,"FOV must contain optical axis");
}
inline CropResult crop(const Fov& native, Size original, unsigned eye, const CropOptions& o={}) {
 validate(native); require(eye<2,"invalid eye");require(original.width&&original.height,"zero image size");
 CropResult r{native,original,{0,0,original.width,original.height}};
 if(!o.enabled)return r;
 require(std::isfinite(o.horizontal)&&o.horizontal>0&&o.horizontal<=1&&std::isfinite(o.vertical)&&o.vertical>0&&o.vertical<=1,"invalid crop retention");
 auto retained=[](uint32_t n,double fraction,uint32_t alignment){if(fraction==1)return n;auto raw=uint32_t(std::floor(double(n)*fraction+1e-9));auto v=alignDown(raw,alignment);require(v>0,"crop leaves no aligned pixels");return v;};
 r.size={retained(original.width,o.horizontal,o.alignmentX),retained(original.height,o.vertical,o.alignmentY)};
 uint32_t removeX=original.width-r.size.width,removeY=original.height-r.size.height;
 double L=std::tan(native.left),R=std::tan(native.right),U=std::tan(native.up),D=std::tan(native.down);
 // Remove only temporal edge: left image left edge, right image right edge.
 r.sourceRect.x=eye==0?removeX:0;
 // Integer crop around the native optical origin. Actual FOV is then derived
 // from this rectangle: no silent stretch after codec alignment/rounding.
 r.sourceRect.y=std::min(removeY,uint32_t(std::llround(double(removeY)*U/(U-D))));
 r.sourceRect.width=r.size.width;r.sourceRect.height=r.size.height;
 double sx=(R-L)/original.width,sy=(U-D)/original.height;
 if(eye==0)r.fov.left=std::atan(L+removeX*sx);else r.fov.right=std::atan(R-removeX*sx);
 if(removeY) {r.fov.up=std::atan(U-r.sourceRect.y*sy);r.fov.down=std::atan(U-(r.sourceRect.y+r.size.height)*sy);}
 validate(r.fov);return r;
}
struct AxisMap {
 uint32_t sourcePixels{},packedPixels{},sourceBegin{},sourceEnd{},packedBegin{},packedEnd{};
 bool identity()const{return sourcePixels==packedPixels&&sourceBegin==packedBegin&&sourceEnd==packedEnd;}
};
struct ShaderAxis {float sourcePixels,packedPixels,sourceBegin,sourceEnd,packedBegin,packedEnd,reserved0,reserved1;};
struct ShaderEye {ShaderAxis x,y;};
struct ShaderParams {ShaderEye eyes[2];};
static_assert(sizeof(ShaderAxis)==32&&sizeof(ShaderEye)==64&&sizeof(ShaderParams)==128,"GPU ABI mismatch");
inline ShaderAxis shader(const AxisMap& m) {return {float(m.sourcePixels),float(m.packedPixels),float(m.sourceBegin),float(m.sourceEnd),float(m.packedBegin),float(m.packedEnd),0,0};}
inline AxisMap identityAxis(uint32_t size) {require(size>0,"zero axis");return {size,size,0,size,0,size};}
inline AxisMap foveate(uint32_t size,double centre,double centreFraction=.60,double edgeRatio=2.,uint32_t alignment=16) {
 require(size>0&&size<=16384,"unsupported axis size");require(std::isfinite(centre)&&centre>=0&&centre<=1,"invalid optical centre");
 require(std::isfinite(centreFraction)&&centreFraction>0&&centreFraction<=1&&std::isfinite(edgeRatio)&&edgeRatio>=1,"invalid foveation settings");require(alignment>0,"zero alignment");
 if(centreFraction==1||edgeRatio==1)return identityAxis(size);
 uint32_t count=std::max(1u,std::min(size,uint32_t(std::floor(size*centreFraction))));
 if(count==size)return identityAxis(size);
 auto begin=int64_t(std::llround(centre*size-.5*count));
 uint32_t a=uint32_t(std::max<int64_t>(0,std::min<int64_t>(size-count,begin))),b=a+count,right=size-b;
 uint32_t idealLeft=uint32_t(std::ceil(a/edgeRatio)),idealRight=uint32_t(std::ceil(right/edgeRatio));
 uint32_t packed=alignUp(idealLeft+count+idealRight,alignment);
 // Keep the centre exactly one source texel per packed texel. Alignment
 // increases only peripheral segment widths, never rescales the centre.
 if(packed>=size)return identityAxis(size);
 uint32_t edges=packed-count;
 uint32_t minL=a?1:0,maxL=a;
 uint32_t minR=right?1:0,maxR=right;
 require(edges>=minL+minR&&edges<=maxL+maxR,"impossible aligned packing");
 uint32_t left=uint32_t(std::llround(double(edges)*a/(a+right)));
 uint32_t low=std::max(minL,edges>maxR?edges-maxR:0),high=std::min(maxL,edges-minR);
 left=std::clamp(left,low,high);
 return {size,packed,a,b,left,left+count};
}
inline void validate(const AxisMap& m) {
 require(m.sourcePixels&&m.packedPixels&&m.packedPixels<=m.sourcePixels&&m.sourceBegin<m.sourceEnd&&m.sourceEnd<=m.sourcePixels&&m.packedBegin<=m.packedEnd&&m.packedEnd<=m.packedPixels,"invalid axis bounds");
 require(m.sourceEnd-m.sourceBegin==m.packedEnd-m.packedBegin,"centre must remain 1:1");
 require(m.packedBegin<=m.sourceBegin&&m.packedPixels-m.packedEnd<=m.sourcePixels-m.sourceEnd,"periphery cannot upscale");
 require((m.sourceBegin==0)==(m.packedBegin==0)&&(m.sourceEnd==m.sourcePixels)==(m.packedEnd==m.packedPixels),"empty edge mismatch");
}
inline double sourceToPacked(const AxisMap& m,double s) {
 s=std::clamp(s,0.,double(m.sourcePixels));
 if(s<m.sourceBegin)return s*double(m.packedBegin)/m.sourceBegin;
 if(s<=m.sourceEnd)return m.packedBegin+s-m.sourceBegin;
 return m.packedEnd+(s-m.sourceEnd)*double(m.packedPixels-m.packedEnd)/(m.sourcePixels-m.sourceEnd);
}
inline double packedToSource(const AxisMap& m,double p) {
 p=std::clamp(p,0.,double(m.packedPixels));
 if(p<m.packedBegin)return p*double(m.sourceBegin)/m.packedBegin;
 if(p<=m.packedEnd)return m.sourceBegin+p-m.packedBegin;
 return m.sourceEnd+(p-m.packedEnd)*double(m.sourcePixels-m.sourceEnd)/(m.packedPixels-m.packedEnd);
}
struct FoveationOptions {bool enabled=false;double centreWidth=.60,centreHeight=.60,edgeRatio=2.;uint32_t alignmentX=16,alignmentY=16;};
struct EyePlan {CropResult render;AxisMap x,y;};
struct Plan {uint32_t protocolVersion=1,features=0;uint64_t session=0,revision=0;std::array<EyePlan,2> eyes;ShaderParams gpu;};
constexpr uint32_t FeatureCrop=1,FeatureFoveation=2;
inline Plan makePlan(const std::array<Fov,2>& native,Size original,const CropOptions& c,const FoveationOptions& f,uint64_t session,uint64_t revision) {
 require(session&&revision,"zero geometry epoch");Plan p;p.session=session;p.revision=revision;p.features=(c.enabled?FeatureCrop:0)|(f.enabled?FeatureFoveation:0);
 for(unsigned eye=0;eye<2;eye++){
  auto& v=p.eyes[eye];v.render=crop(native[eye],original,eye,c);
  double L=std::tan(v.render.fov.left),R=std::tan(v.render.fov.right),U=std::tan(v.render.fov.up),D=std::tan(v.render.fov.down);
  v.x=f.enabled?foveate(v.render.size.width,-L/(R-L),f.centreWidth,f.edgeRatio,f.alignmentX):identityAxis(v.render.size.width);
  v.y=f.enabled?foveate(v.render.size.height,U/(U-D),f.centreHeight,f.edgeRatio,f.alignmentY):identityAxis(v.render.size.height);
  validate(v.x);validate(v.y);p.gpu.eyes[eye]={shader(v.x),shader(v.y)};
 }
 require(p.eyes[0].x.packedPixels==p.eyes[1].x.packedPixels&&p.eyes[0].y.packedPixels==p.eyes[1].y.packedPixels,"SBS encoder requires equal eye dimensions");return p;
}
// Sender/receiver must agree on a digest of the complete serialized descriptor,
// not just the two feature flags. An old ACK cannot activate another session.
struct Epoch {uint64_t session{},revision{},digest{};bool operator==(const Epoch& b)const{return session==b.session&&revision==b.revision&&digest==b.digest;}};
class Negotiation {
public:enum class State {Idle,Offered,Acknowledged,Active,Closed};
private:State state_=State::Idle;Epoch epoch_{};
public:
 State state()const{return state_;}Epoch epoch()const{return epoch_;}
 void offer(Epoch e){require(e.session&&e.revision&&e.digest,"invalid epoch");require(state_!=State::Active,"stop stream before changing geometry");require(state_!=State::Closed||e.session!=epoch_.session,"reconnect needs a new session epoch");epoch_=e;state_=State::Offered;}
 bool acknowledge(Epoch e){if(state_!=State::Offered||!(e==epoch_))return false;state_=State::Acknowledged;return true;}
 bool activate(Epoch e){if(state_!=State::Acknowledged||!(e==epoch_))return false;state_=State::Active;return true;}
 bool acceptsFrame(Epoch e)const{return state_==State::Active&&e==epoch_;}
 void disconnect(){state_=State::Closed;}
};
}}
#endif
