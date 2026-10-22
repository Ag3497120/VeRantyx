// SPIR-V module framing reader. Semantic instruction decoding is layered on top of this word stream.
#include "spirv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t target_id, member_index, decoration, value;
    uint32_t has_value, is_member;
    size_t word_offset;
} pending_decoration;

typedef struct {
    uint32_t id;
    size_t word_offset;
} pending_decoration_group;

typedef struct {
    uint32_t group_id, target_id, member_index, is_member;
    size_t word_offset;
} pending_group_application;

static uint32_t load_word_le(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t swap_word(uint32_t value) {
    return (value >> 24) | ((value >> 8) & 0x0000ff00u) |
           ((value << 8) & 0x00ff0000u) | (value << 24);
}

static size_t id_hash(uint32_t id) {
    return (size_t)(id * 2654435761u);
}

static spirv_id_record *id_slots(spirv_module *module) {
    return module->id_records;
}

const spirv_id_record *spirv_find_id(const spirv_module *module, uint32_t id) {
    if (!module || !id || !module->id_records || !module->id_capacity) return NULL;
    const spirv_id_record *slots = module->id_records;
    size_t index = id_hash(id) & (module->id_capacity - 1);
    for (size_t n = 0; n < module->id_capacity; n++) {
        if (!slots[index].id) return NULL;
        if (slots[index].id == id) return &slots[index];
        index = (index + 1) & (module->id_capacity - 1);
    }
    return NULL;
}

const char *spirv_ext_inst_import_name(const spirv_module *module, uint32_t id) {
    if (!module || !id) return NULL;
    for (uint32_t i = 0; i < module->ext_inst_import_count; i++)
        if (module->ext_inst_imports[i].id == id) return module->ext_inst_imports[i].name;
    return NULL;
}

static int id_table_grow(spirv_module *module) {
    size_t capacity = module->id_capacity ? module->id_capacity * 2 : 64;
    if (capacity < module->id_capacity || capacity > SIZE_MAX / sizeof(spirv_id_record)) return SPIRV_OUT_OF_MEMORY;
    spirv_id_record *slots = calloc(capacity, sizeof *slots);
    if (!slots) return SPIRV_OUT_OF_MEMORY;
    spirv_id_record *old = id_slots(module);
    for (size_t i = 0; i < module->id_capacity; i++) {
        if (!old[i].id) continue;
        size_t index = id_hash(old[i].id) & (capacity - 1);
        while (slots[index].id) index = (index + 1) & (capacity - 1);
        slots[index] = old[i];
    }
    free(old);
    module->id_records = slots;
    module->id_capacity = capacity;
    return SPIRV_OK;
}

static int id_table_insert(spirv_module *module, const spirv_id_record *record) {
    if (!record->id || record->id >= module->id_bound) return SPIRV_INVALID;
    if (!module->id_capacity || module->id_count >= module->id_capacity - module->id_capacity / 3) {
        int rc = id_table_grow(module);
        if (rc != SPIRV_OK) return rc;
    }
    spirv_id_record *slots = id_slots(module);
    size_t index = id_hash(record->id) & (module->id_capacity - 1);
    while (slots[index].id) {
        if (slots[index].id == record->id) return SPIRV_INVALID;
        index = (index + 1) & (module->id_capacity - 1);
    }
    slots[index] = *record;
    module->id_count++;
    return SPIRV_OK;
}

static void release_contents(spirv_module *module) {
    for (uint32_t i = 0; i < module->entry_point_count; i++) {
        free(module->entry_points[i].name);
        free(module->entry_points[i].interfaces);
    }
    free(module->entry_points);
    module->entry_points = NULL;
    module->entry_point_count = 0;
    free(module->functions);
    module->functions = NULL;
    module->function_count = module->function_capacity = 0;
    free(module->basic_blocks);
    module->basic_blocks = NULL;
    module->basic_block_count = module->basic_block_capacity = 0;
    spirv_id_record *slots = id_slots(module);
    for (size_t i = 0; i < module->id_capacity; i++) {
        free(slots[i].literal_words);
        free(slots[i].constituent_ids);
        free(slots[i].specialization_operand_ids);
        free(slots[i].members);
    }
    free(slots);
    module->id_records = NULL;
    module->id_capacity = 0;
    module->id_count = 0;
    free(module->variable_ids);
    module->variable_ids = NULL;
    module->variable_count = module->variable_capacity = 0;
    free(module->decorations);
    module->decorations = NULL;
    module->decoration_count = module->decoration_capacity = 0;
    free(module->decoration_groups);
    module->decoration_groups = NULL;
    module->decoration_group_count = module->decoration_group_capacity = 0;
    free(module->group_applications);
    module->group_applications = NULL;
    module->group_application_count = module->group_application_capacity = 0;
    for (uint32_t i = 0; i < module->ext_inst_import_count; i++) free(module->ext_inst_imports[i].name);
    free(module->ext_inst_imports);
    module->ext_inst_imports = NULL;
    module->ext_inst_import_count = module->ext_inst_import_capacity = 0;
    free(module->words);
    module->words = NULL;
}

static int fail(spirv_module *module, int code, const char *message) {
    snprintf(module->error, sizeof module->error, "%s", message);
    release_contents(module);
    module->word_count = 0;
    module->instruction_count = 0;
    return code;
}

void spirv_free(spirv_module *module) {
    if (!module) return;
    release_contents(module);
    memset(module, 0, sizeof *module);
}

static int id_in_bound(const spirv_module *module, uint32_t id) {
    return id != 0 && id < module->id_bound;
}

static int literal_string(const uint32_t *words, uint32_t first, uint32_t count,
                          char **out, uint32_t *words_used) {
    size_t length = 0;
    int terminated = 0;
    for (uint32_t i = 0; i < count && !terminated; i++) {
        uint32_t word = words[first + i];
        for (uint32_t b = 0; b < 4; b++) {
            uint8_t c = (uint8_t)(word >> (8 * b));
            if (!c) { terminated = 1; break; }
            length++;
        }
    }
    if (!terminated || length == SIZE_MAX) return 0;
    char *string = malloc(length + 1);
    if (!string) return -1;
    size_t at = 0;
    for (uint32_t i = 0; at < length; i++) {
        uint32_t word = words[first + i];
        for (uint32_t b = 0; b < 4 && at < length; b++) string[at++] = (char)(word >> (8 * b));
    }
    string[length] = 0;
    *out = string;
    *words_used = (uint32_t)((length + 1 + 3) / 4);
    return 1;
}

static int append_entry_point(spirv_module *module, uint32_t model, uint32_t function,
                              const char *name, const uint32_t *interfaces, uint32_t interface_count) {
    if (!id_in_bound(module, function)) return SPIRV_INVALID;
    if (module->entry_point_count == UINT32_MAX) return SPIRV_OUT_OF_MEMORY;
    spirv_entry_point item = { .execution_model = model, .function_id = function };
    size_t name_length = strlen(name);
    item.name = malloc(name_length + 1);
    if (!item.name) return SPIRV_OUT_OF_MEMORY;
    memcpy(item.name, name, name_length + 1);
    if (interface_count) {
        item.interfaces = malloc((size_t)interface_count * sizeof *item.interfaces);
        if (!item.interfaces) { free(item.name); return SPIRV_OUT_OF_MEMORY; }
        for (uint32_t i = 0; i < interface_count; i++) {
            if (!id_in_bound(module, interfaces[i])) {
                free(item.name);
                free(item.interfaces);
                return SPIRV_INVALID;
            }
            for (uint32_t j = 0; j < i; j++) if (interfaces[j] == interfaces[i]) {
                free(item.name);
                free(item.interfaces);
                return SPIRV_INVALID;
            }
            item.interfaces[i] = interfaces[i];
        }
    }
    item.interface_count = interface_count;
    spirv_entry_point *items = realloc(module->entry_points,
        (size_t)(module->entry_point_count + 1) * sizeof *items);
    if (!items) { free(item.name); free(item.interfaces); return SPIRV_OUT_OF_MEMORY; }
    module->entry_points = items;
    module->entry_points[module->entry_point_count++] = item;
    return SPIRV_OK;
}

static int parse_entry_point(spirv_module *module, size_t offset, uint32_t count) {
    if (count < 4) return SPIRV_INVALID;
    const uint32_t *words = module->words + offset;
    if (!id_in_bound(module, words[2])) return SPIRV_INVALID;
    char *name = NULL;
    uint32_t name_words = 0;
    int string_result = literal_string(words, 3, count - 3, &name, &name_words);
    if (string_result <= 0) return string_result < 0 ? SPIRV_OUT_OF_MEMORY : SPIRV_INVALID;
    uint32_t interfaces_begin = 3 + name_words;
    if (interfaces_begin > count) { free(name); return SPIRV_INVALID; }
    int result = append_entry_point(module, words[1], words[2], name,
                                    words + interfaces_begin, count - interfaces_begin);
    free(name);
    return result;
}

static int parse_execution_mode(spirv_module *module, size_t offset, uint32_t count, uint32_t opcode) {
    const uint32_t local_size_mode = 17;
    const uint32_t local_size_id_mode = 38;
    if (count < 3) return SPIRV_INVALID;
    const uint32_t *words = module->words + offset;
    uint32_t function_id = words[1], mode = words[2];
    if (!id_in_bound(module, function_id)) return SPIRV_INVALID;
    if (opcode == 331 && module->version < 0x00010200u) return SPIRV_INVALID;
    if ((opcode == 16 && mode != local_size_mode) || (opcode == 331 && mode != local_size_id_mode)) return SPIRV_OK;
    if (count != 6) return SPIRV_INVALID;
    int found = 0;
    for (uint32_t i = 0; i < module->entry_point_count; i++) {
        spirv_entry_point *entry = &module->entry_points[i];
        if (entry->function_id != function_id) continue;
        if (entry->execution_model != SPIRV_EXECUTION_MODEL_GL_COMPUTE &&
            entry->execution_model != SPIRV_EXECUTION_MODEL_KERNEL) return SPIRV_INVALID;
        if (entry->local_size_kind != SPIRV_LOCAL_SIZE_NONE) return SPIRV_INVALID;
        found = 1;
    }
    if (!found) return SPIRV_INVALID;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t value = words[3 + i];
        if (opcode == 16) {
            if (!value) return SPIRV_INVALID;
        } else if (!id_in_bound(module, value)) {
            return SPIRV_INVALID;
        }
    }
    for (uint32_t i = 0; i < module->entry_point_count; i++) {
        spirv_entry_point *entry = &module->entry_points[i];
        if (entry->function_id != function_id) continue;
        entry->local_size_kind = opcode == 16 ? SPIRV_LOCAL_SIZE_LITERAL : SPIRV_LOCAL_SIZE_IDS;
        for (uint32_t axis = 0; axis < 3; axis++) {
            entry->local_size[axis] = words[3 + axis];
            if (opcode == 16) entry->local_size_default[axis] = words[3 + axis];
        }
        if (opcode == 16) entry->has_local_size_default = 1;
    }
    return SPIRV_OK;
}

