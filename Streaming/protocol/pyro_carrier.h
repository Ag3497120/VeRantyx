/* SPDX-License-Identifier: MIT
 * Private paired transport envelope, NOT a PyroWave codec packet.
 * One 1400-byte MVS shard precedes the unmodified native framed bitstream.
 * Strip it before native reassembly. All integers are little endian.
 */
#ifndef MVR_PYRO_CARRIER_H
#define MVR_PYRO_CARRIER_H
#include "../receiver/geometry_types.h"
#include <string.h>
#define MVR_PYRO_PREFIX 1400u
#define MVR_PYRO_MAX_FRAME (32u*1024u*1024u)
struct MvrPyroMeta {unsigned char magic[8];uint32_t version,size;uint64_t pts,payloadBytes;struct MvrAgreement agreement;};
#ifdef __cplusplus
static_assert(sizeof(struct MvrPyroMeta)==264,"carrier ABI");
#else
_Static_assert(sizeof(struct MvrPyroMeta)==264,"carrier ABI");
#endif
static const unsigned char mvrPyroMagic[8]={'M','V','R','P','Y','R','0','1'};
static inline int mvrPyroWrite(unsigned char* out,size_t capacity,const struct MvrAgreement* a,uint64_t pts,size_t payload){
 if(!out||!a||capacity<MVR_PYRO_PREFIX||!payload||payload>MVR_PYRO_MAX_FRAME||a->codec!=2||a->path!=2||!a->session||a->ackDigest!=a->descriptorDigest)return 0;
 struct MvrPyroMeta m;memset(&m,0,sizeof m);memcpy(m.magic,mvrPyroMagic,8);m.version=1;m.size=sizeof m;m.pts=pts;m.payloadBytes=payload;m.agreement=*a;memset(out,0,MVR_PYRO_PREFIX);memcpy(out,&m,sizeof m);return 1;
}
static inline int mvrPyroRead(const void* data,size_t bytes,const struct MvrAgreement* expected,uint64_t pts,size_t payload){
 if(!data||!expected||bytes!=MVR_PYRO_PREFIX||!payload||payload>MVR_PYRO_MAX_FRAME)return 0;
 struct MvrPyroMeta m;memcpy(&m,data,sizeof m);
 if(memcmp(m.magic,mvrPyroMagic,8)||m.version!=1||m.size!=sizeof m||m.pts!=pts||m.payloadBytes!=payload||memcmp(&m.agreement,expected,sizeof *expected))return 0;
 const unsigned char* p=(const unsigned char*)data;for(size_t i=sizeof m;i<MVR_PYRO_PREFIX;i++)if(p[i])return 0;return 1;
}
struct MvrPyroShard {uint32_t frame;unsigned short index,count;uint32_t bytes,flags;uint64_t pts;};
/* Returns0 invalid,1 accepted metadata (do not forward),2 native video. */
static inline int mvrPyroNormalize(struct MvrPyroShard* h,const void* data,size_t size,const struct MvrAgreement* a){
 if(!h||!data||!a||h->bytes<=MVR_PYRO_PREFIX||h->bytes>MVR_PYRO_MAX_FRAME+MVR_PYRO_PREFIX||h->count!=(h->bytes+1399u)/1400u||h->index>=h->count)return 0;
 size_t remaining=h->bytes-(size_t)h->index*1400;
 if(size!=(remaining<1400?remaining:1400))return 0;
 if(!h->index)return mvrPyroRead(data,size,a,h->pts,h->bytes-MVR_PYRO_PREFIX)?1:0;
 h->index--;h->count--;h->bytes-=MVR_PYRO_PREFIX;return 2;
}
#endif
