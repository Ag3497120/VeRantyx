# Standalone warp: low real fps, predicted frames, Vision Pro

Goal: Cyberpunk 2077 (Windows build, VXRuntime) running on Apple Vision Pro (M2) by itself. The game renders real frames at a
low, fixed rate (about 20-22.5 a second); every display refresh in between (90 Hz) gets a frame predicted from the
newest real one with the game's own motion vectors and depth. Stereo comes from depth (one render, two eyes), and
CompositorServices does the last few milliseconds of head motion and foveation itself.

This document is the contract between three pieces: the D3D12 layer's capture (macOS and visionOS alike), the warp
core (`VXRuntime/src/warp_core.m`, Metal only), and the visionOS app's frame loop.

## 1. Capture (D3D12 layer -> `npu_interp_scene`)

`VXRuntime/src/npu_interp.h` is the stable API (the MetalFX upscaler track and the visionOS app use it as is):

| Call | What | When |
|---|---|---|
| `npu_interp_wants_scene()` | 1 while capture is wanted | the D3D12 replay checks it per dispatch |
| `npu_interp_scene(mv, depth, cs, consts)` | the game's motion vectors (RG16Float/RG16Snorm/RG32Float, render size), its depth (Depth32Float(_Stencil8)/Depth16Unorm/R32Float, the same size), FSR2's constants (cbFSR2, 32 words) or NULL | every dispatch that reads both; the best of a frame is kept (FSR2's constants, then a real depth format; the first of the best) |
| `npu_interp_wants_scene_color(&w, &h)` / `npu_interp_scene_color(tex, cs)` | FSR2's output: the upscaled scene before any UI (a colour texture of the window's size written by a dispatch with FSR2's constants) | the HUD split; the last one of a frame |

All are textures the game owns; `npu_interp.m` copies them in the present's command buffer (after the frame's work on
the same queue): the motion vectors as they are, depth to R32Float, the scene colour as it is. At render size these
copies are small (756x491 at Low + FSR2 Performance in a 1512x982 window: 1.5 MB + 1.5 MB).

FSR2's constants are read as FSR 2.1/2.2 or 2.0 lay them out (`fsr2_parse`): display size, tan(vertical FOV/2),
device-to-view depth, motion vector scale (UV units per motion vector unit: B's pixel was at `uv + mv * scale` in the
frame before), jitter.

