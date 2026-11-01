// The warp core: one rendered frame B moved to another time and/or view on the GPU (Metal compute and one draw), from
// B's own motion vectors and depth: asynchronous-spacewarp-style prediction (B's pixels moved on by t of their motion),
// reprojection to a newer camera pose (depth + projection), and a stereo eye (a real eye pose and frustum, or stereo.m's
// DIBR disparity) - all in one warp, so VR costs one warp per eye. Portable: Metal and Foundation only (no
// VideoToolbox, AppKit or CoreAnimation), it builds for macOS, visionOS and the visionOS simulator (make xros-check).
//
// Per call (vxw_encode / vxw_encode_eye), at half resolution (2x2 blocks; M2-class GPUs: 3 compute passes, or 2 and a
// draw into a render-target-only texture):
//   1. map: each block of B: where it lands in the output (B's motion x t, then the pose and the target frustum, then
//      the eye's disparity) and how near it is; the nearest of the blocks landing on a place wins it (an atomic max of
//      nearness | block index). Blocks that are the same in the previous frame A (HUD, subtitles: optional) are held.
//   2. resolve: each block of the output takes the displacement of the block that won it; a place none landed on (a
//      disocclusion) takes the farthest (background) displacement landed in the nearest ring around it (1, 2, 4, 8,
//      16 blocks out), so uncovered areas show the background, never black.
//   3. gather: each output pixel samples B (bilinear) its block's displacement back; with a depth output, that block's
//      distance as depth.
//
// ---- The visionOS app's interface (what a CompositorServices frame loop calls) ------------------------------------
// Per real frame from the game (VXRuntime's D3D12 layer captures these: npu_interp.h, npu_interp_scene()):
//   colour   B: the game's frame (MTLTexture, any float/unorm colour format), w x h. Without the UI when the scene
//            is captured before it (see npu_interp.m, HUD); the latest UI is then composited by the app, unwarped.
//   depth    the scene's depth at render size (FSR2's input: Depth32Float(_Stencil8) or R32Float); reversed Z unless
//            VXW_DEPTH_STD; FSR2's device-to-view factors make it distance (vxw_projection_from_fsr2).
//   motion   the game's motion vectors at render size (RG16Float/RG32Float): B's pixel was at p + mv * (mv_sx, mv_sy)
//            in the frame before. With FSR2: mv_sx = fMotionVectorScale.x * w, mv_sy = fMotionVectorScale.y * h
//            (FSR2's scale is in UV units per motion vector unit).
//   FSR2     its constants (cbFSR2, 2.0-2.2 layouts; npu_interp.m fsr2_parse): tan(vertical FOV/2), device-to-view
//            depth (4 floats), motion vector scale, jitter. The colour the core gets is FSR2's output (unjittered); the
//            jitter matters only to an app that warps FSR2's render-size input instead (then subtract jitter / render
//            size from the sample position before the gather).
//   pose     the game camera's pose for B, and the pose (and frustum) each eye will be shown with: the difference goes
//            in rot/pos (X' = rot * X + pos, view space, B's camera to the eye's), dst_tan is the eye's frustum
//            (ar_view's tangents in CompositorServices: left, right, up, down).
// Per display frame (90 Hz on Vision Pro) and per eye:
//   vxw_params p = { w, h, VXW_STILL | VXW_POSE, t = (display time - B's content time) / the game's frame interval,
//                    mv_sx, mv_sy, projection (vxw_projection_from_fsr2), rot/pos (eye from B's camera), dst_tan,
//                    dst_near = the near plane CompositorServices is told (reversed infinite depth) };
//   vxw_encode_eye(core, cb, &p, B, A, mv, depth, drawable.colorTextures[eye], drawable.depthTextures[eye]);
// Output: each eye's colour in its own texture (any size: CompositorServices' per-eye textures; the gather scales),
// and its depth (reversed infinite, dst_near / distance, Depth32Float in a render pass or R32Float written by compute),
// so CompositorServices' own late reprojection (which needs depth) corrects the last few ms of head motion and its
// rasterization rate maps foveate. The app gets no gaze data and needs none.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VXW_POSE = 1,         // rot/pos applied (else the identity: no unprojection)
    VXW_STEREO = 2,       // the eye's disparity added (screen-space DIBR; with real eye poses use VXW_POSE instead)
    VXW_STILL = 4,        // blocks equal in A and B (within thr) are held (A must be given)
    VXW_AUTO_CONV = 8,    // the stereo screen plane from the depth's near end (vxw_encode_conv's state), not zc
    VXW_DEPTH_STD = 16,   // standard depth (near 0, far 1): turned around before z_a / (d - z_b)
};

typedef struct {
    uint32_t w, h;        // B's size (the motion vectors and depth may be smaller: render size); the output's size is
                          // its texture's
    uint32_t flags;
    float t;              // how far on, in the motion vectors' intervals (0: B's time)
    float mv_sx, mv_sy;   // frame pixels per motion vector unit, signs included: B's pixel was at p + mv * s in A
    float tan_x, tan_y;   // projection: view x = ndc.x * tan_x * z, y = ndc.y * tan_y * z (ndc y up), z the distance
    float z_a, z_b;       // depth to distance: z = z_a / (d - z_b) (FSR2's device-to-view factors; reversed infinite Z:
                          // z_b ~ 0, z_a the near plane; without them z_a 1, z_b 0: distances in near-plane units)
    float rot[9], pos[3]; // VXW_POSE: the target camera from B's: X' = rot * X + pos (row-major), view units
    float eye;            // VXW_STEREO: -0.5 left, +0.5 right (half the disparity each way)
    float S, pop;         // disparity at infinity (output pixels, between the eyes), pop-out of the near end (x S)
    float zc;             // the screen plane's distance (no disparity there); VXW_AUTO_CONV: from the state instead
    float conv;           // VXW_AUTO_CONV: the screen plane relative to the fit (stereo.m's VXRT_STEREO_CONV)
    float thr;            // VXW_STILL: the largest per-channel difference that counts as unchanged
    float dst_tan[4];     // VXW_POSE: the target's frustum, tangents left, right, up, down (all 0: B's, tan_x/tan_y)
    float dst_near;       // the depth output: dst_near / distance (reversed infinite); 0: B's own depth values
} vxw_params;

typedef struct vxw_core vxw_core;

// The pipelines (compiled from source at start: no metallib to ship). NULL if they did not compile.
vxw_core *vxw_create(void *mtl_device);
void vxw_destroy(vxw_core *c);
// Encodes the warp of B into out (any size). out is written by a compute pass when it allows shader writes, else drawn
// to (a drawable's texture: a render pass, pipeline made for its pixel format). a: the previous frame (VXW_STILL),
// or NULL. mv: two-channel float; depth: R32Float (or a depth format). Returns 0 when nothing was encoded.
int vxw_encode(vxw_core *c, void *command_buffer, const vxw_params *p, void *b, void *a, void *mv, void *depth, void *out);
// The same with the output's depth (out_depth: out's size; Depth32Float/Depth16Unorm: a render pass's depth attachment;
// R32Float with shader writes: by the compute gather, when out allows shader writes too), or NULL.
int vxw_encode_eye(vxw_core *c, void *command_buffer, const vxw_params *p, void *b, void *a, void *mv, void *depth, void *out,
                   void *out_depth);
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
