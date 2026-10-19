#include "dxil_ir.h"
#include "gpu_ir_msl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(const char *condition) {
    fprintf(stderr, "dxil_ir: %s\n", condition);
    return 1;
}

static void release_sentinel(void *object) { (void)object; }

static int translate_core_compute(const char *output_path) {
    dxil_type types[5] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_INT, .bits = 1 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FLOAT }
    };
    dxil_value values[7] = {
        { .kind = DXV_FUNCTION, .type = 3 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = -12 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 7 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 8 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 }
    };
    dxil_value locals[11] = {
        { .kind = DXV_INST, .type = 1, .u.index = 0 },
        { .kind = DXV_INST, .type = 2, .u.index = 1 },
        { .kind = DXV_CONST_FLOAT, .type = 4, .u.f = 6.0 },
        { .kind = DXV_CONST_FLOAT, .type = 4, .u.f = 2.0 },
        { .kind = DXV_INST, .type = 4, .u.index = 2 },
        { .kind = DXV_INST, .type = 1, .u.index = 3 },
        { .kind = DXV_INST, .type = 2, .u.index = 4 },
        { .kind = DXV_INST, .type = 1, .u.index = 5 },
        { .kind = DXV_INST, .type = 1, .u.index = 6 },
        { .kind = DXV_INST, .type = 4, .u.index = 7 },
        { .kind = DXV_INST, .type = 1, .u.index = 8 }
    };
    uint32_t add_operands[] = { 1, 2 };
    uint32_t compare_operands[] = { 7, 1 };
    uint32_t divide_operands[] = { 9, 10 };
    uint32_t signed_operands[] = { 1, 2 };
    uint32_t signed_to_float_operands[] = { 1 };
    uint32_t float_to_signed_operands[] = { 9 };
    dxil_inst instructions[10] = {
        { .op = DXI_BINOP, .sub = 0, .type = 1, .result = 7, .block = 0, .n = 2,
          .ops = add_operands },
        { .op = DXI_CMP, .sub = 36, .type = 2, .result = 8, .block = 0, .n = 2,
          .ops = compare_operands },
        { .op = DXI_BINOP, .sub = 3, .type = 4, .result = 11, .block = 0, .n = 2,
          .ops = divide_operands },
        { .op = DXI_BINOP, .sub = 4, .type = 1, .result = 12, .block = 0, .n = 2,
          .ops = signed_operands },
        { .op = DXI_CMP, .sub = 38, .type = 2, .result = 13, .block = 0, .n = 2,
          .ops = signed_operands },
        { .op = DXI_BINOP, .sub = 6, .type = 1, .result = 14, .block = 0, .n = 2,
          .ops = signed_operands },
        { .op = DXI_BINOP, .sub = 9, .type = 1, .result = 15, .block = 0, .n = 2,
          .ops = signed_operands },
        { .op = DXI_CAST, .sub = 6, .type = 4, .result = 16, .block = 0, .n = 1,
          .ops = signed_to_float_operands },
        { .op = DXI_CAST, .sub = 4, .type = 1, .result = 17, .block = 0, .n = 1,
          .ops = float_to_signed_operands },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function function = { .type = 3, .value = 0, .nblocks = 1, .ninsts = 10,
        .insts = instructions, .nlocals = 11, .locals = locals };
    dxil_md metadata[8] = { 0 };
    uint32_t dimensions[] = { 3, 4, 5 };
    uint32_t properties[] = { 2, 6 };
    uint32_t entry_ops[] = { 1, 0, 0, 0, 7 };
    uint32_t entry_list[] = { 7 };
    metadata[0] = (dxil_md){ .kind = DXM_VALUE, .value = 0 };
    metadata[1] = (dxil_md){ .kind = DXM_VALUE, .value = 3 };
    metadata[2] = (dxil_md){ .kind = DXM_VALUE, .value = 4 };
    metadata[3] = (dxil_md){ .kind = DXM_VALUE, .value = 5 };
    metadata[4] = (dxil_md){ .kind = DXM_VALUE, .value = 6 };
    metadata[5] = (dxil_md){ .kind = DXM_NODE, .n = 3, .ops = dimensions };
    metadata[6] = (dxil_md){ .kind = DXM_NODE, .n = 2, .ops = properties };
    metadata[7] = (dxil_md){ .kind = DXM_NODE, .n = 5, .ops = entry_ops };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_list };
    dxil_module module = { .shader_kind = 5, .ntypes = 5, .types = types,
        .nmodule_values = 7, .values = values, .nfunctions = 1, .functions = &function,
        .nmd = 8, .md = metadata, .nnamed = 1, .named = &named };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        fprintf(stderr, "dxil_ir: lowering returned %d\n", rc);
        return check("core DXIL compute shader did not lower");
    }
    if (gpu_ir_shader_source_api(shader) != GPU_IR_SHADER_SOURCE_DXIL ||
        gpu_ir_shader_stage(shader) != GPU_IR_STAGE_COMPUTE ||
        gpu_ir_shader_function_count(shader) != 1 || gpu_ir_shader_block_count(shader) != 1) {
        gpu_ir_shader_release(shader);
        return check("lowered shader lost its stage or function structure");
    }
    uint32_t group_size[3] = { 0, 0, 0 };
    gpu_ir_shader_threadgroup_size(shader, group_size);
    if (group_size[0] != 8 || group_size[1] != 1 || group_size[2] != 1) {
        gpu_ir_shader_release(shader);
        return check("DXIL NumThreads metadata was not preserved");
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    if (rc != GPU_IR_OK || !msl || !msl_size) {
        gpu_ir_shader_release(shader);
        free(msl);
        return check("common MSL backend rejected the lowered DXIL slice");
    }
    int failed = !strstr(msl, " + ") || !strstr(msl, " < ") || !strstr(msl, " / ") ||
        !strstr(msl, "as_type<uint>(as_type<int>(uint(0xfffffff4u)) / as_type<int>(uint(0x00000007u)))") ||
        !strstr(msl, "(as_type<int>(uint(0xfffffff4u)) > as_type<int>(uint(0x00000007u)))") ||
        !strstr(msl, "as_type<uint>(as_type<int>(uint(0xfffffff4u)) % as_type<int>(uint(0x00000007u)))") ||
        !strstr(msl, "as_type<uint>(as_type<int>(uint(0xfffffff4u)) >> as_type<int>(uint(0x00000007u)))") ||
        !strstr(msl, "float(as_type<int>(uint(0xfffffff4u)))") ||
        !strstr(msl, "as_type<uint>(int(as_type<float>(0x40c00000u)))");
    if (!failed && output_path) {
        FILE *output = fopen(output_path, "wb");
        failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
    }
    free(msl);
    if (failed) {
        gpu_ir_shader_release(shader);
        return check("common MSL omitted core arithmetic or comparison");
    }
    gpu_ir_pipeline_desc pipeline_desc = {
        .info = { .kind = GPU_IR_PIPELINE_COMPUTE, .stage_mask = GPU_IR_STAGE_COMPUTE,
            .threadgroup_size = { 8, 1, 1 } },
        .backend_object = (void *)1,
        .release = release_sentinel,
        .stage_shaders = { [GPU_IR_STAGE_SLOT_COMPUTE] = shader }
    };
    gpu_ir_pipeline *pipeline = gpu_ir_pipeline_create(&pipeline_desc);
    gpu_ir_shader_release(shader);
    if (!pipeline) return check("common compute pipeline did not retain the DXIL shader");
    const gpu_ir_shader *retained = gpu_ir_pipeline_shader(pipeline, GPU_IR_STAGE_COMPUTE);
    failed = retained == NULL || gpu_ir_shader_source_api(retained) != GPU_IR_SHADER_SOURCE_DXIL;
    gpu_ir_pipeline_release(pipeline);
    return failed ? check("common pipeline did not expose its retained DXIL shader") : 0;
}

