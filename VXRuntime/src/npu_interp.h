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
