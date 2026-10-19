// API-neutral resource references and ordered GPU operations.
// API front ends validate their own rules before recording; backends provide execution callbacks.
#ifndef VX_GPU_IR_H
#define VX_GPU_IR_H
#include <stddef.h>
#include <stdint.h>

typedef struct gpu_ir_resource gpu_ir_resource;
typedef struct gpu_ir_pipeline_layout gpu_ir_pipeline_layout;
typedef struct gpu_ir_pipeline gpu_ir_pipeline;
typedef struct gpu_ir_buffer_view gpu_ir_buffer_view;
typedef struct gpu_ir_descriptor_state gpu_ir_descriptor_state;
typedef struct gpu_ir_shader gpu_ir_shader;

enum {
    GPU_IR_STAGE_VERTEX = 1u << 0,
    GPU_IR_STAGE_TESS_CONTROL = 1u << 1,
    GPU_IR_STAGE_TESS_EVALUATION = 1u << 2,
    GPU_IR_STAGE_GEOMETRY = 1u << 3,
    GPU_IR_STAGE_FRAGMENT = 1u << 4,
    GPU_IR_STAGE_COMPUTE = 1u << 5,
    GPU_IR_STAGE_TASK = 1u << 6,
    GPU_IR_STAGE_MESH = 1u << 7,
    GPU_IR_STAGE_ALL = 0xffu
};

enum {
    GPU_IR_STAGE_SLOT_VERTEX,
    GPU_IR_STAGE_SLOT_TESS_CONTROL,
    GPU_IR_STAGE_SLOT_TESS_EVALUATION,
    GPU_IR_STAGE_SLOT_GEOMETRY,
    GPU_IR_STAGE_SLOT_FRAGMENT,
    GPU_IR_STAGE_SLOT_COMPUTE,
    GPU_IR_STAGE_SLOT_TASK,
    GPU_IR_STAGE_SLOT_MESH,
    GPU_IR_STAGE_SLOT_COUNT
};

enum {
    GPU_IR_DESCRIPTOR_UNIFORM_BUFFER = 1,
    GPU_IR_DESCRIPTOR_STORAGE_BUFFER,
    GPU_IR_DESCRIPTOR_SAMPLED_IMAGE,
    GPU_IR_DESCRIPTOR_STORAGE_IMAGE,
    GPU_IR_DESCRIPTOR_SAMPLER,
    GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER,
    GPU_IR_DESCRIPTOR_UNIFORM_TEXEL_BUFFER,
    GPU_IR_DESCRIPTOR_STORAGE_TEXEL_BUFFER,
    GPU_IR_DESCRIPTOR_INPUT_ATTACHMENT,
    GPU_IR_DESCRIPTOR_ACCELERATION_STRUCTURE,
    // General resource bindings whose concrete buffer/texture shape comes from the bound resource.
    GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE,
    GPU_IR_DESCRIPTOR_STORAGE_RESOURCE
};

#define GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED UINT32_MAX

// Reserved key for an adapter-owned binding used to lower API push constants to a backend buffer.
// Frontends must not expose this key as an application descriptor.
#define GPU_IR_INTERNAL_SET UINT32_MAX
#define GPU_IR_INTERNAL_BINDING_NAMESPACE UINT32_MAX
#define GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING UINT32_MAX

typedef struct {
    uint32_t set, binding, kind, count, stage_mask, access_mask;
    // Distinguishes register classes such as D3D12 SRV/UAV/CBV/sampler. Vulkan uses namespace zero.
    uint32_t binding_namespace;
} gpu_ir_descriptor_binding;

typedef struct {
    uint32_t offset, size, stage_mask;
} gpu_ir_push_constant_range;

typedef struct {
    const gpu_ir_descriptor_binding *bindings;
    uint32_t binding_count;
    const gpu_ir_push_constant_range *push_constants;
    uint32_t push_constant_count;
} gpu_ir_pipeline_layout_desc;

enum {
    GPU_IR_ACCESS_VERTEX_READ = 1u << 0,
    GPU_IR_ACCESS_INDEX_READ = 1u << 1,
    GPU_IR_ACCESS_UNIFORM_READ = 1u << 2,
    GPU_IR_ACCESS_SHADER_READ = 1u << 3,
    GPU_IR_ACCESS_SHADER_WRITE = 1u << 4,
    GPU_IR_ACCESS_COLOR_WRITE = 1u << 5,
    GPU_IR_ACCESS_DEPTH_READ = 1u << 6,
    GPU_IR_ACCESS_DEPTH_WRITE = 1u << 7,
    GPU_IR_ACCESS_STREAM_WRITE = 1u << 8,
    GPU_IR_ACCESS_INDIRECT_READ = 1u << 9,
    GPU_IR_ACCESS_COPY_READ = 1u << 10,
    GPU_IR_ACCESS_COPY_WRITE = 1u << 11,
    GPU_IR_ACCESS_RESOLVE_READ = 1u << 12,
    GPU_IR_ACCESS_RESOLVE_WRITE = 1u << 13,
    GPU_IR_ACCESS_ACCELERATION_STRUCTURE = 1u << 14,
    GPU_IR_ACCESS_INPUT_ATTACHMENT_READ = 1u << 15,
    GPU_IR_ACCESS_HOST_READ = 1u << 16,
    GPU_IR_ACCESS_HOST_WRITE = 1u << 17,
    GPU_IR_ACCESS_MEMORY_READ = 1u << 18,
    GPU_IR_ACCESS_MEMORY_WRITE = 1u << 19,
    GPU_IR_ACCESS_ACCELERATION_STRUCTURE_WRITE = 1u << 20,
    GPU_IR_ACCESS_COLOR_READ = 1u << 21,
    GPU_IR_ACCESS_ALL = 0x3fffffu
};

