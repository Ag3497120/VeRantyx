// SPDX-License-Identifier: MIT
#include "encoder_policy.h"
#include <math.h>

CFDictionaryRef vrs_encoder_specification(CMVideoCodecType codec, bool throughput) {
    if (codec != kCMVideoCodecType_H264 && codec != kCMVideoCodecType_HEVC) return NULL;
    CFMutableDictionaryRef spec = CFDictionaryCreateMutable(NULL, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!spec) return NULL;
    CFDictionarySetValue(spec, kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder, kCFBooleanTrue);
    if (codec == kCMVideoCodecType_H264)
        CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EncoderID,
            CFSTR("com.apple.videotoolbox.videoencoder.ave.avc"));
    if (throughput)
        CFDictionarySetValue(spec, kVTVideoEncoderSpecification_EnableLowLatencyRateControl, kCFBooleanFalse);
    return spec;
}
OSStatus vrs_require_hardware(VTCompressionSessionRef session) {
    if (!session) return paramErr;
    CFTypeRef value = NULL;
    OSStatus status = VTSessionCopyProperty(session,
        kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, NULL, &value);
    bool hardware = value == kCFBooleanTrue;
    if (value) CFRelease(value);
    return status ? status : (hardware ? noErr : kVTVideoEncoderNotAvailableNowErr);
}
OSStatus vrs_apply_throughput(VTSessionRef session, CMVideoCodecType codec, VRSSetProperty set) {
    if (!session || !set || (codec != kCMVideoCodecType_H264 && codec != kCMVideoCodecType_HEVC)) return paramErr;
    CFStringRef keys[] = { kVTCompressionPropertyKey_RealTime,
        kVTCompressionPropertyKey_AllowFrameReordering, kVTCompressionPropertyKey_MaximizePowerEfficiency };
    OSStatus first = noErr;
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        OSStatus status = set(session, keys[i], kCFBooleanFalse);
        if (!first && status) first = status;
    }
    if (codec == kCMVideoCodecType_H264) {
        OSStatus status = set(session, kVTCompressionPropertyKey_H264EntropyMode, kVTH264EntropyMode_CAVLC);
        if (!first && status) first = status;
    }
    return first;
}
OSStatus vrs_apply_temporal_hevc(VTSessionRef session, double fps, VRSSetProperty set) {
    if (!session || !set || !isfinite(fps) || fps < 24 || fps > 240) return paramErr;
    double half = fps/2;
    int layers = 2, gop = (int)ceil(fps);
    CFNumberRef base = CFNumberCreate(NULL, kCFNumberDoubleType, &half);
    CFNumberRef count = CFNumberCreate(NULL, kCFNumberIntType, &layers);
    CFNumberRef interval = CFNumberCreate(NULL, kCFNumberIntType, &gop);
    if (!base || !count || !interval) {
        if (base) CFRelease(base); if (count) CFRelease(count); if (interval) CFRelease(interval);
        return memFullErr;
    }
    CFStringRef keys[] = { kVTCompressionPropertyKey_AllowTemporalCompression,
        kVTCompressionPropertyKey_AllowOpenGOP, kVTCompressionPropertyKey_BaseLayerFrameRate,
        CFSTR("NumberOfTemporalLayers"), CFSTR("TemporalIDNestingFlag"),
        kVTCompressionPropertyKey_MaxKeyFrameInterval, kVTCompressionPropertyKey_AllowFrameReordering };
    CFTypeRef values[] = { kCFBooleanTrue, kCFBooleanFalse, base, count, kCFBooleanTrue, interval, kCFBooleanFalse };
    OSStatus first = noErr;
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        OSStatus status = set(session, keys[i], values[i]); if (!first && status) first = status;
    }
    CFRelease(base); CFRelease(count); CFRelease(interval);
    return first;
}
OSStatus vrs_set_data_rate_limits(VTSessionRef session, CFArrayRef limits, VRSSetProperty set) {
    if (!session || !set || !limits) return paramErr;
    return set(session, kVTCompressionPropertyKey_DataRateLimits, limits);
}
