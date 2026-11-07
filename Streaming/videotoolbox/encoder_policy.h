// SPDX-License-Identifier: MIT
#pragma once
#include <VideoToolbox/VideoToolbox.h>
#ifdef __cplusplus
extern "C" {
#endif
// Explicit source-level helpers, never installed as process-wide interposers.
// Require hardware; throughput mode disables the low-latency rate-control selector.
// Caller owns the returned dictionary. Probe actual dimensions, do not assume
// that a dimension supported on one Mac is a universal codec/hardware limit.
CFDictionaryRef vrs_encoder_specification(CMVideoCodecType codec, bool throughput);
OSStatus vrs_require_hardware(VTCompressionSessionRef session);
typedef OSStatus (*VRSSetProperty)(VTSessionRef, CFStringRef, CFTypeRef);
// Opt-in throughput policy. RealTime=false is a tested alternative, not a
// blanket recommendation. A source integration must inspect all return codes.
OSStatus vrs_apply_throughput(VTSessionRef session, CMVideoCodecType codec, VRSSetProperty set);
// Experimental two-sublayer HEVC: only enable with a paired, negotiated receiver.
// The receiver must retain T0 dependencies and may discard only proven TSA T1.
OSStatus vrs_apply_temporal_hevc(VTSessionRef session, double fps, VRSSetProperty set);
// Keep caller-supplied byte/window limits, including after configure/reconnect.
OSStatus vrs_set_data_rate_limits(VTSessionRef session, CFArrayRef limits, VRSSetProperty set);
#ifdef __cplusplus
}
#endif