static int translate_bit_intrinsics(const char *output_path) {
    uint32_t unary_arguments[] = { 1, 1 };
    uint32_t ternary_arguments[] = { 1, 1, 1, 1 };
    uint32_t quaternary_arguments[] = { 1, 1, 1, 1, 1 };
    dxil_type types[6] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = 1, .count = 2, .members = unary_arguments },
        { .kind = DXT_FUNCTION, .elem = 1, .count = 4, .members = ternary_arguments },
        { .kind = DXT_FUNCTION, .elem = 1, .count = 5, .members = quaternary_arguments }
    };
    dxil_value values[18] = {
        { .kind = DXV_FUNCTION, .type = 2, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 3, .name = "dx.op.unary.i32" },
        { .kind = DXV_FUNCTION, .type = 4, .name = "dx.op.ternary.i32" },
        { .kind = DXV_FUNCTION, .type = 5, .name = "dx.op.quaternary.i32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 30 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 31 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 32 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 33 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 34 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 51 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 52 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 53 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0x12345678 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0x3a },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 8 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 8 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 }
    };
    dxil_value locals[8] = {
        { .kind = DXV_INST, .type = 1, .u.index = 0 },
        { .kind = DXV_INST, .type = 1, .u.index = 1 },
        { .kind = DXV_INST, .type = 1, .u.index = 2 },
        { .kind = DXV_INST, .type = 1, .u.index = 3 },
        { .kind = DXV_INST, .type = 1, .u.index = 4 },
        { .kind = DXV_INST, .type = 1, .u.index = 5 },
        { .kind = DXV_INST, .type = 1, .u.index = 6 },
        { .kind = DXV_INST, .type = 1, .u.index = 7 }
    };
    uint32_t reverse_arguments[] = { 1, 4, 12 };
    uint32_t count_arguments[] = { 1, 5, 12 };
    uint32_t first_lo_arguments[] = { 1, 6, 12 };
    uint32_t first_hi_arguments[] = { 1, 7, 12 };
    uint32_t first_signed_hi_arguments[] = { 1, 8, 12 };
    uint32_t signed_extract_arguments[] = { 2, 9, 15, 14, 12 };
    uint32_t unsigned_extract_arguments[] = { 2, 10, 15, 14, 12 };
    uint32_t insert_arguments[] = { 3, 11, 15, 14, 13, 12 };
    dxil_inst instructions[9] = {
        { .op = DXI_CALL, .type = 1, .result = 18, .block = 0, .n = 3, .ops = reverse_arguments },
        { .op = DXI_CALL, .type = 1, .result = 19, .block = 0, .n = 3, .ops = count_arguments },
        { .op = DXI_CALL, .type = 1, .result = 20, .block = 0, .n = 3, .ops = first_lo_arguments },
        { .op = DXI_CALL, .type = 1, .result = 21, .block = 0, .n = 3, .ops = first_hi_arguments },
        { .op = DXI_CALL, .type = 1, .result = 22, .block = 0, .n = 3, .ops = first_signed_hi_arguments },
        { .op = DXI_CALL, .type = 1, .result = 23, .block = 0, .n = 5, .ops = signed_extract_arguments },
        { .op = DXI_CALL, .type = 1, .result = 24, .block = 0, .n = 5, .ops = unsigned_extract_arguments },
        { .op = DXI_CALL, .type = 1, .result = 25, .block = 0, .n = 6, .ops = insert_arguments },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function function = { .type = 2, .value = 0, .nblocks = 1, .ninsts = 9,
        .insts = instructions, .nlocals = 8, .locals = locals };
    dxil_md metadata[8] = { 0 };
    uint32_t dimensions[] = { 3, 4, 5 };
    uint32_t properties[] = { 2, 6 };
    uint32_t entry_ops[] = { 1, 0, 0, 0, 7 };
    uint32_t entry_list[] = { 7 };
    metadata[0] = (dxil_md){ .kind = DXM_VALUE, .value = 0 };
    metadata[1] = (dxil_md){ .kind = DXM_VALUE, .value = 16 };
    metadata[2] = (dxil_md){ .kind = DXM_VALUE, .value = 14 };
    metadata[3] = (dxil_md){ .kind = DXM_VALUE, .value = 17 };
    metadata[4] = (dxil_md){ .kind = DXM_VALUE, .value = 17 };
    metadata[5] = (dxil_md){ .kind = DXM_NODE, .n = 3, .ops = dimensions };
    metadata[6] = (dxil_md){ .kind = DXM_NODE, .n = 2, .ops = properties };
    metadata[7] = (dxil_md){ .kind = DXM_NODE, .n = 5, .ops = entry_ops };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_list };
    dxil_module module = { .shader_kind = 5, .ntypes = 6, .types = types,
        .nmodule_values = 18, .values = values, .nfunctions = 1, .functions = &function,
        .nmd = 8, .md = metadata, .nnamed = 1, .named = &named };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: bit-intrinsic lowering returned %d\n", rc);
        return check("DXIL bit intrinsics did not lower to common IR");
    }
    uint32_t expected_operations[] = {
        GPU_IR_SHADER_ARITH_BIT_REVERSE,
        GPU_IR_SHADER_ARITH_BIT_COUNT,
        GPU_IR_SHADER_ARITH_BIT_FIND_LSB,
        GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED,
        GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED,
        GPU_IR_SHADER_ARITH_BIT_FIELD_SIGNED_EXTRACT,
        GPU_IR_SHADER_ARITH_BIT_FIELD_UNSIGNED_EXTRACT,
        GPU_IR_SHADER_ARITH_BIT_FIELD_INSERT
    };
    uint32_t expected_operand_counts[] = { 1, 1, 1, 1, 1, 3, 3, 4 };
    uint32_t seen = 0;
    int valid = 1;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC) continue;
        if (seen >= 8 || instruction->operation != expected_operations[seen] ||
            instruction->operand_count != expected_operand_counts[seen] ||
            instruction->flags != (seen >= 5 ? GPU_IR_SHADER_FLAG_DXIL_BITFIELD_LOW5 : 0)) {
            valid = 0;
            break;
        }
        seen++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    if (valid && seen == 8) rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    else rc = GPU_IR_INVALID;
    gpu_ir_shader_release(shader);
    valid = valid && rc == GPU_IR_OK && msl && msl_size &&
        strstr(msl, "reverse_bits(uint(0x12345678u))") &&
        strstr(msl, "popcount(uint(0x12345678u))") &&
        strstr(msl, "uint vx_find_result_27_0 = vx_find_bits_27_0 == 0u ? 0xffffffffu : "
                    "uint(ctz(vx_find_bits_27_0));") &&
        strstr(msl, "uint vx_find_result_28_0 = vx_find_bits_28_0 == 0u ? 0xffffffffu : "
                    "31u - uint(clz(vx_find_bits_28_0));") &&
        strstr(msl, "int vx_find_signed_29_0 = as_type<int>(uint(0x12345678u));") &&
        strstr(msl, "uint vx_find_result_29_0 = vx_find_bits_29_0 == 0u ? 0xffffffffu : "
                    "31u - uint(clz(vx_find_bits_29_0));") &&
        strstr(msl, "extract_bits(as_type<int>(uint(0x12345678u)), (uint(0x00000008u) & 31u), (uint(0x00000008u) & 31u))") &&
        strstr(msl, "extract_bits(uint(0x12345678u), (uint(0x00000008u) & 31u), (uint(0x00000008u) & 31u))") &&
        strstr(msl, "insert_bits(uint(0x12345678u), uint(0x0000003au), (uint(0x00000008u) & 31u), (uint(0x00000008u) & 31u))");
    if (valid && output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) valid = 0;
    }
    free(msl);
    return valid ? 0 : check("DXIL bit intrinsics lost operand order or emitted invalid common MSL");
}

