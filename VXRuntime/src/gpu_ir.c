// Common ownership and recording for API-neutral GPU operations.
#include "gpu_ir.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t mip_extent(uint32_t value, uint32_t mip);

struct gpu_ir_resource {
    volatile uint32_t refs;
    uint32_t kind;
    union { uint64_t buffer_size; gpu_ir_texture_desc texture; } desc;
    void *backend_object;
    gpu_ir_backend_object_release release;
};

struct gpu_ir_buffer_view {
    volatile uint32_t refs;
    gpu_ir_resource *resource;
    gpu_ir_buffer_view_desc desc;
    void *backend_object;
    gpu_ir_backend_object_release release;
};

struct gpu_ir_pipeline_layout {
    volatile uint32_t refs;
    uint32_t binding_count, push_constant_count;
    gpu_ir_descriptor_binding *bindings;
    gpu_ir_push_constant_range *push_constants;
};

struct gpu_ir_descriptor_state {
    volatile uint32_t refs;
    gpu_ir_pipeline_layout *layout;
    uint32_t value_count;
    gpu_ir_descriptor_value *values;
};

struct gpu_ir_pipeline {
    volatile uint32_t refs;
    gpu_ir_pipeline_info info;
    gpu_ir_pipeline_layout *layout;
    void *backend_object;
    gpu_ir_backend_object_release release;
    gpu_ir_shader *stage_shaders[GPU_IR_STAGE_SLOT_COUNT];
};

struct gpu_ir_shader {
    volatile uint32_t refs;
    uint32_t source_api, stage_mask, entry_function_id;
    uint32_t threadgroup_size[3];
    uint32_t type_count, type_member_count, function_count, block_count;
    uint32_t instruction_count, operand_count, literal_word_count;
    gpu_ir_shader_type *types;
    gpu_ir_shader_member *type_members;
    gpu_ir_shader_function *functions;
    gpu_ir_shader_block *blocks;
    gpu_ir_shader_instruction *instructions;
    gpu_ir_shader_operand *operands;
    uint32_t *literal_words;
};

enum { GPU_IR_RESOURCE_BUFFER = 1, GPU_IR_RESOURCE_TEXTURE, GPU_IR_OP_COPY_BUFFER, GPU_IR_OP_UPDATE_BUFFER,
       GPU_IR_OP_FILL_BUFFER,
       GPU_IR_OP_COPY_RESOURCE,
       GPU_IR_OP_COPY_TEXTURE, GPU_IR_OP_RESOLVE_TEXTURE, GPU_IR_OP_BARRIER, GPU_IR_OP_DISPATCH,
       GPU_IR_OP_DISPATCH_INDIRECT, GPU_IR_OP_DISPATCH_INDIRECT_RANGE,
       GPU_IR_OP_DRAW, GPU_IR_OP_DRAW_INDIRECT, GPU_IR_OP_BIND_DESCRIPTOR_STATE,
       GPU_IR_OP_CLEAR_COLOR, GPU_IR_OP_CLEAR_DEPTH_STENCIL,
       GPU_IR_OP_BEGIN_RENDER_PASS, GPU_IR_OP_END_RENDER_PASS };
typedef struct {
    uint32_t op;
    gpu_ir_resource *src, *dst;
    gpu_ir_pipeline *pipeline;
    gpu_ir_descriptor_state *descriptor_state;
    gpu_ir_resource_access *accesses;
    uint32_t access_count;
    union {
        struct { uint64_t src_offset, dst_offset, size; } buffer;
        struct { uint64_t dst_offset, size; uint8_t *data; } update_buffer;
        struct {
            uint64_t dst_offset, size;
            uint32_t value, pattern_size;
            uint8_t pattern[GPU_IR_MAX_FILL_PATTERN_SIZE];
        } fill_buffer;
        struct {
            gpu_ir_texture_location src, dst;
            uint32_t width, height, depth;
        } texture;
        struct {
            gpu_ir_resolve_location src, dst;
            gpu_ir_resolve_region region;
            uint32_t mode;
        } resolve;
        gpu_ir_clear_color_info clear_color;
        gpu_ir_clear_depth_stencil_info clear_depth_stencil;
        gpu_ir_render_pass_desc render_pass;
        gpu_ir_barrier_desc barrier;
        struct { uint32_t x, y, z; } dispatch;
        gpu_ir_indirect_dispatch_info dispatch_indirect_range;
        gpu_ir_draw_info draw;
        gpu_ir_indirect_draw_info draw_indirect;
    } copy;
} gpu_ir_command;

int gpu_ir_vertex_format_info(uint32_t format, uint32_t *component_count, uint32_t *byte_size,
                              uint32_t *numeric_class) {
    uint32_t components = 0, bytes_per_component = 0, kind = 0;
    switch (format) {
    case GPU_IR_VERTEX_FORMAT_FLOAT32: components = 1; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_FLOAT; break;
    case GPU_IR_VERTEX_FORMAT_FLOAT32X2: components = 2; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_FLOAT; break;
    case GPU_IR_VERTEX_FORMAT_FLOAT32X3: components = 3; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_FLOAT; break;
    case GPU_IR_VERTEX_FORMAT_FLOAT32X4: components = 4; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_FLOAT; break;
    case GPU_IR_VERTEX_FORMAT_SINT32: components = 1; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_SINT; break;
    case GPU_IR_VERTEX_FORMAT_SINT32X2: components = 2; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_SINT; break;
    case GPU_IR_VERTEX_FORMAT_SINT32X3: components = 3; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_SINT; break;
    case GPU_IR_VERTEX_FORMAT_SINT32X4: components = 4; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_SINT; break;
    case GPU_IR_VERTEX_FORMAT_UINT32: components = 1; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_UINT; break;
    case GPU_IR_VERTEX_FORMAT_UINT32X2: components = 2; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_UINT; break;
    case GPU_IR_VERTEX_FORMAT_UINT32X3: components = 3; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_UINT; break;
    case GPU_IR_VERTEX_FORMAT_UINT32X4: components = 4; bytes_per_component = 4; kind = GPU_IR_VERTEX_NUMERIC_UINT; break;
    default: break;
    }
#define FORMAT_RANGE(base, scalar_size, numeric) \
    if (!components && format >= (base) && format <= (base) + 3) { \
        components = format - (base) + 1; bytes_per_component = (scalar_size); kind = (numeric); \
    }
    FORMAT_RANGE(GPU_IR_VERTEX_FORMAT_FLOAT16, 2, GPU_IR_VERTEX_NUMERIC_FLOAT)
    FORMAT_RANGE(GPU_IR_VERTEX_FORMAT_UNORM8, 1, GPU_IR_VERTEX_NUMERIC_FLOAT)
    FORMAT_RANGE(GPU_IR_VERTEX_FORMAT_SNORM8, 1, GPU_IR_VERTEX_NUMERIC_FLOAT)
    FORMAT_RANGE(GPU_IR_VERTEX_FORMAT_UINT8, 1, GPU_IR_VERTEX_NUMERIC_UINT)
    FORMAT_RANGE(GPU_IR_VERTEX_FORMAT_SINT8, 1, GPU_IR_VERTEX_NUMERIC_SINT)
    FORMAT_RANGE(GPU_IR_VERTEX_FORMAT_UNORM16, 2, GPU_IR_VERTEX_NUMERIC_FLOAT)
    FORMAT_RANGE(GPU_IR_VERTEX_FORMAT_SNORM16, 2, GPU_IR_VERTEX_NUMERIC_FLOAT)
    FORMAT_RANGE(GPU_IR_VERTEX_FORMAT_UINT16, 2, GPU_IR_VERTEX_NUMERIC_UINT)
    FORMAT_RANGE(GPU_IR_VERTEX_FORMAT_SINT16, 2, GPU_IR_VERTEX_NUMERIC_SINT)
#undef FORMAT_RANGE
    if (!components) return 0;
    if (component_count) *component_count = components;
    if (byte_size) *byte_size = components * bytes_per_component;
    if (numeric_class) *numeric_class = kind;
    return 1;
}

static int vertex_format_size(uint32_t format) {
    uint32_t byte_size = 0;
    return gpu_ir_vertex_format_info(format, NULL, &byte_size, NULL) ? (int)byte_size : 0;
}

static uint32_t vertex_attribute_size(const gpu_ir_vertex_attribute_desc *attribute) {
    if (!attribute) return 0;
    if (attribute->format == GPU_IR_VERTEX_FORMAT_OPAQUE)
        return attribute->byte_size && attribute->byte_size <= 16 ? attribute->byte_size : 0;
    uint32_t size = (uint32_t)vertex_format_size(attribute->format);
    return size && (!attribute->byte_size || attribute->byte_size == size) ? size : 0;
}

static int add_u64(uint64_t a, uint64_t b, uint64_t *out);
static int mul_u64(uint64_t a, uint64_t b, uint64_t *out);

static int vertex_input_valid(const gpu_ir_vertex_input_desc *input) {
    if (!input || input->binding_count > GPU_IR_MAX_VERTEX_BUFFERS ||
        input->attribute_count > GPU_IR_MAX_VERTEX_ATTRIBUTES ||
        (input->attribute_count && !input->binding_count)) return 0;
    for (uint32_t i = 0; i < input->binding_count; i++) {
        const gpu_ir_vertex_binding_desc *binding = &input->bindings[i];
        if (binding->binding >= GPU_IR_MAX_VERTEX_BUFFERS ||
            (binding->input_rate != GPU_IR_VERTEX_INPUT_RATE_VERTEX &&
             binding->input_rate != GPU_IR_VERTEX_INPUT_RATE_INSTANCE) || !binding->step_rate) return 0;
        for (uint32_t previous = 0; previous < i; previous++)
            if (input->bindings[previous].binding == binding->binding) return 0;
    }
    for (uint32_t i = 0; i < input->attribute_count; i++) {
        const gpu_ir_vertex_attribute_desc *attribute = &input->attributes[i];
        int has_binding = 0;
        if (!vertex_attribute_size(attribute)) return 0;
        for (uint32_t binding = 0; binding < input->binding_count; binding++)
            if (input->bindings[binding].binding == attribute->binding) { has_binding = 1; break; }
        if (!has_binding) return 0;
        for (uint32_t previous = 0; previous < i; previous++)
            if (input->attributes[previous].location == attribute->location) return 0;
    }
    return 1;
}

int gpu_ir_color_blend_state_valid(const gpu_ir_color_blend_state *state) {
    if (!state || state->attachment_count > GPU_IR_MAX_COLOR_ATTACHMENTS) return state == NULL;
    if (state->alpha_to_coverage_enabled > 1) return 0;
    for (uint32_t i = 0; i < state->attachment_count; i++) {
        const gpu_ir_color_blend_attachment *attachment = &state->attachments[i];
        if (attachment->enabled > 1 ||
            attachment->source_color_factor > GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE1_ALPHA ||
            attachment->destination_color_factor > GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE1_ALPHA ||
            attachment->source_alpha_factor > GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE1_ALPHA ||
            attachment->destination_alpha_factor > GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE1_ALPHA ||
            attachment->color_operation > GPU_IR_BLEND_OPERATION_MAXIMUM ||
            attachment->alpha_operation > GPU_IR_BLEND_OPERATION_MAXIMUM ||
            (attachment->write_mask & ~GPU_IR_COLOR_WRITE_ALL)) return 0;
    }
    return 1;
}

static const gpu_ir_vertex_binding_desc *vertex_binding_description(const gpu_ir_vertex_input_desc *input,
                                                                     uint32_t binding) {
    for (uint32_t i = 0; i < input->binding_count; i++)
        if (input->bindings[i].binding == binding) return &input->bindings[i];
    return NULL;
}

static const gpu_ir_vertex_buffer_binding *vertex_buffer_binding(const gpu_ir_vertex_buffer_binding *bindings,
                                                                  uint32_t count, uint32_t binding) {
    for (uint32_t i = 0; i < count; i++)
        if (bindings[i].binding == binding) return &bindings[i];
    return NULL;
}

static int validate_vertex_fetch_ranges(const gpu_ir_pipeline_info *info,
                                       const gpu_ir_vertex_buffer_binding *buffers, uint32_t buffer_count,
                                       uint32_t indexed, uint32_t vertex_count, uint32_t first_vertex,
                                       uint32_t instance_count, uint32_t first_instance,
                                       uint32_t counts_known) {
    if (!info || (!buffers && buffer_count) || indexed > 1 || counts_known > 1) return GPU_IR_INVALID;
    for (uint32_t i = 0; i < info->vertex_input.attribute_count; i++) {
        const gpu_ir_vertex_attribute_desc *attribute = &info->vertex_input.attributes[i];
        const gpu_ir_vertex_binding_desc *binding = vertex_binding_description(&info->vertex_input, attribute->binding);
        const gpu_ir_vertex_buffer_binding *buffer = vertex_buffer_binding(buffers, buffer_count, attribute->binding);
        uint32_t attribute_size = vertex_attribute_size(attribute);
        if (!binding || !buffer || !attribute_size) return GPU_IR_INVALID;
        uint32_t stride = buffer->stride ? buffer->stride : binding->stride;
        uint64_t element_index = 0;
        if (counts_known && binding->input_rate == GPU_IR_VERTEX_INPUT_RATE_VERTEX) {
            if (!indexed) {
                if (!vertex_count) continue;
                element_index = (uint64_t)first_vertex + vertex_count - 1u;
            } // Indexed values are GPU data and cannot be bounded at record time.
        } else if (counts_known && binding->input_rate == GPU_IR_VERTEX_INPUT_RATE_INSTANCE) {
            if (!instance_count) continue;
            uint64_t last_instance = (uint64_t)first_instance + instance_count - 1u;
            element_index = last_instance / binding->step_rate;
        }
        uint64_t fetch_end;
        if (!mul_u64(element_index, stride, &fetch_end) ||
            !add_u64(fetch_end, attribute->offset, &fetch_end) ||
            !add_u64(fetch_end, attribute_size, &fetch_end)) return GPU_IR_RANGE;
        if (fetch_end > buffer->size) return GPU_IR_RANGE;
    }
    return GPU_IR_OK;
}

gpu_ir_pipeline *gpu_ir_pipeline_create(const gpu_ir_pipeline_desc *desc) {
    if (!desc || !desc->backend_object || !desc->release ||
        (desc->info.stage_mask & ~GPU_IR_STAGE_ALL) || !desc->info.stage_mask) return NULL;
    if (desc->info.kind == GPU_IR_PIPELINE_COMPUTE) {
        if (desc->info.stage_mask != GPU_IR_STAGE_COMPUTE || !desc->info.threadgroup_size[0] ||
            !desc->info.threadgroup_size[1] || !desc->info.threadgroup_size[2] ||
            !vertex_input_valid(&desc->info.vertex_input) || desc->info.vertex_input.binding_count ||
            desc->info.vertex_input.attribute_count || desc->info.color_blend.attachment_count) return NULL;
    } else if (desc->info.kind == GPU_IR_PIPELINE_GRAPHICS) {
        uint32_t stages = desc->info.stage_mask;
        int mesh = (stages & GPU_IR_STAGE_MESH) != 0;
        if ((stages & GPU_IR_STAGE_COMPUTE) ||
            (mesh ? (stages & (GPU_IR_STAGE_VERTEX | GPU_IR_STAGE_TESS_CONTROL | GPU_IR_STAGE_TESS_EVALUATION | GPU_IR_STAGE_GEOMETRY)) != 0
                  : !(stages & GPU_IR_STAGE_VERTEX)) ||
            ((stages & GPU_IR_STAGE_TASK) && !mesh) ||
            (!!(stages & GPU_IR_STAGE_TESS_CONTROL) != !!(stages & GPU_IR_STAGE_TESS_EVALUATION)) ||
            desc->info.threadgroup_size[0] || desc->info.threadgroup_size[1] || desc->info.threadgroup_size[2] ||
            !vertex_input_valid(&desc->info.vertex_input) ||
            !gpu_ir_color_blend_state_valid(&desc->info.color_blend)) return NULL;
    } else return NULL;

    uint32_t shader_stages = 0;
    for (uint32_t i = 0; i < GPU_IR_STAGE_SLOT_COUNT; i++) {
        gpu_ir_shader *shader = desc->stage_shaders[i];
        if (!shader) continue;
        uint32_t stage = 1u << i;
        if (!(desc->info.stage_mask & stage) || gpu_ir_shader_stage(shader) != stage) return NULL;
        shader_stages |= stage;
    }
    // A graphics frontend can keep unsupported stages in its native representation while retaining the
    // stages it has lowered into common IR. Any supplied compute shader must still match the compute stage.
    if (desc->info.kind == GPU_IR_PIPELINE_COMPUTE && shader_stages &&
        shader_stages != desc->info.stage_mask) return NULL;
    if (shader_stages & GPU_IR_STAGE_COMPUTE) {
        uint32_t shader_size[3] = { 0, 0, 0 };
        gpu_ir_shader_threadgroup_size(desc->stage_shaders[GPU_IR_STAGE_SLOT_COMPUTE], shader_size);
        if ((shader_size[0] || shader_size[1] || shader_size[2]) &&
            (shader_size[0] != desc->info.threadgroup_size[0] ||
             shader_size[1] != desc->info.threadgroup_size[1] ||
             shader_size[2] != desc->info.threadgroup_size[2])) return NULL;
    }

    gpu_ir_pipeline *pipeline = calloc(1, sizeof *pipeline);
    if (!pipeline) return NULL;
    pipeline->refs = 1;
    pipeline->info = desc->info;
    pipeline->layout = desc->layout;
    gpu_ir_pipeline_layout_retain(pipeline->layout);
    pipeline->backend_object = desc->backend_object;
    pipeline->release = desc->release;
    for (uint32_t i = 0; i < GPU_IR_STAGE_SLOT_COUNT; i++) {
        pipeline->stage_shaders[i] = desc->stage_shaders[i];
        gpu_ir_shader_retain(pipeline->stage_shaders[i]);
    }
    return pipeline;
}

void gpu_ir_pipeline_retain(gpu_ir_pipeline *pipeline) {
    if (pipeline) __atomic_add_fetch(&pipeline->refs, 1, __ATOMIC_RELAXED);
}

