// Initial MSL backend for the API-neutral shader IR. This is intentionally conservative: unsupported shader shapes
// fail during translation instead of producing a kernel that silently changes API-visible behavior.
#include "gpu_ir_msl.h"
#include "gpu_ir_metal.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length, capacity;
    int assign_values;
} msl_builder;

typedef struct {
    uint32_t id, type_id, is_pointer, descriptor_slot;
    uint32_t buffer_writable, descriptor_array_count;
    uint32_t image_readable, image_writable;
    char *expression, *aux_expression;
    char *buffer_index_expression;
    char *descriptor_index_expression;
    char *secondary_descriptor_index_expression;
    uint32_t secondary_descriptor_array_count;
    char *capture_argument_expression;
} msl_symbol;

typedef struct {
    msl_symbol *items;
    uint32_t count, capacity;
} msl_symbols;

static int builder_appendf(msl_builder *builder, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0) { va_end(args); return GPU_IR_INVALID; }
    size_t extra = (size_t)needed;
    if (extra > SIZE_MAX - builder->length - 1) { va_end(args); return GPU_IR_OUT_OF_MEMORY; }
    size_t required = builder->length + extra + 1;
    if (required > builder->capacity) {
        size_t capacity = builder->capacity ? builder->capacity : 256;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) { capacity = required; break; }
            capacity *= 2;
        }
        char *replacement = realloc(builder->data, capacity);
        if (!replacement) { va_end(args); return GPU_IR_OUT_OF_MEMORY; }
        builder->data = replacement;
        builder->capacity = capacity;
    }
    vsnprintf(builder->data + builder->length, builder->capacity - builder->length, format, args);
    va_end(args);
    builder->length += extra;
    return GPU_IR_OK;
}

static char *msl_string_copy(const char *source) {
    size_t length = strlen(source);
    if (length == SIZE_MAX) return NULL;
    char *copy = malloc(length + 1);
    if (copy) memcpy(copy, source, length + 1);
    return copy;
}

static const gpu_ir_shader_type *shader_type(const gpu_ir_shader *shader, uint32_t id) {
    return gpu_ir_shader_find_type(shader, id);
}

static int shader_type_is_u32(const gpu_ir_shader *shader, uint32_t id) {
    const gpu_ir_shader_type *type = shader_type(shader, id);
    return type && type->kind == GPU_IR_SHADER_TYPE_INT && type->bit_width == 32 && !type->signedness;
}

static int shader_type_is_32bit_buffer_scalar(const gpu_ir_shader *shader, uint32_t id) {
    const gpu_ir_shader_type *type = shader_type(shader, id);
    return type && type->bit_width == 32 &&
        (type->kind == GPU_IR_SHADER_TYPE_INT || type->kind == GPU_IR_SHADER_TYPE_FLOAT);
}

static int shader_type_name(const gpu_ir_shader *shader, uint32_t id, char *name, size_t capacity) {
    const gpu_ir_shader_type *type = shader_type(shader, id);
    if (!type || !capacity) return 0;
    if (type->kind == GPU_IR_SHADER_TYPE_VOID) return snprintf(name, capacity, "void") < (int)capacity;
    if (type->kind == GPU_IR_SHADER_TYPE_BOOL) return snprintf(name, capacity, "bool") < (int)capacity;
    if (type->kind == GPU_IR_SHADER_TYPE_INT && type->bit_width == 32)
        return snprintf(name, capacity, "%s", type->signedness ? "int" : "uint") < (int)capacity;
    if (type->kind == GPU_IR_SHADER_TYPE_FLOAT && type->bit_width == 32)
        return snprintf(name, capacity, "float") < (int)capacity;
    if (type->kind == GPU_IR_SHADER_TYPE_VECTOR && type->element_count >= 2 && type->element_count <= 4) {
        char element[16];
        if (!shader_type_name(shader, type->element_type_id, element, sizeof element)) return 0;
        if (!strcmp(element, "bool")) return snprintf(name, capacity, "bool%u", type->element_count) < (int)capacity;
        if (!strcmp(element, "uint")) return snprintf(name, capacity, "uint%u", type->element_count) < (int)capacity;
        if (!strcmp(element, "int")) return snprintf(name, capacity, "int%u", type->element_count) < (int)capacity;
        if (!strcmp(element, "float")) return snprintf(name, capacity, "float%u", type->element_count) < (int)capacity;
    }
    return 0;
}

static int push_constant_type_name(const gpu_ir_shader *shader, uint32_t id,
                                  char *name, size_t capacity) {
    const gpu_ir_shader_type *type = shader_type(shader, id);
    if (!type) return 0;
    const gpu_ir_shader_type *scalar = type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, type->element_type_id) : type;
    if (!scalar || scalar->bit_width != 32 ||
        (scalar->kind != GPU_IR_SHADER_TYPE_INT && scalar->kind != GPU_IR_SHADER_TYPE_FLOAT)) return 0;
    return shader_type_name(shader, id, name, capacity);
}

static int shader_integer_type_name(const gpu_ir_shader *shader, uint32_t id, uint32_t signedness,
                                    char *name, size_t capacity) {
    const gpu_ir_shader_type *type = shader_type(shader, id);
    if (!type || !capacity) return 0;
    if (type->kind == GPU_IR_SHADER_TYPE_INT && type->bit_width == 32)
        return snprintf(name, capacity, "%s", signedness ? "int" : "uint") < (int)capacity;
    if (type->kind == GPU_IR_SHADER_TYPE_VECTOR && type->element_count >= 2 && type->element_count <= 4) {
        const gpu_ir_shader_type *element = shader_type(shader, type->element_type_id);
        if (element && element->kind == GPU_IR_SHADER_TYPE_INT && element->bit_width == 32)
            return snprintf(name, capacity, "%s%u", signedness ? "int" : "uint", type->element_count) <
                (int)capacity;
    }
    return 0;
}

static char *integer_semantic_expression(const gpu_ir_shader *shader, uint32_t type_id,
                                         uint32_t signedness, const char *expression) {
    if (!expression) return NULL;
    const gpu_ir_shader_type *type = shader_type(shader, type_id);
    const gpu_ir_shader_type *scalar = type && type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, type->element_type_id) : type;
    if (!scalar || scalar->kind != GPU_IR_SHADER_TYPE_INT || scalar->bit_width != 32) return NULL;
    if (scalar->signedness == signedness) return msl_string_copy(expression);
    char type_name[32];
    if (!shader_integer_type_name(shader, type_id, signedness, type_name, sizeof type_name)) return NULL;
    msl_builder output = { 0 };
    int rc = builder_appendf(&output, "as_type<%s>(%s)", type_name, expression);
    if (rc != GPU_IR_OK) { free(output.data); return NULL; }
    return output.data;
}

static char *bit_range_low5_expression(const char *expression) {
    if (!expression) return NULL;
    msl_builder output = { 0 };
    int rc = builder_appendf(&output, "(%s & 31u)", expression);
    if (rc != GPU_IR_OK) { free(output.data); return NULL; }
    return output.data;
}

static void symbols_destroy(msl_symbols *symbols) {
    for (uint32_t i = 0; i < symbols->count; i++) {
        free(symbols->items[i].expression);
        free(symbols->items[i].aux_expression);
        free(symbols->items[i].buffer_index_expression);
        free(symbols->items[i].descriptor_index_expression);
        free(symbols->items[i].secondary_descriptor_index_expression);
        free(symbols->items[i].capture_argument_expression);
    }
    free(symbols->items);
    memset(symbols, 0, sizeof *symbols);
}

static const msl_symbol *symbols_find(const msl_symbols *symbols, uint32_t id) {
    for (uint32_t i = 0; i < symbols->count; i++)
        if (symbols->items[i].id == id) return &symbols->items[i];
    return NULL;
}

static int symbols_add(msl_symbols *symbols, uint32_t id, uint32_t type_id, int is_pointer,
                       const char *expression) {
    if (!id || !type_id || !expression) return GPU_IR_INVALID;
    const msl_symbol *existing = symbols_find(symbols, id);
    if (existing) return !is_pointer && !existing->is_pointer && existing->type_id == type_id &&
        !strcmp(existing->expression, expression) ? GPU_IR_OK : GPU_IR_INVALID;
    if (symbols->count == symbols->capacity) {
        uint32_t capacity = symbols->capacity ? symbols->capacity * 2u : 32u;
        size_t allocation_size = (size_t)capacity * sizeof *symbols->items;
        if (capacity < symbols->capacity || allocation_size / sizeof *symbols->items != capacity)
            return GPU_IR_OUT_OF_MEMORY;
        msl_symbol *replacement = realloc(symbols->items, allocation_size);
        if (!replacement) return GPU_IR_OUT_OF_MEMORY;
        symbols->items = replacement;
        symbols->capacity = capacity;
    }
    char *copy = msl_string_copy(expression);
    if (!copy) return GPU_IR_OUT_OF_MEMORY;
    symbols->items[symbols->count++] = (msl_symbol){
        .id = id,
        .type_id = type_id,
        .is_pointer = (uint32_t)(is_pointer != 0),
        .descriptor_slot = UINT32_MAX,
        .expression = copy
    };
    return GPU_IR_OK;
}

static int emit_value_lhs(const gpu_ir_shader *shader, msl_builder *body,
                          const gpu_ir_shader_instruction *instruction) {
    if (body->assign_values)
        return builder_appendf(body, "    vx_value_%u = ", instruction->result_id);
    char type_name[32];
    if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name))
        return GPU_IR_UNSUPPORTED;
    return builder_appendf(body, "    %s vx_value_%u = ", type_name, instruction->result_id);
}

static int symbols_set_buffer_access(msl_symbols *symbols, uint32_t id, uint32_t descriptor_slot,
                                     const char *index_expression, int writable) {
    if (!symbols || descriptor_slot >= GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS) return GPU_IR_INVALID;
    msl_symbol *symbol = NULL;
    for (uint32_t i = 0; i < symbols->count; i++)
        if (symbols->items[i].id == id) { symbol = &symbols->items[i]; break; }
    if (!symbol || !symbol->is_pointer) return GPU_IR_INVALID;
    char *copy = index_expression ? msl_string_copy(index_expression) : NULL;
    if (index_expression && !copy) return GPU_IR_OUT_OF_MEMORY;
    free(symbol->buffer_index_expression);
    symbol->descriptor_slot = descriptor_slot;
    symbol->buffer_writable = writable != 0;
    symbol->buffer_index_expression = copy;
    return GPU_IR_OK;
}

static int symbols_set_image_access(msl_symbols *symbols, uint32_t id, int readable, int writable) {
    if (!symbols || (!readable && !writable)) return GPU_IR_INVALID;
    for (uint32_t i = 0; i < symbols->count; i++) {
        if (symbols->items[i].id != id) continue;
        symbols->items[i].image_readable = readable != 0;
        symbols->items[i].image_writable = writable != 0;
        return GPU_IR_OK;
    }
    return GPU_IR_INVALID;
}

static int symbols_set_descriptor_array_count(msl_symbols *symbols, uint32_t id, uint32_t count) {
    if (!symbols || !count) return GPU_IR_INVALID;
    for (uint32_t i = 0; i < symbols->count; i++) {
        if (symbols->items[i].id != id) continue;
        if (!symbols->items[i].is_pointer) return GPU_IR_INVALID;
        symbols->items[i].descriptor_array_count = count;
        return GPU_IR_OK;
    }
    return GPU_IR_INVALID;
}

static int symbols_set_descriptor_index(msl_symbols *symbols, uint32_t id,
                                               const char *index_expression, uint32_t count) {
    if (!symbols || !index_expression || !count) return GPU_IR_INVALID;
    msl_symbol *symbol = NULL;
    for (uint32_t i = 0; i < symbols->count; i++)
        if (symbols->items[i].id == id) { symbol = &symbols->items[i]; break; }
    if (!symbol) return GPU_IR_INVALID;
    if (symbol->descriptor_index_expression &&
        !strcmp(symbol->descriptor_index_expression, index_expression) &&
        symbol->descriptor_array_count == count) return GPU_IR_OK;
    if (!symbol->descriptor_index_expression) {
        char *copy = msl_string_copy(index_expression);
        if (!copy) return GPU_IR_OUT_OF_MEMORY;
        symbol->descriptor_index_expression = copy;
        symbol->descriptor_array_count = count;
        return GPU_IR_OK;
    }
    if (symbol->secondary_descriptor_index_expression) {
        if (!strcmp(symbol->secondary_descriptor_index_expression, index_expression))
            return symbol->secondary_descriptor_array_count == count ? GPU_IR_OK : GPU_IR_UNSUPPORTED;
        return GPU_IR_UNSUPPORTED;
    }
    char *copy = msl_string_copy(index_expression);
    if (!copy) return GPU_IR_OUT_OF_MEMORY;
    symbol->secondary_descriptor_index_expression = copy;
    symbol->secondary_descriptor_array_count = count;
    return GPU_IR_OK;
}

static int symbols_copy_descriptor_indices(msl_symbols *symbols, uint32_t destination_id,
                                           const msl_symbol *source) {
    if (!source) return GPU_IR_INVALID;
    int rc = GPU_IR_OK;
    if (source->descriptor_index_expression)
        rc = symbols_set_descriptor_index(symbols, destination_id, source->descriptor_index_expression,
                                          source->descriptor_array_count);
    if (rc == GPU_IR_OK && source->secondary_descriptor_index_expression)
        rc = symbols_set_descriptor_index(symbols, destination_id,
            source->secondary_descriptor_index_expression, source->secondary_descriptor_array_count);
    return rc;
}

static int symbols_set_capture_argument(msl_symbols *symbols, uint32_t id, const char *expression) {
    if (!symbols || !expression) return GPU_IR_INVALID;
    msl_symbol *symbol = NULL;
    for (uint32_t i = 0; i < symbols->count; i++)
        if (symbols->items[i].id == id) { symbol = &symbols->items[i]; break; }
    if (!symbol || !symbol->is_pointer) return GPU_IR_INVALID;
    char *copy = msl_string_copy(expression);
    if (!copy) return GPU_IR_OUT_OF_MEMORY;
    free(symbol->capture_argument_expression);
    symbol->capture_argument_expression = copy;
    return GPU_IR_OK;
}

static int symbols_set_aux_expression(msl_symbols *symbols, uint32_t id, const char *expression) {
    if (!symbols || !expression) return GPU_IR_INVALID;
    for (uint32_t i = 0; i < symbols->count; i++) {
        if (symbols->items[i].id != id) continue;
        char *copy = msl_string_copy(expression);
        if (!copy) return GPU_IR_OUT_OF_MEMORY;
        free(symbols->items[i].aux_expression);
        symbols->items[i].aux_expression = copy;
        return GPU_IR_OK;
    }
    return GPU_IR_INVALID;
}

static int append_descriptor_array_selection(msl_builder *selection, const char *prefix, uint32_t variable_id,
                                             uint32_t count, const char *index_expression) {
    if (!selection || !prefix || !count || !index_expression) return GPU_IR_INVALID;
    int rc = GPU_IR_OK;
    for (uint32_t element = 0; rc == GPU_IR_OK && element + 1 < count; element++)
        rc = builder_appendf(selection, "(uint(%s) == %uu ? %s%u_%u : ",
                             index_expression, element, prefix, variable_id, element);
    if (rc == GPU_IR_OK)
        rc = builder_appendf(selection, "%s%u_%u", prefix, variable_id, count - 1);
    for (uint32_t element = 0; rc == GPU_IR_OK && element + 1 < count; element++)
        rc = builder_appendf(selection, ")");
    return rc;
}

static int emit_descriptor_bounds_condition(msl_builder *body, const msl_symbol *symbol) {
    if (!body || !symbol) return GPU_IR_INVALID;
    if (!symbol->descriptor_index_expression && !symbol->secondary_descriptor_index_expression)
        return GPU_IR_INVALID;
    int rc = GPU_IR_OK;
    if (symbol->descriptor_index_expression) {
        if (!symbol->descriptor_array_count) return GPU_IR_INVALID;
        rc = builder_appendf(body, "(uint(%s) < %uu", symbol->descriptor_index_expression,
                             symbol->descriptor_array_count);
    } else {
        if (!symbol->secondary_descriptor_array_count) return GPU_IR_INVALID;
        rc = builder_appendf(body, "(uint(%s) < %uu", symbol->secondary_descriptor_index_expression,
                             symbol->secondary_descriptor_array_count);
    }
    if (rc == GPU_IR_OK && symbol->secondary_descriptor_index_expression) {
        if (!symbol->secondary_descriptor_array_count) return GPU_IR_INVALID;
        rc = builder_appendf(body, " && uint(%s) < %uu", symbol->secondary_descriptor_index_expression,
                             symbol->secondary_descriptor_array_count);
    }
    if (rc == GPU_IR_OK) rc = builder_appendf(body, ")");
    return rc;
}

static const gpu_ir_shader_operand *instruction_operand(const gpu_ir_shader *shader,
                                                        const gpu_ir_shader_instruction *instruction,
                                                        uint32_t index) {
    if (index >= instruction->operand_count) return NULL;
    return gpu_ir_shader_operand_at(shader, instruction->first_operand + index);
}

static int instruction_id_operand(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                                  uint32_t index, uint32_t *id) {
    const gpu_ir_shader_operand *operand = instruction_operand(shader, instruction, index);
    if (!operand || operand->kind != GPU_IR_SHADER_OPERAND_ID || !operand->value) return 0;
    *id = operand->value;
    return 1;
}

static int constant_u32(const gpu_ir_shader *shader, uint32_t id, uint32_t *value) {
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->result_id != id) continue;
        const gpu_ir_shader_type *type = shader_type(shader, instruction->result_type_id);
        if (instruction->opcode != GPU_IR_SHADER_OP_CONSTANT ||
            (instruction->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) ||
            instruction->operation != GPU_IR_SHADER_CONSTANT_SCALAR ||
            !type || type->kind != GPU_IR_SHADER_TYPE_INT || type->bit_width != 32 ||
            instruction->literal_word_count != 1) return 0;
        *value = gpu_ir_shader_literal_word_at(shader, instruction->first_literal);
        return 1;
    }
    return 0;
}

static int shader_barrier_flags(const gpu_ir_shader *shader, uint32_t semantics_id,
                                const char **flags, uint32_t *ordering) {
    uint32_t semantics;
    if (!flags || !ordering || !constant_u32(shader, semantics_id, &semantics)) return GPU_IR_INVALID;
    // SPIR-V Memory Semantics: ordering bits [1..4], memory classes [6..12].
    const uint32_t ordering_mask = 0x1eu;
    const uint32_t vulkan_memory_mask = 0x40u | 0x100u | 0x800u; // Uniform, Workgroup, Image
    if (semantics & ~(ordering_mask | 0x1fc0u)) return GPU_IR_UNSUPPORTED;
    *ordering = semantics & ordering_mask;
    if (*ordering && (*ordering & (*ordering - 1u))) return GPU_IR_INVALID;

    // Vulkan's compute memory classes map to Metal's device, threadgroup, and texture flags.
    // Other SPIR-V classes (for example CrossWorkgroup or SubgroupMemory) have no mapping here.
    uint32_t memory = semantics & 0x1fc0u;
    if (memory & ~vulkan_memory_mask) return GPU_IR_UNSUPPORTED;
    unsigned flags_index = !!(memory & 0x40u) | (!!(memory & 0x100u) << 1) |
                           (!!(memory & 0x800u) << 2);
    static const char *const flag_names[] = {
        "mem_flags::mem_none",
        "mem_flags::mem_device",
        "mem_flags::mem_threadgroup",
        "mem_flags::mem_device | mem_flags::mem_threadgroup",
        "mem_flags::mem_texture",
        "mem_flags::mem_device | mem_flags::mem_texture",
        "mem_flags::mem_threadgroup | mem_flags::mem_texture",
        "mem_flags::mem_device | mem_flags::mem_threadgroup | mem_flags::mem_texture"
    };
    *flags = flag_names[flags_index];
    return GPU_IR_OK;
}

static const char *shader_memory_scope_name(uint32_t scope) {
    switch (scope) {
    case 1: return "thread_scope_device";
    case 2: return "thread_scope_threadgroup";
    case 3: return "thread_scope_simdgroup";
    case 4: return "thread_scope_thread";
    default: return NULL;
    }
}

static int emit_barrier(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                        msl_builder *body) {
    if ((instruction->source_api != GPU_IR_SHADER_SOURCE_SPIRV &&
         instruction->source_api != GPU_IR_SHADER_SOURCE_DXIL) ||
        (instruction->operation != GPU_IR_SHADER_BARRIER_CONTROL &&
         instruction->operation != GPU_IR_SHADER_BARRIER_MEMORY)) return GPU_IR_UNSUPPORTED;
    uint32_t scope, execution_scope = 0, semantics_id;
    if (instruction->operation == GPU_IR_SHADER_BARRIER_CONTROL) {
        if (instruction->operand_count != 3 ||
            !instruction_id_operand(shader, instruction, 0, &execution_scope) ||
            !instruction_id_operand(shader, instruction, 1, &scope) ||
            !instruction_id_operand(shader, instruction, 2, &semantics_id)) return GPU_IR_INVALID;
        if (!constant_u32(shader, execution_scope, &execution_scope) ||
            (execution_scope != 2u && execution_scope != 3u))
            return GPU_IR_UNSUPPORTED;
    } else {
        if (instruction->operand_count != 2 ||
            !instruction_id_operand(shader, instruction, 0, &scope) ||
            !instruction_id_operand(shader, instruction, 1, &semantics_id)) return GPU_IR_INVALID;
    }
    if (!constant_u32(shader, scope, &scope) || !shader_memory_scope_name(scope))
        return GPU_IR_UNSUPPORTED;
    const char *flags = NULL;
    uint32_t ordering = 0;
    int rc = shader_barrier_flags(shader, semantics_id, &flags, &ordering);
    if (rc != GPU_IR_OK) return rc;
    if (instruction->operation == GPU_IR_SHADER_BARRIER_CONTROL) {
        if (scope == 4u && ordering) return GPU_IR_INVALID;
        if (ordering == 0u && strcmp(flags, "mem_flags::mem_none")) return GPU_IR_INVALID;
        if (execution_scope == 2u)
            return builder_appendf(body, "    threadgroup_barrier(%s);\n", flags);
        return builder_appendf(body, "    simdgroup_barrier(%s);\n", flags);
    }
    if (ordering == 0u || scope == 4u) return GPU_IR_INVALID;
    // Metal 3.x exposes seq_cst fences; using the stronger order preserves non-relaxed SPIR-V ordering.
    return builder_appendf(body,
        "    atomic_thread_fence(%s, memory_order_seq_cst, %s);\n", flags,
        shader_memory_scope_name(scope));
}

static int emit_discard(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                        const msl_symbols *symbols, msl_builder *body) {
    if (gpu_ir_shader_stage(shader) != GPU_IR_STAGE_FRAGMENT) return GPU_IR_UNSUPPORTED;
    uint32_t condition_id;
    if (!instruction || instruction->operation || instruction->flags || instruction->result_id ||
        instruction->result_type_id || instruction->operand_count != 1 || instruction->literal_word_count ||
        !instruction_id_operand(shader, instruction, 0, &condition_id)) return GPU_IR_INVALID;
    const msl_symbol *condition = symbols_find(symbols, condition_id);
    const gpu_ir_shader_type *condition_type = condition ? shader_type(shader, condition->type_id) : NULL;
    if (!condition || condition->is_pointer || !condition_type ||
        condition_type->kind != GPU_IR_SHADER_TYPE_BOOL) return GPU_IR_UNSUPPORTED;
    return builder_appendf(body, "    if (%s) discard_fragment();\n", condition->expression);
}

static int constant_expression(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                               const msl_symbols *symbols, char *expression, size_t capacity) {
    if (instruction->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *type = shader_type(shader, instruction->result_type_id);
    if (!type) return GPU_IR_INVALID;
    if (instruction->operation == GPU_IR_SHADER_CONSTANT_BOOL) {
        if (type->kind != GPU_IR_SHADER_TYPE_BOOL || instruction->literal_word_count != 1) return GPU_IR_INVALID;
        uint32_t value = gpu_ir_shader_literal_word_at(shader, instruction->first_literal);
        if (value > 1) return GPU_IR_INVALID;
        return snprintf(expression, capacity, "%s", value ? "true" : "false") < (int)capacity ?
            GPU_IR_OK : GPU_IR_RANGE;
    }
    if (instruction->operation == GPU_IR_SHADER_CONSTANT_COMPOSITE) {
        if (!symbols || type->kind != GPU_IR_SHADER_TYPE_VECTOR || type->element_count < 2 ||
            type->element_count > 4 || instruction->operand_count != type->element_count ||
            instruction->literal_word_count) return GPU_IR_UNSUPPORTED;
        char type_name[32];
        if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name))
            return GPU_IR_UNSUPPORTED;
        msl_builder composite = { 0 };
        int rc = builder_appendf(&composite, "%s(", type_name);
        for (uint32_t i = 0; rc == GPU_IR_OK && i < instruction->operand_count; i++) {
            uint32_t constituent_id;
            if (!instruction_id_operand(shader, instruction, i, &constituent_id)) {
                rc = GPU_IR_INVALID;
                break;
            }
            const msl_symbol *constituent = symbols_find(symbols, constituent_id);
            if (!constituent || constituent->is_pointer || constituent->type_id != type->element_type_id) {
                rc = GPU_IR_UNSUPPORTED;
                break;
            }
            if (i) rc = builder_appendf(&composite, ", ");
            if (rc == GPU_IR_OK) rc = builder_appendf(&composite, "%s", constituent->expression);
        }
        if (rc == GPU_IR_OK) rc = builder_appendf(&composite, ")");
        if (rc == GPU_IR_OK && composite.length >= capacity) rc = GPU_IR_RANGE;
        if (rc == GPU_IR_OK) memcpy(expression, composite.data, composite.length + 1);
        free(composite.data);
        return rc;
    }
    if (instruction->operation != GPU_IR_SHADER_CONSTANT_SCALAR || instruction->literal_word_count != 1)
        return GPU_IR_UNSUPPORTED;
    uint32_t bits = gpu_ir_shader_literal_word_at(shader, instruction->first_literal);
    if (type->kind == GPU_IR_SHADER_TYPE_INT && type->bit_width == 32) {
        if (type->signedness) {
            int32_t signed_value;
            memcpy(&signed_value, &bits, sizeof signed_value);
            return snprintf(expression, capacity, "int(%lldll)", (long long)signed_value) < (int)capacity ?
                GPU_IR_OK : GPU_IR_RANGE;
        }
        return snprintf(expression, capacity, "uint(0x%08xu)", bits) < (int)capacity ? GPU_IR_OK : GPU_IR_RANGE;
    }
    if (type->kind == GPU_IR_SHADER_TYPE_FLOAT && type->bit_width == 32)
        return snprintf(expression, capacity, "as_type<float>(0x%08xu)", bits) < (int)capacity ?
            GPU_IR_OK : GPU_IR_RANGE;
    return GPU_IR_UNSUPPORTED;
}

static int global_resource_shape(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *variable,
                                 uint32_t *descriptor_array_count) {
    if (variable->storage_class != GPU_IR_SHADER_STORAGE_BUFFER ||
        !variable->has_descriptor_set || !variable->has_binding ||
        (variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_READABLE) ||
        (variable->decoration_flags & ~(GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE |
                                        GPU_IR_SHADER_TYPE_FLAG_NON_READABLE)))
        return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *pointer = shader_type(shader, variable->result_type_id);
    const gpu_ir_shader_type *resource_type = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
        shader_type(shader, pointer->element_type_id) : NULL;
    uint32_t descriptor_count = 1;
    if (resource_type && resource_type->kind == GPU_IR_SHADER_TYPE_ARRAY) {
        if (!constant_u32(shader, resource_type->array_length_id, &descriptor_count) || !descriptor_count ||
            descriptor_count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS) return GPU_IR_UNSUPPORTED;
        resource_type = shader_type(shader, resource_type->element_type_id);
    }
    const gpu_ir_shader_type *block = resource_type;
    if (!block || block->kind != GPU_IR_SHADER_TYPE_STRUCT || block->member_count != 1 ||
        !(block->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_BLOCK)) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_member *member = gpu_ir_shader_type_member_at(shader, block->first_member);
    const gpu_ir_shader_type *array = member ? shader_type(shader, member->type_id) : NULL;
    if (!member || (member->decoration_flags &
        (GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE | GPU_IR_SHADER_TYPE_FLAG_NON_READABLE)) ||
        !member->has_offset || member->offset != 0 || !array ||
        array->kind != GPU_IR_SHADER_TYPE_RUNTIME_ARRAY || !array->has_array_stride || array->array_stride != 4 ||
        !shader_type_is_32bit_buffer_scalar(shader, array->element_type_id)) return GPU_IR_UNSUPPORTED;
    if (variable->operand_count) return GPU_IR_UNSUPPORTED;
    if (descriptor_array_count) *descriptor_array_count = descriptor_count;
    return GPU_IR_OK;
}

static int uniform_constant_descriptor_type(const gpu_ir_shader *shader,
                                            const gpu_ir_shader_instruction *variable,
                                            const gpu_ir_shader_type **descriptor_type,
                                            uint32_t *descriptor_count) {
    if (!shader || !variable || variable->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT ||
        !variable->has_descriptor_set || !variable->has_binding || variable->operand_count)
        return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *pointer = shader_type(shader, variable->result_type_id);
    const gpu_ir_shader_type *type = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
        shader_type(shader, pointer->element_type_id) : NULL;
    uint32_t count = 1;
    if (type && type->kind == GPU_IR_SHADER_TYPE_ARRAY) {
        if (!constant_u32(shader, type->array_length_id, &count) || !count ||
            count > GPU_IR_METAL_MAX_COMPUTE_TEXTURES) return GPU_IR_UNSUPPORTED;
        type = shader_type(shader, type->element_type_id);
    }
    if (!pointer || pointer->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT || !type)
        return GPU_IR_UNSUPPORTED;
    if (descriptor_type) *descriptor_type = type;
    if (descriptor_count) *descriptor_count = count;
    return GPU_IR_OK;
}

static int global_storage_image_shape(const gpu_ir_shader *shader,
                                      const gpu_ir_shader_instruction *variable,
                                      const gpu_ir_shader_type **image_type,
                                      int *readable, int *writable) {
    const gpu_ir_shader_type *image = NULL;
    if (!shader || !variable ||
        uniform_constant_descriptor_type(shader, variable, &image, NULL) != GPU_IR_OK ||
        (variable->decoration_flags & ~(GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE |
                                        GPU_IR_SHADER_TYPE_FLAG_NON_READABLE)) ||
        ((variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE) &&
         (variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_READABLE)))
        return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *component = image && image->kind == GPU_IR_SHADER_TYPE_IMAGE ?
        shader_type(shader, image->element_type_id) : NULL;
    if (!image ||
        image->kind != GPU_IR_SHADER_TYPE_IMAGE || !component || component->kind != GPU_IR_SHADER_TYPE_FLOAT ||
        component->bit_width != 32 || image->image_dimension != 1 || image->image_depth ||
        image->image_arrayed || image->image_multisampled || image->image_sampled != 2 ||
        image->image_format != 4 || image->image_access_qualifier)
        return GPU_IR_UNSUPPORTED;
    if (image_type) *image_type = image;
    if (readable) *readable = !(variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_READABLE);
    if (writable) *writable = !(variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE);
    return GPU_IR_OK;
}

static int global_sampled_image_shape(const gpu_ir_shader *shader,
                                      const gpu_ir_shader_instruction *variable,
                                      const gpu_ir_shader_type **image_type) {
    const gpu_ir_shader_type *image = NULL;
    if (!shader || !variable ||
        uniform_constant_descriptor_type(shader, variable, &image, NULL) != GPU_IR_OK ||
        variable->decoration_flags) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *component = image && image->kind == GPU_IR_SHADER_TYPE_IMAGE ?
        shader_type(shader, image->element_type_id) : NULL;
    if (!image || image->kind != GPU_IR_SHADER_TYPE_IMAGE || !component ||
        (component->kind != GPU_IR_SHADER_TYPE_FLOAT && component->kind != GPU_IR_SHADER_TYPE_INT) ||
        component->bit_width != 32 || image->image_dimension != 1 || image->image_depth ||
        image->image_sampled != 1 || image->image_format != 0 || image->image_access_qualifier)
        return GPU_IR_UNSUPPORTED;
    if (image_type) *image_type = image;
    return GPU_IR_OK;
}

static const char *sampled_image_component_name(const gpu_ir_shader *shader,
                                               const gpu_ir_shader_type *image) {
    const gpu_ir_shader_type *component = image && image->kind == GPU_IR_SHADER_TYPE_IMAGE ?
        shader_type(shader, image->element_type_id) : NULL;
    if (!component || component->bit_width != 32) return NULL;
    if (component->kind == GPU_IR_SHADER_TYPE_FLOAT) return "float";
    if (component->kind == GPU_IR_SHADER_TYPE_INT) return component->signedness ? "int" : "uint";
    return NULL;
}

static int pointer_value_from_global(const gpu_ir_shader *shader, uint32_t pointer_id,
                                    uint32_t variable_id, uint32_t depth) {
    if (!shader || !pointer_id || depth > 16) return 0;
    if (pointer_id == variable_id) return 1;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *definition = gpu_ir_shader_instruction_at(shader, i);
        if (!definition || definition->result_id != pointer_id) continue;
        if ((definition->opcode == GPU_IR_SHADER_OP_ACCESS_CHAIN ||
             definition->opcode == GPU_IR_SHADER_OP_COPY) && definition->operand_count) {
            uint32_t source_id;
            return instruction_id_operand(shader, definition, 0, &source_id) &&
                pointer_value_from_global(shader, source_id, variable_id, depth + 1);
        }
        if (definition->opcode == GPU_IR_SHADER_OP_PHI) {
            for (uint32_t operand = 0; operand < definition->operand_count; operand += 2) {
                uint32_t source_id;
                if (instruction_id_operand(shader, definition, operand, &source_id) &&
                    pointer_value_from_global(shader, source_id, variable_id, depth + 1)) return 1;
            }
        }
        return 0;
    }
    return 0;
}

static int sampled_image_value_from_global(const gpu_ir_shader *shader, uint32_t value_id,
                                           uint32_t variable_id, uint32_t depth) {
    if (!shader || !value_id || depth > 16) return 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *definition = gpu_ir_shader_instruction_at(shader, i);
        if (!definition || definition->result_id != value_id) continue;
        if (definition->opcode == GPU_IR_SHADER_OP_LOAD) {
            uint32_t pointer_id;
            return definition->operand_count == 1 &&
                instruction_id_operand(shader, definition, 0, &pointer_id) &&
                pointer_value_from_global(shader, pointer_id, variable_id, depth + 1);
        }
        if (definition->opcode == GPU_IR_SHADER_OP_COPY && definition->operand_count == 1) {
            uint32_t source_id;
            return instruction_id_operand(shader, definition, 0, &source_id) &&
                sampled_image_value_from_global(shader, source_id, variable_id, depth + 1);
        }
        if (definition->opcode == GPU_IR_SHADER_OP_PHI) {
            for (uint32_t operand = 0; operand < definition->operand_count; operand += 2) {
                uint32_t source_id;
                if (instruction_id_operand(shader, definition, operand, &source_id) &&
                    sampled_image_value_from_global(shader, source_id, variable_id, depth + 1)) return 1;
            }
        }
        return 0;
    }
    return 0;
}

static int global_sampled_image_used_for_sampling(const gpu_ir_shader *shader,
                                                  const gpu_ir_shader_instruction *variable) {
    if (!shader || !variable) return 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        uint32_t image_id;
        if (instruction && instruction->opcode == GPU_IR_SHADER_OP_SAMPLED_IMAGE &&
            instruction_id_operand(shader, instruction, 0, &image_id) &&
            sampled_image_value_from_global(shader, image_id, variable->result_id, 0)) return 1;
    }
    return 0;
}

static int global_sampler_shape(const gpu_ir_shader *shader,
                                const gpu_ir_shader_instruction *variable) {
    const gpu_ir_shader_type *sampler = NULL;
    if (!shader || !variable ||
        uniform_constant_descriptor_type(shader, variable, &sampler, NULL) != GPU_IR_OK ||
        variable->decoration_flags) return GPU_IR_UNSUPPORTED;
    return sampler && sampler->kind == GPU_IR_SHADER_TYPE_SAMPLER ? GPU_IR_OK : GPU_IR_UNSUPPORTED;
}

static int global_combined_image_shape(const gpu_ir_shader *shader,
                                       const gpu_ir_shader_instruction *variable,
                                       const gpu_ir_shader_type **image_type) {
    const gpu_ir_shader_type *sampled = NULL;
    if (!shader || !variable ||
        uniform_constant_descriptor_type(shader, variable, &sampled, NULL) != GPU_IR_OK ||
        variable->decoration_flags) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *image = sampled && sampled->kind == GPU_IR_SHADER_TYPE_SAMPLED_IMAGE ?
        shader_type(shader, sampled->element_type_id) : NULL;
    const gpu_ir_shader_type *component = image && image->kind == GPU_IR_SHADER_TYPE_IMAGE ?
        shader_type(shader, image->element_type_id) : NULL;
    if (!sampled ||
        sampled->kind != GPU_IR_SHADER_TYPE_SAMPLED_IMAGE || !image ||
        image->kind != GPU_IR_SHADER_TYPE_IMAGE || !component || component->kind != GPU_IR_SHADER_TYPE_FLOAT ||
        component->bit_width != 32 || image->image_dimension != 1 || image->image_depth ||
        image->image_arrayed || image->image_multisampled || image->image_sampled != 1 ||
        image->image_format != 0 || image->image_access_qualifier)
        return GPU_IR_UNSUPPORTED;
    if (image_type) *image_type = image;
    return GPU_IR_OK;
}

static int storage_image_parameter_type(const gpu_ir_shader *shader,
                                       const gpu_ir_shader_instruction *variable,
                                       char *type_name, size_t type_capacity,
                                       int *readable, int *writable) {
    int can_read = 0, can_write = 0;
    int rc = global_storage_image_shape(shader, variable, NULL, &can_read, &can_write);
    if (rc != GPU_IR_OK) return rc;
    const char *access = can_read && can_write ? "read_write" : can_read ? "read" : "write";
    if (!type_name || !type_capacity ||
        snprintf(type_name, type_capacity, "texture2d<float, access::%s>", access) >= (int)type_capacity)
        return GPU_IR_RANGE;
    if (readable) *readable = can_read;
    if (writable) *writable = can_write;
    return GPU_IR_OK;
}