// Synchronization scopes use common operation names rather than API bit values. Front ends translate
// their stage masks into these bits and keep API-specific validation before recording.
#define GPU_IR_SYNC_STAGE_TOP_OF_PIPE                  (UINT64_C(1) << 0)
#define GPU_IR_SYNC_STAGE_DRAW_INDIRECT                (UINT64_C(1) << 1)
#define GPU_IR_SYNC_STAGE_VERTEX_INPUT                 (UINT64_C(1) << 2)
#define GPU_IR_SYNC_STAGE_VERTEX_SHADER                (UINT64_C(1) << 3)
#define GPU_IR_SYNC_STAGE_TESSELLATION_CONTROL_SHADER   (UINT64_C(1) << 4)
#define GPU_IR_SYNC_STAGE_TESSELLATION_EVALUATION_SHADER (UINT64_C(1) << 5)
#define GPU_IR_SYNC_STAGE_GEOMETRY_SHADER               (UINT64_C(1) << 6)
#define GPU_IR_SYNC_STAGE_FRAGMENT_SHADER               (UINT64_C(1) << 7)
#define GPU_IR_SYNC_STAGE_EARLY_FRAGMENT_TESTS           (UINT64_C(1) << 8)
#define GPU_IR_SYNC_STAGE_LATE_FRAGMENT_TESTS            (UINT64_C(1) << 9)
#define GPU_IR_SYNC_STAGE_COLOR_ATTACHMENT_OUTPUT        (UINT64_C(1) << 10)
#define GPU_IR_SYNC_STAGE_COMPUTE_SHADER                 (UINT64_C(1) << 11)
#define GPU_IR_SYNC_STAGE_BOTTOM_OF_PIPE                 (UINT64_C(1) << 13)
#define GPU_IR_SYNC_STAGE_HOST                            (UINT64_C(1) << 14)
#define GPU_IR_SYNC_STAGE_TASK_SHADER                     (UINT64_C(1) << 15)
#define GPU_IR_SYNC_STAGE_MESH_SHADER                     (UINT64_C(1) << 16)
#define GPU_IR_SYNC_STAGE_RAY_TRACING_SHADER              (UINT64_C(1) << 17)
#define GPU_IR_SYNC_STAGE_ACCELERATION_STRUCTURE_BUILD    (UINT64_C(1) << 18)
#define GPU_IR_SYNC_STAGE_COPY                            (UINT64_C(1) << 19)
#define GPU_IR_SYNC_STAGE_RESOLVE                         (UINT64_C(1) << 20)
#define GPU_IR_SYNC_STAGE_BLIT                            (UINT64_C(1) << 21)
#define GPU_IR_SYNC_STAGE_CLEAR                           (UINT64_C(1) << 22)
#define GPU_IR_SYNC_STAGE_INDEX_INPUT                     (UINT64_C(1) << 23)
#define GPU_IR_SYNC_STAGE_VERTEX_ATTRIBUTE_INPUT          (UINT64_C(1) << 24)
#define GPU_IR_SYNC_STAGE_FRAGMENT_SHADING_RATE           (UINT64_C(1) << 25)
#define GPU_IR_SYNC_STAGE_FRAGMENT_DENSITY                (UINT64_C(1) << 26)
#define GPU_IR_SYNC_STAGE_VIDEO_DECODE                    (UINT64_C(1) << 27)
#define GPU_IR_SYNC_STAGE_VIDEO_ENCODE                    (UINT64_C(1) << 28)
#define GPU_IR_SYNC_STAGE_ACCELERATION_STRUCTURE_COPY     (UINT64_C(1) << 29)
#define GPU_IR_SYNC_STAGE_COPY_INDIRECT                   (UINT64_C(1) << 30)
#define GPU_IR_SYNC_STAGE_TRANSFER \
    (GPU_IR_SYNC_STAGE_COPY | GPU_IR_SYNC_STAGE_RESOLVE | GPU_IR_SYNC_STAGE_BLIT | GPU_IR_SYNC_STAGE_CLEAR)
#define GPU_IR_SYNC_STAGE_PRE_RASTERIZATION_SHADERS \
    (GPU_IR_SYNC_STAGE_VERTEX_SHADER | GPU_IR_SYNC_STAGE_TESSELLATION_CONTROL_SHADER | \
     GPU_IR_SYNC_STAGE_TESSELLATION_EVALUATION_SHADER | GPU_IR_SYNC_STAGE_GEOMETRY_SHADER | \
     GPU_IR_SYNC_STAGE_TASK_SHADER | GPU_IR_SYNC_STAGE_MESH_SHADER)
#define GPU_IR_SYNC_STAGE_ALL_GRAPHICS \
    (GPU_IR_SYNC_STAGE_DRAW_INDIRECT | GPU_IR_SYNC_STAGE_VERTEX_INPUT | GPU_IR_SYNC_STAGE_PRE_RASTERIZATION_SHADERS | \
     GPU_IR_SYNC_STAGE_FRAGMENT_SHADER | GPU_IR_SYNC_STAGE_EARLY_FRAGMENT_TESTS | \
     GPU_IR_SYNC_STAGE_LATE_FRAGMENT_TESTS | GPU_IR_SYNC_STAGE_COLOR_ATTACHMENT_OUTPUT | \
     GPU_IR_SYNC_STAGE_FRAGMENT_SHADING_RATE | GPU_IR_SYNC_STAGE_FRAGMENT_DENSITY)
#define GPU_IR_SYNC_STAGE_ALL \
    (GPU_IR_SYNC_STAGE_TOP_OF_PIPE | GPU_IR_SYNC_STAGE_ALL_GRAPHICS | GPU_IR_SYNC_STAGE_COMPUTE_SHADER | \
     GPU_IR_SYNC_STAGE_TRANSFER | GPU_IR_SYNC_STAGE_BOTTOM_OF_PIPE | GPU_IR_SYNC_STAGE_HOST | \
     GPU_IR_SYNC_STAGE_RAY_TRACING_SHADER | GPU_IR_SYNC_STAGE_ACCELERATION_STRUCTURE_BUILD | \
     GPU_IR_SYNC_STAGE_VIDEO_DECODE | GPU_IR_SYNC_STAGE_VIDEO_ENCODE | \
     GPU_IR_SYNC_STAGE_ACCELERATION_STRUCTURE_COPY | GPU_IR_SYNC_STAGE_COPY_INDIRECT)
#define GPU_IR_SYNC_STAGE_ALL_COMMANDS GPU_IR_SYNC_STAGE_ALL

enum {
    GPU_IR_BARRIER_DEPENDENCY_BY_REGION = 1u << 0,
    GPU_IR_BARRIER_DEPENDENCY_VIEW_LOCAL = 1u << 1,
    GPU_IR_BARRIER_DEPENDENCY_FEEDBACK_LOOP = 1u << 3,
    GPU_IR_BARRIER_DEPENDENCY_QUEUE_FAMILY_OWNERSHIP_ALL_STAGES = 1u << 5,
    GPU_IR_BARRIER_DEPENDENCY_ALL = (1u << 0) | (1u << 1) | (1u << 3) | (1u << 5)
};

enum {
    GPU_IR_IMAGE_LAYOUT_UNDEFINED = 1,
    GPU_IR_IMAGE_LAYOUT_GENERAL,
    GPU_IR_IMAGE_LAYOUT_COLOR_ATTACHMENT,
    GPU_IR_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT,
    GPU_IR_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY,
    GPU_IR_IMAGE_LAYOUT_SHADER_READ_ONLY,
    GPU_IR_IMAGE_LAYOUT_TRANSFER_SOURCE,
    GPU_IR_IMAGE_LAYOUT_TRANSFER_DESTINATION,
    GPU_IR_IMAGE_LAYOUT_PREINITIALIZED,
    GPU_IR_IMAGE_LAYOUT_PRESENT,
    GPU_IR_IMAGE_LAYOUT_READ_ONLY,
    GPU_IR_IMAGE_LAYOUT_ATTACHMENT,
    GPU_IR_IMAGE_LAYOUT_FEEDBACK_LOOP,
    GPU_IR_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE
};

enum {
    GPU_IR_IMAGE_ASPECT_COLOR = 1u << 0,
    GPU_IR_IMAGE_ASPECT_DEPTH = 1u << 1,
    GPU_IR_IMAGE_ASPECT_STENCIL = 1u << 2,
    GPU_IR_IMAGE_ASPECT_PLANE_0 = 1u << 3,
    GPU_IR_IMAGE_ASPECT_PLANE_1 = 1u << 4,
    GPU_IR_IMAGE_ASPECT_PLANE_2 = 1u << 5,
    GPU_IR_IMAGE_ASPECT_METADATA = 1u << 6,
    GPU_IR_IMAGE_ASPECT_ALL = 0x7fu
};

#define GPU_IR_QUEUE_FAMILY_IGNORED UINT32_MAX

enum {
    GPU_IR_BARRIER_TRANSITION = 1,
    GPU_IR_BARRIER_UAV,
    GPU_IR_BARRIER_ALIASING,
    GPU_IR_BARRIER_GLOBAL,
    GPU_IR_BARRIER_MEMORY_DEPENDENCY,
    GPU_IR_BARRIER_BUFFER_DEPENDENCY,
    GPU_IR_BARRIER_IMAGE_DEPENDENCY
};