void gpu_ir_pipeline_release(gpu_ir_pipeline *pipeline) {
    if (!pipeline || __atomic_sub_fetch(&pipeline->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (pipeline->release) pipeline->release(pipeline->backend_object);
    gpu_ir_pipeline_layout_release(pipeline->layout);
    for (uint32_t i = 0; i < GPU_IR_STAGE_SLOT_COUNT; i++)
        gpu_ir_shader_release(pipeline->stage_shaders[i]);
    free(pipeline);
}

const gpu_ir_pipeline_info *gpu_ir_pipeline_description(const gpu_ir_pipeline *pipeline) {
    return pipeline ? &pipeline->info : NULL;
}

const gpu_ir_pipeline_layout *gpu_ir_pipeline_layout_of(const gpu_ir_pipeline *pipeline) {
    return pipeline ? pipeline->layout : NULL;
}

void *gpu_ir_pipeline_backend_object(const gpu_ir_pipeline *pipeline) {
    return pipeline ? pipeline->backend_object : NULL;
}

const gpu_ir_shader *gpu_ir_pipeline_shader(const gpu_ir_pipeline *pipeline, uint32_t stage_mask) {
    if (!pipeline || !stage_mask || (stage_mask & (stage_mask - 1)) || (stage_mask & ~GPU_IR_STAGE_ALL))
        return NULL;
    for (uint32_t i = 0; i < GPU_IR_STAGE_SLOT_COUNT; i++)
        if (stage_mask == (1u << i)) return pipeline->stage_shaders[i];
    return NULL;
}

static int shader_range_valid(uint32_t first, uint32_t count, uint32_t total) {
    return first <= total && count <= total - first;
}

static void *copy_shader_array(const void *source, uint32_t count, size_t element_size) {
    if (!count) return NULL;
    if (!source || count > SIZE_MAX / element_size) return NULL;
    void *copy = malloc((size_t)count * element_size);
    if (copy) memcpy(copy, source, (size_t)count * element_size);
    return copy;
}

static size_t shader_id_hash(uint32_t id) {
    return (size_t)(id * 2654435761u);
}

static int shader_id_set_insert(uint32_t *slots, size_t capacity, uint32_t id) {
    if (!id) return 0;
    size_t index = shader_id_hash(id) & (capacity - 1);
    while (slots[index]) {
        if (slots[index] == id) return 0;
        index = (index + 1) & (capacity - 1);
    }
    slots[index] = id;
    return 1;
}

static int shader_id_set_contains(const uint32_t *slots, size_t capacity, uint32_t id) {
    if (!id) return 0;
    size_t index = shader_id_hash(id) & (capacity - 1);
    while (slots[index]) {
        if (slots[index] == id) return 1;
        index = (index + 1) & (capacity - 1);
    }
    return 0;
}

static const gpu_ir_shader_type *shader_find_type(const gpu_ir_shader_desc *desc, uint32_t id) {
    for (uint32_t i = 0; i < desc->type_count; i++)
        if (desc->types[i].id == id) return &desc->types[i];
    return NULL;
}

static const gpu_ir_shader_instruction *shader_find_instruction(const gpu_ir_shader_desc *desc, uint32_t id) {
    for (uint32_t i = 0; i < desc->instruction_count; i++)
        if (desc->instructions[i].result_id == id) return &desc->instructions[i];
    return NULL;
}

static int shader_instruction_u32_constant(const gpu_ir_shader_desc *desc,
                                           const gpu_ir_shader_instruction *instruction,
                                           uint32_t *value) {
    if (!desc || !instruction || !value || instruction->opcode != GPU_IR_SHADER_OP_CONSTANT ||
        instruction->operation != GPU_IR_SHADER_CONSTANT_SCALAR || instruction->flags ||
        instruction->operand_count || instruction->literal_word_count != 1) return 0;
    const gpu_ir_shader_type *type = shader_find_type(desc, instruction->result_type_id);
    if (!type || type->kind != GPU_IR_SHADER_TYPE_INT || type->bit_width != 32 || type->signedness) return 0;
    *value = desc->literal_words[instruction->first_literal];
    return 1;
}

static const gpu_ir_shader_function *shader_find_function_desc(const gpu_ir_shader_desc *desc, uint32_t id) {
    for (uint32_t i = 0; i < desc->function_count; i++)
        if (desc->functions[i].id == id) return &desc->functions[i];
    return NULL;
}

static int shader_opcode_is_terminator(uint32_t opcode) {
    return opcode == GPU_IR_SHADER_OP_BRANCH || opcode == GPU_IR_SHADER_OP_BRANCH_CONDITIONAL ||
           opcode == GPU_IR_SHADER_OP_SWITCH || opcode == GPU_IR_SHADER_OP_RETURN ||
           opcode == GPU_IR_SHADER_OP_RETURN_VALUE || opcode == GPU_IR_SHADER_OP_UNREACHABLE ||
           opcode == GPU_IR_SHADER_OP_TERMINATE_INVOCATION;
}

gpu_ir_shader *gpu_ir_shader_create(const gpu_ir_shader_desc *desc) {
    if (!desc || desc->source_api < GPU_IR_SHADER_SOURCE_DXBC ||
        desc->source_api > GPU_IR_SHADER_SOURCE_SPIRV || !desc->stage_mask ||
        (desc->stage_mask & ~GPU_IR_STAGE_ALL) || (desc->stage_mask & (desc->stage_mask - 1)) ||
        !desc->entry_function_id || !desc->types || !desc->type_count || !desc->functions ||
        !desc->function_count || !desc->blocks || !desc->block_count || !desc->instructions ||
        !desc->instruction_count || (desc->type_member_count && !desc->type_members) ||
        (desc->operand_count && !desc->operands) || (desc->literal_word_count && !desc->literal_words)) return NULL;
    uint32_t local_size_components = (desc->threadgroup_size[0] != 0) +
        (desc->threadgroup_size[1] != 0) + (desc->threadgroup_size[2] != 0);
    if ((desc->stage_mask == GPU_IR_STAGE_COMPUTE && local_size_components != 0 && local_size_components != 3) ||
        (desc->stage_mask != GPU_IR_STAGE_COMPUTE && local_size_components != 0)) return NULL;

    size_t id_count = (size_t)desc->type_count + desc->function_count;
    if (id_count < desc->type_count || desc->block_count > SIZE_MAX - id_count) return NULL;
    id_count += desc->block_count;
    if (desc->instruction_count > SIZE_MAX - id_count) return NULL;
    for (uint32_t i = 0; i < desc->instruction_count; i++)
        if (desc->instructions[i].result_id && ++id_count == 0) return NULL;
    size_t id_capacity = 64;
    while (id_count >= id_capacity - id_capacity / 3) {
        if (id_capacity > SIZE_MAX / 2) return NULL;
        id_capacity *= 2;
    }
    if (id_capacity > SIZE_MAX / sizeof(uint32_t)) return NULL;
    uint32_t *id_slots = calloc(id_capacity, sizeof *id_slots);
    if (!id_slots) return NULL;
    int valid = 1;
    for (uint32_t i = 0; valid && i < desc->type_count; i++) {
        const gpu_ir_shader_type *type = &desc->types[i];
        valid = type->id && type->kind >= GPU_IR_SHADER_TYPE_VOID && type->kind <= GPU_IR_SHADER_TYPE_FUNCTION &&
            shader_range_valid(type->first_member, type->member_count, desc->type_member_count) &&
            shader_id_set_insert(id_slots, id_capacity, type->id) &&
            type->storage_class <= GPU_IR_SHADER_STORAGE_UNMAPPED;
        if (type->kind == GPU_IR_SHADER_TYPE_POINTER)
            valid &= type->storage_class != GPU_IR_SHADER_STORAGE_UNSPECIFIED;
        else
            valid &= type->storage_class == GPU_IR_SHADER_STORAGE_UNSPECIFIED && !type->source_storage_class;
        if (type->kind == GPU_IR_SHADER_TYPE_INT || type->kind == GPU_IR_SHADER_TYPE_FLOAT)
            valid &= type->bit_width != 0 && type->bit_width <= 64;
        if (type->kind == GPU_IR_SHADER_TYPE_VECTOR || type->kind == GPU_IR_SHADER_TYPE_MATRIX)
            valid &= type->element_type_id && type->element_count >= 2;
        if (type->kind == GPU_IR_SHADER_TYPE_ARRAY)
            valid &= type->element_type_id && type->array_length_id;
        if (type->kind == GPU_IR_SHADER_TYPE_RUNTIME_ARRAY || type->kind == GPU_IR_SHADER_TYPE_POINTER ||
            type->kind == GPU_IR_SHADER_TYPE_SAMPLED_IMAGE || type->kind == GPU_IR_SHADER_TYPE_IMAGE)
            valid &= type->element_type_id != 0;
        if (type->kind == GPU_IR_SHADER_TYPE_FUNCTION) valid &= type->element_type_id != 0;
        const uint32_t type_flags = GPU_IR_SHADER_TYPE_FLAG_BLOCK | GPU_IR_SHADER_TYPE_FLAG_BUFFER_BLOCK |
            GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE | GPU_IR_SHADER_TYPE_FLAG_NON_READABLE |
            GPU_IR_SHADER_TYPE_FLAG_ROW_MAJOR | GPU_IR_SHADER_TYPE_FLAG_COLUMN_MAJOR;
        valid &= !(type->decoration_flags & ~type_flags) && type->has_array_stride <= 1;
    }
    uint32_t entry_index = UINT32_MAX;
    for (uint32_t i = 0; valid && i < desc->function_count; i++) {
        const gpu_ir_shader_function *function = &desc->functions[i];
        valid = function->id && function->result_type_id &&
            shader_range_valid(function->first_block, function->block_count, desc->block_count) &&
            shader_range_valid(function->first_instruction, function->instruction_count, desc->instruction_count) &&
            shader_id_set_insert(id_slots, id_capacity, function->id);
        if (function->id == desc->entry_function_id) entry_index = i;
    }
    valid &= entry_index != UINT32_MAX && desc->functions[entry_index].block_count != 0;
    for (uint32_t i = 0; valid && i < desc->function_count; i++) {
        const gpu_ir_shader_function *function = &desc->functions[i];
        for (uint32_t b = function->first_block; b < function->first_block + function->block_count; b++)
            valid &= desc->blocks[b].function_index == i;
        for (uint32_t n = function->first_instruction; n < function->first_instruction + function->instruction_count; n++)
            valid &= desc->instructions[n].function_index == i;
        valid &= function->parameter_count <= function->instruction_count;
        const gpu_ir_shader_type *function_type = shader_find_type(desc, function->function_type_id);
        valid &= function_type && function_type->kind == GPU_IR_SHADER_TYPE_FUNCTION &&
                 function_type->member_count == function->parameter_count;
        for (uint32_t p = 0; valid && p < function->parameter_count; p++) {
            const gpu_ir_shader_instruction *parameter = &desc->instructions[function->first_instruction + p];
            valid &= parameter->opcode == GPU_IR_SHADER_OP_PARAMETER &&
                     parameter->block_index == UINT32_MAX &&
                     parameter->result_type_id == desc->type_members[function_type->first_member + p].type_id;
        }
    }
    for (uint32_t i = 0; valid && i < desc->block_count; i++) {
        const gpu_ir_shader_block *block = &desc->blocks[i];
        valid = block->function_index < desc->function_count && block->label_id &&
            shader_range_valid(block->first_instruction, block->instruction_count, desc->instruction_count);
        if (valid) valid &= shader_opcode_is_terminator(block->terminator_opcode) &&
                            shader_id_set_insert(id_slots, id_capacity, block->label_id);
        if (valid) {
            const gpu_ir_shader_function *function = &desc->functions[block->function_index];
            valid &= i >= function->first_block && i - function->first_block < function->block_count;
            valid &= block->first_instruction >= function->first_instruction &&
                     block->instruction_count <= function->instruction_count &&
                     block->first_instruction - function->first_instruction <=
                        function->instruction_count - block->instruction_count &&
                     block->instruction_count != 0;
        }
        for (uint32_t n = block->first_instruction; valid && n < block->first_instruction + block->instruction_count; n++)
            valid &= desc->instructions[n].function_index == block->function_index &&
                     desc->instructions[n].block_index == i;
        if (valid)
            valid &= desc->instructions[block->first_instruction + block->instruction_count - 1].opcode ==
                     block->terminator_opcode;
    }
    for (uint32_t i = 0; valid && i < desc->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = &desc->instructions[i];
        valid = instruction->source_api == desc->source_api &&
            instruction->opcode >= GPU_IR_SHADER_OP_UNDEFINED &&
            instruction->opcode <= GPU_IR_SHADER_OP_TERMINATE_INVOCATION &&
            shader_range_valid(instruction->first_operand, instruction->operand_count, desc->operand_count) &&
            shader_range_valid(instruction->first_literal, instruction->literal_word_count, desc->literal_word_count);
        if (instruction->function_index == UINT32_MAX) valid &= instruction->block_index == UINT32_MAX;
        else valid &= instruction->function_index < desc->function_count &&
            (instruction->block_index == UINT32_MAX ||
             (instruction->block_index < desc->block_count &&
              desc->blocks[instruction->block_index].function_index == instruction->function_index));
        if (valid && instruction->function_index != UINT32_MAX)
            valid &= instruction->block_index != UINT32_MAX || instruction->opcode == GPU_IR_SHADER_OP_PARAMETER;
        if (valid && instruction->result_id)
            valid &= shader_id_set_insert(id_slots, id_capacity, instruction->result_id);
        if (valid) {
            const uint32_t known_flags = GPU_IR_SHADER_FLAG_FLOAT | GPU_IR_SHADER_FLAG_SIGNED |
                GPU_IR_SHADER_FLAG_PRECISE | GPU_IR_SHADER_FLAG_NO_SIGNED_WRAP |
                GPU_IR_SHADER_FLAG_NO_UNSIGNED_WRAP | GPU_IR_SHADER_FLAG_FAST_MATH |
                GPU_IR_SHADER_FLAG_SPECIALIZATION | GPU_IR_SHADER_FLAG_DXIL_BITFIELD_LOW5 |
                GPU_IR_SHADER_FLAG_MINMAX_ORDERED_SELECT | GPU_IR_SHADER_FLAG_MINMAX_IGNORE_SINGLE_NAN |
                GPU_IR_SHADER_FLAG_MINMAX_FLUSH_DENORMS;
            valid = !(instruction->flags & ~known_flags);
            if (instruction->flags & GPU_IR_SHADER_FLAG_DXIL_BITFIELD_LOW5)
                valid &= instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL &&
                    instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                    (instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT ||
                     instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT ||
                     instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT);
            const uint32_t float_minmax_flags = GPU_IR_SHADER_FLAG_MINMAX_ORDERED_SELECT |
                GPU_IR_SHADER_FLAG_MINMAX_IGNORE_SINGLE_NAN | GPU_IR_SHADER_FLAG_MINMAX_FLUSH_DENORMS;
            if (instruction->flags & float_minmax_flags)
                valid &= (instruction->flags & float_minmax_flags) == float_minmax_flags &&
                    (instruction->flags & GPU_IR_SHADER_FLAG_FLOAT) &&
                    !(instruction->flags & GPU_IR_SHADER_FLAG_SIGNED) &&
                    instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                    (instruction->operation == GPU_IR_SHADER_ARITH_MINIMUM ||
                     instruction->operation == GPU_IR_SHADER_ARITH_MAXIMUM);
            const uint32_t known_memory_access = GPU_IR_SHADER_MEMORY_VOLATILE |
                GPU_IR_SHADER_MEMORY_ALIGNED | GPU_IR_SHADER_MEMORY_NONTEMPORAL;
            valid &= !(instruction->memory_access_flags & ~known_memory_access);
            if (instruction->memory_access_flags & GPU_IR_SHADER_MEMORY_ALIGNED)
                valid &= instruction->memory_alignment &&
                         !(instruction->memory_alignment & (instruction->memory_alignment - 1u));
            else valid &= instruction->memory_alignment == 0;
            if (instruction->memory_access_flags || instruction->memory_alignment)
                valid &= instruction->opcode == GPU_IR_SHADER_OP_LOAD ||
                         instruction->opcode == GPU_IR_SHADER_OP_STORE;
            if (instruction->opcode == GPU_IR_SHADER_OP_CONSTANT)
                valid &= instruction->operation >= GPU_IR_SHADER_CONSTANT_BOOL &&
                         instruction->operation <= GPU_IR_SHADER_CONSTANT_COMPOSITE;
            else if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE)
                valid &= instruction->operation >= GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC &&
                         instruction->operation <= GPU_IR_SHADER_COMPOSITE_TRANSPOSE;
            else if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC)
                valid &= instruction->operation >= GPU_IR_SHADER_ARITH_ADD &&
                         instruction->operation <= GPU_IR_SHADER_ARITH_MATRIX_TIMES_SCALAR &&
                         ((instruction->operation != GPU_IR_SHADER_ARITH_BIT_COUNT &&
                           instruction->operation != GPU_IR_SHADER_ARITH_BIT_REVERSE &&
                           instruction->operation != GPU_IR_SHADER_ARITH_BIT_FIND_LSB &&
                           instruction->operation != GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED &&
                           instruction->operation != GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SATURATE &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SUBGROUP_ANY &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SUBGROUP_ALL &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SUBGROUP_SMIN &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SUBGROUP_UMIN &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SUBGROUP_SMAX &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SUBGROUP_UMAX &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SUBGROUP_IMUL &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SUBGROUP_BIT_AND &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SUBGROUP_BIT_OR &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SUBGROUP_BIT_XOR) ||
                          (instruction->operand_count == 1 && !instruction->literal_word_count)) &&
                         (instruction->operation != GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT ||
                          (instruction->operand_count == 4 && !instruction->literal_word_count)) &&
                         ((instruction->operation != GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT &&
                           instruction->operation != GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT) ||
                          (instruction->operand_count == 3 && !instruction->literal_word_count)) &&
                         ((instruction->operation != GPU_IR_SHADER_ARITH_INTEGER_MAD &&
                           instruction->operation != GPU_IR_SHADER_ARITH_FLOAT_MAD &&
                           instruction->operation != GPU_IR_SHADER_ARITH_INTERPOLATE &&
                           instruction->operation != GPU_IR_SHADER_ARITH_SMOOTHSTEP) ||
                          (instruction->operand_count == 3 && !instruction->literal_word_count));
            else if (instruction->opcode == GPU_IR_SHADER_OP_COMPARE)
                valid &= instruction->operation >= GPU_IR_SHADER_COMPARE_EQUAL &&
                         instruction->operation <= GPU_IR_SHADER_COMPARE_UNORDERED_GREATER_EQUAL;
            else if (instruction->opcode == GPU_IR_SHADER_OP_CONVERT)
                valid &= instruction->operation >= GPU_IR_SHADER_CONVERT_FLOAT_TO_UNSIGNED &&
                         instruction->operation <= GPU_IR_SHADER_CONVERT_BITCAST;
            else if (instruction->opcode == GPU_IR_SHADER_OP_BARRIER)
                valid &= instruction->operation >= GPU_IR_SHADER_BARRIER_CONTROL &&
                         instruction->operation <= GPU_IR_SHADER_BARRIER_MEMORY;
            else if (instruction->opcode == GPU_IR_SHADER_OP_ARRAY_LENGTH)
                valid &= instruction->operation == 0 && !instruction->flags && instruction->operand_count == 1 &&
                         instruction->literal_word_count == 1;
            else if (instruction->opcode == GPU_IR_SHADER_OP_IMAGE_READ)
                valid &= !instruction->flags && instruction->result_id && instruction->result_type_id &&
                         ((instruction->operation == 0 && instruction->operand_count == 2 &&
                           !instruction->literal_word_count) ||
                          (instruction->operation == GPU_IR_SHADER_IMAGE_READ_2D_LOD_OOB &&
                           (instruction->operand_count == 4 || instruction->operand_count == 5) &&
                           instruction->literal_word_count == 3) ||
                          (instruction->operation == GPU_IR_SHADER_IMAGE_READ_2D_MSAA_OOB &&
                           (instruction->operand_count == 4 || instruction->operand_count == 5) &&
                           instruction->literal_word_count == 3));
            else if (instruction->opcode == GPU_IR_SHADER_OP_IMAGE_WRITE)
                valid &= instruction->operation == 0 && !instruction->flags && !instruction->result_id &&
                         !instruction->result_type_id && instruction->operand_count == 3 &&
                         !instruction->literal_word_count;
            else if (instruction->opcode == GPU_IR_SHADER_OP_SAMPLED_IMAGE)
                valid &= instruction->operation == 0 && !instruction->flags && instruction->result_id &&
                         instruction->result_type_id && instruction->operand_count == 2 &&
                         !instruction->literal_word_count;
            else if (instruction->opcode == GPU_IR_SHADER_OP_IMAGE_SAMPLE)
                valid &= !instruction->flags && instruction->result_id && instruction->result_type_id &&
                         ((instruction->operation == GPU_IR_SHADER_IMAGE_SAMPLE_EXPLICIT_LOD &&
                           instruction->operand_count == 3) ||
                          (instruction->operation == GPU_IR_SHADER_IMAGE_SAMPLE_IMPLICIT_LOD &&
                           desc->stage_mask == GPU_IR_STAGE_FRAGMENT && instruction->operand_count == 2)) &&
                         !instruction->literal_word_count;
            else if (instruction->opcode == GPU_IR_SHADER_OP_IMAGE_QUERY)
                valid &= (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SIZE ||
                          instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_LEVELS ||
                          instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_ARRAY_SIZE ||
                          instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SAMPLES) && !instruction->flags &&
                         instruction->result_id && instruction->result_type_id &&
                         (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SIZE ?
                          (instruction->operand_count == 1 || instruction->operand_count == 2) :
                          instruction->operand_count == 1) &&
                         !instruction->literal_word_count;
            else if (instruction->opcode == GPU_IR_SHADER_OP_ATOMIC)
                valid &= instruction->operation >= GPU_IR_SHADER_ATOMIC_INTEGER_ADD &&
                         instruction->operation <= GPU_IR_SHADER_ATOMIC_INTEGER_STORE &&
                         !instruction->flags &&
                         (instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_STORE ?
                          !instruction->result_id && !instruction->result_type_id :
                          instruction->result_id && instruction->result_type_id) &&
                         instruction->operand_count ==
                             (instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_COMPARE_EXCHANGE ? 6u :
                              instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_INCREMENT ||
                              instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_DECREMENT ||
                              instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_LOAD ? 3u : 4u) &&
                         !instruction->literal_word_count;
            else if (instruction->opcode == GPU_IR_SHADER_OP_DISCARD)
                valid &= desc->stage_mask == GPU_IR_STAGE_FRAGMENT && instruction->operation == 0 &&
                         !instruction->flags && !instruction->result_id && !instruction->result_type_id &&
                         instruction->operand_count == 1 && !instruction->literal_word_count;
            else if (instruction->opcode == GPU_IR_SHADER_OP_TERMINATE_INVOCATION)
                valid &= desc->stage_mask == GPU_IR_STAGE_FRAGMENT && instruction->operation == 0 &&
                         !instruction->flags && !instruction->result_id && !instruction->result_type_id &&
                         !instruction->operand_count && !instruction->literal_word_count;
            else if (instruction->opcode == GPU_IR_SHADER_OP_FUNCTION_CALL)
                valid &= instruction->operation == 0 && !instruction->flags && instruction->result_id &&
                         instruction->operand_count >= 1 && !instruction->literal_word_count;
            else valid &= instruction->operation == 0;
            if (instruction->result_id) valid &= instruction->result_type_id != 0;
            else valid &= instruction->result_type_id == 0;
            if (instruction->opcode == GPU_IR_SHADER_OP_CONSTANT) {
                valid &= !(instruction->flags & ~(GPU_IR_SHADER_FLAG_SPECIALIZATION));
                if (instruction->operation == GPU_IR_SHADER_CONSTANT_BOOL)
                    valid &= !instruction->operand_count && instruction->literal_word_count == 1 &&
                             desc->literal_words[instruction->first_literal] <= 1;
                else if (instruction->operation == GPU_IR_SHADER_CONSTANT_SCALAR) {
                    const gpu_ir_shader_type *type = shader_find_type(desc, instruction->result_type_id);
                    valid &= !instruction->operand_count && type &&
                        (type->kind == GPU_IR_SHADER_TYPE_INT || type->kind == GPU_IR_SHADER_TYPE_FLOAT) &&
                        instruction->literal_word_count == (type->bit_width + 31u) / 32u;
                } else if (instruction->operation == GPU_IR_SHADER_CONSTANT_NULL)
                    valid &= !instruction->operand_count && !instruction->literal_word_count;
                else
                    valid &= instruction->operand_count != 0 && !instruction->literal_word_count;
            } else if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE) {
                valid &= !(instruction->flags & ~GPU_IR_SHADER_FLAG_SPECIALIZATION);
                switch (instruction->operation) {
                case GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC:
                    valid &= instruction->operand_count == 2 && !instruction->literal_word_count;
                    break;
                case GPU_IR_SHADER_COMPOSITE_VECTOR_INSERT_DYNAMIC:
                    valid &= instruction->operand_count == 3 && !instruction->literal_word_count;
                    break;
                case GPU_IR_SHADER_COMPOSITE_VECTOR_SHUFFLE: {
                    const gpu_ir_shader_type *type = shader_find_type(desc, instruction->result_type_id);
                    valid &= instruction->operand_count == 2 && type &&
                        type->kind == GPU_IR_SHADER_TYPE_VECTOR &&
                        instruction->literal_word_count == type->element_count;
                    break;
                }
                case GPU_IR_SHADER_COMPOSITE_CONSTRUCT:
                    valid &= instruction->operand_count != 0 && !instruction->literal_word_count;
                    break;
                case GPU_IR_SHADER_COMPOSITE_EXTRACT:
                    valid &= instruction->operand_count == 1 && instruction->literal_word_count != 0;
                    break;
                case GPU_IR_SHADER_COMPOSITE_INSERT:
                    valid &= instruction->operand_count == 2 && instruction->literal_word_count != 0;
                    break;
                case GPU_IR_SHADER_COMPOSITE_TRANSPOSE:
                    valid &= instruction->operand_count == 1 && !instruction->literal_word_count;
                    break;
                default: valid = 0; break;
                }
            } else if (instruction->opcode == GPU_IR_SHADER_OP_BARRIER) {
                valid &= !instruction->flags && !instruction->result_id && !instruction->result_type_id &&
                         !instruction->literal_word_count &&
                         instruction->operand_count == (instruction->operation == GPU_IR_SHADER_BARRIER_CONTROL ? 3u : 2u);
                for (uint32_t operand = instruction->first_operand;
                     valid && operand < instruction->first_operand + instruction->operand_count; operand++)
                    valid &= desc->operands[operand].kind == GPU_IR_SHADER_OPERAND_ID;
            } else valid &= !(instruction->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION);
            valid &= instruction->has_descriptor_set <= 1 && instruction->has_binding <= 1 &&
                     instruction->has_location <= 1 && instruction->has_component <= 1 &&
                     instruction->has_index <= 1 && instruction->has_builtin <= 1 &&
                     instruction->entry_point_interface <= 1;
            if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE) {
                valid &= instruction->storage_class > GPU_IR_SHADER_STORAGE_UNSPECIFIED &&
                         instruction->storage_class <= GPU_IR_SHADER_STORAGE_UNMAPPED;
                if (instruction->entry_point_interface)
                    valid &= (instruction->source_api == GPU_IR_SHADER_SOURCE_SPIRV ||
                              instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL) &&
                        (instruction->storage_class == GPU_IR_SHADER_STORAGE_INPUT ||
                         instruction->storage_class == GPU_IR_SHADER_STORAGE_OUTPUT);
                valid &= instruction->has_builtin ?
                    (instruction->builtin > GPU_IR_SHADER_BUILTIN_UNSPECIFIED &&
                     instruction->builtin <= GPU_IR_SHADER_BUILTIN_UNMAPPED) :
                    (instruction->builtin == GPU_IR_SHADER_BUILTIN_UNSPECIFIED && !instruction->source_builtin);
            } else
                valid &= !instruction->storage_class && !instruction->source_storage_class &&
                         instruction->builtin == GPU_IR_SHADER_BUILTIN_UNSPECIFIED && !instruction->source_builtin &&
                         !instruction->has_descriptor_set &&
                         !instruction->has_binding && !instruction->has_location &&
                         !instruction->has_component && !instruction->has_index && !instruction->has_builtin &&
                         !instruction->entry_point_interface && !instruction->decoration_flags;
        }
    }
    for (uint32_t i = 0; valid && i < desc->type_count; i++) {
        const gpu_ir_shader_type *type = &desc->types[i];
        if (type->element_type_id) valid &= shader_find_type(desc, type->element_type_id) != NULL;
        if (type->kind == GPU_IR_SHADER_TYPE_FUNCTION) valid &= shader_find_type(desc, type->element_type_id) != NULL;
        if (type->kind == GPU_IR_SHADER_TYPE_ARRAY) valid &= shader_id_set_contains(id_slots, id_capacity, type->array_length_id);
        for (uint32_t member = type->first_member; valid && member < type->first_member + type->member_count; member++)
            valid &= shader_find_type(desc, desc->type_members[member].type_id) != NULL &&
                     !(desc->type_members[member].decoration_flags & ~(
                        GPU_IR_SHADER_TYPE_FLAG_BLOCK | GPU_IR_SHADER_TYPE_FLAG_BUFFER_BLOCK |
                        GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE | GPU_IR_SHADER_TYPE_FLAG_NON_READABLE |
                        GPU_IR_SHADER_TYPE_FLAG_ROW_MAJOR | GPU_IR_SHADER_TYPE_FLAG_COLUMN_MAJOR)) &&
                     desc->type_members[member].has_offset <= 1 &&
                     desc->type_members[member].has_matrix_stride <= 1 &&
                     desc->type_members[member].has_location <= 1 &&
                     desc->type_members[member].has_component <= 1 &&
                     desc->type_members[member].has_index <= 1 &&
                     desc->type_members[member].has_builtin <= 1 &&
                     (desc->type_members[member].has_builtin ?
                        (desc->type_members[member].builtin > GPU_IR_SHADER_BUILTIN_UNSPECIFIED &&
                         desc->type_members[member].builtin <= GPU_IR_SHADER_BUILTIN_UNMAPPED) :
                        (desc->type_members[member].builtin == GPU_IR_SHADER_BUILTIN_UNSPECIFIED &&
                         !desc->type_members[member].source_builtin));
    }
    for (uint32_t i = 0; valid && i < desc->function_count; i++) {
        const gpu_ir_shader_function *function = &desc->functions[i];
        const gpu_ir_shader_type *result_type = shader_find_type(desc, function->result_type_id);
        const gpu_ir_shader_type *function_type = shader_find_type(desc, function->function_type_id);
        valid &= result_type && function_type && function_type->kind == GPU_IR_SHADER_TYPE_FUNCTION &&
                 function_type->element_type_id == function->result_type_id &&
                 function_type->member_count == function->parameter_count;
    }
    for (uint32_t i = 0; valid && i < desc->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = &desc->instructions[i];
        if (instruction->result_type_id) valid &= shader_find_type(desc, instruction->result_type_id) != NULL;
        if (valid && instruction->opcode == GPU_IR_SHADER_OP_VARIABLE) {
            const gpu_ir_shader_type *pointer = shader_find_type(desc, instruction->result_type_id);
            valid &= pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER &&
                     pointer->storage_class == instruction->storage_class;
            uint32_t variable_flags = GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE |
                GPU_IR_SHADER_TYPE_FLAG_NON_READABLE;
            if (instruction->entry_point_interface && instruction->source_api == GPU_IR_SHADER_SOURCE_SPIRV)
                variable_flags |= GPU_IR_SHADER_DECORATION_NO_PERSPECTIVE | GPU_IR_SHADER_DECORATION_FLAT |
                    GPU_IR_SHADER_DECORATION_CENTROID | GPU_IR_SHADER_DECORATION_SAMPLE |
                    GPU_IR_SHADER_DECORATION_INVARIANT;
            valid &= !(instruction->decoration_flags & ~variable_flags);
        }
        if (valid && instruction->opcode == GPU_IR_SHADER_OP_ARRAY_LENGTH) {
            const gpu_ir_shader_operand *operand = &desc->operands[instruction->first_operand];
            const gpu_ir_shader_instruction *structure_value = operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, operand->value) : NULL;
            const gpu_ir_shader_type *pointer = structure_value ?
                shader_find_type(desc, structure_value->result_type_id) : NULL;
            const gpu_ir_shader_type *structure = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
                shader_find_type(desc, pointer->element_type_id) : NULL;
            const gpu_ir_shader_type *result = shader_find_type(desc, instruction->result_type_id);
            uint32_t member_index = desc->literal_words[instruction->first_literal];
            const gpu_ir_shader_member *member = structure && member_index < structure->member_count ?
                &desc->type_members[structure->first_member + member_index] : NULL;
            const gpu_ir_shader_type *array = member ? shader_find_type(desc, member->type_id) : NULL;
            valid &= structure_value && pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER && structure &&
                structure->kind == GPU_IR_SHADER_TYPE_STRUCT && (structure->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_BLOCK) &&
                structure->member_count && member_index == structure->member_count - 1 && member && array &&
                array->kind == GPU_IR_SHADER_TYPE_RUNTIME_ARRAY && array->has_array_stride && array->array_stride &&
                result && result->kind == GPU_IR_SHADER_TYPE_INT &&
                (result->bit_width == 32 || result->bit_width == 64) && !result->signedness;
        }
        if (valid && instruction->opcode == GPU_IR_SHADER_OP_IMAGE_QUERY &&
            instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SIZE && instruction->operand_count == 2) {
            const gpu_ir_shader_operand *lod_operand = &desc->operands[instruction->first_operand + 1u];
            const gpu_ir_shader_instruction *lod_value = lod_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, lod_operand->value) : NULL;
            const gpu_ir_shader_type *lod_type = lod_value ? shader_find_type(desc, lod_value->result_type_id) : NULL;
            valid &= lod_type && lod_type->kind == GPU_IR_SHADER_TYPE_INT && lod_type->bit_width == 32;
        }
        if (valid && instruction->opcode == GPU_IR_SHADER_OP_IMAGE_QUERY &&
            (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_LEVELS ||
             instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_ARRAY_SIZE ||
             instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SAMPLES)) {
            const gpu_ir_shader_type *result = shader_find_type(desc, instruction->result_type_id);
            valid &= result && result->kind == GPU_IR_SHADER_TYPE_INT && result->bit_width == 32;
        }
        if (valid && instruction->opcode == GPU_IR_SHADER_OP_IMAGE_QUERY &&
            instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SAMPLES) {
            const gpu_ir_shader_operand *image_operand = &desc->operands[instruction->first_operand];
            const gpu_ir_shader_instruction *image_value = image_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, image_operand->value) : NULL;
            const gpu_ir_shader_type *image = image_value ?
                shader_find_type(desc, image_value->result_type_id) : NULL;
            const gpu_ir_shader_type *component = image && image->kind == GPU_IR_SHADER_TYPE_IMAGE ?
                shader_find_type(desc, image->element_type_id) : NULL;
            valid &= image_value && image && image->kind == GPU_IR_SHADER_TYPE_IMAGE && component &&
                component->kind == GPU_IR_SHADER_TYPE_FLOAT && component->bit_width == 32 &&
                image->image_dimension == 1 && !image->image_depth && image->image_arrayed <= 1 &&
                image->image_multisampled == 1 && image->image_sampled == 1 &&
                !image->image_format && !image->image_access_qualifier;
        }
        if (valid && instruction->opcode == GPU_IR_SHADER_OP_IMAGE_READ &&
            instruction->operation == GPU_IR_SHADER_IMAGE_READ_2D_LOD_OOB) {
            const gpu_ir_shader_operand *image_operand = &desc->operands[instruction->first_operand];
            const gpu_ir_shader_instruction *image_value = image_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, image_operand->value) : NULL;
            const gpu_ir_shader_type *image = image_value ?
                shader_find_type(desc, image_value->result_type_id) : NULL;
            const gpu_ir_shader_type *component = image && image->kind == GPU_IR_SHADER_TYPE_IMAGE ?
                shader_find_type(desc, image->element_type_id) : NULL;
            const gpu_ir_shader_type *result = shader_find_type(desc, instruction->result_type_id);
            int integer_read = component && component->kind == GPU_IR_SHADER_TYPE_INT &&
                component->bit_width == 32 && component->signedness <= 1 && result &&
                result->kind == GPU_IR_SHADER_TYPE_INT && result->bit_width == 32 && result->signedness <= 1;
            int float_read = component && component->kind == GPU_IR_SHADER_TYPE_FLOAT &&
                component->bit_width == 32 && result && result->kind == GPU_IR_SHADER_TYPE_FLOAT &&
                result->bit_width == 32;
            valid &= image_value && image && image->kind == GPU_IR_SHADER_TYPE_IMAGE && component &&
                (integer_read || float_read) && image->image_dimension == 1 && !image->image_depth &&
                image->image_arrayed <= 1 &&
                instruction->operand_count == (image->image_arrayed ? 5u : 4u) &&
                !image->image_multisampled && image->image_sampled == 1 &&
                !image->image_format && !image->image_access_qualifier;
            valid &= desc->literal_words[instruction->first_literal + 2u] < 4u;
            for (uint32_t coordinate = 1; valid && coordinate < instruction->operand_count; coordinate++) {
                const gpu_ir_shader_operand *coordinate_operand =
                    &desc->operands[instruction->first_operand + coordinate];
                const gpu_ir_shader_instruction *coordinate_value =
                    coordinate_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                        shader_find_instruction(desc, coordinate_operand->value) : NULL;
                const gpu_ir_shader_type *coordinate_type = coordinate_value ?
                    shader_find_type(desc, coordinate_value->result_type_id) : NULL;
                valid &= coordinate_value && coordinate_type && coordinate_type->kind == GPU_IR_SHADER_TYPE_INT &&
                    coordinate_type->bit_width == 32;
            }
        }
        if (valid && instruction->opcode == GPU_IR_SHADER_OP_IMAGE_READ &&
            instruction->operation == GPU_IR_SHADER_IMAGE_READ_2D_MSAA_OOB) {
            const gpu_ir_shader_operand *image_operand = &desc->operands[instruction->first_operand];
            const gpu_ir_shader_instruction *image_value = image_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, image_operand->value) : NULL;
            const gpu_ir_shader_type *image = image_value ?
                shader_find_type(desc, image_value->result_type_id) : NULL;
            const gpu_ir_shader_type *component = image && image->kind == GPU_IR_SHADER_TYPE_IMAGE ?
                shader_find_type(desc, image->element_type_id) : NULL;
            const gpu_ir_shader_type *result = shader_find_type(desc, instruction->result_type_id);
            valid &= image_value && image && image->kind == GPU_IR_SHADER_TYPE_IMAGE && component &&
                component->kind == GPU_IR_SHADER_TYPE_FLOAT && component->bit_width == 32 &&
                result && result->kind == GPU_IR_SHADER_TYPE_FLOAT && result->bit_width == 32 &&
                image->image_dimension == 1 && !image->image_depth && image->image_arrayed <= 1 &&
                instruction->operand_count == (image->image_arrayed ? 5u : 4u) &&
                image->image_multisampled == 1 && image->image_sampled == 1 &&
                !image->image_format && !image->image_access_qualifier &&
                desc->literal_words[instruction->first_literal + 2u] < 4u;
            for (uint32_t coordinate = 1; valid && coordinate < instruction->operand_count; coordinate++) {
                const gpu_ir_shader_operand *coordinate_operand =
                    &desc->operands[instruction->first_operand + coordinate];
                const gpu_ir_shader_instruction *coordinate_value = coordinate_operand->kind ==
                    GPU_IR_SHADER_OPERAND_ID ? shader_find_instruction(desc, coordinate_operand->value) : NULL;
                const gpu_ir_shader_type *coordinate_type = coordinate_value ?
                    shader_find_type(desc, coordinate_value->result_type_id) : NULL;
                valid &= coordinate_value && coordinate_type && coordinate_type->kind == GPU_IR_SHADER_TYPE_INT &&
                    coordinate_type->bit_width == 32;
            }
        }
    }
    for (uint32_t i = 0; valid && i < desc->operand_count; i++) {
        valid = desc->operands[i].kind == GPU_IR_SHADER_OPERAND_ID ||
                desc->operands[i].kind == GPU_IR_SHADER_OPERAND_LITERAL;
        if (valid && desc->operands[i].kind == GPU_IR_SHADER_OPERAND_ID)
            valid &= shader_id_set_contains(id_slots, id_capacity, desc->operands[i].value);
    }
    for (uint32_t i = 0; valid && i < desc->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = &desc->instructions[i];
        if (instruction->opcode == GPU_IR_SHADER_OP_DISCARD) {
            const gpu_ir_shader_operand *condition_operand = &desc->operands[instruction->first_operand];
            const gpu_ir_shader_instruction *condition = condition_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, condition_operand->value) : NULL;
            const gpu_ir_shader_type *condition_type = condition ?
                shader_find_type(desc, condition->result_type_id) : NULL;
            valid = condition && condition_type && condition_type->kind == GPU_IR_SHADER_TYPE_BOOL;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_ATOMIC) {
            const gpu_ir_shader_operand *pointer_operand = &desc->operands[instruction->first_operand];
            const gpu_ir_shader_operand *scope_operand = &desc->operands[instruction->first_operand + 1u];
            const gpu_ir_shader_operand *semantics_operand = &desc->operands[instruction->first_operand + 2u];
            int compare_exchange = instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_COMPARE_EXCHANGE;
            int increment_decrement = instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_INCREMENT ||
                                      instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_DECREMENT;
            int atomic_load = instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_LOAD;
            int atomic_store = instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_STORE;
            const gpu_ir_shader_operand *failure_semantics_operand = compare_exchange ?
                &desc->operands[instruction->first_operand + 3u] : NULL;
            const gpu_ir_shader_operand *compare_operand = compare_exchange ?
                &desc->operands[instruction->first_operand + 4u] : NULL;
            const gpu_ir_shader_operand *value_operand = (increment_decrement || atomic_load) ? NULL :
                &desc->operands[instruction->first_operand + (compare_exchange ? 5u : 3u)];
            const gpu_ir_shader_instruction *pointer_value = pointer_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, pointer_operand->value) : NULL;
            const gpu_ir_shader_instruction *scope_value = scope_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, scope_operand->value) : NULL;
            const gpu_ir_shader_instruction *semantics_value = semantics_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, semantics_operand->value) : NULL;
            const gpu_ir_shader_instruction *failure_semantics_value = failure_semantics_operand &&
                failure_semantics_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, failure_semantics_operand->value) : NULL;
            const gpu_ir_shader_instruction *compare_value = compare_operand &&
                compare_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, compare_operand->value) : NULL;
            const gpu_ir_shader_instruction *value = value_operand &&
                value_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, value_operand->value) : NULL;
            const gpu_ir_shader_type *pointer_type = pointer_value ?
                shader_find_type(desc, pointer_value->result_type_id) : NULL;
            uint32_t atomic_type_id = atomic_store && pointer_type &&
                pointer_type->kind == GPU_IR_SHADER_TYPE_POINTER ? pointer_type->element_type_id :
                instruction->result_type_id;
            const gpu_ir_shader_type *result_type = shader_find_type(desc, atomic_type_id);
            uint32_t scope = 0, semantics = 0, failure_semantics = 0;
            valid = pointer_type && pointer_type->kind == GPU_IR_SHADER_TYPE_POINTER &&
                (atomic_store || pointer_type->element_type_id == instruction->result_type_id) &&
                pointer_type->element_type_id == atomic_type_id && result_type &&
                result_type->kind == GPU_IR_SHADER_TYPE_INT && result_type->bit_width == 32 &&
                result_type->signedness <= 1 && ((increment_decrement || atomic_load) ||
                    (value && value->result_type_id == atomic_type_id)) &&
                (!compare_exchange || (compare_value &&
                    compare_value->result_type_id == instruction->result_type_id)) &&
                shader_instruction_u32_constant(desc, scope_value, &scope) &&
                shader_instruction_u32_constant(desc, semantics_value, &semantics) &&
                (!compare_exchange || shader_instruction_u32_constant(desc, failure_semantics_value,
                                                                        &failure_semantics)) &&
                scope <= 6u && semantics <= 0xffffu && failure_semantics <= 0xffffu;
        }
    }
    for (uint32_t i = 0; valid && i < desc->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = &desc->instructions[i];
        if (instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC) continue;
        int subgroup_integer_reduction = instruction->operation >= GPU_IR_SHADER_ARITH_SUBGROUP_SMIN &&
            instruction->operation <= GPU_IR_SHADER_ARITH_SUBGROUP_BIT_XOR;
        if (subgroup_integer_reduction) {
            const gpu_ir_shader_operand *operand = &desc->operands[instruction->first_operand];
            const gpu_ir_shader_instruction *value = operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, operand->value) : NULL;
            const gpu_ir_shader_type *result = shader_find_type(desc, instruction->result_type_id);
            const gpu_ir_shader_type *input = value ? shader_find_type(desc, value->result_type_id) : NULL;
            int signed_operation = instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_SMIN ||
                                   instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_SMAX;
            int dxil_signless_integer = instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL &&
                result && result->kind == GPU_IR_SHADER_TYPE_INT && result->bit_width == 32;
            valid = value && input && result && result->kind == GPU_IR_SHADER_TYPE_INT &&
                input->kind == GPU_IR_SHADER_TYPE_INT && result->bit_width == 32 && input->bit_width == 32 &&
                result->signedness <= 1 && input->signedness <= 1 &&
                (!signed_operation || ((result->signedness == (uint32_t)signed_operation || dxil_signless_integer) &&
                                       (input->signedness == (uint32_t)signed_operation || dxil_signless_integer))) &&
                value->result_type_id == instruction->result_type_id;
            continue;
        }
        int bit_count = instruction->operation == GPU_IR_SHADER_ARITH_BIT_COUNT;
        int bit_reverse = instruction->operation == GPU_IR_SHADER_ARITH_BIT_REVERSE;
        int bit_find = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIND_LSB ||
            instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED ||
            instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED;
        int bit_field_insert = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT;
        int bit_field_extract = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT ||
                                instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT;
        if (!bit_count && !bit_reverse && !bit_find && !bit_field_insert && !bit_field_extract) continue;
        const gpu_ir_shader_operand *operand = &desc->operands[instruction->first_operand];
        const gpu_ir_shader_instruction *base = operand->kind == GPU_IR_SHADER_OPERAND_ID ?
            shader_find_instruction(desc, operand->value) : NULL;
        const gpu_ir_shader_type *result = shader_find_type(desc, instruction->result_type_id);
        const gpu_ir_shader_type *input = base ? shader_find_type(desc, base->result_type_id) : NULL;
        const gpu_ir_shader_type *result_scalar = result && result->kind == GPU_IR_SHADER_TYPE_VECTOR ?
            shader_find_type(desc, result->element_type_id) : result;
        const gpu_ir_shader_type *input_scalar = input && input->kind == GPU_IR_SHADER_TYPE_VECTOR ?
            shader_find_type(desc, input->element_type_id) : input;
        valid = base && result && input && result_scalar && input_scalar &&
            (result->kind == GPU_IR_SHADER_TYPE_INT || result->kind == GPU_IR_SHADER_TYPE_VECTOR) &&
            (input->kind == GPU_IR_SHADER_TYPE_INT || input->kind == GPU_IR_SHADER_TYPE_VECTOR) &&
            result->kind == input->kind &&
            (result->kind != GPU_IR_SHADER_TYPE_VECTOR || result->element_count == input->element_count) &&
            result_scalar->kind == GPU_IR_SHADER_TYPE_INT && input_scalar->kind == GPU_IR_SHADER_TYPE_INT &&
            result_scalar->bit_width == 32 && input_scalar->bit_width == 32;
        if (valid && (bit_reverse || bit_field_insert || bit_field_extract))
            valid = base->result_type_id == instruction->result_type_id;
        if (valid && bit_field_insert) {
            const gpu_ir_shader_operand *insert_operand = &desc->operands[instruction->first_operand + 1u];
            const gpu_ir_shader_operand *offset_operand = &desc->operands[instruction->first_operand + 2u];
            const gpu_ir_shader_operand *count_operand = &desc->operands[instruction->first_operand + 3u];
            const gpu_ir_shader_instruction *insert = insert_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, insert_operand->value) : NULL;
            const gpu_ir_shader_instruction *offset = offset_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, offset_operand->value) : NULL;
            const gpu_ir_shader_instruction *count = count_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, count_operand->value) : NULL;
            const gpu_ir_shader_type *offset_type = offset ? shader_find_type(desc, offset->result_type_id) : NULL;
            const gpu_ir_shader_type *count_type = count ? shader_find_type(desc, count->result_type_id) : NULL;
            valid = insert && offset_type && count_type && insert->result_type_id == instruction->result_type_id &&
                offset_type->kind == GPU_IR_SHADER_TYPE_INT && offset_type->bit_width == 32 &&
                count_type->kind == GPU_IR_SHADER_TYPE_INT && count_type->bit_width == 32;
        } else if (valid && bit_field_extract) {
            const gpu_ir_shader_operand *offset_operand = &desc->operands[instruction->first_operand + 1u];
            const gpu_ir_shader_operand *count_operand = &desc->operands[instruction->first_operand + 2u];
            const gpu_ir_shader_instruction *offset = offset_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, offset_operand->value) : NULL;
            const gpu_ir_shader_instruction *count = count_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, count_operand->value) : NULL;
            const gpu_ir_shader_type *offset_type = offset ? shader_find_type(desc, offset->result_type_id) : NULL;
            const gpu_ir_shader_type *count_type = count ? shader_find_type(desc, count->result_type_id) : NULL;
            valid = offset_type && count_type && offset_type->kind == GPU_IR_SHADER_TYPE_INT &&
                offset_type->bit_width == 32 && count_type->kind == GPU_IR_SHADER_TYPE_INT &&
                count_type->bit_width == 32;
        }
    }
    for (uint32_t i = 0; valid && i < desc->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = &desc->instructions[i];
        if (instruction->opcode != GPU_IR_SHADER_OP_FUNCTION_CALL) continue;
        const gpu_ir_shader_operand *function_operand = &desc->operands[instruction->first_operand];
        const gpu_ir_shader_function *callee = function_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
            shader_find_function_desc(desc, function_operand->value) : NULL;
        valid = callee && instruction->result_type_id == callee->result_type_id &&
                instruction->operand_count > 0 && instruction->operand_count - 1u == callee->parameter_count;
        for (uint32_t p = 0; valid && p < callee->parameter_count; p++) {
            const gpu_ir_shader_operand *argument_operand =
                &desc->operands[instruction->first_operand + p + 1u];
            const gpu_ir_shader_instruction *argument = argument_operand->kind == GPU_IR_SHADER_OPERAND_ID ?
                shader_find_instruction(desc, argument_operand->value) : NULL;
            const gpu_ir_shader_instruction *parameter =
                &desc->instructions[callee->first_instruction + p];
            valid = argument && parameter->opcode == GPU_IR_SHADER_OP_PARAMETER &&
                    argument->result_type_id == parameter->result_type_id;
        }
    }
    free(id_slots);
    if (!valid) return NULL;

    gpu_ir_shader *shader = calloc(1, sizeof *shader);
    if (!shader) return NULL;
    shader->source_api = desc->source_api;
    shader->stage_mask = desc->stage_mask;
    shader->entry_function_id = desc->entry_function_id;
    memcpy(shader->threadgroup_size, desc->threadgroup_size, sizeof shader->threadgroup_size);
    shader->type_count = desc->type_count;
    shader->type_member_count = desc->type_member_count;
    shader->function_count = desc->function_count;
    shader->block_count = desc->block_count;
    shader->instruction_count = desc->instruction_count;
    shader->operand_count = desc->operand_count;
    shader->literal_word_count = desc->literal_word_count;
    shader->types = copy_shader_array(desc->types, shader->type_count, sizeof *shader->types);
    shader->type_members = copy_shader_array(desc->type_members, shader->type_member_count, sizeof *shader->type_members);
    shader->functions = copy_shader_array(desc->functions, shader->function_count, sizeof *shader->functions);
    shader->blocks = copy_shader_array(desc->blocks, shader->block_count, sizeof *shader->blocks);
    shader->instructions = copy_shader_array(desc->instructions, shader->instruction_count, sizeof *shader->instructions);
    shader->operands = copy_shader_array(desc->operands, shader->operand_count, sizeof *shader->operands);
    shader->literal_words = copy_shader_array(desc->literal_words, shader->literal_word_count, sizeof *shader->literal_words);
    if (!shader->types || (shader->type_member_count && !shader->type_members) || !shader->functions ||
        !shader->blocks || !shader->instructions || (shader->operand_count && !shader->operands) ||
        (shader->literal_word_count && !shader->literal_words)) {
        free(shader->types); free(shader->type_members); free(shader->functions); free(shader->blocks);
        free(shader->instructions); free(shader->operands); free(shader->literal_words); free(shader);
        return NULL;
    }
    shader->refs = 1;
    return shader;
}