static const gpu_ir_shader_type *global_resource_element_type(const gpu_ir_shader *shader,
                                                               const gpu_ir_shader_instruction *variable) {
    const gpu_ir_shader_type *pointer = shader_type(shader, variable->result_type_id);
    const gpu_ir_shader_type *resource = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
        shader_type(shader, pointer->element_type_id) : NULL;
    if (resource && resource->kind == GPU_IR_SHADER_TYPE_ARRAY)
        resource = shader_type(shader, resource->element_type_id);
    const gpu_ir_shader_type *block = resource && resource->kind == GPU_IR_SHADER_TYPE_STRUCT ? resource : NULL;
    const gpu_ir_shader_member *member = block && block->member_count == 1 ?
        gpu_ir_shader_type_member_at(shader, block->first_member) : NULL;
    const gpu_ir_shader_type *array = member ? shader_type(shader, member->type_id) : NULL;
    return array && array->kind == GPU_IR_SHADER_TYPE_RUNTIME_ARRAY ?
        shader_type(shader, array->element_type_id) : NULL;
}

static int global_uniform_buffer_shape(const gpu_ir_shader *shader,
                                       const gpu_ir_shader_instruction *variable) {
    if (variable->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM ||
        !variable->has_descriptor_set || !variable->has_binding || variable->operand_count ||
        (variable->decoration_flags & ~GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE))
        return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *pointer = shader_type(shader, variable->result_type_id);
    const gpu_ir_shader_type *block = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
        shader_type(shader, pointer->element_type_id) : NULL;
    if (!pointer || pointer->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM || !block ||
        block->kind != GPU_IR_SHADER_TYPE_STRUCT || !block->member_count ||
        !(block->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_BLOCK) ||
        (block->decoration_flags & ~(GPU_IR_SHADER_TYPE_FLAG_BLOCK | GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE)))
        return GPU_IR_UNSUPPORTED;
    uint32_t previous_end = 0;
    for (uint32_t i = 0; i < block->member_count; i++) {
        const gpu_ir_shader_member *member = gpu_ir_shader_type_member_at(shader, block->first_member + i);
        if (!member || !member->has_offset || (member->offset & 3u) ||
            member->offset < previous_end || member->offset > UINT32_MAX - 4u ||
            !shader_type_is_32bit_buffer_scalar(shader, member->type_id) ||
            (member->decoration_flags & ~GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE))
            return GPU_IR_UNSUPPORTED;
        previous_end = member->offset + 4u;
    }
    return GPU_IR_OK;
}

static int layout_binding_supported(const gpu_ir_descriptor_binding *entry) {
    if (!entry || !entry->count || entry->count == GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED) return 0;
    if (entry->set == GPU_IR_INTERNAL_SET &&
        entry->binding_namespace == GPU_IR_INTERNAL_BINDING_NAMESPACE &&
        entry->binding == GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING)
        return entry->kind == GPU_IR_DESCRIPTOR_UNIFORM_BUFFER && entry->count == 1 &&
               entry->access_mask == GPU_IR_ACCESS_UNIFORM_READ;
    if (entry->binding_namespace == 0)
        return entry->kind == GPU_IR_DESCRIPTOR_STORAGE_BUFFER ||
               entry->kind == GPU_IR_DESCRIPTOR_UNIFORM_BUFFER ||
               entry->kind == GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE ||
               (entry->kind == GPU_IR_DESCRIPTOR_STORAGE_IMAGE &&
                entry->count <= GPU_IR_METAL_MAX_COMPUTE_STORAGE_IMAGES && entry->access_mask &&
                !(entry->access_mask & ~(GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE))) ||
               (entry->kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE &&
                entry->count <= GPU_IR_METAL_MAX_COMPUTE_TEXTURES &&
                entry->access_mask == GPU_IR_ACCESS_SHADER_READ) ||
               (entry->kind == GPU_IR_DESCRIPTOR_SAMPLER &&
                entry->count <= GPU_IR_METAL_MAX_COMPUTE_SAMPLERS && !entry->access_mask) ||
               (entry->kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER &&
                entry->count <= GPU_IR_METAL_MAX_COMPUTE_TEXTURES &&
                entry->access_mask == GPU_IR_ACCESS_SHADER_READ);
    if (entry->binding_namespace == 1) return entry->kind == GPU_IR_DESCRIPTOR_STORAGE_RESOURCE;
    return 0;
}

static int layout_binding_is_image_descriptor(const gpu_ir_shader *shader,
                                              const gpu_ir_descriptor_binding *entry) {
    if (!shader || !entry || entry->binding_namespace != 0) return 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
        if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE ||
            variable->function_index != UINT32_MAX ||
            variable->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT ||
            variable->resource_set != entry->set || variable->binding_namespace != entry->binding_namespace ||
            variable->binding != entry->binding) continue;
        if (global_storage_image_shape(shader, variable, NULL, NULL, NULL) == GPU_IR_OK ||
            global_sampled_image_shape(shader, variable, NULL) == GPU_IR_OK ||
            global_combined_image_shape(shader, variable, NULL) == GPU_IR_OK) return 1;
    }
    return 0;
}

static int descriptor_slot(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                           const gpu_ir_shader_instruction *variable, uint32_t descriptor_count,
                           uint32_t stage_mask, uint32_t *slot) {
    if (!variable || !slot) return GPU_IR_INVALID;
    int push_constant = variable && variable->storage_class == GPU_IR_SHADER_STORAGE_PUSH_CONSTANT;
    if (push_constant && descriptor_count != 1) return GPU_IR_INVALID;
    if (!layout) {
        uint32_t next_slot = 0;
        for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
            const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(shader, i);
            if (!candidate || candidate->opcode != GPU_IR_SHADER_OP_VARIABLE ||
                candidate->function_index != UINT32_MAX ||
                (candidate->storage_class != GPU_IR_SHADER_STORAGE_BUFFER &&
                 candidate->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM)) continue;
            if (candidate->result_id == variable->result_id) {
                if (descriptor_count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS - next_slot)
                    return GPU_IR_UNSUPPORTED;
                *slot = next_slot;
                return GPU_IR_OK;
            }
            uint32_t candidate_count = 1;
            int shape_result = candidate->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM ?
                global_uniform_buffer_shape(shader, candidate) :
                global_resource_shape(shader, candidate, &candidate_count);
            if (shape_result != GPU_IR_OK || candidate_count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS - next_slot)
                return shape_result == GPU_IR_OK ? GPU_IR_UNSUPPORTED : shape_result;
            next_slot += candidate_count;
        }
        if (push_constant) {
            if (next_slot >= GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS) return GPU_IR_UNSUPPORTED;
            *slot = next_slot;
            return GPU_IR_OK;
        }
        return GPU_IR_NOT_FOUND;
    }
    uint32_t next_slot = 0, selected_slot = 0;
    int found = 0;
    for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(layout); i++) {
        gpu_ir_descriptor_binding entry;
        if (gpu_ir_pipeline_layout_binding_at(layout, i, &entry) != GPU_IR_OK) return GPU_IR_INVALID;
        if (!(entry.stage_mask & stage_mask)) continue;
        if (!layout_binding_supported(&entry)) return GPU_IR_UNSUPPORTED;
        int internal_push_binding = entry.set == GPU_IR_INTERNAL_SET &&
            entry.binding_namespace == GPU_IR_INTERNAL_BINDING_NAMESPACE &&
            entry.binding == GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING;
        if (internal_push_binding) {
            if (entry.kind != GPU_IR_DESCRIPTOR_UNIFORM_BUFFER || entry.count != 1)
                return GPU_IR_UNSUPPORTED;
            if (push_constant) {
                selected_slot = GPU_IR_METAL_BUFFER_RANGE_SLOT;
                found = 1;
            }
            continue;
        }
        if (entry.kind == GPU_IR_DESCRIPTOR_STORAGE_IMAGE ||
            entry.kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE ||
            entry.kind == GPU_IR_DESCRIPTOR_SAMPLER ||
            entry.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER) continue;
        if (layout_binding_is_image_descriptor(shader, &entry)) continue;
        int selected = push_constant ?
            0 : entry.set == variable->resource_set && entry.binding_namespace == variable->binding_namespace &&
            entry.binding == variable->binding;
        if (selected) {
            if ((variable->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM &&
                 entry.kind != GPU_IR_DESCRIPTOR_UNIFORM_BUFFER) ||
                (push_constant && entry.kind != GPU_IR_DESCRIPTOR_UNIFORM_BUFFER) ||
                entry.count < descriptor_count)
                return GPU_IR_UNSUPPORTED;
            selected_slot = next_slot;
            found = 1;
        }
        if (entry.count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS - next_slot) return GPU_IR_UNSUPPORTED;
        next_slot += entry.count;
    }
    if (!found && push_constant) {
        if (next_slot >= GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS) return GPU_IR_UNSUPPORTED;
        selected_slot = next_slot;
        found = 1;
    }
    if (!found) return GPU_IR_NOT_FOUND;
    *slot = selected_slot;
    return GPU_IR_OK;
}

static int descriptor_texture_slot(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                                   const gpu_ir_shader_instruction *variable, uint32_t stage_mask,
                                   uint32_t *slot) {
    if (!shader || !variable || !slot) return GPU_IR_INVALID;
    uint32_t descriptor_count = 0;
    if (global_storage_image_shape(shader, variable, NULL, NULL, NULL) == GPU_IR_OK ||
        global_sampled_image_shape(shader, variable, NULL) == GPU_IR_OK ||
        global_combined_image_shape(shader, variable, NULL) == GPU_IR_OK) {
        if (uniform_constant_descriptor_type(shader, variable, NULL, &descriptor_count) != GPU_IR_OK)
            return GPU_IR_UNSUPPORTED;
    } else return GPU_IR_UNSUPPORTED;
    if (!layout) {
        uint32_t next_slot = 0;
        for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
            const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(shader, i);
            if (!candidate || candidate->opcode != GPU_IR_SHADER_OP_VARIABLE ||
                candidate->function_index != UINT32_MAX ||
                candidate->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) continue;
            if (candidate->result_id == variable->result_id) {
                if (descriptor_count > GPU_IR_METAL_MAX_COMPUTE_TEXTURES - next_slot)
                    return GPU_IR_UNSUPPORTED;
                *slot = next_slot;
                return GPU_IR_OK;
            }
            uint32_t candidate_count = 0;
            if ((global_storage_image_shape(shader, candidate, NULL, NULL, NULL) == GPU_IR_OK ||
                 global_sampled_image_shape(shader, candidate, NULL) == GPU_IR_OK ||
                 global_combined_image_shape(shader, candidate, NULL) == GPU_IR_OK) &&
                uniform_constant_descriptor_type(shader, candidate, NULL, &candidate_count) == GPU_IR_OK) {
                if (candidate_count > GPU_IR_METAL_MAX_COMPUTE_TEXTURES - next_slot)
                    return GPU_IR_UNSUPPORTED;
                next_slot += candidate_count;
            }
        }
        return GPU_IR_NOT_FOUND;
    }
    uint32_t next_slot = 0;
    for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(layout); i++) {
        gpu_ir_descriptor_binding entry;
        if (gpu_ir_pipeline_layout_binding_at(layout, i, &entry) != GPU_IR_OK) return GPU_IR_INVALID;
        if (!(entry.stage_mask & stage_mask) ||
            (entry.kind != GPU_IR_DESCRIPTOR_STORAGE_IMAGE && entry.kind != GPU_IR_DESCRIPTOR_SAMPLED_IMAGE &&
             entry.kind != GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER &&
             entry.kind != GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE))
            continue;
        if (!layout_binding_supported(&entry) || entry.binding_namespace != 0 ||
            entry.count > GPU_IR_METAL_MAX_COMPUTE_TEXTURES - next_slot) return GPU_IR_UNSUPPORTED;
        if (entry.set == variable->resource_set && entry.binding_namespace == variable->binding_namespace &&
            entry.binding == variable->binding) {
            uint32_t expected_kind = global_combined_image_shape(shader, variable, NULL) == GPU_IR_OK ?
                GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER :
                global_storage_image_shape(shader, variable, NULL, NULL, NULL) == GPU_IR_OK ?
                    GPU_IR_DESCRIPTOR_STORAGE_IMAGE : GPU_IR_DESCRIPTOR_SAMPLED_IMAGE;
            int generic_sampled_srv = entry.kind == GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE &&
                global_sampled_image_shape(shader, variable, NULL) == GPU_IR_OK;
            if ((!generic_sampled_srv && entry.kind != expected_kind) || entry.count != descriptor_count)
                return GPU_IR_UNSUPPORTED;
            *slot = next_slot;
            return GPU_IR_OK;
        }
        next_slot += entry.count;
    }
    return GPU_IR_NOT_FOUND;
}

static int descriptor_sampler_slot(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                                   const gpu_ir_shader_instruction *variable, uint32_t stage_mask,
                                   uint32_t *slot) {
    if (!shader || !variable || !slot) return GPU_IR_INVALID;
    uint32_t descriptor_count = 0;
    if (global_sampler_shape(shader, variable) == GPU_IR_OK ||
        global_combined_image_shape(shader, variable, NULL) == GPU_IR_OK) {
        if (uniform_constant_descriptor_type(shader, variable, NULL, &descriptor_count) != GPU_IR_OK)
            return GPU_IR_UNSUPPORTED;
    } else return GPU_IR_UNSUPPORTED;
    if (!layout) {
        uint32_t next_slot = 0;
        for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
            const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(shader, i);
            if (!candidate || candidate->opcode != GPU_IR_SHADER_OP_VARIABLE ||
                candidate->function_index != UINT32_MAX ||
                candidate->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) continue;
            if (candidate->result_id == variable->result_id) {
                if (descriptor_count > GPU_IR_METAL_MAX_COMPUTE_SAMPLERS - next_slot)
                    return GPU_IR_UNSUPPORTED;
                *slot = next_slot;
                return GPU_IR_OK;
            }
            uint32_t candidate_count = 0;
            if ((global_sampler_shape(shader, candidate) == GPU_IR_OK ||
                 global_combined_image_shape(shader, candidate, NULL) == GPU_IR_OK) &&
                uniform_constant_descriptor_type(shader, candidate, NULL, &candidate_count) == GPU_IR_OK) {
                if (candidate_count > GPU_IR_METAL_MAX_COMPUTE_SAMPLERS - next_slot)
                    return GPU_IR_UNSUPPORTED;
                next_slot += candidate_count;
            }
        }
        return GPU_IR_NOT_FOUND;
    }
    uint32_t next_slot = 0;
    for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(layout); i++) {
        gpu_ir_descriptor_binding entry;
        if (gpu_ir_pipeline_layout_binding_at(layout, i, &entry) != GPU_IR_OK) return GPU_IR_INVALID;
        if (!(entry.stage_mask & stage_mask) ||
            (entry.kind != GPU_IR_DESCRIPTOR_SAMPLER &&
             entry.kind != GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER)) continue;
        if (!layout_binding_supported(&entry) || entry.binding_namespace != 0 ||
            entry.count > GPU_IR_METAL_MAX_COMPUTE_SAMPLERS - next_slot) return GPU_IR_UNSUPPORTED;
        if (entry.set == variable->resource_set && entry.binding_namespace == variable->binding_namespace &&
            entry.binding == variable->binding) {
            uint32_t expected_kind = global_combined_image_shape(shader, variable, NULL) == GPU_IR_OK ?
                GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER : GPU_IR_DESCRIPTOR_SAMPLER;
            if (entry.kind != expected_kind || entry.count != descriptor_count) return GPU_IR_UNSUPPORTED;
            *slot = next_slot;
            return GPU_IR_OK;
        }
        next_slot += entry.count;
    }
    return GPU_IR_NOT_FOUND;
}

static int validate_compute_layout(const gpu_ir_pipeline_layout *layout) {
    if (!layout) return GPU_IR_OK;
    uint32_t slot_count = 0, texture_slot_count = 0, sampler_slot_count = 0;
    for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(layout); i++) {
        gpu_ir_descriptor_binding entry;
        if (gpu_ir_pipeline_layout_binding_at(layout, i, &entry) != GPU_IR_OK) return GPU_IR_INVALID;
        if (!(entry.stage_mask & GPU_IR_STAGE_COMPUTE)) continue;
        if (!layout_binding_supported(&entry)) return GPU_IR_UNSUPPORTED;
        if (entry.set == GPU_IR_INTERNAL_SET &&
            entry.binding_namespace == GPU_IR_INTERNAL_BINDING_NAMESPACE &&
            entry.binding == GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING) {
            if (entry.kind != GPU_IR_DESCRIPTOR_UNIFORM_BUFFER || entry.count != 1) return GPU_IR_UNSUPPORTED;
            continue;
        }
        if (entry.kind == GPU_IR_DESCRIPTOR_STORAGE_IMAGE || entry.kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE ||
            entry.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER) {
            if (entry.count > GPU_IR_METAL_MAX_COMPUTE_TEXTURES - texture_slot_count)
                return GPU_IR_UNSUPPORTED;
            texture_slot_count += entry.count;
        }
        if (entry.kind == GPU_IR_DESCRIPTOR_SAMPLER ||
            entry.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER) {
            if (entry.count > GPU_IR_METAL_MAX_COMPUTE_SAMPLERS - sampler_slot_count)
                return GPU_IR_UNSUPPORTED;
            sampler_slot_count += entry.count;
        }
        if (entry.kind == GPU_IR_DESCRIPTOR_STORAGE_IMAGE || entry.kind == GPU_IR_DESCRIPTOR_SAMPLED_IMAGE ||
            entry.kind == GPU_IR_DESCRIPTOR_COMBINED_IMAGE_SAMPLER || entry.kind == GPU_IR_DESCRIPTOR_SAMPLER)
            continue;
        if (entry.count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS - slot_count) return GPU_IR_UNSUPPORTED;
        slot_count += entry.count;
    }
    return GPU_IR_OK;
}

static int compute_builtin_parameter(const gpu_ir_shader *shader,
                                    const gpu_ir_shader_instruction *variable,
                                    const char **type_name, const char **attribute) {
    if (variable->storage_class != GPU_IR_SHADER_STORAGE_INPUT ||
        !variable->has_builtin || variable->operand_count)
        return GPU_IR_UNSUPPORTED;
    static const struct { uint32_t builtin; const char *attribute; } builtins[] = {
        { GPU_IR_SHADER_BUILTIN_NUM_WORKGROUPS, "threadgroups_per_grid" },
        { GPU_IR_SHADER_BUILTIN_WORKGROUP_SIZE, "threads_per_threadgroup" },
        { GPU_IR_SHADER_BUILTIN_WORKGROUP_ID, "threadgroup_position_in_grid" },
        { GPU_IR_SHADER_BUILTIN_LOCAL_INVOCATION_ID, "thread_position_in_threadgroup" },
        { GPU_IR_SHADER_BUILTIN_GLOBAL_INVOCATION_ID, "thread_position_in_grid" },
        { GPU_IR_SHADER_BUILTIN_LOCAL_INVOCATION_INDEX, "thread_index_in_threadgroup" },
        { GPU_IR_SHADER_BUILTIN_SUBGROUP_SIZE, "threads_per_simdgroup" },
        { GPU_IR_SHADER_BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID, "thread_index_in_simdgroup" }
    };
    const char *selected_attribute = NULL;
    for (uint32_t i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
        if (builtins[i].builtin == variable->builtin) { selected_attribute = builtins[i].attribute; break; }
    if (!selected_attribute) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *pointer = shader_type(shader, variable->result_type_id);
    const gpu_ir_shader_type *value = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
        shader_type(shader, pointer->element_type_id) : NULL;
    int vector = variable->builtin == GPU_IR_SHADER_BUILTIN_NUM_WORKGROUPS ||
        variable->builtin == GPU_IR_SHADER_BUILTIN_WORKGROUP_SIZE ||
        variable->builtin == GPU_IR_SHADER_BUILTIN_WORKGROUP_ID ||
        variable->builtin == GPU_IR_SHADER_BUILTIN_LOCAL_INVOCATION_ID ||
        variable->builtin == GPU_IR_SHADER_BUILTIN_GLOBAL_INVOCATION_ID;
    if (vector) {
        if (!value || value->kind != GPU_IR_SHADER_TYPE_VECTOR || value->element_count != 3 ||
            !shader_type_is_u32(shader, value->element_type_id)) return GPU_IR_UNSUPPORTED;
        *type_name = "uint3";
    } else {
        if (!shader_type_is_u32(shader, pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
                                pointer->element_type_id : 0)) return GPU_IR_UNSUPPORTED;
        *type_name = "uint";
    }
    *attribute = selected_attribute;
    return GPU_IR_OK;
}

static int add_global_parameters(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                                 uint32_t stage_mask,
                                 msl_symbols *symbols, msl_builder *parameters, msl_builder *body) {
    uint32_t parameter_count = 0;
    int has_buffer_ranges = 0;
    int combined_runtime_data = 0;
    int uses_push_constants = 0;
    if (layout) {
        for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(layout); i++) {
            gpu_ir_descriptor_binding binding;
            if (gpu_ir_pipeline_layout_binding_at(layout, i, &binding) != GPU_IR_OK) return GPU_IR_INVALID;
            if (binding.set == GPU_IR_INTERNAL_SET &&
                binding.binding_namespace == GPU_IR_INTERNAL_BINDING_NAMESPACE &&
                binding.binding == GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING) {
                if (binding.kind != GPU_IR_DESCRIPTOR_UNIFORM_BUFFER || binding.count != 1 ||
                    !(binding.stage_mask & stage_mask)) return GPU_IR_UNSUPPORTED;
                if (stage_mask == GPU_IR_STAGE_COMPUTE) {
                    combined_runtime_data = 1;
                    break;
                }
            }
        }
    }
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
        if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE || variable->function_index != UINT32_MAX)
            continue;
        int rc;
        char name[64];
        if (variable->storage_class == GPU_IR_SHADER_STORAGE_BUFFER) {
            has_buffer_ranges = 1;
            uint32_t descriptor_count = 1;
            rc = global_resource_shape(shader, variable, &descriptor_count);
            if (rc != GPU_IR_OK) return rc;
            const gpu_ir_shader_type *resource_pointer = shader_type(shader, variable->result_type_id);
            const gpu_ir_shader_type *resource_array = resource_pointer &&
                resource_pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
                shader_type(shader, resource_pointer->element_type_id) : NULL;
            int is_descriptor_array = resource_array && resource_array->kind == GPU_IR_SHADER_TYPE_ARRAY;
            uint32_t slot = 0;
            rc = descriptor_slot(shader, layout, variable, descriptor_count, stage_mask, &slot);
            if (rc != GPU_IR_OK) return rc;
            for (uint32_t j = 0; j < i; j++) {
                const gpu_ir_shader_instruction *prior = gpu_ir_shader_instruction_at(shader, j);
                if (prior && prior->opcode == GPU_IR_SHADER_OP_VARIABLE && prior->function_index == UINT32_MAX &&
                    prior->storage_class == GPU_IR_SHADER_STORAGE_BUFFER &&
                    prior->has_descriptor_set && prior->has_binding &&
                    prior->resource_set == variable->resource_set &&
                    prior->binding_namespace == variable->binding_namespace &&
                    prior->binding == variable->binding)
                    return GPU_IR_INVALID;
            }
            const gpu_ir_shader_type *element_type = global_resource_element_type(shader, variable);
            char element_name[32];
            if (!element_type || !shader_type_name(shader, element_type->id, element_name, sizeof element_name))
                return GPU_IR_UNSUPPORTED;
            const char *qualifier = variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE ?
                "device const" : "device";
            char parameter_type[64];
            if (snprintf(parameter_type, sizeof parameter_type, "%s %s*", qualifier, element_name) >=
                (int)sizeof parameter_type) return GPU_IR_RANGE;
            for (uint32_t element = 0; element < descriptor_count; element++) {
                if (!is_descriptor_array)
                    snprintf(name, sizeof name, "vx_buffer_%u", variable->result_id);
                else
                    snprintf(name, sizeof name, "vx_buffer_%u_%u", variable->result_id, element);
                if (parameter_count++ && builder_appendf(parameters, ", ") != GPU_IR_OK)
                    return GPU_IR_OUT_OF_MEMORY;
                if (builder_appendf(parameters, "%s %s [[buffer(%u)]]", parameter_type, name, slot + element) !=
                    GPU_IR_OK) return GPU_IR_OUT_OF_MEMORY;
            }
            if (!is_descriptor_array) snprintf(name, sizeof name, "vx_buffer_%u", variable->result_id);
            else snprintf(name, sizeof name, "vx_descriptor_array_%u", variable->result_id);
            rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, name);
            if (rc == GPU_IR_OK)
                rc = symbols_set_buffer_access(symbols, variable->result_id, slot, NULL,
                    !(variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE));
            if (rc == GPU_IR_OK && is_descriptor_array)
                rc = symbols_set_descriptor_array_count(symbols, variable->result_id, descriptor_count);
        } else if (variable->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM) {
            has_buffer_ranges = 1;
            rc = global_uniform_buffer_shape(shader, variable);
            uint32_t slot = 0;
            if (rc == GPU_IR_OK) rc = descriptor_slot(shader, layout, variable, 1, stage_mask, &slot);
            if (rc != GPU_IR_OK) return rc;
            for (uint32_t j = 0; j < i; j++) {
                const gpu_ir_shader_instruction *prior = gpu_ir_shader_instruction_at(shader, j);
                if (prior && prior->opcode == GPU_IR_SHADER_OP_VARIABLE && prior->function_index == UINT32_MAX &&
                    prior->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM &&
                    prior->has_descriptor_set && prior->has_binding &&
                    prior->resource_set == variable->resource_set && prior->binding == variable->binding)
                    return GPU_IR_INVALID;
            }
            snprintf(name, sizeof name, "vx_uniform_%u", variable->result_id);
            if (parameter_count++ && builder_appendf(parameters, ", ") != GPU_IR_OK)
                return GPU_IR_OUT_OF_MEMORY;
            rc = builder_appendf(parameters, "constant uint* %s [[buffer(%u)]]", name, slot);
            if (rc == GPU_IR_OK)
                rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, name);
            if (rc == GPU_IR_OK)
                rc = symbols_set_buffer_access(symbols, variable->result_id, slot, "0", 0);
        } else if (variable->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) {
            uint32_t slot = 0, sampler_slot = 0;
            uint32_t descriptor_count = 0;
            int readable = 0, writable = 0;
            const gpu_ir_shader_type *sampled_image_type = NULL;
            int sampled_image = global_sampled_image_shape(shader, variable, &sampled_image_type) == GPU_IR_OK;
            char parameter_type[96];
            if (global_sampler_shape(shader, variable) == GPU_IR_OK) {
                rc = descriptor_sampler_slot(shader, layout, variable, stage_mask, &sampler_slot);
                if (rc != GPU_IR_OK) return rc;
                rc = uniform_constant_descriptor_type(shader, variable, NULL, &descriptor_count);
                if (rc != GPU_IR_OK) return rc;
                for (uint32_t element = 0; element < descriptor_count; element++) {
                    if (descriptor_count == 1)
                        snprintf(name, sizeof name, "vx_sampler_%u", variable->result_id);
                    else
                        snprintf(name, sizeof name, "vx_sampler_%u_%u", variable->result_id, element);
                    if (parameter_count++ && builder_appendf(parameters, ", ") != GPU_IR_OK)
                        return GPU_IR_OUT_OF_MEMORY;
                    rc = builder_appendf(parameters, "sampler %s [[sampler(%u)]]", name,
                                         sampler_slot + element);
                    if (rc != GPU_IR_OK) return rc;
                }
                snprintf(name, sizeof name, descriptor_count == 1 ? "vx_sampler_%u" :
                         "vx_descriptor_array_%u", variable->result_id);
                if (rc == GPU_IR_OK)
                    rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, name);
                if (rc == GPU_IR_OK && descriptor_count > 1)
                    rc = symbols_set_descriptor_array_count(symbols, variable->result_id, descriptor_count);
                if (rc != GPU_IR_OK) return rc;
                continue;
            }
            if (global_combined_image_shape(shader, variable, NULL) == GPU_IR_OK) {
                rc = descriptor_texture_slot(shader, layout, variable, stage_mask, &slot);
                if (rc == GPU_IR_OK) rc = descriptor_sampler_slot(shader, layout, variable, stage_mask,
                                                                   &sampler_slot);
                if (rc != GPU_IR_OK) return rc;
                rc = uniform_constant_descriptor_type(shader, variable, NULL, &descriptor_count);
                if (rc != GPU_IR_OK) return rc;
                for (uint32_t element = 0; element < descriptor_count; element++) {
                    if (descriptor_count == 1)
                        snprintf(name, sizeof name, "vx_image_%u", variable->result_id);
                    else
                        snprintf(name, sizeof name, "vx_image_%u_%u", variable->result_id, element);
                    if (parameter_count++ && builder_appendf(parameters, ", ") != GPU_IR_OK)
                        return GPU_IR_OUT_OF_MEMORY;
                    rc = builder_appendf(parameters,
                        "texture2d<float, access::sample> %s [[texture(%u)]]", name, slot + element);
                    if (rc == GPU_IR_OK && parameter_count++ &&
                        builder_appendf(parameters, ", ") != GPU_IR_OK) return GPU_IR_OUT_OF_MEMORY;
                    if (rc == GPU_IR_OK) rc = descriptor_count == 1 ? builder_appendf(parameters,
                        "sampler vx_sampler_%u [[sampler(%u)]]", variable->result_id, sampler_slot) :
                        builder_appendf(parameters, "sampler vx_sampler_%u_%u [[sampler(%u)]]",
                            variable->result_id, element, sampler_slot + element);
                    if (rc != GPU_IR_OK) return rc;
                }
                snprintf(name, sizeof name, descriptor_count == 1 ? "vx_image_%u" :
                         "vx_descriptor_array_%u", variable->result_id);
                if (rc == GPU_IR_OK)
                    rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, name);
                if (rc == GPU_IR_OK && descriptor_count == 1) {
                    char sampler_name[48];
                    snprintf(sampler_name, sizeof sampler_name, "vx_sampler_%u", variable->result_id);
                    rc = symbols_set_aux_expression(symbols, variable->result_id, sampler_name);
                }
                if (rc == GPU_IR_OK)
                    rc = symbols_set_image_access(symbols, variable->result_id, 1, 0);
                if (rc == GPU_IR_OK && descriptor_count > 1)
                    rc = symbols_set_descriptor_array_count(symbols, variable->result_id, descriptor_count);
                if (rc != GPU_IR_OK) return rc;
                continue;
            }
            rc = storage_image_parameter_type(shader, variable, parameter_type, sizeof parameter_type,
                                               &readable, &writable);
            if (rc != GPU_IR_OK && sampled_image) {
                const char *element = sampled_image_component_name(shader, sampled_image_type);
                int sampled = global_sampled_image_used_for_sampling(shader, variable);
                const gpu_ir_shader_type *component = shader_type(shader, sampled_image_type->element_type_id);
                if (!element || (sampled && component->kind != GPU_IR_SHADER_TYPE_FLOAT))
                    return GPU_IR_UNSUPPORTED;
                const char *texture_type = sampled_image_type && sampled_image_type->image_multisampled ?
                    sampled_image_type->image_arrayed ? "texture2d_ms_array" : "texture2d_ms" :
                    sampled_image_type && sampled_image_type->image_arrayed ? "texture2d_array" : "texture2d";
                const char *access = sampled ? "sample" : "read";
                if (snprintf(parameter_type, sizeof parameter_type, "%s<%s, access::%s>",
                             texture_type, element, access) >= (int)sizeof parameter_type)
                    return GPU_IR_RANGE;
                readable = 1;
                writable = 0;
                rc = GPU_IR_OK;
            }
            if (rc == GPU_IR_OK) rc = descriptor_texture_slot(shader, layout, variable, stage_mask, &slot);
            if (rc != GPU_IR_OK) return rc;
            rc = uniform_constant_descriptor_type(shader, variable, NULL, &descriptor_count);
            if (rc != GPU_IR_OK) return rc;
            for (uint32_t j = 0; j < i; j++) {
                const gpu_ir_shader_instruction *prior = gpu_ir_shader_instruction_at(shader, j);
                if (prior && prior->opcode == GPU_IR_SHADER_OP_VARIABLE && prior->function_index == UINT32_MAX &&
                    prior->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT &&
                    prior->has_descriptor_set && prior->has_binding &&
                    prior->resource_set == variable->resource_set && prior->binding_namespace == variable->binding_namespace &&
                    prior->binding == variable->binding)
                    return GPU_IR_INVALID;
            }
            for (uint32_t element = 0; element < descriptor_count; element++) {
                if (descriptor_count == 1)
                    snprintf(name, sizeof name, "vx_image_%u", variable->result_id);
                else
                    snprintf(name, sizeof name, "vx_image_%u_%u", variable->result_id, element);
                if (parameter_count++ && builder_appendf(parameters, ", ") != GPU_IR_OK)
                    return GPU_IR_OUT_OF_MEMORY;
                rc = builder_appendf(parameters, "%s %s [[texture(%u)]]", parameter_type, name,
                                     slot + element);
                if (rc != GPU_IR_OK) return rc;
            }
            snprintf(name, sizeof name, descriptor_count == 1 ? "vx_image_%u" :
                     "vx_descriptor_array_%u", variable->result_id);
            if (rc == GPU_IR_OK)
                rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, name);
            if (rc == GPU_IR_OK)
                rc = symbols_set_image_access(symbols, variable->result_id, readable, writable);
            if (rc == GPU_IR_OK && descriptor_count > 1)
                rc = symbols_set_descriptor_array_count(symbols, variable->result_id, descriptor_count);
        } else if (variable->storage_class == GPU_IR_SHADER_STORAGE_PUSH_CONSTANT) {
            const gpu_ir_shader_type *pointer = shader_type(shader, variable->result_type_id);
            const gpu_ir_shader_type *block = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
                shader_type(shader, pointer->element_type_id) : NULL;
            if (!pointer || pointer->storage_class != GPU_IR_SHADER_STORAGE_PUSH_CONSTANT || !block ||
                block->kind != GPU_IR_SHADER_TYPE_STRUCT ||
                !(block->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_BLOCK) || variable->operand_count)
                return GPU_IR_UNSUPPORTED;
            uses_push_constants = 1;
            if (combined_runtime_data) {
                snprintf(name, sizeof name, "reinterpret_cast<constant uint*>(vx_runtime_data)");
                rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, name);
            } else {
                uint32_t slot = 0;
                rc = descriptor_slot(shader, layout, variable, 1, stage_mask, &slot);
                if (rc != GPU_IR_OK) return rc;
                snprintf(name, sizeof name, "vx_push_constants_%u", variable->result_id);
                if (parameter_count++ && builder_appendf(parameters, ", ") != GPU_IR_OK)
                    return GPU_IR_OUT_OF_MEMORY;
                rc = builder_appendf(parameters, "constant uint* %s [[buffer(%u)]]", name, slot);
                if (rc == GPU_IR_OK)
                    rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, name);
            }
        } else if (variable->storage_class == GPU_IR_SHADER_STORAGE_WORKGROUP) {
            const gpu_ir_shader_type *pointer = shader_type(shader, variable->result_type_id);
            const gpu_ir_shader_type *element = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
                shader_type(shader, pointer->element_type_id) : NULL;
            char type_name[32];
            char shared_name[48];
            uint32_t array_length = 0;
            int is_array = element && element->kind == GPU_IR_SHADER_TYPE_ARRAY;
            if (!variable->result_id || variable->operand_count || !pointer ||
                pointer->kind != GPU_IR_SHADER_TYPE_POINTER ||
                pointer->storage_class != GPU_IR_SHADER_STORAGE_WORKGROUP || !element)
                return GPU_IR_UNSUPPORTED;
            if (is_array) {
                if (!constant_u32(shader, element->array_length_id, &array_length) || !array_length ||
                    array_length > 65536 ||
                    !shader_type_name(shader, element->element_type_id, type_name, sizeof type_name))
                    return GPU_IR_UNSUPPORTED;
            } else if (!shader_type_name(shader, pointer->element_type_id, type_name, sizeof type_name)) {
                return GPU_IR_UNSUPPORTED;
            }
            snprintf(shared_name, sizeof shared_name, "vx_workgroup_%u", variable->result_id);
            rc = is_array ? builder_appendf(body, "    threadgroup array<%s, %u> %s;\n",
                                            type_name, array_length, shared_name) :
                            builder_appendf(body, "    threadgroup %s %s;\n", type_name, shared_name);
            if (rc == GPU_IR_OK) {
                rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, shared_name);
                if (rc == GPU_IR_OK) {
                    char capture_argument[64];
                    if (snprintf(capture_argument, sizeof capture_argument, "&%s", shared_name) >=
                        (int)sizeof capture_argument) return GPU_IR_RANGE;
                    rc = symbols_set_capture_argument(symbols, variable->result_id, capture_argument);
                }
            }
        } else if (variable->storage_class == GPU_IR_SHADER_STORAGE_INPUT &&
                   stage_mask == GPU_IR_STAGE_COMPUTE) {
            const char *builtin_type = NULL, *builtin_attribute = NULL;
            rc = compute_builtin_parameter(shader, variable, &builtin_type, &builtin_attribute);
            if (rc != GPU_IR_OK) return rc;
            for (uint32_t j = 0; j < i; j++) {
                const gpu_ir_shader_instruction *prior = gpu_ir_shader_instruction_at(shader, j);
                if (prior && prior->opcode == GPU_IR_SHADER_OP_VARIABLE && prior->function_index == UINT32_MAX &&
                    prior->storage_class == GPU_IR_SHADER_STORAGE_INPUT && prior->has_builtin &&
                    prior->builtin == variable->builtin)
                    return GPU_IR_INVALID;
            }
            snprintf(name, sizeof name, "vx_builtin_%u", variable->result_id);
            if (parameter_count++ && builder_appendf(parameters, ", ") != GPU_IR_OK) return GPU_IR_OUT_OF_MEMORY;
            if (builder_appendf(parameters, "%s %s [[%s]]", builtin_type, name, builtin_attribute) != GPU_IR_OK)
                return GPU_IR_OUT_OF_MEMORY;
            rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, name);
        } else if ((variable->storage_class == GPU_IR_SHADER_STORAGE_INPUT ||
                    variable->storage_class == GPU_IR_SHADER_STORAGE_OUTPUT) &&
                   stage_mask != GPU_IR_STAGE_COMPUTE) {
            // Graphics I/O symbols and arguments are emitted by emit_graphics_io.
            continue;
        } else return GPU_IR_UNSUPPORTED;
        if (rc != GPU_IR_OK) return rc;
    }
    if (layout) {
        for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(layout); i++) {
            gpu_ir_descriptor_binding binding;
            if (gpu_ir_pipeline_layout_binding_at(layout, i, &binding) != GPU_IR_OK) return GPU_IR_INVALID;
            if ((binding.stage_mask & stage_mask) &&
                !(binding.set == GPU_IR_INTERNAL_SET &&
                  binding.binding_namespace == GPU_IR_INTERNAL_BINDING_NAMESPACE &&
                  binding.binding == GPU_IR_INTERNAL_PUSH_CONSTANT_BINDING) &&
                (binding.kind == GPU_IR_DESCRIPTOR_STORAGE_BUFFER ||
                 binding.kind == GPU_IR_DESCRIPTOR_UNIFORM_BUFFER)) {
                has_buffer_ranges = 1;
                break;
            }
        }
    }
    if (combined_runtime_data && (uses_push_constants || has_buffer_ranges)) {
        if (parameter_count++ && builder_appendf(parameters, ", ") != GPU_IR_OK) return GPU_IR_OUT_OF_MEMORY;
        if (builder_appendf(parameters, "constant uchar* vx_runtime_data [[buffer(%u)]]",
                            GPU_IR_METAL_BUFFER_RANGE_SLOT) != GPU_IR_OK) return GPU_IR_OUT_OF_MEMORY;
        if (has_buffer_ranges && builder_appendf(body,
            "    constant ulong* vx_buffer_ranges = reinterpret_cast<constant ulong*>(vx_runtime_data + %u);\n",
            GPU_IR_METAL_MAX_PUSH_CONSTANT_SIZE) != GPU_IR_OK) return GPU_IR_OUT_OF_MEMORY;
    } else if (has_buffer_ranges) {
        if (parameter_count++ && builder_appendf(parameters, ", ") != GPU_IR_OK) return GPU_IR_OUT_OF_MEMORY;
        if (builder_appendf(parameters, "constant ulong* vx_buffer_ranges [[buffer(%u)]]",
                            GPU_IR_METAL_BUFFER_RANGE_SLOT) != GPU_IR_OK) return GPU_IR_OUT_OF_MEMORY;
    }
    return GPU_IR_OK;
}