enum {
    GPU_IR_BARRIER_BEGIN_ONLY = 1u << 0,
    GPU_IR_BARRIER_END_ONLY = 1u << 1
};

enum {
    GPU_IR_COMMAND_INVALID = 0,
    GPU_IR_COMMAND_COPY_BUFFER,
    GPU_IR_COMMAND_UPDATE_BUFFER,
    GPU_IR_COMMAND_FILL_BUFFER,
    GPU_IR_COMMAND_COPY_RESOURCE,
    GPU_IR_COMMAND_COPY_TEXTURE,
    GPU_IR_COMMAND_RESOLVE_TEXTURE,
    GPU_IR_COMMAND_BARRIER,
    GPU_IR_COMMAND_DISPATCH,
    GPU_IR_COMMAND_DRAW,
    GPU_IR_COMMAND_DISPATCH_INDIRECT,
    GPU_IR_COMMAND_DISPATCH_INDIRECT_RANGE,
    GPU_IR_COMMAND_DRAW_INDIRECT,
    GPU_IR_COMMAND_BIND_DESCRIPTOR_STATE,
    GPU_IR_COMMAND_CLEAR_COLOR,
    GPU_IR_COMMAND_CLEAR_DEPTH_STENCIL
};

// Half-open signed pixel bounds. API front ends preserve their rectangle values here; backends clip them to the
// selected attachment extent when encoding the clear.
typedef struct { int32_t left, top, right, bottom; } gpu_ir_rect;

enum {
    GPU_IR_CLEAR_DEPTH = 1u << 0,
    GPU_IR_CLEAR_STENCIL = 1u << 1,
    GPU_IR_CLEAR_DEPTH_STENCIL_ALL = GPU_IR_CLEAR_DEPTH | GPU_IR_CLEAR_STENCIL
};

typedef struct {
    float color[4];
    const gpu_ir_rect *rectangles;
    uint32_t rectangle_count;
} gpu_ir_clear_color_info;

typedef struct {
    uint32_t aspects;
    float depth;
    uint32_t stencil;
    const gpu_ir_rect *rectangles;
    uint32_t rectangle_count;
} gpu_ir_clear_depth_stencil_info;

typedef struct {
    uint32_t type, flags, subresource;
    // Legacy barrier state masks, or source/destination access scopes for scoped dependencies.
    uint32_t before_access, after_access;
    gpu_ir_resource *resource, *resource_after;
    // API-neutral execution scopes; unsupported source flags must be rejected by the front end.
    uint64_t source_stage_scope, destination_stage_scope;
    uint32_t dependency_flags;
    // Zero for barriers without an image layout transition.
    uint32_t old_layout, new_layout;
    // Common queue-family identifiers; IGNORED on both sides means no ownership transfer.
    uint32_t source_queue_family, destination_queue_family;
    // BUFFER_DEPENDENCY uses a concrete byte range; IMAGE_DEPENDENCY uses aspect/mip/layer fields below.
    uint64_t buffer_offset, buffer_size;
    uint32_t image_aspects, base_mip_level, mip_level_count;
    uint32_t base_array_layer, array_layer_count;
} gpu_ir_barrier_desc;

typedef struct {
    uint32_t width, height, depth;
    uint32_t array_layers, mip_levels, bytes_per_texel, sample_count;
    uint32_t is_3d;
    // Block geometry is 1x1 for ordinary formats; compressed formats store the bytes per whole block here.
    uint32_t block_width, block_height, bytes_per_block;
    // Normalized format identity for backends that need a native pixel format; zero means layout-only metadata.
    uint32_t format;
} gpu_ir_texture_desc;

enum {
    GPU_IR_FORMAT_UNDEFINED = 0,
    GPU_IR_FORMAT_R8_UNORM,
    GPU_IR_FORMAT_RG8_UNORM,
    GPU_IR_FORMAT_RGBA8_UNORM,
    GPU_IR_FORMAT_BGRA8_UNORM,
    GPU_IR_FORMAT_R16G16B16A16_FLOAT,
    GPU_IR_FORMAT_R32_FLOAT
};

typedef struct {
    uint64_t buffer_offset;
    uint64_t row_pitch, image_pitch;
    uint32_t buffer_width, buffer_height, buffer_depth;
    uint32_t mip_level, array_slice;
    uint32_t origin_x, origin_y, origin_z;
} gpu_ir_texture_location;

typedef struct { uint32_t mip_level, array_slice; } gpu_ir_resolve_location;
typedef struct {
    uint32_t source_x, source_y, destination_x, destination_y, width, height;
} gpu_ir_resolve_region;

enum {
    GPU_IR_RESOLVE_MODE_AVERAGE = 1,
    GPU_IR_RESOLVE_MODE_MIN,
    GPU_IR_RESOLVE_MODE_MAX
};

enum {
    GPU_IR_OK = 0,
    GPU_IR_INVALID = -1,
    GPU_IR_OUT_OF_MEMORY = -2,
    GPU_IR_RANGE = -3,
    GPU_IR_UNSUPPORTED = -4,
    GPU_IR_NOT_FOUND = -5
};

#define GPU_IR_NO_COMMAND UINT32_MAX
#define GPU_IR_MAX_FILL_PATTERN_SIZE 16u

// The backend object is opaque to the common layer. Ownership transfers to the resource or pipeline on success.
typedef void (*gpu_ir_backend_object_release)(void *object);
gpu_ir_resource *gpu_ir_buffer_create(uint64_t size, void *backend_object, gpu_ir_backend_object_release release);
gpu_ir_resource *gpu_ir_texture_create(const gpu_ir_texture_desc *desc, void *backend_object,
                                       gpu_ir_backend_object_release release);
void gpu_ir_resource_retain(gpu_ir_resource *resource);
void gpu_ir_resource_release(gpu_ir_resource *resource);
int gpu_ir_resource_is_buffer(const gpu_ir_resource *resource);
uint64_t gpu_ir_buffer_size(const gpu_ir_resource *resource);
const gpu_ir_texture_desc *gpu_ir_texture_description(const gpu_ir_resource *resource);
void *gpu_ir_backend_object(const gpu_ir_resource *resource);

// Buffer views retain their source resource independently of the API descriptor heap. `element_size` describes a
// typed element; structured views use `structure_stride`; raw views always address 32-bit words. A backend object
// (for example, a Metal texture-buffer view) is optional and transfers to the view only on successful creation.
enum {
    GPU_IR_BUFFER_VIEW_TYPED = 1,
    GPU_IR_BUFFER_VIEW_STRUCTURED,
    GPU_IR_BUFFER_VIEW_RAW
};

typedef struct {
    uint32_t kind, access_mask;
    uint64_t byte_offset, byte_size;
    uint32_t element_count, element_size, structure_stride;
} gpu_ir_buffer_view_desc;

gpu_ir_buffer_view *gpu_ir_buffer_view_create(gpu_ir_resource *resource,
                                               const gpu_ir_buffer_view_desc *desc,
                                               void *backend_object,
                                               gpu_ir_backend_object_release release);
void gpu_ir_buffer_view_retain(gpu_ir_buffer_view *view);
void gpu_ir_buffer_view_release(gpu_ir_buffer_view *view);
const gpu_ir_buffer_view_desc *gpu_ir_buffer_view_description(const gpu_ir_buffer_view *view);
const gpu_ir_resource *gpu_ir_buffer_view_resource(const gpu_ir_buffer_view *view);
void *gpu_ir_buffer_view_backend_object(const gpu_ir_buffer_view *view);