static int translate_compute_system_values(const char *output_path) {
    uint32_t vector_parameters[] = { 1, 1 };
    uint32_t scalar_parameters[] = { 1 };
    dxil_type types[5] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = 1, .count = 2, .members = vector_parameters },
        { .kind = DXT_FUNCTION, .elem = 1, .count = 1, .members = scalar_parameters }
    };
    dxil_value values[15] = {
        { .kind = DXV_FUNCTION, .type = 2, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 3, .name = "dx.op.threadId.i32" },
        { .kind = DXV_FUNCTION, .type = 3, .name = "dx.op.groupId.i32" },
        { .kind = DXV_FUNCTION, .type = 3, .name = "dx.op.threadIdInGroup.i32" },
        { .kind = DXV_FUNCTION, .type = 4, .name = "dx.op.flattenedThreadIdInGroup.i32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 93 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 94 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 95 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 96 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 2 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 8 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 }
    };
    dxil_value locals[4] = {
        { .kind = DXV_INST, .type = 1, .u.index = 0 },
        { .kind = DXV_INST, .type = 1, .u.index = 1 },
        { .kind = DXV_INST, .type = 1, .u.index = 2 },
        { .kind = DXV_INST, .type = 1, .u.index = 3 }
    };
    uint32_t thread_id_args[] = { 1, 5, 9 };
    uint32_t group_id_args[] = { 2, 6, 10 };
    uint32_t local_id_args[] = { 3, 7, 11 };
    uint32_t local_index_args[] = { 4, 8 };
    dxil_inst instructions[5] = {
        { .op = DXI_CALL, .type = 1, .result = 15, .block = 0, .n = 3, .ops = thread_id_args },
        { .op = DXI_CALL, .type = 1, .result = 16, .block = 0, .n = 3, .ops = group_id_args },
        { .op = DXI_CALL, .type = 1, .result = 17, .block = 0, .n = 3, .ops = local_id_args },
        { .op = DXI_CALL, .type = 1, .result = 18, .block = 0, .n = 2, .ops = local_index_args },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function function = { .type = 2, .value = 0, .nblocks = 1, .ninsts = 5,
        .insts = instructions, .nlocals = 4, .locals = locals };
    uint32_t dimensions[] = { 3, 4, 5 };
    uint32_t properties[] = { 2, 6 };
    uint32_t entry_ops[] = { 1, 0, 0, 0, 7 };
    uint32_t entry_list[] = { 7 };
    dxil_md metadata[8] = {
        { .kind = DXM_VALUE, .value = 0 },
        { .kind = DXM_VALUE, .value = 14 },
        { .kind = DXM_VALUE, .value = 12 },
        { .kind = DXM_VALUE, .value = 13 },
        { .kind = DXM_VALUE, .value = 13 },
        { .kind = DXM_NODE, .n = 3, .ops = dimensions },
        { .kind = DXM_NODE, .n = 2, .ops = properties },
        { .kind = DXM_NODE, .n = 5, .ops = entry_ops }
    };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_list };
    dxil_module module = { .shader_kind = 5, .ntypes = 5, .types = types,
        .nmodule_values = 15, .values = values, .nfunctions = 1, .functions = &function,
        .nmd = 8, .md = metadata, .nnamed = 1, .named = &named };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        fprintf(stderr, "dxil_ir: system-value lowering returned %d\n", rc);
        return check("DXIL compute system values did not lower");
    }
    uint32_t seen = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->opcode == GPU_IR_SHADER_OP_VARIABLE && instruction->has_builtin) {
            if (instruction->source_builtin == 93) seen |= 1u << 0;
            else if (instruction->source_builtin == 94) seen |= 1u << 1;
            else if (instruction->source_builtin == 95) seen |= 1u << 2;
            else if (instruction->source_builtin == 96) seen |= 1u << 3;
        }
    }
    if (seen != 0x0fu) {
        gpu_ir_shader_release(shader);
        return check("DXIL compute system-value built-ins were not retained in common IR");
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK || !msl || !msl_size ||
        !strstr(msl, "[[thread_position_in_grid]]") ||
        !strstr(msl, "[[threadgroup_position_in_grid]]") ||
        !strstr(msl, "[[thread_position_in_threadgroup]]") ||
        !strstr(msl, "[[thread_index_in_threadgroup]]") ||
        !strstr(msl, ".x;") || !strstr(msl, ".y;") || !strstr(msl, ".z;")) {
        free(msl);
        return check("common MSL did not emit DXIL compute system values");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) {
            free(msl);
            return check("could not write the DXIL system-value MSL fixture");
        }
    }
    free(msl);
    return 0;
}

