/* SPDX-License-Identifier: MIT
 * Pure size policy for the pinned native SGSR path. No GL/XR or decoder state.
 */
#ifndef MVR_SGSR_POLICY_H
#define MVR_SGSR_POLICY_H
#include <stdint.h>

#define MVR_SGSR_MAX_SBS_WIDTH 16384u
#define MVR_SGSR_MAX_HEIGHT 16384u
#define MVR_SGSR_MAX_OUTPUT_EYE_WIDTH 2048u

struct MvrSgsrSize {
    uint32_t eyeWidth;
    uint32_t height;
    uint32_t sbsWidth;
};

/* Owned by the caller, separately from the native packed-input cache key.
 * Set valid only after successful allocation and record the ACTUAL source and
 * presentation targets. Projection rectangles use presentation, never source
 * by assumption. Clear it when its swapchain/descriptor is replaced.
 */
struct MvrSgsrAllocation {
    uint64_t session;
    uint64_t revision;
    uint64_t descriptorDigest;
    struct MvrSgsrSize source;
    struct MvrSgsrSize presentation;
    unsigned sgsr;
    unsigned valid;
};

/* Failure leaves the destination untouched. Sources wider than 8192 per eye
 * cannot form an SBS image inside the supported 16384-pixel envelope.
 */
static inline int mvrSgsrSourceSize(uint32_t eyeWidth, uint32_t height,
                                    struct MvrSgsrSize* out) {
    struct MvrSgsrSize value;
    if (!out || !eyeWidth || !height || height > MVR_SGSR_MAX_HEIGHT ||
        (uint64_t)eyeWidth * 2u > MVR_SGSR_MAX_SBS_WIDTH) return 0;
    value.eyeWidth = eyeWidth;
    value.height = height;
    value.sbsWidth = eyeWidth * 2u;
    *out = value;
    return 1;
}

/* Match the native STRICT 95% threshold using LOGICAL source height, not the
 * packed decoder height. Invalid/unavailable target dimensions are ineligible.
 * Flag arguments retain native nonzero/zero boolean semantics.
 */
static inline int mvrSgsrShouldUpscale(int requested, int programReady,
                                      int freeTier, uint32_t sourceHeight,
                                      uint32_t recommendedHeight) {
    return requested && programReady && !freeTier && sourceHeight &&
           recommendedHeight && sourceHeight <= MVR_SGSR_MAX_HEIGHT &&
           recommendedHeight <= MVR_SGSR_MAX_HEIGHT &&
           (uint64_t)sourceHeight * 100u <
               (uint64_t)recommendedHeight * 95u;
}

/* Calculate the native output policy AFTER eligibility was established.
 * Keep both integer divisions: replacing the capped-height calculation with
 * 2048*sourceHeight/sourceEyeWidth can differ by a row.
 * An active SGSR branch does not guarantee larger output: the native width
 * cap can yield a 1:1 or smaller target for an unusually wide source.
 */
static inline int mvrSgsrOutputSize(uint32_t sourceEyeWidth,
                                   uint32_t sourceHeight,
                                   uint32_t recommendedHeight,
                                   struct MvrSgsrSize* out) {
    struct MvrSgsrSize source;
    struct MvrSgsrSize value;
    uint64_t eyeWidth, height;
    if (!out || !mvrSgsrSourceSize(sourceEyeWidth, sourceHeight, &source) ||
        !recommendedHeight || recommendedHeight > MVR_SGSR_MAX_HEIGHT) return 0;
    eyeWidth = (uint64_t)recommendedHeight * sourceEyeWidth / sourceHeight;
    height = recommendedHeight;
    if (!eyeWidth) return 0;
    if (eyeWidth > MVR_SGSR_MAX_OUTPUT_EYE_WIDTH) {
        height = (uint64_t)recommendedHeight * MVR_SGSR_MAX_OUTPUT_EYE_WIDTH /
                 eyeWidth;
        eyeWidth = MVR_SGSR_MAX_OUTPUT_EYE_WIDTH;
    }
    if (!height || height > MVR_SGSR_MAX_HEIGHT ||
        eyeWidth * 2u > MVR_SGSR_MAX_SBS_WIDTH) return 0;
    value.eyeWidth = (uint32_t)eyeWidth;
    value.height = (uint32_t)height;
    value.sbsWidth = (uint32_t)(eyeWidth * 2u);
    *out = value;
    return 1;
}

/* Combined selection: feature OFF, unavailable SGSR, free-tier, or a source
 * already reaching the native threshold keeps source geometry EXACTLY.
 * Neither output is modified on invalid source/impossible target geometry.
 */
static inline int mvrSgsrSelectSize(int requested, int programReady, int freeTier,
                                   uint32_t sourceEyeWidth,
                                   uint32_t sourceHeight,
                                   uint32_t recommendedHeight,
                                   struct MvrSgsrSize* out,
                                   unsigned* active) {
    struct MvrSgsrSize value;
    unsigned use;
    if (!out || !mvrSgsrSourceSize(sourceEyeWidth, sourceHeight, &value)) return 0;
    use = (unsigned)!!mvrSgsrShouldUpscale(requested, programReady, freeTier,
                                        sourceHeight, recommendedHeight);
    if (use && !mvrSgsrOutputSize(sourceEyeWidth, sourceHeight,
                                 recommendedHeight, &value)) return 0;
    *out = value;
    if (active) *active = use;
    return 1;
}
#endif
