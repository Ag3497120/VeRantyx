/* SPDX-License-Identifier: MIT */
#include "sei.h"
#include <string.h>
static const unsigned char uuid[16]={'M','V','R','-','G','E','O','M','E','T','R','Y','-','0','0','1'};
static int nal(const unsigned char* p,size_t n,unsigned codec,struct MvrAgreement* out){
 unsigned header=codec==MVR_CODEC_H264?1:2;
 if(n<=header)return 0;
 if(codec==MVR_CODEC_H264){if((p[0]&31)!=6)return 0;if(p[0]&0x80)return -1;}else{unsigned type=(p[0]>>1)&63;if(type!=39)return 0;if((p[0]&0x80)||!(p[1]&7))return -1;}
 /* Geometry only: bound parsing to first4KiBSEINAL; never allocate by untrustedsize. */
 if(n-header>4096)return 0;
 unsigned char rbsp[4096];size_t len=0;unsigned zeros=0;
 for(size_t i=header;i<n;i++){
  unsigned char b=p[i];if(zeros>=2&&b==3){if(i+1>=n||p[i+1]>3)return -1;zeros=0;continue;}
  if(zeros>=2&&b<=2)return -1;
  rbsp[len++]=b;zeros=b==0?zeros+1:0;
 }
 int found=0;struct MvrAgreement candidate;
 for(size_t pos=0;pos<len;){
  if(len-pos==1&&rbsp[pos]==0x80){if(found)*out=candidate;return found;}
  size_t type=0,size=0;unsigned char b;
  do{if(pos>=len||type>4096)return 0;b=rbsp[pos++];type+=b;}while(b==255);
  do{if(pos>=len||size>4096)return 0;b=rbsp[pos++];size+=b;}while(b==255);
  if(size>len-pos)return 0;
  if(type==5&&size>=16&&!memcmp(rbsp+pos,uuid,16)){if(size!=16+sizeof *out||found)return -1;memcpy(&candidate,rbsp+pos+16,sizeof candidate);found=1;}
  pos+=size;
 }
 return -1;
}
static size_t start(const unsigned char* b,size_t n,size_t p,unsigned* count){
 for(size_t i=p;i+3<=n;i++){
  if(b[i]==0&&b[i+1]==0&&b[i+2]==1){*count=3;return i;}
  if(i+4<=n&&b[i]==0&&b[i+1]==0&&b[i+2]==0&&b[i+3]==1){*count=4;return i;}
 }
 return n;
}
int mvrFindGeometrySei(const unsigned char* p,size_t n,unsigned codec,struct MvrAgreement* out){
 if(!p||!out||!n||n>64*1024*1024||(codec!=MVR_CODEC_H264&&codec!=MVR_CODEC_HEVC))return 0;
 unsigned count=0;size_t at=start(p,n,0,&count);struct MvrAgreement found,tmp;int seen=0;
 if(at<n&&at<=3){
  int malformed=0;
  while(at<n){size_t begin=at+count;unsigned nextCount=0;size_t next=start(p,n,begin,&nextCount);int r=nal(p+begin,next-begin,codec,&tmp);if(r<0){malformed=1;break;}if(r>0){if(seen){malformed=1;break;}found=tmp;seen=1;}at=next;count=nextCount;}
  if(!malformed&&seen){*out=found;return 1;}
 }
 /* AVCC/HVCC4-byte length prefix. Try even after an ambiguous00 00 01xx prefix. */
 seen=0;size_t pos=0;
 while(pos+4<=n){size_t len=((size_t)p[pos]<<24)|((size_t)p[pos+1]<<16)|((size_t)p[pos+2]<<8)|p[pos+3];pos+=4;if(!len||len>n-pos)break;int r=nal(p+pos,len,codec,&tmp);if(r<0)return 0;if(r>0){if(seen)return 0;found=tmp;seen=1;}pos+=len;}
 if(pos==n&&seen){*out=found;return 1;}
 return nal(p,n,codec,out)>0;
}
