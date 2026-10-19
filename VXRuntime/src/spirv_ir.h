// Adapter from SPIR-V reflection metadata to the shared, API-neutral GPU layout.
#ifndef VX_SPIRV_IR_H
#define VX_SPIRV_IR_H
#include "gpu_ir.h"
#include "spirv.h"

// Builds the supported descriptor and push-constant portions of a common layout for one selected entry point.
// Unsupported SPIR-V block types and Vulkan-specific layout/entry-point validation return GPU_IR_UNSUPPORTED.
int spirv_build_gpu_ir_layout(const spirv_module *module, const spirv_entry_point *entry_point,
                              gpu_ir_pipeline_layout **layout);

// Lowers the core type and executable subset understood by this front end into an owned, API-neutral shader.
// The selected entry point must belong to module; an unrepresented executable instruction is rejected.
int spirv_build_gpu_ir_shader(const spirv_module *module, const spirv_entry_point *entry_point,
                              gpu_ir_shader **shader);

#endif
