// Minimal Metal executor for API-neutral GPU IR. Vulkan and ordinary backend tests can share this path.
#ifndef VX_GPU_IR_METAL_H
#define VX_GPU_IR_METAL_H

#include "gpu_ir.h"

// Metal reserves one of its 31 compute buffer slots for backend runtime data: the
// SPIR-V robust-buffer-access ranges and, when present, Vulkan push constants.
#define GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS 30u
#define GPU_IR_METAL_MAX_COMPUTE_STORAGE_IMAGES 4u
#define GPU_IR_METAL_MAX_COMPUTE_SAMPLED_IMAGES 16u
#define GPU_IR_METAL_MAX_COMPUTE_TEXTURES \
    (GPU_IR_METAL_MAX_COMPUTE_SAMPLED_IMAGES + GPU_IR_METAL_MAX_COMPUTE_STORAGE_IMAGES)
#define GPU_IR_METAL_MAX_COMPUTE_SAMPLERS 16u
#define GPU_IR_METAL_MAX_ANISOTROPY 16u
#define GPU_IR_METAL_BUFFER_RANGE_SLOT 30u
#define GPU_IR_METAL_VERTEX_BUFFER_BASE 14u
#define GPU_IR_METAL_MAX_PUSH_CONSTANT_SIZE 128u
#define GPU_IR_METAL_RUNTIME_DATA_SIZE \
    (GPU_IR_METAL_MAX_PUSH_CONSTANT_SIZE + GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS * 8u)

typedef struct gpu_ir_metal_device gpu_ir_metal_device;

typedef struct {
    uint32_t min_filter, mag_filter, mip_filter;
    uint32_t address_u, address_v, address_w;
    uint32_t border_color;
    uint32_t normalized_coordinates;
    float max_anisotropy;
    float lod_bias, min_lod, max_lod;
} gpu_ir_metal_sampler_desc;

gpu_ir_metal_device *gpu_ir_metal_device_create(void);
// Wraps a borrowed MTLDevice in the common backend; the returned object retains the device.
gpu_ir_metal_device *gpu_ir_metal_device_create_for_metal_device(void *metal_device);
void gpu_ir_metal_device_release(gpu_ir_metal_device *device);
void *gpu_ir_metal_native_device(const gpu_ir_metal_device *device);
const char *gpu_ir_metal_device_name(const gpu_ir_metal_device *device);
uint64_t gpu_ir_metal_max_buffer_length(const gpu_ir_metal_device *device);
uint64_t gpu_ir_metal_max_threadgroup_memory_length(const gpu_ir_metal_device *device);
uint32_t gpu_ir_metal_max_fragment_input_components(const gpu_ir_metal_device *device);
uint64_t gpu_ir_metal_recommended_working_set_size(const gpu_ir_metal_device *device);
int gpu_ir_metal_has_unified_memory(const gpu_ir_metal_device *device);
uint64_t gpu_ir_metal_linear_texture_alignment(gpu_ir_metal_device *device, uint32_t format);
uint64_t gpu_ir_metal_linear_texture_row_pitch(gpu_ir_metal_device *device, uint32_t format, uint32_t width);
int gpu_ir_metal_storage_texture_supported(gpu_ir_metal_device *device, uint32_t format);
int gpu_ir_metal_color_attachment_supported(gpu_ir_metal_device *device, uint32_t format,
                                           gpu_ir_clear_color_value_type clear_value_type);
uint32_t gpu_ir_metal_sampler_max_anisotropy(gpu_ir_metal_device *device);
void *gpu_ir_metal_sampler_create(gpu_ir_metal_device *device, const gpu_ir_metal_sampler_desc *description);
void gpu_ir_metal_sampler_retain(void *sampler);
void gpu_ir_metal_sampler_release(void *sampler);
// Creates an owned Metal texture view whose texel reads use the API-neutral GPU IR component mapping.
// The caller releases the returned object with gpu_ir_metal_sampler_release-compatible bridging release.
void *gpu_ir_metal_texture_view_with_component_mapping(void *metal_texture, uint32_t component_mapping);
void gpu_ir_metal_texture_view_release(void *metal_texture_view);
void gpu_ir_metal_max_threads_per_threadgroup(const gpu_ir_metal_device *device, uint32_t size[3]);
gpu_ir_resource *gpu_ir_metal_buffer_create(gpu_ir_metal_device *device, uint64_t size,
                                            const void *initial_data);
