// A small, dependency-free reader for SPIR-V modules. It validates framing and the opcode subset needed to build
// early semantic metadata; complete opcode and Vulkan environment validation belongs to later reader layers.
#ifndef VX_SPIRV_H
#define VX_SPIRV_H
#include <stddef.h>
#include <stdint.h>

#define SPIRV_MAGIC 0x07230203u
#define SPIRV_MAX_VERSION 0x00010600u

enum {
    SPIRV_OK = 0,
    SPIRV_INVALID = -1,
    SPIRV_UNSUPPORTED_VERSION = -2,
    SPIRV_OUT_OF_MEMORY = -3,
    SPIRV_UNSUPPORTED = -4
};

enum {
    SPIRV_EXECUTION_MODEL_VERTEX = 0,
    SPIRV_EXECUTION_MODEL_TESS_CONTROL = 1,
    SPIRV_EXECUTION_MODEL_TESS_EVALUATION = 2,
    SPIRV_EXECUTION_MODEL_GEOMETRY = 3,
    SPIRV_EXECUTION_MODEL_FRAGMENT = 4,
    SPIRV_EXECUTION_MODEL_GL_COMPUTE = 5,
    SPIRV_EXECUTION_MODEL_KERNEL = 6,
    SPIRV_EXECUTION_MODEL_ANY = UINT32_MAX
};

enum {
    SPIRV_LOCAL_SIZE_NONE = 0,
    SPIRV_LOCAL_SIZE_LITERAL,
    SPIRV_LOCAL_SIZE_IDS
};

enum {
    SPIRV_ADDRESSING_MODEL_LOGICAL = 0,
    SPIRV_MEMORY_MODEL_GLSL450 = 1
};

enum {
    SPIRV_ID_TYPE = 1,
    SPIRV_ID_CONSTANT,
    SPIRV_ID_VARIABLE,
    SPIRV_ID_DECORATION_GROUP,
    SPIRV_ID_FUNCTION,
    SPIRV_ID_FUNCTION_PARAMETER,
    SPIRV_ID_LABEL,
    SPIRV_ID_INSTRUCTION_RESULT,
    SPIRV_ID_UNDEFINED,
    SPIRV_ID_EXT_INST_IMPORT
};

enum {
    SPIRV_TYPE_VOID = 1,
    SPIRV_TYPE_BOOL,
    SPIRV_TYPE_INT,
    SPIRV_TYPE_FLOAT,
    SPIRV_TYPE_VECTOR,
    SPIRV_TYPE_MATRIX,
    SPIRV_TYPE_IMAGE,
    SPIRV_TYPE_SAMPLER,
    SPIRV_TYPE_SAMPLED_IMAGE,
    SPIRV_TYPE_ARRAY,
    SPIRV_TYPE_RUNTIME_ARRAY,
    SPIRV_TYPE_STRUCT,
    SPIRV_TYPE_POINTER,
    SPIRV_TYPE_FUNCTION
};

enum {
    SPIRV_CONSTANT_BOOL = 1,
    SPIRV_CONSTANT_SCALAR,
    SPIRV_CONSTANT_NULL,
    SPIRV_CONSTANT_COMPOSITE
};

enum {
    SPIRV_DESCRIPTOR_NONE = 0,
    SPIRV_DESCRIPTOR_UNIFORM_BUFFER,
    SPIRV_DESCRIPTOR_STORAGE_BUFFER,
    SPIRV_DESCRIPTOR_SAMPLED_IMAGE,
    SPIRV_DESCRIPTOR_STORAGE_IMAGE,
    SPIRV_DESCRIPTOR_SAMPLER,
    SPIRV_DESCRIPTOR_COMBINED_IMAGE_SAMPLER,
    SPIRV_DESCRIPTOR_INPUT_ATTACHMENT,
    SPIRV_DESCRIPTOR_UNIFORM_TEXEL_BUFFER,
    SPIRV_DESCRIPTOR_STORAGE_TEXEL_BUFFER
};

