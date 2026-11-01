// NPU mode (npu_interp.m): a flat game's presents shown with a frame made with the Neural Engine after each, so the
// window shows twice the game's frame rate: VXRT_NPU_INTERP=1 (or interpolate) the midpoint of each two (the real frame
// half an interval late), VXRT_NPU_INTERP=extrapolate one predicted from the last two (nothing held back). Off by
// default; headsets never see it.
#pragma once
#include <stdint.h>
#include "vx.h"

// 1 when presents on this CAMetalLayer go through npu_interp_frame (the caller then takes no drawable of its own)
int npu_interp_on(void *layer);
// The present of a back buffer texture (its w x h top-left region), encoded into the caller's MTLCommandBuffer,
// which this commits; waitable (or NULL) is released when the GPU has copied the frame.
void npu_interp_frame(void *layer, void *command_buffer, void *texture, uint32_t w, uint32_t h, HANDLE waitable);
// VXRT_NPU_INTERP=race: 1 while the scene's motion vectors and depth are wanted (the D3D12 front end then passes those of
// the dispatch that reads both, its upscaler's, with npu_interp_scene: MTLTextures, each frame)
int npu_interp_wants_scene(void);
// consts: the dispatch's FSR2 constants (32 words: render size, max render size, display size, ...), or NULL
//
// The capture API (stable: the upscaler track (MetalFX) and the visionOS app build on it):
//   - called from the D3D12 front end's replay, any thread, any number of times a frame; all three are MTLTextures the
//     game owns, valid until the game's next frame overwrites them (npu_interp.m copies them in the present's command
//     buffer, which runs after the frame's work on the same queue);
//   - motion_vectors: two-channel float (RG16Float/RG16Snorm/RG32Float), render size; depth: Depth32Float(_Stencil8),
//     Depth16Unorm or R32Float, the same size; consts as FSR2 lays out cbFSR2 (2.0, or 2.1/2.2: npu_interp.m's
//     fsr2_parse reads display size, tan(vertical FOV/2), device-to-view depth, motion vector scale, jitter);
//   - per frame the best candidate is kept: FSR2's constants found, then a depth texture; the first of those wins
//     (FSR2's first pass reads the game's own motion vectors and depth; its later ones its dilated copies).
void npu_interp_scene(void *motion_vectors, void *depth, uint64_t shader, const uint32_t *consts);
// The HUD apart ("hud" in the switch file): 1 (and the window's size) while the scene's colour before the UI is wanted;
// then the D3D12 front end passes FSR2's output (a colour texture of that size written by a dispatch with FSR2's
// constants: the upscaled scene, before any UI) with npu_interp_scene_color, the last of each frame
int npu_interp_wants_scene_color(uint32_t *w, uint32_t *h);
void npu_interp_scene_color(void *texture, uint64_t shader);
