// DXIL -> API-neutral shader IR. This adapter deliberately rejects any source instruction whose
// semantics are not represented by the common shader model; callers can keep using dxil_to_msl.
#ifndef VX_DXIL_IR_H
#define VX_DXIL_IR_H
#include "dxil.h"
#include "gpu_ir.h"

#define DXIL_IR_ENTRY_FUNCTION UINT32_MAX

// Lowers one defined DXIL function. The implementation covers scalar/vector core arithmetic and comparisons,
// selected float32 unary intrinsics and scalar/vector float classification, control flow, selected compute system values,
// compute WaveActiveAnyTrue/WaveActiveAllTrue votes and 32-bit WaveActiveBitAnd/Or/Xor reductions, DXIL barriers and
// 32-bit bit intrinsics (scalar/vector Bfrev, Countbits, FirstbitLo, FirstbitHi, FirstbitSHi; scalar Ibfe, Ubfe, Bfi), and typed
// R32_SINT/R32_UINT/R32_FLOAT buffer SRV/UAV load, store and GetDimensions operations plus 32-bit
// ByteAddressBuffer.Load result fields 0–3 and RawBufferLoad (opcode 139, four-byte alignment) through bounded
// word-index access. It also lowers DXIL
// Integer AtomicBinOp codes and AtomicCompareExchange on a single R32_SINT/R32_UINT typed-buffer UAV to common
// device-scope, relaxed integer atomics. A fragment shader that reads scalar components from one
// metadata-described float32 SV_Position element (with the non-geometry vertex-axis operand undef) and writes
// all four float32 components of one SV_Target0 element in one basic block lowers to common fragment I/O. A
// vertex shader that writes all four float32 components of one metadata-described SV_Position element in one
// basic block lowers to common vertex output.
// Other resource shapes, input semantics and graphics signature forms return GPU_IR_UNSUPPORTED.
// Pass DXIL_IR_ENTRY_FUNCTION to select dx.entryPoints[0], with the first defined function as fallback.
int dxil_build_gpu_ir_shader(const dxil_module *module, uint32_t function_index, gpu_ir_shader **shader);

#endif
