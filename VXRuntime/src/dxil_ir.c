// A conservative DXIL adapter for the shared shader IR. Resource operations outside typed R32 buffer views,
// 32-bit integer/float raw-buffer loads and stores, narrow float 2D dimension queries, 32-bit scalar 2D mip reads
// and other explicit common forms remain on dxil_to_msl.
#include "dxil_ir.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    uint8_t present, no_op;
    uint32_t operation, execution_scope, memory_scope, semantics;
    uint32_t execution_scope_id, memory_scope_id, semantics_id;
} dxil_ir_barrier;

typedef struct {
    uint8_t present;
    uint32_t scope_id, semantics_id;
} dxil_ir_atomic;

typedef struct {
    uint8_t present, mask, is_float;
    uint32_t base_word_index_id;
    uint32_t access_ids[4], word_index_ids[4], value_ids[4];
} dxil_ir_raw_store;

typedef struct {
    uint8_t present, mask;
    uint32_t signature_id, block_index, last_instruction;
    uint32_t values[4];
    uint32_t scalar_type_id, vector_type_id, pointer_type_id;
    uint32_t variable_id, composite_id;
} dxil_ir_color_output;

typedef struct {
    uint8_t present, mask;
    uint32_t signature_id, block_index, last_instruction;
    uint32_t values[4];
    uint32_t scalar_type_id, vector_type_id, pointer_type_id;
    uint32_t variable_id, composite_id;
} dxil_ir_vertex_position_output;

typedef struct {
    uint8_t present;
    uint32_t signature_id, scalar_type_id;
    uint32_t vector_type_id, pointer_type_id, variable_id;
} dxil_ir_fragment_position;

typedef struct {
    uint8_t present;
    uint32_t signature_id, scalar_type_id;
    uint32_t pointer_type_id, variable_id;
} dxil_ir_front_facing;

typedef struct {
    uint8_t present, columns;
    uint32_t signature_id, location, scalar_type_id;
    uint32_t vector_type_id, pointer_type_id, variable_id;
} dxil_ir_vertex_input;

typedef struct {
    uint8_t present;
    uint32_t signature_id, scalar_type_id, semantic_kind, builtin;
    uint32_t pointer_type_id, variable_id;
} dxil_ir_vertex_system_value;

enum { DXIL_IR_MAX_VERTEX_INPUTS = 32 };
enum { DXIL_IR_VERTEX_SYSTEM_VALUE_COUNT = 2 };

enum {
    DXIL_IR_RESOURCE_HANDLE = 1,
    DXIL_IR_RESOURCE_BUFFER_LOAD,
    DXIL_IR_RESOURCE_GET_DIMENSIONS,
    DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS,
    DXIL_IR_RESOURCE_IMAGE_LOAD
};

enum {
    DXIL_IR_COMPONENT_I1 = 1,
    DXIL_IR_COMPONENT_S32 = 4,
    DXIL_IR_COMPONENT_U32 = 5,
    DXIL_IR_COMPONENT_F32 = 9
};
enum { DXIL_IR_SEMANTIC_IS_FRONT_FACE = 13 };
enum {
    DXIL_IR_SEMANTIC_VERTEX_ID = 1,
    DXIL_IR_SEMANTIC_INSTANCE_ID = 2
};

typedef struct {
    uint8_t register_class, valid_metadata, used, structured_dimensions, structured_loads;
    uint32_t range_id, set, binding, count, kind, component_type, structure_stride, variable_id;
    uint32_t structure_stride_words_constant_id;
    uint32_t image_type_id, image_pointer_type_id, image_value_id;
} dxil_ir_resource;

typedef struct {
    uint8_t kind;
    uint32_t resource_index, index_value_id, buffer_load_opcode, buffer_component_mask;
    uint32_t mip_level_value_id, sample_index_value_id;
    uint32_t x_value_id, y_value_id, layer_value_id;
    uint32_t image_value_id;
    uint32_t structured_byte_offset;
    int32_t offset_x, offset_y;
} dxil_ir_resource_value;

static int resource_is_common_float2d_array_srv(const dxil_ir_resource *resource);
static int resource_is_common_image2d_srv(const dxil_ir_resource *resource);
static int resource_is_common_raw_buffer(const dxil_ir_resource *resource);
static int resource_is_common_structured_buffer(const dxil_ir_resource *resource);

typedef struct {
    const dxil_module *module;
    const dxil_function *function;
    uint32_t function_index;
    uint8_t *needed_types, *type_state, *needed_values;
    uint32_t *type_ids;
    uint32_t value_count, value_base, label_base, common_stage;
    uint32_t builtin_mask, builtin_scalar_type;
    uint32_t barrier_integer_type;
    uint32_t builtin_vector_type_id, builtin_vector_pointer_type_id, builtin_scalar_pointer_type_id;
    uint32_t buffer_array_type_id, buffer_block_type_id, buffer_block_pointer_type_id;
    uint32_t buffer_scalar_pointer_type_id, buffer_index_constant_id, buffer_byte_shift_constant_id;
    uint32_t buffer_byte_length_word_constant_id;
    uint32_t buffer_byte_lane_constant_ids[4];
    uint32_t image_query_size_type_id, image_s32_type_id;
    uint32_t synthetic_type_count, synthetic_value_count;
    uint32_t builtin_variable_ids[6], builtin_load_ids[6];
    dxil_ir_color_output color_output;
    dxil_ir_vertex_position_output vertex_position_output;
    dxil_ir_fragment_position fragment_position;
    dxil_ir_front_facing front_facing;
    dxil_ir_vertex_input vertex_inputs[DXIL_IR_MAX_VERTEX_INPUTS];
    uint32_t vertex_input_count;
    dxil_ir_vertex_system_value vertex_system_values[DXIL_IR_VERTEX_SYSTEM_VALUE_COUNT];
    uint32_t *input_load_ids;
    dxil_ir_barrier *barriers;
    dxil_ir_atomic *atomics;
    dxil_ir_raw_store *raw_stores;
    gpu_ir_shader_type *types;
    uint32_t type_count;
    gpu_ir_shader_member *members;
    uint32_t member_count;
    gpu_ir_shader_function function_record;
    gpu_ir_shader_block *blocks;
    uint32_t block_count;
    gpu_ir_shader_instruction *instructions;
    uint32_t instruction_count, instruction_capacity;
    gpu_ir_shader_operand *operands;
    uint32_t operand_count, operand_capacity;
    uint32_t *literals;
    uint32_t literal_count, literal_capacity;
    dxil_ir_resource resources[64];
    uint32_t resource_count, resource_u32_source_type, resource_f32_source_type;
    uint32_t *handle_resource_indices, *resource_access_ids, *resource_typed_value_ids;
    uint32_t *resource_byte_index_ids, *resource_byte_base_index_ids, *resource_raw_length_word_ids;
    uint32_t *resource_structured_length_word_ids;
    uint32_t *resource_structured_base_word_index_ids, *resource_structured_word_index_ids;
    uint32_t *resource_structured_offset_word_constant_ids;
    dxil_ir_resource_value *resource_values;
    uint32_t threadgroup_size[3];
} dxil_ir_builder;

enum {
    DXIL_IR_BUILTIN_THREAD_ID,
    DXIL_IR_BUILTIN_GROUP_ID,
    DXIL_IR_BUILTIN_THREAD_ID_IN_GROUP,
    DXIL_IR_BUILTIN_FLATTENED_THREAD_ID_IN_GROUP,
    DXIL_IR_BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID,
    DXIL_IR_BUILTIN_SUBGROUP_SIZE,
    DXIL_IR_BUILTIN_COUNT
};

static int value_i32(const dxil_ir_builder *builder, uint32_t id, uint32_t *out);
static int call_signature_valid(const dxil_ir_builder *builder, const dxil_inst *instruction);
static int analyze_dxil_input_load(dxil_ir_builder *builder, const dxil_inst *instruction,
                                   uint32_t instruction_index);
static uint32_t dxil_atomic_operation(uint32_t operation);
static int analyze_resource_instructions(dxil_ir_builder *builder);
static int append_synthetic_u32_constant(dxil_ir_builder *builder, uint32_t result_id, uint32_t source_type,
                                         uint32_t value);
static int dxil_system_value_call(dxil_ir_builder *builder, const dxil_inst *instruction,
                                  uint32_t *builtin_index, uint32_t *component, uint32_t *dxil_opcode);
static int dxil_wave_vote_call(const dxil_ir_builder *builder, const dxil_inst *instruction,
                               uint32_t *operation, uint32_t *source_value);
static int dxil_wave_integer_minmax_call(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                         uint32_t *operation, uint32_t *source_value);
static int dxil_wave_integer_bit_call(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                      uint32_t *operation, uint32_t *source_value);
static int dxil_barrier_call(dxil_ir_builder *builder, const dxil_inst *instruction,
                             dxil_ir_barrier *barrier, uint32_t *integer_type);
static int resource_type_matches_component(const dxil_ir_resource *resource, const dxil_type *type);
static int resource_is_common_float2d_ms_srv(const dxil_ir_resource *resource);
static int dxil_bit_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                   uint32_t *operation, uint32_t source_values[4], uint32_t *source_value_count);
static int dxil_integer_minmax_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                              uint32_t *operation, uint32_t *flags,
                                              uint32_t source_values[2]);
static int dxil_integer_mad_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                            uint32_t *flags, uint32_t source_values[3]);
static int dxil_float32_shape(const dxil_module *module, uint32_t type_id, int allow_vector);
static int dxil_float_mad_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                          uint32_t *flags, uint32_t source_values[3]);
static int dxil_float_minmax_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                             uint32_t *operation, uint32_t *flags,
                                             uint32_t source_values[2]);
static int dxil_unary_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                     uint32_t *common_opcode, uint32_t *operation,
                                     uint32_t *common_flags, uint32_t *source_value);

static uint32_t dxil_ir_stage(uint32_t kind) {
    switch (kind) {
    case 0: return GPU_IR_STAGE_FRAGMENT;
    case 1: return GPU_IR_STAGE_VERTEX;
    case 2: return GPU_IR_STAGE_GEOMETRY;
    case 3: return GPU_IR_STAGE_TESS_CONTROL;
    case 4: return GPU_IR_STAGE_TESS_EVALUATION;
    case 5: return GPU_IR_STAGE_COMPUTE;
    case 7: return GPU_IR_STAGE_MESH;
    case 8: return GPU_IR_STAGE_TASK;
    default: return 0;
    }
}

static const dxil_type *source_type(const dxil_module *module, uint32_t id) {
    return module && id < module->ntypes ? &module->types[id] : NULL;
}

static int mark_type(dxil_ir_builder *builder, uint32_t id, uint32_t depth) {
    const dxil_type *type = source_type(builder->module, id);
    if (!type || depth > builder->module->ntypes) return GPU_IR_INVALID;
    if (builder->type_state[id] == 2) return GPU_IR_OK;
    if (builder->type_state[id] == 1) return GPU_IR_UNSUPPORTED;
    builder->type_state[id] = 1;
    int rc = GPU_IR_OK;
    switch (type->kind) {
    case DXT_VOID: break;
    case DXT_INT:
        if (!type->bits || type->bits > 64) rc = GPU_IR_UNSUPPORTED;
        break;
    case DXT_HALF: case DXT_FLOAT: case DXT_DOUBLE: break;
    case DXT_VECTOR:
        if (type->count < 2 || !type->elem) rc = GPU_IR_UNSUPPORTED;
        else rc = mark_type(builder, type->elem, depth + 1);
        break;
    case DXT_POINTER:
        if (!source_type(builder->module, type->elem)) rc = GPU_IR_UNSUPPORTED;
        else rc = mark_type(builder, type->elem, depth + 1);
        break;
    case DXT_FUNCTION:
        if (type->vararg || !source_type(builder->module, type->elem) || (type->count && !type->members))
            rc = GPU_IR_UNSUPPORTED;
        if (rc == GPU_IR_OK) rc = mark_type(builder, type->elem, depth + 1);
        for (uint32_t i = 0; rc == GPU_IR_OK && i < type->count; i++)
            rc = mark_type(builder, type->members[i], depth + 1);
        break;
    default: rc = GPU_IR_UNSUPPORTED; break;
    }
    if (rc == GPU_IR_OK) {
        builder->needed_types[id] = 1;
        builder->type_state[id] = 2;
    } else builder->type_state[id] = 0;
    return rc;
}

static int mark_value(dxil_ir_builder *builder, uint32_t id) {
    if (id >= builder->value_count) return GPU_IR_INVALID;
    if (builder->needed_values[id]) return GPU_IR_OK;
    const dxil_value *value = dxil_value_at(builder->module, builder->function, id);
    if (!value) return GPU_IR_INVALID;
    switch (value->kind) {
    case DXV_CONST_INT: case DXV_CONST_FLOAT: case DXV_CONST_NULL:
        break;
    case DXV_INST:
        if (id < builder->module->nmodule_values || value->u.index >= builder->function->ninsts)
            return GPU_IR_INVALID;
        break;
    case DXV_ARG:
        if (id < builder->module->nmodule_values) return GPU_IR_INVALID;
        break;
    default:
        return GPU_IR_UNSUPPORTED;
    }
    int rc = mark_type(builder, value->type, 0);
    if (rc == GPU_IR_OK) builder->needed_values[id] = 1;
    return rc;
}

static int buffer_load_field_supported(const dxil_ir_resource_value *resource_value,
                                       const dxil_ir_resource *resource, uint32_t field) {
    if (!resource_value || resource_value->kind != DXIL_IR_RESOURCE_BUFFER_LOAD) return 0;
    if (resource_value->buffer_load_opcode == 139)
        return field < 4 && (resource_value->buffer_component_mask & (1u << field)) != 0;
    if (resource_is_common_structured_buffer(resource))
        return field < 4 && resource_value->structured_byte_offset <= resource->structure_stride - 4u &&
            field <= (resource->structure_stride - 4u - resource_value->structured_byte_offset) / 4u;
    if (resource_value->buffer_load_opcode == 68 && resource_is_common_raw_buffer(resource))
        return field < 4;
    return field == 0;
}

static int dxil_discard_condition(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                 uint32_t *condition_id) {
    if (!builder || !instruction || !condition_id || instruction->op != DXI_CALL ||
        instruction->n < 2 || !instruction->ops) return GPU_IR_NOT_FOUND;
    const char *callee_name = dxil_value_name(builder->module, instruction->ops[0]);
    if (!callee_name || strcmp(callee_name, "dx.op.discard")) return GPU_IR_NOT_FOUND;
    if (instruction->n != 3 || instruction->nvals || instruction->flags ||
        instruction->result != UINT32_MAX ||
        !source_type(builder->module, instruction->type) ||
        source_type(builder->module, instruction->type)->kind != DXT_VOID ||
        dxil_dxop(builder->module, builder->function, instruction) != 82)
        return GPU_IR_INVALID;

    const dxil_value *callee = dxil_value_at(builder->module, NULL, instruction->ops[0]);
    const dxil_type *function_type = callee ? source_type(builder->module, callee->type) : NULL;
    if (!callee || callee->kind != DXV_FUNCTION) return GPU_IR_INVALID;
    if (function_type && function_type->kind == DXT_POINTER)
        function_type = source_type(builder->module, function_type->elem);
    if (!function_type || function_type->kind != DXT_FUNCTION || function_type->elem != instruction->type ||
        function_type->vararg || function_type->count != 2 || !function_type->members)
        return GPU_IR_INVALID;
    const dxil_type *opcode_type = source_type(builder->module, function_type->members[0]);
    const dxil_type *condition_type = source_type(builder->module, function_type->members[1]);
    const dxil_value *opcode_value = dxil_value_at(builder->module, builder->function, instruction->ops[1]);
    const dxil_value *condition_value = dxil_value_at(builder->module, builder->function, instruction->ops[2]);
    if (!opcode_type || opcode_type->kind != DXT_INT || opcode_type->bits != 32 ||
        !condition_type || condition_type->kind != DXT_INT || condition_type->bits != 1 ||
        !opcode_value || opcode_value->kind != DXV_CONST_INT || opcode_value->type != function_type->members[0] ||
        opcode_value->u.i != 82 || !condition_value || condition_value->type != function_type->members[1])
        return GPU_IR_INVALID;
    if (builder->common_stage != GPU_IR_STAGE_FRAGMENT) return GPU_IR_UNSUPPORTED;
    *condition_id = instruction->ops[2];
    return GPU_IR_OK;
}

static int mark_instruction_values(dxil_ir_builder *builder, const dxil_inst *instruction) {
    uint32_t value_indexes[4], value_count = 0;
    switch (instruction->op) {
    case DXI_BINOP: case DXI_CMP:
        if (instruction->n != 2) return GPU_IR_INVALID;
        value_indexes[value_count++] = 0; value_indexes[value_count++] = 1;
        break;
    case DXI_CAST:
        if (instruction->n != 1) return GPU_IR_INVALID;
        value_indexes[value_count++] = 0;
        break;
    case DXI_SELECT:
        if (instruction->n != 3) return GPU_IR_INVALID;
        value_indexes[value_count++] = 0; value_indexes[value_count++] = 1; value_indexes[value_count++] = 2;
        break;
    case DXI_EXTRACTELT:
        if (instruction->n != 2) return GPU_IR_INVALID;
        value_indexes[value_count++] = 0; value_indexes[value_count++] = 1;
        break;
    case DXI_INSERTELT:
        if (instruction->n != 3) return GPU_IR_INVALID;
        value_indexes[value_count++] = 0; value_indexes[value_count++] = 1; value_indexes[value_count++] = 2;
        break;
    case DXI_SHUFFLEVEC:
        if (instruction->n != 3) return GPU_IR_INVALID;
        value_indexes[value_count++] = 0; value_indexes[value_count++] = 1;
        break;
    case DXI_EXTRACTVAL:
        if (instruction->nvals != 1 || instruction->n < 2) return GPU_IR_UNSUPPORTED;
        if (instruction->ops[0] < builder->value_count) {
            const dxil_ir_resource_value *resource_value = &builder->resource_values[instruction->ops[0]];
            if (resource_value->kind == DXIL_IR_RESOURCE_BUFFER_LOAD ||
                resource_value->kind == DXIL_IR_RESOURCE_GET_DIMENSIONS ||
                resource_value->kind == DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS ||
                resource_value->kind == DXIL_IR_RESOURCE_IMAGE_LOAD) {
                const dxil_type *result_type = source_type(builder->module, instruction->type);
                if (resource_value->resource_index >= builder->resource_count || instruction->n != 2 ||
                    !result_type) return GPU_IR_UNSUPPORTED;
                const dxil_ir_resource *resource = &builder->resources[resource_value->resource_index];
                uint32_t field = instruction->ops[1];
                if ((resource_value->kind == DXIL_IR_RESOURCE_BUFFER_LOAD &&
                     !buffer_load_field_supported(resource_value, resource, field)) ||
                    (resource_value->kind == DXIL_IR_RESOURCE_GET_DIMENSIONS && field != 0) ||
                    (resource_value->kind == DXIL_IR_RESOURCE_IMAGE_LOAD && field > 3) ||
                    (resource_value->kind == DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS &&
                     ((field != 0 && field != 1 && field != 2 && field != 3) ||
                      (field == 2 && !resource_is_common_float2d_array_srv(resource)))))
                    return GPU_IR_UNSUPPORTED;
                int structured_buffer_load = resource_value->kind == DXIL_IR_RESOURCE_BUFFER_LOAD &&
                    resource_is_common_structured_buffer(resource);
                int result_matches = resource_value->kind == DXIL_IR_RESOURCE_GET_DIMENSIONS ?
                    result_type->kind == DXT_INT && result_type->bits == 32 :
                    resource_value->kind == DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS ?
                        result_type->kind == DXT_INT && result_type->bits == 32 :
                        structured_buffer_load ?
                            (result_type->kind == DXT_FLOAT ||
                             (result_type->kind == DXT_INT && result_type->bits == 32)) :
                        resource_type_matches_component(resource, result_type);
                if (!result_matches) return GPU_IR_UNSUPPORTED;
                if (resource_value->kind == DXIL_IR_RESOURCE_BUFFER_LOAD) {
                    int rc = mark_value(builder, resource_value->index_value_id);
                    if (rc != GPU_IR_OK) return rc;
                }
                if (resource_value->kind == DXIL_IR_RESOURCE_IMAGE_LOAD) {
                    const dxil_ir_resource *image = &builder->resources[resource_value->resource_index];
                    int multisampled = image->kind == 3 || image->kind == 4;
                    int rc = multisampled ? mark_value(builder, resource_value->sample_index_value_id) :
                        mark_value(builder, resource_value->mip_level_value_id);
                    if (rc == GPU_IR_OK) rc = mark_value(builder, resource_value->x_value_id);
                    if (rc == GPU_IR_OK) rc = mark_value(builder, resource_value->y_value_id);
                    if (rc == GPU_IR_OK && (image->kind == 4 || image->kind == 7))
                        rc = mark_value(builder, resource_value->layer_value_id);
                    if (rc != GPU_IR_OK) return rc;
                }
                if (instruction->result == UINT32_MAX || instruction->result >= builder->value_count)
                    return GPU_IR_INVALID;
                return mark_value(builder, instruction->result);
            }
        }
        value_indexes[value_count++] = 0;
        break;
    case DXI_INSERTVAL:
        if (instruction->nvals != 2 || instruction->n < 3) return GPU_IR_UNSUPPORTED;
        value_indexes[value_count++] = 0; value_indexes[value_count++] = 1;
        break;
    case DXI_PHI:
        if (!instruction->n || (instruction->n & 1u)) return GPU_IR_INVALID;
        for (uint32_t i = 0; i < instruction->n; i += 2) {
            int rc = mark_value(builder, instruction->ops[i]);
            if (rc != GPU_IR_OK) return rc;
        }
        break;
    case DXI_BR:
        if (instruction->n == 1) break;
        if (instruction->n != 3) return GPU_IR_INVALID;
        value_indexes[value_count++] = 2;
        break;
    case DXI_SWITCH:
        if (instruction->n < 2 || (instruction->n & 1u)) return GPU_IR_INVALID;
        value_indexes[value_count++] = 0;
        break;
    case DXI_RET:
        if (instruction->n > 1) return GPU_IR_INVALID;
        if (instruction->n) value_indexes[value_count++] = 0;
        break;
    case DXI_UNREACHABLE:
        if (instruction->n) return GPU_IR_INVALID;
        break;
    case DXI_CALL: {
        uint32_t discard_condition;
        int discard_rc = dxil_discard_condition(builder, instruction, &discard_condition);
        if (discard_rc != GPU_IR_NOT_FOUND) {
            if (discard_rc != GPU_IR_OK) return discard_rc;
            return mark_value(builder, discard_condition);
        }
        int opcode = dxil_dxop(builder->module, builder->function, instruction);
        uint32_t vote_operation, vote_value;
        int vote_rc = dxil_wave_vote_call(builder, instruction, &vote_operation, &vote_value);
        if (vote_rc != GPU_IR_NOT_FOUND) {
            if (vote_rc != GPU_IR_OK) return vote_rc;
            int rc = mark_value(builder, vote_value);
            if (rc == GPU_IR_OK) rc = mark_value(builder, instruction->result);
            return rc;
        }
        uint32_t wave_operation, wave_value;
        int wave_rc = dxil_wave_integer_minmax_call(builder, instruction, &wave_operation, &wave_value);
        if (wave_rc != GPU_IR_NOT_FOUND) {
            if (wave_rc != GPU_IR_OK) return wave_rc;
            int rc = mark_value(builder, wave_value);
            if (rc == GPU_IR_OK) rc = mark_value(builder, instruction->result);
            return rc;
        }
        uint32_t wave_bit_operation, wave_bit_value;
        int wave_bit_rc = dxil_wave_integer_bit_call(builder, instruction,
                                                     &wave_bit_operation, &wave_bit_value);
        if (wave_bit_rc != GPU_IR_NOT_FOUND) {
            if (wave_bit_rc != GPU_IR_OK) return wave_bit_rc;
            int rc = mark_value(builder, wave_bit_value);
            if (rc == GPU_IR_OK) rc = mark_value(builder, instruction->result);
            return rc;
        }
        if (opcode == 4) {
            ptrdiff_t instruction_index = instruction - builder->function->insts;
            if (instruction_index < 0 || (uint64_t)instruction_index >= builder->function->ninsts)
                return GPU_IR_INVALID;
            return analyze_dxil_input_load(builder, instruction, (uint32_t)instruction_index);
        }
        if (opcode == 5) {
            if ((!builder->color_output.present && !builder->vertex_position_output.present) ||
                instruction->n != 6 || instruction->flags ||
                instruction->nvals || instruction->result != UINT32_MAX) return GPU_IR_UNSUPPORTED;
            int rc = call_signature_valid(builder, instruction);
            if (rc != GPU_IR_OK) return rc;
            return mark_value(builder, instruction->ops[5]);
        }
        uint32_t common_opcode, operation, flags, source_value;
        uint32_t float_minmax_operation, float_minmax_flags, float_minmax_values[2];
        int float_minmax_rc = dxil_float_minmax_intrinsic_info(builder, instruction, &float_minmax_operation,
                                                                &float_minmax_flags, float_minmax_values);
        if (float_minmax_rc != GPU_IR_NOT_FOUND) {
            if (float_minmax_rc != GPU_IR_OK) return float_minmax_rc;
            int rc = mark_value(builder, float_minmax_values[0]);
            if (rc == GPU_IR_OK) rc = mark_value(builder, float_minmax_values[1]);
            if (rc == GPU_IR_OK) rc = mark_value(builder, instruction->result);
            return rc;
        }
        uint32_t minmax_operation, minmax_flags, minmax_values[2];
        int minmax_rc = dxil_integer_minmax_intrinsic_info(builder, instruction, &minmax_operation,
                                                            &minmax_flags, minmax_values);
        if (minmax_rc != GPU_IR_NOT_FOUND) {
            if (minmax_rc != GPU_IR_OK) return minmax_rc;
            int rc = mark_value(builder, minmax_values[0]);
            if (rc == GPU_IR_OK) rc = mark_value(builder, minmax_values[1]);
            if (rc == GPU_IR_OK) rc = mark_value(builder, instruction->result);
            return rc;
        }
        uint32_t mad_flags, mad_values[3];
        int mad_rc = dxil_integer_mad_intrinsic_info(builder, instruction, &mad_flags, mad_values);
        if (mad_rc != GPU_IR_NOT_FOUND) {
            if (mad_rc != GPU_IR_OK) return mad_rc;
            for (uint32_t i = 0; i < 3; i++) {
                int rc = mark_value(builder, mad_values[i]);
                if (rc != GPU_IR_OK) return rc;
            }
            return mark_value(builder, instruction->result);
        }
        uint32_t float_mad_flags, float_mad_values[3];
        int float_mad_rc = dxil_float_mad_intrinsic_info(builder, instruction,
            &float_mad_flags, float_mad_values);
        if (float_mad_rc != GPU_IR_NOT_FOUND) {
            if (float_mad_rc != GPU_IR_OK) return float_mad_rc;
            for (uint32_t i = 0; i < 3; i++) {
                int rc = mark_value(builder, float_mad_values[i]);
                if (rc != GPU_IR_OK) return rc;
            }
            return mark_value(builder, instruction->result);
        }
        int unary_rc = dxil_unary_intrinsic_info(builder, instruction, &common_opcode, &operation,
                                                  &flags, &source_value);
        if (unary_rc != GPU_IR_NOT_FOUND) {
            if (unary_rc != GPU_IR_OK) return unary_rc;
            int rc = mark_value(builder, source_value);
            if (rc == GPU_IR_OK) rc = mark_value(builder, instruction->result);
            return rc;
        }
        uint32_t bit_operation, bit_source_values[4], bit_source_value_count = 0;
        int bit_rc = dxil_bit_intrinsic_info(builder, instruction, &bit_operation,
                                             bit_source_values, &bit_source_value_count);
        if (bit_rc != GPU_IR_NOT_FOUND) {
            if (bit_rc != GPU_IR_OK) return bit_rc;
            for (uint32_t i = 0; i < bit_source_value_count; i++) {
                int rc = mark_value(builder, bit_source_values[i]);
                if (rc != GPU_IR_OK) return rc;
            }
            return mark_value(builder, instruction->result);
        }
        if (opcode == 57) {
            if (instruction->result >= builder->value_count ||
                builder->resource_values[instruction->result].kind != DXIL_IR_RESOURCE_HANDLE)
                return GPU_IR_INVALID;
            return GPU_IR_OK;
        }
        if (opcode == 66) {
            if (instruction->result >= builder->value_count ||
                builder->resource_values[instruction->result].kind != DXIL_IR_RESOURCE_IMAGE_LOAD)
                return GPU_IR_INVALID;
            return GPU_IR_OK;
        }
        if (opcode == 68 || opcode == 139 || opcode == 72) {
            if (instruction->result >= builder->value_count) return GPU_IR_INVALID;
            const dxil_ir_resource_value *resource_value = &builder->resource_values[instruction->result];
            if (((opcode == 68 || opcode == 139) && resource_value->kind != DXIL_IR_RESOURCE_BUFFER_LOAD) ||
                (opcode == 72 && resource_value->kind != DXIL_IR_RESOURCE_GET_DIMENSIONS &&
                 resource_value->kind != DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS)) return GPU_IR_INVALID;
            if (opcode == 68 || opcode == 139) return mark_value(builder, resource_value->index_value_id);
            if (opcode == 72 && resource_value->kind == DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS &&
                !resource_is_common_float2d_ms_srv(&builder->resources[resource_value->resource_index]))
                return mark_value(builder, resource_value->mip_level_value_id);
            return GPU_IR_OK;
        }
        if (opcode == 78 || opcode == 79) {
            ptrdiff_t index = instruction - builder->function->insts;
            if (index < 0 || (uint64_t)index >= builder->function->ninsts || !builder->atomics ||
                !builder->atomics[index].present || instruction->n != 8) return GPU_IR_INVALID;
            int rc = mark_value(builder, instruction->ops[opcode == 78 ? 4u : 3u]);
            if (rc == GPU_IR_OK && opcode == 79) rc = mark_value(builder, instruction->ops[6]);
            if (rc == GPU_IR_OK) rc = mark_value(builder, instruction->ops[7]);
            if (rc == GPU_IR_OK) rc = mark_value(builder, instruction->result);
            return rc;
        }
        if (opcode == 69) {
            if (!instruction->ops || instruction->n != 10) return GPU_IR_INVALID;
            ptrdiff_t index = instruction - builder->function->insts;
            if (index < 0 || (uint64_t)index >= builder->function->ninsts) return GPU_IR_INVALID;
            if (!builder->resource_access_ids[index]) return GPU_IR_OK;
            int rc = mark_value(builder, instruction->ops[3]);
            if (rc == GPU_IR_OK) rc = mark_value(builder, instruction->ops[5]);
            return rc;
        }
        if (opcode == 140) {
            if (!instruction->ops || instruction->n != 11) return GPU_IR_INVALID;
            ptrdiff_t index = instruction - builder->function->insts;
            if (index < 0 || (uint64_t)index >= builder->function->ninsts ||
                !builder->raw_stores[index].present) return GPU_IR_INVALID;
            int rc = mark_value(builder, instruction->ops[3]);
            for (uint32_t component = 0; rc == GPU_IR_OK && component < 4; component++)
                if (builder->raw_stores[index].mask & (1u << component))
                    rc = mark_value(builder, instruction->ops[5u + component]);
            return rc;
        }
        uint32_t builtin_index, component, dxil_opcode;
        int rc = dxil_system_value_call(builder, instruction, &builtin_index, &component, &dxil_opcode);
        if (rc == GPU_IR_OK) {
            if (builder->builtin_scalar_type != UINT32_MAX && builder->builtin_scalar_type != instruction->type)
                return GPU_IR_UNSUPPORTED;
            builder->builtin_scalar_type = instruction->type;
            builder->builtin_mask |= 1u << builtin_index;
        } else if (rc == GPU_IR_UNSUPPORTED) {
            ptrdiff_t index = instruction - builder->function->insts;
            uint32_t integer_type = UINT32_MAX;
            if (index < 0 || (uint64_t)index >= builder->function->ninsts || !builder->barriers)
                return GPU_IR_INVALID;
            rc = dxil_barrier_call(builder, instruction, &builder->barriers[index], &integer_type);
            if (rc != GPU_IR_OK) return rc;
            if (!builder->barriers[index].no_op) {
                if (builder->barrier_integer_type != UINT32_MAX && builder->barrier_integer_type != integer_type)
                    return GPU_IR_UNSUPPORTED;
                builder->barrier_integer_type = integer_type;
                rc = mark_type(builder, integer_type, 0);
                if (rc != GPU_IR_OK) return rc;
            }
        } else return rc;
        break;
    }
    default:
        return GPU_IR_UNSUPPORTED;
    }
    for (uint32_t i = 0; i < value_count; i++) {
        int rc = mark_value(builder, instruction->ops[value_indexes[i]]);
        if (rc != GPU_IR_OK) return rc;
    }
    if (instruction->result != UINT32_MAX) {
        if (instruction->result >= builder->value_count) return GPU_IR_INVALID;
        const dxil_value *result = dxil_value_at(builder->module, builder->function, instruction->result);
        if (!result || result->kind != DXV_INST || result->type != instruction->type ||
            result->u.index >= builder->function->ninsts) return GPU_IR_INVALID;
        int rc = mark_type(builder, instruction->type, 0);
        if (rc != GPU_IR_OK) return rc;
    }
    return GPU_IR_OK;
}

static uint32_t common_type_id(const dxil_ir_builder *builder, uint32_t source_id) {
    return source_id < builder->module->ntypes ? builder->type_ids[source_id] : 0;
}

