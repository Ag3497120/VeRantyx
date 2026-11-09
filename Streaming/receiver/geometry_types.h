// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <stddef.h>
#define MVR_DESCRIPTOR_VERSION 1u
#define MVR_CROP 1u
#define MVR_FFE 2u
#define MVR_CODEC_H264 0u
#define MVR_CODEC_HEVC 1u
#define MVR_CODEC_PYRO 2u
/* Features unsupported by this first adapter must be rejected, not guessed. */
#define MVR_PATH_EXTERNAL_OES 1u
#define MVR_PATH_PLANAR_YUV 2u
#define MVR_PATH_SGSR 4u
#define MVR_PATH_FRAMEGEN 8u
struct MvrAxis { float sourcePixels,packedPixels,sourceBegin,sourceEnd,packedBegin,packedEnd,pad0,pad1; };
struct MvrEyeGpu { struct MvrAxis x,y; };
struct MvrFov { float left,right,up,down; };
struct MvrAgreement {
 uint32_t version,features,codec,path;
 uint64_t session,revision,descriptorDigest,ackDigest;
 uint32_t sourceWidth,sourceHeight,videoWidth,videoHeight;
 uint32_t textureYIsTop, reserved;
 struct MvrEyeGpu eye[2];
 struct MvrFov renderFov[2];
};