// An immutable snapshot of resource bindings. API front ends keep their own update/bind rules and create a snapshot
// when those rules make descriptor contents visible to execution. `resource` identifies the API-neutral resource;
// `buffer_view` supplies a typed/structured/raw buffer range; backend objects carry API/backend views or samplers.
// For buffer resources, a zero range means the remaining bytes from offset. On successful state creation,
// resource/view references are retained and ownership of optional backend objects transfers to the state. A null
// descriptor is explicit; an omitted value remains unwritten. Value accessors return borrowed references.
enum { GPU_IR_DESCRIPTOR_VALUE_NULL = 1u << 0 };
typedef struct {
    uint32_t set, binding_namespace, binding, array_element;
    uint32_t kind, stage_mask, access_mask, flags;
    gpu_ir_resource *resource;
    gpu_ir_buffer_view *buffer_view;
    uint64_t offset, range;
    void *backend_object;
    gpu_ir_backend_object_release backend_release;
    void *backend_object_aux;
    gpu_ir_backend_object_release backend_release_aux;
    // One descriptor may also name an auxiliary buffer access, such as a D3D12 UAV counter.
    gpu_ir_resource *auxiliary_resource;
    uint32_t auxiliary_access_mask;
    uint64_t auxiliary_offset, auxiliary_range;
} gpu_ir_descriptor_value;

typedef struct {
    const gpu_ir_descriptor_value *values;
    uint32_t value_count;
} gpu_ir_descriptor_state_desc;

gpu_ir_descriptor_state *gpu_ir_descriptor_state_create(gpu_ir_pipeline_layout *layout,
                                                         const gpu_ir_descriptor_state_desc *desc);
void gpu_ir_descriptor_state_retain(gpu_ir_descriptor_state *state);
void gpu_ir_descriptor_state_release(gpu_ir_descriptor_state *state);
const gpu_ir_pipeline_layout *gpu_ir_descriptor_state_layout(const gpu_ir_descriptor_state *state);
uint32_t gpu_ir_descriptor_state_value_count(const gpu_ir_descriptor_state *state);
int gpu_ir_descriptor_state_value_at(const gpu_ir_descriptor_state *state, uint32_t index,
                                     gpu_ir_descriptor_value *out);
int gpu_ir_descriptor_state_find(const gpu_ir_descriptor_state *state, uint32_t set, uint32_t binding_namespace,
                                 uint32_t binding, uint32_t array_element, gpu_ir_descriptor_value *out);
int gpu_ir_descriptor_state_find_for_stage(const gpu_ir_descriptor_state *state, uint32_t set,
                                           uint32_t binding_namespace, uint32_t binding,
                                           uint32_t array_element, uint32_t stage_mask,
                                           gpu_ir_descriptor_value *out);

// Immutable API-neutral descriptor and push-constant layout. Front ends validate their own binding rules, then map
// compatible bindings to these records; a repeated (set,namespace,binding) key is valid only for disjoint stages.
gpu_ir_pipeline_layout *gpu_ir_pipeline_layout_create(const gpu_ir_pipeline_layout_desc *desc);
void gpu_ir_pipeline_layout_retain(gpu_ir_pipeline_layout *layout);
void gpu_ir_pipeline_layout_release(gpu_ir_pipeline_layout *layout);
uint32_t gpu_ir_pipeline_layout_binding_count(const gpu_ir_pipeline_layout *layout);
int gpu_ir_pipeline_layout_binding_at(const gpu_ir_pipeline_layout *layout, uint32_t index, gpu_ir_descriptor_binding *out);
int gpu_ir_pipeline_layout_find_binding(const gpu_ir_pipeline_layout *layout, uint32_t set, uint32_t binding,
                                        gpu_ir_descriptor_binding *out);
int gpu_ir_pipeline_layout_find_binding_in_namespace(const gpu_ir_pipeline_layout *layout, uint32_t set,
                                                      uint32_t binding_namespace, uint32_t binding,
                                                      gpu_ir_descriptor_binding *out);
int gpu_ir_pipeline_layout_find_binding_for_stage(const gpu_ir_pipeline_layout *layout, uint32_t set,
                                                   uint32_t binding_namespace, uint32_t binding,
                                                   uint32_t stage_mask, gpu_ir_descriptor_binding *out);
uint32_t gpu_ir_pipeline_layout_push_constant_count(const gpu_ir_pipeline_layout *layout);
int gpu_ir_pipeline_layout_push_constant_at(const gpu_ir_pipeline_layout *layout, uint32_t index,
                                            gpu_ir_push_constant_range *out);

enum {
    GPU_IR_PIPELINE_GRAPHICS = 1,
    GPU_IR_PIPELINE_COMPUTE
};

typedef struct {
    uint32_t kind, stage_mask;
    uint32_t threadgroup_size[3];
} gpu_ir_pipeline_info;

typedef struct {
    gpu_ir_pipeline_info info;
    gpu_ir_pipeline_layout *layout;
    void *backend_object;
    gpu_ir_backend_object_release release;
    // Optional complete normalized stage set, indexed by the matching GPU_IR_STAGE_SLOT_* value.
    // Existing direct-translation front ends may leave this empty while they migrate their shader readers.
    gpu_ir_shader *stage_shaders[GPU_IR_STAGE_SLOT_COUNT];
} gpu_ir_pipeline_desc;

// A retained, API-neutral pipeline record. API front ends validate and lower their shader/layout state, while the
// backend object stays opaque and can be replaced by another backend without changing command recording.
gpu_ir_pipeline *gpu_ir_pipeline_create(const gpu_ir_pipeline_desc *desc);
void gpu_ir_pipeline_retain(gpu_ir_pipeline *pipeline);
void gpu_ir_pipeline_release(gpu_ir_pipeline *pipeline);
const gpu_ir_pipeline_info *gpu_ir_pipeline_description(const gpu_ir_pipeline *pipeline);
const gpu_ir_pipeline_layout *gpu_ir_pipeline_layout_of(const gpu_ir_pipeline *pipeline);
void *gpu_ir_pipeline_backend_object(const gpu_ir_pipeline *pipeline);
const gpu_ir_shader *gpu_ir_pipeline_shader(const gpu_ir_pipeline *pipeline, uint32_t stage_mask);

// API-neutral typed shader representation. IDs occupy one namespace across types, functions, block labels and values;
// a source frontend with separate namespaces must remap its IDs. Front ends retain source opcode/API information for
// diagnostics while normalizing types, control flow and common operations for translation backends.
enum {
    GPU_IR_SHADER_SOURCE_DXBC = 1,
    GPU_IR_SHADER_SOURCE_DXIL,
    GPU_IR_SHADER_SOURCE_SPIRV
};

enum {
    GPU_IR_SHADER_TYPE_VOID = 1,
    GPU_IR_SHADER_TYPE_BOOL,
    GPU_IR_SHADER_TYPE_INT,
    GPU_IR_SHADER_TYPE_FLOAT,
    GPU_IR_SHADER_TYPE_VECTOR,
    GPU_IR_SHADER_TYPE_MATRIX,
    GPU_IR_SHADER_TYPE_ARRAY,
    GPU_IR_SHADER_TYPE_RUNTIME_ARRAY,
    GPU_IR_SHADER_TYPE_STRUCT,
    GPU_IR_SHADER_TYPE_POINTER,
    GPU_IR_SHADER_TYPE_IMAGE,
    GPU_IR_SHADER_TYPE_SAMPLER,
    GPU_IR_SHADER_TYPE_SAMPLED_IMAGE,
    GPU_IR_SHADER_TYPE_FUNCTION
};

