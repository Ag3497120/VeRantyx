// SPIR-V → Metal Shading Language, directly (as dxbc.c does for DXBC), for the Vulkan front end.
// It reads the SPIR-V that GLSL and HLSL compilers produce for real games (structured control flow, phis, images,
// buffers of any std140/std430 layout, subgroup operations) and writes one MSL entry point.
//
// Resource model (the Vulkan front end binds the same way):
//   descriptor set s      -> [[buffer(s)]], an argument buffer of 8-byte words (Metal 3 resource IDs and GPU
//                            addresses written by vkUpdateDescriptorSets); each binding starts at the word the
//                            front end chose (vkmsl_binding.word). Words per descriptor: buffers 2 (address, size),
//                            images, samplers, texel buffers 1, combined image samplers 1 + 1 (all textures of the
//                            binding first, then all samplers).
//   push constants        -> [[buffer(VKMSL_PUSH_BUFFER)]] (raw bytes)
//   auxiliary data        -> [[buffer(VKMSL_AUX_BUFFER)]]: uint dynamic offsets[], indexed by vkmsl_binding.dynamic
//   vertex buffer binding b -> [[buffer(VKMSL_VERTEX_BUFFER + b)]] through the pipeline's vertex descriptor
#ifndef VX_VK_MSL_H
#define VX_VK_MSL_H
#include <stddef.h>
#include <stdint.h>

#define VKMSL_MAX_SETS 8u
#define VKMSL_PUSH_BUFFER 8u
#define VKMSL_AUX_BUFFER 9u
#define VKMSL_VERTEX_BUFFER 14u
#define VKMSL_MAX_VERTEX_BUFFERS 16u
// Geometry shaders run as two kernels and a draw (vk_command.m): the vertex shader as a kernel writing each vertex's
// outputs as float4 slots (position, the user locations, two of clip distances, then point size/layer/viewport), the
// geometry shader as a kernel over the primitives writing its strips as triangles of slots of its own outputs, and
// "vkpass", a vertex function in the geometry shader's library that draws those triangles.
#define VKMSL_GS_PARAMS 10u
#define VKMSL_GS_IN 11u
#define VKMSL_GS_OUT 12u
#define VKMSL_GS_COUNT 13u
#define VKMSL_GS_INDEX 30u
typedef struct {
    uint32_t count, instances, first, first_instance;   // the draw (vertices or indices)
    int32_t base_vertex;
    uint32_t indexed, index16, prims;                   // prims: primitives the geometry kernel runs over
    uint32_t topology, invocations, in_slots, out_max_tris;   // topology: VkPrimitiveTopology of the draw
} vkmsl_gs_params;
#define VKMSL_NO_DYNAMIC 0xffffffffu

// Words in a set's argument buffer per descriptor of a Vulkan descriptor type (VkDescriptorType values).
uint32_t vkmsl_descriptor_words(uint32_t vk_descriptor_type);

typedef struct {
    uint32_t set, binding;
    uint32_t type;        // VkDescriptorType
    uint32_t count;       // descriptors in the binding (an unsized array in the shader takes this size)
    uint32_t word;        // its first word in the set's argument buffer
    uint32_t dynamic;     // a dynamic buffer: its first index in the auxiliary dynamic offsets, else VKMSL_NO_DYNAMIC
} vkmsl_binding;

typedef struct {
    uint32_t id;          // SpecId
    uint32_t size;        // 1 (bool as 32-bit), 4 or 8 bytes
    uint64_t value;
} vkmsl_spec;

enum {
    VKMSL_STAGE_VERTEX = 0, VKMSL_STAGE_TESS_CONTROL = 1, VKMSL_STAGE_TESS_EVALUATION = 2, VKMSL_STAGE_GEOMETRY = 3,
    VKMSL_STAGE_FRAGMENT = 4, VKMSL_STAGE_COMPUTE = 5
};

typedef struct {
    uint32_t stage;                       // VKMSL_STAGE_*: the entry point's execution model
    const char *entry;                    // its name (NULL: the first entry point of that stage)
    const vkmsl_binding *bindings;
    uint32_t binding_count;
    const vkmsl_spec *specs;
    uint32_t spec_count;
    // Vertex shaders: the type Metal fetches each input location as (0: the shader's own type). Metal converts a
    // vertex format only into a matching scalar class, so a location fed with UINT data to a float input is fetched
    // as uint and converted in the shader. Bits: 1 float, 2 int, 3 uint.
    uint8_t vertex_input_class[32];
    uint32_t flags;                       // VKMSL_FLAG_*
    uint32_t vary_slots;                  // geometry shaders: the user locations the vertex kernel wrote (its vary_locations)
} vkmsl_options;

enum {
    VKMSL_FLAG_NO_FLIP_Y = 1u << 0,       // keep gl_Position.y (Vulkan's clip y points down, Metal's up: negated)
    VKMSL_FLAG_RASTERIZER_DISCARD = 1u << 1,
    VKMSL_FLAG_VERTEX_KERNEL = 1u << 2    // a vertex shader as the kernel before a geometry shader
};

typedef struct {
    char *msl;                            // malloc-owned MSL source
    size_t msl_size;
    char entry_name[64];                  // the MSL function's name
    uint32_t workgroup_size[3];
    uint32_t sets_used;                   // bit per descriptor set
    uint8_t uses_push_constants, uses_aux, writes_depth, uses_discard, early_fragment_tests;
    uint8_t uses_sample_rate, writes_sample_mask, uses_layer, uses_viewport_index;
    uint32_t vertex_inputs;               // locations read
    uint32_t fragment_outputs;            // color locations written
    uint32_t clip_distances;              // number written
    uint32_t vary_locations;              // vertex kernels: user locations written (slots 1..n)
    uint32_t gs_in_vertices, gs_max_triangles, gs_out_slots, gs_invocations;   // geometry shaders
    char error[256];                      // why translation failed
} vkmsl_result;

enum { VKMSL_OK = 0, VKMSL_INVALID = -1, VKMSL_UNSUPPORTED = -2, VKMSL_OUT_OF_MEMORY = -3 };

int vkmsl_translate(const uint32_t *words, size_t word_count, const vkmsl_options *options, vkmsl_result *result);
void vkmsl_result_free(vkmsl_result *result);
// The descriptor bindings a module's entry point of the stage declares (types inferred from the variables; counts 0
// for unsized arrays, words 0). *count may exceed capacity.
int vkmsl_reflect(const uint32_t *words, size_t word_count, uint32_t stage, vkmsl_binding *out, uint32_t capacity,
                  uint32_t *count);

#endif
