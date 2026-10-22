// Lower the descriptor reflection subset into the common GPU resource-layout vocabulary.
#include "spirv_ir.h"
#include <stdlib.h>
#include <string.h>

enum {
    SPIRV_OPCODE_CAPABILITY = 17,
    SPIRV_CAPABILITY_GROUP_NON_UNIFORM = 61,
    SPIRV_CAPABILITY_GROUP_NON_UNIFORM_VOTE = 62,
    SPIRV_CAPABILITY_GROUP_NON_UNIFORM_ARITHMETIC = 63,
    SPIRV_SCOPE_SUBGROUP = 3
};

static uint32_t gpu_stage(uint32_t model) {
    switch (model) {
    case SPIRV_EXECUTION_MODEL_VERTEX: return GPU_IR_STAGE_VERTEX;
    case SPIRV_EXECUTION_MODEL_TESS_CONTROL: return GPU_IR_STAGE_TESS_CONTROL;
    case SPIRV_EXECUTION_MODEL_TESS_EVALUATION: return GPU_IR_STAGE_TESS_EVALUATION;
    case SPIRV_EXECUTION_MODEL_GEOMETRY: return GPU_IR_STAGE_GEOMETRY;
    case SPIRV_EXECUTION_MODEL_FRAGMENT: return GPU_IR_STAGE_FRAGMENT;
    case SPIRV_EXECUTION_MODEL_GL_COMPUTE: return GPU_IR_STAGE_COMPUTE;
    default: return 0;
    }
}

static int gpu_descriptor(uint32_t type, uint32_t *kind, uint32_t *access) {
    switch (type) {
    case SPIRV_DESCRIPTOR_UNIFORM_BUFFER:
        *kind = GPU_IR_DESCRIPTOR_UNIFORM_BUFFER;
        *access = GPU_IR_ACCESS_UNIFORM_READ;
        return 1;
    case SPIRV_DESCRIPTOR_STORAGE_BUFFER:
        *kind = GPU_IR_DESCRIPTOR_STORAGE_BUFFER;
        *access = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE;
        return 1;
    case SPIRV_DESCRIPTOR_SAMPLED_IMAGE:
        *kind = GPU_IR_DESCRIPTOR_SAMPLED_IMAGE;
        *access = GPU_IR_ACCESS_SHADER_READ;
        return 1;
    case SPIRV_DESCRIPTOR_STORAGE_IMAGE:
        *kind = GPU_IR_DESCRIPTOR_STORAGE_IMAGE;
        *access = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE;
        return 1;
    case SPIRV_DESCRIPTOR_SAMPLER:
        *kind = GPU_IR_DESCRIPTOR_SAMPLER;
        *access = 0;
        return 1;
    case SPIRV_DESCRIPTOR_COMBINED_IMAGE_SAMPLER:
        *kind = GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER;
        *access = GPU_IR_ACCESS_SHADER_READ;
        return 1;
    case SPIRV_DESCRIPTOR_INPUT_ATTACHMENT:
        *kind = GPU_IR_DESCRIPTOR_INPUT_ATTACHMENT;
        *access = GPU_IR_ACCESS_SHADER_READ;
        return 1;
    case SPIRV_DESCRIPTOR_UNIFORM_TEXEL_BUFFER:
        *kind = GPU_IR_DESCRIPTOR_UNIFORM_TEXEL_BUFFER;
        *access = GPU_IR_ACCESS_SHADER_READ;
        return 1;
    case SPIRV_DESCRIPTOR_STORAGE_TEXEL_BUFFER:
        *kind = GPU_IR_DESCRIPTOR_STORAGE_TEXEL_BUFFER;
        *access = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE;
        return 1;
    default:
        return 0;
    }
}

static int gpu_storage_access(const spirv_module *module, const spirv_id_record *variable,
                             const spirv_descriptor_info *descriptor, uint32_t kind, uint32_t *access) {
    if (kind != GPU_IR_DESCRIPTOR_STORAGE_BUFFER && kind != GPU_IR_DESCRIPTOR_STORAGE_IMAGE &&
        kind != GPU_IR_DESCRIPTOR_STORAGE_TEXEL_BUFFER) return 1;
    const spirv_id_record *base = spirv_find_id(module, descriptor->base_type_id);
    if (!base || base->kind != SPIRV_ID_TYPE) return 0;
    uint32_t flags = variable->decoration_flags | base->decoration_flags;
    if ((flags & SPIRV_DECORATION_NON_WRITABLE) && (flags & SPIRV_DECORATION_NON_READABLE)) return 0;
    if (flags & SPIRV_DECORATION_NON_WRITABLE) {
        *access = GPU_IR_ACCESS_SHADER_READ;
        return 1;
    }
    if (flags & SPIRV_DECORATION_NON_READABLE) {
        *access = GPU_IR_ACCESS_SHADER_WRITE;
        return 1;
    }
    if (kind == GPU_IR_DESCRIPTOR_STORAGE_BUFFER && base->type_kind == SPIRV_TYPE_STRUCT && base->member_count) {
        int all_non_writable = 1, all_non_readable = 1;
        for (uint32_t i = 0; i < base->member_count; i++) {
            uint32_t member_flags = base->members[i].decoration_flags;
            if ((member_flags & SPIRV_DECORATION_NON_WRITABLE) &&
                (member_flags & SPIRV_DECORATION_NON_READABLE)) return 0;
            all_non_writable &= (member_flags & SPIRV_DECORATION_NON_WRITABLE) != 0;
            all_non_readable &= (member_flags & SPIRV_DECORATION_NON_READABLE) != 0;
        }
        if (all_non_writable) *access = GPU_IR_ACCESS_SHADER_READ;
        else if (all_non_readable) *access = GPU_IR_ACCESS_SHADER_WRITE;
    }
    return 1;
}

static int scalar_size(const spirv_module *module, uint32_t type_id, uint64_t *size) {
    const spirv_id_record *type = spirv_find_id(module, type_id);
    if (!type || type->kind != SPIRV_ID_TYPE ||
        (type->type_kind != SPIRV_TYPE_INT && type->type_kind != SPIRV_TYPE_FLOAT) ||
        !type->width || type->width > 64 || (type->width & 7u)) return GPU_IR_UNSUPPORTED;
    *size = type->width / 8u;
    return GPU_IR_OK;
}

// Compute the occupied byte extent from explicit SPIR-V layout decorations. A containing struct
// supplies matrix layout decorations to a matrix or array-of-matrices member.
static int type_extent(const spirv_module *module, uint32_t type_id, uint32_t matrix_stride,
                       uint32_t matrix_flags, uint32_t depth, uint64_t *size) {
    if (depth > module->id_bound) return GPU_IR_UNSUPPORTED;
    const spirv_id_record *type = spirv_find_id(module, type_id);
    if (!type || type->kind != SPIRV_ID_TYPE) return GPU_IR_INVALID;
    uint64_t element_size = 0;
    switch (type->type_kind) {
    case SPIRV_TYPE_INT:
    case SPIRV_TYPE_FLOAT:
        return scalar_size(module, type_id, size);
    case SPIRV_TYPE_VECTOR: {
        int rc = scalar_size(module, type->element_type, &element_size);
        if (rc != GPU_IR_OK) return rc;
        if (element_size > UINT64_MAX / type->element_count) return GPU_IR_UNSUPPORTED;
        *size = element_size * type->element_count;
        return GPU_IR_OK;
    }
    case SPIRV_TYPE_MATRIX: {
        const spirv_id_record *column = spirv_find_id(module, type->element_type);
        if (!column || column->kind != SPIRV_ID_TYPE || column->type_kind != SPIRV_TYPE_VECTOR)
            return GPU_IR_INVALID;
        uint32_t major = matrix_flags & (SPIRV_DECORATION_ROW_MAJOR | SPIRV_DECORATION_COL_MAJOR);
        if (!matrix_stride || (major != SPIRV_DECORATION_ROW_MAJOR && major != SPIRV_DECORATION_COL_MAJOR))
            return GPU_IR_UNSUPPORTED;
        const spirv_id_record *scalar = spirv_find_id(module, column->element_type);
        if (!scalar) return GPU_IR_INVALID;
        int rc = scalar_size(module, scalar->id, &element_size);
        if (rc != GPU_IR_OK) return rc;
        uint32_t major_count = major == SPIRV_DECORATION_ROW_MAJOR ? column->element_count : type->element_count;
        uint32_t minor_count = major == SPIRV_DECORATION_ROW_MAJOR ? type->element_count : column->element_count;
        if (!major_count || !minor_count || element_size > UINT64_MAX / minor_count) return GPU_IR_UNSUPPORTED;
        uint64_t minor_size = element_size * minor_count;
        if (matrix_stride < minor_size || (uint64_t)(major_count - 1) > (UINT64_MAX - minor_size) / matrix_stride)
            return GPU_IR_UNSUPPORTED;
        *size = (uint64_t)(major_count - 1) * matrix_stride + minor_size;
        return GPU_IR_OK;
    }
    case SPIRV_TYPE_ARRAY: {
        const spirv_id_record *length = spirv_find_id(module, type->length_id);
        const spirv_id_record *length_type = length ? spirv_find_id(module, length->type_id) : NULL;
        if (!length || length->kind != SPIRV_ID_CONSTANT || length->constant_kind != SPIRV_CONSTANT_SCALAR ||
            length->is_specialization || !length_type || length_type->kind != SPIRV_ID_TYPE ||
            length_type->type_kind != SPIRV_TYPE_INT || !length_type->width || length_type->width > 64 ||
            !length->value_bits || length->value_bits > UINT32_MAX) return GPU_IR_UNSUPPORTED;
        if (length_type->signedness && (length->value_bits & (UINT64_C(1) << (length_type->width - 1))))
            return GPU_IR_INVALID;
        if (!type->has_array_stride) return GPU_IR_UNSUPPORTED;
        int rc = type_extent(module, type->element_type, matrix_stride, matrix_flags, depth + 1, &element_size);
        if (rc != GPU_IR_OK) return rc;
        if (type->array_stride < element_size ||
            (uint64_t)(length->value_bits - 1) > (UINT64_MAX - element_size) / type->array_stride)
            return GPU_IR_UNSUPPORTED;
        *size = (uint64_t)(length->value_bits - 1) * type->array_stride + element_size;
        return GPU_IR_OK;
    }
    case SPIRV_TYPE_STRUCT: {
        if (!type->member_count) return GPU_IR_UNSUPPORTED;
        uint64_t end = 0;
        for (uint32_t i = 0; i < type->member_count; i++) {
            const spirv_member_record *member = &type->members[i];
            if (!member->has_offset) return GPU_IR_UNSUPPORTED;
            int rc = type_extent(module, member->type_id,
                member->has_matrix_stride ? member->matrix_stride : 0,
                member->decoration_flags, depth + 1, &element_size);
            if (rc != GPU_IR_OK) return rc;
            if (element_size > UINT64_MAX - member->offset) return GPU_IR_UNSUPPORTED;
            uint64_t member_end = (uint64_t)member->offset + element_size;
            if (member_end > end) end = member_end;
        }
        *size = end;
        return GPU_IR_OK;
    }
    default:
        return GPU_IR_UNSUPPORTED;
    }
}

static int push_constant_layout(const spirv_module *module, const spirv_id_record *variable,
                                uint32_t stage, gpu_ir_push_constant_range *range) {
    const spirv_id_record *pointer = spirv_find_id(module, variable->type_id);
    const spirv_id_record *block = spirv_find_id(module, variable->pointee_type_id);
    if (!pointer || pointer->kind != SPIRV_ID_TYPE || pointer->type_kind != SPIRV_TYPE_POINTER ||
        pointer->storage_class != 9 || !block || block->kind != SPIRV_ID_TYPE ||
        block->type_kind != SPIRV_TYPE_STRUCT || !(block->decoration_flags & SPIRV_DECORATION_BLOCK))
        return GPU_IR_UNSUPPORTED;
    uint64_t first = UINT64_MAX, end = 0;
    for (uint32_t i = 0; i < block->member_count; i++) {
        const spirv_member_record *member = &block->members[i];
        uint64_t member_size;
        if (!member->has_offset) return GPU_IR_UNSUPPORTED;
        int rc = type_extent(module, member->type_id,
            member->has_matrix_stride ? member->matrix_stride : 0,
            member->decoration_flags, 0, &member_size);
        if (rc != GPU_IR_OK) return rc;
        if (member_size > UINT64_MAX - member->offset) return GPU_IR_UNSUPPORTED;
        uint64_t member_end = (uint64_t)member->offset + member_size;
        if (member->offset < first) first = member->offset;
        if (member_end > end) end = member_end;
    }
    if (first == UINT64_MAX || end <= first || end > UINT32_MAX - 3u) return GPU_IR_UNSUPPORTED;
    uint64_t aligned_first = first & ~UINT64_C(3);
    uint64_t aligned_end = (end + 3u) & ~UINT64_C(3);
    if (aligned_end <= aligned_first || aligned_end - aligned_first > UINT32_MAX) return GPU_IR_UNSUPPORTED;
    *range = (gpu_ir_push_constant_range){ (uint32_t)aligned_first,
        (uint32_t)(aligned_end - aligned_first), stage };
    return GPU_IR_OK;
}

int spirv_build_gpu_ir_layout(const spirv_module *module, const spirv_entry_point *entry_point,
                              gpu_ir_pipeline_layout **layout) {
    if (layout) *layout = NULL;
    if (!module || !entry_point || !layout || !module->words || !module->id_records) return GPU_IR_INVALID;
    int belongs_to_module = 0;
    for (uint32_t i = 0; i < module->entry_point_count; i++)
        if (entry_point == &module->entry_points[i]) { belongs_to_module = 1; break; }
    if (!belongs_to_module) return GPU_IR_INVALID;
    uint32_t stage = gpu_stage(entry_point->execution_model);
    if (!stage) return GPU_IR_UNSUPPORTED;

    uint32_t descriptor_count = spirv_descriptor_count(module);
    gpu_ir_descriptor_binding *bindings = descriptor_count ?
        calloc(descriptor_count, sizeof *bindings) : NULL;
    if (descriptor_count && !bindings) return GPU_IR_OUT_OF_MEMORY;
    gpu_ir_push_constant_range *push_constants = module->variable_count ?
        calloc(module->variable_count, sizeof *push_constants) : NULL;
    if (module->variable_count && !push_constants) { free(bindings); return GPU_IR_OUT_OF_MEMORY; }
    uint32_t count = 0, push_count = 0;
    for (uint32_t i = 0; i < spirv_variable_count(module); i++) {
        const spirv_id_record *variable = spirv_variable_at(module, i);
        if (!variable) { free(bindings); free(push_constants); return GPU_IR_INVALID; }
        if (variable->storage_class == 9) {
            if (push_count) { free(bindings); free(push_constants); return GPU_IR_UNSUPPORTED; }
            int rc = push_constant_layout(module, variable, stage, &push_constants[push_count]);
            if (rc != GPU_IR_OK) { free(bindings); free(push_constants); return rc; }
            push_count++;
            continue;
        }
        if (!variable->has_descriptor_set || !variable->has_binding) continue;
        spirv_descriptor_info descriptor;
        int rc = spirv_reflect_descriptor(module, variable, &descriptor);
        if (rc != SPIRV_OK) {
            free(bindings); free(push_constants);
            return rc == SPIRV_OUT_OF_MEMORY ? GPU_IR_OUT_OF_MEMORY :
                   rc == SPIRV_UNSUPPORTED ? GPU_IR_UNSUPPORTED : GPU_IR_INVALID;
        }
        uint32_t kind, access;
        if (!gpu_descriptor(descriptor.descriptor_type, &kind, &access)) {
            free(bindings); free(push_constants);
            return GPU_IR_UNSUPPORTED;
        }
        if (!gpu_storage_access(module, variable, &descriptor, kind, &access)) {
            free(bindings); free(push_constants);
            return GPU_IR_UNSUPPORTED;
        }
        uint32_t elements = descriptor.is_runtime_array ? GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED : descriptor.array_count;
        if (!elements || (descriptor.is_runtime_array && descriptor.array_count)) {
            free(bindings); free(push_constants);
            return GPU_IR_INVALID;
        }
        bindings[count++] = (gpu_ir_descriptor_binding){
            .set = descriptor.descriptor_set,
            .binding = descriptor.binding,
            .kind = kind,
            .count = elements,
            .stage_mask = stage,
            .access_mask = access,
            .binding_namespace = 0
        };
    }
    gpu_ir_pipeline_layout_desc description = { .bindings = bindings, .binding_count = count,
        .push_constants = push_constants, .push_constant_count = push_count };
    *layout = gpu_ir_pipeline_layout_create(&description);
    free(bindings);
    free(push_constants);
    return *layout ? GPU_IR_OK : GPU_IR_INVALID;
}

