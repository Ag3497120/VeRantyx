#include "sei.hpp"
#include <iostream>
#include <random>
using namespace mvr::sei;
static uint64_t checks=0;
static void check(bool b,const char* m){++checks;if(!b)throw std::runtime_error(m);}
template<class F>void reject(F f){bool threw=false;try{f();}catch(const std::invalid_argument&){threw=true;}check(threw,"invalid data accepted");}
static Bytes frame(unsigned width,Codec codec){Bytes b;auto v=codec==Codec::H264?Bytes{0x65,0x88,0x84,0x21}:Bytes{0x26,0x01,0x88,0x21};appendLength(b,v.size(),width);b.insert(b.end(),v.begin(),v.end());return b;}
int main(){try{
 Agreement random{};std::mt19937 rng(0x4d5652);for(auto& b:random)b=uint8_t(4+rng()%250);
 for(auto codec:{Codec::H264,Codec::Hevc})for(unsigned width:{1u,2u,4u}){
  auto original=frame(width,codec);auto packed=prepend(original,width,codec,random);auto length=readLength(packed.data(),width);
  check(Bytes(packed.begin()+width+length,packed.end())==original,"original AU modified");
  check(fromAccessUnit(packed,width,codec)==random,"roundtrip failed");
  check(!fromAccessUnit(original,width,codec),"non-SEI frame acquired metadata");
 }
 for(unsigned fill=0;fill<4;fill++){
  Agreement a{};a.fill(uint8_t(fill));
  for(auto codec:{Codec::H264,Codec::Hevc}){
   auto n=nal(codec,a);check(fromNal(n.data(),n.size(),codec)==a,"zero-rich metadata roundtrip");
   for(unsigned width:{2u,4u})check(fromAccessUnit(prepend(frame(width,codec),width,codec,a),width,codec)==a,"escaped AU roundtrip");
   if(n.size()>255)reject([&]{prepend(frame(1,codec),1,codec,a);});
  }
 }
 for(unsigned iteration=0;iteration<500;iteration++){
  Bytes source(1+rng()%300);for(auto& b:source)b=uint8_t(rng()%5);auto packed=escape(source);check(unescape(packed.data(),packed.size())==source,"escape inverse failure");
 }
 for(auto invalid:{Bytes{0,0,0},Bytes{0,0,1},Bytes{0,0,2},Bytes{0,0,3},Bytes{0,0,3,4}})reject([&]{unescape(invalid.data(),invalid.size());});
 reject([&]{prepend(frame(4,Codec::H264),3,Codec::H264,random);});
 reject([&]{validateAccessUnit(nullptr,0,4);});reject([&]{validateAccessUnit(Bytes{0,0,0,0}.data(),4,4);});
 auto original=frame(4,Codec::H264);for(size_t n=0;n<original.size();n++)reject([&]{validateAccessUnit(original.data(),n,4);});
 auto h264=nal(Codec::H264,random);auto missing=h264;missing.pop_back();reject([&]{fromNal(missing.data(),missing.size(),Codec::H264);});
 auto bad=h264;bad.back()=0;reject([&]{fromNal(bad.data(),bad.size(),Codec::H264);});bad=h264;bad[0]|=0x80;reject([&]{fromNal(bad.data(),bad.size(),Codec::H264);});
 auto hevc=nal(Codec::Hevc,random);hevc[1]=0;reject([&]{fromNal(hevc.data(),hevc.size(),Codec::Hevc);});
 auto wrongUuid=h264;wrongUuid[3]='X';check(!fromNal(wrongUuid.data(),wrongUuid.size(),Codec::H264),"unknown UUID accepted");
 auto oversized=h264;oversized[2]=249;reject([&]{fromNal(oversized.data(),oversized.size(),Codec::H264);});
 auto unit=prepend(original,4,Codec::H264,random);auto duplicate=prepend(unit,4,Codec::H264,random);reject([&]{fromAccessUnit(duplicate,4,Codec::H264);});
 std::cout<<"PASS "<<checks<<" SEI assertions; H264/HEVC AVCC1/2/4, escaping, preserved AU, malformed inputs; no VT/live stream test\n";return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