int gpu_ir_shader_specialize(const gpu_ir_shader *shader, const gpu_ir_shader_constant_override *overrides,
                             uint32_t override_count, const uint32_t threadgroup_size[3],
                             gpu_ir_shader **specialized_shader) {
    if (specialized_shader) *specialized_shader = NULL;
    if (!shader || !specialized_shader || (override_count && !overrides) ||
        (threadgroup_size && (shader->stage_mask != GPU_IR_STAGE_COMPUTE || !threadgroup_size[0] ||
                              !threadgroup_size[1] || !threadgroup_size[2]))) return GPU_IR_INVALID;

    gpu_ir_shader_instruction *instructions = copy_shader_array(shader->instructions, shader->instruction_count,
                                                                  sizeof *instructions);
    uint32_t *literal_words = copy_shader_array(shader->literal_words, shader->literal_word_count,
                                                sizeof *literal_words);
    if (!instructions || (shader->literal_word_count && !literal_words)) {
        free(instructions); free(literal_words);
        return GPU_IR_OUT_OF_MEMORY;
    }

    // Specialization turns every scalar specialization constant into an ordinary constant, using its default
    // literal when no matching API override exists. Composite and derived specialization forms remain unsupported.
    for (uint32_t i = 0; i < shader->instruction_count; i++) {
        gpu_ir_shader_instruction *instruction = &instructions[i];
        if (!(instruction->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION)) continue;
        if (instruction->opcode != GPU_IR_SHADER_OP_CONSTANT ||
            (instruction->operation != GPU_IR_SHADER_CONSTANT_SCALAR &&
             instruction->operation != GPU_IR_SHADER_CONSTANT_BOOL) || !instruction->result_id ||
            !instruction->literal_word_count ||
            !shader_range_valid(instruction->first_literal, instruction->literal_word_count,
                                shader->literal_word_count)) {
            free(instructions); free(literal_words);
            return GPU_IR_UNSUPPORTED;
        }
        instruction->flags &= ~GPU_IR_SHADER_FLAG_SPECIALIZATION;
    }

    for (uint32_t i = 0; i < override_count; i++) {
        const gpu_ir_shader_constant_override *override = &overrides[i];
        if (!override->result_id || !override->literal_words || !override->literal_word_count) {
            free(instructions); free(literal_words);
            return GPU_IR_INVALID;
        }
        for (uint32_t j = 0; j < i; j++) if (overrides[j].result_id == override->result_id) {
            free(instructions); free(literal_words);
            return GPU_IR_INVALID;
        }
        gpu_ir_shader_instruction *target = NULL;
        for (uint32_t j = 0; j < shader->instruction_count; j++)
            if (shader->instructions[j].result_id == override->result_id) { target = &instructions[j]; break; }
        const gpu_ir_shader_instruction *original = NULL;
        for (uint32_t j = 0; j < shader->instruction_count; j++)
            if (shader->instructions[j].result_id == override->result_id) { original = &shader->instructions[j]; break; }
        if (!target || !original || !(original->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) ||
            target->opcode != GPU_IR_SHADER_OP_CONSTANT ||
            (target->operation != GPU_IR_SHADER_CONSTANT_SCALAR && target->operation != GPU_IR_SHADER_CONSTANT_BOOL) ||
            override->literal_word_count != target->literal_word_count ||
            !shader_range_valid(target->first_literal, target->literal_word_count, shader->literal_word_count)) {
            free(instructions); free(literal_words);
            return GPU_IR_INVALID;
        }
        memcpy(literal_words + target->first_literal, override->literal_words,
               (size_t)override->literal_word_count * sizeof *literal_words);
    }

    uint32_t size[3];
    if (threadgroup_size) memcpy(size, threadgroup_size, sizeof size);
    else memcpy(size, shader->threadgroup_size, sizeof size);
    gpu_ir_shader_desc desc = {
        .source_api = shader->source_api, .stage_mask = shader->stage_mask,
        .entry_function_id = shader->entry_function_id,
        .threadgroup_size = { size[0], size[1], size[2] },
        .types = shader->types, .type_count = shader->type_count,
        .type_members = shader->type_members, .type_member_count = shader->type_member_count,
        .functions = shader->functions, .function_count = shader->function_count,
        .blocks = shader->blocks, .block_count = shader->block_count,
        .instructions = instructions, .instruction_count = shader->instruction_count,
        .operands = shader->operands, .operand_count = shader->operand_count,
        .literal_words = literal_words, .literal_word_count = shader->literal_word_count
    };
    gpu_ir_shader *result = gpu_ir_shader_create(&desc);
    free(instructions);
    free(literal_words);
    if (!result) return GPU_IR_OUT_OF_MEMORY;
    *specialized_shader = result;
    return GPU_IR_OK;
}

