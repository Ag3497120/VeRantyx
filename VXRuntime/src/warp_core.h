// The warp core: one rendered frame B moved to another time and/or view on the GPU (Metal compute and one draw), from
// B's own motion vectors and depth: asynchronous-spacewarp-style prediction (B's pixels moved on by t of their motion),
// reprojection to a newer camera pose (depth + projection), and a stereo eye (stereo.m's DIBR disparity) - all three
// in one warp, so VR costs one warp per eye. Portable: Metal and Foundation only (no VideoToolbox, AppKit or
// CoreAnimation), it builds for macOS and visionOS (CompositorServices' textures are plain MTLTextures).
//
// Per call (vxw_encode), at half resolution (2x2 blocks of the frame; M2-class GPUs: 3 compute passes and a fill):
//   1. map: each block of B: where it lands (B's motion x t, then the pose, then the eye's disparity) and how near it
//      is; the nearest of the blocks landing on a place wins it (an atomic max of nearness | block index). Blocks that
//      are the same in the previous frame A (HUD, subtitles: optional) are held where they are.
//   2. resolve: each block of the output takes the displacement of the block that won it; a place none landed on (a
//      disocclusion) takes the farthest (background) displacement landed in the nearest ring around it (1, 2, 4, 8,
//      16 blocks out), so uncovered areas show the background, never black.
//   3. gather: each output pixel samples B (bilinear) its block's displacement back.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VXW_POSE = 1,         // rot/pos applied (else the identity: no unprojection)
    VXW_STEREO = 2,       // the eye's disparity added
    VXW_STILL = 4,        // blocks equal in A and B (within thr) are held (A must be given)
    VXW_AUTO_CONV = 8,    // the stereo screen plane from the depth's near end (vxw_encode_conv's state), not zc
    VXW_DEPTH_STD = 16,   // standard depth (near 0, far 1): turned around before z_a / (d - z_b)
};

typedef struct {
    uint32_t w, h;        // the frame (B and the output); the motion vectors and depth may be smaller (render size)
    uint32_t flags;
    float t;              // how far on, in the motion vectors' intervals (0: B's time)
    float mv_sx, mv_sy;   // frame pixels per motion vector unit, signs included: B's pixel was at p + mv * s in A
    float tan_x, tan_y;   // projection: view x = ndc.x * tan_x * z, y = ndc.y * tan_y * z (ndc y up), z the distance
    float z_a, z_b;       // depth to distance: z = z_a / (d - z_b) (FSR2's device-to-view factors; reversed infinite Z:
                          // z_b ~ 0, z_a the near plane; without them z_a 1, z_b 0: distances in near-plane units)
    float rot[9], pos[3]; // VXW_POSE: the target camera from B's: X' = rot * X + pos (row-major), view units
    float eye;            // VXW_STEREO: -0.5 left, +0.5 right (half the disparity each way)
    float S, pop;         // disparity at infinity (frame pixels, between the eyes), pop-out of the near end (x S)
    float zc;             // the screen plane's distance (no disparity there); VXW_AUTO_CONV: from the state instead
    float conv;           // VXW_AUTO_CONV: the screen plane relative to the fit (stereo.m's VXRT_STEREO_CONV)
    float thr;            // VXW_STILL: the largest per-channel difference that counts as unchanged
} vxw_params;

typedef struct vxw_core vxw_core;

// The pipelines (compiled from source at start: no metallib to ship). NULL if they did not compile.
vxw_core *vxw_create(void *mtl_device);
void vxw_destroy(vxw_core *c);
// Encodes the warp of B into out (w x h). out is written by a compute pass when it allows shader writes, else drawn
// to (a drawable's texture: a render pass, pipeline made for its pixel format). a: the previous frame (VXW_STILL),
// or NULL. mv: two-channel float; depth: R32Float (or a depth format). Returns 0 when nothing was encoded.
int vxw_encode(vxw_core *c, void *command_buffer, const vxw_params *p, void *b, void *a, void *mv, void *depth, void *out);
// VXW_AUTO_CONV: the near end of the depth range (the pct-th of 16x16 samples of inverse depth), smoothed over frames
// into the core's state; call once per real frame before the eyes' vxw_encode.
void vxw_encode_conv(vxw_core *c, void *command_buffer, const vxw_params *p, void *depth, float pct);
// The largest motion (frame pixels per interval, of blocks not held) the last completed map saw; -1 before one.
float vxw_last_motion(vxw_core *c);
// The view's projection from FSR2's constants as npu_interp.m reads them: tan(half the vertical FOV), the
// device-to-view factors (FSR2: view depth = [1] / (d - [0]); [2] tan_x, [3] tan_y). Fills tan_x/tan_y/z_a/z_b; 0 if implausible.
int vxw_projection_from_fsr2(vxw_params *p, float tan_half_fov_y, const float d2v[4]);

#ifdef __cplusplus
}
#endif
