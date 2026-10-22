#include "spirv_ir.h"
#include "gpu_ir_msl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(const char *name, int condition) {
    if (condition) printf("spirv: %s: ok\n", name);
    else { printf("spirv: %s: FAILED\n", name); failures++; }
}

static void write_le(uint8_t *dst, uint32_t word) {
    dst[0] = (uint8_t)word;
    dst[1] = (uint8_t)(word >> 8);
    dst[2] = (uint8_t)(word >> 16);
    dst[3] = (uint8_t)(word >> 24);
}

static void write_be(uint8_t *dst, uint32_t word) {
    dst[0] = (uint8_t)(word >> 24);
    dst[1] = (uint8_t)(word >> 16);
    dst[2] = (uint8_t)(word >> 8);
    dst[3] = (uint8_t)word;
}

static void make_module(uint8_t *data, int big_endian, uint32_t version, uint32_t bound, uint32_t schema,
                        const uint32_t *instructions, size_t instruction_words) {
    uint32_t header[5] = { SPIRV_MAGIC, version, 0x00010600u, bound, schema };
    for (size_t i = 0; i < 5; i++) (big_endian ? write_be : write_le)(data + i * 4, header[i]);
    for (size_t i = 0; i < instruction_words; i++)
        (big_endian ? write_be : write_le)(data + (i + 5) * 4, instructions[i]);
}