static uint32_t gpu_shader_type_kind(uint32_t kind) {
    switch (kind) {
    case SPIRV_TYPE_VOID: return GPU_IR_SHADER_TYPE_VOID;
    case SPIRV_TYPE_BOOL: return GPU_IR_SHADER_TYPE_BOOL;
    case SPIRV_TYPE_INT: return GPU_IR_SHADER_TYPE_INT;
    case SPIRV_TYPE_FLOAT: return GPU_IR_SHADER_TYPE_FLOAT;
    case SPIRV_TYPE_VECTOR: return GPU_IR_SHADER_TYPE_VECTOR;
    case SPIRV_TYPE_MATRIX: return GPU_IR_SHADER_TYPE_MATRIX;
    case SPIRV_TYPE_ARRAY: return GPU_IR_SHADER_TYPE_ARRAY;
    case SPIRV_TYPE_RUNTIME_ARRAY: return GPU_IR_SHADER_TYPE_RUNTIME_ARRAY;
    case SPIRV_TYPE_STRUCT: return GPU_IR_SHADER_TYPE_STRUCT;
    case SPIRV_TYPE_POINTER: return GPU_IR_SHADER_TYPE_POINTER;
    case SPIRV_TYPE_IMAGE: return GPU_IR_SHADER_TYPE_IMAGE;
    case SPIRV_TYPE_SAMPLER: return GPU_IR_SHADER_TYPE_SAMPLER;
    case SPIRV_TYPE_SAMPLED_IMAGE: return GPU_IR_SHADER_TYPE_SAMPLED_IMAGE;
    case SPIRV_TYPE_FUNCTION: return GPU_IR_SHADER_TYPE_FUNCTION;
    default: return 0;
    }
}

static uint32_t gpu_shader_decoration_flags(uint32_t flags) {
    uint32_t mapped = 0;
    if (flags & SPIRV_DECORATION_BLOCK) mapped |= GPU_IR_SHADER_TYPE_FLAG_BLOCK;
    if (flags & SPIRV_DECORATION_BUFFER_BLOCK) mapped |= GPU_IR_SHADER_TYPE_FLAG_BUFFER_BLOCK;
    if (flags & SPIRV_DECORATION_NON_WRITABLE) mapped |= GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE;
    if (flags & SPIRV_DECORATION_NON_READABLE) mapped |= GPU_IR_SHADER_TYPE_FLAG_NON_READABLE;
    if (flags & SPIRV_DECORATION_ROW_MAJOR) mapped |= GPU_IR_SHADER_TYPE_FLAG_ROW_MAJOR;
    if (flags & SPIRV_DECORATION_COL_MAJOR) mapped |= GPU_IR_SHADER_TYPE_FLAG_COLUMN_MAJOR;
    if (flags & SPIRV_DECORATION_NO_PERSPECTIVE) mapped |= GPU_IR_SHADER_DECORATION_NO_PERSPECTIVE;
    if (flags & SPIRV_DECORATION_FLAT) mapped |= GPU_IR_SHADER_DECORATION_FLAT;
    if (flags & SPIRV_DECORATION_CENTROID) mapped |= GPU_IR_SHADER_DECORATION_CENTROID;
    if (flags & SPIRV_DECORATION_SAMPLE) mapped |= GPU_IR_SHADER_DECORATION_SAMPLE;
    if (flags & SPIRV_DECORATION_INVARIANT) mapped |= GPU_IR_SHADER_DECORATION_INVARIANT;
    return mapped;
}

static uint32_t gpu_shader_storage_class(uint32_t source_storage_class) {
    switch (source_storage_class) {
    case 0: return GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT; // UniformConstant
    case 1: return GPU_IR_SHADER_STORAGE_INPUT;
    case 2: return GPU_IR_SHADER_STORAGE_UNIFORM;
    case 3: return GPU_IR_SHADER_STORAGE_OUTPUT;
    case 4: return GPU_IR_SHADER_STORAGE_WORKGROUP;
    case 5: return GPU_IR_SHADER_STORAGE_CROSS_WORKGROUP;
    case 6: return GPU_IR_SHADER_STORAGE_PRIVATE;
    case 7: return GPU_IR_SHADER_STORAGE_FUNCTION;
    case 8: return GPU_IR_SHADER_STORAGE_GENERIC;
    case 9: return GPU_IR_SHADER_STORAGE_PUSH_CONSTANT;
    case 10: return GPU_IR_SHADER_STORAGE_ATOMIC_COUNTER;
    case 11: return GPU_IR_SHADER_STORAGE_IMAGE;
    case 12: return GPU_IR_SHADER_STORAGE_BUFFER;
    case 5349: return GPU_IR_SHADER_STORAGE_PHYSICAL_BUFFER;
    default: return GPU_IR_SHADER_STORAGE_UNMAPPED;
    }
}

static uint32_t gpu_shader_builtin(uint32_t source_builtin) {
    switch (source_builtin) {
    case 0: return GPU_IR_SHADER_BUILTIN_POSITION;
    case 15: return GPU_IR_SHADER_BUILTIN_FRAG_COORD;
    case 17: return GPU_IR_SHADER_BUILTIN_FRONT_FACING;
    case 22: return GPU_IR_SHADER_BUILTIN_FRAG_DEPTH;
    case 24: return GPU_IR_SHADER_BUILTIN_NUM_WORKGROUPS;
    case 25: return GPU_IR_SHADER_BUILTIN_WORKGROUP_SIZE;
    case 26: return GPU_IR_SHADER_BUILTIN_WORKGROUP_ID;
    case 27: return GPU_IR_SHADER_BUILTIN_LOCAL_INVOCATION_ID;
    case 28: return GPU_IR_SHADER_BUILTIN_GLOBAL_INVOCATION_ID;
    case 29: return GPU_IR_SHADER_BUILTIN_LOCAL_INVOCATION_INDEX;
    case 36: return GPU_IR_SHADER_BUILTIN_SUBGROUP_SIZE;
    case 41: return GPU_IR_SHADER_BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID;
    case 42: return GPU_IR_SHADER_BUILTIN_VERTEX_INDEX;
    case 43: return GPU_IR_SHADER_BUILTIN_INSTANCE_INDEX;
    default: return GPU_IR_SHADER_BUILTIN_UNMAPPED;
    }
}

static void gpu_shader_variable_metadata(const spirv_module *module,
                                         gpu_ir_shader_instruction *instruction) {
    const spirv_id_record *variable = spirv_find_id(module, instruction->result_id);
    if (!variable || variable->kind != SPIRV_ID_VARIABLE) return;
    instruction->storage_class = gpu_shader_storage_class(variable->storage_class);
    instruction->source_storage_class = variable->storage_class;
    instruction->decoration_flags = gpu_shader_decoration_flags(variable->decoration_flags);
    instruction->resource_set = variable->descriptor_set;
    instruction->binding = variable->binding;
    instruction->has_descriptor_set = variable->has_descriptor_set;
    instruction->has_binding = variable->has_binding;
    instruction->location = variable->location;
    instruction->component = variable->component;
    instruction->index = variable->index;
    instruction->builtin = variable->has_builtin ? gpu_shader_builtin(variable->builtin) :
        GPU_IR_SHADER_BUILTIN_UNSPECIFIED;
    instruction->source_builtin = variable->has_builtin ? variable->builtin : 0;
    instruction->has_location = variable->has_location;
    instruction->has_component = variable->has_component;
    instruction->has_index = variable->has_index;
    instruction->has_builtin = variable->has_builtin;
}

static uint32_t gpu_shader_flags_for_type(const spirv_module *module, uint32_t type_id) {
    const spirv_id_record *type = spirv_find_id(module, type_id);
    uint32_t depth = 0;
    while (type && type->kind == SPIRV_ID_TYPE &&
           (type->type_kind == SPIRV_TYPE_VECTOR || type->type_kind == SPIRV_TYPE_MATRIX ||
            type->type_kind == SPIRV_TYPE_ARRAY || type->type_kind == SPIRV_TYPE_RUNTIME_ARRAY) &&
           depth++ < module->id_bound)
        type = spirv_find_id(module, type->element_type);
    if (!type || type->kind != SPIRV_ID_TYPE) return 0;
    uint32_t flags = 0;
    if (type->type_kind == SPIRV_TYPE_FLOAT) flags |= GPU_IR_SHADER_FLAG_FLOAT;
    if (type->type_kind == SPIRV_TYPE_INT && type->signedness) flags |= GPU_IR_SHADER_FLAG_SIGNED;
    return flags;
}

static int gpu_shader_is_32bit_integer_scalar_constant(const spirv_module *module, uint32_t id) {
    const spirv_id_record *value = spirv_find_id(module, id);
    const spirv_id_record *type = value ? spirv_find_id(module, value->type_id) : NULL;
    return value && value->kind == SPIRV_ID_CONSTANT && value->constant_kind == SPIRV_CONSTANT_SCALAR &&
           type && type->kind == SPIRV_ID_TYPE && type->type_kind == SPIRV_TYPE_INT && type->width == 32;
}

static int gpu_shader_u32_constant(const spirv_module *module, uint32_t id, uint32_t *value) {
    const spirv_id_record *constant = spirv_find_id(module, id);
    const spirv_id_record *type = constant ? spirv_find_id(module, constant->type_id) : NULL;
    if (!value || !constant || constant->kind != SPIRV_ID_CONSTANT || constant->is_specialization ||
        constant->constant_kind != SPIRV_CONSTANT_SCALAR || !type || type->kind != SPIRV_ID_TYPE ||
        type->type_kind != SPIRV_TYPE_INT || type->width != 32 || type->signedness)
        return 0;
    *value = (uint32_t)constant->value_bits;
    return 1;
}

static int gpu_shader_has_capability(const spirv_module *module, uint32_t capability) {
    spirv_iterator iterator;
    spirv_instruction instruction;
    spirv_iter_begin(module, &iterator);
    int next;
    while ((next = spirv_iter_next(&iterator, &instruction)) > 0) {
        if (instruction.opcode == SPIRV_OPCODE_CAPABILITY && instruction.word_count == 2 &&
            instruction.words[1] == capability) return 1;
    }
    return 0;
}

static int gpu_shader_is_zero_32bit_integer_scalar_constant(const spirv_module *module, uint32_t id) {
    const spirv_id_record *value = spirv_find_id(module, id);
    return gpu_shader_is_32bit_integer_scalar_constant(module, id) && value && !value->value_bits;
}

static int gpu_shader_is_zero_32bit_float_scalar_constant(const spirv_module *module, uint32_t id) {
    const spirv_id_record *value = spirv_find_id(module, id);
    const spirv_id_record *type = value ? spirv_find_id(module, value->type_id) : NULL;
    return value && value->kind == SPIRV_ID_CONSTANT && value->constant_kind == SPIRV_CONSTANT_SCALAR &&
           type && type->kind == SPIRV_ID_TYPE && type->type_kind == SPIRV_TYPE_FLOAT && type->width == 32 &&
           value->value_bits == 0;
}

// Keep the core SPIR-V memory-access guarantees in common IR. Pointer visibility/availability and
// extension-specific aliasing operands need API/backend semantics that this frontend cannot provide yet.
static int gpu_shader_memory_access(const spirv_instruction *source, uint32_t mask_word, int is_store,
                                    uint32_t *flags, uint32_t *alignment) {
    uint32_t n = source->word_count;
    *flags = 0;
    *alignment = 0;
    if (n == mask_word) return GPU_IR_OK;
    if (n < mask_word + 1u) return GPU_IR_INVALID;
    uint32_t mask = source->words[mask_word];
    const uint32_t core_mask = 7u;
    const uint32_t known_extension_mask = 0x38u | 0x30000u;
    if (mask & ~(core_mask | known_extension_mask)) return GPU_IR_INVALID;
    if ((mask & 0x8u) && (!is_store || !(mask & 0x20u))) return GPU_IR_INVALID;
    if ((mask & 0x10u) && (is_store || !(mask & 0x20u))) return GPU_IR_INVALID;
    if (mask & known_extension_mask) return GPU_IR_UNSUPPORTED;
    if (mask & 1u) *flags |= GPU_IR_SHADER_MEMORY_VOLATILE;
    if (mask & 2u) *flags |= GPU_IR_SHADER_MEMORY_ALIGNED;
    if (mask & 4u) *flags |= GPU_IR_SHADER_MEMORY_NONTEMPORAL;
    uint32_t expected_words = mask_word + 1u + ((mask & 2u) != 0);
    if (n != expected_words) return GPU_IR_INVALID;
    if (mask & 2u) {
        *alignment = source->words[mask_word + 1u];
        if (!*alignment || (*alignment & (*alignment - 1u))) return GPU_IR_INVALID;
    }
    return GPU_IR_OK;
}

static uint32_t gpu_shader_arithmetic(uint32_t opcode) {
    switch (opcode) {
    case 128: case 129: return GPU_IR_SHADER_ARITH_ADD;
    case 130: case 131: return GPU_IR_SHADER_ARITH_SUBTRACT;
    case 132: case 133: return GPU_IR_SHADER_ARITH_MULTIPLY;
    case 134: case 135: case 136: return GPU_IR_SHADER_ARITH_DIVIDE;
    case 137: case 138: case 140: return GPU_IR_SHADER_ARITH_REMAINDER;
    case 139: case 141: return GPU_IR_SHADER_ARITH_MODULO;
    case 142: return GPU_IR_SHADER_ARITH_VECTOR_TIMES_SCALAR;
    case 148: return GPU_IR_SHADER_ARITH_DOT;
    case 126: case 127: return GPU_IR_SHADER_ARITH_NEGATE;
    case 194: return GPU_IR_SHADER_ARITH_SHIFT_RIGHT_LOGICAL;
    case 195: return GPU_IR_SHADER_ARITH_SHIFT_RIGHT_ARITHMETIC;
    case 196: return GPU_IR_SHADER_ARITH_SHIFT_LEFT;
    case 197: return GPU_IR_SHADER_ARITH_BIT_OR;
    case 198: return GPU_IR_SHADER_ARITH_BIT_XOR;
    case 199: return GPU_IR_SHADER_ARITH_BIT_AND;
    case 200: return GPU_IR_SHADER_ARITH_BIT_NOT;
    case 201: return GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT;
    case 202: return GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT;
    case 203: return GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT;
    case 204: return GPU_IR_SHADER_ARITH_BIT_REVERSE;
    case 205: return GPU_IR_SHADER_ARITH_BIT_COUNT;
    case 166: return GPU_IR_SHADER_ARITH_LOGICAL_OR;
    case 167: return GPU_IR_SHADER_ARITH_LOGICAL_AND;
    case 168: return GPU_IR_SHADER_ARITH_LOGICAL_NOT;
    case 154: return GPU_IR_SHADER_ARITH_ANY;
    case 155: return GPU_IR_SHADER_ARITH_ALL;
    default: return 0;
    }
}

static uint32_t gpu_shader_glsl450_arithmetic(uint32_t opcode) {
    switch (opcode) {
    case 4: return GPU_IR_SHADER_ARITH_ABSOLUTE;       // FAbs
    case 8: return GPU_IR_SHADER_ARITH_FLOOR;          // Floor
    case 9: return GPU_IR_SHADER_ARITH_CEILING;         // Ceil
    case 26: return GPU_IR_SHADER_ARITH_POWER;          // Pow
    case 31: return GPU_IR_SHADER_ARITH_SQRT;           // Sqrt
    case 32: return GPU_IR_SHADER_ARITH_INVERSE_SQRT;   // InverseSqrt
    case 37: return GPU_IR_SHADER_ARITH_MINIMUM;        // FMin
    case 40: return GPU_IR_SHADER_ARITH_MAXIMUM;        // FMax
    case 43: return GPU_IR_SHADER_ARITH_CLAMP;          // FClamp
    case 46: return GPU_IR_SHADER_ARITH_INTERPOLATE;    // FMix
    case 48: return GPU_IR_SHADER_ARITH_STEP;           // Step
    case 49: return GPU_IR_SHADER_ARITH_SMOOTHSTEP;     // SmoothStep
    case 50: return GPU_IR_SHADER_ARITH_FMA;            // Fma
    case 1: return GPU_IR_SHADER_ARITH_ROUND;            // Round
    case 2: return GPU_IR_SHADER_ARITH_ROUND_EVEN;       // RoundEven
    case 3: return GPU_IR_SHADER_ARITH_TRUNC;            // Trunc
    case 10: return GPU_IR_SHADER_ARITH_FRACT;           // Fract
    case 13: return GPU_IR_SHADER_ARITH_SIN;             // Sin
    case 14: return GPU_IR_SHADER_ARITH_COS;             // Cos
    case 15: return GPU_IR_SHADER_ARITH_TAN;             // Tan
    case 16: return GPU_IR_SHADER_ARITH_ASIN;            // Asin
    case 17: return GPU_IR_SHADER_ARITH_ACOS;            // Acos
    case 18: return GPU_IR_SHADER_ARITH_ATAN;            // Atan
    case 19: return GPU_IR_SHADER_ARITH_SINH;            // Sinh
    case 20: return GPU_IR_SHADER_ARITH_COSH;            // Cosh
    case 21: return GPU_IR_SHADER_ARITH_TANH;            // Tanh
    case 22: return GPU_IR_SHADER_ARITH_ASINH;           // Asinh
    case 23: return GPU_IR_SHADER_ARITH_ACOSH;           // Acosh
    case 24: return GPU_IR_SHADER_ARITH_ATANH;           // Atanh
    case 27: return GPU_IR_SHADER_ARITH_EXP;             // Exp
    case 28: return GPU_IR_SHADER_ARITH_LOG;             // Log
    case 29: return GPU_IR_SHADER_ARITH_EXP2;            // Exp2
    case 30: return GPU_IR_SHADER_ARITH_LOG2;            // Log2
    case 73: return GPU_IR_SHADER_ARITH_BIT_FIND_LSB;    // FindILsb
    case 74: return GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED; // FindSMsb
    case 75: return GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED; // FindUMsb
    default: return 0;
    }
}