void gpu_ir_shader_retain(gpu_ir_shader *shader) {
    if (shader) __atomic_add_fetch(&shader->refs, 1, __ATOMIC_RELAXED);
}

void gpu_ir_shader_release(gpu_ir_shader *shader) {
    if (!shader || __atomic_sub_fetch(&shader->refs, 1, __ATOMIC_ACQ_REL)) return;
    free(shader->types); free(shader->type_members); free(shader->functions); free(shader->blocks);
    free(shader->instructions); free(shader->operands); free(shader->literal_words); free(shader);
}

uint32_t gpu_ir_shader_source_api(const gpu_ir_shader *shader) { return shader ? shader->source_api : 0; }
uint32_t gpu_ir_shader_stage(const gpu_ir_shader *shader) { return shader ? shader->stage_mask : 0; }
uint32_t gpu_ir_shader_entry_function(const gpu_ir_shader *shader) { return shader ? shader->entry_function_id : 0; }
void gpu_ir_shader_threadgroup_size(const gpu_ir_shader *shader, uint32_t size[3]) {
    if (!size) return;
    if (shader) memcpy(size, shader->threadgroup_size, sizeof shader->threadgroup_size);
    else memset(size, 0, 3 * sizeof *size);
}
uint32_t gpu_ir_shader_type_count(const gpu_ir_shader *shader) { return shader ? shader->type_count : 0; }
const gpu_ir_shader_type *gpu_ir_shader_type_at(const gpu_ir_shader *shader, uint32_t index) {
    return shader && index < shader->type_count ? &shader->types[index] : NULL;
}
const gpu_ir_shader_type *gpu_ir_shader_find_type(const gpu_ir_shader *shader, uint32_t id) {
    if (!shader || !id) return NULL;
    for (uint32_t i = 0; i < shader->type_count; i++) if (shader->types[i].id == id) return &shader->types[i];
    return NULL;
}
uint32_t gpu_ir_shader_type_member_count(const gpu_ir_shader *shader) { return shader ? shader->type_member_count : 0; }
const gpu_ir_shader_member *gpu_ir_shader_type_member_at(const gpu_ir_shader *shader, uint32_t index) {
    return shader && index < shader->type_member_count ? &shader->type_members[index] : NULL;
}
uint32_t gpu_ir_shader_function_count(const gpu_ir_shader *shader) { return shader ? shader->function_count : 0; }
const gpu_ir_shader_function *gpu_ir_shader_function_at(const gpu_ir_shader *shader, uint32_t index) {
    return shader && index < shader->function_count ? &shader->functions[index] : NULL;
}
const gpu_ir_shader_function *gpu_ir_shader_find_function(const gpu_ir_shader *shader, uint32_t id) {
    if (!shader || !id) return NULL;
    for (uint32_t i = 0; i < shader->function_count; i++) if (shader->functions[i].id == id) return &shader->functions[i];
    return NULL;
}
uint32_t gpu_ir_shader_block_count(const gpu_ir_shader *shader) { return shader ? shader->block_count : 0; }
const gpu_ir_shader_block *gpu_ir_shader_block_at(const gpu_ir_shader *shader, uint32_t index) {
    return shader && index < shader->block_count ? &shader->blocks[index] : NULL;
}
uint32_t gpu_ir_shader_instruction_count(const gpu_ir_shader *shader) { return shader ? shader->instruction_count : 0; }
const gpu_ir_shader_instruction *gpu_ir_shader_instruction_at(const gpu_ir_shader *shader, uint32_t index) {
    return shader && index < shader->instruction_count ? &shader->instructions[index] : NULL;
}
uint32_t gpu_ir_shader_operand_count(const gpu_ir_shader *shader) { return shader ? shader->operand_count : 0; }
const gpu_ir_shader_operand *gpu_ir_shader_operand_at(const gpu_ir_shader *shader, uint32_t index) {
    return shader && index < shader->operand_count ? &shader->operands[index] : NULL;
}
uint32_t gpu_ir_shader_literal_word_count(const gpu_ir_shader *shader) { return shader ? shader->literal_word_count : 0; }
uint32_t gpu_ir_shader_literal_word_at(const gpu_ir_shader *shader, uint32_t index) {
    return shader && index < shader->literal_word_count ? shader->literal_words[index] : 0;
}

static gpu_ir_resource *resource_create(uint32_t kind, void *desc, void *backend_object,
                                         gpu_ir_backend_object_release release) {
    if (!backend_object || !release) return NULL;
    gpu_ir_resource *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->refs = 1;
    r->kind = kind;
    if (kind == GPU_IR_RESOURCE_BUFFER) r->desc.buffer_size = *(const uint64_t *)desc;
    else r->desc.texture = *(const gpu_ir_texture_desc *)desc;
    r->backend_object = backend_object;
    r->release = release;
    return r;
}

gpu_ir_resource *gpu_ir_buffer_create(uint64_t size, void *backend_object, gpu_ir_backend_object_release release) {
    if (!size) return NULL;
    return resource_create(GPU_IR_RESOURCE_BUFFER, &size, backend_object, release);
}

gpu_ir_resource *gpu_ir_texture_create(const gpu_ir_texture_desc *desc, void *backend_object,
                                       gpu_ir_backend_object_release release) {
    if (!desc || !desc->width || !desc->height || !desc->depth || !desc->array_layers ||
        !desc->mip_levels || !desc->sample_count ||
        (desc->is_3d && desc->array_layers != 1)) return NULL;
    gpu_ir_texture_desc normalized = *desc;
    if (!normalized.block_width && !normalized.block_height && !normalized.bytes_per_block) {
        normalized.block_width = normalized.block_height = 1;
        normalized.bytes_per_block = normalized.bytes_per_texel;
    }
    if (!normalized.block_width || !normalized.block_height || !normalized.bytes_per_block ||
        (normalized.sample_count > 1 && (normalized.block_width != 1 || normalized.block_height != 1))) return NULL;
    return resource_create(GPU_IR_RESOURCE_TEXTURE, &normalized, backend_object, release);
}

void gpu_ir_resource_retain(gpu_ir_resource *r) {
    if (r) __atomic_add_fetch(&r->refs, 1, __ATOMIC_RELAXED);
}

void gpu_ir_resource_release(gpu_ir_resource *r) {
    if (!r || __atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (r->release) r->release(r->backend_object);
    free(r);
}

int gpu_ir_resource_is_buffer(const gpu_ir_resource *r) { return r && r->kind == GPU_IR_RESOURCE_BUFFER; }
uint64_t gpu_ir_buffer_size(const gpu_ir_resource *r) { return gpu_ir_resource_is_buffer(r) ? r->desc.buffer_size : 0; }
const gpu_ir_texture_desc *gpu_ir_texture_description(const gpu_ir_resource *r) {
    return r && r->kind == GPU_IR_RESOURCE_TEXTURE ? &r->desc.texture : NULL;
}
void *gpu_ir_backend_object(const gpu_ir_resource *r) { return r ? r->backend_object : NULL; }

gpu_ir_buffer_view *gpu_ir_buffer_view_create(gpu_ir_resource *resource,
                                               const gpu_ir_buffer_view_desc *desc,
                                               void *backend_object,
                                               gpu_ir_backend_object_release release) {
    if (!gpu_ir_resource_is_buffer(resource) || !desc || !desc->element_count || !desc->byte_size ||
        !desc->access_mask || (desc->access_mask & ~(GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE)) ||
        (backend_object && !release) || (!backend_object && release)) return NULL;
    uint64_t element_size;
    switch (desc->kind) {
    case GPU_IR_BUFFER_VIEW_TYPED:
        if (!desc->element_size || desc->structure_stride) return NULL;
        element_size = desc->element_size;
        break;
    case GPU_IR_BUFFER_VIEW_STRUCTURED:
        if (desc->element_size || !desc->structure_stride) return NULL;
        element_size = desc->structure_stride;
        break;
    case GPU_IR_BUFFER_VIEW_RAW:
        if (desc->element_size || desc->structure_stride) return NULL;
        element_size = 4;
        break;
    default: return NULL;
    }
    if ((uint64_t)desc->element_count * element_size != desc->byte_size ||
        (desc->kind == GPU_IR_BUFFER_VIEW_RAW && ((desc->byte_offset | desc->byte_size) & 3u)) ||
        desc->byte_offset > gpu_ir_buffer_size(resource) ||
        desc->byte_size > gpu_ir_buffer_size(resource) - desc->byte_offset) return NULL;
    gpu_ir_buffer_view *view = calloc(1, sizeof *view);
    if (!view) return NULL;
    view->refs = 1;
    view->resource = resource;
    gpu_ir_resource_retain(resource);
    view->desc = *desc;
    view->backend_object = backend_object;
    view->release = release;
    return view;
}

void gpu_ir_buffer_view_retain(gpu_ir_buffer_view *view) {
    if (view) __atomic_add_fetch(&view->refs, 1, __ATOMIC_RELAXED);
}

void gpu_ir_buffer_view_release(gpu_ir_buffer_view *view) {
    if (!view || __atomic_sub_fetch(&view->refs, 1, __ATOMIC_ACQ_REL)) return;
    if (view->release) view->release(view->backend_object);
    gpu_ir_resource_release(view->resource);
    free(view);
}

const gpu_ir_buffer_view_desc *gpu_ir_buffer_view_description(const gpu_ir_buffer_view *view) {
    return view ? &view->desc : NULL;
}

const gpu_ir_resource *gpu_ir_buffer_view_resource(const gpu_ir_buffer_view *view) {
    return view ? view->resource : NULL;
}

void *gpu_ir_buffer_view_backend_object(const gpu_ir_buffer_view *view) {
    return view ? view->backend_object : NULL;
}

static int valid_stage_mask(uint32_t stages) { return stages && !(stages & ~GPU_IR_STAGE_ALL); }

static int valid_sync_stage_scope(uint64_t stages) { return !(stages & ~GPU_IR_SYNC_STAGE_ALL); }

static int valid_resource_access(const gpu_ir_resource_access *access) {
    if (!access || !access->resource || !valid_stage_mask(access->stage_mask) || !access->access_mask ||
        (access->access_mask & ~GPU_IR_ACCESS_ALL)) return 0;
    const uint32_t buffer_only = GPU_IR_ACCESS_VERTEX_READ | GPU_IR_ACCESS_INDEX_READ |
        GPU_IR_ACCESS_UNIFORM_READ | GPU_IR_ACCESS_STREAM_WRITE | GPU_IR_ACCESS_INDIRECT_READ;
    const uint32_t texture_only = GPU_IR_ACCESS_COLOR_WRITE | GPU_IR_ACCESS_DEPTH_READ |
        GPU_IR_ACCESS_DEPTH_WRITE | GPU_IR_ACCESS_RESOLVE_READ | GPU_IR_ACCESS_RESOLVE_WRITE |
        GPU_IR_ACCESS_INPUT_ATTACHMENT_READ | GPU_IR_ACCESS_COLOR_READ;
    if (gpu_ir_resource_is_buffer(access->resource) && (access->access_mask & texture_only)) return 0;
    if (!gpu_ir_resource_is_buffer(access->resource) && (access->access_mask & buffer_only)) return 0;
    return 1;
}

static int copy_resource_accesses(const gpu_ir_resource_access *accesses, uint32_t count,
                                  gpu_ir_resource_access **out) {
    *out = NULL;
    if (count && !accesses) return GPU_IR_INVALID;
    if (!count) return GPU_IR_OK;
    if (sizeof(*accesses) > SIZE_MAX / count) return GPU_IR_OUT_OF_MEMORY;
    gpu_ir_resource_access *copy = malloc((size_t)count * sizeof *copy);
    if (!copy) return GPU_IR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < count; i++) {
        if (!valid_resource_access(&accesses[i])) { free(copy); return GPU_IR_INVALID; }
        copy[i] = accesses[i];
    }
    for (uint32_t i = 0; i < count; i++) gpu_ir_resource_retain(copy[i].resource);
    *out = copy;
    return GPU_IR_OK;
}

static int resource_accesses_match_pipeline(const gpu_ir_resource_access *accesses, uint32_t count,
                                            const gpu_ir_pipeline_info *pipeline) {
    for (uint32_t i = 0; i < count; i++)
        if (accesses[i].stage_mask & ~pipeline->stage_mask) return 0;
    return 1;
}

static void release_resource_accesses(gpu_ir_resource_access *accesses, uint32_t count) {
    if (!accesses) return;
    for (uint32_t i = 0; i < count; i++) gpu_ir_resource_release(accesses[i].resource);
    free(accesses);
}