int main(void) {
    // OpNop; OpMemoryModel Logical GLSL450; an unknown opcode with one operand (framing remains forward-compatible).
    const uint32_t instructions[] = { 0x00010000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0002ea60u, 0x12345678u };
    uint8_t bytes[(5 + sizeof instructions / sizeof instructions[0]) * 4];
    spirv_module module;
    make_module(bytes, 0, 0x00010600u, 2, 0, instructions, sizeof instructions / sizeof instructions[0]);
    int rc = spirv_read(bytes, sizeof bytes, &module);
    int ok = rc == SPIRV_OK && module.version == 0x00010600u && module.generator == 0x00010600u &&
             module.id_bound == 2 && module.instruction_count == 3 && module.has_memory_model &&
             module.addressing_model == SPIRV_ADDRESSING_MODEL_LOGICAL &&
             module.memory_model == SPIRV_MEMORY_MODEL_GLSL450 && spirv_vulkan_memory_model_supported(&module);
    check("header, memory model and forward-compatible instruction framing", ok);
    if (rc == SPIRV_OK) {
        spirv_iterator iterator;
        spirv_instruction instruction;
        spirv_iter_begin(&module, &iterator);
        ok = spirv_iter_next(&iterator, &instruction) == 1 && instruction.opcode == 0 && instruction.word_count == 1 &&
             spirv_iter_next(&iterator, &instruction) == 1 && instruction.opcode == 14 && instruction.word_count == 3 &&
             spirv_iter_next(&iterator, &instruction) == 1 && instruction.opcode == 60000 && instruction.word_count == 2 &&
             instruction.words[1] == 0x12345678u && spirv_iter_next(&iterator, &instruction) == 0;
        check("sequential instruction iteration", ok);
    } else check("sequential instruction iteration", 0);
    spirv_free(&module);

    make_module(bytes, 0, 0x00010600u, 1, 0, NULL, 0);
    rc = spirv_read(bytes, 5 * 4, &module);
    check("Vulkan compatibility rejects a missing OpMemoryModel", rc == SPIRV_OK &&
          !spirv_vulkan_memory_model_supported(&module));
    spirv_free(&module);

    const uint32_t physical_memory_model[] = { 0x0003000eu, 1, SPIRV_MEMORY_MODEL_GLSL450 };
    make_module(bytes, 0, 0x00010600u, 1, 0, physical_memory_model,
                sizeof physical_memory_model / sizeof physical_memory_model[0]);
    rc = spirv_read(bytes, (5 + sizeof physical_memory_model / sizeof physical_memory_model[0]) * 4, &module);
    check("Vulkan compatibility rejects a physical addressing model when buffer device address is unavailable",
          rc == SPIRV_OK && module.has_memory_model && module.addressing_model == 1 &&
          !spirv_vulkan_memory_model_supported(&module));
    spirv_free(&module);

    const uint32_t vulkan_memory_model[] = { 0x0003000eu, SPIRV_ADDRESSING_MODEL_LOGICAL, 3 };
    make_module(bytes, 0, 0x00010600u, 1, 0, vulkan_memory_model,
                sizeof vulkan_memory_model / sizeof vulkan_memory_model[0]);
    rc = spirv_read(bytes, (5 + sizeof vulkan_memory_model / sizeof vulkan_memory_model[0]) * 4, &module);
    check("Vulkan compatibility rejects the Vulkan memory model while its feature is unadvertised",
          rc == SPIRV_OK && module.memory_model == 3 && !spirv_vulkan_memory_model_supported(&module));
    spirv_free(&module);

    const uint32_t duplicate_memory_model[] = {
        0x0003000eu, SPIRV_ADDRESSING_MODEL_LOGICAL, SPIRV_MEMORY_MODEL_GLSL450,
        0x0003000eu, SPIRV_ADDRESSING_MODEL_LOGICAL, SPIRV_MEMORY_MODEL_GLSL450
    };
    make_module(bytes, 0, 0x00010600u, 1, 0, duplicate_memory_model,
                sizeof duplicate_memory_model / sizeof duplicate_memory_model[0]);
    check("duplicate OpMemoryModel instructions are rejected", spirv_read(bytes,
          (5 + sizeof duplicate_memory_model / sizeof duplicate_memory_model[0]) * 4, &module) == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t extended_import[] = {
        0x0006000bu, 1, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0
    };
    uint8_t extended_import_bytes[(5 + sizeof extended_import / sizeof extended_import[0]) * 4];
    make_module(extended_import_bytes, 0, 0x00010600u, 2, 0, extended_import,
                sizeof extended_import / sizeof extended_import[0]);
    rc = spirv_read(extended_import_bytes, sizeof extended_import_bytes, &module);
    check("GLSL extended-instruction imports are indexed with their IDs and names",
          rc == SPIRV_OK && spirv_find_id(&module, 1) &&
          spirv_find_id(&module, 1)->kind == SPIRV_ID_EXT_INST_IMPORT &&
          spirv_ext_inst_import_name(&module, 1) &&
          !strcmp(spirv_ext_inst_import_name(&module, 1), "GLSL.std.450"));
    spirv_free(&module);

    const uint32_t unterminated_import[] = { 0x0003000bu, 1, 0x44434241u };
    uint8_t unterminated_import_bytes[(5 + sizeof unterminated_import / sizeof unterminated_import[0]) * 4];
    make_module(unterminated_import_bytes, 0, 0x00010600u, 2, 0, unterminated_import,
                sizeof unterminated_import / sizeof unterminated_import[0]);
    check("unterminated extended-instruction import names are rejected",
          spirv_read(unterminated_import_bytes, sizeof unterminated_import_bytes, &module) == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t duplicate_import[] = {
        0x0006000bu, 1, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0,
        0x0006000bu, 1, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0
    };
    uint8_t duplicate_import_bytes[(5 + sizeof duplicate_import / sizeof duplicate_import[0]) * 4];
    make_module(duplicate_import_bytes, 0, 0x00010600u, 2, 0, duplicate_import,
                sizeof duplicate_import / sizeof duplicate_import[0]);
    check("duplicate extended-instruction import IDs are rejected",
          spirv_read(duplicate_import_bytes, sizeof duplicate_import_bytes, &module) == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t function_body[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 1, 0x6e69616du, 0,
        0x00060010u, 1, 17, 1, 1, 1,
        0x00020013u, 2,                 // OpTypeVoid
        0x00030021u, 3, 2,              // OpTypeFunction
        0x00050036u, 2, 1, 0, 3,        // OpFunction
        0x000200f8u, 4,                 // OpLabel
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t function_bytes[(5 + sizeof function_body / sizeof function_body[0]) * 4];
    make_module(function_bytes, 0, 0x00010600u, 5, 0, function_body,
                sizeof function_body / sizeof function_body[0]);
    rc = spirv_read(function_bytes, sizeof function_bytes, &module);
    const spirv_function_record *function = rc == SPIRV_OK ? spirv_find_function(&module, 1) : NULL;
    const spirv_basic_block *basic_block = function ? spirv_function_block_at(&module, function, 0) : NULL;
    check("function and basic-block boundaries retain entry, body and terminator records",
          function && spirv_function_count(&module) == 1 && spirv_function_at(&module, 0) == function &&
          function->result_type_id == 2 && function->function_type_id == 3 &&
          function->parameter_count == 0 && spirv_function_block_count(&module, function) == 1 && basic_block &&
          basic_block->label_id == 4 && basic_block->instruction_count == 2 && basic_block->terminator_opcode == 253);
    if (function && basic_block) {
        spirv_iterator function_iterator, block_iterator;
        spirv_instruction instruction;
        uint16_t function_opcodes[4] = { 0 };
        uint32_t function_instruction_count = 0;
        int next_instruction = -1;
        int iter_ok = spirv_function_iter_begin(&module, function, &function_iterator) == SPIRV_OK;
        while (iter_ok && (next_instruction = spirv_iter_next(&function_iterator, &instruction)) > 0) {
            if (function_instruction_count >= 4) { iter_ok = 0; break; }
            function_opcodes[function_instruction_count++] = instruction.opcode;
        }
        iter_ok &= next_instruction == 0 && function_instruction_count == 4 && function_opcodes[0] == 54 &&
                   function_opcodes[1] == 248 && function_opcodes[2] == 253 && function_opcodes[3] == 56;
        iter_ok &= spirv_block_iter_begin(&module, basic_block, &block_iterator) == SPIRV_OK &&
                   spirv_iter_next(&block_iterator, &instruction) == 1 && instruction.opcode == 248 &&
                   spirv_iter_next(&block_iterator, &instruction) == 1 && instruction.opcode == 253 &&
                   spirv_iter_next(&block_iterator, &instruction) == 0;
        check("function and block iterators stop at their own encoded ranges", iter_ok);
    } else check("function and block iterators stop at their own encoded ranges", 0);
    spirv_free(&module);

    const uint32_t typed_shader[] = {
        0x00020011u, 6025,              // BitInstructions capability
        0x0007000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 10, 0x6e69616du, 0, 6, 9,
        0x00060010u, 10, 17, 1, 1, 1,
        0x00040047u, 6, 30, 3,          // input variable Location 3
        0x00040047u, 9, 34, 2,          // descriptor set 2
        0x00040047u, 9, 33, 4,          // binding 4
        0x00050048u, 4, 0, 35, 8,       // struct member Offset 8
        0x00020013u, 1,                 // void
        0x00040015u, 2, 32, 0,          // uint32
        0x00030021u, 3, 1,              // void()
        0x0003001eu, 4, 2,              // struct { uint }
        0x00040020u, 5, 1, 2,           // Input pointer to uint
        0x0002001au, 7,                 // sampler
        0x00040020u, 8, 0, 7,           // UniformConstant pointer to sampler
        0x0004002bu, 2, 11, 7,          // module-scope scalar constants
        0x0004002bu, 2, 12, 3,
        0x0004003bu, 5, 6, 1,           // input variable
        0x0004003bu, 8, 9, 0,           // sampler descriptor
        0x00050036u, 1, 10, 0, 3,       // entry function
        0x000200f8u, 13,
        0x00050080u, 2, 14, 11, 12,     // IAdd
        0x000400cdu, 2, 15, 11,         // OpBitCount
        0x000400ccu, 2, 16, 11,         // OpBitReverse
        0x000700c9u, 2, 17, 11, 12, 12, 12, // OpBitFieldInsert
        0x000600cau, 2, 18, 11, 12, 12, // OpBitFieldSExtract
        0x000600cbu, 2, 19, 11, 12, 12, // OpBitFieldUExtract
        0x000100fdu,
        0x00010038u
    };
    uint8_t typed_shader_bytes[(5 + sizeof typed_shader / sizeof typed_shader[0]) * 4];
    make_module(typed_shader_bytes, 0, 0x00010600u, 20, 0, typed_shader,
                sizeof typed_shader / sizeof typed_shader[0]);
    rc = spirv_read(typed_shader_bytes, sizeof typed_shader_bytes, &module);
    const spirv_entry_point *shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    gpu_ir_shader *common_shader = NULL;
    int shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_function *common_function = common_shader ? gpu_ir_shader_function_at(common_shader, 0) : NULL;
    const gpu_ir_shader_block *common_block = common_shader ? gpu_ir_shader_block_at(common_shader, 0) : NULL;
    uint32_t common_threadgroup_size[3] = { 0, 0, 0 };
    gpu_ir_shader_threadgroup_size(common_shader, common_threadgroup_size);
    const gpu_ir_shader_instruction *common_variable = common_shader ? gpu_ir_shader_instruction_at(common_shader, 2) : NULL;
    const gpu_ir_shader_instruction *common_descriptor = common_shader ? gpu_ir_shader_instruction_at(common_shader, 3) : NULL;
    const gpu_ir_shader_instruction *common_add = common_shader ? gpu_ir_shader_instruction_at(common_shader, 4) : NULL;
    const gpu_ir_shader_instruction *common_bit_count = common_shader ? gpu_ir_shader_instruction_at(common_shader, 5) : NULL;
    const gpu_ir_shader_instruction *common_bit_reverse = common_shader ? gpu_ir_shader_instruction_at(common_shader, 6) : NULL;
    const gpu_ir_shader_instruction *common_bit_field_insert = common_shader ? gpu_ir_shader_instruction_at(common_shader, 7) : NULL;
    const gpu_ir_shader_instruction *common_bit_field_signed_extract = common_shader ? gpu_ir_shader_instruction_at(common_shader, 8) : NULL;
    const gpu_ir_shader_instruction *common_bit_field_unsigned_extract = common_shader ? gpu_ir_shader_instruction_at(common_shader, 9) : NULL;
    int typed_shader_ok = shader_rc == GPU_IR_OK && common_shader && common_function && common_block && common_add &&
        common_bit_count && common_bit_reverse && common_bit_field_insert &&
        common_bit_field_signed_extract && common_bit_field_unsigned_extract &&
        gpu_ir_shader_source_api(common_shader) == GPU_IR_SHADER_SOURCE_SPIRV &&
        gpu_ir_shader_stage(common_shader) == GPU_IR_STAGE_COMPUTE &&
        gpu_ir_shader_entry_function(common_shader) == 10 &&
        common_threadgroup_size[0] == 1 && common_threadgroup_size[1] == 1 && common_threadgroup_size[2] == 1 &&
        gpu_ir_shader_type_count(common_shader) == 7 &&
        gpu_ir_shader_type_member_count(common_shader) == 1 &&
        gpu_ir_shader_type_member_at(common_shader, 0) &&
        gpu_ir_shader_type_member_at(common_shader, 0)->type_id == 2 &&
        gpu_ir_shader_type_member_at(common_shader, 0)->has_offset &&
        gpu_ir_shader_type_member_at(common_shader, 0)->offset == 8 &&
        common_variable && common_variable->opcode == GPU_IR_SHADER_OP_VARIABLE &&
        common_variable->storage_class == GPU_IR_SHADER_STORAGE_INPUT &&
        common_variable->source_storage_class == 1 && common_variable->has_location &&
        common_variable->location == 3 &&
        common_descriptor && common_descriptor->opcode == GPU_IR_SHADER_OP_VARIABLE &&
        common_descriptor->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT &&
        common_descriptor->source_storage_class == 0 && common_descriptor->has_descriptor_set &&
        common_descriptor->resource_set == 2 && common_descriptor->has_binding &&
        common_descriptor->binding == 4 &&
        common_function->id == 10 && common_function->parameter_count == 0 &&
        common_function->instruction_count == 7 && common_block->label_id == 13 &&
        common_block->instruction_count == 7 &&
        common_block->terminator_opcode == GPU_IR_SHADER_OP_RETURN &&
        common_add->source_opcode == 128 && common_add->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
        common_add->operation == GPU_IR_SHADER_ARITH_ADD && common_add->result_id == 14 &&
        common_add->result_type_id == 2 && common_add->operand_count == 2 &&
        common_bit_count->source_opcode == 205 && common_bit_count->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
        common_bit_count->operation == GPU_IR_SHADER_ARITH_BIT_COUNT && common_bit_count->result_id == 15 &&
        common_bit_count->result_type_id == 2 && common_bit_count->operand_count == 1 &&
        common_bit_reverse->source_opcode == 204 && common_bit_reverse->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
        common_bit_reverse->operation == GPU_IR_SHADER_ARITH_BIT_REVERSE && common_bit_reverse->result_id == 16 &&
        common_bit_reverse->result_type_id == 2 && common_bit_reverse->operand_count == 1 &&
        common_bit_field_insert->source_opcode == 201 &&
        common_bit_field_insert->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT &&
        common_bit_field_insert->result_id == 17 && common_bit_field_insert->operand_count == 4 &&
        common_bit_field_signed_extract->source_opcode == 202 &&
        common_bit_field_signed_extract->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT &&
        common_bit_field_signed_extract->result_id == 18 && common_bit_field_signed_extract->operand_count == 3 &&
        common_bit_field_unsigned_extract->source_opcode == 203 &&
        common_bit_field_unsigned_extract->operation == GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT &&
        common_bit_field_unsigned_extract->result_id == 19 && common_bit_field_unsigned_extract->operand_count == 3;
    check("SPIR-V compute entry lowers to owned common types, parameters, normalized arithmetic and blocks",
          typed_shader_ok);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    const uint32_t builtin_shader[] = {
        0x0009000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 10, 0x6e69616du, 0, 8, 9, 12, 13,
        0x00060010u, 10, 17, 8, 4, 1,
        0x00040047u, 8, 11, 28,          // BuiltIn GlobalInvocationId
        0x00040047u, 9, 11, 29,          // BuiltIn LocalInvocationIndex
        0x00040047u, 12, 11, 36,         // BuiltIn SubgroupSize
        0x00040047u, 13, 11, 41,         // BuiltIn SubgroupLocalInvocationId
        0x00020013u, 1,                  // void
        0x00040015u, 2, 32, 0,           // uint32
        0x00040017u, 3, 2, 3,            // uint3
        0x00040020u, 4, 1, 3,            // Input pointer to uint3
        0x00040020u, 5, 1, 2,            // Input pointer to uint
        0x00030021u, 6, 1,               // void()
        0x0004003bu, 4, 8, 1,
        0x0004003bu, 5, 9, 1,
        0x0004003bu, 5, 12, 1,
        0x0004003bu, 5, 13, 1,
        0x00050036u, 1, 10, 0, 6,
        0x000200f8u, 11,
        0x000100fdu,
        0x00010038u
    };
    uint8_t builtin_bytes[(5 + sizeof builtin_shader / sizeof builtin_shader[0]) * 4];
    make_module(builtin_bytes, 0, 0x00010600u, 14, 0, builtin_shader,
                sizeof builtin_shader / sizeof builtin_shader[0]);
    rc = spirv_read(builtin_bytes, sizeof builtin_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *global_id = common_shader ? gpu_ir_shader_instruction_at(common_shader, 0) : NULL;
    const gpu_ir_shader_instruction *local_index = common_shader ? gpu_ir_shader_instruction_at(common_shader, 1) : NULL;
    const gpu_ir_shader_instruction *subgroup_size = common_shader ? gpu_ir_shader_instruction_at(common_shader, 2) : NULL;
    const gpu_ir_shader_instruction *subgroup_lane = common_shader ? gpu_ir_shader_instruction_at(common_shader, 3) : NULL;
    check("SPIR-V compute and subgroup built-ins normalize into API-neutral shader semantics",
          rc == SPIRV_OK && shader_rc == GPU_IR_OK && global_id && local_index && subgroup_size && subgroup_lane &&
          global_id->storage_class == GPU_IR_SHADER_STORAGE_INPUT && global_id->source_storage_class == 1 &&
          global_id->builtin == GPU_IR_SHADER_BUILTIN_GLOBAL_INVOCATION_ID && global_id->source_builtin == 28 &&
          local_index->storage_class == GPU_IR_SHADER_STORAGE_INPUT && local_index->source_storage_class == 1 &&
          local_index->builtin == GPU_IR_SHADER_BUILTIN_LOCAL_INVOCATION_INDEX && local_index->source_builtin == 29 &&
          subgroup_size->builtin == GPU_IR_SHADER_BUILTIN_SUBGROUP_SIZE && subgroup_size->source_builtin == 36 &&
          subgroup_lane->builtin == GPU_IR_SHADER_BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID &&
          subgroup_lane->source_builtin == 41);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    const uint32_t subgroup_vote_shader[] = {
        0x00020011u, 1,                    // OpCapability Shader
        0x00020011u, 61,                   // OpCapability GroupNonUniform
        0x00020011u, 62,                   // OpCapability GroupNonUniformVote
        0x0003000eu, 0, 1,                // OpMemoryModel Logical GLSL450
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 10, 0x6e69616du, 0,
        0x00060010u, 10, 17, 1, 1, 1,     // OpExecutionMode LocalSize
        0x00020013u, 1,                    // OpTypeVoid
        0x00020014u, 2,                    // OpTypeBool
        0x00040015u, 3, 32, 0,            // OpTypeInt uint32
        0x0004002bu, 3, 4, 3,             // OpConstant Subgroup scope
        0x00030029u, 2, 5,                // OpConstantTrue
        0x0003002au, 2, 6,                // OpConstantFalse
        0x00030021u, 7, 1,                // OpTypeFunction void()
        0x00050036u, 1, 10, 0, 7,         // OpFunction
        0x000200f8u, 11,                  // OpLabel
        0x0005014fu, 2, 12, 4, 5,         // OpGroupNonUniformAny
        0x0005014eu, 2, 13, 4, 6,         // OpGroupNonUniformAll
        0x000100fdu,                      // OpReturn
        0x00010038u                       // OpFunctionEnd
    };
    uint8_t subgroup_vote_bytes[(5 + sizeof subgroup_vote_shader / sizeof subgroup_vote_shader[0]) * 4];
    make_module(subgroup_vote_bytes, 0, 0x00010600u, 14, 0, subgroup_vote_shader,
                sizeof subgroup_vote_shader / sizeof subgroup_vote_shader[0]);
    rc = spirv_read(subgroup_vote_bytes, sizeof subgroup_vote_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *subgroup_any = NULL, *subgroup_all = NULL;
    for (uint32_t i = 0; common_shader && i < gpu_ir_shader_instruction_count(common_shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(common_shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC) continue;
        if (instruction->result_id == 12) subgroup_any = instruction;
        if (instruction->result_id == 13) subgroup_all = instruction;
    }
    char *subgroup_vote_msl = NULL;
    size_t subgroup_vote_msl_size = 0;
    int subgroup_vote_msl_rc = common_shader ? gpu_ir_msl_emit_compute(common_shader,
        &subgroup_vote_msl, &subgroup_vote_msl_size) : GPU_IR_INVALID;
    check("SPIR-V subgroup All/Any lower through shared IR to active-lane MSL votes",
          rc == SPIRV_OK && shader_rc == GPU_IR_OK && subgroup_any && subgroup_all &&
          subgroup_any->source_opcode == 335 &&
          subgroup_any->operation == GPU_IR_SHADER_ARITH_SUBGROUP_ANY && subgroup_any->operand_count == 1 &&
          subgroup_all->source_opcode == 334 &&
          subgroup_all->operation == GPU_IR_SHADER_ARITH_SUBGROUP_ALL && subgroup_all->operand_count == 1 &&
          subgroup_vote_msl_rc == GPU_IR_OK && subgroup_vote_msl && subgroup_vote_msl_size &&
          strstr(subgroup_vote_msl, "simd_any(true)") && strstr(subgroup_vote_msl, "simd_all(false)"));
    free(subgroup_vote_msl);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    uint32_t wrong_scope_shader[sizeof subgroup_vote_shader / sizeof subgroup_vote_shader[0]];
    memcpy(wrong_scope_shader, subgroup_vote_shader, sizeof wrong_scope_shader);
    wrong_scope_shader[31] = 2; // OpConstant id 4: Workgroup scope is invalid for these vote operations.
    uint8_t wrong_scope_bytes[(5 + sizeof wrong_scope_shader / sizeof wrong_scope_shader[0]) * 4];
    make_module(wrong_scope_bytes, 0, 0x00010600u, 14, 0, wrong_scope_shader,
                sizeof wrong_scope_shader / sizeof wrong_scope_shader[0]);
    rc = spirv_read(wrong_scope_bytes, sizeof wrong_scope_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("SPIR-V subgroup votes reject non-Subgroup execution scopes",
          rc == SPIRV_OK && shader_rc == GPU_IR_INVALID && !common_shader);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    uint32_t missing_vote_capability_shader[sizeof subgroup_vote_shader / sizeof subgroup_vote_shader[0]];
    size_t missing_vote_capability_words = 0;
    for (size_t source_word = 0; source_word < sizeof subgroup_vote_shader / sizeof subgroup_vote_shader[0];) {
        uint32_t header = subgroup_vote_shader[source_word];
        uint32_t word_count = header >> 16, opcode = header & 0xffffu;
        if (opcode == 17 && word_count == 2 &&
            (subgroup_vote_shader[source_word + 1] == 61 || subgroup_vote_shader[source_word + 1] == 62)) {
            source_word += word_count;
            continue;
        }
        memcpy(missing_vote_capability_shader + missing_vote_capability_words,
            subgroup_vote_shader + source_word, (size_t)word_count * sizeof(uint32_t));
        missing_vote_capability_words += word_count;
        source_word += word_count;
    }
    uint8_t missing_vote_capability_bytes[(5 + sizeof missing_vote_capability_shader / sizeof missing_vote_capability_shader[0]) * 4];
    make_module(missing_vote_capability_bytes, 0, 0x00010600u, 14, 0,
        missing_vote_capability_shader, missing_vote_capability_words);
    rc = spirv_read(missing_vote_capability_bytes, (5 + missing_vote_capability_words) * 4, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("SPIR-V subgroup votes require GroupNonUniform and GroupNonUniformVote capabilities",
          rc == SPIRV_OK && shader_rc == GPU_IR_INVALID && !common_shader);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    const uint32_t subgroup_minmax_shader[] = {
        0x00020011u, 1,                     // OpCapability Shader
        0x00020011u, 61,                    // OpCapability GroupNonUniform
        0x00020011u, 63,                    // OpCapability GroupNonUniformArithmetic
        0x0003000eu, 0, 1,                 // OpMemoryModel Logical GLSL450
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 12, 0x6e69616du, 0,
        0x00060010u, 12, 17, 1, 1, 1,      // OpExecutionMode LocalSize
        0x00020013u, 1,                     // OpTypeVoid
        0x00040015u, 2, 32, 1,             // OpTypeInt int32
        0x00040015u, 3, 32, 0,             // OpTypeInt uint32
        0x00030021u, 9, 1,                 // OpTypeFunction void()
        0x0004002bu, 3, 5, 3,              // OpConstant Subgroup scope
        0x0004002bu, 3, 6, 0,              // OpConstant Reduce group operation
        0x0004002bu, 2, 7, 0xfffffffeu,    // OpConstant signed input -2
        0x0004002bu, 3, 8, 7,              // OpConstant unsigned input 7
        0x00050036u, 1, 12, 0, 9,          // OpFunction
        0x000200f8u, 13,                   // OpLabel
        0x00060160u, 2, 14, 5, 6, 7,       // OpGroupNonUniformSMin
        0x00060161u, 3, 15, 5, 6, 8,       // OpGroupNonUniformUMin
        0x00060163u, 2, 16, 5, 6, 7,       // OpGroupNonUniformSMax
        0x00060164u, 3, 17, 5, 6, 8,       // OpGroupNonUniformUMax
        0x0006015fu, 2, 18, 5, 6, 7,       // OpGroupNonUniformIMul
        0x00060166u, 3, 19, 5, 6, 8,       // OpGroupNonUniformBitwiseAnd
        0x00060167u, 3, 20, 5, 6, 8,       // OpGroupNonUniformBitwiseOr
        0x00060168u, 3, 21, 5, 6, 8,       // OpGroupNonUniformBitwiseXor
        0x000100fdu,                       // OpReturn
        0x00010038u                        // OpFunctionEnd
    };
    uint8_t subgroup_minmax_bytes[(5 + sizeof subgroup_minmax_shader / sizeof subgroup_minmax_shader[0]) * 4];
    make_module(subgroup_minmax_bytes, 0, 0x00010600u, 22, 0, subgroup_minmax_shader,
                sizeof subgroup_minmax_shader / sizeof subgroup_minmax_shader[0]);
    rc = spirv_read(subgroup_minmax_bytes, sizeof subgroup_minmax_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *subgroup_smin = NULL, *subgroup_umin = NULL;
    const gpu_ir_shader_instruction *subgroup_smax = NULL, *subgroup_umax = NULL;
    const gpu_ir_shader_instruction *subgroup_imul = NULL, *subgroup_band = NULL;
    const gpu_ir_shader_instruction *subgroup_bor = NULL, *subgroup_bxor = NULL;
    for (uint32_t i = 0; common_shader && i < gpu_ir_shader_instruction_count(common_shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(common_shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC) continue;
        if (instruction->result_id == 14) subgroup_smin = instruction;
        if (instruction->result_id == 15) subgroup_umin = instruction;
        if (instruction->result_id == 16) subgroup_smax = instruction;
        if (instruction->result_id == 17) subgroup_umax = instruction;
        if (instruction->result_id == 18) subgroup_imul = instruction;
        if (instruction->result_id == 19) subgroup_band = instruction;
        if (instruction->result_id == 20) subgroup_bor = instruction;
        if (instruction->result_id == 21) subgroup_bxor = instruction;
    }
    char *subgroup_minmax_msl = NULL;
    size_t subgroup_minmax_msl_size = 0;
    int subgroup_minmax_msl_rc = common_shader ? gpu_ir_msl_emit_compute(common_shader,
        &subgroup_minmax_msl, &subgroup_minmax_msl_size) : GPU_IR_INVALID;
    check("SPIR-V integer subgroup multiply/min/max/bitwise use Subgroup Reduce and common MSL reductions",
          rc == SPIRV_OK && shader_rc == GPU_IR_OK && subgroup_smin && subgroup_umin &&
          subgroup_smax && subgroup_umax && subgroup_imul && subgroup_band && subgroup_bor && subgroup_bxor &&
          subgroup_smin->source_opcode == 352 &&
          subgroup_smin->operation == GPU_IR_SHADER_ARITH_SUBGROUP_SMIN && subgroup_smin->operand_count == 1 &&
          subgroup_umin->source_opcode == 353 &&
          subgroup_umin->operation == GPU_IR_SHADER_ARITH_SUBGROUP_UMIN && subgroup_umin->operand_count == 1 &&
          subgroup_smax->source_opcode == 355 &&
          subgroup_smax->operation == GPU_IR_SHADER_ARITH_SUBGROUP_SMAX && subgroup_smax->operand_count == 1 &&
          subgroup_umax->source_opcode == 356 &&
          subgroup_umax->operation == GPU_IR_SHADER_ARITH_SUBGROUP_UMAX && subgroup_umax->operand_count == 1 &&
          subgroup_imul->source_opcode == 351 &&
          subgroup_imul->operation == GPU_IR_SHADER_ARITH_SUBGROUP_IMUL && subgroup_imul->operand_count == 1 &&
          subgroup_band->source_opcode == 358 &&
          subgroup_band->operation == GPU_IR_SHADER_ARITH_SUBGROUP_BIT_AND && subgroup_band->operand_count == 1 &&
          subgroup_bor->source_opcode == 359 &&
          subgroup_bor->operation == GPU_IR_SHADER_ARITH_SUBGROUP_BIT_OR && subgroup_bor->operand_count == 1 &&
          subgroup_bxor->source_opcode == 360 &&
          subgroup_bxor->operation == GPU_IR_SHADER_ARITH_SUBGROUP_BIT_XOR && subgroup_bxor->operand_count == 1 &&
          subgroup_minmax_msl_rc == GPU_IR_OK && subgroup_minmax_msl && subgroup_minmax_msl_size &&
          strstr(subgroup_minmax_msl, "simd_min(") && strstr(subgroup_minmax_msl, "simd_max(") &&
          strstr(subgroup_minmax_msl, "simd_product(") && strstr(subgroup_minmax_msl, "simd_and(") &&
          strstr(subgroup_minmax_msl, "simd_or(") && strstr(subgroup_minmax_msl, "simd_xor("));
    free(subgroup_minmax_msl);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    uint32_t wrong_minmax_scope[sizeof subgroup_minmax_shader / sizeof subgroup_minmax_shader[0]];
    memcpy(wrong_minmax_scope, subgroup_minmax_shader, sizeof wrong_minmax_scope);
    wrong_minmax_scope[36] = 2; // Workgroup scope is invalid for OpGroupNonUniform reductions.
    uint8_t wrong_minmax_scope_bytes[(5 + sizeof wrong_minmax_scope / sizeof wrong_minmax_scope[0]) * 4];
    make_module(wrong_minmax_scope_bytes, 0, 0x00010600u, 18, 0, wrong_minmax_scope,
                sizeof wrong_minmax_scope / sizeof wrong_minmax_scope[0]);
    rc = spirv_read(wrong_minmax_scope_bytes, sizeof wrong_minmax_scope_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("SPIR-V integer subgroup reductions reject non-Subgroup execution scopes",
          rc == SPIRV_OK && shader_rc == GPU_IR_INVALID && !common_shader);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    uint32_t missing_minmax_capability[sizeof subgroup_minmax_shader / sizeof subgroup_minmax_shader[0]];
    memcpy(missing_minmax_capability, subgroup_minmax_shader, sizeof missing_minmax_capability);
    missing_minmax_capability[5] = 62; // Vote capability does not enable arithmetic subgroup operations.
    uint8_t missing_minmax_capability_bytes[(5 + sizeof missing_minmax_capability / sizeof missing_minmax_capability[0]) * 4];
    make_module(missing_minmax_capability_bytes, 0, 0x00010600u, 18, 0, missing_minmax_capability,
                sizeof missing_minmax_capability / sizeof missing_minmax_capability[0]);
    rc = spirv_read(missing_minmax_capability_bytes, sizeof missing_minmax_capability_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("SPIR-V integer subgroup reductions require GroupNonUniformArithmetic",
          rc == SPIRV_OK && shader_rc == GPU_IR_INVALID && !common_shader);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    const uint32_t vertex_interface_shader[] = {
        0x0008000fu, SPIRV_EXECUTION_MODEL_VERTEX, 12, 0x6e69616du, 0, 9, 10, 11,
        0x00040047u, 9, 11, 42,         // BuiltIn VertexIndex
        0x00040047u, 10, 11, 43,        // BuiltIn InstanceIndex
        0x00040047u, 11, 11, 0,         // BuiltIn Position
        0x00020013u, 1,                 // void
        0x00040015u, 2, 32, 0,          // uint32
        0x00030016u, 3, 32,             // float32
        0x00040017u, 4, 3, 4,           // float4
        0x00040020u, 5, 1, 2,           // Input pointer to uint
        0x00040020u, 6, 3, 4,           // Output pointer to float4
        0x00030021u, 8, 1,              // void()
        0x0004003bu, 5, 9, 1,
        0x0004003bu, 5, 10, 1,
        0x0004003bu, 6, 11, 3,
        0x0004003bu, 5, 14, 1,         // unrelated, unlisted entry-point input
        0x00050036u, 1, 12, 0, 8,
        0x000200f8u, 13,
        0x000100fdu,
        0x00010038u
    };
    uint8_t vertex_interface_bytes[(5 + sizeof vertex_interface_shader / sizeof vertex_interface_shader[0]) * 4];
    make_module(vertex_interface_bytes, 0, 0x00010600u, 15, 0, vertex_interface_shader,
                sizeof vertex_interface_shader / sizeof vertex_interface_shader[0]);
    rc = spirv_read(vertex_interface_bytes, sizeof vertex_interface_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_VERTEX) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *vertex_index = common_shader ? gpu_ir_shader_instruction_at(common_shader, 0) : NULL;
    const gpu_ir_shader_instruction *instance_index = common_shader ? gpu_ir_shader_instruction_at(common_shader, 1) : NULL;
    const gpu_ir_shader_instruction *position = common_shader ? gpu_ir_shader_instruction_at(common_shader, 2) : NULL;
    int unrelated_interface_excluded = 1;
    for (uint32_t i = 0; common_shader && i < gpu_ir_shader_instruction_count(common_shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(common_shader, i);
        if (instruction && instruction->result_id == 14) unrelated_interface_excluded = 0;
    }
    check("SPIR-V vertex entry retains VertexIndex, InstanceIndex and Position semantics in common IR",
          rc == SPIRV_OK && shader_rc == GPU_IR_OK && common_shader &&
          gpu_ir_shader_stage(common_shader) == GPU_IR_STAGE_VERTEX && vertex_index && instance_index && position &&
          unrelated_interface_excluded &&
          vertex_index->result_id == 9 && vertex_index->storage_class == GPU_IR_SHADER_STORAGE_INPUT &&
          vertex_index->entry_point_interface &&
          vertex_index->builtin == GPU_IR_SHADER_BUILTIN_VERTEX_INDEX && vertex_index->source_builtin == 42 &&
          instance_index->result_id == 10 && instance_index->storage_class == GPU_IR_SHADER_STORAGE_INPUT &&
          instance_index->entry_point_interface &&
          instance_index->builtin == GPU_IR_SHADER_BUILTIN_INSTANCE_INDEX && instance_index->source_builtin == 43 &&
          position->result_id == 11 && position->storage_class == GPU_IR_SHADER_STORAGE_OUTPUT &&
          position->entry_point_interface &&
          position->builtin == GPU_IR_SHADER_BUILTIN_POSITION && position->source_builtin == 0);
    char *vertex_msl = NULL;
    size_t vertex_msl_size = 0;
    int vertex_msl_rc = common_shader ? gpu_ir_msl_emit_vertex(common_shader, &vertex_msl,
        &vertex_msl_size) : GPU_IR_INVALID;
    check("Vulkan InstanceIndex maps to Metal instance_id without a D3D base-instance adjustment",
          vertex_msl_rc == GPU_IR_OK && vertex_msl && vertex_msl_size &&
          strstr(vertex_msl, "uint vx_builtin_10 [[instance_id]]") &&
          !strstr(vertex_msl, "[[base_instance]]"));
    free(vertex_msl);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    const uint32_t fragment_interface_shader[] = {
        0x0009000fu, SPIRV_EXECUTION_MODEL_FRAGMENT, 14, 0x6e69616du, 0, 10, 11, 12, 13,
        0x00040047u, 10, 11, 15,        // BuiltIn FragCoord
        0x00040047u, 11, 11, 17,        // BuiltIn FrontFacing
        0x00040047u, 12, 30, 0,         // Location 0
        0x00040047u, 13, 11, 22,        // BuiltIn FragDepth
        0x00020013u, 1,                 // void
        0x00020014u, 2,                 // bool
        0x00030016u, 3, 32,             // float32
        0x00040017u, 4, 3, 4,           // float4
        0x00040020u, 5, 1, 4,           // Input pointer to float4
        0x00040020u, 6, 1, 2,           // Input pointer to bool
        0x00040020u, 7, 3, 4,           // Output pointer to float4
        0x00030021u, 8, 1,              // void()
        0x00040020u, 9, 3, 3,           // Output pointer to float
        0x0004003bu, 5, 10, 1,
        0x0004003bu, 6, 11, 1,
        0x0004003bu, 7, 12, 3,
        0x0004003bu, 9, 13, 3,
        0x00050036u, 1, 14, 0, 8,
        0x000200f8u, 15,
        0x000100fdu,
        0x00010038u
    };
    uint8_t fragment_interface_bytes[(5 + sizeof fragment_interface_shader / sizeof fragment_interface_shader[0]) * 4];
    make_module(fragment_interface_bytes, 0, 0x00010600u, 16, 0, fragment_interface_shader,
                sizeof fragment_interface_shader / sizeof fragment_interface_shader[0]);
    rc = spirv_read(fragment_interface_bytes, sizeof fragment_interface_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_FRAGMENT) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *frag_coord = common_shader ? gpu_ir_shader_instruction_at(common_shader, 0) : NULL;
    const gpu_ir_shader_instruction *front_facing = common_shader ? gpu_ir_shader_instruction_at(common_shader, 1) : NULL;
    const gpu_ir_shader_instruction *color_output = common_shader ? gpu_ir_shader_instruction_at(common_shader, 2) : NULL;
    const gpu_ir_shader_instruction *frag_depth = common_shader ? gpu_ir_shader_instruction_at(common_shader, 3) : NULL;
    check("SPIR-V fragment entry retains built-ins and location-decorated outputs in common IR",
          rc == SPIRV_OK && shader_rc == GPU_IR_OK && common_shader &&
          gpu_ir_shader_stage(common_shader) == GPU_IR_STAGE_FRAGMENT && frag_coord && front_facing && color_output && frag_depth &&
          frag_coord->storage_class == GPU_IR_SHADER_STORAGE_INPUT &&
          frag_coord->entry_point_interface &&
          frag_coord->builtin == GPU_IR_SHADER_BUILTIN_FRAG_COORD && frag_coord->source_builtin == 15 &&
          front_facing->storage_class == GPU_IR_SHADER_STORAGE_INPUT &&
          front_facing->entry_point_interface &&
          front_facing->builtin == GPU_IR_SHADER_BUILTIN_FRONT_FACING && front_facing->source_builtin == 17 &&
          color_output->storage_class == GPU_IR_SHADER_STORAGE_OUTPUT && color_output->has_location &&
          color_output->entry_point_interface &&
          color_output->location == 0 && !color_output->has_builtin &&
          frag_depth->storage_class == GPU_IR_SHADER_STORAGE_OUTPUT && frag_depth->has_builtin &&
          frag_depth->entry_point_interface &&
          frag_depth->builtin == GPU_IR_SHADER_BUILTIN_FRAG_DEPTH && frag_depth->source_builtin == 22);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    const uint32_t barrier_shader[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 1, 0x6e69616du, 0,
        0x00060010u, 1, 17, 1, 1, 1,
        0x00020013u, 2,                 // void
        0x00040015u, 3, 32, 0,          // uint32
        0x00030021u, 4, 2,              // void()
        0x0004002bu, 3, 5, 2,           // Scope Workgroup
        0x0004002bu, 3, 6, 2,           // Scope Workgroup
        0x0004002bu, 3, 7, 0x108,       // AcquireRelease | WorkgroupMemory
        0x00050036u, 2, 1, 0, 4,
        0x000200f8u, 8,
        0x000400e0u, 5, 6, 7,           // OpControlBarrier
        0x000300e1u, 6, 7,              // OpMemoryBarrier
        0x000100fdu,
        0x00010038u
    };
    uint8_t barrier_bytes[(5 + sizeof barrier_shader / sizeof barrier_shader[0]) * 4];
    make_module(barrier_bytes, 0, 0x00010600u, 9, 0, barrier_shader,
                sizeof barrier_shader / sizeof barrier_shader[0]);
    rc = spirv_read(barrier_bytes, sizeof barrier_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *control_barrier = common_shader ? gpu_ir_shader_instruction_at(common_shader, 3) : NULL;
    const gpu_ir_shader_instruction *memory_barrier = common_shader ? gpu_ir_shader_instruction_at(common_shader, 4) : NULL;
    const gpu_ir_shader_operand *control_scope = control_barrier ?
        gpu_ir_shader_operand_at(common_shader, control_barrier->first_operand) : NULL;
    const gpu_ir_shader_operand *control_memory_scope = control_barrier ?
        gpu_ir_shader_operand_at(common_shader, control_barrier->first_operand + 1) : NULL;
    const gpu_ir_shader_operand *control_semantics = control_barrier ?
        gpu_ir_shader_operand_at(common_shader, control_barrier->first_operand + 2) : NULL;
    const gpu_ir_shader_operand *memory_scope = memory_barrier ?
        gpu_ir_shader_operand_at(common_shader, memory_barrier->first_operand) : NULL;
    const gpu_ir_shader_operand *memory_semantics = memory_barrier ?
        gpu_ir_shader_operand_at(common_shader, memory_barrier->first_operand + 1) : NULL;
    check("SPIR-V control and memory barriers retain their distinct scopes and semantics IDs",
          rc == SPIRV_OK && shader_rc == GPU_IR_OK && common_shader && control_barrier && memory_barrier &&
          control_barrier->opcode == GPU_IR_SHADER_OP_BARRIER &&
          control_barrier->operation == GPU_IR_SHADER_BARRIER_CONTROL && control_barrier->operand_count == 3 &&
          control_scope && control_scope->kind == GPU_IR_SHADER_OPERAND_ID && control_scope->value == 5 &&
          control_memory_scope && control_memory_scope->value == 6 && control_semantics && control_semantics->value == 7 &&
          memory_barrier->opcode == GPU_IR_SHADER_OP_BARRIER &&
          memory_barrier->operation == GPU_IR_SHADER_BARRIER_MEMORY && memory_barrier->operand_count == 2 &&
          memory_scope && memory_scope->value == 6 && memory_semantics && memory_semantics->value == 7);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    const uint32_t memory_access_shader[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 10, 0x6e69616du, 0,
        0x00060010u, 10, 17, 1, 1, 1,
        0x00020013u, 1,                 // void
        0x00040015u, 2, 32, 0,          // uint32
        0x00040020u, 3, 7, 2,           // Function pointer to uint32
        0x00030021u, 4, 1,              // void()
        0x0004002bu, 2, 5, 2,           // uint32 Workgroup scope constant
        0x00050036u, 1, 10, 0, 4,
        0x000200f8u, 11,
        0x0004003bu, 3, 12, 7,          // Function local uint32
        0x0005003eu, 12, 5, 7, 4,       // OpStore: Volatile | Aligned | Nontemporal, align 4
        0x0006003du, 2, 13, 12, 3, 8,   // OpLoad: Volatile | Aligned, align 8
        0x000100fdu,
        0x00010038u
    };
    uint8_t memory_access_bytes[(5 + sizeof memory_access_shader / sizeof memory_access_shader[0]) * 4];
    make_module(memory_access_bytes, 0, 0x00010600u, 14, 0, memory_access_shader,
                sizeof memory_access_shader / sizeof memory_access_shader[0]);
    rc = spirv_read(memory_access_bytes, sizeof memory_access_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *common_store = NULL, *common_load = NULL;
    for (uint32_t i = 0; common_shader && i < gpu_ir_shader_instruction_count(common_shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(common_shader, i);
        if (instruction->opcode == GPU_IR_SHADER_OP_STORE) common_store = instruction;
        if (instruction->opcode == GPU_IR_SHADER_OP_LOAD) common_load = instruction;
    }
    check("SPIR-V load/store preserve volatility, explicit alignment and non-temporal metadata",
          rc == SPIRV_OK && shader_rc == GPU_IR_OK && common_store && common_load &&
          common_store->memory_access_flags == (GPU_IR_SHADER_MEMORY_VOLATILE |
              GPU_IR_SHADER_MEMORY_ALIGNED | GPU_IR_SHADER_MEMORY_NONTEMPORAL) &&
          common_store->memory_alignment == 4 &&
          common_load->memory_access_flags == (GPU_IR_SHADER_MEMORY_VOLATILE | GPU_IR_SHADER_MEMORY_ALIGNED) &&
          common_load->memory_alignment == 8);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    uint32_t unsupported_memory_access_shader[sizeof memory_access_shader / sizeof memory_access_shader[0]];
    memcpy(unsupported_memory_access_shader, memory_access_shader, sizeof memory_access_shader);
    unsupported_memory_access_shader[42] = 0x28; // MakePointerAvailable | NonPrivatePointer.
    unsupported_memory_access_shader[43] = 5; // Workgroup scope constant.
    make_module(memory_access_bytes, 0, 0x00010600u, 14, 0, unsupported_memory_access_shader,
                sizeof unsupported_memory_access_shader / sizeof unsupported_memory_access_shader[0]);
    rc = spirv_read(memory_access_bytes, sizeof memory_access_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("SPIR-V load/store reject pointer availability until its synchronization semantics are mapped",
          rc == SPIRV_OK && shader_rc == GPU_IR_UNSUPPORTED && !common_shader);
    spirv_free(&module);

    memcpy(unsupported_memory_access_shader, memory_access_shader, sizeof memory_access_shader);
    unsupported_memory_access_shader[42] = 0x40; // Reserved Memory Access bit.
    make_module(memory_access_bytes, 0, 0x00010600u, 14, 0, unsupported_memory_access_shader,
                sizeof unsupported_memory_access_shader / sizeof unsupported_memory_access_shader[0]);
    rc = spirv_read(memory_access_bytes, sizeof memory_access_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("SPIR-V rejects reserved Memory Access mask bits", rc == SPIRV_OK &&
          shader_rc == GPU_IR_INVALID && !common_shader);
    spirv_free(&module);

    memcpy(unsupported_memory_access_shader, memory_access_shader, sizeof memory_access_shader);
    unsupported_memory_access_shader[48] = 0x28; // MakePointerAvailable | NonPrivatePointer on a load.
    unsupported_memory_access_shader[49] = 5; // Workgroup scope constant.
    make_module(memory_access_bytes, 0, 0x00010600u, 14, 0, unsupported_memory_access_shader,
                sizeof unsupported_memory_access_shader / sizeof unsupported_memory_access_shader[0]);
    rc = spirv_read(memory_access_bytes, sizeof memory_access_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("SPIR-V rejects pointer-availability access on a load", rc == SPIRV_OK &&
          shader_rc == GPU_IR_INVALID && !common_shader);
    spirv_free(&module);

    memcpy(unsupported_memory_access_shader, memory_access_shader, sizeof memory_access_shader);
    unsupported_memory_access_shader[43] = 3; // Aligned requires a nonzero power-of-two literal.
    make_module(memory_access_bytes, 0, 0x00010600u, 14, 0, unsupported_memory_access_shader,
                sizeof unsupported_memory_access_shader / sizeof unsupported_memory_access_shader[0]);
    rc = spirv_read(memory_access_bytes, sizeof memory_access_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("SPIR-V rejects non-power-of-two explicit memory alignment", rc == SPIRV_OK &&
          shader_rc == GPU_IR_INVALID && !common_shader);
    spirv_free(&module);

    const uint32_t composite_shader[] = {
        0x00020011u, 1,                  // OpCapability Shader
        0x0003000eu, 0, 1,               // OpMemoryModel Logical GLSL450
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 20, 0x6e69616du, 0,
        0x00060010u, 20, 17, 1, 1, 1,
        0x00020013u, 1,                  // void
        0x00030016u, 2, 32,              // float32
        0x00040017u, 3, 2, 2,            // vec2<float>
        0x00040018u, 4, 3, 2,            // mat2<float>
        0x00040015u, 5, 32, 0,           // uint32
        0x00030021u, 6, 1,               // void()
        0x0004002bu, 2, 10, 0x3f800000u, // 1.0
        0x0004002bu, 2, 11, 0x40000000u, // 2.0
        0x0004002bu, 5, 12, 0,
        0x0004002bu, 5, 13, 1,
        0x00030001u, 2, 17,              // module-scope OpUndef
        0x0005002cu, 3, 14, 10, 11,      // vec2(1, 2)
        0x0005002cu, 3, 15, 11, 10,      // vec2(2, 1)
        0x0005002cu, 4, 16, 14, 15,      // mat2(vec2(1, 2), vec2(2, 1))
        0x0005002cu, 3, 18, 17, 10,      // vec2(undefined, 1)
        0x00050036u, 1, 20, 0, 6,
        0x000200f8u, 21,
        0x00050051u, 2, 22, 14, 1,       // OpCompositeExtract
        0x0007004fu, 3, 23, 14, 15, 2, 0,// OpVectorShuffle
        0x00060052u, 3, 24, 11, 14, 0,   // OpCompositeInsert
        0x0005004du, 2, 25, 14, 13,      // OpVectorExtractDynamic
        0x0006004eu, 3, 26, 14, 10, 13,  // OpVectorInsertDynamic
        0x00050050u, 3, 27, 10, 11,      // OpCompositeConstruct
        0x00040054u, 4, 28, 16,          // OpTranspose
        0x000100fdu,
        0x00010038u
    };
    uint8_t composite_shader_bytes[(5 + sizeof composite_shader / sizeof composite_shader[0]) * 4];
    make_module(composite_shader_bytes, 0, 0x00010600u, 29, 0, composite_shader,
                sizeof composite_shader / sizeof composite_shader[0]);
    rc = spirv_read(composite_shader_bytes, sizeof composite_shader_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *extract = NULL, *shuffle = NULL, *insert = NULL;
    const gpu_ir_shader_instruction *dynamic_extract = NULL, *dynamic_insert = NULL;
    const gpu_ir_shader_instruction *construct = NULL, *transpose = NULL, *composite_constant = NULL;
    for (uint32_t i = 0; common_shader && i < gpu_ir_shader_instruction_count(common_shader); i++) {
        const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(common_shader, i);
        if (candidate->source_opcode == 44 && candidate->result_id == 14) composite_constant = candidate;
        if (candidate->source_opcode == 81) extract = candidate;
        if (candidate->source_opcode == 79) shuffle = candidate;
        if (candidate->source_opcode == 82) insert = candidate;
        if (candidate->source_opcode == 77) dynamic_extract = candidate;
        if (candidate->source_opcode == 78) dynamic_insert = candidate;
        if (candidate->source_opcode == 80) construct = candidate;
        if (candidate->source_opcode == 84) transpose = candidate;
    }
    const gpu_ir_shader_instruction *global_undef = NULL, *undef_composite = NULL;
    for (uint32_t i = 0; common_shader && i < gpu_ir_shader_instruction_count(common_shader); i++) {
        const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(common_shader, i);
        if (candidate->source_opcode == 1 && candidate->result_id == 17) global_undef = candidate;
        if (candidate->source_opcode == 44 && candidate->result_id == 18) undef_composite = candidate;
    }
    int composite_shader_ok = shader_rc == GPU_IR_OK && common_shader && composite_constant && global_undef &&
        undef_composite && extract &&
        shuffle && insert && dynamic_extract && dynamic_insert && construct && transpose &&
        composite_constant->opcode == GPU_IR_SHADER_OP_CONSTANT &&
        composite_constant->operation == GPU_IR_SHADER_CONSTANT_COMPOSITE &&
        composite_constant->operand_count == 2 &&
        gpu_ir_shader_operand_at(common_shader, composite_constant->first_operand)->value == 10 &&
        gpu_ir_shader_operand_at(common_shader, composite_constant->first_operand + 1)->value == 11 &&
        extract->operation == GPU_IR_SHADER_COMPOSITE_EXTRACT && extract->literal_word_count == 1 &&
        gpu_ir_shader_literal_word_at(common_shader, extract->first_literal) == 1 &&
        shuffle->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_SHUFFLE && shuffle->literal_word_count == 2 &&
        gpu_ir_shader_literal_word_at(common_shader, shuffle->first_literal) == 2 &&
        gpu_ir_shader_literal_word_at(common_shader, shuffle->first_literal + 1) == 0 &&
        insert->operation == GPU_IR_SHADER_COMPOSITE_INSERT && insert->literal_word_count == 1 &&
        dynamic_extract->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC &&
        dynamic_insert->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_INSERT_DYNAMIC &&
        construct->operation == GPU_IR_SHADER_COMPOSITE_CONSTRUCT && construct->operand_count == 2 &&
        transpose->operation == GPU_IR_SHADER_COMPOSITE_TRANSPOSE &&
        global_undef->opcode == GPU_IR_SHADER_OP_UNDEFINED && undef_composite->operand_count == 2 &&
        gpu_ir_shader_operand_at(common_shader, undef_composite->first_operand)->value == 17;
    check("SPIR-V composite constants, vector operations, literal indexes and transpose lower to common IR",
          composite_shader_ok);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    uint32_t invalid_shuffle[sizeof composite_shader / sizeof composite_shader[0]];
    memcpy(invalid_shuffle, composite_shader, sizeof composite_shader);
    for (size_t i = 0; i < sizeof invalid_shuffle / sizeof invalid_shuffle[0];) {
        uint32_t word_count = invalid_shuffle[i] >> 16;
        if ((invalid_shuffle[i] & 0xffffu) == 79) { invalid_shuffle[i + 5] = 99; break; }
        if (!word_count || word_count > sizeof invalid_shuffle / sizeof invalid_shuffle[0] - i) break;
        i += word_count;
    }
    make_module(composite_shader_bytes, 0, 0x00010600u, 29, 0, invalid_shuffle,
                sizeof invalid_shuffle / sizeof invalid_shuffle[0]);
    rc = spirv_read(composite_shader_bytes, sizeof composite_shader_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("SPIR-V vector shuffle rejects selectors outside both source vectors",
          rc == SPIRV_OK && shader_rc == GPU_IR_INVALID && common_shader == NULL);
    spirv_free(&module);

    uint32_t invalid_constant_dependency[sizeof composite_shader / sizeof composite_shader[0]];
    memcpy(invalid_constant_dependency, composite_shader, sizeof composite_shader);
    for (size_t i = 0; i < sizeof invalid_constant_dependency / sizeof invalid_constant_dependency[0];) {
        uint32_t word_count = invalid_constant_dependency[i] >> 16;
        if ((invalid_constant_dependency[i] & 0xffffu) == 43 &&
            word_count == 4 && invalid_constant_dependency[i + 2] == 10) {
            invalid_constant_dependency[i] = (invalid_constant_dependency[i] & 0xffff0000u) | 50u;
            break;
        }
        if (!word_count || word_count > sizeof invalid_constant_dependency / sizeof invalid_constant_dependency[0] - i) break;
        i += word_count;
    }
    make_module(composite_shader_bytes, 0, 0x00010600u, 29, 0, invalid_constant_dependency,
                sizeof invalid_constant_dependency / sizeof invalid_constant_dependency[0]);
    rc = spirv_read(composite_shader_bytes, sizeof composite_shader_bytes, &module);
    check("OpConstantComposite rejects specialization-constant constituents",
          rc == SPIRV_INVALID);
    spirv_free(&module);

    uint32_t specialization_composite[sizeof composite_shader / sizeof composite_shader[0]];
    memcpy(specialization_composite, composite_shader, sizeof composite_shader);
    for (size_t i = 0; i < sizeof specialization_composite / sizeof specialization_composite[0];) {
        uint32_t word_count = specialization_composite[i] >> 16;
        uint32_t opcode = specialization_composite[i] & 0xffffu;
        if (word_count >= 3 && opcode == 43 && specialization_composite[i + 2] == 10)
            specialization_composite[i] = (specialization_composite[i] & 0xffff0000u) | 50u;
        if (word_count >= 3 && opcode == 44 &&
            (specialization_composite[i + 2] == 14 || specialization_composite[i + 2] == 15 ||
             specialization_composite[i + 2] == 16 || specialization_composite[i + 2] == 18))
            specialization_composite[i] = (specialization_composite[i] & 0xffff0000u) | 51u;
        if (!word_count || word_count > sizeof specialization_composite / sizeof specialization_composite[0] - i)
            break;
        i += word_count;
    }
    make_module(composite_shader_bytes, 0, 0x00010600u, 29, 0, specialization_composite,
                sizeof specialization_composite / sizeof specialization_composite[0]);
    rc = spirv_read(composite_shader_bytes, sizeof composite_shader_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *specialized_composite = NULL;
    for (uint32_t i = 0; common_shader && i < gpu_ir_shader_instruction_count(common_shader); i++) {
        const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(common_shader, i);
        if (candidate->source_opcode == 51 && candidate->result_id == 14) specialized_composite = candidate;
    }
    check("OpSpecConstantComposite lowers with its specialization flag",
          shader_rc == GPU_IR_OK && specialized_composite &&
          specialized_composite->operation == GPU_IR_SHADER_CONSTANT_COMPOSITE &&
          (specialized_composite->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION));
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    uint32_t invalid_composite_spec_id[sizeof specialization_composite / sizeof specialization_composite[0] + 4];
    memcpy(invalid_composite_spec_id, specialization_composite, 16 * sizeof(uint32_t));
    const uint32_t composite_spec_id_decoration[] = { 0x00040047u, 14, 1, 20 };
    memcpy(invalid_composite_spec_id + 16, composite_spec_id_decoration, sizeof composite_spec_id_decoration);
    memcpy(invalid_composite_spec_id + 20, specialization_composite + 16,
           (sizeof specialization_composite / sizeof specialization_composite[0] - 16) * sizeof(uint32_t));
    uint8_t invalid_composite_spec_id_bytes[(5 + sizeof invalid_composite_spec_id /
        sizeof invalid_composite_spec_id[0]) * 4];
    make_module(invalid_composite_spec_id_bytes, 0, 0x00010600u, 29, 0, invalid_composite_spec_id,
                sizeof invalid_composite_spec_id / sizeof invalid_composite_spec_id[0]);
    rc = spirv_read(invalid_composite_spec_id_bytes, sizeof invalid_composite_spec_id_bytes, &module);
    check("SPIR-V rejects SpecId decorations on composite specialization constants", rc == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t shader_switch[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 5, 0x6e69616du, 0,
        0x00020013u, 1,
        0x00040015u, 2, 32, 0,
        0x00030021u, 3, 1,
        0x0004002bu, 2, 4, 42,
        0x00050036u, 1, 5, 0, 3,
        0x000200f8u, 6,
        0x000500fbu, 4, 6, 42, 7,     // switch literal remains a literal; labels remain ID operands
        0x000200f8u, 7,
        0x000100fdu,
        0x00010038u
    };
    uint8_t shader_switch_bytes[(5 + sizeof shader_switch / sizeof shader_switch[0]) * 4];
    make_module(shader_switch_bytes, 0, 0x00010600u, 8, 0, shader_switch,
                sizeof shader_switch / sizeof shader_switch[0]);
    rc = spirv_read(shader_switch_bytes, sizeof shader_switch_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *common_switch = common_shader ?
        gpu_ir_shader_instruction_at(common_shader, 1) : NULL;
    check("SPIR-V switch lowering separates ID targets from width-encoded literal words",
          shader_rc == GPU_IR_OK && common_switch &&
          common_switch->opcode == GPU_IR_SHADER_OP_SWITCH && common_switch->operand_count == 3 &&
          common_switch->literal_word_count == 1 &&
          gpu_ir_shader_operand_at(common_shader, common_switch->first_operand)->value == 4 &&
          gpu_ir_shader_operand_at(common_shader, common_switch->first_operand + 1)->value == 6 &&
          gpu_ir_shader_operand_at(common_shader, common_switch->first_operand + 2)->value == 7 &&
          gpu_ir_shader_literal_word_at(common_shader, common_switch->first_literal) == 42);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    const uint32_t reachable_functions[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 4, 0x6e69616du, 0,
        0x00020013u, 1,
        0x00030021u, 3, 1,
        0x00050036u, 1, 4, 0, 3,
        0x000200f8u, 6,
        0x00040039u, 1, 7, 5,         // main calls helper
        0x000100fdu,
        0x00010038u,
        0x00050036u, 1, 5, 0, 3,
        0x000200f8u, 8,
        0x000100fdu,
        0x00010038u,
        0x00050036u, 1, 9, 0, 3,      // unreachable function uses unsupported OpExtInst
        0x000200f8u, 10,
        0x0005000cu, 1, 11, 99, 0,
        0x000100fdu,
        0x00010038u
    };
    uint8_t reachable_bytes[(5 + sizeof reachable_functions / sizeof reachable_functions[0]) * 4];
    make_module(reachable_bytes, 0, 0x00010600u, 12, 0, reachable_functions,
                sizeof reachable_functions / sizeof reachable_functions[0]);
    rc = spirv_read(reachable_bytes, sizeof reachable_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("common SPIR-V shader includes reachable calls while excluding unrelated unsupported functions",
          shader_rc == GPU_IR_OK && common_shader &&
          gpu_ir_shader_function_count(common_shader) == 2 &&
          gpu_ir_shader_find_function(common_shader, 4) &&
          gpu_ir_shader_find_function(common_shader, 5) &&
          !gpu_ir_shader_find_function(common_shader, 9));
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    const uint32_t recursive_shader_calls[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 4, 0x6e69616du, 0,
        0x00020013u, 1,
        0x00030021u, 3, 1,
        0x00050036u, 1, 4, 0, 3,
        0x000200f8u, 6,
        0x00040039u, 1, 7, 5,
        0x000100fdu,
        0x00010038u,
        0x00050036u, 1, 5, 0, 3,
        0x000200f8u, 8,
        0x00040039u, 1, 9, 4,
        0x000100fdu,
        0x00010038u
    };
    uint8_t recursive_bytes[(5 + sizeof recursive_shader_calls / sizeof recursive_shader_calls[0]) * 4];
    make_module(recursive_bytes, 0, 0x00010600u, 10, 0, recursive_shader_calls,
                sizeof recursive_shader_calls / sizeof recursive_shader_calls[0]);
    rc = spirv_read(recursive_bytes, sizeof recursive_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("recursive call graphs are rejected for a Vulkan shader entry point",
          rc == SPIRV_OK && shader_rc == GPU_IR_INVALID && common_shader == NULL);
    spirv_free(&module);

    const uint32_t unsupported_shader_instruction[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 4, 0x6e69616du, 0,
        0x0006000bu, 7, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0,
        0x00020013u, 1,
        0x00040015u, 2, 32, 0,
        0x00030021u, 3, 1,
        0x0004002bu, 2, 8, 0,
        0x00050036u, 1, 4, 0, 3,
        0x000200f8u, 5,
        0x0006000cu, 2, 6, 7, 999, 8, // unknown GLSL.std.450 instruction has no common lowering
        0x000100fdu,
        0x00010038u
    };
    uint8_t unsupported_shader_bytes[(5 + sizeof unsupported_shader_instruction / sizeof unsupported_shader_instruction[0]) * 4];
    make_module(unsupported_shader_bytes, 0, 0x00010600u, 9, 0, unsupported_shader_instruction,
                sizeof unsupported_shader_instruction / sizeof unsupported_shader_instruction[0]);
    rc = spirv_read(unsupported_shader_bytes, sizeof unsupported_shader_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    check("reachable GLSL extended instructions without a semantic lowering are rejected explicitly",
          rc == SPIRV_OK && shader_rc == GPU_IR_UNSUPPORTED && common_shader == NULL);
    spirv_free(&module);

    const uint32_t bit_find_shader[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 10, 0x6e69616du, 0,
        0x00060010u, 10, 17, 1, 1, 1,
        0x0006000bu, 7, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0,
        0x00020013u, 1,                  // void
        0x00040015u, 2, 32, 1,          // signed int
        0x00040015u, 3, 32, 0,          // unsigned int
        0x00040017u, 4, 2, 2,           // int2
        0x00040017u, 5, 3, 2,           // uint2
        0x00030021u, 6, 1,              // void()
        0x0004002bu, 2, 8, 0xfffffff9u, // int -7
        0x0004002bu, 2, 9, 7,           // int 7
        0x0005002cu, 4, 11, 8, 9,       // int2(-7, 7)
        0x0004002bu, 3, 12, 0x80000000u,
        0x0004002bu, 3, 13, 1,
        0x0005002cu, 5, 14, 12, 13,     // uint2 high-bit, one
        0x00050036u, 1, 10, 0, 6,
        0x000200f8u, 15,
        0x0006000cu, 4, 16, 7, 73, 14,  // FindILsb accepts unsigned input and signed result
        0x0006000cu, 4, 17, 7, 74, 11,  // FindSMsb
        0x0006000cu, 4, 18, 7, 75, 14,  // FindUMsb accepts unsigned input and signed result
        0x000100fdu,
        0x00010038u
    };
    uint8_t bit_find_bytes[(5 + sizeof bit_find_shader / sizeof bit_find_shader[0]) * 4];
    make_module(bit_find_bytes, 0, 0x00010600u, 20, 0, bit_find_shader,
                sizeof bit_find_shader / sizeof bit_find_shader[0]);
    rc = spirv_read(bit_find_bytes, sizeof bit_find_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const uint32_t bit_find_ids[] = { 16, 17, 18 };
    const uint32_t bit_find_ops[] = {
        GPU_IR_SHADER_ARITH_BIT_FIND_LSB,
        GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED,
        GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED
    };
    const uint32_t bit_find_inputs[] = { 14, 11, 14 };
    int bit_find_ok = rc == SPIRV_OK && shader_rc == GPU_IR_OK && common_shader;
    for (uint32_t i = 0; bit_find_ok && i < 3; i++) {
        const gpu_ir_shader_instruction *found = NULL;
        for (uint32_t j = 0; j < gpu_ir_shader_instruction_count(common_shader); j++) {
            const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(common_shader, j);
            if (candidate && candidate->result_id == bit_find_ids[i]) { found = candidate; break; }
        }
        const gpu_ir_shader_operand *input = found && found->operand_count == 1 ?
            gpu_ir_shader_operand_at(common_shader, found->first_operand) : NULL;
        bit_find_ok = found && found->source_opcode == 12 && found->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            found->operation == bit_find_ops[i] && found->result_type_id == 4 &&
            found->flags == 0 && input && input->kind == GPU_IR_SHADER_OPERAND_ID &&
            input->value == bit_find_inputs[i];
    }
    check("GLSL.std.450 FindILsb, FindSMsb and FindUMsb normalize scalar/vector integer operands to common IR",
          bit_find_ok);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    const uint32_t float_mix_shader[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 10, 0x6e69616du, 0,
        0x00060010u, 10, 17, 1, 1, 1,
        0x0006000bu, 7, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0,
        0x00020013u, 1,                  // void
        0x00030016u, 2, 32,             // float
        0x00040017u, 3, 2, 4,           // float4
        0x00030021u, 4, 1,              // void()
        0x0004002bu, 2, 5, 0x3e800000u, // float 0.25
        0x0004002bu, 2, 6, 0x3f800000u, // float 1.0
        0x0004002bu, 2, 17, 0x3f000000u, // float 0.5
        0x0004002bu, 2, 18, 0x3f400000u, // float 0.75
        0x0007002cu, 3, 9, 5, 6, 5, 6, // float4
        0x0007002cu, 3, 19, 17, 18, 17, 18, // float4
        0x0007002cu, 3, 20, 6, 5, 6, 5, // float4
        0x0004002bu, 2, 21, 0x3e800000u, // float 0.25
        0x0004002bu, 2, 22, 0x3f400000u, // float 0.75
        0x0004002bu, 2, 23, 0x3f000000u, // float 0.5
        0x0007002cu, 3, 24, 21, 21, 21, 21, // float4 edge0
        0x0007002cu, 3, 25, 22, 22, 22, 22, // float4 edge1
        0x0007002cu, 3, 26, 23, 23, 23, 23, // float4 x
        0x00050036u, 1, 10, 0, 4,
        0x000200f8u, 15,
        0x0008000cu, 3, 16, 7, 46, 9, 19, 20, // FMix(x, y, a)
        0x0007000cu, 3, 27, 7, 48, 24, 26, // Step(edge, x)
        0x0008000cu, 3, 28, 7, 49, 24, 25, 26, // SmoothStep(edge0, edge1, x)
        0x000100fdu,
        0x00010038u
    };
    uint8_t float_mix_bytes[(5 + sizeof float_mix_shader / sizeof float_mix_shader[0]) * 4];
    make_module(float_mix_bytes, 0, 0x00010600u, 29, 0, float_mix_shader,
                sizeof float_mix_shader / sizeof float_mix_shader[0]);
    rc = spirv_read(float_mix_bytes, sizeof float_mix_bytes, &module);
    shader_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_shader = NULL;
    shader_rc = shader_entry ? spirv_build_gpu_ir_shader(&module, shader_entry, &common_shader) : GPU_IR_INVALID;
    const gpu_ir_shader_instruction *float_mix = NULL;
    for (uint32_t i = 0; common_shader && i < gpu_ir_shader_instruction_count(common_shader); i++) {
        const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(common_shader, i);
        if (candidate && candidate->result_id == 16) { float_mix = candidate; break; }
    }
    const gpu_ir_shader_operand *float_mix_x = float_mix ?
        gpu_ir_shader_operand_at(common_shader, float_mix->first_operand) : NULL;
    const gpu_ir_shader_operand *float_mix_y = float_mix ?
        gpu_ir_shader_operand_at(common_shader, float_mix->first_operand + 1u) : NULL;
    const gpu_ir_shader_operand *float_mix_a = float_mix ?
        gpu_ir_shader_operand_at(common_shader, float_mix->first_operand + 2u) : NULL;
    const gpu_ir_shader_instruction *float_step = NULL, *float_smoothstep = NULL;
    for (uint32_t i = 0; common_shader && i < gpu_ir_shader_instruction_count(common_shader); i++) {
        const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(common_shader, i);
        if (!candidate) continue;
        if (candidate->result_id == 27) float_step = candidate;
        if (candidate->result_id == 28) float_smoothstep = candidate;
    }
    const gpu_ir_shader_operand *float_step_edge = float_step ?
        gpu_ir_shader_operand_at(common_shader, float_step->first_operand) : NULL;
    const gpu_ir_shader_operand *float_step_x = float_step ?
        gpu_ir_shader_operand_at(common_shader, float_step->first_operand + 1u) : NULL;
    const gpu_ir_shader_operand *float_smoothstep_edge0 = float_smoothstep ?
        gpu_ir_shader_operand_at(common_shader, float_smoothstep->first_operand) : NULL;
    const gpu_ir_shader_operand *float_smoothstep_edge1 = float_smoothstep ?
        gpu_ir_shader_operand_at(common_shader, float_smoothstep->first_operand + 1u) : NULL;
    const gpu_ir_shader_operand *float_smoothstep_x = float_smoothstep ?
        gpu_ir_shader_operand_at(common_shader, float_smoothstep->first_operand + 2u) : NULL;
    char *float_mix_msl = NULL;
    size_t float_mix_msl_size = 0;
    int float_mix_msl_rc = common_shader ? gpu_ir_msl_emit_compute(common_shader, &float_mix_msl,
                                                                     &float_mix_msl_size) : GPU_IR_INVALID;
    const char *expected_float_mix =
        "float4(as_type<float>(0x3e800000u), as_type<float>(0x3f800000u), "
        "as_type<float>(0x3e800000u), as_type<float>(0x3f800000u)) * "
        "(float4(1.0f) - float4(as_type<float>(0x3f800000u), as_type<float>(0x3e800000u), "
        "as_type<float>(0x3f800000u), as_type<float>(0x3e800000u))) + "
        "float4(as_type<float>(0x3f000000u), as_type<float>(0x3f400000u), "
        "as_type<float>(0x3f000000u), as_type<float>(0x3f400000u)) * "
        "float4(as_type<float>(0x3f800000u), as_type<float>(0x3e800000u), "
        "as_type<float>(0x3f800000u), as_type<float>(0x3e800000u));";
    const char *expected_float_step =
        "float4 vx_value_27 = step(float4(as_type<float>(0x3e800000u), as_type<float>(0x3e800000u), "
        "as_type<float>(0x3e800000u), as_type<float>(0x3e800000u)), "
        "float4(as_type<float>(0x3f000000u), as_type<float>(0x3f000000u), "
        "as_type<float>(0x3f000000u), as_type<float>(0x3f000000u)));";
    const char *expected_float_smoothstep =
        "float4 vx_value_28 = smoothstep(float4(as_type<float>(0x3e800000u), as_type<float>(0x3e800000u), "
        "as_type<float>(0x3e800000u), as_type<float>(0x3e800000u)), "
        "float4(as_type<float>(0x3f400000u), as_type<float>(0x3f400000u), "
        "as_type<float>(0x3f400000u), as_type<float>(0x3f400000u)), "
        "float4(as_type<float>(0x3f000000u), as_type<float>(0x3f000000u), "
        "as_type<float>(0x3f000000u), as_type<float>(0x3f000000u)));";
    int float_mix_ok = rc == SPIRV_OK && shader_rc == GPU_IR_OK && float_mix &&
        float_mix->source_api == GPU_IR_SHADER_SOURCE_SPIRV && float_mix->source_opcode == 12 &&
        float_mix->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
        float_mix->operation == GPU_IR_SHADER_ARITH_INTERPOLATE && float_mix->result_type_id == 3 &&
        float_mix->flags == GPU_IR_SHADER_FLAG_FLOAT && float_mix->operand_count == 3 &&
        float_mix_x && float_mix_x->kind == GPU_IR_SHADER_OPERAND_ID && float_mix_x->value == 9 &&
        float_mix_y && float_mix_y->kind == GPU_IR_SHADER_OPERAND_ID && float_mix_y->value == 19 &&
        float_mix_a && float_mix_a->kind == GPU_IR_SHADER_OPERAND_ID && float_mix_a->value == 20 &&
        float_mix_msl_rc == GPU_IR_OK && float_mix_msl_size && float_mix_msl &&
        strstr(float_mix_msl, expected_float_mix) &&
        float_step && float_step->operation == GPU_IR_SHADER_ARITH_STEP && float_step->operand_count == 2 &&
        float_step_edge && float_step_edge->kind == GPU_IR_SHADER_OPERAND_ID && float_step_edge->value == 24 &&
        float_step_x && float_step_x->kind == GPU_IR_SHADER_OPERAND_ID && float_step_x->value == 26 &&
        float_smoothstep && float_smoothstep->operation == GPU_IR_SHADER_ARITH_SMOOTHSTEP &&
        float_smoothstep->operand_count == 3 &&
        float_smoothstep_edge0 && float_smoothstep_edge0->kind == GPU_IR_SHADER_OPERAND_ID &&
        float_smoothstep_edge0->value == 24 &&
        float_smoothstep_edge1 && float_smoothstep_edge1->kind == GPU_IR_SHADER_OPERAND_ID &&
        float_smoothstep_edge1->value == 25 &&
        float_smoothstep_x && float_smoothstep_x->kind == GPU_IR_SHADER_OPERAND_ID &&
        float_smoothstep_x->value == 26 &&
        strstr(float_mix_msl, expected_float_step) && strstr(float_mix_msl, expected_float_smoothstep);
    check("GLSL.std.450 FMix, Step and SmoothStep preserve operands in common IR and emit their specified MSL operations",
          float_mix_ok);
    free(float_mix_msl);
    gpu_ir_shader_release(common_shader);
    spirv_free(&module);

    uint32_t unterminated_function[sizeof function_body / sizeof function_body[0]];
    memcpy(unterminated_function, function_body, sizeof function_body);
    unterminated_function[sizeof unterminated_function / sizeof unterminated_function[0] - 2] = 0x00010000u;
    make_module(function_bytes, 0, 0x00010600u, 5, 0, unterminated_function,
                sizeof unterminated_function / sizeof unterminated_function[0]);
    rc = spirv_read(function_bytes, sizeof function_bytes, &module);
    check("unterminated basic blocks are rejected before shader lowering", rc == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t parameter_function[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00040015u, 3, 32, 0,          // OpTypeInt
        0x00040021u, 4, 2, 3,           // OpTypeFunction (void, uint)
        0x00050036u, 2, 5, 0, 4,        // OpFunction
        0x00030037u, 3, 6,              // OpFunctionParameter
        0x000200f8u, 7,                 // OpLabel
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t parameter_bytes[(5 + sizeof parameter_function / sizeof parameter_function[0]) * 4];
    make_module(parameter_bytes, 0, 0x00010600u, 8, 0, parameter_function,
                sizeof parameter_function / sizeof parameter_function[0]);
    rc = spirv_read(parameter_bytes, sizeof parameter_bytes, &module);
    function = rc == SPIRV_OK ? spirv_find_function(&module, 5) : NULL;
    check("function parameter count and types match the declared signature",
          function && function->parameter_count == 1 && function->block_count == 1);
    const spirv_id_record *function_id_record = rc == SPIRV_OK ? spirv_find_id(&module, 5) : NULL;
    const spirv_id_record *parameter_id_record = rc == SPIRV_OK ? spirv_find_id(&module, 6) : NULL;
    const spirv_id_record *label_id_record = rc == SPIRV_OK ? spirv_find_id(&module, 7) : NULL;
    check("function, parameter and label result IDs share the module uniqueness index",
          function_id_record && function_id_record->kind == SPIRV_ID_FUNCTION &&
          parameter_id_record && parameter_id_record->kind == SPIRV_ID_FUNCTION_PARAMETER &&
          parameter_id_record->type_id == 3 && parameter_id_record->owner_function_id == 5 &&
          label_id_record && label_id_record->kind == SPIRV_ID_LABEL && label_id_record->owner_function_id == 5);
    spirv_free(&module);

    const uint32_t duplicate_label_body[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00030021u, 3, 2,              // OpTypeFunction
        0x00050036u, 2, 1, 0, 3,        // OpFunction
        0x000200f8u, 4,                 // OpLabel
        0x000200f9u, 4,                 // OpBranch
        0x000200f8u, 4,                 // Duplicate OpLabel result ID
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t duplicate_body_bytes[(5 + sizeof duplicate_label_body / sizeof duplicate_label_body[0]) * 4];
    make_module(duplicate_body_bytes, 0, 0x00010600u, 5, 0, duplicate_label_body,
                sizeof duplicate_label_body / sizeof duplicate_label_body[0]);
    rc = spirv_read(duplicate_body_bytes, sizeof duplicate_body_bytes, &module);
    check("duplicate basic-block result IDs are rejected by the ID index", rc == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t duplicate_function_body[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00030021u, 3, 2,              // OpTypeFunction
        0x00050036u, 2, 1, 0, 3,        // OpFunction
        0x00010038u,                    // OpFunctionEnd (bodyless declaration)
        0x00050036u, 2, 1, 0, 3,        // Duplicate OpFunction result ID
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t duplicate_function_bytes[(5 + sizeof duplicate_function_body / sizeof duplicate_function_body[0]) * 4];
    make_module(duplicate_function_bytes, 0, 0x00010600u, 4, 0, duplicate_function_body,
                sizeof duplicate_function_body / sizeof duplicate_function_body[0]);
    rc = spirv_read(duplicate_function_bytes, sizeof duplicate_function_bytes, &module);
    check("duplicate function result IDs are rejected by the ID index", rc == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t duplicate_parameter_body[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00040015u, 3, 32, 0,          // OpTypeInt
        0x00050021u, 4, 2, 3, 3,        // OpTypeFunction (void, uint, uint)
        0x00050036u, 2, 5, 0, 4,        // OpFunction
        0x00030037u, 3, 6,              // OpFunctionParameter
        0x00030037u, 3, 6,              // Duplicate OpFunctionParameter result ID
        0x000200f8u, 7,                 // OpLabel
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t duplicate_parameter_bytes[(5 + sizeof duplicate_parameter_body / sizeof duplicate_parameter_body[0]) * 4];
    make_module(duplicate_parameter_bytes, 0, 0x00010600u, 8, 0, duplicate_parameter_body,
                sizeof duplicate_parameter_body / sizeof duplicate_parameter_body[0]);
    rc = spirv_read(duplicate_parameter_bytes, sizeof duplicate_parameter_bytes, &module);
    check("duplicate function-parameter result IDs are rejected by the ID index", rc == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t forward_branch[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00030021u, 3, 2,              // OpTypeFunction
        0x00050036u, 2, 1, 0, 3,        // OpFunction
        0x000200f8u, 4,                 // OpLabel
        0x000200f9u, 6,                 // OpBranch to a later block in this function
        0x000200f8u, 6,                 // OpLabel
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t branch_bytes[(5 + sizeof forward_branch / sizeof forward_branch[0]) * 4];
    make_module(branch_bytes, 0, 0x00010600u, 7, 0, forward_branch,
                sizeof forward_branch / sizeof forward_branch[0]);
    rc = spirv_read(branch_bytes, sizeof branch_bytes, &module);
    check("forward branch targets resolve to a label in the same function", rc == SPIRV_OK);
    spirv_free(&module);

    const uint32_t missing_branch_target[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00030021u, 3, 2,              // OpTypeFunction
        0x00050036u, 2, 1, 0, 3,        // OpFunction
        0x000200f8u, 4,                 // OpLabel
        0x000200f9u, 5,                 // OpBranch to an undefined label
        0x000200f8u, 6,                 // OpLabel
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    make_module(branch_bytes, 0, 0x00010600u, 7, 0, missing_branch_target,
                sizeof missing_branch_target / sizeof missing_branch_target[0]);
    rc = spirv_read(branch_bytes, sizeof branch_bytes, &module);
    check("branch to an undefined label is rejected", rc == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t cross_function_branch[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00030021u, 3, 2,              // OpTypeFunction
        0x00050036u, 2, 1, 0, 3,        // First OpFunction
        0x000200f8u, 4,                 // OpLabel
        0x000200f9u, 7,                 // OpBranch to another function's label
        0x00010038u,                    // OpFunctionEnd
        0x00050036u, 2, 5, 0, 3,        // Second OpFunction
        0x000200f8u, 7,                 // OpLabel
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t cross_function_bytes[(5 + sizeof cross_function_branch / sizeof cross_function_branch[0]) * 4];
    make_module(cross_function_bytes, 0, 0x00010600u, 8, 0, cross_function_branch,
                sizeof cross_function_branch / sizeof cross_function_branch[0]);
    rc = spirv_read(cross_function_bytes, sizeof cross_function_bytes, &module);
    check("branch to another function's label is rejected", rc == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t same_conditional_targets[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00020014u, 3,                 // OpTypeBool
        0x00040021u, 4, 2, 3,           // OpTypeFunction (void, bool)
        0x00050036u, 2, 1, 0, 4,        // OpFunction
        0x00030037u, 3, 5,              // OpFunctionParameter
        0x000200f8u, 6,                 // OpLabel
        0x000400fau, 5, 7, 7,           // OpBranchConditional: same true and false target
        0x000200f8u, 7,                 // OpLabel
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t conditional_bytes[(5 + sizeof same_conditional_targets / sizeof same_conditional_targets[0]) * 4];
    make_module(conditional_bytes, 0, 0x00010600u, 8, 0, same_conditional_targets,
                sizeof same_conditional_targets / sizeof same_conditional_targets[0]);
    rc = spirv_read(conditional_bytes, sizeof conditional_bytes, &module);
    check("SPIR-V 1.6 rejects a conditional branch with identical targets", rc == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t conditional_weights[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00020014u, 3,                 // OpTypeBool
        0x00040021u, 4, 2, 3,           // OpTypeFunction (void, bool)
        0x00050036u, 2, 1, 0, 4,        // OpFunction
        0x00030037u, 3, 5,              // OpFunctionParameter
        0x000200f8u, 6,                 // OpLabel
        0x000600fau, 5, 7, 8, 0, 0,     // OpBranchConditional: both weights are zero
        0x000200f8u, 7,                 // OpLabel
        0x000100fdu,                    // OpReturn
        0x000200f8u, 8,                 // OpLabel
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t weights_bytes[(5 + sizeof conditional_weights / sizeof conditional_weights[0]) * 4];
    make_module(weights_bytes, 0, 0x00010600u, 9, 0, conditional_weights,
                sizeof conditional_weights / sizeof conditional_weights[0]);
    rc = spirv_read(weights_bytes, sizeof weights_bytes, &module);
    check("conditional branch weights must have a nonzero sum", rc == SPIRV_INVALID);
    spirv_free(&module);

    uint32_t overflowing_weights[sizeof conditional_weights / sizeof conditional_weights[0]];
    memcpy(overflowing_weights, conditional_weights, sizeof conditional_weights);
    overflowing_weights[22] = UINT32_MAX;
    overflowing_weights[23] = 1;
    make_module(weights_bytes, 0, 0x00010600u, 9, 0, overflowing_weights,
                sizeof overflowing_weights / sizeof overflowing_weights[0]);
    rc = spirv_read(weights_bytes, sizeof weights_bytes, &module);
    check("conditional branch weight sums cannot overflow 32 bits", rc == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t switch_arithmetic[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00040015u, 3, 32, 0,          // OpTypeInt uint32
        0x00050021u, 4, 2, 3, 3,        // OpTypeFunction (void, uint, uint)
        0x00050036u, 2, 1, 0, 4,        // OpFunction
        0x00030037u, 3, 5,              // OpFunctionParameter
        0x00030037u, 3, 6,              // OpFunctionParameter
        0x000200f8u, 7,                 // OpLabel
        0x00050080u, 3, 8, 5, 6,        // OpIAdd: typed instruction result
        0x000500fbu, 8, 9, 42, 10,      // OpSwitch on the arithmetic result
        0x000200f8u, 9,                 // Default OpLabel
        0x000100fdu,                    // OpReturn
        0x000200f8u, 10,                // Case OpLabel
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t switch_bytes[(5 + sizeof switch_arithmetic / sizeof switch_arithmetic[0]) * 4];
    make_module(switch_bytes, 0, 0x00010600u, 11, 0, switch_arithmetic,
                sizeof switch_arithmetic / sizeof switch_arithmetic[0]);
    rc = spirv_read(switch_bytes, sizeof switch_bytes, &module);
    const spirv_id_record *sum_value = rc == SPIRV_OK ? spirv_find_id(&module, 8) : NULL;
    check("typed arithmetic result IDs resolve 32-bit OpSwitch case targets",
          rc == SPIRV_OK && sum_value && sum_value->kind == SPIRV_ID_INSTRUCTION_RESULT &&
          sum_value->type_id == 3 && sum_value->owner_function_id == 1);
    spirv_free(&module);

    uint32_t switch_unknown_target[sizeof switch_arithmetic / sizeof switch_arithmetic[0]];
    memcpy(switch_unknown_target, switch_arithmetic, sizeof switch_arithmetic);
    switch_unknown_target[33] = 11;    // Case target is in bound but has no OpLabel definition.
    make_module(switch_bytes, 0, 0x00010600u, 12, 0, switch_unknown_target,
                sizeof switch_unknown_target / sizeof switch_unknown_target[0]);
    rc = spirv_read(switch_bytes, sizeof switch_bytes, &module);
    check("typed OpSwitch rejects an undefined case label", rc == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t switch_wide_selector[] = {
        0x00020013u, 2,                 // OpTypeVoid
        0x00040015u, 3, 64, 0,          // OpTypeInt uint64
        0x00040021u, 4, 2, 3,           // OpTypeFunction (void, uint64)
        0x00050036u, 2, 1, 0, 4,        // OpFunction
        0x00030037u, 3, 5,              // OpFunctionParameter
        0x000200f8u, 6,                 // OpLabel
        0x000600fbu, 5, 7, 0x89abcdefu, 1, 8, // OpSwitch: two literal words then case label
        0x000200f8u, 7,                 // Default OpLabel
        0x000100fdu,                    // OpReturn
        0x000200f8u, 8,                 // Case OpLabel
        0x000100fdu,                    // OpReturn
        0x00010038u                     // OpFunctionEnd
    };
    uint8_t wide_switch_bytes[(5 + sizeof switch_wide_selector / sizeof switch_wide_selector[0]) * 4];
    make_module(wide_switch_bytes, 0, 0x00010600u, 9, 0, switch_wide_selector,
                sizeof switch_wide_selector / sizeof switch_wide_selector[0]);
    rc = spirv_read(wide_switch_bytes, sizeof wide_switch_bytes, &module);
    check("64-bit OpSwitch literals use their width-dependent two-word encoding", rc == SPIRV_OK);
    spirv_free(&module);

    uint32_t mismatched_parameter[sizeof parameter_function / sizeof parameter_function[0]];
    memcpy(mismatched_parameter, parameter_function, sizeof parameter_function);
    mismatched_parameter[16] = 2;       // OpFunctionParameter type does not match the function signature.
    make_module(parameter_bytes, 0, 0x00010600u, 8, 0, mismatched_parameter,
                sizeof mismatched_parameter / sizeof mismatched_parameter[0]);
    rc = spirv_read(parameter_bytes, sizeof parameter_bytes, &module);
    check("mismatched function parameter types are rejected", rc == SPIRV_INVALID);
    spirv_free(&module);

    // Synthetic entry-point metadata: main, GLCompute, LocalSize = (8, 4, 2), no interface variables.
    const uint32_t compute[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 1, 0x6e69616du, 0,
        0x00060010u, 1, 17, 8, 4, 2
    };
    uint8_t compute_bytes[(5 + sizeof compute / sizeof compute[0]) * 4];
    make_module(compute_bytes, 0, 0x00010600u, 2, 0, compute, sizeof compute / sizeof compute[0]);
    rc = spirv_read(compute_bytes, sizeof compute_bytes, &module);
    const spirv_entry_point *entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    check("compute entry point name, execution model and literal local size",
          entry && entry->function_id == 1 && entry->interface_count == 0 &&
          entry->local_size_kind == SPIRV_LOCAL_SIZE_LITERAL && entry->local_size[0] == 8 &&
          entry->local_size[1] == 4 && entry->local_size[2] == 2 && entry->has_local_size_default &&
          entry->local_size_default[0] == 8 && entry->local_size_default[1] == 4 && entry->local_size_default[2] == 2);
    spirv_free(&module);

    make_module(compute_bytes, 1, 0x00010600u, 2, 0, compute, sizeof compute / sizeof compute[0]);
    rc = spirv_read(compute_bytes, sizeof compute_bytes, &module);
    entry = rc == SPIRV_OK ? spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    check("big-endian entry-point and execution-mode decoding",
          entry && entry->local_size_default[0] == 8 && entry->local_size_default[1] == 4 && entry->local_size_default[2] == 2);
    spirv_free(&module);

    const uint32_t compute_id_size[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 1, 0x6e69616du, 0,
        0x0006014bu, 1, 38, 4, 5, 6,
        0x00040015u, 7, 32, 0,
        0x00040032u, 7, 4, 4,
        0x00040032u, 7, 5, 5,
        0x00040032u, 7, 6, 6
    };
    uint8_t compute_id_bytes[(5 + sizeof compute_id_size / sizeof compute_id_size[0]) * 4];
    make_module(compute_id_bytes, 0, 0x00010600u, 8, 0, compute_id_size,
                sizeof compute_id_size / sizeof compute_id_size[0]);
    rc = spirv_read(compute_id_bytes, sizeof compute_id_bytes, &module);
    entry = rc == SPIRV_OK ? spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_ANY) : NULL;
    check("LocalSizeId resolved through forward-referenced specialization constants",
          entry && entry->local_size_kind == SPIRV_LOCAL_SIZE_IDS && entry->local_size[0] == 4 &&
          entry->local_size[1] == 5 && entry->local_size[2] == 6 && entry->has_local_size_default &&
          entry->local_size_default[0] == 4 && entry->local_size_default[1] == 5 && entry->local_size_default[2] == 6);
    const spirv_id_record *integer_type = rc == SPIRV_OK ? spirv_find_id(&module, 7) : NULL;
    const spirv_id_record *spec_value = rc == SPIRV_OK ? spirv_find_id(&module, 4) : NULL;
    check("scalar type and specialization constants indexed by result ID",
          integer_type && integer_type->kind == SPIRV_ID_TYPE && integer_type->type_kind == SPIRV_TYPE_INT &&
          integer_type->width == 32 && integer_type->signedness == 0 && spec_value &&
          spec_value->kind == SPIRV_ID_CONSTANT && spec_value->is_specialization && spec_value->value_bits == 4);
    spirv_free(&module);

    const uint32_t specialization_shifts[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 1, 0x6e69616du, 0,
        0x00060010u, 1, 17, 1, 1, 1,
        0x00040047u, 6, 1, 7,          // SpecId 7
        0x00020013u, 2,                // OpTypeVoid
        0x00040015u, 3, 32, 0,         // OpTypeInt uint32
        0x00040015u, 4, 32, 1,         // OpTypeInt int32
        0x00030021u, 5, 2,             // OpTypeFunction void()
        0x00040032u, 3, 6, 0x80000001u,// OpSpecConstant
        0x0004002bu, 3, 7, 1,          // shift amount
        0x00060034u, 3, 8, 194, 6, 7,  // OpSpecConstantOp ShiftRightLogical
        0x00060034u, 3, 9, 196, 6, 7,  // OpSpecConstantOp ShiftLeftLogical
        0x0004002bu, 4, 10, 0x80000001u,// signed base
        0x0004002bu, 3, 11, 1,          // unsigned shift amount
        0x00060034u, 4, 12, 195, 10, 11,// OpSpecConstantOp ShiftRightArithmetic
        0x00050036u, 2, 1, 0, 5,
        0x000200f8u, 13,
        0x000100fdu,
        0x00010038u
    };
    uint8_t specialization_shift_bytes[(5 + sizeof specialization_shifts / sizeof specialization_shifts[0]) * 4];
    make_module(specialization_shift_bytes, 0, 0x00010600u, 14, 0, specialization_shifts,
                sizeof specialization_shifts / sizeof specialization_shifts[0]);
    rc = spirv_read(specialization_shift_bytes, sizeof specialization_shift_bytes, &module);
    const spirv_id_record *logical_right = rc == SPIRV_OK ? spirv_find_id(&module, 8) : NULL;
    const spirv_id_record *logical_left = rc == SPIRV_OK ? spirv_find_id(&module, 9) : NULL;
    const spirv_id_record *arithmetic_right = rc == SPIRV_OK ? spirv_find_id(&module, 12) : NULL;
    check("32-bit specialization shifts preserve logical and sign-filled results",
          rc == SPIRV_OK && logical_right && logical_right->value_bits == 0x40000000u &&
          logical_left && logical_left->value_bits == 2 && arithmetic_right &&
          arithmetic_right->value_bits == 0xc0000000u);
    spirv_free(&module);
    uint32_t invalid_specialization_shift[sizeof specialization_shifts / sizeof specialization_shifts[0]];
    memcpy(invalid_specialization_shift, specialization_shifts, sizeof specialization_shifts);
    invalid_specialization_shift[35] = 32; // OpConstant shift amount for result ID 7.
    make_module(specialization_shift_bytes, 0, 0x00010600u, 14, 0, invalid_specialization_shift,
                sizeof invalid_specialization_shift / sizeof invalid_specialization_shift[0]);
    rc = spirv_read(specialization_shift_bytes, sizeof specialization_shift_bytes, &module);
    check("specialization shifts that produce poison are rejected instead of assigned a fabricated value",
          rc == SPIRV_UNSUPPORTED);
    spirv_free(&module);

    const uint32_t specialization_division[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 1, 0x6e69616du, 0,
        0x00060010u, 1, 17, 1, 1, 1,
        0x00020013u, 2,                // OpTypeVoid
        0x00040015u, 3, 32, 0,         // OpTypeInt uint32
        0x00040015u, 4, 32, 1,         // OpTypeInt int32
        0x00030021u, 5, 2,             // OpTypeFunction void()
        0x00040032u, 3, 6, 7,          // OpSpecConstant uint 7
        0x0004002bu, 3, 7, 2,          // OpConstant uint 2
        0x00060034u, 3, 8, 134, 6, 7,  // OpSpecConstantOp UDiv => 3
        0x00060034u, 3, 9, 137, 6, 7,  // OpSpecConstantOp UMod => 1
        0x0004002bu, 4, 10, 0xfffffff9u,// OpConstant int -7
        0x0004002bu, 4, 11, 2,          // OpConstant int 2
        0x00060034u, 4, 12, 135, 10, 11,// OpSpecConstantOp SDiv => -3
        0x00050036u, 2, 1, 0, 5,
        0x000200f8u, 13,
        0x000100fdu,
        0x00010038u
    };
    uint8_t specialization_division_bytes[(5 + sizeof specialization_division /
        sizeof specialization_division[0]) * 4];
    make_module(specialization_division_bytes, 0, 0x00010600u, 14, 0, specialization_division,
                sizeof specialization_division / sizeof specialization_division[0]);
    rc = spirv_read(specialization_division_bytes, sizeof specialization_division_bytes, &module);
    const spirv_id_record *unsigned_quotient = rc == SPIRV_OK ? spirv_find_id(&module, 8) : NULL;
    const spirv_id_record *unsigned_remainder = rc == SPIRV_OK ? spirv_find_id(&module, 9) : NULL;
    const spirv_id_record *signed_quotient = rc == SPIRV_OK ? spirv_find_id(&module, 12) : NULL;
    check("32-bit unsigned quotient/remainder and signed quotient resolve from specialization constants",
          rc == SPIRV_OK && unsigned_quotient && unsigned_quotient->value_bits == 3 && unsigned_remainder &&
          unsigned_remainder->value_bits == 1 && signed_quotient && signed_quotient->value_bits == 0xfffffffdu);
    spirv_free(&module);

    uint32_t invalid_specialization_division[sizeof specialization_division / sizeof specialization_division[0]];
    memcpy(invalid_specialization_division, specialization_division, sizeof specialization_division);
    for (size_t offset = 0; offset < sizeof invalid_specialization_division / sizeof invalid_specialization_division[0];) {
        uint32_t word_count = invalid_specialization_division[offset] >> 16;
        uint32_t opcode = invalid_specialization_division[offset] & 0xffffu;
        if (!word_count || word_count > sizeof invalid_specialization_division /
            sizeof invalid_specialization_division[0] - offset) break;
        if (opcode == 43 && invalid_specialization_division[offset + 2] == 7)
            invalid_specialization_division[offset + 3] = 0;
        offset += word_count;
    }
    make_module(specialization_division_bytes, 0, 0x00010600u, 14, 0, invalid_specialization_division,
                sizeof invalid_specialization_division / sizeof invalid_specialization_division[0]);
    rc = spirv_read(specialization_division_bytes, sizeof specialization_division_bytes, &module);
    check("integer specialization division by zero is rejected instead of assigned a fabricated value",
          rc == SPIRV_UNSUPPORTED);
    spirv_free(&module);

    memcpy(invalid_specialization_division, specialization_division, sizeof specialization_division);
    for (size_t offset = 0; offset < sizeof invalid_specialization_division / sizeof invalid_specialization_division[0];) {
        uint32_t word_count = invalid_specialization_division[offset] >> 16;
        uint32_t opcode = invalid_specialization_division[offset] & 0xffffu;
        if (!word_count || word_count > sizeof invalid_specialization_division /
            sizeof invalid_specialization_division[0] - offset) break;
        if (opcode == 43 && invalid_specialization_division[offset + 2] == 10)
            invalid_specialization_division[offset + 3] = 0x80000000u;
        if (opcode == 43 && invalid_specialization_division[offset + 2] == 11)
            invalid_specialization_division[offset + 3] = UINT32_MAX;
        offset += word_count;
    }
    make_module(specialization_division_bytes, 0, 0x00010600u, 14, 0, invalid_specialization_division,
                sizeof invalid_specialization_division / sizeof invalid_specialization_division[0]);
    rc = spirv_read(specialization_division_bytes, sizeof specialization_division_bytes, &module);
    check("signed specialization division overflow is rejected instead of invoking host integer undefined behavior",
          rc == SPIRV_UNSUPPORTED);
    spirv_free(&module);

    // A pipeline specialization map can replace these LocalSizeId defaults; common IR must not freeze them early.
    const uint32_t compute_local_size_id_shader[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 1, 0x6e69616du, 0,
        0x0006014bu, 1, 38, 5, 6, 7, // OpExecutionModeId LocalSizeId
        0x00040047u, 5, 1, 10,        // SpecId 10
        0x00040047u, 6, 1, 11,        // SpecId 11
        0x00040047u, 7, 1, 12,        // SpecId 12
        0x00020013u, 2,              // OpTypeVoid
        0x00040015u, 3, 32, 0,       // OpTypeInt uint32
        0x00030021u, 4, 2,           // OpTypeFunction void()
        0x00040032u, 3, 5, 8,        // three specialization constants
        0x00040032u, 3, 6, 4,
        0x00040032u, 3, 7, 2,
        0x00050036u, 2, 1, 0, 4,
        0x000200f8u, 8,
        0x000100fdu,
        0x00010038u
    };
    uint8_t compute_local_size_id_bytes[(5 + sizeof compute_local_size_id_shader /
        sizeof compute_local_size_id_shader[0]) * 4];
    make_module(compute_local_size_id_bytes, 0, 0x00010600u, 9, 0, compute_local_size_id_shader,
        sizeof compute_local_size_id_shader / sizeof compute_local_size_id_shader[0]);
    rc = spirv_read(compute_local_size_id_bytes, sizeof compute_local_size_id_bytes, &module);
    entry = rc == SPIRV_OK ? spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    const spirv_id_record *local_x_spec = rc == SPIRV_OK ? spirv_find_id(&module, 5) : NULL;
    const spirv_id_record *local_y_spec = rc == SPIRV_OK ? spirv_find_id(&module, 6) : NULL;
    check("SpecId decorations bind Vulkan constant IDs to scalar specialization constants",
          local_x_spec && local_x_spec->has_specialization_id && local_x_spec->specialization_id == 10 &&
          local_y_spec && local_y_spec->has_specialization_id && local_y_spec->specialization_id == 11);
    gpu_ir_shader *local_size_id_shader = NULL;
    int local_size_id_lower_rc = entry ? spirv_build_gpu_ir_shader(&module, entry, &local_size_id_shader) : GPU_IR_INVALID;
    uint32_t local_size_id_common[3] = { 1, 1, 1 };
    gpu_ir_shader_threadgroup_size(local_size_id_shader, local_size_id_common);
    check("LocalSizeId defaults remain unresolved in common IR pending pipeline specialization",
          local_size_id_lower_rc == GPU_IR_OK && local_size_id_shader &&
          local_size_id_common[0] == 0 && local_size_id_common[1] == 0 && local_size_id_common[2] == 0);
    uint32_t local_override_values[3] = { 4, 2, 1 }, local_override_threads[3] = { 4, 2, 1 };
    gpu_ir_shader_constant_override local_overrides[3] = {
        { 5, &local_override_values[0], 1 }, { 6, &local_override_values[1], 1 },
        { 7, &local_override_values[2], 1 }
    };
    gpu_ir_shader *specialized_local_size_shader = NULL;
    int local_specialize_rc = local_size_id_shader ? gpu_ir_shader_specialize(local_size_id_shader,
        local_overrides, 3, local_override_threads, &specialized_local_size_shader) : GPU_IR_INVALID;
    uint32_t specialized_local_size[3] = { 0 };
    gpu_ir_shader_threadgroup_size(specialized_local_size_shader, specialized_local_size);
    const gpu_ir_shader_instruction *specialized_size_constant = specialized_local_size_shader ?
        gpu_ir_shader_instruction_at(specialized_local_size_shader, 0) : NULL;
    check("SPIR-V SpecId constants specialize into effective LocalSizeId values",
          local_specialize_rc == GPU_IR_OK && specialized_local_size_shader && specialized_size_constant &&
          !(specialized_size_constant->flags & GPU_IR_SHADER_FLAG_SPECIALIZATION) &&
          gpu_ir_shader_literal_word_at(specialized_local_size_shader, 0) == 4 &&
          specialized_local_size[0] == 4 && specialized_local_size[1] == 2 && specialized_local_size[2] == 1);
    gpu_ir_shader_release(specialized_local_size_shader);
    gpu_ir_shader_release(local_size_id_shader);
    spirv_free(&module);

    const uint32_t float_vector[] = {
        0x00030016u, 1, 32,
        0x00040017u, 2, 1, 4,
        0x00040018u, 3, 2, 4,
        0x0004002bu, 1, 4, 0x3f800000u
    };
    uint8_t type_bytes[(5 + sizeof float_vector / sizeof float_vector[0]) * 4];
    make_module(type_bytes, 0, 0x00010600u, 5, 0, float_vector,
                sizeof float_vector / sizeof float_vector[0]);
    rc = spirv_read(type_bytes, sizeof type_bytes, &module);
    const spirv_id_record *float_type = rc == SPIRV_OK ? spirv_find_id(&module, 1) : NULL;
    const spirv_id_record *vector_type = rc == SPIRV_OK ? spirv_find_id(&module, 2) : NULL;
    const spirv_id_record *matrix_type = rc == SPIRV_OK ? spirv_find_id(&module, 3) : NULL;
    const spirv_id_record *float_constant = rc == SPIRV_OK ? spirv_find_id(&module, 4) : NULL;
    check("float/vector/matrix declarations and scalar literal retained",
          float_type && float_type->type_kind == SPIRV_TYPE_FLOAT && float_type->width == 32 &&
          vector_type && vector_type->type_kind == SPIRV_TYPE_VECTOR && vector_type->element_type == 1 &&
          vector_type->element_count == 4 && matrix_type && matrix_type->type_kind == SPIRV_TYPE_MATRIX &&
          matrix_type->element_type == 2 && matrix_type->element_count == 4 && float_constant &&
          float_constant->kind == SPIRV_ID_CONSTANT && float_constant->value_bits == 0x3f800000u);
    spirv_free(&module);

    // Descriptor and interface reflection. Decorations precede their types/variables in the module.
    const uint32_t reflected[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 29, 0x6e69616du, 0, // GLCompute entry point "main"
        0x00040047u, 8, 34, 2,                 // DescriptorSet 2
        0x00040047u, 8, 33, 5,                 // Binding 5
        0x00030047u, 6, 2,                     // Block
        0x00050048u, 6, 0, 35, 0,              // member 0 Offset 0
        0x00040047u, 3, 6, 16,                 // ArrayStride 16
        0x00040047u, 10, 30, 3,                // Location 3
        0x00040047u, 16, 34, 1, 0x00040047u, 16, 33, 1,
        0x00040047u, 18, 34, 1, 0x00040047u, 18, 33, 2,
        0x00040047u, 20, 34, 1, 0x00040047u, 20, 33, 3,
        0x00040047u, 23, 34, 1, 0x00040047u, 23, 33, 4, 0x00030047u, 23, 25,
        0x00040047u, 26, 34, 1, 0x00040047u, 26, 33, 5,
        0x00040047u, 28, 34, 1, 0x00040047u, 28, 33, 6,
        0x00030047u, 28, 24,                  // NonWritable storage buffer
        0x00040015u, 1, 32, 0,                 // unsigned int
        0x0004002bu, 1, 2, 4,                  // array length
        0x0004001cu, 3, 1, 2,                  // uint[4]
        0x0003001eu, 6, 1,                     // block struct { uint }
        0x00040020u, 7, 2, 6,                  // Uniform pointer to block
        0x0004003bu, 7, 8, 2,                  // Uniform variable
        0x00040020u, 9, 1, 1,                  // Input pointer to uint
        0x0004003bu, 9, 10, 1,                 // Input variable
        0x00030016u, 11, 32,                   // float
        0x00090019u, 12, 11, 1, 0, 0, 0, 1, 0,// sampled 2D image
        0x0003001bu, 13, 12,                   // sampled image
        0x0002001au, 14,                       // sampler
        0x00040020u, 15, 0, 14, 0x0004003bu, 15, 16, 0,
        0x00040020u, 17, 0, 13, 0x0004003bu, 17, 18, 0,
        0x00040020u, 19, 0, 12, 0x0004003bu, 19, 20, 0,
        0x00090019u, 21, 11, 1, 0, 0, 0, 2, 0,
        0x00040020u, 22, 0, 21, 0x0004003bu, 22, 23, 0,
        0x0004001cu, 24, 14, 2, 0x00040020u, 25, 0, 24, 0x0004003bu, 25, 26, 0,
        0x00040020u, 27, 12, 6, 0x0004003bu, 27, 28, 12
    };
    uint8_t reflected_bytes[(5 + sizeof reflected / sizeof reflected[0]) * 4];
    make_module(reflected_bytes, 0, 0x00010600u, 30, 0, reflected,
                sizeof reflected / sizeof reflected[0]);
    rc = spirv_read(reflected_bytes, sizeof reflected_bytes, &module);
    const spirv_id_record *block = rc == SPIRV_OK ? spirv_find_id(&module, 6) : NULL;
    const spirv_id_record *array = rc == SPIRV_OK ? spirv_find_id(&module, 3) : NULL;
    const spirv_id_record *descriptor = rc == SPIRV_OK ? spirv_variable_at(&module, 0) : NULL;
    const spirv_id_record *input = rc == SPIRV_OK ? spirv_variable_at(&module, 1) : NULL;
    const spirv_id_record *image = rc == SPIRV_OK ? spirv_find_id(&module, 12) : NULL;
    spirv_descriptor_info descriptors[7];
    int descriptors_ok = rc == SPIRV_OK && spirv_descriptor_count(&module) == 7;
    for (uint32_t i = 0; descriptors_ok && i < 7; i++)
        descriptors_ok = spirv_descriptor_at(&module, i, &descriptors[i]) == SPIRV_OK;
    const spirv_entry_point *compute_entry = rc == SPIRV_OK ?
        spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    gpu_ir_pipeline_layout *common_layout = NULL;
    int layout_rc = compute_entry ? spirv_build_gpu_ir_layout(&module, compute_entry, &common_layout) : GPU_IR_INVALID;
    gpu_ir_descriptor_binding common_ubo = { 0 }, common_sampler_array = { 0 };
    gpu_ir_descriptor_binding common_storage = { 0 }, common_storage_image = { 0 };
    int common_layout_ok = layout_rc == GPU_IR_OK && common_layout &&
        gpu_ir_pipeline_layout_binding_count(common_layout) == 7 &&
        gpu_ir_pipeline_layout_find_binding(common_layout, 2, 5, &common_ubo) == GPU_IR_OK &&
        common_ubo.kind == GPU_IR_DESCRIPTOR_UNIFORM_BUFFER && common_ubo.count == 1 &&
        common_ubo.stage_mask == GPU_IR_STAGE_COMPUTE && common_ubo.access_mask == GPU_IR_ACCESS_UNIFORM_READ &&
        gpu_ir_pipeline_layout_find_binding(common_layout, 1, 5, &common_sampler_array) == GPU_IR_OK &&
        common_sampler_array.kind == GPU_IR_DESCRIPTOR_SAMPLER && common_sampler_array.count == 4 &&
        common_sampler_array.stage_mask == GPU_IR_STAGE_COMPUTE &&
        gpu_ir_pipeline_layout_find_binding(common_layout, 1, 4, &common_storage_image) == GPU_IR_OK &&
        common_storage_image.kind == GPU_IR_DESCRIPTOR_STORAGE_IMAGE &&
        common_storage_image.access_mask == GPU_IR_ACCESS_SHADER_WRITE &&
        gpu_ir_pipeline_layout_find_binding(common_layout, 1, 6, &common_storage) == GPU_IR_OK &&
        common_storage.kind == GPU_IR_DESCRIPTOR_STORAGE_BUFFER &&
        common_storage.access_mask == GPU_IR_ACCESS_SHADER_READ;
    check("SPIR-V descriptor bindings lower to the shared stage-aware GPU layout", common_layout_ok);
    gpu_ir_pipeline_layout_release(common_layout);
    check("descriptor set/binding, resource kinds/counts, block layout and interface variables",
          rc == SPIRV_OK && spirv_variable_count(&module) == 8 && block &&
          (block->decoration_flags & SPIRV_DECORATION_BLOCK) && block->member_count == 1 &&
          block->members[0].has_offset && block->members[0].offset == 0 && array &&
          array->type_kind == SPIRV_TYPE_ARRAY && array->length_id == 2 && array->has_array_stride &&
          array->array_stride == 16 && descriptor && descriptor->kind == SPIRV_ID_VARIABLE &&
          descriptor->storage_class == 2 && descriptor->pointee_type_id == 6 &&
          descriptor->has_descriptor_set && descriptor->descriptor_set == 2 &&
          descriptor->has_binding && descriptor->binding == 5 && input && input->has_location &&
          input->location == 3 && input->pointee_type_id == 1 && image && image->type_kind == SPIRV_TYPE_IMAGE &&
          image->image_dimension == 1 && image->image_sampled == 1 && descriptors_ok &&
          descriptors[0].descriptor_type == SPIRV_DESCRIPTOR_UNIFORM_BUFFER && descriptors[0].array_count == 1 &&
          descriptors[1].descriptor_type == SPIRV_DESCRIPTOR_SAMPLER &&
          descriptors[2].descriptor_type == SPIRV_DESCRIPTOR_COMBINED_IMAGE_SAMPLER &&
          descriptors[3].descriptor_type == SPIRV_DESCRIPTOR_SAMPLED_IMAGE &&
          descriptors[4].descriptor_type == SPIRV_DESCRIPTOR_STORAGE_IMAGE &&
          descriptors[5].descriptor_type == SPIRV_DESCRIPTOR_SAMPLER && descriptors[5].array_count == 4 &&
          descriptors[6].descriptor_type == SPIRV_DESCRIPTOR_STORAGE_BUFFER);
    spirv_free(&module);

    const uint32_t grouped_descriptor[] = {
        0x00040047u, 20, 34, 2,                 // group DescriptorSet 2 (forward reference)
        0x00040047u, 20, 33, 5,                 // group Binding 5
        0x00020049u, 20,                        // OpDecorationGroup
        0x0003004au, 20, 8,                     // OpGroupDecorate group -> variable
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 29, 0x6e69616du, 0,
        0x00030047u, 6, 2,                       // Block
        0x00050048u, 6, 0, 35, 0,               // member Offset 0
        0x00040015u, 1, 32, 0,
        0x0003001eu, 6, 1,
        0x00040020u, 7, 2, 6,
        0x0004003bu, 7, 8, 2
    };
    uint8_t grouped_descriptor_bytes[(5 + sizeof grouped_descriptor / sizeof grouped_descriptor[0]) * 4];
    make_module(grouped_descriptor_bytes, 0, 0x00010600u, 30, 0, grouped_descriptor,
                sizeof grouped_descriptor / sizeof grouped_descriptor[0]);
    rc = spirv_read(grouped_descriptor_bytes, sizeof grouped_descriptor_bytes, &module);
    descriptor = rc == SPIRV_OK ? spirv_variable_at(&module, 0) : NULL;
    compute_entry = rc == SPIRV_OK ? spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_layout = NULL;
    layout_rc = compute_entry ? spirv_build_gpu_ir_layout(&module, compute_entry, &common_layout) : GPU_IR_INVALID;
    common_ubo = (gpu_ir_descriptor_binding){ 0 };
    common_layout_ok = layout_rc == GPU_IR_OK && common_layout && descriptor && descriptor->has_binding &&
        descriptor->binding == 5 && descriptor->has_descriptor_set && descriptor->descriptor_set == 2 &&
        gpu_ir_pipeline_layout_find_binding(common_layout, 2, 5, &common_ubo) == GPU_IR_OK &&
        common_ubo.kind == GPU_IR_DESCRIPTOR_UNIFORM_BUFFER && common_ubo.stage_mask == GPU_IR_STAGE_COMPUTE;
    check("OpDecorationGroup and OpGroupDecorate lower descriptor set and binding metadata", common_layout_ok);
    gpu_ir_pipeline_layout_release(common_layout);
    spirv_free(&module);

    const uint32_t grouped_member[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 11, 0x6e69616du, 0,
        0x00040047u, 30, 35, 16,                // group Offset 16 (forward reference)
        0x00020049u, 30,
        0x0004004bu, 30, 4, 0,                  // OpGroupMemberDecorate group -> struct member 0
        0x00030047u, 4, 2,                       // Block
        0x00030016u, 1, 32,
        0x0003001eu, 4, 1,
        0x00040020u, 5, 9, 4,
        0x0004003bu, 5, 6, 9
    };
    uint8_t grouped_member_bytes[(5 + sizeof grouped_member / sizeof grouped_member[0]) * 4];
    make_module(grouped_member_bytes, 0, 0x00010600u, 31, 0, grouped_member,
                sizeof grouped_member / sizeof grouped_member[0]);
    rc = spirv_read(grouped_member_bytes, sizeof grouped_member_bytes, &module);
    compute_entry = rc == SPIRV_OK ? spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_layout = NULL;
    layout_rc = compute_entry ? spirv_build_gpu_ir_layout(&module, compute_entry, &common_layout) : GPU_IR_INVALID;
    gpu_ir_push_constant_range grouped_push_range = { 0 };
    int grouped_push_layout_ok = layout_rc == GPU_IR_OK && common_layout &&
        gpu_ir_pipeline_layout_push_constant_at(common_layout, 0, &grouped_push_range) == GPU_IR_OK &&
        grouped_push_range.offset == 16 && grouped_push_range.size == 4 &&
        grouped_push_range.stage_mask == GPU_IR_STAGE_COMPUTE;
    check("OpGroupMemberDecorate applies grouped member offsets to push-constant layout", grouped_push_layout_ok);
    gpu_ir_pipeline_layout_release(common_layout);
    spirv_free(&module);

    const uint32_t invalid_group_target[] = {
        0x00020049u, 4,
        0x00020049u, 5,
        0x0003004au, 4, 5
    };
    uint8_t invalid_group_target_bytes[(5 + sizeof invalid_group_target / sizeof invalid_group_target[0]) * 4];
    make_module(invalid_group_target_bytes, 0, 0x00010600u, 6, 0, invalid_group_target,
                sizeof invalid_group_target / sizeof invalid_group_target[0]);
    check("OpGroupDecorate rejects decoration-group targets", spirv_read(invalid_group_target_bytes,
          sizeof invalid_group_target_bytes, &module) == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t invalid_group_order[] = {
        0x00020049u, 4,
        0x00040047u, 4, 33, 1
    };
    uint8_t invalid_group_order_bytes[(5 + sizeof invalid_group_order / sizeof invalid_group_order[0]) * 4];
    make_module(invalid_group_order_bytes, 0, 0x00010600u, 5, 0, invalid_group_order,
                sizeof invalid_group_order / sizeof invalid_group_order[0]);
    check("decoration-group templates after OpDecorationGroup are rejected", spirv_read(invalid_group_order_bytes,
          sizeof invalid_group_order_bytes, &module) == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t duplicate_decoration[] = {
        0x00030047u, 4, 2,                       // Block
        0x00030047u, 4, 2,                       // duplicate Block
        0x0002001eu, 4
    };
    uint8_t duplicate_decoration_bytes[(5 + sizeof duplicate_decoration / sizeof duplicate_decoration[0]) * 4];
    make_module(duplicate_decoration_bytes, 0, 0x00010600u, 5, 0, duplicate_decoration,
                sizeof duplicate_decoration / sizeof duplicate_decoration[0]);
    check("duplicate tracked decorations are rejected", spirv_read(duplicate_decoration_bytes,
          sizeof duplicate_decoration_bytes, &module) == SPIRV_INVALID);
    spirv_free(&module);

    const uint32_t push_array[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 11, 0x6e69616du, 0,
        0x00040047u, 6, 6, 16,                  // ArrayStride 16
        0x00030047u, 7, 2,                      // Block
        0x00050048u, 7, 0, 35, 0,               // member 0 Offset 0
        0x00050048u, 7, 1, 35, 16,              // member 1 Offset 16
        0x00040015u, 1, 32, 0,                  // uint
        0x0004002bu, 1, 2, 2,                   // array length 2
        0x00030016u, 3, 32,                     // float
        0x00040017u, 4, 3, 4,                   // float4
        0x0004001cu, 6, 4, 2,                   // float4[2]
        0x0004001eu, 7, 1, 6,                   // block { uint, float4[2] }
        0x00040020u, 8, 9, 7,                   // PushConstant pointer to block
        0x0004003bu, 8, 9, 9                    // push-constant variable
    };
    uint8_t push_array_bytes[(5 + sizeof push_array / sizeof push_array[0]) * 4];
    make_module(push_array_bytes, 0, 0x00010600u, 12, 0, push_array,
                sizeof push_array / sizeof push_array[0]);
    rc = spirv_read(push_array_bytes, sizeof push_array_bytes, &module);
    compute_entry = rc == SPIRV_OK ? spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_layout = NULL;
    layout_rc = compute_entry ? spirv_build_gpu_ir_layout(&module, compute_entry, &common_layout) : GPU_IR_INVALID;
    gpu_ir_push_constant_range push_range = { 0 };
    int push_layout_ok = layout_rc == GPU_IR_OK && common_layout &&
        gpu_ir_pipeline_layout_binding_count(common_layout) == 0 &&
        gpu_ir_pipeline_layout_push_constant_count(common_layout) == 1 &&
        gpu_ir_pipeline_layout_push_constant_at(common_layout, 0, &push_range) == GPU_IR_OK &&
        push_range.offset == 0 && push_range.size == 48 && push_range.stage_mask == GPU_IR_STAGE_COMPUTE;
    check("SPIR-V push-constant scalar, array stride and member offsets lower to a common byte range", push_layout_ok);
    gpu_ir_pipeline_layout_release(common_layout);
    spirv_free(&module);

    const uint32_t push_matrix[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 7, 0x6e69616du, 0,
        0x00030047u, 4, 2,                       // Block
        0x00040048u, 4, 0, 4,                    // RowMajor
        0x00050048u, 4, 0, 7, 16,                // MatrixStride 16
        0x00050048u, 4, 0, 35, 0,                // Offset 0
        0x00030016u, 1, 32,                      // float
        0x00040017u, 2, 1, 3,                    // float3 column vector
        0x00040018u, 3, 2, 2,                    // two columns, three rows
        0x0003001eu, 4, 3,                       // block { row_major float3x2 }
        0x00040020u, 5, 9, 4,
        0x0004003bu, 5, 6, 9
    };
    uint8_t push_matrix_bytes[(5 + sizeof push_matrix / sizeof push_matrix[0]) * 4];
    make_module(push_matrix_bytes, 0, 0x00010600u, 8, 0, push_matrix,
                sizeof push_matrix / sizeof push_matrix[0]);
    rc = spirv_read(push_matrix_bytes, sizeof push_matrix_bytes, &module);
    compute_entry = rc == SPIRV_OK ? spirv_find_entry_point(&module, "main", SPIRV_EXECUTION_MODEL_GL_COMPUTE) : NULL;
    common_layout = NULL;
    layout_rc = compute_entry ? spirv_build_gpu_ir_layout(&module, compute_entry, &common_layout) : GPU_IR_INVALID;
    push_range = (gpu_ir_push_constant_range){ 0 };
    push_layout_ok = layout_rc == GPU_IR_OK && common_layout &&
        gpu_ir_pipeline_layout_push_constant_count(common_layout) == 1 &&
        gpu_ir_pipeline_layout_push_constant_at(common_layout, 0, &push_range) == GPU_IR_OK &&
        push_range.offset == 0 && push_range.size == 40 && push_range.stage_mask == GPU_IR_STAGE_COMPUTE;
    check("SPIR-V row-major matrix stride lowers to its occupied push-constant extent", push_layout_ok);
    gpu_ir_pipeline_layout_release(common_layout);
    spirv_free(&module);

    const uint32_t bad_variable_storage[] = {
        0x00040015u, 3, 32, 0,                 // unsigned int
        0x00040020u, 1, 4, 3, // pointer storage class Workgroup
        0x0004003bu, 1, 4, 2  // variable claims Uniform
    };
    uint8_t bad_variable_bytes[(5 + sizeof bad_variable_storage / sizeof bad_variable_storage[0]) * 4];
    make_module(bad_variable_bytes, 0, 0x00010600u, 5, 0, bad_variable_storage,
                sizeof bad_variable_storage / sizeof bad_variable_storage[0]);
    check("variable storage class mismatch rejected",
          spirv_read(bad_variable_bytes, sizeof bad_variable_bytes, &module) == SPIRV_INVALID);

    const uint32_t bad_entry_name[] = { 0x0004000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 1, 0x41414141u };
    make_module(compute_bytes, 0, 0x00010600u, 2, 0, bad_entry_name,
                sizeof bad_entry_name / sizeof bad_entry_name[0]);
    check("unterminated entry point name rejected",
          spirv_read(compute_bytes, (5 + sizeof bad_entry_name / sizeof bad_entry_name[0]) * 4, &module) == SPIRV_INVALID);

    const uint32_t zero_local_size[] = {
        0x0005000fu, SPIRV_EXECUTION_MODEL_GL_COMPUTE, 1, 0x6e69616du, 0,
        0x00060010u, 1, 17, 8, 0, 2
    };
    make_module(compute_bytes, 0, 0x00010600u, 2, 0, zero_local_size,
                sizeof zero_local_size / sizeof zero_local_size[0]);
    check("zero local-size dimension rejected",
          spirv_read(compute_bytes, (5 + sizeof zero_local_size / sizeof zero_local_size[0]) * 4, &module) == SPIRV_INVALID);

    make_module(bytes, 1, 0x00010300u, 1, 0, NULL, 0);
    rc = spirv_read(bytes, 5 * 4, &module);
    check("big-endian word stream", rc == SPIRV_OK && module.version == 0x00010300u && module.word_count == 5);
    spirv_free(&module);

    uint8_t short_header[19] = { 0 };
    uint8_t invalid_magic[20] = { 0 };
    check("short and partial-word inputs rejected", spirv_read(short_header, sizeof short_header, &module) == SPIRV_INVALID &&
          spirv_read(bytes, 21, &module) == SPIRV_INVALID);
    check("invalid magic rejected", spirv_read(invalid_magic, sizeof invalid_magic, &module) == SPIRV_INVALID);

    make_module(bytes, 0, 0x00010700u, 1, 0, NULL, 0);
    check("future version rejected explicitly", spirv_read(bytes, 20, &module) == SPIRV_UNSUPPORTED_VERSION);
    make_module(bytes, 0, 0x00010301u, 1, 0, NULL, 0);
    check("reserved version bits rejected", spirv_read(bytes, 20, &module) == SPIRV_UNSUPPORTED_VERSION);
    make_module(bytes, 0, 0x00010000u, 0, 0, NULL, 0);
    check("zero id bound rejected", spirv_read(bytes, 20, &module) == SPIRV_INVALID);
    make_module(bytes, 0, 0x00010000u, 1, 1, NULL, 0);
    check("nonzero reserved schema rejected", spirv_read(bytes, 20, &module) == SPIRV_INVALID);

    const uint32_t zero_count[] = { 0x00000001u };
    make_module(bytes, 0, 0x00010000u, 1, 0, zero_count, 1);
    check("zero instruction word count rejected", spirv_read(bytes, 24, &module) == SPIRV_INVALID);
    const uint32_t truncated[] = { 0x0003000eu, 0x00000000u };
    make_module(bytes, 0, 0x00010000u, 1, 0, truncated, 2);
    check("truncated instruction rejected", spirv_read(bytes, 28, &module) == SPIRV_INVALID);

    printf("spirv: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