static uint32_t common_value_id(const dxil_ir_builder *builder, uint32_t source_id) {
    return source_id < builder->value_count ? builder->value_base + source_id : 0;
}

static uint32_t common_label_id(const dxil_ir_builder *builder, uint32_t source_block) {
    return source_block < builder->block_count ? builder->label_base + source_block : 0;
}

static int append_operand(dxil_ir_builder *builder, uint32_t kind, uint32_t value) {
    if (builder->operand_count == UINT32_MAX) return GPU_IR_RANGE;
    if (builder->operand_count == builder->operand_capacity) {
        uint32_t capacity = builder->operand_capacity ? builder->operand_capacity * 2u : 32u;
        if (capacity < builder->operand_capacity || (size_t)capacity > SIZE_MAX / sizeof *builder->operands)
            return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_shader_operand *replacement = realloc(builder->operands, (size_t)capacity * sizeof *replacement);
        if (!replacement) return GPU_IR_OUT_OF_MEMORY;
        builder->operands = replacement;
        builder->operand_capacity = capacity;
    }
    builder->operands[builder->operand_count++] = (gpu_ir_shader_operand){ kind, value };
    return GPU_IR_OK;
}

static int append_literal(dxil_ir_builder *builder, uint32_t value) {
    if (builder->literal_count == UINT32_MAX) return GPU_IR_RANGE;
    if (builder->literal_count == builder->literal_capacity) {
        uint32_t capacity = builder->literal_capacity ? builder->literal_capacity * 2u : 16u;
        if (capacity < builder->literal_capacity || (size_t)capacity > SIZE_MAX / sizeof *builder->literals)
            return GPU_IR_OUT_OF_MEMORY;
        uint32_t *replacement = realloc(builder->literals, (size_t)capacity * sizeof *replacement);
        if (!replacement) return GPU_IR_OUT_OF_MEMORY;
        builder->literals = replacement;
        builder->literal_capacity = capacity;
    }
    builder->literals[builder->literal_count++] = value;
    return GPU_IR_OK;
}

static int append_instruction(dxil_ir_builder *builder, gpu_ir_shader_instruction *instruction) {
    if (builder->instruction_count == UINT32_MAX) return GPU_IR_RANGE;
    if (builder->instruction_count == builder->instruction_capacity) {
        uint32_t capacity = builder->instruction_capacity ? builder->instruction_capacity * 2u : 32u;
        if (capacity < builder->instruction_capacity ||
            (size_t)capacity > SIZE_MAX / sizeof *builder->instructions) return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_shader_instruction *replacement = realloc(builder->instructions,
            (size_t)capacity * sizeof *replacement);
        if (!replacement) return GPU_IR_OUT_OF_MEMORY;
        builder->instructions = replacement;
        builder->instruction_capacity = capacity;
    }
    builder->instructions[builder->instruction_count++] = *instruction;
    return GPU_IR_OK;
}

static uint32_t value_scalar_kind(const dxil_module *module, uint32_t type_id) {
    const dxil_type *type = source_type(module, type_id);
    if (type && type->kind == DXT_VECTOR) type = source_type(module, type->elem);
    return type ? type->kind : DXT_OTHER;
}

static const dxil_type *numeric_scalar_type(const dxil_module *module, const dxil_type *type,
                                            uint32_t *component_count) {
    if (!module || !type || !component_count) return NULL;
    *component_count = 1;
    if (type->kind == DXT_VECTOR) {
        if (type->count < 2 || !type->elem) return NULL;
        *component_count = type->count;
        type = source_type(module, type->elem);
    }
    if (!type || (type->kind != DXT_INT && type->kind != DXT_HALF &&
                  type->kind != DXT_FLOAT && type->kind != DXT_DOUBLE)) return NULL;
    return type;
}

static int append_fast_math_hint(uint32_t dxil_flags, uint32_t *common_flags) {
    // LLVM bitcode's FastMathMap is an eight-bit set of optional optimization permissions.
    if (!common_flags || (dxil_flags & ~0xffu)) return GPU_IR_UNSUPPORTED;
    if (dxil_flags) *common_flags |= GPU_IR_SHADER_FLAG_FAST_MATH;
    return GPU_IR_OK;
}

static int value_i32(const dxil_ir_builder *builder, uint32_t id, uint32_t *out) {
    const dxil_value *value = dxil_value_at(builder->module, builder->function, id);
    const dxil_type *type = value ? source_type(builder->module, value->type) : NULL;
    if (!value || value->kind != DXV_CONST_INT || !type || type->kind != DXT_INT ||
        !type->bits || type->bits > 32) return 0;
    *out = (uint32_t)value->u.i;
    return 1;
}

static int value_i32_or_null(const dxil_ir_builder *builder, uint32_t id, uint32_t *out) {
    const dxil_value *value = dxil_value_at(builder->module, builder->function, id);
    const dxil_type *type = value ? source_type(builder->module, value->type) : NULL;
    if (!value || !type || type->kind != DXT_INT || !type->bits || type->bits > 32) return 0;
    if (value->kind == DXV_CONST_NULL) *out = 0;
    else if (value->kind == DXV_CONST_INT) *out = (uint32_t)value->u.i;
    else return 0;
    return 1;
}

static int value_has_integer_width(const dxil_ir_builder *builder, uint32_t id, uint32_t width) {
    const dxil_value *value = dxil_value_at(builder->module, builder->function, id);
    const dxil_type *type = value ? source_type(builder->module, value->type) : NULL;
    return type && type->kind == DXT_INT && type->bits == width;
}

static int metadata_u32(const dxil_module *module, const dxil_md *node, uint32_t index, uint32_t *out) {
    int64_t value;
    const dxil_md *field = dxil_md_op(module, node, index);
    if (!out || !dxil_md_int(module, field, &value) || value < 0 || (uint64_t)value > UINT32_MAX) return 0;
    *out = (uint32_t)value;
    return 1;
}

static int metadata_i32(const dxil_module *module, const dxil_md *node, uint32_t index, int32_t *out) {
    int64_t value;
    const dxil_md *field = dxil_md_op(module, node, index);
    if (!out || !dxil_md_int(module, field, &value) || value < INT32_MIN || value > INT32_MAX) return 0;
    *out = (int32_t)value;
    return 1;
}

static int dxil_fragment_position_signature(const dxil_module *module, const dxil_function *function,
                                            uint32_t signature_id) {
    const dxil_named_md *entries = dxil_named(module, "dx.entryPoints");
    if (!entries || !entries->nodes) return entries ? GPU_IR_INVALID : GPU_IR_NOT_FOUND;
    const dxil_md *inputs = NULL;
    for (uint32_t i = 0; i < entries->n; i++) {
        if (entries->nodes[i] >= module->nmd) return GPU_IR_INVALID;
        const dxil_md *entry = &module->md[entries->nodes[i]];
        const dxil_md *function_ref = dxil_md_op(module, entry, 0);
        if (!function_ref || function_ref->kind != DXM_VALUE || function_ref->value != function->value) continue;
        const dxil_md *signatures = dxil_md_op(module, entry, 2);
        inputs = dxil_md_op(module, signatures, 0);
        if (!inputs || inputs->kind != DXM_NODE) return GPU_IR_UNSUPPORTED;
        break;
    }
    if (!inputs) return GPU_IR_NOT_FOUND;

    const dxil_md *match = NULL;
    for (uint32_t i = 0; i < inputs->n; i++) {
        const dxil_md *element = dxil_md_op(module, inputs, i);
        uint32_t id;
        if (!element || element->kind != DXM_NODE || !metadata_u32(module, element, 0, &id))
            return GPU_IR_INVALID;
        if (id == signature_id) {
            if (match) return GPU_IR_INVALID;
            match = element;
        }
    }
    if (!match) return GPU_IR_NOT_FOUND;

    const dxil_md *name = dxil_md_op(module, match, 1);
    const dxil_md *semantic_indices = dxil_md_op(module, match, 4);
    uint32_t component_type, system_value, semantic_index, rows, columns, start_row, start_column;
    if (match->n != 11 || !name || name->kind != DXM_STRING || !name->str ||
        !metadata_u32(module, match, 2, &component_type) ||
        !metadata_u32(module, match, 3, &system_value) ||
        !semantic_indices || semantic_indices->kind != DXM_NODE || semantic_indices->n != 1 ||
        !metadata_u32(module, semantic_indices, 0, &semantic_index) ||
        !metadata_u32(module, match, 6, &rows) || !metadata_u32(module, match, 7, &columns) ||
        !metadata_u32(module, match, 8, &start_row) || !metadata_u32(module, match, 9, &start_column))
        return GPU_IR_INVALID;
    if (strcasecmp(name->str, "SV_Position") || component_type != 9 || system_value != 3 ||
        semantic_index != 0 || rows != 1 || columns != 4 || start_row != 0 || start_column != 0)
        return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static const dxil_md *dxil_entry_input_signatures(const dxil_module *module,
                                                   const dxil_function *function, int *status) {
    const dxil_named_md *entries = dxil_named(module, "dx.entryPoints");
    if (!entries) { *status = GPU_IR_NOT_FOUND; return NULL; }
    if (!entries->nodes) { *status = GPU_IR_INVALID; return NULL; }
    for (uint32_t i = 0; i < entries->n; i++) {
        if (entries->nodes[i] >= module->nmd) { *status = GPU_IR_INVALID; return NULL; }
        const dxil_md *entry = &module->md[entries->nodes[i]];
        const dxil_md *function_ref = dxil_md_op(module, entry, 0);
        if (!function_ref || function_ref->kind != DXM_VALUE || function_ref->value != function->value)
            continue;
        const dxil_md *signatures = dxil_md_op(module, entry, 2);
        const dxil_md *inputs = dxil_md_op(module, signatures, 0);
        if (!inputs || inputs->kind != DXM_NODE) { *status = GPU_IR_UNSUPPORTED; return NULL; }
        *status = GPU_IR_OK;
        return inputs;
    }
    *status = GPU_IR_NOT_FOUND;
    return NULL;
}

static int dxil_vertex_input_signature(const dxil_module *module, const dxil_function *function,
                                       uint32_t signature_id, uint32_t *location, uint32_t *columns) {
    int rc = GPU_IR_OK;
    const dxil_md *inputs = dxil_entry_input_signatures(module, function, &rc);
    if (rc != GPU_IR_OK) return rc;
    uint32_t attribute = 0;
    int found = 0;
    for (uint32_t i = 0; i < inputs->n; i++) {
        const dxil_md *element = dxil_md_op(module, inputs, i);
        uint32_t id, component_type, system_value, rows, element_columns;
        if (!element || element->kind != DXM_NODE || element->n != 11 ||
            !metadata_u32(module, element, 0, &id) ||
            !metadata_u32(module, element, 2, &component_type) ||
            !metadata_u32(module, element, 3, &system_value) ||
            !metadata_u32(module, element, 6, &rows) ||
            !metadata_u32(module, element, 7, &element_columns) || !rows)
            return GPU_IR_INVALID;
        if (id == signature_id) {
            if (found) return GPU_IR_INVALID;
            if (system_value != 0 || component_type != DXIL_IR_COMPONENT_F32 || rows != 1 ||
                element_columns < 1 || element_columns > 4 || attribute >= DXIL_IR_MAX_VERTEX_INPUTS)
                return GPU_IR_UNSUPPORTED;
            *location = attribute;
            *columns = element_columns;
            found = 1;
        }
        // Keep this in sync with dxil_msl's vertex attribute reflection: system-value inputs
        // do not consume Metal attribute indices, while each user signature row does.
        if (system_value != 1 && system_value != 2 && system_value != 13 &&
            system_value != 12 && system_value != 10 && system_value != 14) {
            if (rows > DXIL_IR_MAX_VERTEX_INPUTS - attribute) return GPU_IR_UNSUPPORTED;
            attribute += rows;
        }
    }
    return found ? GPU_IR_OK : GPU_IR_NOT_FOUND;
}

static int dxil_vertex_system_value_signature(const dxil_module *module, const dxil_function *function,
                                               uint32_t signature_id, uint32_t *system_value_index) {
    int rc = GPU_IR_OK;
    const dxil_md *inputs = dxil_entry_input_signatures(module, function, &rc);
    if (rc != GPU_IR_OK) return rc;
    const dxil_md *match = NULL;
    for (uint32_t i = 0; i < inputs->n; i++) {
        const dxil_md *element = dxil_md_op(module, inputs, i);
        uint32_t id;
        if (!element || element->kind != DXM_NODE || !metadata_u32(module, element, 0, &id))
            return GPU_IR_INVALID;
        if (id == signature_id) {
            if (match) return GPU_IR_INVALID;
            match = element;
        }
    }
    if (!match) return GPU_IR_NOT_FOUND;

    const dxil_md *name = dxil_md_op(module, match, 1);
    const dxil_md *semantic_indices = dxil_md_op(module, match, 4);
    uint32_t component_type, semantic_kind, semantic_index, rows, columns, start_column;
    int32_t start_row;
    if (match->n != 11 || !name || name->kind != DXM_STRING || !name->str ||
        !metadata_u32(module, match, 2, &component_type) ||
        !metadata_u32(module, match, 3, &semantic_kind) ||
        !semantic_indices || semantic_indices->kind != DXM_NODE || semantic_indices->n != 1 ||
        !metadata_u32(module, semantic_indices, 0, &semantic_index) ||
        !metadata_u32(module, match, 6, &rows) || !metadata_u32(module, match, 7, &columns) ||
        !metadata_i32(module, match, 8, &start_row) || !metadata_u32(module, match, 9, &start_column))
        return GPU_IR_INVALID;

    uint32_t index;
    if (!strcasecmp(name->str, "SV_VertexID")) {
        if (semantic_kind != DXIL_IR_SEMANTIC_VERTEX_ID) return GPU_IR_UNSUPPORTED;
        index = 0;
    } else if (!strcasecmp(name->str, "SV_InstanceID")) {
        if (semantic_kind != DXIL_IR_SEMANTIC_INSTANCE_ID) return GPU_IR_UNSUPPORTED;
        index = 1;
    } else {
        // Generic signatures are handled by the adjacent vertex-attribute path.
        return GPU_IR_NOT_FOUND;
    }
    if (component_type != 0 || semantic_index != 0 || rows != 1 || columns != 1 ||
        (start_row != 0 && start_row != -1) || start_column != 0) return GPU_IR_UNSUPPORTED;
    *system_value_index = index;
    return GPU_IR_OK;
}

static int dxil_front_facing_signature(const dxil_module *module, const dxil_function *function,
                                       uint32_t signature_id) {
    int rc = GPU_IR_OK;
    const dxil_md *inputs = dxil_entry_input_signatures(module, function, &rc);
    if (rc != GPU_IR_OK) return rc;
    const dxil_md *match = NULL;
    for (uint32_t i = 0; i < inputs->n; i++) {
        const dxil_md *element = dxil_md_op(module, inputs, i);
        uint32_t id;
        if (!element || element->kind != DXM_NODE || !metadata_u32(module, element, 0, &id))
            return GPU_IR_INVALID;
        if (id == signature_id) {
            if (match) return GPU_IR_INVALID;
            match = element;
        }
    }
    if (!match) return GPU_IR_NOT_FOUND;

    const dxil_md *name = dxil_md_op(module, match, 1);
    const dxil_md *semantic_indices = dxil_md_op(module, match, 4);
    uint32_t component_type, semantic_kind, semantic_index, rows, columns, start_column;
    int32_t start_row;
    if (match->n != 11 || !name || name->kind != DXM_STRING || !name->str ||
        !metadata_u32(module, match, 2, &component_type) ||
        !metadata_u32(module, match, 3, &semantic_kind) ||
        !semantic_indices || semantic_indices->kind != DXM_NODE || semantic_indices->n != 1 ||
        !metadata_u32(module, semantic_indices, 0, &semantic_index) ||
        !metadata_u32(module, match, 6, &rows) || !metadata_u32(module, match, 7, &columns) ||
        !metadata_i32(module, match, 8, &start_row) || !metadata_u32(module, match, 9, &start_column))
        return GPU_IR_INVALID;
    if (strcasecmp(name->str, "SV_IsFrontFace") || component_type != DXIL_IR_COMPONENT_I1 ||
        semantic_kind != DXIL_IR_SEMANTIC_IS_FRONT_FACE ||
        semantic_index != 0 || rows != 1 || columns != 1 ||
        (start_row != 0 && start_row != -1) || start_column != 0)
        return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static int analyze_dxil_input_load(dxil_ir_builder *builder, const dxil_inst *instruction,
                                   uint32_t instruction_index) {
    if (!builder || !instruction || instruction->op != DXI_CALL || instruction->n != 6 ||
        instruction->flags || instruction->nvals || instruction->result == UINT32_MAX ||
        instruction_index >= builder->function->ninsts ||
        (builder->common_stage != GPU_IR_STAGE_FRAGMENT && builder->common_stage != GPU_IR_STAGE_VERTEX))
        return GPU_IR_UNSUPPORTED;
    int rc = call_signature_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    uint32_t opcode, signature_id, row, component;
    if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != 4 ||
        !value_i32(builder, instruction->ops[2], &signature_id) ||
        !value_i32(builder, instruction->ops[3], &row) || row != 0 ||
        !value_i32(builder, instruction->ops[4], &component) || component > 3 ||
        !value_has_integer_width(builder, instruction->ops[1], 32) ||
        !value_has_integer_width(builder, instruction->ops[2], 32) ||
        !value_has_integer_width(builder, instruction->ops[3], 32) ||
        !value_has_integer_width(builder, instruction->ops[4], 8) ||
        !value_has_integer_width(builder, instruction->ops[5], 32)) return GPU_IR_INVALID;
    const dxil_value *vertex_index = dxil_value_at(builder->module, builder->function, instruction->ops[5]);
    const dxil_type *result_type = source_type(builder->module, instruction->type);
    const dxil_value *result = instruction->result < builder->value_count ?
        dxil_value_at(builder->module, builder->function, instruction->result) : NULL;
    if (!vertex_index || vertex_index->kind != DXV_CONST_UNDEF) return GPU_IR_UNSUPPORTED;
    if (!result || result->kind != DXV_INST || result->type != instruction->type ||
        result->u.index != instruction_index) return GPU_IR_INVALID;
    if (builder->common_stage == GPU_IR_STAGE_FRAGMENT) {
        if (result_type && result_type->kind == DXT_FLOAT && result_type->bits == 32) {
            if (component > 3) return GPU_IR_INVALID;
            rc = dxil_fragment_position_signature(builder->module, builder->function, signature_id);
            if (rc != GPU_IR_OK) return rc == GPU_IR_NOT_FOUND ? GPU_IR_UNSUPPORTED : rc;
            if (builder->fragment_position.present &&
                (builder->fragment_position.signature_id != signature_id ||
                 builder->fragment_position.scalar_type_id != instruction->type)) return GPU_IR_UNSUPPORTED;
            builder->fragment_position.present = 1;
            builder->fragment_position.signature_id = signature_id;
            builder->fragment_position.scalar_type_id = instruction->type;
        } else if (result_type && result_type->kind == DXT_INT && result_type->bits == 1 && component == 0) {
            rc = dxil_front_facing_signature(builder->module, builder->function, signature_id);
            if (rc != GPU_IR_OK) return rc == GPU_IR_NOT_FOUND ? GPU_IR_UNSUPPORTED : rc;
            if (builder->front_facing.present &&
                (builder->front_facing.signature_id != signature_id ||
                 builder->front_facing.scalar_type_id != instruction->type)) return GPU_IR_UNSUPPORTED;
            builder->front_facing.present = 1;
            builder->front_facing.signature_id = signature_id;
            builder->front_facing.scalar_type_id = instruction->type;
        } else return GPU_IR_UNSUPPORTED;
    } else {
        if (result_type && result_type->kind == DXT_INT && result_type->bits == 32 && component == 0) {
            uint32_t system_value_index = 0;
            rc = dxil_vertex_system_value_signature(builder->module, builder->function, signature_id,
                                                     &system_value_index);
            if (rc != GPU_IR_OK) return rc == GPU_IR_NOT_FOUND ? GPU_IR_UNSUPPORTED : rc;
            if (system_value_index >= DXIL_IR_VERTEX_SYSTEM_VALUE_COUNT) return GPU_IR_INVALID;
            dxil_ir_vertex_system_value *system_value = &builder->vertex_system_values[system_value_index];
            uint32_t builtin = system_value_index == 0 ? GPU_IR_SHADER_BUILTIN_VERTEX_INDEX :
                GPU_IR_SHADER_BUILTIN_INSTANCE_ID;
            uint32_t semantic_kind = system_value_index == 0 ? DXIL_IR_SEMANTIC_VERTEX_ID :
                DXIL_IR_SEMANTIC_INSTANCE_ID;
            if (system_value->present && (system_value->signature_id != signature_id ||
                system_value->scalar_type_id != instruction->type)) return GPU_IR_UNSUPPORTED;
            *system_value = (dxil_ir_vertex_system_value){ .present = 1, .signature_id = signature_id,
                .scalar_type_id = instruction->type, .semantic_kind = semantic_kind, .builtin = builtin };
        } else {
            if (!result_type || result_type->kind != DXT_FLOAT || result_type->bits != 32)
                return GPU_IR_UNSUPPORTED;
            uint32_t location = 0, columns = 0;
            rc = dxil_vertex_input_signature(builder->module, builder->function, signature_id, &location, &columns);
            if (rc != GPU_IR_OK) return rc == GPU_IR_NOT_FOUND ? GPU_IR_UNSUPPORTED : rc;
            if (component >= columns) return GPU_IR_UNSUPPORTED;
            dxil_ir_vertex_input *input = NULL;
            for (uint32_t i = 0; i < builder->vertex_input_count; i++)
                if (builder->vertex_inputs[i].signature_id == signature_id) input = &builder->vertex_inputs[i];
            if (!input) {
                if (builder->vertex_input_count >= DXIL_IR_MAX_VERTEX_INPUTS) return GPU_IR_UNSUPPORTED;
                input = &builder->vertex_inputs[builder->vertex_input_count++];
                *input = (dxil_ir_vertex_input){ .present = 1, .signature_id = signature_id,
                    .location = location, .columns = (uint8_t)columns, .scalar_type_id = instruction->type };
            } else if (input->location != location || input->columns != columns ||
                       input->scalar_type_id != instruction->type) return GPU_IR_UNSUPPORTED;
        }
    }
    builder->input_load_ids[instruction_index] = 1;
    return mark_value(builder, instruction->result);
}

static int metadata_tag_u32(const dxil_module *module, const dxil_md *node, uint32_t tag, uint32_t *out) {
    if (!node || node->kind != DXM_NODE) return 0;
    for (uint32_t i = 0; i + 1 < node->n; i += 2) {
        uint32_t actual_tag, value;
        if (metadata_u32(module, node, i, &actual_tag) && actual_tag == tag &&
            metadata_u32(module, node, i + 1, &value)) {
            *out = value;
            return 1;
        }
    }
    return 0;
}

// This first graphics slice intentionally accepts only a complete RGBA write to SV_Target0. DXIL accesses
// signature elements by their metadata ID and supplies scalar components, so the adapter gathers those four
// values and constructs one API-neutral float4 output at the last write site.
static int dxil_target0_signature(const dxil_module *module, const dxil_function *function,
                                  uint32_t *signature_id) {
    const dxil_named_md *entries = dxil_named(module, "dx.entryPoints");
    if (!entries) return GPU_IR_NOT_FOUND;
    if (!entries->nodes) return GPU_IR_INVALID;
    const dxil_md *outputs = NULL;
    for (uint32_t i = 0; i < entries->n; i++) {
        if (entries->nodes[i] >= module->nmd) return GPU_IR_INVALID;
        const dxil_md *entry = &module->md[entries->nodes[i]];
        const dxil_md *function_ref = dxil_md_op(module, entry, 0);
        if (!function_ref || function_ref->kind != DXM_VALUE || function_ref->value != function->value) continue;
        const dxil_md *signatures = dxil_md_op(module, entry, 2);
        outputs = dxil_md_op(module, signatures, 1);
        if (!outputs || outputs->kind != DXM_NODE) return GPU_IR_UNSUPPORTED;
        break;
    }
    if (!outputs) return GPU_IR_NOT_FOUND;
    int found_target0 = 0;
    uint32_t target0_id = 0;
    for (uint32_t i = 0; i < outputs->n; i++) {
        const dxil_md *element = dxil_md_op(module, outputs, i);
        const dxil_md *name = dxil_md_op(module, element, 1);
        if (!element || element->n != 11 || !name || name->kind != DXM_STRING || !name->str)
            return GPU_IR_INVALID;
        if (strcasecmp(name->str, "SV_Target")) continue;
        uint32_t semantic_index, rows, columns, start_row, start_column, id;
        const dxil_md *indices = dxil_md_op(module, element, 4);
        if (!metadata_u32(module, element, 0, &id) || !indices || indices->kind != DXM_NODE ||
            indices->n != 1 || !metadata_u32(module, indices, 0, &semantic_index) ||
            !metadata_u32(module, element, 6, &rows) || !metadata_u32(module, element, 7, &columns) ||
            !metadata_u32(module, element, 8, &start_row) ||
            !metadata_u32(module, element, 9, &start_column)) return GPU_IR_INVALID;
        if (semantic_index == 0) {
            if (rows != 1 || columns != 4 || start_row != 0 || start_column != 0)
                return GPU_IR_UNSUPPORTED;
            if (found_target0) return GPU_IR_INVALID;
            target0_id = id;
            found_target0 = 1;
        }
    }
    if (!found_target0) return GPU_IR_NOT_FOUND;
    *signature_id = target0_id;
    return GPU_IR_OK;
}

static int dxil_vertex_position_signature(const dxil_module *module, const dxil_function *function,
                                         uint32_t *signature_id) {
    const dxil_named_md *entries = dxil_named(module, "dx.entryPoints");
    if (!entries) return GPU_IR_NOT_FOUND;
    if (!entries->nodes) return GPU_IR_INVALID;
    const dxil_md *outputs = NULL;
    for (uint32_t i = 0; i < entries->n; i++) {
        if (entries->nodes[i] >= module->nmd) return GPU_IR_INVALID;
        const dxil_md *entry = &module->md[entries->nodes[i]];
        const dxil_md *function_ref = dxil_md_op(module, entry, 0);
        if (!function_ref || function_ref->kind != DXM_VALUE || function_ref->value != function->value) continue;
        const dxil_md *signatures = dxil_md_op(module, entry, 2);
        outputs = dxil_md_op(module, signatures, 1);
        if (!outputs || outputs->kind != DXM_NODE) return GPU_IR_UNSUPPORTED;
        break;
    }
    if (!outputs) return GPU_IR_NOT_FOUND;
    int found_position = 0;
    uint32_t position_id = 0;
    for (uint32_t i = 0; i < outputs->n; i++) {
        const dxil_md *element = dxil_md_op(module, outputs, i);
        const dxil_md *name = dxil_md_op(module, element, 1);
        if (!element || element->n != 11 || !name || name->kind != DXM_STRING || !name->str)
            return GPU_IR_INVALID;
        if (strcasecmp(name->str, "SV_Position")) continue;
        const dxil_md *indices = dxil_md_op(module, element, 4);
        uint32_t id = UINT32_MAX, component_type = UINT32_MAX, system_value = UINT32_MAX;
        uint32_t semantic_index = UINT32_MAX, rows = UINT32_MAX, columns = UINT32_MAX;
        uint32_t start_row = UINT32_MAX, start_column = UINT32_MAX;
        int metadata_valid = metadata_u32(module, element, 0, &id) &&
            metadata_u32(module, element, 2, &component_type) &&
            metadata_u32(module, element, 3, &system_value) && indices && indices->kind == DXM_NODE &&
            indices->n == 1 && metadata_u32(module, indices, 0, &semantic_index) &&
            metadata_u32(module, element, 6, &rows) && metadata_u32(module, element, 7, &columns) &&
            metadata_u32(module, element, 8, &start_row) && metadata_u32(module, element, 9, &start_column);
        if (!metadata_valid) {
            return GPU_IR_INVALID;
        }
        if (component_type != 9 || system_value != 3 || semantic_index != 0 || rows != 1 ||
            columns != 4 || start_row != 0 || start_column != 0) return GPU_IR_UNSUPPORTED;
        if (found_position) return GPU_IR_INVALID;
        position_id = id;
        found_position = 1;
    }
    if (!found_position) return GPU_IR_NOT_FOUND;
    *signature_id = position_id;
    return GPU_IR_OK;
}

static int analyze_dxil_target0_stores(dxil_ir_builder *builder) {
    uint32_t store_count = 0;
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        const dxil_inst *instruction = &builder->function->insts[i];
        if (instruction->op == DXI_CALL && dxil_dxop(builder->module, builder->function, instruction) == 5)
            store_count++;
    }
    if (!store_count) return GPU_IR_OK;
    if (builder->common_stage != GPU_IR_STAGE_FRAGMENT) return GPU_IR_UNSUPPORTED;
    uint32_t output_signature_id = 0;
    int rc = dxil_target0_signature(builder->module, builder->function, &output_signature_id);
    if (rc != GPU_IR_OK) return rc == GPU_IR_NOT_FOUND ? GPU_IR_UNSUPPORTED : rc;
    dxil_ir_color_output output = { .present = 1, .signature_id = output_signature_id,
                                    .scalar_type_id = UINT32_MAX, .block_index = UINT32_MAX };
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        const dxil_inst *instruction = &builder->function->insts[i];
        if (instruction->op != DXI_CALL || dxil_dxop(builder->module, builder->function, instruction) != 5)
            continue;
        if (instruction->flags || instruction->nvals || instruction->result != UINT32_MAX || instruction->n != 6)
            return GPU_IR_INVALID;
        rc = call_signature_valid(builder, instruction);
        if (rc != GPU_IR_OK) return rc;
        uint32_t opcode, signature_id, row, component;
        if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != 5 ||
            !value_i32(builder, instruction->ops[2], &signature_id) ||
            !value_i32(builder, instruction->ops[3], &row) ||
            !value_i32(builder, instruction->ops[4], &component) || component > 3 ||
            !value_has_integer_width(builder, instruction->ops[1], 32) ||
            !value_has_integer_width(builder, instruction->ops[2], 32) ||
            !value_has_integer_width(builder, instruction->ops[3], 32) ||
            !value_has_integer_width(builder, instruction->ops[4], 8)) return GPU_IR_INVALID;
        if (signature_id != output_signature_id || row != 0) return GPU_IR_UNSUPPORTED;
        const dxil_value *value = dxil_value_at(builder->module, builder->function, instruction->ops[5]);
        const dxil_type *type = value ? source_type(builder->module, value->type) : NULL;
        if (!type || type->kind != DXT_FLOAT || type->bits != 32) return GPU_IR_UNSUPPORTED;
        if (output.mask & (1u << component)) return GPU_IR_UNSUPPORTED;
        if (output.block_index != UINT32_MAX && output.block_index != instruction->block)
            return GPU_IR_UNSUPPORTED;
        output.scalar_type_id = value->type;
        output.block_index = instruction->block;
        output.last_instruction = i;
        output.values[component] = instruction->ops[5];
        output.mask |= (uint8_t)(1u << component);
    }
    if (output.mask != 0x0fu || output.scalar_type_id == UINT32_MAX) return GPU_IR_UNSUPPORTED;
    builder->color_output = output;
    return GPU_IR_OK;
}

static int analyze_dxil_vertex_position_stores(dxil_ir_builder *builder) {
    uint32_t store_count = 0;
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        const dxil_inst *instruction = &builder->function->insts[i];
        if (instruction->op == DXI_CALL && dxil_dxop(builder->module, builder->function, instruction) == 5)
            store_count++;
    }
    if (!store_count) return GPU_IR_OK;
    if (builder->common_stage != GPU_IR_STAGE_VERTEX) return GPU_IR_UNSUPPORTED;
    uint32_t output_signature_id = 0;
    int rc = dxil_vertex_position_signature(builder->module, builder->function, &output_signature_id);
    if (rc != GPU_IR_OK) return rc == GPU_IR_NOT_FOUND ? GPU_IR_UNSUPPORTED : rc;
    dxil_ir_vertex_position_output output = {
        .present = 1, .signature_id = output_signature_id,
        .scalar_type_id = UINT32_MAX, .block_index = UINT32_MAX
    };
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        const dxil_inst *instruction = &builder->function->insts[i];
        if (instruction->op != DXI_CALL || dxil_dxop(builder->module, builder->function, instruction) != 5)
            continue;
        if (instruction->flags || instruction->nvals || instruction->result != UINT32_MAX || instruction->n != 6)
            return GPU_IR_INVALID;
        rc = call_signature_valid(builder, instruction);
        if (rc != GPU_IR_OK) return rc;
        uint32_t opcode, signature_id, row, component;
        if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != 5 ||
            !value_i32(builder, instruction->ops[2], &signature_id) ||
            !value_i32(builder, instruction->ops[3], &row) ||
            !value_i32(builder, instruction->ops[4], &component) || component > 3 ||
            !value_has_integer_width(builder, instruction->ops[1], 32) ||
            !value_has_integer_width(builder, instruction->ops[2], 32) ||
            !value_has_integer_width(builder, instruction->ops[3], 32) ||
            !value_has_integer_width(builder, instruction->ops[4], 8)) return GPU_IR_INVALID;
        if (signature_id != output_signature_id || row != 0) return GPU_IR_UNSUPPORTED;
        const dxil_value *value = dxil_value_at(builder->module, builder->function, instruction->ops[5]);
        const dxil_type *type = value ? source_type(builder->module, value->type) : NULL;
        if (!type || type->kind != DXT_FLOAT || type->bits != 32) return GPU_IR_UNSUPPORTED;
        if (output.mask & (1u << component)) return GPU_IR_UNSUPPORTED;
        if (output.block_index != UINT32_MAX && output.block_index != instruction->block)
            return GPU_IR_UNSUPPORTED;
        output.scalar_type_id = value->type;
        output.block_index = instruction->block;
        output.last_instruction = i;
        output.values[component] = instruction->ops[5];
        output.mask |= (uint8_t)(1u << component);
    }
    if (output.mask != 0x0fu || output.scalar_type_id == UINT32_MAX) return GPU_IR_UNSUPPORTED;
    builder->vertex_position_output = output;
    return GPU_IR_OK;
}

