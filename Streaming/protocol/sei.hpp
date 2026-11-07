#ifndef MVR_GEOMETRY_SEI_HPP
#define MVR_GEOMETRY_SEI_HPP
#include <array>
#include <vector>
#include <optional>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <algorithm>

// Codec-standard unregistered SEI carrier. No VideoToolbox dependency.
// This carrier is not authentication. Agreement validation remains mandatory.
namespace mvr { namespace sei {
using Bytes=std::vector<uint8_t>;
using Agreement=std::array<uint8_t,232>;
enum class Codec {H264,Hevc};
constexpr std::array<uint8_t,16> Uuid{{'M','V','R','-','G','E','O','M','E','T','R','Y','-','0','0','1'}};
constexpr size_t MaxAccessUnit=64*1024*1024;
inline void require(bool b,const char* s){if(!b)throw std::invalid_argument(s);}
inline void lengthSize(unsigned n){require(n==1||n==2||n==4,"NAL length width must be1,2or4");}
inline Bytes escape(const Bytes& rbsp){Bytes out;out.reserve(rbsp.size()+rbsp.size()/8);unsigned zeros=0;for(auto c:rbsp){if(zeros>=2&&c<=3){out.push_back(3);zeros=0;}out.push_back(c);zeros=c==0?zeros+1:0;}return out;}
inline Bytes unescape(const uint8_t* data,size_t size){Bytes out;out.reserve(size);unsigned zeros=0;for(size_t i=0;i<size;i++){uint8_t c=data[i];if(zeros>=2&&c==3){require(i+1<size&&data[i+1]<=3,"malformed emulation prevention");zeros=0;continue;}require(!(zeros>=2&&c<=2),"missing emulation prevention");out.push_back(c);zeros=c==0?zeros+1:0;}return out;}
inline Bytes nal(Codec codec,const Agreement& agreement){
 Bytes rbsp;rbsp.reserve(251);rbsp.push_back(5);rbsp.push_back(248);rbsp.insert(rbsp.end(),Uuid.begin(),Uuid.end());rbsp.insert(rbsp.end(),agreement.begin(),agreement.end());rbsp.push_back(0x80);
 auto ebsp=escape(rbsp);Bytes result;if(codec==Codec::H264)result.push_back(0x06);else if(codec==Codec::Hevc){result.push_back(uint8_t(39u<<1));result.push_back(0x01);}else throw std::invalid_argument("unsupported codec");result.insert(result.end(),ebsp.begin(),ebsp.end());return result;
}
inline uint32_t readLength(const uint8_t* data,unsigned width){uint32_t n=0;for(unsigned i=0;i<width;i++)n=(n<<8)|data[i];return n;}
inline void appendLength(Bytes& b,size_t n,unsigned width){lengthSize(width);uint64_t maximum=width==4?uint64_t(UINT32_MAX):((uint64_t(1)<<(8*width))-1);require(n&&n<=maximum,"SEI NAL exceeds length field");for(unsigned i=0;i<width;i++)b.push_back(uint8_t(n>>(8*(width-i-1))));}
inline void validateAccessUnit(const uint8_t* data,size_t size,unsigned width){
 lengthSize(width);require(data&&size&&size<=MaxAccessUnit,"invalid access unit size");size_t pos=0;while(pos<size){require(width<=size-pos,"truncated NAL length");uint32_t n=readLength(data+pos,width);pos+=width;require(n&&n<=size-pos,"invalid/truncated NAL");pos+=n;}
}
inline Bytes prepend(const uint8_t* accessUnit,size_t size,unsigned width,Codec codec,const Agreement& agreement){
 validateAccessUnit(accessUnit,size,width);auto metadata=nal(codec,agreement);Bytes result;result.reserve(size+metadata.size()+width);appendLength(result,metadata.size(),width);result.insert(result.end(),metadata.begin(),metadata.end());result.insert(result.end(),accessUnit,accessUnit+size);return result;
}
inline Bytes prepend(const Bytes& accessUnit,unsigned width,Codec codec,const Agreement& agreement){return prepend(accessUnit.data(),accessUnit.size(),width,codec,agreement);}
inline std::optional<Agreement> fromNal(const uint8_t* data,size_t size,Codec codec){
 require(data&&size,"empty NAL");size_t header=0;
 if(codec==Codec::H264){if((data[0]&0x1f)!=6)return std::nullopt;require(!(data[0]&0x80),"forbidden H264 header");header=1;}
 else if(codec==Codec::Hevc){require(size>=2,"short HEVC NAL");if(((data[0]>>1)&63)!=39)return std::nullopt;require(!(data[0]&0x80)&&(data[1]&7),"forbidden HEVC header");header=2;}
 else throw std::invalid_argument("unsupported codec");
 auto rbsp=unescape(data+header,size-header);std::optional<Agreement> found;size_t pos=0;bool trailing=false;
 auto field=[&](){uint32_t result=0;while(true){require(pos<rbsp.size(),"truncated SEI field");uint8_t n=rbsp[pos++];require(result<=MaxAccessUnit-n,"SEI field overflow");result+=n;if(n!=255)return result;}};
 while(pos<rbsp.size()){
  if(rbsp.size()-pos==1&&rbsp[pos]==0x80){trailing=true;++pos;break;}
  uint32_t type=field(),length=field();require(length<=rbsp.size()-pos,"truncated SEI payload");
  if(type==5&&length>=Uuid.size()&&std::equal(Uuid.begin(),Uuid.end(),rbsp.begin()+pos)){
   require(length==Uuid.size()+Agreement{}.size(),"wrong agreement size");require(!found,"duplicate geometry SEI");Agreement a;std::copy_n(rbsp.begin()+pos+Uuid.size(),a.size(),a.begin());found=a;
  }pos+=length;
 }
 require(trailing&&pos==rbsp.size(),"missing/invalid RBSP trailing bits");return found;
}
inline std::optional<Agreement> fromAccessUnit(const Bytes& b,unsigned width,Codec codec){
 validateAccessUnit(b.data(),b.size(),width);std::optional<Agreement> found;size_t pos=0;while(pos<b.size()){
  auto count=readLength(b.data()+pos,width);pos+=width;auto a=fromNal(b.data()+pos,count,codec);if(a){require(!found,"multiple geometry SEI NALs");found=a;}pos+=count;
 }return found;
}
}}
#endif