enum {
    GPU_IR_SHADER_TYPE_FLAG_BLOCK = 1u << 0,
    GPU_IR_SHADER_TYPE_FLAG_BUFFER_BLOCK = 1u << 1,
    GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE = 1u << 2,
    GPU_IR_SHADER_TYPE_FLAG_NON_READABLE = 1u << 3,
    GPU_IR_SHADER_TYPE_FLAG_ROW_MAJOR = 1u << 4,
    GPU_IR_SHADER_TYPE_FLAG_COLUMN_MAJOR = 1u << 5
};

// Common semantic classes. `source_storage_class` retains the frontend's numeric value for diagnostics and
// validation; backends and future shader frontends must use these normalized values for behavior.
enum {
    GPU_IR_SHADER_STORAGE_UNSPECIFIED = 0,
    GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT,
    GPU_IR_SHADER_STORAGE_INPUT,
    GPU_IR_SHADER_STORAGE_UNIFORM,
    GPU_IR_SHADER_STORAGE_OUTPUT,
    GPU_IR_SHADER_STORAGE_WORKGROUP,
    GPU_IR_SHADER_STORAGE_CROSS_WORKGROUP,
    GPU_IR_SHADER_STORAGE_PRIVATE,
    GPU_IR_SHADER_STORAGE_FUNCTION,
    GPU_IR_SHADER_STORAGE_GENERIC,
    GPU_IR_SHADER_STORAGE_PUSH_CONSTANT,
    GPU_IR_SHADER_STORAGE_ATOMIC_COUNTER,
    GPU_IR_SHADER_STORAGE_IMAGE,
    GPU_IR_SHADER_STORAGE_BUFFER,
    GPU_IR_SHADER_STORAGE_PHYSICAL_BUFFER,
    GPU_IR_SHADER_STORAGE_UNMAPPED
};

// The normalized built-ins cover common execution values. A frontend preserves the source-specific numeric value
// separately, and uses UNMAPPED when it cannot assign API-neutral semantics.
enum {
    GPU_IR_SHADER_BUILTIN_UNSPECIFIED = 0,
    GPU_IR_SHADER_BUILTIN_NUM_WORKGROUPS,
    GPU_IR_SHADER_BUILTIN_WORKGROUP_SIZE,
    GPU_IR_SHADER_BUILTIN_WORKGROUP_ID,
    GPU_IR_SHADER_BUILTIN_LOCAL_INVOCATION_ID,
    GPU_IR_SHADER_BUILTIN_GLOBAL_INVOCATION_ID,
    GPU_IR_SHADER_BUILTIN_LOCAL_INVOCATION_INDEX,
    GPU_IR_SHADER_BUILTIN_UNMAPPED
};

typedef struct {
    uint32_t id, kind, bit_width, signedness;
    uint32_t element_type_id, element_count, storage_class, source_storage_class, array_length_id;
    uint32_t first_member, member_count, array_stride, has_array_stride, decoration_flags;
    uint32_t image_dimension, image_depth, image_arrayed, image_multisampled;
    uint32_t image_sampled, image_format, image_access_qualifier;
} gpu_ir_shader_type;

typedef struct {
    uint32_t type_id, decoration_flags, offset, matrix_stride;
    uint32_t location, component, index, builtin, source_builtin;
    uint32_t has_offset, has_matrix_stride, has_location, has_component, has_index, has_builtin;
} gpu_ir_shader_member;

typedef struct {
    uint32_t id, result_type_id, function_type_id;
    uint32_t first_block, block_count, first_instruction, instruction_count, parameter_count;
} gpu_ir_shader_function;

typedef struct {
    uint32_t function_index, label_id, first_instruction, instruction_count;
    uint32_t terminator_opcode;
} gpu_ir_shader_block;

enum {
    GPU_IR_SHADER_OP_UNDEFINED = 1,
    GPU_IR_SHADER_OP_CONSTANT,
    GPU_IR_SHADER_OP_PARAMETER,
    GPU_IR_SHADER_OP_VARIABLE,
    GPU_IR_SHADER_OP_LOAD,
    GPU_IR_SHADER_OP_STORE,
    GPU_IR_SHADER_OP_ACCESS_CHAIN,
    GPU_IR_SHADER_OP_COPY,
    GPU_IR_SHADER_OP_CONVERT,
    GPU_IR_SHADER_OP_COMPOSITE,
    GPU_IR_SHADER_OP_ARITHMETIC,
    GPU_IR_SHADER_OP_COMPARE,
    GPU_IR_SHADER_OP_SELECT,
    GPU_IR_SHADER_OP_PHI,
    GPU_IR_SHADER_OP_FUNCTION_CALL,
    GPU_IR_SHADER_OP_SELECTION_MERGE,
    GPU_IR_SHADER_OP_LOOP_MERGE,
    GPU_IR_SHADER_OP_BARRIER,
    GPU_IR_SHADER_OP_BRANCH,
    GPU_IR_SHADER_OP_BRANCH_CONDITIONAL,
    GPU_IR_SHADER_OP_SWITCH,
    GPU_IR_SHADER_OP_RETURN,
    GPU_IR_SHADER_OP_RETURN_VALUE,
    GPU_IR_SHADER_OP_UNREACHABLE,
    GPU_IR_SHADER_OP_ARRAY_LENGTH,
    GPU_IR_SHADER_OP_IMAGE_READ,
    GPU_IR_SHADER_OP_IMAGE_WRITE,
    GPU_IR_SHADER_OP_SAMPLED_IMAGE,
    GPU_IR_SHADER_OP_IMAGE_SAMPLE
};

enum {
    GPU_IR_SHADER_CONSTANT_BOOL = 1,
    GPU_IR_SHADER_CONSTANT_SCALAR,
    GPU_IR_SHADER_CONSTANT_NULL,
    GPU_IR_SHADER_CONSTANT_COMPOSITE
};

enum {
    GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC = 1,
    GPU_IR_SHADER_COMPOSITE_VECTOR_INSERT_DYNAMIC,
    GPU_IR_SHADER_COMPOSITE_VECTOR_SHUFFLE,
    GPU_IR_SHADER_COMPOSITE_CONSTRUCT,
    GPU_IR_SHADER_COMPOSITE_EXTRACT,
    GPU_IR_SHADER_COMPOSITE_INSERT,
    GPU_IR_SHADER_COMPOSITE_TRANSPOSE
};

enum {
    GPU_IR_SHADER_BARRIER_CONTROL = 1,
    GPU_IR_SHADER_BARRIER_MEMORY
};