static int analyze_dxil_graphics_stores(dxil_ir_builder *builder) {
    if (builder->common_stage == GPU_IR_STAGE_FRAGMENT) return analyze_dxil_target0_stores(builder);
    if (builder->common_stage == GPU_IR_STAGE_VERTEX) return analyze_dxil_vertex_position_stores(builder);
    return analyze_dxil_target0_stores(builder);
}

static int parse_resource_metadata(dxil_ir_builder *builder) {
    const dxil_named_md *named = dxil_named(builder->module, "dx.resources");
    if (!named) return GPU_IR_OK;
    if (named->n != 1 || !named->nodes || named->nodes[0] >= builder->module->nmd) return GPU_IR_INVALID;
    const dxil_md *lists = &builder->module->md[named->nodes[0]];
    if (lists->kind != DXM_NODE || lists->n > 4) return GPU_IR_INVALID;
    for (uint32_t cls = 0; cls < 4; cls++) {
        const dxil_md *list = dxil_md_op(builder->module, lists, cls);
        if (!list) continue;
        if (list->kind != DXM_NODE) return GPU_IR_INVALID;
        for (uint32_t range = 0; range < list->n; range++) {
            if (builder->resource_count >= sizeof builder->resources / sizeof builder->resources[0])
                return GPU_IR_UNSUPPORTED;
            const dxil_md *record = dxil_md_op(builder->module, list, range);
            if (!record || record->kind != DXM_NODE) return GPU_IR_INVALID;
            dxil_ir_resource resource = { .register_class = (uint8_t)cls, .range_id = range };
            resource.valid_metadata = metadata_u32(builder->module, record, 3, &resource.set) &&
                metadata_u32(builder->module, record, 4, &resource.binding) &&
                metadata_u32(builder->module, record, 5, &resource.count) && resource.count != 0;
            if (cls <= 1) {
                resource.valid_metadata &= metadata_u32(builder->module, record, 6, &resource.kind);
                const dxil_md *properties = dxil_md_op(builder->module, record, cls == 0 ? 8 : 10);
                if (resource.kind == 12)
                    resource.valid_metadata &= metadata_tag_u32(builder->module, properties, 1,
                                                                  &resource.structure_stride);
                else if (resource.kind != 11)
                    resource.valid_metadata &= metadata_tag_u32(builder->module, properties, 0,
                                                                  &resource.component_type);
            }
            builder->resources[builder->resource_count++] = resource;
        }
    }
    builder->resource_u32_source_type = UINT32_MAX;
    builder->resource_f32_source_type = UINT32_MAX;
    for (uint32_t type = 0; type < builder->module->ntypes; type++) {
        const dxil_type *candidate = source_type(builder->module, type);
        if (candidate && candidate->kind == DXT_INT && candidate->bits == 32 &&
            builder->resource_u32_source_type == UINT32_MAX) {
            builder->resource_u32_source_type = type;
        }
        if (candidate && candidate->kind == DXT_FLOAT && builder->resource_f32_source_type == UINT32_MAX)
            builder->resource_f32_source_type = type;
    }
    return GPU_IR_OK;
}

static int resource_index_by_range(const dxil_ir_builder *builder, uint32_t cls, uint32_t range,
                                   uint32_t *resource_index) {
    for (uint32_t i = 0; i < builder->resource_count; i++) {
        const dxil_ir_resource *resource = &builder->resources[i];
        if (resource->register_class == cls && resource->range_id == range) {
            *resource_index = i;
            return GPU_IR_OK;
        }
    }
    return GPU_IR_NOT_FOUND;
}

static int resource_is_common_r32_buffer(const dxil_ir_resource *resource) {
    return resource && resource->valid_metadata && resource->register_class <= 1 &&
        resource->kind == 10 && (resource->component_type == DXIL_IR_COMPONENT_S32 ||
                                 resource->component_type == DXIL_IR_COMPONENT_U32 ||
                                 resource->component_type == DXIL_IR_COMPONENT_F32) &&
        resource->count == 1;
}

static int resource_is_common_raw_buffer(const dxil_ir_resource *resource) {
    return resource && resource->valid_metadata && resource->register_class == 0 &&
        resource->kind == 11 && resource->count == 1;
}

static int resource_is_common_raw_buffer_uav(const dxil_ir_resource *resource) {
    return resource && resource->valid_metadata && resource->register_class == 1 &&
        resource->kind == 11 && resource->count == 1;
}

static int resource_is_common_structured_u32_buffer(const dxil_ir_resource *resource) {
    return resource && resource->register_class == 1 && resource->structure_stride == 4 &&
        resource->valid_metadata && resource->kind == 12 && resource->count == 1;
}

static int resource_is_common_structured_buffer(const dxil_ir_resource *resource) {
    return resource && resource->valid_metadata && resource->register_class <= 1 &&
        resource->kind == 12 && resource->count == 1 && resource->structure_stride >= 4 &&
        resource->structure_stride % 4 == 0;
}

static int resource_is_common_buffer_backing(const dxil_ir_resource *resource) {
    return resource_is_common_r32_buffer(resource) || resource_is_common_structured_buffer(resource) ||
        resource_is_common_raw_buffer(resource) || resource_is_common_raw_buffer_uav(resource);
}

static int resource_is_common_float2d_srv(const dxil_ir_resource *resource) {
    // DXIL resource kinds 2 and 7 are Texture2D and Texture2DArray. This slice handles
    // non-MSAA float SRVs for dimension queries; the array form adds its layer count.
    return resource && resource->valid_metadata && resource->register_class == 0 &&
        (resource->kind == 2 || resource->kind == 7) &&
        resource->component_type == DXIL_IR_COMPONENT_F32 && resource->count == 1;
}

static int resource_is_common_float2d_ms_srv(const dxil_ir_resource *resource) {
    return resource && resource->valid_metadata && resource->register_class == 0 &&
        resource->kind == 3 && resource->component_type == DXIL_IR_COMPONENT_F32 && resource->count == 1;
}

static int resource_is_common_float2d_ms_array_srv(const dxil_ir_resource *resource) {
    return resource && resource->valid_metadata && resource->register_class == 0 &&
        resource->kind == 4 && resource->component_type == DXIL_IR_COMPONENT_F32 && resource->count == 1;
}

static int resource_is_common_uint2d_srv(const dxil_ir_resource *resource) {
    return resource && resource->valid_metadata && resource->register_class == 0 &&
        (resource->kind == 2 || resource->kind == 7) &&
        resource->component_type == DXIL_IR_COMPONENT_U32 && resource->count == 1;
}

static int resource_is_common_sint2d_srv(const dxil_ir_resource *resource) {
    return resource && resource->valid_metadata && resource->register_class == 0 &&
        (resource->kind == 2 || resource->kind == 7) &&
        resource->component_type == DXIL_IR_COMPONENT_S32 && resource->count == 1;
}

static int resource_is_common_image2d_srv(const dxil_ir_resource *resource) {
    return resource_is_common_float2d_srv(resource) || resource_is_common_uint2d_srv(resource) ||
        resource_is_common_sint2d_srv(resource);
}

static int resource_is_common_image2d_or_ms_srv(const dxil_ir_resource *resource) {
    return resource_is_common_image2d_srv(resource) || resource_is_common_float2d_ms_srv(resource) ||
        resource_is_common_float2d_ms_array_srv(resource);
}

static int resource_is_common_float2d_array_srv(const dxil_ir_resource *resource) {
    return resource_is_common_float2d_srv(resource) && resource->kind == 7;
}

static int resource_type_matches_component(const dxil_ir_resource *resource, const dxil_type *type) {
    if (!resource || !type) return 0;
    if (resource_is_common_raw_buffer(resource) || resource_is_common_raw_buffer_uav(resource))
        return (type->kind == DXT_INT && type->bits == 32) || type->kind == DXT_FLOAT;
    if (resource->component_type == DXIL_IR_COMPONENT_S32 ||
        resource->component_type == DXIL_IR_COMPONENT_U32)
        return type->kind == DXT_INT && type->bits == 32;
    if (resource->component_type == DXIL_IR_COMPONENT_F32) return type->kind == DXT_FLOAT;
    return 0;
}

static int call_signature_valid(const dxil_ir_builder *builder, const dxil_inst *instruction) {
    if (!instruction || instruction->op != DXI_CALL || !instruction->n || !instruction->ops)
        return GPU_IR_INVALID;
    const dxil_value *callee = dxil_value_at(builder->module, builder->function, instruction->ops[0]);
    const dxil_type *callee_type = callee ? source_type(builder->module, callee->type) : NULL;
    if (callee_type && callee_type->kind == DXT_POINTER) callee_type = source_type(builder->module, callee_type->elem);
    if (!callee || callee->kind != DXV_FUNCTION || !callee_type || callee_type->kind != DXT_FUNCTION ||
        callee_type->vararg || callee_type->elem != instruction->type || callee_type->count != instruction->n - 1u ||
        (callee_type->count && !callee_type->members)) return GPU_IR_INVALID;
    for (uint32_t i = 0; i < callee_type->count; i++) {
        const dxil_value *argument = dxil_value_at(builder->module, builder->function, instruction->ops[i + 1u]);
        if (!argument || argument->type != callee_type->members[i]) return GPU_IR_INVALID;
    }
    return GPU_IR_OK;
}

// DXIL bit intrinsics use scalar i32 operands. Normalize their source-specific argument order to the common
// GPU IR's (base, offset, count) and (base, insert, offset, count) forms.
static int dxil_bit_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                   uint32_t *operation, uint32_t source_values[4], uint32_t *source_value_count) {
    if (!builder || !instruction || !operation || !source_values || !source_value_count) return GPU_IR_INVALID;
    int dxop = dxil_dxop(builder->module, builder->function, instruction);
    uint32_t expected_arguments = 0;
    switch (dxop) {
    case 30: *operation = GPU_IR_SHADER_ARITH_BIT_REVERSE; expected_arguments = 1; break;
    case 31: *operation = GPU_IR_SHADER_ARITH_BIT_COUNT; expected_arguments = 1; break;
    case 32: *operation = GPU_IR_SHADER_ARITH_BIT_FIND_LSB; expected_arguments = 1; break;
    case 33: *operation = GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED; expected_arguments = 1; break;
    case 34: *operation = GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED; expected_arguments = 1; break;
    case 51: *operation = GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT; expected_arguments = 3; break;
    case 52: *operation = GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT; expected_arguments = 3; break;
    case 53: *operation = GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT; expected_arguments = 4; break;
    default: return GPU_IR_NOT_FOUND;
    }
    if (instruction->op != DXI_CALL || instruction->flags || instruction->n != expected_arguments + 2u)
        return GPU_IR_INVALID;
    int rc = call_signature_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    if (instruction->result == UINT32_MAX || instruction->result >= builder->value_count)
        return GPU_IR_INVALID;
    const dxil_value *result = dxil_value_at(builder->module, builder->function, instruction->result);
    const dxil_type *result_type = result ? source_type(builder->module, result->type) : NULL;
    const dxil_type *instruction_type = source_type(builder->module, instruction->type);
    if (!result || result->kind != DXV_INST || result->type != instruction->type ||
        result->u.index >= builder->function->ninsts) return GPU_IR_INVALID;
    uint32_t result_components = 1;
    const dxil_type *result_scalar = numeric_scalar_type(builder->module, instruction_type, &result_components);
    if (!result_type || !result_scalar || result_scalar->kind != DXT_INT || result_scalar->bits != 32 ||
        result_components > 4 || (instruction_type->kind == DXT_VECTOR &&
        (result_components < 2 || expected_arguments != 1)))
        return GPU_IR_UNSUPPORTED;

    for (uint32_t i = 0; i < expected_arguments; i++) {
        const dxil_value *argument = dxil_value_at(builder->module, builder->function, instruction->ops[i + 2u]);
        const dxil_type *type = argument ? source_type(builder->module, argument->type) : NULL;
        if (!argument || !type) return GPU_IR_INVALID;
        if (expected_arguments == 1) {
            uint32_t components = 1;
            const dxil_type *scalar = numeric_scalar_type(builder->module, type, &components);
            if (argument->type != instruction->type || !scalar || scalar->kind != DXT_INT ||
                scalar->bits != 32 || components != result_components)
                return GPU_IR_UNSUPPORTED;
        } else if (type->kind != DXT_INT || type->bits != 32) return GPU_IR_UNSUPPORTED;
    }
    if (expected_arguments == 1) {
        source_values[0] = instruction->ops[2];
        *source_value_count = 1;
    } else if (dxop == 51 || dxop == 52) {
        // DXIL Ibfe/Ubfe arguments are (width, offset, base).
        source_values[0] = instruction->ops[4];
        source_values[1] = instruction->ops[3];
        source_values[2] = instruction->ops[2];
        *source_value_count = 3;
    } else {
        // DXIL Bfi arguments are (width, offset, insert, base).
        source_values[0] = instruction->ops[5];
        source_values[1] = instruction->ops[4];
        source_values[2] = instruction->ops[3];
        source_values[3] = instruction->ops[2];
        *source_value_count = 4;
    }
    return GPU_IR_OK;
}

static int resource_result_valid(const dxil_ir_builder *builder, const dxil_inst *instruction) {
    if (instruction->result == UINT32_MAX || instruction->result >= builder->value_count) return GPU_IR_INVALID;
    const dxil_value *result = dxil_value_at(builder->module, builder->function, instruction->result);
    return result && result->kind == DXV_INST && result->type == instruction->type &&
        result->u.index < builder->function->ninsts ? GPU_IR_OK : GPU_IR_INVALID;
}

// DXIL's integer min/max opcodes carry signedness in the opcode rather than in LLVM's signless i32 type.
static int dxil_integer_minmax_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                              uint32_t *operation, uint32_t *flags,
                                              uint32_t source_values[2]) {
    if (!builder || !instruction || !operation || !flags || !source_values) return GPU_IR_INVALID;
    int dxop = dxil_dxop(builder->module, builder->function, instruction);
    switch (dxop) {
    case 37: *operation = GPU_IR_SHADER_ARITH_MAXIMUM; *flags = GPU_IR_SHADER_FLAG_SIGNED; break;
    case 38: *operation = GPU_IR_SHADER_ARITH_MINIMUM; *flags = GPU_IR_SHADER_FLAG_SIGNED; break;
    case 39: *operation = GPU_IR_SHADER_ARITH_MAXIMUM; *flags = 0; break;
    case 40: *operation = GPU_IR_SHADER_ARITH_MINIMUM; *flags = 0; break;
    default: return GPU_IR_NOT_FOUND;
    }
    if (instruction->op != DXI_CALL || instruction->flags || instruction->n != 4 || !instruction->ops)
        return GPU_IR_INVALID;
    int rc = call_signature_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    uint32_t actual_opcode;
    if (!value_i32(builder, instruction->ops[1], &actual_opcode) || actual_opcode != (uint32_t)dxop)
        return GPU_IR_INVALID;
    rc = resource_result_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    const dxil_type *result_type = source_type(builder->module, instruction->type);
    if (!result_type || result_type->kind != DXT_INT || result_type->bits != 32)
        return GPU_IR_UNSUPPORTED;
    for (uint32_t i = 0; i < 2; i++) {
        const dxil_value *argument = dxil_value_at(builder->module, builder->function, instruction->ops[i + 2u]);
        const dxil_type *argument_type = argument ? source_type(builder->module, argument->type) : NULL;
        if (!argument || !argument_type) return GPU_IR_INVALID;
        if (argument->type != instruction->type || argument_type->kind != DXT_INT || argument_type->bits != 32)
            return GPU_IR_UNSUPPORTED;
        source_values[i] = instruction->ops[i + 2u];
    }
    return GPU_IR_OK;
}

static int dxil_integer32_shape(const dxil_module *module, uint32_t type_id) {
    const dxil_type *type = source_type(module, type_id);
    if (!type) return 0;
    if (type->kind == DXT_INT) return type->bits == 32;
    if (type->kind != DXT_VECTOR || type->count < 2 || type->count > 4) return 0;
    const dxil_type *element = source_type(module, type->elem);
    return element && element->kind == DXT_INT && element->bits == 32;
}

// DXIL IMad/UMad encode signedness in the opcode. The 32-bit result keeps the low product-plus-add bits.
static int dxil_integer_mad_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                            uint32_t *flags, uint32_t source_values[3]) {
    if (!builder || !instruction || !flags || !source_values) return GPU_IR_INVALID;
    int dxop = dxil_dxop(builder->module, builder->function, instruction);
    if (dxop != 48 && dxop != 49) return GPU_IR_NOT_FOUND;
    *flags = dxop == 48 ? GPU_IR_SHADER_FLAG_SIGNED : 0;
    if (instruction->op != DXI_CALL || instruction->flags || instruction->n != 5 || !instruction->ops)
        return GPU_IR_INVALID;
    int rc = call_signature_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    uint32_t actual_opcode;
    if (!value_i32(builder, instruction->ops[1], &actual_opcode) || actual_opcode != (uint32_t)dxop)
        return GPU_IR_INVALID;
    rc = resource_result_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    if (!dxil_integer32_shape(builder->module, instruction->type)) return GPU_IR_UNSUPPORTED;
    for (uint32_t i = 0; i < 3; i++) {
        const dxil_value *argument = dxil_value_at(builder->module, builder->function, instruction->ops[i + 2u]);
        if (!argument) return GPU_IR_INVALID;
        if (argument->type != instruction->type || !dxil_integer32_shape(builder->module, argument->type))
            return GPU_IR_UNSUPPORTED;
        source_values[i] = instruction->ops[i + 2u];
    }
    return GPU_IR_OK;
}

static int dxil_float32_shape(const dxil_module *module, uint32_t type_id, int allow_vector) {
    const dxil_type *type = source_type(module, type_id);
    if (!type) return 0;
    if (type->kind == DXT_FLOAT) return 1;
    if (!allow_vector || type->kind != DXT_VECTOR || type->count < 2 || type->count > 4) return 0;
    const dxil_type *element = source_type(module, type->elem);
    return element && element->kind == DXT_FLOAT;
}

static int dxil_bool_shape_matches(const dxil_module *module, uint32_t float_type_id,
                                   uint32_t bool_type_id) {
    const dxil_type *float_type = source_type(module, float_type_id);
    const dxil_type *bool_type = source_type(module, bool_type_id);
    if (!float_type || !bool_type) return 0;
    if (float_type->kind == DXT_FLOAT)
        return bool_type->kind == DXT_INT && bool_type->bits == 1;
    if (float_type->kind != DXT_VECTOR || float_type->count < 2 || float_type->count > 4 ||
        bool_type->kind != DXT_VECTOR || bool_type->count != float_type->count) return 0;
    const dxil_type *float_element = source_type(module, float_type->elem);
    const dxil_type *bool_element = source_type(module, bool_type->elem);
    return float_element && float_element->kind == DXT_FLOAT &&
        bool_element && bool_element->kind == DXT_INT && bool_element->bits == 1;
}

static int dxil_float_mad_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                          uint32_t *flags, uint32_t source_values[3]) {
    if (!builder || !instruction || !flags || !source_values) return GPU_IR_INVALID;
    int dxop = dxil_dxop(builder->module, builder->function, instruction);
    if (dxop != 46) return GPU_IR_NOT_FOUND;
    *flags = GPU_IR_SHADER_FLAG_FLOAT;
    if (instruction->op != DXI_CALL || instruction->flags || instruction->n != 5 || !instruction->ops)
        return GPU_IR_INVALID;
    int rc = call_signature_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    uint32_t actual_opcode;
    if (!value_i32(builder, instruction->ops[1], &actual_opcode) || actual_opcode != (uint32_t)dxop)
        return GPU_IR_INVALID;
    rc = resource_result_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    if (!dxil_float32_shape(builder->module, instruction->type, 1)) return GPU_IR_UNSUPPORTED;
    for (uint32_t i = 0; i < 3; i++) {
        const dxil_value *argument = dxil_value_at(builder->module, builder->function, instruction->ops[i + 2u]);
        if (!argument) return GPU_IR_INVALID;
        if (argument->type != instruction->type || !dxil_float32_shape(builder->module, argument->type, 1))
            return GPU_IR_UNSUPPORTED;
        source_values[i] = instruction->ops[i + 2u];
    }
    return GPU_IR_OK;
}

static int dxil_float_minmax_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                            uint32_t *operation, uint32_t *flags,
                                            uint32_t source_values[2]) {
    if (!builder || !instruction || !operation || !flags || !source_values) return GPU_IR_INVALID;
    int dxop = dxil_dxop(builder->module, builder->function, instruction);
    switch (dxop) {
    case 35: *operation = GPU_IR_SHADER_ARITH_MAXIMUM; break;
    case 36: *operation = GPU_IR_SHADER_ARITH_MINIMUM; break;
    default: return GPU_IR_NOT_FOUND;
    }
    if (instruction->op != DXI_CALL || instruction->flags || instruction->n != 4 || !instruction->ops)
        return GPU_IR_INVALID;
    int rc = call_signature_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    uint32_t actual_opcode;
    if (!value_i32(builder, instruction->ops[1], &actual_opcode) || actual_opcode != (uint32_t)dxop)
        return GPU_IR_INVALID;
    rc = resource_result_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    if (!dxil_float32_shape(builder->module, instruction->type, 1)) return GPU_IR_UNSUPPORTED;
    for (uint32_t i = 0; i < 2; i++) {
        const dxil_value *argument = dxil_value_at(builder->module, builder->function, instruction->ops[i + 2u]);
        if (!argument) return GPU_IR_INVALID;
        if (argument->type != instruction->type || !dxil_float32_shape(builder->module, argument->type, 1))
            return GPU_IR_UNSUPPORTED;
        source_values[i] = instruction->ops[i + 2u];
    }
    *flags = GPU_IR_SHADER_FLAG_FLOAT | GPU_IR_SHADER_FLAG_MINMAX_ORDERED_SELECT |
        GPU_IR_SHADER_FLAG_MINMAX_IGNORE_SINGLE_NAN | GPU_IR_SHADER_FLAG_MINMAX_FLUSH_DENORMS;
    return GPU_IR_OK;
}

// Normalize DXIL's overloaded unary intrinsics to the operation and arity used by the common shader IR.
static int dxil_unary_intrinsic_info(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                    uint32_t *common_opcode, uint32_t *operation,
                                    uint32_t *common_flags, uint32_t *source_value) {
    if (!builder || !instruction || !common_opcode || !operation || !common_flags || !source_value)
        return GPU_IR_INVALID;
    int dxop = dxil_dxop(builder->module, builder->function, instruction);
    uint32_t selected_opcode = GPU_IR_SHADER_OP_ARITHMETIC;
    uint32_t selected_operation;
    switch (dxop) {
    case 6: selected_operation = GPU_IR_SHADER_ARITH_ABSOLUTE; break;
    case 7: selected_operation = GPU_IR_SHADER_ARITH_SATURATE; break;
    case 8: selected_opcode = GPU_IR_SHADER_OP_COMPARE; selected_operation = GPU_IR_SHADER_COMPARE_IS_NAN; break;
    case 9: selected_opcode = GPU_IR_SHADER_OP_COMPARE; selected_operation = GPU_IR_SHADER_COMPARE_IS_INF; break;
    case 10: selected_opcode = GPU_IR_SHADER_OP_COMPARE; selected_operation = GPU_IR_SHADER_COMPARE_IS_FINITE; break;
    case 11: selected_opcode = GPU_IR_SHADER_OP_COMPARE; selected_operation = GPU_IR_SHADER_COMPARE_IS_NORMAL; break;
    case 12: selected_operation = GPU_IR_SHADER_ARITH_COS; break;
    case 13: selected_operation = GPU_IR_SHADER_ARITH_SIN; break;
    case 14: selected_operation = GPU_IR_SHADER_ARITH_TAN; break;
    case 15: selected_operation = GPU_IR_SHADER_ARITH_ACOS; break;
    case 16: selected_operation = GPU_IR_SHADER_ARITH_ASIN; break;
    case 17: selected_operation = GPU_IR_SHADER_ARITH_ATAN; break;
    case 18: selected_operation = GPU_IR_SHADER_ARITH_COSH; break;
    case 19: selected_operation = GPU_IR_SHADER_ARITH_SINH; break;
    case 20: selected_operation = GPU_IR_SHADER_ARITH_TANH; break;
    case 21: selected_operation = GPU_IR_SHADER_ARITH_EXP2; break;
    case 22: selected_operation = GPU_IR_SHADER_ARITH_FRACT; break;
    case 23: selected_operation = GPU_IR_SHADER_ARITH_LOG2; break;
    case 24: selected_operation = GPU_IR_SHADER_ARITH_SQRT; break;
    case 25: selected_operation = GPU_IR_SHADER_ARITH_INVERSE_SQRT; break;
    case 26: selected_operation = GPU_IR_SHADER_ARITH_ROUND_EVEN; break;
    case 27: selected_operation = GPU_IR_SHADER_ARITH_FLOOR; break;
    case 28: selected_operation = GPU_IR_SHADER_ARITH_CEILING; break;
    case 29: selected_operation = GPU_IR_SHADER_ARITH_TRUNC; break;
    default: return GPU_IR_NOT_FOUND;
    }
    if (instruction->op != DXI_CALL || instruction->flags || instruction->n != 3 || !instruction->ops)
        return GPU_IR_INVALID;
    int rc = call_signature_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    uint32_t actual_opcode;
    if (!value_i32(builder, instruction->ops[1], &actual_opcode) || actual_opcode != (uint32_t)dxop)
        return GPU_IR_INVALID;
    rc = resource_result_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    const dxil_value *argument = dxil_value_at(builder->module, builder->function, instruction->ops[2]);
    const dxil_type *result_type = source_type(builder->module, instruction->type);
    if (!argument || !result_type) return GPU_IR_INVALID;
    if (selected_opcode == GPU_IR_SHADER_OP_COMPARE) {
        if (!dxil_float32_shape(builder->module, argument->type, 1) ||
            !dxil_bool_shape_matches(builder->module, argument->type, instruction->type))
            return GPU_IR_UNSUPPORTED;
    } else if (!dxil_float32_shape(builder->module, argument->type, 1) || argument->type != instruction->type) {
        return GPU_IR_UNSUPPORTED;
    }
    *common_opcode = selected_opcode;
    *operation = selected_operation;
    *common_flags = GPU_IR_SHADER_FLAG_FLOAT;
    *source_value = instruction->ops[2];
    return GPU_IR_OK;
}

static int resource_handle_index(const dxil_ir_builder *builder, uint32_t value_id, uint32_t *resource_index) {
    if (value_id >= builder->value_count || !builder->handle_resource_indices) return GPU_IR_INVALID;
    uint32_t index = builder->handle_resource_indices[value_id];
    if (index == UINT32_MAX || index >= builder->resource_count) return GPU_IR_UNSUPPORTED;
    *resource_index = index;
    return GPU_IR_OK;
}