static int reject_unsupported_intrinsic(void) {
    dxil_type types[2] = { { .kind = DXT_VOID }, { .kind = DXT_FUNCTION, .elem = 0 } };
    dxil_value values[1] = { { .kind = DXV_FUNCTION, .type = 1 } };
    dxil_inst instructions[2] = {
        { .op = DXI_CALL, .type = 0, .result = UINT32_MAX, .block = 0 },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function function = { .type = 1, .value = 0, .nblocks = 1, .ninsts = 2, .insts = instructions };
    dxil_module module = { .shader_kind = 1, .ntypes = 2, .types = types,
        .nmodule_values = 1, .values = values, .nfunctions = 1, .functions = &function };
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (shader) gpu_ir_shader_release(shader);
    return rc == GPU_IR_UNSUPPORTED && !shader ? 0 : check("unsupported DXIL call was not rejected conservatively");
}

static int translate_cfg_and_phi(void) {
    dxil_type types[4] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_INT, .bits = 1 },
        { .kind = DXT_FUNCTION, .elem = 0 }
    };
    dxil_value values[7] = {
        { .kind = DXV_FUNCTION, .type = 3 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 16 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 8 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 }
    };
    dxil_value locals[3] = {
        { .kind = DXV_INST, .type = 1, .u.index = 1 },
        { .kind = DXV_INST, .type = 2, .u.index = 2 },
        { .kind = DXV_INST, .type = 1, .u.index = 4 }
    };
    uint32_t phi_operands[] = { 1, 0, 9, 2 };
    uint32_t compare_operands[] = { 7, 2 };
    uint32_t branch_conditional[] = { 2, 3, 8 };
    uint32_t add_operands[] = { 7, 1 };
    uint32_t branch_entry[] = { 1 };
    uint32_t branch_loop[] = { 1 };
    dxil_inst instructions[7] = {
        { .op = DXI_BR, .type = 0, .result = UINT32_MAX, .block = 0,
          .n = 1, .ops = branch_entry },
        { .op = DXI_PHI, .type = 1, .result = 7, .block = 1,
          .n = 4, .ops = phi_operands },
        { .op = DXI_CMP, .sub = 36, .type = 2, .result = 8, .block = 1,
          .n = 2, .ops = compare_operands },
        { .op = DXI_BR, .type = 0, .result = UINT32_MAX, .block = 1,
          .n = 3, .ops = branch_conditional },
        { .op = DXI_BINOP, .sub = 0, .type = 1, .result = 9, .block = 2,
          .n = 2, .ops = add_operands },
        { .op = DXI_BR, .type = 0, .result = UINT32_MAX, .block = 2,
          .n = 1, .ops = branch_loop },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 3 }
    };
    dxil_function function = { .type = 3, .value = 0, .nblocks = 4, .ninsts = 7,
        .insts = instructions, .nlocals = 3, .locals = locals };
    dxil_md metadata[8] = { 0 };
    uint32_t dimensions[] = { 3, 4, 5 };
    uint32_t properties[] = { 2, 6 };
    uint32_t entry_ops[] = { 1, 0, 0, 0, 7 };
    uint32_t entry_list[] = { 7 };
    metadata[0] = (dxil_md){ .kind = DXM_VALUE, .value = 0 };
    metadata[1] = (dxil_md){ .kind = DXM_VALUE, .value = 3 };
    metadata[2] = (dxil_md){ .kind = DXM_VALUE, .value = 4 };
    metadata[3] = (dxil_md){ .kind = DXM_VALUE, .value = 5 };
    metadata[4] = (dxil_md){ .kind = DXM_VALUE, .value = 6 };
    metadata[5] = (dxil_md){ .kind = DXM_NODE, .n = 3, .ops = dimensions };
    metadata[6] = (dxil_md){ .kind = DXM_NODE, .n = 2, .ops = properties };
    metadata[7] = (dxil_md){ .kind = DXM_NODE, .n = 5, .ops = entry_ops };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_list };
    dxil_module module = { .shader_kind = 5, .ntypes = 4, .types = types,
        .nmodule_values = 7, .values = values, .nfunctions = 1, .functions = &function,
        .nmd = 8, .md = metadata, .nnamed = 1, .named = &named };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    if (rc != GPU_IR_OK || !shader) return check("DXIL CFG/phi shader did not lower");
    char *msl = NULL;
    size_t size = 0;
    rc = gpu_ir_msl_emit_compute(shader, &msl, &size);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK || !msl || !size || !strstr(msl, "switch (vx_block)") ||
        !strstr(msl, "vx_phi_in_")) {
        free(msl);
        return check("DXIL branch/phi lowering did not reach the common CFG emitter");
    }
    free(msl);
    return 0;
}

static int translate_vector_compare_and_select(void) {
    uint32_t function_parameters[] = { 3, 3, 5 };
    dxil_type types[8] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 1 },
        { .kind = DXT_FLOAT },
        { .kind = DXT_VECTOR, .elem = 2, .count = 2 },
        { .kind = DXT_VECTOR, .elem = 1, .count = 2 },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_VECTOR, .elem = 5, .count = 2 },
        { .kind = DXT_FUNCTION, .elem = 0, .count = 3, .members = function_parameters }
    };
    dxil_value values[1] = { { .kind = DXV_FUNCTION, .type = 7 } };
    dxil_value locals[11] = {
        { .kind = DXV_ARG, .type = 3, .u.index = 0 },
        { .kind = DXV_ARG, .type = 3, .u.index = 1 },
        { .kind = DXV_ARG, .type = 5, .u.index = 2 },
        { .kind = DXV_INST, .type = 4, .u.index = 0 },
        { .kind = DXV_INST, .type = 3, .u.index = 1 },
        { .kind = DXV_INST, .type = 6, .u.index = 2 },
        { .kind = DXV_INST, .type = 3, .u.index = 3 },
        { .kind = DXV_INST, .type = 2, .u.index = 4 },
        { .kind = DXV_INST, .type = 3, .u.index = 5 },
        { .kind = DXV_INST, .type = 3, .u.index = 6 },
        { .kind = DXV_INST, .type = 2, .u.index = 7 }
    };
    uint32_t compare_operands[] = { 1, 2 };
    uint32_t select_operands[] = { 4, 1, 2 };
    uint32_t float_to_signed_operands[] = { 1 };
    uint32_t signed_to_float_operands[] = { 6 };
    uint32_t extract_element_operands[] = { 5, 3 };
    uint32_t insert_element_operands[] = { 5, 8, 3 };
    uint32_t insert_value_operands[] = { 5, 8, 0 };
    uint32_t extract_value_operands[] = { 10, 0 };
    dxil_inst instructions[9] = {
        { .op = DXI_CMP, .sub = 1, .type = 4, .result = 4, .block = 0,
          .n = 2, .ops = compare_operands },
        { .op = DXI_SELECT, .type = 3, .result = 5, .block = 0,
          .n = 3, .ops = select_operands },
        { .op = DXI_CAST, .sub = 4, .type = 6, .result = 6, .block = 0,
          .n = 1, .ops = float_to_signed_operands },
        { .op = DXI_CAST, .sub = 6, .type = 3, .result = 7, .block = 0,
          .n = 1, .ops = signed_to_float_operands },
        { .op = DXI_EXTRACTELT, .type = 2, .result = 8, .block = 0,
          .n = 2, .ops = extract_element_operands },
        { .op = DXI_INSERTELT, .type = 3, .result = 9, .block = 0,
          .n = 3, .ops = insert_element_operands },
        { .op = DXI_INSERTVAL, .type = 3, .result = 10, .block = 0,
          .n = 3, .nvals = 2, .ops = insert_value_operands },
        { .op = DXI_EXTRACTVAL, .type = 2, .result = 11, .block = 0,
          .n = 2, .nvals = 1, .ops = extract_value_operands },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function function = { .type = 7, .value = 0, .first_arg = 1, .nblocks = 1,
        .ninsts = 9, .insts = instructions, .nlocals = 11, .locals = locals };
    dxil_module module = { .shader_kind = 1, .ntypes = 8, .types = types,
        .nmodule_values = 1, .values = values, .nfunctions = 1, .functions = &function };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        if (shader) gpu_ir_shader_release(shader);
        return check("DXIL vector compare/select did not lower into common IR");
    }
    int found_compare = 0, found_select = 0, found_dynamic_extract = 0;
    int found_dynamic_insert = 0, found_static_extract = 0, found_static_insert = 0;
    int signed_conversions = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction->opcode == GPU_IR_SHADER_OP_COMPARE) {
            const gpu_ir_shader_type *result = gpu_ir_shader_find_type(shader, instruction->result_type_id);
            const gpu_ir_shader_type *element = result ? gpu_ir_shader_find_type(shader, result->element_type_id) : NULL;
            found_compare = result && result->kind == GPU_IR_SHADER_TYPE_VECTOR && result->element_count == 2 &&
                element && element->kind == GPU_IR_SHADER_TYPE_BOOL;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_SELECT) {
            found_select = instruction->operand_count == 3 && instruction->result_type_id != 0;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
                   instruction->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_EXTRACT_DYNAMIC) {
            found_dynamic_extract = instruction->operand_count == 2 && instruction->result_type_id != 0;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
                   instruction->operation == GPU_IR_SHADER_COMPOSITE_VECTOR_INSERT_DYNAMIC) {
            found_dynamic_insert = instruction->operand_count == 3 && instruction->result_type_id != 0;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
                   instruction->operation == GPU_IR_SHADER_COMPOSITE_EXTRACT) {
            found_static_extract = instruction->operand_count == 1 && instruction->literal_word_count == 1 &&
                gpu_ir_shader_literal_word_at(shader, instruction->first_literal) == 0;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
                   instruction->operation == GPU_IR_SHADER_COMPOSITE_INSERT) {
            found_static_insert = instruction->operand_count == 2 && instruction->literal_word_count == 1 &&
                gpu_ir_shader_literal_word_at(shader, instruction->first_literal) == 0;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_CONVERT &&
                   (instruction->operation == GPU_IR_SHADER_CONVERT_FLOAT_TO_SIGNED ||
                    instruction->operation == GPU_IR_SHADER_CONVERT_SIGNED_TO_FLOAT)) {
            const gpu_ir_shader_type *result = gpu_ir_shader_find_type(shader, instruction->result_type_id);
            signed_conversions += result && result->kind == GPU_IR_SHADER_TYPE_VECTOR && result->element_count == 2;
        }
    }
    gpu_ir_shader_release(shader);
    if (!found_compare || !found_select || !found_dynamic_extract || !found_dynamic_insert ||
        !found_static_extract || !found_static_insert || signed_conversions != 2)
        return check("common IR lost vector comparison/selection or vector extraction/insertion shape");

    instructions[4].type = 3;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (shader) gpu_ir_shader_release(shader);
    instructions[4].type = 2;
    if (rc != GPU_IR_INVALID) return check("DXIL extractelement accepted a vector-typed result");
    insert_element_operands[1] = 5;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (shader) gpu_ir_shader_release(shader);
    insert_element_operands[1] = 8;
    if (rc != GPU_IR_INVALID) return check("DXIL insertelement accepted a vector as its scalar element");
    insert_value_operands[1] = 5;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (shader) gpu_ir_shader_release(shader);
    return rc == GPU_IR_INVALID ? 0 : check("DXIL insertvalue accepted a vector as its scalar element");
}