enum {
    GPU_IR_SHADER_ARITH_ADD = 1,
    GPU_IR_SHADER_ARITH_SUBTRACT,
    GPU_IR_SHADER_ARITH_MULTIPLY,
    GPU_IR_SHADER_ARITH_DIVIDE,
    GPU_IR_SHADER_ARITH_REMAINDER,
    GPU_IR_SHADER_ARITH_MODULO,
    GPU_IR_SHADER_ARITH_NEGATE,
    GPU_IR_SHADER_ARITH_SHIFT_LEFT,
    GPU_IR_SHADER_ARITH_SHIFT_RIGHT_LOGICAL,
    GPU_IR_SHADER_ARITH_SHIFT_RIGHT_ARITHMETIC,
    GPU_IR_SHADER_ARITH_BIT_AND,
    GPU_IR_SHADER_ARITH_BIT_OR,
    GPU_IR_SHADER_ARITH_BIT_XOR,
    GPU_IR_SHADER_ARITH_BIT_NOT,
    GPU_IR_SHADER_ARITH_LOGICAL_AND,
    GPU_IR_SHADER_ARITH_LOGICAL_OR,
    GPU_IR_SHADER_ARITH_LOGICAL_NOT,
    GPU_IR_SHADER_ARITH_ANY,
    GPU_IR_SHADER_ARITH_ALL,
    GPU_IR_SHADER_ARITH_VECTOR_TIMES_SCALAR,
    GPU_IR_SHADER_ARITH_DOT,
    GPU_IR_SHADER_ARITH_ABSOLUTE,
    GPU_IR_SHADER_ARITH_FLOOR,
    GPU_IR_SHADER_ARITH_CEILING,
    GPU_IR_SHADER_ARITH_SQRT,
    GPU_IR_SHADER_ARITH_INVERSE_SQRT,
    GPU_IR_SHADER_ARITH_MINIMUM,
    GPU_IR_SHADER_ARITH_MAXIMUM,
    GPU_IR_SHADER_ARITH_CLAMP,
    GPU_IR_SHADER_ARITH_FMA,
    GPU_IR_SHADER_ARITH_POWER,
    GPU_IR_SHADER_ARITH_TRUNC,
    GPU_IR_SHADER_ARITH_FRACT,
    GPU_IR_SHADER_ARITH_ROUND,
    GPU_IR_SHADER_ARITH_ROUND_EVEN,
    GPU_IR_SHADER_ARITH_SIN,
    GPU_IR_SHADER_ARITH_COS,
    GPU_IR_SHADER_ARITH_TAN,
    GPU_IR_SHADER_ARITH_ASIN,
    GPU_IR_SHADER_ARITH_ACOS,
    GPU_IR_SHADER_ARITH_ATAN,
    GPU_IR_SHADER_ARITH_SINH,
    GPU_IR_SHADER_ARITH_COSH,
    GPU_IR_SHADER_ARITH_TANH,
    GPU_IR_SHADER_ARITH_ASINH,
    GPU_IR_SHADER_ARITH_ACOSH,
    GPU_IR_SHADER_ARITH_ATANH,
    GPU_IR_SHADER_ARITH_EXP,
    GPU_IR_SHADER_ARITH_LOG,
    GPU_IR_SHADER_ARITH_EXP2,
    GPU_IR_SHADER_ARITH_LOG2,
    GPU_IR_SHADER_ARITH_BIT_COUNT,
    GPU_IR_SHADER_ARITH_BIT_REVERSE,
    GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT,
    GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT,
    GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT,
    GPU_IR_SHADER_ARITH_BIT_FIND_LSB,
    GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED,
    GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED,
    GPU_IR_SHADER_ARITH_SATURATE
};

enum {
    GPU_IR_SHADER_COMPARE_EQUAL = 1,
    GPU_IR_SHADER_COMPARE_NOT_EQUAL,
    GPU_IR_SHADER_COMPARE_LESS,
    GPU_IR_SHADER_COMPARE_LESS_EQUAL,
    GPU_IR_SHADER_COMPARE_GREATER,
    GPU_IR_SHADER_COMPARE_GREATER_EQUAL,
    GPU_IR_SHADER_COMPARE_IS_NAN,
    GPU_IR_SHADER_COMPARE_IS_INF,
    GPU_IR_SHADER_COMPARE_IS_FINITE,
    GPU_IR_SHADER_COMPARE_IS_NORMAL,
    GPU_IR_SHADER_COMPARE_SIGN_BIT_SET,
    GPU_IR_SHADER_COMPARE_ORDERED,
    GPU_IR_SHADER_COMPARE_UNORDERED,
    GPU_IR_SHADER_COMPARE_LESS_GREATER,
    GPU_IR_SHADER_COMPARE_ORDERED_EQUAL,
    GPU_IR_SHADER_COMPARE_UNORDERED_EQUAL,
    GPU_IR_SHADER_COMPARE_ORDERED_NOT_EQUAL,
    GPU_IR_SHADER_COMPARE_UNORDERED_NOT_EQUAL,
    GPU_IR_SHADER_COMPARE_ORDERED_LESS,
    GPU_IR_SHADER_COMPARE_UNORDERED_LESS,
    GPU_IR_SHADER_COMPARE_ORDERED_LESS_EQUAL,
    GPU_IR_SHADER_COMPARE_UNORDERED_LESS_EQUAL,
    GPU_IR_SHADER_COMPARE_ORDERED_GREATER,
    GPU_IR_SHADER_COMPARE_UNORDERED_GREATER,
    GPU_IR_SHADER_COMPARE_ORDERED_GREATER_EQUAL,
    GPU_IR_SHADER_COMPARE_UNORDERED_GREATER_EQUAL
};

enum {
    GPU_IR_SHADER_CONVERT_FLOAT_TO_UNSIGNED = 1,
    GPU_IR_SHADER_CONVERT_FLOAT_TO_SIGNED,
    GPU_IR_SHADER_CONVERT_SIGNED_TO_FLOAT,
    GPU_IR_SHADER_CONVERT_UNSIGNED_TO_FLOAT,
    GPU_IR_SHADER_CONVERT_UNSIGNED_WIDTH,
    GPU_IR_SHADER_CONVERT_SIGNED_WIDTH,
    GPU_IR_SHADER_CONVERT_FLOAT_WIDTH,
    GPU_IR_SHADER_CONVERT_BITCAST
};

enum {
    GPU_IR_SHADER_OPERAND_ID = 1,
    GPU_IR_SHADER_OPERAND_LITERAL
};

enum {
    GPU_IR_SHADER_FLAG_FLOAT = 1u << 0,
    GPU_IR_SHADER_FLAG_SIGNED = 1u << 1, // signed operation semantics, including signless integer source types
    GPU_IR_SHADER_FLAG_PRECISE = 1u << 2,
    GPU_IR_SHADER_FLAG_NO_SIGNED_WRAP = 1u << 3,
    GPU_IR_SHADER_FLAG_NO_UNSIGNED_WRAP = 1u << 4,
    GPU_IR_SHADER_FLAG_FAST_MATH = 1u << 5,
    GPU_IR_SHADER_FLAG_SPECIALIZATION = 1u << 6,
    // DXIL BFE/BFI consume only the low five bits of width and offset; SPIR-V does not use this flag.
    GPU_IR_SHADER_FLAG_DXIL_BITFIELD_LOW5 = 1u << 7
};

enum {
    GPU_IR_SHADER_MEMORY_VOLATILE = 1u << 0,
    GPU_IR_SHADER_MEMORY_ALIGNED = 1u << 1,
    GPU_IR_SHADER_MEMORY_NONTEMPORAL = 1u << 2
};

typedef struct {
    uint32_t kind, value;
} gpu_ir_shader_operand;

typedef struct {
    uint32_t source_api, opcode, operation, source_opcode;
    uint32_t result_id, result_type_id, function_index, block_index, flags;
    uint32_t memory_access_flags, memory_alignment;
    // Used by VARIABLE instructions: API-neutral register-space/descriptor-set binding and interface decorations.
    uint32_t storage_class, source_storage_class, resource_set, binding_namespace, binding;
    uint32_t decoration_flags, location, component, index, builtin, source_builtin;
    uint32_t has_descriptor_set, has_binding, has_location, has_component, has_index, has_builtin;
    uint32_t first_operand, operand_count, first_literal, literal_word_count;
} gpu_ir_shader_instruction;