static int analyze_resource_instructions(dxil_ir_builder *builder) {
    int rc = parse_resource_metadata(builder);
    if (rc != GPU_IR_OK) return rc;
    for (uint32_t i = 0; i < builder->value_count; i++) builder->handle_resource_indices[i] = UINT32_MAX;
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        const dxil_inst *instruction = &builder->function->insts[i];
        if (instruction->op != DXI_CALL) continue;
        int dxop = dxil_dxop(builder->module, builder->function, instruction);
        if (dxop != 57 && dxop != 66 && dxop != 68 && dxop != 69 && dxop != 72 && dxop != 78 && dxop != 79 &&
            dxop != 139 && dxop != 140)
            continue;
        rc = call_signature_valid(builder, instruction);
        if (rc != GPU_IR_OK) return rc;
        if (dxop == 57) {
            if (instruction->n != 6 || resource_result_valid(builder, instruction) != GPU_IR_OK)
                return GPU_IR_INVALID;
            const dxil_type *handle_type = source_type(builder->module, instruction->type);
            if (!handle_type || handle_type->kind != DXT_STRUCT || !handle_type->name ||
                strcmp(handle_type->name, "dx.types.Handle")) return GPU_IR_INVALID;
            uint32_t opcode, cls, range, index, nonuniform, resource_index;
            if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != 57 ||
                !value_i32_or_null(builder, instruction->ops[2], &cls) || cls > 3 ||
                !value_i32_or_null(builder, instruction->ops[3], &range) ||
                !value_i32_or_null(builder, instruction->ops[4], &index) ||
                !value_i32_or_null(builder, instruction->ops[5], &nonuniform)) return GPU_IR_UNSUPPORTED;
            if (!value_has_integer_width(builder, instruction->ops[2], 8) ||
                !value_has_integer_width(builder, instruction->ops[3], 32) ||
                !value_has_integer_width(builder, instruction->ops[4], 32) ||
                !value_has_integer_width(builder, instruction->ops[5], 1)) return GPU_IR_INVALID;
            if (index != 0 || nonuniform != 0) return GPU_IR_UNSUPPORTED;
            rc = resource_index_by_range(builder, cls, range, &resource_index);
            if (rc != GPU_IR_OK) return rc == GPU_IR_NOT_FOUND ? GPU_IR_INVALID : rc;
            builder->handle_resource_indices[instruction->result] = resource_index;
            builder->resource_values[instruction->result] = (dxil_ir_resource_value){
                .kind = DXIL_IR_RESOURCE_HANDLE, .resource_index = resource_index
            };
        } else if (dxop == 66) {
            uint32_t opcode, resource_index;
            if (instruction->n != 10 || resource_result_valid(builder, instruction) != GPU_IR_OK)
                return GPU_IR_INVALID;
            if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != 66)
                return GPU_IR_INVALID;
            rc = resource_handle_index(builder, instruction->ops[2], &resource_index);
            if (rc != GPU_IR_OK) return rc;
            dxil_ir_resource *resource = &builder->resources[resource_index];
            if (!resource_is_common_uint2d_srv(resource) && !resource_is_common_sint2d_srv(resource) &&
                !resource_is_common_float2d_srv(resource) && !resource_is_common_float2d_ms_srv(resource) &&
                !resource_is_common_float2d_ms_array_srv(resource))
                return GPU_IR_UNSUPPORTED;
            const dxil_value *handle = dxil_value_at(builder->module, builder->function, instruction->ops[2]);
            const dxil_type *handle_type = handle ? source_type(builder->module, handle->type) : NULL;
            const dxil_type *result_type = source_type(builder->module, instruction->type);
            if (!handle_type || handle_type->kind != DXT_STRUCT || !handle_type->name ||
                strcmp(handle_type->name, "dx.types.Handle")) return GPU_IR_INVALID;
            if (!result_type || result_type->kind != DXT_STRUCT || result_type->count != 5 ||
                !result_type->members) return GPU_IR_INVALID;
            for (uint32_t field = 0; field < result_type->count; field++) {
                const dxil_type *field_type = source_type(builder->module, result_type->members[field]);
                if (!field_type || (field < 4 ? !resource_type_matches_component(resource, field_type) :
                    field_type->kind != DXT_INT || field_type->bits != 32))
                    return GPU_IR_UNSUPPORTED;
            }
            for (uint32_t operand = 3; operand < 10; operand++)
                if (!value_has_integer_width(builder, instruction->ops[operand], 32)) return GPU_IR_INVALID;
            const dxil_value *coord_z = dxil_value_at(builder->module, builder->function, instruction->ops[6]);
            const dxil_value *offset_z = dxil_value_at(builder->module, builder->function, instruction->ops[9]);
            if (!coord_z || ((resource->kind == 2 || resource->kind == 3) && coord_z->kind != DXV_CONST_UNDEF) ||
                !offset_z || offset_z->kind != DXV_CONST_UNDEF) return GPU_IR_UNSUPPORTED;
            if ((resource->kind == 4 || resource->kind == 7) &&
                !value_has_integer_width(builder, instruction->ops[6], 32))
                return GPU_IR_UNSUPPORTED;
            int32_t offsets[2] = { 0, 0 };
            for (uint32_t axis = 0; axis < 2; axis++) {
                uint32_t operand = instruction->ops[7u + axis];
                const dxil_value *offset = dxil_value_at(builder->module, builder->function, operand);
                if (!offset) return GPU_IR_INVALID;
                if (offset->kind == DXV_CONST_UNDEF || offset->kind == DXV_CONST_NULL) continue;
                uint32_t bits;
                if (!value_i32(builder, operand, &bits)) return GPU_IR_UNSUPPORTED;
                memcpy(&offsets[axis], &bits, sizeof bits);
                // TextureLoad offsets are static signed integers restricted to [-8, 7] by DXIL.
                if (offsets[axis] < -8 || offsets[axis] > 7) return GPU_IR_UNSUPPORTED;
            }
            builder->resource_values[instruction->result] = (dxil_ir_resource_value){
                .kind = DXIL_IR_RESOURCE_IMAGE_LOAD,
                .resource_index = resource_index,
                .mip_level_value_id = (resource->kind == 3 || resource->kind == 4) ? 0 : instruction->ops[3],
                .sample_index_value_id = (resource->kind == 3 || resource->kind == 4) ? instruction->ops[3] : 0,
                .x_value_id = instruction->ops[4],
                .y_value_id = instruction->ops[5],
                .layer_value_id = (resource->kind == 4 || resource->kind == 7) ? instruction->ops[6] : 0,
                .offset_x = offsets[0],
                .offset_y = offsets[1]
            };
            resource->used = 1;
        } else if (dxop == 68 || dxop == 72 || dxop == 139) {
            uint32_t opcode, resource_index;
            uint32_t expected_count = dxop == 68 ? 5u : dxop == 139 ? 7u : 4u;
            if (instruction->n != expected_count || resource_result_valid(builder, instruction) != GPU_IR_OK)
                return GPU_IR_INVALID;
            if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != (uint32_t)dxop)
                return GPU_IR_INVALID;
            rc = resource_handle_index(builder, instruction->ops[2], &resource_index);
            if (rc != GPU_IR_OK) return rc;
            dxil_ir_resource *resource = &builder->resources[resource_index];
            int raw_buffer_load = dxop == 68 || dxop == 139;
            int raw_buffer_resource = raw_buffer_load && resource_is_common_raw_buffer(resource);
            int structured_buffer_load = dxop == 68 && resource_is_common_structured_buffer(resource);
            int raw_buffer_dimension = dxop == 72 &&
                (resource_is_common_raw_buffer(resource) || resource_is_common_raw_buffer_uav(resource));
            int structured_buffer_dimension = dxop == 72 && resource_is_common_structured_buffer(resource);
            int buffer_resource = dxop == 139 ? raw_buffer_resource : dxop == 72 ?
                (resource_is_common_r32_buffer(resource) || raw_buffer_dimension || structured_buffer_dimension) :
                (resource_is_common_r32_buffer(resource) || raw_buffer_resource || structured_buffer_load);
            int image_resource = dxop == 72 && (resource_is_common_float2d_srv(resource) ||
                                                 resource_is_common_float2d_ms_srv(resource));
            if (!buffer_resource && !image_resource) return GPU_IR_UNSUPPORTED;
            const dxil_value *handle = dxil_value_at(builder->module, builder->function, instruction->ops[2]);
            const dxil_type *handle_type = handle ? source_type(builder->module, handle->type) : NULL;
            if (!handle_type || handle_type->kind != DXT_STRUCT || !handle_type->name ||
                strcmp(handle_type->name, "dx.types.Handle")) return GPU_IR_INVALID;
            const dxil_type *result_type = source_type(builder->module, instruction->type);
            uint32_t fields = dxop == 72 ? 4u : 5u;
            if (!result_type || result_type->kind != DXT_STRUCT || result_type->count != fields ||
                !result_type->members) return GPU_IR_INVALID;
            for (uint32_t field = 0; field < fields; field++) {
                const dxil_type *field_type = source_type(builder->module, result_type->members[field]);
                int component_field = dxop != 72 && field < 4;
                int structured_component = structured_buffer_load && component_field &&
                    field_type &&
                    (field_type->kind == DXT_FLOAT ||
                     (field_type->kind == DXT_INT && field_type->bits == 32));
                if (!field_type || (component_field ?
                    (!structured_component && !resource_type_matches_component(resource, field_type)) :
                    field_type->kind != DXT_INT || field_type->bits != 32))
                    return GPU_IR_UNSUPPORTED;
            }
            uint32_t index_value = 0;
            uint32_t mip_level_value = 0;
            uint32_t buffer_component_mask = 1u;
            uint32_t structured_byte_offset = 0;
            if (raw_buffer_load) {
                uint32_t offset_operand = 4u;
                const dxil_value *offset = dxil_value_at(builder->module, builder->function,
                                                          instruction->ops[offset_operand]);
                const dxil_value *index = dxil_value_at(builder->module, builder->function, instruction->ops[3]);
                const dxil_type *index_type = index ? source_type(builder->module, index->type) : NULL;
                if (!offset || !index || !index_type || index_type->kind != DXT_INT || index_type->bits != 32 ||
                    !value_has_integer_width(builder, instruction->ops[offset_operand], 32))
                    return GPU_IR_UNSUPPORTED;
                if (structured_buffer_load) {
                    if (index->kind == DXV_CONST_UNDEF ||
                        !value_i32_or_null(builder, instruction->ops[offset_operand], &structured_byte_offset) ||
                        (structured_byte_offset & 3u) || structured_byte_offset > resource->structure_stride - 4u)
                        return GPU_IR_UNSUPPORTED;
                } else if (offset->kind != DXV_CONST_UNDEF) return GPU_IR_UNSUPPORTED;
                if (dxop == 139) {
                    uint32_t mask, alignment;
                    const dxil_value *element_offset = dxil_value_at(builder->module, builder->function,
                                                                      instruction->ops[4]);
                    if (!element_offset || element_offset->kind != DXV_CONST_UNDEF ||
                        !value_has_integer_width(builder, instruction->ops[4], 32) ||
                        !value_i32(builder, instruction->ops[5], &mask) || !mask || (mask & ~15u) ||
                        !value_has_integer_width(builder, instruction->ops[5], 8) ||
                        !value_i32(builder, instruction->ops[6], &alignment) || alignment != 4u ||
                        !value_has_integer_width(builder, instruction->ops[6], 32))
                        return GPU_IR_UNSUPPORTED;
                    buffer_component_mask = mask;
                }
                index_value = instruction->ops[3];
            } else {
                const dxil_value *mip = dxil_value_at(builder->module, builder->function, instruction->ops[3]);
                if (!mip || !value_has_integer_width(builder, instruction->ops[3], 32))
                    return GPU_IR_UNSUPPORTED;
                if (image_resource) {
                    if (resource_is_common_float2d_ms_srv(resource)) {
                        if (mip->kind != DXV_CONST_UNDEF) return GPU_IR_UNSUPPORTED;
                    } else {
                        mip_level_value = instruction->ops[3];
                    }
                } else if (mip->kind != DXV_CONST_UNDEF) return GPU_IR_UNSUPPORTED;
            }
            builder->resource_values[instruction->result] = (dxil_ir_resource_value){
                .kind = raw_buffer_load ? DXIL_IR_RESOURCE_BUFFER_LOAD : image_resource ?
                    DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS : DXIL_IR_RESOURCE_GET_DIMENSIONS,
                .resource_index = resource_index, .index_value_id = index_value,
                .buffer_load_opcode = (uint32_t)dxop, .buffer_component_mask = buffer_component_mask,
                .mip_level_value_id = resource_is_common_float2d_ms_srv(resource) ? 0 : mip_level_value,
                .structured_byte_offset = structured_byte_offset
            };
            resource->used = 1;
            if (structured_buffer_dimension) resource->structured_dimensions = 1;
            if (structured_buffer_load) resource->structured_loads = 1;
        } else if (dxop == 78) {
            uint32_t opcode, operation, resource_index;
            if (instruction->n != 8 || resource_result_valid(builder, instruction) != GPU_IR_OK)
                return GPU_IR_INVALID;
            if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != 78 ||
                !value_i32(builder, instruction->ops[3], &operation)) return GPU_IR_INVALID;
            if (!dxil_atomic_operation(operation)) return GPU_IR_UNSUPPORTED;
            rc = resource_handle_index(builder, instruction->ops[2], &resource_index);
            if (rc != GPU_IR_OK) return rc;
            dxil_ir_resource *resource = &builder->resources[resource_index];
            const dxil_type *result_type = source_type(builder->module, instruction->type);
            const dxil_value *coord1 = dxil_value_at(builder->module, builder->function, instruction->ops[5]);
            const dxil_value *coord2 = dxil_value_at(builder->module, builder->function, instruction->ops[6]);
            if (!resource_is_common_r32_buffer(resource) || resource->register_class != 1 ||
                (resource->component_type != DXIL_IR_COMPONENT_U32 &&
                 resource->component_type != DXIL_IR_COMPONENT_S32) ||
                resource->kind != 10 || resource->count != 1)
                return GPU_IR_UNSUPPORTED;
            if (!value_has_integer_width(builder, instruction->ops[4], 32) ||
                !value_has_integer_width(builder, instruction->ops[7], 32) ||
                !result_type || result_type->kind != DXT_INT || result_type->bits != 32 ||
                !coord1 || coord1->kind != DXV_CONST_UNDEF || !coord2 || coord2->kind != DXV_CONST_UNDEF)
                return GPU_IR_UNSUPPORTED;
            builder->atomics[i].present = 1;
            builder->resource_access_ids[i] = 1;
            resource->used = 1;
        } else if (dxop == 79) {
            uint32_t opcode, resource_index;
            if (instruction->n != 8 || resource_result_valid(builder, instruction) != GPU_IR_OK)
                return GPU_IR_INVALID;
            if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != 79)
                return GPU_IR_INVALID;
            rc = resource_handle_index(builder, instruction->ops[2], &resource_index);
            if (rc != GPU_IR_OK) return rc;
            dxil_ir_resource *resource = &builder->resources[resource_index];
            const dxil_type *result_type = source_type(builder->module, instruction->type);
            const dxil_value *coord1 = dxil_value_at(builder->module, builder->function, instruction->ops[4]);
            const dxil_value *coord2 = dxil_value_at(builder->module, builder->function, instruction->ops[5]);
            if (!resource_is_common_r32_buffer(resource) || resource->register_class != 1 ||
                (resource->component_type != DXIL_IR_COMPONENT_U32 &&
                 resource->component_type != DXIL_IR_COMPONENT_S32) ||
                resource->kind != 10 || resource->count != 1)
                return GPU_IR_UNSUPPORTED;
            if (!value_has_integer_width(builder, instruction->ops[3], 32) ||
                !value_has_integer_width(builder, instruction->ops[6], 32) ||
                !value_has_integer_width(builder, instruction->ops[7], 32) ||
                !result_type || result_type->kind != DXT_INT || result_type->bits != 32 ||
                !coord1 || coord1->kind != DXV_CONST_UNDEF ||
                !coord2 || coord2->kind != DXV_CONST_UNDEF)
                return GPU_IR_UNSUPPORTED;
            builder->atomics[i].present = 1;
            builder->resource_access_ids[i] = 1;
            resource->used = 1;
        } else if (dxop == 69) {
            uint32_t opcode, resource_index, mask;
            if (instruction->n != 10 || instruction->result != UINT32_MAX) return GPU_IR_INVALID;
            if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != 69) return GPU_IR_INVALID;
            rc = resource_handle_index(builder, instruction->ops[2], &resource_index);
            if (rc != GPU_IR_OK) return rc;
            dxil_ir_resource *resource = &builder->resources[resource_index];
            int structured_u32 = resource_is_common_structured_u32_buffer(resource);
            if ((!resource_is_common_r32_buffer(resource) && !structured_u32) || resource->register_class != 1)
                return GPU_IR_UNSUPPORTED;
            const dxil_value *index = dxil_value_at(builder->module, builder->function, instruction->ops[3]);
            const dxil_type *index_type = index ? source_type(builder->module, index->type) : NULL;
            const dxil_value *offset = dxil_value_at(builder->module, builder->function, instruction->ops[4]);
            const dxil_value *value = dxil_value_at(builder->module, builder->function, instruction->ops[5]);
            const dxil_type *value_type = value ? source_type(builder->module, value->type) : NULL;
            uint32_t byte_offset = 0;
            // Typed stores and the supported four-byte structured scalar store use offset zero.
            if (!index_type || index_type->kind != DXT_INT || index_type->bits != 32 ||
                !offset || !value_has_integer_width(builder, instruction->ops[4], 32) ||
                (offset->kind != DXV_CONST_UNDEF &&
                 (!value_i32_or_null(builder, instruction->ops[4], &byte_offset) || byte_offset != 0)) ||
                (structured_u32 ? (!value_type || value_type->kind != DXT_INT || value_type->bits != 32) :
                    !resource_type_matches_component(resource, value_type)) ||
                !value_i32_or_null(builder, instruction->ops[9], &mask) ||
                !value_has_integer_width(builder, instruction->ops[9], 8) || (mask & ~15u))
                return GPU_IR_UNSUPPORTED;
            for (uint32_t component = 1; component < 4; component++) {
                const dxil_value *other = dxil_value_at(builder->module, builder->function,
                                                         instruction->ops[5u + component]);
                if (!other || other->type != value->type) return GPU_IR_INVALID;
            }
            if (mask & 1u) {
                resource->used = 1;
                builder->resource_access_ids[i] = 1; // Mark this instruction for a synthetic access-chain ID.
                if (resource->component_type == DXIL_IR_COMPONENT_F32)
                    builder->resource_typed_value_ids[i] = 1;
            }
        } else if (dxop == 140) {
            uint32_t opcode, resource_index, mask, alignment;
            if (instruction->n != 11 || instruction->result != UINT32_MAX) return GPU_IR_INVALID;
            if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != 140) return GPU_IR_INVALID;
            rc = resource_handle_index(builder, instruction->ops[2], &resource_index);
            if (rc != GPU_IR_OK) return rc;
            dxil_ir_resource *resource = &builder->resources[resource_index];
            const dxil_value *offset = dxil_value_at(builder->module, builder->function, instruction->ops[3]);
            const dxil_value *element_offset = dxil_value_at(builder->module, builder->function,
                                                              instruction->ops[4]);
            const dxil_value *first_value = dxil_value_at(builder->module, builder->function, instruction->ops[5]);
            const dxil_type *value_type = first_value ? source_type(builder->module, first_value->type) : NULL;
            if (!resource_is_common_raw_buffer_uav(resource) || !offset ||
                !value_has_integer_width(builder, instruction->ops[3], 32) || !element_offset ||
                element_offset->kind != DXV_CONST_UNDEF ||
                !value_has_integer_width(builder, instruction->ops[4], 32) || !value_type ||
                !resource_type_matches_component(resource, value_type) ||
                (!value_i32(builder, instruction->ops[9], &mask)) ||
                !value_has_integer_width(builder, instruction->ops[9], 8) ||
                (mask != 1u && mask != 3u && mask != 7u && mask != 15u) ||
                !value_i32(builder, instruction->ops[10], &alignment) || alignment != 4u ||
                !value_has_integer_width(builder, instruction->ops[10], 32))
                return GPU_IR_UNSUPPORTED;
            for (uint32_t component = 1; component < 4; component++) {
                const dxil_value *other = dxil_value_at(builder->module, builder->function,
                                                         instruction->ops[5u + component]);
                if (!other || other->type != first_value->type) return GPU_IR_INVALID;
            }
            builder->raw_stores[i] = (dxil_ir_raw_store){
                .present = 1, .mask = (uint8_t)mask, .is_float = value_type->kind == DXT_FLOAT
            };
            resource->used = 1;
        }
    }
    int has_resource = 0, has_float_image_resource = 0, has_uint_image_resource = 0;
    for (uint32_t i = 0; i < builder->resource_count; i++) {
        const dxil_ir_resource *resource = &builder->resources[i];
        has_resource |= resource->used;
        has_float_image_resource |= resource->used && (resource_is_common_float2d_srv(resource) ||
                                                        resource_is_common_float2d_ms_srv(resource) ||
                                                        resource_is_common_float2d_ms_array_srv(resource));
        has_uint_image_resource |= resource->used && resource_is_common_uint2d_srv(resource);
    }
    if (has_resource) {
        if (builder->resource_u32_source_type == UINT32_MAX) return GPU_IR_UNSUPPORTED;
        rc = mark_type(builder, builder->resource_u32_source_type, 0);
        if (rc != GPU_IR_OK) return rc;
    }
    if (has_float_image_resource) {
        if (builder->resource_f32_source_type == UINT32_MAX) return GPU_IR_UNSUPPORTED;
        rc = mark_type(builder, builder->resource_f32_source_type, 0);
        if (rc != GPU_IR_OK) return rc;
    }
    if (has_uint_image_resource) {
        if (builder->resource_u32_source_type == UINT32_MAX) return GPU_IR_UNSUPPORTED;
        rc = mark_type(builder, builder->resource_u32_source_type, 0);
        if (rc != GPU_IR_OK) return rc;
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        const dxil_inst *instruction = &builder->function->insts[i];
        if (instruction->op != DXI_EXTRACTVAL || instruction->n < 2 || !instruction->ops) continue;
        const dxil_ir_resource_value *aggregate = instruction->ops[0] < builder->value_count ?
            &builder->resource_values[instruction->ops[0]] : NULL;
        if (!aggregate || (aggregate->kind != DXIL_IR_RESOURCE_BUFFER_LOAD &&
                           aggregate->kind != DXIL_IR_RESOURCE_GET_DIMENSIONS &&
                           aggregate->kind != DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS &&
                           aggregate->kind != DXIL_IR_RESOURCE_IMAGE_LOAD)) continue;
        if (aggregate->resource_index >= builder->resource_count) return GPU_IR_UNSUPPORTED;
        const dxil_ir_resource *resource = &builder->resources[aggregate->resource_index];
        uint32_t field = instruction->n == 2 ? instruction->ops[1] : UINT32_MAX;
        if (instruction->nvals != 1 || instruction->n != 2 ||
            (aggregate->kind == DXIL_IR_RESOURCE_BUFFER_LOAD ?
                !buffer_load_field_supported(aggregate, resource, field) :
             aggregate->kind == DXIL_IR_RESOURCE_IMAGE_LOAD ? field > 3 :
             aggregate->kind == DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS ?
                ((field != 0 && field != 1 && field != 2 && field != 3) ||
                 (field == 2 && !resource_is_common_float2d_array_srv(resource))) : field != 0))
            return GPU_IR_UNSUPPORTED;
        const dxil_type *result_type = source_type(builder->module, instruction->type);
        if (!result_type ||
            resource_result_valid(builder, instruction) != GPU_IR_OK) return GPU_IR_UNSUPPORTED;
        int structured_buffer_load = aggregate->kind == DXIL_IR_RESOURCE_BUFFER_LOAD &&
            resource_is_common_structured_buffer(resource);
        int result_matches = aggregate->kind == DXIL_IR_RESOURCE_GET_DIMENSIONS ||
            aggregate->kind == DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS ?
            result_type->kind == DXT_INT && result_type->bits == 32 :
            structured_buffer_load ?
                (result_type->kind == DXT_FLOAT ||
                 (result_type->kind == DXT_INT && result_type->bits == 32)) :
            resource_type_matches_component(resource, result_type);
        if (!result_matches) return GPU_IR_UNSUPPORTED;
        if (aggregate->kind == DXIL_IR_RESOURCE_BUFFER_LOAD) {
            builder->resource_access_ids[i] = 1;
            if (structured_buffer_load) {
                builder->resource_structured_base_word_index_ids[i] = 1;
                if (aggregate->structured_byte_offset || field != 0) {
                    builder->resource_structured_word_index_ids[i] = 1;
                    builder->resource_structured_offset_word_constant_ids[i] = 1;
                }
            }
            if (resource_is_common_raw_buffer(resource)) {
                builder->resource_byte_index_ids[i] = 1;
                if (field != 0)
                    builder->resource_byte_base_index_ids[i] = 1;
            }
            if (resource->component_type == DXIL_IR_COMPONENT_F32 ||
                ((resource_is_common_raw_buffer(resource) || structured_buffer_load) &&
                 result_type->kind == DXT_FLOAT))
                builder->resource_typed_value_ids[i] = 1;
        } else if (aggregate->kind == DXIL_IR_RESOURCE_GET_DIMENSIONS &&
                   (resource_is_common_raw_buffer(resource) || resource_is_common_raw_buffer_uav(resource))) {
            builder->resource_raw_length_word_ids[i] = 1;
        } else if (aggregate->kind == DXIL_IR_RESOURCE_GET_DIMENSIONS &&
                   resource_is_common_structured_buffer(resource)) {
            builder->resource_structured_length_word_ids[i] = 1;
        }
    }
    return GPU_IR_OK;
}

static int dxil_system_value_call(dxil_ir_builder *builder, const dxil_inst *instruction,
                                  uint32_t *builtin_index, uint32_t *component, uint32_t *dxil_opcode) {
    if (!builder || !instruction || instruction->op != DXI_CALL || instruction->n < 2 ||
        !instruction->ops || !builtin_index || !component || !dxil_opcode ||
        builder->common_stage != GPU_IR_STAGE_COMPUTE) return GPU_IR_UNSUPPORTED;
    const dxil_value *callee = dxil_value_at(builder->module, builder->function, instruction->ops[0]);
    if (!callee || callee->kind != DXV_FUNCTION || !callee->name) return GPU_IR_UNSUPPORTED;

    // DXIL.rst assigns opcodes 93-96 to ThreadId, GroupId, ThreadIdInGroup and FlattenedThreadIdInGroup.
    uint32_t selected_builtin, expected_opcode, selected_component = 0;
    uint32_t expected_operands;
    if (!strcmp(callee->name, "dx.op.threadId.i32")) {
        selected_builtin = DXIL_IR_BUILTIN_THREAD_ID;
        expected_opcode = 93;
        expected_operands = 3;
    } else if (!strcmp(callee->name, "dx.op.groupId.i32")) {
        selected_builtin = DXIL_IR_BUILTIN_GROUP_ID;
        expected_opcode = 94;
        expected_operands = 3;
    } else if (!strcmp(callee->name, "dx.op.threadIdInGroup.i32")) {
        selected_builtin = DXIL_IR_BUILTIN_THREAD_ID_IN_GROUP;
        expected_opcode = 95;
        expected_operands = 3;
    } else if (!strcmp(callee->name, "dx.op.flattenedThreadIdInGroup.i32")) {
        selected_builtin = DXIL_IR_BUILTIN_FLATTENED_THREAD_ID_IN_GROUP;
        expected_opcode = 96;
        expected_operands = 2;
    } else if (!strcmp(callee->name, "dx.op.waveGetLaneIndex")) {
        selected_builtin = DXIL_IR_BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID;
        expected_opcode = 111;
        expected_operands = 2;
    } else if (!strcmp(callee->name, "dx.op.waveGetLaneCount")) {
        selected_builtin = DXIL_IR_BUILTIN_SUBGROUP_SIZE;
        expected_opcode = 112;
        expected_operands = 2;
    } else return GPU_IR_UNSUPPORTED;
    if (instruction->n != expected_operands) return GPU_IR_INVALID;

    uint32_t callee_type_id = callee->type;
    const dxil_type *callee_type = source_type(builder->module, callee_type_id);
    if (callee_type && callee_type->kind == DXT_POINTER) {
        callee_type_id = callee_type->elem;
        callee_type = source_type(builder->module, callee_type_id);
    }
    if (!callee_type || callee_type->kind != DXT_FUNCTION || callee_type->vararg ||
        callee_type->elem != instruction->type || callee_type->count != expected_operands - 1u ||
        (callee_type->count && !callee_type->members)) return GPU_IR_INVALID;
    for (uint32_t parameter = 0; parameter < callee_type->count; parameter++) {
        const dxil_value *argument = dxil_value_at(builder->module, builder->function,
            instruction->ops[parameter + 1u]);
        const dxil_type *parameter_type = source_type(builder->module, callee_type->members[parameter]);
        if (!argument || argument->type != callee_type->members[parameter] || !parameter_type ||
            parameter_type->kind != DXT_INT || parameter_type->bits != 32) return GPU_IR_INVALID;
    }

    uint32_t actual_opcode;
    if (!value_i32(builder, instruction->ops[1], &actual_opcode)) return GPU_IR_INVALID;
    if (actual_opcode != expected_opcode) return GPU_IR_INVALID;
    if (expected_operands == 3) {
        if (!value_i32_or_null(builder, instruction->ops[2], &selected_component) || selected_component > 2)
            return GPU_IR_INVALID;
    }
    const dxil_type *result_type = source_type(builder->module, instruction->type);
    if (!result_type || result_type->kind != DXT_INT || result_type->bits != 32 ||
        instruction->result == UINT32_MAX || instruction->result >= builder->value_count)
        return GPU_IR_INVALID;
    const dxil_value *result = dxil_value_at(builder->module, builder->function, instruction->result);
    if (!result || result->kind != DXV_INST || result->type != instruction->type ||
        result->u.index >= builder->function->ninsts) return GPU_IR_INVALID;

    *builtin_index = selected_builtin;
    *component = selected_component;
    *dxil_opcode = expected_opcode;
    return GPU_IR_OK;
}

// DXIL WaveActiveAnyTrue/AllTrue vote over active lanes. This common route is compute-only until
// pixel helper-lane participation can be represented and validated independently.
static int dxil_wave_vote_call(const dxil_ir_builder *builder, const dxil_inst *instruction,
                               uint32_t *operation, uint32_t *source_value) {
    if (!builder || !instruction || !operation || !source_value || instruction->op != DXI_CALL ||
        !instruction->ops)
        return GPU_IR_INVALID;
    int dxop = dxil_dxop(builder->module, builder->function, instruction);
    if (dxop != 113 && dxop != 114) return GPU_IR_NOT_FOUND;
    if (builder->common_stage != GPU_IR_STAGE_COMPUTE) return GPU_IR_UNSUPPORTED;
    if (instruction->flags || instruction->n != 3 || !instruction->ops) return GPU_IR_INVALID;
    int rc = call_signature_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    uint32_t actual_opcode = 0;
    if (!value_i32(builder, instruction->ops[1], &actual_opcode) || actual_opcode != (uint32_t)dxop)
        return GPU_IR_INVALID;
    rc = resource_result_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    const dxil_type *result_type = source_type(builder->module, instruction->type);
    const dxil_value *condition = dxil_value_at(builder->module, builder->function, instruction->ops[2]);
    const dxil_type *condition_type = condition ? source_type(builder->module, condition->type) : NULL;
    if (!result_type || result_type->kind != DXT_INT || result_type->bits != 1 || !condition_type ||
        condition_type->kind != DXT_INT || condition_type->bits != 1) return GPU_IR_UNSUPPORTED;
    *operation = dxop == 113 ? GPU_IR_SHADER_ARITH_SUBGROUP_ANY : GPU_IR_SHADER_ARITH_SUBGROUP_ALL;
    *source_value = instruction->ops[2];
    return GPU_IR_OK;
}

// Normalize the integer WaveActiveMin/Max forms for the common subgroup-reduction path.
// The shared IR keeps the LLVM signless i32 representation but records DXIL's signedness in the operation.
static int dxil_wave_integer_minmax_call(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                         uint32_t *operation, uint32_t *source_value) {
    if (!builder || !instruction || !operation || !source_value || instruction->op != DXI_CALL ||
        !instruction->ops) return GPU_IR_INVALID;
    if (dxil_dxop(builder->module, builder->function, instruction) != 119) return GPU_IR_NOT_FOUND;
    if (builder->common_stage != GPU_IR_STAGE_COMPUTE) return GPU_IR_UNSUPPORTED;
    if (instruction->flags || instruction->n != 5) return GPU_IR_INVALID;
    int rc = call_signature_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    uint32_t actual_opcode = 0, wave_operation = 0, signedness = 0;
    if (!value_i32(builder, instruction->ops[1], &actual_opcode) || actual_opcode != 119 ||
        !value_i32(builder, instruction->ops[3], &wave_operation) ||
        !value_i32(builder, instruction->ops[4], &signedness) || signedness > 1)
        return GPU_IR_INVALID;
    // Sum/product are deliberately left on the direct DXIL path until their API-compatible subgroup
    // semantics (including signed wrap and floating-point behavior) are represented in common IR.
    if (wave_operation != 2 && wave_operation != 3) return GPU_IR_NOT_FOUND;
    rc = resource_result_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    const dxil_type *result_type = source_type(builder->module, instruction->type);
    const dxil_value *value = dxil_value_at(builder->module, builder->function, instruction->ops[2]);
    const dxil_type *value_type = value ? source_type(builder->module, value->type) : NULL;
    if (!result_type || result_type->kind != DXT_INT || result_type->bits != 32 || !value_type ||
        value->type != instruction->type || value_type->kind != DXT_INT || value_type->bits != 32)
        return GPU_IR_UNSUPPORTED;
    *operation = wave_operation == 2 ?
        (signedness ? GPU_IR_SHADER_ARITH_SUBGROUP_UMIN : GPU_IR_SHADER_ARITH_SUBGROUP_SMIN) :
        (signedness ? GPU_IR_SHADER_ARITH_SUBGROUP_UMAX : GPU_IR_SHADER_ARITH_SUBGROUP_SMAX);
    *source_value = instruction->ops[2];
    return GPU_IR_OK;
}

// Normalize WaveActiveBitAnd/Or/Xor into the shared subgroup-reduction operations.
static int dxil_wave_integer_bit_call(const dxil_ir_builder *builder, const dxil_inst *instruction,
                                      uint32_t *operation, uint32_t *source_value) {
    if (!builder || !instruction || !operation || !source_value || instruction->op != DXI_CALL ||
        !instruction->ops) return GPU_IR_INVALID;
    if (dxil_dxop(builder->module, builder->function, instruction) != 120) return GPU_IR_NOT_FOUND;
    if (builder->common_stage != GPU_IR_STAGE_COMPUTE) return GPU_IR_UNSUPPORTED;
    if (instruction->flags || instruction->n != 4) return GPU_IR_INVALID;
    int rc = call_signature_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    uint32_t actual_opcode = 0, bit_operation = UINT32_MAX;
    if (!value_i32(builder, instruction->ops[1], &actual_opcode) || actual_opcode != 120 ||
        !value_i32(builder, instruction->ops[3], &bit_operation) || bit_operation > 2)
        return GPU_IR_INVALID;
    rc = resource_result_valid(builder, instruction);
    if (rc != GPU_IR_OK) return rc;
    const dxil_type *result_type = source_type(builder->module, instruction->type);
    const dxil_value *value = dxil_value_at(builder->module, builder->function, instruction->ops[2]);
    const dxil_type *value_type = value ? source_type(builder->module, value->type) : NULL;
    if (!result_type || result_type->kind != DXT_INT || result_type->bits != 32 || !value_type ||
        value->type != instruction->type || value_type->kind != DXT_INT || value_type->bits != 32)
        return GPU_IR_UNSUPPORTED;
    *operation = bit_operation == 0 ? GPU_IR_SHADER_ARITH_SUBGROUP_BIT_AND :
                 bit_operation == 1 ? GPU_IR_SHADER_ARITH_SUBGROUP_BIT_OR :
                                      GPU_IR_SHADER_ARITH_SUBGROUP_BIT_XOR;
    *source_value = instruction->ops[2];
    return GPU_IR_OK;
}

static int dxil_barrier_call(dxil_ir_builder *builder, const dxil_inst *instruction,
                             dxil_ir_barrier *barrier, uint32_t *integer_type) {
    if (!builder || !instruction || !barrier || !integer_type || instruction->op != DXI_CALL ||
        instruction->n != 3 || !instruction->ops || instruction->result != UINT32_MAX ||
        builder->common_stage != GPU_IR_STAGE_COMPUTE) return GPU_IR_UNSUPPORTED;
    const dxil_value *callee = dxil_value_at(builder->module, builder->function, instruction->ops[0]);
    if (!callee || callee->kind != DXV_FUNCTION || !callee->name || strcmp(callee->name, "dx.op.barrier"))
        return GPU_IR_UNSUPPORTED;
    if (dxil_dxop(builder->module, builder->function, instruction) != 80) return GPU_IR_INVALID;

    uint32_t callee_type_id = callee->type;
    const dxil_type *callee_type = source_type(builder->module, callee_type_id);
    if (callee_type && callee_type->kind == DXT_POINTER) {
        callee_type_id = callee_type->elem;
        callee_type = source_type(builder->module, callee_type_id);
    }
    const dxil_type *void_type = source_type(builder->module, instruction->type);
    if (!callee_type || callee_type->kind != DXT_FUNCTION || callee_type->vararg ||
        callee_type->elem != instruction->type || callee_type->count != 2 || !callee_type->members ||
        !void_type || void_type->kind != DXT_VOID) return GPU_IR_INVALID;
    const dxil_value *opcode_value = dxil_value_at(builder->module, builder->function, instruction->ops[1]);
    const dxil_value *mode_value = dxil_value_at(builder->module, builder->function, instruction->ops[2]);
    const dxil_type *opcode_type = opcode_value ? source_type(builder->module, opcode_value->type) : NULL;
    const dxil_type *mode_type = mode_value ? source_type(builder->module, mode_value->type) : NULL;
    if (!opcode_value || !mode_value || opcode_value->type != callee_type->members[0] ||
        mode_value->type != callee_type->members[1] || !opcode_type || opcode_type->kind != DXT_INT ||
        opcode_type->bits != 32 || !mode_type || mode_type->kind != DXT_INT || mode_type->bits != 32)
        return GPU_IR_INVALID;
    uint32_t opcode;
    if (!value_i32(builder, instruction->ops[1], &opcode) || opcode != 80) return GPU_IR_INVALID;
    if (mode_value->kind != DXV_CONST_INT && mode_value->kind != DXV_CONST_NULL) return GPU_IR_UNSUPPORTED;
    uint32_t mode = mode_value->kind == DXV_CONST_NULL ? 0u : (uint32_t)mode_value->u.i;
    if (mode & ~15u) return GPU_IR_UNSUPPORTED;
    memset(barrier, 0, sizeof *barrier);
    barrier->present = 1;
    *integer_type = mode_value->type;
    if (!mode) {
        barrier->no_op = 1;
        return GPU_IR_OK;
    }

    const uint32_t sync = 1u, uav_global = 2u, uav_group = 4u, threadgroup_memory = 8u;
    if ((mode & (uav_global | uav_group)) == (uav_global | uav_group) ||
        ((mode & sync) && !(mode & (uav_global | uav_group | threadgroup_memory))))
        return GPU_IR_INVALID;
    uint32_t memory_scope = (mode & uav_global) ? 1u : 2u; // Device scope, otherwise workgroup scope.
    uint32_t semantics = 0x8u; // AcquireRelease; Barrier is stronger than relaxed memory ordering.
    if (mode & (uav_global | uav_group)) semantics |= 0x40u; // Common Uniform/device-memory class.
    if (mode & threadgroup_memory) semantics |= 0x100u;     // Common Workgroup-memory class.
    barrier->operation = (mode & sync) ? GPU_IR_SHADER_BARRIER_CONTROL : GPU_IR_SHADER_BARRIER_MEMORY;
    barrier->execution_scope = 2u;
    barrier->memory_scope = memory_scope;
    barrier->semantics = semantics;
    return GPU_IR_OK;
}