static int read_dxil_fixture(const char *path, dxil_module *module) {
    FILE *input = path ? fopen(path, "rb") : NULL;
    if (!input) return check("could not open the DXIL fixture");
    if (fseek(input, 0, SEEK_END)) { fclose(input); return check("could not seek the DXIL fixture"); }
    long length = ftell(input);
    if (length <= 0 || fseek(input, 0, SEEK_SET)) { fclose(input); return check("DXIL fixture is empty"); }
    uint8_t *bytes = malloc((size_t)length);
    if (!bytes) { fclose(input); return check("could not allocate the DXIL fixture buffer"); }
    int failed = fread(bytes, 1, (size_t)length, input) != (size_t)length;
    if (fclose(input)) failed = 1;
    if (failed) { free(bytes); return check("could not read the DXIL fixture"); }

    if (dxil_read(bytes, (size_t)length, module)) {
        free(bytes);
        fprintf(stderr, "dxil_ir: fixture parser: %s\n", module->error);
        return check("could not parse the DXIL fixture");
    }
    free(bytes);
    return 0;
}

static int read_typed_buffer_fixture(dxil_module *module) {
    return read_dxil_fixture("tests/shaders12/typed_buffer_cs.dxil", module);
}

static int read_typed_sint_buffer_fixture(dxil_module *module) {
    return read_dxil_fixture("tests/shaders12/typed_sint_buffer_cs.dxil", module);
}

static int set_typed_resource_component(dxil_module *module, uint32_t resource_class, uint32_t component_type);

static int translate_typed_buffer_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_typed_buffer_fixture(&module)) return 1;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: typed-buffer lowering returned %d\n", rc);
        return check("typed R32_UINT buffer DXIL did not lower to common IR");
    }
    uint32_t srv_count = 0, uav_count = 0, access_count = 0, load_count = 0, store_count = 0, length_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("common shader lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_BUFFER) {
            if (instruction->binding_namespace == 0 && instruction->decoration_flags == GPU_IR_SHADER_TYPE_FLAG_NON_WRITABLE)
                srv_count++;
            else if (instruction->binding_namespace == 1 && instruction->decoration_flags == 0)
                uav_count++;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_ACCESS_CHAIN) access_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_LOAD) load_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_STORE) store_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_ARRAY_LENGTH) length_count++;
    }
    if (srv_count != 1 || uav_count != 1 || access_count != 2 || load_count != 1 ||
        store_count != 1 || length_count != 1) {
        gpu_ir_shader_release(shader);
        return check("typed-buffer DXIL did not preserve SRV/UAV, load, store and dimensions operations");
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    if (rc != GPU_IR_OK || !msl || !msl_size || !strstr(msl, "device const uint*") ||
        !strstr(msl, "device uint*") || !strstr(msl, "vx_buffer_ranges[0]") ||
        !strstr(msl, "vx_buffer_ranges[1]")) {
        free(msl);
        gpu_ir_shader_release(shader);
        return check("typed-buffer DXIL did not emit bounded read-only and writable MSL buffer parameters");
    }
    free(msl);

    const gpu_ir_descriptor_binding bindings[] = {
        { .set = 0, .binding = 0, .kind = GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE, .count = 1,
          .stage_mask = GPU_IR_STAGE_COMPUTE, .access_mask = GPU_IR_ACCESS_SHADER_READ, .binding_namespace = 0 },
        { .set = 0, .binding = 0, .kind = GPU_IR_DESCRIPTOR_STORAGE_RESOURCE, .count = 1,
          .stage_mask = GPU_IR_STAGE_COMPUTE,
          .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE, .binding_namespace = 1 }
    };
    const gpu_ir_pipeline_layout_desc layout_desc = { .bindings = bindings, .binding_count = 2 };
    gpu_ir_pipeline_layout *layout = gpu_ir_pipeline_layout_create(&layout_desc);
    if (!layout) { gpu_ir_shader_release(shader); return check("D3D12 SRV/UAV common descriptor layout was rejected"); }
    rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK || !msl || !msl_size || !strstr(msl, "[[buffer(0)]]") ||
        !strstr(msl, "[[buffer(1)]]")) {
        free(msl);
        return check("common MSL did not map D3D12 SRV/UAV register namespaces to distinct Metal slots");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) {
            free(msl);
            return check("could not write the typed-buffer common-MSL fixture");
        }
    }
    free(msl);
    return 0;
}