int gpu_ir_resource_access_conflicts(const gpu_ir_resource_access *a, const gpu_ir_resource_access *b) {
    if (!valid_resource_access(a) || !valid_resource_access(b) || a->resource != b->resource) return 0;
    const uint32_t writes = GPU_IR_ACCESS_SHADER_WRITE | GPU_IR_ACCESS_COLOR_WRITE | GPU_IR_ACCESS_DEPTH_WRITE |
        GPU_IR_ACCESS_STREAM_WRITE | GPU_IR_ACCESS_COPY_WRITE | GPU_IR_ACCESS_RESOLVE_WRITE |
        GPU_IR_ACCESS_HOST_WRITE | GPU_IR_ACCESS_MEMORY_WRITE | GPU_IR_ACCESS_ACCELERATION_STRUCTURE_WRITE;
    return !!((a->access_mask & writes) || (b->access_mask & writes));
}

static int valid_descriptor_binding(const gpu_ir_descriptor_binding *b) {
    const uint32_t rw = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE;
    if (!b || !b->count || !valid_stage_mask(b->stage_mask)) return 0;
    switch (b->kind) {
    case GPU_IR_DESCRIPTOR_UNIFORM_BUFFER: return b->access_mask == GPU_IR_ACCESS_UNIFORM_READ;
    case GPU_IR_DESCRIPTOR_STORAGE_BUFFER:
    case GPU_IR_DESCRIPTOR_STORAGE_IMAGE:
    case GPU_IR_DESCRIPTOR_STORAGE_TEXEL_BUFFER: return b->access_mask && !(b->access_mask & ~rw);
    case GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE: return b->access_mask == GPU_IR_ACCESS_SHADER_READ;
    case GPU_IR_DESCRIPTOR_STORAGE_RESOURCE: return b->access_mask && !(b->access_mask & ~rw);
    case GPU_IR_DESCRIPTOR_SAMPLED_IMAGE:
    case GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER:
    case GPU_IR_DESCRIPTOR_UNIFORM_TEXEL_BUFFER:
    case GPU_IR_DESCRIPTOR_INPUT_ATTACHMENT: return b->access_mask == GPU_IR_ACCESS_SHADER_READ;
    case GPU_IR_DESCRIPTOR_SAMPLER: return b->access_mask == 0;
    case GPU_IR_DESCRIPTOR_ACCELERATION_STRUCTURE:
        return b->access_mask == (GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_ACCELERATION_STRUCTURE);
    default: return 0;
    }
}

static int binding_compare(const void *left, const void *right) {
    const gpu_ir_descriptor_binding *a = left, *b = right;
    if (a->set != b->set) return a->set < b->set ? -1 : 1;
    if (a->binding_namespace != b->binding_namespace)
        return a->binding_namespace < b->binding_namespace ? -1 : 1;
    if (a->binding != b->binding) return a->binding < b->binding ? -1 : 1;
    if (a->stage_mask != b->stage_mask) return a->stage_mask < b->stage_mask ? -1 : 1;
    return 0;
}

static int same_binding_key(const gpu_ir_descriptor_binding *a, const gpu_ir_descriptor_binding *b) {
    return a->set == b->set && a->binding_namespace == b->binding_namespace && a->binding == b->binding;
}

static int push_constant_compare(const void *left, const void *right) {
    const gpu_ir_push_constant_range *a = left, *b = right;
    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    if (a->size != b->size) return a->size < b->size ? -1 : 1;
    if (a->stage_mask != b->stage_mask) return a->stage_mask < b->stage_mask ? -1 : 1;
    return 0;
}

gpu_ir_pipeline_layout *gpu_ir_pipeline_layout_create(const gpu_ir_pipeline_layout_desc *desc) {
    if (!desc || (desc->binding_count && !desc->bindings) ||
        (desc->push_constant_count && !desc->push_constants) ||
        (desc->binding_count && sizeof(gpu_ir_descriptor_binding) > SIZE_MAX / desc->binding_count) ||
        (desc->push_constant_count && sizeof(gpu_ir_push_constant_range) > SIZE_MAX / desc->push_constant_count)) return NULL;
    gpu_ir_pipeline_layout *layout = calloc(1, sizeof *layout);
    if (!layout) return NULL;
    if (desc->binding_count) {
        layout->bindings = malloc((size_t)desc->binding_count * sizeof *layout->bindings);
        if (!layout->bindings) goto fail;
        memcpy(layout->bindings, desc->bindings, (size_t)desc->binding_count * sizeof *layout->bindings);
        layout->binding_count = desc->binding_count;
        qsort(layout->bindings, layout->binding_count, sizeof *layout->bindings, binding_compare);
        for (uint32_t i = 0; i < layout->binding_count; i++) {
            if (!valid_descriptor_binding(&layout->bindings[i])) goto fail;
            for (uint32_t j = i; j > 0;) {
                const gpu_ir_descriptor_binding *previous = &layout->bindings[--j];
                if (!same_binding_key(previous, &layout->bindings[i])) break;
                if (previous->stage_mask & layout->bindings[i].stage_mask) goto fail;
            }
        }
    }
    if (desc->push_constant_count) {
        layout->push_constants = malloc((size_t)desc->push_constant_count * sizeof *layout->push_constants);
        if (!layout->push_constants) goto fail;
        memcpy(layout->push_constants, desc->push_constants,
               (size_t)desc->push_constant_count * sizeof *layout->push_constants);
        layout->push_constant_count = desc->push_constant_count;
        qsort(layout->push_constants, layout->push_constant_count, sizeof *layout->push_constants, push_constant_compare);
        for (uint32_t i = 0; i < layout->push_constant_count; i++) {
            const gpu_ir_push_constant_range *range = &layout->push_constants[i];
            if (!range->size || ((range->offset | range->size) & 3) || !valid_stage_mask(range->stage_mask) ||
                range->size > UINT32_MAX - range->offset) goto fail;
            uint32_t end = range->offset + range->size;
            for (uint32_t j = 0; j < i; j++) {
                const gpu_ir_push_constant_range *previous = &layout->push_constants[j];
                uint32_t previous_end = previous->offset + previous->size;
                if (range->offset < previous_end && previous->offset < end &&
                    (range->stage_mask & previous->stage_mask)) goto fail;
            }
        }
    }
    layout->refs = 1;
    return layout;
fail:
    free(layout->bindings);
    free(layout->push_constants);
    free(layout);
    return NULL;
}

void gpu_ir_pipeline_layout_retain(gpu_ir_pipeline_layout *layout) {
    if (layout) __atomic_add_fetch(&layout->refs, 1, __ATOMIC_RELAXED);
}

void gpu_ir_pipeline_layout_release(gpu_ir_pipeline_layout *layout) {
    if (!layout || __atomic_sub_fetch(&layout->refs, 1, __ATOMIC_ACQ_REL)) return;
    free(layout->bindings);
    free(layout->push_constants);
    free(layout);
}

uint32_t gpu_ir_pipeline_layout_binding_count(const gpu_ir_pipeline_layout *layout) {
    return layout ? layout->binding_count : 0;
}

int gpu_ir_pipeline_layout_binding_at(const gpu_ir_pipeline_layout *layout, uint32_t index, gpu_ir_descriptor_binding *out) {
    if (!layout || !out) return GPU_IR_INVALID;
    if (index >= layout->binding_count) return GPU_IR_RANGE;
    *out = layout->bindings[index];
    return GPU_IR_OK;
}

int gpu_ir_pipeline_layout_find_binding(const gpu_ir_pipeline_layout *layout, uint32_t set, uint32_t binding,
                                        gpu_ir_descriptor_binding *out) {
    return gpu_ir_pipeline_layout_find_binding_in_namespace(layout, set, 0, binding, out);
}

int gpu_ir_pipeline_layout_find_binding_in_namespace(const gpu_ir_pipeline_layout *layout, uint32_t set,
                                                      uint32_t binding_namespace, uint32_t binding,
                                                      gpu_ir_descriptor_binding *out) {
    if (!layout || !out) return GPU_IR_INVALID;
    uint32_t low = 0, high = layout->binding_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        const gpu_ir_descriptor_binding *candidate = &layout->bindings[middle];
        if (candidate->set < set ||
            (candidate->set == set && candidate->binding_namespace < binding_namespace) ||
            (candidate->set == set && candidate->binding_namespace == binding_namespace &&
             candidate->binding < binding)) low = middle + 1;
        else high = middle;
    }
    if (low == layout->binding_count || layout->bindings[low].set != set ||
        layout->bindings[low].binding_namespace != binding_namespace || layout->bindings[low].binding != binding)
        return GPU_IR_NOT_FOUND;
    *out = layout->bindings[low++];
    while (low < layout->binding_count && same_binding_key(out, &layout->bindings[low])) {
        const gpu_ir_descriptor_binding *next = &layout->bindings[low++];
        if (out->kind != next->kind || out->count != next->count || out->access_mask != next->access_mask)
            return GPU_IR_INVALID;
        out->stage_mask |= next->stage_mask;
    }
    return GPU_IR_OK;
}

int gpu_ir_pipeline_layout_find_binding_for_stage(const gpu_ir_pipeline_layout *layout, uint32_t set,
                                                   uint32_t binding_namespace, uint32_t binding,
                                                   uint32_t stage_mask, gpu_ir_descriptor_binding *out) {
    if (!layout || !out || !valid_stage_mask(stage_mask)) return GPU_IR_INVALID;
    uint32_t low = 0, high = layout->binding_count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        const gpu_ir_descriptor_binding *candidate = &layout->bindings[middle];
        if (candidate->set < set ||
            (candidate->set == set && candidate->binding_namespace < binding_namespace) ||
            (candidate->set == set && candidate->binding_namespace == binding_namespace &&
             candidate->binding < binding)) low = middle + 1;
        else high = middle;
    }
    if (low == layout->binding_count || layout->bindings[low].set != set ||
        layout->bindings[low].binding_namespace != binding_namespace || layout->bindings[low].binding != binding)
        return GPU_IR_NOT_FOUND;
    int found = 0;
    for (uint32_t i = low; i < layout->binding_count && same_binding_key(&layout->bindings[i], &layout->bindings[low]); i++) {
        if ((layout->bindings[i].stage_mask & stage_mask) != stage_mask) continue;
        if (found) return GPU_IR_INVALID;
        *out = layout->bindings[i];
        found = 1;
    }
    return found ? GPU_IR_OK : GPU_IR_NOT_FOUND;
}

uint32_t gpu_ir_pipeline_layout_push_constant_count(const gpu_ir_pipeline_layout *layout) {
    return layout ? layout->push_constant_count : 0;
}

int gpu_ir_pipeline_layout_push_constant_at(const gpu_ir_pipeline_layout *layout, uint32_t index,
                                            gpu_ir_push_constant_range *out) {
    if (!layout || !out) return GPU_IR_INVALID;
    if (index >= layout->push_constant_count) return GPU_IR_RANGE;
    *out = layout->push_constants[index];
    return GPU_IR_OK;
}

static int descriptor_value_compare(const void *left, const void *right) {
    const gpu_ir_descriptor_value *a = left, *b = right;
    if (a->set != b->set) return a->set < b->set ? -1 : 1;
    if (a->binding_namespace != b->binding_namespace)
        return a->binding_namespace < b->binding_namespace ? -1 : 1;
    if (a->binding != b->binding) return a->binding < b->binding ? -1 : 1;
    if (a->array_element != b->array_element) return a->array_element < b->array_element ? -1 : 1;
    if (a->stage_mask != b->stage_mask) return a->stage_mask < b->stage_mask ? -1 : 1;
    return 0;
}

static int descriptor_value_same_key(const gpu_ir_descriptor_value *a, const gpu_ir_descriptor_value *b) {
    return a->set == b->set && a->binding_namespace == b->binding_namespace &&
           a->binding == b->binding && a->array_element == b->array_element;
}

static const gpu_ir_resource *descriptor_value_resource(const gpu_ir_descriptor_value *value) {
    return value->resource ? value->resource : gpu_ir_buffer_view_resource(value->buffer_view);
}

static int valid_descriptor_value(const gpu_ir_pipeline_layout *layout, gpu_ir_descriptor_value *value) {
    if (!layout || !value || !valid_stage_mask(value->stage_mask) ||
        (value->flags & ~GPU_IR_DESCRIPTOR_VALUE_NULL) || (value->access_mask & ~GPU_IR_ACCESS_ALL) ||
        (!!value->backend_object != !!value->backend_release) ||
        (!!value->backend_object_aux != !!value->backend_release_aux)) return 0;
    if (value->component_mapping & ~0xfffu) return 0;
    for (uint32_t component = 0; component < 4; component++)
        if (((value->component_mapping >> (component * 3)) & 7u) > GPU_IR_COMPONENT_SWIZZLE_ONE) return 0;
    gpu_ir_descriptor_binding binding;
    if (gpu_ir_pipeline_layout_find_binding_for_stage(layout, value->set, value->binding_namespace,
            value->binding, value->stage_mask, &binding) != GPU_IR_OK || binding.kind != value->kind ||
        (binding.count != GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED && value->array_element >= binding.count) ||
        (binding.access_mask & ~value->access_mask)) return 0;

    if (value->flags & GPU_IR_DESCRIPTOR_VALUE_NULL)
        return !value->component_mapping && !value->resource && !value->buffer_view && !value->offset && !value->range &&
               !value->backend_object && !value->backend_object_aux && !value->auxiliary_resource &&
               !value->auxiliary_access_mask && !value->auxiliary_offset && !value->auxiliary_range;

    const gpu_ir_resource *resource = descriptor_value_resource(value);
    if (value->resource && value->buffer_view && value->resource != gpu_ir_buffer_view_resource(value->buffer_view))
        return 0;
    if (value->auxiliary_resource) {
        if (!gpu_ir_resource_is_buffer(value->auxiliary_resource) || !value->auxiliary_access_mask ||
            (value->auxiliary_access_mask & ~GPU_IR_ACCESS_ALL)) return 0;
        uint64_t auxiliary_size = gpu_ir_buffer_size(value->auxiliary_resource);
        if (value->auxiliary_offset >= auxiliary_size ||
            !value->auxiliary_range || value->auxiliary_range > auxiliary_size - value->auxiliary_offset) return 0;
    } else if (value->auxiliary_access_mask || value->auxiliary_offset || value->auxiliary_range) return 0;
    if (value->buffer_view) {
        const gpu_ir_buffer_view_desc *view = gpu_ir_buffer_view_description(value->buffer_view);
        if (!view || (view->access_mask & binding.access_mask) != binding.access_mask || value->offset || value->range)
            return 0;
    }

    switch (binding.kind) {
    case GPU_IR_DESCRIPTOR_UNIFORM_BUFFER:
        // (the adapter's push-constant binding may carry its bytes inline: range bytes at backend_object, which
        //  the state owns and frees with backend_release)
        if (value->set == GPU_IR_INTERNAL_SET && value->binding_namespace == GPU_IR_INTERNAL_BINDING_NAMESPACE &&
            value->binding == GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING && !value->resource && value->backend_object) {
            return value->range && !value->offset && !value->buffer_view && !value->backend_object_aux &&
                   !value->auxiliary_resource;
        }
        if (!gpu_ir_resource_is_buffer(resource) || value->buffer_view || value->backend_object_aux) return 0;
        break;
    case GPU_IR_DESCRIPTOR_STORAGE_BUFFER:
        if (!gpu_ir_resource_is_buffer(resource) || value->backend_object_aux) return 0;
        break;
    case GPU_IR_DESCRIPTOR_SAMPLED_IMAGE:
    case GPU_IR_DESCRIPTOR_STORAGE_IMAGE:
    case GPU_IR_DESCRIPTOR_INPUT_ATTACHMENT:
        if (!gpu_ir_texture_description(resource) || value->buffer_view || value->offset || value->range ||
            value->backend_object_aux) return 0;
        break;
    case GPU_IR_DESCRIPTOR_SAMPLER:
        if (value->resource || value->buffer_view || value->offset || value->range || !value->backend_object ||
            value->backend_object_aux) return 0;
        break;
    case GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER:
        if (!gpu_ir_texture_description(resource) || value->buffer_view || value->offset || value->range ||
            !value->backend_object || value->backend_object_aux) return 0;
        break;
    case GPU_IR_DESCRIPTOR_UNIFORM_TEXEL_BUFFER:
        if (!value->buffer_view || gpu_ir_buffer_view_description(value->buffer_view)->kind != GPU_IR_BUFFER_VIEW_TYPED ||
            !gpu_ir_resource_is_buffer(resource) || value->backend_object_aux) return 0;
        break;
    case GPU_IR_DESCRIPTOR_STORAGE_TEXEL_BUFFER:
        if (!value->buffer_view || gpu_ir_buffer_view_description(value->buffer_view)->kind != GPU_IR_BUFFER_VIEW_TYPED ||
            !gpu_ir_resource_is_buffer(resource) || value->backend_object_aux) return 0;
        break;
    case GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE:
    case GPU_IR_DESCRIPTOR_STORAGE_RESOURCE:
        if (!resource || value->backend_object_aux) return 0;
        break;
    case GPU_IR_DESCRIPTOR_ACCELERATION_STRUCTURE:
        if (!value->backend_object || value->resource || value->buffer_view || value->offset || value->range) return 0;
        break;
    default: return 0;
    }
    // (a counter: storage buffers, and D3D12's general UAV bindings when the bound resource is a buffer)
    if (value->auxiliary_resource && binding.kind != GPU_IR_DESCRIPTOR_STORAGE_BUFFER &&
        !(binding.kind == GPU_IR_DESCRIPTOR_STORAGE_RESOURCE && gpu_ir_resource_is_buffer(resource))) return 0;

    if (gpu_ir_resource_is_buffer(resource) && !value->buffer_view) {
        uint64_t size = gpu_ir_buffer_size(resource);
        if (value->offset >= size) return 0;
        if (!value->range) value->range = size - value->offset;
        else if (value->range > size - value->offset) return 0;
    } else if (value->offset || value->range) return 0;
    return 1;
}

gpu_ir_descriptor_state *gpu_ir_descriptor_state_create(gpu_ir_pipeline_layout *layout,
                                                         const gpu_ir_descriptor_state_desc *desc) {
    if (!layout || !desc || (desc->value_count && !desc->values) ||
        (desc->value_count && sizeof(gpu_ir_descriptor_value) > SIZE_MAX / desc->value_count)) return NULL;
    gpu_ir_descriptor_value *values = desc->value_count ?
        malloc((size_t)desc->value_count * sizeof *values) : NULL;
    if (desc->value_count && !values) return NULL;
    if (desc->value_count) memcpy(values, desc->values, (size_t)desc->value_count * sizeof *values);
    for (uint32_t i = 0; i < desc->value_count; i++)
        if (!valid_descriptor_value(layout, &values[i])) { free(values); return NULL; }
    if (desc->value_count > 1) qsort(values, desc->value_count, sizeof *values, descriptor_value_compare);
    for (uint32_t i = 0; i < desc->value_count; i++) {
        for (uint32_t j = i; j > 0;) {
            const gpu_ir_descriptor_value *previous = &values[--j];
            if (!descriptor_value_same_key(previous, &values[i])) break;
            if (previous->stage_mask & values[i].stage_mask) { free(values); return NULL; }
        }
    }
    gpu_ir_descriptor_state *state = calloc(1, sizeof *state);
    if (!state) { free(values); return NULL; }
    state->refs = 1;
    state->layout = layout;
    state->value_count = desc->value_count;
    state->values = values;
    gpu_ir_pipeline_layout_retain(layout);
    for (uint32_t i = 0; i < state->value_count; i++) {
        gpu_ir_resource_retain(state->values[i].resource);
        gpu_ir_buffer_view_retain(state->values[i].buffer_view);
        gpu_ir_resource_retain(state->values[i].auxiliary_resource);
    }
    return state;
}

void gpu_ir_descriptor_state_retain(gpu_ir_descriptor_state *state) {
    if (state) __atomic_add_fetch(&state->refs, 1, __ATOMIC_RELAXED);
}

void gpu_ir_descriptor_state_release(gpu_ir_descriptor_state *state) {
    if (!state || __atomic_sub_fetch(&state->refs, 1, __ATOMIC_ACQ_REL)) return;
    for (uint32_t i = 0; i < state->value_count; i++) {
        gpu_ir_resource_release(state->values[i].resource);
        gpu_ir_buffer_view_release(state->values[i].buffer_view);
        gpu_ir_resource_release(state->values[i].auxiliary_resource);
        if (state->values[i].backend_release) state->values[i].backend_release(state->values[i].backend_object);
        if (state->values[i].backend_release_aux)
            state->values[i].backend_release_aux(state->values[i].backend_object_aux);
    }
    gpu_ir_pipeline_layout_release(state->layout);
    free(state->values);
    free(state);
}

const gpu_ir_pipeline_layout *gpu_ir_descriptor_state_layout(const gpu_ir_descriptor_state *state) {
    return state ? state->layout : NULL;
}

uint32_t gpu_ir_descriptor_state_value_count(const gpu_ir_descriptor_state *state) {
    return state ? state->value_count : 0;
}

int gpu_ir_descriptor_state_value_at(const gpu_ir_descriptor_state *state, uint32_t index,
                                     gpu_ir_descriptor_value *out) {
    if (!state || !out) return GPU_IR_INVALID;
    if (index >= state->value_count) return GPU_IR_RANGE;
    *out = state->values[index];
    return GPU_IR_OK;
}

int gpu_ir_descriptor_state_find_for_stage(const gpu_ir_descriptor_state *state, uint32_t set,
                                           uint32_t binding_namespace, uint32_t binding,
                                           uint32_t array_element, uint32_t stage_mask,
                                           gpu_ir_descriptor_value *out) {
    if (!state || !out || !valid_stage_mask(stage_mask)) return GPU_IR_INVALID;
    int found = 0;
    for (uint32_t i = 0; i < state->value_count; i++) {
        const gpu_ir_descriptor_value *candidate = &state->values[i];
        if (candidate->set != set || candidate->binding_namespace != binding_namespace ||
            candidate->binding != binding || candidate->array_element != array_element ||
            (candidate->stage_mask & stage_mask) != stage_mask) continue;
        if (found) return GPU_IR_INVALID;
        *out = *candidate;
        found = 1;
    }
    return found ? GPU_IR_OK : GPU_IR_NOT_FOUND;
}