enum {
    SPIRV_DECORATION_BLOCK = 1u << 0,
    SPIRV_DECORATION_BUFFER_BLOCK = 1u << 1,
    SPIRV_DECORATION_NON_WRITABLE = 1u << 2,
    SPIRV_DECORATION_NON_READABLE = 1u << 3,
    SPIRV_DECORATION_ROW_MAJOR = 1u << 4,
    SPIRV_DECORATION_COL_MAJOR = 1u << 5,
    SPIRV_DECORATION_NO_PERSPECTIVE = 1u << 6,
    SPIRV_DECORATION_FLAT = 1u << 7,
    SPIRV_DECORATION_CENTROID = 1u << 8,
    SPIRV_DECORATION_SAMPLE = 1u << 9,
    SPIRV_DECORATION_INVARIANT = 1u << 10
};

typedef struct {
    uint32_t type_id;
    uint32_t decoration_flags;
    uint32_t offset, matrix_stride, location, component, index, builtin;
    uint32_t has_offset, has_matrix_stride, has_location, has_component, has_index, has_builtin;
} spirv_member_record;

typedef struct {
    uint32_t id;
    uint32_t kind;                // SPIRV_ID_*; body IDs carry identity only unless otherwise documented
    uint32_t type_kind;           // SPIRV_TYPE_* when kind is SPIRV_ID_TYPE
    uint32_t type_id;             // declared type for constants/parameters
    uint32_t owner_function_id;   // enclosing function for function-parameter and label IDs
    uint32_t width;               // integer/float bit width
    uint32_t signedness;          // OpTypeInt's Signedness operand
    uint32_t element_type;        // vector component or matrix column type
    uint32_t element_count;       // vector/matrix component count
    uint32_t constant_kind;
    uint32_t is_specialization;
    uint32_t specialization_opcode;             // Embedded opcode for OpSpecConstantOp; zero for literal constants.
    uint32_t specialization_operand_count;
    uint32_t *specialization_operand_ids;
    uint32_t specialization_evaluation_state;   // 0 pending, 1 resolving, 2 resolved.
    uint32_t literal_word_count;
    uint32_t *literal_words;      // little-word-order payload for scalar literal constants
    uint32_t constituent_count;
    uint32_t *constituent_ids;    // OpConstantComposite / OpSpecConstantComposite IDs
    uint64_t value_bits;          // low 64 bits, when present
    uint32_t storage_class;       // OpTypePointer or OpVariable storage class
    uint32_t initializer_id;      // optional OpVariable initializer
    uint32_t pointee_type_id;     // resolved for pointer types and variables
    uint32_t length_id;           // OpTypeArray length constant
    uint32_t image_dimension, image_depth, image_arrayed, image_multisampled;
    uint32_t image_sampled, image_format, image_access_qualifier;
    uint32_t decoration_flags;
    uint32_t specialization_id, has_specialization_id;
    uint32_t array_stride, binding, descriptor_set, location, component, index, builtin, offset, matrix_stride;
    uint32_t has_array_stride, has_binding, has_descriptor_set, has_location;
    uint32_t has_component, has_index, has_builtin, has_offset, has_matrix_stride;
    uint32_t member_count;
    spirv_member_record *members;
} spirv_id_record;

typedef struct {
    uint32_t id;
    char *name;
} spirv_ext_inst_import;

typedef struct {
    uint32_t execution_model;
    uint32_t function_id;
    char *name;
    uint32_t *interfaces;
    uint32_t interface_count;
    uint32_t local_size_kind;
    uint32_t local_size[3];       // literal dimensions, or the three IDs for LocalSizeId
    uint64_t local_size_default[3];
    uint32_t has_local_size_default;
} spirv_entry_point;

typedef struct {
    uint32_t variable_id;
    uint32_t descriptor_set, binding;
    uint32_t descriptor_type;
    uint32_t array_count;          // zero when the declaration uses a runtime descriptor array
    uint32_t is_runtime_array;
    uint32_t base_type_id;         // descriptor element type after unwrapping descriptor arrays
} spirv_descriptor_info;

typedef struct {
    uint32_t function_id, result_type_id, function_type_id;
    uint32_t parameter_count, first_block, block_count;
    size_t word_offset, word_count; // includes OpFunction and OpFunctionEnd
} spirv_function_record;

typedef struct {
    uint32_t function_id, label_id;
    uint32_t instruction_count;
    uint16_t terminator_opcode;
    size_t word_offset, word_count; // includes OpLabel and the terminating instruction
} spirv_basic_block;