static dxil_md *mutable_metadata_operand(dxil_module *module, dxil_md *node, uint32_t index) {
    if (!module || !node || node->kind != DXM_NODE || index >= node->n || !node->ops ||
        !node->ops[index] || node->ops[index] - 1u >= module->nmd) return NULL;
    return &module->md[node->ops[index] - 1u];
}

static int set_typed_resource_component(dxil_module *module, uint32_t resource_class, uint32_t component_type) {
    const dxil_named_md *named = dxil_named(module, "dx.resources");
    if (!named || named->n != 1 || !named->nodes || named->nodes[0] >= module->nmd) return 0;
    dxil_md *lists = &module->md[named->nodes[0]];
    dxil_md *resources = mutable_metadata_operand(module, lists, resource_class);
    if (!resources || resources->kind != DXM_NODE) return 0;
    for (uint32_t i = 0; i < resources->n; i++) {
        dxil_md *record = mutable_metadata_operand(module, resources, i);
        dxil_md *properties = mutable_metadata_operand(module, record, resource_class == 0 ? 8u : 10u);
        if (!properties || properties->kind != DXM_NODE) return 0;
        for (uint32_t field = 0; field + 1u < properties->n; field += 2u) {
            dxil_md *tag_md = mutable_metadata_operand(module, properties, field);
            dxil_md *value_md = mutable_metadata_operand(module, properties, field + 1u);
            int64_t tag = -1;
            if (!tag_md || !dxil_md_int(module, tag_md, &tag) || tag != 0) continue;
            if (!value_md || value_md->kind != DXM_VALUE || value_md->value >= module->nmodule_values ||
                module->values[value_md->value].kind != DXV_CONST_INT) return 0;
            module->values[value_md->value].u.i = component_type;
            break;
        }
    }
    return resources->n != 0;
}

static int translate_typed_sint_buffer_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_typed_sint_buffer_fixture(&module)) return 1;

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: typed-sint lowering returned %d\n", rc);
        return check("typed R32_SINT buffer DXIL did not lower to common IR");
    }

    uint32_t signed_divide_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_DIVIDE &&
            (instruction->flags & GPU_IR_SHADER_FLAG_SIGNED)) signed_divide_count++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    gpu_ir_shader_release(shader);
    int valid = rc == GPU_IR_OK && msl && msl_size && signed_divide_count == 1 &&
        strstr(msl, "device const uint*") && strstr(msl, "device uint*") &&
        strstr(msl, "vx_buffer_ranges[0]") && strstr(msl, "vx_buffer_ranges[1]") &&
        strstr(msl, "as_type<int>(") && strstr(msl, " / ");
    if (valid && output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) {
            free(msl);
            return check("could not write the typed R32_SINT common-MSL fixture");
        }
    }
    free(msl);
    return valid ? 0 : check("typed R32_SINT loads and stores did not emit bounded common MSL");
}

static dxil_type *function_signature(dxil_module *module, dxil_value *function) {
    if (!module || !function || function->kind != DXV_FUNCTION || function->type >= module->ntypes) return NULL;
    dxil_type *type = &module->types[function->type];
    if (type->kind == DXT_POINTER) {
        if (type->elem >= module->ntypes) return NULL;
        type = &module->types[type->elem];
    }
    return type->kind == DXT_FUNCTION ? type : NULL;
}

static int make_typed_buffer_fixture_float(dxil_module *module) {
    uint32_t float_type = UINT32_MAX;
    for (uint32_t i = 0; i < module->ntypes; i++) {
        if (module->types[i].kind == DXT_FLOAT) { float_type = i; break; }
    }
    if (float_type == UINT32_MAX) {
        if (module->ntypes == UINT32_MAX || (size_t)module->ntypes + 1u > SIZE_MAX / sizeof *module->types)
            return 0;
        dxil_type *types = realloc(module->types, ((size_t)module->ntypes + 1u) * sizeof *types);
        if (!types) return 0;
        module->types = types;
        float_type = module->ntypes++;
        module->types[float_type] = (dxil_type){ .kind = DXT_FLOAT };
    }
    dxil_type *resource_return = NULL;
    for (uint32_t i = 0; i < module->ntypes; i++) {
        dxil_type *type = &module->types[i];
        if (type->kind == DXT_STRUCT && type->name && !strcmp(type->name, "dx.types.ResRet.i32")) {
            resource_return = type;
            break;
        }
    }
    if (!resource_return || resource_return->count != 5 || !resource_return->members) return 0;
    for (uint32_t i = 0; i < 4; i++) resource_return->members[i] = float_type;

    int changed_store_signature = 0;
    for (uint32_t i = 0; i < module->nmodule_values; i++) {
        dxil_value *value = &module->values[i];
        if (value->kind != DXV_FUNCTION || !value->name || strcmp(value->name, "dx.op.bufferStore.i32")) continue;
        dxil_type *signature = function_signature(module, value);
        if (!signature || signature->count != 9 || !signature->members) return 0;
        for (uint32_t parameter = 4; parameter <= 7; parameter++) signature->members[parameter] = float_type;
        changed_store_signature = 1;
    }
    if (!changed_store_signature || !set_typed_resource_component(module, 0, 9) ||
        !set_typed_resource_component(module, 1, 9)) return 0;

    dxil_function *function = NULL;
    for (uint32_t i = 0; i < module->nfunctions; i++) {
        if (!module->functions[i].is_declaration && module->functions[i].name &&
            !strcmp(module->functions[i].name, "main")) { function = &module->functions[i]; break; }
    }
    if (!function) return 0;
    uint32_t loaded_aggregate = UINT32_MAX, loaded_float = UINT32_MAX, arithmetic_result = UINT32_MAX;
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op == DXI_CALL && dxil_dxop(module, function, instruction) == 68)
            loaded_aggregate = instruction->result;
    }
    if (loaded_aggregate == UINT32_MAX) return 0;
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op == DXI_EXTRACTVAL && instruction->n == 2 && instruction->ops &&
            instruction->ops[0] == loaded_aggregate && instruction->ops[1] == 0) {
            instruction->type = float_type;
            loaded_float = instruction->result;
            if (loaded_float < module->nmodule_values ||
                loaded_float - module->nmodule_values >= function->nlocals) return 0;
            function->locals[loaded_float - module->nmodule_values].type = float_type;
            break;
        }
    }
    if (loaded_float == UINT32_MAX) return 0;
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op == DXI_BINOP && instruction->n == 2 && instruction->ops &&
            (instruction->ops[0] == loaded_float || instruction->ops[1] == loaded_float)) {
            instruction->type = float_type;
            instruction->ops[0] = loaded_float;
            instruction->ops[1] = loaded_float;
            arithmetic_result = instruction->result;
            if (arithmetic_result < module->nmodule_values ||
                arithmetic_result - module->nmodule_values >= function->nlocals) return 0;
            function->locals[arithmetic_result - module->nmodule_values].type = float_type;
            break;
        }
    }
    if (arithmetic_result == UINT32_MAX) return 0;
    int changed_store_call = 0;
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op != DXI_CALL || dxil_dxop(module, function, instruction) != 69) continue;
        if (instruction->n != 10 || !instruction->ops) return 0;
        for (uint32_t component = 0; component < 4; component++) instruction->ops[5u + component] = arithmetic_result;
        changed_store_call = 1;
    }
    return changed_store_call;
}

