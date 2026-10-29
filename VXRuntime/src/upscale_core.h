// The upscale core: a game's temporal upscaler (FSR2's dispatches, recognised by d3d12.m) replaced by one Apple
// scaler encode - MetalFX's temporal scaler (macOS / iOS only: the visionOS SDK marks it API_UNAVAILABLE(visionos),
// as it does the denoised scaler and the frame interpolator) or its spatial scaler (on visionOS too). Portable: Metal,
// MetalFX and Foundation only; it builds for macOS and visionOS (the temporal path is compiled out there).
//
// Per call (vxu_encode): the scaler for these formats and sizes (made once, again when they change), its output in
// an RGBA16Float texture of its own, then a compute copy into the game's output (any float/unorm format, a view too)
// over the display rectangle. Inputs the scaler does not take as they are (a depth-stencil depth, a colour format it
// rejects) are copied into ones it does, once per frame.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { VXU_TEMPORAL = 1, VXU_SPATIAL = 2 };

typedef struct {
    void *color, *depth, *motion;    // id<MTLTexture>, borrowed (depth and motion: VXU_TEMPORAL only)
    void *output;                    // id<MTLTexture>, borrowed: written over [0, out_w) x [0, out_h)
    uint32_t content_w, content_h;   // the rendered region at the top left of color/depth/motion (the render size)
    uint32_t out_w, out_h;           // the display size
    float jitter_x, jitter_y;        // the scaler's jitter offset (pixels of the render size)
    float mv_scale_x, mv_scale_y;    // motion texture value x scale = render pixels to the previous frame's position
    float pre_exposure;              // (0: 1)
    int reset;                       // history discarded (camera cut, first frame, size change)
    int depth_reversed;              // 1: near 1, far 0
    int auto_exposure;               // VXU_TEMPORAL: MetalFX's own exposure (else none: 1.0)
    int hdr;                         // VXU_SPATIAL: linear HDR colour (else perceptual)
    int sync_init;                   // VXU_TEMPORAL: the final upscaler compiled as the scaler is made (blocks), not
                                     // after it in the background (an interim, slower one meanwhile)
} vxu_frame;

// 1 when this device can run the mode (mode compiled in and supported)
int vxu_supported(void *device, int mode);
// Encodes the scaler and the copy into cb (no encoder may be open on it). 0 on success, else -1 with why filled in.
// The first success and every (re)creation of the scaler also put a line into why (note != 0).
int vxu_encode(void *command_buffer, int mode, const vxu_frame *f, char *why, size_t why_len, int *note);

#ifdef __cplusplus
}
#endif
