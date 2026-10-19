// DXIL -> API-neutral shader IR. This adapter deliberately rejects any source instruction whose
// semantics are not represented by the common shader model; callers can keep using dxil_to_msl.
#ifndef VX_DXIL_IR_H
#define VX_DXIL_IR_H
#include "dxil.h"
#include "gpu_ir.h"

#define DXIL_IR_ENTRY_FUNCTION UINT32_MAX

// Lowers one defined DXIL function. The implementation covers scalar/vector core arithmetic, selected float32
// unary intrinsics, scalar float classification, control flow, selected compute system values, DXIL barriers and
// 32-bit scalar bit intrinsics (Bfrev, Countbits, FirstbitLo, FirstbitHi, FirstbitSHi, Ibfe, Ubfe, Bfi), and typed
// R32_SINT/R32_UINT/R32_FLOAT buffer SRV/UAV load, store and GetDimensions operations; other resource shapes and
// unsupported LLVM instructions return GPU_IR_UNSUPPORTED.
// Pass DXIL_IR_ENTRY_FUNCTION to select dx.entryPoints[0], with the first defined function as fallback.
int dxil_build_gpu_ir_shader(const dxil_module *module, uint32_t function_index, gpu_ir_shader **shader);

#endif