static int add_global_constants(const gpu_ir_shader *shader, msl_symbols *symbols) {
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->function_index != UINT32_MAX || instruction->opcode != GPU_IR_SHADER_OP_CONSTANT)
            continue;
        char expression[256];
        int rc = constant_expression(shader, instruction, symbols, expression, sizeof expression);
        if (rc != GPU_IR_OK) return rc;
        rc = symbols_add(symbols, instruction->result_id, instruction->result_type_id, 0, expression);
        if (rc != GPU_IR_OK) return rc;
    }
    return GPU_IR_OK;
}

static int emit_function_variable(const gpu_ir_shader *shader,
                                  const gpu_ir_shader_instruction *instruction,
                                  const msl_symbols *symbols, msl_builder *body,
                                  msl_symbols *mutable_symbols) {
    const gpu_ir_shader_type *pointer = shader_type(shader, instruction->result_type_id);
    if (!instruction->result_id || instruction->function_index == UINT32_MAX ||
        instruction->operand_count > 1 || !pointer || pointer->kind != GPU_IR_SHADER_TYPE_POINTER ||
        pointer->storage_class != GPU_IR_SHADER_STORAGE_FUNCTION) return GPU_IR_UNSUPPORTED;

    char type_name[32];
    const gpu_ir_shader_type *element = shader_type(shader, pointer->element_type_id);
    uint32_t array_length = 0;
    int is_array = element && element->kind == GPU_IR_SHADER_TYPE_ARRAY;
    if (is_array) {
        if (instruction->operand_count || !constant_u32(shader, element->array_length_id, &array_length) ||
            !array_length || array_length > 4096 ||
            !shader_type_name(shader, element->element_type_id, type_name, sizeof type_name))
            return GPU_IR_UNSUPPORTED;
    } else if (!shader_type_name(shader, pointer->element_type_id, type_name, sizeof type_name)) {
        return GPU_IR_UNSUPPORTED;
    }
    char name[48];
    snprintf(name, sizeof name, "vx_local_%u", instruction->result_id);
    int rc;
    if (is_array) {
        rc = builder_appendf(body, "    array<%s, %u> %s;\n", type_name, array_length, name);
    } else if (instruction->operand_count) {
        uint32_t initializer_id;
        if (!instruction_id_operand(shader, instruction, 0, &initializer_id)) return GPU_IR_INVALID;
        const msl_symbol *initializer = symbols_find(symbols, initializer_id);
        if (!initializer || initializer->is_pointer || initializer->type_id != pointer->element_type_id)
            return GPU_IR_UNSUPPORTED;
        rc = builder_appendf(body, "    %s %s = %s;\n", type_name, name, initializer->expression);
    } else {
        rc = builder_appendf(body, "    %s %s;\n", type_name, name);
    }
    if (rc == GPU_IR_OK)
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 1, name);
    return rc;
}

static int emit_access_chain(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                             const msl_symbols *symbols, msl_symbols *mutable_symbols) {
    uint32_t base_id;
    if (instruction->operand_count < 2 || !instruction_id_operand(shader, instruction, 0, &base_id))
        return GPU_IR_INVALID;
    const msl_symbol *base = symbols_find(symbols, base_id);
    const gpu_ir_shader_type *pointer = base ? shader_type(shader, base->type_id) : NULL;
    if (!base || !base->is_pointer || !pointer || pointer->kind != GPU_IR_SHADER_TYPE_POINTER ||
        (pointer->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT &&
        (pointer->storage_class != GPU_IR_SHADER_STORAGE_BUFFER &&
         pointer->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM &&
         pointer->storage_class != GPU_IR_SHADER_STORAGE_PUSH_CONSTANT &&
         pointer->storage_class != GPU_IR_SHADER_STORAGE_FUNCTION &&
         pointer->storage_class != GPU_IR_SHADER_STORAGE_WORKGROUP))) return GPU_IR_UNSUPPORTED;
    // symbols_add may reallocate the symbol array. Keep a value snapshot so the base
    // expression and access state remain valid after adding the derived pointer.
    const msl_symbol base_snapshot = *base;
    base = &base_snapshot;
    uint32_t current_type_id = pointer->element_type_id;
    uint32_t descriptor_slot = base->descriptor_slot;
    uint32_t descriptor_array_element = UINT32_MAX;
    uint32_t buffer_descriptor_array_count = base->descriptor_array_count;
    uint32_t push_constant_offset = 0;
    uint32_t uniform_buffer_offset = 0;
    uint32_t push_matrix_stride = 0;
    char *buffer_index_expression = base->buffer_index_expression ?
        msl_string_copy(base->buffer_index_expression) : NULL;
    if (base->buffer_index_expression && !buffer_index_expression) return GPU_IR_OUT_OF_MEMORY;
    char *descriptor_index_expression = base->descriptor_index_expression ?
        msl_string_copy(base->descriptor_index_expression) : NULL;
    if (base->descriptor_index_expression && !descriptor_index_expression) {
        free(buffer_index_expression);
        return GPU_IR_OUT_OF_MEMORY;
    }
    char *descriptor_aux_expression = base->aux_expression ? msl_string_copy(base->aux_expression) : NULL;
    if (base->aux_expression && !descriptor_aux_expression) {
        free(buffer_index_expression);
        free(descriptor_index_expression);
        return GPU_IR_OUT_OF_MEMORY;
    }
    int dynamic_descriptor_is_image = 0;
    int dynamic_descriptor_is_combined = 0;
    msl_builder expression = { 0 };
    int rc = builder_appendf(&expression, "%s", base->expression);
    for (uint32_t operand_index = 1; rc == GPU_IR_OK && operand_index < instruction->operand_count; operand_index++) {
        uint32_t index_id;
        if (!instruction_id_operand(shader, instruction, operand_index, &index_id)) { rc = GPU_IR_INVALID; break; }
        const gpu_ir_shader_type *current = shader_type(shader, current_type_id);
        if (!current) { rc = GPU_IR_INVALID; break; }
        if (current->kind == GPU_IR_SHADER_TYPE_ARRAY &&
            pointer->storage_class == GPU_IR_SHADER_STORAGE_BUFFER && base->descriptor_array_count) {
            uint32_t descriptor_element;
            if (operand_index != 1 || descriptor_slot == UINT32_MAX ||
                base->descriptor_array_count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS - descriptor_slot) {
                rc = GPU_IR_UNSUPPORTED;
                break;
            }
            if (constant_u32(shader, index_id, &descriptor_element)) {
                if (descriptor_element >= base->descriptor_array_count) {
                    rc = GPU_IR_UNSUPPORTED;
                    break;
                }
                char buffer_name[64];
                int name_length = snprintf(buffer_name, sizeof buffer_name, "vx_buffer_%u_%u",
                                           base_id, descriptor_element);
                if (name_length < 0 || (size_t)name_length >= sizeof buffer_name) {
                    rc = GPU_IR_RANGE;
                    break;
                }
                free(expression.data);
                memset(&expression, 0, sizeof expression);
                rc = builder_appendf(&expression, "%s", buffer_name);
                descriptor_slot += descriptor_element;
            } else {
                const msl_symbol *index = symbols_find(symbols, index_id);
                const gpu_ir_shader_type *index_type = index ? shader_type(shader, index->type_id) : NULL;
                if (!index || index->is_pointer || !index_type || index_type->kind != GPU_IR_SHADER_TYPE_INT ||
                    index_type->bit_width != 32 || descriptor_index_expression) {
                    rc = GPU_IR_UNSUPPORTED;
                    break;
                }
                msl_builder selected = { 0 };
                for (uint32_t element = 0; rc == GPU_IR_OK && element + 1 < base->descriptor_array_count; element++)
                    rc = builder_appendf(&selected, "(uint(%s) == %uu ? vx_buffer_%u_%u : ",
                                         index->expression, element, base_id, element);
                if (rc == GPU_IR_OK)
                    rc = builder_appendf(&selected, "vx_buffer_%u_%u", base_id,
                                         base->descriptor_array_count - 1);
                for (uint32_t element = 0; rc == GPU_IR_OK && element + 1 < base->descriptor_array_count; element++)
                    rc = builder_appendf(&selected, ")");
                if (rc == GPU_IR_OK) {
                    free(expression.data);
                    expression = selected;
                    memset(&selected, 0, sizeof selected);
                    msl_builder normalized_index = { 0 };
                    rc = builder_appendf(&normalized_index, "uint(%s)", index->expression);
                    if (rc == GPU_IR_OK) descriptor_index_expression = normalized_index.data;
                    else free(normalized_index.data);
                    buffer_descriptor_array_count = base->descriptor_array_count;
                }
                free(selected.data);
            }
            current_type_id = current->element_type_id;
        } else if (current->kind == GPU_IR_SHADER_TYPE_ARRAY &&
                   pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT &&
                   base->descriptor_array_count) {
            uint32_t descriptor_element;
            if (operand_index != 1) {
                rc = GPU_IR_UNSUPPORTED;
                break;
            }
            char descriptor_name[64];
            const gpu_ir_shader_instruction *global = NULL;
            for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
                const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(shader, i);
                if (candidate && candidate->opcode == GPU_IR_SHADER_OP_VARIABLE &&
                    candidate->function_index == UINT32_MAX && candidate->result_id == base_id) {
                    global = candidate;
                    break;
                }
            }
            if (!global) { rc = GPU_IR_INVALID; break; }
            int constant_index = constant_u32(shader, index_id, &descriptor_element);
            if (constant_index && descriptor_element >= base->descriptor_array_count) {
                rc = GPU_IR_UNSUPPORTED;
                break;
            }
            int sampler_array = global_sampler_shape(shader, global) == GPU_IR_OK;
            int combined_array = global_combined_image_shape(shader, global, NULL) == GPU_IR_OK;
            int storage_image_array = global_storage_image_shape(shader, global, NULL, NULL, NULL) == GPU_IR_OK;
            int sampled_image_array = global_sampled_image_shape(shader, global, NULL) == GPU_IR_OK;
            if (!sampler_array && !combined_array && !storage_image_array &&
                !sampled_image_array) {
                rc = GPU_IR_UNSUPPORTED;
                break;
            }
            free(expression.data);
            memset(&expression, 0, sizeof expression);
            if (constant_index) {
                if (descriptor_element > UINT32_MAX - base_id) { rc = GPU_IR_RANGE; break; }
                int name_length = snprintf(descriptor_name, sizeof descriptor_name,
                    sampler_array ? "vx_sampler_%u_%u" : "vx_image_%u_%u", base_id, descriptor_element);
                if (name_length < 0 || (size_t)name_length >= sizeof descriptor_name) {
                    rc = GPU_IR_RANGE;
                    break;
                }
                rc = builder_appendf(&expression, "%s", descriptor_name);
                descriptor_array_element = descriptor_element;
            } else {
                const msl_symbol *index = symbols_find(symbols, index_id);
                const gpu_ir_shader_type *index_type = index ? shader_type(shader, index->type_id) : NULL;
                if (!index || index->is_pointer || !index_type ||
                    index_type->kind != GPU_IR_SHADER_TYPE_INT || index_type->bit_width != 32 ||
                    descriptor_index_expression) {
                    rc = GPU_IR_UNSUPPORTED;
                    break;
                }
                msl_builder selected = { 0 };
                rc = append_descriptor_array_selection(&selected,
                    sampler_array ? "vx_sampler_" : "vx_image_", base_id,
                    base->descriptor_array_count, index->expression);
                if (rc == GPU_IR_OK) {
                    expression = selected;
                    memset(&selected, 0, sizeof selected);
                    if (combined_array) {
                        msl_builder selected_sampler = { 0 };
                        rc = append_descriptor_array_selection(&selected_sampler, "vx_sampler_", base_id,
                            base->descriptor_array_count, index->expression);
                        if (rc == GPU_IR_OK) {
                            free(descriptor_aux_expression);
                            descriptor_aux_expression = selected_sampler.data;
                        }
                        else free(selected_sampler.data);
                    }
                    msl_builder normalized_index = { 0 };
                    if (rc == GPU_IR_OK) rc = builder_appendf(&normalized_index, "%s", index->expression);
                    if (rc == GPU_IR_OK) descriptor_index_expression = normalized_index.data;
                    else free(normalized_index.data);
                    dynamic_descriptor_is_image = storage_image_array || sampled_image_array || combined_array;
                    dynamic_descriptor_is_combined = combined_array;
                }
                free(selected.data);
            }
            current_type_id = current->element_type_id;
        } else if (current->kind == GPU_IR_SHADER_TYPE_STRUCT) {
            uint32_t member_index;
            if (!constant_u32(shader, index_id, &member_index) || member_index >= current->member_count) {
                rc = GPU_IR_UNSUPPORTED; break;
            }
            const gpu_ir_shader_member *member = gpu_ir_shader_type_member_at(shader, current->first_member + member_index);
            if (!member) { rc = GPU_IR_INVALID; break; }
            if (pointer->storage_class == GPU_IR_SHADER_STORAGE_PUSH_CONSTANT) {
                if (!member->has_offset || (member->offset & 3u) ||
                    member->offset > UINT32_MAX - push_constant_offset) {
                    rc = GPU_IR_UNSUPPORTED; break;
                }
                push_constant_offset += member->offset;
                const gpu_ir_shader_type *member_type = shader_type(shader, member->type_id);
                push_matrix_stride = member_type && member_type->kind == GPU_IR_SHADER_TYPE_MATRIX &&
                    member->has_matrix_stride && !(member->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_ROW_MAJOR) ?
                    member->matrix_stride : 0;
            } else if (pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM) {
                if (operand_index != 1 || current_type_id != pointer->element_type_id || !member->has_offset ||
                    (member->offset & 3u) || member->offset > UINT32_MAX - uniform_buffer_offset) {
                    rc = GPU_IR_UNSUPPORTED; break;
                }
                uniform_buffer_offset += member->offset;
            } else if (member_index != 0 || current->member_count != 1) {
                rc = GPU_IR_UNSUPPORTED; break;
            }
            current_type_id = member->type_id;
        } else if (current->kind == GPU_IR_SHADER_TYPE_ARRAY &&
                   pointer->storage_class == GPU_IR_SHADER_STORAGE_PUSH_CONSTANT) {
            uint32_t element_index, array_length;
            if (!constant_u32(shader, index_id, &element_index) ||
                !constant_u32(shader, current->array_length_id, &array_length) ||
                element_index >= array_length || !current->has_array_stride || !current->array_stride ||
                (current->array_stride & 3u) ||
                element_index > (UINT32_MAX - push_constant_offset) / current->array_stride) {
                rc = GPU_IR_UNSUPPORTED; break;
            }
            push_constant_offset += element_index * current->array_stride;
            current_type_id = current->element_type_id;
            push_matrix_stride = 0;
        } else if (current->kind == GPU_IR_SHADER_TYPE_MATRIX &&
                   pointer->storage_class == GPU_IR_SHADER_STORAGE_PUSH_CONSTANT) {
            uint32_t column;
            if (!push_matrix_stride || (push_matrix_stride & 3u) ||
                !constant_u32(shader, index_id, &column) || column >= current->element_count ||
                column > (UINT32_MAX - push_constant_offset) / push_matrix_stride) {
                rc = GPU_IR_UNSUPPORTED; break;
            }
            push_constant_offset += column * push_matrix_stride;
            current_type_id = current->element_type_id;
            push_matrix_stride = 0;
        } else if (current->kind == GPU_IR_SHADER_TYPE_VECTOR &&
                   pointer->storage_class == GPU_IR_SHADER_STORAGE_PUSH_CONSTANT) {
            uint32_t component;
            if (!constant_u32(shader, index_id, &component) || component >= current->element_count ||
                component > (UINT32_MAX - push_constant_offset) / 4u) {
                rc = GPU_IR_UNSUPPORTED; break;
            }
            push_constant_offset += component * 4u;
            current_type_id = current->element_type_id;
            push_matrix_stride = 0;
        } else if (current->kind == GPU_IR_SHADER_TYPE_ARRAY &&
                   (pointer->storage_class == GPU_IR_SHADER_STORAGE_FUNCTION ||
                    pointer->storage_class == GPU_IR_SHADER_STORAGE_WORKGROUP)) {
            const msl_symbol *index = symbols_find(symbols, index_id);
            const gpu_ir_shader_type *index_type = index ? shader_type(shader, index->type_id) : NULL;
            if (!index || index->is_pointer || !index_type || index_type->kind != GPU_IR_SHADER_TYPE_INT ||
                index_type->bit_width != 32) { rc = GPU_IR_UNSUPPORTED; break; }
            rc = builder_appendf(&expression, "[%s]", index->expression);
            current_type_id = current->element_type_id;
        } else if (current->kind == GPU_IR_SHADER_TYPE_RUNTIME_ARRAY) {
            const msl_symbol *index = symbols_find(symbols, index_id);
            if (!index || index->is_pointer || !shader_type_is_u32(shader, index->type_id) ||
                !shader_type_is_32bit_buffer_scalar(shader, current->element_type_id) ||
                !current->has_array_stride || current->array_stride != 4 || descriptor_slot == UINT32_MAX ||
                buffer_index_expression) { rc = GPU_IR_UNSUPPORTED; break; }
            rc = builder_appendf(&expression, "[%s]", index->expression);
            if (rc == GPU_IR_OK) {
                buffer_index_expression = msl_string_copy(index->expression);
                if (!buffer_index_expression) rc = GPU_IR_OUT_OF_MEMORY;
            }
            current_type_id = current->element_type_id;
        } else {
            rc = GPU_IR_UNSUPPORTED;
        }
    }
    if (rc == GPU_IR_OK && pointer->storage_class == GPU_IR_SHADER_STORAGE_PUSH_CONSTANT) {
        char pointee_name[32];
        if ((push_constant_offset & 3u) ||
            !push_constant_type_name(shader, current_type_id, pointee_name, sizeof pointee_name)) {
            rc = GPU_IR_UNSUPPORTED;
        } else {
            free(expression.data);
            memset(&expression, 0, sizeof expression);
            rc = builder_appendf(&expression, "reinterpret_cast<constant %s*>(%s + %u)",
                                 pointee_name, base->expression, push_constant_offset / 4u);
        }
    }
    if (rc == GPU_IR_OK && pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM &&
        current_type_id != pointer->element_type_id) {
        char pointee_name[32];
        if ((uniform_buffer_offset & 3u) ||
            !shader_type_name(shader, current_type_id, pointee_name, sizeof pointee_name)) {
            rc = GPU_IR_UNSUPPORTED;
        } else if (!shader_type_is_u32(shader, current_type_id) || uniform_buffer_offset) {
            free(expression.data);
            memset(&expression, 0, sizeof expression);
            rc = builder_appendf(&expression, "reinterpret_cast<constant %s*>(%s + %u)",
                                 pointee_name, base->expression, uniform_buffer_offset / 4u);
        }
        if (rc == GPU_IR_OK) {
            char index[32];
            int length = snprintf(index, sizeof index, "%u", uniform_buffer_offset / 4u);
            if (length < 0 || (size_t)length >= sizeof index) {
                rc = GPU_IR_RANGE;
            } else {
                char *copy = msl_string_copy(index);
                if (!copy) rc = GPU_IR_OUT_OF_MEMORY;
                else {
                    free(buffer_index_expression);
                    buffer_index_expression = copy;
                }
            }
        }
    }
    const gpu_ir_shader_type *result_pointer = shader_type(shader, instruction->result_type_id);
    if (rc == GPU_IR_OK && (!result_pointer || result_pointer->kind != GPU_IR_SHADER_TYPE_POINTER ||
        result_pointer->storage_class != pointer->storage_class || result_pointer->element_type_id != current_type_id))
        rc = GPU_IR_INVALID;
    if (rc == GPU_IR_OK) {
        rc = symbols_add(mutable_symbols, instruction->result_id,
            instruction->result_type_id, 1, expression.data ? expression.data : "");
        if (rc == GPU_IR_OK && descriptor_array_element != UINT32_MAX &&
            pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) {
            const gpu_ir_shader_type *array = shader_type(shader, pointer->element_type_id);
            const gpu_ir_shader_instruction *global = NULL;
            if (!array || array->kind != GPU_IR_SHADER_TYPE_ARRAY) rc = GPU_IR_INVALID;
            for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
                const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(shader, i);
                if (candidate && candidate->opcode == GPU_IR_SHADER_OP_VARIABLE &&
                    candidate->function_index == UINT32_MAX && candidate->result_id == base_id) {
                    global = candidate;
                    break;
                }
            }
            if (!global) rc = GPU_IR_INVALID;
            if (rc == GPU_IR_OK && global_combined_image_shape(shader, global, NULL) == GPU_IR_OK) {
                char sampler_name[64];
                int name_length = snprintf(sampler_name, sizeof sampler_name,
                    "vx_sampler_%u_%u", base_id, descriptor_array_element);
                if (name_length < 0 || (size_t)name_length >= sizeof sampler_name) rc = GPU_IR_RANGE;
                if (rc == GPU_IR_OK)
                rc = symbols_set_aux_expression(mutable_symbols, instruction->result_id, sampler_name);
            }
            const gpu_ir_shader_type *element = array ? shader_type(shader, array->element_type_id) : NULL;
            if (rc == GPU_IR_OK && element &&
                (element->kind == GPU_IR_SHADER_TYPE_IMAGE || element->kind == GPU_IR_SHADER_TYPE_SAMPLED_IMAGE))
                rc = symbols_set_image_access(mutable_symbols, instruction->result_id,
                    base->image_readable, base->image_writable);
        }
        if (rc == GPU_IR_OK && descriptor_index_expression &&
            pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) {
            if (dynamic_descriptor_is_image)
                rc = symbols_set_image_access(mutable_symbols, instruction->result_id,
                    base->image_readable, base->image_writable);
            if (rc == GPU_IR_OK && dynamic_descriptor_is_combined)
                rc = symbols_set_aux_expression(mutable_symbols, instruction->result_id,
                    descriptor_aux_expression);
            if (rc == GPU_IR_OK)
                rc = symbols_set_descriptor_index(mutable_symbols, instruction->result_id,
                    descriptor_index_expression, base->descriptor_array_count);
        }
        if (rc == GPU_IR_OK && descriptor_slot != UINT32_MAX)
            rc = symbols_set_buffer_access(mutable_symbols, instruction->result_id,
                                           descriptor_slot, buffer_index_expression, base->buffer_writable);
        if (rc == GPU_IR_OK && pointer->storage_class == GPU_IR_SHADER_STORAGE_BUFFER &&
            descriptor_index_expression)
            rc = symbols_set_descriptor_index(mutable_symbols, instruction->result_id,
                descriptor_index_expression, buffer_descriptor_array_count);
    }
    free(buffer_index_expression);
    free(descriptor_index_expression);
    free(descriptor_aux_expression);
    free(expression.data);
    return rc;
}

static int emit_buffer_bounds_condition(msl_builder *body, const msl_symbol *pointer) {
    if (!pointer || pointer->descriptor_slot >= GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS ||
        !pointer->buffer_index_expression)
        return GPU_IR_INVALID;
    if (pointer->descriptor_index_expression) {
        if (!pointer->descriptor_array_count || pointer->descriptor_array_count >
            GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS - pointer->descriptor_slot) return GPU_IR_INVALID;
        return builder_appendf(body,
            "(uint(%s) < %uu && (ulong(%s) + ulong(1)) * ulong(4) <= vx_buffer_ranges[%u + uint(%s)])",
            pointer->descriptor_index_expression, pointer->descriptor_array_count,
            pointer->buffer_index_expression, pointer->descriptor_slot,
            pointer->descriptor_index_expression);
    }
    return builder_appendf(body, "((ulong(%s) + ulong(1)) * ulong(4) <= vx_buffer_ranges[%u])",
                           pointer->buffer_index_expression, pointer->descriptor_slot);
}