int gpu_ir_descriptor_state_find(const gpu_ir_descriptor_state *state, uint32_t set, uint32_t binding_namespace,
                                 uint32_t binding, uint32_t array_element, gpu_ir_descriptor_value *out) {
    if (!state || !out) return GPU_IR_INVALID;
    int found = 0;
    for (uint32_t i = 0; i < state->value_count; i++) {
        const gpu_ir_descriptor_value *candidate = &state->values[i];
        if (candidate->set != set || candidate->binding_namespace != binding_namespace ||
            candidate->binding != binding || candidate->array_element != array_element) continue;
        if (found) return GPU_IR_INVALID;
        *out = *candidate;
        found = 1;
    }
    return found ? GPU_IR_OK : GPU_IR_NOT_FOUND;
}

uint64_t gpu_ir_stream_bytes;
size_t gpu_ir_command_size(void) { return sizeof(gpu_ir_command); }

void gpu_ir_stream_init(gpu_ir_stream *s) { if (s) *s = (gpu_ir_stream){ 0 }; }

void gpu_ir_stream_clear(gpu_ir_stream *s) {
    if (!s) return;
    gpu_ir_command *cmds = s->commands;
    for (uint32_t i = 0; i < s->count; i++) {
        if (cmds[i].op == GPU_IR_OP_UPDATE_BUFFER) free(cmds[i].copy.update_buffer.data);
        if (cmds[i].op == GPU_IR_OP_CLEAR_COLOR) free((void *)cmds[i].copy.clear_color.rectangles);
        if (cmds[i].op == GPU_IR_OP_CLEAR_DEPTH_STENCIL)
            free((void *)cmds[i].copy.clear_depth_stencil.rectangles);
        if (cmds[i].op == GPU_IR_OP_DRAW)
            for (uint32_t vertex = 0; vertex < cmds[i].copy.draw.vertex_buffer_count; vertex++)
                gpu_ir_resource_release(cmds[i].copy.draw.vertex_buffers[vertex].resource);
        if (cmds[i].op == GPU_IR_OP_DRAW_INDIRECT) {
            gpu_ir_resource_release(cmds[i].copy.draw_indirect.index_buffer);
            for (uint32_t vertex = 0; vertex < cmds[i].copy.draw_indirect.vertex_buffer_count; vertex++)
                gpu_ir_resource_release(cmds[i].copy.draw_indirect.vertex_buffers[vertex].resource);
        }
        gpu_ir_resource_release(cmds[i].src);
        gpu_ir_resource_release(cmds[i].dst);
        gpu_ir_pipeline_release(cmds[i].pipeline);
        gpu_ir_descriptor_state_release(cmds[i].descriptor_state);
        release_resource_accesses(cmds[i].accesses, cmds[i].access_count);
    }
    s->count = 0;
}

void gpu_ir_stream_destroy(gpu_ir_stream *s) {
    if (!s) return;
    gpu_ir_stream_clear(s);
    __atomic_sub_fetch(&gpu_ir_stream_bytes, (uint64_t)s->capacity * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
    free(s->commands);
    *s = (gpu_ir_stream){ 0 };
}

uint32_t gpu_ir_stream_count(const gpu_ir_stream *s) { return s ? s->count : 0; }

static gpu_ir_command *stream_append_command(gpu_ir_stream *s, uint32_t op) {
    if (!s || s->count == UINT32_MAX) return NULL;
    if (s->count == s->capacity) {
        uint32_t capacity = s->capacity ? s->capacity * 2 : 16;
        if (capacity < s->capacity || (size_t)capacity > SIZE_MAX / sizeof(gpu_ir_command)) return NULL;
        gpu_ir_command *commands = realloc(s->commands, (size_t)capacity * sizeof *commands);
        if (!commands) return NULL;
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(capacity - s->capacity) * sizeof *commands, __ATOMIC_RELAXED);
        s->commands = commands;
        s->capacity = capacity;
    }
    gpu_ir_command *command = &((gpu_ir_command *)s->commands)[s->count];
    memset(command, 0, sizeof *command);
    command->op = op;
    return command;
}

static gpu_ir_rect *copy_clear_rectangles(const gpu_ir_rect *rectangles, uint32_t count) {
    if (!count) return NULL;
    if (!rectangles || (size_t)count > SIZE_MAX / sizeof *rectangles) return NULL;
    gpu_ir_rect *copy = malloc((size_t)count * sizeof *copy);
    if (copy) memcpy(copy, rectangles, (size_t)count * sizeof *copy);
    return copy;
}

int gpu_ir_record_clear_color_layers_typed(gpu_ir_stream *s, gpu_ir_resource *texture,
                                          gpu_ir_clear_color_value_type value_type, const void *color,
                                          uint32_t base_array_layer, uint32_t array_layer_count,
                                          const gpu_ir_rect *rectangles, uint32_t rectangle_count,
                                          uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    const gpu_ir_texture_desc *description = gpu_ir_texture_description(texture);
    if (!s || !description || !color ||
        value_type < GPU_IR_CLEAR_COLOR_VALUE_FLOAT || value_type > GPU_IR_CLEAR_COLOR_VALUE_SINT ||
        (rectangle_count && !rectangles) || (!array_layer_count && base_array_layer) ||
        (array_layer_count && (base_array_layer >= description->array_layers ||
            array_layer_count > description->array_layers - base_array_layer)))
        return GPU_IR_INVALID;
    if (s->count == UINT32_MAX) return GPU_IR_OUT_OF_MEMORY;
    gpu_ir_rect *rect_copy = copy_clear_rectangles(rectangles, rectangle_count);
    if (rectangle_count && !rect_copy) return GPU_IR_OUT_OF_MEMORY;
    gpu_ir_command *command = stream_append_command(s, GPU_IR_OP_CLEAR_COLOR);
    if (!command) { free(rect_copy); return GPU_IR_OUT_OF_MEMORY; }
    command->dst = texture;
    memcpy(command->copy.clear_color.color, color, sizeof command->copy.clear_color.color);
    command->copy.clear_color.value_type = value_type;
    command->copy.clear_color.base_array_layer = base_array_layer;
    command->copy.clear_color.array_layer_count = array_layer_count;
    command->copy.clear_color.rectangles = rect_copy;
    command->copy.clear_color.rectangle_count = rectangle_count;
    gpu_ir_resource_retain(texture);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_clear_color_typed(gpu_ir_stream *s, gpu_ir_resource *texture,
                                    gpu_ir_clear_color_value_type value_type, const void *color,
                                    const gpu_ir_rect *rectangles, uint32_t rectangle_count, uint32_t *index) {
    return gpu_ir_record_clear_color_layers_typed(s, texture, value_type, color, 0, 0,
                                                  rectangles, rectangle_count, index);
}

int gpu_ir_record_clear_color(gpu_ir_stream *s, gpu_ir_resource *texture, const float color[4],
                              const gpu_ir_rect *rectangles, uint32_t rectangle_count, uint32_t *index) {
    return gpu_ir_record_clear_color_typed(s, texture, GPU_IR_CLEAR_COLOR_VALUE_FLOAT, color,
                                           rectangles, rectangle_count, index);
}

int gpu_ir_record_clear_depth_stencil(gpu_ir_stream *s, gpu_ir_resource *texture, uint32_t aspects,
                                      float depth, uint32_t stencil, const gpu_ir_rect *rectangles,
                                      uint32_t rectangle_count, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    if (!s || !gpu_ir_texture_description(texture) || !aspects ||
        (aspects & ~GPU_IR_CLEAR_DEPTH_STENCIL_ALL) || (rectangle_count && !rectangles)) return GPU_IR_INVALID;
    if (s->count == UINT32_MAX) return GPU_IR_OUT_OF_MEMORY;
    gpu_ir_rect *rect_copy = copy_clear_rectangles(rectangles, rectangle_count);
    if (rectangle_count && !rect_copy) return GPU_IR_OUT_OF_MEMORY;
    gpu_ir_command *command = stream_append_command(s, GPU_IR_OP_CLEAR_DEPTH_STENCIL);
    if (!command) { free(rect_copy); return GPU_IR_OUT_OF_MEMORY; }
    command->dst = texture;
    command->copy.clear_depth_stencil = (gpu_ir_clear_depth_stencil_info){
        .aspects = aspects, .depth = depth, .stencil = stencil,
        .rectangles = rect_copy, .rectangle_count = rectangle_count
    };
    gpu_ir_resource_retain(texture);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

static int stream_render_pass_active(const gpu_ir_stream *stream) {
    if (!stream) return 0;
    int active = 0;
    const gpu_ir_command *commands = stream->commands;
    for (uint32_t i = 0; i < stream->count; i++) {
        if (commands[i].op == GPU_IR_OP_BEGIN_RENDER_PASS) {
            if (active) return -1;
            active = 1;
        } else if (commands[i].op == GPU_IR_OP_END_RENDER_PASS) {
            if (!active) return -1;
            active = 0;
        }
    }
    return active;
}

int gpu_ir_format_is_depth(uint32_t format) {
    return format == GPU_IR_FORMAT_D16_UNORM || format == GPU_IR_FORMAT_D32_FLOAT ||
           format == GPU_IR_FORMAT_D32_FLOAT_S8_UINT;
}

static int record_begin_render_pass(gpu_ir_stream *s, gpu_ir_resource *color_attachment,
                                    gpu_ir_resource *depth_attachment,
                                    const gpu_ir_render_pass_desc *description, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    const gpu_ir_texture_desc *texture = gpu_ir_texture_description(color_attachment);
    if (!s || !texture || !description || texture->is_3d || texture->sample_count != 1 ||
        description->load_action > GPU_IR_RENDER_PASS_DONT_CARE ||
        description->store_action > GPU_IR_RENDER_PASS_STORE_DONT_CARE ||
        (description->load_action == GPU_IR_RENDER_PASS_CLEAR &&
         description->clear_value_type > GPU_IR_CLEAR_COLOR_VALUE_SINT) ||
        description->mip_level >= texture->mip_levels || description->array_layer >= texture->array_layers ||
        description->render_area.left < 0 || description->render_area.top < 0 ||
        description->render_area.right <= description->render_area.left ||
        description->render_area.bottom <= description->render_area.top) return GPU_IR_INVALID;
    uint32_t width = mip_extent(texture->width, description->mip_level);
    uint32_t height = mip_extent(texture->height, description->mip_level);
    if ((uint32_t)description->render_area.right > width ||
        (uint32_t)description->render_area.bottom > height) return GPU_IR_RANGE;
    if (depth_attachment) {
        // (the depth attachment: one level, one layer, the color level's size, single-sample like it)
        const gpu_ir_texture_desc *depth = gpu_ir_texture_description(depth_attachment);
        if (!depth || !gpu_ir_format_is_depth(depth->format) || depth->is_3d || depth->sample_count != 1 ||
            depth->width != width || depth->height != height ||
            description->depth_load_action > GPU_IR_RENDER_PASS_DONT_CARE ||
            description->depth_store_action > GPU_IR_RENDER_PASS_STORE_DONT_CARE ||
            (description->depth_load_action == GPU_IR_RENDER_PASS_CLEAR &&
             !(description->clear_depth >= 0.0f && description->clear_depth <= 1.0f))) return GPU_IR_INVALID;
    }
    if (stream_render_pass_active(s) != 0) return GPU_IR_INVALID;
    gpu_ir_command *command = stream_append_command(s, GPU_IR_OP_BEGIN_RENDER_PASS);
    if (!command) return GPU_IR_OUT_OF_MEMORY;
    command->src = color_attachment;
    command->dst = depth_attachment;            // (released with the command, as every command's src and dst)
    command->copy.render_pass = *description;
    gpu_ir_resource_retain(color_attachment);
    if (depth_attachment) gpu_ir_resource_retain(depth_attachment);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_begin_render_pass(gpu_ir_stream *s, gpu_ir_resource *color_attachment,
                                    const gpu_ir_render_pass_desc *description, uint32_t *index) {
    return record_begin_render_pass(s, color_attachment, NULL, description, index);
}

int gpu_ir_record_begin_render_pass_depth(gpu_ir_stream *s, gpu_ir_resource *color_attachment,
                                          gpu_ir_resource *depth_attachment,
                                          const gpu_ir_render_pass_desc *description, uint32_t *index) {
    if (!depth_attachment) {
        if (index) *index = GPU_IR_NO_COMMAND;
        return GPU_IR_INVALID;
    }
    return record_begin_render_pass(s, color_attachment, depth_attachment, description, index);
}

int gpu_ir_record_end_render_pass(gpu_ir_stream *s, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    if (!s || stream_render_pass_active(s) != 1) return GPU_IR_INVALID;
    gpu_ir_command *command = stream_append_command(s, GPU_IR_OP_END_RENDER_PASS);
    if (!command) return GPU_IR_OUT_OF_MEMORY;
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_copy_buffer(gpu_ir_stream *s, gpu_ir_resource *src, uint64_t so,
                              gpu_ir_resource *dst, uint64_t dso, uint64_t size, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    if (!s || !gpu_ir_resource_is_buffer(src) || !gpu_ir_resource_is_buffer(dst)) return GPU_IR_INVALID;
    if (!size) return GPU_IR_OK;
    if (so > src->desc.buffer_size || size > src->desc.buffer_size - so ||
        dso > dst->desc.buffer_size || size > dst->desc.buffer_size - dso) return GPU_IR_RANGE;
    if (s->count == UINT32_MAX) return GPU_IR_OUT_OF_MEMORY;
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) return GPU_IR_OUT_OF_MEMORY;
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_COPY_BUFFER, .src = src, .dst = dst,
                             .copy.buffer = { so, dso, size } };
    gpu_ir_resource_retain(src);
    gpu_ir_resource_retain(dst);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_update_buffer(gpu_ir_stream *s, gpu_ir_resource *dst, uint64_t dst_offset,
                                const void *data, uint64_t size, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    if (!s || !gpu_ir_resource_is_buffer(dst) || !data || !size) return GPU_IR_INVALID;
    if (dst_offset > dst->desc.buffer_size || size > dst->desc.buffer_size - dst_offset || size > SIZE_MAX)
        return GPU_IR_RANGE;
    if (s->count == UINT32_MAX) return GPU_IR_OUT_OF_MEMORY;
    uint8_t *owned_data = malloc((size_t)size);
    if (!owned_data) return GPU_IR_OUT_OF_MEMORY;
    memcpy(owned_data, data, (size_t)size);
    if (s->count == s->capacity) {
        uint32_t capacity = s->capacity ? s->capacity * 2 : 16;
        if (capacity < s->capacity || (size_t)capacity > SIZE_MAX / sizeof(gpu_ir_command)) {
            free(owned_data);
            return GPU_IR_OUT_OF_MEMORY;
        }
        gpu_ir_command *commands = realloc(s->commands, (size_t)capacity * sizeof *commands);
        if (!commands) {
            free(owned_data);
            return GPU_IR_OUT_OF_MEMORY;
        }
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(capacity - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = commands;
        s->capacity = capacity;
    }
    gpu_ir_command *command = &((gpu_ir_command *)s->commands)[s->count];
    *command = (gpu_ir_command){ .op = GPU_IR_OP_UPDATE_BUFFER, .dst = dst,
        .copy.update_buffer = { dst_offset, size, owned_data } };
    gpu_ir_resource_retain(dst);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_fill_buffer(gpu_ir_stream *s, gpu_ir_resource *dst, uint64_t dst_offset,
                              uint64_t size, uint32_t value, uint32_t *index) {
    return gpu_ir_record_fill_buffer_pattern(s, dst, dst_offset, size, &value, sizeof value, index);
}

int gpu_ir_record_fill_buffer_pattern(gpu_ir_stream *s, gpu_ir_resource *dst, uint64_t dst_offset,
                                      uint64_t size, const void *pattern, uint32_t pattern_size,
                                      uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    if (!s || !pattern || !gpu_ir_resource_is_buffer(dst) || !pattern_size ||
        pattern_size > GPU_IR_MAX_FILL_PATTERN_SIZE || (pattern_size & (pattern_size - 1u)))
        return GPU_IR_INVALID;
    if (dst_offset % pattern_size || size % pattern_size) return GPU_IR_INVALID;
    if (dst_offset > dst->desc.buffer_size || size > dst->desc.buffer_size - dst_offset)
        return GPU_IR_RANGE;
    if (!size) return GPU_IR_OK;
    if (s->count == UINT32_MAX) return GPU_IR_OUT_OF_MEMORY;
    if (s->count == s->capacity) {
        uint32_t capacity = s->capacity ? s->capacity * 2 : 16;
        if (capacity < s->capacity || (size_t)capacity > SIZE_MAX / sizeof(gpu_ir_command))
            return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_command *commands = realloc(s->commands, (size_t)capacity * sizeof *commands);
        if (!commands) return GPU_IR_OUT_OF_MEMORY;
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(capacity - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = commands;
        s->capacity = capacity;
    }
    gpu_ir_command *command = &((gpu_ir_command *)s->commands)[s->count];
    *command = (gpu_ir_command){ .op = GPU_IR_OP_FILL_BUFFER, .dst = dst,
        .copy.fill_buffer = { .dst_offset = dst_offset, .size = size, .pattern_size = pattern_size } };
    memcpy(command->copy.fill_buffer.pattern, pattern, pattern_size);
    if (pattern_size == sizeof command->copy.fill_buffer.value)
        memcpy(&command->copy.fill_buffer.value, pattern, sizeof command->copy.fill_buffer.value);
    gpu_ir_resource_retain(dst);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_copy_resource(gpu_ir_stream *s, gpu_ir_resource *src, gpu_ir_resource *dst, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    if (!s || !src || !dst || src == dst || src->kind != dst->kind) return GPU_IR_INVALID;
    if (src->kind == GPU_IR_RESOURCE_BUFFER) {
        if (src->desc.buffer_size != dst->desc.buffer_size) return GPU_IR_RANGE;
    } else {
        const gpu_ir_texture_desc *a = &src->desc.texture, *b = &dst->desc.texture;
        if (a->width != b->width || a->height != b->height || a->depth != b->depth ||
            a->array_layers != b->array_layers || a->mip_levels != b->mip_levels ||
            a->bytes_per_texel != b->bytes_per_texel || a->block_width != b->block_width ||
            a->block_height != b->block_height || a->bytes_per_block != b->bytes_per_block ||
            a->sample_count != b->sample_count ||
            a->is_3d != b->is_3d) return GPU_IR_RANGE;
    }
    if (s->count == UINT32_MAX) return GPU_IR_OUT_OF_MEMORY;
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) return GPU_IR_OUT_OF_MEMORY;
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_COPY_RESOURCE, .src = src, .dst = dst };
    gpu_ir_resource_retain(src);
    gpu_ir_resource_retain(dst);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

static uint32_t mip_extent(uint32_t value, uint32_t mip) {
    return mip >= 32 ? 1 : (value >> mip) ? (value >> mip) : 1;
}

static int add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (b > UINT64_MAX - a) return 0;
    *out = a + b;
    return 1;
}

static int mul_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (a && b > UINT64_MAX / a) return 0;
    *out = a * b;
    return 1;
}

static uint64_t ceil_div_u64(uint64_t value, uint32_t divisor) {
    return value / divisor + (value % divisor != 0);
}

static int buffer_copy_fits(const gpu_ir_resource *buffer, const gpu_ir_texture_location *location,
                            const gpu_ir_texture_desc *texture, uint32_t width, uint32_t height, uint32_t depth) {
    uint64_t right, bottom, min_image_pitch, end_x, end_y, row_end;
    if (location->origin_x > location->buffer_width || width > location->buffer_width - location->origin_x ||
        location->origin_y > location->buffer_height || height > location->buffer_height - location->origin_y ||
        location->origin_z > location->buffer_depth || depth > location->buffer_depth - location->origin_z) return 0;
    uint32_t bw = texture->block_width, bh = texture->block_height;
    if ((location->origin_x % bw) || (location->origin_y % bh) ||
        ((width % bw) && ceil_div_u64((uint64_t)location->origin_x + width, bw) * bw != location->buffer_width) ||
        ((height % bh) && ceil_div_u64((uint64_t)location->origin_y + height, bh) * bh != location->buffer_height) ||
        location->row_pitch % texture->bytes_per_block) return 0;
    end_x = (uint64_t)location->origin_x + width;
    end_y = (uint64_t)location->origin_y + height;
    row_end = ceil_div_u64(end_y, bh);
    if (!mul_u64(ceil_div_u64(end_x, bw), texture->bytes_per_block, &right) ||
        !mul_u64(location->row_pitch, row_end, &min_image_pitch)) return 0;
    if (location->row_pitch < right || location->image_pitch < min_image_pitch) return 0;
    if (!mul_u64(row_end - 1, location->row_pitch, &bottom)) return 0;
    uint64_t end = location->buffer_offset, term;
    if (!mul_u64((uint64_t)location->origin_z + depth - 1, location->image_pitch, &term) || !add_u64(end, term, &end) ||
        !add_u64(end, bottom, &end) || !mul_u64(ceil_div_u64(end_x, bw), texture->bytes_per_block, &term) ||
        !add_u64(end, term, &end)) return 0;
    return end <= buffer->desc.buffer_size;
}

static int texture_location_fits(const gpu_ir_resource *resource, const gpu_ir_texture_location *location,
                                 uint32_t width, uint32_t height, uint32_t depth) {
    const gpu_ir_texture_desc *desc = gpu_ir_texture_description(resource);
    if (!desc || location->mip_level >= desc->mip_levels) return 0;
    if (desc->is_3d) {
        if (location->array_slice != 0) return 0;
    } else if (location->array_slice >= desc->array_layers || location->origin_z || depth != 1) return 0;
    uint32_t mip_width = mip_extent(desc->width, location->mip_level);
    uint32_t mip_height = mip_extent(desc->height, location->mip_level);
    uint32_t mip_depth = desc->is_3d ? mip_extent(desc->depth, location->mip_level) : 1;
    if (location->origin_x > mip_width || width > mip_width - location->origin_x ||
        location->origin_y > mip_height || height > mip_height - location->origin_y ||
        location->origin_z > mip_depth || depth > mip_depth - location->origin_z) return 0;
    if ((location->origin_x % desc->block_width) || (location->origin_y % desc->block_height) ||
        ((width % desc->block_width) && (uint64_t)location->origin_x + width != mip_width) ||
        ((height % desc->block_height) && (uint64_t)location->origin_y + height != mip_height)) return 0;
    return 1;
}

int gpu_ir_record_copy_texture(gpu_ir_stream *s, gpu_ir_resource *src,
                               const gpu_ir_texture_location *src_location, gpu_ir_resource *dst,
                               const gpu_ir_texture_location *dst_location, uint32_t width,
                               uint32_t height, uint32_t depth, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    if (!s || !src || !dst || !src_location || !dst_location) return GPU_IR_INVALID;
    if (!width || !height || !depth) return GPU_IR_OK;
    const gpu_ir_texture_desc *src_texture = gpu_ir_texture_description(src);
    const gpu_ir_texture_desc *dst_texture = gpu_ir_texture_description(dst);
    if ((!src_texture && !gpu_ir_resource_is_buffer(src)) || (!dst_texture && !gpu_ir_resource_is_buffer(dst)) ||
        (!src_texture && !dst_texture)) return GPU_IR_INVALID;
    if ((src_texture && !texture_location_fits(src, src_location, width, height, depth)) ||
        (dst_texture && !texture_location_fits(dst, dst_location, width, height, depth))) return GPU_IR_RANGE;
    const gpu_ir_texture_desc *format = src_texture ? src_texture : dst_texture;
    if (!format->bytes_per_block) return GPU_IR_UNSUPPORTED;
    if ((src_texture && dst_texture &&
         (src_texture->bytes_per_texel != dst_texture->bytes_per_texel ||
          src_texture->block_width != dst_texture->block_width || src_texture->block_height != dst_texture->block_height ||
          src_texture->bytes_per_block != dst_texture->bytes_per_block ||
          src_texture->sample_count != dst_texture->sample_count)) ||
        ((src_texture && !dst_texture && src_texture->sample_count != 1) ||
         (dst_texture && !src_texture && dst_texture->sample_count != 1)) ||
        (!src_texture && !buffer_copy_fits(src, src_location, dst_texture, width, height, depth)) ||
        (!dst_texture && !buffer_copy_fits(dst, dst_location, src_texture, width, height, depth))) return GPU_IR_RANGE;
    if (s->count == UINT32_MAX) return GPU_IR_OUT_OF_MEMORY;
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) return GPU_IR_OUT_OF_MEMORY;
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_COPY_TEXTURE, .src = src, .dst = dst,
                             .copy.texture = { *src_location, *dst_location, width, height, depth } };
    gpu_ir_resource_retain(src);
    gpu_ir_resource_retain(dst);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_resolve_texture_region(gpu_ir_stream *s, gpu_ir_resource *src,
                                         const gpu_ir_resolve_location *src_location, gpu_ir_resource *dst,
                                         const gpu_ir_resolve_location *dst_location,
                                         const gpu_ir_resolve_region *region, uint32_t mode, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    if (!s || !src || !dst || src == dst || !src_location || !dst_location || !region ||
        mode < GPU_IR_RESOLVE_MODE_AVERAGE || mode > GPU_IR_RESOLVE_MODE_MAX) return GPU_IR_INVALID;
    const gpu_ir_texture_desc *source = gpu_ir_texture_description(src);
    const gpu_ir_texture_desc *destination = gpu_ir_texture_description(dst);
    if (!source || !destination || source->sample_count <= 1 || destination->sample_count != 1 ||
        source->bytes_per_texel != destination->bytes_per_texel ||
        source->block_width != destination->block_width || source->block_height != destination->block_height ||
        source->bytes_per_block != destination->bytes_per_block || source->is_3d != destination->is_3d ||
        source->block_width != 1 || source->block_height != 1 || src_location->mip_level >= source->mip_levels ||
        dst_location->mip_level >= destination->mip_levels || src_location->array_slice >= source->array_layers ||
        dst_location->array_slice >= destination->array_layers) return GPU_IR_INVALID;
    uint32_t src_width = mip_extent(source->width, src_location->mip_level);
    uint32_t src_height = mip_extent(source->height, src_location->mip_level);
    uint32_t dst_width = mip_extent(destination->width, dst_location->mip_level);
    uint32_t dst_height = mip_extent(destination->height, dst_location->mip_level);
    if (!region->width || !region->height || region->source_x > src_width || region->source_y > src_height ||
        region->width > src_width - region->source_x || region->height > src_height - region->source_y ||
        region->destination_x > dst_width || region->destination_y > dst_height ||
        region->width > dst_width - region->destination_x || region->height > dst_height - region->destination_y)
        return GPU_IR_RANGE;
    if (s->count == UINT32_MAX) return GPU_IR_OUT_OF_MEMORY;
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) return GPU_IR_OUT_OF_MEMORY;
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_RESOLVE_TEXTURE, .src = src, .dst = dst,
                             .copy.resolve = { *src_location, *dst_location, *region, mode } };
    gpu_ir_resource_retain(src);
    gpu_ir_resource_retain(dst);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_resolve_texture(gpu_ir_stream *s, gpu_ir_resource *src,
                                  const gpu_ir_resolve_location *src_location, gpu_ir_resource *dst,
                                  const gpu_ir_resolve_location *dst_location, uint32_t mode, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    const gpu_ir_texture_desc *source = gpu_ir_texture_description(src);
    const gpu_ir_texture_desc *destination = gpu_ir_texture_description(dst);
    if (!source || !destination || !src_location || !dst_location ||
        src_location->mip_level >= source->mip_levels || dst_location->mip_level >= destination->mip_levels ||
        src_location->array_slice >= source->array_layers || dst_location->array_slice >= destination->array_layers)
        return GPU_IR_INVALID;
    uint32_t width = mip_extent(source->width, src_location->mip_level);
    uint32_t height = mip_extent(source->height, src_location->mip_level);
    uint32_t depth = source->is_3d ? mip_extent(source->depth, src_location->mip_level) : 1;
    uint32_t dst_depth = destination->is_3d ? mip_extent(destination->depth, dst_location->mip_level) : 1;
    if (width != mip_extent(destination->width, dst_location->mip_level) ||
        height != mip_extent(destination->height, dst_location->mip_level) || depth != dst_depth ||
        source->array_layers != destination->array_layers) return GPU_IR_RANGE;
    gpu_ir_resolve_region region = { .width = width, .height = height };
    return gpu_ir_record_resolve_texture_region(s, src, src_location, dst, dst_location, &region, mode, index);
}

int gpu_ir_record_barrier(gpu_ir_stream *s, const gpu_ir_barrier_desc *barrier, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    if (!s || !barrier || barrier->type < GPU_IR_BARRIER_TRANSITION || barrier->type > GPU_IR_BARRIER_IMAGE_DEPENDENCY)
        return GPU_IR_INVALID;
    if ((barrier->type == GPU_IR_BARRIER_TRANSITION && !barrier->resource) ||
        (barrier->type == GPU_IR_BARRIER_UAV && barrier->resource_after) ||
        (barrier->type != GPU_IR_BARRIER_ALIASING && barrier->resource_after) ||
        (barrier->flags & ~(GPU_IR_BARRIER_BEGIN_ONLY | GPU_IR_BARRIER_END_ONLY)) ||
        (barrier->flags == (GPU_IR_BARRIER_BEGIN_ONLY | GPU_IR_BARRIER_END_ONLY)) ||
        (barrier->flags && barrier->type != GPU_IR_BARRIER_TRANSITION && barrier->type != GPU_IR_BARRIER_GLOBAL))
        return GPU_IR_INVALID;
    if ((barrier->before_access | barrier->after_access) & ~GPU_IR_ACCESS_ALL ||
        !valid_sync_stage_scope(barrier->source_stage_scope) ||
        !valid_sync_stage_scope(barrier->destination_stage_scope) ||
        (barrier->dependency_flags & ~GPU_IR_BARRIER_DEPENDENCY_ALL)) return GPU_IR_INVALID;

    const int is_scoped_type = barrier->type >= GPU_IR_BARRIER_MEMORY_DEPENDENCY &&
        barrier->type <= GPU_IR_BARRIER_IMAGE_DEPENDENCY;
    if (is_scoped_type) {
        if (barrier->flags || barrier->resource_after) return GPU_IR_INVALID;
        if ((barrier->source_queue_family == GPU_IR_QUEUE_FAMILY_IGNORED) !=
            (barrier->destination_queue_family == GPU_IR_QUEUE_FAMILY_IGNORED)) return GPU_IR_INVALID;
        if (barrier->type == GPU_IR_BARRIER_MEMORY_DEPENDENCY) {
            if (barrier->resource || barrier->old_layout || barrier->new_layout ||
                barrier->buffer_offset || barrier->buffer_size || barrier->image_aspects ||
                barrier->base_mip_level || barrier->mip_level_count || barrier->base_array_layer ||
                barrier->array_layer_count || barrier->source_queue_family != barrier->destination_queue_family)
                return GPU_IR_INVALID;
        } else if (barrier->type == GPU_IR_BARRIER_BUFFER_DEPENDENCY) {
            if (!gpu_ir_resource_is_buffer(barrier->resource)) return GPU_IR_INVALID;
            if (barrier->old_layout || barrier->new_layout || barrier->image_aspects ||
                barrier->base_mip_level || barrier->mip_level_count || barrier->base_array_layer ||
                barrier->array_layer_count) return GPU_IR_INVALID;
            uint64_t size = gpu_ir_buffer_size(barrier->resource);
            if (barrier->buffer_size == 0 || barrier->buffer_offset > size ||
                barrier->buffer_size > size - barrier->buffer_offset) return GPU_IR_RANGE;
        } else {
            const gpu_ir_texture_desc *texture = gpu_ir_texture_description(barrier->resource);
            const uint32_t known_aspects = GPU_IR_IMAGE_ASPECT_ALL;
            if (!texture) return GPU_IR_INVALID;
            if (barrier->old_layout < GPU_IR_IMAGE_LAYOUT_UNDEFINED ||
                barrier->old_layout > GPU_IR_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE ||
                barrier->new_layout < GPU_IR_IMAGE_LAYOUT_UNDEFINED ||
                barrier->new_layout > GPU_IR_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE ||
                !barrier->image_aspects || (barrier->image_aspects & ~known_aspects) ||
                !barrier->mip_level_count || !barrier->array_layer_count ||
                barrier->buffer_offset || barrier->buffer_size) return GPU_IR_INVALID;
            if (barrier->base_mip_level >= texture->mip_levels ||
                barrier->mip_level_count > texture->mip_levels - barrier->base_mip_level ||
                barrier->base_array_layer >= texture->array_layers ||
                barrier->array_layer_count > texture->array_layers - barrier->base_array_layer)
                return GPU_IR_RANGE;
        }
    } else if (barrier->source_stage_scope || barrier->destination_stage_scope || barrier->dependency_flags ||
               barrier->old_layout || barrier->new_layout || barrier->buffer_offset || barrier->buffer_size ||
               barrier->image_aspects || barrier->base_mip_level || barrier->mip_level_count ||
               barrier->base_array_layer || barrier->array_layer_count) return GPU_IR_INVALID;
    if (s->count == UINT32_MAX) return GPU_IR_OUT_OF_MEMORY;
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) return GPU_IR_OUT_OF_MEMORY;
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_BARRIER, .src = barrier->resource, .dst = barrier->resource_after,
                             .copy.barrier = *barrier };
    gpu_ir_resource_retain(barrier->resource);
    gpu_ir_resource_retain(barrier->resource_after);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_dispatch(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                           uint32_t x, uint32_t y, uint32_t z, uint32_t *index) {
    return gpu_ir_record_dispatch_with_accesses(s, pipeline, x, y, z, NULL, 0, index);
}

int gpu_ir_record_dispatch_with_accesses(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                         uint32_t x, uint32_t y, uint32_t z,
                                         const gpu_ir_resource_access *accesses, uint32_t access_count,
                                         uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!s || !info || info->kind != GPU_IR_PIPELINE_COMPUTE) return GPU_IR_INVALID;
    gpu_ir_resource_access *access_copy = NULL;
    int rc = copy_resource_accesses(accesses, access_count, &access_copy);
    if (rc != GPU_IR_OK) return rc;
    if (!resource_accesses_match_pipeline(access_copy, access_count, info)) {
        release_resource_accesses(access_copy, access_count);
        return GPU_IR_INVALID;
    }
    if (!x || !y || !z) { release_resource_accesses(access_copy, access_count); return GPU_IR_OK; }
    if (s->count == UINT32_MAX) { release_resource_accesses(access_copy, access_count); return GPU_IR_OUT_OF_MEMORY; }
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) {
            release_resource_accesses(access_copy, access_count);
            return GPU_IR_OUT_OF_MEMORY;
        }
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) { release_resource_accesses(access_copy, access_count); return GPU_IR_OUT_OF_MEMORY; }
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_DISPATCH, .pipeline = pipeline,
                             .accesses = access_copy, .access_count = access_count,
                             .copy.dispatch = { x, y, z } };
    gpu_ir_pipeline_retain(pipeline);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_dispatch_indirect(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                    gpu_ir_resource *arguments, uint64_t offset, uint32_t *index) {
    return gpu_ir_record_dispatch_indirect_with_accesses(s, pipeline, arguments, offset, NULL, 0, index);
}