static int translate_typed_float_buffer_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_typed_buffer_fixture(&module)) return 1;
    if (!make_typed_buffer_fixture_float(&module)) {
        dxil_free(&module);
        return check("could not construct the typed R32_FLOAT DXIL test module");
    }
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: typed-float lowering returned %d\n", rc);
        return check("typed R32_FLOAT buffer DXIL did not lower to common IR");
    }
    uint32_t bitcast_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction->opcode == GPU_IR_SHADER_OP_CONVERT &&
            instruction->operation == GPU_IR_SHADER_CONVERT_BITCAST) bitcast_count++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK || !msl || !msl_size || bitcast_count != 2 ||
        !strstr(msl, "as_type<float>(") || !strstr(msl, "as_type<uint>(") || !strstr(msl, " + ")) {
        free(msl);
        return check("typed R32_FLOAT loads, arithmetic and stores did not use bit-preserving common MSL");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) {
            free(msl);
            return check("could not write the typed-float common-MSL fixture");
        }
    }
    free(msl);
    return 0;
}

static int translate_signed_conversion_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/signed_conversion_cs.dxil", &module)) return 1;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: signed-conversion lowering returned %d\n", rc);
        return check("signed-conversion DXIL did not lower to common IR");
    }
    uint32_t float_to_signed_count = 0, signed_to_float_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_CONVERT) continue;
        if (instruction->operation == GPU_IR_SHADER_CONVERT_FLOAT_TO_SIGNED) float_to_signed_count++;
        if (instruction->operation == GPU_IR_SHADER_CONVERT_SIGNED_TO_FLOAT) signed_to_float_count++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK || !msl || !msl_size || float_to_signed_count != 1 || signed_to_float_count != 1 ||
        !strstr(msl, "float(as_type<int>(") || !strstr(msl, "as_type<uint>(int(")) {
        free(msl);
        return check("signed DXIL conversions did not preserve their operation in common MSL");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) {
            free(msl);
            return check("could not write the signed-conversion common-MSL fixture");
        }
    }
    free(msl);
    return 0;
}

static int translate_typed_float_dxil_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/typed_float_buffer_cs.dxil", &module)) return 1;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: real typed-float lowering returned %d\n", rc);
        return check("real typed R32_FLOAT DXIL did not lower to common IR");
    }
    uint32_t bitcast_count = 0, unsigned_to_float_count = 0, float_arithmetic_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("common float shader lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_CONVERT) {
            if (instruction->operation == GPU_IR_SHADER_CONVERT_BITCAST) bitcast_count++;
            if (instruction->operation == GPU_IR_SHADER_CONVERT_UNSIGNED_TO_FLOAT) unsigned_to_float_count++;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                   (instruction->flags & GPU_IR_SHADER_FLAG_FLOAT)) float_arithmetic_count++;
    }
    const gpu_ir_descriptor_binding bindings[] = {
        { .set = 0, .binding = 0, .kind = GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE, .count = 1,
          .stage_mask = GPU_IR_STAGE_COMPUTE, .access_mask = GPU_IR_ACCESS_SHADER_READ, .binding_namespace = 0 },
        { .set = 0, .binding = 0, .kind = GPU_IR_DESCRIPTOR_STORAGE_RESOURCE, .count = 1,
          .stage_mask = GPU_IR_STAGE_COMPUTE,
          .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE, .binding_namespace = 1 }
    };
    const gpu_ir_pipeline_layout_desc layout_desc = { .bindings = bindings, .binding_count = 2 };
    gpu_ir_pipeline_layout *layout = gpu_ir_pipeline_layout_create(&layout_desc);
    if (!layout) { gpu_ir_shader_release(shader); return check("typed-float DXIL descriptor layout was rejected"); }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK || !msl || !msl_size || bitcast_count != 2 || unsigned_to_float_count != 1 ||
        float_arithmetic_count != 3 || !strstr(msl, "as_type<float>(") || !strstr(msl, "as_type<uint>(") ||
        !strstr(msl, "float(") || !strstr(msl, "[[buffer(0)]]") || !strstr(msl, "[[buffer(1)]]")) {
        free(msl);
        return check("real typed R32_FLOAT DXIL did not emit bit-preserving common MSL with register namespaces");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) {
            free(msl);
            return check("could not write the real typed-float common-MSL fixture");
        }
    }
    free(msl);
    return 0;
}

