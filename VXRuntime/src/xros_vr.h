// The immersive presenter on Vision Pro (src/xros_vr.m; VISIONOS_PORT.md, "VR presentation"). A flat game's D3D12
// swap chain shown in an ImmersiveSpace through CompositorServices instead of a window: each present's picture (and
// the frame's depth and motion vectors when the D3D12 layer finds them) goes into one of three slots with the head pose
// it is shown from; the app's compositor loop (xros/app, VXImmersive.swift) warps the newest slot into each eye on
// every compositor frame (src/warp_core.h: one warp per eye with the eye's real pose and frustum, the game's depth
// giving the stereo and the head-rotation reprojection, its motion vectors the NPU race's predictions to the frame's
// display time) and writes depth for the system's own late reprojection. The game's rate is its own; Present's pacing
// comes from the compositor's frames.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The app turns it on before the program starts (launch argument -present vr, the default); 0: the flat window.
void vx_xros_vr_enable(int on);
int vx_xros_vr_on(void);

// ---- the game's side (src/d3d12.m) ----
// A render pass with a depth attachment ended (cb: its command buffer): as stereo.h's vx_stereo_depth_pass.
void vx_xros_vr_depth_pass(void *cb, void *depth_texture, uint32_t draws, uint32_t writes, uint32_t funcs);
// A present: the back buffer's top-left w x h (and this frame's depth or scene) copied into a slot in cb, which the
// caller commits; waitable_release (or NULL) is called with arg once the GPU has copied it.
void vx_xros_vr_submit(void *cb, void *back, uint32_t w, uint32_t h, void (*waitable_release)(void *), void *arg);

// ---- the compositor loop's side (the app) ----
// Each compositor frame, first: the head's pose for this frame (origin from device, column-major 4x4, ARKit's
// right-up-back axes) and its predicted presentation time (CACurrentMediaTime's clock). Also the tick that paces
// sync-interval presents (src/xros_ui.m). Returns 1 when a game frame is there to show.
int vx_xros_vr_begin_frame(const float origin_from_device[16], double presentation_time);
// One eye: the warp of the frame into out (the drawable's colour texture for that view) and out_depth (its depth
// texture), encoded in cb. view_from_origin: the eye's view matrix (inverse of origin-from-device x the view's
// transform), tan: the eye's frustum tangents (left, right, top, bottom, all positive outwards), near_m: the near
// plane the drawable's depth range says (reversed infinite depth: near / distance). 0: nothing encoded (no frame yet).
int vx_xros_vr_draw_eye(void *cb, int eye, const float view_from_origin[16], const float tan[4], float near_m, void *out, void *out_depth);
// After the eyes: cb's completion lets the slot be written again (NULL: the frame was cancelled, nothing encoded).
void vx_xros_vr_end_frame(void *cb);
// One line of JSON fields for the app's log (per second: compositor frames, game frames, warp GPU time, slot age...).
void vx_xros_vr_stats(char *out, unsigned size);

#ifdef __cplusplus
}
#endif