static int emit_load(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                     const msl_symbols *symbols, msl_builder *body, msl_symbols *mutable_symbols) {
    uint32_t pointer_id;
    if (instruction->operand_count != 1 || !instruction_id_operand(shader, instruction, 0, &pointer_id))
        return GPU_IR_INVALID;
    const msl_symbol *pointer_symbol = symbols_find(symbols, pointer_id);
    const gpu_ir_shader_type *pointer = pointer_symbol ? shader_type(shader, pointer_symbol->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    if (!pointer_symbol || !pointer_symbol->is_pointer || !pointer || pointer->kind != GPU_IR_SHADER_TYPE_POINTER ||
        pointer->element_type_id != instruction->result_type_id || !result_type ||
        instruction->memory_access_flags) return GPU_IR_UNSUPPORTED;
    if ((result_type->kind == GPU_IR_SHADER_TYPE_IMAGE ||
         result_type->kind == GPU_IR_SHADER_TYPE_SAMPLER ||
         result_type->kind == GPU_IR_SHADER_TYPE_SAMPLED_IMAGE) &&
        pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) {
        int rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id,
                             0, pointer_symbol->expression);
        if (rc == GPU_IR_OK && pointer_symbol->descriptor_index_expression)
            rc = symbols_copy_descriptor_indices(mutable_symbols, instruction->result_id, pointer_symbol);
        if (rc == GPU_IR_OK && pointer_symbol->aux_expression)
            rc = symbols_set_aux_expression(mutable_symbols, instruction->result_id,
                                             pointer_symbol->aux_expression);
        if (rc == GPU_IR_OK && (result_type->kind == GPU_IR_SHADER_TYPE_IMAGE ||
                                result_type->kind == GPU_IR_SHADER_TYPE_SAMPLED_IMAGE))
            rc = symbols_set_image_access(mutable_symbols, instruction->result_id,
                pointer_symbol->image_readable, pointer_symbol->image_writable);
        return rc;
    }
    char type_name[32];
    if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name)) return GPU_IR_UNSUPPORTED;
    int rc;
    if (pointer_symbol->descriptor_slot != UINT32_MAX) {
        if (!shader_type_is_32bit_buffer_scalar(shader, instruction->result_type_id) ||
            !pointer_symbol->buffer_index_expression) return GPU_IR_UNSUPPORTED;
        const char *zero = shader_type_is_u32(shader, instruction->result_type_id) ? "0u" :
            (shader_type(shader, instruction->result_type_id)->kind == GPU_IR_SHADER_TYPE_FLOAT ? "0.0f" : "0");
        if (body->assign_values)
            rc = builder_appendf(body, "    vx_value_%u = %s;\n    if (", instruction->result_id, zero);
        else
            rc = builder_appendf(body, "    %s vx_value_%u = %s;\n    if (", type_name, instruction->result_id,
                                 zero);
        if (rc == GPU_IR_OK) rc = emit_buffer_bounds_condition(body, pointer_symbol);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, ") vx_value_%u = %s%s%s;\n",
            instruction->result_id,
            pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM ? "(*" : "",
            pointer_symbol->expression,
            pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM ? ")" : "");
    } else {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = (pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM ||
                                   pointer->storage_class == GPU_IR_SHADER_STORAGE_PUSH_CONSTANT) ?
            builder_appendf(body, "(*%s);\n", pointer_symbol->expression) :
            builder_appendf(body, "%s;\n", pointer_symbol->expression);
    }
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_image_read_mip_oob(const gpu_ir_shader *shader,
                                   const gpu_ir_shader_instruction *instruction,
                                   const msl_symbols *symbols, msl_builder *body,
                                   msl_symbols *mutable_symbols) {
    uint32_t image_id, x_id, y_id, mip_id, layer_id = 0;
    if ((instruction->operand_count != 4 && instruction->operand_count != 5) ||
        instruction->literal_word_count != 3 ||
        !instruction_id_operand(shader, instruction, 0, &image_id) ||
        !instruction_id_operand(shader, instruction, 1, &x_id) ||
        !instruction_id_operand(shader, instruction, 2, &y_id) ||
        !instruction_id_operand(shader, instruction, 3, &mip_id) ||
        (instruction->operand_count == 5 &&
         !instruction_id_operand(shader, instruction, 4, &layer_id))) return GPU_IR_INVALID;
    const msl_symbol *image = symbols_find(symbols, image_id);
    const msl_symbol *x = symbols_find(symbols, x_id);
    const msl_symbol *y = symbols_find(symbols, y_id);
    const msl_symbol *mip = symbols_find(symbols, mip_id);
    const msl_symbol *layer = layer_id ? symbols_find(symbols, layer_id) : NULL;
    const gpu_ir_shader_type *image_type = image ? shader_type(shader, image->type_id) : NULL;
    const gpu_ir_shader_type *component = image_type && image_type->kind == GPU_IR_SHADER_TYPE_IMAGE ?
        shader_type(shader, image_type->element_type_id) : NULL;
    const gpu_ir_shader_type *x_type = x ? shader_type(shader, x->type_id) : NULL;
    const gpu_ir_shader_type *y_type = y ? shader_type(shader, y->type_id) : NULL;
    const gpu_ir_shader_type *mip_type = mip ? shader_type(shader, mip->type_id) : NULL;
    const gpu_ir_shader_type *layer_type = layer ? shader_type(shader, layer->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    int integer_read = component && component->kind == GPU_IR_SHADER_TYPE_INT &&
        component->bit_width == 32 && component->signedness <= 1 && result_type &&
        result_type->kind == GPU_IR_SHADER_TYPE_INT && result_type->bit_width == 32 &&
        result_type->signedness <= 1;
    int float_read = component && component->kind == GPU_IR_SHADER_TYPE_FLOAT && component->bit_width == 32 &&
        result_type && result_type->kind == GPU_IR_SHADER_TYPE_FLOAT && result_type->bit_width == 32;
    if (!image || image->is_pointer || !image->image_readable || !image_type ||
        image_type->kind != GPU_IR_SHADER_TYPE_IMAGE || image_type->image_dimension != 1 ||
        image_type->image_depth || image_type->image_arrayed > 1 ||
        image_type->image_arrayed != (instruction->operand_count == 5) || image_type->image_multisampled ||
        image_type->image_sampled != 1 || image_type->image_format || image_type->image_access_qualifier ||
        !(integer_read || float_read) || !x || x->is_pointer || !x_type || x_type->kind != GPU_IR_SHADER_TYPE_INT ||
        x_type->bit_width != 32 || !y || y->is_pointer || !y_type || y_type->kind != GPU_IR_SHADER_TYPE_INT ||
        y_type->bit_width != 32 || !mip || mip->is_pointer || !mip_type ||
        mip_type->kind != GPU_IR_SHADER_TYPE_INT || mip_type->bit_width != 32 ||
        (image_type->image_arrayed && (!layer || layer->is_pointer || !layer_type ||
         layer_type->kind != GPU_IR_SHADER_TYPE_INT || layer_type->bit_width != 32)) || !result_type)
        return GPU_IR_UNSUPPORTED;

    const char *read_cast_prefix = component->signedness == result_type->signedness ? "" :
        (result_type->signedness ? "as_type<int>(" : "as_type<uint>(");
    const char *read_cast_suffix = read_cast_prefix[0] ? ")" : "";

    uint32_t offset_x_bits = gpu_ir_shader_literal_word_at(shader, instruction->first_literal);
    uint32_t offset_y_bits = gpu_ir_shader_literal_word_at(shader, instruction->first_literal + 1u);
    int32_t offset_x, offset_y;
    memcpy(&offset_x, &offset_x_bits, sizeof offset_x);
    memcpy(&offset_y, &offset_y_bits, sizeof offset_y);
    uint32_t component_index = gpu_ir_shader_literal_word_at(shader, instruction->first_literal + 2u);
    static const char *const component_names[] = { "x", "y", "z", "w" };
    if (component_index >= sizeof component_names / sizeof component_names[0]) return GPU_IR_INVALID;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK) {
        const char *zero = float_read ? "0.0f;\n" : result_type->signedness ? "0;\n" : "0u;\n";
        rc = builder_appendf(body, zero);
    }
    if (rc == GPU_IR_OK) rc = builder_appendf(body,
        "    long vx_read_mip_%u = long(as_type<int>(uint(%s)));\n"
        "    long vx_read_x_%u = long(as_type<int>(uint(%s))) + long(%lldll);\n"
        "    long vx_read_y_%u = long(as_type<int>(uint(%s))) + long(%lldll);\n",
        instruction->result_id, mip->expression,
        instruction->result_id, x->expression, (long long)offset_x,
        instruction->result_id, y->expression, (long long)offset_y);
    if (rc == GPU_IR_OK && image_type->image_arrayed)
        rc = builder_appendf(body,
            "    long vx_read_layer_%u = long(as_type<int>(uint(%s)));\n",
            instruction->result_id, layer->expression);
    if (rc == GPU_IR_OK) rc = builder_appendf(body,
        "    if (vx_read_mip_%u >= 0 && ulong(vx_read_mip_%u) < ulong(%s.get_num_mip_levels())) {\n"
        "        uint vx_read_level_%u = uint(vx_read_mip_%u);\n"
        "        long vx_read_width_%u = long(%s.get_width(vx_read_level_%u));\n"
        "        long vx_read_height_%u = long(%s.get_height(vx_read_level_%u));\n",
        instruction->result_id, instruction->result_id, image->expression,
        instruction->result_id, instruction->result_id,
        instruction->result_id, image->expression, instruction->result_id,
        instruction->result_id, image->expression, instruction->result_id);
    if (rc == GPU_IR_OK && image_type->image_arrayed)
        rc = builder_appendf(body,
            "        if (vx_read_layer_%u >= 0 && ulong(vx_read_layer_%u) < ulong(%s.get_array_size()) &&"
            " vx_read_x_%u >= 0 && vx_read_y_%u >= 0 && vx_read_x_%u < vx_read_width_%u &&"
            " vx_read_y_%u < vx_read_height_%u)\n"
            "            vx_value_%u = %s%s.read(uint2(uint(vx_read_x_%u), uint(vx_read_y_%u)),"
            " uint(vx_read_layer_%u), vx_read_level_%u).%s%s;\n",
            instruction->result_id, instruction->result_id, image->expression,
            instruction->result_id, instruction->result_id,
            instruction->result_id, instruction->result_id,
            instruction->result_id, instruction->result_id,
            instruction->result_id, read_cast_prefix, image->expression,
            instruction->result_id, instruction->result_id,
            instruction->result_id, instruction->result_id, component_names[component_index], read_cast_suffix);
    else if (rc == GPU_IR_OK)
        rc = builder_appendf(body,
            "        if (vx_read_x_%u >= 0 && vx_read_y_%u >= 0 &&"
            " vx_read_x_%u < vx_read_width_%u && vx_read_y_%u < vx_read_height_%u)\n"
            "            vx_value_%u = %s%s.read(uint2(uint(vx_read_x_%u), uint(vx_read_y_%u)),"
            " vx_read_level_%u).%s%s;\n",
            instruction->result_id, instruction->result_id,
            instruction->result_id, instruction->result_id,
            instruction->result_id, instruction->result_id,
            instruction->result_id, read_cast_prefix, image->expression,
            instruction->result_id, instruction->result_id,
            instruction->result_id, component_names[component_index], read_cast_suffix);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, "    }\n");
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_image_read_msaa_oob(const gpu_ir_shader *shader,
                                    const gpu_ir_shader_instruction *instruction,
                                    const msl_symbols *symbols, msl_builder *body,
                                    msl_symbols *mutable_symbols) {
    uint32_t image_id, x_id, y_id, layer_id = 0, sample_id;
    if ((instruction->operand_count != 4 && instruction->operand_count != 5) ||
        instruction->literal_word_count != 3 ||
        !instruction_id_operand(shader, instruction, 0, &image_id) ||
        !instruction_id_operand(shader, instruction, 1, &x_id) ||
        !instruction_id_operand(shader, instruction, 2, &y_id)) return GPU_IR_INVALID;
    const msl_symbol *image = symbols_find(symbols, image_id);
    const msl_symbol *x = symbols_find(symbols, x_id);
    const msl_symbol *y = symbols_find(symbols, y_id);
    const gpu_ir_shader_type *image_type = image ? shader_type(shader, image->type_id) : NULL;
    int arrayed = image_type && image_type->image_arrayed;
    if ((arrayed && instruction->operand_count != 5) || (!arrayed && instruction->operand_count != 4) ||
        (arrayed && !instruction_id_operand(shader, instruction, 3, &layer_id)) ||
        !instruction_id_operand(shader, instruction, arrayed ? 4u : 3u, &sample_id)) return GPU_IR_INVALID;
    const msl_symbol *layer = arrayed ? symbols_find(symbols, layer_id) : NULL;
    const msl_symbol *sample = symbols_find(symbols, sample_id);
    const gpu_ir_shader_type *component = image_type && image_type->kind == GPU_IR_SHADER_TYPE_IMAGE ?
        shader_type(shader, image_type->element_type_id) : NULL;
    const gpu_ir_shader_type *x_type = x ? shader_type(shader, x->type_id) : NULL;
    const gpu_ir_shader_type *y_type = y ? shader_type(shader, y->type_id) : NULL;
    const gpu_ir_shader_type *layer_type = layer ? shader_type(shader, layer->type_id) : NULL;
    const gpu_ir_shader_type *sample_type = sample ? shader_type(shader, sample->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    if (!image || image->is_pointer || !image->image_readable || !image_type ||
        image_type->kind != GPU_IR_SHADER_TYPE_IMAGE || image_type->image_dimension != 1 ||
        image_type->image_depth || image_type->image_arrayed > 1 || image_type->image_multisampled != 1 ||
        image_type->image_sampled != 1 || image_type->image_format || image_type->image_access_qualifier ||
        !component || component->kind != GPU_IR_SHADER_TYPE_FLOAT || component->bit_width != 32 ||
        !result_type || result_type->kind != GPU_IR_SHADER_TYPE_FLOAT || result_type->bit_width != 32 ||
        !x || x->is_pointer || !x_type || x_type->kind != GPU_IR_SHADER_TYPE_INT || x_type->bit_width != 32 ||
        !y || y->is_pointer || !y_type || y_type->kind != GPU_IR_SHADER_TYPE_INT || y_type->bit_width != 32 ||
        (arrayed && (!layer || layer->is_pointer || !layer_type || layer_type->kind != GPU_IR_SHADER_TYPE_INT ||
                     layer_type->bit_width != 32)) ||
        !sample || sample->is_pointer || !sample_type || sample_type->kind != GPU_IR_SHADER_TYPE_INT ||
        sample_type->bit_width != 32) return GPU_IR_UNSUPPORTED;

    uint32_t offset_x_bits = gpu_ir_shader_literal_word_at(shader, instruction->first_literal);
    uint32_t offset_y_bits = gpu_ir_shader_literal_word_at(shader, instruction->first_literal + 1u);
    int32_t offset_x, offset_y;
    memcpy(&offset_x, &offset_x_bits, sizeof offset_x);
    memcpy(&offset_y, &offset_y_bits, sizeof offset_y);
    uint32_t component_index = gpu_ir_shader_literal_word_at(shader, instruction->first_literal + 2u);
    static const char *const component_names[] = { "x", "y", "z", "w" };
    if (component_index >= sizeof component_names / sizeof component_names[0]) return GPU_IR_INVALID;

    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, "0.0f;\n");
    if (rc == GPU_IR_OK) rc = builder_appendf(body,
        "    long vx_read_x_%u = long(as_type<int>(uint(%s))) + long(%lldll);\n"
        "    long vx_read_y_%u = long(as_type<int>(uint(%s))) + long(%lldll);\n",
        instruction->result_id, x->expression, (long long)offset_x,
        instruction->result_id, y->expression, (long long)offset_y);
    if (rc == GPU_IR_OK && arrayed) rc = builder_appendf(body,
        "    long vx_read_layer_%u = long(as_type<int>(uint(%s)));\n"
        "    if (vx_read_x_%u >= 0 && vx_read_y_%u >= 0 && vx_read_layer_%u >= 0 &&"
        " vx_read_x_%u < long(%s.get_width()) && vx_read_y_%u < long(%s.get_height()) &&"
        " vx_read_layer_%u < long(%s.get_array_size()))\n"
        "        vx_value_%u = %s.read(uint2(uint(vx_read_x_%u), uint(vx_read_y_%u)),"
        " uint(vx_read_layer_%u), uint(%s)).%s;\n",
        instruction->result_id, layer->expression,
        instruction->result_id, instruction->result_id, instruction->result_id,
        instruction->result_id, image->expression, instruction->result_id, image->expression,
        instruction->result_id, image->expression,
        instruction->result_id, image->expression, instruction->result_id, instruction->result_id,
        instruction->result_id, sample->expression, component_names[component_index]);
    if (rc == GPU_IR_OK && !arrayed) rc = builder_appendf(body,
        "    if (vx_read_x_%u >= 0 && vx_read_y_%u >= 0 &&"
        " vx_read_x_%u < long(%s.get_width()) && vx_read_y_%u < long(%s.get_height()))\n"
        "        vx_value_%u = %s.read(uint2(uint(vx_read_x_%u), uint(vx_read_y_%u)),"
        " uint(%s)).%s;\n",
        instruction->result_id, instruction->result_id,
        instruction->result_id, image->expression, instruction->result_id, image->expression,
        instruction->result_id, image->expression, instruction->result_id, instruction->result_id,
        sample->expression, component_names[component_index]);
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_image_read(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                           const msl_symbols *symbols, msl_builder *body, msl_symbols *mutable_symbols) {
    if (instruction->operation == GPU_IR_SHADER_IMAGE_READ_2D_LOD_OOB)
        return emit_image_read_mip_oob(shader, instruction, symbols, body, mutable_symbols);
    if (instruction->operation == GPU_IR_SHADER_IMAGE_READ_2D_MSAA_OOB)
        return emit_image_read_msaa_oob(shader, instruction, symbols, body, mutable_symbols);
    uint32_t image_id, coordinate_id;
    if (instruction->operand_count != 2 || instruction->literal_word_count ||
        !instruction_id_operand(shader, instruction, 0, &image_id) ||
        !instruction_id_operand(shader, instruction, 1, &coordinate_id)) return GPU_IR_INVALID;
    const msl_symbol *image = symbols_find(symbols, image_id);
    const msl_symbol *coordinate = symbols_find(symbols, coordinate_id);
    const gpu_ir_shader_type *image_type = image ? shader_type(shader, image->type_id) : NULL;
    const gpu_ir_shader_type *coordinate_type = coordinate ? shader_type(shader, coordinate->type_id) : NULL;
    const gpu_ir_shader_type *coordinate_component = coordinate_type &&
        coordinate_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, coordinate_type->element_type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    const gpu_ir_shader_type *result_component = result_type && result_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, result_type->element_type_id) : NULL;
    int storage_read = instruction->source_opcode == 98 && image_type &&
        image_type->image_sampled == 2 && image_type->image_format == 4;
    int sampled_fetch = instruction->source_opcode == 95 && image_type &&
        image_type->image_sampled == 1 && image_type->image_format == 0;
    if (!image || image->is_pointer || !image->image_readable || !image_type ||
        image_type->kind != GPU_IR_SHADER_TYPE_IMAGE || (!storage_read && !sampled_fetch) ||
        !coordinate || coordinate->is_pointer || !coordinate_type ||
        coordinate_type->kind != GPU_IR_SHADER_TYPE_VECTOR || coordinate_type->element_count != 2 ||
        !coordinate_component || coordinate_component->kind != GPU_IR_SHADER_TYPE_INT ||
        coordinate_component->bit_width != 32 || !result_type || result_type->kind != GPU_IR_SHADER_TYPE_VECTOR ||
        result_type->element_count != 4 || !result_component || result_component->kind != GPU_IR_SHADER_TYPE_FLOAT ||
        result_component->bit_width != 32)
        return GPU_IR_UNSUPPORTED;
    int rc;
    if (image->descriptor_index_expression || image->secondary_descriptor_index_expression) {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK)
            rc = builder_appendf(body, "float4(0.0f);\n    if ");
        if (rc == GPU_IR_OK) rc = emit_descriptor_bounds_condition(body, image);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, " vx_value_%u = ", instruction->result_id);
        if (rc == GPU_IR_OK)
            rc = builder_appendf(body, "%s.read(uint2(%s));\n", image->expression, coordinate->expression);
    } else {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK)
            rc = builder_appendf(body, "%s.read(uint2(%s));\n", image->expression, coordinate->expression);
    }
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_image_query_size(const gpu_ir_shader *shader,
                                 const gpu_ir_shader_instruction *instruction,
                                 const msl_symbols *symbols, msl_builder *body,
                                 msl_symbols *mutable_symbols) {
    uint32_t image_id, lod_id = 0;
    if (instruction->operation != GPU_IR_SHADER_IMAGE_QUERY_SIZE ||
        (instruction->operand_count != 1 && instruction->operand_count != 2) ||
        instruction->literal_word_count || !instruction_id_operand(shader, instruction, 0, &image_id))
        return GPU_IR_INVALID;
    int has_lod = instruction->operand_count == 2;
    if (has_lod && !instruction_id_operand(shader, instruction, 1, &lod_id)) return GPU_IR_INVALID;
    const msl_symbol *image = symbols_find(symbols, image_id);
    const msl_symbol *lod = has_lod ? symbols_find(symbols, lod_id) : NULL;
    const gpu_ir_shader_type *image_type = image ? shader_type(shader, image->type_id) : NULL;
    const gpu_ir_shader_type *lod_type = lod ? shader_type(shader, lod->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    const gpu_ir_shader_type *component_type = result_type && result_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, result_type->element_type_id) : NULL;
    int includes_array_size = result_type && result_type->kind == GPU_IR_SHADER_TYPE_VECTOR &&
        result_type->element_count == 3;
    char result_name[32];
    if (!image || image->is_pointer || !image_type || image_type->kind != GPU_IR_SHADER_TYPE_IMAGE ||
        image_type->image_dimension != 1 || image_type->image_depth ||
        (image_type->image_multisampled && (has_lod || image_type->image_arrayed)) ||
        (has_lod && (!lod || lod->is_pointer || !lod_type ||
        lod_type->kind != GPU_IR_SHADER_TYPE_INT || lod_type->bit_width != 32)) ||
        !component_type || component_type->kind != GPU_IR_SHADER_TYPE_INT ||
        component_type->bit_width != 32 || !result_type || result_type->kind != GPU_IR_SHADER_TYPE_VECTOR ||
        (result_type->element_count != 2 && (!image_type->image_arrayed || result_type->element_count != 3)) ||
        !shader_type_name(shader, instruction->result_type_id,
                                                             result_name, sizeof result_name))
        return GPU_IR_UNSUPPORTED;
    const char *zero = includes_array_size ?
        (component_type->signedness ? "int3(0)" : "uint3(0u)") :
        (component_type->signedness ? "int2(0)" : "uint2(0u)");
    const char *scalar = component_type->signedness ? "int" : "uint";
    int indexed = image->descriptor_index_expression || image->secondary_descriptor_index_expression;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK && indexed) rc = builder_appendf(body, "%s;\n    if ", zero);
    if (rc == GPU_IR_OK && indexed) rc = emit_descriptor_bounds_condition(body, image);
    if (rc == GPU_IR_OK && indexed)
        rc = builder_appendf(body, " vx_value_%u = ", instruction->result_id);
    if (rc == GPU_IR_OK && has_lod && includes_array_size)
        rc = builder_appendf(body,
            "%s(%s(%s.get_width(uint(%s))), %s(%s.get_height(uint(%s))), %s(%s.get_array_size()));\n",
            result_name, scalar, image->expression, lod->expression,
            scalar, image->expression, lod->expression, scalar, image->expression);
    else if (rc == GPU_IR_OK && has_lod)
        rc = builder_appendf(body, "%s(%s(%s.get_width(uint(%s))), %s(%s.get_height(uint(%s))));\n",
                             result_name, scalar, image->expression, lod->expression,
                             scalar, image->expression, lod->expression);
    else if (rc == GPU_IR_OK && includes_array_size)
        rc = builder_appendf(body,
            "%s(%s(%s.get_width()), %s(%s.get_height()), %s(%s.get_array_size()));\n",
            result_name, scalar, image->expression, scalar, image->expression,
            scalar, image->expression);
    else if (rc == GPU_IR_OK)
        rc = builder_appendf(body, "%s(%s(%s.get_width()), %s(%s.get_height()));\n",
                             result_name, scalar, image->expression, scalar, image->expression);
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_image_query_levels(const gpu_ir_shader *shader,
                                   const gpu_ir_shader_instruction *instruction,
                                   const msl_symbols *symbols, msl_builder *body,
                                   msl_symbols *mutable_symbols) {
    uint32_t image_id;
    if (instruction->operation != GPU_IR_SHADER_IMAGE_QUERY_LEVELS || instruction->operand_count != 1 ||
        instruction->literal_word_count || !instruction_id_operand(shader, instruction, 0, &image_id))
        return GPU_IR_INVALID;
    const msl_symbol *image = symbols_find(symbols, image_id);
    const gpu_ir_shader_type *image_type = image ? shader_type(shader, image->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    if (!image || image->is_pointer || !image_type || image_type->kind != GPU_IR_SHADER_TYPE_IMAGE ||
        image_type->image_dimension != 1 || image_type->image_depth ||
        image_type->image_multisampled || (image_type->image_sampled != 1 && image_type->image_sampled != 2) ||
        !result_type || result_type->kind != GPU_IR_SHADER_TYPE_INT || result_type->bit_width != 32)
        return GPU_IR_UNSUPPORTED;
    const char *scalar = result_type->signedness ? "int" : "uint";
    const char *zero = result_type->signedness ? "0" : "0u";
    int indexed = image->descriptor_index_expression || image->secondary_descriptor_index_expression;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK && indexed) rc = builder_appendf(body, "%s;\n    if ", zero);
    if (rc == GPU_IR_OK && indexed) rc = emit_descriptor_bounds_condition(body, image);
    if (rc == GPU_IR_OK && indexed)
        rc = builder_appendf(body, " vx_value_%u = ", instruction->result_id);
    if (rc == GPU_IR_OK)
        rc = builder_appendf(body, "%s(%s.get_num_mip_levels());\n", scalar, image->expression);
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_image_query_array_size(const gpu_ir_shader *shader,
                                       const gpu_ir_shader_instruction *instruction,
                                       const msl_symbols *symbols, msl_builder *body,
                                       msl_symbols *mutable_symbols) {
    uint32_t image_id;
    if (instruction->operation != GPU_IR_SHADER_IMAGE_QUERY_ARRAY_SIZE || instruction->operand_count != 1 ||
        instruction->literal_word_count || !instruction_id_operand(shader, instruction, 0, &image_id))
        return GPU_IR_INVALID;
    const msl_symbol *image = symbols_find(symbols, image_id);
    const gpu_ir_shader_type *image_type = image ? shader_type(shader, image->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    if (!image || image->is_pointer || !image_type || image_type->kind != GPU_IR_SHADER_TYPE_IMAGE ||
        image_type->image_dimension != 1 || !image_type->image_arrayed || image_type->image_depth ||
        image_type->image_multisampled || image_type->image_sampled != 1 ||
        !result_type || result_type->kind != GPU_IR_SHADER_TYPE_INT || result_type->bit_width != 32)
        return GPU_IR_UNSUPPORTED;
    const char *scalar = result_type->signedness ? "int" : "uint";
    const char *zero = result_type->signedness ? "0" : "0u";
    int indexed = image->descriptor_index_expression || image->secondary_descriptor_index_expression;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK && indexed) rc = builder_appendf(body, "%s;\n    if ", zero);
    if (rc == GPU_IR_OK && indexed) rc = emit_descriptor_bounds_condition(body, image);
    if (rc == GPU_IR_OK && indexed) rc = builder_appendf(body, " vx_value_%u = ", instruction->result_id);
    if (rc == GPU_IR_OK)
        rc = builder_appendf(body, "%s(%s.get_array_size());\n", scalar, image->expression);
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_image_query_samples(const gpu_ir_shader *shader,
                                    const gpu_ir_shader_instruction *instruction,
                                    const msl_symbols *symbols, msl_builder *body,
                                    msl_symbols *mutable_symbols) {
    uint32_t image_id;
    if (instruction->operation != GPU_IR_SHADER_IMAGE_QUERY_SAMPLES || instruction->operand_count != 1 ||
        instruction->literal_word_count || !instruction_id_operand(shader, instruction, 0, &image_id))
        return GPU_IR_INVALID;
    const msl_symbol *image = symbols_find(symbols, image_id);
    const gpu_ir_shader_type *image_type = image ? shader_type(shader, image->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    if (!image || image->is_pointer || !image_type || image_type->kind != GPU_IR_SHADER_TYPE_IMAGE ||
        image_type->image_dimension != 1 || image_type->image_depth || image_type->image_arrayed > 1 ||
        image_type->image_multisampled != 1 || image_type->image_sampled != 1 ||
        !result_type || result_type->kind != GPU_IR_SHADER_TYPE_INT || result_type->bit_width != 32)
        return GPU_IR_UNSUPPORTED;
    const char *scalar = result_type->signedness ? "int" : "uint";
    const char *zero = result_type->signedness ? "0" : "0u";
    int indexed = image->descriptor_index_expression || image->secondary_descriptor_index_expression;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK && indexed) rc = builder_appendf(body, "%s;\n    if ", zero);
    if (rc == GPU_IR_OK && indexed) rc = emit_descriptor_bounds_condition(body, image);
    if (rc == GPU_IR_OK && indexed) rc = builder_appendf(body, " vx_value_%u = ", instruction->result_id);
    if (rc == GPU_IR_OK)
        rc = builder_appendf(body, "%s(%s.get_num_samples());\n", scalar, image->expression);
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_image_query(const gpu_ir_shader *shader,
                            const gpu_ir_shader_instruction *instruction,
                            const msl_symbols *symbols, msl_builder *body,
                            msl_symbols *mutable_symbols) {
    if (!instruction) return GPU_IR_INVALID;
    if (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SIZE)
        return emit_image_query_size(shader, instruction, symbols, body, mutable_symbols);
    if (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_LEVELS)
        return emit_image_query_levels(shader, instruction, symbols, body, mutable_symbols);
    if (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_ARRAY_SIZE)
        return emit_image_query_array_size(shader, instruction, symbols, body, mutable_symbols);
    if (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SAMPLES)
        return emit_image_query_samples(shader, instruction, symbols, body, mutable_symbols);
    return GPU_IR_UNSUPPORTED;
}

static int emit_sampled_image(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                              const msl_symbols *symbols, msl_symbols *mutable_symbols) {
    uint32_t image_id, sampler_id;
    if (instruction->operand_count != 2 || instruction->literal_word_count ||
        !instruction_id_operand(shader, instruction, 0, &image_id) ||
        !instruction_id_operand(shader, instruction, 1, &sampler_id)) return GPU_IR_INVALID;
    const msl_symbol *image = symbols_find(symbols, image_id);
    const msl_symbol *sampler = symbols_find(symbols, sampler_id);
    const gpu_ir_shader_type *image_type = image ? shader_type(shader, image->type_id) : NULL;
    const gpu_ir_shader_type *sampler_type = sampler ? shader_type(shader, sampler->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    if (!image || image->is_pointer || !image_type || image_type->kind != GPU_IR_SHADER_TYPE_IMAGE ||
        image_type->image_dimension != 1 || image_type->image_sampled != 1 ||
        !sampler || sampler->is_pointer || !sampler_type || sampler_type->kind != GPU_IR_SHADER_TYPE_SAMPLER ||
        !result_type || result_type->kind != GPU_IR_SHADER_TYPE_SAMPLED_IMAGE ||
        result_type->element_type_id != image_type->id)
        return GPU_IR_UNSUPPORTED;
    int rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id,
                         0, image->expression);
    if (rc == GPU_IR_OK) rc = symbols_set_aux_expression(mutable_symbols, instruction->result_id,
                                                        sampler->expression);
    if (rc == GPU_IR_OK) rc = symbols_set_image_access(mutable_symbols, instruction->result_id, 1, 0);
    if (rc == GPU_IR_OK && (image->descriptor_index_expression ||
                            image->secondary_descriptor_index_expression))
        rc = symbols_copy_descriptor_indices(mutable_symbols, instruction->result_id, image);
    if (rc == GPU_IR_OK && (sampler->descriptor_index_expression ||
                            sampler->secondary_descriptor_index_expression))
        rc = symbols_copy_descriptor_indices(mutable_symbols, instruction->result_id, sampler);
    return rc;
}

static int emit_image_sample(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                             const msl_symbols *symbols, msl_builder *body,
                             msl_symbols *mutable_symbols) {
    int implicit_lod = instruction->operation == GPU_IR_SHADER_IMAGE_SAMPLE_IMPLICIT_LOD;
    if ((instruction->operation != GPU_IR_SHADER_IMAGE_SAMPLE_EXPLICIT_LOD && !implicit_lod) ||
        instruction->operand_count != (implicit_lod ? 2u : 3u) || instruction->literal_word_count)
        return GPU_IR_INVALID;
    uint32_t sampled_id, coordinate_id, lod_id = 0;
    if (!instruction_id_operand(shader, instruction, 0, &sampled_id) ||
        !instruction_id_operand(shader, instruction, 1, &coordinate_id) ||
        (!implicit_lod && !instruction_id_operand(shader, instruction, 2, &lod_id))) return GPU_IR_INVALID;
    const msl_symbol *sampled = symbols_find(symbols, sampled_id);
    const msl_symbol *coordinate = symbols_find(symbols, coordinate_id);
    const msl_symbol *lod = implicit_lod ? NULL : symbols_find(symbols, lod_id);
    const gpu_ir_shader_type *sampled_type = sampled ? shader_type(shader, sampled->type_id) : NULL;
    const gpu_ir_shader_type *image_type = sampled_type && sampled_type->kind == GPU_IR_SHADER_TYPE_SAMPLED_IMAGE ?
        shader_type(shader, sampled_type->element_type_id) : NULL;
    const gpu_ir_shader_type *coordinate_type = coordinate ? shader_type(shader, coordinate->type_id) : NULL;
    const gpu_ir_shader_type *coordinate_component = coordinate_type &&
        coordinate_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, coordinate_type->element_type_id) : NULL;
    const gpu_ir_shader_type *lod_type = lod ? shader_type(shader, lod->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    const gpu_ir_shader_type *result_component = result_type && result_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, result_type->element_type_id) : NULL;
    const gpu_ir_shader_type *image_component = image_type && image_type->kind == GPU_IR_SHADER_TYPE_IMAGE ?
        shader_type(shader, image_type->element_type_id) : NULL;
    if (!sampled || sampled->is_pointer || !sampled->aux_expression || !sampled_type ||
        sampled_type->kind != GPU_IR_SHADER_TYPE_SAMPLED_IMAGE || !image_type ||
        image_type->kind != GPU_IR_SHADER_TYPE_IMAGE || image_type->image_dimension != 1 ||
        image_type->image_depth || image_type->image_arrayed || image_type->image_multisampled ||
        image_type->image_sampled != 1 || !image_component || image_component->kind != GPU_IR_SHADER_TYPE_FLOAT ||
        image_component->bit_width != 32 || !coordinate || coordinate->is_pointer || !coordinate_type ||
        coordinate_type->kind != GPU_IR_SHADER_TYPE_VECTOR || coordinate_type->element_count != 2 ||
        !coordinate_component || coordinate_component->kind != GPU_IR_SHADER_TYPE_FLOAT ||
        coordinate_component->bit_width != 32 ||
        (!implicit_lod && (!lod || lod->is_pointer || !lod_type ||
         lod_type->kind != GPU_IR_SHADER_TYPE_FLOAT || lod_type->bit_width != 32)) ||
        (implicit_lod && gpu_ir_shader_stage(shader) != GPU_IR_STAGE_FRAGMENT) ||
        !result_type || result_type->kind != GPU_IR_SHADER_TYPE_VECTOR || result_type->element_count != 4 ||
        !result_component || result_component->kind != GPU_IR_SHADER_TYPE_FLOAT ||
        result_component->bit_width != 32 || result_component->id != image_component->id)
        return GPU_IR_UNSUPPORTED;
    int rc = GPU_IR_OK;
    // Materialize derivatives before any nonuniform descriptor-bounds branch so helper invocations
    // contribute the same gradients as the SPIR-V implicit-LOD operation.
    if (implicit_lod)
        rc = builder_appendf(body, "    float2 vx_sample_dx_%u = dfdx(%s);\n"
                             "    float2 vx_sample_dy_%u = dfdy(%s);\n",
                             instruction->result_id, coordinate->expression,
                             instruction->result_id, coordinate->expression);
    if (sampled->descriptor_index_expression || sampled->secondary_descriptor_index_expression) {
        if (rc == GPU_IR_OK) rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "float4(0.0f);\n    if ");
        if (rc == GPU_IR_OK) rc = emit_descriptor_bounds_condition(body, sampled);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, " vx_value_%u = ", instruction->result_id);
        if (rc == GPU_IR_OK && implicit_lod)
            rc = builder_appendf(body, "%s.sample(%s, %s, gradient2d(vx_sample_dx_%u, vx_sample_dy_%u));\n",
                                 sampled->expression, sampled->aux_expression, coordinate->expression,
                                 instruction->result_id, instruction->result_id);
        else if (rc == GPU_IR_OK)
            rc = builder_appendf(body, "%s.sample(%s, %s, level(%s));\n",
                                 sampled->expression, sampled->aux_expression,
                                 coordinate->expression, lod->expression);
    } else {
        if (rc == GPU_IR_OK) rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK && implicit_lod)
            rc = builder_appendf(body, "%s.sample(%s, %s, gradient2d(vx_sample_dx_%u, vx_sample_dy_%u));\n",
                                 sampled->expression, sampled->aux_expression, coordinate->expression,
                                 instruction->result_id, instruction->result_id);
        else if (rc == GPU_IR_OK)
            rc = builder_appendf(body, "%s.sample(%s, %s, level(%s));\n",
                                 sampled->expression, sampled->aux_expression,
                                 coordinate->expression, lod->expression);
    }
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_image_write(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                            const msl_symbols *symbols, msl_builder *body) {
    uint32_t image_id, coordinate_id, texel_id;
    if (instruction->operand_count != 3 || instruction->literal_word_count ||
        !instruction_id_operand(shader, instruction, 0, &image_id) ||
        !instruction_id_operand(shader, instruction, 1, &coordinate_id) ||
        !instruction_id_operand(shader, instruction, 2, &texel_id)) return GPU_IR_INVALID;
    const msl_symbol *image = symbols_find(symbols, image_id);
    const msl_symbol *coordinate = symbols_find(symbols, coordinate_id);
    const msl_symbol *texel = symbols_find(symbols, texel_id);
    const gpu_ir_shader_type *image_type = image ? shader_type(shader, image->type_id) : NULL;
    const gpu_ir_shader_type *coordinate_type = coordinate ? shader_type(shader, coordinate->type_id) : NULL;
    const gpu_ir_shader_type *coordinate_component = coordinate_type &&
        coordinate_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, coordinate_type->element_type_id) : NULL;
    const gpu_ir_shader_type *texel_type = texel ? shader_type(shader, texel->type_id) : NULL;
    const gpu_ir_shader_type *texel_component = texel_type && texel_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, texel_type->element_type_id) : NULL;
    if (!image || image->is_pointer || !image->image_writable || !image_type ||
        image_type->kind != GPU_IR_SHADER_TYPE_IMAGE || image_type->image_format != 4 ||
        !coordinate || coordinate->is_pointer || !coordinate_type ||
        coordinate_type->kind != GPU_IR_SHADER_TYPE_VECTOR || coordinate_type->element_count != 2 ||
        !coordinate_component || coordinate_component->kind != GPU_IR_SHADER_TYPE_INT ||
        coordinate_component->bit_width != 32 || !texel || texel->is_pointer || !texel_type ||
        texel_type->kind != GPU_IR_SHADER_TYPE_VECTOR || texel_type->element_count != 4 ||
        !texel_component || texel_component->kind != GPU_IR_SHADER_TYPE_FLOAT || texel_component->bit_width != 32)
        return GPU_IR_UNSUPPORTED;
    if (image->descriptor_index_expression || image->secondary_descriptor_index_expression) {
        int rc = builder_appendf(body, "    if ");
        if (rc == GPU_IR_OK) rc = emit_descriptor_bounds_condition(body, image);
        if (rc == GPU_IR_OK)
            rc = builder_appendf(body, " %s.write(%s, uint2(%s));\n",
                image->expression, texel->expression, coordinate->expression);
        return rc;
    }
    return builder_appendf(body, "    %s.write(%s, uint2(%s));\n",
                           image->expression, texel->expression, coordinate->expression);
}

static int emit_store(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                      const msl_symbols *symbols, msl_builder *body) {
    uint32_t pointer_id, value_id;
    if (instruction->operand_count != 2 || !instruction_id_operand(shader, instruction, 0, &pointer_id) ||
        !instruction_id_operand(shader, instruction, 1, &value_id) || instruction->memory_access_flags)
        return GPU_IR_INVALID;
    const msl_symbol *pointer_symbol = symbols_find(symbols, pointer_id);
    const msl_symbol *value = symbols_find(symbols, value_id);
    const gpu_ir_shader_type *pointer = pointer_symbol ? shader_type(shader, pointer_symbol->type_id) : NULL;
    if (!pointer_symbol || !pointer_symbol->is_pointer || !pointer || pointer->kind != GPU_IR_SHADER_TYPE_POINTER ||
        !value || value->is_pointer || pointer->element_type_id != value->type_id) return GPU_IR_UNSUPPORTED;
    if (pointer->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM ||
        pointer->storage_class == GPU_IR_SHADER_STORAGE_PUSH_CONSTANT) return GPU_IR_UNSUPPORTED;
    if (pointer_symbol->descriptor_slot != UINT32_MAX) {
        if (!pointer_symbol->buffer_writable) return GPU_IR_UNSUPPORTED;
        if (!shader_type_is_32bit_buffer_scalar(shader, pointer->element_type_id) ||
            !pointer_symbol->buffer_index_expression ||
            !shader_type_is_32bit_buffer_scalar(shader, value->type_id))
            return GPU_IR_UNSUPPORTED;
        int rc = builder_appendf(body, "    if (");
        if (rc == GPU_IR_OK) rc = emit_buffer_bounds_condition(body, pointer_symbol);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, ") { %s = %s; }\n",
                                                 pointer_symbol->expression, value->expression);
        return rc;
    }
    return builder_appendf(body, "    %s = %s;\n", pointer_symbol->expression, value->expression);
}