typedef struct {
    uint32_t version;
    uint32_t generator;
    uint32_t id_bound;
    uint32_t schema;
    uint32_t has_memory_model;
    uint32_t addressing_model;
    uint32_t memory_model;
    uint32_t *words;             // canonical host-endian copy; includes the five-word module header
    size_t word_count;
    size_t instruction_count;
    spirv_entry_point *entry_points;
    uint32_t entry_point_count;
    spirv_function_record *functions;
    uint32_t function_count, function_capacity;
    spirv_basic_block *basic_blocks;
    uint32_t basic_block_count, basic_block_capacity;
    void *id_records;             // open-addressed semantic ID table, owned by this module
    size_t id_capacity;
    size_t id_count;
    uint32_t *variable_ids;       // OpVariable result IDs in module order
    uint32_t variable_count;
    uint32_t variable_capacity;
    void *decorations;            // pending OpDecorate/OpMemberDecorate records
    size_t decoration_count;
    size_t decoration_capacity;
    void *decoration_groups;      // OpDecorationGroup IDs and definition positions
    size_t decoration_group_count;
    size_t decoration_group_capacity;
    void *group_applications;     // OpGroupDecorate/OpGroupMemberDecorate applications
    size_t group_application_count;
    size_t group_application_capacity;
    spirv_ext_inst_import *ext_inst_imports;
    uint32_t ext_inst_import_count;
    uint32_t ext_inst_import_capacity;
    char error[192];
} spirv_module;

typedef struct {
    uint16_t opcode;
    uint16_t word_count;         // includes the instruction header word
    const uint32_t *words;       // points at the instruction header word in module->words
} spirv_instruction;

typedef struct {
    const spirv_module *module;
    size_t word_offset;           // starts at word 5
    size_t end_word_offset;       // exclusive bound; module end for the module iterator
} spirv_iterator;

// Returns SPIRV_OK or an error with a human-readable module->error. Accepts byte streams in either endian order.
int spirv_read(const void *data, size_t size, spirv_module *module);
void spirv_free(spirv_module *module);
// This Vulkan 1.0 frontend supports the Logical + GLSL450 SPIR-V memory model only.
int spirv_vulkan_memory_model_supported(const spirv_module *module);
const spirv_entry_point *spirv_find_entry_point(const spirv_module *module, const char *name,
                                                 uint32_t execution_model);
const spirv_id_record *spirv_find_id(const spirv_module *module, uint32_t id);
const char *spirv_ext_inst_import_name(const spirv_module *module, uint32_t id);
uint32_t spirv_variable_count(const spirv_module *module);
const spirv_id_record *spirv_variable_at(const spirv_module *module, uint32_t index);
int spirv_reflect_descriptor(const spirv_module *module, const spirv_id_record *variable,
                             spirv_descriptor_info *descriptor);
uint32_t spirv_descriptor_count(const spirv_module *module);
int spirv_descriptor_at(const spirv_module *module, uint32_t index, spirv_descriptor_info *descriptor);
uint32_t spirv_function_count(const spirv_module *module);
const spirv_function_record *spirv_function_at(const spirv_module *module, uint32_t index);
const spirv_function_record *spirv_find_function(const spirv_module *module, uint32_t function_id);
uint32_t spirv_function_block_count(const spirv_module *module, const spirv_function_record *function);
const spirv_basic_block *spirv_function_block_at(const spirv_module *module,
                                                  const spirv_function_record *function, uint32_t index);

void spirv_iter_begin(const spirv_module *module, spirv_iterator *iterator);
int spirv_function_iter_begin(const spirv_module *module, const spirv_function_record *function,
                              spirv_iterator *iterator);
int spirv_block_iter_begin(const spirv_module *module, const spirv_basic_block *block,
                           spirv_iterator *iterator);
// Returns 1 for an instruction, 0 at end, or SPIRV_INVALID if iterator/module state is corrupt.
int spirv_iter_next(spirv_iterator *iterator, spirv_instruction *instruction);

static inline uint32_t spirv_version_major(uint32_t version) { return (version >> 16) & 0xffu; }
static inline uint32_t spirv_version_minor(uint32_t version) { return (version >> 8) & 0xffu; }

#endif
