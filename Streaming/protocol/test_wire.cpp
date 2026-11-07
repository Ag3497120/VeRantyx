#include "mvr_geometry_wire.hpp"
#include <iostream>
#include <random>
using namespace mvr;
static uint64_t checks=0;
static void check(bool b,const char* message){++checks;if(!b)throw std::runtime_error(message);}
template<class F>static void reject(F f){bool caught=false;try{f();}catch(const std::invalid_argument&){caught=true;}check(caught,"invalid input accepted");}
static void put32(wire::Bytes& b,size_t at,uint32_t n){for(size_t i=0;i<4;i++)b[at+i]=uint8_t(n>>(8*i));}
static void put64(wire::Bytes& b,size_t at,uint64_t n){for(size_t i=0;i<8;i++)b[at+i]=uint8_t(n>>(8*i));}
static void repairDigest(wire::Bytes& b){put64(b,32,wire::digest(b.data()+wire::HeaderBytes,b.size()-wire::HeaderBytes));}
static wire::Descriptor fixture(bool crop,bool foveation,uint64_t session=1,uint64_t revision=1){
 wire::Descriptor d{};d.baseline={2064,2208};d.native={geometry::Fov{-.94,.76,.88,-.84},geometry::Fov{-.76,.94,.88,-.84}};
 geometry::CropOptions c;c.enabled=crop;geometry::FoveationOptions f;f.enabled=foveation;d.plan=geometry::makePlan(d.native,d.baseline,c,f,session,revision);return d;
}
int main(){try{
 for(unsigned flags=0;flags<4;flags++){
  auto d=fixture(flags&1,flags&2);auto b=wire::encodeOffer(d);check(b.size()==324,"descriptor size");check(b[0]=='M'&&b[1]=='V'&&b[2]=='G'&&b[3]=='1',"magic endian");
  auto decoded=wire::decodeOffer(b);check(decoded.plan.features==flags,"features roundtrip");
  check(wire::encodeOffer(decoded)==b,"canonical roundtrip differs");
  check(std::memcmp(&d.plan.gpu,&decoded.plan.gpu,sizeof d.plan.gpu)==0,"GPU ABI not reconstructed");
  for(size_t n=0;n<b.size();n++){wire::Bytes truncated(b.begin(),b.begin()+n);reject([&]{wire::decodeOffer(truncated);});}
  auto trailing=b;trailing.push_back(0);reject([&]{wire::decodeOffer(trailing);});
  for(size_t i=wire::HeaderBytes;i<b.size();i++){auto bad=b;bad[i]^=0x80;reject([&]{wire::decodeOffer(bad);});}
  wire::Sender sender;wire::Receiver receiver;auto offer=sender.offer(d);auto e=sender.epoch();
  check(!sender.acceptsFrame(e)&&!receiver.acceptsFrame(e),"pre-offer frame");auto ack=receiver.acceptOffer(offer);
  check(!receiver.active()&&!receiver.acceptsFrame(e),"receiver active before commit");
  auto staleAck=ack;put64(staleAck,24,e.revision+1);reject([&]{sender.acceptAck(staleAck);});
  auto commit=sender.acceptAck(ack);check(!sender.acceptsFrame(e),"sender active before commit sent");
  auto staleCommit=commit;put64(staleCommit,16,e.session+1);check(!receiver.acceptCommit(staleCommit),"stale commit activated");
  sender.commitSent();check(receiver.acceptCommit(commit),"valid commit rejected");
  check(sender.acceptsFrame(e)&&receiver.acceptsFrame(e)&&receiver.active(),"active geometry missing");
  check(!receiver.acceptsFrame({e.session,e.revision+1,e.digest}),"stale frame accepted");
  reject([&]{receiver.acceptOffer(offer);});reject([&]{sender.offer(d);});
  sender.disconnect();receiver.disconnect();check(!receiver.active()&&!receiver.acceptsFrame(e),"disconnect retained geometry");
  reject([&]{receiver.acceptOffer(offer);});reject([&]{sender.offer(d);});
  auto next=fixture(flags&1,flags&2,2,1);auto o2=sender.offer(next);auto a2=receiver.acceptOffer(o2);auto c2=sender.acceptAck(a2);sender.commitSent();check(receiver.acceptCommit(c2),"new session restart failed");
 }
 auto d=fixture(true,true);auto b=wire::encodeOffer(d);
 // Descriptor validation is independent of the accidental-corruption digest:
 // recompute the digest after deliberately constructing malformed descriptors.
 auto modify=[&](size_t at,uint32_t value){auto x=b;put32(x,at,value);repairDigest(x);return x;};
 reject([&]{wire::decodeOffer(modify(40,0x80000000));}); // unknown feature
 reject([&]{wire::decodeOffer(modify(44,0));}); // baseline width
 reject([&]{wire::decodeOffer(modify(44,0xffffffff));}); // size/overflow
 auto invalidFov=b;put64(invalidFov,52,0x7ff8000000000000ull);repairDigest(invalidFov);reject([&]{wire::decodeOffer(invalidFov);});
 auto staleGeometry=b;put64(staleGeometry,84,0);repairDigest(staleGeometry);reject([&]{wire::decodeOffer(staleGeometry);});
 // First eye renderWidth:12+64 into body. sourceRect follows8bytes later.
 reject([&]{wire::decodeOffer(modify(116,1745));});
 reject([&]{wire::decodeOffer(modify(124,0));}); // shifts left eye onto nasal crop
 reject([&]{wire::decodeOffer(modify(124,0xffffffff));});
 reject([&]{wire::decodeOffer(modify(140,16385));}); // axis source width cap/mismatch
 reject([&]{wire::decodeOffer(modify(144,3000));}); // expands packed width
 reject([&]{wire::decodeOffer(modify(156,0));}); // impossible centre/edge
 wire::Capabilities noCrop;noCrop.features=geometry::FeatureFoveation;reject([&]{wire::decodeOffer(b,noCrop);});
 wire::Capabilities small;small.maxFrameWidth=3000;reject([&]{wire::decodeOffer(b,small);});
 wire::Capabilities noFeatures;noFeatures.features=0;auto off=wire::encodeOffer(fixture(false,false));check(wire::decodeOffer(off,noFeatures).plan.features==0,"no-op rejected by no-feature receiver");
 for(auto where:{size_t(0),size_t(4),size_t(6),size_t(8),size_t(12)}){auto x=b;x[where]^=0xff;reject([&]{wire::decodeOffer(x);});}
 auto zeroSession=b;put64(zeroSession,16,0);reject([&]{wire::decodeOffer(zeroSession);});
 // No floats are accepted for the GPU ABI: the sender's forged uniform values
 // are ignored and the receiver rebuilds them from validated integer maps.
 auto forged=d;forged.plan.gpu.eyes[0].x.sourcePixels=std::numeric_limits<float>::infinity();
 auto restored=wire::decodeOffer(wire::encodeOffer(forged));check(std::isfinite(restored.plan.gpu.eyes[0].x.sourcePixels),"untrusted GPU float survived");
 // Random truncation/corruption corpus; deterministic and bounded.
 std::mt19937 rng(0x4d565247);
 for(unsigned i=0;i<500;i++){auto x=b;auto at=wire::HeaderBytes+rng()%(b.size()-wire::HeaderBytes);x[at]^=1u<<(rng()%8);reject([&]{wire::decodeOffer(x);});}
 std::cout<<"PASS "<<checks<<" wire assertions; offer324B, ACK40B, COMMIT40B; no live network/runtime test\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