static int metadata_numthreads(const dxil_module *module, const dxil_function *function, uint32_t size[3]) {
    const dxil_named_md *entries = dxil_named(module, "dx.entryPoints");
    if (!entries || !entries->n) return GPU_IR_UNSUPPORTED;
    for (uint32_t e = 0; e < entries->n; e++) {
        if (entries->nodes[e] >= module->nmd) return GPU_IR_INVALID;
        const dxil_md *entry = &module->md[entries->nodes[e]];
        const dxil_md *function_ref = dxil_md_op(module, entry, 0);
        if (!function_ref || function_ref->kind != DXM_VALUE || function_ref->value != function->value) continue;
        const dxil_md *properties = dxil_md_op(module, entry, 4);
        if (!properties || properties->kind != DXM_NODE) return GPU_IR_UNSUPPORTED;
        for (uint32_t i = 0; i + 1 < properties->n; i += 2) {
            int64_t tag;
            if (!dxil_md_int(module, dxil_md_op(module, properties, i), &tag)) return GPU_IR_INVALID;
            if (tag != 4) continue;
            const dxil_md *dimensions = dxil_md_op(module, properties, i + 1);
            if (!dimensions || dimensions->kind != DXM_NODE || dimensions->n != 3) return GPU_IR_INVALID;
            for (uint32_t axis = 0; axis < 3; axis++) {
                int64_t dimension;
                if (!dxil_md_int(module, dxil_md_op(module, dimensions, axis), &dimension) ||
                    dimension <= 0 || dimension > UINT32_MAX) return GPU_IR_INVALID;
                size[axis] = (uint32_t)dimension;
            }
            return GPU_IR_OK;
        }
        return GPU_IR_UNSUPPORTED;
    }
    return GPU_IR_UNSUPPORTED;
}

static int entry_function_index(const dxil_module *module, uint32_t *function_index) {
    if (!module || !function_index || (module->nfunctions && !module->functions)) return GPU_IR_INVALID;
    const dxil_named_md *entries = dxil_named(module, "dx.entryPoints");
    if (entries && entries->n) {
        if (!entries->nodes || entries->nodes[0] >= module->nmd) return GPU_IR_INVALID;
        const dxil_md *entry = &module->md[entries->nodes[0]];
        const dxil_md *function_ref = dxil_md_op(module, entry, 0);
        if (function_ref && function_ref->kind == DXM_VALUE) {
            for (uint32_t i = 0; i < module->nfunctions; i++) {
                if (module->functions[i].value == function_ref->value && !module->functions[i].is_declaration) {
                    *function_index = i;
                    return GPU_IR_OK;
                }
            }
            return GPU_IR_INVALID;
        }
    }
    for (uint32_t i = 0; i < module->nfunctions; i++) {
        if (!module->functions[i].is_declaration) {
            *function_index = i;
            return GPU_IR_OK;
        }
    }
    return GPU_IR_UNSUPPORTED;
}

static int build_types(dxil_ir_builder *builder) {
    const dxil_module *module = builder->module;
    uint32_t member_count = 0, type_count = 0;
    for (uint32_t i = 0; i < module->ntypes; i++) if (builder->needed_types[i]) {
        const dxil_type *source = &module->types[i];
        if (source->kind == DXT_FUNCTION) {
            if (source->count > UINT32_MAX - member_count) return GPU_IR_RANGE;
            member_count += source->count;
        }
        type_count++;
    }
    int has_buffer_resources = 0, has_image_resources = 0;
    for (uint32_t i = 0; i < builder->resource_count; i++) {
        const dxil_ir_resource *resource = &builder->resources[i];
        has_buffer_resources |= resource->used && resource_is_common_buffer_backing(resource);
        has_image_resources |= resource->used && resource_is_common_image2d_or_ms_srv(resource);
    }
    if (has_buffer_resources) {
        if (member_count == UINT32_MAX) return GPU_IR_RANGE;
        member_count++;
    }
    if (builder->synthetic_type_count > UINT32_MAX - type_count) return GPU_IR_RANGE;
    type_count += builder->synthetic_type_count;
    builder->types = calloc(type_count ? type_count : 1, sizeof *builder->types);
    builder->members = calloc(member_count ? member_count : 1, sizeof *builder->members);
    if (!builder->types || !builder->members) return GPU_IR_OUT_OF_MEMORY;
    builder->member_count = member_count;
    uint32_t next_member = 0;
    for (uint32_t i = 0; i < module->ntypes; i++) {
        if (!builder->needed_types[i]) continue;
        const dxil_type *source = &module->types[i];
        gpu_ir_shader_type *target = &builder->types[builder->type_count++];
        target->id = builder->type_ids[i];
        switch (source->kind) {
        case DXT_VOID: target->kind = GPU_IR_SHADER_TYPE_VOID; break;
        case DXT_INT:
            target->kind = source->bits == 1 ? GPU_IR_SHADER_TYPE_BOOL : GPU_IR_SHADER_TYPE_INT;
            target->bit_width = source->bits;
            break;
        case DXT_HALF: target->kind = GPU_IR_SHADER_TYPE_FLOAT; target->bit_width = 16; break;
        case DXT_FLOAT: target->kind = GPU_IR_SHADER_TYPE_FLOAT; target->bit_width = 32; break;
        case DXT_DOUBLE: target->kind = GPU_IR_SHADER_TYPE_FLOAT; target->bit_width = 64; break;
        case DXT_VECTOR:
            target->kind = GPU_IR_SHADER_TYPE_VECTOR;
            target->element_type_id = common_type_id(builder, source->elem);
            target->element_count = source->count;
            break;
        case DXT_POINTER:
            target->kind = GPU_IR_SHADER_TYPE_POINTER;
            target->element_type_id = common_type_id(builder, source->elem);
            target->source_storage_class = source->bits;
            target->storage_class = source->bits == 3 ? GPU_IR_SHADER_STORAGE_WORKGROUP :
                source->bits == 0 ? GPU_IR_SHADER_STORAGE_FUNCTION : GPU_IR_SHADER_STORAGE_UNMAPPED;
            break;
        case DXT_FUNCTION:
            target->kind = GPU_IR_SHADER_TYPE_FUNCTION;
            target->element_type_id = common_type_id(builder, source->elem);
            target->first_member = next_member;
            target->member_count = source->count;
            for (uint32_t p = 0; p < source->count; p++)
                builder->members[next_member++].type_id = common_type_id(builder, source->members[p]);
            break;
        default: return GPU_IR_UNSUPPORTED;
        }
    }
    if (next_member + (has_buffer_resources ? 1u : 0u) != builder->member_count) return GPU_IR_INVALID;
    if (builder->builtin_vector_type_id) {
        gpu_ir_shader_type *vector = &builder->types[builder->type_count++];
        *vector = (gpu_ir_shader_type){
            .id = builder->builtin_vector_type_id,
            .kind = GPU_IR_SHADER_TYPE_VECTOR,
            .element_type_id = common_type_id(builder, builder->builtin_scalar_type),
            .element_count = 3
        };
        gpu_ir_shader_type *pointer = &builder->types[builder->type_count++];
        *pointer = (gpu_ir_shader_type){
            .id = builder->builtin_vector_pointer_type_id,
            .kind = GPU_IR_SHADER_TYPE_POINTER,
            .element_type_id = builder->builtin_vector_type_id,
            .storage_class = GPU_IR_SHADER_STORAGE_INPUT
        };
    }
    if (builder->builtin_scalar_pointer_type_id) {
        gpu_ir_shader_type *pointer = &builder->types[builder->type_count++];
        *pointer = (gpu_ir_shader_type){
            .id = builder->builtin_scalar_pointer_type_id,
            .kind = GPU_IR_SHADER_TYPE_POINTER,
            .element_type_id = common_type_id(builder, builder->builtin_scalar_type),
            .storage_class = GPU_IR_SHADER_STORAGE_INPUT
        };
    }
    if (builder->color_output.present) {
        gpu_ir_shader_type *vector = &builder->types[builder->type_count++];
        *vector = (gpu_ir_shader_type){
            .id = builder->color_output.vector_type_id,
            .kind = GPU_IR_SHADER_TYPE_VECTOR,
            .element_type_id = common_type_id(builder, builder->color_output.scalar_type_id),
            .element_count = 4
        };
        gpu_ir_shader_type *pointer = &builder->types[builder->type_count++];
        *pointer = (gpu_ir_shader_type){
            .id = builder->color_output.pointer_type_id,
            .kind = GPU_IR_SHADER_TYPE_POINTER,
            .element_type_id = builder->color_output.vector_type_id,
            .storage_class = GPU_IR_SHADER_STORAGE_OUTPUT,
            .source_storage_class = 3
        };
    }
    if (builder->vertex_position_output.present) {
        gpu_ir_shader_type *vector = &builder->types[builder->type_count++];
        *vector = (gpu_ir_shader_type){
            .id = builder->vertex_position_output.vector_type_id,
            .kind = GPU_IR_SHADER_TYPE_VECTOR,
            .element_type_id = common_type_id(builder, builder->vertex_position_output.scalar_type_id),
            .element_count = 4
        };
        gpu_ir_shader_type *pointer = &builder->types[builder->type_count++];
        *pointer = (gpu_ir_shader_type){
            .id = builder->vertex_position_output.pointer_type_id,
            .kind = GPU_IR_SHADER_TYPE_POINTER,
            .element_type_id = builder->vertex_position_output.vector_type_id,
            .storage_class = GPU_IR_SHADER_STORAGE_OUTPUT,
            .source_storage_class = 3
        };
    }
    if (builder->fragment_position.present) {
        gpu_ir_shader_type *vector = &builder->types[builder->type_count++];
        *vector = (gpu_ir_shader_type){
            .id = builder->fragment_position.vector_type_id,
            .kind = GPU_IR_SHADER_TYPE_VECTOR,
            .element_type_id = common_type_id(builder, builder->fragment_position.scalar_type_id),
            .element_count = 4
        };
        gpu_ir_shader_type *pointer = &builder->types[builder->type_count++];
        *pointer = (gpu_ir_shader_type){
            .id = builder->fragment_position.pointer_type_id,
            .kind = GPU_IR_SHADER_TYPE_POINTER,
            .element_type_id = builder->fragment_position.vector_type_id,
            .storage_class = GPU_IR_SHADER_STORAGE_INPUT
        };
    }
    if (builder->front_facing.present) {
        gpu_ir_shader_type *pointer = &builder->types[builder->type_count++];
        *pointer = (gpu_ir_shader_type){
            .id = builder->front_facing.pointer_type_id,
            .kind = GPU_IR_SHADER_TYPE_POINTER,
            .element_type_id = common_type_id(builder, builder->front_facing.scalar_type_id),
            .storage_class = GPU_IR_SHADER_STORAGE_INPUT
        };
    }
    for (uint32_t i = 0; i < DXIL_IR_VERTEX_SYSTEM_VALUE_COUNT; i++) {
        dxil_ir_vertex_system_value *system_value = &builder->vertex_system_values[i];
        if (!system_value->present) continue;
        gpu_ir_shader_type *pointer = &builder->types[builder->type_count++];
        *pointer = (gpu_ir_shader_type){
            .id = system_value->pointer_type_id,
            .kind = GPU_IR_SHADER_TYPE_POINTER,
            .element_type_id = common_type_id(builder, system_value->scalar_type_id),
            .storage_class = GPU_IR_SHADER_STORAGE_INPUT,
            .source_storage_class = 1
        };
    }
    for (uint32_t i = 0; i < builder->vertex_input_count; i++) {
        dxil_ir_vertex_input *input = &builder->vertex_inputs[i];
        if (input->columns > 1) {
            gpu_ir_shader_type *vector = &builder->types[builder->type_count++];
            *vector = (gpu_ir_shader_type){
                .id = input->vector_type_id,
                .kind = GPU_IR_SHADER_TYPE_VECTOR,
                .element_type_id = common_type_id(builder, input->scalar_type_id),
                .element_count = input->columns
            };
        }
        gpu_ir_shader_type *pointer = &builder->types[builder->type_count++];
        *pointer = (gpu_ir_shader_type){
            .id = input->pointer_type_id,
            .kind = GPU_IR_SHADER_TYPE_POINTER,
            .element_type_id = input->vector_type_id,
            .storage_class = GPU_IR_SHADER_STORAGE_INPUT,
            .source_storage_class = 1
        };
    }
    if (has_buffer_resources) {
        gpu_ir_shader_type *array = &builder->types[builder->type_count++];
        *array = (gpu_ir_shader_type){
            .id = builder->buffer_array_type_id,
            .kind = GPU_IR_SHADER_TYPE_RUNTIME_ARRAY,
            .element_type_id = common_type_id(builder, builder->resource_u32_source_type),
            .array_stride = 4,
            .has_array_stride = 1
        };
        gpu_ir_shader_type *block = &builder->types[builder->type_count++];
        *block = (gpu_ir_shader_type){
            .id = builder->buffer_block_type_id,
            .kind = GPU_IR_SHADER_TYPE_STRUCT,
            .first_member = next_member,
            .member_count = 1,
            .decoration_flags = GPU_IR_SHADER_TYPE_FLAG_BLOCK
        };
        builder->members[next_member++] = (gpu_ir_shader_member){
            .type_id = builder->buffer_array_type_id,
            .offset = 0,
            .has_offset = 1
        };
        gpu_ir_shader_type *block_pointer = &builder->types[builder->type_count++];
        *block_pointer = (gpu_ir_shader_type){
            .id = builder->buffer_block_pointer_type_id,
            .kind = GPU_IR_SHADER_TYPE_POINTER,
            .element_type_id = builder->buffer_block_type_id,
            .storage_class = GPU_IR_SHADER_STORAGE_BUFFER
        };
        gpu_ir_shader_type *scalar_pointer = &builder->types[builder->type_count++];
        *scalar_pointer = (gpu_ir_shader_type){
            .id = builder->buffer_scalar_pointer_type_id,
            .kind = GPU_IR_SHADER_TYPE_POINTER,
            .element_type_id = common_type_id(builder, builder->resource_u32_source_type),
            .storage_class = GPU_IR_SHADER_STORAGE_BUFFER
        };
    }
    if (has_image_resources) {
        gpu_ir_shader_type *size_vector = &builder->types[builder->type_count++];
        *size_vector = (gpu_ir_shader_type){
            .id = builder->image_query_size_type_id,
            .kind = GPU_IR_SHADER_TYPE_VECTOR,
            .element_type_id = common_type_id(builder, builder->resource_u32_source_type),
            .element_count = 2
        };
        int has_sint_image_resource = 0;
        for (uint32_t i = 0; i < builder->resource_count; i++) {
            const dxil_ir_resource *resource = &builder->resources[i];
            has_sint_image_resource |= resource->used && resource_is_common_sint2d_srv(resource);
        }
        if (has_sint_image_resource) {
            if (!builder->image_s32_type_id) return GPU_IR_INVALID;
            builder->types[builder->type_count++] = (gpu_ir_shader_type){
                .id = builder->image_s32_type_id,
                .kind = GPU_IR_SHADER_TYPE_INT,
                .bit_width = 32,
                .signedness = 1
            };
        }
        for (uint32_t i = 0; i < builder->resource_count; i++) {
            const dxil_ir_resource *resource = &builder->resources[i];
            if (!resource->used || !resource_is_common_image2d_or_ms_srv(resource)) continue;
            uint32_t image_source_type = resource->component_type == DXIL_IR_COMPONENT_F32 ?
                builder->resource_f32_source_type : builder->resource_u32_source_type;
            if (resource->component_type == DXIL_IR_COMPONENT_S32) image_source_type = builder->image_s32_type_id;
            if (image_source_type == UINT32_MAX || !image_source_type) return GPU_IR_UNSUPPORTED;
            gpu_ir_shader_type *image = &builder->types[builder->type_count++];
            *image = (gpu_ir_shader_type){
                .id = resource->image_type_id,
                .kind = GPU_IR_SHADER_TYPE_IMAGE,
                .element_type_id = resource->component_type == DXIL_IR_COMPONENT_S32 ?
                    builder->image_s32_type_id : common_type_id(builder, image_source_type),
                .image_dimension = 1,
                .image_arrayed = resource->kind == 4 || resource->kind == 7,
                .image_multisampled = resource->kind == 3 || resource->kind == 4,
                .image_sampled = 1
            };
            gpu_ir_shader_type *pointer = &builder->types[builder->type_count++];
            *pointer = (gpu_ir_shader_type){
                .id = resource->image_pointer_type_id,
                .kind = GPU_IR_SHADER_TYPE_POINTER,
                .element_type_id = resource->image_type_id,
                .storage_class = GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT
            };
        }
    }
    return builder->type_count == type_count ? GPU_IR_OK : GPU_IR_INVALID;
}

static int append_constant(dxil_ir_builder *builder, uint32_t source_id) {
    const dxil_value *source = dxil_value_at(builder->module, builder->function, source_id);
    if (!source || (source->kind != DXV_CONST_INT && source->kind != DXV_CONST_FLOAT &&
                    source->kind != DXV_CONST_NULL)) return GPU_IR_UNSUPPORTED;
    const dxil_type *type = source_type(builder->module, source->type);
    uint32_t literal_count = 0, raw[2] = { 0, 0 }, operation;
    if (!type) return GPU_IR_INVALID;
    if (source->kind == DXV_CONST_NULL) {
        if (type->kind == DXT_INT) {
            if (!type->bits || type->bits > 64) return GPU_IR_UNSUPPORTED;
            operation = type->bits == 1 ? GPU_IR_SHADER_CONSTANT_BOOL : GPU_IR_SHADER_CONSTANT_SCALAR;
            literal_count = type->bits == 1 ? 1u : (type->bits + 31u) / 32u;
        } else if (type->kind == DXT_FLOAT || type->kind == DXT_DOUBLE) {
            operation = GPU_IR_SHADER_CONSTANT_SCALAR;
            literal_count = type->kind == DXT_DOUBLE ? 2u : 1u;
        } else return GPU_IR_UNSUPPORTED;
    } else if (source->kind == DXV_CONST_INT) {
        if (type->kind != DXT_INT || !type->bits || type->bits > 64) return GPU_IR_UNSUPPORTED;
        uint64_t bits = (uint64_t)source->u.i;
        if (type->bits < 64) bits &= (UINT64_C(1) << type->bits) - 1;
        if (type->bits == 1) {
            operation = GPU_IR_SHADER_CONSTANT_BOOL;
            raw[0] = bits != 0;
            literal_count = 1;
        } else {
            operation = GPU_IR_SHADER_CONSTANT_SCALAR;
            raw[0] = (uint32_t)bits;
            literal_count = (type->bits + 31u) / 32u;
            if (literal_count > 1) raw[1] = (uint32_t)(bits >> 32);
        }
    } else if (type->kind == DXT_FLOAT) {
        // LLVM float values are retained as doubles, even when their source type is float.
        float narrowed = (float)source->u.f;
        memcpy(raw, &narrowed, sizeof narrowed);
        operation = GPU_IR_SHADER_CONSTANT_SCALAR;
        literal_count = 1;
    } else if (type->kind == DXT_DOUBLE) {
        memcpy(raw, &source->u.f, sizeof source->u.f);
        operation = GPU_IR_SHADER_CONSTANT_SCALAR;
        literal_count = 2;
    } else return GPU_IR_UNSUPPORTED;

    uint32_t first_literal = builder->literal_count;
    for (uint32_t i = 0; i < literal_count; i++) {
        int rc = append_literal(builder, raw[i]);
        if (rc != GPU_IR_OK) return rc;
    }
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_CONSTANT,
        .operation = operation,
        .source_opcode = 0,
        .result_id = common_value_id(builder, source_id),
        .result_type_id = common_type_id(builder, source->type),
        .function_index = UINT32_MAX,
        .block_index = UINT32_MAX,
        .first_literal = first_literal,
        .literal_word_count = literal_count
    };
    return append_instruction(builder, &instruction);
}

static uint32_t arithmetic_operation(const dxil_type *type, uint32_t sub) {
    int is_float = type && (type->kind == DXT_FLOAT || type->kind == DXT_HALF || type->kind == DXT_DOUBLE);
    if (is_float) {
        switch (sub) {
        case 0: return GPU_IR_SHADER_ARITH_ADD;
        case 1: return GPU_IR_SHADER_ARITH_SUBTRACT;
        case 2: return GPU_IR_SHADER_ARITH_MULTIPLY;
        case 3: case 4: return GPU_IR_SHADER_ARITH_DIVIDE;
        case 6: return GPU_IR_SHADER_ARITH_REMAINDER;
        default: return 0;
        }
    }
    switch (sub) {
    case 0: return GPU_IR_SHADER_ARITH_ADD;
    case 1: return GPU_IR_SHADER_ARITH_SUBTRACT;
    case 2: return GPU_IR_SHADER_ARITH_MULTIPLY;
    case 3: case 4: return GPU_IR_SHADER_ARITH_DIVIDE;
    case 5: case 6: return GPU_IR_SHADER_ARITH_REMAINDER;
    case 7: return GPU_IR_SHADER_ARITH_SHIFT_LEFT;
    case 8: return GPU_IR_SHADER_ARITH_SHIFT_RIGHT_LOGICAL;
    case 9: return GPU_IR_SHADER_ARITH_SHIFT_RIGHT_ARITHMETIC;
    case 10: return GPU_IR_SHADER_ARITH_BIT_AND;
    case 11: return GPU_IR_SHADER_ARITH_BIT_OR;
    case 12: return GPU_IR_SHADER_ARITH_BIT_XOR;
    default: return 0;
    }
}

static uint32_t compare_operation(const dxil_type *type, uint32_t predicate) {
    int is_float = type && (type->kind == DXT_FLOAT || type->kind == DXT_HALF || type->kind == DXT_DOUBLE);
    if (type && type->kind == DXT_VECTOR) is_float = 0;
    if (is_float) {
        static const uint8_t operations[16] = {
            0,
            GPU_IR_SHADER_COMPARE_ORDERED_EQUAL,
            GPU_IR_SHADER_COMPARE_ORDERED_GREATER,
            GPU_IR_SHADER_COMPARE_ORDERED_GREATER_EQUAL,
            GPU_IR_SHADER_COMPARE_ORDERED_LESS,
            GPU_IR_SHADER_COMPARE_ORDERED_LESS_EQUAL,
            GPU_IR_SHADER_COMPARE_ORDERED_NOT_EQUAL,
            GPU_IR_SHADER_COMPARE_ORDERED,
            GPU_IR_SHADER_COMPARE_UNORDERED,
            GPU_IR_SHADER_COMPARE_UNORDERED_EQUAL,
            GPU_IR_SHADER_COMPARE_UNORDERED_GREATER,
            GPU_IR_SHADER_COMPARE_UNORDERED_GREATER_EQUAL,
            GPU_IR_SHADER_COMPARE_UNORDERED_LESS,
            GPU_IR_SHADER_COMPARE_UNORDERED_LESS_EQUAL,
            GPU_IR_SHADER_COMPARE_UNORDERED_NOT_EQUAL,
            0
        };
        return predicate < 16 ? operations[predicate] : 0;
    }
    switch (predicate) {
    case 32: return GPU_IR_SHADER_COMPARE_EQUAL;
    case 33: return GPU_IR_SHADER_COMPARE_NOT_EQUAL;
    case 34: return GPU_IR_SHADER_COMPARE_GREATER;
    case 35: return GPU_IR_SHADER_COMPARE_GREATER_EQUAL;
    case 36: return GPU_IR_SHADER_COMPARE_LESS;
    case 37: return GPU_IR_SHADER_COMPARE_LESS_EQUAL;
    case 38: return GPU_IR_SHADER_COMPARE_GREATER;
    case 39: return GPU_IR_SHADER_COMPARE_GREATER_EQUAL;
    case 40: return GPU_IR_SHADER_COMPARE_LESS;
    case 41: return GPU_IR_SHADER_COMPARE_LESS_EQUAL;
    default: return 0;
    }
}

static int append_mapped(dxil_ir_builder *builder, const dxil_inst *source, uint32_t block_index,
                         uint32_t opcode, uint32_t operation, uint32_t result_type, uint32_t flags,
                         const uint32_t *source_values, uint32_t source_value_count,
                         const uint32_t *source_labels, uint32_t source_label_count,
                         const uint32_t *literal_values, uint32_t literal_value_count) {
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = opcode,
        .operation = operation,
        .source_opcode = source->op,
        .result_id = source->result == UINT32_MAX ? 0 : common_value_id(builder, source->result),
        .result_type_id = source->result == UINT32_MAX ? 0 : common_type_id(builder, result_type),
        .function_index = 0,
        .block_index = block_index,
        .flags = flags,
        .first_operand = builder->operand_count,
        .first_literal = builder->literal_count
    };
    int rc = GPU_IR_OK;
    for (uint32_t i = 0; rc == GPU_IR_OK && i < source_value_count; i++) {
        uint32_t id = common_value_id(builder, source_values[i]);
        if (!id) return GPU_IR_INVALID;
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, id);
    }
    for (uint32_t i = 0; rc == GPU_IR_OK && i < source_label_count; i++) {
        uint32_t id = common_label_id(builder, source_labels[i]);
        if (!id) return GPU_IR_INVALID;
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, id);
    }
    for (uint32_t i = 0; rc == GPU_IR_OK && i < literal_value_count; i++)
        rc = append_literal(builder, literal_values[i]);
    if (rc != GPU_IR_OK) return rc;
    instruction.operand_count = builder->operand_count - instruction.first_operand;
    instruction.literal_word_count = builder->literal_count - instruction.first_literal;
    return append_instruction(builder, &instruction);
}

static int append_resource_access_chain_with_common_index(dxil_ir_builder *builder,
                                                          const dxil_ir_resource *resource,
                                                          uint32_t access_chain_id,
                                                          uint32_t common_index_id,
                                                          uint32_t block_index) {
    if (!resource || !resource->variable_id || !access_chain_id || !builder->buffer_index_constant_id ||
        !common_index_id || block_index >= builder->block_count) return GPU_IR_INVALID;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_ACCESS_CHAIN,
        .source_opcode = DXI_CALL,
        .result_id = access_chain_id,
        .result_type_id = builder->buffer_scalar_pointer_type_id,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, resource->variable_id);
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, builder->buffer_index_constant_id);
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, common_index_id);
    if (rc != GPU_IR_OK) return rc;
    instruction.operand_count = builder->operand_count - instruction.first_operand;
    return append_instruction(builder, &instruction);
}

static int append_resource_access_chain(dxil_ir_builder *builder, const dxil_ir_resource *resource,
                                        uint32_t access_chain_id, uint32_t index_value_id,
                                        uint32_t block_index) {
    if (index_value_id >= builder->value_count) return GPU_IR_INVALID;
    return append_resource_access_chain_with_common_index(builder, resource, access_chain_id,
        common_value_id(builder, index_value_id), block_index);
}

static int append_raw_buffer_word_index(dxil_ir_builder *builder, uint32_t byte_offset_value_id,
                                        uint32_t word_index_id, uint32_t block_index) {
    if (!builder || byte_offset_value_id >= builder->value_count || !word_index_id ||
        !builder->buffer_byte_shift_constant_id || block_index >= builder->block_count)
        return GPU_IR_INVALID;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_ARITHMETIC,
        .operation = GPU_IR_SHADER_ARITH_SHIFT_RIGHT_LOGICAL,
        .source_opcode = DXI_CALL,
        .result_id = word_index_id,
        .result_type_id = common_type_id(builder, builder->resource_u32_source_type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, common_value_id(builder, byte_offset_value_id));
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, builder->buffer_byte_shift_constant_id);
    if (rc != GPU_IR_OK || !instruction.result_type_id) return rc == GPU_IR_OK ? GPU_IR_INVALID : rc;
    instruction.operand_count = builder->operand_count - instruction.first_operand;
    return append_instruction(builder, &instruction);
}

static int append_raw_buffer_component_word_index(dxil_ir_builder *builder, uint32_t base_word_index_id,
                                                  uint32_t lane, uint32_t word_index_id,
                                                  uint32_t block_index) {
    if (!builder || !base_word_index_id || lane == 0 || lane >= 4 ||
        !builder->buffer_byte_lane_constant_ids[lane] || !word_index_id ||
        block_index >= builder->block_count) return GPU_IR_INVALID;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_ARITHMETIC,
        .operation = GPU_IR_SHADER_ARITH_ADD,
        .source_opcode = DXI_CALL,
        .result_id = word_index_id,
        .result_type_id = common_type_id(builder, builder->resource_u32_source_type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, base_word_index_id);
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, builder->buffer_byte_lane_constant_ids[lane]);
    if (rc != GPU_IR_OK || !instruction.result_type_id) return rc == GPU_IR_OK ? GPU_IR_INVALID : rc;
    instruction.operand_count = builder->operand_count - instruction.first_operand;
    return append_instruction(builder, &instruction);
}

static int append_structured_buffer_word_index(dxil_ir_builder *builder,
                                               const dxil_ir_resource *resource,
                                               const dxil_ir_resource_value *resource_value,
                                               uint32_t byte_offset,
                                               uint32_t base_word_index_id, uint32_t word_index_id,
                                               uint32_t offset_word_constant_id, uint32_t block_index) {
    if (!builder || !resource || !resource_value || resource_value->index_value_id >= builder->value_count ||
        !resource->structure_stride_words_constant_id || !base_word_index_id || block_index >= builder->block_count ||
        (byte_offset && (!word_index_id || !offset_word_constant_id)))
        return GPU_IR_INVALID;
    gpu_ir_shader_instruction multiply = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_ARITHMETIC,
        .operation = GPU_IR_SHADER_ARITH_MULTIPLY,
        .source_opcode = DXI_CALL,
        .result_id = base_word_index_id,
        .result_type_id = common_type_id(builder, builder->resource_u32_source_type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID,
                            common_value_id(builder, resource_value->index_value_id));
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, resource->structure_stride_words_constant_id);
    if (rc != GPU_IR_OK || !multiply.result_type_id) return rc == GPU_IR_OK ? GPU_IR_INVALID : rc;
    multiply.operand_count = builder->operand_count - multiply.first_operand;
    rc = append_instruction(builder, &multiply);
    if (rc != GPU_IR_OK || !byte_offset) return rc;
    gpu_ir_shader_instruction add_offset = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_ARITHMETIC,
        .operation = GPU_IR_SHADER_ARITH_ADD,
        .source_opcode = DXI_CALL,
        .result_id = word_index_id,
        .result_type_id = common_type_id(builder, builder->resource_u32_source_type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, base_word_index_id);
    if (rc == GPU_IR_OK) rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, offset_word_constant_id);
    if (rc != GPU_IR_OK || !add_offset.result_type_id) return rc == GPU_IR_OK ? GPU_IR_INVALID : rc;
    add_offset.operand_count = builder->operand_count - add_offset.first_operand;
    return append_instruction(builder, &add_offset);
}

static int append_resource_image_load(dxil_ir_builder *builder, const dxil_inst *source,
                                      const dxil_ir_resource_value *resource_value, uint32_t block_index) {
    if (!builder || !source || !resource_value || resource_value->kind != DXIL_IR_RESOURCE_IMAGE_LOAD ||
        resource_value->resource_index >= builder->resource_count || !resource_value->image_value_id)
        return GPU_IR_INVALID;
    const dxil_ir_resource *resource = &builder->resources[resource_value->resource_index];
    gpu_ir_shader_instruction load = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_LOAD,
        .source_opcode = DXI_CALL,
        .result_id = resource_value->image_value_id,
        .result_type_id = resource->image_type_id,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, resource->variable_id);
    if (rc != GPU_IR_OK) return rc;
    load.operand_count = builder->operand_count - load.first_operand;
    rc = append_instruction(builder, &load);
    if (rc != GPU_IR_OK) return rc;

    int multisampled = resource->kind == 3 || resource->kind == 4;
    gpu_ir_shader_instruction read = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_IMAGE_READ,
        .operation = multisampled ? GPU_IR_SHADER_IMAGE_READ_2D_MSAA_OOB :
            GPU_IR_SHADER_IMAGE_READ_2D_LOD_OOB,
        .source_opcode = 66,
        .result_id = common_value_id(builder, source->result),
        .result_type_id = common_type_id(builder, source->type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .first_literal = builder->literal_count
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, resource_value->image_value_id);
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID,
                            common_value_id(builder, resource_value->x_value_id));
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID,
                            common_value_id(builder, resource_value->y_value_id));
    if (rc == GPU_IR_OK && resource->kind == 4)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID,
                            common_value_id(builder, resource_value->layer_value_id));
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID,
                            common_value_id(builder, multisampled ? resource_value->sample_index_value_id :
                                resource_value->mip_level_value_id));
    if (rc == GPU_IR_OK && resource->kind == 7)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID,
                            common_value_id(builder, resource_value->layer_value_id));
    if (rc == GPU_IR_OK) rc = append_literal(builder, (uint32_t)resource_value->offset_x);
    if (rc == GPU_IR_OK) rc = append_literal(builder, (uint32_t)resource_value->offset_y);
    if (rc == GPU_IR_OK) rc = append_literal(builder, source->ops[1]);
    if (rc != GPU_IR_OK) return rc;
    read.operand_count = builder->operand_count - read.first_operand;
    read.literal_word_count = builder->literal_count - read.first_literal;
    return append_instruction(builder, &read);
}