static int gpu_shader_is_glsl450_bit_find(uint32_t operation) {
    return operation == GPU_IR_SHADER_ARITH_BIT_FIND_LSB ||
        operation == GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED ||
        operation == GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED;
}

static const spirv_id_record *gpu_shader_type_record(const spirv_module *module, uint32_t type_id);
static int gpu_shader_numeric_shape(const spirv_module *module, uint32_t type_id,
                                   const spirv_id_record **scalar_type, uint32_t *component_count);

static int gpu_shader_validate_glsl450_arithmetic(const spirv_module *module,
                                                   const spirv_instruction *source,
                                                   uint32_t operation) {
    const uint32_t *w = source->words;
    if (gpu_shader_is_glsl450_bit_find(operation)) {
        if (source->word_count != 6) return GPU_IR_INVALID;
        const char *set_name = spirv_ext_inst_import_name(module, w[3]);
        if (!set_name) return GPU_IR_INVALID;
        if (strcmp(set_name, "GLSL.std.450")) return GPU_IR_UNSUPPORTED;

        const spirv_id_record *result = NULL, *input = NULL;
        uint32_t result_components = 0, input_components = 0;
        const spirv_id_record *value = spirv_find_id(module, w[5]);
        const spirv_id_record *value_type = value ? gpu_shader_type_record(module, value->type_id) : NULL;
        if (!value || !value_type ||
            !gpu_shader_numeric_shape(module, w[1], &result, &result_components) ||
            !gpu_shader_numeric_shape(module, value_type->id, &input, &input_components) ||
            input->type_kind != SPIRV_TYPE_INT || result->type_kind != SPIRV_TYPE_INT ||
            input_components != result_components || input->width != result->width)
            return GPU_IR_INVALID;
        return result->width == 32 ? GPU_IR_OK : GPU_IR_UNSUPPORTED;
    }
    uint32_t operand_count = operation == GPU_IR_SHADER_ARITH_CLAMP ||
        operation == GPU_IR_SHADER_ARITH_INTERPOLATE ||
        operation == GPU_IR_SHADER_ARITH_SMOOTHSTEP ||
        operation == GPU_IR_SHADER_ARITH_FMA ? 3u :
        operation == GPU_IR_SHADER_ARITH_ABSOLUTE || operation == GPU_IR_SHADER_ARITH_FLOOR ||
        operation == GPU_IR_SHADER_ARITH_CEILING || operation == GPU_IR_SHADER_ARITH_SQRT ||
        operation == GPU_IR_SHADER_ARITH_INVERSE_SQRT || operation == GPU_IR_SHADER_ARITH_TRUNC ||
        operation == GPU_IR_SHADER_ARITH_FRACT || operation == GPU_IR_SHADER_ARITH_ROUND ||
        operation == GPU_IR_SHADER_ARITH_ROUND_EVEN ||
        (operation >= GPU_IR_SHADER_ARITH_SIN && operation <= GPU_IR_SHADER_ARITH_ATANH) ||
        (operation >= GPU_IR_SHADER_ARITH_EXP && operation <= GPU_IR_SHADER_ARITH_LOG2) ? 1u : 2u;
    if (source->word_count != operand_count + 5u) return GPU_IR_INVALID;
    const char *set_name = spirv_ext_inst_import_name(module, w[3]);
    if (!set_name) return GPU_IR_INVALID;
    if (strcmp(set_name, "GLSL.std.450")) return GPU_IR_UNSUPPORTED;

    const spirv_id_record *result = NULL;
    uint32_t result_components = 0;
    if (!gpu_shader_numeric_shape(module, w[1], &result, &result_components)) return GPU_IR_INVALID;
    if (result->type_kind != SPIRV_TYPE_FLOAT || result->width != 32 ||
        result_components < 1 || result_components > 4) return GPU_IR_UNSUPPORTED;
    for (uint32_t i = 0; i < operand_count; i++) {
        const spirv_id_record *value = spirv_find_id(module, w[5 + i]);
        uint32_t components = 0;
        const spirv_id_record *value_type = value ?
            gpu_shader_type_record(module, value->type_id) : NULL;
        const spirv_id_record *value_scalar = NULL;
        if (!value || !value_type ||
            !gpu_shader_numeric_shape(module, value_type->id, &value_scalar, &components) ||
            value->type_id != w[1] || components != result_components ||
            !value_scalar || value_scalar->type_kind != SPIRV_TYPE_FLOAT || value_scalar->width != 32)
            return GPU_IR_INVALID;
    }
    return GPU_IR_OK;
}

static uint32_t gpu_shader_compare(uint32_t opcode) {
    switch (opcode) {
    case 164: case 170: return GPU_IR_SHADER_COMPARE_EQUAL;
    case 165: case 171: return GPU_IR_SHADER_COMPARE_NOT_EQUAL;
    case 172: case 173: return GPU_IR_SHADER_COMPARE_GREATER;
    case 174: case 175: return GPU_IR_SHADER_COMPARE_GREATER_EQUAL;
    case 176: case 177: return GPU_IR_SHADER_COMPARE_LESS;
    case 178: case 179: return GPU_IR_SHADER_COMPARE_LESS_EQUAL;
    case 156: return GPU_IR_SHADER_COMPARE_IS_NAN;
    case 157: return GPU_IR_SHADER_COMPARE_IS_INF;
    case 158: return GPU_IR_SHADER_COMPARE_IS_FINITE;
    case 159: return GPU_IR_SHADER_COMPARE_IS_NORMAL;
    case 160: return GPU_IR_SHADER_COMPARE_SIGN_BIT_SET;
    case 161: return GPU_IR_SHADER_COMPARE_LESS_GREATER;
    case 162: return GPU_IR_SHADER_COMPARE_ORDERED;
    case 163: return GPU_IR_SHADER_COMPARE_UNORDERED;
    case 180: return GPU_IR_SHADER_COMPARE_ORDERED_EQUAL;
    case 181: return GPU_IR_SHADER_COMPARE_UNORDERED_EQUAL;
    case 182: return GPU_IR_SHADER_COMPARE_ORDERED_NOT_EQUAL;
    case 183: return GPU_IR_SHADER_COMPARE_UNORDERED_NOT_EQUAL;
    case 184: return GPU_IR_SHADER_COMPARE_ORDERED_LESS;
    case 185: return GPU_IR_SHADER_COMPARE_UNORDERED_LESS;
    case 186: return GPU_IR_SHADER_COMPARE_ORDERED_GREATER;
    case 187: return GPU_IR_SHADER_COMPARE_UNORDERED_GREATER;
    case 188: return GPU_IR_SHADER_COMPARE_ORDERED_LESS_EQUAL;
    case 189: return GPU_IR_SHADER_COMPARE_UNORDERED_LESS_EQUAL;
    case 190: return GPU_IR_SHADER_COMPARE_ORDERED_GREATER_EQUAL;
    case 191: return GPU_IR_SHADER_COMPARE_UNORDERED_GREATER_EQUAL;
    default: return 0;
    }
}

static uint32_t gpu_shader_conversion(uint32_t opcode) {
    switch (opcode) {
    case 109: return GPU_IR_SHADER_CONVERT_FLOAT_TO_UNSIGNED;
    case 110: return GPU_IR_SHADER_CONVERT_FLOAT_TO_SIGNED;
    case 111: return GPU_IR_SHADER_CONVERT_SIGNED_TO_FLOAT;
    case 112: return GPU_IR_SHADER_CONVERT_UNSIGNED_TO_FLOAT;
    case 113: return GPU_IR_SHADER_CONVERT_UNSIGNED_WIDTH;
    case 114: return GPU_IR_SHADER_CONVERT_SIGNED_WIDTH;
    case 115: return GPU_IR_SHADER_CONVERT_FLOAT_WIDTH;
    case 124: return GPU_IR_SHADER_CONVERT_BITCAST;
    default: return 0;
    }
}

static int gpu_shader_emit(gpu_ir_shader_instruction *instructions, uint32_t instruction_capacity,
                           uint32_t *instruction_count, gpu_ir_shader_operand *operands,
                           uint32_t operand_capacity, uint32_t *operand_count,
                           uint32_t *literal_words, uint32_t literal_capacity, uint32_t *literal_count,
                           uint32_t source_opcode, uint32_t opcode, uint32_t operation,
                           uint32_t result_type, uint32_t result_id, uint32_t function_index,
                           uint32_t block_index, uint32_t flags,
                           const uint32_t *operand_ids, uint32_t id_count,
                           const uint32_t *literals, uint32_t word_count) {
    if (*instruction_count >= instruction_capacity || id_count > operand_capacity - *operand_count ||
        word_count > literal_capacity - *literal_count) return GPU_IR_OUT_OF_MEMORY;
    gpu_ir_shader_instruction item = {
        .source_api = GPU_IR_SHADER_SOURCE_SPIRV,
        .opcode = opcode,
        .operation = operation,
        .source_opcode = source_opcode,
        .result_id = result_id,
        .result_type_id = result_type,
        .function_index = function_index,
        .block_index = block_index,
        .flags = flags,
        .first_operand = *operand_count,
        .operand_count = id_count,
        .first_literal = *literal_count,
        .literal_word_count = word_count
    };
    for (uint32_t i = 0; i < id_count; i++)
        operands[(*operand_count)++] = (gpu_ir_shader_operand){ GPU_IR_SHADER_OPERAND_ID, operand_ids[i] };
    if (word_count) {
        memcpy(literal_words + *literal_count, literals, (size_t)word_count * sizeof *literal_words);
        *literal_count += word_count;
    }
    instructions[(*instruction_count)++] = item;
    return GPU_IR_OK;
}

static int gpu_shader_emit_constant(const spirv_module *module, const spirv_instruction *source,
                                   uint32_t *instruction_count, gpu_ir_shader_instruction *instructions,
                                   uint32_t instruction_capacity, gpu_ir_shader_operand *operands,
                                   uint32_t operand_capacity, uint32_t *operand_count,
                                   uint32_t *literal_words, uint32_t literal_capacity,
                                   uint32_t *literal_count) {
    const uint32_t *w = source->words;
    if (source->word_count < 3) return GPU_IR_INVALID;
    const spirv_id_record *constant = spirv_find_id(module, w[2]);
    if (!constant || constant->kind != SPIRV_ID_CONSTANT || constant->type_id != w[1]) return GPU_IR_INVALID;
    uint32_t bool_word = constant->value_bits ? 1u : 0u;
    uint32_t operation = 0, flags = constant->is_specialization ? GPU_IR_SHADER_FLAG_SPECIALIZATION : 0;
    const uint32_t *literal_data = NULL;
    const uint32_t *ids = NULL;
    uint32_t id_count = 0;
    uint32_t literal_words_count = 0;
    switch (constant->constant_kind) {
    case SPIRV_CONSTANT_BOOL:
        operation = GPU_IR_SHADER_CONSTANT_BOOL;
        literal_data = &bool_word;
        literal_words_count = 1;
        break;
    case SPIRV_CONSTANT_SCALAR:
        operation = GPU_IR_SHADER_CONSTANT_SCALAR;
        literal_data = constant->literal_words;
        literal_words_count = constant->literal_word_count;
        break;
    case SPIRV_CONSTANT_NULL:
        operation = GPU_IR_SHADER_CONSTANT_NULL;
        break;
    case SPIRV_CONSTANT_COMPOSITE:
        operation = GPU_IR_SHADER_CONSTANT_COMPOSITE;
        ids = constant->constituent_ids;
        id_count = constant->constituent_count;
        break;
    default:
        return GPU_IR_UNSUPPORTED;
    }
    return gpu_shader_emit(instructions, instruction_capacity, instruction_count, operands, operand_capacity,
        operand_count, literal_words, literal_capacity, literal_count, source->opcode,
        GPU_IR_SHADER_OP_CONSTANT, operation, w[1], w[2], UINT32_MAX, UINT32_MAX, flags,
        ids, id_count, literal_data, literal_words_count);
}

static uint32_t gpu_shader_value_type_id(const spirv_module *module, uint32_t value_id) {
    const spirv_id_record *value = spirv_find_id(module, value_id);
    if (!value) return 0;
    switch (value->kind) {
    case SPIRV_ID_CONSTANT:
    case SPIRV_ID_VARIABLE:
    case SPIRV_ID_FUNCTION_PARAMETER:
    case SPIRV_ID_INSTRUCTION_RESULT:
    case SPIRV_ID_UNDEFINED:
        return value->type_id;
    default:
        return 0;
    }
}

static const spirv_id_record *gpu_shader_type_record(const spirv_module *module, uint32_t type_id) {
    const spirv_id_record *type = spirv_find_id(module, type_id);
    return type && type->kind == SPIRV_ID_TYPE ? type : NULL;
}

static int gpu_shader_is_scalar_integer(const spirv_module *module, uint32_t type_id) {
    const spirv_id_record *type = gpu_shader_type_record(module, type_id);
    return type && type->type_kind == SPIRV_TYPE_INT;
}

static int gpu_shader_array_length(const spirv_module *module, const spirv_id_record *array_type,
                                   uint64_t *length_value) {
    const spirv_id_record *length = array_type ? spirv_find_id(module, array_type->length_id) : NULL;
    const spirv_id_record *length_type = length ? gpu_shader_type_record(module, length->type_id) : NULL;
    if (!length || length->kind != SPIRV_ID_CONSTANT || length->constant_kind != SPIRV_CONSTANT_SCALAR ||
        !length_type || length_type->type_kind != SPIRV_TYPE_INT || !length_type->width || length_type->width > 64)
        return 0;
    if (length_type->signedness) {
        uint64_t sign_bit = UINT64_C(1) << (length_type->width - 1);
        if (length->value_bits & sign_bit) return 0;
    }
    if (length_value) *length_value = length->value_bits;
    return length->value_bits != 0;
}

static int gpu_shader_composite_element(const spirv_module *module, uint32_t composite_type_id,
                                        uint32_t index, uint32_t *element_type_id) {
    const spirv_id_record *type = gpu_shader_type_record(module, composite_type_id);
    if (!type || !element_type_id) return 0;
    switch (type->type_kind) {
    case SPIRV_TYPE_VECTOR:
    case SPIRV_TYPE_MATRIX:
        if (index >= type->element_count) return 0;
        *element_type_id = type->element_type;
        return 1;
    case SPIRV_TYPE_ARRAY: {
        uint64_t length = 0;
        if (!gpu_shader_array_length(module, type, &length) || index >= length) return 0;
        *element_type_id = type->element_type;
        return 1;
    }
    case SPIRV_TYPE_STRUCT:
        if (index >= type->member_count) return 0;
        *element_type_id = type->members[index].type_id;
        return 1;
    default:
        return 0;
    }
}

static int gpu_shader_composite_path_type(const spirv_module *module, uint32_t type_id,
                                          const uint32_t *indexes, uint32_t index_count,
                                          uint32_t *selected_type_id) {
    if (!indexes || !index_count || !selected_type_id) return 0;
    uint32_t current_type = type_id;
    for (uint32_t i = 0; i < index_count; i++) {
        uint32_t child_type = 0;
        if (!gpu_shader_composite_element(module, current_type, indexes[i], &child_type)) return 0;
        current_type = child_type;
    }
    *selected_type_id = current_type;
    return 1;
}

