#include "pyro_carrier.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){
 struct MvrAgreement a={0};a.codec=2;a.path=2;a.features=3;a.session=123;a.revision=1;a.descriptorDigest=a.ackDigest=456;
 unsigned char bytes[1400],copy[1400];CHECK(mvrPyroWrite(bytes,sizeof bytes,&a,987,2799));CHECK(mvrPyroRead(bytes,sizeof bytes,&a,987,2799));memcpy(copy,bytes,sizeof copy);
 for(unsigned i=0;i<1400;i++){bytes[i]^=1;CHECK(!mvrPyroRead(bytes,sizeof bytes,&a,987,2799));bytes[i]^=1;}
 CHECK(!mvrPyroRead(bytes,1399,&a,987,2799));CHECK(!mvrPyroRead(bytes,1401,&a,987,2799));CHECK(!mvrPyroRead(bytes,1400,&a,988,2799));CHECK(!mvrPyroRead(bytes,1400,&a,987,2800));
 struct MvrAgreement wrong=a;wrong.session++;CHECK(!mvrPyroRead(bytes,1400,&wrong,987,2799));
 CHECK(!mvrPyroWrite(bytes,1399,&a,987,2799));CHECK(!mvrPyroWrite(bytes,1400,&a,987,0));CHECK(!mvrPyroWrite(bytes,1400,&a,987,MVR_PYRO_MAX_FRAME+1));
 for(unsigned size=1;size<12000;size+=17){
  CHECK(mvrPyroWrite(bytes,1400,&a,987,size));unsigned total=size+1400;unsigned count=(total+1399)/1400;
  for(unsigned i=0;i<count;i++){
   struct MvrPyroShard h={42,(unsigned short)i,(unsigned short)count,total,73,987};size_t n=total-i*1400;if(n>1400)n=1400;
   int r=mvrPyroNormalize(&h,bytes,n,&a);CHECK(r==(i?2:1));CHECK(h.frame==42&&h.flags==73&&h.pts==987);
   if(i)CHECK(h.index==i-1&&h.count==count-1&&h.bytes==size);
  }
 }
 struct MvrPyroShard bad={0,0,0,0,0,0};CHECK(!mvrPyroNormalize(&bad,bytes,1400,&a));bad.bytes=0xffffffffu;CHECK(!mvrPyroNormalize(&bad,bytes,1400,&a));
 printf("PASS: %u Pyro carrier and real shard-normalization checks\n",checks);return 0;
}