static int append_resource_extract(dxil_ir_builder *builder, const dxil_inst *source, uint32_t block_index) {
    if (!source || source->op != DXI_EXTRACTVAL || !source->ops || source->n != 2 ||
        source->ops[0] >= builder->value_count) return GPU_IR_INVALID;
    const dxil_ir_resource_value *resource_value = &builder->resource_values[source->ops[0]];
    if ((resource_value->kind != DXIL_IR_RESOURCE_BUFFER_LOAD &&
         resource_value->kind != DXIL_IR_RESOURCE_GET_DIMENSIONS &&
         resource_value->kind != DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS &&
         resource_value->kind != DXIL_IR_RESOURCE_IMAGE_LOAD) ||
        resource_value->resource_index >= builder->resource_count) return GPU_IR_UNSUPPORTED;
    if (resource_value->kind == DXIL_IR_RESOURCE_IMAGE_LOAD) {
        if (source->ops[1] > 3) return GPU_IR_UNSUPPORTED;
        return append_resource_image_load(builder, source, resource_value, block_index);
    }
    const dxil_ir_resource *resource = &builder->resources[resource_value->resource_index];
    if (resource_value->kind == DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS) {
        uint32_t field = source->ops[1];
        gpu_ir_shader_instruction instruction = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .source_opcode = DXI_EXTRACTVAL,
            .function_index = 0,
            .block_index = block_index,
            .first_operand = builder->operand_count,
            .first_literal = builder->literal_count
        };
        int rc = GPU_IR_OK;
        if (field == 2 && resource_is_common_float2d_array_srv(resource)) {
            instruction.opcode = GPU_IR_SHADER_OP_IMAGE_QUERY;
            instruction.operation = GPU_IR_SHADER_IMAGE_QUERY_ARRAY_SIZE;
            instruction.result_id = common_value_id(builder, source->result);
            instruction.result_type_id = common_type_id(builder, source->type);
            rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, resource->image_value_id);
        } else if (field == 2) return GPU_IR_UNSUPPORTED;
        else if (field == 3) {
            instruction.opcode = GPU_IR_SHADER_OP_IMAGE_QUERY;
            instruction.operation = resource_is_common_float2d_ms_srv(resource) ?
                GPU_IR_SHADER_IMAGE_QUERY_SAMPLES : GPU_IR_SHADER_IMAGE_QUERY_LEVELS;
            instruction.result_id = common_value_id(builder, source->result);
            instruction.result_type_id = common_type_id(builder, source->type);
            rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, resource->image_value_id);
        } else if (field == 0 || field == 1) {
            instruction.opcode = GPU_IR_SHADER_OP_COMPOSITE;
            instruction.operation = GPU_IR_SHADER_COMPOSITE_EXTRACT;
            instruction.result_id = common_value_id(builder, source->result);
            instruction.result_type_id = common_type_id(builder, source->type);
            rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID,
                                common_value_id(builder, source->ops[0]));
            if (rc == GPU_IR_OK) rc = append_literal(builder, field);
        } else return GPU_IR_UNSUPPORTED;
        if (rc != GPU_IR_OK) return rc;
        instruction.operand_count = builder->operand_count - instruction.first_operand;
        instruction.literal_word_count = builder->literal_count - instruction.first_literal;
        return append_instruction(builder, &instruction);
    }
    uint32_t buffer_field = source->ops[1];
    if (resource_value->kind == DXIL_IR_RESOURCE_BUFFER_LOAD) {
        if (!buffer_load_field_supported(resource_value, resource, buffer_field)) return GPU_IR_UNSUPPORTED;
    } else if (buffer_field != 0) return GPU_IR_UNSUPPORTED;
    if (resource_value->kind == DXIL_IR_RESOURCE_GET_DIMENSIONS) {
        int raw_buffer = resource_is_common_raw_buffer(resource) ||
            resource_is_common_raw_buffer_uav(resource);
        int structured_buffer = resource_is_common_structured_buffer(resource);
        ptrdiff_t instruction_index = source - builder->function->insts;
        if (instruction_index < 0 || (uint64_t)instruction_index >= builder->function->ninsts)
            return GPU_IR_INVALID;
        uint32_t result_id = raw_buffer ? builder->resource_raw_length_word_ids[instruction_index] :
            structured_buffer ? builder->resource_structured_length_word_ids[instruction_index] :
            common_value_id(builder, source->result);
        if (!result_id) return GPU_IR_INVALID;
        gpu_ir_shader_instruction instruction = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .opcode = GPU_IR_SHADER_OP_ARRAY_LENGTH,
            .source_opcode = DXI_EXTRACTVAL,
            .result_id = result_id,
            .result_type_id = raw_buffer || structured_buffer ?
                common_type_id(builder, builder->resource_u32_source_type) :
                common_type_id(builder, source->type),
            .function_index = 0,
            .block_index = block_index,
            .first_operand = builder->operand_count,
            .first_literal = builder->literal_count
        };
        int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, resource->variable_id);
        if (rc == GPU_IR_OK) rc = append_literal(builder, 0);
        if (rc != GPU_IR_OK) return rc;
        instruction.operand_count = builder->operand_count - instruction.first_operand;
        instruction.literal_word_count = builder->literal_count - instruction.first_literal;
        rc = append_instruction(builder, &instruction);
        if (rc != GPU_IR_OK || (!raw_buffer && !structured_buffer)) return rc;
        if (raw_buffer && !builder->buffer_byte_length_word_constant_id) return GPU_IR_INVALID;
        if (structured_buffer && !resource->structure_stride_words_constant_id) return GPU_IR_INVALID;
        gpu_ir_shader_instruction scale = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .opcode = GPU_IR_SHADER_OP_ARITHMETIC,
            .operation = raw_buffer ? GPU_IR_SHADER_ARITH_MULTIPLY : GPU_IR_SHADER_ARITH_DIVIDE,
            .source_opcode = DXI_EXTRACTVAL,
            .result_id = common_value_id(builder, source->result),
            .result_type_id = common_type_id(builder, source->type),
            .function_index = 0,
            .block_index = block_index,
            .first_operand = builder->operand_count
        };
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, result_id);
        if (rc == GPU_IR_OK)
            rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, raw_buffer ?
                builder->buffer_byte_length_word_constant_id : resource->structure_stride_words_constant_id);
        if (rc != GPU_IR_OK) return rc;
        scale.operand_count = builder->operand_count - scale.first_operand;
        return append_instruction(builder, &scale);
    }
    ptrdiff_t instruction_index = source - builder->function->insts;
    if (instruction_index < 0 || (uint64_t)instruction_index >= builder->function->ninsts)
        return GPU_IR_INVALID;
    uint32_t access_chain_id = builder->resource_access_ids[instruction_index];
    uint32_t common_index_id = common_value_id(builder, resource_value->index_value_id);
    int structured_buffer = resource_is_common_structured_buffer(resource);
    int rc = GPU_IR_OK;
    if (structured_buffer) {
        uint32_t base_word_index_id = builder->resource_structured_base_word_index_ids[instruction_index];
        uint32_t word_index_id = builder->resource_structured_word_index_ids[instruction_index];
        uint32_t offset_constant_id = builder->resource_structured_offset_word_constant_ids[instruction_index];
        uint32_t structured_byte_offset = resource_value->structured_byte_offset + buffer_field * 4u;
        if (!base_word_index_id || (structured_byte_offset &&
            (!word_index_id || !offset_constant_id))) return GPU_IR_INVALID;
        rc = append_structured_buffer_word_index(builder, resource, resource_value, structured_byte_offset,
                                                 base_word_index_id, word_index_id, offset_constant_id,
                                                 block_index);
        common_index_id = structured_byte_offset ? word_index_id : base_word_index_id;
    } else if (resource_is_common_raw_buffer(resource)) {
        uint32_t word_index_id = builder->resource_byte_index_ids[instruction_index];
        if (!word_index_id) return GPU_IR_INVALID;
        uint32_t base_word_index_id = word_index_id;
        if (buffer_field != 0) {
            base_word_index_id = builder->resource_byte_base_index_ids[instruction_index];
            if (!base_word_index_id) return GPU_IR_INVALID;
        }
        rc = append_raw_buffer_word_index(builder, resource_value->index_value_id,
                                          base_word_index_id, block_index);
        if (rc == GPU_IR_OK && buffer_field != 0)
            rc = append_raw_buffer_component_word_index(builder, base_word_index_id, buffer_field,
                                                        word_index_id, block_index);
        common_index_id = word_index_id;
    }
    if (rc == GPU_IR_OK)
        rc = append_resource_access_chain_with_common_index(builder, resource, access_chain_id,
                                                             common_index_id, block_index);
    if (rc != GPU_IR_OK) return rc;
    const dxil_type *result_type = source_type(builder->module, source->type);
    int is_float = resource->component_type == DXIL_IR_COMPONENT_F32 ||
        ((resource_is_common_raw_buffer(resource) || structured_buffer) &&
         result_type && result_type->kind == DXT_FLOAT);
    uint32_t loaded_value_id = common_value_id(builder, source->result);
    uint32_t load_result_id = is_float ? builder->resource_typed_value_ids[instruction_index] : loaded_value_id;
    if (!load_result_id) return GPU_IR_INVALID;
    gpu_ir_shader_instruction load = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_LOAD,
        .source_opcode = DXI_CALL,
        .result_id = load_result_id,
        .result_type_id = common_type_id(builder, builder->resource_u32_source_type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, access_chain_id);
    if (rc != GPU_IR_OK) return rc;
    load.operand_count = builder->operand_count - load.first_operand;
    rc = append_instruction(builder, &load);
    if (rc != GPU_IR_OK || !is_float) return rc;
    gpu_ir_shader_instruction bitcast = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_CONVERT,
        .operation = GPU_IR_SHADER_CONVERT_BITCAST,
        .source_opcode = DXI_EXTRACTVAL,
        .result_id = loaded_value_id,
        .result_type_id = common_type_id(builder, source->type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, load_result_id);
    if (rc != GPU_IR_OK) return rc;
    bitcast.operand_count = builder->operand_count - bitcast.first_operand;
    return append_instruction(builder, &bitcast);
}

static int append_resource_image_query_size(dxil_ir_builder *builder, const dxil_inst *source,
                                            uint32_t block_index) {
    if (!builder || !source || source->op != DXI_CALL || source->result >= builder->value_count)
        return GPU_IR_INVALID;
    const dxil_ir_resource_value *resource_value = &builder->resource_values[source->result];
    if (resource_value->kind != DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS ||
        resource_value->resource_index >= builder->resource_count || !builder->image_query_size_type_id)
        return GPU_IR_UNSUPPORTED;
    const dxil_ir_resource *resource = &builder->resources[resource_value->resource_index];
    gpu_ir_shader_instruction load = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_LOAD,
        .source_opcode = source->op,
        .result_id = resource->image_value_id,
        .result_type_id = resource->image_type_id,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, resource->variable_id);
    if (rc != GPU_IR_OK) return rc;
    load.operand_count = builder->operand_count - load.first_operand;
    rc = append_instruction(builder, &load);
    if (rc != GPU_IR_OK) return rc;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_IMAGE_QUERY,
        .operation = GPU_IR_SHADER_IMAGE_QUERY_SIZE,
        .source_opcode = source->op,
        .result_id = common_value_id(builder, source->result),
        .result_type_id = builder->image_query_size_type_id,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, resource->image_value_id);
    if (rc != GPU_IR_OK) return rc;
    if (!resource_is_common_float2d_ms_srv(resource)) {
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID,
                            common_value_id(builder, resource_value->mip_level_value_id));
        if (rc != GPU_IR_OK) return rc;
    }
    instruction.operand_count = builder->operand_count - instruction.first_operand;
    return append_instruction(builder, &instruction);
}

static int append_resource_store(dxil_ir_builder *builder, const dxil_inst *source, uint32_t block_index) {
    ptrdiff_t instruction_index = source - builder->function->insts;
    if (instruction_index < 0 || (uint64_t)instruction_index >= builder->function->ninsts || source->n != 10)
        return GPU_IR_INVALID;
    uint32_t access_chain_id = builder->resource_access_ids[instruction_index];
    if (!access_chain_id) return GPU_IR_OK;
    uint32_t resource_index;
    int rc = resource_handle_index(builder, source->ops[2], &resource_index);
    if (rc != GPU_IR_OK || resource_index >= builder->resource_count) return rc == GPU_IR_OK ? GPU_IR_INVALID : rc;
    const dxil_ir_resource *resource = &builder->resources[resource_index];
    rc = append_resource_access_chain(builder, resource, access_chain_id, source->ops[3], block_index);
    if (rc != GPU_IR_OK) return rc;
    uint32_t store_value_id = common_value_id(builder, source->ops[5]);
    if (resource->component_type == DXIL_IR_COMPONENT_F32) {
        uint32_t bitcast_id = builder->resource_typed_value_ids[instruction_index];
        if (!bitcast_id) return GPU_IR_INVALID;
        gpu_ir_shader_instruction bitcast = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .opcode = GPU_IR_SHADER_OP_CONVERT,
            .operation = GPU_IR_SHADER_CONVERT_BITCAST,
            .source_opcode = DXI_CALL,
            .result_id = bitcast_id,
            .result_type_id = common_type_id(builder, builder->resource_u32_source_type),
            .function_index = 0,
            .block_index = block_index,
            .first_operand = builder->operand_count
        };
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, store_value_id);
        if (rc != GPU_IR_OK) return rc;
        bitcast.operand_count = builder->operand_count - bitcast.first_operand;
        rc = append_instruction(builder, &bitcast);
        if (rc != GPU_IR_OK) return rc;
        store_value_id = bitcast_id;
    }
    gpu_ir_shader_instruction store = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_STORE,
        .source_opcode = DXI_CALL,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, access_chain_id);
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, store_value_id);
    if (rc != GPU_IR_OK) return rc;
    store.operand_count = builder->operand_count - store.first_operand;
    return append_instruction(builder, &store);
}

static int append_raw_buffer_store(dxil_ir_builder *builder, const dxil_inst *source,
                                   uint32_t block_index) {
    ptrdiff_t instruction_index = source - builder->function->insts;
    if (instruction_index < 0 || (uint64_t)instruction_index >= builder->function->ninsts ||
        source->n != 11) return GPU_IR_INVALID;
    dxil_ir_raw_store *store_info = &builder->raw_stores[instruction_index];
    if (!store_info->present || !store_info->base_word_index_id) return GPU_IR_INVALID;
    uint32_t resource_index;
    int rc = resource_handle_index(builder, source->ops[2], &resource_index);
    if (rc != GPU_IR_OK || resource_index >= builder->resource_count)
        return rc == GPU_IR_OK ? GPU_IR_INVALID : rc;
    const dxil_ir_resource *resource = &builder->resources[resource_index];
    if (!resource_is_common_raw_buffer_uav(resource)) return GPU_IR_UNSUPPORTED;
    rc = append_raw_buffer_word_index(builder, source->ops[3], store_info->base_word_index_id, block_index);
    if (rc != GPU_IR_OK) return rc;
    for (uint32_t lane = 0; lane < 4; lane++) {
        if (!(store_info->mask & (1u << lane))) continue;
        uint32_t word_index_id = lane == 0 ? store_info->base_word_index_id : store_info->word_index_ids[lane];
        if (!store_info->access_ids[lane] || !word_index_id) return GPU_IR_INVALID;
        if (lane != 0) {
            rc = append_raw_buffer_component_word_index(builder, store_info->base_word_index_id, lane,
                                                        word_index_id, block_index);
            if (rc != GPU_IR_OK) return rc;
        }
        rc = append_resource_access_chain_with_common_index(builder, resource, store_info->access_ids[lane],
                                                             word_index_id, block_index);
        if (rc != GPU_IR_OK) return rc;
        uint32_t value_id = common_value_id(builder, source->ops[5u + lane]);
        if (store_info->is_float) {
            uint32_t bitcast_id = store_info->value_ids[lane];
            if (!bitcast_id) return GPU_IR_INVALID;
            gpu_ir_shader_instruction bitcast = {
                .source_api = GPU_IR_SHADER_SOURCE_DXIL,
                .opcode = GPU_IR_SHADER_OP_CONVERT,
                .operation = GPU_IR_SHADER_CONVERT_BITCAST,
                .source_opcode = DXI_CALL,
                .result_id = bitcast_id,
                .result_type_id = common_type_id(builder, builder->resource_u32_source_type),
                .function_index = 0,
                .block_index = block_index,
                .first_operand = builder->operand_count
            };
            rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, value_id);
            if (rc != GPU_IR_OK) return rc;
            bitcast.operand_count = builder->operand_count - bitcast.first_operand;
            rc = append_instruction(builder, &bitcast);
            if (rc != GPU_IR_OK) return rc;
            value_id = bitcast_id;
        }
        gpu_ir_shader_instruction write = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .opcode = GPU_IR_SHADER_OP_STORE,
            .source_opcode = DXI_CALL,
            .function_index = 0,
            .block_index = block_index,
            .first_operand = builder->operand_count
        };
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, store_info->access_ids[lane]);
        if (rc == GPU_IR_OK) rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, value_id);
        if (rc != GPU_IR_OK) return rc;
        write.operand_count = builder->operand_count - write.first_operand;
        rc = append_instruction(builder, &write);
        if (rc != GPU_IR_OK) return rc;
    }
    return GPU_IR_OK;
}

static uint32_t dxil_atomic_operation(uint32_t operation) {
    // DXIL AtomicBinOpCode: Add, And, Or, Xor, IMin, IMax, UMin, UMax, Exchange.
    static const uint32_t operations[] = {
        GPU_IR_SHADER_ATOMIC_INTEGER_ADD,
        GPU_IR_SHADER_ATOMIC_INTEGER_AND,
        GPU_IR_SHADER_ATOMIC_INTEGER_OR,
        GPU_IR_SHADER_ATOMIC_INTEGER_XOR,
        GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MINIMUM,
        GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MAXIMUM,
        GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MINIMUM,
        GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MAXIMUM,
        GPU_IR_SHADER_ATOMIC_INTEGER_EXCHANGE
    };
    return operation < sizeof operations / sizeof operations[0] ? operations[operation] : 0;
}

static int append_resource_atomic(dxil_ir_builder *builder, const dxil_inst *source,
                                  uint32_t block_index) {
    ptrdiff_t instruction_index = source - builder->function->insts;
    if (instruction_index < 0 || (uint64_t)instruction_index >= builder->function->ninsts ||
        !builder->atomics[instruction_index].present || source->n != 8) return GPU_IR_INVALID;
    uint32_t resource_index;
    int rc = resource_handle_index(builder, source->ops[2], &resource_index);
    if (rc != GPU_IR_OK || resource_index >= builder->resource_count)
        return rc == GPU_IR_OK ? GPU_IR_INVALID : rc;
    const dxil_ir_resource *resource = &builder->resources[resource_index];
    const dxil_ir_atomic *atomic = &builder->atomics[instruction_index];
    uint32_t operation;
    uint32_t access_chain_id = builder->resource_access_ids[instruction_index];
    if (!access_chain_id || !atomic->scope_id || !atomic->semantics_id ||
        !value_i32(builder, source->ops[3], &operation) || !dxil_atomic_operation(operation))
        return GPU_IR_INVALID;
    rc = append_resource_access_chain(builder, resource, access_chain_id, source->ops[4], block_index);
    if (rc != GPU_IR_OK) return rc;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_ATOMIC,
        .operation = dxil_atomic_operation(operation),
        .source_opcode = source->op,
        .result_id = common_value_id(builder, source->result),
        .result_type_id = common_type_id(builder, source->type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, access_chain_id);
    if (rc == GPU_IR_OK) rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, atomic->scope_id);
    if (rc == GPU_IR_OK) rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, atomic->semantics_id);
    if (rc == GPU_IR_OK) rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID,
                                               common_value_id(builder, source->ops[7]));
    if (rc != GPU_IR_OK) return rc;
    instruction.operand_count = builder->operand_count - instruction.first_operand;
    return append_instruction(builder, &instruction);
}

static int append_resource_atomic_compare_exchange(dxil_ir_builder *builder, const dxil_inst *source,
                                                    uint32_t block_index) {
    ptrdiff_t instruction_index = source - builder->function->insts;
    if (instruction_index < 0 || (uint64_t)instruction_index >= builder->function->ninsts ||
        !builder->atomics[instruction_index].present || source->n != 8) return GPU_IR_INVALID;
    uint32_t resource_index;
    int rc = resource_handle_index(builder, source->ops[2], &resource_index);
    if (rc != GPU_IR_OK || resource_index >= builder->resource_count)
        return rc == GPU_IR_OK ? GPU_IR_INVALID : rc;
    const dxil_ir_resource *resource = &builder->resources[resource_index];
    const dxil_ir_atomic *atomic = &builder->atomics[instruction_index];
    uint32_t access_chain_id = builder->resource_access_ids[instruction_index];
    if (!access_chain_id || !atomic->scope_id || !atomic->semantics_id) return GPU_IR_INVALID;
    rc = append_resource_access_chain(builder, resource, access_chain_id, source->ops[3], block_index);
    if (rc != GPU_IR_OK) return rc;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_ATOMIC,
        .operation = GPU_IR_SHADER_ATOMIC_INTEGER_COMPARE_EXCHANGE,
        .source_opcode = source->op,
        .result_id = common_value_id(builder, source->result),
        .result_type_id = common_type_id(builder, source->type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, access_chain_id);
    if (rc == GPU_IR_OK) rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, atomic->scope_id);
    if (rc == GPU_IR_OK) rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, atomic->semantics_id);
    if (rc == GPU_IR_OK) rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, atomic->semantics_id);
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, common_value_id(builder, source->ops[6]));
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, common_value_id(builder, source->ops[7]));
    if (rc != GPU_IR_OK) return rc;
    instruction.operand_count = builder->operand_count - instruction.first_operand;
    return append_instruction(builder, &instruction);
}

static uint32_t common_builtin(uint32_t index) {
    static const uint32_t builtins[DXIL_IR_BUILTIN_COUNT] = {
        GPU_IR_SHADER_BUILTIN_GLOBAL_INVOCATION_ID,
        GPU_IR_SHADER_BUILTIN_WORKGROUP_ID,
        GPU_IR_SHADER_BUILTIN_LOCAL_INVOCATION_ID,
        GPU_IR_SHADER_BUILTIN_LOCAL_INVOCATION_INDEX,
        GPU_IR_SHADER_BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID,
        GPU_IR_SHADER_BUILTIN_SUBGROUP_SIZE
    };
    return index < DXIL_IR_BUILTIN_COUNT ? builtins[index] : GPU_IR_SHADER_BUILTIN_UNSPECIFIED;
}

static uint32_t dxil_builtin_opcode(uint32_t index) {
    static const uint32_t opcodes[DXIL_IR_BUILTIN_COUNT] = { 93, 94, 95, 96, 111, 112 };
    return index < DXIL_IR_BUILTIN_COUNT ? opcodes[index] : 0;
}

static int dxil_builtin_is_scalar(uint32_t index) {
    return index == DXIL_IR_BUILTIN_FLATTENED_THREAD_ID_IN_GROUP ||
        index == DXIL_IR_BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID ||
        index == DXIL_IR_BUILTIN_SUBGROUP_SIZE;
}

static int append_builtin_variables(dxil_ir_builder *builder) {
    for (uint32_t index = 0; index < DXIL_IR_BUILTIN_COUNT; index++) {
        if (!(builder->builtin_mask & (1u << index))) continue;
        gpu_ir_shader_instruction instruction = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .opcode = GPU_IR_SHADER_OP_VARIABLE,
            .source_opcode = DXI_CALL,
            .result_id = builder->builtin_variable_ids[index],
            .result_type_id = dxil_builtin_is_scalar(index) ?
                builder->builtin_scalar_pointer_type_id : builder->builtin_vector_pointer_type_id,
            .function_index = UINT32_MAX,
            .block_index = UINT32_MAX,
            .storage_class = GPU_IR_SHADER_STORAGE_INPUT,
            .builtin = common_builtin(index),
            .source_builtin = dxil_builtin_opcode(index),
            .has_builtin = 1
        };
        int rc = append_instruction(builder, &instruction);
        if (rc != GPU_IR_OK) return rc;
    }
    return GPU_IR_OK;
}

static int append_dxil_target0_variable(dxil_ir_builder *builder) {
    if (!builder->color_output.present) return GPU_IR_OK;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_VARIABLE,
        .source_opcode = DXI_CALL,
        .result_id = builder->color_output.variable_id,
        .result_type_id = builder->color_output.pointer_type_id,
        .function_index = UINT32_MAX,
        .block_index = UINT32_MAX,
        .storage_class = GPU_IR_SHADER_STORAGE_OUTPUT,
        .location = 0,
        .has_location = 1,
        .entry_point_interface = 1
    };
    return append_instruction(builder, &instruction);
}

static int append_dxil_vertex_position_variable(dxil_ir_builder *builder) {
    if (!builder->vertex_position_output.present) return GPU_IR_OK;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_VARIABLE,
        .source_opcode = DXI_CALL,
        .result_id = builder->vertex_position_output.variable_id,
        .result_type_id = builder->vertex_position_output.pointer_type_id,
        .function_index = UINT32_MAX,
        .block_index = UINT32_MAX,
        .storage_class = GPU_IR_SHADER_STORAGE_OUTPUT,
        .builtin = GPU_IR_SHADER_BUILTIN_POSITION,
        .source_builtin = 3,
        .has_builtin = 1,
        .entry_point_interface = 1
    };
    return append_instruction(builder, &instruction);
}

static int append_dxil_fragment_position_variable(dxil_ir_builder *builder) {
    if (!builder->fragment_position.present) return GPU_IR_OK;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_VARIABLE,
        .source_opcode = DXI_CALL,
        .result_id = builder->fragment_position.variable_id,
        .result_type_id = builder->fragment_position.pointer_type_id,
        .function_index = UINT32_MAX,
        .block_index = UINT32_MAX,
        .storage_class = GPU_IR_SHADER_STORAGE_INPUT,
        .builtin = GPU_IR_SHADER_BUILTIN_FRAG_COORD,
        .source_builtin = 3,
        .has_builtin = 1,
        .entry_point_interface = 1
    };
    return append_instruction(builder, &instruction);
}

static int append_dxil_fragment_position_load(dxil_ir_builder *builder, const dxil_inst *source,
                                              uint32_t block_index) {
    ptrdiff_t index = source - builder->function->insts;
    if (!builder->fragment_position.present || index < 0 ||
        (uint64_t)index >= builder->function->ninsts || !builder->input_load_ids[index])
        return GPU_IR_INVALID;
    uint32_t component;
    if (!value_i32(builder, source->ops[4], &component) || component > 3) return GPU_IR_INVALID;
    gpu_ir_shader_instruction load = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_LOAD,
        .source_opcode = DXI_CALL,
        .result_id = builder->input_load_ids[index],
        .result_type_id = builder->fragment_position.vector_type_id,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 1
    };
    int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, builder->fragment_position.variable_id);
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &load);
    if (rc != GPU_IR_OK) return rc;
    gpu_ir_shader_instruction extract = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_COMPOSITE,
        .operation = GPU_IR_SHADER_COMPOSITE_EXTRACT,
        .source_opcode = DXI_CALL,
        .result_id = common_value_id(builder, source->result),
        .result_type_id = common_type_id(builder, source->type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 1,
        .first_literal = builder->literal_count,
        .literal_word_count = 1
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, builder->input_load_ids[index]);
    if (rc == GPU_IR_OK) rc = append_literal(builder, component);
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &extract);
    return rc;
}

static int append_dxil_front_facing_variable(dxil_ir_builder *builder) {
    if (!builder->front_facing.present) return GPU_IR_OK;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_VARIABLE,
        .source_opcode = DXI_CALL,
        .result_id = builder->front_facing.variable_id,
        .result_type_id = builder->front_facing.pointer_type_id,
        .function_index = UINT32_MAX,
        .block_index = UINT32_MAX,
        .storage_class = GPU_IR_SHADER_STORAGE_INPUT,
        .builtin = GPU_IR_SHADER_BUILTIN_FRONT_FACING,
        .source_builtin = 13,
        .has_builtin = 1,
        .entry_point_interface = 1
    };
    return append_instruction(builder, &instruction);
}

static int append_dxil_vertex_system_value_variables(dxil_ir_builder *builder) {
    for (uint32_t i = 0; i < DXIL_IR_VERTEX_SYSTEM_VALUE_COUNT; i++) {
        const dxil_ir_vertex_system_value *system_value = &builder->vertex_system_values[i];
        if (!system_value->present) continue;
        gpu_ir_shader_instruction variable = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .opcode = GPU_IR_SHADER_OP_VARIABLE,
            .source_opcode = DXI_CALL,
            .result_id = system_value->variable_id,
            .result_type_id = system_value->pointer_type_id,
            .function_index = UINT32_MAX,
            .block_index = UINT32_MAX,
            .storage_class = GPU_IR_SHADER_STORAGE_INPUT,
            .builtin = system_value->builtin,
            .source_builtin = system_value->semantic_kind,
            .has_builtin = 1,
            .entry_point_interface = 1
        };
        int rc = append_instruction(builder, &variable);
        if (rc != GPU_IR_OK) return rc;
    }
    return GPU_IR_OK;
}

static int append_dxil_vertex_system_value_load(dxil_ir_builder *builder, const dxil_inst *source,
                                                uint32_t block_index) {
    ptrdiff_t index = source - builder->function->insts;
    uint32_t signature_id, system_value_index = 0;
    if (index < 0 || (uint64_t)index >= builder->function->ninsts || source->n != 6 ||
        builder->input_load_ids[index] != UINT32_MAX ||
        !value_i32(builder, source->ops[2], &signature_id)) return GPU_IR_INVALID;
    int rc = dxil_vertex_system_value_signature(builder->module, builder->function, signature_id,
                                                 &system_value_index);
    if (rc != GPU_IR_OK || system_value_index >= DXIL_IR_VERTEX_SYSTEM_VALUE_COUNT)
        return rc == GPU_IR_OK ? GPU_IR_INVALID : rc;
    const dxil_ir_vertex_system_value *system_value = &builder->vertex_system_values[system_value_index];
    if (!system_value->present || system_value->signature_id != signature_id ||
        common_type_id(builder, source->type) != common_type_id(builder, system_value->scalar_type_id))
        return GPU_IR_INVALID;
    gpu_ir_shader_instruction load = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_LOAD,
        .source_opcode = DXI_CALL,
        .result_id = common_value_id(builder, source->result),
        .result_type_id = common_type_id(builder, source->type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 1
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, system_value->variable_id);
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &load);
    return rc;
}

static int append_dxil_front_facing_load(dxil_ir_builder *builder, const dxil_inst *source,
                                         uint32_t block_index) {
    ptrdiff_t index = source - builder->function->insts;
    uint32_t signature_id;
    if (!builder->front_facing.present || index < 0 ||
        (uint64_t)index >= builder->function->ninsts || !builder->input_load_ids[index] ||
        !value_i32(builder, source->ops[2], &signature_id) ||
        signature_id != builder->front_facing.signature_id ||
        common_type_id(builder, source->type) != common_type_id(builder, builder->front_facing.scalar_type_id))
        return GPU_IR_INVALID;
    gpu_ir_shader_instruction load = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_LOAD,
        .source_opcode = DXI_CALL,
        .result_id = common_value_id(builder, source->result),
        .result_type_id = common_type_id(builder, source->type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 1
    };
    int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, builder->front_facing.variable_id);
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &load);
    return rc;
}

static int append_dxil_vertex_input_variable(dxil_ir_builder *builder) {
    for (uint32_t i = 0; i < builder->vertex_input_count; i++) {
        const dxil_ir_vertex_input *input = &builder->vertex_inputs[i];
        gpu_ir_shader_instruction variable = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .opcode = GPU_IR_SHADER_OP_VARIABLE,
            .source_opcode = DXI_CALL,
            .result_id = input->variable_id,
            .result_type_id = input->pointer_type_id,
            .function_index = UINT32_MAX,
            .block_index = UINT32_MAX,
            .storage_class = GPU_IR_SHADER_STORAGE_INPUT,
            .location = input->location,
            .has_location = 1,
            .entry_point_interface = 1
        };
        int rc = append_instruction(builder, &variable);
        if (rc != GPU_IR_OK) return rc;
    }
    return GPU_IR_OK;
}

static int append_dxil_vertex_input_load(dxil_ir_builder *builder, const dxil_inst *source,
                                         uint32_t block_index) {
    ptrdiff_t index = source - builder->function->insts;
    if (index < 0 || (uint64_t)index >= builder->function->ninsts ||
        !builder->input_load_ids[index] || source->n != 6) return GPU_IR_INVALID;
    uint32_t signature_id, component;
    if (!value_i32(builder, source->ops[2], &signature_id) ||
        !value_i32(builder, source->ops[4], &component)) return GPU_IR_INVALID;
    const dxil_ir_vertex_input *input = NULL;
    for (uint32_t i = 0; i < builder->vertex_input_count; i++)
        if (builder->vertex_inputs[i].signature_id == signature_id) input = &builder->vertex_inputs[i];
    if (!input || component >= input->columns) return GPU_IR_INVALID;
    uint32_t load_id = input->columns == 1 ? common_value_id(builder, source->result) :
        builder->input_load_ids[index];
    gpu_ir_shader_instruction load = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_LOAD,
        .source_opcode = DXI_CALL,
        .result_id = load_id,
        .result_type_id = input->vector_type_id,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 1
    };
    int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, input->variable_id);
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &load);
    if (rc != GPU_IR_OK || input->columns == 1) return rc;
    gpu_ir_shader_instruction extract = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_COMPOSITE,
        .operation = GPU_IR_SHADER_COMPOSITE_EXTRACT,
        .source_opcode = DXI_CALL,
        .result_id = common_value_id(builder, source->result),
        .result_type_id = common_type_id(builder, source->type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 1,
        .first_literal = builder->literal_count,
        .literal_word_count = 1
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, load_id);
    if (rc == GPU_IR_OK) rc = append_literal(builder, component);
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &extract);
    return rc;
}