static int gpu_shader_validate_composite(const spirv_module *module, const spirv_instruction *source) {
    const uint32_t *w = source->words;
    uint32_t n = source->word_count;
    if (n < 3) return GPU_IR_INVALID;
    switch (source->opcode) {
    case 77: if (n != 5) return GPU_IR_INVALID; break;
    case 78: case 82: if (n < 6) return GPU_IR_INVALID; break;
    case 79: if (n < 7) return GPU_IR_INVALID; break;
    case 80: if (n < 4) return GPU_IR_INVALID; break;
    case 81: if (n < 5) return GPU_IR_INVALID; break;
    case 84: if (n != 4) return GPU_IR_INVALID; break;
    default: return GPU_IR_UNSUPPORTED;
    }
    const uint32_t result_type_id = w[1];
    const spirv_id_record *result_type = gpu_shader_type_record(module, result_type_id);
    if (!result_type) return GPU_IR_INVALID;
    switch (source->opcode) {
    case 77: { // OpVectorExtractDynamic
        uint32_t vector_type_id = gpu_shader_value_type_id(module, w[3]);
        const spirv_id_record *vector_type = gpu_shader_type_record(module, vector_type_id);
        uint32_t index_type_id = gpu_shader_value_type_id(module, w[4]);
        return n == 5 && vector_type && vector_type->type_kind == SPIRV_TYPE_VECTOR &&
            vector_type->element_type == result_type_id && gpu_shader_is_scalar_integer(module, index_type_id) ?
            GPU_IR_OK : GPU_IR_INVALID;
    }
    case 78: { // OpVectorInsertDynamic
        uint32_t vector_type_id = gpu_shader_value_type_id(module, w[3]);
        uint32_t component_type_id = gpu_shader_value_type_id(module, w[4]);
        uint32_t index_type_id = gpu_shader_value_type_id(module, w[5]);
        return n == 6 && result_type->type_kind == SPIRV_TYPE_VECTOR && vector_type_id == result_type_id &&
            component_type_id == result_type->element_type && gpu_shader_is_scalar_integer(module, index_type_id) ?
            GPU_IR_OK : GPU_IR_INVALID;
    }
    case 79: { // OpVectorShuffle
        if (n < 7 || result_type->type_kind != SPIRV_TYPE_VECTOR || n - 5 != result_type->element_count)
            return GPU_IR_INVALID;
        uint32_t first_type_id = gpu_shader_value_type_id(module, w[3]);
        uint32_t second_type_id = gpu_shader_value_type_id(module, w[4]);
        const spirv_id_record *first = gpu_shader_type_record(module, first_type_id);
        const spirv_id_record *second = gpu_shader_type_record(module, second_type_id);
        if (!first || !second || first->type_kind != SPIRV_TYPE_VECTOR || second->type_kind != SPIRV_TYPE_VECTOR ||
            first->element_type != result_type->element_type || second->element_type != result_type->element_type)
            return GPU_IR_INVALID;
        uint64_t source_component_count = (uint64_t)first->element_count + second->element_count;
        for (uint32_t i = 5; i < n; i++)
            if (w[i] != UINT32_MAX && w[i] >= source_component_count) return GPU_IR_INVALID;
        return GPU_IR_OK;
    }
    case 80: { // OpCompositeConstruct
        if (n < 4) return GPU_IR_INVALID;
        if (result_type->type_kind == SPIRV_TYPE_VECTOR) {
            uint32_t component_count = 0;
            for (uint32_t i = 3; i < n; i++) {
                const spirv_id_record *part = gpu_shader_type_record(module, gpu_shader_value_type_id(module, w[i]));
                if (!part) return GPU_IR_INVALID;
                if (part->type_kind == SPIRV_TYPE_VECTOR) {
                    if (part->element_type != result_type->element_type) return GPU_IR_INVALID;
                    component_count += part->element_count;
                } else {
                    if (w[i] == 0 || gpu_shader_value_type_id(module, w[i]) != result_type->element_type)
                        return GPU_IR_INVALID;
                    component_count++;
                }
            }
            return n >= 5 && component_count == result_type->element_count ? GPU_IR_OK : GPU_IR_INVALID;
        }
        uint32_t expected_count = result_type->type_kind == SPIRV_TYPE_STRUCT ?
            result_type->member_count : result_type->type_kind == SPIRV_TYPE_MATRIX ?
            result_type->element_count : 0;
        if (result_type->type_kind == SPIRV_TYPE_ARRAY) {
            uint64_t length = 0;
            if (!gpu_shader_array_length(module, result_type, &length) || length > UINT32_MAX)
                return GPU_IR_INVALID;
            expected_count = (uint32_t)length;
        }
        if (!expected_count || n - 3 != expected_count) return GPU_IR_INVALID;
        for (uint32_t i = 0; i < expected_count; i++) {
            uint32_t expected_type = 0;
            if (!gpu_shader_composite_element(module, result_type_id, i, &expected_type) ||
                gpu_shader_value_type_id(module, w[3 + i]) != expected_type) return GPU_IR_INVALID;
        }
        return GPU_IR_OK;
    }
    case 81: { // OpCompositeExtract
        uint32_t composite_type_id = gpu_shader_value_type_id(module, w[3]);
        uint32_t selected_type_id = 0;
        return n >= 5 && gpu_shader_composite_path_type(module, composite_type_id, w + 4, n - 4,
            &selected_type_id) && selected_type_id == result_type_id ? GPU_IR_OK : GPU_IR_INVALID;
    }
    case 82: { // OpCompositeInsert
        uint32_t object_type_id = gpu_shader_value_type_id(module, w[3]);
        uint32_t composite_type_id = gpu_shader_value_type_id(module, w[4]);
        uint32_t selected_type_id = 0;
        return n >= 6 && composite_type_id == result_type_id &&
            gpu_shader_composite_path_type(module, composite_type_id, w + 5, n - 5, &selected_type_id) &&
            selected_type_id == object_type_id ? GPU_IR_OK : GPU_IR_INVALID;
    }
    case 84: { // OpTranspose
        uint32_t matrix_type_id = gpu_shader_value_type_id(module, w[3]);
        const spirv_id_record *matrix_type = gpu_shader_type_record(module, matrix_type_id);
        if (n != 4 || result_type->type_kind != SPIRV_TYPE_MATRIX || !matrix_type ||
            matrix_type->type_kind != SPIRV_TYPE_MATRIX) return GPU_IR_INVALID;
        const spirv_id_record *result_column = gpu_shader_type_record(module, result_type->element_type);
        const spirv_id_record *matrix_column = gpu_shader_type_record(module, matrix_type->element_type);
        if (!result_column || !matrix_column || result_column->type_kind != SPIRV_TYPE_VECTOR ||
            matrix_column->type_kind != SPIRV_TYPE_VECTOR || result_column->element_type != matrix_column->element_type ||
            result_type->element_count != matrix_column->element_count ||
            matrix_type->element_count != result_column->element_count) return GPU_IR_INVALID;
        return GPU_IR_OK;
    }
    default:
        return GPU_IR_UNSUPPORTED;
    }
}

static int gpu_shader_validate_array_length(const spirv_module *module, const spirv_instruction *source) {
    const uint32_t *w = source->words;
    if (source->word_count != 5) return GPU_IR_INVALID;
    const spirv_id_record *result_type = gpu_shader_type_record(module, w[1]);
    const spirv_id_record *structure_pointer = spirv_find_id(module, w[3]);
    const spirv_id_record *pointer_type = structure_pointer ?
        gpu_shader_type_record(module, structure_pointer->type_id) : NULL;
    const spirv_id_record *structure_type = pointer_type && pointer_type->type_kind == SPIRV_TYPE_POINTER ?
        gpu_shader_type_record(module, pointer_type->pointee_type_id) : NULL;
    if (!result_type || result_type->type_kind != SPIRV_TYPE_INT ||
        (result_type->width != 32 && result_type->width != 64) ||
        result_type->signedness || !structure_pointer ||
        (structure_pointer->kind != SPIRV_ID_VARIABLE &&
         structure_pointer->kind != SPIRV_ID_INSTRUCTION_RESULT &&
         structure_pointer->kind != SPIRV_ID_FUNCTION_PARAMETER) ||
        !pointer_type || pointer_type->type_kind != SPIRV_TYPE_POINTER || !structure_type ||
        structure_type->type_kind != SPIRV_TYPE_STRUCT || !structure_type->member_count ||
        w[4] != structure_type->member_count - 1) return GPU_IR_INVALID;
    const spirv_id_record *array_type = gpu_shader_type_record(module,
        structure_type->members[w[4]].type_id);
    return array_type && array_type->type_kind == SPIRV_TYPE_RUNTIME_ARRAY ? GPU_IR_OK : GPU_IR_INVALID;
}

static int gpu_shader_numeric_shape(const spirv_module *module, uint32_t type_id,
                                    const spirv_id_record **scalar_type, uint32_t *component_count) {
    const spirv_id_record *type = gpu_shader_type_record(module, type_id);
    if (!type || !scalar_type || !component_count) return 0;
    if (type->type_kind == SPIRV_TYPE_VECTOR) {
        *scalar_type = gpu_shader_type_record(module, type->element_type);
        *component_count = type->element_count;
    } else {
        *scalar_type = type;
        *component_count = 1;
    }
    return *scalar_type && (*scalar_type)->kind == SPIRV_ID_TYPE &&
        ((*scalar_type)->type_kind == SPIRV_TYPE_INT || (*scalar_type)->type_kind == SPIRV_TYPE_FLOAT) &&
        (*scalar_type)->width && *component_count;
}

static int gpu_shader_validate_conversion(const spirv_module *module, const spirv_instruction *source,
                                          uint32_t operation) {
    const uint32_t *w = source->words;
    if (source->word_count != 4) return GPU_IR_INVALID;
    const spirv_id_record *input = NULL, *result = NULL;
    uint32_t input_components = 0, result_components = 0;
    if (!gpu_shader_numeric_shape(module, gpu_shader_value_type_id(module, w[3]), &input, &input_components) ||
        !gpu_shader_numeric_shape(module, w[1], &result, &result_components)) return GPU_IR_INVALID;
    if (operation != GPU_IR_SHADER_CONVERT_BITCAST && input_components != result_components) return GPU_IR_INVALID;
    switch (operation) {
    case GPU_IR_SHADER_CONVERT_FLOAT_TO_UNSIGNED:
        return input->type_kind == SPIRV_TYPE_FLOAT && result->type_kind == SPIRV_TYPE_INT &&
            !result->signedness ? GPU_IR_OK : GPU_IR_INVALID;
    case GPU_IR_SHADER_CONVERT_FLOAT_TO_SIGNED:
        return input->type_kind == SPIRV_TYPE_FLOAT && result->type_kind == SPIRV_TYPE_INT &&
            result->signedness ? GPU_IR_OK : GPU_IR_INVALID;
    case GPU_IR_SHADER_CONVERT_SIGNED_TO_FLOAT:
        return input->type_kind == SPIRV_TYPE_INT && input->signedness && result->type_kind == SPIRV_TYPE_FLOAT ?
            GPU_IR_OK : GPU_IR_INVALID;
    case GPU_IR_SHADER_CONVERT_UNSIGNED_TO_FLOAT:
        return input->type_kind == SPIRV_TYPE_INT && !input->signedness && result->type_kind == SPIRV_TYPE_FLOAT ?
            GPU_IR_OK : GPU_IR_INVALID;
    case GPU_IR_SHADER_CONVERT_UNSIGNED_WIDTH:
        return input->type_kind == SPIRV_TYPE_INT && !input->signedness && result->type_kind == SPIRV_TYPE_INT &&
            !result->signedness ? GPU_IR_OK : GPU_IR_INVALID;
    case GPU_IR_SHADER_CONVERT_SIGNED_WIDTH:
        return input->type_kind == SPIRV_TYPE_INT && input->signedness && result->type_kind == SPIRV_TYPE_INT &&
            result->signedness ? GPU_IR_OK : GPU_IR_INVALID;
    case GPU_IR_SHADER_CONVERT_FLOAT_WIDTH:
        return input->type_kind == SPIRV_TYPE_FLOAT && result->type_kind == SPIRV_TYPE_FLOAT ?
            GPU_IR_OK : GPU_IR_INVALID;
    case GPU_IR_SHADER_CONVERT_BITCAST: {
        if (input->width > UINT32_MAX / input_components || result->width > UINT32_MAX / result_components)
            return GPU_IR_INVALID;
        uint32_t input_bits = input->width * input_components;
        uint32_t result_bits = result->width * result_components;
        return input_bits == result_bits ? GPU_IR_OK : GPU_IR_INVALID;
    }
    default: return GPU_IR_UNSUPPORTED;
    }
}

static int gpu_shader_validate_bit_count(const spirv_module *module, const spirv_instruction *source) {
    const uint32_t *w = source->words;
    const spirv_id_record *input = NULL, *result = NULL;
    uint32_t input_components = 0, result_components = 0;
    if (source->word_count != 4) return GPU_IR_INVALID;
    if (!gpu_shader_numeric_shape(module, gpu_shader_value_type_id(module, w[3]), &input, &input_components) ||
        !gpu_shader_numeric_shape(module, w[1], &result, &result_components) ||
        input->type_kind != SPIRV_TYPE_INT || result->type_kind != SPIRV_TYPE_INT ||
        input_components != result_components) return GPU_IR_INVALID;
    return input->width == 32 && result->width == 32 ? GPU_IR_OK : GPU_IR_UNSUPPORTED;
}

static int gpu_shader_validate_bit_reverse(const spirv_module *module, const spirv_instruction *source) {
    const uint32_t *w = source->words;
    const spirv_id_record *input = NULL, *result = NULL;
    uint32_t input_components = 0, result_components = 0;
    if (source->word_count != 4) return GPU_IR_INVALID;
    if (gpu_shader_value_type_id(module, w[3]) != w[1] ||
        !gpu_shader_numeric_shape(module, w[1], &result, &result_components) ||
        !gpu_shader_numeric_shape(module, gpu_shader_value_type_id(module, w[3]), &input, &input_components) ||
        input->type_kind != SPIRV_TYPE_INT || result->type_kind != SPIRV_TYPE_INT ||
        input_components != result_components) return GPU_IR_INVALID;
    return input->width == 32 && result->width == 32 ? GPU_IR_OK : GPU_IR_UNSUPPORTED;
}

static int gpu_shader_validate_subgroup_vote(const spirv_module *module, const spirv_instruction *source) {
    const uint32_t *w = source->words;
    if (source->word_count != 5) return GPU_IR_INVALID;
    uint32_t scope = 0;
    const spirv_id_record *result = gpu_shader_type_record(module, w[1]);
    const spirv_id_record *predicate = spirv_find_id(module, w[4]);
    const spirv_id_record *predicate_type = predicate ? gpu_shader_type_record(module, predicate->type_id) : NULL;
    if (!gpu_shader_has_capability(module, SPIRV_CAPABILITY_GROUP_NON_UNIFORM) ||
        !gpu_shader_has_capability(module, SPIRV_CAPABILITY_GROUP_NON_UNIFORM_VOTE) ||
        !gpu_shader_u32_constant(module, w[3], &scope) || scope != SPIRV_SCOPE_SUBGROUP || !result ||
        result->type_kind != SPIRV_TYPE_BOOL || !predicate ||
        (predicate->kind != SPIRV_ID_CONSTANT && predicate->kind != SPIRV_ID_INSTRUCTION_RESULT &&
         predicate->kind != SPIRV_ID_FUNCTION_PARAMETER) || !predicate_type ||
        predicate_type->type_kind != SPIRV_TYPE_BOOL) return GPU_IR_INVALID;
    return GPU_IR_OK;
}

static int gpu_shader_validate_subgroup_integer_reduction(const spirv_module *module,
                                                           const spirv_instruction *source,
                                                           uint32_t opcode) {
    const uint32_t *w = source->words;
    if (source->word_count != 6) return GPU_IR_INVALID;
    uint32_t scope = 0, group_operation = UINT32_MAX;
    const spirv_id_record *result = gpu_shader_type_record(module, w[1]);
    const spirv_id_record *value = spirv_find_id(module, w[5]);
    const spirv_id_record *input = value ? gpu_shader_type_record(module, value->type_id) : NULL;
    int signed_operation = opcode == 352 || opcode == 355;
    int minmax_operation = opcode == 352 || opcode == 353 || opcode == 355 || opcode == 356;
    if (!gpu_shader_has_capability(module, SPIRV_CAPABILITY_GROUP_NON_UNIFORM) ||
        !gpu_shader_has_capability(module, SPIRV_CAPABILITY_GROUP_NON_UNIFORM_ARITHMETIC) ||
        !gpu_shader_u32_constant(module, w[3], &scope) || scope != SPIRV_SCOPE_SUBGROUP ||
        !gpu_shader_u32_constant(module, w[4], &group_operation) || group_operation != 0 ||
        !result || result->type_kind != SPIRV_TYPE_INT || result->width != 32 || !value || !input ||
        value->type_id != w[1] || input->type_kind != SPIRV_TYPE_INT || input->width != 32 ||
        (minmax_operation && (result->signedness != (uint32_t)signed_operation ||
                              input->signedness != (uint32_t)signed_operation)) ||
        result->signedness != input->signedness ||
        (value->kind != SPIRV_ID_CONSTANT && value->kind != SPIRV_ID_INSTRUCTION_RESULT &&
         value->kind != SPIRV_ID_FUNCTION_PARAMETER)) return GPU_IR_INVALID;
    return GPU_IR_OK;
}