int gpu_ir_record_dispatch_indirect_with_accesses(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                                  gpu_ir_resource *arguments, uint64_t offset,
                                                  const gpu_ir_resource_access *accesses, uint32_t access_count,
                                                  uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!s || !info || info->kind != GPU_IR_PIPELINE_COMPUTE || !gpu_ir_resource_is_buffer(arguments) || (offset & 3))
        return GPU_IR_INVALID;
    uint64_t size = gpu_ir_buffer_size(arguments);
    if (offset > size || 12 > size - offset) return GPU_IR_RANGE;
    gpu_ir_resource_access *access_copy = NULL;
    int rc = copy_resource_accesses(accesses, access_count, &access_copy);
    if (rc != GPU_IR_OK) return rc;
    if (!resource_accesses_match_pipeline(access_copy, access_count, info)) {
        release_resource_accesses(access_copy, access_count);
        return GPU_IR_INVALID;
    }
    if (s->count == UINT32_MAX) { release_resource_accesses(access_copy, access_count); return GPU_IR_OUT_OF_MEMORY; }
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) {
            release_resource_accesses(access_copy, access_count);
            return GPU_IR_OUT_OF_MEMORY;
        }
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) { release_resource_accesses(access_copy, access_count); return GPU_IR_OUT_OF_MEMORY; }
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_DISPATCH_INDIRECT, .src = arguments, .pipeline = pipeline,
                             .accesses = access_copy, .access_count = access_count,
                             .copy.buffer.src_offset = offset };
    gpu_ir_resource_retain(arguments);
    gpu_ir_pipeline_retain(pipeline);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_dispatch_indirect_range(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                          gpu_ir_resource *arguments,
                                          const gpu_ir_indirect_dispatch_info *dispatch, uint32_t *index) {
    return gpu_ir_record_dispatch_indirect_range_with_accesses(s, pipeline, arguments, dispatch, NULL, 0, index);
}

int gpu_ir_record_dispatch_indirect_range_with_accesses(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                                        gpu_ir_resource *arguments,
                                                        const gpu_ir_indirect_dispatch_info *dispatch,
                                                        const gpu_ir_resource_access *accesses,
                                                        uint32_t access_count, uint32_t *index) {
    return gpu_ir_record_dispatch_indirect_range_counted_with_accesses(s, pipeline, arguments, NULL,
                                                                        dispatch, accesses, access_count, index);
}

int gpu_ir_record_dispatch_indirect_range_counted(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                                  gpu_ir_resource *arguments, gpu_ir_resource *count_arguments,
                                                  const gpu_ir_indirect_dispatch_info *dispatch, uint32_t *index) {
    return gpu_ir_record_dispatch_indirect_range_counted_with_accesses(s, pipeline, arguments, count_arguments,
                                                                        dispatch, NULL, 0, index);
}

int gpu_ir_record_dispatch_indirect_range_counted_with_accesses(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                                                gpu_ir_resource *arguments,
                                                                gpu_ir_resource *count_arguments,
                                                                const gpu_ir_indirect_dispatch_info *dispatch,
                                                                const gpu_ir_resource_access *accesses,
                                                                uint32_t access_count, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!s || !dispatch || !info || info->kind != GPU_IR_PIPELINE_COMPUTE ||
        !gpu_ir_resource_is_buffer(arguments) || (count_arguments && !gpu_ir_resource_is_buffer(count_arguments)) ||
        dispatch->stride < 12 || (dispatch->stride & 3) || (dispatch->argument_offset & 3) ||
        (count_arguments && (dispatch->count_offset & 3))) return GPU_IR_INVALID;
    if (count_arguments && (dispatch->count_offset > gpu_ir_buffer_size(count_arguments) ||
        4 > gpu_ir_buffer_size(count_arguments) - dispatch->count_offset)) return GPU_IR_RANGE;
    if (dispatch->dispatch_count) {
        uint64_t last_offset, required_end;
        uint64_t buffer_size = gpu_ir_buffer_size(arguments);
        if (!mul_u64((uint64_t)dispatch->dispatch_count - 1, dispatch->stride, &last_offset) ||
            !add_u64(dispatch->argument_offset, last_offset, &required_end) ||
            !add_u64(required_end, 12, &required_end) || required_end > buffer_size) return GPU_IR_RANGE;
    } else {
        if (dispatch->argument_offset > gpu_ir_buffer_size(arguments)) return GPU_IR_RANGE;
    }

    gpu_ir_resource_access *access_copy = NULL;
    int rc = copy_resource_accesses(accesses, access_count, &access_copy);
    if (rc != GPU_IR_OK) return rc;
    if (!resource_accesses_match_pipeline(access_copy, access_count, info)) {
        release_resource_accesses(access_copy, access_count);
        return GPU_IR_INVALID;
    }
    if (!dispatch->dispatch_count) {
        release_resource_accesses(access_copy, access_count);
        return GPU_IR_OK;
    }
    if (s->count == UINT32_MAX) { release_resource_accesses(access_copy, access_count); return GPU_IR_OUT_OF_MEMORY; }
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) {
            release_resource_accesses(access_copy, access_count);
            return GPU_IR_OUT_OF_MEMORY;
        }
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) { release_resource_accesses(access_copy, access_count); return GPU_IR_OUT_OF_MEMORY; }
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_DISPATCH_INDIRECT_RANGE, .src = arguments,
                             .dst = count_arguments, .pipeline = pipeline,
                             .accesses = access_copy, .access_count = access_count,
                             .copy.dispatch_indirect_range = *dispatch };
    gpu_ir_resource_retain(arguments);
    gpu_ir_resource_retain(count_arguments);
    gpu_ir_pipeline_retain(pipeline);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_draw(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                       const gpu_ir_draw_info *draw, uint32_t *index) {
    return gpu_ir_record_draw_with_accesses(s, pipeline, draw, NULL, 0, index);
}

int gpu_ir_record_draw_with_accesses(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                     const gpu_ir_draw_info *draw,
                                     const gpu_ir_resource_access *accesses, uint32_t access_count,
                                     uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!s || !draw || !info || info->kind != GPU_IR_PIPELINE_GRAPHICS ||
        draw->topology < GPU_IR_TOPOLOGY_POINT_LIST || draw->topology > GPU_IR_TOPOLOGY_TRIANGLE_STRIP ||
        draw->indexed > 1 || draw->vertex_buffer_count > GPU_IR_MAX_VERTEX_BUFFERS ||
        draw->vertex_buffers_captured > 1 ||
        draw->has_viewport > 1 || draw->has_scissor > 1 || draw->has_blend_constants > 1) return GPU_IR_INVALID;
    int has_work = draw->instance_count && (draw->indexed ? draw->index_count : draw->vertex_count);
    if (draw->has_viewport && (!isfinite(draw->viewport.x) || !isfinite(draw->viewport.y) ||
        !isfinite(draw->viewport.width) || !isfinite(draw->viewport.height) ||
        !isfinite(draw->viewport.min_depth) || !isfinite(draw->viewport.max_depth) ||
        draw->viewport.width <= 0.0f || draw->viewport.height == 0.0f)) return GPU_IR_INVALID;
    if (draw->indexed) {
        if (draw->vertex_count || draw->first_vertex) return GPU_IR_INVALID;
        if (has_work && draw->vertex_buffers_captured && !draw->index_buffer) return GPU_IR_INVALID;
        if (draw->index_buffer) {
            uint64_t index_size = draw->index_type == GPU_IR_INDEX_TYPE_UINT16 ? 2u :
                draw->index_type == GPU_IR_INDEX_TYPE_UINT32 ? 4u : 0u;
            if (!index_size || !gpu_ir_resource_is_buffer(draw->index_buffer) ||
                draw->index_buffer_offset % index_size) return GPU_IR_INVALID;
            uint64_t buffer_size = gpu_ir_buffer_size(draw->index_buffer);
            if (!draw->index_buffer_size || draw->index_buffer_offset > buffer_size ||
                draw->index_buffer_size > buffer_size - draw->index_buffer_offset) return GPU_IR_RANGE;
            uint64_t first_offset, bytes;
            if (!mul_u64(draw->first_index, index_size, &first_offset) ||
                !mul_u64(draw->index_count, index_size, &bytes) || first_offset > draw->index_buffer_size ||
                bytes > draw->index_buffer_size - first_offset) return GPU_IR_RANGE;
        } else if (draw->index_type || draw->index_buffer_offset || draw->index_buffer_size) return GPU_IR_INVALID;
    } else if (draw->index_count || draw->first_index || draw->vertex_offset || draw->index_type ||
               draw->index_buffer_offset || draw->index_buffer_size || draw->index_buffer) return GPU_IR_INVALID;
    for (uint32_t i = 0; i < draw->vertex_buffer_count; i++) {
        const gpu_ir_vertex_buffer_binding *binding = &draw->vertex_buffers[i];
        uint64_t buffer_size = gpu_ir_buffer_size(binding->resource);
        if (binding->binding >= GPU_IR_MAX_VERTEX_BUFFERS || !gpu_ir_resource_is_buffer(binding->resource) ||
            !binding->size || binding->offset > buffer_size || binding->size > buffer_size - binding->offset)
            return GPU_IR_INVALID;
        for (uint32_t previous = 0; previous < i; previous++)
            if (draw->vertex_buffers[previous].binding == binding->binding) return GPU_IR_INVALID;
    }
    for (uint32_t i = 0; has_work && i < info->vertex_input.attribute_count; i++) {
        uint32_t required_binding = info->vertex_input.attributes[i].binding;
        int found = 0;
        for (uint32_t binding = 0; binding < draw->vertex_buffer_count; binding++)
            if (draw->vertex_buffers[binding].binding == required_binding) { found = 1; break; }
        if (!found) return GPU_IR_INVALID;
    }
    if (has_work && draw->vertex_buffers_captured) {
        int vertex_range = validate_vertex_fetch_ranges(info, draw->vertex_buffers, draw->vertex_buffer_count,
            draw->indexed, draw->vertex_count, draw->first_vertex, draw->instance_count,
            draw->first_instance, 1);
        if (vertex_range != GPU_IR_OK) return vertex_range;
    }
    gpu_ir_resource_access *access_copy = NULL;
    int rc = copy_resource_accesses(accesses, access_count, &access_copy);
    if (rc != GPU_IR_OK) return rc;
    if (!resource_accesses_match_pipeline(access_copy, access_count, info)) {
        release_resource_accesses(access_copy, access_count);
        return GPU_IR_INVALID;
    }
    if (s->count == UINT32_MAX) { release_resource_accesses(access_copy, access_count); return GPU_IR_OUT_OF_MEMORY; }
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) {
            release_resource_accesses(access_copy, access_count);
            return GPU_IR_OUT_OF_MEMORY;
        }
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) { release_resource_accesses(access_copy, access_count); return GPU_IR_OUT_OF_MEMORY; }
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_DRAW, .src = draw->index_buffer, .pipeline = pipeline,
                             .accesses = access_copy, .access_count = access_count, .copy.draw = *draw };
    if (draw->index_buffer) gpu_ir_resource_retain(draw->index_buffer);
    for (uint32_t i = 0; i < draw->vertex_buffer_count; i++)
        gpu_ir_resource_retain(draw->vertex_buffers[i].resource);
    gpu_ir_pipeline_retain(pipeline);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_draw_indirect(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                gpu_ir_resource *arguments, const gpu_ir_indirect_draw_info *draw,
                                uint32_t *index) {
    return gpu_ir_record_draw_indirect_counted_with_accesses(s, pipeline, arguments, NULL, draw,
                                                              NULL, 0, index);
}