static int append_dxil_target0_store(dxil_ir_builder *builder, uint32_t block_index) {
    if (!builder->color_output.present || block_index != builder->color_output.block_index) return GPU_IR_INVALID;
    gpu_ir_shader_instruction composite = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_COMPOSITE,
        .operation = GPU_IR_SHADER_COMPOSITE_CONSTRUCT,
        .source_opcode = DXI_CALL,
        .result_id = builder->color_output.composite_id,
        .result_type_id = builder->color_output.vector_type_id,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 4
    };
    int rc = GPU_IR_OK;
    for (uint32_t component = 0; rc == GPU_IR_OK && component < 4; component++)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID,
                            common_value_id(builder, builder->color_output.values[component]));
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &composite);
    if (rc != GPU_IR_OK) return rc;

    gpu_ir_shader_instruction store = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_STORE,
        .source_opcode = DXI_CALL,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 2
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, builder->color_output.variable_id);
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, builder->color_output.composite_id);
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &store);
    return rc;
}

static int append_dxil_vertex_position_store(dxil_ir_builder *builder, uint32_t block_index) {
    dxil_ir_vertex_position_output *output = &builder->vertex_position_output;
    if (!output->present || block_index != output->block_index) return GPU_IR_INVALID;
    gpu_ir_shader_instruction composite = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_COMPOSITE,
        .operation = GPU_IR_SHADER_COMPOSITE_CONSTRUCT,
        .source_opcode = DXI_CALL,
        .result_id = output->composite_id,
        .result_type_id = output->vector_type_id,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 4
    };
    int rc = GPU_IR_OK;
    for (uint32_t component = 0; rc == GPU_IR_OK && component < 4; component++)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, common_value_id(builder, output->values[component]));
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &composite);
    if (rc != GPU_IR_OK) return rc;

    gpu_ir_shader_instruction store = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_STORE,
        .source_opcode = DXI_CALL,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 2
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, output->variable_id);
    if (rc == GPU_IR_OK) rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, output->composite_id);
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &store);
    return rc;
}

static int append_resource_globals(dxil_ir_builder *builder) {
    for (uint32_t i = 0; i < builder->resource_count; i++) {
        const dxil_ir_resource *resource = &builder->resources[i];
        if (!resource->used) continue;
        int image_resource = resource_is_common_image2d_or_ms_srv(resource);
        gpu_ir_shader_instruction instruction = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .opcode = GPU_IR_SHADER_OP_VARIABLE,
            .source_opcode = DXI_CALL,
            .result_id = resource->variable_id,
            .result_type_id = image_resource ? resource->image_pointer_type_id :
                builder->buffer_block_pointer_type_id,
            .function_index = UINT32_MAX,
            .block_index = UINT32_MAX,
            .storage_class = image_resource ? GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT :
                GPU_IR_SHADER_STORAGE_BUFFER,
            .resource_set = resource->set,
            .binding_namespace = resource->register_class,
            .binding = resource->binding,
            .decoration_flags = image_resource ? 0 :
                resource->register_class == 0 ? GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE : 0,
            .has_descriptor_set = 1,
            .has_binding = 1
        };
        int rc = append_instruction(builder, &instruction);
        if (rc != GPU_IR_OK) return rc;
    }
    if (builder->buffer_index_constant_id) {
        int rc = append_synthetic_u32_constant(builder, builder->buffer_index_constant_id,
                                               builder->resource_u32_source_type, 0);
        if (rc != GPU_IR_OK) return rc;
    }
    if (builder->buffer_byte_shift_constant_id) {
        int rc = append_synthetic_u32_constant(builder, builder->buffer_byte_shift_constant_id,
                                               builder->resource_u32_source_type, 2);
        if (rc != GPU_IR_OK) return rc;
        for (uint32_t lane = 1; lane < 4; lane++) {
            rc = append_synthetic_u32_constant(builder, builder->buffer_byte_lane_constant_ids[lane],
                                               builder->resource_u32_source_type, lane);
            if (rc != GPU_IR_OK) return rc;
        }
    }
    if (builder->buffer_byte_length_word_constant_id) {
        int rc = append_synthetic_u32_constant(builder, builder->buffer_byte_length_word_constant_id,
                                               builder->resource_u32_source_type, 4);
        if (rc != GPU_IR_OK) return rc;
    }
    for (uint32_t i = 0; i < builder->resource_count; i++) {
        const dxil_ir_resource *resource = &builder->resources[i];
        if (!resource->used || (!resource->structured_dimensions && !resource->structured_loads)) continue;
        int rc = append_synthetic_u32_constant(builder, resource->structure_stride_words_constant_id,
                                               builder->resource_u32_source_type,
                                               resource->structure_stride / 4u);
        if (rc != GPU_IR_OK) return rc;
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        uint32_t constant_id = builder->resource_structured_offset_word_constant_ids[i];
        if (!constant_id) continue;
        const dxil_inst *extract = &builder->function->insts[i];
        if (extract->op != DXI_EXTRACTVAL || extract->n != 2 || !extract->ops ||
            extract->ops[0] >= builder->value_count || extract->ops[1] >= 4) return GPU_IR_INVALID;
        const dxil_ir_resource_value *resource_value = &builder->resource_values[extract->ops[0]];
        if (resource_value->kind != DXIL_IR_RESOURCE_BUFFER_LOAD ||
            resource_value->resource_index >= builder->resource_count ||
            !resource_is_common_structured_buffer(&builder->resources[resource_value->resource_index]))
            return GPU_IR_INVALID;
        uint32_t byte_offset = resource_value->structured_byte_offset + extract->ops[1] * 4u;
        int rc = append_synthetic_u32_constant(builder, constant_id, builder->resource_u32_source_type,
                                               byte_offset / 4u);
        if (rc != GPU_IR_OK) return rc;
    }
    return GPU_IR_OK;
}

static int append_builtin_load(dxil_ir_builder *builder, uint32_t index) {
    uint32_t result_type = dxil_builtin_is_scalar(index) ?
        common_type_id(builder, builder->builtin_scalar_type) : builder->builtin_vector_type_id;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_LOAD,
        .source_opcode = DXI_CALL,
        .result_id = builder->builtin_load_ids[index],
        .result_type_id = result_type,
        .function_index = 0,
        .block_index = 0,
        .first_operand = builder->operand_count,
        .operand_count = 1
    };
    int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, builder->builtin_variable_ids[index]);
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &instruction);
    if (rc == GPU_IR_OK) {
        gpu_ir_shader_block *entry = &builder->blocks[0];
        if (!entry->instruction_count) entry->first_instruction = builder->instruction_count - 1u;
        entry->instruction_count++;
    }
    return rc;
}

static int append_system_value_result(dxil_ir_builder *builder, const dxil_inst *source,
                                      uint32_t block_index) {
    uint32_t index, component, opcode;
    int rc = dxil_system_value_call(builder, source, &index, &component, &opcode);
    if (rc != GPU_IR_OK) return rc;
    if (!(builder->builtin_mask & (1u << index)) || block_index >= builder->block_count ||
        common_type_id(builder, source->type) != common_type_id(builder, builder->builtin_scalar_type))
        return GPU_IR_INVALID;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = dxil_builtin_is_scalar(index) ? GPU_IR_SHADER_OP_COPY : GPU_IR_SHADER_OP_COMPOSITE,
        .operation = dxil_builtin_is_scalar(index) ? 0 : GPU_IR_SHADER_COMPOSITE_EXTRACT,
        .source_opcode = DXI_CALL,
        .result_id = common_value_id(builder, source->result),
        .result_type_id = common_type_id(builder, source->type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count,
        .operand_count = 1,
        .first_literal = builder->literal_count,
        .literal_word_count = dxil_builtin_is_scalar(index) ? 0 : 1
    };
    rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, builder->builtin_load_ids[index]);
    if (rc == GPU_IR_OK && instruction.literal_word_count) rc = append_literal(builder, component);
    if (rc == GPU_IR_OK) rc = append_instruction(builder, &instruction);
    return rc;
}

static int append_synthetic_u32_constant(dxil_ir_builder *builder, uint32_t result_id, uint32_t source_type,
                                         uint32_t value) {
    uint32_t literal = builder->literal_count;
    int rc = append_literal(builder, value);
    if (rc != GPU_IR_OK) return rc;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_CONSTANT,
        .operation = GPU_IR_SHADER_CONSTANT_SCALAR,
        .result_id = result_id,
        .result_type_id = common_type_id(builder, source_type),
        .function_index = UINT32_MAX,
        .block_index = UINT32_MAX,
        .first_literal = literal,
        .literal_word_count = 1
    };
    if (!instruction.result_type_id) return GPU_IR_INVALID;
    return append_instruction(builder, &instruction);
}

static int append_dxil_barrier_constants(dxil_ir_builder *builder) {
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        const dxil_ir_atomic *atomic = &builder->atomics[i];
        if (!atomic->present) continue;
        int rc = append_synthetic_u32_constant(builder, atomic->scope_id,
                                                builder->resource_u32_source_type, 1u);
        if (rc == GPU_IR_OK)
            rc = append_synthetic_u32_constant(builder, atomic->semantics_id,
                                                builder->resource_u32_source_type, 0u);
        if (rc != GPU_IR_OK) return rc;
    }
    if (builder->barrier_integer_type == UINT32_MAX) return GPU_IR_OK;
    const uint32_t type = builder->barrier_integer_type;
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        const dxil_ir_barrier *barrier = &builder->barriers[i];
        if (!barrier->present || barrier->no_op) continue;
        int rc = GPU_IR_OK;
        if (barrier->operation == GPU_IR_SHADER_BARRIER_CONTROL)
            rc = append_synthetic_u32_constant(builder, barrier->execution_scope_id, type,
                                                barrier->execution_scope);
        if (rc == GPU_IR_OK)
            rc = append_synthetic_u32_constant(builder, barrier->memory_scope_id, type, barrier->memory_scope);
        if (rc == GPU_IR_OK)
            rc = append_synthetic_u32_constant(builder, barrier->semantics_id, type, barrier->semantics);
        if (rc != GPU_IR_OK) return rc;
    }
    return GPU_IR_OK;
}

static int append_dxil_barrier_result(dxil_ir_builder *builder, const dxil_inst *source,
                                      uint32_t block_index) {
    ptrdiff_t index = source - builder->function->insts;
    if (index < 0 || (uint64_t)index >= builder->function->ninsts || !builder->barriers)
        return GPU_IR_INVALID;
    const dxil_ir_barrier *barrier = &builder->barriers[index];
    if (!barrier->present) return GPU_IR_UNSUPPORTED;
    if (barrier->no_op) return GPU_IR_OK;
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_BARRIER,
        .operation = barrier->operation,
        .source_opcode = DXI_CALL,
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    int rc = GPU_IR_OK;
    if (barrier->operation == GPU_IR_SHADER_BARRIER_CONTROL)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, barrier->execution_scope_id);
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, barrier->memory_scope_id);
    if (rc == GPU_IR_OK)
        rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, barrier->semantics_id);
    if (rc != GPU_IR_OK) return rc;
    instruction.operand_count = builder->operand_count - instruction.first_operand;
    return append_instruction(builder, &instruction);
}

