// Conservative Metal Shading Language emitter for the API-neutral shader IR.
#ifndef VX_GPU_IR_MSL_H
#define VX_GPU_IR_MSL_H
#include "gpu_ir.h"
#include <stddef.h>

// Emits a compute kernel named "vx_main". The current lowering deliberately supports a small,
// validated subset; unknown instructions or resource layouts return GPU_IR_UNSUPPORTED.
// On success, *source is malloc-owned by the caller and must be freed.
int gpu_ir_msl_emit_compute(const gpu_ir_shader *shader, char **source, size_t *source_size);

// Maps compute-visible scalar buffer bindings in the supplied layout to dense Metal buffer slots. Vulkan
// storage buffers and D3D12 read-only/storage resources (namespaces zero and one) are supported when count is one.
int gpu_ir_msl_emit_compute_with_layout(const gpu_ir_shader *shader,
                                        const gpu_ir_pipeline_layout *layout,
                                        char **source, size_t *source_size);

#endif
