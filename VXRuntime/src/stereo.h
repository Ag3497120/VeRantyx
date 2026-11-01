// Stereo 3D from flat games (VXRT_STEREO=1): a left and a right eye made at present time from the presented picture
// and the scene's depth buffer (depth-image-based rendering), shown in the window (side by side, anaglyph, one eye)
// and/or streamed side by side to the Vision Pro through the Mac encoder's shared frame file. See stereo.m.
#pragma once
#include <stdint.h>

// VXRT_STEREO=1 (read once). Off: none of the calls below are made.
int vx_stereo_on(void);
// A render pass with a depth attachment ended (on the thread that replays it, `cb` the command buffer its encoder
// was in): `draws` depth-tested draws, `writes` of them writing depth, `funcs` a bit per D3D12_COMPARISON_FUNC of
// the depth-writing draws. The scene's depth is copied here, at the end of each pass that wrote it.
void vx_stereo_depth_pass(void *cb, void *depth_texture, uint32_t draws, uint32_t writes, uint32_t funcs);
// A present of `back` (its top-left `w` x `h` is the picture), before the window's copy is committed on `queue`:
// encodes the stereo pass in a command buffer of its own (committed here, so before the window's). Returns the
// texture the window shows instead of the back buffer (`w` x `h`, all of it), or NULL to show the back buffer.
void *vx_stereo_present(void *queue, void *back, uint32_t w, uint32_t h);
