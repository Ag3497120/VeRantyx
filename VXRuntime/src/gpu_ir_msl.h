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

// Validates fragment user-location inputs against matching vertex outputs. The current graphics subset supports
// smooth float32 scalar/vector varyings at component/index zero; other interpolation and interface modes are rejected.
int gpu_ir_msl_validate_graphics_link(const gpu_ir_shader *vertex_shader,
                                      const gpu_ir_shader *fragment_shader,
                                      uint32_t max_vertex_output_components,
                                      uint32_t max_fragment_input_components);

// Emits a deliberately narrow graphics-stage subset. Vertex shaders accept VertexIndex / InstanceIndex built-ins,
// Position, and smooth float32 scalar/vector user outputs; fragment shaders accept FragCoord / FrontFacing, matching
// user inputs, and one float4 Location 0 output. User vertex attributes and other interface state remain unsupported.
int gpu_ir_msl_emit_vertex(const gpu_ir_shader *shader, char **source, size_t *source_size);
int gpu_ir_msl_emit_fragment(const gpu_ir_shader *shader, char **source, size_t *source_size);
// Layout-aware graphics variants map shader-visible descriptors to dense per-stage Metal resource slots.
int gpu_ir_msl_emit_vertex_with_layout(const gpu_ir_shader *shader,
                                      const gpu_ir_pipeline_layout *layout,
                                      char **source, size_t *source_size);
int gpu_ir_msl_emit_fragment_with_layout(const gpu_ir_shader *shader,
                                         const gpu_ir_pipeline_layout *layout,
                                         char **source, size_t *source_size);

#endif