typedef struct {
    uint32_t source_api, stage_mask, entry_function_id;
    // Literal compute LocalSize when known; all zero for non-compute stages or LocalSizeId pending specialization.
    uint32_t threadgroup_size[3];
    const gpu_ir_shader_type *types;
    uint32_t type_count;
    const gpu_ir_shader_member *type_members;
    uint32_t type_member_count;
    const gpu_ir_shader_function *functions;
    uint32_t function_count;
    const gpu_ir_shader_block *blocks;
    uint32_t block_count;
    const gpu_ir_shader_instruction *instructions;
    uint32_t instruction_count;
    const gpu_ir_shader_operand *operands;
    uint32_t operand_count;
    const uint32_t *literal_words;
    uint32_t literal_word_count;
} gpu_ir_shader_desc;

typedef struct {
    uint32_t result_id;
    const uint32_t *literal_words;
    uint32_t literal_word_count;
} gpu_ir_shader_constant_override;

gpu_ir_shader *gpu_ir_shader_create(const gpu_ir_shader_desc *desc);
// Resolves specialization constants to defaults plus optional pipeline overrides and returns a new immutable shader.
// threadgroup_size may be NULL to preserve the shader's current size, or provide the resolved compute LocalSize.
int gpu_ir_shader_specialize(const gpu_ir_shader *shader, const gpu_ir_shader_constant_override *overrides,
                             uint32_t override_count, const uint32_t threadgroup_size[3],
                             gpu_ir_shader **specialized_shader);
void gpu_ir_shader_retain(gpu_ir_shader *shader);
void gpu_ir_shader_release(gpu_ir_shader *shader);
uint32_t gpu_ir_shader_source_api(const gpu_ir_shader *shader);
uint32_t gpu_ir_shader_stage(const gpu_ir_shader *shader);
uint32_t gpu_ir_shader_entry_function(const gpu_ir_shader *shader);
void gpu_ir_shader_threadgroup_size(const gpu_ir_shader *shader, uint32_t size[3]);
uint32_t gpu_ir_shader_type_count(const gpu_ir_shader *shader);
const gpu_ir_shader_type *gpu_ir_shader_type_at(const gpu_ir_shader *shader, uint32_t index);
const gpu_ir_shader_type *gpu_ir_shader_find_type(const gpu_ir_shader *shader, uint32_t type_id);
uint32_t gpu_ir_shader_type_member_count(const gpu_ir_shader *shader);
const gpu_ir_shader_member *gpu_ir_shader_type_member_at(const gpu_ir_shader *shader, uint32_t index);
uint32_t gpu_ir_shader_function_count(const gpu_ir_shader *shader);
const gpu_ir_shader_function *gpu_ir_shader_function_at(const gpu_ir_shader *shader, uint32_t index);
const gpu_ir_shader_function *gpu_ir_shader_find_function(const gpu_ir_shader *shader, uint32_t function_id);
uint32_t gpu_ir_shader_block_count(const gpu_ir_shader *shader);
const gpu_ir_shader_block *gpu_ir_shader_block_at(const gpu_ir_shader *shader, uint32_t index);
uint32_t gpu_ir_shader_instruction_count(const gpu_ir_shader *shader);
const gpu_ir_shader_instruction *gpu_ir_shader_instruction_at(const gpu_ir_shader *shader, uint32_t index);
uint32_t gpu_ir_shader_operand_count(const gpu_ir_shader *shader);
const gpu_ir_shader_operand *gpu_ir_shader_operand_at(const gpu_ir_shader *shader, uint32_t index);
uint32_t gpu_ir_shader_literal_word_count(const gpu_ir_shader *shader);
uint32_t gpu_ir_shader_literal_word_at(const gpu_ir_shader *shader, uint32_t index);

enum {
    GPU_IR_TOPOLOGY_POINT_LIST = 1,
    GPU_IR_TOPOLOGY_LINE_LIST,
    GPU_IR_TOPOLOGY_LINE_STRIP,
    GPU_IR_TOPOLOGY_TRIANGLE_LIST,
    GPU_IR_TOPOLOGY_TRIANGLE_STRIP
};

typedef struct {
    uint32_t topology, indexed;
    uint32_t vertex_count, index_count, instance_count;
    uint32_t first_vertex, first_index, first_instance;
    int32_t vertex_offset;
} gpu_ir_draw_info;

typedef struct {
    uint32_t topology, indexed, draw_count, stride;
    uint64_t argument_offset, count_offset;
} gpu_ir_indirect_draw_info;

typedef struct {
    uint32_t dispatch_count, stride;
    uint64_t argument_offset, count_offset;
} gpu_ir_indirect_dispatch_info;

// A conservative, whole-resource use made by one GPU command. Front ends lower API-specific
// resource states and subresource ranges to these access and shader-stage masks.
typedef struct {
    gpu_ir_resource *resource;
    uint32_t stage_mask, access_mask;
} gpu_ir_resource_access;

// Returns nonzero when two uses of the same common resource conflict because either writes.
int gpu_ir_resource_access_conflicts(const gpu_ir_resource_access *a, const gpu_ir_resource_access *b);

typedef struct {
    void *context;
    int (*copy_resource)(void *context, const gpu_ir_resource *src, const gpu_ir_resource *dst);
    int (*copy_buffer)(void *context, const gpu_ir_resource *src, uint64_t src_offset,
                       const gpu_ir_resource *dst, uint64_t dst_offset, uint64_t size);
    int (*update_buffer)(void *context, const gpu_ir_resource *dst, uint64_t dst_offset,
                         const void *data, uint64_t size);
    int (*fill_buffer)(void *context, const gpu_ir_resource *dst, uint64_t dst_offset,
                       uint64_t size, uint32_t value);
    int (*copy_texture)(void *context, const gpu_ir_resource *src, const gpu_ir_texture_location *src_location,
                        const gpu_ir_resource *dst, const gpu_ir_texture_location *dst_location,
                        uint32_t width, uint32_t height, uint32_t depth);
    int (*resolve_texture)(void *context, const gpu_ir_resource *src, const gpu_ir_resolve_location *src_location,
                           const gpu_ir_resource *dst, const gpu_ir_resolve_location *dst_location,
                           const gpu_ir_resolve_region *region, uint32_t mode);
    int (*barrier)(void *context, const gpu_ir_barrier_desc *barrier);
    int (*dispatch)(void *context, const gpu_ir_pipeline *pipeline,
                    uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z,
                    const gpu_ir_resource_access *accesses, uint32_t access_count);
    int (*dispatch_indirect)(void *context, const gpu_ir_pipeline *pipeline,
                             const gpu_ir_resource *arguments, uint64_t argument_offset,
                             const gpu_ir_resource_access *accesses, uint32_t access_count);
    int (*dispatch_indirect_range)(void *context, const gpu_ir_pipeline *pipeline,
                                   const gpu_ir_resource *arguments, const gpu_ir_resource *count_arguments,
                                   const gpu_ir_indirect_dispatch_info *dispatch,
                                   const gpu_ir_resource_access *accesses, uint32_t access_count);
    int (*draw)(void *context, const gpu_ir_pipeline *pipeline, const gpu_ir_draw_info *draw,
                const gpu_ir_resource_access *accesses, uint32_t access_count);
    int (*draw_indirect)(void *context, const gpu_ir_pipeline *pipeline,
                         const gpu_ir_resource *arguments, const gpu_ir_resource *count_arguments,
                         const gpu_ir_indirect_draw_info *draw,
                         const gpu_ir_resource_access *accesses, uint32_t access_count);
    int (*bind_descriptor_state)(void *context, const gpu_ir_descriptor_state *state);
    int (*clear_color)(void *context, const gpu_ir_resource *texture,
                       const gpu_ir_clear_color_info *clear);
    int (*clear_depth_stencil)(void *context, const gpu_ir_resource *texture,
                               const gpu_ir_clear_depth_stencil_info *clear);
    int (*fill_buffer_pattern)(void *context, const gpu_ir_resource *dst, uint64_t dst_offset,
                               uint64_t size, const void *pattern, uint32_t pattern_size);
} gpu_ir_backend;