static int translate_unary_intrinsics(const char *output_path) {
    enum { unary_count = 24 };
    static const uint32_t dxil_opcodes[unary_count] = {
        6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
        18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29
    };
    static const uint32_t common_operations[unary_count] = {
        GPU_IR_SHADER_ARITH_ABSOLUTE, GPU_IR_SHADER_ARITH_SATURATE,
        GPU_IR_SHADER_COMPARE_IS_NAN, GPU_IR_SHADER_COMPARE_IS_INF,
        GPU_IR_SHADER_COMPARE_IS_FINITE, GPU_IR_SHADER_COMPARE_IS_NORMAL,
        GPU_IR_SHADER_ARITH_COS, GPU_IR_SHADER_ARITH_SIN, GPU_IR_SHADER_ARITH_TAN,
        GPU_IR_SHADER_ARITH_ACOS, GPU_IR_SHADER_ARITH_ASIN, GPU_IR_SHADER_ARITH_ATAN,
        GPU_IR_SHADER_ARITH_COSH, GPU_IR_SHADER_ARITH_SINH, GPU_IR_SHADER_ARITH_TANH,
        GPU_IR_SHADER_ARITH_EXP2, GPU_IR_SHADER_ARITH_FRACT, GPU_IR_SHADER_ARITH_LOG2,
        GPU_IR_SHADER_ARITH_SQRT, GPU_IR_SHADER_ARITH_INVERSE_SQRT,
        GPU_IR_SHADER_ARITH_ROUND_EVEN, GPU_IR_SHADER_ARITH_FLOOR,
        GPU_IR_SHADER_ARITH_CEILING, GPU_IR_SHADER_ARITH_TRUNC
    };
    uint32_t float_arguments[] = { 1, 2 };
    uint32_t bool_arguments[] = { 1, 2 };
    dxil_type types[7] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_FLOAT },
        { .kind = DXT_INT, .bits = 1 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = 2, .count = 2, .members = float_arguments },
        { .kind = DXT_FUNCTION, .elem = 3, .count = 2, .members = bool_arguments }
    };
    dxil_value values[32] = { 0 };
    values[0] = (dxil_value){ .kind = DXV_FUNCTION, .type = 4, .name = "main" };
    values[1] = (dxil_value){ .kind = DXV_FUNCTION, .type = 5, .name = "dx.op.unary.f32" };
    values[2] = (dxil_value){ .kind = DXV_FUNCTION, .type = 6, .name = "dx.op.unary.i1" };
    for (uint32_t i = 0; i < unary_count; i++)
        values[3 + i] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = dxil_opcodes[i] };
    values[27] = (dxil_value){ .kind = DXV_CONST_FLOAT, .type = 2, .u.f = 0.75 };
    values[28] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 4 };
    values[29] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 8 };
    values[30] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 1 };
    values[31] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 1 };

    dxil_value locals[unary_count];
    uint32_t arguments[unary_count][3];
    dxil_inst instructions[unary_count + 1];
    for (uint32_t i = 0; i < unary_count; i++) {
        int is_compare = dxil_opcodes[i] >= 8 && dxil_opcodes[i] <= 11;
        arguments[i][0] = is_compare ? 2u : 1u;
        arguments[i][1] = 3u + i;
        arguments[i][2] = 27;
        locals[i] = (dxil_value){ .kind = DXV_INST, .type = is_compare ? 3u : 2u, .u.index = i };
        instructions[i] = (dxil_inst){ .op = DXI_CALL, .type = is_compare ? 3u : 2u,
            .result = 32u + i, .block = 0, .n = 3, .ops = arguments[i] };
    }
    instructions[unary_count] = (dxil_inst){ .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 };
    dxil_function function = { .type = 4, .value = 0, .nblocks = 1,
        .ninsts = unary_count + 1, .insts = instructions, .nlocals = unary_count, .locals = locals };
    dxil_md metadata[8] = { 0 };
    uint32_t dimensions[] = { 3, 4, 5 };
    uint32_t properties[] = { 2, 6 };
    uint32_t entry_ops[] = { 1, 0, 0, 0, 7 };
    uint32_t entry_list[] = { 7 };
    metadata[0] = (dxil_md){ .kind = DXM_VALUE, .value = 0 };
    metadata[1] = (dxil_md){ .kind = DXM_VALUE, .value = 28 };
    metadata[2] = (dxil_md){ .kind = DXM_VALUE, .value = 29 };
    metadata[3] = (dxil_md){ .kind = DXM_VALUE, .value = 30 };
    metadata[4] = (dxil_md){ .kind = DXM_VALUE, .value = 31 };
    metadata[5] = (dxil_md){ .kind = DXM_NODE, .n = 3, .ops = dimensions };
    metadata[6] = (dxil_md){ .kind = DXM_NODE, .n = 2, .ops = properties };
    metadata[7] = (dxil_md){ .kind = DXM_NODE, .n = 5, .ops = entry_ops };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_list };
    dxil_module module = { .shader_kind = 5, .ntypes = 7, .types = types,
        .nmodule_values = 32, .values = values, .nfunctions = 1, .functions = &function,
        .nmd = 8, .md = metadata, .nnamed = 1, .named = &named };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: unary-intrinsic lowering returned %d\n", rc);
        return check("DXIL float unary intrinsics did not lower to common IR");
    }
    uint32_t seen = 0;
    int valid = 1;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || (instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC &&
                             instruction->opcode != GPU_IR_SHADER_OP_COMPARE)) continue;
        if (seen >= unary_count) { valid = 0; break; }
        uint32_t expected_opcode = dxil_opcodes[seen] >= 8 && dxil_opcodes[seen] <= 11 ?
            GPU_IR_SHADER_OP_COMPARE : GPU_IR_SHADER_OP_ARITHMETIC;
        if (instruction->opcode != expected_opcode || instruction->operation != common_operations[seen] ||
            instruction->operand_count != 1 || instruction->flags != GPU_IR_SHADER_FLAG_FLOAT) {
            valid = 0;
            break;
        }
        seen++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    if (valid && seen == unary_count) rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    else rc = GPU_IR_INVALID;
    gpu_ir_shader_release(shader);
    const char *expected_msl[] = { "fabs(", "fmin(fmax(", "isnan(", "isinf(", "isfinite(", "isnormal(",
        "cos(", "sin(", "tan(", "acos(", "asin(", "atan(", "cosh(", "sinh(", "tanh(",
        "exp2(", "fract(", "log2(", "sqrt(", "rsqrt(", "rint(", "floor(", "ceil(", "trunc(" };
    valid = valid && rc == GPU_IR_OK && msl && msl_size;
    for (uint32_t i = 0; valid && i < unary_count; i++) valid = strstr(msl, expected_msl[i]) != NULL;
    if (valid && output_path) {
        FILE *output = fopen(output_path, "wb");
        valid = output && fwrite(msl, 1, msl_size, output) == msl_size;
        if (output && fclose(output)) valid = 0;
    }
    free(msl);
    if (!valid) return check("common DXIL unary and scalar classification MSL did not match normalized operations");
    puts("dxil_ir: float unary, rounding, saturate and scalar classification intrinsics lower to common MSL: ok");
    return 0;
}

int main(int argc, char **argv) {
    const char *system_values_output = NULL;
    const char *core_compute_output = NULL;
    const char *bit_intrinsics_output = NULL;
    const char *typed_buffer_output = NULL;
    const char *typed_sint_buffer_output = NULL;
    const char *typed_float_buffer_output = NULL;
    const char *typed_float_dxil_output = NULL;
    const char *signed_conversion_output = NULL;
    const char *unary_intrinsics_output = NULL;
    if (argc == 3 && !strcmp(argv[1], "--write-system-values")) system_values_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-core-compute")) core_compute_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-bit-intrinsics")) bit_intrinsics_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-typed-buffer")) typed_buffer_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-typed-sint-buffer")) typed_sint_buffer_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-typed-float-buffer")) typed_float_buffer_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-typed-float-dxil")) typed_float_dxil_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-signed-conversions")) signed_conversion_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-unary-intrinsics")) unary_intrinsics_output = argv[2];
    else if (argc != 1) return check("unknown command-line option");
    if (translate_core_compute(core_compute_output)) return 1;
    if (translate_bit_intrinsics(bit_intrinsics_output)) return 1;
    if (translate_compute_system_values(system_values_output)) return 1;
    if (translate_cfg_and_phi()) return 1;
    if (translate_vector_compare_and_select()) return 1;
    if (translate_typed_buffer_fixture(typed_buffer_output)) return 1;
    if (translate_typed_sint_buffer_fixture(typed_sint_buffer_output)) return 1;
    if (translate_typed_float_buffer_fixture(typed_float_buffer_output)) return 1;
    if (translate_typed_float_dxil_fixture(typed_float_dxil_output)) return 1;
    if (translate_signed_conversion_fixture(signed_conversion_output)) return 1;
    if (translate_unary_intrinsics(unary_intrinsics_output)) return 1;
    if (reject_unsupported_intrinsic()) return 1;
    return 0;
}