static int parse_memory_model(spirv_module *module, const uint32_t *words, uint32_t count) {
    if (!module || !words || count != 3 || module->has_memory_model) return SPIRV_INVALID;
    module->addressing_model = words[1];
    module->memory_model = words[2];
    module->has_memory_model = 1;
    return SPIRV_OK;
}

static int append_variable_id(spirv_module *module, uint32_t id) {
    if (module->variable_count == module->variable_capacity) {
        uint32_t capacity = module->variable_capacity ? module->variable_capacity * 2 : 16;
        if (capacity < module->variable_capacity || (size_t)capacity > SIZE_MAX / sizeof(uint32_t))
            return SPIRV_OUT_OF_MEMORY;
        uint32_t *ids = realloc(module->variable_ids, (size_t)capacity * sizeof *ids);
        if (!ids) return SPIRV_OUT_OF_MEMORY;
        module->variable_ids = ids;
        module->variable_capacity = capacity;
    }
    module->variable_ids[module->variable_count++] = id;
    return SPIRV_OK;
}

static int insert_type_record(spirv_module *module, spirv_id_record *type) {
    type->kind = SPIRV_ID_TYPE;
    int rc = id_table_insert(module, type);
    if (rc != SPIRV_OK) free(type->members);
    return rc;
}

static int parse_type(spirv_module *module, const uint32_t *words, uint32_t count, uint32_t opcode) {
    spirv_id_record type = { .id = count > 1 ? words[1] : 0, .kind = SPIRV_ID_TYPE };
    switch (opcode) {
    case 19: // OpTypeVoid
        if (count != 2) return SPIRV_INVALID;
        type.type_kind = SPIRV_TYPE_VOID;
        break;
    case 20: // OpTypeBool
        if (count != 2) return SPIRV_INVALID;
        type.type_kind = SPIRV_TYPE_BOOL;
        type.width = 1;
        break;
    case 21: // OpTypeInt
        if (count != 4 || !words[2] || words[3] > 1) return SPIRV_INVALID;
        type.type_kind = SPIRV_TYPE_INT;
        type.width = words[2];
        type.signedness = words[3];
        break;
    case 22: // OpTypeFloat
        if (count != 3 || !words[2]) return SPIRV_INVALID;
        type.type_kind = SPIRV_TYPE_FLOAT;
        type.width = words[2];
        break;
    case 23: case 24: { // OpTypeVector / OpTypeMatrix
        if (count != 4 || words[3] < 2 || words[3] > 4) return SPIRV_INVALID;
        const spirv_id_record *component = spirv_find_id(module, words[2]);
        if (!component || component->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
        if (opcode == 23 && component->type_kind != SPIRV_TYPE_BOOL && component->type_kind != SPIRV_TYPE_INT &&
            component->type_kind != SPIRV_TYPE_FLOAT) return SPIRV_INVALID;
        if (opcode == 24 && component->type_kind != SPIRV_TYPE_VECTOR) return SPIRV_INVALID;
        type.type_kind = opcode == 23 ? SPIRV_TYPE_VECTOR : SPIRV_TYPE_MATRIX;
        type.element_type = words[2];
        type.element_count = words[3];
        break;
    }
    case 25: // OpTypeImage
        if ((count != 9 && count != 10) || words[3] > 6 || words[4] > 2 || words[5] > 1 ||
            words[6] > 1 || words[7] > 2 || (count == 10 && words[9] > 2)) return SPIRV_INVALID;
        type.type_kind = SPIRV_TYPE_IMAGE;
        type.element_type = words[2];
        type.image_dimension = words[3];
        type.image_depth = words[4];
        type.image_arrayed = words[5];
        type.image_multisampled = words[6];
        type.image_sampled = words[7];
        type.image_format = words[8];
        if (count == 10) type.image_access_qualifier = words[9];
        break;
    case 26: // OpTypeSampler
        if (count != 2) return SPIRV_INVALID;
        type.type_kind = SPIRV_TYPE_SAMPLER;
        break;
    case 27: // OpTypeSampledImage
        if (count != 3) return SPIRV_INVALID;
        type.type_kind = SPIRV_TYPE_SAMPLED_IMAGE;
        type.element_type = words[2];
        break;
    case 28: // OpTypeArray
        if (count != 4) return SPIRV_INVALID;
        type.type_kind = SPIRV_TYPE_ARRAY;
        type.element_type = words[2];
        type.length_id = words[3];
        break;
    case 29: // OpTypeRuntimeArray
        if (count != 3) return SPIRV_INVALID;
        type.type_kind = SPIRV_TYPE_RUNTIME_ARRAY;
        type.element_type = words[2];
        break;
    case 30: case 33: { // OpTypeStruct / OpTypeFunction
        if (count < (opcode == 33 ? 3u : 2u)) return SPIRV_INVALID;
        type.type_kind = opcode == 30 ? SPIRV_TYPE_STRUCT : SPIRV_TYPE_FUNCTION;
        type.element_type = opcode == 33 && count > 2 ? words[2] : 0; // function result type
        uint32_t first = opcode == 33 ? 3 : 2;
        type.member_count = count - first;
        if (type.member_count) {
            type.members = calloc(type.member_count, sizeof *type.members);
            if (!type.members) return SPIRV_OUT_OF_MEMORY;
            for (uint32_t i = 0; i < type.member_count; i++) type.members[i].type_id = words[first + i];
        }
        break;
    }
    case 32: // OpTypePointer
        if (count != 4) return SPIRV_INVALID;
        type.type_kind = SPIRV_TYPE_POINTER;
        type.storage_class = words[2];
        type.element_type = type.pointee_type_id = words[3];
        break;
    default:
        return SPIRV_OK;
    }
    return insert_type_record(module, &type);
}

static int parse_variable(spirv_module *module, const uint32_t *words, uint32_t count) {
    if (count != 4 && count != 5) return SPIRV_INVALID;
    if (!id_in_bound(module, words[1]) || !id_in_bound(module, words[2]) ||
        (count == 5 && !id_in_bound(module, words[4]))) return SPIRV_INVALID;
    spirv_id_record variable = {
        .id = words[2], .kind = SPIRV_ID_VARIABLE, .type_id = words[1],
        .storage_class = words[3], .initializer_id = count == 5 ? words[4] : 0
    };
    int rc = id_table_insert(module, &variable);
    if (rc != SPIRV_OK) return rc;
    return append_variable_id(module, variable.id);
}

static int parse_module_undef(spirv_module *module, const uint32_t *words, uint32_t count) {
    if (count != 3 || !id_in_bound(module, words[2])) return SPIRV_INVALID;
    const spirv_id_record *type = spirv_find_id(module, words[1]);
    if (!type || type->kind != SPIRV_ID_TYPE || type->type_kind == SPIRV_TYPE_VOID) return SPIRV_INVALID;
    spirv_id_record undefined = {
        .id = words[2], .kind = SPIRV_ID_UNDEFINED, .type_id = words[1]
    };
    return id_table_insert(module, &undefined);
}

static int parse_ext_inst_import(spirv_module *module, const uint32_t *words, uint32_t count) {
    if (count < 3 || !id_in_bound(module, words[1])) return SPIRV_INVALID;
    char *name = NULL;
    uint32_t string_words = 0;
    int string_result = literal_string(words, 2, count - 2, &name, &string_words);
    if (string_result <= 0) return string_result < 0 ? SPIRV_OUT_OF_MEMORY : SPIRV_INVALID;
    if (string_words != count - 2) { free(name); return SPIRV_INVALID; }

    spirv_id_record import_id = { .id = words[1], .kind = SPIRV_ID_EXT_INST_IMPORT };
    int rc = id_table_insert(module, &import_id);
    if (rc != SPIRV_OK) { free(name); return rc; }
    if (module->ext_inst_import_count == UINT32_MAX) { free(name); return SPIRV_OUT_OF_MEMORY; }
    if (module->ext_inst_import_count == module->ext_inst_import_capacity) {
        uint32_t capacity = module->ext_inst_import_capacity ? module->ext_inst_import_capacity * 2u : 8u;
        if (capacity < module->ext_inst_import_capacity ||
            (size_t)capacity > SIZE_MAX / sizeof *module->ext_inst_imports) {
            free(name);
            return SPIRV_OUT_OF_MEMORY;
        }
        spirv_ext_inst_import *replacement = realloc(module->ext_inst_imports,
            (size_t)capacity * sizeof *module->ext_inst_imports);
        if (!replacement) { free(name); return SPIRV_OUT_OF_MEMORY; }
        module->ext_inst_imports = replacement;
        module->ext_inst_import_capacity = capacity;
    }
    module->ext_inst_imports[module->ext_inst_import_count++] = (spirv_ext_inst_import){ words[1], name };
    return SPIRV_OK;
}

static int decoration_is_tracked(uint32_t decoration, int *has_value) {
    switch (decoration) {
    case 1: // SpecId
        *has_value = 1;
        return 1;
    case 2: case 3: case 4: case 5: case 13: case 14: case 16: case 17: case 18: case 24: case 25:
        // Block, BufferBlock, matrix layout, interpolation, invariant and access qualifiers.
        *has_value = 0;
        return 1;
    case 6: case 7: case 11: case 30: case 31: case 32: case 33: case 34: case 35:
        *has_value = 1; // ArrayStride, MatrixStride, BuiltIn, I/O and descriptor decorations, Offset
        return 1;
    default:
        return 0;
    }
}

static int append_decoration_group(spirv_module *module, uint32_t id, size_t word_offset) {
    if (module->decoration_group_count == module->decoration_group_capacity) {
        size_t capacity = module->decoration_group_capacity ? module->decoration_group_capacity * 2 : 8;
        if (capacity < module->decoration_group_capacity ||
            capacity > SIZE_MAX / sizeof(pending_decoration_group)) return SPIRV_OUT_OF_MEMORY;
        pending_decoration_group *items = realloc(module->decoration_groups, capacity * sizeof *items);
        if (!items) return SPIRV_OUT_OF_MEMORY;
        module->decoration_groups = items;
        module->decoration_group_capacity = capacity;
    }
    pending_decoration_group *items = module->decoration_groups;
    items[module->decoration_group_count++] = (pending_decoration_group){ id, word_offset };
    return SPIRV_OK;
}

static int append_group_application(spirv_module *module, uint32_t group_id, uint32_t target_id,
                                    uint32_t member_index, int is_member, size_t word_offset) {
    if (module->group_application_count == module->group_application_capacity) {
        size_t capacity = module->group_application_capacity ? module->group_application_capacity * 2 : 16;
        if (capacity < module->group_application_capacity ||
            capacity > SIZE_MAX / sizeof(pending_group_application)) return SPIRV_OUT_OF_MEMORY;
        pending_group_application *items = realloc(module->group_applications, capacity * sizeof *items);
        if (!items) return SPIRV_OUT_OF_MEMORY;
        module->group_applications = items;
        module->group_application_capacity = capacity;
    }
    pending_group_application *items = module->group_applications;
    items[module->group_application_count++] = (pending_group_application){
        group_id, target_id, member_index, (uint32_t)is_member, word_offset
    };
    return SPIRV_OK;
}

static int parse_decoration(spirv_module *module, const uint32_t *words, uint32_t count,
                            int is_member, size_t word_offset) {
    uint32_t base = is_member ? 4u : 3u;
    if (count < base) return SPIRV_INVALID;
    uint32_t target = words[1];
    uint32_t member = is_member ? words[2] : UINT32_MAX;
    uint32_t decoration = words[is_member ? 3 : 2];
    int has_value = 0;
    if (!decoration_is_tracked(decoration, &has_value)) return SPIRV_OK;
    if (!id_in_bound(module, target) || count != base + (uint32_t)has_value) return SPIRV_INVALID;
    if (module->decoration_count == module->decoration_capacity) {
        size_t capacity = module->decoration_capacity ? module->decoration_capacity * 2 : 32;
        if (capacity < module->decoration_capacity || capacity > SIZE_MAX / sizeof(pending_decoration))
            return SPIRV_OUT_OF_MEMORY;
        pending_decoration *items = realloc(module->decorations, capacity * sizeof *items);
        if (!items) return SPIRV_OUT_OF_MEMORY;
        module->decorations = items;
        module->decoration_capacity = capacity;
    }
    pending_decoration *items = module->decorations;
    items[module->decoration_count++] = (pending_decoration){
        .target_id = target, .member_index = member, .decoration = decoration,
        .value = has_value ? words[base] : 0, .has_value = (uint32_t)has_value,
        .is_member = (uint32_t)is_member, .word_offset = word_offset
    };
    return SPIRV_OK;
}

static int parse_decoration_group(spirv_module *module, const uint32_t *words,
                                  uint32_t count, size_t word_offset) {
    if (count != 2 || !id_in_bound(module, words[1])) return SPIRV_INVALID;
    spirv_id_record group = { .id = words[1], .kind = SPIRV_ID_DECORATION_GROUP };
    int rc = id_table_insert(module, &group);
    if (rc != SPIRV_OK) return rc;
    return append_decoration_group(module, group.id, word_offset);
}

static int parse_group_decorate(spirv_module *module, const uint32_t *words,
                                uint32_t count, int is_member, size_t word_offset) {
    if (count < (is_member ? 4u : 3u) ||
        (is_member && ((count - 2u) & 1u)) || !id_in_bound(module, words[1])) return SPIRV_INVALID;
    if (is_member) {
        for (uint32_t i = 2; i < count; i += 2) {
            if (!id_in_bound(module, words[i])) return SPIRV_INVALID;
            int rc = append_group_application(module, words[1], words[i], words[i + 1], 1, word_offset);
            if (rc != SPIRV_OK) return rc;
        }
    } else {
        for (uint32_t i = 2; i < count; i++) {
            if (!id_in_bound(module, words[i])) return SPIRV_INVALID;
            int rc = append_group_application(module, words[1], words[i], UINT32_MAX, 0, word_offset);
            if (rc != SPIRV_OK) return rc;
        }
    }
    return SPIRV_OK;
}

static int parse_constant(spirv_module *module, const uint32_t *words, uint32_t count, uint32_t opcode) {
    uint32_t kind, type_id, result_id, first_literal = 0, literal_count = 0, specialization = 0;
    uint32_t first_constituent = 0, constituent_count = 0;
    uint32_t specialization_opcode = 0, first_specialization_operand = 0, specialization_operand_count = 0;
    uint64_t bits = 0;
    switch (opcode) {
    case 41: case 42: case 48: case 49: // true/false scalar constants
        if (count != 3) return SPIRV_INVALID;
        type_id = words[1]; result_id = words[2];
        kind = SPIRV_CONSTANT_BOOL;
        bits = opcode == 41 || opcode == 48;
        specialization = opcode == 48 || opcode == 49;
        break;
    case 43: case 50: { // OpConstant / OpSpecConstant scalar literal
        if (count < 4) return SPIRV_INVALID;
        type_id = words[1]; result_id = words[2]; first_literal = 3; literal_count = count - 3;
        const spirv_id_record *type = spirv_find_id(module, type_id);
        if (!type || type->kind != SPIRV_ID_TYPE ||
            (type->type_kind != SPIRV_TYPE_INT && type->type_kind != SPIRV_TYPE_FLOAT)) return SPIRV_INVALID;
        uint64_t expected = ((uint64_t)type->width + 31u) / 32u;
        if (!expected || literal_count != expected) return SPIRV_INVALID;
        kind = SPIRV_CONSTANT_SCALAR;
        bits = words[first_literal];
        if (literal_count > 1) bits |= (uint64_t)words[first_literal + 1] << 32;
        specialization = opcode == 50;
        break;
    }
    case 46: // OpConstantNull
        if (count != 3) return SPIRV_INVALID;
        type_id = words[1]; result_id = words[2]; kind = SPIRV_CONSTANT_NULL;
        break;
    case 44: case 51: // OpConstantComposite / OpSpecConstantComposite
        if (count < 4) return SPIRV_INVALID;
        type_id = words[1]; result_id = words[2]; kind = SPIRV_CONSTANT_COMPOSITE;
        first_constituent = 3; constituent_count = count - 3;
        specialization = opcode == 51;
        break;
    case 52: { // OpSpecConstantOp: currently a scalar 32-bit integer subset.
        if (count != 6) return SPIRV_INVALID;
        specialization_opcode = words[3];
        if (specialization_opcode != 128 && specialization_opcode != 130 && specialization_opcode != 132 &&
            specialization_opcode != 134 && specialization_opcode != 135 && specialization_opcode != 137 &&
            (specialization_opcode < 194 || specialization_opcode > 196) &&
            specialization_opcode != 197 && specialization_opcode != 198 && specialization_opcode != 199)
            return SPIRV_UNSUPPORTED; // integer arithmetic/division, shifts, and bitwise operations
        type_id = words[1]; result_id = words[2]; kind = SPIRV_CONSTANT_SCALAR;
        const spirv_id_record *type = spirv_find_id(module, type_id);
        if (!type || type->kind != SPIRV_ID_TYPE || type->type_kind != SPIRV_TYPE_INT || type->width != 32)
            return SPIRV_UNSUPPORTED;
        if (((specialization_opcode == 134 || specialization_opcode == 137) && type->signedness != 0) ||
            (specialization_opcode == 135 && type->signedness != 1)) return SPIRV_INVALID;
        if (!id_in_bound(module, words[4]) || !id_in_bound(module, words[5])) return SPIRV_INVALID;
        literal_count = 1;
        first_specialization_operand = 4;
        specialization_operand_count = 2;
        specialization = 1;
        break;
    }
    default:
        return SPIRV_OK;
    }
    const spirv_id_record *type = spirv_find_id(module, type_id);
    if (kind != SPIRV_CONSTANT_NULL && (!type || type->kind != SPIRV_ID_TYPE)) return SPIRV_INVALID;
    if (kind == SPIRV_CONSTANT_NULL && type && type->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
    if (kind == SPIRV_CONSTANT_BOOL && type->type_kind != SPIRV_TYPE_BOOL) return SPIRV_INVALID;
    if (kind == SPIRV_CONSTANT_NULL && type && type->type_kind == SPIRV_TYPE_VOID) return SPIRV_INVALID;
    spirv_id_record constant = {
        .id = result_id, .kind = SPIRV_ID_CONSTANT, .type_id = type_id,
        .constant_kind = kind, .is_specialization = specialization,
        .specialization_opcode = specialization_opcode,
        .specialization_operand_count = specialization_operand_count,
        .specialization_evaluation_state = specialization_opcode ? 0u : 2u,
        .literal_word_count = literal_count, .value_bits = bits,
        .constituent_count = constituent_count
    };
    if (literal_count) {
        constant.literal_words = malloc((size_t)literal_count * sizeof *constant.literal_words);
        if (!constant.literal_words) return SPIRV_OUT_OF_MEMORY;
        if (specialization_opcode) constant.literal_words[0] = 0;
        else memcpy(constant.literal_words, words + first_literal,
                    (size_t)literal_count * sizeof *constant.literal_words);
    }
    if (specialization_operand_count) {
        constant.specialization_operand_ids = malloc((size_t)specialization_operand_count *
                                                       sizeof *constant.specialization_operand_ids);
        if (!constant.specialization_operand_ids) {
            free(constant.literal_words);
            return SPIRV_OUT_OF_MEMORY;
        }
        memcpy(constant.specialization_operand_ids, words + first_specialization_operand,
               (size_t)specialization_operand_count * sizeof *constant.specialization_operand_ids);
    }
    if (constituent_count) {
        constant.constituent_ids = malloc((size_t)constituent_count * sizeof *constant.constituent_ids);
        if (!constant.constituent_ids) { free(constant.literal_words); return SPIRV_OUT_OF_MEMORY; }
        for (uint32_t i = 0; i < constituent_count; i++) {
            if (!id_in_bound(module, words[first_constituent + i])) {
                free(constant.literal_words); free(constant.constituent_ids);
                return SPIRV_INVALID;
            }
            constant.constituent_ids[i] = words[first_constituent + i];
        }
    }
    int rc = id_table_insert(module, &constant);
    if (rc != SPIRV_OK) {
        free(constant.literal_words);
        free(constant.constituent_ids);
        free(constant.specialization_operand_ids);
    }
    return rc;
}

static int parse_semantic_declaration(spirv_module *module, size_t offset, uint32_t count, uint32_t opcode) {
    const uint32_t *words = module->words + offset;
    int rc = parse_type(module, words, count, opcode);
    if (rc != SPIRV_OK || (opcode >= 19 && opcode <= 33)) return rc;
    if (opcode == 59) return parse_variable(module, words, count);
    return parse_constant(module, words, count, opcode);
}

static int set_decorated_value(uint32_t *slot, uint32_t *has_slot, uint32_t value) {
    if (*has_slot) return SPIRV_INVALID;
    *slot = value;
    *has_slot = 1;
    return SPIRV_OK;
}

static int set_decoration_flag(uint32_t *flags, uint32_t flag) {
    if (*flags & flag) return SPIRV_INVALID;
    *flags |= flag;
    return SPIRV_OK;
}

static int apply_one_decoration(spirv_id_record *record, spirv_member_record *member,
                                const pending_decoration *decoration) {
    uint32_t *flags = member ? &member->decoration_flags : &record->decoration_flags;
    uint32_t *value_slot = NULL, *has_slot = NULL;
    switch (decoration->decoration) {
    case 1:
        if (member || record->kind != SPIRV_ID_CONSTANT || !record->is_specialization ||
            record->specialization_opcode ||
            (record->constant_kind != SPIRV_CONSTANT_SCALAR && record->constant_kind != SPIRV_CONSTANT_BOOL))
            return SPIRV_INVALID;
        value_slot = &record->specialization_id; has_slot = &record->has_specialization_id; break;
    case 2:
        if (member || record->kind != SPIRV_ID_TYPE || record->type_kind != SPIRV_TYPE_STRUCT) return SPIRV_INVALID;
        return set_decoration_flag(flags, SPIRV_DECORATION_BLOCK);
    case 3:
        if (member || record->kind != SPIRV_ID_TYPE || record->type_kind != SPIRV_TYPE_STRUCT) return SPIRV_INVALID;
        return set_decoration_flag(flags, SPIRV_DECORATION_BUFFER_BLOCK);
    case 4:
        if (!member) return SPIRV_INVALID;
        return set_decoration_flag(flags, SPIRV_DECORATION_ROW_MAJOR);
    case 5:
        if (!member) return SPIRV_INVALID;
        return set_decoration_flag(flags, SPIRV_DECORATION_COL_MAJOR);
    case 13:
        if ((!member && record->kind != SPIRV_ID_VARIABLE)) return SPIRV_INVALID;
        return set_decoration_flag(flags, SPIRV_DECORATION_NO_PERSPECTIVE);
    case 14:
        if ((!member && record->kind != SPIRV_ID_VARIABLE)) return SPIRV_INVALID;
        return set_decoration_flag(flags, SPIRV_DECORATION_FLAT);
    case 16:
        if ((!member && record->kind != SPIRV_ID_VARIABLE)) return SPIRV_INVALID;
        return set_decoration_flag(flags, SPIRV_DECORATION_CENTROID);
    case 17:
        if ((!member && record->kind != SPIRV_ID_VARIABLE)) return SPIRV_INVALID;
        return set_decoration_flag(flags, SPIRV_DECORATION_SAMPLE);
    case 18:
        if ((!member && record->kind != SPIRV_ID_VARIABLE)) return SPIRV_INVALID;
        return set_decoration_flag(flags, SPIRV_DECORATION_INVARIANT);
    case 24: return set_decoration_flag(flags, SPIRV_DECORATION_NON_WRITABLE);
    case 25: return set_decoration_flag(flags, SPIRV_DECORATION_NON_READABLE);
    case 6:
        if (member || record->kind != SPIRV_ID_TYPE ||
            (record->type_kind != SPIRV_TYPE_ARRAY && record->type_kind != SPIRV_TYPE_RUNTIME_ARRAY) || !decoration->value)
            return SPIRV_INVALID;
        value_slot = &record->array_stride; has_slot = &record->has_array_stride; break;
    case 7:
        if (!member || !decoration->value) return SPIRV_INVALID;
        value_slot = &member->matrix_stride; has_slot = &member->has_matrix_stride; break;
    case 11:
        if (member) { value_slot = &member->builtin; has_slot = &member->has_builtin; }
        else if (record->kind == SPIRV_ID_VARIABLE) {
            value_slot = &record->builtin; has_slot = &record->has_builtin;
        } else return SPIRV_INVALID;
        break;
    case 30:
        if (member) { value_slot = &member->location; has_slot = &member->has_location; }
        else if (record->kind == SPIRV_ID_VARIABLE) {
            value_slot = &record->location; has_slot = &record->has_location;
        } else return SPIRV_INVALID;
        break;
    case 31:
        if (member) { value_slot = &member->component; has_slot = &member->has_component; }
        else if (record->kind == SPIRV_ID_VARIABLE) {
            value_slot = &record->component; has_slot = &record->has_component;
        } else return SPIRV_INVALID;
        break;
    case 32:
        if (member) { value_slot = &member->index; has_slot = &member->has_index; }
        else if (record->kind == SPIRV_ID_VARIABLE) {
            value_slot = &record->index; has_slot = &record->has_index;
        } else return SPIRV_INVALID;
        break;
    case 33:
        if (member || record->kind != SPIRV_ID_VARIABLE) return SPIRV_INVALID;
        value_slot = &record->binding; has_slot = &record->has_binding; break;
    case 34:
        if (member || record->kind != SPIRV_ID_VARIABLE) return SPIRV_INVALID;
        value_slot = &record->descriptor_set; has_slot = &record->has_descriptor_set; break;
    case 35:
        if (!member) return SPIRV_INVALID;
        value_slot = &member->offset; has_slot = &member->has_offset; break;
    default:
        return SPIRV_OK;
    }
    if (decoration->has_value) return set_decorated_value(value_slot, has_slot, decoration->value);
    return SPIRV_OK;
}

static const pending_decoration_group *find_decoration_group(const spirv_module *module, uint32_t id) {
    const pending_decoration_group *groups = module->decoration_groups;
    for (size_t i = 0; i < module->decoration_group_count; i++)
        if (groups[i].id == id) return &groups[i];
    return NULL;
}

static spirv_id_record *find_id_slot(spirv_module *module, uint32_t id) {
    if (!id || !module->id_capacity) return NULL;
    spirv_id_record *records = id_slots(module);
    size_t index = id_hash(id) & (module->id_capacity - 1);
    for (size_t n = 0; n < module->id_capacity; n++) {
        if (!records[index].id) return NULL;
        if (records[index].id == id) return &records[index];
        index = (index + 1) & (module->id_capacity - 1);
    }
    return NULL;
}

static int apply_decorations(spirv_module *module) {
    if (module->decoration_count && !module->id_capacity) return SPIRV_INVALID;
    pending_decoration *items = module->decorations;
    for (size_t i = 0; i < module->decoration_count; i++) {
        pending_decoration *item = &items[i];
        spirv_id_record *record = find_id_slot(module, item->target_id);
        if (!record) return SPIRV_INVALID;
        if (record->kind == SPIRV_ID_DECORATION_GROUP) {
            const pending_decoration_group *group = find_decoration_group(module, item->target_id);
            if (item->is_member || !group || item->word_offset >= group->word_offset) return SPIRV_INVALID;
            continue;
        }
        spirv_member_record *member = NULL;
        if (item->is_member) {
            if (record->kind != SPIRV_ID_TYPE || record->type_kind != SPIRV_TYPE_STRUCT ||
                item->member_index >= record->member_count) return SPIRV_INVALID;
            member = &record->members[item->member_index];
        }
        int rc = apply_one_decoration(record, member, item);
        if (rc != SPIRV_OK) return rc;
    }

    pending_group_application *applications = module->group_applications;
    for (size_t i = 0; i < module->group_application_count; i++) {
        pending_group_application *application = &applications[i];
        spirv_id_record *group_record = find_id_slot(module, application->group_id);
        const pending_decoration_group *group = find_decoration_group(module, application->group_id);
        spirv_id_record *target = find_id_slot(module, application->target_id);
        if (!group_record || group_record->kind != SPIRV_ID_DECORATION_GROUP || !group || !target ||
            application->word_offset <= group->word_offset || target->kind == SPIRV_ID_DECORATION_GROUP)
            return SPIRV_INVALID;
        spirv_member_record *member = NULL;
        if (application->is_member) {
            if (target->kind != SPIRV_ID_TYPE || target->type_kind != SPIRV_TYPE_STRUCT ||
                application->member_index >= target->member_count) return SPIRV_INVALID;
            member = &target->members[application->member_index];
        }
        for (size_t j = 0; j < module->decoration_count; j++) {
            pending_decoration *decoration = &items[j];
            if (decoration->target_id != application->group_id) continue;
            if (decoration->is_member || decoration->word_offset >= group->word_offset) return SPIRV_INVALID;
            pending_decoration applied = *decoration;
            applied.target_id = application->target_id;
            applied.member_index = application->member_index;
            applied.is_member = application->is_member;
            int rc = apply_one_decoration(target, member, &applied);
            if (rc != SPIRV_OK) return rc;
        }
    }
    return SPIRV_OK;
}

static int validate_semantic_records(spirv_module *module) {
    spirv_id_record *slots = id_slots(module);
    for (size_t i = 0; i < module->id_capacity; i++) {
        spirv_id_record *record = &slots[i];
        if (!record->id) continue;
        if (record->has_specialization_id) {
            if (record->kind != SPIRV_ID_CONSTANT || !record->is_specialization) return SPIRV_INVALID;
            for (size_t j = i + 1; j < module->id_capacity; j++)
                if (slots[j].id && slots[j].has_specialization_id &&
                    slots[j].specialization_id == record->specialization_id) return SPIRV_INVALID;
        }
        if (record->kind == SPIRV_ID_CONSTANT) {
            const spirv_id_record *type = spirv_find_id(module, record->type_id);
            if (!type || type->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
            if (record->constant_kind != SPIRV_CONSTANT_COMPOSITE) {
                if (record->constituent_count || record->constituent_ids) return SPIRV_INVALID;
                continue;
            }
            if (record->literal_word_count || !record->constituent_count || !record->constituent_ids)
                return SPIRV_INVALID;
            uint64_t expected_count = 0;
            switch (type->type_kind) {
            case SPIRV_TYPE_VECTOR:
            case SPIRV_TYPE_MATRIX:
                expected_count = type->element_count;
                break;
            case SPIRV_TYPE_ARRAY: {
                const spirv_id_record *length = spirv_find_id(module, type->length_id);
                if (!length || length->kind != SPIRV_ID_CONSTANT ||
                    length->constant_kind != SPIRV_CONSTANT_SCALAR || !length->value_bits)
                    return SPIRV_INVALID;
                expected_count = length->value_bits;
                break;
            }
            case SPIRV_TYPE_STRUCT:
                expected_count = type->member_count;
                break;
            default:
                return SPIRV_INVALID;
            }
            if (expected_count != record->constituent_count) return SPIRV_INVALID;
            for (uint32_t c = 0; c < record->constituent_count; c++) {
                uint32_t expected_type = type->type_kind == SPIRV_TYPE_STRUCT ?
                    type->members[c].type_id : type->element_type;
                const spirv_id_record *constituent = spirv_find_id(module, record->constituent_ids[c]);
                if (!constituent || constituent->type_id != expected_type) return SPIRV_INVALID;
                if (constituent->kind == SPIRV_ID_CONSTANT) {
                    if (!record->is_specialization && constituent->is_specialization) return SPIRV_INVALID;
                } else if (constituent->kind != SPIRV_ID_UNDEFINED) {
                    return SPIRV_INVALID;
                }
            }
            continue;
        }
        if (record->kind == SPIRV_ID_VARIABLE) {
            const spirv_id_record *pointer = spirv_find_id(module, record->type_id);
            if (!pointer || pointer->kind != SPIRV_ID_TYPE || pointer->type_kind != SPIRV_TYPE_POINTER ||
                pointer->storage_class != record->storage_class) return SPIRV_INVALID;
            record->pointee_type_id = pointer->pointee_type_id;
            if (record->has_location && record->has_builtin) return SPIRV_INVALID;
            continue;
        }
        if (record->kind == SPIRV_ID_DECORATION_GROUP || record->kind == SPIRV_ID_FUNCTION ||
            record->kind == SPIRV_ID_FUNCTION_PARAMETER || record->kind == SPIRV_ID_LABEL ||
            record->kind == SPIRV_ID_INSTRUCTION_RESULT || record->kind == SPIRV_ID_UNDEFINED ||
            record->kind == SPIRV_ID_EXT_INST_IMPORT) continue;
        if (record->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
        const spirv_id_record *element = record->element_type ? spirv_find_id(module, record->element_type) : NULL;
        switch (record->type_kind) {
        case SPIRV_TYPE_VECTOR:
            if (!element || element->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
            break;
        case SPIRV_TYPE_MATRIX:
            if (!element || element->kind != SPIRV_ID_TYPE || element->type_kind != SPIRV_TYPE_VECTOR) return SPIRV_INVALID;
            break;
        case SPIRV_TYPE_IMAGE:
            if (!element || element->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
            break;
        case SPIRV_TYPE_SAMPLED_IMAGE:
            if (!element || element->kind != SPIRV_ID_TYPE || element->type_kind != SPIRV_TYPE_IMAGE) return SPIRV_INVALID;
            break;
        case SPIRV_TYPE_ARRAY: {
            const spirv_id_record *length = spirv_find_id(module, record->length_id);
            if (!element || element->kind != SPIRV_ID_TYPE || !length || length->kind != SPIRV_ID_CONSTANT ||
                length->constant_kind != SPIRV_CONSTANT_SCALAR) return SPIRV_INVALID;
            const spirv_id_record *length_type = spirv_find_id(module, length->type_id);
            if (!length_type || length_type->kind != SPIRV_ID_TYPE || length_type->type_kind != SPIRV_TYPE_INT ||
                !length->value_bits) return SPIRV_INVALID;
            if (!length_type->width || length_type->width > 64) return SPIRV_UNSUPPORTED;
            if (length_type->signedness &&
                (length->value_bits & (UINT64_C(1) << (length_type->width - 1)))) return SPIRV_INVALID;
            break;
        }
        case SPIRV_TYPE_RUNTIME_ARRAY:
        case SPIRV_TYPE_POINTER:
            if (!element || element->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
            if (record->type_kind == SPIRV_TYPE_POINTER) record->pointee_type_id = record->element_type;
            break;
        case SPIRV_TYPE_STRUCT:
        case SPIRV_TYPE_FUNCTION:
            for (uint32_t m = 0; m < record->member_count; m++) {
                const spirv_id_record *member_type = spirv_find_id(module, record->members[m].type_id);
                if (!member_type || member_type->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
            }
            if (record->type_kind == SPIRV_TYPE_FUNCTION &&
                (!element || element->kind != SPIRV_ID_TYPE)) return SPIRV_INVALID;
            break;
        default:
            break;
        }
        if ((record->decoration_flags & SPIRV_DECORATION_BLOCK) &&
            (record->decoration_flags & SPIRV_DECORATION_BUFFER_BLOCK)) return SPIRV_INVALID;
        if (record->has_location && record->has_builtin) return SPIRV_INVALID;
        for (uint32_t m = 0; m < record->member_count; m++) {
            spirv_member_record *member = &record->members[m];
            if (member->has_location && member->has_builtin) return SPIRV_INVALID;
            if ((member->decoration_flags & (SPIRV_DECORATION_ROW_MAJOR | SPIRV_DECORATION_COL_MAJOR)) ==
                (SPIRV_DECORATION_ROW_MAJOR | SPIRV_DECORATION_COL_MAJOR)) return SPIRV_INVALID;
        }
    }
    return SPIRV_OK;
}

static int resolve_specialization_constant_value(spirv_module *module, spirv_id_record *constant,
                                                  uint32_t depth) {
    if (!module || !constant || constant->kind != SPIRV_ID_CONSTANT) return SPIRV_INVALID;
    if (!constant->specialization_opcode) return SPIRV_OK;
    if (constant->specialization_evaluation_state == 2) return SPIRV_OK;
    if (constant->specialization_evaluation_state == 1 || depth >= module->id_bound ||
        constant->specialization_operand_count != 2 || !constant->specialization_operand_ids ||
        constant->literal_word_count != 1 || !constant->literal_words) return SPIRV_INVALID;
    const spirv_id_record *result_type = spirv_find_id(module, constant->type_id);
    if (!result_type || result_type->kind != SPIRV_ID_TYPE || result_type->type_kind != SPIRV_TYPE_INT ||
        result_type->width != 32) return SPIRV_UNSUPPORTED;
    constant->specialization_evaluation_state = 1;
    uint32_t operands[2];
    for (uint32_t i = 0; i < 2; i++) {
        spirv_id_record *operand = (spirv_id_record *)spirv_find_id(module, constant->specialization_operand_ids[i]);
        const spirv_id_record *operand_type = operand ? spirv_find_id(module, operand->type_id) : NULL;
        int shift_amount = i == 1 && constant->specialization_opcode >= 194 &&
            constant->specialization_opcode <= 196;
        if (!operand || operand->kind != SPIRV_ID_CONSTANT || operand->constant_kind != SPIRV_CONSTANT_SCALAR ||
            (!shift_amount && operand->type_id != constant->type_id) ||
            (shift_amount && (!operand_type || operand_type->kind != SPIRV_ID_TYPE ||
                              operand_type->type_kind != SPIRV_TYPE_INT || operand_type->width != 32)) ||
            operand->literal_word_count != 1 || !operand->literal_words) {
            constant->specialization_evaluation_state = 0;
            return SPIRV_INVALID;
        }
        int rc = resolve_specialization_constant_value(module, operand, depth + 1u);
        if (rc != SPIRV_OK) { constant->specialization_evaluation_state = 0; return rc; }
        operands[i] = (uint32_t)operand->value_bits;
    }
    uint32_t value;
    switch (constant->specialization_opcode) {
    case 128: value = operands[0] + operands[1]; break; // IAdd, modulo 2^32
    case 130: value = operands[0] - operands[1]; break; // ISub, modulo 2^32
    case 132: value = operands[0] * operands[1]; break; // IMul, modulo 2^32
    case 134: // UDiv
        if (!operands[1]) {
            constant->specialization_evaluation_state = 0;
            return SPIRV_UNSUPPORTED; // The SPIR-V result is undefined for a zero divisor.
        }
        value = operands[0] / operands[1];
        break;
    case 135: { // SDiv
        if (!operands[1] || (operands[0] == UINT32_C(0x80000000) && operands[1] == UINT32_MAX)) {
            constant->specialization_evaluation_state = 0;
            return SPIRV_UNSUPPORTED; // Zero divisors and signed overflow have undefined results.
        }
        int64_t dividend = (operands[0] & UINT32_C(0x80000000)) ?
            (int64_t)operands[0] - INT64_C(0x100000000) : operands[0];
        int64_t divisor = (operands[1] & UINT32_C(0x80000000)) ?
            (int64_t)operands[1] - INT64_C(0x100000000) : operands[1];
        value = (uint32_t)(dividend / divisor);
        break;
    }
    case 137: // UMod
        if (!operands[1]) {
            constant->specialization_evaluation_state = 0;
            return SPIRV_UNSUPPORTED; // The SPIR-V result is undefined for a zero divisor.
        }
        value = operands[0] % operands[1];
        break;
    case 194: // ShiftRightLogical
    case 195: // ShiftRightArithmetic
    case 196: // ShiftLeftLogical
        if (operands[1] >= 32) {
            constant->specialization_evaluation_state = 0;
            return SPIRV_UNSUPPORTED; // The SPIR-V result is poison for an out-of-range shift.
        }
        if (constant->specialization_opcode == 194) value = operands[0] >> operands[1];
        else if (constant->specialization_opcode == 195) {
            value = operands[1] ? operands[0] >> operands[1] : operands[0];
            if (operands[1] && (operands[0] & UINT32_C(0x80000000)))
                value |= UINT32_MAX << (32u - operands[1]);
        } else value = operands[0] << operands[1];
        break;
    case 197: value = operands[0] | operands[1]; break; // BitwiseOr
    case 198: value = operands[0] ^ operands[1]; break; // BitwiseXor
    case 199: value = operands[0] & operands[1]; break; // BitwiseAnd
    default:
        constant->specialization_evaluation_state = 0;
        return SPIRV_UNSUPPORTED;
    }
    constant->value_bits = value;
    constant->literal_words[0] = value;
    constant->specialization_evaluation_state = 2;
    return SPIRV_OK;
}

static int resolve_specialization_constant_values(spirv_module *module) {
    spirv_id_record *slots = id_slots(module);
    for (size_t i = 0; i < module->id_capacity; i++) {
        spirv_id_record *constant = &slots[i];
        if (!constant->id || constant->kind != SPIRV_ID_CONSTANT || !constant->specialization_opcode) continue;
        int rc = resolve_specialization_constant_value(module, constant, 0);
        if (rc != SPIRV_OK) return rc;
    }
    return SPIRV_OK;
}

static int resolve_local_size_ids(spirv_module *module) {
    for (uint32_t i = 0; i < module->entry_point_count; i++) {
        spirv_entry_point *entry = &module->entry_points[i];
        if (entry->local_size_kind != SPIRV_LOCAL_SIZE_IDS) continue;
        uint64_t values[3];
        int resolved = 1;
        for (uint32_t axis = 0; axis < 3; axis++) {
            const spirv_id_record *constant = spirv_find_id(module, entry->local_size[axis]);
            if (!constant || constant->kind != SPIRV_ID_CONSTANT) { resolved = 0; continue; }
            const spirv_id_record *type = spirv_find_id(module, constant->type_id);
            if (!type || type->kind != SPIRV_ID_TYPE || type->type_kind != SPIRV_TYPE_INT) return SPIRV_INVALID;
            if (constant->constant_kind == SPIRV_CONSTANT_NULL && !constant->is_specialization) return SPIRV_INVALID;
            if (type->width > 64 || constant->literal_word_count == 0 ||
                constant->constant_kind == SPIRV_CONSTANT_NULL) { resolved = 0; continue; }
            if (!constant->value_bits && !constant->is_specialization) return SPIRV_INVALID;
            values[axis] = constant->value_bits;
        }
        if (resolved) {
            memcpy(entry->local_size_default, values, sizeof values);
            entry->has_local_size_default = 1;
        }
    }
    return SPIRV_OK;
}

static int append_function(spirv_module *module, const spirv_function_record *function) {
    if (module->function_count == module->function_capacity) {
        uint32_t capacity = module->function_capacity ? module->function_capacity * 2u : 16u;
        if (capacity < module->function_capacity || (size_t)capacity > SIZE_MAX / sizeof *module->functions)
            return SPIRV_OUT_OF_MEMORY;
        spirv_function_record *functions = realloc(module->functions, (size_t)capacity * sizeof *functions);
        if (!functions) return SPIRV_OUT_OF_MEMORY;
        module->functions = functions;
        module->function_capacity = capacity;
    }
    module->functions[module->function_count++] = *function;
    return SPIRV_OK;
}

static int append_basic_block(spirv_module *module, const spirv_basic_block *block) {
    if (module->basic_block_count == module->basic_block_capacity) {
        uint32_t capacity = module->basic_block_capacity ? module->basic_block_capacity * 2u : 32u;
        if (capacity < module->basic_block_capacity || (size_t)capacity > SIZE_MAX / sizeof *module->basic_blocks)
            return SPIRV_OUT_OF_MEMORY;
        spirv_basic_block *blocks = realloc(module->basic_blocks, (size_t)capacity * sizeof *blocks);
        if (!blocks) return SPIRV_OUT_OF_MEMORY;
        module->basic_blocks = blocks;
        module->basic_block_capacity = capacity;
    }
    module->basic_blocks[module->basic_block_count++] = *block;
    return SPIRV_OK;
}

static int function_terminator(uint16_t opcode) {
    return (opcode >= 249 && opcode <= 255) || opcode == 4416;
}

static int function_line_debug(uint16_t opcode) {
    return opcode == 8 || opcode == 317; // OpLine and OpNoLine
}

// These core instructions have Result Type and Result Id as their first two operands. The list covers
// common values used to resolve control-flow selector types; it is not an opcode validator, and other
// result-producing instructions remain available through the raw iterators.
static int instruction_has_typed_result(uint16_t opcode) {
    return opcode == 1 || opcode == 12 || opcode == 57 || opcode == 60 || opcode == 61 ||
           (opcode >= 65 && opcode <= 70) || (opcode >= 77 && opcode <= 84) ||
           (opcode >= 86 && opcode <= 98) || (opcode >= 100 && opcode <= 107) ||
           (opcode >= 109 && opcode <= 124) || (opcode >= 126 && opcode <= 152) ||
           (opcode >= 154 && opcode <= 191) || (opcode >= 194 && opcode <= 205) ||
           (opcode >= 207 && opcode <= 215) || opcode == 227 ||
           (opcode >= 229 && opcode <= 242) || opcode == 245 || opcode == 334 || opcode == 335 ||
           (opcode >= 352 && opcode <= 357);
}

// Index function and block boundaries without interpreting the instruction semantics. The saved word ranges
// let later shader-lowering passes walk a validated body while retaining the original encoded operands.
static int index_function_bodies(spirv_module *module) {
    const uint32_t no_index = UINT32_MAX;
    uint32_t current_function = no_index, current_block = no_index;
    uint32_t expected_parameters = 0;
    int block_terminated = 0;
    spirv_iterator iterator;
    spirv_instruction instruction;
    spirv_iter_begin(module, &iterator);
    int next;
    while ((next = spirv_iter_next(&iterator, &instruction)) > 0) {
        size_t offset = (size_t)(instruction.words - module->words);
        spirv_function_record *function = current_function == no_index ? NULL : &module->functions[current_function];
        spirv_basic_block *block = current_block == no_index ? NULL : &module->basic_blocks[current_block];

        if (instruction.opcode == 54) { // OpFunction
            if (function || instruction.word_count != 5) return SPIRV_INVALID;
            uint32_t result_type_id = instruction.words[1], function_id = instruction.words[2];
            uint32_t function_type_id = instruction.words[4];
            const spirv_id_record *result_type = spirv_find_id(module, result_type_id);
            const spirv_id_record *function_type = spirv_find_id(module, function_type_id);
            if (!id_in_bound(module, function_id) || spirv_find_id(module, function_id) ||
                !result_type || result_type->kind != SPIRV_ID_TYPE || !function_type ||
                function_type->kind != SPIRV_ID_TYPE || function_type->type_kind != SPIRV_TYPE_FUNCTION ||
                function_type->element_type != result_type_id) return SPIRV_INVALID;
            expected_parameters = function_type->member_count;
            spirv_id_record function_id_record = { .id = function_id, .kind = SPIRV_ID_FUNCTION };
            int rc = id_table_insert(module, &function_id_record);
            if (rc != SPIRV_OK) return rc;
            spirv_function_record record = {
                .function_id = function_id, .result_type_id = result_type_id,
                .function_type_id = function_type_id, .first_block = module->basic_block_count,
                .word_offset = offset, .word_count = instruction.word_count
            };
            rc = append_function(module, &record);
            if (rc != SPIRV_OK) return rc;
            current_function = module->function_count - 1;
            current_block = no_index;
            block_terminated = 0;
            continue;
        }

        if (instruction.opcode == 56) { // OpFunctionEnd
            if (!function || instruction.word_count != 1 || (block && !block_terminated) ||
                function->parameter_count != expected_parameters) return SPIRV_INVALID;
            function->word_count += instruction.word_count;
            current_function = current_block = no_index;
            block_terminated = 0;
            continue;
        }

        if (!function) {
            if (instruction.opcode == 55 || instruction.opcode == 248 || function_terminator(instruction.opcode))
                return SPIRV_INVALID;
            continue;
        }
        function->word_count += instruction.word_count;

        if (instruction.opcode == 55) { // OpFunctionParameter
            if (block || instruction.word_count != 3 || function->parameter_count >= expected_parameters ||
                !id_in_bound(module, instruction.words[2])) return SPIRV_INVALID;
            const spirv_id_record *function_type = spirv_find_id(module, function->function_type_id);
            if (!function_type || instruction.words[1] !=
                function_type->members[function->parameter_count].type_id) return SPIRV_INVALID;
            spirv_id_record parameter_id_record = {
                .id = instruction.words[2], .kind = SPIRV_ID_FUNCTION_PARAMETER,
                .type_id = instruction.words[1], .owner_function_id = function->function_id
            };
            int rc = id_table_insert(module, &parameter_id_record);
            if (rc != SPIRV_OK) return rc;
            function->parameter_count++;
            continue;
        }

        if (instruction.opcode == 248) { // OpLabel
            if (instruction.word_count != 2 || (block && !block_terminated) ||
                !id_in_bound(module, instruction.words[1]) || spirv_find_id(module, instruction.words[1]))
                return SPIRV_INVALID;
            spirv_id_record label_id_record = {
                .id = instruction.words[1], .kind = SPIRV_ID_LABEL,
                .owner_function_id = function->function_id
            };
            int rc = id_table_insert(module, &label_id_record);
            if (rc != SPIRV_OK) return rc;
            spirv_basic_block record = {
                .function_id = function->function_id, .label_id = instruction.words[1],
                .instruction_count = 1, .word_offset = offset, .word_count = instruction.word_count
            };
            rc = append_basic_block(module, &record);
            if (rc != SPIRV_OK) return rc;
            current_block = module->basic_block_count - 1;
            function->block_count++;
            block_terminated = 0;
            continue;
        }

        if (instruction_has_typed_result(instruction.opcode)) {
            const spirv_id_record *existing_result = instruction.word_count >= 3 ?
                spirv_find_id(module, instruction.words[2]) : NULL;
            if (instruction.opcode == 1 && !block && existing_result &&
                existing_result->kind == SPIRV_ID_UNDEFINED) continue;
            if (!block || instruction.word_count < 3 || !id_in_bound(module, instruction.words[2]))
                return SPIRV_INVALID;
            const spirv_id_record *result_type = spirv_find_id(module, instruction.words[1]);
            if (!result_type || result_type->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
            spirv_id_record result = {
                .id = instruction.words[2], .kind = SPIRV_ID_INSTRUCTION_RESULT,
                .type_id = instruction.words[1], .owner_function_id = function->function_id
            };
            int rc = id_table_insert(module, &result);
            if (rc != SPIRV_OK) return rc;
        }

        if (!block && function_terminator(instruction.opcode)) return SPIRV_INVALID;
        if (block) {
            if (block_terminated) {
                if (function_line_debug(instruction.opcode)) continue;
                return SPIRV_INVALID;
            }
            block->instruction_count++;
            block->word_count += instruction.word_count;
            if (function_terminator(instruction.opcode)) {
                block->terminator_opcode = instruction.opcode;
                block_terminated = 1;
            }
        }
    }
    if (next < 0 || current_function != no_index) return SPIRV_INVALID;
    return SPIRV_OK;
}

static int branch_target_in_function(const spirv_module *module, uint32_t label_id, uint32_t function_id) {
    const spirv_id_record *label = spirv_find_id(module, label_id);
    return label && label->kind == SPIRV_ID_LABEL && label->owner_function_id == function_id;
}

// Validate the label operands whose positions are independent of value types. OpSwitch case-target positions
// depend on the selector's integer width, so its default target is checked here and case operands await typed IDs.
static int validate_function_branch_targets(const spirv_module *module) {
    for (uint32_t i = 0; i < module->function_count; i++) {
        const spirv_function_record *function = &module->functions[i];
        spirv_iterator iterator;
        if (spirv_function_iter_begin(module, function, &iterator) != SPIRV_OK) return SPIRV_INVALID;
        spirv_instruction instruction;
        int next;
        while ((next = spirv_iter_next(&iterator, &instruction)) > 0) {
            switch (instruction.opcode) {
            case 249: // OpBranch
                if (instruction.word_count != 2 ||
                    !branch_target_in_function(module, instruction.words[1], function->function_id))
                    return SPIRV_INVALID;
                break;
            case 250: // OpBranchConditional (branch weights are both present or both absent)
                if ((instruction.word_count != 4 && instruction.word_count != 6) ||
                    !branch_target_in_function(module, instruction.words[2], function->function_id) ||
                    !branch_target_in_function(module, instruction.words[3], function->function_id))
                    return SPIRV_INVALID;
                if (module->version >= 0x00010600u && instruction.words[2] == instruction.words[3])
                    return SPIRV_INVALID;
                if (instruction.word_count == 6) {
                    uint64_t weight_sum = (uint64_t)instruction.words[4] + instruction.words[5];
                    if (!weight_sum || weight_sum > UINT32_MAX) return SPIRV_INVALID;
                }
                break;
            case 251: // OpSwitch: selector, default label, then width-dependent literal/label pairs
                if (instruction.word_count < 3 ||
                    !branch_target_in_function(module, instruction.words[2], function->function_id))
                    return SPIRV_INVALID;
                {
                    const spirv_id_record *selector = spirv_find_id(module, instruction.words[1]);
                    if (!selector) break; // Its defining opcode is outside this reader's typed-result subset.
                    if (selector->kind != SPIRV_ID_CONSTANT && selector->kind != SPIRV_ID_FUNCTION_PARAMETER &&
                        selector->kind != SPIRV_ID_INSTRUCTION_RESULT && selector->kind != SPIRV_ID_UNDEFINED)
                        return SPIRV_INVALID;
                    const spirv_id_record *selector_type = spirv_find_id(module, selector->type_id);
                    if (!selector_type || selector_type->kind != SPIRV_ID_TYPE ||
                        selector_type->type_kind != SPIRV_TYPE_INT || !selector_type->width) return SPIRV_INVALID;
                    uint64_t literal_words = ((uint64_t)selector_type->width + 31u) / 32u;
                    uint64_t pair_words = literal_words + 1u;
                    uint64_t payload_words = instruction.word_count - 3u;
                    if (!literal_words || payload_words % pair_words) return SPIRV_INVALID;
                    for (uint64_t operand = 3; operand < instruction.word_count; operand += pair_words) {
                        uint64_t target_index = operand + literal_words;
                        if (target_index >= instruction.word_count ||
                            !branch_target_in_function(module, instruction.words[target_index], function->function_id))
                            return SPIRV_INVALID;
                    }
                }
                break;
            default:
                break;
            }
        }
        if (next < 0) return SPIRV_INVALID;
    }
    return SPIRV_OK;
}

int spirv_read(const void *data, size_t size, spirv_module *module) {
    if (!module) return SPIRV_INVALID;
    memset(module, 0, sizeof *module);
    if (!data || size < 5 * sizeof(uint32_t) || (size & 3))
        return fail(module, SPIRV_INVALID, "SPIR-V input must contain a five-word header and whole words");
    size_t words = size / sizeof(uint32_t);
    const uint8_t *bytes = data;
    uint32_t magic = load_word_le(bytes);
    int swap = magic == swap_word(SPIRV_MAGIC);
    if (magic != SPIRV_MAGIC && !swap)
        return fail(module, SPIRV_INVALID, "invalid SPIR-V magic number");
    if (words > SIZE_MAX / sizeof(uint32_t))
        return fail(module, SPIRV_OUT_OF_MEMORY, "SPIR-V module is too large");
    module->words = malloc(words * sizeof(uint32_t));
    if (!module->words) return fail(module, SPIRV_OUT_OF_MEMORY, "out of memory reading SPIR-V module");
    module->word_count = words;
    for (size_t i = 0; i < words; i++) {
        uint32_t word = load_word_le(bytes + 4 * i);
        module->words[i] = swap ? swap_word(word) : word;
    }

    module->version = module->words[1];
    module->generator = module->words[2];
    module->id_bound = module->words[3];
    module->schema = module->words[4];
    if ((module->version & 0xff0000ffu) || spirv_version_major(module->version) != 1 ||
        module->version > SPIRV_MAX_VERSION)
        return fail(module, SPIRV_UNSUPPORTED_VERSION, "unsupported or malformed SPIR-V version");
    if (!module->id_bound)
        return fail(module, SPIRV_INVALID, "SPIR-V id bound must be nonzero");
    if (module->schema)
        return fail(module, SPIRV_INVALID, "SPIR-V instruction schema is reserved and must be zero");

    size_t offset = 5;
    while (offset < words) {
        uint32_t header = module->words[offset];
        uint32_t count = header >> 16;
        if (!count)
            return fail(module, SPIRV_INVALID, "SPIR-V instruction has a zero word count");
        if (count > words - offset)
            return fail(module, SPIRV_INVALID, "SPIR-V instruction extends past the module");
        module->instruction_count++;
        offset += count;
    }

    offset = 5;
    int in_function = 0;
    while (offset < words) {
        uint32_t header = module->words[offset];
        uint32_t count = header >> 16;
        uint32_t opcode = header & 0xffffu;
        int rc = SPIRV_OK;
        if (opcode == 54) in_function = 1;
        if (opcode == 56) in_function = 0;
        if (opcode == 14) rc = parse_memory_model(module, module->words + offset, count);
        else if (opcode == 15) rc = parse_entry_point(module, offset, count);
        else if (opcode == 16 || opcode == 331) rc = parse_execution_mode(module, offset, count, opcode);
        else if (opcode == 71 || opcode == 72)
            rc = parse_decoration(module, module->words + offset, count, opcode == 72, offset);
        else if (opcode == 73)
            rc = parse_decoration_group(module, module->words + offset, count, offset);
        else if (opcode == 74 || opcode == 75)
            rc = parse_group_decorate(module, module->words + offset, count, opcode == 75, offset);
        else if (opcode == 1 && !in_function) rc = parse_module_undef(module, module->words + offset, count);
        else if (opcode == 11) rc = in_function ? SPIRV_INVALID :
            parse_ext_inst_import(module, module->words + offset, count);
        else rc = parse_semantic_declaration(module, offset, count, opcode);
        if (rc != SPIRV_OK)
            return fail(module, rc, rc == SPIRV_OUT_OF_MEMORY ? "out of memory parsing SPIR-V semantic records" :
                        "malformed SPIR-V entry point, execution mode, type, variable, decoration or constant");
        offset += count;
    }
    int rc = apply_decorations(module);
    if (rc != SPIRV_OK) return fail(module, rc, "SPIR-V decoration target or operands are invalid");
    rc = validate_semantic_records(module);
    if (rc != SPIRV_OK) return fail(module, rc, "SPIR-V type, variable or decoration references are invalid");
    rc = resolve_specialization_constant_values(module);
    if (rc != SPIRV_OK) return fail(module, rc, "SPIR-V specialization expression is malformed or unsupported");
    rc = resolve_local_size_ids(module);
    if (rc != SPIRV_OK) return fail(module, rc, "LocalSizeId does not refer to an integer scalar constant");
    rc = index_function_bodies(module);
    if (rc != SPIRV_OK) return fail(module, rc, rc == SPIRV_OUT_OF_MEMORY ?
                                    "out of memory indexing SPIR-V function bodies" :
                                    "malformed SPIR-V function or basic block structure");
    rc = validate_function_branch_targets(module);
    if (rc != SPIRV_OK) return fail(module, rc, "SPIR-V branch target is missing or belongs to another function");
    return SPIRV_OK;
}

int spirv_vulkan_memory_model_supported(const spirv_module *module) {
    return module && module->has_memory_model &&
           module->addressing_model == SPIRV_ADDRESSING_MODEL_LOGICAL &&
           module->memory_model == SPIRV_MEMORY_MODEL_GLSL450;
}

const spirv_entry_point *spirv_find_entry_point(const spirv_module *module, const char *name,
                                                 uint32_t execution_model) {
    if (!module || !name) return NULL;
    for (uint32_t i = 0; i < module->entry_point_count; i++) {
        const spirv_entry_point *entry = &module->entry_points[i];
        if (!strcmp(entry->name, name) &&
            (execution_model == SPIRV_EXECUTION_MODEL_ANY || entry->execution_model == execution_model)) return entry;
    }
    return NULL;
}

uint32_t spirv_variable_count(const spirv_module *module) {
    return module ? module->variable_count : 0;
}

const spirv_id_record *spirv_variable_at(const spirv_module *module, uint32_t index) {
    if (!module || index >= module->variable_count) return NULL;
    return spirv_find_id(module, module->variable_ids[index]);
}

int spirv_reflect_descriptor(const spirv_module *module, const spirv_id_record *variable,
                             spirv_descriptor_info *descriptor) {
    if (!module || !variable || !descriptor || variable->kind != SPIRV_ID_VARIABLE ||
        spirv_find_id(module, variable->id) != variable ||
        !variable->has_descriptor_set || !variable->has_binding) return SPIRV_INVALID;
    const spirv_id_record *type = spirv_find_id(module, variable->pointee_type_id);
    if (!type || type->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
    uint64_t array_count = 1;
    uint32_t runtime_array = 0;
    while (type->type_kind == SPIRV_TYPE_ARRAY || type->type_kind == SPIRV_TYPE_RUNTIME_ARRAY) {
        if (type->type_kind == SPIRV_TYPE_RUNTIME_ARRAY) {
            runtime_array = 1;
            array_count = 0;
        } else if (!runtime_array) {
            const spirv_id_record *length = spirv_find_id(module, type->length_id);
            const spirv_id_record *length_type = length ? spirv_find_id(module, length->type_id) : NULL;
            if (!length || length->kind != SPIRV_ID_CONSTANT || length->constant_kind != SPIRV_CONSTANT_SCALAR ||
                !length_type || length_type->kind != SPIRV_ID_TYPE || length_type->type_kind != SPIRV_TYPE_INT ||
                length_type->width > 64 || !length->value_bits) return SPIRV_INVALID;
            if (length_type->signedness) {
                uint64_t sign = UINT64_C(1) << (length_type->width - 1);
                if (length->value_bits & sign) return SPIRV_INVALID;
            }
            if (length->value_bits > UINT32_MAX || array_count > UINT32_MAX / length->value_bits)
                return SPIRV_UNSUPPORTED;
            array_count *= length->value_bits;
        }
        type = spirv_find_id(module, type->element_type);
        if (!type || type->kind != SPIRV_ID_TYPE) return SPIRV_INVALID;
    }

    uint32_t descriptor_type = SPIRV_DESCRIPTOR_NONE;
    if (variable->storage_class == 2) { // Uniform
        if (type->type_kind != SPIRV_TYPE_STRUCT) return SPIRV_UNSUPPORTED;
        if (type->decoration_flags & SPIRV_DECORATION_BLOCK) descriptor_type = SPIRV_DESCRIPTOR_UNIFORM_BUFFER;
        else if (type->decoration_flags & SPIRV_DECORATION_BUFFER_BLOCK) descriptor_type = SPIRV_DESCRIPTOR_STORAGE_BUFFER;
    } else if (variable->storage_class == 12) { // StorageBuffer
        if (type->type_kind != SPIRV_TYPE_STRUCT ||
            !(type->decoration_flags & SPIRV_DECORATION_BLOCK)) return SPIRV_UNSUPPORTED;
        descriptor_type = SPIRV_DESCRIPTOR_STORAGE_BUFFER;
    } else if (variable->storage_class == 0) { // UniformConstant
        if (type->type_kind == SPIRV_TYPE_SAMPLER) descriptor_type = SPIRV_DESCRIPTOR_SAMPLER;
        else if (type->type_kind == SPIRV_TYPE_SAMPLED_IMAGE)
            descriptor_type = SPIRV_DESCRIPTOR_COMBINED_IMAGE_SAMPLER;
        else if (type->type_kind == SPIRV_TYPE_IMAGE) {
            if (type->image_dimension == 6) descriptor_type = SPIRV_DESCRIPTOR_INPUT_ATTACHMENT;
            else if (type->image_dimension == 5 && type->image_sampled == 1)
                descriptor_type = SPIRV_DESCRIPTOR_UNIFORM_TEXEL_BUFFER;
            else if (type->image_dimension == 5 && type->image_sampled == 2)
                descriptor_type = SPIRV_DESCRIPTOR_STORAGE_TEXEL_BUFFER;
            else if (type->image_sampled == 1) descriptor_type = SPIRV_DESCRIPTOR_SAMPLED_IMAGE;
            else if (type->image_sampled == 2) descriptor_type = SPIRV_DESCRIPTOR_STORAGE_IMAGE;
        }
    }
    if (descriptor_type == SPIRV_DESCRIPTOR_NONE) return SPIRV_UNSUPPORTED;
    *descriptor = (spirv_descriptor_info){
        .variable_id = variable->id, .descriptor_set = variable->descriptor_set,
        .binding = variable->binding, .descriptor_type = descriptor_type,
        .array_count = runtime_array ? 0 : (uint32_t)array_count,
        .is_runtime_array = runtime_array, .base_type_id = type->id
    };
    return SPIRV_OK;
}

uint32_t spirv_descriptor_count(const spirv_module *module) {
    if (!module) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < module->variable_count; i++) {
        const spirv_id_record *variable = spirv_variable_at(module, i);
        if (variable && variable->has_descriptor_set && variable->has_binding) count++;
    }
    return count;
}

int spirv_descriptor_at(const spirv_module *module, uint32_t index, spirv_descriptor_info *descriptor) {
    if (!module || !descriptor) return SPIRV_INVALID;
    for (uint32_t i = 0; i < module->variable_count; i++) {
        const spirv_id_record *variable = spirv_variable_at(module, i);
        if (!variable || !variable->has_descriptor_set || !variable->has_binding) continue;
        if (!index--) return spirv_reflect_descriptor(module, variable, descriptor);
    }
    return SPIRV_INVALID;
}

uint32_t spirv_function_count(const spirv_module *module) {
    return module ? module->function_count : 0;
}

const spirv_function_record *spirv_function_at(const spirv_module *module, uint32_t index) {
    if (!module || index >= module->function_count) return NULL;
    return &module->functions[index];
}

const spirv_function_record *spirv_find_function(const spirv_module *module, uint32_t function_id) {
    if (!module || !function_id) return NULL;
    for (uint32_t i = 0; i < module->function_count; i++)
        if (module->functions[i].function_id == function_id) return &module->functions[i];
    return NULL;
}

static int owns_function(const spirv_module *module, const spirv_function_record *function) {
    if (!module || !function) return 0;
    for (uint32_t i = 0; i < module->function_count; i++)
        if (&module->functions[i] == function) return 1;
    return 0;
}

uint32_t spirv_function_block_count(const spirv_module *module, const spirv_function_record *function) {
    return owns_function(module, function) ? function->block_count : 0;
}

const spirv_basic_block *spirv_function_block_at(const spirv_module *module,
                                                  const spirv_function_record *function, uint32_t index) {
    if (!owns_function(module, function) || index >= function->block_count ||
        function->first_block > module->basic_block_count ||
        index >= module->basic_block_count - function->first_block) return NULL;
    const spirv_basic_block *block = &module->basic_blocks[function->first_block + index];
    return block->function_id == function->function_id ? block : NULL;
}

void spirv_iter_begin(const spirv_module *module, spirv_iterator *iterator) {
    if (!iterator) return;
    iterator->module = module;
    iterator->word_offset = 5;
    iterator->end_word_offset = module ? module->word_count : 0;
}

int spirv_function_iter_begin(const spirv_module *module, const spirv_function_record *function,
                              spirv_iterator *iterator) {
    if (!iterator || !owns_function(module, function) || function->word_offset < 5 ||
        function->word_offset > module->word_count || function->word_count > module->word_count - function->word_offset)
        return SPIRV_INVALID;
    iterator->module = module;
    iterator->word_offset = function->word_offset;
    iterator->end_word_offset = function->word_offset + function->word_count;
    return SPIRV_OK;
}

int spirv_block_iter_begin(const spirv_module *module, const spirv_basic_block *block,
                           spirv_iterator *iterator) {
    if (!module || !block || !iterator || block->word_offset < 5 || block->word_offset > module->word_count ||
        block->word_count > module->word_count - block->word_offset) return SPIRV_INVALID;
    int owned = 0;
    for (uint32_t i = 0; i < module->basic_block_count; i++)
        if (&module->basic_blocks[i] == block) { owned = 1; break; }
    if (!owned) return SPIRV_INVALID;
    iterator->module = module;
    iterator->word_offset = block->word_offset;
    iterator->end_word_offset = block->word_offset + block->word_count;
    return SPIRV_OK;
}

int spirv_iter_next(spirv_iterator *iterator, spirv_instruction *instruction) {
    if (!iterator || !instruction || !iterator->module || !iterator->module->words ||
        iterator->module->word_count < 5 || iterator->word_offset < 5 ||
        iterator->word_offset > iterator->module->word_count || iterator->end_word_offset < 5 ||
        iterator->end_word_offset > iterator->module->word_count || iterator->word_offset > iterator->end_word_offset)
        return SPIRV_INVALID;
    const spirv_module *module = iterator->module;
    if (iterator->word_offset == iterator->end_word_offset) return 0;
    uint32_t header = module->words[iterator->word_offset];
    uint32_t count = header >> 16;
    if (!count || count > UINT16_MAX || count > iterator->end_word_offset - iterator->word_offset)
        return SPIRV_INVALID;
    instruction->opcode = (uint16_t)header;
    instruction->word_count = (uint16_t)count;
    instruction->words = module->words + iterator->word_offset;
    iterator->word_offset += count;
    return 1;
}