static int emit_atomic(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                       const msl_symbols *symbols, msl_builder *body, msl_symbols *mutable_symbols) {
    uint32_t pointer_id, scope_id, semantics_id, failure_semantics_id = 0;
    uint32_t compare_id = 0, value_id = 0, scope, semantics, failure_semantics = 0;
    int atomic_release_before = 0, atomic_acquire_after = 0;
    int compare_exchange = instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_COMPARE_EXCHANGE;
    int increment_decrement = instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_INCREMENT ||
                              instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_DECREMENT;
    int atomic_load = instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_LOAD;
    int atomic_store = instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_STORE;
    int has_value = !increment_decrement && !atomic_load;
    if ((instruction->source_api != GPU_IR_SHADER_SOURCE_SPIRV &&
         instruction->source_api != GPU_IR_SHADER_SOURCE_DXIL) ||
        instruction->operation < GPU_IR_SHADER_ATOMIC_INTEGER_ADD ||
        instruction->operation > GPU_IR_SHADER_ATOMIC_INTEGER_STORE || instruction->flags ||
        (atomic_store ? (instruction->result_id || instruction->result_type_id) :
                        (!instruction->result_id || !instruction->result_type_id)) ||
        instruction->operand_count != (compare_exchange ? 6u :
            increment_decrement || atomic_load ? 3u : 4u) ||
        instruction->literal_word_count ||
        !instruction_id_operand(shader, instruction, 0, &pointer_id) ||
        !instruction_id_operand(shader, instruction, 1, &scope_id) ||
        !instruction_id_operand(shader, instruction, 2, &semantics_id) ||
        (compare_exchange && (!instruction_id_operand(shader, instruction, 3, &failure_semantics_id) ||
                              !instruction_id_operand(shader, instruction, 4, &compare_id))) ||
        (has_value &&
         !instruction_id_operand(shader, instruction, compare_exchange ? 5u : 3u, &value_id)))
        return GPU_IR_UNSUPPORTED;
    if (!constant_u32(shader, scope_id, &scope) || !constant_u32(shader, semantics_id, &semantics))
        return GPU_IR_INVALID;
    if (compare_exchange && !constant_u32(shader, failure_semantics_id, &failure_semantics))
        return GPU_IR_INVALID;
    if (scope != 1u) return GPU_IR_UNSUPPORTED;
    if (instruction->source_api != GPU_IR_SHADER_SOURCE_SPIRV) {
        if (semantics || failure_semantics) return GPU_IR_UNSUPPORTED;
    } else {
        uint32_t order = semantics & 0x1eu;
        if ((semantics && semantics != (0x40u | order)) || (failure_semantics &&
            failure_semantics != (0x40u | (failure_semantics & 0x1eu)))) return GPU_IR_UNSUPPORTED;
        if (atomic_load) {
            if (semantics != 0u && semantics != 0x42u) return GPU_IR_UNSUPPORTED;
        } else if (atomic_store) {
            if (semantics != 0u && semantics != 0x44u) return GPU_IR_UNSUPPORTED;
        } else if (compare_exchange) {
            if ((semantics != 0u && semantics != 0x42u && semantics != 0x44u && semantics != 0x48u) ||
                (failure_semantics != 0u && failure_semantics != 0x42u) ||
                (failure_semantics == 0x42u && semantics != 0x42u && semantics != 0x48u))
                return GPU_IR_UNSUPPORTED;
        } else if (semantics != 0u && semantics != 0x42u && semantics != 0x44u && semantics != 0x48u) {
            return GPU_IR_UNSUPPORTED;
        }
        uint32_t failure_order = failure_semantics & 0x1eu;
        atomic_release_before = order == 0x4u || order == 0x8u;
        atomic_acquire_after = order == 0x2u || order == 0x8u || failure_order == 0x2u;
    }
    const msl_symbol *pointer_symbol = symbols_find(symbols, pointer_id);
    const msl_symbol *compare = compare_exchange ? symbols_find(symbols, compare_id) : NULL;
    const msl_symbol *value = has_value ? symbols_find(symbols, value_id) : NULL;
    const gpu_ir_shader_type *pointer = pointer_symbol ? shader_type(shader, pointer_symbol->type_id) : NULL;
    if (!pointer_symbol || !pointer_symbol->is_pointer || !pointer ||
        pointer->kind != GPU_IR_SHADER_TYPE_POINTER ||
        pointer->storage_class != GPU_IR_SHADER_STORAGE_BUFFER)
        return GPU_IR_UNSUPPORTED;
    uint32_t atomic_type_id = atomic_store ? pointer->element_type_id : instruction->result_type_id;
    const gpu_ir_shader_type *type = shader_type(shader, atomic_type_id);
    if (pointer->element_type_id != atomic_type_id || pointer_symbol->descriptor_slot == UINT32_MAX ||
        (!atomic_load && !pointer_symbol->buffer_writable) || !pointer_symbol->buffer_index_expression ||
        (has_value && (!value || value->is_pointer)) ||
        (compare_exchange && (!compare || compare->is_pointer ||
            compare->type_id != atomic_type_id)) ||
        (has_value && value->type_id != atomic_type_id) ||
        !type || type->kind != GPU_IR_SHADER_TYPE_INT ||
        type->bit_width != 32 || type->signedness > 1)
        return GPU_IR_UNSUPPORTED;
    int atomic_signed = type->signedness != 0;
    const char *atomic_function = NULL;
    switch (instruction->operation) {
    case GPU_IR_SHADER_ATOMIC_INTEGER_ADD: atomic_function = "atomic_fetch_add_explicit"; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_SUBTRACT: atomic_function = "atomic_fetch_sub_explicit"; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_AND: atomic_function = "atomic_fetch_and_explicit"; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_OR: atomic_function = "atomic_fetch_or_explicit"; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_XOR: atomic_function = "atomic_fetch_xor_explicit"; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MINIMUM:
        atomic_function = "atomic_fetch_min_explicit"; atomic_signed = 1; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MAXIMUM:
        atomic_function = "atomic_fetch_max_explicit"; atomic_signed = 1; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MINIMUM:
        atomic_function = "atomic_fetch_min_explicit"; atomic_signed = 0; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MAXIMUM:
        atomic_function = "atomic_fetch_max_explicit"; atomic_signed = 0; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_EXCHANGE: atomic_function = "atomic_exchange_explicit"; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_COMPARE_EXCHANGE:
        atomic_function = "atomic_compare_exchange_weak_explicit"; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_INCREMENT:
        atomic_function = "atomic_fetch_add_explicit"; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_DECREMENT:
        atomic_function = "atomic_fetch_sub_explicit"; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_LOAD: atomic_function = "atomic_load_explicit"; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_STORE: atomic_function = "atomic_store_explicit"; break;
    default: return GPU_IR_UNSUPPORTED;
    }
    const char *atomic_type = atomic_signed ? "atomic_int" : "atomic_uint";
    const char *atomic_value_type = atomic_signed ? "int" : "uint";
    char type_name[32];
    if (!shader_type_name(shader, atomic_type_id, type_name, sizeof type_name))
        return GPU_IR_UNSUPPORTED;
    int rc = GPU_IR_OK;
    if (!atomic_store && !body->assign_values)
        rc = builder_appendf(body, "    %s vx_value_%u = 0;\n", type_name, instruction->result_id);
    if (rc == GPU_IR_OK && compare_exchange)
        rc = builder_appendf(body, "    %s vx_compare_%u = as_type<%s>(%s);\n"
                             "    %s vx_expected_%u = vx_compare_%u;\n", atomic_value_type,
                             instruction->result_id, atomic_value_type, compare->expression, atomic_value_type,
                             instruction->result_id, instruction->result_id);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, "    if (");
    if (rc == GPU_IR_OK) rc = emit_buffer_bounds_condition(body, pointer_symbol);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, ") {\n");
    if (rc == GPU_IR_OK && atomic_release_before)
        rc = builder_appendf(body,
            "        atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);\n");
    if (rc == GPU_IR_OK && compare_exchange) rc = builder_appendf(body,
        "        do { %s vx_observed_%u = vx_expected_%u; "
        "if (%s(reinterpret_cast<device %s*>(&%s), &vx_observed_%u, "
        "as_type<%s>(%s), memory_order_relaxed, memory_order_relaxed)) break; "
        "vx_expected_%u = vx_observed_%u; } while (vx_expected_%u == vx_compare_%u);\n"
        "        vx_value_%u = as_type<%s>(vx_expected_%u);\n", atomic_value_type, instruction->result_id,
        instruction->result_id, atomic_function, atomic_type, pointer_symbol->expression,
        instruction->result_id, atomic_value_type, value->expression, instruction->result_id,
        instruction->result_id, instruction->result_id, instruction->result_id, instruction->result_id,
        type_name, instruction->result_id);
    if (rc == GPU_IR_OK && !compare_exchange && increment_decrement) rc = builder_appendf(body,
        "        vx_value_%u = as_type<%s>(%s(reinterpret_cast<device %s*>(&%s), %s, "
        "memory_order_relaxed));\n", instruction->result_id, type_name, atomic_function, atomic_type,
        pointer_symbol->expression, atomic_signed ? "1" : "1u");
    if (rc == GPU_IR_OK && atomic_load) rc = builder_appendf(body,
        "        vx_value_%u = as_type<%s>(%s(reinterpret_cast<device const %s*>(&%s), "
        "memory_order_relaxed));\n", instruction->result_id, type_name, atomic_function, atomic_type,
        pointer_symbol->expression);
    if (rc == GPU_IR_OK && atomic_store) rc = builder_appendf(body,
        "        %s(reinterpret_cast<device %s*>(&%s), as_type<%s>(%s), memory_order_relaxed);\n",
        atomic_function, atomic_type, pointer_symbol->expression, atomic_value_type, value->expression);
    if (rc == GPU_IR_OK && !compare_exchange && !increment_decrement && !atomic_load && !atomic_store)
        rc = builder_appendf(body,
        "        vx_value_%u = as_type<%s>(%s(reinterpret_cast<device %s*>(&%s), "
        "as_type<%s>(%s), memory_order_relaxed));\n", instruction->result_id, type_name,
        atomic_function, atomic_type, pointer_symbol->expression, atomic_value_type, value->expression);
    if (rc == GPU_IR_OK && atomic_acquire_after)
        rc = builder_appendf(body,
            "        atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);\n");
    if (rc == GPU_IR_OK) rc = builder_appendf(body, "    }\n");
    if (rc == GPU_IR_OK && !atomic_store) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_bit_find(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                         const msl_symbol *input, const gpu_ir_shader_type *result_scalar,
                         msl_builder *body, msl_symbols *mutable_symbols) {
    const gpu_ir_shader_type *input_type = shader_type(shader, input->type_id);
    const gpu_ir_shader_type *input_scalar = input_type && input_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, input_type->element_type_id) : input_type;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    uint32_t component_count = input_type && input_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        input_type->element_count : 1;
    int signed_msb = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED;
    int lsb = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIND_LSB;
    if (!input_type || !input_scalar || input_scalar->kind != GPU_IR_SHADER_TYPE_INT ||
        input_scalar->bit_width != 32 || !result_type || result_scalar->kind != GPU_IR_SHADER_TYPE_INT ||
        result_scalar->bit_width != 32 || component_count < 1 || component_count > 4 ||
        (input_type->kind == GPU_IR_SHADER_TYPE_VECTOR) !=
            (result_type->kind == GPU_IR_SHADER_TYPE_VECTOR) ||
        (result_type->kind == GPU_IR_SHADER_TYPE_VECTOR && result_type->element_count != component_count))
        return GPU_IR_UNSUPPORTED;

    char *normalized_input = integer_semantic_expression(shader, input->type_id,
        signed_msb ? 1u : 0u, input->expression);
    char result_type_name[32];
    char result_scalar_type_name[32];
    const gpu_ir_shader_type *result_element = result_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, result_type->element_type_id) : result_type;
    if (!normalized_input || !result_element ||
        !shader_type_name(shader, instruction->result_type_id, result_type_name, sizeof result_type_name) ||
        !shader_type_name(shader, result_element->id, result_scalar_type_name, sizeof result_scalar_type_name)) {
        free(normalized_input);
        return GPU_IR_UNSUPPORTED;
    }

    static const char *components[] = { "x", "y", "z", "w" };
    msl_builder value = { 0 };
    int rc = GPU_IR_OK;
    for (uint32_t i = 0; rc == GPU_IR_OK && i < component_count; i++) {
        char component_expression[256];
        int length = input_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
            snprintf(component_expression, sizeof component_expression, "(%s).%s",
                normalized_input, components[i]) :
            snprintf(component_expression, sizeof component_expression, "%s", normalized_input);
        if (length < 0 || (size_t)length >= sizeof component_expression) { rc = GPU_IR_RANGE; break; }
        if (signed_msb) {
            rc = builder_appendf(body,
                "    int vx_find_signed_%u_%u = %s;\n"
                "    uint vx_find_bits_%u_%u = vx_find_signed_%u_%u < 0 ? ~uint(vx_find_signed_%u_%u) : uint(vx_find_signed_%u_%u);\n",
                instruction->result_id, i, component_expression,
                instruction->result_id, i, instruction->result_id, i,
                instruction->result_id, i, instruction->result_id, i);
        } else {
            rc = builder_appendf(body, "    uint vx_find_bits_%u_%u = uint(%s);\n",
                instruction->result_id, i, component_expression);
        }
        if (rc == GPU_IR_OK) rc = builder_appendf(body,
            lsb ?
                "    uint vx_find_result_%u_%u = vx_find_bits_%u_%u == 0u ? 0xffffffffu : uint(ctz(vx_find_bits_%u_%u));\n" :
                "    uint vx_find_result_%u_%u = vx_find_bits_%u_%u == 0u ? 0xffffffffu : 31u - uint(clz(vx_find_bits_%u_%u));\n",
            instruction->result_id, i, instruction->result_id, i, instruction->result_id, i);
        if (rc == GPU_IR_OK && component_count > 1) {
            if (i) rc = builder_appendf(&value, ", ");
            if (rc == GPU_IR_OK && result_element->signedness)
                rc = builder_appendf(&value, "as_type<int>(vx_find_result_%u_%u)", instruction->result_id, i);
            else if (rc == GPU_IR_OK)
                rc = builder_appendf(&value, "vx_find_result_%u_%u", instruction->result_id, i);
        } else if (rc == GPU_IR_OK && result_element->signedness) {
            rc = builder_appendf(&value, "as_type<int>(vx_find_result_%u_%u)", instruction->result_id, i);
        } else if (rc == GPU_IR_OK) {
            rc = builder_appendf(&value, "vx_find_result_%u_%u", instruction->result_id, i);
        }
    }
    free(normalized_input);
    if (rc == GPU_IR_OK && component_count > 1) {
        msl_builder constructed = { 0 };
        rc = builder_appendf(&constructed, "%s(%s)", result_type_name, value.data ? value.data : "");
        free(value.data);
        value = constructed;
    }
    if (rc == GPU_IR_OK) rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, "%s;\n", value.data ? value.data : "");
    free(value.data);
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_ordered_float_minmax(const gpu_ir_shader *shader,
                                     const gpu_ir_shader_instruction *instruction,
                                     const msl_symbol *left, const msl_symbol *right,
                                     msl_builder *body, msl_symbols *mutable_symbols) {
    static const char *const components[] = { "x", "y", "z", "w" };
    const uint32_t required_flags = GPU_IR_SHADER_FLAG_MINMAX_ORDERED_SELECT |
        GPU_IR_SHADER_FLAG_MINMAX_IGNORE_SINGLE_NAN | GPU_IR_SHADER_FLAG_MINMAX_FLUSH_DENORMS;
    const gpu_ir_shader_type *type = shader_type(shader, instruction->result_type_id);
    uint32_t lanes = 1;
    if (!type || type->kind == GPU_IR_SHADER_TYPE_POINTER || !left || !right || left->is_pointer ||
        right->is_pointer || left->type_id != instruction->result_type_id ||
        right->type_id != instruction->result_type_id ||
        (instruction->flags & required_flags) != required_flags ||
        (instruction->flags & GPU_IR_SHADER_FLAG_FLOAT) == 0 ||
        (instruction->flags & GPU_IR_SHADER_FLAG_FAST_MATH) ||
        (instruction->operation != GPU_IR_SHADER_ARITH_MINIMUM &&
         instruction->operation != GPU_IR_SHADER_ARITH_MAXIMUM)) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *scalar = type;
    if (type->kind == GPU_IR_SHADER_TYPE_VECTOR) {
        lanes = type->element_count;
        scalar = shader_type(shader, type->element_type_id);
    }
    if (!scalar || scalar->kind != GPU_IR_SHADER_TYPE_FLOAT || scalar->bit_width != 32 ||
        lanes < 1 || lanes > 4) return GPU_IR_UNSUPPORTED;

    const char *comparison = instruction->operation == GPU_IR_SHADER_ARITH_MINIMUM ? "<" : ">=";
    char type_name[32];
    if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name))
        return GPU_IR_UNSUPPORTED;
    int rc = GPU_IR_OK;
    for (uint32_t lane = 0; rc == GPU_IR_OK && lane < lanes; lane++) {
        const char *suffix = lanes == 1 ? "" : components[lane];
        char bits_a[48], bits_b[48], compare_a[48], compare_b[48];
        snprintf(bits_a, sizeof bits_a, "vx_minmax_bits_a_%u_%u", instruction->result_id, lane);
        snprintf(bits_b, sizeof bits_b, "vx_minmax_bits_b_%u_%u", instruction->result_id, lane);
        snprintf(compare_a, sizeof compare_a, "vx_minmax_cmp_a_%u_%u", instruction->result_id, lane);
        snprintf(compare_b, sizeof compare_b, "vx_minmax_cmp_b_%u_%u", instruction->result_id, lane);
        rc = builder_appendf(body, "    uint %s = as_type<uint>(%s%s);\n"
            "    uint %s = as_type<uint>(%s%s);\n"
            "    float %s = ((%s & 0x7f800000u) == 0u && (%s & 0x007fffffu) != 0u) "
            "? as_type<float>(%s & 0x80000000u) : as_type<float>(%s);\n"
            "    float %s = ((%s & 0x7f800000u) == 0u && (%s & 0x007fffffu) != 0u) "
            "? as_type<float>(%s & 0x80000000u) : as_type<float>(%s);\n",
            bits_a, left->expression, suffix, bits_b, right->expression, suffix,
            compare_a, bits_a, bits_a, bits_a, bits_a,
            compare_b, bits_b, bits_b, bits_b, bits_b);
    }
    if (rc == GPU_IR_OK) rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK && lanes > 1) rc = builder_appendf(body, "%s(", type_name);
    for (uint32_t lane = 0; rc == GPU_IR_OK && lane < lanes; lane++) {
        const char *suffix = lanes == 1 ? "" : components[lane];
        char compare_a[48], compare_b[48];
        snprintf(compare_a, sizeof compare_a, "vx_minmax_cmp_a_%u_%u", instruction->result_id, lane);
        snprintf(compare_b, sizeof compare_b, "vx_minmax_cmp_b_%u_%u", instruction->result_id, lane);
        rc = builder_appendf(body,
            "%s(isnan(%s%s) ? %s%s : (isnan(%s%s) ? %s%s : (%s %s %s ? %s%s : %s%s)))",
            lane ? ", " : "", left->expression, suffix, right->expression, suffix,
            right->expression, suffix, left->expression, suffix,
            compare_a, comparison, compare_b, left->expression, suffix, right->expression, suffix);
    }
    if (rc == GPU_IR_OK) rc = builder_appendf(body, lanes > 1 ? ");\n" : ";\n");
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_arithmetic(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                           const msl_symbols *symbols, msl_builder *body, msl_symbols *mutable_symbols) {
    uint32_t left_id, right_id = 0, third_id = 0, fourth_id = 0;
    int vector_times_scalar = instruction->operation == GPU_IR_SHADER_ARITH_VECTOR_TIMES_SCALAR;
    int dot_operation = instruction->operation == GPU_IR_SHADER_ARITH_DOT;
    int reduction = instruction->operation == GPU_IR_SHADER_ARITH_ANY ||
                    instruction->operation == GPU_IR_SHADER_ARITH_ALL;
    int subgroup_vote = instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_ANY ||
                        instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_ALL;
    int subgroup_integer_reduction = instruction->operation >= GPU_IR_SHADER_ARITH_SUBGROUP_SMIN &&
                                     instruction->operation <= GPU_IR_SHADER_ARITH_SUBGROUP_BIT_XOR;
    int bit_count = instruction->operation == GPU_IR_SHADER_ARITH_BIT_COUNT;
    int bit_reverse = instruction->operation == GPU_IR_SHADER_ARITH_BIT_REVERSE;
    int bit_find_lsb = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIND_LSB;
    int bit_find_msb_unsigned = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED;
    int bit_find_msb_signed = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED;
    int bit_find = bit_find_lsb || bit_find_msb_unsigned || bit_find_msb_signed;
    int bit_field_insert = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT;
    int bit_field_signed_extract = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT;
    int bit_field_unsigned_extract = instruction->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT;
    int bit_field_extract = bit_field_signed_extract || bit_field_unsigned_extract;
    int dxil_bitfield_low5 = (instruction->flags & GPU_IR_SHADER_FLAG_DXIL_BITFIELD_LOW5) != 0;
    int integer_mad = instruction->operation == GPU_IR_SHADER_ARITH_INTEGER_MAD;
    int float_mad = instruction->operation == GPU_IR_SHADER_ARITH_FLOAT_MAD;
    int interpolate = instruction->operation == GPU_IR_SHADER_ARITH_INTERPOLATE;
    int ternary = instruction->operation == GPU_IR_SHADER_ARITH_CLAMP ||
                  instruction->operation == GPU_IR_SHADER_ARITH_FMA ||
                  instruction->operation == GPU_IR_SHADER_ARITH_SMOOTHSTEP || integer_mad || float_mad || interpolate;
    int unary = instruction->operation == GPU_IR_SHADER_ARITH_NEGATE ||
                instruction->operation == GPU_IR_SHADER_ARITH_BIT_NOT ||
                instruction->operation == GPU_IR_SHADER_ARITH_BIT_COUNT ||
                instruction->operation == GPU_IR_SHADER_ARITH_BIT_REVERSE ||
                bit_find_lsb || bit_find_msb_unsigned || bit_find_msb_signed ||
                instruction->operation == GPU_IR_SHADER_ARITH_LOGICAL_NOT || reduction ||
                subgroup_vote || subgroup_integer_reduction ||
                instruction->operation == GPU_IR_SHADER_ARITH_SATURATE ||
                instruction->operation == GPU_IR_SHADER_ARITH_ABSOLUTE ||
                instruction->operation == GPU_IR_SHADER_ARITH_FLOOR ||
                instruction->operation == GPU_IR_SHADER_ARITH_CEILING ||
                instruction->operation == GPU_IR_SHADER_ARITH_SQRT ||
                instruction->operation == GPU_IR_SHADER_ARITH_INVERSE_SQRT ||
                instruction->operation == GPU_IR_SHADER_ARITH_TRUNC ||
                instruction->operation == GPU_IR_SHADER_ARITH_FRACT ||
                instruction->operation == GPU_IR_SHADER_ARITH_ROUND ||
                instruction->operation == GPU_IR_SHADER_ARITH_ROUND_EVEN ||
                (instruction->operation >= GPU_IR_SHADER_ARITH_SIN &&
                 instruction->operation <= GPU_IR_SHADER_ARITH_ATANH) ||
                (instruction->operation >= GPU_IR_SHADER_ARITH_EXP &&
                 instruction->operation <= GPU_IR_SHADER_ARITH_LOG2);
    uint32_t expected_operands = bit_field_insert ? 4u : bit_field_extract ? 3u : ternary ? 3u : unary ? 1u : 2u;
    if (dxil_bitfield_low5 && ((!bit_field_insert && !bit_field_extract) ||
        instruction->source_api != GPU_IR_SHADER_SOURCE_DXIL)) return GPU_IR_UNSUPPORTED;
    if (instruction->operand_count != expected_operands ||
        !instruction_id_operand(shader, instruction, 0, &left_id) ||
        (!unary && !instruction_id_operand(shader, instruction, 1, &right_id)) ||
        ((ternary || bit_field_extract || bit_field_insert) &&
         !instruction_id_operand(shader, instruction, 2, &third_id)) ||
        (bit_field_insert && !instruction_id_operand(shader, instruction, 3, &fourth_id)))
        return GPU_IR_UNSUPPORTED;
    const msl_symbol *left = symbols_find(symbols, left_id);
    const msl_symbol *right = unary ? NULL : symbols_find(symbols, right_id);
    const msl_symbol *third = (ternary || bit_field_extract || bit_field_insert) ?
        symbols_find(symbols, third_id) : NULL;
    const msl_symbol *fourth = bit_field_insert ? symbols_find(symbols, fourth_id) : NULL;
    const gpu_ir_shader_type *type = shader_type(shader, instruction->result_type_id);
    int right_matches = bit_field_extract ? right &&
        shader_type(shader, right->type_id) && shader_type(shader, right->type_id)->kind == GPU_IR_SHADER_TYPE_INT :
        bit_field_insert ? right && right->type_id == instruction->result_type_id :
        vector_times_scalar ? right && type && type->kind == GPU_IR_SHADER_TYPE_VECTOR &&
        right->type_id == type->element_type_id : dot_operation ? right && left &&
        right->type_id == left->type_id : right && right->type_id == instruction->result_type_id;
    if (!left || left->is_pointer || (!reduction && !dot_operation && !bit_count && !bit_find &&
        left->type_id != instruction->result_type_id) ||
        (!unary && (!right || right->is_pointer || !right_matches)) ||
        (ternary && (!third || third->is_pointer || third->type_id != instruction->result_type_id)) ||
        (bit_field_extract && (!third || third->is_pointer ||
            !shader_type(shader, third->type_id) || shader_type(shader, third->type_id)->kind != GPU_IR_SHADER_TYPE_INT)) ||
        (bit_field_insert && (!third || third->is_pointer || !fourth || fourth->is_pointer ||
            !shader_type(shader, third->type_id) || shader_type(shader, third->type_id)->kind != GPU_IR_SHADER_TYPE_INT ||
            !shader_type(shader, fourth->type_id) || shader_type(shader, fourth->type_id)->kind != GPU_IR_SHADER_TYPE_INT)))
        return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *scalar = type && type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, type->element_type_id) : type;
    if (!scalar) return GPU_IR_UNSUPPORTED;
    int is_float = scalar->kind == GPU_IR_SHADER_TYPE_FLOAT && scalar->bit_width == 32;
    int is_int = scalar->kind == GPU_IR_SHADER_TYPE_INT && scalar->bit_width == 32;
    int type_is_signed = is_int && scalar->signedness;
    int is_bool = scalar->kind == GPU_IR_SHADER_TYPE_BOOL;
    int is_signed = is_int && (instruction->flags & GPU_IR_SHADER_FLAG_SIGNED);
    int integer_minmax = is_int && (instruction->operation == GPU_IR_SHADER_ARITH_MINIMUM ||
                                    instruction->operation == GPU_IR_SHADER_ARITH_MAXIMUM);
    const uint32_t float_minmax_flags = GPU_IR_SHADER_FLAG_MINMAX_ORDERED_SELECT |
        GPU_IR_SHADER_FLAG_MINMAX_IGNORE_SINGLE_NAN | GPU_IR_SHADER_FLAG_MINMAX_FLUSH_DENORMS;
    int ordered_float_minmax = is_float && (instruction->flags & float_minmax_flags) != 0;
    if (!!(instruction->flags & GPU_IR_SHADER_FLAG_FLOAT) != is_float ||
        ((instruction->flags & GPU_IR_SHADER_FLAG_SIGNED) && !is_int)) return GPU_IR_UNSUPPORTED;

    if (bit_count) {
        const gpu_ir_shader_type *input = shader_type(shader, left->type_id);
        const gpu_ir_shader_type *input_scalar = input && input->kind == GPU_IR_SHADER_TYPE_VECTOR ?
            shader_type(shader, input->element_type_id) : input;
        int same_shape = input && type && input->kind == type->kind &&
            (input->kind != GPU_IR_SHADER_TYPE_VECTOR || input->element_count == type->element_count);
        if (!is_int || !same_shape || !input_scalar || input_scalar->kind != GPU_IR_SHADER_TYPE_INT ||
            input_scalar->bit_width != 32 ||
            (input->kind != GPU_IR_SHADER_TYPE_INT && input->kind != GPU_IR_SHADER_TYPE_VECTOR))
            return GPU_IR_UNSUPPORTED;
    }
    if (bit_find) {
        const gpu_ir_shader_type *input = shader_type(shader, left->type_id);
        const gpu_ir_shader_type *input_scalar = input && input->kind == GPU_IR_SHADER_TYPE_VECTOR ?
            shader_type(shader, input->element_type_id) : input;
        int same_shape = input && type && input->kind == type->kind &&
            (input->kind != GPU_IR_SHADER_TYPE_VECTOR || input->element_count == type->element_count);
        if (!is_int || !same_shape || !input_scalar || input_scalar->kind != GPU_IR_SHADER_TYPE_INT ||
            input_scalar->bit_width != 32 ||
            (input->kind != GPU_IR_SHADER_TYPE_INT && input->kind != GPU_IR_SHADER_TYPE_VECTOR))
            return GPU_IR_UNSUPPORTED;
    }

    char type_name[32];
    if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name)) return GPU_IR_UNSUPPORTED;
    const char *operator = NULL;
    int special_modulo = 0;
    switch (instruction->operation) {
    case GPU_IR_SHADER_ARITH_ADD:
    case GPU_IR_SHADER_ARITH_SUBTRACT:
    case GPU_IR_SHADER_ARITH_MULTIPLY:
    case GPU_IR_SHADER_ARITH_DIVIDE:
        if (!is_float && !is_int) return GPU_IR_UNSUPPORTED;
        operator = instruction->operation == GPU_IR_SHADER_ARITH_ADD ? "+" :
                   instruction->operation == GPU_IR_SHADER_ARITH_SUBTRACT ? "-" :
                   instruction->operation == GPU_IR_SHADER_ARITH_MULTIPLY ? "*" : "/";
        break;
    case GPU_IR_SHADER_ARITH_VECTOR_TIMES_SCALAR:
        if (!is_float || type->kind != GPU_IR_SHADER_TYPE_VECTOR) return GPU_IR_UNSUPPORTED;
        operator = "*";
        break;
    case GPU_IR_SHADER_ARITH_DOT: {
        const gpu_ir_shader_type *vector = left ? shader_type(shader, left->type_id) : NULL;
        if (!is_float || !vector || vector->kind != GPU_IR_SHADER_TYPE_VECTOR ||
            vector->element_type_id != instruction->result_type_id ||
            vector->element_count < 2 || vector->element_count > 4) return GPU_IR_UNSUPPORTED;
        break;
    }
    case GPU_IR_SHADER_ARITH_REMAINDER:
        if (is_float) operator = "fmod(%s, %s)";
        else if (is_int) operator = "%";
        else return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_MODULO:
        if (is_float) operator = "(%s - (%s * floor(%s / %s)))";
        else if (is_signed) special_modulo = 2;
        else return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_NEGATE:
        if (!is_float && !is_signed) return GPU_IR_UNSUPPORTED;
        operator = "-";
        break;
    case GPU_IR_SHADER_ARITH_BIT_NOT:
        if (!is_int) return GPU_IR_UNSUPPORTED;
        operator = "~";
        break;
    case GPU_IR_SHADER_ARITH_BIT_COUNT:
        if (!is_int) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_BIT_REVERSE:
        if (!is_int) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_BIT_FIND_LSB:
    case GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED:
    case GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED:
        if (!is_int) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT:
    case GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT:
    case GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT:
        if (!is_int) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_SATURATE:
        if (!is_float) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_SHIFT_LEFT:
        if (!is_int) return GPU_IR_UNSUPPORTED;
        operator = "<<";
        break;
    case GPU_IR_SHADER_ARITH_SHIFT_RIGHT_LOGICAL:
        if (!is_int) return GPU_IR_UNSUPPORTED;
        operator = ">>";
        break;
    case GPU_IR_SHADER_ARITH_SHIFT_RIGHT_ARITHMETIC:
        if (!is_signed) return GPU_IR_UNSUPPORTED;
        operator = ">>";
        break;
    case GPU_IR_SHADER_ARITH_BIT_AND:
        if (!is_int) return GPU_IR_UNSUPPORTED;
        operator = "&";
        break;
    case GPU_IR_SHADER_ARITH_BIT_OR:
        if (!is_int) return GPU_IR_UNSUPPORTED;
        operator = "|";
        break;
    case GPU_IR_SHADER_ARITH_BIT_XOR:
        if (!is_int) return GPU_IR_UNSUPPORTED;
        operator = "^";
        break;
    case GPU_IR_SHADER_ARITH_LOGICAL_AND:
        if (!is_bool) return GPU_IR_UNSUPPORTED;
        operator = type->kind == GPU_IR_SHADER_TYPE_VECTOR ? "&" : "&&";
        break;
    case GPU_IR_SHADER_ARITH_LOGICAL_OR:
        if (!is_bool) return GPU_IR_UNSUPPORTED;
        operator = type->kind == GPU_IR_SHADER_TYPE_VECTOR ? "|" : "||";
        break;
    case GPU_IR_SHADER_ARITH_LOGICAL_NOT:
        if (!is_bool) return GPU_IR_UNSUPPORTED;
        operator = "!";
        break;
    case GPU_IR_SHADER_ARITH_ANY:
    case GPU_IR_SHADER_ARITH_ALL:
        if (type->kind != GPU_IR_SHADER_TYPE_BOOL || !scalar ||
            !shader_type(shader, left->type_id) ||
            shader_type(shader, left->type_id)->kind != GPU_IR_SHADER_TYPE_VECTOR || !is_bool)
            return GPU_IR_UNSUPPORTED;
        operator = instruction->operation == GPU_IR_SHADER_ARITH_ANY ? "any(%s)" : "all(%s)";
        break;
    case GPU_IR_SHADER_ARITH_SUBGROUP_ANY:
    case GPU_IR_SHADER_ARITH_SUBGROUP_ALL:
        if (!is_bool || type->kind != GPU_IR_SHADER_TYPE_BOOL || !left ||
            left->type_id != instruction->result_type_id) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_SUBGROUP_SMIN:
    case GPU_IR_SHADER_ARITH_SUBGROUP_SMAX:
        if ((!type_is_signed && instruction->source_api != GPU_IR_SHADER_SOURCE_DXIL) ||
            type->kind != GPU_IR_SHADER_TYPE_INT || type->bit_width != 32 || !left ||
            left->type_id != instruction->result_type_id) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_SUBGROUP_UMIN:
    case GPU_IR_SHADER_ARITH_SUBGROUP_UMAX:
        if (!is_int || (is_signed && instruction->source_api != GPU_IR_SHADER_SOURCE_DXIL) ||
            type->kind != GPU_IR_SHADER_TYPE_INT || type->bit_width != 32 || !left ||
            left->type_id != instruction->result_type_id) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_SUBGROUP_IMUL:
    case GPU_IR_SHADER_ARITH_SUBGROUP_BIT_AND:
    case GPU_IR_SHADER_ARITH_SUBGROUP_BIT_OR:
    case GPU_IR_SHADER_ARITH_SUBGROUP_BIT_XOR:
        if (!is_int || type->kind != GPU_IR_SHADER_TYPE_INT || type->bit_width != 32 || !left ||
            left->type_id != instruction->result_type_id) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_ABSOLUTE:
    case GPU_IR_SHADER_ARITH_FLOOR:
    case GPU_IR_SHADER_ARITH_CEILING:
    case GPU_IR_SHADER_ARITH_SQRT:
    case GPU_IR_SHADER_ARITH_INVERSE_SQRT:
    case GPU_IR_SHADER_ARITH_TRUNC:
    case GPU_IR_SHADER_ARITH_FRACT:
    case GPU_IR_SHADER_ARITH_ROUND:
    case GPU_IR_SHADER_ARITH_ROUND_EVEN:
    case GPU_IR_SHADER_ARITH_SIN:
    case GPU_IR_SHADER_ARITH_COS:
    case GPU_IR_SHADER_ARITH_TAN:
    case GPU_IR_SHADER_ARITH_ASIN:
    case GPU_IR_SHADER_ARITH_ACOS:
    case GPU_IR_SHADER_ARITH_ATAN:
    case GPU_IR_SHADER_ARITH_SINH:
    case GPU_IR_SHADER_ARITH_COSH:
    case GPU_IR_SHADER_ARITH_TANH:
    case GPU_IR_SHADER_ARITH_ASINH:
    case GPU_IR_SHADER_ARITH_ACOSH:
    case GPU_IR_SHADER_ARITH_ATANH:
    case GPU_IR_SHADER_ARITH_EXP:
    case GPU_IR_SHADER_ARITH_LOG:
    case GPU_IR_SHADER_ARITH_EXP2:
    case GPU_IR_SHADER_ARITH_LOG2:
        if (!is_float) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_MINIMUM:
    case GPU_IR_SHADER_ARITH_MAXIMUM:
    case GPU_IR_SHADER_ARITH_POWER:
        if (!is_float && !integer_minmax) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_CLAMP:
    case GPU_IR_SHADER_ARITH_FMA:
    case GPU_IR_SHADER_ARITH_INTERPOLATE:
    case GPU_IR_SHADER_ARITH_STEP:
    case GPU_IR_SHADER_ARITH_SMOOTHSTEP:
        if (!is_float) return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_INTEGER_MAD:
        if (!is_int || (type->kind != GPU_IR_SHADER_TYPE_INT &&
            (type->kind != GPU_IR_SHADER_TYPE_VECTOR || type->element_count < 2 || type->element_count > 4)))
            return GPU_IR_UNSUPPORTED;
        break;
    case GPU_IR_SHADER_ARITH_FLOAT_MAD:
        if (!is_float || (type->kind != GPU_IR_SHADER_TYPE_FLOAT &&
            (type->kind != GPU_IR_SHADER_TYPE_VECTOR || type->element_count < 2 || type->element_count > 4)))
            return GPU_IR_UNSUPPORTED;
        break;
    default: return GPU_IR_UNSUPPORTED;
    }

    uint32_t operation_signedness = is_signed;
    if (instruction->operation == GPU_IR_SHADER_ARITH_SHIFT_RIGHT_LOGICAL) operation_signedness = 0;
    else if (instruction->operation == GPU_IR_SHADER_ARITH_SHIFT_RIGHT_ARITHMETIC) operation_signedness = 1;
    int signed_sensitive = instruction->operation == GPU_IR_SHADER_ARITH_DIVIDE ||
        instruction->operation == GPU_IR_SHADER_ARITH_REMAINDER ||
        instruction->operation == GPU_IR_SHADER_ARITH_SHIFT_RIGHT_LOGICAL ||
        instruction->operation == GPU_IR_SHADER_ARITH_SHIFT_RIGHT_ARITHMETIC || integer_minmax;
    if (instruction->operation == GPU_IR_SHADER_ARITH_MODULO && is_int &&
        (uint32_t)type_is_signed != operation_signedness) return GPU_IR_UNSUPPORTED;
    if (is_int && signed_sensitive && (uint32_t)type_is_signed != operation_signedness) {
        char result_type_name[32];
        char *left_expression = integer_semantic_expression(shader, left->type_id,
            operation_signedness, left->expression);
        char *right_expression = unary ? NULL : integer_semantic_expression(shader, right->type_id,
            operation_signedness, right->expression);
        if (!left_expression || (!unary && !right_expression) ||
            !shader_type_name(shader, instruction->result_type_id, result_type_name, sizeof result_type_name)) {
            free(left_expression); free(right_expression);
            return GPU_IR_UNSUPPORTED;
        }
        int rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK && integer_minmax) {
            const char *function = instruction->operation == GPU_IR_SHADER_ARITH_MINIMUM ? "min" : "max";
            rc = builder_appendf(body, "as_type<%s>(%s(%s, %s));\n", result_type_name,
                function, left_expression, right_expression);
        } else if (rc == GPU_IR_OK) {
            rc = builder_appendf(body, "as_type<%s>(%s %s %s);\n", result_type_name,
                left_expression, operator, right_expression);
        }
        free(left_expression); free(right_expression);
        if (rc == GPU_IR_OK) {
            char name[48];
            snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
            rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
        }
        return rc;
    }

    if (ordered_float_minmax)
        return emit_ordered_float_minmax(shader, instruction, left, right, body, mutable_symbols);

    int rc;
    if (bit_count) {
        const gpu_ir_shader_type *input = shader_type(shader, left->type_id);
        const gpu_ir_shader_type *input_scalar = input && input->kind == GPU_IR_SHADER_TYPE_VECTOR ?
            shader_type(shader, input->element_type_id) : input;
        if (!input_scalar) return GPU_IR_UNSUPPORTED;
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK && input_scalar->signedness) {
            char unsigned_type[32];
            if (input->kind == GPU_IR_SHADER_TYPE_VECTOR) {
                int length = snprintf(unsigned_type, sizeof unsigned_type, "uint%u", input->element_count);
                if (length < 0 || (size_t)length >= sizeof unsigned_type) return GPU_IR_RANGE;
            } else {
                memcpy(unsigned_type, "uint", sizeof "uint");
            }
            rc = builder_appendf(body, "%s(popcount(%s(%s)));\n",
                type_name, unsigned_type, left->expression);
        } else if (rc == GPU_IR_OK) {
            rc = builder_appendf(body, "%s(popcount(%s));\n", type_name, left->expression);
        }
    } else if (bit_reverse) {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "reverse_bits(%s);\n", left->expression);
    } else if (bit_find) {
        return emit_bit_find(shader, instruction, left, scalar, body, mutable_symbols);
    } else if (bit_field_insert) {
        char *offset_expression = integer_semantic_expression(shader, third->type_id, 0, third->expression);
        char *count_expression = integer_semantic_expression(shader, fourth->type_id, 0, fourth->expression);
        if (dxil_bitfield_low5 && offset_expression && count_expression) {
            char *masked_offset = bit_range_low5_expression(offset_expression);
            char *masked_count = bit_range_low5_expression(count_expression);
            free(offset_expression); free(count_expression);
            offset_expression = masked_offset; count_expression = masked_count;
        }
        if (!offset_expression || !count_expression) {
            free(offset_expression); free(count_expression);
            return GPU_IR_UNSUPPORTED;
        }
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "insert_bits(%s, %s, %s, %s);\n",
            left->expression, right->expression, offset_expression, count_expression);
        free(offset_expression); free(count_expression);
    } else if (bit_field_extract) {
        char *base_expression = integer_semantic_expression(shader, left->type_id,
            bit_field_signed_extract, left->expression);
        char *offset_expression = integer_semantic_expression(shader, right->type_id, 0, right->expression);
        char *count_expression = integer_semantic_expression(shader, third->type_id, 0, third->expression);
        if (dxil_bitfield_low5 && offset_expression && count_expression) {
            char *masked_offset = bit_range_low5_expression(offset_expression);
            char *masked_count = bit_range_low5_expression(count_expression);
            free(offset_expression); free(count_expression);
            offset_expression = masked_offset; count_expression = masked_count;
        }
        const gpu_ir_shader_type *result_scalar = type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
            shader_type(shader, type->element_type_id) : type;
        if (!base_expression || !offset_expression || !count_expression || !result_scalar) {
            free(base_expression); free(offset_expression); free(count_expression);
            return GPU_IR_UNSUPPORTED;
        }
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK && result_scalar->signedness != (uint32_t)bit_field_signed_extract)
            rc = builder_appendf(body, "as_type<%s>(", type_name);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "extract_bits(%s, %s, %s)",
            base_expression, offset_expression, count_expression);
        if (rc == GPU_IR_OK && result_scalar->signedness != (uint32_t)bit_field_signed_extract)
            rc = builder_appendf(body, ")");
        if (rc == GPU_IR_OK) rc = builder_appendf(body, ";\n");
        free(base_expression); free(offset_expression); free(count_expression);
    } else if (integer_mad) {
        if (type->kind == GPU_IR_SHADER_TYPE_VECTOR) {
            char *left_unsigned = integer_semantic_expression(shader, left->type_id, 0, left->expression);
            char *right_unsigned = integer_semantic_expression(shader, right->type_id, 0, right->expression);
            char *third_unsigned = integer_semantic_expression(shader, third->type_id, 0, third->expression);
            if (!left_unsigned || !right_unsigned || !third_unsigned) {
                free(left_unsigned); free(right_unsigned); free(third_unsigned);
                return GPU_IR_UNSUPPORTED;
            }
            rc = emit_value_lhs(shader, body, instruction);
            if (rc == GPU_IR_OK && type_is_signed) rc = builder_appendf(body,
                "as_type<%s>(%s * %s + %s);\n", type_name,
                left_unsigned, right_unsigned, third_unsigned);
            else if (rc == GPU_IR_OK) rc = builder_appendf(body,
                "%s * %s + %s;\n", left_unsigned, right_unsigned, third_unsigned);
            free(left_unsigned); free(right_unsigned); free(third_unsigned);
        } else {
            rc = emit_value_lhs(shader, body, instruction);
            if (rc == GPU_IR_OK && type_is_signed) rc = builder_appendf(body,
                "as_type<%s>(uint(%s) * uint(%s) + uint(%s));\n", type_name,
                left->expression, right->expression, third->expression);
            else if (rc == GPU_IR_OK) rc = builder_appendf(body,
                "uint(%s) * uint(%s) + uint(%s);\n", left->expression, right->expression, third->expression);
        }
    } else if (float_mad) {
        char product_name[48];
        int product_length = snprintf(product_name, sizeof product_name, "vx_fmad_product_%u", instruction->result_id);
        if (product_length < 0 || (size_t)product_length >= sizeof product_name) return GPU_IR_RANGE;
        rc = builder_appendf(body, "    volatile %s %s = %s * %s;\n", type_name,
            product_name, left->expression, right->expression);
        if (rc == GPU_IR_OK) rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "%s + %s;\n", product_name, third->expression);
    } else if (interpolate) {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body,
            "%s * (%s(1.0f) - %s) + %s * %s;\n",
            left->expression, type_name, third->expression, right->expression, third->expression);
    } else if (instruction->operation == GPU_IR_SHADER_ARITH_SATURATE) {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body,
            "fmin(fmax(%s, %s(0.0f)), %s(1.0f));\n", left->expression, type_name, type_name);
    } else if (subgroup_vote) {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "%s(%s);\n",
            instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_ANY ? "simd_any" : "simd_all",
            left->expression);
    } else if (subgroup_integer_reduction) {
        const char *function =
            instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_SMIN ||
            instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_UMIN ? "simd_min" :
            instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_SMAX ||
            instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_UMAX ? "simd_max" :
            instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_IMUL ? "simd_product" :
            instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_BIT_AND ? "simd_and" :
            instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_BIT_OR ? "simd_or" : "simd_xor";
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK && instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL &&
            (instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_SMIN ||
             instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_SMAX))
            rc = builder_appendf(body, "as_type<uint>(%s(as_type<int>(%s)));\n", function, left->expression);
        else if (rc == GPU_IR_OK) rc = builder_appendf(body, "%s(%s);\n", function, left->expression);
    } else if (instruction->operation >= GPU_IR_SHADER_ARITH_ABSOLUTE) {
        const char *function = instruction->operation == GPU_IR_SHADER_ARITH_ABSOLUTE ? "fabs" :
            instruction->operation == GPU_IR_SHADER_ARITH_FLOOR ? "floor" :
            instruction->operation == GPU_IR_SHADER_ARITH_CEILING ? "ceil" :
            instruction->operation == GPU_IR_SHADER_ARITH_SQRT ? "sqrt" :
            instruction->operation == GPU_IR_SHADER_ARITH_INVERSE_SQRT ? "rsqrt" :
            instruction->operation == GPU_IR_SHADER_ARITH_MINIMUM ? (is_int ? "min" : "fmin") :
            instruction->operation == GPU_IR_SHADER_ARITH_MAXIMUM ? (is_int ? "max" : "fmax") :
            instruction->operation == GPU_IR_SHADER_ARITH_CLAMP ? "clamp" :
            instruction->operation == GPU_IR_SHADER_ARITH_STEP ? "step" :
            instruction->operation == GPU_IR_SHADER_ARITH_SMOOTHSTEP ? "smoothstep" :
            instruction->operation == GPU_IR_SHADER_ARITH_FMA ? "fma" :
            instruction->operation == GPU_IR_SHADER_ARITH_POWER ? "pow" :
            instruction->operation == GPU_IR_SHADER_ARITH_TRUNC ? "trunc" :
            instruction->operation == GPU_IR_SHADER_ARITH_FRACT ? "fract" :
            instruction->operation == GPU_IR_SHADER_ARITH_ROUND ? "round" :
            instruction->operation == GPU_IR_SHADER_ARITH_ROUND_EVEN ? "rint" :
            instruction->operation == GPU_IR_SHADER_ARITH_SIN ? "sin" :
            instruction->operation == GPU_IR_SHADER_ARITH_COS ? "cos" :
            instruction->operation == GPU_IR_SHADER_ARITH_TAN ? "tan" :
            instruction->operation == GPU_IR_SHADER_ARITH_ASIN ? "asin" :
            instruction->operation == GPU_IR_SHADER_ARITH_ACOS ? "acos" :
            instruction->operation == GPU_IR_SHADER_ARITH_ATAN ? "atan" :
            instruction->operation == GPU_IR_SHADER_ARITH_SINH ? "sinh" :
            instruction->operation == GPU_IR_SHADER_ARITH_COSH ? "cosh" :
            instruction->operation == GPU_IR_SHADER_ARITH_TANH ? "tanh" :
            instruction->operation == GPU_IR_SHADER_ARITH_ASINH ? "asinh" :
            instruction->operation == GPU_IR_SHADER_ARITH_ACOSH ? "acosh" :
            instruction->operation == GPU_IR_SHADER_ARITH_ATANH ? "atanh" :
            instruction->operation == GPU_IR_SHADER_ARITH_EXP ? "exp" :
            instruction->operation == GPU_IR_SHADER_ARITH_LOG ? "log" :
            instruction->operation == GPU_IR_SHADER_ARITH_EXP2 ? "exp2" : "log2";
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) {
            if (ternary) rc = builder_appendf(body, "%s(%s, %s, %s);\n", function,
                left->expression, right->expression, third->expression);
            else if (unary) rc = builder_appendf(body, "%s(%s);\n", function, left->expression);
            else rc = builder_appendf(body, "%s(%s, %s);\n", function,
                left->expression, right->expression);
        }
    } else if (dot_operation) {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "dot(%s, %s);\n",
            left->expression, right->expression);
    } else if (special_modulo == 2) {
        if (type->kind == GPU_IR_SHADER_TYPE_VECTOR) {
            rc = builder_appendf(body,
                "    %s vx_remainder_%u = %s %% %s;\n",
                type_name, instruction->result_id, left->expression, right->expression);
            if (rc == GPU_IR_OK) rc = emit_value_lhs(shader, body, instruction);
            if (rc == GPU_IR_OK) rc = builder_appendf(body,
                "select(vx_remainder_%u, vx_remainder_%u + %s, "
                "(vx_remainder_%u != %s(0)) & ((vx_remainder_%u < %s(0)) != (%s < %s(0))));\n",
                instruction->result_id, instruction->result_id, right->expression,
                instruction->result_id, type_name, instruction->result_id, type_name,
                right->expression, type_name);
        } else {
            rc = builder_appendf(body,
                "    int vx_remainder_%u = %s %% %s;\n",
                instruction->result_id, left->expression, right->expression);
            if (rc == GPU_IR_OK) rc = emit_value_lhs(shader, body, instruction);
            if (rc == GPU_IR_OK) rc = builder_appendf(body,
                "(vx_remainder_%u != 0 && ((vx_remainder_%u < 0) != (%s < 0))) ? "
                "(vx_remainder_%u + %s) : vx_remainder_%u;\n",
                instruction->result_id, instruction->result_id, right->expression,
                instruction->result_id, right->expression, instruction->result_id);
        }
    } else if (instruction->operation == GPU_IR_SHADER_ARITH_SHIFT_RIGHT_LOGICAL && type_is_signed) {
        char unsigned_type[32];
        if (type->kind == GPU_IR_SHADER_TYPE_VECTOR) {
            int n = snprintf(unsigned_type, sizeof unsigned_type, "uint%u", type->element_count);
            if (n < 0 || (size_t)n >= sizeof unsigned_type) return GPU_IR_RANGE;
        } else {
            memcpy(unsigned_type, "uint", sizeof "uint");
        }
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "as_type<%s>(%s(%s) >> %s(%s));\n",
            type_name, unsigned_type, left->expression, unsigned_type, right->expression);
    } else if (instruction->operation == GPU_IR_SHADER_ARITH_REMAINDER && is_float) {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "fmod(%s, %s);\n", left->expression, right->expression);
    } else if (instruction->operation == GPU_IR_SHADER_ARITH_MODULO && is_float) {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "(%s - (%s * floor(%s / %s)));\n",
            left->expression, right->expression, left->expression, right->expression);
    } else if (reduction) {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, operator, left->expression);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, ";\n");
    } else if (unary) {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "%s%s;\n", operator, left->expression);
    } else {
        rc = emit_value_lhs(shader, body, instruction);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "%s %s %s;\n",
            left->expression, operator, right->expression);
    }
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_comparison(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                           const msl_symbols *symbols, msl_builder *body, msl_symbols *mutable_symbols) {
    uint32_t left_id, right_id;
    int unary = instruction->operand_count == 1;
    if ((!unary && instruction->operand_count != 2) ||
        !instruction_id_operand(shader, instruction, 0, &left_id) ||
        (!unary && !instruction_id_operand(shader, instruction, 1, &right_id))) return GPU_IR_INVALID;
    const msl_symbol *left = symbols_find(symbols, left_id);
    const msl_symbol *right = unary ? left : symbols_find(symbols, right_id);
    const gpu_ir_shader_type *input_type = left ? shader_type(shader, left->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    if (!left || !right || left->is_pointer || right->is_pointer || left->type_id != right->type_id ||
        !input_type || !result_type) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *input_scalar_type = input_type;
    uint32_t input_lanes = 1;
    int vector_input = input_type->kind == GPU_IR_SHADER_TYPE_VECTOR;
    if (vector_input) {
        input_lanes = input_type->element_count;
        input_scalar_type = shader_type(shader, input_type->element_type_id);
        if (input_lanes < 2 || input_lanes > 4 || !input_scalar_type) return GPU_IR_UNSUPPORTED;
    }
    const gpu_ir_shader_type *result_scalar_type = result_type;
    if (vector_input) {
        if (result_type->kind != GPU_IR_SHADER_TYPE_VECTOR || result_type->element_count != input_lanes)
            return GPU_IR_UNSUPPORTED;
        result_scalar_type = shader_type(shader, result_type->element_type_id);
        if (!result_scalar_type) return GPU_IR_UNSUPPORTED;
    }
    if (result_scalar_type->kind != GPU_IR_SHADER_TYPE_BOOL || input_scalar_type->bit_width != 32 ||
        (input_scalar_type->kind != GPU_IR_SHADER_TYPE_INT && input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) ||
        !!(instruction->flags & GPU_IR_SHADER_FLAG_FLOAT) !=
            (input_scalar_type->kind == GPU_IR_SHADER_TYPE_FLOAT) ||
        ((instruction->flags & GPU_IR_SHADER_FLAG_SIGNED) && input_scalar_type->kind != GPU_IR_SHADER_TYPE_INT))
        return GPU_IR_UNSUPPORTED;

    const char *expression = NULL;
    switch (instruction->operation) {
    case GPU_IR_SHADER_COMPARE_IS_NAN:
        if (!unary || input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = "isnan(%s)";
        break;
    case GPU_IR_SHADER_COMPARE_IS_INF:
        if (!unary || input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = "isinf(%s)";
        break;
    case GPU_IR_SHADER_COMPARE_IS_FINITE:
        if (!unary || input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = "isfinite(%s)";
        break;
    case GPU_IR_SHADER_COMPARE_IS_NORMAL:
        if (!unary || input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = "isnormal(%s)";
        break;
    case GPU_IR_SHADER_COMPARE_EQUAL:
    case GPU_IR_SHADER_COMPARE_ORDERED_EQUAL: expression = "(%s == %s)"; break;
    case GPU_IR_SHADER_COMPARE_NOT_EQUAL: expression = "(%s != %s)"; break;
    case GPU_IR_SHADER_COMPARE_LESS:
    case GPU_IR_SHADER_COMPARE_ORDERED_LESS: expression = "(%s < %s)"; break;
    case GPU_IR_SHADER_COMPARE_LESS_EQUAL:
    case GPU_IR_SHADER_COMPARE_ORDERED_LESS_EQUAL: expression = "(%s <= %s)"; break;
    case GPU_IR_SHADER_COMPARE_GREATER:
    case GPU_IR_SHADER_COMPARE_ORDERED_GREATER: expression = "(%s > %s)"; break;
    case GPU_IR_SHADER_COMPARE_GREATER_EQUAL:
    case GPU_IR_SHADER_COMPARE_ORDERED_GREATER_EQUAL: expression = "(%s >= %s)"; break;
    case GPU_IR_SHADER_COMPARE_UNORDERED_EQUAL:
        if (input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = vector_input ? "((isnan(%s) | isnan(%s)) | (%s == %s))" :
            "(isnan(%s) || isnan(%s) || (%s == %s))";
        break;
    case GPU_IR_SHADER_COMPARE_ORDERED_NOT_EQUAL:
        if (input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = vector_input ? "((!isnan(%s) & !isnan(%s)) & (%s != %s))" :
            "(!isnan(%s) && !isnan(%s) && (%s != %s))";
        break;
    case GPU_IR_SHADER_COMPARE_UNORDERED_NOT_EQUAL:
        if (input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = vector_input ? "((isnan(%s) | isnan(%s)) | (%s != %s))" :
            "(isnan(%s) || isnan(%s) || (%s != %s))";
        break;
    case GPU_IR_SHADER_COMPARE_UNORDERED_LESS:
        if (input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = vector_input ? "((isnan(%s) | isnan(%s)) | (%s < %s))" :
            "(isnan(%s) || isnan(%s) || (%s < %s))";
        break;
    case GPU_IR_SHADER_COMPARE_UNORDERED_LESS_EQUAL:
        if (input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = vector_input ? "((isnan(%s) | isnan(%s)) | (%s <= %s))" :
            "(isnan(%s) || isnan(%s) || (%s <= %s))";
        break;
    case GPU_IR_SHADER_COMPARE_UNORDERED_GREATER:
        if (input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = vector_input ? "((isnan(%s) | isnan(%s)) | (%s > %s))" :
            "(isnan(%s) || isnan(%s) || (%s > %s))";
        break;
    case GPU_IR_SHADER_COMPARE_UNORDERED_GREATER_EQUAL:
        if (input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = vector_input ? "((isnan(%s) | isnan(%s)) | (%s >= %s))" :
            "(isnan(%s) || isnan(%s) || (%s >= %s))";
        break;
    case GPU_IR_SHADER_COMPARE_LESS_GREATER:
        if (input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = vector_input ? "((%s < %s) | (%s > %s))" : "((%s < %s) || (%s > %s))";
        break;
    case GPU_IR_SHADER_COMPARE_ORDERED:
        if (input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = vector_input ? "(!isnan(%s) & !isnan(%s))" : "(!isnan(%s) && !isnan(%s))";
        break;
    case GPU_IR_SHADER_COMPARE_UNORDERED:
        if (input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
        expression = vector_input ? "(isnan(%s) | isnan(%s))" : "(isnan(%s) || isnan(%s))";
        break;
    default: return GPU_IR_UNSUPPORTED;
    }
    if (vector_input && instruction->operation >= GPU_IR_SHADER_COMPARE_IS_NAN &&
        instruction->operation <= GPU_IR_SHADER_COMPARE_IS_NORMAL &&
        input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT) return GPU_IR_UNSUPPORTED;
    if (!vector_input && input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT &&
        instruction->operation >= GPU_IR_SHADER_COMPARE_ORDERED_EQUAL) return GPU_IR_UNSUPPORTED;
    if (vector_input && input_scalar_type->kind != GPU_IR_SHADER_TYPE_FLOAT &&
        instruction->operation >= GPU_IR_SHADER_COMPARE_ORDERED_EQUAL) return GPU_IR_UNSUPPORTED;

    char *left_expression = NULL, *right_expression = NULL;
    if (input_scalar_type->kind == GPU_IR_SHADER_TYPE_INT &&
        input_scalar_type->signedness != !!(instruction->flags & GPU_IR_SHADER_FLAG_SIGNED)) {
        left_expression = integer_semantic_expression(shader, left->type_id,
            !!(instruction->flags & GPU_IR_SHADER_FLAG_SIGNED), left->expression);
        right_expression = integer_semantic_expression(shader, right->type_id,
            !!(instruction->flags & GPU_IR_SHADER_FLAG_SIGNED), right->expression);
    } else {
        left_expression = msl_string_copy(left->expression);
        right_expression = msl_string_copy(right->expression);
    }
    if (!left_expression || !right_expression) {
        free(left_expression); free(right_expression);
        return GPU_IR_UNSUPPORTED;
    }
    int rc;
    rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, expression, left_expression, right_expression,
                                               left_expression, right_expression);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, ";\n");
    free(left_expression); free(right_expression);
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_select(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                       const msl_symbols *symbols, msl_builder *body, msl_symbols *mutable_symbols) {
    uint32_t condition_id, true_id, false_id;
    if (instruction->operand_count != 3 ||
        !instruction_id_operand(shader, instruction, 0, &condition_id) ||
        !instruction_id_operand(shader, instruction, 1, &true_id) ||
        !instruction_id_operand(shader, instruction, 2, &false_id)) return GPU_IR_INVALID;
    const msl_symbol *condition = symbols_find(symbols, condition_id);
    const msl_symbol *on_true = symbols_find(symbols, true_id);
    const msl_symbol *on_false = symbols_find(symbols, false_id);
    const gpu_ir_shader_type *condition_type = condition ? shader_type(shader, condition->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    char type_name[32];
    if (!condition || condition->is_pointer || !condition_type ||
        (condition_type->kind != GPU_IR_SHADER_TYPE_BOOL && condition_type->kind != GPU_IR_SHADER_TYPE_VECTOR) ||
        !on_true || !on_false || on_true->is_pointer || on_false->is_pointer ||
        on_true->type_id != instruction->result_type_id || on_false->type_id != instruction->result_type_id ||
        !shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name)) return GPU_IR_UNSUPPORTED;

    int vector_condition = condition_type->kind == GPU_IR_SHADER_TYPE_VECTOR;
    if (vector_condition) {
        const gpu_ir_shader_type *condition_element = shader_type(shader, condition_type->element_type_id);
        if (!condition_element || condition_element->kind != GPU_IR_SHADER_TYPE_BOOL ||
            condition_type->element_count < 2 || condition_type->element_count > 4 || !result_type ||
            result_type->kind != GPU_IR_SHADER_TYPE_VECTOR ||
            result_type->element_count != condition_type->element_count) return GPU_IR_UNSUPPORTED;
    }

    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK && !vector_condition) rc = builder_appendf(body, "(%s ? %s : %s);\n",
        condition->expression, on_true->expression, on_false->expression);
    if (rc == GPU_IR_OK && vector_condition) {
        static const char *const components[] = { "x", "y", "z", "w" };
        rc = builder_appendf(body, "%s(", type_name);
        for (uint32_t i = 0; rc == GPU_IR_OK && i < condition_type->element_count; i++) {
            rc = builder_appendf(body, "%s(%s.%s ? %s.%s : %s.%s)", i ? ", " : "",
                condition->expression, components[i], on_true->expression, components[i],
                on_false->expression, components[i]);
        }
        if (rc == GPU_IR_OK) rc = builder_appendf(body, ");\n");
    }
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_composite_extract(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                                  const msl_symbols *symbols, msl_builder *body, msl_symbols *mutable_symbols) {
    uint32_t composite_id, index_id = 0;
    int dynamic = instruction->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC;
    if ((!dynamic && (instruction->operation != GPU_IR_SHADER_COMPOSITE_EXTRACT ||
                      instruction->operand_count != 1 || instruction->literal_word_count != 1)) ||
        (dynamic && (instruction->operand_count != 2 || instruction->literal_word_count ||
                     !instruction_id_operand(shader, instruction, 1, &index_id))) ||
        !instruction_id_operand(shader, instruction, 0, &composite_id)) return GPU_IR_UNSUPPORTED;
    const msl_symbol *composite = symbols_find(symbols, composite_id);
    const gpu_ir_shader_type *vector = composite ? shader_type(shader, composite->type_id) : NULL;
    if (!composite || composite->is_pointer || !vector || vector->kind != GPU_IR_SHADER_TYPE_VECTOR ||
        vector->element_count < 2 || vector->element_count > 4 ||
        vector->element_type_id != instruction->result_type_id) return GPU_IR_UNSUPPORTED;
    uint32_t index = 0;
    const msl_symbol *dynamic_index = NULL;
    if (dynamic) {
        dynamic_index = symbols_find(symbols, index_id);
        const gpu_ir_shader_type *index_type = dynamic_index ? shader_type(shader, dynamic_index->type_id) : NULL;
        if (!dynamic_index || dynamic_index->is_pointer || !index_type ||
            index_type->kind != GPU_IR_SHADER_TYPE_INT || index_type->bit_width != 32)
            return GPU_IR_UNSUPPORTED;
    } else {
        index = gpu_ir_shader_literal_word_at(shader, instruction->first_literal);
        if (index >= vector->element_count) return GPU_IR_INVALID;
    }
    static const char *const components[] = { "x", "y", "z", "w" };
    char type_name[32];
    if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name)) return GPU_IR_UNSUPPORTED;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK) rc = dynamic ?
        builder_appendf(body, "(%s)[%s];\n", composite->expression, dynamic_index->expression) :
        builder_appendf(body, "%s.%s;\n", composite->expression, components[index]);
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_composite_vector_insert(const gpu_ir_shader *shader,
                                        const gpu_ir_shader_instruction *instruction,
                                        const msl_symbols *symbols, msl_builder *body,
                                        msl_symbols *mutable_symbols) {
    int dynamic = instruction->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_INSERT_DYNAMIC;
    uint32_t vector_id, component_id, index_id = 0;
    if ((dynamic && (instruction->operand_count != 3 || instruction->literal_word_count ||
                     !instruction_id_operand(shader, instruction, 0, &vector_id) ||
                     !instruction_id_operand(shader, instruction, 1, &component_id) ||
                     !instruction_id_operand(shader, instruction, 2, &index_id))) ||
        (!dynamic && (instruction->operation != GPU_IR_SHADER_COMPOSITE_INSERT ||
                      instruction->operand_count != 2 || instruction->literal_word_count != 1 ||
                      !instruction_id_operand(shader, instruction, 0, &component_id) ||
                      !instruction_id_operand(shader, instruction, 1, &vector_id)))) return GPU_IR_UNSUPPORTED;
    const msl_symbol *vector_value = symbols_find(symbols, vector_id);
    const msl_symbol *component = symbols_find(symbols, component_id);
    const msl_symbol *dynamic_index = dynamic ? symbols_find(symbols, index_id) : NULL;
    const gpu_ir_shader_type *vector = vector_value ? shader_type(shader, vector_value->type_id) : NULL;
    const gpu_ir_shader_type *component_type = component ? shader_type(shader, component->type_id) : NULL;
    const gpu_ir_shader_type *index_type = dynamic_index ? shader_type(shader, dynamic_index->type_id) : NULL;
    if (!vector_value || vector_value->is_pointer || vector_value->type_id != instruction->result_type_id ||
        !vector || vector->kind != GPU_IR_SHADER_TYPE_VECTOR || vector->element_count < 2 ||
        vector->element_count > 4 || !component || component->is_pointer || !component_type ||
        component->type_id != vector->element_type_id || (dynamic && (!dynamic_index || dynamic_index->is_pointer ||
        !index_type || index_type->kind != GPU_IR_SHADER_TYPE_INT || index_type->bit_width != 32)))
        return GPU_IR_UNSUPPORTED;
    uint32_t index = dynamic ? 0u : gpu_ir_shader_literal_word_at(shader, instruction->first_literal);
    if (!dynamic && index >= vector->element_count) return GPU_IR_INVALID;
    char type_name[32];
    if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name))
        return GPU_IR_UNSUPPORTED;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, "%s;\n", vector_value->expression);
    if (rc == GPU_IR_OK) rc = dynamic ?
        builder_appendf(body, "    vx_value_%u[%s] = %s;\n", instruction->result_id,
                        dynamic_index->expression, component->expression) :
        builder_appendf(body, "    vx_value_%u[%uu] = %s;\n", instruction->result_id,
                        index, component->expression);
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_composite_construct(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                                    const msl_symbols *symbols, msl_builder *body,
                                    msl_symbols *mutable_symbols) {
    const gpu_ir_shader_type *vector = shader_type(shader, instruction->result_type_id);
    if (instruction->operation != GPU_IR_SHADER_COMPOSITE_CONSTRUCT || instruction->literal_word_count ||
        !vector || vector->kind != GPU_IR_SHADER_TYPE_VECTOR || vector->element_count < 2 ||
        vector->element_count > 4 || instruction->operand_count != vector->element_count)
        return GPU_IR_UNSUPPORTED;
    char type_name[32];
    if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name))
        return GPU_IR_UNSUPPORTED;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, "%s(", type_name);
    for (uint32_t i = 0; rc == GPU_IR_OK && i < instruction->operand_count; i++) {
        uint32_t constituent_id;
        if (!instruction_id_operand(shader, instruction, i, &constituent_id)) return GPU_IR_INVALID;
        const msl_symbol *constituent = symbols_find(symbols, constituent_id);
        if (!constituent || constituent->is_pointer || constituent->type_id != vector->element_type_id)
            return GPU_IR_UNSUPPORTED;
        rc = builder_appendf(body, "%s%s", i ? ", " : "", constituent->expression);
    }
    if (rc == GPU_IR_OK) rc = builder_appendf(body, ");\n");
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_copy(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                     const msl_symbols *symbols, msl_builder *body, msl_symbols *mutable_symbols) {
    uint32_t value_id;
    if (instruction->operand_count != 1 || !instruction_id_operand(shader, instruction, 0, &value_id))
        return GPU_IR_INVALID;
    const msl_symbol *value = symbols_find(symbols, value_id);
    char type_name[32];
    if (!value || value->is_pointer || value->type_id != instruction->result_type_id ||
        !shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name)) return GPU_IR_UNSUPPORTED;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, "%s;\n", value->expression);
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_array_length(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                             const msl_symbols *symbols, msl_builder *body, msl_symbols *mutable_symbols) {
    uint32_t structure_id;
    if (instruction->operand_count != 1 || instruction->literal_word_count != 1 ||
        !instruction_id_operand(shader, instruction, 0, &structure_id)) return GPU_IR_INVALID;
    if (!shader_type_is_u32(shader, instruction->result_type_id)) return GPU_IR_UNSUPPORTED;
    const msl_symbol *structure_symbol = symbols_find(symbols, structure_id);
    const gpu_ir_shader_type *pointer = structure_symbol ? shader_type(shader, structure_symbol->type_id) : NULL;
    const gpu_ir_shader_type *structure = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
        shader_type(shader, pointer->element_type_id) : NULL;
    uint32_t member_index = gpu_ir_shader_literal_word_at(shader, instruction->first_literal);
    const gpu_ir_shader_member *member = structure && member_index < structure->member_count ?
        gpu_ir_shader_type_member_at(shader, structure->first_member + member_index) : NULL;
    const gpu_ir_shader_type *array = member ? shader_type(shader, member->type_id) : NULL;
    if (!structure_symbol || !structure_symbol->is_pointer || !pointer ||
        pointer->storage_class != GPU_IR_SHADER_STORAGE_BUFFER || !structure ||
        structure->kind != GPU_IR_SHADER_TYPE_STRUCT || !(structure->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_BLOCK) ||
        !structure->member_count || member_index != structure->member_count - 1 || !member || !member->has_offset ||
        member->offset != 0 || !array || array->kind != GPU_IR_SHADER_TYPE_RUNTIME_ARRAY ||
        !array->has_array_stride || array->array_stride != 4 ||
        !shader_type_is_32bit_buffer_scalar(shader, array->element_type_id) ||
        structure_symbol->descriptor_slot == UINT32_MAX || structure_symbol->buffer_index_expression)
        return GPU_IR_UNSUPPORTED;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK && structure_symbol->descriptor_index_expression) {
        if (!structure_symbol->descriptor_array_count || structure_symbol->descriptor_array_count >
            GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS - structure_symbol->descriptor_slot)
            return GPU_IR_UNSUPPORTED;
        rc = builder_appendf(body,
            "uint(uint(%s) < %uu ? vx_buffer_ranges[%u + uint(%s)] / 4ul : 0ul);\n",
            structure_symbol->descriptor_index_expression, structure_symbol->descriptor_array_count,
            structure_symbol->descriptor_slot, structure_symbol->descriptor_index_expression);
    } else if (rc == GPU_IR_OK) {
        rc = builder_appendf(body, "uint(vx_buffer_ranges[%u] / 4ul);\n",
                             structure_symbol->descriptor_slot);
    }
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_convert(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                        const msl_symbols *symbols, msl_builder *body, msl_symbols *mutable_symbols) {
    uint32_t input_id;
    if (instruction->operand_count != 1 || instruction->literal_word_count ||
        !instruction_id_operand(shader, instruction, 0, &input_id)) return GPU_IR_INVALID;
    const msl_symbol *input = symbols_find(symbols, input_id);
    const gpu_ir_shader_type *input_type = input ? shader_type(shader, input->type_id) : NULL;
    const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
    const gpu_ir_shader_type *input_scalar = input_type && input_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, input_type->element_type_id) : input_type;
    const gpu_ir_shader_type *result_scalar = result_type && result_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, result_type->element_type_id) : result_type;
    uint32_t input_components = input_type && input_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        input_type->element_count : 1;
    uint32_t result_components = result_type && result_type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        result_type->element_count : 1;
    if (!input || input->is_pointer || !input_scalar || !result_scalar ||
        input_components != result_components || input_scalar->bit_width != 32 || result_scalar->bit_width != 32 ||
        (input_scalar->kind != GPU_IR_SHADER_TYPE_INT && input_scalar->kind != GPU_IR_SHADER_TYPE_FLOAT) ||
        (result_scalar->kind != GPU_IR_SHADER_TYPE_INT && result_scalar->kind != GPU_IR_SHADER_TYPE_FLOAT))
        return GPU_IR_UNSUPPORTED;
    int compatible = 0;
    switch (instruction->operation) {
    case GPU_IR_SHADER_CONVERT_FLOAT_TO_UNSIGNED:
        compatible = input_scalar->kind == GPU_IR_SHADER_TYPE_FLOAT && result_scalar->kind == GPU_IR_SHADER_TYPE_INT &&
            !result_scalar->signedness; break;
    case GPU_IR_SHADER_CONVERT_FLOAT_TO_SIGNED:
        compatible = input_scalar->kind == GPU_IR_SHADER_TYPE_FLOAT && result_scalar->kind == GPU_IR_SHADER_TYPE_INT;
        break;
    case GPU_IR_SHADER_CONVERT_SIGNED_TO_FLOAT:
        compatible = input_scalar->kind == GPU_IR_SHADER_TYPE_INT && result_scalar->kind == GPU_IR_SHADER_TYPE_FLOAT;
        break;
    case GPU_IR_SHADER_CONVERT_UNSIGNED_TO_FLOAT:
        compatible = input_scalar->kind == GPU_IR_SHADER_TYPE_INT && !input_scalar->signedness &&
            result_scalar->kind == GPU_IR_SHADER_TYPE_FLOAT; break;
    case GPU_IR_SHADER_CONVERT_UNSIGNED_WIDTH:
        compatible = input_scalar->kind == GPU_IR_SHADER_TYPE_INT && !input_scalar->signedness &&
            result_scalar->kind == GPU_IR_SHADER_TYPE_INT && !result_scalar->signedness; break;
    case GPU_IR_SHADER_CONVERT_SIGNED_WIDTH:
        compatible = input_scalar->kind == GPU_IR_SHADER_TYPE_INT && input_scalar->signedness &&
            result_scalar->kind == GPU_IR_SHADER_TYPE_INT && result_scalar->signedness; break;
    case GPU_IR_SHADER_CONVERT_FLOAT_WIDTH:
        compatible = input_scalar->kind == GPU_IR_SHADER_TYPE_FLOAT && result_scalar->kind == GPU_IR_SHADER_TYPE_FLOAT;
        break;
    case GPU_IR_SHADER_CONVERT_BITCAST:
        compatible = (input_scalar->kind == GPU_IR_SHADER_TYPE_INT || input_scalar->kind == GPU_IR_SHADER_TYPE_FLOAT) &&
            (result_scalar->kind == GPU_IR_SHADER_TYPE_INT || result_scalar->kind == GPU_IR_SHADER_TYPE_FLOAT);
        break;
    default: return GPU_IR_UNSUPPORTED;
    }
    if (!compatible) return GPU_IR_UNSUPPORTED;
    char type_name[32];
    if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name))
        return GPU_IR_UNSUPPORTED;
    int rc = emit_value_lhs(shader, body, instruction);
    if (rc == GPU_IR_OK && instruction->operation == GPU_IR_SHADER_CONVERT_SIGNED_TO_FLOAT) {
        char *signed_expression = integer_semantic_expression(shader, input->type_id, 1, input->expression);
        if (!signed_expression) rc = GPU_IR_UNSUPPORTED;
        else rc = builder_appendf(body, "%s(%s);\n", type_name, signed_expression);
        free(signed_expression);
    } else if (rc == GPU_IR_OK && instruction->operation == GPU_IR_SHADER_CONVERT_FLOAT_TO_SIGNED) {
        char signed_type_name[32];
        if (!shader_integer_type_name(shader, instruction->result_type_id, 1,
                                      signed_type_name, sizeof signed_type_name))
            rc = GPU_IR_UNSUPPORTED;
        else if (result_scalar->signedness)
            rc = builder_appendf(body, "%s(%s);\n", signed_type_name, input->expression);
        else
            rc = builder_appendf(body, "as_type<%s>(%s(%s));\n", type_name,
                                 signed_type_name, input->expression);
    } else if (rc == GPU_IR_OK) {
        rc = instruction->operation == GPU_IR_SHADER_CONVERT_BITCAST ?
            builder_appendf(body, "as_type<%s>(%s);\n", type_name, input->expression) :
            builder_appendf(body, "%s(%s);\n", type_name, input->expression);
    }
    if (rc == GPU_IR_OK) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

enum {
    MSL_HELPER_CAPTURE_NONE,
    MSL_HELPER_CAPTURE_BUFFER,
    MSL_HELPER_CAPTURE_STORAGE_IMAGE,
    MSL_HELPER_CAPTURE_SAMPLER,
    MSL_HELPER_CAPTURE_COMBINED_IMAGE,
    MSL_HELPER_CAPTURE_INPUT,
    MSL_HELPER_CAPTURE_WORKGROUP,
    MSL_HELPER_CAPTURE_PUSH_CONSTANT
};

static int helper_capture_kind(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *variable,
                               const gpu_ir_pipeline_layout *layout, uint32_t *kind, uint32_t *slot,
                               char *type_name, size_t type_capacity) {
    if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE ||
        variable->function_index != UINT32_MAX || !kind || !slot || !type_name || !type_capacity)
        return GPU_IR_INVALID;
    *kind = MSL_HELPER_CAPTURE_NONE;
    *slot = UINT32_MAX;
    type_name[0] = 0;
    if (variable->storage_class == GPU_IR_SHADER_STORAGE_BUFFER) {
        const gpu_ir_shader_type *resource_pointer = shader_type(shader, variable->result_type_id);
        const gpu_ir_shader_type *resource_type = resource_pointer &&
            resource_pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
            shader_type(shader, resource_pointer->element_type_id) : NULL;
        if (resource_type && resource_type->kind == GPU_IR_SHADER_TYPE_ARRAY) return GPU_IR_UNSUPPORTED;
        uint32_t descriptor_count = 1;
        int rc = global_resource_shape(shader, variable, &descriptor_count);
        if (rc != GPU_IR_OK) return rc;
        if (descriptor_count != 1) return GPU_IR_UNSUPPORTED;
        rc = descriptor_slot(shader, layout, variable, descriptor_count, GPU_IR_STAGE_COMPUTE, slot);
        if (rc != GPU_IR_OK) return rc;
        const gpu_ir_shader_type *element_type = global_resource_element_type(shader, variable);
        char element_name[32];
        if (!element_type || !shader_type_name(shader, element_type->id, element_name, sizeof element_name))
            return GPU_IR_UNSUPPORTED;
        const char *qualifier = variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE ?
            "device const" : "device";
        if (snprintf(type_name, type_capacity, "%s %s*", qualifier, element_name) >= (int)type_capacity)
            return GPU_IR_RANGE;
        *kind = MSL_HELPER_CAPTURE_BUFFER;
    } else if (variable->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) {
        if (global_sampler_shape(shader, variable) == GPU_IR_OK) {
            int rc = descriptor_sampler_slot(shader, layout, variable, GPU_IR_STAGE_COMPUTE, slot);
            if (rc != GPU_IR_OK) return rc;
            if (snprintf(type_name, type_capacity, "sampler") >= (int)type_capacity) return GPU_IR_RANGE;
            *kind = MSL_HELPER_CAPTURE_SAMPLER;
            return GPU_IR_OK;
        }
        if (global_combined_image_shape(shader, variable, NULL) == GPU_IR_OK) {
            uint32_t sampler_slot;
            int rc = descriptor_texture_slot(shader, layout, variable, GPU_IR_STAGE_COMPUTE, slot);
            if (rc == GPU_IR_OK) rc = descriptor_sampler_slot(shader, layout, variable,
                                                               GPU_IR_STAGE_COMPUTE, &sampler_slot);
            if (rc != GPU_IR_OK) return rc;
            if (snprintf(type_name, type_capacity, "texture2d<float, access::sample>") >= (int)type_capacity)
                return GPU_IR_RANGE;
            *kind = MSL_HELPER_CAPTURE_COMBINED_IMAGE;
            return GPU_IR_OK;
        }
        int readable = 0, writable = 0;
        int rc = storage_image_parameter_type(shader, variable, type_name, type_capacity,
                                               &readable, &writable);
        if (rc != GPU_IR_OK && global_sampled_image_shape(shader, variable, NULL) == GPU_IR_OK) {
            const gpu_ir_shader_type *image = NULL;
            if (global_sampled_image_shape(shader, variable, &image) != GPU_IR_OK) return GPU_IR_INVALID;
            const char *element = sampled_image_component_name(shader, image);
            int sampled = global_sampled_image_used_for_sampling(shader, variable);
            const gpu_ir_shader_type *component = shader_type(shader, image->element_type_id);
            if (!element || (sampled && component->kind != GPU_IR_SHADER_TYPE_FLOAT)) return GPU_IR_UNSUPPORTED;
            const char *texture_type = image->image_multisampled ?
                image->image_arrayed ? "texture2d_ms_array" : "texture2d_ms" :
                image->image_arrayed ? "texture2d_array" : "texture2d";
            const char *access = sampled ? "sample" : "read";
            if (snprintf(type_name, type_capacity, "%s<%s, access::%s>", texture_type, element, access) >=
                (int)type_capacity)
                return GPU_IR_RANGE;
            readable = 1;
            writable = 0;
            rc = GPU_IR_OK;
        }
        if (rc == GPU_IR_OK) rc = descriptor_texture_slot(shader, layout, variable,
                                                           GPU_IR_STAGE_COMPUTE, slot);
        if (rc != GPU_IR_OK) return rc;
        *kind = MSL_HELPER_CAPTURE_STORAGE_IMAGE;
    } else if (variable->storage_class == GPU_IR_SHADER_STORAGE_INPUT) {
        const char *builtin_type = NULL, *attribute = NULL;
        int rc = compute_builtin_parameter(shader, variable, &builtin_type, &attribute);
        if (rc != GPU_IR_OK) return rc;
        if (snprintf(type_name, type_capacity, "%s", builtin_type) >= (int)type_capacity) return GPU_IR_RANGE;
        *kind = MSL_HELPER_CAPTURE_INPUT;
    } else if (variable->storage_class == GPU_IR_SHADER_STORAGE_PUSH_CONSTANT) {
        const gpu_ir_shader_type *pointer = shader_type(shader, variable->result_type_id);
        const gpu_ir_shader_type *block = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
            shader_type(shader, pointer->element_type_id) : NULL;
        if (!pointer || pointer->storage_class != GPU_IR_SHADER_STORAGE_PUSH_CONSTANT || !block ||
            block->kind != GPU_IR_SHADER_TYPE_STRUCT || variable->operand_count)
            return GPU_IR_UNSUPPORTED;
        if (snprintf(type_name, type_capacity, "constant uint*") >= (int)type_capacity) return GPU_IR_RANGE;
        *kind = MSL_HELPER_CAPTURE_PUSH_CONSTANT;
    } else if (variable->storage_class == GPU_IR_SHADER_STORAGE_WORKGROUP) {
        const gpu_ir_shader_type *pointer = shader_type(shader, variable->result_type_id);
        const gpu_ir_shader_type *element = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
            shader_type(shader, pointer->element_type_id) : NULL;
        char element_name[32];
        if (!pointer || pointer->kind != GPU_IR_SHADER_TYPE_POINTER ||
            pointer->storage_class != GPU_IR_SHADER_STORAGE_WORKGROUP || !element || variable->operand_count)
            return GPU_IR_UNSUPPORTED;
        if (element->kind == GPU_IR_SHADER_TYPE_ARRAY) {
            uint32_t array_length = 0;
            if (!constant_u32(shader, element->array_length_id, &array_length) || !array_length ||
                array_length > 65536 ||
                !shader_type_name(shader, element->element_type_id, element_name, sizeof element_name))
                return GPU_IR_UNSUPPORTED;
            if (snprintf(type_name, type_capacity, "threadgroup array<%s, %u>*", element_name,
                         array_length) >= (int)type_capacity) return GPU_IR_RANGE;
        } else {
            if (!shader_type_name(shader, pointer->element_type_id, element_name, sizeof element_name))
                return GPU_IR_UNSUPPORTED;
            if (snprintf(type_name, type_capacity, "threadgroup %s*", element_name) >= (int)type_capacity)
                return GPU_IR_RANGE;
        }
        *kind = MSL_HELPER_CAPTURE_WORKGROUP;
    } else {
        return GPU_IR_UNSUPPORTED;
    }
    return GPU_IR_OK;
}

static int append_helper_captures(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                                  const msl_symbols *symbols, int with_parameter_names,
                                  uint32_t *parameter_count, msl_builder *output) {
    int has_buffer = 0;
    int rc = GPU_IR_OK;
    for (uint32_t i = 0; rc == GPU_IR_OK && i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
        if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE ||
            variable->function_index != UINT32_MAX) continue;
        uint32_t kind, slot;
        char type_name[96], name[48];
        rc = helper_capture_kind(shader, variable, layout, &kind, &slot, type_name, sizeof type_name);
        if (rc != GPU_IR_OK || kind == MSL_HELPER_CAPTURE_NONE) continue;
        if (symbols) {
            const msl_symbol *capture = symbols_find(symbols, variable->result_id);
            if (!capture || !capture->is_pointer) return GPU_IR_UNSUPPORTED;
        }
        if (with_parameter_names) {
            snprintf(name, sizeof name, "vx_capture_%u", variable->result_id);
            rc = builder_appendf(output, "%s%s %s", *parameter_count ? ", " : "", type_name, name);
        } else {
            rc = builder_appendf(output, "%s%s", *parameter_count ? ", " : "", type_name);
        }
        if (rc == GPU_IR_OK) (*parameter_count)++;
        if (rc == GPU_IR_OK && kind == MSL_HELPER_CAPTURE_COMBINED_IMAGE) {
            if (with_parameter_names) {
                snprintf(name, sizeof name, "vx_capture_sampler_%u", variable->result_id);
                rc = builder_appendf(output, ", sampler %s", name);
            } else {
                rc = builder_appendf(output, ", sampler");
            }
            if (rc == GPU_IR_OK) (*parameter_count)++;
        }
        if (kind == MSL_HELPER_CAPTURE_BUFFER) has_buffer = 1;
    }
    if (rc == GPU_IR_OK && has_buffer) {
        rc = builder_appendf(output, "%sconstant ulong* vx_buffer_ranges",
            *parameter_count ? ", " : "");
        if (rc == GPU_IR_OK) (*parameter_count)++;
    }
    return rc;
}

static int append_helper_call_captures(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                                       const msl_symbols *symbols, uint32_t *argument_count,
                                       msl_builder *body) {
    int has_buffer = 0;
    int rc = GPU_IR_OK;
    for (uint32_t i = 0; rc == GPU_IR_OK && i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
        if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE ||
            variable->function_index != UINT32_MAX) continue;
        uint32_t kind, slot;
        char type_name[96];
        rc = helper_capture_kind(shader, variable, layout, &kind, &slot, type_name, sizeof type_name);
        if (rc != GPU_IR_OK || kind == MSL_HELPER_CAPTURE_NONE) continue;
        const msl_symbol *capture = symbols_find(symbols, variable->result_id);
        if (!capture || !capture->is_pointer) return GPU_IR_UNSUPPORTED;
        const char *argument = capture->capture_argument_expression ? capture->capture_argument_expression :
            capture->expression;
        rc = builder_appendf(body, "%s%s", *argument_count ? ", " : "", argument);
        if (rc == GPU_IR_OK) (*argument_count)++;
        if (rc == GPU_IR_OK && kind == MSL_HELPER_CAPTURE_COMBINED_IMAGE) {
            if (!capture->aux_expression) return GPU_IR_UNSUPPORTED;
            rc = builder_appendf(body, ", %s", capture->aux_expression);
            if (rc == GPU_IR_OK) (*argument_count)++;
        }
        if (kind == MSL_HELPER_CAPTURE_BUFFER) has_buffer = 1;
    }
    if (rc == GPU_IR_OK && has_buffer) {
        rc = builder_appendf(body, "%svx_buffer_ranges", *argument_count ? ", " : "");
        if (rc == GPU_IR_OK) (*argument_count)++;
    }
    return rc;
}

static int helper_parameter_type_name(const gpu_ir_shader *shader, uint32_t type_id,
                                      char *type_name, size_t type_capacity, int *is_pointer) {
    if (!shader || !type_name || !type_capacity || !is_pointer) return GPU_IR_INVALID;
    const gpu_ir_shader_type *type = shader_type(shader, type_id);
    if (!type) return GPU_IR_INVALID;
    *is_pointer = 0;
    if (type->kind != GPU_IR_SHADER_TYPE_POINTER)
        return shader_type_name(shader, type_id, type_name, type_capacity) ? GPU_IR_OK : GPU_IR_UNSUPPORTED;

    const char *address_space = type->storage_class == GPU_IR_SHADER_STORAGE_FUNCTION ? "thread" :
        type->storage_class == GPU_IR_SHADER_STORAGE_WORKGROUP ? "threadgroup" : NULL;
    const gpu_ir_shader_type *pointee = shader_type(shader, type->element_type_id);
    char pointee_name[64];
    if (!address_space || !pointee) return GPU_IR_UNSUPPORTED;
    if (pointee->kind == GPU_IR_SHADER_TYPE_ARRAY) {
        uint32_t length = 0, max_length = type->storage_class == GPU_IR_SHADER_STORAGE_FUNCTION ? 4096u : 65536u;
        char element_name[32];
        if (!constant_u32(shader, pointee->array_length_id, &length) || !length || length > max_length ||
            !shader_type_name(shader, pointee->element_type_id, element_name, sizeof element_name))
            return GPU_IR_UNSUPPORTED;
        if (snprintf(pointee_name, sizeof pointee_name, "array<%s, %u>", element_name, length) >=
            (int)sizeof pointee_name) return GPU_IR_RANGE;
    } else if (!shader_type_name(shader, type->element_type_id, pointee_name, sizeof pointee_name)) {
        return GPU_IR_UNSUPPORTED;
    }
    if (snprintf(type_name, type_capacity, "%s %s*", address_space, pointee_name) >= (int)type_capacity)
        return GPU_IR_RANGE;
    *is_pointer = 1;
    return GPU_IR_OK;
}

static int append_helper_signature(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                                   const gpu_ir_shader_function *function, int with_parameter_names,
                                   const msl_symbols *symbols, msl_builder *output) {
    const gpu_ir_shader_type *function_type = shader_type(shader, function->function_type_id);
    char result_name[32];
    if (!function_type || function_type->kind != GPU_IR_SHADER_TYPE_FUNCTION ||
        function_type->element_type_id != function->result_type_id ||
        function_type->member_count != function->parameter_count ||
        !shader_type_name(shader, function->result_type_id, result_name, sizeof result_name))
        return GPU_IR_UNSUPPORTED;
    int rc = builder_appendf(output, "%s vx_fn_%u(", result_name, function->id);
    uint32_t parameter_count = 0;
    for (uint32_t i = 0; rc == GPU_IR_OK && i < function->parameter_count; i++) {
        const gpu_ir_shader_instruction *parameter = gpu_ir_shader_instruction_at(shader,
            function->first_instruction + i);
        const gpu_ir_shader_member *function_parameter = gpu_ir_shader_type_member_at(shader,
            function_type->first_member + i);
        char parameter_name[96];
        int parameter_is_pointer = 0;
        if (!parameter || !function_parameter || parameter->opcode != GPU_IR_SHADER_OP_PARAMETER ||
            parameter->block_index != UINT32_MAX || parameter->result_type_id != function_parameter->type_id ||
            helper_parameter_type_name(shader, parameter->result_type_id, parameter_name,
                                       sizeof parameter_name, &parameter_is_pointer) != GPU_IR_OK ||
            !strcmp(parameter_name, "void")) return GPU_IR_UNSUPPORTED;
        rc = builder_appendf(output, "%s%s%s", i ? ", " : "", parameter_name,
                             with_parameter_names ? " " : "");
        if (rc == GPU_IR_OK && with_parameter_names)
            rc = builder_appendf(output, "vx_arg_%u", parameter->result_id);
        if (rc == GPU_IR_OK) parameter_count++;
    }
    if (rc == GPU_IR_OK)
        rc = append_helper_captures(shader, layout, symbols, with_parameter_names, &parameter_count, output);
    if (rc == GPU_IR_OK) rc = builder_appendf(output, ")");
    return rc;
}

static int emit_function_call(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                              const gpu_ir_shader_instruction *instruction, const msl_symbols *symbols,
                              msl_builder *body,
                              msl_symbols *mutable_symbols) {
    uint32_t function_id;
    if (!instruction->result_id || instruction->literal_word_count || !instruction->operand_count ||
        !instruction_id_operand(shader, instruction, 0, &function_id)) return GPU_IR_INVALID;
    const gpu_ir_shader_function *function = gpu_ir_shader_find_function(shader, function_id);
    if (!function || function_id == gpu_ir_shader_entry_function(shader) ||
        function->result_type_id != instruction->result_type_id ||
        instruction->operand_count - 1u != function->parameter_count) return GPU_IR_INVALID;
    const gpu_ir_shader_type *return_type = shader_type(shader, function->result_type_id);
    char return_type_name[32];
    if (!return_type || (return_type->kind != GPU_IR_SHADER_TYPE_VOID &&
        !shader_type_name(shader, function->result_type_id, return_type_name, sizeof return_type_name)))
        return GPU_IR_UNSUPPORTED;
    int returns_value = return_type->kind != GPU_IR_SHADER_TYPE_VOID;
    int rc;
    uint32_t argument_count = 0;
    if (returns_value) rc = emit_value_lhs(shader, body, instruction);
    else rc = builder_appendf(body, "    ");
    if (rc == GPU_IR_OK) rc = builder_appendf(body, "vx_fn_%u(", function->id);
    for (uint32_t i = 0; rc == GPU_IR_OK && i < function->parameter_count; i++) {
        uint32_t argument_id;
        const gpu_ir_shader_instruction *parameter = gpu_ir_shader_instruction_at(shader,
            function->first_instruction + i);
        const msl_symbol *argument;
        const gpu_ir_shader_type *parameter_type = parameter ? shader_type(shader, parameter->result_type_id) : NULL;
        if (!instruction_id_operand(shader, instruction, i + 1, &argument_id) || !parameter ||
            parameter->opcode != GPU_IR_SHADER_OP_PARAMETER || !parameter_type) return GPU_IR_INVALID;
        argument = symbols_find(symbols, argument_id);
        if (!argument || argument->type_id != parameter->result_type_id ||
            (parameter_type->kind == GPU_IR_SHADER_TYPE_POINTER) != (argument->is_pointer != 0))
            return GPU_IR_UNSUPPORTED;
        if (argument->is_pointer) {
            if (argument->capture_argument_expression)
                rc = builder_appendf(body, "%s%s", i ? ", " : "", argument->capture_argument_expression);
            else
                rc = builder_appendf(body, "%s&%s", i ? ", " : "", argument->expression);
        } else {
            rc = builder_appendf(body, "%s%s", i ? ", " : "", argument->expression);
        }
        if (rc == GPU_IR_OK) argument_count++;
    }
    if (rc == GPU_IR_OK)
        rc = append_helper_call_captures(shader, layout, symbols, &argument_count, body);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, ");\n");
    if (rc == GPU_IR_OK && returns_value) {
        char name[48];
        snprintf(name, sizeof name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(mutable_symbols, instruction->result_id, instruction->result_type_id, 0, name);
    }
    return rc;
}

static int emit_multi_block_function(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                                     const gpu_ir_shader_function *function, msl_symbols *symbols,
                                     msl_builder *body);

static int emit_helper_function(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                                const gpu_ir_shader_function *function, msl_builder *output) {
    if (!function || !function->block_count) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_block *block = gpu_ir_shader_block_at(shader, function->first_block);
    const gpu_ir_shader_type *return_type = shader_type(shader, function->result_type_id);
    if (!block || block->function_index >= gpu_ir_shader_function_count(shader) ||
        !block->instruction_count || !return_type) return GPU_IR_INVALID;
    msl_symbols symbols = { 0 };
    msl_builder body = { 0 };
    int rc = add_global_constants(shader, &symbols);
    for (uint32_t i = 0; rc == GPU_IR_OK && i < function->parameter_count; i++) {
        const gpu_ir_shader_instruction *parameter = gpu_ir_shader_instruction_at(shader,
            function->first_instruction + i);
        char name[48], expression[96], parameter_type_name[96];
        int is_pointer = 0;
        if (!parameter || parameter->opcode != GPU_IR_SHADER_OP_PARAMETER) { rc = GPU_IR_INVALID; break; }
        snprintf(name, sizeof name, "vx_arg_%u", parameter->result_id);
        rc = helper_parameter_type_name(shader, parameter->result_type_id, parameter_type_name,
                                         sizeof parameter_type_name, &is_pointer);
        if (rc != GPU_IR_OK) break;
        if (!is_pointer) {
            rc = symbols_add(&symbols, parameter->result_id, parameter->result_type_id, 0, name);
        } else {
            if (snprintf(expression, sizeof expression, "(*%s)", name) >= (int)sizeof expression) {
                rc = GPU_IR_RANGE;
                break;
            }
            rc = symbols_add(&symbols, parameter->result_id, parameter->result_type_id, 1, expression);
            if (rc == GPU_IR_OK)
                rc = symbols_set_capture_argument(&symbols, parameter->result_id, name);
        }
    }
    for (uint32_t i = 0; rc == GPU_IR_OK && i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
        if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE ||
            variable->function_index != UINT32_MAX) continue;
        uint32_t kind, slot;
        char type_name[96], name[48];
        rc = helper_capture_kind(shader, variable, layout, &kind, &slot, type_name, sizeof type_name);
        if (rc != GPU_IR_OK || kind == MSL_HELPER_CAPTURE_NONE) continue;
        snprintf(name, sizeof name, "vx_capture_%u", variable->result_id);
        const char *expression = kind == MSL_HELPER_CAPTURE_WORKGROUP ? "" : name;
        if (kind == MSL_HELPER_CAPTURE_WORKGROUP) {
            if (snprintf(name, sizeof name, "vx_capture_%u", variable->result_id) >= (int)sizeof name) {
                rc = GPU_IR_RANGE;
                break;
            }
            if (snprintf(type_name, sizeof type_name, "(*%s)", name) >= (int)sizeof type_name) {
                rc = GPU_IR_RANGE;
                break;
            }
            expression = type_name;
        }
        rc = symbols_add(&symbols, variable->result_id, variable->result_type_id, 1, expression);
        if (rc == GPU_IR_OK && kind == MSL_HELPER_CAPTURE_WORKGROUP)
            rc = symbols_set_capture_argument(&symbols, variable->result_id, name);
        if (rc == GPU_IR_OK && kind == MSL_HELPER_CAPTURE_BUFFER)
            rc = symbols_set_buffer_access(&symbols, variable->result_id, slot, NULL,
                !(variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE));
        if (rc == GPU_IR_OK && kind == MSL_HELPER_CAPTURE_STORAGE_IMAGE) {
            int sampled_image = global_sampled_image_shape(shader, variable, NULL) == GPU_IR_OK;
            rc = symbols_set_image_access(&symbols, variable->result_id,
                sampled_image || !(variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_READABLE),
                !sampled_image && !(variable->decoration_flags & GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE));
        }
        if (rc == GPU_IR_OK && kind == MSL_HELPER_CAPTURE_COMBINED_IMAGE) {
            char sampler_name[48];
            snprintf(sampler_name, sizeof sampler_name, "vx_capture_sampler_%u", variable->result_id);
            rc = symbols_set_aux_expression(&symbols, variable->result_id, sampler_name);
            if (rc == GPU_IR_OK) rc = symbols_set_image_access(&symbols, variable->result_id, 1, 0);
        }
    }
    if (rc == GPU_IR_OK && (function->block_count > 1 ||
        (block->terminator_opcode != GPU_IR_SHADER_OP_RETURN &&
         block->terminator_opcode != GPU_IR_SHADER_OP_RETURN_VALUE)))
        rc = emit_multi_block_function(shader, layout, function, &symbols, &body);
    for (uint32_t i = 0; rc == GPU_IR_OK && function->block_count == 1 &&
         (block->terminator_opcode == GPU_IR_SHADER_OP_RETURN ||
          block->terminator_opcode == GPU_IR_SHADER_OP_RETURN_VALUE) && i < block->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader,
            block->first_instruction + i);
        if (!instruction || instruction->function_index != block->function_index ||
            instruction->block_index != function->first_block) { rc = GPU_IR_INVALID; break; }
        int is_last = i + 1 == block->instruction_count;
        switch (instruction->opcode) {
        case GPU_IR_SHADER_OP_VARIABLE:
            rc = emit_function_variable(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_ACCESS_CHAIN:
            rc = emit_access_chain(shader, instruction, &symbols, &symbols);
            break;
        case GPU_IR_SHADER_OP_LOAD:
            rc = emit_load(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_IMAGE_READ:
            rc = emit_image_read(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_IMAGE_QUERY:
            rc = emit_image_query(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_ATOMIC:
            rc = emit_atomic(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_IMAGE_WRITE:
            rc = emit_image_write(shader, instruction, &symbols, &body);
            break;
        case GPU_IR_SHADER_OP_SAMPLED_IMAGE:
            rc = emit_sampled_image(shader, instruction, &symbols, &symbols);
            break;
        case GPU_IR_SHADER_OP_IMAGE_SAMPLE:
            rc = emit_image_sample(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_STORE:
            rc = emit_store(shader, instruction, &symbols, &body);
            break;
        case GPU_IR_SHADER_OP_ARITHMETIC:
            rc = emit_arithmetic(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_COMPARE:
            rc = emit_comparison(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_SELECT:
            rc = emit_select(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_COMPOSITE:
            if (instruction->operation == GPU_IR_SHADER_COMPOSITE_CONSTRUCT)
                rc = emit_composite_construct(shader, instruction, &symbols, &body, &symbols);
            else if (instruction->operation == GPU_IR_SHADER_COMPOSITE_EXTRACT ||
                     instruction->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC)
                rc = emit_composite_extract(shader, instruction, &symbols, &body, &symbols);
            else if (instruction->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_INSERT_DYNAMIC ||
                     instruction->operation == GPU_IR_SHADER_COMPOSITE_INSERT)
                rc = emit_composite_vector_insert(shader, instruction, &symbols, &body, &symbols);
            else rc = GPU_IR_UNSUPPORTED;
            break;
        case GPU_IR_SHADER_OP_COPY:
            rc = emit_copy(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_CONVERT:
            rc = emit_convert(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_FUNCTION_CALL:
            rc = emit_function_call(shader, layout, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_BARRIER:
            rc = emit_barrier(shader, instruction, &body);
            break;
        case GPU_IR_SHADER_OP_DISCARD:
            rc = emit_discard(shader, instruction, &symbols, &body);
            break;
        case GPU_IR_SHADER_OP_RETURN:
            if (!is_last || return_type->kind != GPU_IR_SHADER_TYPE_VOID || instruction->operand_count ||
                instruction->result_id) rc = GPU_IR_INVALID;
            else rc = builder_appendf(&body, "    return;\n");
            break;
        case GPU_IR_SHADER_OP_RETURN_VALUE: {
            uint32_t value_id;
            const msl_symbol *value;
            if (!is_last || return_type->kind == GPU_IR_SHADER_TYPE_VOID || instruction->result_id ||
                instruction->operand_count != 1 || !instruction_id_operand(shader, instruction, 0, &value_id)) {
                rc = GPU_IR_INVALID;
                break;
            }
            value = symbols_find(&symbols, value_id);
            if (!value || value->is_pointer || value->type_id != function->result_type_id) {
                rc = GPU_IR_UNSUPPORTED;
                break;
            }
            rc = builder_appendf(&body, "    return %s;\n", value->expression);
            break;
        }
        default:
            rc = GPU_IR_UNSUPPORTED;
            break;
        }
    }
    if (rc == GPU_IR_OK) rc = append_helper_signature(shader, layout, function, 1, &symbols, output);
    if (rc == GPU_IR_OK) rc = builder_appendf(output, " {\n%s}\n\n", body.data ? body.data : "");
    symbols_destroy(&symbols);
    free(body.data);
    return rc;
}

static const gpu_ir_shader_block *find_function_block(const gpu_ir_shader *shader,
                                                       const gpu_ir_shader_function *function,
                                                       uint32_t label_id, uint32_t *block_index) {
    for (uint32_t i = 0; i < function->block_count; i++) {
        uint32_t index = function->first_block + i;
        const gpu_ir_shader_block *block = gpu_ir_shader_block_at(shader, index);
        if (block && block->label_id == label_id) {
            if (block_index) *block_index = index;
            return block;
        }
    }
    return NULL;
}

static int block_branches_to(const gpu_ir_shader *shader, const gpu_ir_shader_block *block,
                             uint32_t target_label) {
    if (!block || !block->instruction_count) return 0;
    const gpu_ir_shader_instruction *terminator = gpu_ir_shader_instruction_at(shader,
        block->first_instruction + block->instruction_count - 1);
    if (!terminator) return 0;
    if (terminator->opcode == GPU_IR_SHADER_OP_BRANCH) {
        uint32_t target;
        return terminator->operand_count == 1 && instruction_id_operand(shader, terminator, 0, &target) &&
               target == target_label;
    }
    if (terminator->opcode == GPU_IR_SHADER_OP_BRANCH_CONDITIONAL) {
        uint32_t on_true, on_false;
        return terminator->operand_count >= 3 &&
               instruction_id_operand(shader, terminator, 1, &on_true) &&
               instruction_id_operand(shader, terminator, 2, &on_false) &&
               (on_true == target_label || on_false == target_label);
    }
    if (terminator->opcode == GPU_IR_SHADER_OP_SWITCH) {
        if (terminator->operand_count < 2) return 0;
        for (uint32_t i = 1; i < terminator->operand_count; i++) {
            uint32_t target;
            if (instruction_id_operand(shader, terminator, i, &target) && target == target_label) return 1;
        }
    }
    return 0;
}

static int emit_cfg_phi(const gpu_ir_shader *shader, const gpu_ir_shader_instruction *instruction,
                        const gpu_ir_shader_function *function, uint32_t target_label,
                        const msl_symbols *symbols, msl_builder *body) {
    if (!instruction->result_id || instruction->operand_count < 2 || (instruction->operand_count & 1u) ||
        instruction->literal_word_count) return GPU_IR_INVALID;
    uint32_t pair_count = instruction->operand_count / 2;
    char type_name[32];
    if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name))
        return GPU_IR_UNSUPPORTED;
    uint32_t predecessor_count = 0;
    for (uint32_t i = 0; i < function->block_count; i++) {
        const gpu_ir_shader_block *candidate = gpu_ir_shader_block_at(shader, function->first_block + i);
        if (block_branches_to(shader, candidate, target_label)) predecessor_count++;
    }
    if (pair_count != predecessor_count) return GPU_IR_INVALID;
    int rc = builder_appendf(body, "    %s vx_phi_in_%u = {};\n", type_name, instruction->result_id);
    for (uint32_t i = 0; rc == GPU_IR_OK && i < instruction->operand_count; i += 2) {
        uint32_t value_id, label_id;
        if (!instruction_id_operand(shader, instruction, i, &value_id) ||
            !instruction_id_operand(shader, instruction, i + 1, &label_id)) return GPU_IR_INVALID;
        const gpu_ir_shader_block *predecessor = find_function_block(shader, function, label_id, NULL);
        if (!predecessor || !block_branches_to(shader, predecessor, target_label)) return GPU_IR_INVALID;
        for (uint32_t previous = 1; previous < i; previous += 2) {
            uint32_t previous_label;
            if (!instruction_id_operand(shader, instruction, previous, &previous_label)) return GPU_IR_INVALID;
            if (previous_label == label_id) return GPU_IR_INVALID;
        }
        const msl_symbol *value = symbols_find(symbols, value_id);
        const gpu_ir_shader_type *incoming_type = value ? shader_type(shader, value->type_id) : NULL;
        const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
        if (!value || value->is_pointer || !incoming_type || !result_type ||
            value->type_id != instruction->result_type_id ||
            (result_type->kind != GPU_IR_SHADER_TYPE_BOOL && result_type->kind != GPU_IR_SHADER_TYPE_INT &&
             result_type->kind != GPU_IR_SHADER_TYPE_FLOAT && result_type->kind != GPU_IR_SHADER_TYPE_VECTOR))
            return GPU_IR_UNSUPPORTED;
        rc = builder_appendf(body, "    %s (vx_pred == %uu) vx_phi_in_%u = %s;\n",
            i ? "else if" : "if", label_id, instruction->result_id, value->expression);
    }
    const gpu_ir_shader_type *return_type = shader_type(shader, function->result_type_id);
    if (!return_type) return GPU_IR_INVALID;
    char return_type_name[32];
    if (return_type->kind != GPU_IR_SHADER_TYPE_VOID &&
        !shader_type_name(shader, function->result_type_id, return_type_name, sizeof return_type_name))
        return GPU_IR_UNSUPPORTED;
    if (rc == GPU_IR_OK) rc = return_type->kind == GPU_IR_SHADER_TYPE_VOID ?
        builder_appendf(body, "    else return;\n") :
        builder_appendf(body, "    else return %s(0);\n", return_type_name);
    return rc;
}

static int emit_cfg_branch(const gpu_ir_shader *shader, const gpu_ir_shader_function *function,
                           uint32_t current_label, const gpu_ir_shader_instruction *instruction,
                           const msl_symbols *symbols, msl_builder *body) {
    uint32_t target;
    if (instruction->operand_count != 1 || instruction->literal_word_count ||
        !instruction_id_operand(shader, instruction, 0, &target) ||
        !find_function_block(shader, function, target, NULL)) return GPU_IR_INVALID;
    (void)symbols;
    return builder_appendf(body, "    vx_pred = %uu;\n    vx_block = %uu;\n    continue;\n",
                           current_label, target);
}

static int emit_cfg_conditional_branch(const gpu_ir_shader *shader,
                                      const gpu_ir_shader_function *function, uint32_t current_label,
                                      const gpu_ir_shader_instruction *instruction,
                                      const msl_symbols *symbols, msl_builder *body) {
    uint32_t condition_id, on_true, on_false;
    if ((instruction->operand_count != 3 && instruction->operand_count != 5) ||
        instruction->literal_word_count != (instruction->operand_count == 5 ? 2u : 0u) ||
        !instruction_id_operand(shader, instruction, 0, &condition_id) ||
        !instruction_id_operand(shader, instruction, 1, &on_true) ||
        !instruction_id_operand(shader, instruction, 2, &on_false) ||
        !find_function_block(shader, function, on_true, NULL) ||
        !find_function_block(shader, function, on_false, NULL)) return GPU_IR_INVALID;
    const msl_symbol *condition = symbols_find(symbols, condition_id);
    const gpu_ir_shader_type *condition_type = condition ? shader_type(shader, condition->type_id) : NULL;
    if (!condition || condition->is_pointer || !condition_type || condition_type->kind != GPU_IR_SHADER_TYPE_BOOL)
        return GPU_IR_UNSUPPORTED;
    int rc = builder_appendf(body,
        "    if (%s) { vx_pred = %uu; vx_block = %uu; } else { vx_pred = %uu; vx_block = %uu; }\n"
        "    continue;\n", condition->expression, current_label, on_true, current_label, on_false);
    return rc;
}

static int emit_cfg_switch(const gpu_ir_shader *shader, const gpu_ir_shader_function *function,
                           uint32_t current_label, const gpu_ir_shader_instruction *instruction,
                           const msl_symbols *symbols, msl_builder *body) {
    if (instruction->operand_count < 2 || instruction->literal_word_count != instruction->operand_count - 2)
        return GPU_IR_INVALID;
    uint32_t selector_id, default_label;
    if (!instruction_id_operand(shader, instruction, 0, &selector_id) ||
        !instruction_id_operand(shader, instruction, 1, &default_label) ||
        !find_function_block(shader, function, default_label, NULL)) return GPU_IR_INVALID;
    const msl_symbol *selector = symbols_find(symbols, selector_id);
    const gpu_ir_shader_type *selector_type = selector ? shader_type(shader, selector->type_id) : NULL;
    if (!selector || selector->is_pointer || !selector_type || selector_type->kind != GPU_IR_SHADER_TYPE_INT ||
        selector_type->bit_width != 32 || instruction->literal_word_count != instruction->operand_count - 2)
        return GPU_IR_UNSUPPORTED;
    const char *literal_type = selector_type->signedness ? "as_type<int>" : "uint";
    uint32_t case_count = instruction->operand_count - 2;
    int rc = GPU_IR_OK;
    for (uint32_t i = 0; rc == GPU_IR_OK && i < case_count; i++) {
        const gpu_ir_shader_operand *target_operand = instruction_operand(shader, instruction, i + 2);
        uint32_t literal = gpu_ir_shader_literal_word_at(shader, instruction->first_literal + i);
        if (!target_operand || target_operand->kind != GPU_IR_SHADER_OPERAND_ID ||
            !find_function_block(shader, function, target_operand->value, NULL)) return GPU_IR_INVALID;
        for (uint32_t previous = 0; previous < i; previous++)
            if (gpu_ir_shader_literal_word_at(shader, instruction->first_literal + previous) == literal)
                return GPU_IR_INVALID;
        rc = builder_appendf(body, "    %s (%s == %s(0x%08xu)) { vx_pred = %uu; vx_block = %uu; }\n",
            i ? "else if" : "if", selector->expression, literal_type, literal, current_label,
            target_operand->value);
    }
    if (rc == GPU_IR_OK) rc = builder_appendf(body,
        "%s { vx_pred = %uu; vx_block = %uu; }\n    continue;\n",
        case_count ? "    else" : "    ", current_label, default_label);
    return rc;
}

static int emit_multi_block_function(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                                     const gpu_ir_shader_function *function, msl_symbols *symbols,
                                     msl_builder *body) {
    if (!function || !function->block_count) return GPU_IR_UNSUPPORTED;
    uint32_t function_index = UINT32_MAX;
    for (uint32_t i = 0; i < gpu_ir_shader_function_count(shader); i++)
        if (gpu_ir_shader_function_at(shader, i) == function) { function_index = i; break; }
    if (function_index == UINT32_MAX) return GPU_IR_INVALID;
    const gpu_ir_shader_type *return_type = shader_type(shader, function->result_type_id);
    if (!return_type) return GPU_IR_INVALID;
    char return_type_name[32];
    int returns_value = return_type->kind != GPU_IR_SHADER_TYPE_VOID;
    if (returns_value && !shader_type_name(shader, function->result_type_id,
                                           return_type_name, sizeof return_type_name))
        return GPU_IR_UNSUPPORTED;
    body->assign_values = 1;

    int rc = GPU_IR_OK;
    // Hoist SSA values so a value remains visible when control crosses a block or repeats a loop.
    for (uint32_t i = 0; rc == GPU_IR_OK && i < function->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader,
            function->first_instruction + i);
        if (!instruction || instruction->function_index != function_index) return GPU_IR_INVALID;
        if (instruction->opcode == GPU_IR_SHADER_OP_PARAMETER || !instruction->result_id ||
            instruction->opcode == GPU_IR_SHADER_OP_VARIABLE ||
            instruction->opcode == GPU_IR_SHADER_OP_ACCESS_CHAIN) continue;
        const gpu_ir_shader_type *result_type = shader_type(shader, instruction->result_type_id);
        if (!result_type) return GPU_IR_INVALID;
        // Opaque resources and sampled-image combinations alias shader parameters.
        if (instruction->opcode == GPU_IR_SHADER_OP_LOAD &&
            (result_type->kind == GPU_IR_SHADER_TYPE_IMAGE || result_type->kind == GPU_IR_SHADER_TYPE_SAMPLER ||
             result_type->kind == GPU_IR_SHADER_TYPE_SAMPLED_IMAGE))
            continue;
        if (instruction->opcode == GPU_IR_SHADER_OP_SAMPLED_IMAGE)
            continue;
        if (result_type->kind == GPU_IR_SHADER_TYPE_VOID) {
            if (instruction->opcode != GPU_IR_SHADER_OP_FUNCTION_CALL) return GPU_IR_INVALID;
            continue;
        }
        char type_name[32], value_name[48];
        if (!shader_type_name(shader, instruction->result_type_id, type_name, sizeof type_name))
            return GPU_IR_UNSUPPORTED;
        snprintf(value_name, sizeof value_name, "vx_value_%u", instruction->result_id);
        rc = symbols_add(symbols, instruction->result_id, instruction->result_type_id, 0, value_name);
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "    %s %s = {};\n", type_name, value_name);
    }
    if (rc != GPU_IR_OK) return rc;

    // Function variables have function scope even when control returns to their declaring block in a loop.
    uint32_t entry_block = function->first_block;
    for (uint32_t i = 0; i < function->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader,
            function->first_instruction + i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_VARIABLE) continue;
        if (instruction->function_index != function_index || instruction->block_index != entry_block ||
            instruction->storage_class != GPU_IR_SHADER_STORAGE_FUNCTION) return GPU_IR_UNSUPPORTED;
        rc = emit_function_variable(shader, instruction, symbols, body, symbols);
        if (rc != GPU_IR_OK) return rc;
    }

    // Access-chain expressions can refer to loop-carried values, which are now hoisted above.
    for (uint32_t i = 0; i < function->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader,
            function->first_instruction + i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_ACCESS_CHAIN) continue;
        if (instruction->function_index != function_index) return GPU_IR_INVALID;
        rc = emit_access_chain(shader, instruction, symbols, symbols);
        if (rc != GPU_IR_OK) return rc;
    }

    const gpu_ir_shader_block *entry = gpu_ir_shader_block_at(shader, entry_block);
    if (!entry) return GPU_IR_INVALID;
    rc = builder_appendf(body, "    uint vx_block = %uu;\n    uint vx_pred = 0u;\n    while (true) {\n"
        "        switch (vx_block) {\n", entry->label_id);
    for (uint32_t b = 0; rc == GPU_IR_OK && b < function->block_count; b++) {
        uint32_t block_index = function->first_block + b;
        const gpu_ir_shader_block *block = gpu_ir_shader_block_at(shader, block_index);
        if (!block || block->function_index != function_index) return GPU_IR_INVALID;
        rc = builder_appendf(body, "        case %uu: {\n", block->label_id);
        uint32_t phi_count = 0;
        while (phi_count < block->instruction_count) {
            const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader,
                block->first_instruction + phi_count);
            if (!instruction || instruction->function_index != function_index || instruction->block_index != block_index)
                return GPU_IR_INVALID;
            if (instruction->opcode != GPU_IR_SHADER_OP_PHI) break;
            phi_count++;
        }
        for (uint32_t i = 0; rc == GPU_IR_OK && i < phi_count; i++) {
            const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader,
                block->first_instruction + i);
            rc = emit_cfg_phi(shader, instruction, function, block->label_id, symbols, body);
        }
        for (uint32_t i = 0; rc == GPU_IR_OK && i < phi_count; i++) {
            const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader,
                block->first_instruction + i);
            if (!instruction) return GPU_IR_INVALID;
            rc = builder_appendf(body, "    vx_value_%u = vx_phi_in_%u;\n",
                                 instruction->result_id, instruction->result_id);
        }
        for (uint32_t i = phi_count; rc == GPU_IR_OK && i < block->instruction_count; i++) {
            const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader,
                block->first_instruction + i);
            if (!instruction || instruction->function_index != function_index || instruction->block_index != block_index)
                return GPU_IR_INVALID;
            switch (instruction->opcode) {
            case GPU_IR_SHADER_OP_VARIABLE:
            case GPU_IR_SHADER_OP_ACCESS_CHAIN:
            case GPU_IR_SHADER_OP_SELECTION_MERGE:
            case GPU_IR_SHADER_OP_LOOP_MERGE:
                break;
            case GPU_IR_SHADER_OP_LOAD:
                rc = emit_load(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_IMAGE_READ:
                rc = emit_image_read(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_IMAGE_QUERY:
                rc = emit_image_query(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_ATOMIC:
                rc = emit_atomic(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_IMAGE_WRITE:
                rc = emit_image_write(shader, instruction, symbols, body);
                break;
            case GPU_IR_SHADER_OP_SAMPLED_IMAGE:
                rc = emit_sampled_image(shader, instruction, symbols, symbols);
                break;
            case GPU_IR_SHADER_OP_IMAGE_SAMPLE:
                rc = emit_image_sample(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_STORE:
                rc = emit_store(shader, instruction, symbols, body);
                break;
            case GPU_IR_SHADER_OP_ARITHMETIC:
                rc = emit_arithmetic(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_COMPARE:
                rc = emit_comparison(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_SELECT:
                rc = emit_select(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_COMPOSITE:
                if (instruction->operation == GPU_IR_SHADER_COMPOSITE_CONSTRUCT)
                    rc = emit_composite_construct(shader, instruction, symbols, body, symbols);
                else if (instruction->operation == GPU_IR_SHADER_COMPOSITE_EXTRACT ||
                         instruction->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC)
                    rc = emit_composite_extract(shader, instruction, symbols, body, symbols);
                else if (instruction->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_INSERT_DYNAMIC ||
                         instruction->operation == GPU_IR_SHADER_COMPOSITE_INSERT)
                    rc = emit_composite_vector_insert(shader, instruction, symbols, body, symbols);
                else rc = GPU_IR_UNSUPPORTED;
                break;
            case GPU_IR_SHADER_OP_COPY:
                rc = emit_copy(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_CONVERT:
                rc = emit_convert(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_FUNCTION_CALL:
                rc = emit_function_call(shader, layout, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_ARRAY_LENGTH:
                rc = emit_array_length(shader, instruction, symbols, body, symbols);
                break;
            case GPU_IR_SHADER_OP_BARRIER:
                rc = emit_barrier(shader, instruction, body);
                break;
            case GPU_IR_SHADER_OP_DISCARD:
                rc = emit_discard(shader, instruction, symbols, body);
                break;
            case GPU_IR_SHADER_OP_BRANCH:
                rc = emit_cfg_branch(shader, function, block->label_id, instruction, symbols, body);
                break;
            case GPU_IR_SHADER_OP_BRANCH_CONDITIONAL:
                rc = emit_cfg_conditional_branch(shader, function, block->label_id, instruction, symbols, body);
                break;
            case GPU_IR_SHADER_OP_SWITCH:
                rc = emit_cfg_switch(shader, function, block->label_id, instruction, symbols, body);
                break;
            case GPU_IR_SHADER_OP_RETURN:
                if (returns_value || instruction->result_id || instruction->operand_count ||
                    instruction->literal_word_count)
                    rc = GPU_IR_INVALID;
                else rc = builder_appendf(body, "    return;\n");
                break;
            case GPU_IR_SHADER_OP_RETURN_VALUE: {
                uint32_t value_id;
                const msl_symbol *value;
                if (!returns_value || instruction->result_id || instruction->operand_count != 1 ||
                    instruction->literal_word_count ||
                    !instruction_id_operand(shader, instruction, 0, &value_id)) {
                    rc = GPU_IR_INVALID;
                    break;
                }
                value = symbols_find(symbols, value_id);
                if (!value || value->is_pointer || value->type_id != function->result_type_id) {
                    rc = GPU_IR_UNSUPPORTED;
                    break;
                }
                rc = builder_appendf(body, "    return %s;\n", value->expression);
                break;
            }
            case GPU_IR_SHADER_OP_UNREACHABLE:
                rc = returns_value ? builder_appendf(body, "    return %s(0);\n", return_type_name) :
                                     builder_appendf(body, "    return;\n");
                break;
            case GPU_IR_SHADER_OP_PHI:
                rc = GPU_IR_INVALID;
                break;
            default:
                rc = GPU_IR_UNSUPPORTED;
                break;
            }
        }
        if (rc == GPU_IR_OK) rc = builder_appendf(body, "        }\n");
    }
    if (rc == GPU_IR_OK) rc = returns_value ?
        builder_appendf(body, "        default: return %s(0);\n        }\n    }\n", return_type_name) :
        builder_appendf(body, "        default: return;\n        }\n    }\n");
    return rc;
}

int gpu_ir_msl_emit_compute_with_layout(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                                        char **source, size_t *source_size) {
    if (source) *source = NULL;
    if (source_size) *source_size = 0;
    if (!shader || !source || !source_size || gpu_ir_shader_stage(shader) != GPU_IR_STAGE_COMPUTE)
        return GPU_IR_INVALID;
    int layout_result = validate_compute_layout(layout);
    if (layout_result != GPU_IR_OK) return layout_result;
    uint32_t threadgroup_size[3];
    gpu_ir_shader_threadgroup_size(shader, threadgroup_size);
    if (!threadgroup_size[0] || !threadgroup_size[1] || !threadgroup_size[2]) return GPU_IR_UNSUPPORTED;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) return GPU_IR_INVALID;
        if (instruction->function_index == UINT32_MAX &&
            instruction->opcode != GPU_IR_SHADER_OP_CONSTANT && instruction->opcode != GPU_IR_SHADER_OP_VARIABLE)
            return GPU_IR_UNSUPPORTED;
    }
    if (!gpu_ir_shader_function_count(shader)) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_function *function = gpu_ir_shader_find_function(shader,
        gpu_ir_shader_entry_function(shader));
    if (!function || function->parameter_count) return GPU_IR_UNSUPPORTED;
    uint32_t entry_function_index = UINT32_MAX;
    for (uint32_t i = 0; i < gpu_ir_shader_function_count(shader); i++)
        if (gpu_ir_shader_function_at(shader, i) == function) { entry_function_index = i; break; }
    if (entry_function_index == UINT32_MAX) return GPU_IR_INVALID;

    msl_symbols symbols = { 0 };
    msl_builder parameters = { 0 }, body = { 0 }, helpers = { 0 }, output = { 0 };
    int rc = add_global_parameters(shader, layout, GPU_IR_STAGE_COMPUTE,
                                   &symbols, &parameters, &body);
    if (rc == GPU_IR_OK) rc = add_global_constants(shader, &symbols);
    for (uint32_t i = 0; rc == GPU_IR_OK && i < gpu_ir_shader_function_count(shader); i++) {
        const gpu_ir_shader_function *candidate = gpu_ir_shader_function_at(shader, i);
        if (!candidate) { rc = GPU_IR_INVALID; break; }
        if (candidate->id == function->id) continue;
        rc = append_helper_signature(shader, layout, candidate, 0, NULL, &helpers);
        if (rc == GPU_IR_OK) rc = builder_appendf(&helpers, ";\n");
    }
    for (uint32_t i = 0; rc == GPU_IR_OK && i < gpu_ir_shader_function_count(shader); i++) {
        const gpu_ir_shader_function *candidate = gpu_ir_shader_function_at(shader, i);
        if (!candidate) { rc = GPU_IR_INVALID; break; }
        if (candidate->id != function->id) rc = emit_helper_function(shader, layout, candidate, &helpers);
    }
    const gpu_ir_shader_block *entry_block = gpu_ir_shader_block_at(shader, function->first_block);
    if (!entry_block) rc = GPU_IR_INVALID;
    if (rc == GPU_IR_OK && (function->block_count > 1 ||
        entry_block->terminator_opcode != GPU_IR_SHADER_OP_RETURN))
        rc = emit_multi_block_function(shader, layout, function, &symbols, &body);
    else if (rc == GPU_IR_OK) {
        const gpu_ir_shader_block *block = gpu_ir_shader_block_at(shader, function->first_block);
        if (!block || block->function_index != entry_function_index ||
            block->terminator_opcode != GPU_IR_SHADER_OP_RETURN)
            rc = GPU_IR_UNSUPPORTED;
        for (uint32_t i = 0; rc == GPU_IR_OK && i < block->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, block->first_instruction + i);
        if (!instruction || instruction->function_index != block->function_index ||
            instruction->block_index != function->first_block) { rc = GPU_IR_INVALID; break; }
        switch (instruction->opcode) {
        case GPU_IR_SHADER_OP_VARIABLE:
            rc = emit_function_variable(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_ACCESS_CHAIN:
            rc = emit_access_chain(shader, instruction, &symbols, &symbols);
            break;
        case GPU_IR_SHADER_OP_LOAD:
            rc = emit_load(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_IMAGE_READ:
            rc = emit_image_read(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_IMAGE_QUERY:
            rc = emit_image_query(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_ATOMIC:
            rc = emit_atomic(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_IMAGE_WRITE:
            rc = emit_image_write(shader, instruction, &symbols, &body);
            break;
        case GPU_IR_SHADER_OP_SAMPLED_IMAGE:
            rc = emit_sampled_image(shader, instruction, &symbols, &symbols);
            break;
        case GPU_IR_SHADER_OP_IMAGE_SAMPLE:
            rc = emit_image_sample(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_STORE:
            rc = emit_store(shader, instruction, &symbols, &body);
            break;
        case GPU_IR_SHADER_OP_ARITHMETIC:
            rc = emit_arithmetic(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_COMPARE:
            rc = emit_comparison(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_SELECT:
            rc = emit_select(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_COMPOSITE:
            if (instruction->operation == GPU_IR_SHADER_COMPOSITE_CONSTRUCT)
                rc = emit_composite_construct(shader, instruction, &symbols, &body, &symbols);
            else if (instruction->operation == GPU_IR_SHADER_COMPOSITE_EXTRACT ||
                     instruction->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC)
                rc = emit_composite_extract(shader, instruction, &symbols, &body, &symbols);
            else if (instruction->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_INSERT_DYNAMIC ||
                     instruction->operation == GPU_IR_SHADER_COMPOSITE_INSERT)
                rc = emit_composite_vector_insert(shader, instruction, &symbols, &body, &symbols);
            else
                rc = GPU_IR_UNSUPPORTED;
            break;
        case GPU_IR_SHADER_OP_COPY:
            rc = emit_copy(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_CONVERT:
            rc = emit_convert(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_FUNCTION_CALL:
            rc = emit_function_call(shader, layout, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_ARRAY_LENGTH:
            rc = emit_array_length(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_BARRIER:
            rc = emit_barrier(shader, instruction, &body);
            break;
        case GPU_IR_SHADER_OP_RETURN:
            if (i + 1 != block->instruction_count || instruction->result_id || instruction->operand_count)
                rc = GPU_IR_INVALID;
            else rc = builder_appendf(&body, "    return;\n");
            break;
        default:
            rc = GPU_IR_UNSUPPORTED;
            break;
        }
        }
    }
    if (rc == GPU_IR_OK) rc = builder_appendf(&output,
        "#include <metal_stdlib>\nusing namespace metal;\n\n%s\nkernel void vx_main(%s) {\n%s}\n",
        helpers.data ? helpers.data : "", parameters.data ? parameters.data : "", body.data ? body.data : "");
    if (rc == GPU_IR_OK) {
        *source = output.data;
        *source_size = output.length;
        output.data = NULL;
    }
    symbols_destroy(&symbols);
    free(parameters.data);
    free(body.data);
    free(helpers.data);
    free(output.data);
    return rc;
}

int gpu_ir_msl_emit_compute(const gpu_ir_shader *shader, char **source, size_t *source_size) {
    return gpu_ir_msl_emit_compute_with_layout(shader, NULL, source, source_size);
}

static int graphics_float4(const gpu_ir_shader *shader, uint32_t type_id) {
    const gpu_ir_shader_type *type = shader_type(shader, type_id);
    const gpu_ir_shader_type *component = type && type->kind == GPU_IR_SHADER_TYPE_VECTOR ?
        shader_type(shader, type->element_type_id) : NULL;
    return type && type->kind == GPU_IR_SHADER_TYPE_VECTOR && type->element_count == 4 && component &&
        component->kind == GPU_IR_SHADER_TYPE_FLOAT && component->bit_width == 32;
}

static int graphics_integer32(const gpu_ir_shader *shader, uint32_t type_id) {
    const gpu_ir_shader_type *type = shader_type(shader, type_id);
    return type && type->kind == GPU_IR_SHADER_TYPE_INT && type->bit_width == 32 && type->signedness <= 1;
}

static int graphics_variable_element_type(const gpu_ir_shader *shader,
                                           const gpu_ir_shader_instruction *variable,
                                           const gpu_ir_shader_type **element) {
    const gpu_ir_shader_type *pointer = shader_type(shader, variable->result_type_id);
    if (!pointer || pointer->kind != GPU_IR_SHADER_TYPE_POINTER || pointer->storage_class != variable->storage_class)
        return GPU_IR_UNSUPPORTED;
    *element = shader_type(shader, pointer->element_type_id);
    return *element ? GPU_IR_OK : GPU_IR_INVALID;
}

enum { MSL_GRAPHICS_MAX_VARYINGS = 31 };

typedef struct {
    uint32_t location, type_id, variable_id;
} msl_graphics_varying;

static const uint32_t graphics_unsupported_interface_qualifiers =
    GPU_IR_SHADER_DECORATION_NO_PERSPECTIVE | GPU_IR_SHADER_DECORATION_FLAT |
    GPU_IR_SHADER_DECORATION_CENTROID | GPU_IR_SHADER_DECORATION_SAMPLE |
    GPU_IR_SHADER_DECORATION_INVARIANT;

static int graphics_user_varying_type_name(const gpu_ir_shader *shader, uint32_t type_id,
                                           char *name, size_t capacity) {
    const gpu_ir_shader_type *type = shader_type(shader, type_id);
    if (!type || !name || !capacity) return 0;
    if (type->kind == GPU_IR_SHADER_TYPE_FLOAT && type->bit_width == 32)
        return snprintf(name, capacity, "float") < (int)capacity;
    if (type->kind == GPU_IR_SHADER_TYPE_VECTOR && type->element_count >= 2 && type->element_count <= 4) {
        const gpu_ir_shader_type *component = shader_type(shader, type->element_type_id);
        return component && component->kind == GPU_IR_SHADER_TYPE_FLOAT && component->bit_width == 32 &&
            snprintf(name, capacity, "float%u", type->element_count) < (int)capacity;
    }
    return 0;
}

static int graphics_vertex_attribute_type_name(const gpu_ir_shader *shader, uint32_t type_id,
                                               char *name, size_t capacity) {
    if (graphics_user_varying_type_name(shader, type_id, name, capacity)) return 1;
    const gpu_ir_shader_type *type = shader_type(shader, type_id);
    if (!type || type->kind != GPU_IR_SHADER_TYPE_INT || type->bit_width != 32 ||
        type->signedness > 1 || !name || !capacity) return 0;
    return snprintf(name, capacity, "%s", type->signedness ? "int" : "uint") < (int)capacity;
}

static uint32_t graphics_user_varying_component_count(const gpu_ir_shader *shader, uint32_t type_id) {
    const gpu_ir_shader_type *type = shader_type(shader, type_id);
    if (type && type->kind == GPU_IR_SHADER_TYPE_FLOAT && type->bit_width == 32) return 1;
    if (type && type->kind == GPU_IR_SHADER_TYPE_VECTOR && type->element_count >= 2 && type->element_count <= 4) {
        const gpu_ir_shader_type *component = shader_type(shader, type->element_type_id);
        if (component && component->kind == GPU_IR_SHADER_TYPE_FLOAT && component->bit_width == 32)
            return type->element_count;
    }
    return 0;
}

static int graphics_user_varying(const gpu_ir_shader *shader,
                                 const gpu_ir_shader_instruction *variable,
                                 char type_name[32]) {
    const gpu_ir_shader_type *element = NULL;
    if (!variable || variable->has_builtin || !variable->has_location || variable->location >= 31 ||
        (variable->has_component && variable->component != 0) ||
        (variable->has_index && variable->index != 0) ||
        (variable->decoration_flags & graphics_unsupported_interface_qualifiers) ||
        graphics_variable_element_type(shader, variable, &element) != GPU_IR_OK ||
        !graphics_user_varying_type_name(shader, element->id, type_name, 32)) return GPU_IR_UNSUPPORTED;
    return GPU_IR_OK;
}

static int graphics_varying_add(msl_graphics_varying *varyings, uint32_t *count,
                                const gpu_ir_shader_instruction *variable, uint32_t type_id) {
    if (!varyings || !count || !variable || *count >= MSL_GRAPHICS_MAX_VARYINGS)
        return GPU_IR_UNSUPPORTED;
    for (uint32_t i = 0; i < *count; i++)
        if (varyings[i].location == variable->location) return GPU_IR_INVALID;
    varyings[(*count)++] = (msl_graphics_varying){
        .location = variable->location,
        .type_id = type_id,
        .variable_id = variable->result_id
    };
    return GPU_IR_OK;
}

int gpu_ir_msl_validate_graphics_link(const gpu_ir_shader *vertex_shader,
                                      const gpu_ir_shader *fragment_shader,
                                      uint32_t max_vertex_output_components,
                                      uint32_t max_fragment_input_components) {
    if (!vertex_shader || !fragment_shader ||
        gpu_ir_shader_stage(vertex_shader) != GPU_IR_STAGE_VERTEX ||
        gpu_ir_shader_stage(fragment_shader) != GPU_IR_STAGE_FRAGMENT ||
        max_vertex_output_components < 4 || max_fragment_input_components < 1) return GPU_IR_INVALID;
    uint32_t vertex_components = 0, fragment_components = 0;
    int has_position = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(vertex_shader); i++) {
        const gpu_ir_shader_instruction *output = gpu_ir_shader_instruction_at(vertex_shader, i);
        if (!output || output->opcode != GPU_IR_SHADER_OP_VARIABLE || output->function_index != UINT32_MAX ||
            !output->entry_point_interface || output->storage_class != GPU_IR_SHADER_STORAGE_OUTPUT) continue;
        if (output->decoration_flags & graphics_unsupported_interface_qualifiers) return GPU_IR_UNSUPPORTED;
        uint32_t components = 0;
        if (output->has_builtin && output->builtin == GPU_IR_SHADER_BUILTIN_POSITION) {
            const gpu_ir_shader_type *element = NULL;
            if (has_position || output->has_location ||
                graphics_variable_element_type(vertex_shader, output, &element) != GPU_IR_OK ||
                !graphics_float4(vertex_shader, element->id)) return GPU_IR_UNSUPPORTED;
            components = 4;
            has_position = 1;
        } else if (!output->has_builtin && output->has_location) {
            char type_name[32];
            if (graphics_user_varying(vertex_shader, output, type_name) != GPU_IR_OK ||
                output->location >= MSL_GRAPHICS_MAX_VARYINGS) return GPU_IR_UNSUPPORTED;
            const gpu_ir_shader_type *element = NULL;
            if (graphics_variable_element_type(vertex_shader, output, &element) != GPU_IR_OK) return GPU_IR_INVALID;
            components = graphics_user_varying_component_count(vertex_shader, element->id);
        } else {
            return GPU_IR_UNSUPPORTED;
        }
        if (!components || components > max_vertex_output_components - vertex_components)
            return GPU_IR_UNSUPPORTED;
        vertex_components += components;
    }
    if (!has_position) return GPU_IR_UNSUPPORTED;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(fragment_shader); i++) {
        const gpu_ir_shader_instruction *input = gpu_ir_shader_instruction_at(fragment_shader, i);
        if (!input || input->opcode != GPU_IR_SHADER_OP_VARIABLE || input->function_index != UINT32_MAX ||
            !input->entry_point_interface || input->storage_class != GPU_IR_SHADER_STORAGE_INPUT) continue;
        if (input->decoration_flags & graphics_unsupported_interface_qualifiers) return GPU_IR_UNSUPPORTED;
        if (input->has_builtin) {
            uint32_t components = 0;
            if (input->builtin == GPU_IR_SHADER_BUILTIN_FRAG_COORD) components = 4;
            else if (input->builtin == GPU_IR_SHADER_BUILTIN_FRONT_FACING) components = 1;
            else return GPU_IR_UNSUPPORTED;
            if (components > max_fragment_input_components - fragment_components)
                return GPU_IR_UNSUPPORTED;
            fragment_components += components;
            continue;
        }
        char input_type_name[32];
        if (graphics_user_varying(fragment_shader, input, input_type_name) != GPU_IR_OK)
            return GPU_IR_UNSUPPORTED;
        if (input->location >= MSL_GRAPHICS_MAX_VARYINGS) return GPU_IR_UNSUPPORTED;
        const gpu_ir_shader_type *input_element = NULL;
        if (graphics_variable_element_type(fragment_shader, input, &input_element) != GPU_IR_OK)
            return GPU_IR_INVALID;
        uint32_t components = graphics_user_varying_component_count(fragment_shader, input_element->id);
        if (!components || components > max_fragment_input_components - fragment_components)
            return GPU_IR_UNSUPPORTED;
        fragment_components += components;
        uint32_t matches = 0;
        for (uint32_t j = 0; j < gpu_ir_shader_instruction_count(vertex_shader); j++) {
            const gpu_ir_shader_instruction *output = gpu_ir_shader_instruction_at(vertex_shader, j);
            if (!output || output->opcode != GPU_IR_SHADER_OP_VARIABLE || output->function_index != UINT32_MAX ||
                !output->entry_point_interface || output->storage_class != GPU_IR_SHADER_STORAGE_OUTPUT ||
                output->has_builtin || !output->has_location || output->location != input->location) continue;
            char output_type_name[32];
            if (graphics_user_varying(vertex_shader, output, output_type_name) != GPU_IR_OK ||
                strcmp(input_type_name, output_type_name)) return GPU_IR_UNSUPPORTED;
            matches++;
        }
        if (matches != 1) return GPU_IR_UNSUPPORTED;
    }
    return GPU_IR_OK;
}

static int emit_graphics_io(const gpu_ir_shader *shader, uint32_t stage, msl_symbols *symbols,
                            msl_builder *parameters, msl_builder *body,
                            msl_graphics_varying *varyings, uint32_t *varying_count,
                            char output_name[48]) {
    uint32_t required_output_count = 0, interface_components = 0;
    int vertex_stage_in_added = 0, vertex_base_instance_added = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
        if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE ||
            variable->function_index != UINT32_MAX || !variable->entry_point_interface) continue;
        if (variable->storage_class != GPU_IR_SHADER_STORAGE_INPUT &&
            variable->storage_class != GPU_IR_SHADER_STORAGE_OUTPUT) return GPU_IR_UNSUPPORTED;
        const gpu_ir_shader_type *element = NULL;
        int rc = graphics_variable_element_type(shader, variable, &element);
        if (rc != GPU_IR_OK) return rc;
        if (variable->decoration_flags & graphics_unsupported_interface_qualifiers)
            return GPU_IR_UNSUPPORTED;
        char name[48];
        snprintf(name, sizeof name, "vx_builtin_%u", variable->result_id);
        if (variable->storage_class == GPU_IR_SHADER_STORAGE_INPUT) {
            const char *attribute = NULL;
            char type_name[32];
            if (stage == GPU_IR_STAGE_VERTEX &&
                variable->has_builtin && (variable->builtin == GPU_IR_SHADER_BUILTIN_VERTEX_INDEX ||
                 variable->builtin == GPU_IR_SHADER_BUILTIN_INSTANCE_INDEX ||
                 variable->builtin == GPU_IR_SHADER_BUILTIN_INSTANCE_ID)) {
                if (!graphics_integer32(shader, element->id)) return GPU_IR_UNSUPPORTED;
                attribute = variable->builtin == GPU_IR_SHADER_BUILTIN_VERTEX_INDEX ? "vertex_id" : "instance_id";
                if (snprintf(type_name, sizeof type_name, "uint") >= (int)sizeof type_name)
                    return GPU_IR_RANGE;
            } else if (stage == GPU_IR_STAGE_FRAGMENT && variable->has_builtin &&
                       variable->builtin == GPU_IR_SHADER_BUILTIN_FRAG_COORD) {
                if (!graphics_float4(shader, element->id)) return GPU_IR_UNSUPPORTED;
                attribute = "position";
                if (snprintf(type_name, sizeof type_name, "float4") >= (int)sizeof type_name)
                    return GPU_IR_RANGE;
            } else if (stage == GPU_IR_STAGE_FRAGMENT && variable->has_builtin &&
                       variable->builtin == GPU_IR_SHADER_BUILTIN_FRONT_FACING) {
                if (element->kind != GPU_IR_SHADER_TYPE_BOOL) return GPU_IR_UNSUPPORTED;
                attribute = "front_facing";
                if (snprintf(type_name, sizeof type_name, "bool") >= (int)sizeof type_name)
                    return GPU_IR_RANGE;
            } else if (stage == GPU_IR_STAGE_FRAGMENT && !variable->has_builtin && variable->has_location) {
                rc = graphics_user_varying(shader, variable, type_name);
                if (rc != GPU_IR_OK) return rc;
                uint32_t components = graphics_user_varying_component_count(shader, element->id);
                if (!components || components > 124u - interface_components) return GPU_IR_UNSUPPORTED;
                interface_components += components;
                rc = graphics_varying_add(varyings, varying_count, variable, element->id);
                if (rc != GPU_IR_OK) return rc;
                char expression[80];
                if (snprintf(expression, sizeof expression, "vx_input.vx_location_%u", variable->location) >=
                    (int)sizeof expression) return GPU_IR_RANGE;
                rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, expression);
                if (rc != GPU_IR_OK) return rc;
                continue;
            } else if (stage == GPU_IR_STAGE_VERTEX && !variable->has_builtin && variable->has_location) {
                if (variable->location >= 31 || (variable->has_component && variable->component != 0) ||
                    (variable->has_index && variable->index != 0) ||
                    !graphics_vertex_attribute_type_name(shader, element->id, type_name, sizeof type_name))
                    return GPU_IR_UNSUPPORTED;
                for (uint32_t j = 0; j < i; j++) {
                    const gpu_ir_shader_instruction *prior = gpu_ir_shader_instruction_at(shader, j);
                    if (prior && prior->opcode == GPU_IR_SHADER_OP_VARIABLE &&
                        prior->function_index == UINT32_MAX && prior->entry_point_interface &&
                        prior->storage_class == GPU_IR_SHADER_STORAGE_INPUT && !prior->has_builtin &&
                        prior->has_location && prior->location == variable->location) return GPU_IR_INVALID;
                }
                if (!vertex_stage_in_added) {
                    if (parameters->length && builder_appendf(parameters, ", ") != GPU_IR_OK)
                        return GPU_IR_OUT_OF_MEMORY;
                    rc = builder_appendf(parameters, "VxVertexInput vx_input [[stage_in]]");
                    vertex_stage_in_added = 1;
                }
                char expression[80];
                if (snprintf(expression, sizeof expression, "vx_input.vx_attribute_%u", variable->location) >=
                    (int)sizeof expression) return GPU_IR_RANGE;
                if (rc == GPU_IR_OK)
                    rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, expression);
                if (rc != GPU_IR_OK) return rc;
                continue;
            } else return GPU_IR_UNSUPPORTED;
            if (stage == GPU_IR_STAGE_FRAGMENT && variable->has_builtin) {
                uint32_t components = variable->builtin == GPU_IR_SHADER_BUILTIN_FRAG_COORD ? 4u : 1u;
                if (components > 124u - interface_components) return GPU_IR_UNSUPPORTED;
                interface_components += components;
            }
            for (uint32_t j = 0; j < i; j++) {
                const gpu_ir_shader_instruction *prior = gpu_ir_shader_instruction_at(shader, j);
                int instance_attribute_collision = prior && stage == GPU_IR_STAGE_VERTEX &&
                    ((prior->builtin == GPU_IR_SHADER_BUILTIN_INSTANCE_INDEX ||
                      prior->builtin == GPU_IR_SHADER_BUILTIN_INSTANCE_ID) &&
                     (variable->builtin == GPU_IR_SHADER_BUILTIN_INSTANCE_INDEX ||
                      variable->builtin == GPU_IR_SHADER_BUILTIN_INSTANCE_ID));
                if (prior && prior->opcode == GPU_IR_SHADER_OP_VARIABLE &&
                    prior->function_index == UINT32_MAX && prior->entry_point_interface &&
                    prior->storage_class == GPU_IR_SHADER_STORAGE_INPUT &&
                    (prior->builtin == variable->builtin || instance_attribute_collision))
                    return GPU_IR_INVALID;
            }
            if (stage == GPU_IR_STAGE_VERTEX && variable->builtin == GPU_IR_SHADER_BUILTIN_INSTANCE_ID &&
                !vertex_base_instance_added) {
                if (parameters->length && builder_appendf(parameters, ", ") != GPU_IR_OK)
                    return GPU_IR_OUT_OF_MEMORY;
                rc = builder_appendf(parameters, "uint vx_base_instance [[base_instance]]");
                if (rc != GPU_IR_OK) return rc;
                vertex_base_instance_added = 1;
            }
            if (parameters->length && builder_appendf(parameters, ", ") != GPU_IR_OK)
                return GPU_IR_OUT_OF_MEMORY;
            rc = builder_appendf(parameters, "%s %s [[%s]]", type_name, name, attribute);
            if (rc != GPU_IR_OK) return rc;
            char expression[80];
            const char *input_expression = name;
            if (stage == GPU_IR_STAGE_VERTEX && variable->has_builtin &&
                variable->builtin == GPU_IR_SHADER_BUILTIN_INSTANCE_ID) {
                if (snprintf(expression, sizeof expression, "%s - vx_base_instance", name) >=
                    (int)sizeof expression) return GPU_IR_RANGE;
                input_expression = expression;
            } else if (stage == GPU_IR_STAGE_VERTEX && element->signedness) {
                if (snprintf(expression, sizeof expression, "as_type<int>(%s)", name) >=
                    (int)sizeof expression) return GPU_IR_RANGE;
                input_expression = expression;
            }
            rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, input_expression);
            if (rc != GPU_IR_OK) return rc;
            continue;
        }

        if (stage == GPU_IR_STAGE_VERTEX) {
            if (variable->has_builtin && variable->builtin == GPU_IR_SHADER_BUILTIN_POSITION) {
                if (variable->has_location || !graphics_float4(shader, element->id) || required_output_count++)
                    return GPU_IR_UNSUPPORTED;
                if (interface_components > 124u) return GPU_IR_UNSUPPORTED;
                interface_components += 4u;
            } else if (!variable->has_builtin && variable->has_location) {
                char type_name[32];
                rc = graphics_user_varying(shader, variable, type_name);
                if (rc != GPU_IR_OK) return rc;
                uint32_t components = graphics_user_varying_component_count(shader, element->id);
                if (!components || components > 128u - interface_components) return GPU_IR_UNSUPPORTED;
                interface_components += components;
                rc = graphics_varying_add(varyings, varying_count, variable, element->id);
                if (rc != GPU_IR_OK) return rc;
            } else return GPU_IR_UNSUPPORTED;
        } else {
            if (variable->has_builtin || !variable->has_location || variable->location != 0 ||
                (variable->has_component && variable->component != 0) ||
                (variable->has_index && variable->index != 0) || !graphics_float4(shader, element->id))
                return GPU_IR_UNSUPPORTED;
            if (required_output_count++) return GPU_IR_UNSUPPORTED;
        }
        char type_name[32];
        if (!shader_type_name(shader, element->id, type_name, sizeof type_name)) return GPU_IR_UNSUPPORTED;
        snprintf(name, sizeof name, "vx_io_%u", variable->result_id);
        rc = builder_appendf(body, "    %s %s = %s(0.0f);\n", type_name, name, type_name);
        if (rc == GPU_IR_OK)
            rc = symbols_add(symbols, variable->result_id, variable->result_type_id, 1, name);
        if (rc != GPU_IR_OK) return rc;
        if (stage == GPU_IR_STAGE_FRAGMENT ||
            (variable->has_builtin && variable->builtin == GPU_IR_SHADER_BUILTIN_POSITION))
            snprintf(output_name, 48, "%s", name);
    }
    if (stage == GPU_IR_STAGE_FRAGMENT && *varying_count) {
        if (parameters->length && builder_appendf(parameters, ", ") != GPU_IR_OK)
            return GPU_IR_OUT_OF_MEMORY;
        int rc = builder_appendf(parameters, "VxFragmentInput vx_input [[stage_in]]");
        if (rc != GPU_IR_OK) return rc;
    }
    return required_output_count == 1 ? GPU_IR_OK : GPU_IR_UNSUPPORTED;
}

static int emit_graphics_interface_struct(const gpu_ir_shader *shader, uint32_t stage,
                                          const msl_graphics_varying *varyings,
                                          uint32_t varying_count, msl_builder *output) {
    if (stage == GPU_IR_STAGE_FRAGMENT && !varying_count) return GPU_IR_OK;
    int rc = GPU_IR_OK;
    if (stage == GPU_IR_STAGE_VERTEX) {
        uint32_t attribute_count = 0;
        for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
            const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
            if (variable && variable->opcode == GPU_IR_SHADER_OP_VARIABLE &&
                variable->function_index == UINT32_MAX && variable->entry_point_interface &&
                variable->storage_class == GPU_IR_SHADER_STORAGE_INPUT && !variable->has_builtin &&
                variable->has_location) attribute_count++;
        }
        if (attribute_count) {
            rc = builder_appendf(output, "struct VxVertexInput {");
            for (uint32_t i = 0; rc == GPU_IR_OK && i < gpu_ir_shader_instruction_count(shader); i++) {
                const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
                if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE ||
                    variable->function_index != UINT32_MAX || !variable->entry_point_interface ||
                    variable->storage_class != GPU_IR_SHADER_STORAGE_INPUT || variable->has_builtin ||
                    !variable->has_location) continue;
                const gpu_ir_shader_type *element = NULL;
                char type_name[32];
                int type_rc = graphics_variable_element_type(shader, variable, &element);
                if (type_rc != GPU_IR_OK ||
                    !graphics_vertex_attribute_type_name(shader, element->id, type_name, sizeof type_name))
                    return GPU_IR_UNSUPPORTED;
                rc = builder_appendf(output, "\n    %s vx_attribute_%u [[attribute(%u)]];", type_name,
                                     variable->location, variable->location);
            }
            if (rc == GPU_IR_OK) rc = builder_appendf(output, "\n};\n\n");
        }
        if (rc == GPU_IR_OK)
            rc = builder_appendf(output, "struct VxVertexOutput { float4 position [[position]];");
    } else {
        rc = builder_appendf(output, "struct VxFragmentInput {");
    }
    for (uint32_t i = 0; rc == GPU_IR_OK && i < varying_count; i++) {
        char type_name[32];
        if (!graphics_user_varying_type_name(shader, varyings[i].type_id, type_name, sizeof type_name))
            return GPU_IR_UNSUPPORTED;
        rc = builder_appendf(output, "\n    %s vx_location_%u [[user(locn%u)]];",
                            type_name, varyings[i].location, varyings[i].location);
    }
    if (rc == GPU_IR_OK) rc = builder_appendf(output, "\n};\n\n");
    return rc;
}

static int emit_graphics_vertex_return(msl_builder *body, const char *position_name,
                                       const msl_graphics_varying *varyings, uint32_t varying_count) {
    int rc = builder_appendf(body,
        "    VxVertexOutput vx_output;\n    vx_output.position = %s;\n", position_name);
    for (uint32_t i = 0; rc == GPU_IR_OK && i < varying_count; i++)
        rc = builder_appendf(body, "    vx_output.vx_location_%u = vx_io_%u;\n",
                            varyings[i].location, varyings[i].variable_id);
    if (rc == GPU_IR_OK) rc = builder_appendf(body, "    return vx_output;\n");
    return rc;
}

static int emit_graphics_stage(const gpu_ir_shader *shader, const gpu_ir_pipeline_layout *layout,
                               uint32_t stage,
                               char **source, size_t *source_size) {
    if (source) *source = NULL;
    if (source_size) *source_size = 0;
    if (!shader || !source || !source_size || gpu_ir_shader_stage(shader) != stage)
        return GPU_IR_INVALID;
    if (gpu_ir_shader_function_count(shader) != 1) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_function *function = gpu_ir_shader_find_function(shader,
        gpu_ir_shader_entry_function(shader));
    if (!function || function->parameter_count || function->block_count != 1) return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_type *return_type = shader_type(shader, function->result_type_id);
    const gpu_ir_shader_type *function_type = shader_type(shader, function->function_type_id);
    if (!return_type || return_type->kind != GPU_IR_SHADER_TYPE_VOID || !function_type ||
        function_type->kind != GPU_IR_SHADER_TYPE_FUNCTION || function_type->member_count != 0)
        return GPU_IR_UNSUPPORTED;
    const gpu_ir_shader_block *block = gpu_ir_shader_block_at(shader, function->first_block);
    if (!block || block->function_index != 0 ||
        (block->terminator_opcode != GPU_IR_SHADER_OP_RETURN &&
         block->terminator_opcode != GPU_IR_SHADER_OP_TERMINATE_INVOCATION))
        return GPU_IR_UNSUPPORTED;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) return GPU_IR_INVALID;
        if (instruction->function_index == UINT32_MAX &&
            instruction->opcode != GPU_IR_SHADER_OP_CONSTANT && instruction->opcode != GPU_IR_SHADER_OP_VARIABLE)
            return GPU_IR_UNSUPPORTED;
    }

    msl_symbols symbols = { 0 };
    msl_builder parameters = { 0 }, body = { 0 }, output = { 0 };
    msl_graphics_varying varyings[MSL_GRAPHICS_MAX_VARYINGS] = { 0 };
    uint32_t varying_count = 0;
    char output_name[48] = { 0 };
    int rc = emit_graphics_io(shader, stage, &symbols, &parameters, &body,
                              varyings, &varying_count, output_name);
    if (rc == GPU_IR_OK) rc = add_global_parameters(shader, layout, stage,
                                                     &symbols, &parameters, &body);
    if (rc == GPU_IR_OK) rc = add_global_constants(shader, &symbols);
    for (uint32_t i = 0; rc == GPU_IR_OK && i < block->instruction_count; i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader,
            block->first_instruction + i);
        if (!instruction || instruction->function_index != 0 || instruction->block_index != function->first_block) {
            rc = GPU_IR_INVALID;
            break;
        }
        switch (instruction->opcode) {
        case GPU_IR_SHADER_OP_VARIABLE:
            rc = emit_function_variable(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_LOAD:
            rc = emit_load(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_STORE:
            rc = emit_store(shader, instruction, &symbols, &body);
            break;
        case GPU_IR_SHADER_OP_ARITHMETIC:
            rc = emit_arithmetic(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_COMPARE:
            rc = emit_comparison(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_SELECT:
            rc = emit_select(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_COMPOSITE:
            if (instruction->operation == GPU_IR_SHADER_COMPOSITE_CONSTRUCT)
                rc = emit_composite_construct(shader, instruction, &symbols, &body, &symbols);
            else if (instruction->operation == GPU_IR_SHADER_COMPOSITE_EXTRACT)
                rc = emit_composite_extract(shader, instruction, &symbols, &body, &symbols);
            else
                rc = GPU_IR_UNSUPPORTED;
            break;
        case GPU_IR_SHADER_OP_CONVERT:
            rc = emit_convert(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_COPY:
            rc = emit_copy(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_SAMPLED_IMAGE:
            rc = emit_sampled_image(shader, instruction, &symbols, &symbols);
            break;
        case GPU_IR_SHADER_OP_IMAGE_SAMPLE:
            rc = emit_image_sample(shader, instruction, &symbols, &body, &symbols);
            break;
        case GPU_IR_SHADER_OP_DISCARD:
            rc = emit_discard(shader, instruction, &symbols, &body);
            break;
        case GPU_IR_SHADER_OP_TERMINATE_INVOCATION:
            if (stage != GPU_IR_STAGE_FRAGMENT || i + 1 != block->instruction_count ||
                instruction->result_id || instruction->result_type_id || instruction->operand_count ||
                instruction->literal_word_count)
                rc = GPU_IR_INVALID;
            else rc = builder_appendf(&body,
                "    discard_fragment();\n    return %s;\n", output_name);
            break;
        case GPU_IR_SHADER_OP_RETURN:
            if (i + 1 != block->instruction_count || instruction->result_id || instruction->operand_count) {
                rc = GPU_IR_INVALID;
            } else if (stage == GPU_IR_STAGE_VERTEX) {
                rc = emit_graphics_vertex_return(&body, output_name, varyings, varying_count);
            } else {
                rc = builder_appendf(&body, "    return %s;\n", output_name);
            }
            break;
        default:
            rc = GPU_IR_UNSUPPORTED;
            break;
        }
        if (rc != GPU_IR_OK) break;
    }
    if (rc == GPU_IR_OK)
        rc = builder_appendf(&output, "#include <metal_stdlib>\nusing namespace metal;\n\n");
    if (rc == GPU_IR_OK) rc = emit_graphics_interface_struct(shader, stage, varyings, varying_count, &output);
    if (rc == GPU_IR_OK && stage == GPU_IR_STAGE_VERTEX)
        rc = builder_appendf(&output,
            "vertex VxVertexOutput vx_vertex(%s) {\n%s}\n",
            parameters.data ? parameters.data : "", body.data ? body.data : "");
    else if (rc == GPU_IR_OK)
        rc = builder_appendf(&output,
            "fragment float4 vx_fragment(%s) {\n%s}\n",
            parameters.data ? parameters.data : "", body.data ? body.data : "");
    if (rc == GPU_IR_OK) {
        *source = output.data;
        *source_size = output.length;
        output.data = NULL;
    }
    symbols_destroy(&symbols);
    free(parameters.data);
    free(body.data);
    free(output.data);
    return rc;
}

int gpu_ir_msl_emit_vertex(const gpu_ir_shader *shader, char **source, size_t *source_size) {
    return emit_graphics_stage(shader, NULL, GPU_IR_STAGE_VERTEX, source, source_size);
}

int gpu_ir_msl_emit_fragment(const gpu_ir_shader *shader, char **source, size_t *source_size) {
    return emit_graphics_stage(shader, NULL, GPU_IR_STAGE_FRAGMENT, source, source_size);
}

int gpu_ir_msl_emit_vertex_with_layout(const gpu_ir_shader *shader,
                                      const gpu_ir_pipeline_layout *layout,
                                      char **source, size_t *source_size) {
    return emit_graphics_stage(shader, layout, GPU_IR_STAGE_VERTEX, source, source_size);
}

int gpu_ir_msl_emit_fragment_with_layout(const gpu_ir_shader *shader,
                                         const gpu_ir_pipeline_layout *layout,
                                         char **source, size_t *source_size) {
    return emit_graphics_stage(shader, layout, GPU_IR_STAGE_FRAGMENT, source, source_size);
}