typedef struct { void *commands; uint32_t count, capacity; } gpu_ir_stream;

void gpu_ir_stream_init(gpu_ir_stream *stream);
void gpu_ir_stream_clear(gpu_ir_stream *stream);
void gpu_ir_stream_destroy(gpu_ir_stream *stream);
uint32_t gpu_ir_stream_count(const gpu_ir_stream *stream);
int gpu_ir_record_copy_buffer(gpu_ir_stream *stream, gpu_ir_resource *src, uint64_t src_offset,
                              gpu_ir_resource *dst, uint64_t dst_offset, uint64_t size, uint32_t *index);
// Copies data into stream-owned storage at record time; the backend may consume the bytes during execution.
int gpu_ir_record_update_buffer(gpu_ir_stream *stream, gpu_ir_resource *dst, uint64_t dst_offset,
                                const void *data, uint64_t size, uint32_t *index);
// Records a 32-bit repeated pattern fill. Offset and size must be four-byte aligned.
int gpu_ir_record_fill_buffer(gpu_ir_stream *stream, gpu_ir_resource *dst, uint64_t dst_offset,
                              uint64_t size, uint32_t value, uint32_t *index);
// Records a repeated 1, 2, 4, 8, or 16-byte pattern. The stream snapshots pattern bytes at record time.
int gpu_ir_record_fill_buffer_pattern(gpu_ir_stream *stream, gpu_ir_resource *dst, uint64_t dst_offset,
                                      uint64_t size, const void *pattern, uint32_t pattern_size,
                                      uint32_t *index);
int gpu_ir_record_copy_resource(gpu_ir_stream *stream, gpu_ir_resource *src, gpu_ir_resource *dst, uint32_t *index);
int gpu_ir_record_copy_texture(gpu_ir_stream *stream, gpu_ir_resource *src,
                               const gpu_ir_texture_location *src_location, gpu_ir_resource *dst,
                               const gpu_ir_texture_location *dst_location, uint32_t width,
                               uint32_t height, uint32_t depth, uint32_t *index);
int gpu_ir_record_resolve_texture(gpu_ir_stream *stream, gpu_ir_resource *src,
                                  const gpu_ir_resolve_location *src_location, gpu_ir_resource *dst,
                                  const gpu_ir_resolve_location *dst_location, uint32_t mode, uint32_t *index);
int gpu_ir_record_resolve_texture_region(gpu_ir_stream *stream, gpu_ir_resource *src,
                                         const gpu_ir_resolve_location *src_location, gpu_ir_resource *dst,
                                         const gpu_ir_resolve_location *dst_location,
                                         const gpu_ir_resolve_region *region, uint32_t mode, uint32_t *index);
// Records an attachment clear. The stream copies the rectangle list and retains the texture. An empty rectangle
// list means the full selected attachment; API front ends validate attachment format and access state.
int gpu_ir_record_clear_color(gpu_ir_stream *stream, gpu_ir_resource *texture, const float color[4],
                              const gpu_ir_rect *rectangles, uint32_t rectangle_count, uint32_t *index);
int gpu_ir_record_clear_depth_stencil(gpu_ir_stream *stream, gpu_ir_resource *texture, uint32_t aspects,
                                      float depth, uint32_t stencil, const gpu_ir_rect *rectangles,
                                      uint32_t rectangle_count, uint32_t *index);
int gpu_ir_record_barrier(gpu_ir_stream *stream, const gpu_ir_barrier_desc *barrier, uint32_t *index);
int gpu_ir_record_dispatch(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                           uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z,
                           uint32_t *index);
int gpu_ir_record_dispatch_with_accesses(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                         uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z,
                                         const gpu_ir_resource_access *accesses, uint32_t access_count,
                                         uint32_t *index);
int gpu_ir_record_dispatch_indirect(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                    gpu_ir_resource *arguments, uint64_t argument_offset, uint32_t *index);
int gpu_ir_record_dispatch_indirect_with_accesses(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                                  gpu_ir_resource *arguments, uint64_t argument_offset,
                                                  const gpu_ir_resource_access *accesses, uint32_t access_count,
                                                  uint32_t *index);
int gpu_ir_record_dispatch_indirect_range(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                          gpu_ir_resource *arguments,
                                          const gpu_ir_indirect_dispatch_info *dispatch, uint32_t *index);
int gpu_ir_record_dispatch_indirect_range_with_accesses(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                                        gpu_ir_resource *arguments,
                                                        const gpu_ir_indirect_dispatch_info *dispatch,
                                                        const gpu_ir_resource_access *accesses,
                                                        uint32_t access_count, uint32_t *index);
int gpu_ir_record_dispatch_indirect_range_counted(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                                  gpu_ir_resource *arguments, gpu_ir_resource *count_arguments,
                                                  const gpu_ir_indirect_dispatch_info *dispatch, uint32_t *index);
int gpu_ir_record_dispatch_indirect_range_counted_with_accesses(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                                                gpu_ir_resource *arguments,
                                                                gpu_ir_resource *count_arguments,
                                                                const gpu_ir_indirect_dispatch_info *dispatch,
                                                                const gpu_ir_resource_access *accesses,
                                                                uint32_t access_count, uint32_t *index);
int gpu_ir_record_draw(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                       const gpu_ir_draw_info *draw, uint32_t *index);
int gpu_ir_record_draw_with_accesses(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                     const gpu_ir_draw_info *draw,
                                     const gpu_ir_resource_access *accesses, uint32_t access_count,
                                     uint32_t *index);
int gpu_ir_record_draw_indirect(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                               gpu_ir_resource *arguments, const gpu_ir_indirect_draw_info *draw,
                               uint32_t *index);
int gpu_ir_record_draw_indirect_counted(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                       gpu_ir_resource *arguments, gpu_ir_resource *count_arguments,
                                       const gpu_ir_indirect_draw_info *draw, uint32_t *index);
int gpu_ir_record_draw_indirect_with_accesses(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                              gpu_ir_resource *arguments, const gpu_ir_indirect_draw_info *draw,
                                              const gpu_ir_resource_access *accesses, uint32_t access_count,
                                              uint32_t *index);
int gpu_ir_record_draw_indirect_counted_with_accesses(gpu_ir_stream *stream, gpu_ir_pipeline *pipeline,
                                                       gpu_ir_resource *arguments,
                                                       gpu_ir_resource *count_arguments,
                                                       const gpu_ir_indirect_draw_info *draw,
                                                       const gpu_ir_resource_access *accesses,
                                                       uint32_t access_count, uint32_t *index);
uint32_t gpu_ir_command_access_count(const gpu_ir_stream *stream, uint32_t index);
int gpu_ir_command_access_at(const gpu_ir_stream *stream, uint32_t index, uint32_t access_index,
                             gpu_ir_resource_access *out);
int gpu_ir_record_bind_descriptor_state(gpu_ir_stream *stream, gpu_ir_descriptor_state *state, uint32_t *index);
uint32_t gpu_ir_command_kind(const gpu_ir_stream *stream, uint32_t index);
int gpu_ir_execute(const gpu_ir_stream *stream, uint32_t index, const gpu_ir_backend *backend);
// Execute-time access override for APIs whose resource bindings are resolved at submission/replay time.
int gpu_ir_execute_with_accesses(const gpu_ir_stream *stream, uint32_t index, const gpu_ir_backend *backend,
                                 const gpu_ir_resource_access *accesses, uint32_t access_count);

#endif