static int gpu_shader_validate_bit_field(const spirv_module *module, const spirv_instruction *source,
                                         uint32_t opcode) {
    const uint32_t *w = source->words;
    uint32_t insert = opcode == 201;
    if (source->word_count != (insert ? 7u : 6u)) return GPU_IR_INVALID;
    uint32_t base_type_id = gpu_shader_value_type_id(module, w[3]);
    const spirv_id_record *base = NULL, *result = NULL;
    uint32_t base_components = 0, result_components = 0;
    if (base_type_id != w[1] ||
        !gpu_shader_numeric_shape(module, base_type_id, &base, &base_components) ||
        !gpu_shader_numeric_shape(module, w[1], &result, &result_components) ||
        base->type_kind != SPIRV_TYPE_INT || result->type_kind != SPIRV_TYPE_INT ||
        base_components != result_components) return GPU_IR_INVALID;

    uint32_t offset_id = w[insert ? 5 : 4], count_id = w[insert ? 6 : 5];
    const spirv_id_record *offset = gpu_shader_type_record(module, gpu_shader_value_type_id(module, offset_id));
    const spirv_id_record *count = gpu_shader_type_record(module, gpu_shader_value_type_id(module, count_id));
    if (!offset || !count || offset->type_kind != SPIRV_TYPE_INT || count->type_kind != SPIRV_TYPE_INT)
        return GPU_IR_INVALID;
    if (insert && gpu_shader_value_type_id(module, w[4]) != w[1]) return GPU_IR_INVALID;
    if (base->width != 32 || result->width != 32 || offset->width != 32 || count->width != 32)
        return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

// The first storage-image lowering is deliberately exact: optimal RGBA8 2D images map to
// Metal texture2d<float>; optional image-operands and other image shapes need separate semantics.
static int gpu_shader_validate_image_instruction(const spirv_module *module,
                                                 const spirv_instruction *source,
                                                 uint32_t opcode) {
    const uint32_t *w = source->words;
    int fetch = opcode == 95;
    int read = opcode == 98;
    if (fetch) {
        if (source->word_count == 5) {
            // A one-level image with no image operands selects its only level.
        } else if (source->word_count == 7 && w[5] == 2 &&
                   gpu_shader_is_zero_32bit_integer_scalar_constant(module, w[6])) {
            // OpImageFetch's Lod operand is an integer ID; the supported image has exactly one mip.
        } else return GPU_IR_UNSUPPORTED;
    } else if ((read && source->word_count != 5) || (!read && source->word_count != 4)) {
        return GPU_IR_UNSUPPORTED;
    }
    uint32_t image_id = read || fetch ? w[3] : w[1];
    uint32_t coordinate_id = read || fetch ? w[4] : w[2];
    uint32_t texel_type_id = read || fetch ? w[1] : gpu_shader_value_type_id(module, w[3]);
    uint32_t image_type_id = gpu_shader_value_type_id(module, image_id);
    const spirv_id_record *image = spirv_find_id(module, image_type_id);
    const spirv_id_record *sampled = image ? spirv_find_id(module, image->element_type) : NULL;
    const spirv_id_record *coordinate_value = spirv_find_id(module, coordinate_id);
    const spirv_id_record *coordinate = coordinate_value ? spirv_find_id(module, coordinate_value->type_id) : NULL;
    const spirv_id_record *coordinate_component = coordinate && coordinate->type_kind == SPIRV_TYPE_VECTOR ?
        spirv_find_id(module, coordinate->element_type) : NULL;
    const spirv_id_record *texel = spirv_find_id(module, texel_type_id);
    const spirv_id_record *texel_component = texel && texel->type_kind == SPIRV_TYPE_VECTOR ?
        spirv_find_id(module, texel->element_type) : NULL;
    if (!image || image->kind != SPIRV_ID_TYPE || image->type_kind != SPIRV_TYPE_IMAGE ||
        image->image_dimension != 1 || image->image_depth || image->image_arrayed ||
        image->image_multisampled || image->image_sampled != (fetch ? 1u : 2u) ||
        image->image_format != (fetch ? 0u : 4u) ||
        image->image_access_qualifier || !sampled || sampled->kind != SPIRV_ID_TYPE ||
        sampled->type_kind != SPIRV_TYPE_FLOAT || sampled->width != 32 ||
        !coordinate || coordinate->kind != SPIRV_ID_TYPE || coordinate->type_kind != SPIRV_TYPE_VECTOR ||
        coordinate->element_count != 2 || !coordinate_component || coordinate_component->kind != SPIRV_ID_TYPE ||
        coordinate_component->type_kind != SPIRV_TYPE_INT || coordinate_component->width != 32 ||
        !texel || texel->kind != SPIRV_ID_TYPE || texel->type_kind != SPIRV_TYPE_VECTOR ||
        texel->element_count != 4 || texel->element_type != sampled->id || !texel_component ||
        texel_component->kind != SPIRV_ID_TYPE || texel_component->type_kind != SPIRV_TYPE_FLOAT ||
        texel_component->width != 32) return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static int gpu_shader_validate_image_query_size(const spirv_module *module,
                                                const spirv_instruction *source,
                                                uint32_t opcode) {
    const uint32_t *w = source->words;
    if (source->word_count != (opcode == 103 ? 5u : 4u)) return GPU_IR_INVALID;
    if (opcode == 103) {
        const spirv_id_record *lod = spirv_find_id(module, gpu_shader_value_type_id(module, w[4]));
        if (!lod || lod->kind != SPIRV_ID_TYPE || lod->type_kind != SPIRV_TYPE_INT || lod->width != 32)
            return GPU_IR_UNSUPPORTED;
    }
    const spirv_id_record *image = spirv_find_id(module, gpu_shader_value_type_id(module, w[3]));
    const spirv_id_record *sampled = image ? spirv_find_id(module, image->element_type) : NULL;
    const spirv_id_record *result = spirv_find_id(module, w[1]);
    const spirv_id_record *component = result && result->type_kind == SPIRV_TYPE_VECTOR ?
        spirv_find_id(module, result->element_type) : NULL;
    int sampled_color = image && image->image_sampled == 1 && image->image_format == 0;
    int storage_rgba8 = image && image->image_sampled == 2 && image->image_format == 4;
    if (!image || image->kind != SPIRV_ID_TYPE || image->type_kind != SPIRV_TYPE_IMAGE ||
        image->image_dimension != 1 || image->image_depth ||
        image->image_multisampled || (!sampled_color && !storage_rgba8) || image->image_access_qualifier ||
        !sampled || sampled->kind != SPIRV_ID_TYPE || sampled->type_kind != SPIRV_TYPE_FLOAT ||
        sampled->width != 32 || !result || result->kind != SPIRV_ID_TYPE ||
        result->type_kind != SPIRV_TYPE_VECTOR ||
        result->element_count != (image->image_arrayed ? 3u : 2u) || !component ||
        component->kind != SPIRV_ID_TYPE || component->type_kind != SPIRV_TYPE_INT || component->width != 32)
        return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static int gpu_shader_validate_image_query_levels(const spirv_module *module,
                                                   const spirv_instruction *source) {
    if (!module || !source || source->opcode != 106 || source->word_count != 4) return GPU_IR_INVALID;
    const uint32_t *w = source->words;
    const spirv_id_record *image = spirv_find_id(module, gpu_shader_value_type_id(module, w[3]));
    const spirv_id_record *sampled = image ? spirv_find_id(module, image->element_type) : NULL;
    const spirv_id_record *result = spirv_find_id(module, w[1]);
    int sampled_color = image && image->image_sampled == 1 && image->image_format == 0;
    int storage_rgba8 = image && image->image_sampled == 2 && image->image_format == 4;
    if (!image || image->kind != SPIRV_ID_TYPE || image->type_kind != SPIRV_TYPE_IMAGE ||
        image->image_dimension != 1 || image->image_depth ||
        image->image_multisampled || (!sampled_color && !storage_rgba8) || image->image_access_qualifier ||
        !sampled || sampled->kind != SPIRV_ID_TYPE || sampled->type_kind != SPIRV_TYPE_FLOAT ||
        sampled->width != 32 || !result || result->kind != SPIRV_ID_TYPE ||
        result->type_kind != SPIRV_TYPE_INT || result->width != 32)
        return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static int gpu_shader_validate_image_query_samples(const spirv_module *module,
                                                    const spirv_instruction *source) {
    if (!module || !source || source->opcode != 107 || source->word_count != 4) return GPU_IR_INVALID;
    const uint32_t *w = source->words;
    const spirv_id_record *image = spirv_find_id(module, gpu_shader_value_type_id(module, w[3]));
    const spirv_id_record *sampled = image ? spirv_find_id(module, image->element_type) : NULL;
    const spirv_id_record *result = spirv_find_id(module, w[1]);
    if (!image || image->kind != SPIRV_ID_TYPE || image->type_kind != SPIRV_TYPE_IMAGE ||
        image->image_dimension != 1 || image->image_depth || image->image_arrayed > 1 ||
        image->image_multisampled != 1 || image->image_sampled != 1 || image->image_format ||
        image->image_access_qualifier || !sampled || sampled->kind != SPIRV_ID_TYPE ||
        sampled->type_kind != SPIRV_TYPE_FLOAT || sampled->width != 32 || !result ||
        result->kind != SPIRV_ID_TYPE || result->type_kind != SPIRV_TYPE_INT || result->width != 32)
        return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static int gpu_shader_validate_sampled_image(const spirv_module *module,
                                            const spirv_instruction *source) {
    if (!module || !source || source->opcode != 86 || source->word_count != 5) return GPU_IR_INVALID;
    const uint32_t *w = source->words;
    const spirv_id_record *result_type = spirv_find_id(module, w[1]);
    const spirv_id_record *image = spirv_find_id(module, gpu_shader_value_type_id(module, w[3]));
    const spirv_id_record *sampler = spirv_find_id(module, gpu_shader_value_type_id(module, w[4]));
    if (!result_type || result_type->kind != SPIRV_ID_TYPE ||
        result_type->type_kind != SPIRV_TYPE_SAMPLED_IMAGE || !image ||
        image->kind != SPIRV_ID_TYPE || image->type_kind != SPIRV_TYPE_IMAGE ||
        image->image_sampled != 1 || image->image_dimension == 6 || image->image_dimension == 5 ||
        !sampler || sampler->kind != SPIRV_ID_TYPE || sampler->type_kind != SPIRV_TYPE_SAMPLER ||
        result_type->element_type != image->id) return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static int gpu_shader_validate_image_sample(const spirv_module *module, const spirv_instruction *source) {
    if (!module || !source || (source->opcode != 87 && source->opcode != 88)) return GPU_IR_INVALID;
    int implicit_lod = source->opcode == 87;
    if (source->word_count != (implicit_lod ? 5u : 7u)) return GPU_IR_INVALID;
    const uint32_t *w = source->words;
    const spirv_id_record *sampled_value = spirv_find_id(module, w[3]);
    const spirv_id_record *sampled_type = sampled_value ? spirv_find_id(module, sampled_value->type_id) : NULL;
    const spirv_id_record *image = sampled_type && sampled_type->kind == SPIRV_ID_TYPE &&
        sampled_type->type_kind == SPIRV_TYPE_SAMPLED_IMAGE ?
        spirv_find_id(module, sampled_type->element_type) : NULL;
    const spirv_id_record *coordinate_value = spirv_find_id(module, w[4]);
    const spirv_id_record *coordinate = coordinate_value ? spirv_find_id(module, coordinate_value->type_id) : NULL;
    const spirv_id_record *coordinate_component = coordinate && coordinate->type_kind == SPIRV_TYPE_VECTOR ?
        spirv_find_id(module, coordinate->element_type) : NULL;
    const spirv_id_record *result = spirv_find_id(module, w[1]);
    const spirv_id_record *result_component = result && result->type_kind == SPIRV_TYPE_VECTOR ?
        spirv_find_id(module, result->element_type) : NULL;
    const spirv_id_record *sampled_component = image ? spirv_find_id(module, image->element_type) : NULL;
    if ((!implicit_lod && (w[5] != 2 || !gpu_shader_is_zero_32bit_float_scalar_constant(module, w[6]))) ||
        !image || image->kind != SPIRV_ID_TYPE || image->type_kind != SPIRV_TYPE_IMAGE ||
        image->image_dimension != 1 || image->image_depth || image->image_arrayed ||
        image->image_multisampled || image->image_sampled != 1 || image->image_format != 0 ||
        image->image_access_qualifier || !sampled_component || sampled_component->kind != SPIRV_ID_TYPE ||
        sampled_component->type_kind != SPIRV_TYPE_FLOAT || sampled_component->width != 32 ||
        !coordinate || coordinate->kind != SPIRV_ID_TYPE || coordinate->type_kind != SPIRV_TYPE_VECTOR ||
        coordinate->element_count != 2 || !coordinate_component || coordinate_component->kind != SPIRV_ID_TYPE ||
        coordinate_component->type_kind != SPIRV_TYPE_FLOAT || coordinate_component->width != 32 ||
        !result || result->kind != SPIRV_ID_TYPE || result->type_kind != SPIRV_TYPE_VECTOR ||
        result->element_count != 4 || result->element_type != sampled_component->id || !result_component ||
        result_component->kind != SPIRV_ID_TYPE || result_component->type_kind != SPIRV_TYPE_FLOAT ||
        result_component->width != 32)
        return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static uint32_t gpu_shader_atomic_operation(uint32_t opcode) {
    switch (opcode) {
    case 227: return GPU_IR_SHADER_ATOMIC_INTEGER_LOAD;
    case 228: return GPU_IR_SHADER_ATOMIC_INTEGER_STORE;
    case 229: return GPU_IR_SHADER_ATOMIC_INTEGER_EXCHANGE;
    case 234: return GPU_IR_SHADER_ATOMIC_INTEGER_ADD;
    case 235: return GPU_IR_SHADER_ATOMIC_INTEGER_SUBTRACT;
    case 236: return GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MINIMUM;
    case 237: return GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MINIMUM;
    case 238: return GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MAXIMUM;
    case 239: return GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MAXIMUM;
    case 240: return GPU_IR_SHADER_ATOMIC_INTEGER_AND;
    case 241: return GPU_IR_SHADER_ATOMIC_INTEGER_OR;
    case 242: return GPU_IR_SHADER_ATOMIC_INTEGER_XOR;
    case 232: return GPU_IR_SHADER_ATOMIC_INTEGER_INCREMENT;
    case 233: return GPU_IR_SHADER_ATOMIC_INTEGER_DECREMENT;
    default: return 0;
    }
}

static int gpu_shader_atomic_memory_semantics_supported(uint32_t opcode, uint32_t semantics) {
    if (semantics == 0) return 1;
    if (opcode == 227) return semantics == 0x42u; // UniformMemory | Acquire.
    if (opcode == 228) return semantics == 0x44u; // UniformMemory | Release.
    if (opcode == 229 || opcode == 230 || (opcode >= 232 && opcode <= 242))
        return semantics == 0x42u || semantics == 0x44u || semantics == 0x48u;
    return 0;
}

static int gpu_shader_validate_atomic_load_store(const spirv_module *module,
                                                 const spirv_instruction *source) {
    if (!module || !source || (source->opcode != 227 && source->opcode != 228) ||
        source->word_count != (source->opcode == 227 ? 6u : 5u)) return GPU_IR_INVALID;
    const uint32_t *w = source->words;
    int atomic_load = source->opcode == 227;
    uint32_t pointer_id = atomic_load ? w[3] : w[1];
    uint32_t atomic_type_id = atomic_load ? w[1] : 0;
    uint32_t scope_id = atomic_load ? w[4] : w[2];
    uint32_t semantics_id = atomic_load ? w[5] : w[3];
    const spirv_id_record *result = atomic_load ? spirv_find_id(module, w[2]) : NULL;
    const spirv_id_record *pointer = spirv_find_id(module, pointer_id);
    const spirv_id_record *pointer_type = pointer ? spirv_find_id(module, pointer->type_id) : NULL;
    const spirv_id_record *value = atomic_load ? NULL : spirv_find_id(module, w[4]);
    if (!pointer || !pointer_type || pointer_type->kind != SPIRV_ID_TYPE ||
        pointer_type->type_kind != SPIRV_TYPE_POINTER) return GPU_IR_INVALID;
    if (atomic_load && (!result ||
        (result->kind != SPIRV_ID_INSTRUCTION_RESULT && result->kind != SPIRV_ID_UNDEFINED) ||
        result->type_id != atomic_type_id)) return GPU_IR_INVALID;
    if (!atomic_load) {
        atomic_type_id = pointer_type->element_type;
        if (!value || (value->kind != SPIRV_ID_INSTRUCTION_RESULT && value->kind != SPIRV_ID_CONSTANT &&
                       value->kind != SPIRV_ID_UNDEFINED) || value->type_id != atomic_type_id)
            return GPU_IR_INVALID;
    }
    const spirv_id_record *atomic_type = spirv_find_id(module, atomic_type_id);
    if (!atomic_type || atomic_type->kind != SPIRV_ID_TYPE || atomic_type->type_kind != SPIRV_TYPE_INT ||
        pointer_type->element_type != atomic_type_id) return GPU_IR_INVALID;
    if (atomic_type->width != 32) return GPU_IR_UNSUPPORTED;
    if (pointer_type->storage_class != 12) return GPU_IR_UNSUPPORTED;
    uint32_t scope = 0, semantics = 0;
    if (!gpu_shader_u32_constant(module, scope_id, &scope) ||
        !gpu_shader_u32_constant(module, semantics_id, &semantics)) return GPU_IR_INVALID;
    if (scope != 1 || !gpu_shader_atomic_memory_semantics_supported(source->opcode, semantics))
        return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static int gpu_shader_validate_atomic_increment_decrement(const spirv_module *module,
                                                           const spirv_instruction *source) {
    if (!module || !source || (source->opcode != 232 && source->opcode != 233) || source->word_count != 6)
        return GPU_IR_INVALID;
    const uint32_t *w = source->words;
    const spirv_id_record *result = spirv_find_id(module, w[2]);
    const spirv_id_record *result_type = spirv_find_id(module, w[1]);
    const spirv_id_record *pointer = spirv_find_id(module, w[3]);
    const spirv_id_record *pointer_type = pointer ? spirv_find_id(module, pointer->type_id) : NULL;
    uint32_t scope = 0, semantics = 0;
    if (!result || (result->kind != SPIRV_ID_INSTRUCTION_RESULT && result->kind != SPIRV_ID_UNDEFINED) ||
        !result_type || result_type->kind != SPIRV_ID_TYPE || result_type->type_kind != SPIRV_TYPE_INT ||
        !pointer || !pointer_type || pointer_type->kind != SPIRV_ID_TYPE ||
        pointer_type->type_kind != SPIRV_TYPE_POINTER || result->type_id != w[1] ||
        pointer_type->element_type != w[1]) return GPU_IR_INVALID;
    if (result_type->width != 32) return GPU_IR_UNSUPPORTED;
    if (pointer_type->storage_class != 12) return GPU_IR_UNSUPPORTED;
    if (!gpu_shader_u32_constant(module, w[4], &scope) ||
        !gpu_shader_u32_constant(module, w[5], &semantics)) return GPU_IR_INVALID;
    if (scope != 1 || !gpu_shader_atomic_memory_semantics_supported(source->opcode, semantics))
        return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static int gpu_shader_validate_atomic_rmw(const spirv_module *module, const spirv_instruction *source) {
    if (!module || !source || !gpu_shader_atomic_operation(source->opcode) || source->word_count != 7)
        return GPU_IR_INVALID;
    const uint32_t *w = source->words;
    const spirv_id_record *result = spirv_find_id(module, w[2]);
    const spirv_id_record *result_type = spirv_find_id(module, w[1]);
    const spirv_id_record *pointer = spirv_find_id(module, w[3]);
    const spirv_id_record *pointer_type = pointer ? spirv_find_id(module, pointer->type_id) : NULL;
    const spirv_id_record *pointee_type = pointer_type && pointer_type->kind == SPIRV_ID_TYPE &&
        pointer_type->type_kind == SPIRV_TYPE_POINTER ? spirv_find_id(module, pointer_type->element_type) : NULL;
    const spirv_id_record *value = spirv_find_id(module, w[6]);
    uint32_t scope = 0, semantics = 0;
    if (!result || (result->kind != SPIRV_ID_INSTRUCTION_RESULT && result->kind != SPIRV_ID_UNDEFINED) ||
        !result_type || result_type->kind != SPIRV_ID_TYPE || result_type->type_kind != SPIRV_TYPE_INT ||
        !pointer || !pointer_type || pointer_type->kind != SPIRV_ID_TYPE ||
        pointer_type->type_kind != SPIRV_TYPE_POINTER || !pointee_type || pointee_type->kind != SPIRV_ID_TYPE ||
        result->type_id != w[1] || !value ||
        (value->kind != SPIRV_ID_INSTRUCTION_RESULT && value->kind != SPIRV_ID_CONSTANT &&
         value->kind != SPIRV_ID_UNDEFINED) || value->type_id != w[1] || pointer_type->element_type != w[1])
        return GPU_IR_INVALID;
    if (result_type->width != 32) return GPU_IR_UNSUPPORTED;
    if (pointer_type->storage_class != 12) return GPU_IR_UNSUPPORTED;
    if (!gpu_shader_u32_constant(module, w[4], &scope) ||
        !gpu_shader_u32_constant(module, w[5], &semantics)) return GPU_IR_INVALID;
    if (scope != 1 || !gpu_shader_atomic_memory_semantics_supported(source->opcode, semantics))
        return GPU_IR_UNSUPPORTED; // Device scope; the MSL path implements non-relaxed orders with fences.
    return GPU_IR_OK;
}

static int gpu_shader_validate_atomic_compare_exchange(const spirv_module *module,
                                                       const spirv_instruction *source) {
    if (!module || !source || source->opcode != 230 || source->word_count != 9) return GPU_IR_INVALID;
    const uint32_t *w = source->words;
    const spirv_id_record *result = spirv_find_id(module, w[2]);
    const spirv_id_record *result_type = spirv_find_id(module, w[1]);
    const spirv_id_record *pointer = spirv_find_id(module, w[3]);
    const spirv_id_record *pointer_type = pointer ? spirv_find_id(module, pointer->type_id) : NULL;
    const spirv_id_record *pointee_type = pointer_type && pointer_type->kind == SPIRV_ID_TYPE &&
        pointer_type->type_kind == SPIRV_TYPE_POINTER ? spirv_find_id(module, pointer_type->element_type) : NULL;
    const spirv_id_record *value = spirv_find_id(module, w[7]);
    const spirv_id_record *comparator = spirv_find_id(module, w[8]);
    uint32_t scope = 0, equal_semantics = 0, unequal_semantics = 0;
    if (!result || (result->kind != SPIRV_ID_INSTRUCTION_RESULT && result->kind != SPIRV_ID_UNDEFINED) ||
        !result_type || result_type->kind != SPIRV_ID_TYPE || result_type->type_kind != SPIRV_TYPE_INT ||
        !pointer || !pointer_type || pointer_type->kind != SPIRV_ID_TYPE ||
        pointer_type->type_kind != SPIRV_TYPE_POINTER || !pointee_type || pointee_type->kind != SPIRV_ID_TYPE ||
        result->type_id != w[1] || !value || !comparator ||
        (value->kind != SPIRV_ID_INSTRUCTION_RESULT && value->kind != SPIRV_ID_CONSTANT &&
         value->kind != SPIRV_ID_UNDEFINED) ||
        (comparator->kind != SPIRV_ID_INSTRUCTION_RESULT && comparator->kind != SPIRV_ID_CONSTANT &&
         comparator->kind != SPIRV_ID_UNDEFINED) ||
        value->type_id != w[1] || comparator->type_id != w[1] || pointer_type->element_type != w[1])
        return GPU_IR_INVALID;
    if (result_type->width != 32) return GPU_IR_UNSUPPORTED;
    if (pointer_type->storage_class != 12) return GPU_IR_UNSUPPORTED; // StorageBuffer only.
    if (!gpu_shader_u32_constant(module, w[4], &scope) ||
        !gpu_shader_u32_constant(module, w[5], &equal_semantics) ||
        !gpu_shader_u32_constant(module, w[6], &unequal_semantics)) return GPU_IR_INVALID;
    if (scope != 1 || !gpu_shader_atomic_memory_semantics_supported(source->opcode, equal_semantics) ||
        !(unequal_semantics == 0 || unequal_semantics == 0x42u) ||
        (unequal_semantics == 0x42u && equal_semantics != 0x42u && equal_semantics != 0x48u))
        return GPU_IR_UNSUPPORTED; // Device scope; failure order cannot be stronger than success.
    return GPU_IR_OK;
}

static int gpu_shader_emit_function_instruction(const spirv_module *module,
                                                const spirv_instruction *source,
                                                uint32_t function_index, uint32_t block_index,
                                                uint32_t *instruction_count,
                                                gpu_ir_shader_instruction *instructions,
                                                uint32_t instruction_capacity,
                                                gpu_ir_shader_operand *operands,
                                                uint32_t operand_capacity,
                                                uint32_t *operand_count,
                                                uint32_t *literal_words,
                                                uint32_t literal_capacity,
                                                uint32_t *literal_count) {
    const uint32_t *w = source->words;
    uint32_t n = source->word_count, opcode = source->opcode;
    if (opcode == 0 || opcode == 8 || opcode == 317) return GPU_IR_OK; // Nop and line records have no executable effect.
    uint32_t ir_opcode = 0, operation = 0, result_type = 0, result_id = 0, flags = 0;
    uint32_t memory_access_flags = 0, memory_alignment = 0;
    const uint32_t *ids = NULL, *literals = NULL;
    uint32_t sampled_image_operands[3], atomic_operands[6];
    uint32_t id_count = 0, literal_count_for_instruction = 0;
    if (opcode == 77 || opcode == 78 || opcode == 79 || opcode == 80 ||
        opcode == 81 || opcode == 82 || opcode == 84) {
        int validation = gpu_shader_validate_composite(module, source);
        if (validation != GPU_IR_OK) return validation;
    }
    if (opcode == 68) {
        int validation = gpu_shader_validate_array_length(module, source);
        if (validation != GPU_IR_OK) return validation;
    }
    if (opcode == 86) {
        int validation = gpu_shader_validate_sampled_image(module, source);
        if (validation != GPU_IR_OK) return validation;
    }
    if (opcode == 87 || opcode == 88) {
        int validation = gpu_shader_validate_image_sample(module, source);
        if (validation != GPU_IR_OK) return validation;
    }
    if (opcode == 95 || opcode == 98 || opcode == 99) {
        int validation = gpu_shader_validate_image_instruction(module, source, opcode);
        if (validation != GPU_IR_OK) return validation;
    }
    if (opcode == 103 || opcode == 104) {
        int validation = gpu_shader_validate_image_query_size(module, source, opcode);
        if (validation != GPU_IR_OK) return validation;
    }
    if (opcode == 106) {
        int validation = gpu_shader_validate_image_query_levels(module, source);
        if (validation != GPU_IR_OK) return validation;
    }
    if (opcode == 107) {
        int validation = gpu_shader_validate_image_query_samples(module, source);
        if (validation != GPU_IR_OK) return validation;
    }
    if (opcode == 334 || opcode == 335) {
        int validation = gpu_shader_validate_subgroup_vote(module, source);
        if (validation != GPU_IR_OK) return validation;
    }
    if (opcode == 351 || opcode == 352 || opcode == 353 || opcode == 355 || opcode == 356 ||
        opcode == 358 || opcode == 359 || opcode == 360) {
        int validation = gpu_shader_validate_subgroup_integer_reduction(module, source, opcode);
        if (validation != GPU_IR_OK) return validation;
    }
    if (opcode == 230 || gpu_shader_atomic_operation(opcode)) {
        int validation = opcode == 230 ? gpu_shader_validate_atomic_compare_exchange(module, source) :
            (opcode == 227 || opcode == 228) ? gpu_shader_validate_atomic_load_store(module, source) :
            (opcode == 232 || opcode == 233) ? gpu_shader_validate_atomic_increment_decrement(module, source) :
            gpu_shader_validate_atomic_rmw(module, source);
        if (validation != GPU_IR_OK) return validation;
    }
    switch (opcode) {
    case 1: // OpUndef
        if (n != 3) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_UNDEFINED; result_type = w[1]; result_id = w[2];
        break;
    case 41: case 42: case 43: case 44: case 46: case 48: case 49: case 50: case 51:
        return GPU_IR_INVALID; // SPIR-V constants are module-scope declarations.
    case 55: // OpFunctionParameter
        if (n != 3) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_PARAMETER; result_type = w[1]; result_id = w[2];
        break;
    case 12: { // OpExtInst
        if (n < 6) return GPU_IR_INVALID;
        operation = gpu_shader_glsl450_arithmetic(w[4]);
        if (!operation) return GPU_IR_UNSUPPORTED;
        int validation = gpu_shader_validate_glsl450_arithmetic(module, source, operation);
        if (validation != GPU_IR_OK) return validation;
        ir_opcode = GPU_IR_SHADER_OP_ARITHMETIC;
        result_type = w[1]; result_id = w[2];
        if (!gpu_shader_is_glsl450_bit_find(operation)) flags = GPU_IR_SHADER_FLAG_FLOAT;
        ids = w + 5; id_count = n - 5;
        break;
    }
    case 334: case 335: // OpGroupNonUniformAll / Any; only Subgroup scope has Vulkan semantics.
        ir_opcode = GPU_IR_SHADER_OP_ARITHMETIC;
        operation = opcode == 335 ? GPU_IR_SHADER_ARITH_SUBGROUP_ANY : GPU_IR_SHADER_ARITH_SUBGROUP_ALL;
        result_type = w[1]; result_id = w[2]; ids = w + 4; id_count = 1;
        break;
    case 351: case 352: case 353: case 355: case 356: case 358: case 359: case 360:
        // Subgroup integer multiply, min/max, and bitwise reductions.
        ir_opcode = GPU_IR_SHADER_OP_ARITHMETIC;
        operation = opcode == 351 ? GPU_IR_SHADER_ARITH_SUBGROUP_IMUL :
                    opcode == 352 ? GPU_IR_SHADER_ARITH_SUBGROUP_SMIN :
                    opcode == 353 ? GPU_IR_SHADER_ARITH_SUBGROUP_UMIN :
                    opcode == 355 ? GPU_IR_SHADER_ARITH_SUBGROUP_SMAX :
                    opcode == 356 ? GPU_IR_SHADER_ARITH_SUBGROUP_UMAX :
                    opcode == 358 ? GPU_IR_SHADER_ARITH_SUBGROUP_BIT_AND :
                    opcode == 359 ? GPU_IR_SHADER_ARITH_SUBGROUP_BIT_OR : GPU_IR_SHADER_ARITH_SUBGROUP_BIT_XOR;
        result_type = w[1]; result_id = w[2]; ids = w + 5; id_count = 1;
        break;
    case 57: // OpFunctionCall
        if (n < 4) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_FUNCTION_CALL; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = n - 3;
        break;
    case 59: // OpVariable. Memory placement follows the pointer result type.
        if (n != 4 && n != 5) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_VARIABLE; result_type = w[1]; result_id = w[2];
        if (n == 5) { ids = w + 4; id_count = 1; }
        break;
    case 61: // OpLoad
        if (n < 4) return GPU_IR_INVALID;
        {
            int memory_rc = gpu_shader_memory_access(source, 4, 0, &memory_access_flags, &memory_alignment);
            if (memory_rc != GPU_IR_OK) return memory_rc;
        }
        ir_opcode = GPU_IR_SHADER_OP_LOAD; result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 1;
        break;
    case 62: // OpStore
        if (n < 3) return GPU_IR_INVALID;
        {
            int memory_rc = gpu_shader_memory_access(source, 3, 1, &memory_access_flags, &memory_alignment);
            if (memory_rc != GPU_IR_OK) return memory_rc;
        }
        ir_opcode = GPU_IR_SHADER_OP_STORE; ids = w + 1; id_count = 2;
        break;
    case 86: // OpSampledImage
        ir_opcode = GPU_IR_SHADER_OP_SAMPLED_IMAGE; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = 2;
        break;
    case 87: // OpImageSampleImplicitLod without optional Image Operands
        ir_opcode = GPU_IR_SHADER_OP_IMAGE_SAMPLE;
        operation = GPU_IR_SHADER_IMAGE_SAMPLE_IMPLICIT_LOD;
        result_type = w[1]; result_id = w[2];
        sampled_image_operands[0] = w[3];
        sampled_image_operands[1] = w[4];
        ids = sampled_image_operands; id_count = 2;
        break;
    case 88: // OpImageSampleExplicitLod with Lod 0
        ir_opcode = GPU_IR_SHADER_OP_IMAGE_SAMPLE; result_type = w[1]; result_id = w[2];
        sampled_image_operands[0] = w[3];
        sampled_image_operands[1] = w[4];
        sampled_image_operands[2] = w[6];
        ids = sampled_image_operands; id_count = 3;
        break;
    case 98: // OpImageRead without optional Image Operands
        ir_opcode = GPU_IR_SHADER_OP_IMAGE_READ; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = 2;
        break;
    case 95: // OpImageFetch at mip level zero
        ir_opcode = GPU_IR_SHADER_OP_IMAGE_READ; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = 2;
        break;
    case 103: case 104: // OpImageQuerySizeLod / OpImageQuerySize
        ir_opcode = GPU_IR_SHADER_OP_IMAGE_QUERY; operation = GPU_IR_SHADER_IMAGE_QUERY_SIZE;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = opcode == 103 ? 2u : 1u;
        break;
    case 106: // OpImageQueryLevels
        ir_opcode = GPU_IR_SHADER_OP_IMAGE_QUERY; operation = GPU_IR_SHADER_IMAGE_QUERY_LEVELS;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 1;
        break;
    case 107: // OpImageQuerySamples
        ir_opcode = GPU_IR_SHADER_OP_IMAGE_QUERY; operation = GPU_IR_SHADER_IMAGE_QUERY_SAMPLES;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 1;
        break;
    case 99: // OpImageWrite without optional Image Operands
        ir_opcode = GPU_IR_SHADER_OP_IMAGE_WRITE; ids = w + 1; id_count = 3;
        break;
    case 224: // OpControlBarrier (execution scope, memory scope, memory semantics)
        if (n != 4) return GPU_IR_INVALID;
        if (!gpu_shader_is_32bit_integer_scalar_constant(module, w[1]) ||
            !gpu_shader_is_32bit_integer_scalar_constant(module, w[2]) ||
            !gpu_shader_is_32bit_integer_scalar_constant(module, w[3])) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_BARRIER; operation = GPU_IR_SHADER_BARRIER_CONTROL;
        ids = w + 1; id_count = 3;
        break;
    case 225: // OpMemoryBarrier (memory scope, memory semantics)
        if (n != 3) return GPU_IR_INVALID;
        if (!gpu_shader_is_32bit_integer_scalar_constant(module, w[1]) ||
            !gpu_shader_is_32bit_integer_scalar_constant(module, w[2]))
            return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_BARRIER; operation = GPU_IR_SHADER_BARRIER_MEMORY;
        ids = w + 1; id_count = 2;
        break;
    case 65: case 66: // OpAccessChain / OpInBoundsAccessChain
        if (n < 5) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_ACCESS_CHAIN; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = n - 3;
        break;
    case 68: // OpArrayLength. The member operand is a literal, not an ID.
        ir_opcode = GPU_IR_SHADER_OP_ARRAY_LENGTH; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = 1;
        literals = w + 4; literal_count_for_instruction = 1;
        break;
    case 227: // OpAtomicLoad (pointer, scope, semantics).
        ir_opcode = GPU_IR_SHADER_OP_ATOMIC; operation = GPU_IR_SHADER_ATOMIC_INTEGER_LOAD;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 3;
        break;
    case 228: // OpAtomicStore has no result.
        ir_opcode = GPU_IR_SHADER_OP_ATOMIC; operation = GPU_IR_SHADER_ATOMIC_INTEGER_STORE;
        ids = w + 1; id_count = 4;
        break;
    case 232: case 233: // OpAtomicIIncrement / OpAtomicIDecrement return the old value.
        ir_opcode = GPU_IR_SHADER_OP_ATOMIC; operation = gpu_shader_atomic_operation(opcode);
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 3;
        break;
    case 229: case 234: case 235: case 236: case 237: case 238: case 239: case 240: case 241: case 242:
        // Integer read-modify-write operations share the four-operand common-IR atomic shape.
        ir_opcode = GPU_IR_SHADER_OP_ATOMIC; operation = gpu_shader_atomic_operation(opcode);
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 4;
        break;
    case 230: // OpAtomicCompareExchange. The legacy Kernel-only Weak opcode is not a Vulkan shader operation.
        ir_opcode = GPU_IR_SHADER_OP_ATOMIC;
        operation = GPU_IR_SHADER_ATOMIC_INTEGER_COMPARE_EXCHANGE;
        result_type = w[1]; result_id = w[2];
        atomic_operands[0] = w[3]; // pointer
        atomic_operands[1] = w[4]; // scope
        atomic_operands[2] = w[5]; // equal semantics
        atomic_operands[3] = w[6]; // unequal semantics
        atomic_operands[4] = w[8]; // comparator (expected value)
        atomic_operands[5] = w[7]; // value (desired value)
        ids = atomic_operands; id_count = 6;
        break;
    case 77: // OpVectorExtractDynamic
        if (n != 5) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 2;
        break;
    case 78: // OpVectorInsertDynamic
        if (n != 6) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = GPU_IR_SHADER_COMPOSITE_VECTOR_INSERT_DYNAMIC;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 3;
        break;
    case 79: // OpVectorShuffle: component selectors are literal words.
        if (n < 7) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = GPU_IR_SHADER_COMPOSITE_VECTOR_SHUFFLE;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 2;
        literals = w + 5; literal_count_for_instruction = n - 5;
        break;
    case 80: // OpCompositeConstruct
        if (n < 4) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = GPU_IR_SHADER_COMPOSITE_CONSTRUCT;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = n - 3;
        break;
    case 81: // OpCompositeExtract
        if (n < 5) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = GPU_IR_SHADER_COMPOSITE_EXTRACT;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 1;
        literals = w + 4; literal_count_for_instruction = n - 4;
        break;
    case 82: // OpCompositeInsert
        if (n < 6) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = GPU_IR_SHADER_COMPOSITE_INSERT;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 2;
        literals = w + 5; literal_count_for_instruction = n - 5;
        break;
    case 83: // OpCopyObject
        if (n != 4) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_COPY; result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 1;
        break;
    case 84: // OpTranspose
        if (n != 4) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = GPU_IR_SHADER_COMPOSITE_TRANSPOSE;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 1;
        break;
    case 109: case 110: case 111: case 112: case 113: case 114: case 115: case 124:
        if (n != 4) return GPU_IR_INVALID;
        operation = gpu_shader_conversion(opcode);
        if (!operation) return GPU_IR_UNSUPPORTED;
        {
            int validation = gpu_shader_validate_conversion(module, source, operation);
            if (validation != GPU_IR_OK) return validation;
        }
        ir_opcode = GPU_IR_SHADER_OP_CONVERT; result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 1;
        break;
    case 126: case 127: case 128: case 129: case 130: case 131: case 132: case 133:
    case 134: case 135: case 136: case 137: case 138: case 139: case 140: case 141:
    case 154: case 155: case 166: case 167: case 168: case 194: case 195: case 196:
    case 197: case 198: case 199: case 200: {
        if (n < 4) return GPU_IR_INVALID;
        operation = gpu_shader_arithmetic(opcode);
        if (!operation) return GPU_IR_UNSUPPORTED;
        uint32_t unary = (opcode == 126 || opcode == 127 || opcode == 154 || opcode == 155 || opcode == 168 || opcode == 200);
        if (n != (unary ? 4u : 5u)) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_ARITHMETIC; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = unary ? 1 : 2;
        const spirv_id_record *input = spirv_find_id(module, w[3]);
        flags = gpu_shader_flags_for_type(module, input ? input->type_id : result_type);
        break;
    }
    case 205: { // OpBitCount
        operation = gpu_shader_arithmetic(opcode);
        if (!operation) return GPU_IR_UNSUPPORTED;
        int validation = gpu_shader_validate_bit_count(module, source);
        if (validation != GPU_IR_OK) return validation;
        ir_opcode = GPU_IR_SHADER_OP_ARITHMETIC; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = 1;
        flags = gpu_shader_flags_for_type(module, gpu_shader_value_type_id(module, w[3]));
        break;
    }
    case 204: { // OpBitReverse
        operation = gpu_shader_arithmetic(opcode);
        if (!operation) return GPU_IR_UNSUPPORTED;
        int validation = gpu_shader_validate_bit_reverse(module, source);
        if (validation != GPU_IR_OK) return validation;
        ir_opcode = GPU_IR_SHADER_OP_ARITHMETIC; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = 1;
        flags = gpu_shader_flags_for_type(module, gpu_shader_value_type_id(module, w[3]));
        break;
    }
    case 201: case 202: case 203: {
        operation = gpu_shader_arithmetic(opcode);
        if (!operation) return GPU_IR_UNSUPPORTED;
        int validation = gpu_shader_validate_bit_field(module, source, opcode);
        if (validation != GPU_IR_OK) return validation;
        uint32_t insert = opcode == 201;
        ir_opcode = GPU_IR_SHADER_OP_ARITHMETIC; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = insert ? 4 : 3;
        flags = gpu_shader_flags_for_type(module, gpu_shader_value_type_id(module, w[3]));
        break;
    }
    case 142: { // OpVectorTimesScalar
        if (n != 5) return GPU_IR_INVALID;
        const spirv_id_record *result = gpu_shader_type_record(module, w[1]);
        uint32_t vector_type_id = gpu_shader_value_type_id(module, w[3]);
        uint32_t scalar_type_id = gpu_shader_value_type_id(module, w[4]);
        const spirv_id_record *vector = gpu_shader_type_record(module, vector_type_id);
        const spirv_id_record *component = vector && vector->type_kind == SPIRV_TYPE_VECTOR ?
            gpu_shader_type_record(module, vector->element_type) : NULL;
        const spirv_id_record *scalar = gpu_shader_type_record(module, scalar_type_id);
        if (!result || result->type_kind != SPIRV_TYPE_VECTOR || vector_type_id != w[1] ||
            !vector || vector->type_kind != SPIRV_TYPE_VECTOR || !component || !scalar ||
            scalar_type_id != vector->element_type || scalar_type_id != result->element_type)
            return GPU_IR_INVALID;
        if (component->type_kind != SPIRV_TYPE_FLOAT || component->width != 32 ||
            scalar->type_kind != SPIRV_TYPE_FLOAT || scalar->width != 32)
            return GPU_IR_UNSUPPORTED;
        ir_opcode = GPU_IR_SHADER_OP_ARITHMETIC;
        operation = GPU_IR_SHADER_ARITH_VECTOR_TIMES_SCALAR;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 2;
        flags = GPU_IR_SHADER_FLAG_FLOAT;
        break;
    }
    case 143: case 144: case 145: case 146: { // OpMatrixTimesScalar, VectorTimesMatrix, MatrixTimesVector, ...Matrix
        if (n != 5) return GPU_IR_INVALID;
        const spirv_id_record *result = gpu_shader_type_record(module, w[1]);
        const spirv_id_record *left = gpu_shader_type_record(module, gpu_shader_value_type_id(module, w[3]));
        const spirv_id_record *right = gpu_shader_type_record(module, gpu_shader_value_type_id(module, w[4]));
        // (the matrix operands: their column vectors and the float scalar beneath)
        const spirv_id_record *matrix = opcode == 144 ? right : left;
        const spirv_id_record *column = matrix && matrix->type_kind == SPIRV_TYPE_MATRIX ?
            gpu_shader_type_record(module, matrix->element_type) : NULL;
        const spirv_id_record *scalar = column && column->type_kind == SPIRV_TYPE_VECTOR ?
            gpu_shader_type_record(module, column->element_type) : NULL;
        if (!result || !left || !right || !column || !scalar) return GPU_IR_INVALID;
        if (scalar->type_kind != SPIRV_TYPE_FLOAT || scalar->width != 32) return GPU_IR_UNSUPPORTED;
        int valid;
        if (opcode == 143)          // M * s: the matrix's type, a scalar of its component type
            valid = result->id == left->id && right->id == scalar->id;
        else if (opcode == 145)     // M * v: v has one component per column, the result one per row
            valid = right->type_kind == SPIRV_TYPE_VECTOR && right->element_type == scalar->id &&
                    right->element_count == matrix->element_count && result->id == column->id;
        else if (opcode == 144)     // v * M: v has one component per row, the result one per column
            valid = left->type_kind == SPIRV_TYPE_VECTOR && left->id == column->id &&
                    result->type_kind == SPIRV_TYPE_VECTOR && result->element_type == scalar->id &&
                    result->element_count == matrix->element_count;
        else {                      // A * B: A's columns are the result's, B has a row per column of A
            const spirv_id_record *right_column = right->type_kind == SPIRV_TYPE_MATRIX ?
                gpu_shader_type_record(module, right->element_type) : NULL;
            valid = right_column && right_column->type_kind == SPIRV_TYPE_VECTOR &&
                    right_column->element_type == scalar->id && right_column->element_count == left->element_count &&
                    result->type_kind == SPIRV_TYPE_MATRIX && result->element_type == column->id &&
                    result->element_count == right->element_count;
        }
        if (!valid) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_ARITHMETIC;
        operation = opcode == 143 ? GPU_IR_SHADER_ARITH_MATRIX_TIMES_SCALAR :
                    opcode == 144 ? GPU_IR_SHADER_ARITH_VECTOR_TIMES_MATRIX :
                    opcode == 145 ? GPU_IR_SHADER_ARITH_MATRIX_TIMES_VECTOR : GPU_IR_SHADER_ARITH_MATRIX_TIMES_MATRIX;
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 2;
        flags = GPU_IR_SHADER_FLAG_FLOAT;
        break;
    }
    case 148: { // OpDot
        if (n != 5) return GPU_IR_INVALID;
        const spirv_id_record *result = gpu_shader_type_record(module, w[1]);
        uint32_t left_type_id = gpu_shader_value_type_id(module, w[3]);
        uint32_t right_type_id = gpu_shader_value_type_id(module, w[4]);
        const spirv_id_record *left = gpu_shader_type_record(module, left_type_id);
        const spirv_id_record *right = gpu_shader_type_record(module, right_type_id);
        const spirv_id_record *component = left && left->type_kind == SPIRV_TYPE_VECTOR ?
            gpu_shader_type_record(module, left->element_type) : NULL;
        if (!result || result->type_kind != SPIRV_TYPE_FLOAT ||
            !left || left->type_kind != SPIRV_TYPE_VECTOR || right_type_id != left_type_id ||
            !right || right->type_kind != SPIRV_TYPE_VECTOR || !component ||
            left->element_type != w[1] || right->element_type != w[1]) return GPU_IR_INVALID;
        if (result->width != 32 || component->type_kind != SPIRV_TYPE_FLOAT || component->width != 32)
            return GPU_IR_UNSUPPORTED;
        ir_opcode = GPU_IR_SHADER_OP_ARITHMETIC;
        operation = gpu_shader_arithmetic(opcode);
        result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 2;
        flags = GPU_IR_SHADER_FLAG_FLOAT;
        break;
    }
    case 156: case 157: case 158: case 159: case 160: case 161: case 162: case 163:
    case 164: case 165: case 170: case 171: case 172: case 173: case 174: case 175:
    case 176: case 177: case 178: case 179: case 180: case 181: case 182: case 183:
    case 184: case 185: case 186: case 187: case 188: case 189: case 190: case 191: {
        if (n < 4) return GPU_IR_INVALID;
        operation = gpu_shader_compare(opcode);
        if (!operation) return GPU_IR_UNSUPPORTED;
        uint32_t unary = opcode >= 156 && opcode <= 160;
        if (n != (unary ? 4u : 5u)) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_COMPARE; result_type = w[1]; result_id = w[2];
        ids = w + 3; id_count = unary ? 1 : 2;
        const spirv_id_record *input = spirv_find_id(module, w[3]);
        flags = gpu_shader_flags_for_type(module, input ? input->type_id : 0);
        break;
    }
    case 169: // OpSelect
        if (n != 6) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_SELECT; result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = 3;
        break;
    case 245: // OpPhi: value/parent-label pairs
        if (n < 5 || ((n - 3) & 1u)) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_PHI; result_type = w[1]; result_id = w[2]; ids = w + 3; id_count = n - 3;
        break;
    case 246: // OpLoopMerge
        if (n < 4) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_LOOP_MERGE; ids = w + 1; id_count = 2;
        literals = w + 3; literal_count_for_instruction = n - 3;
        break;
    case 247: // OpSelectionMerge
        if (n != 3) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_SELECTION_MERGE; ids = w + 1; id_count = 1;
        literals = w + 2; literal_count_for_instruction = 1;
        break;
    case 249: // OpBranch
        if (n != 2) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_BRANCH; ids = w + 1; id_count = 1;
        break;
    case 250: // OpBranchConditional
        if (n != 4 && n != 6) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_BRANCH_CONDITIONAL; ids = w + 1; id_count = 3;
        if (n == 6) { literals = w + 4; literal_count_for_instruction = 2; }
        break;
    case 251: { // OpSwitch: case literals stay in the literal stream; IDs are selector/default/case labels.
        if (n < 3) return GPU_IR_INVALID;
        const spirv_id_record *selector = spirv_find_id(module, w[1]);
        const spirv_id_record *type = selector ? spirv_find_id(module, selector->type_id) : NULL;
        if (!type || type->kind != SPIRV_ID_TYPE || type->type_kind != SPIRV_TYPE_INT || !type->width)
            return GPU_IR_UNSUPPORTED;
        uint32_t literal_words_per_case = (type->width + 31u) / 32u;
        uint32_t stride = literal_words_per_case + 1u;
        if (!stride || (n - 3u) % stride) return GPU_IR_INVALID;
        uint32_t case_count = (n - 3u) / stride;
        /* Repack non-contiguous targets into the operand array after checking capacity. */
        ir_opcode = GPU_IR_SHADER_OP_SWITCH;
        if (*instruction_count >= instruction_capacity || case_count + 2u > operand_capacity - *operand_count ||
            case_count * literal_words_per_case > literal_capacity - *literal_count) return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_shader_instruction item = {
            .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .opcode = ir_opcode, .source_opcode = opcode,
            .function_index = function_index, .block_index = block_index,
            .first_operand = *operand_count, .operand_count = case_count + 2u,
            .first_literal = *literal_count, .literal_word_count = case_count * literal_words_per_case
        };
        operands[(*operand_count)++] = (gpu_ir_shader_operand){ GPU_IR_SHADER_OPERAND_ID, w[1] };
        operands[(*operand_count)++] = (gpu_ir_shader_operand){ GPU_IR_SHADER_OPERAND_ID, w[2] };
        for (uint32_t c = 0; c < case_count; c++) {
            uint32_t literal_offset = 3u + c * stride;
            memcpy(literal_words + *literal_count, w + literal_offset,
                   (size_t)literal_words_per_case * sizeof *literal_words);
            *literal_count += literal_words_per_case;
            operands[(*operand_count)++] = (gpu_ir_shader_operand){
                GPU_IR_SHADER_OPERAND_ID, w[literal_offset + literal_words_per_case] };
        }
        instructions[(*instruction_count)++] = item;
        return GPU_IR_OK;
    }
    case 253:
        if (n != 1) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_RETURN;
        break;
    case 254:
        if (n != 2) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_RETURN_VALUE; ids = w + 1; id_count = 1;
        break;
    case 255:
        if (n != 1) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_UNREACHABLE;
        break;
    case 252: // OpKill
        if (n != 1) return GPU_IR_INVALID;
        ir_opcode = GPU_IR_SHADER_OP_TERMINATE_INVOCATION;
        break;
    default:
        return GPU_IR_UNSUPPORTED;
    }
    int rc = gpu_shader_emit(instructions, instruction_capacity, instruction_count, operands, operand_capacity,
        operand_count, literal_words, literal_capacity, literal_count, opcode, ir_opcode, operation,
        result_type, result_id, function_index, block_index, flags, ids, id_count,
        literals, literal_count_for_instruction);
    if (rc == GPU_IR_OK && opcode == 59)
        gpu_shader_variable_metadata(module, &instructions[*instruction_count - 1]);
    if (rc == GPU_IR_OK && (opcode == 61 || opcode == 62)) {
        instructions[*instruction_count - 1].memory_access_flags = memory_access_flags;
        instructions[*instruction_count - 1].memory_alignment = memory_alignment;
    }
    return rc;
}

static uint32_t gpu_shader_source_function_index(const spirv_module *module, uint32_t function_id) {
    for (uint32_t i = 0; i < spirv_function_count(module); i++) {
        const spirv_function_record *function = spirv_function_at(module, i);
        if (function && function->function_id == function_id) return i;
    }
    return UINT32_MAX;
}

static int gpu_shader_entry_point_has_interface(const spirv_entry_point *entry_point, uint32_t variable_id) {
    for (uint32_t i = 0; i < entry_point->interface_count; i++)
        if (entry_point->interfaces[i] == variable_id) return 1;
    return 0;
}

int spirv_build_gpu_ir_shader(const spirv_module *module, const spirv_entry_point *entry_point,
                              gpu_ir_shader **shader) {
    if (shader) *shader = NULL;
    if (!module || !entry_point || !shader || !module->words || !module->id_records ||
        module->instruction_count > UINT32_MAX || module->word_count > UINT32_MAX) return GPU_IR_INVALID;
    int belongs_to_module = 0;
    for (uint32_t i = 0; i < module->entry_point_count; i++)
        if (entry_point == &module->entry_points[i]) { belongs_to_module = 1; break; }
    if (!belongs_to_module) return GPU_IR_INVALID;
    uint32_t stage = gpu_stage(entry_point->execution_model);
    const spirv_function_record *entry_function = spirv_find_function(module, entry_point->function_id);
    if (!stage || !entry_function || !entry_function->block_count) return GPU_IR_UNSUPPORTED;
    uint32_t threadgroup_size[3] = { 0, 0, 0 };
    // LocalSizeId values may be overridden by VkSpecializationInfo at pipeline creation. This builder has no
    // specialization map, so only a literal LocalSize is safe to publish as a fixed Metal threadgroup size.
    if (stage == GPU_IR_STAGE_COMPUTE && entry_point->local_size_kind == SPIRV_LOCAL_SIZE_LITERAL) {
        for (uint32_t axis = 0; axis < 3; axis++) {
            uint32_t dimension = entry_point->local_size[axis];
            if (!dimension) return GPU_IR_INVALID;
            threadgroup_size[axis] = dimension;
        }
    }
    const spirv_id_record *entry_return_type = spirv_find_id(module, entry_function->result_type_id);
    if (!entry_return_type || entry_return_type->kind != SPIRV_ID_TYPE ||
        entry_return_type->type_kind != SPIRV_TYPE_VOID || entry_function->parameter_count)
        return GPU_IR_INVALID;

    uint32_t source_function_count = spirv_function_count(module);
    uint8_t *included_functions = calloc(source_function_count ? source_function_count : 1, 1);
    uint32_t *function_map = malloc((size_t)(source_function_count ? source_function_count : 1) * sizeof *function_map);
    typedef struct { spirv_iterator iterator; uint32_t function_index; } call_frame;
    call_frame *call_stack = calloc(source_function_count ? source_function_count : 1, sizeof *call_stack);
    if (!included_functions || !function_map || !call_stack) {
        free(included_functions); free(function_map); free(call_stack);
        return GPU_IR_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < source_function_count; i++) function_map[i] = UINT32_MAX;
    uint32_t entry_source_index = gpu_shader_source_function_index(module, entry_point->function_id);
    if (entry_source_index == UINT32_MAX) {
        free(included_functions); free(function_map); free(call_stack);
        return GPU_IR_INVALID;
    }
    included_functions[entry_source_index] = 1;
    uint32_t depth = 0;
    const spirv_function_record *entry_source = spirv_function_at(module, entry_source_index);
    int rc = entry_source && spirv_function_iter_begin(module, entry_source,
        &call_stack[depth].iterator) == SPIRV_OK ? GPU_IR_OK : GPU_IR_INVALID;
    call_stack[depth++].function_index = entry_source_index;
    spirv_instruction call;
    while (depth && rc == GPU_IR_OK) {
        call_frame *frame = &call_stack[depth - 1];
        int call_next = spirv_iter_next(&frame->iterator, &call);
        if (call_next < 0) { rc = GPU_IR_INVALID; break; }
        if (!call_next) {
            included_functions[frame->function_index] = 2;
            depth--;
            continue;
        }
        if (call.opcode != 57) continue;
        if (call.word_count < 4) { rc = GPU_IR_INVALID; break; }
        uint32_t target = gpu_shader_source_function_index(module, call.words[3]);
        if (target == UINT32_MAX) { rc = GPU_IR_INVALID; break; }
        if (included_functions[target] == 1) { rc = GPU_IR_INVALID; break; } // Vulkan forbids recursion.
        if (included_functions[target] == 2) continue;
        if (depth >= source_function_count) { rc = GPU_IR_INVALID; break; }
        const spirv_function_record *target_function = spirv_function_at(module, target);
        if (!target_function || spirv_function_iter_begin(module, target_function,
            &call_stack[depth].iterator) != SPIRV_OK) { rc = GPU_IR_INVALID; break; }
        included_functions[target] = 1;
        call_stack[depth++].function_index = target;
    }
    uint32_t function_count = 0, block_count = 0;
    for (uint32_t i = 0; rc == GPU_IR_OK && i < source_function_count; i++) {
        if (!included_functions[i]) continue;
        const spirv_function_record *function = spirv_function_at(module, i);
        if (!function) { rc = GPU_IR_INVALID; break; }
        if (!function->block_count) { rc = GPU_IR_UNSUPPORTED; break; }
        if (function->block_count > UINT32_MAX - block_count) { rc = GPU_IR_INVALID; break; }
        function_map[i] = function_count++;
        block_count += function->block_count;
    }
    if (rc != GPU_IR_OK) {
        free(included_functions); free(function_map); free(call_stack);
        return rc;
    }
    uint32_t instruction_capacity = (uint32_t)module->instruction_count;
    uint32_t word_capacity = (uint32_t)module->word_count;
    gpu_ir_shader_type *types = calloc(instruction_capacity ? instruction_capacity : 1, sizeof *types);
    gpu_ir_shader_member *type_members = calloc(word_capacity ? word_capacity : 1, sizeof *type_members);
    gpu_ir_shader_function *functions = calloc(function_count ? function_count : 1, sizeof *functions);
    gpu_ir_shader_block *blocks = calloc(block_count ? block_count : 1, sizeof *blocks);
    gpu_ir_shader_instruction *instructions = calloc(instruction_capacity ? instruction_capacity : 1, sizeof *instructions);
    gpu_ir_shader_operand *operands = calloc(word_capacity ? word_capacity : 1, sizeof *operands);
    uint32_t *literal_words = calloc(word_capacity ? word_capacity : 1, sizeof *literal_words);
    if (!types || !type_members || !functions || !blocks || !instructions || !operands || !literal_words) {
        free(types); free(type_members); free(functions); free(blocks); free(instructions); free(operands); free(literal_words);
        free(included_functions); free(function_map); free(call_stack);
        return GPU_IR_OUT_OF_MEMORY;
    }
    uint32_t type_count = 0, type_member_count = 0;
    uint32_t instruction_count = 0, operand_count = 0, literal_count = 0;
    spirv_iterator iterator;
    spirv_instruction source;
    spirv_iter_begin(module, &iterator);
    int next;
    while ((next = spirv_iter_next(&iterator, &source)) > 0) {
        if (source.opcode < 19 || source.opcode > 33) continue;
        if (source.word_count < 2 || type_count >= instruction_capacity) { rc = GPU_IR_INVALID; break; }
        const spirv_id_record *record = spirv_find_id(module, source.words[1]);
        uint32_t kind = record ? gpu_shader_type_kind(record->type_kind) : 0;
        if (!record || record->kind != SPIRV_ID_TYPE || !kind ||
            record->member_count > word_capacity - type_member_count) { rc = GPU_IR_UNSUPPORTED; break; }
        if ((record->type_kind == SPIRV_TYPE_INT || record->type_kind == SPIRV_TYPE_FLOAT) &&
            (!record->width || record->width > 64)) { rc = GPU_IR_UNSUPPORTED; break; }
        gpu_ir_shader_type *type = &types[type_count++];
        *type = (gpu_ir_shader_type){
            .id = record->id, .kind = kind, .bit_width = record->width,
            .signedness = record->signedness, .element_type_id = record->element_type,
            .element_count = record->element_count,
            .storage_class = record->type_kind == SPIRV_TYPE_POINTER ?
                gpu_shader_storage_class(record->storage_class) : GPU_IR_SHADER_STORAGE_UNSPECIFIED,
            .source_storage_class = record->type_kind == SPIRV_TYPE_POINTER ? record->storage_class : 0,
            .array_length_id = record->length_id, .first_member = type_member_count,
            .member_count = record->member_count, .array_stride = record->array_stride,
            .has_array_stride = record->has_array_stride,
            .decoration_flags = gpu_shader_decoration_flags(record->decoration_flags),
            .image_dimension = record->image_dimension, .image_depth = record->image_depth,
            .image_arrayed = record->image_arrayed, .image_multisampled = record->image_multisampled,
            .image_sampled = record->image_sampled, .image_format = record->image_format,
            .image_access_qualifier = record->image_access_qualifier
        };
        for (uint32_t m = 0; m < record->member_count; m++) {
            const spirv_member_record *member = &record->members[m];
            type_members[type_member_count++] = (gpu_ir_shader_member){
                .type_id = member->type_id,
                .decoration_flags = gpu_shader_decoration_flags(member->decoration_flags),
                .offset = member->offset, .matrix_stride = member->matrix_stride,
                .location = member->location, .component = member->component,
                .index = member->index,
                .builtin = member->has_builtin ? gpu_shader_builtin(member->builtin) :
                    GPU_IR_SHADER_BUILTIN_UNSPECIFIED,
                .source_builtin = member->has_builtin ? member->builtin : 0,
                .has_offset = member->has_offset, .has_matrix_stride = member->has_matrix_stride,
                .has_location = member->has_location, .has_component = member->has_component,
                .has_index = member->has_index, .has_builtin = member->has_builtin
            };
        }
    }
    if (next < 0 && rc == GPU_IR_OK) rc = GPU_IR_INVALID;

    uint32_t block_cursor = 0;
    for (uint32_t f = 0; rc == GPU_IR_OK && f < source_function_count; f++) {
        if (!included_functions[f]) continue;
        uint32_t common_function_index = function_map[f];
        const spirv_function_record *source_function = spirv_function_at(module, f);
        if (!source_function || source_function->block_count > block_count - block_cursor) { rc = GPU_IR_INVALID; break; }
        functions[common_function_index] = (gpu_ir_shader_function){
            .id = source_function->function_id,
            .result_type_id = source_function->result_type_id,
            .function_type_id = source_function->function_type_id,
            .first_block = block_cursor,
            .block_count = source_function->block_count,
            .first_instruction = 0,
            .instruction_count = 0,
            .parameter_count = source_function->parameter_count
        };
        for (uint32_t b = 0; b < source_function->block_count; b++) {
            const spirv_basic_block *source_block = spirv_function_block_at(module, source_function, b);
            if (!source_block) { rc = GPU_IR_INVALID; break; }
            blocks[block_cursor++] = (gpu_ir_shader_block){
                .function_index = common_function_index, .label_id = source_block->label_id,
                .terminator_opcode = 0
            };
        }
    }
    if (block_cursor != block_count && rc == GPU_IR_OK) rc = GPU_IR_INVALID;

    // Module-scope constants and variables are emitted before functions.
    if (rc == GPU_IR_OK) {
        int in_function = 0;
        spirv_iter_begin(module, &iterator);
        while ((next = spirv_iter_next(&iterator, &source)) > 0) {
            if (source.opcode == 54) { in_function = 1; continue; }
            if (source.opcode == 56) { in_function = 0; continue; }
            if (in_function) continue;
            if (source.opcode == 1) {
                if (source.word_count != 3) { rc = GPU_IR_INVALID; break; }
                const spirv_id_record *undefined = spirv_find_id(module, source.words[2]);
                if (!undefined || undefined->kind != SPIRV_ID_UNDEFINED || undefined->type_id != source.words[1]) {
                    rc = GPU_IR_INVALID; break;
                }
                rc = gpu_shader_emit(instructions, instruction_capacity, &instruction_count, operands,
                    word_capacity, &operand_count, literal_words, word_capacity, &literal_count,
                    source.opcode, GPU_IR_SHADER_OP_UNDEFINED, 0, source.words[1], source.words[2],
                    UINT32_MAX, UINT32_MAX, 0, NULL, 0, NULL, 0);
            } else if (source.opcode == 41 || source.opcode == 42 || source.opcode == 43 || source.opcode == 44 ||
                source.opcode == 46 || source.opcode == 48 || source.opcode == 49 ||
                source.opcode == 50 || source.opcode == 51 || source.opcode == 52) {
                rc = gpu_shader_emit_constant(module, &source, &instruction_count, instructions,
                    instruction_capacity, operands, word_capacity, &operand_count, literal_words,
                    word_capacity, &literal_count);
            } else if (source.opcode == 59) {
                if (source.word_count != 4 && source.word_count != 5) { rc = GPU_IR_INVALID; break; }
                const spirv_id_record *variable = spirv_find_id(module, source.words[2]);
                if (!variable || variable->kind != SPIRV_ID_VARIABLE) { rc = GPU_IR_INVALID; break; }
                int stage_interface = variable->storage_class == 1 || variable->storage_class == 3;
                if (stage_interface && !gpu_shader_entry_point_has_interface(entry_point, variable->id))
                    continue;
                const uint32_t *init = source.word_count == 5 ? source.words + 4 : NULL;
                rc = gpu_shader_emit(instructions, instruction_capacity, &instruction_count, operands,
                    word_capacity, &operand_count, literal_words, word_capacity, &literal_count,
                    source.opcode, GPU_IR_SHADER_OP_VARIABLE, 0, source.words[1], source.words[2],
                    UINT32_MAX, UINT32_MAX, 0, init, init ? 1u : 0u, NULL, 0);
                if (rc == GPU_IR_OK) {
                    gpu_ir_shader_instruction *lowered = &instructions[instruction_count - 1];
                    gpu_shader_variable_metadata(module, lowered);
                    lowered->entry_point_interface = stage_interface;
                }
            } else if (source.opcode == 45 || source.opcode == 52) {
                rc = GPU_IR_UNSUPPORTED;
            }
            if (rc != GPU_IR_OK) break;
        }
        if (next < 0 && rc == GPU_IR_OK) rc = GPU_IR_INVALID;
    }

    for (uint32_t f = 0; rc == GPU_IR_OK && f < source_function_count; f++) {
        if (!included_functions[f]) continue;
        uint32_t common_function_index = function_map[f];
        const spirv_function_record *source_function = spirv_function_at(module, f);
        gpu_ir_shader_function *function = &functions[common_function_index];
        function->first_instruction = instruction_count;
        uint32_t block_in_function = 0, current_block = UINT32_MAX, parameters = 0;
        spirv_iterator function_iterator;
        if (!source_function || spirv_function_iter_begin(module, source_function, &function_iterator) != SPIRV_OK) {
            rc = GPU_IR_INVALID; break;
        }
        while ((next = spirv_iter_next(&function_iterator, &source)) > 0) {
            if (source.opcode == 54 || source.opcode == 56) continue;
            if (source.opcode == 0 || source.opcode == 8 || source.opcode == 317) continue;
            if (source.opcode == 248) {
                if (source.word_count != 2 || block_in_function >= source_function->block_count) { rc = GPU_IR_INVALID; break; }
                current_block = function->first_block + block_in_function++;
                if (blocks[current_block].label_id != source.words[1]) { rc = GPU_IR_INVALID; break; }
                blocks[current_block].first_instruction = instruction_count;
                continue;
            }
            if (source.opcode == 55) {
                if (current_block != UINT32_MAX || parameters >= source_function->parameter_count) { rc = GPU_IR_INVALID; break; }
                rc = gpu_shader_emit_function_instruction(module, &source, common_function_index, UINT32_MAX,
                    &instruction_count, instructions, instruction_capacity, operands, word_capacity,
                    &operand_count, literal_words, word_capacity, &literal_count);
                parameters++;
                if (rc != GPU_IR_OK) break;
                continue;
            }
            if (current_block == UINT32_MAX) { rc = GPU_IR_INVALID; break; }
            uint32_t before = instruction_count;
            rc = gpu_shader_emit_function_instruction(module, &source, common_function_index, current_block,
                &instruction_count, instructions, instruction_capacity, operands, word_capacity,
                &operand_count, literal_words, word_capacity, &literal_count);
            if (rc != GPU_IR_OK) break;
            if (instruction_count != before) {
                blocks[current_block].instruction_count++;
                uint32_t lowered_opcode = instructions[instruction_count - 1].opcode;
                if (lowered_opcode == GPU_IR_SHADER_OP_BRANCH ||
                    lowered_opcode == GPU_IR_SHADER_OP_BRANCH_CONDITIONAL ||
                    lowered_opcode == GPU_IR_SHADER_OP_SWITCH ||
                    lowered_opcode == GPU_IR_SHADER_OP_RETURN ||
                    lowered_opcode == GPU_IR_SHADER_OP_RETURN_VALUE ||
                    lowered_opcode == GPU_IR_SHADER_OP_UNREACHABLE ||
                    lowered_opcode == GPU_IR_SHADER_OP_TERMINATE_INVOCATION)
                    blocks[current_block].terminator_opcode = lowered_opcode;
            }
        }
        if (next < 0 || parameters != source_function->parameter_count ||
            block_in_function != source_function->block_count) { rc = GPU_IR_INVALID; break; }
        function->instruction_count = instruction_count - function->first_instruction;
    }

    if (rc == GPU_IR_OK) {
        gpu_ir_shader_desc desc = {
            .source_api = GPU_IR_SHADER_SOURCE_SPIRV,
            .stage_mask = stage,
            .entry_function_id = entry_point->function_id,
            .threadgroup_size = { threadgroup_size[0], threadgroup_size[1], threadgroup_size[2] },
            .types = types, .type_count = type_count,
            .type_members = type_members, .type_member_count = type_member_count,
            .functions = functions, .function_count = function_count,
            .blocks = blocks, .block_count = block_count,
            .instructions = instructions, .instruction_count = instruction_count,
            .operands = operands, .operand_count = operand_count,
            .literal_words = literal_words, .literal_word_count = literal_count
        };
        *shader = gpu_ir_shader_create(&desc);
        if (!*shader) rc = GPU_IR_INVALID;
    }
    free(types); free(type_members); free(functions); free(blocks); free(instructions); free(operands); free(literal_words);
    free(included_functions); free(function_map); free(call_stack);
    return rc;
}
