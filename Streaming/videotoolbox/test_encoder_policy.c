#include "encoder_policy.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static unsigned calls;
static CFStringRef fail_key;
static CFArrayRef limits;
static OSStatus setter(VTSessionRef session, CFStringRef key, CFTypeRef value) {
    assert(session); ++calls;
    if (CFEqual(key,kVTCompressionPropertyKey_DataRateLimits)) assert(value == limits);
    if (CFEqual(key,kVTCompressionPropertyKey_BaseLayerFrameRate)) {
        double fps=0;assert(CFNumberGetValue(value,kCFNumberDoubleType,&fps) && fps==36);
    }
    if (fail_key && CFEqual(key,fail_key)) return -12345;
    return noErr;
}
int main(void) {
    CFDictionaryRef spec=vrs_encoder_specification(kCMVideoCodecType_H264,true);assert(spec);
    assert(CFDictionaryGetValue(spec,kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder)==kCFBooleanTrue);
    assert(CFDictionaryGetValue(spec,kVTVideoEncoderSpecification_EnableLowLatencyRateControl)==kCFBooleanFalse);CFRelease(spec);
    spec=vrs_encoder_specification(kCMVideoCodecType_HEVC,false);assert(spec);
    assert(!CFDictionaryContainsKey(spec,kVTVideoEncoderSpecification_EncoderID));CFRelease(spec);
    VTSessionRef session=(VTSessionRef)(uintptr_t)1;
    assert(vrs_apply_throughput(session,kCMVideoCodecType_H264,setter)==0 && calls==4);
    calls=0;fail_key=kVTCompressionPropertyKey_AllowFrameReordering;
    assert(vrs_apply_throughput(session,kCMVideoCodecType_HEVC,setter)==-12345 && calls==3);
    fail_key=NULL;calls=0;assert(vrs_apply_temporal_hevc(session,72,setter)==0 && calls==7);
    calls=0;assert(vrs_apply_temporal_hevc(session,NAN,setter)==paramErr && calls==0);
    double bytes=125000,seconds=1;CFNumberRef a=CFNumberCreate(NULL,kCFNumberDoubleType,&bytes),b=CFNumberCreate(NULL,kCFNumberDoubleType,&seconds);
    const void *values[]={a,b};limits=CFArrayCreate(NULL,values,2,&kCFTypeArrayCallBacks);
    fail_key=kVTCompressionPropertyKey_DataRateLimits;assert(vrs_set_data_rate_limits(session,limits,setter)==-12345);
    CFRelease(limits);CFRelease(a);CFRelease(b);
    puts("VideoToolbox policy: hardware selection, explicit opt-ins, temporal contract, rate-limit forwarding and property failures passed");
}