int gpu_ir_record_draw_indirect_counted(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                        gpu_ir_resource *arguments, gpu_ir_resource *count_arguments,
                                        const gpu_ir_indirect_draw_info *draw, uint32_t *index) {
    return gpu_ir_record_draw_indirect_counted_with_accesses(s, pipeline, arguments, count_arguments,
                                                              draw, NULL, 0, index);
}

int gpu_ir_record_draw_indirect_with_accesses(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                              gpu_ir_resource *arguments, const gpu_ir_indirect_draw_info *draw,
                                              const gpu_ir_resource_access *accesses, uint32_t access_count,
                                              uint32_t *index) {
    return gpu_ir_record_draw_indirect_counted_with_accesses(s, pipeline, arguments, NULL, draw,
                                                              accesses, access_count, index);
}

int gpu_ir_record_draw_indirect_counted_with_accesses(gpu_ir_stream *s, gpu_ir_pipeline *pipeline,
                                                       gpu_ir_resource *arguments,
                                                       gpu_ir_resource *count_arguments,
                                                       const gpu_ir_indirect_draw_info *draw,
                                                       const gpu_ir_resource_access *accesses,
                                                       uint32_t access_count, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!s || !draw || !info || info->kind != GPU_IR_PIPELINE_GRAPHICS ||
        !gpu_ir_resource_is_buffer(arguments) ||
        (count_arguments && !gpu_ir_resource_is_buffer(count_arguments)) ||
        draw->topology < GPU_IR_TOPOLOGY_POINT_LIST || draw->topology > GPU_IR_TOPOLOGY_TRIANGLE_STRIP ||
        draw->indexed > 1 || draw->has_viewport > 1 || draw->has_scissor > 1 ||
        draw->has_blend_constants > 1 ||
        draw->vertex_buffers_captured > 1 ||
        draw->vertex_buffer_count > GPU_IR_MAX_VERTEX_BUFFERS ||
        (draw->argument_offset & 3) || (draw->count_offset & 3) ||
        (draw->index_buffer && (!gpu_ir_resource_is_buffer(draw->index_buffer) || !draw->indexed ||
            (draw->index_type != GPU_IR_INDEX_TYPE_UINT16 && draw->index_type != GPU_IR_INDEX_TYPE_UINT32) ||
            (draw->index_buffer_offset & (draw->index_type == GPU_IR_INDEX_TYPE_UINT16 ? 1u : 3u)))) ||
        (!draw->index_buffer && (draw->index_type || draw->index_buffer_offset || draw->index_buffer_size)))
        return GPU_IR_INVALID;
    if (draw->draw_count && draw->vertex_buffers_captured && draw->indexed && !draw->index_buffer)
        return GPU_IR_INVALID;
    if (draw->index_buffer) {
        uint64_t buffer_size = gpu_ir_buffer_size(draw->index_buffer);
        if (!draw->index_buffer_size || draw->index_buffer_offset > buffer_size ||
            draw->index_buffer_size > buffer_size - draw->index_buffer_offset) return GPU_IR_RANGE;
    }
    for (uint32_t i = 0; i < draw->vertex_buffer_count; i++) {
        const gpu_ir_vertex_buffer_binding *binding = &draw->vertex_buffers[i];
        uint64_t buffer_size = gpu_ir_buffer_size(binding->resource);
        if (binding->binding >= GPU_IR_MAX_VERTEX_BUFFERS || !gpu_ir_resource_is_buffer(binding->resource) ||
            !binding->size || binding->offset > buffer_size || binding->size > buffer_size - binding->offset)
            return GPU_IR_INVALID;
        for (uint32_t previous = 0; previous < i; previous++)
            if (draw->vertex_buffers[previous].binding == binding->binding) return GPU_IR_INVALID;
    }
    for (uint32_t i = 0; draw->draw_count && draw->vertex_buffers_captured &&
         i < info->vertex_input.attribute_count; i++) {
        uint32_t required_binding = info->vertex_input.attributes[i].binding;
        int found = 0;
        for (uint32_t binding = 0; binding < draw->vertex_buffer_count; binding++)
            if (draw->vertex_buffers[binding].binding == required_binding) { found = 1; break; }
        if (!found) return GPU_IR_INVALID;
    }
    if (draw->draw_count && draw->vertex_buffers_captured) {
        int vertex_range = validate_vertex_fetch_ranges(info, draw->vertex_buffers, draw->vertex_buffer_count,
            draw->indexed, 0, 0, 0, 0, 0);
        if (vertex_range != GPU_IR_OK) return vertex_range;
    }
    uint32_t argument_size = draw->indexed ? 20 : 16;
    if (draw->stride < argument_size || (draw->stride & 3)) return GPU_IR_INVALID;
    uint64_t buffer_size = gpu_ir_buffer_size(arguments);
    if (draw->argument_offset > buffer_size) return GPU_IR_RANGE;
    if (!draw->draw_count) return GPU_IR_OK;
    uint64_t last_offset, required_end;
    if (!mul_u64((uint64_t)draw->draw_count - 1, draw->stride, &last_offset) ||
        !add_u64(draw->argument_offset, last_offset, &required_end) ||
        !add_u64(required_end, argument_size, &required_end) || required_end > buffer_size) return GPU_IR_RANGE;
    if (count_arguments && (draw->count_offset > gpu_ir_buffer_size(count_arguments) ||
        4 > gpu_ir_buffer_size(count_arguments) - draw->count_offset)) return GPU_IR_RANGE;

    gpu_ir_resource_access *access_copy = NULL;
    int rc = copy_resource_accesses(accesses, access_count, &access_copy);
    if (rc != GPU_IR_OK) return rc;
    if (!resource_accesses_match_pipeline(access_copy, access_count, info)) {
        release_resource_accesses(access_copy, access_count);
        return GPU_IR_INVALID;
    }
    if (s->count == UINT32_MAX) { release_resource_accesses(access_copy, access_count); return GPU_IR_OUT_OF_MEMORY; }
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) {
            release_resource_accesses(access_copy, access_count);
            return GPU_IR_OUT_OF_MEMORY;
        }
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) { release_resource_accesses(access_copy, access_count); return GPU_IR_OUT_OF_MEMORY; }
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_DRAW_INDIRECT, .src = arguments, .dst = count_arguments,
                             .pipeline = pipeline,
                             .accesses = access_copy, .access_count = access_count,
                             .copy.draw_indirect = *draw };
    gpu_ir_resource_retain(arguments);
    gpu_ir_resource_retain(count_arguments);
    gpu_ir_resource_retain(draw->index_buffer);
    for (uint32_t i = 0; i < draw->vertex_buffer_count; i++)
        gpu_ir_resource_retain(draw->vertex_buffers[i].resource);
    gpu_ir_pipeline_retain(pipeline);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

int gpu_ir_record_bind_descriptor_state(gpu_ir_stream *s, gpu_ir_descriptor_state *state, uint32_t *index) {
    if (index) *index = GPU_IR_NO_COMMAND;
    if (!s || !state) return GPU_IR_INVALID;
    if (s->count == UINT32_MAX) return GPU_IR_OUT_OF_MEMORY;
    if (s->count == s->capacity) {
        uint32_t cap = s->capacity ? s->capacity * 2 : 16;
        if (cap < s->capacity || (size_t)cap > SIZE_MAX / sizeof(gpu_ir_command)) return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_command *cmds = realloc(s->commands, (size_t)cap * sizeof *cmds);
        if (!cmds) return GPU_IR_OUT_OF_MEMORY;
        __atomic_add_fetch(&gpu_ir_stream_bytes, (uint64_t)(cap - s->capacity) * sizeof(gpu_ir_command), __ATOMIC_RELAXED);
        s->commands = cmds;
        s->capacity = cap;
    }
    gpu_ir_command *cmd = &((gpu_ir_command *)s->commands)[s->count];
    *cmd = (gpu_ir_command){ .op = GPU_IR_OP_BIND_DESCRIPTOR_STATE, .descriptor_state = state };
    gpu_ir_descriptor_state_retain(state);
    if (index) *index = s->count;
    s->count++;
    return GPU_IR_OK;
}

uint32_t gpu_ir_command_kind(const gpu_ir_stream *s, uint32_t index) {
    if (!s || index >= s->count) return GPU_IR_COMMAND_INVALID;
    switch (((const gpu_ir_command *)s->commands)[index].op) {
    case GPU_IR_OP_COPY_BUFFER: return GPU_IR_COMMAND_COPY_BUFFER;
    case GPU_IR_OP_UPDATE_BUFFER: return GPU_IR_COMMAND_UPDATE_BUFFER;
    case GPU_IR_OP_FILL_BUFFER: return GPU_IR_COMMAND_FILL_BUFFER;
    case GPU_IR_OP_COPY_RESOURCE: return GPU_IR_COMMAND_COPY_RESOURCE;
    case GPU_IR_OP_COPY_TEXTURE: return GPU_IR_COMMAND_COPY_TEXTURE;
    case GPU_IR_OP_RESOLVE_TEXTURE: return GPU_IR_COMMAND_RESOLVE_TEXTURE;
    case GPU_IR_OP_BARRIER: return GPU_IR_COMMAND_BARRIER;
    case GPU_IR_OP_DISPATCH: return GPU_IR_COMMAND_DISPATCH;
    case GPU_IR_OP_DRAW: return GPU_IR_COMMAND_DRAW;
    case GPU_IR_OP_DISPATCH_INDIRECT: return GPU_IR_COMMAND_DISPATCH_INDIRECT;
    case GPU_IR_OP_DISPATCH_INDIRECT_RANGE: return GPU_IR_COMMAND_DISPATCH_INDIRECT_RANGE;
    case GPU_IR_OP_DRAW_INDIRECT: return GPU_IR_COMMAND_DRAW_INDIRECT;
    case GPU_IR_OP_BIND_DESCRIPTOR_STATE: return GPU_IR_COMMAND_BIND_DESCRIPTOR_STATE;
    case GPU_IR_OP_CLEAR_COLOR: return GPU_IR_COMMAND_CLEAR_COLOR;
    case GPU_IR_OP_CLEAR_DEPTH_STENCIL: return GPU_IR_COMMAND_CLEAR_DEPTH_STENCIL;
    case GPU_IR_OP_BEGIN_RENDER_PASS: return GPU_IR_COMMAND_BEGIN_RENDER_PASS;
    case GPU_IR_OP_END_RENDER_PASS: return GPU_IR_COMMAND_END_RENDER_PASS;
    default: return GPU_IR_COMMAND_INVALID;
    }
}

uint32_t gpu_ir_command_access_count(const gpu_ir_stream *s, uint32_t index) {
    if (!s || index >= s->count) return 0;
    return ((const gpu_ir_command *)s->commands)[index].access_count;
}

int gpu_ir_command_access_at(const gpu_ir_stream *s, uint32_t index, uint32_t access_index,
                             gpu_ir_resource_access *out) {
    if (!s || !out || index >= s->count) return GPU_IR_INVALID;
    const gpu_ir_command *cmd = &((const gpu_ir_command *)s->commands)[index];
    if (access_index >= cmd->access_count) return GPU_IR_RANGE;
    *out = cmd->accesses[access_index];
    return GPU_IR_OK;
}

int gpu_ir_execute(const gpu_ir_stream *s, uint32_t index, const gpu_ir_backend *backend) {
    if (!s || !backend || index >= s->count) return GPU_IR_INVALID;
    const gpu_ir_command *cmd = &((const gpu_ir_command *)s->commands)[index];
    if (cmd->op == GPU_IR_OP_COPY_BUFFER) {
        if (!backend->copy_buffer) return GPU_IR_UNSUPPORTED;
        return backend->copy_buffer(backend->context, cmd->src, cmd->copy.buffer.src_offset,
                                    cmd->dst, cmd->copy.buffer.dst_offset, cmd->copy.buffer.size);
    }
    if (cmd->op == GPU_IR_OP_UPDATE_BUFFER) {
        if (!backend->update_buffer) return GPU_IR_UNSUPPORTED;
        return backend->update_buffer(backend->context, cmd->dst, cmd->copy.update_buffer.dst_offset,
                                      cmd->copy.update_buffer.data, cmd->copy.update_buffer.size);
    }
    if (cmd->op == GPU_IR_OP_FILL_BUFFER) {
        if (backend->fill_buffer_pattern)
            return backend->fill_buffer_pattern(backend->context, cmd->dst, cmd->copy.fill_buffer.dst_offset,
                cmd->copy.fill_buffer.size, cmd->copy.fill_buffer.pattern, cmd->copy.fill_buffer.pattern_size);
        if (cmd->copy.fill_buffer.pattern_size != sizeof cmd->copy.fill_buffer.value || !backend->fill_buffer)
            return GPU_IR_UNSUPPORTED;
        return backend->fill_buffer(backend->context, cmd->dst, cmd->copy.fill_buffer.dst_offset,
                                    cmd->copy.fill_buffer.size, cmd->copy.fill_buffer.value);
    }
    if (cmd->op == GPU_IR_OP_COPY_RESOURCE) {
        if (!backend->copy_resource) return GPU_IR_UNSUPPORTED;
        return backend->copy_resource(backend->context, cmd->src, cmd->dst);
    }
    if (cmd->op == GPU_IR_OP_COPY_TEXTURE) {
        if (!backend->copy_texture) return GPU_IR_UNSUPPORTED;
        return backend->copy_texture(backend->context, cmd->src, &cmd->copy.texture.src,
                                     cmd->dst, &cmd->copy.texture.dst, cmd->copy.texture.width,
                                     cmd->copy.texture.height, cmd->copy.texture.depth);
    }
    if (cmd->op == GPU_IR_OP_RESOLVE_TEXTURE) {
        if (!backend->resolve_texture) return GPU_IR_UNSUPPORTED;
        return backend->resolve_texture(backend->context, cmd->src, &cmd->copy.resolve.src,
                                        cmd->dst, &cmd->copy.resolve.dst, &cmd->copy.resolve.region,
                                        cmd->copy.resolve.mode);
    }
    if (cmd->op == GPU_IR_OP_BARRIER) {
        if (!backend->barrier) return GPU_IR_UNSUPPORTED;
        return backend->barrier(backend->context, &cmd->copy.barrier);
    }
    if (cmd->op == GPU_IR_OP_DISPATCH) {
        if (!backend->dispatch) return GPU_IR_UNSUPPORTED;
        return backend->dispatch(backend->context, cmd->pipeline, cmd->copy.dispatch.x,
                                 cmd->copy.dispatch.y, cmd->copy.dispatch.z,
                                 cmd->accesses, cmd->access_count);
    }
    if (cmd->op == GPU_IR_OP_DISPATCH_INDIRECT) {
        if (!backend->dispatch_indirect) return GPU_IR_UNSUPPORTED;
        return backend->dispatch_indirect(backend->context, cmd->pipeline, cmd->src, cmd->copy.buffer.src_offset,
                                          cmd->accesses, cmd->access_count);
    }
    if (cmd->op == GPU_IR_OP_DISPATCH_INDIRECT_RANGE) {
        if (!backend->dispatch_indirect_range) return GPU_IR_UNSUPPORTED;
        return backend->dispatch_indirect_range(backend->context, cmd->pipeline, cmd->src, cmd->dst,
                                                &cmd->copy.dispatch_indirect_range,
                                                cmd->accesses, cmd->access_count);
    }
    if (cmd->op == GPU_IR_OP_DRAW) {
        if (!backend->draw) return GPU_IR_UNSUPPORTED;
        return backend->draw(backend->context, cmd->pipeline, &cmd->copy.draw,
                             cmd->accesses, cmd->access_count);
    }
    if (cmd->op == GPU_IR_OP_DRAW_INDIRECT) {
        if (!backend->draw_indirect) return GPU_IR_UNSUPPORTED;
        return backend->draw_indirect(backend->context, cmd->pipeline, cmd->src, cmd->dst,
                                      &cmd->copy.draw_indirect,
                                      cmd->accesses, cmd->access_count);
    }
    if (cmd->op == GPU_IR_OP_BIND_DESCRIPTOR_STATE) {
        if (!backend->bind_descriptor_state) return GPU_IR_UNSUPPORTED;
        return backend->bind_descriptor_state(backend->context, cmd->descriptor_state);
    }
    if (cmd->op == GPU_IR_OP_CLEAR_COLOR) {
        if (!backend->clear_color) return GPU_IR_UNSUPPORTED;
        return backend->clear_color(backend->context, cmd->dst, &cmd->copy.clear_color);
    }
    if (cmd->op == GPU_IR_OP_CLEAR_DEPTH_STENCIL) {
        if (!backend->clear_depth_stencil) return GPU_IR_UNSUPPORTED;
        return backend->clear_depth_stencil(backend->context, cmd->dst, &cmd->copy.clear_depth_stencil);
    }
    if (cmd->op == GPU_IR_OP_BEGIN_RENDER_PASS) {
        if (!backend->begin_render_pass) return GPU_IR_UNSUPPORTED;
        return backend->begin_render_pass(backend->context, cmd->src, cmd->dst, &cmd->copy.render_pass);
    }
    if (cmd->op == GPU_IR_OP_END_RENDER_PASS) {
        if (!backend->end_render_pass) return GPU_IR_UNSUPPORTED;
        return backend->end_render_pass(backend->context);
    }
    return GPU_IR_UNSUPPORTED;
}

int gpu_ir_execute_with_accesses(const gpu_ir_stream *s, uint32_t index, const gpu_ir_backend *backend,
                                 const gpu_ir_resource_access *accesses, uint32_t access_count) {
    if (!s || !backend || index >= s->count) return GPU_IR_INVALID;
    const gpu_ir_command *cmd = &((const gpu_ir_command *)s->commands)[index];
    if (cmd->op != GPU_IR_OP_DISPATCH && cmd->op != GPU_IR_OP_DISPATCH_INDIRECT &&
        cmd->op != GPU_IR_OP_DISPATCH_INDIRECT_RANGE &&
        cmd->op != GPU_IR_OP_DRAW && cmd->op != GPU_IR_OP_DRAW_INDIRECT)
        return GPU_IR_INVALID;

    gpu_ir_resource_access *copy = NULL;
    int rc = copy_resource_accesses(accesses, access_count, &copy);
    if (rc != GPU_IR_OK) return rc;
    if (!resource_accesses_match_pipeline(copy, access_count, gpu_ir_pipeline_description(cmd->pipeline))) {
        release_resource_accesses(copy, access_count);
        return GPU_IR_INVALID;
    }

    if (cmd->op == GPU_IR_OP_DISPATCH) {
        rc = backend->dispatch ? backend->dispatch(backend->context, cmd->pipeline,
            cmd->copy.dispatch.x, cmd->copy.dispatch.y, cmd->copy.dispatch.z, copy, access_count) : GPU_IR_UNSUPPORTED;
    } else if (cmd->op == GPU_IR_OP_DISPATCH_INDIRECT) {
        rc = backend->dispatch_indirect ? backend->dispatch_indirect(backend->context, cmd->pipeline, cmd->src,
            cmd->copy.buffer.src_offset, copy, access_count) : GPU_IR_UNSUPPORTED;
    } else if (cmd->op == GPU_IR_OP_DISPATCH_INDIRECT_RANGE) {
        rc = backend->dispatch_indirect_range ? backend->dispatch_indirect_range(backend->context, cmd->pipeline,
            cmd->src, cmd->dst, &cmd->copy.dispatch_indirect_range, copy, access_count) : GPU_IR_UNSUPPORTED;
    } else if (cmd->op == GPU_IR_OP_DRAW) {
        rc = backend->draw ? backend->draw(backend->context, cmd->pipeline, &cmd->copy.draw,
            copy, access_count) : GPU_IR_UNSUPPORTED;
    } else {
        rc = backend->draw_indirect ? backend->draw_indirect(backend->context, cmd->pipeline, cmd->src, cmd->dst,
            &cmd->copy.draw_indirect, copy, access_count) : GPU_IR_UNSUPPORTED;
    }
    release_resource_accesses(copy, access_count);
    return rc;
}
