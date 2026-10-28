// The messages between the runtime's NPU mode (npu_interp.m) and its interpolator process (tools/vxrt_npu.m), on a
// socket that is the helper's descriptor 3. Each request starts with its op; every request has one reply.
#pragma once
#include <stdint.h>

enum { NPU_FD = 3, NPU_MAX_TILES = 4, NPU_MAX_SLOTS = 8, NPU_OP_INIT = 1, NPU_OP_PROC = 2 };
enum { NPU_MODE_INTERPOLATE = 1, NPU_MODE_FLOW = 2 };

// INIT, mode NPU_MODE_INTERPOLATE: one frame interpolation session per tile of tile_w x tile_h (width a multiple of
// 16), and src_slots + dst_slots 420v buffers per tile.
// mode NPU_MODE_FLOW: one optical flow session for frames of tile_w x tile_h (tiles 1), src_slots RGhA (RGBA half
// float) frames and dst_slots pairs of flows (2C0h: two half floats, flow_w x flow_h, covering the whole frame).
typedef struct { uint32_t op, tile_w, tile_h, tiles, src_slots, dst_slots, mode; } npu_init_req;
// status 0 = ready; ids: the IOSurface IDs, src slot by slot (each slot's tiles in order), then dst likewise; for
// NPU_MODE_FLOW the dst slots' forward flows (src -> next frame), then their backward flows.
// flow_scale: frame pixels per unit of a flow vector (measured at INIT on a known shift).
typedef struct { int32_t status; float start_ms; uint32_t flow_w, flow_h; float flow_scale; uint32_t ids[2 * NPU_MAX_SLOTS * NPU_MAX_TILES]; } npu_init_reply;
// PROC: dst slot := the midpoint of src slots prev and cur, or the flows between them (seq: the frame's number).
typedef struct { uint32_t op, seq, prev, cur, dst; } npu_proc_req;
typedef struct { uint32_t seq; int32_t status; float ms; } npu_proc_reply;
