#ifndef MVR_GEOMETRY_WIRE_HPP
#define MVR_GEOMETRY_WIRE_HPP
#include "../geometry/mvr_geometry.hpp"
#include <vector>
#include <cstring>
#include <optional>

// A standalone descriptor contract, not an existing client wire protocol.
// Transport only over the application's EXISTING authenticated control channel.
// FNV64 detects accidental mismatches; it provides NO cryptographic integrity.
namespace mvr { namespace wire {
using geometry::Epoch;using geometry::Plan;using geometry::Size;using geometry::Fov;
using Bytes=std::vector<uint8_t>;
constexpr uint32_t Magic=0x3147564d; // byte order "MVG1"
constexpr uint16_t Version=1;
constexpr uint32_t HeaderBytes=40,DescriptorBytes=284;
enum class Kind:uint16_t {Offer=1,Ack=2,Commit=3};
struct Capabilities {
 uint32_t features=geometry::FeatureCrop|geometry::FeatureFoveation;
 uint32_t maxEyeWidth=16384,maxEyeHeight=16384,maxFrameWidth=16384;
};
struct Descriptor {Size baseline;std::array<Fov,2> native;Plan plan;};
struct Header {Kind kind;uint64_t session,revision,digest;uint32_t bodyBytes;Epoch epoch()const{return {session,revision,digest};}};
inline void require(bool b,const char* m){geometry::require(b,m);}
inline uint64_t digest(const uint8_t* p,size_t count){uint64_t h=14695981039346656037ull;for(size_t i=0;i<count;i++){h^=p[i];h*=1099511628211ull;}return h;}
inline uint64_t digest(const Bytes& b){return digest(b.data(),b.size());}
struct Writer {
 Bytes b;
 void u16(uint16_t v){for(unsigned i=0;i<2;i++)b.push_back(uint8_t(v>>(i*8)));}
 void u32(uint32_t v){for(unsigned i=0;i<4;i++)b.push_back(uint8_t(v>>(i*8)));}
 void u64(uint64_t v){for(unsigned i=0;i<8;i++)b.push_back(uint8_t(v>>(i*8)));}
 void f64(double v){static_assert(sizeof(double)==8,"64bit double required");uint64_t bits;std::memcpy(&bits,&v,8);u64(bits);}
 void fov(Fov f){f64(f.left);f64(f.right);f64(f.up);f64(f.down);}
 void axis(const geometry::AxisMap& a){u32(a.sourcePixels);u32(a.packedPixels);u32(a.sourceBegin);u32(a.sourceEnd);u32(a.packedBegin);u32(a.packedEnd);}
};
struct Reader {
 const uint8_t* p;size_t size,pos=0;
 explicit Reader(const Bytes& b):p(b.data()),size(b.size()){}
 Reader(const uint8_t* bytes,size_t count):p(bytes),size(count){}
 uint64_t integer(size_t n){require(pos<=size&&n<=size-pos,"truncated message");uint64_t v=0;for(size_t i=0;i<n;i++)v|=uint64_t(p[pos++])<<(8*i);return v;}
 uint16_t u16(){return uint16_t(integer(2));}uint32_t u32(){return uint32_t(integer(4));}uint64_t u64(){return integer(8);}
 double f64(){uint64_t bits=u64();double v;std::memcpy(&v,&bits,8);return v;}
 Fov fov(){Fov v;v.left=f64();v.right=f64();v.up=f64();v.down=f64();return v;}
 geometry::AxisMap axis(){geometry::AxisMap a;a.sourcePixels=u32();a.packedPixels=u32();a.sourceBegin=u32();a.sourceEnd=u32();a.packedBegin=u32();a.packedEnd=u32();return a;}
};
inline void checkSize(Size v,const Capabilities& caps){
 require(v.width&&v.height&&v.width<=16384&&v.height<=16384,"invalid/beyond-contract dimensions");
 require(v.width<=caps.maxEyeWidth&&v.height<=caps.maxEyeHeight,"receiver image capability exceeded");
 require(uint64_t(v.width)*2<=caps.maxFrameWidth,"receiver SBS frame capability exceeded");
}
inline bool closeAngle(double a,double b){return std::abs(a-b)<=1e-7;}
inline void validate(const Descriptor& d,const Capabilities& caps={}){
 const auto& p=d.plan;
 require(p.protocolVersion==Version&&p.session&&p.revision,"invalid descriptor epoch/version");
 require((p.features&~uint32_t(3))==0&&(p.features&~caps.features)==0,"unsupported feature flags");
 checkSize(d.baseline,caps);
 for(unsigned eye=0;eye<2;eye++){
  geometry::validate(d.native[eye]);const auto& e=p.eyes[eye];const auto& r=e.render;geometry::validate(r.fov);checkSize(r.size,caps);
  require(r.sourceRect.width==r.size.width&&r.sourceRect.height==r.size.height,"render and crop sizes differ");
  require(r.sourceRect.x<=d.baseline.width&&r.sourceRect.y<=d.baseline.height&&uint64_t(r.sourceRect.x)+r.size.width<=d.baseline.width&&uint64_t(r.sourceRect.y)+r.size.height<=d.baseline.height,"crop rectangle outside baseline");
  if(p.features&geometry::FeatureCrop){
   require(eye==0?uint64_t(r.sourceRect.x)+r.size.width==d.baseline.width:r.sourceRect.x==0,"nasal crop forbidden");
  }else require(r.sourceRect.x==0&&r.sourceRect.y==0&&r.size.width==d.baseline.width&&r.size.height==d.baseline.height,"crop without capability flag");
  double L=std::tan(d.native[eye].left),R=std::tan(d.native[eye].right),U=std::tan(d.native[eye].up),D=std::tan(d.native[eye].down);
  double sx=(R-L)/d.baseline.width,sy=(U-D)/d.baseline.height;
  Fov expected{std::atan(L+r.sourceRect.x*sx),std::atan(L+(r.sourceRect.x+r.size.width)*sx),std::atan(U-r.sourceRect.y*sy),std::atan(U-(r.sourceRect.y+r.size.height)*sy)};
  require(closeAngle(r.fov.left,expected.left)&&closeAngle(r.fov.right,expected.right)&&closeAngle(r.fov.up,expected.up)&&closeAngle(r.fov.down,expected.down),"FOV does not describe retained pixel rays");
  geometry::validate(e.x);geometry::validate(e.y);
  require(e.x.sourcePixels==r.size.width&&e.y.sourcePixels==r.size.height,"packing source and render size differ");
  checkSize({e.x.packedPixels,e.y.packedPixels},caps);
  if(!(p.features&geometry::FeatureFoveation))require(e.x.identity()&&e.y.identity(),"packing without feature flag");
 }
 require(p.eyes[0].render.size.width==p.eyes[1].render.size.width&&p.eyes[0].render.size.height==p.eyes[1].render.size.height,"different SBS render dimensions");
 require(p.eyes[0].x.packedPixels==p.eyes[1].x.packedPixels&&p.eyes[0].y.packedPixels==p.eyes[1].y.packedPixels,"different SBS video dimensions");
}
inline void rebuildGpu(Plan& p){for(unsigned eye=0;eye<2;eye++)p.gpu.eyes[eye]={geometry::shader(p.eyes[eye].x),geometry::shader(p.eyes[eye].y)};}
inline Bytes body(const Descriptor& d){
 validate(d);Writer w;w.u32(d.plan.features);w.u32(d.baseline.width);w.u32(d.baseline.height);
 for(unsigned eye=0;eye<2;eye++){
  const auto& e=d.plan.eyes[eye];w.fov(d.native[eye]);w.fov(e.render.fov);w.u32(e.render.size.width);w.u32(e.render.size.height);
  auto r=e.render.sourceRect;w.u32(r.x);w.u32(r.y);w.u32(r.width);w.u32(r.height);w.axis(e.x);w.axis(e.y);
 }
 require(w.b.size()==DescriptorBytes,"internal descriptor length mismatch");return w.b;
}
inline Bytes message(Kind kind,Epoch epoch,const Bytes& payload={}){
 require(epoch.session&&epoch.revision&&epoch.digest,"invalid message epoch");
 require(kind==Kind::Offer||kind==Kind::Ack||kind==Kind::Commit,"unknown message kind");
 require(kind==Kind::Offer?payload.size()==DescriptorBytes:payload.empty(),"invalid message payload size");
 Writer w;w.u32(Magic);w.u16(Version);w.u16(uint16_t(kind));w.u32(HeaderBytes);w.u32(uint32_t(payload.size()));w.u64(epoch.session);w.u64(epoch.revision);w.u64(epoch.digest);w.b.insert(w.b.end(),payload.begin(),payload.end());return w.b;
}
inline Header header(const Bytes& b){
 Reader r(b);require(r.u32()==Magic,"wrong message magic");require(r.u16()==Version,"unsupported wire version");uint16_t kind=r.u16();require(kind>=1&&kind<=3,"invalid message kind");require(r.u32()==HeaderBytes,"wrong header length");
 uint32_t n=r.u32();Header h{Kind(kind),r.u64(),r.u64(),r.u64(),n};
 require(h.session&&h.revision&&h.digest,"zero epoch component");
 require(n==(h.kind==Kind::Offer?DescriptorBytes:0),"wrong body length");require(b.size()==size_t(HeaderBytes)+n,"truncated or trailing payload");
 if(h.kind==Kind::Offer)require(digest(b.data()+HeaderBytes,n)==h.digest,"descriptor digest mismatch");return h;
}
inline Bytes encodeOffer(const Descriptor& d){auto b=body(d);return message(Kind::Offer,{d.plan.session,d.plan.revision,digest(b)},b);}
inline Descriptor decodeOffer(const Bytes& bytes,const Capabilities& caps={}){
 auto h=header(bytes);require(h.kind==Kind::Offer,"expected offer");Reader r(bytes.data()+HeaderBytes,h.bodyBytes);Descriptor d{};d.plan.protocolVersion=Version;d.plan.session=h.session;d.plan.revision=h.revision;
 d.plan.features=r.u32();d.baseline={r.u32(),r.u32()};
 for(unsigned eye=0;eye<2;eye++){
  auto& e=d.plan.eyes[eye];d.native[eye]=r.fov();e.render.fov=r.fov();e.render.size={r.u32(),r.u32()};e.render.sourceRect={r.u32(),r.u32(),r.u32(),r.u32()};e.x=r.axis();e.y=r.axis();
 }
 require(r.pos==r.size,"descriptor trailing fields");validate(d,caps);rebuildGpu(d.plan);return d;
}
class Sender {
 geometry::Negotiation state_;std::optional<Descriptor> pending_;
public:
 Bytes offer(const Descriptor& d){auto b=encodeOffer(d);auto h=header(b);state_.offer(h.epoch());pending_=d;rebuildGpu(pending_->plan);return b;}
 Bytes acceptAck(const Bytes& b){auto h=header(b);require(h.kind==Kind::Ack&&state_.acknowledge(h.epoch()),"unexpected/stale ACK");return message(Kind::Commit,h.epoch());}
 void commitSent(){require(state_.activate(state_.epoch()),"commit without matching ACK");}
 bool acceptsFrame(Epoch e)const{return state_.acceptsFrame(e);}Epoch epoch()const{return state_.epoch();}
 void disconnect(){state_.disconnect();pending_.reset();}
};
class Receiver {
 Capabilities caps_;geometry::Negotiation state_;std::optional<Descriptor> pending_,active_;
public:
 explicit Receiver(Capabilities caps={}):caps_(caps){}
 Bytes acceptOffer(const Bytes& b){
  auto d=decodeOffer(b,caps_);auto h=header(b);
  require(state_.state()!=geometry::Negotiation::State::Active,"stop stream before new geometry");
  auto previous=state_.epoch();
  if(previous.session==h.session)require(h.revision>previous.revision,"stale descriptor revision");
  state_.offer(h.epoch());require(state_.acknowledge(h.epoch()),"internal ACK state");pending_=d;active_.reset();return message(Kind::Ack,h.epoch());
 }
 bool acceptCommit(const Bytes& b){auto h=header(b);if(h.kind!=Kind::Commit||!state_.activate(h.epoch()))return false;active_=pending_;pending_.reset();return true;}
 bool acceptsFrame(Epoch e)const{return state_.acceptsFrame(e);}
 const Descriptor* active()const{return active_?&*active_:nullptr;}
 void disconnect(){state_.disconnect();pending_.reset();active_.reset();}
};
}}
#endif