Cyberpunk 2077 (Low + FSR2 Performance, 1512x982 window), measured: FSR2's first pass (cs fe05c67c) reads the game's
motion vectors (756x491 RG16Float) and depth (756x491 Depth32Float_Stencil8); its constants (a CBV table's entry) are
FSR 2.0's layout: render 756x491, display 1512x982, luma mip 23x15, 1/display size, jitter [10], device-to-view
(FLT_EPSILON, -1, 0.1, FLT_EPSILON: reversed infinite, near plane 0.1), motion vector scale (1, 1) [22] (already UV
units), downscale 0.5 [24], tan(FOV/2) 0.889 [27] (83.3 degrees vertical). Depth: 52% of the frame exactly 0 (the
sky): reversed. A vote over sample points (which unit and signs match B to A) agrees with FSR2's scale (654 of 672).

## 2. The warp core (`warp_core.h`)

One call per output (per eye): `vxw_encode_eye(core, cb, &p, B, A, mv, depth, out, out_depth)`. Inputs:

- `B`: the frame to warp (FSR2's output when the HUD is split, else the final frame), `p.w x p.h`.
- `A`: the frame before (only for `VXW_STILL`, the old HUD guess; not used when the HUD is split).
- `mv`, `depth`: from the capture; `p.mv_sx = scale.x * w`, `p.mv_sy = scale.y * h`.
- `p.t`: how far on, in game frame intervals: `(display time - B's content time) / (B's content time - A's)`.
- projection: `vxw_projection_from_fsr2(&p, tan_half_fov_y, d2v)`.
- pose (`VXW_POSE`): `rot/pos` = the eye's camera from B's camera (view space); `dst_tan` = the eye's frustum
  (left, right, up, down tangents, as CompositorServices gives them); `dst_near` = the near plane the app tells
  CompositorServices (the depth out is reversed infinite: `dst_near / distance`).

Output: `out` (any size, any colour format; compute when it allows shader writes, else a render pass) and
`out_depth` (Depth32Float through a render pass's depth attachment, or R32Float by compute). Disocclusions take the
background's displacement from the nearest ring; nothing is ever black. Cost at 1920x1080: about 0.25 ms an eye on
the M1 Max GPU (`build/warp_core_test --bench`).

`make check` runs the synthetic tests (pose, parallax, motion vectors 1-5 intervals on, stereo, HUD, an asymmetric
eye with depth out); `make xros-check` compiles the core for visionOS and its simulator.

## 3. Pacing (`VXRT_NPU_REAL_FPS=N`, or `fpsN` in the switch file)

The game thread waits in Present until its next start on the refresh grid, every `k = round(refresh / N)` refreshes,
and that start is the frame's content time. A real frame is shown when the display reaches its content time + D,
where D is the latency envelope (content time to GPU done; up at once, down slowly) plus the decision margin, rounded up
to whole refreshes, so every real frame is in by its time and comes due exactly on a refresh. Every refresh in between
shows B predicted to its content time. The window's content then advances by exactly one refresh per refresh, whatever
each real frame's GPU time was (as long as it stays under D).

On Vision Pro: 22.5 real frames a second x 4 = 90 Hz (1 real + 3 predicted); 18 x 5 also fits.

## 4. HUD split

Measured at El Coyote (run18, pass dump of one frame): Cyberpunk's FSR2 output is RG11B10Float at 1512x982, HDR and
before tonemapping, and the UI is drawn into its own 1512x982 RGBA8 sRGB layer on black (a full-screen pass, then ~26
draws: minimap, quest text, subtitles, the health icon); one full-screen draw then composites tonemapped scene + UI layer
into the swap chain's RGBA8 buffer. So:

- "final minus FSR2's output" cannot isolate the UI here (another colour space, post-processing in between); the layer
  measures it (`hud` in the switch file: share of the final frame equal to FSR2's output, every 300 frames) and found
  0.0%, so that split stays off by itself.
- The clean split is the game's own UI layer: capture the colour input of the final composite pass that is a
  window-size RGBA8 texture drawn by more than one draw this frame (the UI layer), with its alpha. Then
  `scene = (final - UI.rgb) / (1 - UI.a)` where the UI is not opaque (opaque UI pixels: holes for the warp core's
  fill), warp the scene alone, and put the newest UI back on unwarped: `out = UI.rgb + (1 - UI.a) * warp(scene)`.
  No guessing where the HUD is. (Next round; the capture point is a render pass's colour input, not a dispatch's.)

Until then the old guess stays: blocks equal in A and B are held (`VXW_STILL`).

## 5. The Neural Engine

Two uses, neither in the per-refresh path's critical latency:

1. Disocclusion inpainting (prototype: `VXRuntime/tools/ane/inpaint.py`): a ~70k-weight U-Net fills the warp's holes
   (only the holes: composited where the core found nothing landed), at the core's block grid (half resolution). Trained
   offline on recorded real frames with synthetic disocclusion-shaped holes, converted to Core ML (mlprogram, fp16).
   Runs once per predicted frame on the ANE while the GPU warps the next eye; the holes' colour is a frame late at most.
2. Motion extrapolation (design): from the last 3 motion vector fields (render size, recorded by
   `VXRT_NPU_INTERP_RECORD` as `fNNNNN_mv_WxH.rg16f`), predict the next field's change (acceleration), so predictions 3-4
   refreshes on follow curved motion (turning while driving) instead of a straight line. Input 3 x 2 channels + depth at
   quarter render size, output 2 channels; trained on recorded driving clips (self-supervised: the next real frame's
   vectors are the label). Needs recordings of big motion (walking, driving), which the user drives to.

visionOS has no VTOpticalFlow or MTLFXFrameInterpolator; it has Core ML, Metal, CompositorServices, ARKit and
VTLowLatencyFrameInterpolation. The game's own motion vectors make the optical flow unnecessary: in race mode the flow's
helper process starts only after 3 s without motion vectors (menus, a game without FSR2) and never when they come.