static int configure_synthetic_ids(dxil_ir_builder *builder) {
    uint64_t next_type = (uint64_t)builder->module->ntypes + 1u;
    int has_vector_builtin = (builder->builtin_mask & 0x7u) != 0;
    if (has_vector_builtin) {
        if (next_type + 1u > UINT32_MAX) return GPU_IR_RANGE;
        builder->builtin_vector_type_id = (uint32_t)next_type++;
        builder->builtin_vector_pointer_type_id = (uint32_t)next_type++;
    }
    if (builder->builtin_mask & ((1u << DXIL_IR_BUILTIN_FLATTENED_THREAD_ID_IN_GROUP) |
                                 (1u << DXIL_IR_BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID) |
                                 (1u << DXIL_IR_BUILTIN_SUBGROUP_SIZE))) {
        if (next_type > UINT32_MAX) return GPU_IR_RANGE;
        builder->builtin_scalar_pointer_type_id = (uint32_t)next_type++;
    }
    int has_buffer_resources = 0, has_raw_byte_accesses = 0, has_raw_buffer_dimensions = 0;
    int has_image_resources = 0, has_sint_image_resource = 0;
    for (uint32_t i = 0; i < builder->resource_count; i++) {
        dxil_ir_resource *resource = &builder->resources[i];
        has_buffer_resources |= resource->used && resource_is_common_buffer_backing(resource);
        has_image_resources |= resource->used && resource_is_common_image2d_or_ms_srv(resource);
        has_sint_image_resource |= resource->used && resource_is_common_sint2d_srv(resource);
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        has_raw_byte_accesses |= builder->resource_byte_index_ids[i] != 0 || builder->raw_stores[i].present;
        has_raw_buffer_dimensions |= builder->resource_raw_length_word_ids[i] != 0;
    }
    if (has_buffer_resources) {
        if (next_type + 3u > UINT32_MAX) return GPU_IR_RANGE;
        builder->buffer_array_type_id = (uint32_t)next_type++;
        builder->buffer_block_type_id = (uint32_t)next_type++;
        builder->buffer_block_pointer_type_id = (uint32_t)next_type++;
        builder->buffer_scalar_pointer_type_id = (uint32_t)next_type++;
    }
    if (has_image_resources) {
        if (next_type >= UINT32_MAX) return GPU_IR_RANGE;
        builder->image_query_size_type_id = (uint32_t)next_type++;
        if (has_sint_image_resource) {
            if (next_type >= UINT32_MAX) return GPU_IR_RANGE;
            builder->image_s32_type_id = (uint32_t)next_type++;
        }
        for (uint32_t i = 0; i < builder->resource_count; i++) {
            dxil_ir_resource *resource = &builder->resources[i];
            if (!resource->used || !resource_is_common_image2d_or_ms_srv(resource)) continue;
            if (next_type + 1u > UINT32_MAX) return GPU_IR_RANGE;
            resource->image_type_id = (uint32_t)next_type++;
            resource->image_pointer_type_id = (uint32_t)next_type++;
        }
    }
    if (builder->color_output.present) {
        if (next_type + 2u > UINT32_MAX) return GPU_IR_RANGE;
        builder->color_output.vector_type_id = (uint32_t)next_type++;
        builder->color_output.pointer_type_id = (uint32_t)next_type++;
    }
    if (builder->vertex_position_output.present) {
        if (next_type + 2u > UINT32_MAX) return GPU_IR_RANGE;
        builder->vertex_position_output.vector_type_id = (uint32_t)next_type++;
        builder->vertex_position_output.pointer_type_id = (uint32_t)next_type++;
    }
    if (builder->fragment_position.present) {
        if (next_type + 2u > UINT32_MAX) return GPU_IR_RANGE;
        builder->fragment_position.vector_type_id = (uint32_t)next_type++;
        builder->fragment_position.pointer_type_id = (uint32_t)next_type++;
    }
    if (builder->front_facing.present) {
        if (next_type >= UINT32_MAX) return GPU_IR_RANGE;
        builder->front_facing.pointer_type_id = (uint32_t)next_type++;
    }
    for (uint32_t i = 0; i < DXIL_IR_VERTEX_SYSTEM_VALUE_COUNT; i++) {
        dxil_ir_vertex_system_value *system_value = &builder->vertex_system_values[i];
        if (!system_value->present) continue;
        if (next_type >= UINT32_MAX) return GPU_IR_RANGE;
        system_value->pointer_type_id = (uint32_t)next_type++;
    }
    for (uint32_t i = 0; i < builder->vertex_input_count; i++) {
        dxil_ir_vertex_input *input = &builder->vertex_inputs[i];
        if (input->columns > 1) {
            if (next_type >= UINT32_MAX) return GPU_IR_RANGE;
            input->vector_type_id = (uint32_t)next_type++;
        } else {
            input->vector_type_id = common_type_id(builder, input->scalar_type_id);
        }
        if (next_type >= UINT32_MAX) return GPU_IR_RANGE;
        input->pointer_type_id = (uint32_t)next_type++;
    }
    builder->synthetic_type_count = (uint32_t)(next_type - ((uint64_t)builder->module->ntypes + 1u));
    uint64_t value_base = (uint64_t)builder->module->ntypes + builder->synthetic_type_count + 1u;
    uint64_t next_value = value_base + builder->value_count;
    if (value_base > UINT32_MAX || next_value > UINT32_MAX) return GPU_IR_RANGE;
    builder->value_base = (uint32_t)value_base;
    for (uint32_t index = 0; index < DXIL_IR_BUILTIN_COUNT; index++) {
        if (!(builder->builtin_mask & (1u << index))) continue;
        if (next_value + 1u > UINT32_MAX) return GPU_IR_RANGE;
        builder->builtin_variable_ids[index] = (uint32_t)next_value++;
        builder->builtin_load_ids[index] = (uint32_t)next_value++;
        builder->synthetic_value_count += 2u;
    }
    if (builder->color_output.present) {
        if (next_value + 2u > UINT32_MAX) return GPU_IR_RANGE;
        builder->color_output.variable_id = (uint32_t)next_value++;
        builder->color_output.composite_id = (uint32_t)next_value++;
        builder->synthetic_value_count += 2u;
    }
    if (builder->vertex_position_output.present) {
        if (next_value + 2u > UINT32_MAX) return GPU_IR_RANGE;
        builder->vertex_position_output.variable_id = (uint32_t)next_value++;
        builder->vertex_position_output.composite_id = (uint32_t)next_value++;
        builder->synthetic_value_count += 2u;
    }
    if (builder->fragment_position.present) {
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->fragment_position.variable_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    if (builder->front_facing.present) {
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->front_facing.variable_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    for (uint32_t i = 0; i < DXIL_IR_VERTEX_SYSTEM_VALUE_COUNT; i++) {
        dxil_ir_vertex_system_value *system_value = &builder->vertex_system_values[i];
        if (!system_value->present) continue;
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        system_value->variable_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    for (uint32_t i = 0; i < builder->vertex_input_count; i++) {
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->vertex_inputs[i].variable_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        if (!builder->input_load_ids[i]) continue;
        if (builder->common_stage == GPU_IR_STAGE_VERTEX) {
            uint32_t signature_id = 0;
            if (!value_i32(builder, builder->function->insts[i].ops[2], &signature_id)) return GPU_IR_INVALID;
            uint32_t system_value_index = 0;
            int system_value_rc = dxil_vertex_system_value_signature(builder->module, builder->function,
                signature_id, &system_value_index);
            if (system_value_rc == GPU_IR_OK) {
                if (system_value_index >= DXIL_IR_VERTEX_SYSTEM_VALUE_COUNT ||
                    !builder->vertex_system_values[system_value_index].present ||
                    builder->vertex_system_values[system_value_index].signature_id != signature_id)
                    return GPU_IR_INVALID;
                builder->input_load_ids[i] = UINT32_MAX;
                continue;
            }
            if (system_value_rc != GPU_IR_NOT_FOUND) return system_value_rc;
            const dxil_ir_vertex_input *input = NULL;
            for (uint32_t j = 0; j < builder->vertex_input_count; j++)
                if (builder->vertex_inputs[j].signature_id == signature_id) input = &builder->vertex_inputs[j];
            if (!input) return GPU_IR_INVALID;
            if (input->columns == 1) {
                builder->input_load_ids[i] = UINT32_MAX;
                continue;
            }
        } else if (builder->common_stage == GPU_IR_STAGE_FRAGMENT && builder->front_facing.present) {
            uint32_t signature_id = 0;
            if (!value_i32(builder, builder->function->insts[i].ops[2], &signature_id)) return GPU_IR_INVALID;
            if (signature_id == builder->front_facing.signature_id) {
                builder->input_load_ids[i] = UINT32_MAX;
                continue;
            }
        }
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->input_load_ids[i] = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    for (uint32_t i = 0; i < builder->resource_count; i++) {
        dxil_ir_resource *resource = &builder->resources[i];
        if (!resource->used) continue;
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        resource->variable_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    for (uint32_t i = 0; i < builder->resource_count; i++) {
        dxil_ir_resource *resource = &builder->resources[i];
        if (!resource->used || (!resource->structured_dimensions && !resource->structured_loads)) continue;
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        resource->structure_stride_words_constant_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    for (uint32_t i = 0; i < builder->resource_count; i++) {
        dxil_ir_resource *resource = &builder->resources[i];
        if (!resource->used || !resource_is_common_image2d_or_ms_srv(resource)) continue;
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        resource->image_value_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        const dxil_inst *instruction = &builder->function->insts[i];
        if (instruction->op != DXI_CALL || instruction->result >= builder->value_count ||
            builder->resource_values[instruction->result].kind != DXIL_IR_RESOURCE_IMAGE_LOAD) continue;
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->resource_values[instruction->result].image_value_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    if (has_buffer_resources) {
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->buffer_index_constant_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    if (has_raw_byte_accesses) {
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->buffer_byte_shift_constant_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
        for (uint32_t lane = 1; lane < 4; lane++) {
            if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
            builder->buffer_byte_lane_constant_ids[lane] = (uint32_t)next_value++;
            builder->synthetic_value_count++;
        }
    }
    if (has_raw_buffer_dimensions) {
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->buffer_byte_length_word_constant_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        if (!builder->resource_raw_length_word_ids[i]) continue;
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->resource_raw_length_word_ids[i] = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        if (!builder->resource_structured_length_word_ids[i]) continue;
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->resource_structured_length_word_ids[i] = (uint32_t)next_value++;
        builder->synthetic_value_count++;
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        if (builder->resource_structured_base_word_index_ids[i]) {
            if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
            builder->resource_structured_base_word_index_ids[i] = (uint32_t)next_value++;
            builder->synthetic_value_count++;
        }
        if (builder->resource_structured_word_index_ids[i]) {
            if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
            builder->resource_structured_word_index_ids[i] = (uint32_t)next_value++;
            builder->synthetic_value_count++;
        }
        if (builder->resource_structured_offset_word_constant_ids[i]) {
            if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
            builder->resource_structured_offset_word_constant_ids[i] = (uint32_t)next_value++;
            builder->synthetic_value_count++;
        }
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        if (!builder->resource_access_ids[i]) continue;
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        builder->resource_access_ids[i] = (uint32_t)next_value++;
        builder->synthetic_value_count++;
        if (builder->resource_typed_value_ids[i]) {
            if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
            builder->resource_typed_value_ids[i] = (uint32_t)next_value++;
            builder->synthetic_value_count++;
        }
        if (builder->resource_byte_index_ids[i]) {
            if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
            builder->resource_byte_index_ids[i] = (uint32_t)next_value++;
            builder->synthetic_value_count++;
        }
        if (builder->resource_byte_base_index_ids[i]) {
            if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
            builder->resource_byte_base_index_ids[i] = (uint32_t)next_value++;
            builder->synthetic_value_count++;
        }
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        dxil_ir_raw_store *store = &builder->raw_stores[i];
        if (!store->present) continue;
        if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
        store->base_word_index_id = (uint32_t)next_value++;
        builder->synthetic_value_count++;
        for (uint32_t lane = 0; lane < 4; lane++) {
            if (!(store->mask & (1u << lane))) continue;
            if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
            store->access_ids[lane] = (uint32_t)next_value++;
            builder->synthetic_value_count++;
            if (lane != 0) {
                if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
                store->word_index_ids[lane] = (uint32_t)next_value++;
                builder->synthetic_value_count++;
            }
            if (store->is_float) {
                if (next_value >= UINT32_MAX) return GPU_IR_RANGE;
                store->value_ids[lane] = (uint32_t)next_value++;
                builder->synthetic_value_count++;
            }
        }
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        dxil_ir_atomic *atomic = &builder->atomics[i];
        if (!atomic->present) continue;
        if ((uint64_t)next_value + 2u > UINT32_MAX) return GPU_IR_RANGE;
        atomic->scope_id = (uint32_t)next_value++;
        atomic->semantics_id = (uint32_t)next_value++;
        builder->synthetic_value_count += 2u;
    }
    for (uint32_t i = 0; i < builder->function->ninsts; i++) {
        dxil_ir_barrier *barrier = &builder->barriers[i];
        if (!barrier->present || barrier->no_op) continue;
        uint32_t count = barrier->operation == GPU_IR_SHADER_BARRIER_CONTROL ? 3u : 2u;
        if (next_value + count > UINT32_MAX) return GPU_IR_RANGE;
        if (count == 3) barrier->execution_scope_id = (uint32_t)next_value++;
        barrier->memory_scope_id = (uint32_t)next_value++;
        barrier->semantics_id = (uint32_t)next_value++;
        builder->synthetic_value_count += count;
    }
    uint64_t label_base = next_value;
    if (!builder->block_count || label_base + builder->block_count - 1u > UINT32_MAX) return GPU_IR_RANGE;
    builder->label_base = (uint32_t)label_base;
    return GPU_IR_OK;
}

static int append_phi(dxil_ir_builder *builder, const dxil_inst *source, uint32_t block_index) {
    gpu_ir_shader_instruction instruction = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL,
        .opcode = GPU_IR_SHADER_OP_PHI,
        .source_opcode = source->op,
        .result_id = common_value_id(builder, source->result),
        .result_type_id = common_type_id(builder, source->type),
        .function_index = 0,
        .block_index = block_index,
        .first_operand = builder->operand_count
    };
    for (uint32_t i = 0; i < source->n; i += 2) {
        uint32_t value = common_value_id(builder, source->ops[i]);
        uint32_t label = common_label_id(builder, source->ops[i + 1]);
        if (!value || !label) return GPU_IR_INVALID;
        int rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, value);
        if (rc == GPU_IR_OK) rc = append_operand(builder, GPU_IR_SHADER_OPERAND_ID, label);
        if (rc != GPU_IR_OK) return rc;
    }
    instruction.operand_count = builder->operand_count - instruction.first_operand;
    return append_instruction(builder, &instruction);
}

static int lower_shuffle_mask(const dxil_ir_builder *builder, const dxil_inst *source,
                              uint32_t output_lanes, uint32_t input_lanes, uint32_t *mask) {
    const dxil_value *value = dxil_value_at(builder->module, builder->function, source->ops[2]);
    const dxil_type *mask_type = value ? source_type(builder->module, value->type) : NULL;
    if (!value || !mask_type || mask_type->kind != DXT_VECTOR || mask_type->count != output_lanes ||
        !source_type(builder->module, mask_type->elem) ||
        source_type(builder->module, mask_type->elem)->kind != DXT_INT ||
        source_type(builder->module, mask_type->elem)->bits != 32) return GPU_IR_UNSUPPORTED;
    if (value->kind == DXV_CONST_AGGREGATE) {
        if (value->u.list.n != output_lanes || !value->u.list.ids) return GPU_IR_INVALID;
        for (uint32_t i = 0; i < output_lanes; i++) {
            const dxil_value *lane = dxil_value_at(builder->module, builder->function, value->u.list.ids[i]);
            if (!lane || lane->kind != DXV_CONST_INT) {
                if (lane && lane->kind == DXV_CONST_UNDEF) { mask[i] = UINT32_MAX; continue; }
                return GPU_IR_UNSUPPORTED;
            }
            int64_t index = lane->u.i;
            if (index < 0 || (uint64_t)index >= (uint64_t)input_lanes * 2u) return GPU_IR_UNSUPPORTED;
            mask[i] = (uint32_t)index;
        }
        return GPU_IR_OK;
    }
    if (value->kind == DXV_CONST_DATA) {
        if (value->u.list.n != output_lanes || !value->data) return GPU_IR_INVALID;
        for (uint32_t i = 0; i < output_lanes; i++) {
            uint64_t index = value->data[i];
            if (index == UINT32_MAX) mask[i] = UINT32_MAX;
            else if (index >= (uint64_t)input_lanes * 2u) return GPU_IR_UNSUPPORTED;
            else mask[i] = (uint32_t)index;
        }
        return GPU_IR_OK;
    }
    return GPU_IR_UNSUPPORTED;
}

static int lower_instruction_inner(dxil_ir_builder *builder, const dxil_inst *source, uint32_t block_index,
                                   uint32_t *operands, uint32_t *labels, uint32_t *literals,
                                   uint32_t temporary_capacity) {
    if (source->op == DXI_EXTRACTVAL && source->n && source->ops && source->ops[0] < builder->value_count) {
        uint32_t kind = builder->resource_values[source->ops[0]].kind;
        if (kind == DXIL_IR_RESOURCE_BUFFER_LOAD || kind == DXIL_IR_RESOURCE_GET_DIMENSIONS ||
            kind == DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS || kind == DXIL_IR_RESOURCE_IMAGE_LOAD)
            return append_resource_extract(builder, source, block_index);
    }
    uint32_t operation = 0, opcode = 0, flags = 0, operand_count = 0, label_count = 0, literal_count = 0;
    const dxil_type *type = source_type(builder->module, source->type);
    uint32_t result_type = source->type;
    switch (source->op) {
    case DXI_BINOP: {
        if (!type) return GPU_IR_UNSUPPORTED;
        const dxil_value *left = dxil_value_at(builder->module, builder->function, source->ops[0]);
        const dxil_value *right = dxil_value_at(builder->module, builder->function, source->ops[1]);
        if (!left || !right || left->type != source->type || right->type != source->type)
            return GPU_IR_INVALID;
        uint32_t scalar_kind = value_scalar_kind(builder->module, source->type);
        operation = arithmetic_operation(scalar_kind == DXT_OTHER ? type : source_type(builder->module,
            type->kind == DXT_VECTOR ? type->elem : source->type), source->sub);
        if (!operation) return GPU_IR_UNSUPPORTED;
        if (scalar_kind == DXT_FLOAT || scalar_kind == DXT_HALF || scalar_kind == DXT_DOUBLE) {
            flags = GPU_IR_SHADER_FLAG_FLOAT;
            if (append_fast_math_hint(source->flags, &flags) != GPU_IR_OK) return GPU_IR_UNSUPPORTED;
        } else if (scalar_kind == DXT_INT && source_type(builder->module,
            type->kind == DXT_VECTOR ? type->elem : source->type)->bits == 1) {
            if (source->flags) return GPU_IR_UNSUPPORTED;
            if (source->sub == 10) operation = GPU_IR_SHADER_ARITH_LOGICAL_AND;
            else if (source->sub == 11) operation = GPU_IR_SHADER_ARITH_LOGICAL_OR;
            else return GPU_IR_UNSUPPORTED;
        } else if (scalar_kind == DXT_INT) {
            if (source->flags) return GPU_IR_UNSUPPORTED;
            if (source->sub == 4 || source->sub == 6 || source->sub == 9)
                flags = GPU_IR_SHADER_FLAG_SIGNED;
        } else return GPU_IR_UNSUPPORTED;
        opcode = GPU_IR_SHADER_OP_ARITHMETIC;
        operands[0] = source->ops[0]; operands[1] = source->ops[1]; operand_count = 2;
        break;
    }
    case DXI_CMP: {
        const dxil_value *left = dxil_value_at(builder->module, builder->function, source->ops[0]);
        const dxil_value *right = dxil_value_at(builder->module, builder->function, source->ops[1]);
        const dxil_type *left_type = left ? source_type(builder->module, left->type) : NULL;
        if (!left || !right || left->type != right->type || !type)
            return GPU_IR_INVALID;
        if (left_type && left_type->kind == DXT_VECTOR) {
            const dxil_type *result_element = type->kind == DXT_VECTOR ?
                source_type(builder->module, type->elem) : NULL;
            if (type->kind != DXT_VECTOR || type->count != left_type->count || !result_element ||
                result_element->kind != DXT_INT || result_element->bits != 1) return GPU_IR_INVALID;
        } else if (type->kind != DXT_INT || type->bits != 1) return GPU_IR_INVALID;
        const dxil_type *left_scalar = left_type && left_type->kind == DXT_VECTOR ?
            source_type(builder->module, left_type->elem) : left_type;
        if (left_scalar && left_scalar->kind == DXT_INT && left_scalar->bits == 1)
            return GPU_IR_UNSUPPORTED;
        operation = compare_operation(left_scalar, source->sub);
        if (!operation) return GPU_IR_UNSUPPORTED;
        if (left_scalar && (left_scalar->kind == DXT_FLOAT || left_scalar->kind == DXT_HALF ||
                            left_scalar->kind == DXT_DOUBLE)) {
            flags = GPU_IR_SHADER_FLAG_FLOAT;
            if (append_fast_math_hint(source->flags, &flags) != GPU_IR_OK) return GPU_IR_UNSUPPORTED;
        } else if (!left_scalar || left_scalar->kind != DXT_INT) return GPU_IR_UNSUPPORTED;
        else if (source->flags) return GPU_IR_UNSUPPORTED;
        else if (source->sub >= 38 && source->sub <= 41) flags = GPU_IR_SHADER_FLAG_SIGNED;
        opcode = GPU_IR_SHADER_OP_COMPARE;
        operands[0] = source->ops[0]; operands[1] = source->ops[1]; operand_count = 2;
        break;
    }
    case DXI_CAST: {
        if (source->n != 1 || source->flags) return GPU_IR_UNSUPPORTED;
        const dxil_value *input_value = dxil_value_at(builder->module, builder->function, source->ops[0]);
        const dxil_type *input = input_value ? source_type(builder->module, input_value->type) : NULL;
        const dxil_type *output = type;
        if (!input || !output) return GPU_IR_INVALID;
        uint32_t input_components, output_components;
        const dxil_type *input_scalar = numeric_scalar_type(builder->module, input, &input_components);
        const dxil_type *output_scalar = numeric_scalar_type(builder->module, output, &output_components);
        if (!input_scalar || !output_scalar) return GPU_IR_UNSUPPORTED;
        int input_float = input_scalar->kind == DXT_FLOAT || input_scalar->kind == DXT_DOUBLE ||
            input_scalar->kind == DXT_HALF;
        int output_float = output_scalar->kind == DXT_FLOAT || output_scalar->kind == DXT_DOUBLE ||
            output_scalar->kind == DXT_HALF;
        if (source->sub >= 3 && source->sub <= 8 && input_components != output_components)
            return GPU_IR_UNSUPPORTED;
        if ((source->sub == 0 || source->sub == 1) && input->kind == DXT_INT && output->kind == DXT_INT)
            operation = GPU_IR_SHADER_CONVERT_UNSIGNED_WIDTH;
        else if (source->sub == 3 && input_float && output_scalar->kind == DXT_INT)
            operation = GPU_IR_SHADER_CONVERT_FLOAT_TO_UNSIGNED;
        else if (source->sub == 4 && input_float && output_scalar->kind == DXT_INT)
            operation = GPU_IR_SHADER_CONVERT_FLOAT_TO_SIGNED;
        else if (source->sub == 5 && input_scalar->kind == DXT_INT && output_float)
            operation = GPU_IR_SHADER_CONVERT_UNSIGNED_TO_FLOAT;
        else if (source->sub == 6 && input_scalar->kind == DXT_INT && output_float)
            operation = GPU_IR_SHADER_CONVERT_SIGNED_TO_FLOAT;
        else if ((source->sub == 7 || source->sub == 8) && input_float && output_float)
            operation = GPU_IR_SHADER_CONVERT_FLOAT_WIDTH;
        else if (source->sub == 11 && (input_scalar->kind == DXT_INT || input_float) &&
                 (output_scalar->kind == DXT_INT || output_float)) operation = GPU_IR_SHADER_CONVERT_BITCAST;
        else return GPU_IR_UNSUPPORTED;
        if (source->sub >= 3 && source->sub <= 6 &&
            (input_scalar->kind == DXT_INT ? input_scalar->bits : input_scalar->kind == DXT_FLOAT ? 32u : 0u) != 32u)
            return GPU_IR_UNSUPPORTED;
        if (source->sub >= 3 && source->sub <= 6 &&
            (output_scalar->kind == DXT_INT ? output_scalar->bits : output_scalar->kind == DXT_FLOAT ? 32u : 0u) != 32u)
            return GPU_IR_UNSUPPORTED;
        opcode = GPU_IR_SHADER_OP_CONVERT;
        operands[0] = source->ops[0]; operand_count = 1;
        break;
    }
    case DXI_SELECT:
        {
            const dxil_value *condition = dxil_value_at(builder->module, builder->function, source->ops[0]);
            const dxil_value *yes = dxil_value_at(builder->module, builder->function, source->ops[1]);
            const dxil_value *no = dxil_value_at(builder->module, builder->function, source->ops[2]);
            const dxil_type *condition_type = condition ? source_type(builder->module, condition->type) : NULL;
            const dxil_type *result = source_type(builder->module, result_type);
            if (!condition || !yes || !no || !condition_type || !result ||
                yes->type != result_type || no->type != result_type) return GPU_IR_INVALID;
            if (condition_type->kind == DXT_VECTOR) {
                const dxil_type *condition_element = source_type(builder->module, condition_type->elem);
                if (!condition_element || condition_element->kind != DXT_INT || condition_element->bits != 1 ||
                    result->kind != DXT_VECTOR || result->count != condition_type->count)
                    return GPU_IR_INVALID;
            } else if (condition_type->kind != DXT_INT || condition_type->bits != 1) return GPU_IR_INVALID;
        }
        opcode = GPU_IR_SHADER_OP_SELECT;
        for (uint32_t i = 0; i < 3; i++) operands[operand_count++] = source->ops[i];
        break;
    case DXI_EXTRACTELT: {
        if (source->n != 2 || source->flags) return GPU_IR_INVALID;
        const dxil_value *vector_value = dxil_value_at(builder->module, builder->function, source->ops[0]);
        const dxil_value *index_value = dxil_value_at(builder->module, builder->function, source->ops[1]);
        const dxil_type *vector_type = vector_value ? source_type(builder->module, vector_value->type) : NULL;
        const dxil_type *index_type = index_value ? source_type(builder->module, index_value->type) : NULL;
        if (!vector_value || !index_value || !type || !vector_type || vector_type->kind != DXT_VECTOR ||
            vector_type->elem != source->type) return GPU_IR_INVALID;
        if (!index_type || index_type->kind != DXT_INT || !index_type->bits) return GPU_IR_INVALID;
        opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC;
        operands[0] = source->ops[0]; operands[1] = source->ops[1]; operand_count = 2;
        break;
    }
    case DXI_INSERTELT: {
        if (source->n != 3 || source->flags) return GPU_IR_INVALID;
        const dxil_value *vector_value = dxil_value_at(builder->module, builder->function, source->ops[0]);
        const dxil_value *element_value = dxil_value_at(builder->module, builder->function, source->ops[1]);
        const dxil_value *index_value = dxil_value_at(builder->module, builder->function, source->ops[2]);
        const dxil_type *vector_type = vector_value ? source_type(builder->module, vector_value->type) : NULL;
        const dxil_type *index_type = index_value ? source_type(builder->module, index_value->type) : NULL;
        if (!vector_value || !element_value || !index_value || !type ||
            vector_value->type != source->type || !vector_type || vector_type->kind != DXT_VECTOR ||
            !source_type(builder->module, vector_type->elem) || element_value->type != vector_type->elem)
            return GPU_IR_INVALID;
        if (!index_type || index_type->kind != DXT_INT || !index_type->bits) return GPU_IR_INVALID;
        opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = GPU_IR_SHADER_COMPOSITE_VECTOR_INSERT_DYNAMIC;
        for (uint32_t i = 0; i < 3; i++) operands[operand_count++] = source->ops[i];
        break;
    }
    case DXI_EXTRACTVAL: case DXI_INSERTVAL: {
        uint32_t values_needed = source->op == DXI_EXTRACTVAL ? 1u : 2u;
        if (source->flags || source->nvals != values_needed || source->n <= values_needed ||
            source->n - values_needed > temporary_capacity) return GPU_IR_UNSUPPORTED;
        const dxil_value *composite = dxil_value_at(builder->module, builder->function, source->ops[0]);
        const dxil_type *composite_type = composite ? source_type(builder->module, composite->type) : NULL;
        if (!composite_type || composite_type->kind != DXT_VECTOR || source->n - values_needed != 1)
            return GPU_IR_UNSUPPORTED;
        if (source->ops[values_needed] >= composite_type->count) return GPU_IR_INVALID;
        if (source->op == DXI_EXTRACTVAL) {
            if (!type || source->type != composite_type->elem) return GPU_IR_INVALID;
        } else {
            const dxil_value *object = dxil_value_at(builder->module, builder->function, source->ops[1]);
            if (!type || source->type != composite->type || !object || object->type != composite_type->elem)
                return GPU_IR_INVALID;
        }
        opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = source->op == DXI_EXTRACTVAL ? GPU_IR_SHADER_COMPOSITE_EXTRACT :
            GPU_IR_SHADER_COMPOSITE_INSERT;
        if (source->op == DXI_EXTRACTVAL) operands[operand_count++] = source->ops[0];
        else {
            // The common IR orders composite insertion as object then composite; LLVM uses the reverse.
            operands[operand_count++] = source->ops[1];
            operands[operand_count++] = source->ops[0];
        }
        literals[0] = source->ops[values_needed]; literal_count = 1;
        break;
    }
    case DXI_SHUFFLEVEC: {
        const dxil_value *left = dxil_value_at(builder->module, builder->function, source->ops[0]);
        const dxil_type *left_type = left ? source_type(builder->module, left->type) : NULL;
        if (!left_type || left_type->kind != DXT_VECTOR || !type || type->kind != DXT_VECTOR ||
            type->elem != left_type->elem || type->count > temporary_capacity)
            return GPU_IR_UNSUPPORTED;
        int rc = lower_shuffle_mask(builder, source, type->count, left_type->count, literals);
        if (rc != GPU_IR_OK) return rc;
        opcode = GPU_IR_SHADER_OP_COMPOSITE;
        operation = GPU_IR_SHADER_COMPOSITE_VECTOR_SHUFFLE;
        operands[0] = source->ops[0]; operands[1] = source->ops[1]; operand_count = 2;
        literal_count = type->count;
        break;
    }
    case DXI_PHI:
        opcode = GPU_IR_SHADER_OP_PHI;
        for (uint32_t i = 0; i < source->n; i += 2) {
            operands[operand_count++] = source->ops[i];
            if (label_count >= temporary_capacity) return GPU_IR_UNSUPPORTED;
            labels[label_count++] = source->ops[i + 1];
        }
        break;
    case DXI_BR:
        if (source->n == 1) {
            opcode = GPU_IR_SHADER_OP_BRANCH;
            labels[label_count++] = source->ops[0];
        } else if (source->n == 3) {
            opcode = GPU_IR_SHADER_OP_BRANCH_CONDITIONAL;
            operands[operand_count++] = source->ops[2];
            labels[label_count++] = source->ops[0];
            labels[label_count++] = source->ops[1];
        } else return GPU_IR_INVALID;
        break;
    case DXI_SWITCH: {
        if (source->n < 2 || (source->n & 1u)) return GPU_IR_INVALID;
        uint32_t cases = (source->n - 2u) / 2u;
        if (cases > temporary_capacity || cases + 1u > temporary_capacity) return GPU_IR_UNSUPPORTED;
        const dxil_value *selector = dxil_value_at(builder->module, builder->function, source->ops[0]);
        const dxil_type *selector_type = selector ? source_type(builder->module, selector->type) : NULL;
        if (!selector_type || selector_type->kind != DXT_INT || selector_type->bits > 32) return GPU_IR_UNSUPPORTED;
        opcode = GPU_IR_SHADER_OP_SWITCH;
        operands[operand_count++] = source->ops[0];
        labels[label_count++] = source->ops[1];
        for (uint32_t i = 0; i < cases; i++) {
            uint32_t case_value;
            if (!value_i32(builder, source->ops[2 + i * 2], &case_value)) return GPU_IR_UNSUPPORTED;
            literals[literal_count++] = case_value;
            if (label_count >= temporary_capacity) return GPU_IR_UNSUPPORTED;
            labels[label_count++] = source->ops[3 + i * 2];
        }
        break;
    }
    case DXI_RET:
        if (source->n) {
            opcode = GPU_IR_SHADER_OP_RETURN_VALUE;
            operands[operand_count++] = source->ops[0];
        } else opcode = GPU_IR_SHADER_OP_RETURN;
        break;
    case DXI_CALL: {
        uint32_t discard_condition;
        int discard_rc = dxil_discard_condition(builder, source, &discard_condition);
        if (discard_rc != GPU_IR_NOT_FOUND) {
            if (discard_rc != GPU_IR_OK) return discard_rc;
            return append_mapped(builder, source, block_index, GPU_IR_SHADER_OP_DISCARD,
                0, 0, 0, &discard_condition, 1, NULL, 0, NULL, 0);
        }
        int dxop = dxil_dxop(builder->module, builder->function, source);
        uint32_t vote_operation, vote_value;
        int vote_rc = dxil_wave_vote_call(builder, source, &vote_operation, &vote_value);
        if (vote_rc != GPU_IR_NOT_FOUND) {
            if (vote_rc != GPU_IR_OK) return vote_rc;
            return append_mapped(builder, source, block_index, GPU_IR_SHADER_OP_ARITHMETIC,
                vote_operation, source->type, 0, &vote_value, 1, NULL, 0, NULL, 0);
        }
        uint32_t wave_operation, wave_value;
        int wave_rc = dxil_wave_integer_minmax_call(builder, source, &wave_operation, &wave_value);
        if (wave_rc != GPU_IR_NOT_FOUND) {
            if (wave_rc != GPU_IR_OK) return wave_rc;
            return append_mapped(builder, source, block_index, GPU_IR_SHADER_OP_ARITHMETIC,
                wave_operation, source->type, 0, &wave_value, 1, NULL, 0, NULL, 0);
        }
        uint32_t wave_bit_operation, wave_bit_value;
        int wave_bit_rc = dxil_wave_integer_bit_call(builder, source,
                                                     &wave_bit_operation, &wave_bit_value);
        if (wave_bit_rc != GPU_IR_NOT_FOUND) {
            if (wave_bit_rc != GPU_IR_OK) return wave_bit_rc;
            return append_mapped(builder, source, block_index, GPU_IR_SHADER_OP_ARITHMETIC,
                wave_bit_operation, source->type, 0, &wave_bit_value, 1, NULL, 0, NULL, 0);
        }
        if (dxop == 4) {
            if (builder->common_stage == GPU_IR_STAGE_VERTEX) {
                uint32_t signature_id = 0;
                if (!value_i32(builder, source->ops[2], &signature_id)) return GPU_IR_INVALID;
                uint32_t system_value_index = 0;
                int system_value_rc = dxil_vertex_system_value_signature(builder->module, builder->function,
                    signature_id, &system_value_index);
                if (system_value_rc == GPU_IR_OK)
                    return append_dxil_vertex_system_value_load(builder, source, block_index);
                if (system_value_rc != GPU_IR_NOT_FOUND) return system_value_rc;
                return append_dxil_vertex_input_load(builder, source, block_index);
            }
            uint32_t signature_id;
            if (!value_i32(builder, source->ops[2], &signature_id)) return GPU_IR_INVALID;
            if (builder->front_facing.present && signature_id == builder->front_facing.signature_id)
                return append_dxil_front_facing_load(builder, source, block_index);
            return append_dxil_fragment_position_load(builder, source, block_index);
        }
        if (dxop == 5) {
            ptrdiff_t instruction_index = source - builder->function->insts;
            if ((!builder->color_output.present && !builder->vertex_position_output.present) ||
                instruction_index < 0 ||
                (uint64_t)instruction_index >= builder->function->ninsts) return GPU_IR_UNSUPPORTED;
            if (builder->vertex_position_output.present)
                return (uint32_t)instruction_index == builder->vertex_position_output.last_instruction ?
                    append_dxil_vertex_position_store(builder, block_index) : GPU_IR_OK;
            return (uint32_t)instruction_index == builder->color_output.last_instruction ?
                append_dxil_target0_store(builder, block_index) : GPU_IR_OK;
        }
        if (dxop == 57 || dxop == 66 || dxop == 68 || dxop == 139) return GPU_IR_OK;
        if (dxop == 72) {
            if (source->result >= builder->value_count) return GPU_IR_INVALID;
            return builder->resource_values[source->result].kind == DXIL_IR_RESOURCE_IMAGE_GET_DIMENSIONS ?
                append_resource_image_query_size(builder, source, block_index) : GPU_IR_OK;
        }
        if (dxop == 69) return append_resource_store(builder, source, block_index);
        if (dxop == 140) return append_raw_buffer_store(builder, source, block_index);
        if (dxop == 78) return append_resource_atomic(builder, source, block_index);
        if (dxop == 79) return append_resource_atomic_compare_exchange(builder, source, block_index);
        uint32_t float_minmax_operation, float_minmax_flags, float_minmax_values[2];
        int float_minmax_rc = dxil_float_minmax_intrinsic_info(builder, source, &float_minmax_operation,
                                                                &float_minmax_flags, float_minmax_values);
        if (float_minmax_rc != GPU_IR_NOT_FOUND) {
            if (float_minmax_rc != GPU_IR_OK) return float_minmax_rc;
            return append_mapped(builder, source, block_index, GPU_IR_SHADER_OP_ARITHMETIC,
                float_minmax_operation, source->type, float_minmax_flags,
                float_minmax_values, 2, NULL, 0, NULL, 0);
        }
        uint32_t minmax_operation, minmax_flags, minmax_values[2];
        int minmax_rc = dxil_integer_minmax_intrinsic_info(builder, source, &minmax_operation,
                                                            &minmax_flags, minmax_values);
        if (minmax_rc != GPU_IR_NOT_FOUND) {
            if (minmax_rc != GPU_IR_OK) return minmax_rc;
            return append_mapped(builder, source, block_index, GPU_IR_SHADER_OP_ARITHMETIC,
                minmax_operation, source->type, minmax_flags, minmax_values, 2, NULL, 0, NULL, 0);
        }
        uint32_t mad_flags, mad_values[3];
        int mad_rc = dxil_integer_mad_intrinsic_info(builder, source, &mad_flags, mad_values);
        if (mad_rc != GPU_IR_NOT_FOUND) {
            if (mad_rc != GPU_IR_OK) return mad_rc;
            return append_mapped(builder, source, block_index, GPU_IR_SHADER_OP_ARITHMETIC,
                GPU_IR_SHADER_ARITH_INTEGER_MAD, source->type, mad_flags, mad_values, 3, NULL, 0, NULL, 0);
        }
        uint32_t float_mad_flags, float_mad_values[3];
        int float_mad_rc = dxil_float_mad_intrinsic_info(builder, source,
            &float_mad_flags, float_mad_values);
        if (float_mad_rc != GPU_IR_NOT_FOUND) {
            if (float_mad_rc != GPU_IR_OK) return float_mad_rc;
            return append_mapped(builder, source, block_index, GPU_IR_SHADER_OP_ARITHMETIC,
                GPU_IR_SHADER_ARITH_FLOAT_MAD, source->type, float_mad_flags, float_mad_values, 3,
                NULL, 0, NULL, 0);
        }
        uint32_t common_opcode, unary_operation, unary_flags, unary_source;
        int unary_rc = dxil_unary_intrinsic_info(builder, source, &common_opcode, &unary_operation,
                                                  &unary_flags, &unary_source);
        if (unary_rc != GPU_IR_NOT_FOUND) {
            if (unary_rc != GPU_IR_OK) return unary_rc;
            return append_mapped(builder, source, block_index, common_opcode, unary_operation,
                source->type, unary_flags, &unary_source, 1, NULL, 0, NULL, 0);
        }
        uint32_t bit_operation, bit_source_values[4], bit_source_value_count = 0;
        int bit_rc = dxil_bit_intrinsic_info(builder, source, &bit_operation,
                                             bit_source_values, &bit_source_value_count);
        if (bit_rc != GPU_IR_NOT_FOUND) {
            if (bit_rc != GPU_IR_OK) return bit_rc;
            uint32_t bit_flags = bit_operation == GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT ||
                bit_operation == GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT ||
                bit_operation == GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT ?
                GPU_IR_SHADER_FLAG_DXIL_BITFIELD_LOW5 : 0;
            return append_mapped(builder, source, block_index, GPU_IR_SHADER_OP_ARITHMETIC,
                bit_operation, source->type, bit_flags, bit_source_values, bit_source_value_count,
                NULL, 0, NULL, 0);
        }
        int rc = append_system_value_result(builder, source, block_index);
        return rc == GPU_IR_UNSUPPORTED ? append_dxil_barrier_result(builder, source, block_index) : rc;
    }
    case DXI_UNREACHABLE: opcode = GPU_IR_SHADER_OP_UNREACHABLE; break;
    default: return GPU_IR_UNSUPPORTED;
    }
    if (source->op == DXI_PHI) return append_phi(builder, source, block_index);
    return append_mapped(builder, source, block_index, opcode, operation, result_type, flags,
        operands, operand_count, labels, label_count, literals, literal_count);
}

static int lower_instruction(dxil_ir_builder *builder, const dxil_inst *source, uint32_t block_index) {
    uint32_t capacity = source->n ? source->n : 1u;
    if (source->op == DXI_SHUFFLEVEC) {
        const dxil_type *result = source_type(builder->module, source->type);
        if (result && result->kind == DXT_VECTOR && result->count > capacity) capacity = result->count;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof(uint32_t)) return GPU_IR_OUT_OF_MEMORY;
    uint32_t *operands = calloc(capacity, sizeof *operands);
    uint32_t *labels = calloc(capacity, sizeof *labels);
    uint32_t *literals = calloc(capacity, sizeof *literals);
    if (!operands || !labels || !literals) {
        free(operands); free(labels); free(literals);
        return GPU_IR_OUT_OF_MEMORY;
    }
    int rc = lower_instruction_inner(builder, source, block_index, operands, labels, literals, capacity);
    free(operands); free(labels); free(literals);
    return rc;
}

static void builder_free(dxil_ir_builder *builder) {
    free(builder->needed_types); free(builder->type_state); free(builder->needed_values);
    free(builder->type_ids); free(builder->types); free(builder->members); free(builder->barriers);
    free(builder->atomics);
    free(builder->raw_stores);
    free(builder->blocks); free(builder->instructions); free(builder->operands); free(builder->literals);
    free(builder->handle_resource_indices); free(builder->resource_access_ids);
    free(builder->resource_typed_value_ids); free(builder->resource_byte_index_ids);
    free(builder->resource_byte_base_index_ids);
    free(builder->resource_raw_length_word_ids);
    free(builder->resource_structured_length_word_ids);
    free(builder->resource_structured_base_word_index_ids);
    free(builder->resource_structured_word_index_ids);
    free(builder->resource_structured_offset_word_constant_ids);
    free(builder->resource_values); free(builder->input_load_ids);
}

int dxil_build_gpu_ir_shader(const dxil_module *module, uint32_t function_index, gpu_ir_shader **shader) {
    if (shader) *shader = NULL;
    if (!module || !shader || !module->types || !module->values || !module->functions ||
        !module->ntypes || module->nmodule_values > UINT32_MAX - 1u)
        return GPU_IR_INVALID;
    if (function_index == DXIL_IR_ENTRY_FUNCTION) {
        int select_rc = entry_function_index(module, &function_index);
        if (select_rc != GPU_IR_OK) return select_rc;
    }
    if (function_index >= module->nfunctions) return GPU_IR_INVALID;
    const dxil_function *function = &module->functions[function_index];
    uint32_t stage = dxil_ir_stage(module->shader_kind);
    if (!stage || function->is_declaration || !function->nblocks || !function->insts ||
        (function->nlocals && !function->locals)) return GPU_IR_UNSUPPORTED;
    dxil_ir_builder builder = { .module = module, .function = function, .function_index = function_index,
        .common_stage = stage, .block_count = function->nblocks, .builtin_scalar_type = UINT32_MAX,
        .resource_u32_source_type = UINT32_MAX };
    if (module->nmodule_values > UINT32_MAX - function->nlocals) return GPU_IR_RANGE;
    builder.value_count = module->nmodule_values + function->nlocals;
    builder.needed_types = calloc(module->ntypes, 1);
    builder.type_state = calloc(module->ntypes, 1);
    builder.needed_values = calloc(builder.value_count ? builder.value_count : 1, 1);
    builder.type_ids = calloc(module->ntypes, sizeof *builder.type_ids);
    builder.blocks = calloc(function->nblocks, sizeof *builder.blocks);
    builder.barriers = calloc(function->ninsts, sizeof *builder.barriers);
    builder.atomics = calloc(function->ninsts ? function->ninsts : 1, sizeof *builder.atomics);
    builder.raw_stores = calloc(function->ninsts ? function->ninsts : 1, sizeof *builder.raw_stores);
    builder.handle_resource_indices = calloc(builder.value_count ? builder.value_count : 1,
                                             sizeof *builder.handle_resource_indices);
    builder.resource_values = calloc(builder.value_count ? builder.value_count : 1,
                                     sizeof *builder.resource_values);
    builder.input_load_ids = calloc(function->ninsts ? function->ninsts : 1,
                                    sizeof *builder.input_load_ids);
    builder.resource_access_ids = calloc(function->ninsts ? function->ninsts : 1,
                                         sizeof *builder.resource_access_ids);
    builder.resource_typed_value_ids = calloc(function->ninsts ? function->ninsts : 1,
                                                sizeof *builder.resource_typed_value_ids);
    builder.resource_byte_index_ids = calloc(function->ninsts ? function->ninsts : 1,
                                                sizeof *builder.resource_byte_index_ids);
    builder.resource_byte_base_index_ids = calloc(function->ninsts ? function->ninsts : 1,
                                                    sizeof *builder.resource_byte_base_index_ids);
    builder.resource_raw_length_word_ids = calloc(function->ninsts ? function->ninsts : 1,
                                                    sizeof *builder.resource_raw_length_word_ids);
    builder.resource_structured_length_word_ids = calloc(function->ninsts ? function->ninsts : 1,
        sizeof *builder.resource_structured_length_word_ids);
    builder.resource_structured_base_word_index_ids = calloc(function->ninsts ? function->ninsts : 1,
        sizeof *builder.resource_structured_base_word_index_ids);
    builder.resource_structured_word_index_ids = calloc(function->ninsts ? function->ninsts : 1,
        sizeof *builder.resource_structured_word_index_ids);
    builder.resource_structured_offset_word_constant_ids = calloc(function->ninsts ? function->ninsts : 1,
        sizeof *builder.resource_structured_offset_word_constant_ids);
    builder.barrier_integer_type = UINT32_MAX;
    if (!builder.needed_types || !builder.type_state || !builder.needed_values || !builder.type_ids ||
        !builder.blocks || !builder.barriers || !builder.atomics || !builder.raw_stores ||
        !builder.handle_resource_indices || !builder.resource_values || !builder.input_load_ids ||
        !builder.resource_access_ids || !builder.resource_typed_value_ids || !builder.resource_byte_index_ids ||
        !builder.resource_byte_base_index_ids || !builder.resource_raw_length_word_ids ||
        !builder.resource_structured_length_word_ids || !builder.resource_structured_base_word_index_ids ||
        !builder.resource_structured_word_index_ids || !builder.resource_structured_offset_word_constant_ids) {
        builder_free(&builder); return GPU_IR_OUT_OF_MEMORY;
    }

    for (uint32_t i = 0; i < module->ntypes; i++) builder.type_ids[i] = i + 1u;
    uint32_t function_type_id = function->type;
    const dxil_type *function_type = source_type(module, function_type_id);
    if (function_type && function_type->kind == DXT_POINTER) {
        function_type_id = function_type->elem;
        function_type = source_type(module, function_type_id);
    }
    if (!function_type || function_type->kind != DXT_FUNCTION ||
        function->value >= module->nmodule_values || function_type->vararg) {
        builder_free(&builder); return GPU_IR_UNSUPPORTED;
    }
    int rc = mark_type(&builder, function_type_id, 0);
    if (rc == GPU_IR_OK) rc = analyze_resource_instructions(&builder);
    if (rc == GPU_IR_OK) rc = analyze_dxil_graphics_stores(&builder);
    uint8_t *terminated = calloc(function->nblocks, 1);
    if (!terminated) { builder_free(&builder); return GPU_IR_OUT_OF_MEMORY; }
    for (uint32_t i = 0; rc == GPU_IR_OK && i < function->ninsts; i++) {
        const dxil_inst *instruction = &function->insts[i];
        if (instruction->block >= function->nblocks || (instruction->n && !instruction->ops)) {
            rc = GPU_IR_INVALID; break;
        }
        if (terminated[instruction->block]) { rc = GPU_IR_INVALID; break; }
        rc = mark_instruction_values(&builder, instruction);
        if (instruction->op == DXI_RET || instruction->op == DXI_BR ||
            instruction->op == DXI_SWITCH || instruction->op == DXI_UNREACHABLE)
            terminated[instruction->block] = 1;
    }
    for (uint32_t block = 0; rc == GPU_IR_OK && block < function->nblocks; block++)
        if (!terminated[block]) rc = GPU_IR_INVALID;
    free(terminated);
    if (rc == GPU_IR_OK && stage == GPU_IR_STAGE_COMPUTE)
        rc = metadata_numthreads(module, function, builder.threadgroup_size);
    if (rc == GPU_IR_OK) rc = configure_synthetic_ids(&builder);
    if (rc != GPU_IR_OK) { builder_free(&builder); return rc; }

    rc = build_types(&builder);
    if (rc != GPU_IR_OK) { builder_free(&builder); return rc; }
    for (uint32_t block = 0; block < function->nblocks; block++) {
        builder.blocks[block] = (gpu_ir_shader_block){
            .function_index = 0,
            .label_id = common_label_id(&builder, block),
            .first_instruction = 0,
            .instruction_count = 0
        };
    }

    for (uint32_t id = 0; id < builder.value_count; id++) {
        if (!builder.needed_values[id]) continue;
        const dxil_value *value = dxil_value_at(module, function, id);
        if (value && (value->kind == DXV_CONST_INT || value->kind == DXV_CONST_FLOAT ||
                      value->kind == DXV_CONST_NULL)) {
            rc = append_constant(&builder, id);
            if (rc != GPU_IR_OK) break;
        }
    }
    if (rc == GPU_IR_OK) rc = append_builtin_variables(&builder);
    if (rc == GPU_IR_OK) rc = append_dxil_fragment_position_variable(&builder);
    if (rc == GPU_IR_OK) rc = append_dxil_front_facing_variable(&builder);
    if (rc == GPU_IR_OK) rc = append_dxil_vertex_system_value_variables(&builder);
    if (rc == GPU_IR_OK) rc = append_dxil_vertex_input_variable(&builder);
    if (rc == GPU_IR_OK) rc = append_dxil_vertex_position_variable(&builder);
    if (rc == GPU_IR_OK) rc = append_dxil_target0_variable(&builder);
    if (rc == GPU_IR_OK) rc = append_resource_globals(&builder);
    if (rc == GPU_IR_OK) rc = append_dxil_barrier_constants(&builder);
    if (rc != GPU_IR_OK) { builder_free(&builder); return rc; }

    const dxil_value *entry_value = dxil_value_at(module, NULL, function->value);
    if (!entry_value || entry_value->kind != DXV_FUNCTION) { builder_free(&builder); return GPU_IR_INVALID; }
    uint32_t entry_type = common_type_id(&builder, function_type_id);
    const dxil_type *return_type_source = source_type(module, function_type->elem);
    uint32_t return_type = common_type_id(&builder, function_type->elem);
    if (!entry_type || !return_type_source || !return_type) { builder_free(&builder); return GPU_IR_INVALID; }
    builder.function_record = (gpu_ir_shader_function){
        .id = common_value_id(&builder, function->value),
        .result_type_id = return_type,
        .function_type_id = entry_type,
        .first_block = 0,
        .block_count = function->nblocks,
        .first_instruction = builder.instruction_count,
        .instruction_count = 0,
        .parameter_count = function_type->count
    };
    for (uint32_t parameter = 0; parameter < function_type->count; parameter++) {
        uint32_t source_id = function->first_arg + parameter;
        const dxil_value *argument = dxil_value_at(module, function, source_id);
        if (!argument || argument->kind != DXV_ARG || argument->u.index != parameter) { rc = GPU_IR_INVALID; break; }
        gpu_ir_shader_instruction instruction = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .opcode = GPU_IR_SHADER_OP_PARAMETER,
            .source_opcode = 0,
            .result_id = common_value_id(&builder, source_id),
            .result_type_id = common_type_id(&builder, argument->type),
            .function_index = 0,
            .block_index = UINT32_MAX
        };
        rc = append_instruction(&builder, &instruction);
        if (rc != GPU_IR_OK) break;
    }
    for (uint32_t builtin = 0; rc == GPU_IR_OK && builtin < DXIL_IR_BUILTIN_COUNT; builtin++) {
        if (!(builder.builtin_mask & (1u << builtin))) continue;
        rc = append_builtin_load(&builder, builtin);
    }
    for (uint32_t i = 0; rc == GPU_IR_OK && i < function->ninsts; i++) {
        const dxil_inst *source = &function->insts[i];
        uint32_t block = source->block;
        gpu_ir_shader_block *target = &builder.blocks[block];
        if (!target->instruction_count) target->first_instruction = builder.instruction_count;
        uint32_t before = builder.instruction_count;
        rc = lower_instruction(&builder, source, block);
        if (rc != GPU_IR_OK) break;
        if (builder.instruction_count != before) {
            target->instruction_count += builder.instruction_count - before;
            uint32_t opcode = builder.instructions[builder.instruction_count - 1].opcode;
            if (opcode == GPU_IR_SHADER_OP_BRANCH || opcode == GPU_IR_SHADER_OP_BRANCH_CONDITIONAL ||
                opcode == GPU_IR_SHADER_OP_SWITCH || opcode == GPU_IR_SHADER_OP_RETURN ||
                opcode == GPU_IR_SHADER_OP_RETURN_VALUE || opcode == GPU_IR_SHADER_OP_UNREACHABLE)
                target->terminator_opcode = opcode;
        }
    }
    if (rc == GPU_IR_OK) {
        builder.function_record.instruction_count = builder.instruction_count - builder.function_record.first_instruction;
        gpu_ir_shader_desc desc = {
            .source_api = GPU_IR_SHADER_SOURCE_DXIL,
            .stage_mask = stage,
            .entry_function_id = builder.function_record.id,
            .threadgroup_size = { builder.threadgroup_size[0], builder.threadgroup_size[1],
                                  builder.threadgroup_size[2] },
            .types = builder.types, .type_count = builder.type_count,
            .type_members = builder.members, .type_member_count = builder.member_count,
            .functions = &builder.function_record, .function_count = 1,
            .blocks = builder.blocks, .block_count = builder.block_count,
            .instructions = builder.instructions, .instruction_count = builder.instruction_count,
            .operands = builder.operands, .operand_count = builder.operand_count,
            .literal_words = builder.literals, .literal_word_count = builder.literal_count
        };
        *shader = gpu_ir_shader_create(&desc);
        if (!*shader) rc = GPU_IR_INVALID;
    }
    builder_free(&builder);
    return rc;
}