gpu_ir_resource *gpu_ir_metal_texture_create_from_buffer(gpu_ir_metal_device *device,
    gpu_ir_resource *buffer, uint64_t offset, uint64_t row_pitch, const gpu_ir_texture_desc *description,
    int shader_read, int shader_write);
void *gpu_ir_metal_buffer_contents(const gpu_ir_resource *resource);
gpu_ir_pipeline *gpu_ir_metal_compute_pipeline_create(gpu_ir_metal_device *device,
                                                      const gpu_ir_shader *shader,
                                                      gpu_ir_pipeline_layout *layout,
                                                      char *error, size_t error_capacity);
// Compiles through the supplied borrowed MTLDevice pointer and returns a pipeline retaining its Metal state.
// This lets API front ends use the common shader backend with the exact device they created their other PSOs on.
gpu_ir_pipeline *gpu_ir_metal_compute_pipeline_create_for_metal_device(void *metal_device,
                                                                       const gpu_ir_shader *shader,
                                                                       gpu_ir_pipeline_layout *layout,
                                                                       char *error, size_t error_capacity);
// Creates a single-color graphics pipeline from backend MSL entry points named vx_vertex and vx_fragment.
// This is the backend boundary used by ordinary raster tests while API shader front ends are being connected.
gpu_ir_pipeline *gpu_ir_metal_graphics_pipeline_create(gpu_ir_metal_device *device,
    const char *vertex_source, const char *fragment_source, uint32_t color_format,
    char *error, size_t error_capacity);
gpu_ir_pipeline *gpu_ir_metal_graphics_pipeline_create_with_vertex_input(gpu_ir_metal_device *device,
    const char *vertex_source, const char *fragment_source, uint32_t color_format,
    const gpu_ir_vertex_input_desc *vertex_input, char *error, size_t error_capacity);
gpu_ir_pipeline *gpu_ir_metal_graphics_pipeline_create_with_layout(gpu_ir_metal_device *device,
    const char *vertex_source, const char *fragment_source, uint32_t color_format,
    const gpu_ir_vertex_input_desc *vertex_input, gpu_ir_pipeline_layout *layout,
    char *error, size_t error_capacity);
gpu_ir_pipeline *gpu_ir_metal_graphics_pipeline_create_with_blend(gpu_ir_metal_device *device,
    const char *vertex_source, const char *fragment_source, uint32_t color_format,
    const gpu_ir_vertex_input_desc *vertex_input, const gpu_ir_color_blend_state *color_blend,
    gpu_ir_pipeline_layout *layout, char *error, size_t error_capacity);
// Encode a common attachment clear into an existing Metal command buffer. The caller closes its active encoder
// before calling and preserves API command order around the resulting render pass.
int gpu_ir_metal_encode_clear_color(gpu_ir_metal_device *device, void *command_buffer,
                                    const gpu_ir_resource *texture,
                                    const gpu_ir_clear_color_info *clear);
int gpu_ir_metal_encode_clear_depth_stencil(gpu_ir_metal_device *device, void *command_buffer,
                                            const gpu_ir_resource *texture,
                                            const gpu_ir_clear_depth_stencil_info *clear);
int gpu_ir_metal_execute_stream(gpu_ir_metal_device *device, const gpu_ir_stream *stream);
// Present a BGRA8 common-IR texture through a caller-owned CAMetalLayer using the backend's Metal queue.
int gpu_ir_metal_present_texture(gpu_ir_metal_device *device, const gpu_ir_resource *texture,
                                 void *metal_layer, int display_sync_enabled);

#endif
