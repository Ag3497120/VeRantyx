#include "dxil_ir.h"
#include "dxil_msl.h"
#include "gpu_ir_metal.h"
#include "gpu_ir_msl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(const char *condition) {
    fprintf(stderr, "dxil_ir: %s\n", condition);
    return 1;
}

static int check_dxil_target0_common_ir(const char *output_path) {
    uint32_t store_parameters[] = { 1, 1, 1, 2, 3 };
    uint32_t load_parameters[] = { 1, 1, 1, 2, 1 };
    dxil_type types[] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_INT, .bits = 8 },
        { .kind = DXT_FLOAT, .bits = 32 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = 0, .count = 5, .members = store_parameters },
        { .kind = DXT_FUNCTION, .elem = 3, .count = 5, .members = load_parameters },
        { .kind = DXT_INT, .bits = 1 },
        { .kind = DXT_FUNCTION, .elem = 7, .count = 5, .members = load_parameters }
    };
    dxil_value values[] = {
        { .kind = DXV_FUNCTION, .type = 4, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 5, .name = "dx.op.storeOutput.f32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 5 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 2 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 3 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 0.25 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 0.5 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 0.75 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 1.0 },
        { .kind = DXV_FUNCTION, .type = 6, .name = "dx.op.loadInput.f32" },
        { .kind = DXV_CONST_UNDEF, .type = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 3 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 9 },
        { .kind = DXV_FUNCTION, .type = 8, .name = "dx.op.loadInput.i1" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 13 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = -1 }
    };
    uint32_t entry_ops[] = { 1, 2, 18, 0, 0 };
    uint32_t signature_ops[] = { 17, 16, 0 };
    uint32_t output_list_ops[] = { 15 };
    uint32_t position_indices[] = { 3 };
    uint32_t input_list_ops[] = { 25, 36 };
    uint32_t position_element_ops[] = { 3, 20, 21, 22, 23, 11, 10, 11, 3, 3, 24 };
    uint32_t index_list_ops[] = { 8 };
    uint32_t front_face_indices[] = { 3 };
    uint32_t front_face_element_ops[] = { 26, 27, 28, 29, 35, 30, 31, 32, 37, 34, 24 };
    uint32_t element_ops[] = { 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14 };
    dxil_md metadata[37] = {
        { .kind = DXM_VALUE, .value = 0 },
        { .kind = DXM_STRING, .str = "main" },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_STRING, .str = "SV_Target" },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_NODE, .n = 1, .ops = index_list_ops },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_VALUE, .type = 1, .value = 3 },
        { .kind = DXM_VALUE, .type = 1, .value = 4 },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_NULL },
        { .kind = DXM_NODE, .n = 11, .ops = element_ops },
        { .kind = DXM_NODE, .n = 1, .ops = output_list_ops },
        { .kind = DXM_NODE, .n = 2, .ops = input_list_ops },
        { .kind = DXM_NODE, .n = 3, .ops = signature_ops },
        { .kind = DXM_NODE, .n = 5, .ops = entry_ops },
        { .kind = DXM_STRING, .str = "SV_Position" },
        { .kind = DXM_VALUE, .type = 1, .value = 17 },
        { .kind = DXM_VALUE, .type = 1, .value = 16 },
        { .kind = DXM_NODE, .n = 1, .ops = position_indices },
        { .kind = DXM_NULL },
        { .kind = DXM_NODE, .n = 11, .ops = position_element_ops },
        { .kind = DXM_VALUE, .type = 1, .value = 3 },
        { .kind = DXM_STRING, .str = "SV_IsFrontFace" },
        { .kind = DXM_VALUE, .type = 1, .value = 3 },
        { .kind = DXM_VALUE, .type = 1, .value = 19 },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_VALUE, .type = 1, .value = 3 },
        { .kind = DXM_VALUE, .type = 1, .value = 3 },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_NODE, .n = 1, .ops = front_face_indices },
        { .kind = DXM_NODE, .n = 11, .ops = front_face_element_ops },
        { .kind = DXM_VALUE, .type = 1, .value = 20 }
    };
    uint32_t entry_metadata[] = { 18 };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_metadata };
    uint32_t call_args[4][6] = {
        { 1, 5, 2, 2, 6, 21 }, { 1, 5, 2, 2, 7, 22 },
        { 1, 5, 2, 2, 8, 23 }, { 1, 5, 2, 2, 9, 26 }
    };
    uint32_t load_args[4][6] = {
        { 14, 4, 2, 2, 6, 15 }, { 14, 4, 2, 2, 7, 15 },
        { 14, 4, 2, 2, 8, 15 }, { 14, 4, 2, 2, 9, 15 }
    };
    uint32_t front_face_args[6] = { 18, 4, 3, 2, 6, 15 };
    uint32_t select_args[3] = { 25, 13, 24 };
    dxil_inst instructions[11] = { 0 };
    for (uint32_t component = 0; component < 4; component++) {
        instructions[component] = (dxil_inst){ .op = DXI_CALL, .type = 3, .result = 21 + component,
            .block = 0, .n = 6, .ops = load_args[component] };
        instructions[component + 6] = (dxil_inst){ .op = DXI_CALL, .type = 0, .result = UINT32_MAX,
            .block = 0, .n = 6, .ops = call_args[component] };
    }
    instructions[4] = (dxil_inst){ .op = DXI_CALL, .type = 7, .result = 25,
        .block = 0, .n = 6, .ops = front_face_args };
    instructions[5] = (dxil_inst){ .op = DXI_SELECT, .type = 3, .result = 26,
        .block = 0, .n = 3, .ops = select_args };
    instructions[10] = (dxil_inst){ .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 };
    dxil_value locals[8] = {
        { .kind = DXV_INST, .type = 3, .u.index = 0 },
        { .kind = DXV_INST, .type = 3, .u.index = 1 },
        { .kind = DXV_INST, .type = 3, .u.index = 2 },
        { .kind = DXV_INST, .type = 3, .u.index = 3 },
        { .kind = DXV_INST, .type = 7, .u.index = 4 },
        { .kind = DXV_INST, .type = 3, .u.index = 5 }
    };
    dxil_function functions[] = {
        { .name = "main", .type = 4, .value = 0, .nblocks = 1, .ninsts = 11, .insts = instructions,
          .nlocals = 6, .locals = locals },
        { .name = "dx.op.storeOutput.f32", .type = 5, .value = 1, .is_declaration = 1 },
        { .name = "dx.op.loadInput.f32", .type = 6, .value = 14, .is_declaration = 1 },
        { .name = "dx.op.loadInput.i1", .type = 8, .value = 18, .is_declaration = 1 }
    };
    dxil_module module = {
        .shader_kind = 0, .ntypes = sizeof types / sizeof types[0], .types = types,
        .nmodule_values = sizeof values / sizeof values[0], .values = values,
        .nfunctions = sizeof functions / sizeof functions[0], .functions = functions,
        .nmd = sizeof metadata / sizeof metadata[0], .md = metadata,
        .nnamed = 1, .named = &named
    };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: target0 output lowering returned %d\n", rc);
        return check("DXIL SV_Target0 RGBA stores did not lower to common IR");
    }
    uint32_t input_variables = 0, front_facing_variables = 0, output_variables = 0, loads = 0;
    uint32_t extracts = 0, composites = 0, selects = 0, stores = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) continue;
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE && instruction->entry_point_interface &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_INPUT && instruction->has_builtin &&
            instruction->builtin == GPU_IR_SHADER_BUILTIN_FRAG_COORD &&
            instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL)
            input_variables++;
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE && instruction->entry_point_interface &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_INPUT && instruction->has_builtin &&
            instruction->builtin == GPU_IR_SHADER_BUILTIN_FRONT_FACING &&
            instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL) front_facing_variables++;
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE && instruction->entry_point_interface &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_OUTPUT && instruction->has_location &&
            instruction->location == 0 && instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL)
            output_variables++;
        if (instruction->opcode == GPU_IR_SHADER_OP_LOAD && instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL)
            loads++;
        if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
            instruction->operation == GPU_IR_SHADER_COMPOSITE_EXTRACT && instruction->operand_count == 1)
            extracts++;
        if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
            instruction->operation == GPU_IR_SHADER_COMPOSITE_CONSTRUCT && instruction->operand_count == 4)
            composites++;
        if (instruction->opcode == GPU_IR_SHADER_OP_SELECT) selects++;
        if (instruction->opcode == GPU_IR_SHADER_OP_STORE && instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL)
            stores++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_fragment(shader, &msl, &msl_size);
    int failed = rc != GPU_IR_OK || !msl || !msl_size || input_variables != 1 || front_facing_variables != 1 ||
        output_variables != 1 || loads != 5 || extracts != 4 || composites != 1 || selects != 1 || stores != 1 ||
        !strstr(msl, "fragment float4 vx_fragment(") || !strstr(msl, "[[position]]") ||
        !strstr(msl, "bool vx_builtin_") || !strstr(msl, "[[front_facing]]") ||
        !strstr(msl, "? as_type<float>(0x3f800000u) : vx_value_") ||
        !strstr(msl, ".x;") || !strstr(msl, ".y;") || !strstr(msl, ".z;") || !strstr(msl, ".w;") ||
        !strstr(msl, "return vx_io_");
    if (!failed && output_path) {
        FILE *output = fopen(output_path, "wb");
        failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
    }
    free(msl);
    gpu_ir_shader_release(shader);
    if (failed) {
        fprintf(stderr, "dxil_ir: target0 MSL emission returned %d\n", rc);
        return check("common DXIL color output was not represented as one location-0 float4");
    }

    // DXIL uses -1 for an unresolved system-generated signature row in newer shader models;
    // the legacy zero row remains accepted, while ordinary packed rows are not front-face inputs.
    front_face_element_ops[8] = 33;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK) return check("legacy DXIL front-face signature row was rejected");
    front_face_element_ops[8] = 31;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_UNSUPPORTED || shader)
        return check("packed DXIL row was accepted for the front-face system value");
    front_face_element_ops[8] = 37;

    // Incomplete writes and a duplicate component must stay outside this deliberately narrow common path.
    load_args[0][5] = 2;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_UNSUPPORTED || shader)
        return check("DXIL pixel loadInput with a defined vertex axis was accepted");
    load_args[0][5] = 15;
    instructions[9] = instructions[10];
    functions[0].ninsts = 10;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_UNSUPPORTED || shader)
        return check("incomplete DXIL target output was accepted by the common path");
    call_args[3][4] = 8;
    instructions[9] = (dxil_inst){ .op = DXI_CALL, .type = 0, .result = UINT32_MAX,
        .block = 0, .n = 6, .ops = call_args[3] };
    functions[0].ninsts = 11;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_UNSUPPORTED || shader)
        return check("duplicate DXIL target output component was accepted by the common path");
    puts("dxil_ir: DXIL SV_Position/SV_IsFrontFace inputs and SV_Target0 output lower to common IR and MSL: ok");
    return 0;
}

static int check_dxil_vertex_position_output(const char *output_path) {
    uint32_t store_parameters[] = { 1, 1, 1, 2, 3 };
    dxil_type types[] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_INT, .bits = 8 },
        { .kind = DXT_FLOAT, .bits = 32 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = 0, .count = 5, .members = store_parameters }
    };
    dxil_value values[] = {
        { .kind = DXV_FUNCTION, .type = 4, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 5, .name = "dx.op.storeOutput.f32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 5 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 2 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 3 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 0.0 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 0.0 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 0.5 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 1.0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 3 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 9 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 }
    };
    uint32_t signature_element_ids[] = { 3 };
    uint32_t output_list_ids[] = { 13 };
    uint32_t signatures[] = { 14, 15, 0 };
    uint32_t entry[] = { 1, 2, 16, 0, 0 };
    uint32_t entry_list[] = { 16 };
    uint32_t position_element[] = { 3, 4, 5, 6, 7, 8, 9, 10, 3, 3, 11 };
    dxil_md metadata[] = {
        { .kind = DXM_VALUE, .value = 0 },
        { .kind = DXM_STRING, .str = "main" },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_STRING, .str = "SV_Position" },
        { .kind = DXM_VALUE, .type = 1, .value = 14 },
        { .kind = DXM_VALUE, .type = 1, .value = 13 },
        { .kind = DXM_NODE, .n = 1, .ops = signature_element_ids },
        { .kind = DXM_NULL },
        { .kind = DXM_VALUE, .type = 1, .value = 15 },
        { .kind = DXM_VALUE, .type = 1, .value = 16 },
        { .kind = DXM_VALUE, .type = 1, .value = 2 },
        { .kind = DXM_NULL },
        { .kind = DXM_NODE, .n = 11, .ops = position_element },
        { .kind = DXM_NODE },
        { .kind = DXM_NODE, .n = 1, .ops = output_list_ids },
        { .kind = DXM_NODE, .n = 3, .ops = signatures },
        { .kind = DXM_NODE, .n = 5, .ops = entry }
    };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_list };
    uint32_t store_args[4][6] = {
        { 1, 3, 2, 2, 5, 9 }, { 1, 3, 2, 2, 6, 10 },
        { 1, 3, 2, 2, 7, 11 }, { 1, 3, 2, 2, 8, 12 }
    };
    dxil_inst instructions[5] = { 0 };
    for (uint32_t component = 0; component < 4; component++) {
        instructions[component] = (dxil_inst){
            .op = DXI_CALL, .type = 0, .result = UINT32_MAX, .block = 0,
            .n = 6, .ops = store_args[component]
        };
    }
    instructions[4] = (dxil_inst){ .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 };
    dxil_function functions[] = {
        { .name = "main", .type = 4, .value = 0, .nblocks = 1, .ninsts = 5, .insts = instructions },
        { .name = "dx.op.storeOutput.f32", .type = 5, .value = 1, .is_declaration = 1 }
    };
    dxil_module module = {
        .shader_kind = 1, .ntypes = sizeof types / sizeof types[0], .types = types,
        .nmodule_values = sizeof values / sizeof values[0], .values = values,
        .nfunctions = sizeof functions / sizeof functions[0], .functions = functions,
        .nmd = sizeof metadata / sizeof metadata[0], .md = metadata,
        .nnamed = 1, .named = &named
    };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: vertex position lowering returned %d\n", rc);
        return check("DXIL vertex SV_Position stores did not lower to common IR");
    }
    uint32_t position_outputs = 0, composites = 0, stores = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) continue;
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE && instruction->entry_point_interface &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_OUTPUT && instruction->has_builtin &&
            instruction->builtin == GPU_IR_SHADER_BUILTIN_POSITION &&
            instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL)
            position_outputs++;
        if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
            instruction->operation == GPU_IR_SHADER_COMPOSITE_CONSTRUCT && instruction->operand_count == 4)
            composites++;
        if (instruction->opcode == GPU_IR_SHADER_OP_STORE && instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL)
            stores++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_vertex(shader, &msl, &msl_size);
    int failed = rc != GPU_IR_OK || !msl || !msl_size || position_outputs != 1 || composites != 1 || stores != 1 ||
        !strstr(msl, "vertex VxVertexOutput vx_vertex(") || !strstr(msl, "[[position]]") ||
        !strstr(msl, "vx_output.position = vx_io_") || !strstr(msl, "return vx_output;");
    if (!failed && output_path) {
        FILE *output = fopen(output_path, "wb");
        failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
    }
    free(msl);
    gpu_ir_shader_release(shader);
    if (failed) {
        fprintf(stderr, "dxil_ir: vertex position MSL emission returned %d\n", rc);
        return check("common DXIL vertex output was not emitted as a Metal position");
    }

    instructions[3] = instructions[4];
    functions[0].ninsts = 4;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_UNSUPPORTED || shader)
        return check("incomplete DXIL vertex SV_Position output was accepted");
    store_args[3][4] = 7;
    instructions[3] = (dxil_inst){ .op = DXI_CALL, .type = 0, .result = UINT32_MAX, .block = 0,
        .n = 6, .ops = store_args[3] };
    functions[0].ninsts = 5;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_UNSUPPORTED || shader)
        return check("duplicate DXIL vertex SV_Position component was accepted");
    puts("dxil_ir: DXIL vertex SV_Position stores lower to a built-in common position and MSL: ok");
    return 0;
}

static int check_dxil_vertex_input(const char *output_path) {
    uint32_t store_parameters[] = { 1, 1, 1, 2, 3 };
    uint32_t load_parameters[] = { 1, 1, 1, 2, 1 };
    dxil_type types[] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_INT, .bits = 8 },
        { .kind = DXT_FLOAT, .bits = 32 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = 0, .count = 5, .members = store_parameters },
        { .kind = DXT_FUNCTION, .elem = 3, .count = 5, .members = load_parameters }
    };
    dxil_value values[] = {
        { .kind = DXV_FUNCTION, .type = 4, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 5, .name = "dx.op.storeOutput.f32" },
        { .kind = DXV_FUNCTION, .type = 6, .name = "dx.op.loadInput.f32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 5 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 3 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 5 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 2 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 3 },
        { .kind = DXV_CONST_UNDEF, .type = 1 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 0.5 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 1.0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 9 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 2 }
    };
    uint32_t input_index[] = { 6 }, output_index[] = { 6 };
    uint32_t input_element_ops[] = { 4, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
    uint32_t output_element_ops[] = { 16, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25 };
    uint32_t input_list_ops[] = { 14 }, output_list_ops[] = { 26 };
    uint32_t signature_ops[] = { 27, 28, 0 }, entry_ops[] = { 1, 2, 29, 0, 0 };
    dxil_md metadata[] = {
        { .kind = DXM_VALUE, .value = 0 },
        { .kind = DXM_STRING, .str = "main" },
        { .kind = DXM_STRING, .str = "POSITION" },
        { .kind = DXM_VALUE, .type = 1, .value = 5 },
        { .kind = DXM_VALUE, .type = 1, .value = 15 },
        { .kind = DXM_VALUE, .type = 1, .value = 7 },
        { .kind = DXM_NODE, .n = 1, .ops = input_index },
        { .kind = DXM_NULL },
        { .kind = DXM_VALUE, .type = 1, .value = 16 },
        { .kind = DXM_VALUE, .type = 1, .value = 17 },
        { .kind = DXM_VALUE, .type = 1, .value = 7 },
        { .kind = DXM_VALUE, .type = 1, .value = 7 },
        { .kind = DXM_NULL },
        { .kind = DXM_NODE, .n = 11, .ops = input_element_ops },
        { .kind = DXM_STRING, .str = "SV_Position" },
        { .kind = DXM_VALUE, .type = 1, .value = 6 },
        { .kind = DXM_VALUE, .type = 1, .value = 15 },
        { .kind = DXM_VALUE, .type = 1, .value = 5 },
        { .kind = DXM_NODE, .n = 1, .ops = output_index },
        { .kind = DXM_NULL },
        { .kind = DXM_VALUE, .type = 1, .value = 16 },
        { .kind = DXM_VALUE, .type = 1, .value = 3 },
        { .kind = DXM_VALUE, .type = 1, .value = 7 },
        { .kind = DXM_VALUE, .type = 1, .value = 7 },
        { .kind = DXM_NULL },
        { .kind = DXM_NODE, .n = 11, .ops = output_element_ops },
        { .kind = DXM_NODE, .n = 1, .ops = input_list_ops },
        { .kind = DXM_NODE, .n = 1, .ops = output_list_ops },
        { .kind = DXM_NODE, .n = 3, .ops = signature_ops },
        { .kind = DXM_NODE, .n = 5, .ops = entry_ops }
    };
    uint32_t entry_metadata[] = { 29 };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_metadata };
    uint32_t load_args[2][6] = {
        { 2, 3, 5, 7, 8, 12 }, { 2, 3, 5, 7, 9, 12 }
    };
    uint32_t store_args[4][6] = {
        { 1, 4, 6, 7, 8, 18 }, { 1, 4, 6, 7, 9, 19 },
        { 1, 4, 6, 7, 10, 13 }, { 1, 4, 6, 7, 11, 14 }
    };
    dxil_inst instructions[7] = { 0 };
    for (uint32_t i = 0; i < 2; i++)
        instructions[i] = (dxil_inst){ .op = DXI_CALL, .type = 3, .result = 18 + i,
            .block = 0, .n = 6, .ops = load_args[i] };
    for (uint32_t i = 0; i < 4; i++)
        instructions[i + 2] = (dxil_inst){ .op = DXI_CALL, .type = 0, .result = UINT32_MAX,
            .block = 0, .n = 6, .ops = store_args[i] };
    instructions[6] = (dxil_inst){ .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 };
    dxil_value locals[] = {
        { .kind = DXV_INST, .type = 3, .u.index = 0 },
        { .kind = DXV_INST, .type = 3, .u.index = 1 }
    };
    dxil_function functions[] = {
        { .name = "main", .type = 4, .value = 0, .nblocks = 1, .ninsts = 7,
          .insts = instructions, .nlocals = 2, .locals = locals },
        { .name = "dx.op.storeOutput.f32", .type = 5, .value = 1, .is_declaration = 1 },
        { .name = "dx.op.loadInput.f32", .type = 6, .value = 2, .is_declaration = 1 }
    };
    dxil_module module = {
        .shader_kind = 1, .ntypes = sizeof types / sizeof types[0], .types = types,
        .nmodule_values = sizeof values / sizeof values[0], .values = values,
        .nfunctions = sizeof functions / sizeof functions[0], .functions = functions,
        .nmd = sizeof metadata / sizeof metadata[0], .md = metadata,
        .nnamed = 1, .named = &named
    };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: vertex input lowering returned %d\n", rc);
        return check("DXIL float2 vertex input and SV_Position did not lower to common IR");
    }
    uint32_t input_variables = 0, loads = 0, extracts = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) continue;
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE && instruction->entry_point_interface &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_INPUT && instruction->has_location &&
            instruction->location == 0 && instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL)
            input_variables++;
        if (instruction->opcode == GPU_IR_SHADER_OP_LOAD && instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL)
            loads++;
        if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
            instruction->operation == GPU_IR_SHADER_COMPOSITE_EXTRACT &&
            instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL) extracts++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_vertex(shader, &msl, &msl_size);
    int failed = rc != GPU_IR_OK || !msl || !msl_size || input_variables != 1 || loads != 2 || extracts != 2 ||
        !strstr(msl, "VxVertexInput") || !strstr(msl, "vx_attribute_0 [[attribute(0)]]") ||
        !strstr(msl, "vx_input.vx_attribute_0") || !strstr(msl, "[[position]]");
    if (!failed && output_path) {
        FILE *output = fopen(output_path, "wb");
        failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
    }
    free(msl);
    gpu_ir_shader_release(shader);
    if (failed) {
        fprintf(stderr, "dxil_ir: vertex input MSL emission returned %d\n", rc);
        return check("DXIL generic float vertex input was not mapped to Metal attribute 0");
    }

    metadata[9].value = 16;
    load_args[1][4] = 8;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        return check("DXIL scalar vertex input did not lower to common IR");
    }
    msl = NULL;
    msl_size = 0;
    rc = gpu_ir_msl_emit_vertex(shader, &msl, &msl_size);
    failed = rc != GPU_IR_OK || !msl || !msl_size ||
        !strstr(msl, "float vx_attribute_0 [[attribute(0)]]") ||
        !strstr(msl, "vx_input.vx_attribute_0");
    free(msl);
    gpu_ir_shader_release(shader);
    if (failed) return check("DXIL scalar vertex input was not emitted as a Metal scalar attribute");

    metadata[9].value = 17;
    load_args[1][4] = 10;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_UNSUPPORTED || shader)
        return check("DXIL loadInput component beyond the vertex signature width was accepted");
    puts("dxil_ir: DXIL float vertex inputs map signature IDs to common IR locations and MSL attributes: ok");
    return 0;
}

static int check_dxil_vertex_system_values(const char *output_path) {
    uint32_t store_parameters[] = { 1, 1, 1, 2, 3 };
    uint32_t load_parameters[] = { 1, 1, 1, 2, 1 };
    dxil_type types[] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_INT, .bits = 8 },
        { .kind = DXT_FLOAT, .bits = 32 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = 0, .count = 5, .members = store_parameters },
        { .kind = DXT_FUNCTION, .elem = 1, .count = 5, .members = load_parameters }
    };
    dxil_value values[] = {
        { .kind = DXV_FUNCTION, .type = 4, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 5, .name = "dx.op.storeOutput.f32" },
        { .kind = DXV_FUNCTION, .type = 6, .name = "dx.op.loadInput.i32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 },       // dx.op.loadInput
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 5 },       // dx.op.storeOutput
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 2 },       // SV_VertexID signature id
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 3 },       // SV_InstanceID signature id
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },       // SV_Position signature id
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },       // VertexID system value
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 2 },       // InstanceID system value
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },       // one row
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },       // one column
        { .kind = DXV_CONST_INT, .type = 1, .u.i = -1 },      // SM 5.1+ system-generated row
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 9 },       // float32 component type
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 3 },       // SV_Position system value
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 0 },       // component 0
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 2 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 3 },
        { .kind = DXV_CONST_UNDEF, .type = 1 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 0.0 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 0.0 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 0.5 },
        { .kind = DXV_CONST_FLOAT, .type = 3, .u.f = 1.0 }
    };
    uint32_t vertex_semantic_index[] = { 8 }, instance_semantic_index[] = { 21 },
        position_semantic_index[] = { 34 };
    uint32_t vertex_signature_ops[] = { 3, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14 };
    uint32_t instance_signature_ops[] = { 16, 17, 18, 19, 20, 22, 23, 24, 25, 26, 27 };
    uint32_t position_signature_ops[] = { 29, 30, 31, 32, 33, 35, 36, 37, 38, 39, 40 };
    uint32_t input_signatures[] = { 15, 28 }, output_signatures[] = { 41 };
    uint32_t signature_lists[] = { 42, 43, 0 }, entry_ops[] = { 1, 2, 44, 0, 0 };
    dxil_md metadata[] = {
        { .kind = DXM_VALUE, .value = 0 },
        { .kind = DXM_STRING, .str = "main" },
        { .kind = DXM_VALUE, .type = 1, .value = 5 },
        { .kind = DXM_STRING, .str = "SV_VertexID" },
        { .kind = DXM_VALUE, .type = 1, .value = 8 },
        { .kind = DXM_VALUE, .type = 1, .value = 10 },
        { .kind = DXM_NODE, .n = 1, .ops = vertex_semantic_index },
        { .kind = DXM_VALUE, .type = 1, .value = 8 },
        { .kind = DXM_NULL },
        { .kind = DXM_VALUE, .type = 1, .value = 12 },
        { .kind = DXM_VALUE, .type = 1, .value = 13 },
        { .kind = DXM_VALUE, .type = 1, .value = 15 },
        { .kind = DXM_VALUE, .type = 1, .value = 21 },
        { .kind = DXM_NULL },
        { .kind = DXM_NODE, .n = 11, .ops = vertex_signature_ops },
        { .kind = DXM_VALUE, .type = 1, .value = 6 },
        { .kind = DXM_STRING, .str = "SV_InstanceID" },
        { .kind = DXM_VALUE, .type = 1, .value = 8 },
        { .kind = DXM_VALUE, .type = 1, .value = 11 },
        { .kind = DXM_NODE, .n = 1, .ops = instance_semantic_index },
        { .kind = DXM_VALUE, .type = 1, .value = 8 },
        { .kind = DXM_NULL },
        { .kind = DXM_VALUE, .type = 1, .value = 12 },
        { .kind = DXM_VALUE, .type = 1, .value = 13 },
        { .kind = DXM_VALUE, .type = 1, .value = 14 },
        { .kind = DXM_VALUE, .type = 1, .value = 21 },
        { .kind = DXM_NULL },
        { .kind = DXM_NODE, .n = 11, .ops = instance_signature_ops },
        { .kind = DXM_VALUE, .type = 1, .value = 7 },
        { .kind = DXM_STRING, .str = "SV_Position" },
        { .kind = DXM_VALUE, .type = 1, .value = 16 },
        { .kind = DXM_VALUE, .type = 1, .value = 17 },
        { .kind = DXM_NODE, .n = 1, .ops = position_semantic_index },
        { .kind = DXM_VALUE, .type = 1, .value = 8 },
        { .kind = DXM_NULL },
        { .kind = DXM_VALUE, .type = 1, .value = 18 },
        { .kind = DXM_VALUE, .type = 1, .value = 19 },
        { .kind = DXM_VALUE, .type = 1, .value = 20 },
        { .kind = DXM_VALUE, .type = 1, .value = 21 },
        { .kind = DXM_NULL },
        { .kind = DXM_NODE, .n = 11, .ops = position_signature_ops },
        { .kind = DXM_NODE, .n = 2, .ops = input_signatures },
        { .kind = DXM_NODE, .n = 1, .ops = output_signatures },
        { .kind = DXM_NODE, .n = 3, .ops = signature_lists },
        { .kind = DXM_NODE, .n = 5, .ops = entry_ops }
    };
    uint32_t entry_metadata[] = { 44 };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_metadata };
    uint32_t load_args[2][6] = {
        { 2, 3, 5, 8, 22, 26 }, { 2, 3, 6, 8, 22, 26 }
    };
    uint32_t store_args[4][6] = {
        { 1, 4, 7, 8, 22, 27 }, { 1, 4, 7, 8, 23, 28 },
        { 1, 4, 7, 8, 24, 29 }, { 1, 4, 7, 8, 25, 30 }
    };
    dxil_inst instructions[7] = { 0 };
    instructions[0] = (dxil_inst){ .op = DXI_CALL, .type = 1, .result = 31,
        .block = 0, .n = 6, .ops = load_args[0] };
    instructions[1] = (dxil_inst){ .op = DXI_CALL, .type = 1, .result = 32,
        .block = 0, .n = 6, .ops = load_args[1] };
    for (uint32_t i = 0; i < 4; i++)
        instructions[i + 2] = (dxil_inst){ .op = DXI_CALL, .type = 0, .result = UINT32_MAX,
            .block = 0, .n = 6, .ops = store_args[i] };
    instructions[6] = (dxil_inst){ .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 };
    dxil_value locals[] = {
        { .kind = DXV_INST, .type = 1, .u.index = 0 },
        { .kind = DXV_INST, .type = 1, .u.index = 1 }
    };
    dxil_function functions[] = {
        { .name = "main", .type = 4, .value = 0, .nblocks = 1, .ninsts = 7,
          .insts = instructions, .nlocals = 2, .locals = locals },
        { .name = "dx.op.storeOutput.f32", .type = 5, .value = 1, .is_declaration = 1 },
        { .name = "dx.op.loadInput.i32", .type = 6, .value = 2, .is_declaration = 1 }
    };
    dxil_module module = {
        .shader_kind = 1, .ntypes = sizeof types / sizeof types[0], .types = types,
        .nmodule_values = sizeof values / sizeof values[0], .values = values,
        .nfunctions = sizeof functions / sizeof functions[0], .functions = functions,
        .nmd = sizeof metadata / sizeof metadata[0], .md = metadata,
        .nnamed = 1, .named = &named
    };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: vertex system-value lowering returned %d\n", rc);
        return check("DXIL SV_VertexID/SV_InstanceID did not lower to common IR");
    }
    uint32_t vertex_index_variables = 0, instance_id_variables = 0;
    uint32_t vertex_index_loads = 0, instance_id_loads = 0;
    uint32_t vertex_index_variable_id = 0, instance_id_variable_id = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) continue;
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE && instruction->entry_point_interface &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_INPUT && instruction->has_builtin) {
            if (instruction->builtin == GPU_IR_SHADER_BUILTIN_VERTEX_INDEX) {
                vertex_index_variables++;
                vertex_index_variable_id = instruction->result_id;
            } else if (instruction->builtin == GPU_IR_SHADER_BUILTIN_INSTANCE_ID) {
                instance_id_variables++;
                instance_id_variable_id = instruction->result_id;
            }
        }
        if (instruction->opcode != GPU_IR_SHADER_OP_LOAD || instruction->operand_count != 1) continue;
        const gpu_ir_shader_operand *operand = gpu_ir_shader_operand_at(shader, instruction->first_operand);
        if (!operand || operand->kind != GPU_IR_SHADER_OPERAND_ID) continue;
        if (operand->value == vertex_index_variable_id) vertex_index_loads++;
        if (operand->value == instance_id_variable_id) instance_id_loads++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_vertex(shader, &msl, &msl_size);
    int failed = rc != GPU_IR_OK || !msl || !msl_size || vertex_index_variables != 1 ||
        instance_id_variables != 1 || vertex_index_loads != 1 || instance_id_loads != 1 ||
        !strstr(msl, "[[vertex_id]]") || !strstr(msl, "[[instance_id]]") ||
        !strstr(msl, "[[base_instance]]") || !strstr(msl, " - vx_base_instance;") ||
        !strstr(msl, "[[position]]");
    if (!failed && output_path) {
        FILE *output = fopen(output_path, "wb");
        failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
    }
    free(msl);
    gpu_ir_shader_release(shader);
    if (failed) {
        fprintf(stderr, "dxil_ir: vertex system-value MSL emission returned %d\n", rc);
        return check("DXIL vertex system values were not mapped to Metal vertex/instance IDs");
    }

    // SM 5.1+ allows -1 for an unresolved system-generated row; packed ordinary rows stay unsupported.
    metadata[24].value = 12;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_UNSUPPORTED || shader)
        return check("ordinary DXIL signature row was accepted for SV_InstanceID");
    puts("dxil_ir: DXIL SV_VertexID and zero-relative SV_InstanceID lower through common IR to Metal: ok");
    return 0;
}

static int check_discard_dxil_lowering(void) {
    uint32_t intrinsic_parameters[] = { 1, 2 };
    dxil_type types[] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_INT, .bits = 1 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = 0, .count = 2, .members = intrinsic_parameters }
    };
    dxil_value values[] = {
        { .kind = DXV_FUNCTION, .type = 3, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 4, .name = "dx.op.discard" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 82 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 1 }
    };
    uint32_t discard_arguments[] = { 1, 2, 3 };
    dxil_inst instructions[] = {
        { .op = DXI_CALL, .type = 0, .result = UINT32_MAX, .block = 0,
          .n = 3, .ops = discard_arguments },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function functions[] = {
        { .name = "main", .type = 3, .value = 0, .nblocks = 1,
          .ninsts = 2, .insts = instructions },
        { .name = "dx.op.discard", .type = 4, .value = 1, .is_declaration = 1 }
    };
    dxil_module module = {
        .shader_kind = 0, .ntypes = sizeof types / sizeof types[0], .types = types,
        .nmodule_values = sizeof values / sizeof values[0], .values = values,
        .nfunctions = sizeof functions / sizeof functions[0], .functions = functions
    };
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    const gpu_ir_shader_instruction *discard = NULL;
    for (uint32_t i = 0; shader && i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(shader, i);
        if (candidate && candidate->opcode == GPU_IR_SHADER_OP_DISCARD) discard = candidate;
    }
    const gpu_ir_shader_operand *condition = discard ?
        gpu_ir_shader_operand_at(shader, discard->first_operand) : NULL;
    const gpu_ir_shader_instruction *condition_value = NULL;
    for (uint32_t i = 0; shader && condition && condition->kind == GPU_IR_SHADER_OPERAND_ID &&
         i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(shader, i);
        if (candidate && candidate->result_id == condition->value) condition_value = candidate;
    }
    int valid = rc == GPU_IR_OK && shader && discard && discard->source_api == GPU_IR_SHADER_SOURCE_DXIL &&
        discard->result_id == 0 && discard->result_type_id == 0 && discard->operand_count == 1 &&
        condition && condition->kind == GPU_IR_SHADER_OPERAND_ID && condition_value &&
        condition_value->opcode == GPU_IR_SHADER_OP_CONSTANT &&
        condition_value->operation == GPU_IR_SHADER_CONSTANT_BOOL &&
        gpu_ir_shader_literal_word_at(shader, condition_value->first_literal) == 1;
    gpu_ir_shader_release(shader);
    if (!valid) {
        fprintf(stderr, "dxil_ir: discard lowering returned %d\n", rc);
        return check("pixel DXIL Discard did not lower to a scalar common-IR discard");
    }
    module.shader_kind = 1;
    shader = NULL;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_UNSUPPORTED || shader)
        return check("DXIL Discard was accepted outside the pixel stage");
    puts("dxil_ir: pixel DXIL Discard lowers to common IR and rejects other stages: ok");
    return 0;
}

static int check_discard_fragment_msl(const char *output_path) {
    gpu_ir_shader_type types[] = {
        { .id = 1, .kind = GPU_IR_SHADER_TYPE_VOID },
        { .id = 2, .kind = GPU_IR_SHADER_TYPE_BOOL },
        { .id = 3, .kind = GPU_IR_SHADER_TYPE_FLOAT, .bit_width = 32 },
        { .id = 4, .kind = GPU_IR_SHADER_TYPE_VECTOR, .element_type_id = 3, .element_count = 4 },
        { .id = 5, .kind = GPU_IR_SHADER_TYPE_POINTER, .element_type_id = 4,
          .storage_class = GPU_IR_SHADER_STORAGE_OUTPUT, .source_storage_class = 3 },
        { .id = 6, .kind = GPU_IR_SHADER_TYPE_FUNCTION, .element_type_id = 1 }
    };
    gpu_ir_shader_function function = {
        .id = 10, .result_type_id = 1, .function_type_id = 6,
        .first_block = 0, .block_count = 1, .first_instruction = 2,
        .instruction_count = 2
    };
    gpu_ir_shader_block block = {
        .function_index = 0, .label_id = 11, .first_instruction = 2,
        .instruction_count = 2, .terminator_opcode = GPU_IR_SHADER_OP_RETURN
    };
    gpu_ir_shader_operand operand = { GPU_IR_SHADER_OPERAND_ID, 21 };
    uint32_t literal = 1;
    gpu_ir_shader_instruction instructions[] = {
        { .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .opcode = GPU_IR_SHADER_OP_VARIABLE,
          .result_id = 20, .result_type_id = 5, .function_index = UINT32_MAX, .block_index = UINT32_MAX,
          .storage_class = GPU_IR_SHADER_STORAGE_OUTPUT, .source_storage_class = 3,
          .location = 0, .has_location = 1, .entry_point_interface = 1 },
        { .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .opcode = GPU_IR_SHADER_OP_CONSTANT,
          .operation = GPU_IR_SHADER_CONSTANT_BOOL, .source_opcode = 41,
          .result_id = 21, .result_type_id = 2, .function_index = UINT32_MAX, .block_index = UINT32_MAX,
          .literal_word_count = 1 },
        { .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .opcode = GPU_IR_SHADER_OP_DISCARD,
          .result_id = 0, .result_type_id = 0, .function_index = 0, .block_index = 0,
          .operand_count = 1 },
        { .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .opcode = GPU_IR_SHADER_OP_RETURN,
          .source_opcode = 253, .function_index = 0, .block_index = 0 }
    };
    gpu_ir_shader_desc desc = {
        .source_api = GPU_IR_SHADER_SOURCE_SPIRV, .stage_mask = GPU_IR_STAGE_FRAGMENT,
        .entry_function_id = 10, .types = types, .type_count = sizeof types / sizeof types[0],
        .functions = &function, .function_count = 1, .blocks = &block, .block_count = 1,
        .instructions = instructions, .instruction_count = sizeof instructions / sizeof instructions[0],
        .operands = &operand, .operand_count = 1, .literal_words = &literal, .literal_word_count = 1
    };
    gpu_ir_shader *shader = gpu_ir_shader_create(&desc);
    char *msl = NULL;
    size_t msl_size = 0;
    int rc = shader ? gpu_ir_msl_emit_fragment(shader, &msl, &msl_size) : GPU_IR_INVALID;
    int valid = rc == GPU_IR_OK && msl && msl_size &&
        strstr(msl, "if (true) discard_fragment();") && strstr(msl, "return vx_io_20;");
    if (valid && output_path) {
        FILE *output = fopen(output_path, "wb");
        valid = output && fwrite(msl, 1, msl_size, output) == msl_size;
        if (output && fclose(output)) valid = 0;
    }
    free(msl);
    gpu_ir_shader_release(shader);
    if (!valid) {
        fprintf(stderr, "dxil_ir: common fragment discard MSL emission returned %d\n", rc);
        return check("common IR conditional discard did not emit fragment MSL");
    }
    desc.stage_mask = GPU_IR_STAGE_VERTEX;
    shader = gpu_ir_shader_create(&desc);
    if (shader) {
        gpu_ir_shader_release(shader);
        return check("common IR accepted fragment discard in a vertex shader");
    }
    desc.stage_mask = GPU_IR_STAGE_FRAGMENT;
    operand.value = 20;
    shader = gpu_ir_shader_create(&desc);
    if (shader) {
        gpu_ir_shader_release(shader);
        return check("common IR accepted a non-boolean discard condition");
    }
    puts("dxil_ir: common fragment discard emits discard_fragment in MSL: ok");
    return 0;
}

static int execute_atomic_shader_on_metal(const gpu_ir_shader *shader, const uint32_t initial[4],
                                          uint32_t expected0, uint32_t expected1,
                                          uint32_t alternate1) {
    const size_t initial_size = sizeof(uint32_t) * 4u;
    gpu_ir_metal_device *device = gpu_ir_metal_device_create();
    gpu_ir_resource *buffer = device ? gpu_ir_metal_buffer_create(device, initial_size, initial) : NULL;
    gpu_ir_descriptor_binding binding = {
        .set = 0, .binding = 0, .kind = GPU_IR_DESCRIPTOR_STORAGE_RESOURCE, .count = 1,
        .stage_mask = GPU_IR_STAGE_COMPUTE,
        .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
        .binding_namespace = 1
    };
    gpu_ir_pipeline_layout_desc layout_description = { .bindings = &binding, .binding_count = 1 };
    gpu_ir_pipeline_layout *layout = gpu_ir_pipeline_layout_create(&layout_description);
    char error[512] = { 0 };
    gpu_ir_pipeline *pipeline = device && layout ?
        gpu_ir_metal_compute_pipeline_create(device, shader, layout, error, sizeof error) : NULL;
    gpu_ir_descriptor_value value = {
        .set = 0, .binding_namespace = 1, .binding = 0, .array_element = 0,
        .kind = GPU_IR_DESCRIPTOR_STORAGE_RESOURCE, .stage_mask = GPU_IR_STAGE_COMPUTE,
        .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
        .resource = buffer, .range = initial_size
    };
    gpu_ir_descriptor_state_desc state_description = { .values = &value, .value_count = 1 };
    gpu_ir_descriptor_state *state = pipeline && buffer ?
        gpu_ir_descriptor_state_create(layout, &state_description) : NULL;
    gpu_ir_stream stream;
    gpu_ir_stream_init(&stream);
    int rc = pipeline && state ? gpu_ir_record_bind_descriptor_state(&stream, state, NULL) : GPU_IR_INVALID;
    gpu_ir_resource_access access = {
        .resource = buffer, .stage_mask = GPU_IR_STAGE_COMPUTE,
        .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE
    };
    if (rc == GPU_IR_OK)
        rc = gpu_ir_record_dispatch_with_accesses(&stream, pipeline, 64, 1, 1, &access, 1, NULL);
    if (rc == GPU_IR_OK) rc = gpu_ir_metal_execute_stream(device, &stream);
    const uint32_t *result = buffer ? gpu_ir_metal_buffer_contents(buffer) : NULL;
    int result1_matches = result && result[1] == expected1;
    if (result && alternate1 != UINT32_MAX && result[1] == alternate1) result1_matches = 1;
    int ok = rc == GPU_IR_OK && result && result[0] == expected0 && result1_matches &&
        result[2] == initial[2] && result[3] == initial[3];
    if (!ok)
        fprintf(stderr, "dxil_ir: Metal atomic execution rc=%d results={%u,%u} expected={%u,%u}%s%s\n",
                rc, result ? result[0] : 0u, result ? result[1] : 0u, expected0, expected1,
                error[0] ? ": " : "", error);
    gpu_ir_stream_destroy(&stream);
    gpu_ir_descriptor_state_release(state);
    gpu_ir_pipeline_release(pipeline);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_resource_release(buffer);
    gpu_ir_metal_device_release(device);
    return ok;
}

static int execute_atomic_operation_on_metal(const gpu_ir_shader *shader, uint32_t operation) {
    uint32_t initial[4] = { 41u, 0u, 0xabcdef02u, 0xabcdef03u };
    uint32_t expected0 = 0, expected1 = 0, alternate1 = UINT32_MAX;
    switch (operation) {
    case GPU_IR_SHADER_ATOMIC_INTEGER_ADD: expected0 = 105u; expected1 = 4640u; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_AND: expected0 = 1u; expected1 = 0u; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_OR: expected0 = 41u; expected1 = 41u; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_XOR: expected0 = 41u; expected1 = 0u; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MINIMUM:
    case GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MINIMUM: expected0 = 1u; expected1 = 0u; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MAXIMUM:
    case GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MAXIMUM: expected0 = 41u; expected1 = 41u; break;
    case GPU_IR_SHADER_ATOMIC_INTEGER_EXCHANGE: expected0 = 1u; expected1 = 1u; alternate1 = 41u; break;
    default: break;
    }
    return execute_atomic_shader_on_metal(shader, initial, expected0, expected1, alternate1);
}

static int execute_atomic_compare_exchange_on_metal(const gpu_ir_shader *shader) {
    uint32_t initial[4] = { 41u, 0u, 0xabcdef02u, 0xabcdef03u };
    return execute_atomic_shader_on_metal(shader, initial, 1u, 1u, 41u);
}

static int execute_raw_buffer_shader_on_metal_with_ranges(const gpu_ir_shader *shader,
                                                          const uint32_t input_words[4],
                                                          const uint32_t expected_words[4],
                                                          size_t input_range, size_t output_range) {
    const uint32_t output_words[4] = { 0u, 0u, 0u, 0u };
    const size_t byte_size = 4u * sizeof input_words[0];
    if (input_range > byte_size || output_range > byte_size) return 0;
    gpu_ir_metal_device *device = gpu_ir_metal_device_create();
    gpu_ir_resource *input = device ? gpu_ir_metal_buffer_create(device, byte_size, input_words) : NULL;
    gpu_ir_resource *output = device ? gpu_ir_metal_buffer_create(device, byte_size, output_words) : NULL;
    const gpu_ir_descriptor_binding bindings[] = {
        { .set = 0, .binding = 0, .kind = GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE, .count = 1,
          .stage_mask = GPU_IR_STAGE_COMPUTE, .access_mask = GPU_IR_ACCESS_SHADER_READ, .binding_namespace = 0 },
        { .set = 0, .binding = 0, .kind = GPU_IR_DESCRIPTOR_STORAGE_RESOURCE, .count = 1,
          .stage_mask = GPU_IR_STAGE_COMPUTE,
          .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE, .binding_namespace = 1 }
    };
    gpu_ir_pipeline_layout_desc layout_description = { .bindings = bindings, .binding_count = 2 };
    gpu_ir_pipeline_layout *layout = gpu_ir_pipeline_layout_create(&layout_description);
    char error[512] = { 0 };
    gpu_ir_pipeline *pipeline = device && input && output && layout ?
        gpu_ir_metal_compute_pipeline_create(device, shader, layout, error, sizeof error) : NULL;
    gpu_ir_descriptor_value values[] = {
        { .set = 0, .binding_namespace = 0, .binding = 0, .array_element = 0,
          .kind = GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE, .stage_mask = GPU_IR_STAGE_COMPUTE,
          .access_mask = GPU_IR_ACCESS_SHADER_READ, .resource = input, .range = input_range },
        { .set = 0, .binding_namespace = 1, .binding = 0, .array_element = 0,
          .kind = GPU_IR_DESCRIPTOR_STORAGE_RESOURCE, .stage_mask = GPU_IR_STAGE_COMPUTE,
          .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE,
          .resource = output, .range = output_range }
    };
    gpu_ir_descriptor_state_desc state_description = { .values = values, .value_count = 2 };
    gpu_ir_descriptor_state *state = pipeline ? gpu_ir_descriptor_state_create(layout, &state_description) : NULL;
    gpu_ir_stream stream;
    gpu_ir_stream_init(&stream);
    int rc = state ? gpu_ir_record_bind_descriptor_state(&stream, state, NULL) : GPU_IR_INVALID;
    gpu_ir_resource_access accesses[] = {
        { .resource = input, .stage_mask = GPU_IR_STAGE_COMPUTE, .access_mask = GPU_IR_ACCESS_SHADER_READ },
        { .resource = output, .stage_mask = GPU_IR_STAGE_COMPUTE,
          .access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE }
    };
    if (rc == GPU_IR_OK)
        rc = gpu_ir_record_dispatch_with_accesses(&stream, pipeline, 1, 1, 1, accesses, 2, NULL);
    if (rc == GPU_IR_OK) rc = gpu_ir_metal_execute_stream(device, &stream);
    const uint32_t *result = output ? gpu_ir_metal_buffer_contents(output) : NULL;
    int output_matches = result != NULL;
    for (uint32_t i = 0; result && expected_words && i < 4; i++)
        output_matches &= result[i] == expected_words[i];
    int ok = rc == GPU_IR_OK && output_matches;
    if (!ok)
        fprintf(stderr, "dxil_ir: Metal raw-buffer execution rc=%d output={%u,%u,%u,%u} expected={%u,%u,%u,%u}%s%s\n",
                rc, result ? result[0] : 0u, result ? result[1] : 0u, result ? result[2] : 0u,
                result ? result[3] : 0u, expected_words ? expected_words[0] : 0u,
                expected_words ? expected_words[1] : 0u, expected_words ? expected_words[2] : 0u,
                expected_words ? expected_words[3] : 0u, error[0] ? ": " : "", error);
    gpu_ir_stream_destroy(&stream);
    gpu_ir_descriptor_state_release(state);
    gpu_ir_pipeline_release(pipeline);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_resource_release(input);
    gpu_ir_resource_release(output);
    gpu_ir_metal_device_release(device);
    return ok;
}

static int execute_raw_buffer_shader_on_metal(const gpu_ir_shader *shader, const uint32_t input_words[4],
                                              const uint32_t expected_words[4]) {
    const size_t byte_size = 4u * sizeof input_words[0];
    return execute_raw_buffer_shader_on_metal_with_ranges(shader, input_words, expected_words,
                                                          byte_size, byte_size);
}

static int execute_raw_buffer_load_shader_on_metal(const gpu_ir_shader *shader, uint32_t expected_value) {
    const uint32_t input_words[4] = { 11u, 22u, 33u, 44u };
    const uint32_t expected_words[4] = { 0u, expected_value, 0u, 0u };
    return execute_raw_buffer_shader_on_metal(shader, input_words, expected_words);
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

static int translate_vector_bit_intrinsics(void) {
    uint32_t main_parameters[] = { 2 };
    uint32_t unary_parameters[] = { 1, 2 };
    dxil_type types[5] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_VECTOR, .elem = 1, .count = 2 },
        { .kind = DXT_FUNCTION, .elem = 0, .count = 1, .members = main_parameters },
        { .kind = DXT_FUNCTION, .elem = 2, .count = 2, .members = unary_parameters }
    };
    dxil_value values[11] = {
        { .kind = DXV_FUNCTION, .type = 3, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 4, .name = "dx.op.unary.v2i32" },
        { .kind = DXV_FUNCTION, .type = 4, .name = "dx.op.unary.v2i32" },
        { .kind = DXV_FUNCTION, .type = 4, .name = "dx.op.unary.v2i32" },
        { .kind = DXV_FUNCTION, .type = 4, .name = "dx.op.unary.v2i32" },
        { .kind = DXV_FUNCTION, .type = 4, .name = "dx.op.unary.v2i32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 30 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 31 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 32 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 33 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 34 }
    };
    dxil_value locals[8] = {
        { .kind = DXV_ARG, .type = 2, .u.index = 0 },
        { .kind = DXV_INST, .type = 2, .u.index = 0 },
        { .kind = DXV_INST, .type = 2, .u.index = 1 },
        { .kind = DXV_INST, .type = 2, .u.index = 2 },
        { .kind = DXV_INST, .type = 2, .u.index = 3 },
        { .kind = DXV_INST, .type = 2, .u.index = 4 }
    };
    uint32_t call_operands[5][3] = {
        { 1, 6, 11 }, { 2, 7, 11 }, { 3, 8, 11 }, { 4, 9, 11 }, { 5, 10, 11 }
    };
    dxil_inst instructions[6] = { 0 };
    for (uint32_t i = 0; i < 5; i++) {
        instructions[i] = (dxil_inst){ .op = DXI_CALL, .type = 2, .result = 12 + i, .block = 0,
            .n = 3, .ops = call_operands[i] };
    }
    instructions[5] = (dxil_inst){ .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 };
    dxil_function function = { .type = 3, .value = 0, .first_arg = 11, .nblocks = 1,
        .ninsts = 6, .insts = instructions, .nlocals = 6, .locals = locals };
    dxil_module module = { .shader_kind = 1, .ntypes = 5, .types = types,
        .nmodule_values = 11, .values = values, .nfunctions = 1, .functions = &function };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: vector bit-intrinsic lowering returned %d\n", rc);
        return check("DXIL vector bit intrinsics did not lower into common IR");
    }
    const uint32_t expected_operations[5] = {
        GPU_IR_SHADER_ARITH_BIT_REVERSE, GPU_IR_SHADER_ARITH_BIT_COUNT,
        GPU_IR_SHADER_ARITH_BIT_FIND_LSB, GPU_IR_SHADER_ARITH_BIT_FIND_MSB_UNSIGNED,
        GPU_IR_SHADER_ARITH_BIT_FIND_MSB_SIGNED
    };
    uint32_t seen = 0;
    int valid = 1;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC) continue;
        const gpu_ir_shader_type *vector = gpu_ir_shader_find_type(shader, instruction->result_type_id);
        const gpu_ir_shader_type *element = vector && vector->kind == GPU_IR_SHADER_TYPE_VECTOR ?
            gpu_ir_shader_find_type(shader, vector->element_type_id) : NULL;
        if (seen >= 5 || instruction->operation != expected_operations[seen] ||
            instruction->operand_count != 1 || !vector || vector->kind != GPU_IR_SHADER_TYPE_VECTOR ||
            vector->element_count != 2 || !element || element->kind != GPU_IR_SHADER_TYPE_INT ||
            element->bit_width != 32) {
            valid = 0;
            break;
        }
        seen++;
    }
    gpu_ir_shader_release(shader);
    if (!valid || seen != 5) return check("DXIL vector bit operations lost their result shape or opcode");
    puts("dxil_ir: DXIL 32-bit vector bit reverse/count/first-bit operations lower to common IR: ok");
    return 0;
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
        { .kind = DXV_CONST_NULL, .type = 1 },
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

static int translate_compute_subgroup_queries(const char *output_path) {
    uint32_t unary_parameters[] = { 1 };
    uint32_t vote_parameters[] = { 1, 4 };
    uint32_t wave_parameters[] = { 1, 1, 1, 1 };
    uint32_t bit_parameters[] = { 1, 1, 1 };
    dxil_type types[8] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = 1, .count = 1, .members = unary_parameters },
        { .kind = DXT_INT, .bits = 1 },
        { .kind = DXT_FUNCTION, .elem = 4, .count = 2, .members = vote_parameters },
        { .kind = DXT_FUNCTION, .elem = 1, .count = 4, .members = wave_parameters },
        { .kind = DXT_FUNCTION, .elem = 1, .count = 3, .members = bit_parameters }
    };
    dxil_value values[26] = {
        { .kind = DXV_FUNCTION, .type = 2, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 3, .name = "dx.op.waveGetLaneIndex" },
        { .kind = DXV_FUNCTION, .type = 3, .name = "dx.op.waveGetLaneCount" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 111 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 112 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 8 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_FUNCTION, .type = 5, .name = "dx.op.waveAnyTrue" },
        { .kind = DXV_FUNCTION, .type = 5, .name = "dx.op.waveAllTrue" },
        { .kind = DXV_CONST_INT, .type = 4, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 4, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 113 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 114 },
        { .kind = DXV_FUNCTION, .type = 6, .name = "dx.op.waveActiveOp.i32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 119 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 2 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 3 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_FUNCTION, .type = 7, .name = "dx.op.waveActiveBit.i32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 120 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 0 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 2 }
    };
    dxil_value locals[11] = {
        { .kind = DXV_INST, .type = 1, .u.index = 0 },
        { .kind = DXV_INST, .type = 1, .u.index = 1 },
        { .kind = DXV_INST, .type = 4, .u.index = 2 },
        { .kind = DXV_INST, .type = 4, .u.index = 3 },
        { .kind = DXV_INST, .type = 1, .u.index = 4 },
        { .kind = DXV_INST, .type = 1, .u.index = 5 },
        { .kind = DXV_INST, .type = 1, .u.index = 6 },
        { .kind = DXV_INST, .type = 1, .u.index = 7 },
        { .kind = DXV_INST, .type = 1, .u.index = 8 },
        { .kind = DXV_INST, .type = 1, .u.index = 9 },
        { .kind = DXV_INST, .type = 1, .u.index = 10 }
    };
    uint32_t lane_index_args[] = { 1, 3 };
    uint32_t lane_count_args[] = { 2, 4 };
    uint32_t any_args[] = { 9, 13, 11 };
    uint32_t all_args[] = { 10, 14, 12 };
    uint32_t wave_min_args[] = { 15, 16, 26, 17, 18 };
    uint32_t wave_max_args[] = { 15, 16, 26, 19, 20 };
    uint32_t wave_umin_args[] = { 15, 16, 26, 17, 20 };
    uint32_t wave_smax_args[] = { 15, 16, 26, 19, 18 };
    uint32_t wave_band_args[] = { 21, 22, 6, 23 };
    uint32_t wave_bor_args[] = { 21, 22, 6, 24 };
    uint32_t wave_bxor_args[] = { 21, 22, 6, 25 };
    dxil_inst instructions[12] = {
        { .op = DXI_CALL, .type = 1, .result = 26, .block = 0, .n = 2, .ops = lane_index_args },
        { .op = DXI_CALL, .type = 1, .result = 27, .block = 0, .n = 2, .ops = lane_count_args },
        { .op = DXI_CALL, .type = 4, .result = 28, .block = 0, .n = 3, .ops = any_args },
        { .op = DXI_CALL, .type = 4, .result = 29, .block = 0, .n = 3, .ops = all_args },
        { .op = DXI_CALL, .type = 1, .result = 30, .block = 0, .n = 5, .ops = wave_min_args },
        { .op = DXI_CALL, .type = 1, .result = 31, .block = 0, .n = 5, .ops = wave_max_args },
        { .op = DXI_CALL, .type = 1, .result = 32, .block = 0, .n = 5, .ops = wave_umin_args },
        { .op = DXI_CALL, .type = 1, .result = 33, .block = 0, .n = 5, .ops = wave_smax_args },
        { .op = DXI_CALL, .type = 1, .result = 34, .block = 0, .n = 4, .ops = wave_band_args },
        { .op = DXI_CALL, .type = 1, .result = 35, .block = 0, .n = 4, .ops = wave_bor_args },
        { .op = DXI_CALL, .type = 1, .result = 36, .block = 0, .n = 4, .ops = wave_bxor_args },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function functions[7] = {
        { .type = 2, .value = 0, .nblocks = 1, .ninsts = 12, .insts = instructions,
          .nlocals = 11, .locals = locals },
        { .type = 3, .value = 1, .is_declaration = 1 },
        { .type = 3, .value = 2, .is_declaration = 1 },
        { .type = 5, .value = 9, .is_declaration = 1 },
        { .type = 5, .value = 10, .is_declaration = 1 },
        { .type = 6, .value = 15, .is_declaration = 1 },
        { .type = 7, .value = 21, .is_declaration = 1 }
    };
    dxil_md metadata[8] = { 0 };
    uint32_t dimensions[] = { 3, 4, 5 };
    uint32_t properties[] = { 2, 6 };
    uint32_t entry_ops[] = { 1, 0, 0, 0, 7 };
    uint32_t entry_list[] = { 7 };
    metadata[0] = (dxil_md){ .kind = DXM_VALUE, .value = 0 };
    metadata[1] = (dxil_md){ .kind = DXM_VALUE, .value = 5 };
    metadata[2] = (dxil_md){ .kind = DXM_VALUE, .value = 6 };
    metadata[3] = (dxil_md){ .kind = DXM_VALUE, .value = 7 };
    metadata[4] = (dxil_md){ .kind = DXM_VALUE, .value = 8 };
    metadata[5] = (dxil_md){ .kind = DXM_NODE, .n = 3, .ops = dimensions };
    metadata[6] = (dxil_md){ .kind = DXM_NODE, .n = 2, .ops = properties };
    metadata[7] = (dxil_md){ .kind = DXM_NODE, .n = 5, .ops = entry_ops };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_list };
    dxil_module module = { .shader_kind = 5, .ntypes = 8, .types = types,
        .nmodule_values = 26, .values = values, .nfunctions = 7, .functions = functions,
        .nmd = 8, .md = metadata, .nnamed = 1, .named = &named };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: common subgroup-query lowering returned %d\n", rc);
        return check("DXIL WaveGetLaneIndex/WaveGetLaneCount did not lower to common IR");
    }
    uint32_t builtin_mask = 0, copy_count = 0, vote_any_count = 0, vote_all_count = 0;
    uint32_t wave_smin_count = 0, wave_umin_count = 0;
    uint32_t wave_smax_count = 0, wave_umax_count = 0;
    uint32_t wave_band_count = 0, wave_bor_count = 0, wave_bxor_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) continue;
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE && instruction->has_builtin) {
            if (instruction->source_builtin == 111 &&
                instruction->builtin == GPU_IR_SHADER_BUILTIN_SUBGROUP_LOCAL_INVOCATION_ID)
                builtin_mask |= 1u;
            else if (instruction->source_builtin == 112 &&
                     instruction->builtin == GPU_IR_SHADER_BUILTIN_SUBGROUP_SIZE)
                builtin_mask |= 2u;
        }
        if (instruction->opcode == GPU_IR_SHADER_OP_COPY && instruction->source_opcode == DXI_CALL)
            copy_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_ANY) {
            const gpu_ir_shader_operand *input = gpu_ir_shader_operand_at(shader, instruction->first_operand);
            if (instruction->source_opcode == DXI_CALL && instruction->operand_count == 1 && input &&
                input->kind == GPU_IR_SHADER_OPERAND_ID) vote_any_count++;
        }
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_ALL) {
            const gpu_ir_shader_operand *input = gpu_ir_shader_operand_at(shader, instruction->first_operand);
            if (instruction->source_opcode == DXI_CALL && instruction->operand_count == 1 && input &&
                input->kind == GPU_IR_SHADER_OPERAND_ID) vote_all_count++;
        }
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_SMIN &&
                instruction->source_opcode == DXI_CALL && instruction->operand_count == 1)
            wave_smin_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_UMIN &&
                instruction->source_opcode == DXI_CALL && instruction->operand_count == 1)
            wave_umin_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_SMAX &&
                instruction->source_opcode == DXI_CALL && instruction->operand_count == 1)
            wave_smax_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_UMAX &&
                instruction->source_opcode == DXI_CALL && instruction->operand_count == 1)
            wave_umax_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_BIT_AND &&
                instruction->source_opcode == DXI_CALL && instruction->operand_count == 1)
            wave_band_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_BIT_OR &&
                instruction->source_opcode == DXI_CALL && instruction->operand_count == 1)
            wave_bor_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
                instruction->operation == GPU_IR_SHADER_ARITH_SUBGROUP_BIT_XOR &&
                instruction->source_opcode == DXI_CALL && instruction->operand_count == 1)
            wave_bxor_count++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    if (builtin_mask == 3u && copy_count == 2u && vote_any_count == 1u && vote_all_count == 1u &&
        wave_smin_count == 1u && wave_umin_count == 1u &&
        wave_smax_count == 1u && wave_umax_count == 1u &&
        wave_band_count == 1u && wave_bor_count == 1u && wave_bxor_count == 1u)
        rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    else rc = GPU_IR_INVALID;
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK || !msl || !msl_size ||
        !strstr(msl, "[[thread_index_in_simdgroup]]") ||
        !strstr(msl, "[[threads_per_simdgroup]]") ||
        !strstr(msl, "simd_any(true)") || !strstr(msl, "simd_all(false)") ||
        !strstr(msl, "simd_min(") || !strstr(msl, "simd_max(") ||
        !strstr(msl, "as_type<uint>(simd_min(as_type<int>(") ||
        !strstr(msl, "as_type<uint>(simd_max(as_type<int>(") ||
        !strstr(msl, "simd_and(") || !strstr(msl, "simd_or(") || !strstr(msl, "simd_xor(")) {
        free(msl);
        return check("common MSL did not preserve DXIL subgroup queries as Metal execution built-ins");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) {
            free(msl);
            return check("could not write the common subgroup-query MSL fixture");
        }
    }
    free(msl);
    puts("dxil_ir: wave lane queries, active any/all votes, integer min/max, and bitwise reductions lower through API-neutral subgroup IR to MSL: ok");
    return 0;
}

static int reject_unsupported_intrinsic(void) {
    dxil_type types[2] = { { .kind = DXT_VOID }, { .kind = DXT_FUNCTION, .elem = 0 } };
    dxil_value values[2] = {
        { .kind = DXV_FUNCTION, .type = 1, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 1, .name = "unsupported.external" }
    };
    uint32_t call_args[] = { 1 };
    dxil_inst instructions[2] = {
        { .op = DXI_CALL, .type = 0, .result = UINT32_MAX, .block = 0, .n = 1, .ops = call_args },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function functions[2] = {
        { .type = 1, .value = 0, .nblocks = 1, .ninsts = 2, .insts = instructions },
        { .type = 1, .value = 1, .is_declaration = 1 }
    };
    dxil_module module = { .shader_kind = 1, .ntypes = 2, .types = types,
        .nmodule_values = 2, .values = values, .nfunctions = 2, .functions = functions };
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

static int translate_vector_classification_adapter(void) {
    uint32_t main_parameters[] = { 3 };
    uint32_t unary_parameters[] = { 1, 3 };
    dxil_type types[8] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_FLOAT },
        { .kind = DXT_VECTOR, .elem = 2, .count = 2 },
        { .kind = DXT_INT, .bits = 1 },
        { .kind = DXT_VECTOR, .elem = 4, .count = 2 },
        { .kind = DXT_FUNCTION, .elem = 0, .count = 1, .members = main_parameters },
        { .kind = DXT_FUNCTION, .elem = 5, .count = 2, .members = unary_parameters }
    };
    dxil_value values[3] = {
        { .kind = DXV_FUNCTION, .type = 6, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 7, .name = "dx.op.unary.v2i1" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 8 }
    };
    uint32_t call_operands[] = { 1, 2, 3 };
    dxil_inst instructions[2] = {
        { .op = DXI_CALL, .type = 5, .result = 4, .block = 0, .n = 3, .ops = call_operands },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_value locals[2] = {
        { .kind = DXV_ARG, .type = 3, .u.index = 0 },
        { .kind = DXV_INST, .type = 5, .u.index = 0 }
    };
    dxil_function function = { .type = 6, .value = 0, .first_arg = 3, .nblocks = 1, .ninsts = 2,
        .insts = instructions, .nlocals = 2, .locals = locals };
    dxil_module module = { .shader_kind = 1, .ntypes = 8, .types = types, .nmodule_values = 3,
        .values = values, .nfunctions = 1, .functions = &function };
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: vector classification lowering returned %d\n", rc);
        return check("DXIL vector classification intrinsic did not lower to common IR");
    }
    int found = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_COMPARE ||
            instruction->operation != GPU_IR_SHADER_COMPARE_IS_NAN) continue;
        const gpu_ir_shader_type *result = gpu_ir_shader_find_type(shader, instruction->result_type_id);
        const gpu_ir_shader_type *element = result ? gpu_ir_shader_find_type(shader, result->element_type_id) : NULL;
        found = instruction->operand_count == 1 && instruction->flags == GPU_IR_SHADER_FLAG_FLOAT &&
            result && result->kind == GPU_IR_SHADER_TYPE_VECTOR && result->element_count == 2 &&
            element && element->kind == GPU_IR_SHADER_TYPE_BOOL;
    }
    gpu_ir_shader_release(shader);
    if (!found) return check("DXIL vector classification lost its bool-vector shape in common IR");
    puts("dxil_ir: vector unary classification keeps its bool-vector shape: ok");
    return 0;
}

static int translate_vector_compare_predicates(void) {
    static const uint32_t expected_operations[] = {
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
        GPU_IR_SHADER_COMPARE_UNORDERED_NOT_EQUAL
    };
    uint32_t function_parameters[] = { 2, 2 };
    dxil_type types[6] = {
        { .kind = DXT_VOID },
        { .kind = DXT_FLOAT },
        { .kind = DXT_VECTOR, .elem = 1, .count = 2 },
        { .kind = DXT_INT, .bits = 1 },
        { .kind = DXT_VECTOR, .elem = 3, .count = 2 },
        { .kind = DXT_FUNCTION, .elem = 0, .count = 2, .members = function_parameters }
    };
    dxil_value values[] = { { .kind = DXV_FUNCTION, .type = 5, .name = "main" } };
    uint32_t compare_operands[] = { 1, 2 };
    dxil_value locals[2u + sizeof expected_operations / sizeof expected_operations[0]];
    dxil_inst instructions[sizeof expected_operations / sizeof expected_operations[0] + 1u];
    locals[0] = (dxil_value){ .kind = DXV_ARG, .type = 2, .u.index = 0 };
    locals[1] = (dxil_value){ .kind = DXV_ARG, .type = 2, .u.index = 1 };
    for (uint32_t i = 0; i < sizeof expected_operations / sizeof expected_operations[0]; i++) {
        locals[2u + i] = (dxil_value){ .kind = DXV_INST, .type = 4, .u.index = i };
        instructions[i] = (dxil_inst){ .op = DXI_CMP, .sub = i + 1u, .type = 4,
            .result = 3u + i, .block = 0, .n = 2, .ops = compare_operands };
    }
    instructions[sizeof expected_operations / sizeof expected_operations[0]] =
        (dxil_inst){ .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 };
    dxil_function function = { .type = 5, .value = 0, .first_arg = 1, .nblocks = 1,
        .ninsts = sizeof instructions / sizeof instructions[0], .insts = instructions,
        .nlocals = sizeof locals / sizeof locals[0], .locals = locals };
    dxil_module module = { .shader_kind = 1, .ntypes = 6, .types = types,
        .nmodule_values = 1, .values = values, .nfunctions = 1, .functions = &function };
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: float2 compare predicates lowered with %d\n", rc);
        return check("DXIL vector floating comparisons did not lower to common IR");
    }
    uint32_t seen = 0;
    int valid = 1;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_COMPARE) continue;
        const gpu_ir_shader_type *result = gpu_ir_shader_find_type(shader, instruction->result_type_id);
        const gpu_ir_shader_type *element = result ? gpu_ir_shader_find_type(shader, result->element_type_id) : NULL;
        if (seen >= sizeof expected_operations / sizeof expected_operations[0] ||
            instruction->operation != expected_operations[seen] || instruction->operand_count != 2 ||
            instruction->flags != GPU_IR_SHADER_FLAG_FLOAT || !result ||
            result->kind != GPU_IR_SHADER_TYPE_VECTOR || result->element_count != 2 ||
            !element || element->kind != GPU_IR_SHADER_TYPE_BOOL) {
            valid = 0;
            break;
        }
        seen++;
    }
    gpu_ir_shader_release(shader);
    if (!valid || seen != sizeof expected_operations / sizeof expected_operations[0])
        return check("DXIL vector comparison predicates lost their operation or bool-vector shape");
    puts("dxil_ir: all 14 DXIL float comparison predicates keep bool2 shape in common IR: ok");
    return 0;
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

static int reject_wave_shader_unsupported(void) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/wave_ops_partial_cs.dxil", &module)) return 1;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    gpu_ir_shader_release(shader);
    dxil_free(&module);
    if (rc != GPU_IR_UNSUPPORTED || shader)
        return check("DXIL wave operations did not remain a clean common-IR fallback");
    return 0;
}

static int quad_intrinsic_fixture(uint32_t intrinsic, int selector_kind, int64_t selector,
                                  uint32_t operand_count, const char *expected, int should_translate) {
    dxil_type types[5] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = 1 },
        { .kind = DXT_INT, .bits = 8 }
    };
    dxil_value values[5] = {
        { .kind = DXV_FUNCTION, .type = 2, .u.index = 0 },
        { .kind = DXV_FUNCTION, .type = 3, .u.index = 1, .name = "dx.op.quadOp.i32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = intrinsic },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 11 },
        { .kind = DXV_CONST_INT, .type = intrinsic == 123 ? 4u : 1u, .u.i = selector }
    };
    if (selector_kind == 0) values[4].kind = DXV_CONST_UNDEF;
    uint32_t operands[] = { 1, 2, 3, 4 };
    dxil_inst instructions[2] = {
        { .op = DXI_CALL, .type = 1, .result = 5, .block = 0, .n = operand_count, .ops = operands },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_value locals[] = { { .kind = DXV_INST, .type = 1, .u.index = 0 } };
    dxil_function functions[2] = {
        { .type = 2, .value = 0, .nblocks = 1, .ninsts = 2, .insts = instructions,
          .nlocals = 1, .locals = locals },
        { .type = 3, .value = 1, .is_declaration = 1 }
    };
    dxil_module module = { .shader_kind = 5, .ntypes = 5, .types = types,
        .nmodule_values = 5, .values = values, .nfunctions = 2, .functions = functions };
    dxil_msl output = { 0 };
    int rc = dxil_to_msl(&module, &output);
    int valid = should_translate ? rc == 0 && output.msl && strstr(output.msl, expected) :
                                   rc != 0 && !output.msl && strstr(output.error, "QuadOp");
    if (!valid && should_translate && output.msl)
        fprintf(stderr, "dxil_ir: quad fixture %u selector %lld emitted:\n%s\n", intrinsic,
                (long long)selector, output.msl);
    dxil_msl_free(&output);
    return valid ? 0 : check("DXIL quad intrinsic accepted an invalid selector or emitted the wrong Metal shuffle");
}

static int test_quad_intrinsic_validation(void) {
    if (quad_intrinsic_fixture(123, 1, 0, 4, "quad_shuffle_xor(11u, ushort(1))", 1) ||
        quad_intrinsic_fixture(123, 1, 1, 4, "quad_shuffle_xor(11u, ushort(2))", 1) ||
        quad_intrinsic_fixture(123, 1, 2, 4, "quad_shuffle_xor(11u, ushort(3))", 1) ||
        quad_intrinsic_fixture(123, 1, 3, 4, "", 0) ||
        quad_intrinsic_fixture(123, 0, 0, 4, "", 0) ||
        quad_intrinsic_fixture(123, 1, 0, 3, "", 0) ||
        quad_intrinsic_fixture(122, 1, 2, 4, "quad_shuffle(11u, ushort(2u))", 1))
        return 1;
    puts("dxil_ir: quad shuffle selectors and operands are validated before MSL emission: ok");
    return 0;
}

static int wave_query_fixture(uint32_t intrinsic, uint32_t result_type, uint32_t input_type,
                              uint32_t operand_count, const char *expected, const char *expected_error) {
    dxil_type types[5] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 1 },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_FUNCTION, .elem = 0 },
        { .kind = DXT_FUNCTION, .elem = result_type }
    };
    dxil_value values[5] = {
        { .kind = DXV_FUNCTION, .type = 3, .u.index = 0 },
        { .kind = DXV_FUNCTION, .type = 4, .u.index = 1, .name = "dx.op.waveQuery" },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = intrinsic },
        { .kind = DXV_CONST_INT, .type = input_type, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 2, .u.i = 0 }
    };
    uint32_t operands[] = { 1, 2, 3, 4 };
    dxil_inst instructions[2] = {
        { .op = DXI_CALL, .type = result_type, .result = 5, .block = 0,
          .n = operand_count, .ops = operands },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_value locals[] = { { .kind = DXV_INST, .type = result_type, .u.index = 0 } };
    dxil_function functions[2] = {
        { .type = 3, .value = 0, .nblocks = 1, .ninsts = 2, .insts = instructions,
          .nlocals = 1, .locals = locals },
        { .type = 4, .value = 1, .is_declaration = 1 }
    };
    dxil_module module = { .shader_kind = 5, .ntypes = 5, .types = types,
        .nmodule_values = 5, .values = values, .nfunctions = 2, .functions = functions };
    dxil_msl output = { 0 };
    int rc = dxil_to_msl(&module, &output);
    int valid = expected_error ? rc != 0 && !output.msl && strstr(output.error, expected_error) :
                                 rc == 0 && output.msl && strstr(output.msl, expected);
    if (!valid && output.msl)
        fprintf(stderr, "dxil_ir: wave query %u emitted unexpected MSL:\n%s\n", intrinsic, output.msl);
    dxil_msl_free(&output);
    return valid ? 0 : check("DXIL wave query accepted an invalid signature or rejected a valid signature");
}

static int test_wave_query_validation(void) {
    if (wave_query_fixture(110, 1, 0, 2, "simd_ballot", NULL) ||
        wave_query_fixture(111, 2, 0, 2, "vx_lane", NULL) ||
        wave_query_fixture(112, 2, 0, 2, "vx_lanes", NULL) ||
        wave_query_fixture(113, 1, 1, 3, "simd_ballot", NULL) ||
        wave_query_fixture(114, 1, 1, 3, "simd_ballot", NULL) ||
        wave_query_fixture(135, 2, 1, 3, "popcount(uint(vx_v))", NULL) ||
        wave_query_fixture(136, 2, 1, 3, "vx_wbc_true", NULL) ||
        wave_query_fixture(110, 2, 0, 2, NULL, "WaveIsFirstLane") ||
        wave_query_fixture(111, 2, 0, 3, NULL, "WaveGetLaneIndex") ||
        wave_query_fixture(112, 1, 0, 2, NULL, "WaveGetLaneCount") ||
        wave_query_fixture(113, 1, 0, 2, NULL, "WaveActiveVote") ||
        wave_query_fixture(113, 1, 2, 3, NULL, "WaveActiveVote") ||
        wave_query_fixture(114, 2, 1, 3, NULL, "WaveActiveVote") ||
        wave_query_fixture(135, 2, 2, 3, NULL, "WaveActiveCountBits") ||
        wave_query_fixture(136, 1, 1, 3, NULL, "WavePrefixCountBits"))
        return 1;
    puts("dxil_ir: basic wave query signatures are validated before MSL emission: ok");
    return 0;
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

static int set_texture_dimensions_mip_level(dxil_module *module, uint32_t mip_level) {
    if (!module) return 0;
    dxil_function *function = NULL;
    for (uint32_t i = 0; i < module->nfunctions; i++)
        if (!module->functions[i].is_declaration) { function = &module->functions[i]; break; }
    if (!function) return 0;
    uint32_t mip_value_id = UINT32_MAX;
    uint32_t value_count = module->nmodule_values + function->nlocals;
    for (uint32_t id = 0; id < value_count; id++) {
        const dxil_value *value = dxil_value_at(module, function, id);
        const dxil_type *type = value && value->type < module->ntypes ? &module->types[value->type] : NULL;
        if (value && value->kind == DXV_CONST_INT && value->u.i == mip_level &&
            type && type->kind == DXT_INT && type->bits == 32) {
            mip_value_id = id;
            break;
        }
    }
    if (mip_value_id == UINT32_MAX) return 0;
    uint32_t matches = 0;
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op == DXI_CALL && dxil_dxop(module, function, instruction) == 72) {
            if (instruction->n != 4 || !instruction->ops) return 0;
            instruction->ops[3] = mip_value_id;
            matches++;
        }
    }
    return matches == 1;
}

static int set_texture_dimensions_array_size(dxil_module *module) {
    const dxil_named_md *named = dxil_named(module, "dx.resources");
    if (!named || named->n != 1 || !named->nodes || named->nodes[0] >= module->nmd) return 0;
    dxil_md *lists = &module->md[named->nodes[0]];
    dxil_md *resources = mutable_metadata_operand(module, lists, 0);
    if (!resources || resources->kind != DXM_NODE || resources->n != 1) return 0;
    dxil_md *record = mutable_metadata_operand(module, resources, 0);
    dxil_md *kind_md = mutable_metadata_operand(module, record, 6);
    if (!kind_md || kind_md->kind != DXM_VALUE || kind_md->value >= module->nmodule_values ||
        module->values[kind_md->value].kind != DXV_CONST_INT) return 0;
    dxil_function *function = NULL;
    for (uint32_t i = 0; i < module->nfunctions; i++)
        if (!module->functions[i].is_declaration) { function = &module->functions[i]; break; }
    if (!function) return 0;

    // The parser interns equal LLVM constants. Give the bufferStore index its own local constant
    // before changing the shared metadata constant from Texture2D (2) to Texture2DArray (7).
    uint32_t kind_value_id = kind_md->value;
    dxil_value *kind_value = &module->values[kind_value_id];
    if (kind_value->u.i != 2 || function->nlocals == UINT32_MAX) return 0;
    dxil_value *locals = realloc(function->locals, (size_t)(function->nlocals + 1u) * sizeof *locals);
    if (!locals) return 0;
    function->locals = locals;
    uint32_t replacement_id = module->nmodule_values + function->nlocals;
    function->locals[function->nlocals++] = (dxil_value){
        .kind = DXV_CONST_INT, .type = kind_value->type, .u.i = kind_value->u.i
    };
    uint32_t call_uses = 0;
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op != DXI_CALL || !instruction->ops) continue;
        for (uint32_t operand = 1; operand < instruction->n; operand++) {
            if (instruction->ops[operand] == kind_value_id) {
                instruction->ops[operand] = replacement_id;
                call_uses++;
            }
        }
    }
    if (!call_uses) return 0;
    kind_value->u.i = 7; // DXIL_RESOURCE_KIND_TEXTURE2DARRAY

    uint32_t matches = 0;
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op != DXI_EXTRACTVAL || instruction->n != 2 || instruction->nvals != 1 ||
            !instruction->ops || instruction->ops[1] != 3) continue;
        instruction->ops[1] = 2; // Texture2DArray GetDimensions returns ArraySize at field 2.
        matches++;
    }
    return matches == 1;
}

static int set_texture_load_array_resource(dxil_module *module) {
    const dxil_named_md *named = dxil_named(module, "dx.resources");
    if (!named || named->n != 1 || !named->nodes || named->nodes[0] >= module->nmd) return 0;
    dxil_md *lists = &module->md[named->nodes[0]];
    dxil_md *resources = mutable_metadata_operand(module, lists, 0);
    if (!resources || resources->kind != DXM_NODE || resources->n != 1) return 0;
    dxil_md *record = mutable_metadata_operand(module, resources, 0);
    dxil_md *kind_md = mutable_metadata_operand(module, record, 6);
    if (!kind_md || kind_md->kind != DXM_VALUE || kind_md->value >= module->nmodule_values) return 0;
    dxil_value *kind_value = &module->values[kind_md->value];
    if (kind_value->kind != DXV_CONST_INT || kind_value->u.i != 2) return 0;

    dxil_function *function = NULL;
    for (uint32_t i = 0; i < module->nfunctions; i++)
        if (!module->functions[i].is_declaration) { function = &module->functions[i]; break; }
    if (!function || function->nlocals == UINT32_MAX ||
        function->nlocals > UINT32_MAX - module->nmodule_values) return 0;
    dxil_value *locals = realloc(function->locals, (size_t)(function->nlocals + 1u) * sizeof *locals);
    if (!locals) return 0;
    function->locals = locals;
    uint32_t kind_value_id = kind_md->value;
    uint32_t replacement_id = module->nmodule_values + function->nlocals;
    function->locals[function->nlocals++] = (dxil_value){
        .kind = DXV_CONST_INT, .type = kind_value->type, .u.i = kind_value->u.i
    };
    uint32_t handle_kind_uses = 0;
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op != DXI_CALL || !instruction->ops) continue;
        for (uint32_t operand = 1; operand < instruction->n; operand++) {
            if (instruction->ops[operand] == kind_value_id) {
                instruction->ops[operand] = replacement_id;
                handle_kind_uses++;
            }
        }
    }
    if (!handle_kind_uses) return 0;
    kind_value->u.i = 7; // DXIL_RESOURCE_KIND_TEXTURE2DARRAY.

    uint32_t zero_id = UINT32_MAX;
    uint32_t zero_type = UINT32_MAX;
    for (uint32_t i = 0; i < module->nmodule_values; i++) {
        const dxil_value *candidate = &module->values[i];
        const dxil_type *type = candidate->type < module->ntypes ? &module->types[candidate->type] : NULL;
        if (((candidate->kind == DXV_CONST_INT && candidate->u.i == 0) || candidate->kind == DXV_CONST_NULL) && type &&
            type->kind == DXT_INT && type->bits == 32) { zero_id = i; zero_type = candidate->type; break; }
    }
    if (zero_id == UINT32_MAX || zero_type == UINT32_MAX || function->nlocals == UINT32_MAX ||
        function->nlocals >= UINT32_MAX - module->nmodule_values) return 0;
    dxil_value *array_locals = realloc(function->locals, (size_t)(function->nlocals + 1u) * sizeof *array_locals);
    if (!array_locals) return 0;
    function->locals = array_locals;
    uint32_t negative_layer_id = module->nmodule_values + function->nlocals;
    function->locals[function->nlocals++] = (dxil_value){
        .kind = DXV_CONST_INT, .type = zero_type, .u.i = -1
    };
    uint32_t loads = 0;
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op != DXI_CALL || dxil_dxop(module, function, instruction) != 66) continue;
        if (instruction->n != 10 || !instruction->ops) return 0;
        instruction->ops[6] = loads == 0 ? negative_layer_id : zero_id;
        // Texture2DArray's third coordinate is the array layer; the first read probes a negative layer.
        loads++;
    }
    return loads == 8;
}

static int set_texture_load_msaa_array_resource(dxil_module *module) {
    const dxil_named_md *named = dxil_named(module, "dx.resources");
    if (!named || named->n != 1 || !named->nodes || named->nodes[0] >= module->nmd) return 0;
    dxil_md *lists = &module->md[named->nodes[0]];
    dxil_md *resources = mutable_metadata_operand(module, lists, 0);
    if (!resources || resources->kind != DXM_NODE || resources->n != 1) return 0;
    dxil_md *record = mutable_metadata_operand(module, resources, 0);
    dxil_md *kind_md = mutable_metadata_operand(module, record, 6);
    if (!kind_md || kind_md->kind != DXM_VALUE || kind_md->value >= module->nmodule_values) return 0;
    dxil_value *kind_value = &module->values[kind_md->value];
    if (kind_value->kind != DXV_CONST_INT || kind_value->u.i != 3) return 0;

    dxil_function *function = NULL;
    for (uint32_t i = 0; i < module->nfunctions; i++)
        if (!module->functions[i].is_declaration) { function = &module->functions[i]; break; }
    if (!function || function->nlocals == UINT32_MAX ||
        function->nlocals > UINT32_MAX - module->nmodule_values) return 0;
    dxil_value *locals = realloc(function->locals, (size_t)(function->nlocals + 1u) * sizeof *locals);
    if (!locals) return 0;
    function->locals = locals;
    uint32_t kind_value_id = kind_md->value;
    uint32_t replacement_id = module->nmodule_values + function->nlocals;
    function->locals[function->nlocals++] = (dxil_value){
        .kind = DXV_CONST_INT, .type = kind_value->type, .u.i = kind_value->u.i
    };
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op != DXI_CALL || !instruction->ops) continue;
        for (uint32_t operand = 1; operand < instruction->n; operand++) {
            if (instruction->ops[operand] == kind_value_id) {
                instruction->ops[operand] = replacement_id;
            }
        }
    }
    kind_value->u.i = 4; // DXIL_RESOURCE_KIND_TEXTURE2DMSARRAY.

    uint32_t loads = 0;
    for (uint32_t i = 0; i < function->ninsts; i++) {
        dxil_inst *instruction = &function->insts[i];
        if (instruction->op != DXI_CALL || dxil_dxop(module, function, instruction) != 66) continue;
        if (instruction->n != 10 || !instruction->ops || function->nlocals == UINT32_MAX ||
            function->nlocals >= UINT32_MAX - module->nmodule_values) return 0;
        const dxil_value *sample_index = dxil_value_at(module, function, instruction->ops[3]);
        if (!sample_index || sample_index->type >= module->ntypes ||
            module->types[sample_index->type].kind != DXT_INT || module->types[sample_index->type].bits != 32)
            return 0;
        uint32_t layer_type = sample_index->type;
        dxil_value *array_locals = realloc(function->locals, (size_t)(function->nlocals + 1u) * sizeof *array_locals);
        if (!array_locals) return 0;
        function->locals = array_locals;
        uint32_t layer_id = module->nmodule_values + function->nlocals;
        function->locals[function->nlocals++] = (dxil_value){
            .kind = DXV_CONST_INT, .type = layer_type, .u.i = -1
        };
        instruction->ops[6] = layer_id;
        loads++;
    }
    return loads == 1;
}

static int set_texture_load_component_field(dxil_module *module, uint32_t field) {
    if (!module || field > 3) return 0;
    for (uint32_t function_index = 0; function_index < module->nfunctions; function_index++) {
        dxil_function *function = &module->functions[function_index];
        if (function->is_declaration) continue;
        for (uint32_t instruction_index = 0; instruction_index < function->ninsts; instruction_index++) {
            dxil_inst *instruction = &function->insts[instruction_index];
            if (instruction->op != DXI_EXTRACTVAL || instruction->n != 2 || instruction->nvals != 1 ||
                !instruction->ops || instruction->ops[1] != 0) continue;
            instruction->ops[1] = field;
            return 1;
        }
    }
    return 0;
}

static int translate_texture_dimensions_fixture(const char *output_path, uint32_t mip_level, int array_size) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/texture_dimensions_cs.dxil", &module)) return 1;
    if (array_size && !set_texture_dimensions_array_size(&module)) {
        dxil_free(&module);
        return check("could not create the synthetic Texture2DArray GetDimensions DXIL fixture");
    }
    if (mip_level && !set_texture_dimensions_mip_level(&module, mip_level)) {
        dxil_free(&module);
        return check("could not select a nonzero mip level in the Texture2D GetDimensions DXIL test");
    }
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: texture-dimensions lowering returned %d\n", rc);
        return check("Texture2D GetDimensions DXIL did not lower to common IR");
    }
    uint32_t image_count = 0, size_queries = 0, size_queries_with_lod = 0;
    uint32_t level_queries = 0, array_size_queries = 0, extracts = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("texture-dimensions IR lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) {
            const gpu_ir_shader_type *pointer = gpu_ir_shader_find_type(shader, instruction->result_type_id);
            const gpu_ir_shader_type *image = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
                gpu_ir_shader_find_type(shader, pointer->element_type_id) : NULL;
            if (image && image->kind == GPU_IR_SHADER_TYPE_IMAGE && image->image_dimension == 1 &&
                image->image_sampled == 1 && image->image_arrayed == (uint32_t)array_size &&
                image->image_multisampled == 0)
                image_count++;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_IMAGE_QUERY) {
            if (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SIZE) {
                size_queries++;
                if (instruction->operand_count == 2) size_queries_with_lod++;
            }
            if (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_LEVELS) level_queries++;
            if (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_ARRAY_SIZE) array_size_queries++;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_COMPOSITE &&
                   instruction->operation == GPU_IR_SHADER_COMPOSITE_EXTRACT) extracts++;
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
    if (!layout) { gpu_ir_shader_release(shader); return check("texture-dimensions D3D12 layout was rejected"); }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    const char *expected_extent_query = array_size ? "get_array_size()" : "get_num_mip_levels()";
    if (rc != GPU_IR_OK || !msl || !msl_size || image_count != 1 || size_queries != 1 ||
        size_queries_with_lod != 1 || level_queries != (uint32_t)!array_size ||
        array_size_queries != (uint32_t)array_size || extracts != 2 || !strstr(msl, "get_width(uint(") ||
        !strstr(msl, "get_height(uint(") || !strstr(msl, expected_extent_query) ||
        (array_size && !strstr(msl, "texture2d_array<float, access::read>")) ||
        (array_size && !strstr(msl, "[uint(0x00000002u)] = vx_value_59;")) ||
        !strstr(msl, "[[texture(0)]]") || !strstr(msl, "[[buffer(0)]]")) {
        fprintf(stderr, "dxil_ir: texture dimensions diagnostics rc=%d image=%u size=%u lod=%u levels=%u arrays=%u extracts=%u msl=%zu\n",
                rc, image_count, size_queries, size_queries_with_lod, level_queries,
                array_size_queries, extracts, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        return check("Texture GetDimensions did not emit its dimensions through common IR/MSL");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) {
            free(msl);
            return check("could not write the Texture2D dimensions common-MSL fixture");
        }
    }
    free(msl);
    printf("dxil_ir: %s<float4>.GetDimensions at mip %u lowers dimension metadata through common IR/MSL: ok\n",
           array_size ? "synthetic Texture2DArray" : "Texture2D", mip_level);
    return 0;
}

static int translate_texture_dimensions_msaa_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/texture_dimensions_msaa_cs.dxil", &module)) return 1;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: multisample texture-dimensions lowering returned %d\n", rc);
        return check("Texture2DMS GetDimensions DXIL did not lower to common IR");
    }
    uint32_t image_count = 0, size_queries = 0, size_queries_with_lod = 0;
    uint32_t sample_queries = 0, level_queries = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("MSAA dimension IR lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) {
            const gpu_ir_shader_type *pointer = gpu_ir_shader_find_type(shader, instruction->result_type_id);
            const gpu_ir_shader_type *image = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
                gpu_ir_shader_find_type(shader, pointer->element_type_id) : NULL;
            if (image && image->kind == GPU_IR_SHADER_TYPE_IMAGE && image->image_dimension == 1 &&
                image->image_sampled == 1 && image->image_arrayed == 0 && image->image_multisampled == 1)
                image_count++;
        } else if (instruction->opcode == GPU_IR_SHADER_OP_IMAGE_QUERY) {
            if (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SIZE) {
                size_queries++;
                if (instruction->operand_count == 2) size_queries_with_lod++;
            }
            if (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_SAMPLES) sample_queries++;
            if (instruction->operation == GPU_IR_SHADER_IMAGE_QUERY_LEVELS) level_queries++;
        }
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
    if (!layout) { gpu_ir_shader_release(shader); return check("MSAA GetDimensions D3D12 layout was rejected"); }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK || !msl || !msl_size || image_count != 1 || size_queries != 1 ||
        size_queries_with_lod || sample_queries != 1 || level_queries ||
        !strstr(msl, "texture2d_ms<float, access::read>") || !strstr(msl, ".get_width()") ||
        !strstr(msl, ".get_height()") || !strstr(msl, ".get_num_samples()") ||
        !strstr(msl, "[[texture(0)]]") || !strstr(msl, "[[buffer(0)]]")) {
        fprintf(stderr, "dxil_ir: MSAA dimensions diagnostics rc=%d image=%u size=%u size_lod=%u samples=%u levels=%u msl=%zu\n",
                rc, image_count, size_queries, size_queries_with_lod, sample_queries, level_queries, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        return check("Texture2DMS GetDimensions did not emit dimensions and sample count through common IR/MSL");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) { free(msl); return check("could not write the Texture2DMS dimensions common-MSL fixture"); }
    }
    free(msl);
    puts("dxil_ir: real Texture2DMS.GetDimensions lowers dimensions and sample count through common IR/MSL: ok");
    return 0;
}

static int translate_texture_load_msaa_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/texture_load_msaa_cs.dxil", &module)) return 1;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: multisample texture-load lowering returned %d\n", rc);
        return check("Texture2DMS.Load DXIL did not lower to common IR");
    }
    uint32_t image_count = 0, read_count = 0, bad_read_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("MSAA load IR lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) {
            const gpu_ir_shader_type *pointer = gpu_ir_shader_find_type(shader, instruction->result_type_id);
            const gpu_ir_shader_type *image = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
                gpu_ir_shader_find_type(shader, pointer->element_type_id) : NULL;
            if (image && image->kind == GPU_IR_SHADER_TYPE_IMAGE && image->image_dimension == 1 &&
                image->image_sampled == 1 && !image->image_arrayed && image->image_multisampled == 1)
                image_count++;
        }
        if (instruction->opcode == GPU_IR_SHADER_OP_IMAGE_READ) {
            if (instruction->operation == GPU_IR_SHADER_IMAGE_READ_2D_MSAA_OOB &&
                instruction->operand_count == 4 && instruction->literal_word_count == 3) read_count++;
            else bad_read_count++;
        }
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
    if (!layout) { gpu_ir_shader_release(shader); return check("Texture2DMS.Load D3D12 layout was rejected"); }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK || !msl || !msl_size || image_count != 1 || read_count != 1 || bad_read_count ||
        !strstr(msl, "texture2d_ms<float, access::read>") ||
        !strstr(msl, "get_width()") || !strstr(msl, "get_height()") ||
        !strstr(msl, ".read(uint2(uint(vx_read_x_") ||
        !strstr(msl, ")), uint(uint(0x00000002u))).x;") ||
        !strstr(msl, "[[texture(0)]]") || !strstr(msl, "[[buffer(0)]]")) {
        fprintf(stderr, "dxil_ir: MSAA TextureLoad diagnostics rc=%d image=%u reads=%u bad_reads=%u msl=%zu\n",
                rc, image_count, read_count, bad_read_count, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        return check("Texture2DMS.Load did not preserve its sample-index read through common IR/MSL");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) { free(msl); return check("could not write the Texture2DMS.Load common-MSL fixture"); }
    }
    free(msl);
    puts("dxil_ir: real Texture2DMS.Load lowers sample-index reads through common IR/MSL: ok");
    return 0;
}

static int translate_texture_load_msaa_array_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/texture_load_msaa_cs.dxil", &module)) return 1;
    if (!set_texture_load_msaa_array_resource(&module)) {
        dxil_free(&module);
        return check("could not construct the Texture2DMSArray.Load adapter fixture");
    }
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: multisample array texture-load lowering returned %d\n", rc);
        return check("Texture2DMSArray.Load DXIL did not lower to common IR");
    }
    uint32_t image_count = 0, read_count = 0, bad_read_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("MSAA array load IR lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) {
            const gpu_ir_shader_type *pointer = gpu_ir_shader_find_type(shader, instruction->result_type_id);
            const gpu_ir_shader_type *image = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
                gpu_ir_shader_find_type(shader, pointer->element_type_id) : NULL;
            if (image && image->kind == GPU_IR_SHADER_TYPE_IMAGE && image->image_dimension == 1 &&
                image->image_sampled == 1 && image->image_arrayed && image->image_multisampled == 1)
                image_count++;
        }
        if (instruction->opcode == GPU_IR_SHADER_OP_IMAGE_READ) {
            if (instruction->operation == GPU_IR_SHADER_IMAGE_READ_2D_MSAA_OOB &&
                instruction->operand_count == 5 && instruction->literal_word_count == 3) read_count++;
            else bad_read_count++;
        }
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
    if (!layout) { gpu_ir_shader_release(shader); return check("Texture2DMSArray.Load D3D12 layout was rejected"); }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    if (rc != GPU_IR_OK || !msl || !msl_size || image_count != 1 || read_count != 1 || bad_read_count ||
        !strstr(msl, "texture2d_ms_array<float, access::read>") ||
        !strstr(msl, "get_array_size()") ||
        !strstr(msl, "vx_read_layer_") ||
        !strstr(msl, ".read(uint2(uint(vx_read_x_")) {
        fprintf(stderr, "dxil_ir: MSAA array TextureLoad diagnostics rc=%d image=%u reads=%u bad_reads=%u msl=%zu\n",
                rc, image_count, read_count, bad_read_count, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        return check("Texture2DMSArray.Load did not preserve layer and sample indices through common IR/MSL");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) { free(msl); return check("could not write the Texture2DMSArray.Load common-MSL fixture"); }
    }
    free(msl);
    puts("dxil_ir: synthetic Texture2DMSArray.Load lowers layer/sample reads through common IR/MSL: ok");
    return 0;
}

static int translate_texture_load_fixture_from(const char *fixture_path, const char *output_path,
                                               int array_resource, int synthesize_array_resource,
                                               uint32_t expected_load_count, int component_kind,
                                               int component_field_override) {
    int signed_image = component_kind == 1, float_image = component_kind == 2;
    const char *component_name = float_image ? "float" : signed_image ? "int" : "uint";
    dxil_module module = { 0 };
    if (read_dxil_fixture(fixture_path, &module)) return 1;
    if (component_field_override >= 0 &&
        !set_texture_load_component_field(&module, (uint32_t)component_field_override)) {
        dxil_free(&module);
        return check("could not select a synthetic TextureLoad return component");
    }
    if (synthesize_array_resource && !set_texture_load_array_resource(&module)) {
        dxil_free(&module);
        return check("could not create the synthetic Texture2DArray.Load DXIL fixture");
    }
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: texture-load lowering returned %d\n", rc);
        return check(float_image ? "Texture2D<float>.Load DXIL did not lower to common IR" :
                     signed_image ? "Texture2D<int>.Load DXIL did not lower to common IR" :
                                    "Texture2D<uint>.Load DXIL did not lower to common IR");
    }
    uint32_t image_count = 0, buffer_count = 0, read_count = 0, store_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("Texture2D.Load IR lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT) image_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_VARIABLE &&
            instruction->storage_class == GPU_IR_SHADER_STORAGE_BUFFER) buffer_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_IMAGE_READ &&
            instruction->operation == GPU_IR_SHADER_IMAGE_READ_2D_LOD_OOB &&
            instruction->operand_count == (array_resource ? 5u : 4u) &&
            instruction->literal_word_count == 3) read_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_STORE) store_count++;
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
    if (!layout) { gpu_ir_shader_release(shader); return check("Texture2D.Load D3D12 layout was rejected"); }
    char *msl = NULL;
    size_t msl_size = 0;
    rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    gpu_ir_pipeline_layout_release(layout);
    gpu_ir_shader_release(shader);
    char texture_type[64];
    snprintf(texture_type, sizeof texture_type, array_resource ? "texture2d_array<%s, access::read>" :
             "texture2d<%s, access::read>", component_name);
    int valid = rc == GPU_IR_OK && msl && msl_size && image_count == 1 && buffer_count == 1 &&
        read_count == expected_load_count && store_count == expected_load_count &&
        strstr(msl, texture_type) && strstr(msl, "get_num_mip_levels()") &&
        strstr(msl, "get_width(vx_read_level_") && strstr(msl, "get_height(vx_read_level_") &&
        strstr(msl, "long(as_type<int>(uint(") && strstr(msl, "long(1ll)") &&
        strstr(msl, "long(-1ll)") && strstr(msl, ".read(uint2(uint(vx_read_x_") &&
        (component_field_override < 0 || strstr(msl, component_field_override == 1 ? ").y;" :
                                                 component_field_override == 2 ? ").z;" : ").w;")) &&
        (!signed_image || strstr(msl, "as_type<uint>(vx_image_")) &&
        (!float_image || strstr(msl, "0.0f;")) &&
        (!synthesize_array_resource || !array_resource ||
         strstr(msl, "long(as_type<int>(uint(uint(0xffffffffu))))")) &&
        (!array_resource || (strstr(msl, "get_array_size()") && strstr(msl, "vx_read_layer_") &&
                             strstr(msl, " >= 0 && ulong(vx_read_layer_") &&
                             strstr(msl, "uint(vx_read_layer_"))) &&
        strstr(msl, "[[texture(0)]]") && strstr(msl, "[[buffer(0)]]");
    if (!valid) {
        fprintf(stderr, "dxil_ir: Texture%s<%s>.Load diagnostics rc=%d images=%u buffers=%u reads=%u stores=%u msl=%zu\n",
                array_resource ? "2DArray" : "2D", component_name,
                rc, image_count, buffer_count, read_count, store_count, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        return check(float_image ? "float Texture2D.Load did not preserve its element type and OOB zero in common MSL" :
                     signed_image ? "signed Texture2D.Load did not preserve its integer image type and bits in common MSL" :
                     array_resource ? "Texture2DArray.Load layer bounds and offsets did not reach common MSL" :
                                      "Texture2D.Load bounds and offsets did not reach common MSL");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) {
            free(msl);
            return check("could not write the Texture2D.Load common-MSL fixture");
        }
    }
    free(msl);
    printf("dxil_ir: Texture%s<%s>.Load and four-byte structured UAV stores lower through common IR/MSL: ok\n",
           array_resource ? "2DArray" : "2D", component_name);
    return 0;
}

static int translate_texture_load_fixture(const char *output_path, int array_resource) {
    return translate_texture_load_fixture_from("tests/shaders12/texload_bounds_cs.dxil", output_path,
                                               array_resource, array_resource, 8, 0, -1);
}

static int translate_texture_load_array_dxil_fixture(const char *output_path) {
    return translate_texture_load_fixture_from("tests/shaders12/texload_array_bounds_cs.dxil",
                                                output_path, 1, 0, 9, 0, -1);
}

static int translate_texture_load_sint_dxil_fixture(const char *output_path) {
    return translate_texture_load_fixture_from("tests/shaders12/texload_sint_bounds_cs.dxil",
                                                output_path, 0, 0, 7, 1, -1);
}

static int translate_texture_load_array_sint_dxil_fixture(const char *output_path) {
    return translate_texture_load_fixture_from("tests/shaders12/texload_array_sint_bounds_cs.dxil",
                                                output_path, 1, 0, 9, 1, -1);
}

static int translate_texture_load_float_dxil_fixture(const char *output_path) {
    return translate_texture_load_fixture_from("tests/shaders12/texload_float_bounds_cs.dxil",
                                                output_path, 0, 0, 7, 2, -1);
}

static int translate_texture_load_float_channel_dxil_fixture(const char *output_path, uint32_t component) {
    return translate_texture_load_fixture_from("tests/shaders12/texload_float_bounds_cs.dxil",
                                                output_path, 0, 0, 7, 2, (int)component);
}

static int translate_direct_texture_load_mapping_fixture(const char *fixture_path, const char *vector_type,
                                                         const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture(fixture_path, &module)) return 1;
    dxil_msl direct = { 0 };
    int failed = dxil_to_msl(&module, &direct) != 0;
    dxil_free(&module);
    char swizzle_call[64];
    snprintf(swizzle_call, sizeof swizzle_call, "vx_desc_swizzle(%s(", vector_type);
    if (!failed && (!strstr(direct.msl, swizzle_call) ||
                    !strstr(direct.msl, "uint mapping = (d->flags >> 8u) & 0x1fffu"))) {
        fprintf(stderr, "DXIL direct MSL omitted descriptor-backed SRV component routing\n%s\n",
                direct.msl ? direct.msl : "");
        failed = 1;
    }
    if (!failed && output_path) {
        FILE *output = fopen(output_path, "wb");
        size_t size = strlen(direct.msl);
        int write_failed = !output || fwrite(direct.msl, 1, size, output) != size;
        if (output && fclose(output)) write_failed = 1;
        if (write_failed) failed = check("could not write the direct DXIL component-mapping fixture");
    }
    if (failed) fprintf(stderr, "dxil_ir: direct component-mapping translation failed: %s\n", direct.error);
    dxil_msl_free(&direct);
    return failed;
}

static int translate_direct_typed_buffer_mapping_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/typed_buffer_cs.dxil", &module)) return 1;
    dxil_msl direct = { 0 };
    int failed = dxil_to_msl(&module, &direct) != 0;
    dxil_free(&module);
    if (!failed && (direct.uses_component_mapping_unsupported_read ||
                    !strstr(direct.msl, "? vx_desc_swizzle(uint4(") || !strstr(direct.msl, ": uint4(0)"))) {
        fprintf(stderr, "DXIL typed-buffer MSL did not apply SRV component mapping to this load form\n%s\n",
                direct.msl ? direct.msl : "");
        failed = 1;
    }
    if (!failed && output_path) {
        FILE *output = fopen(output_path, "wb");
        size_t size = strlen(direct.msl);
        int write_failed = !output || fwrite(direct.msl, 1, size, output) != size;
        if (output && fclose(output)) write_failed = 1;
        if (write_failed) failed = check("could not write the direct typed-buffer load fixture");
    }
    if (failed) fprintf(stderr, "dxil_ir: direct typed-buffer component-mapping lowering failed: %s\n", direct.error);
    dxil_msl_free(&direct);
    return failed;
}

static int remap_typed_srv_metadata_to_kind(dxil_module *module, uint32_t target_kind) {
    const dxil_named_md *resources = dxil_named(module, "dx.resources");
    if (!resources || !resources->n || resources->nodes[0] >= module->nmd) return 0;
    const dxil_md *lists = &module->md[resources->nodes[0]];
    const dxil_md *srvs = dxil_md_op(module, lists, DXIL_SRV);
    int changed = 0;
    for (uint32_t i = 0; srvs && i < srvs->n; i++) {
        const dxil_md *resource = dxil_md_op(module, srvs, i);
        const dxil_md *kind_node = dxil_md_op(module, resource, 6);
        int64_t kind;
        if (!dxil_md_int(module, kind_node, &kind) || kind != 10 ||
            !kind_node || kind_node->kind != DXM_VALUE || kind_node->value >= module->nmodule_values) continue;
        dxil_value *kind_value = &module->values[kind_node->value];
        if (kind_value->kind != DXV_CONST_INT) continue;
        kind_value->u.i = target_kind;
        changed++;
    }
    return changed;
}

static int set_structured_load_byte_offset_zero(dxil_module *module) {
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) != 68 || instruction->n < 5) continue;
            const dxil_value *index = dxil_value_at(module, function, instruction->ops[3]);
            if (!index || index->type >= module->ntypes) continue;
            uint32_t local_count = function->nlocals;
            dxil_value *locals = realloc(function->locals, (size_t)(local_count + 1) * sizeof *locals);
            if (!locals) return 0;
            function->locals = locals;
            function->locals[local_count] = (dxil_value){ .kind = DXV_CONST_INT, .type = index->type, .u.i = 0 };
            instruction->ops[4] = module->nmodule_values + local_count;
            function->nlocals++;
            return 1;
        }
    }
    return 0;
}

static uint32_t append_int_local(dxil_module *module, dxil_function *function, uint32_t type, int64_t value) {
    uint32_t local_count = function->nlocals;
    dxil_value *locals = realloc(function->locals, (size_t)(local_count + 1) * sizeof *locals);
    if (!locals) return UINT32_MAX;
    function->locals = locals;
    function->locals[local_count] = (dxil_value){ .kind = DXV_CONST_INT, .type = type, .u.i = value };
    function->nlocals++;
    return module->nmodule_values + local_count;
}

static uint32_t append_undef_local(dxil_module *module, dxil_function *function, uint32_t type) {
    uint32_t local_count = function->nlocals;
    dxil_value *locals = realloc(function->locals, (size_t)(local_count + 1) * sizeof *locals);
    if (!locals) return UINT32_MAX;
    function->locals = locals;
    function->locals[local_count] = (dxil_value){ .kind = DXV_CONST_UNDEF, .type = type };
    function->nlocals++;
    return module->nmodule_values + local_count;
}

static void shift_fixture_local_value_id(uint32_t *id, uint32_t old_module_value_count) {
    if (id && *id != UINT32_MAX && *id >= old_module_value_count) (*id)++;
}

static void rebase_fixture_local_value_ids(dxil_module *module, uint32_t old_module_value_count) {
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        shift_fixture_local_value_id(&function->first_arg, old_module_value_count);
        for (uint32_t vi = 0; vi < function->nlocals; vi++) {
            dxil_value *value = &function->locals[vi];
            if (value->kind == DXV_CONST_AGGREGATE || value->kind == DXV_CONST_DATA) {
                for (uint32_t j = 0; j < value->u.list.n; j++)
                    shift_fixture_local_value_id(&value->u.list.ids[j], old_module_value_count);
            } else if (value->kind == DXV_CONST_EXPR) {
                for (uint32_t j = 0; j < value->u.expr.n; j++)
                    shift_fixture_local_value_id(&value->u.expr.ops[j], old_module_value_count);
            }
        }
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            shift_fixture_local_value_id(&instruction->result, old_module_value_count);
            switch (instruction->op) {
            case DXI_CALL: case DXI_BINOP: case DXI_CMP: case DXI_SELECT:
            case DXI_EXTRACTELT: case DXI_INSERTELT: case DXI_RET:
            case DXI_STORE: case DXI_ATOMICRMW: case DXI_CMPXCHG: case DXI_FENCE:
                for (uint32_t j = 0; j < instruction->n; j++)
                    shift_fixture_local_value_id(&instruction->ops[j], old_module_value_count);
                break;
            case DXI_CAST: case DXI_ALLOCA: case DXI_LOAD:
                if (instruction->n) shift_fixture_local_value_id(&instruction->ops[0], old_module_value_count);
                break;
            case DXI_EXTRACTVAL: case DXI_INSERTVAL:
                for (uint32_t j = 0; j < instruction->nvals; j++)
                    shift_fixture_local_value_id(&instruction->ops[j], old_module_value_count);
                break;
            case DXI_PHI:
                for (uint32_t j = 0; j < instruction->n; j += 2)
                    shift_fixture_local_value_id(&instruction->ops[j], old_module_value_count);
                break;
            case DXI_GEP:
                for (uint32_t j = 1; j < instruction->n; j++)
                    shift_fixture_local_value_id(&instruction->ops[j], old_module_value_count);
                break;
            case DXI_BR:
                if (instruction->n == 3)
                    shift_fixture_local_value_id(&instruction->ops[2], old_module_value_count);
                break;
            case DXI_SWITCH:
                for (uint32_t j = 0; j < instruction->n; j += 2)
                    shift_fixture_local_value_id(&instruction->ops[j], old_module_value_count);
                break;
            default:
                break;
            }
        }
    }
}

static int set_typed_resource_kind_to_raw(dxil_module *module, uint32_t resource_class) {
    const dxil_named_md *named = dxil_named(module, "dx.resources");
    if (!named || named->n != 1 || !named->nodes || named->nodes[0] >= module->nmd) return 0;
    dxil_md *lists = &module->md[named->nodes[0]];
    dxil_md *resources = mutable_metadata_operand(module, lists, resource_class);
    int changed = 0;
    for (uint32_t i = 0; resources && i < resources->n; i++) {
        if (!resources->ops || !resources->ops[i] || resources->ops[i] - 1u >= module->nmd) continue;
        uint32_t resource_index = resources->ops[i] - 1u;
        dxil_md *resource = &module->md[resource_index];
        if (!resource->ops || resource->n <= 6 || !resource->ops[6] ||
            resource->ops[6] - 1u >= module->nmd) continue;
        uint32_t kind_index = resource->ops[6] - 1u;
        dxil_md *kind = &module->md[kind_index];
        int64_t kind_value;
        if (!resource || !kind || !dxil_md_int(module, kind, &kind_value) || kind_value != 10 ||
            kind->kind != DXM_VALUE || kind->value >= module->nmodule_values) continue;
        const dxil_value *old_value = &module->values[kind->value];
        if (old_value->kind != DXV_CONST_INT) continue;
        uint32_t old_type = old_value->type;
        uint32_t old_module_value_count = module->nmodule_values;
        if (old_module_value_count == UINT32_MAX) return 0;
        dxil_value *values = realloc(module->values, (size_t)(module->nmodule_values + 1u) * sizeof *values);
        if (!values) return 0;
        module->values = values;
        module->values[old_module_value_count] = (dxil_value){
            .kind = DXV_CONST_INT, .type = old_type, .u.i = 11
        };
        module->nmodule_values++;
        rebase_fixture_local_value_ids(module, old_module_value_count);
        if (module->nmd == UINT32_MAX) return 0;
        uint32_t new_kind_index = module->nmd;
        dxil_md *metadata = realloc(module->md, (size_t)(module->nmd + 1u) * sizeof *metadata);
        if (!metadata) return 0;
        module->md = metadata;
        module->md[module->nmd++] = (dxil_md){ .kind = DXM_VALUE, .value = old_module_value_count };
        module->md[resource_index].ops[6] = new_kind_index + 1u;
        changed++;
    }
    return changed;
}

static int set_typed_srv_kind_to_raw(dxil_module *module) {
    return set_typed_resource_kind_to_raw(module, DXIL_SRV);
}

static int set_typed_uav_kind_to_raw(dxil_module *module) {
    return set_typed_resource_kind_to_raw(module, DXIL_UAV);
}

static int append_fixture_module_integer(dxil_module *module, uint32_t type, int64_t value,
                                         uint32_t *value_id) {
    if (!module || !value_id || module->nmodule_values == UINT32_MAX) return 0;
    uint32_t old_count = module->nmodule_values;
    dxil_value *values = realloc(module->values, (size_t)(old_count + 1u) * sizeof *values);
    if (!values) return 0;
    module->values = values;
    module->values[old_count] = (dxil_value){ .kind = DXV_CONST_INT, .type = type, .u.i = value };
    module->nmodule_values++;
    rebase_fixture_local_value_ids(module, old_count);
    *value_id = old_count;
    return 1;
}

static int set_typed_resource_kind_to_structured(dxil_module *module, uint32_t resource_class,
                                                 uint32_t stride) {
    const dxil_named_md *named = dxil_named(module, "dx.resources");
    if (!named || named->n != 1 || !named->nodes || named->nodes[0] >= module->nmd ||
        (resource_class != DXIL_SRV && resource_class != DXIL_UAV) || stride < 4 || stride % 4) return 0;
    uint32_t properties_index = resource_class == DXIL_SRV ? 8u : 10u;
    dxil_md *resources = mutable_metadata_operand(module, &module->md[named->nodes[0]], resource_class);
    if (!resources || resources->kind != DXM_NODE) return 0;
    uint32_t resource_index = UINT32_MAX, kind_type = UINT32_MAX;
    for (uint32_t i = 0; i < resources->n; i++) {
        dxil_md *resource = mutable_metadata_operand(module, resources, i);
        const dxil_md *kind = dxil_md_op(module, resource, 6);
        int64_t kind_value;
        if (!resource || !dxil_md_int(module, kind, &kind_value) || kind_value != 10 ||
            !kind || kind->kind != DXM_VALUE || kind->value >= module->nmodule_values ||
            module->values[kind->value].kind != DXV_CONST_INT || resource->n <= properties_index) continue;
        resource_index = (uint32_t)(resource - module->md);
        kind_type = module->values[kind->value].type;
        break;
    }
    if (resource_index == UINT32_MAX || kind_type == UINT32_MAX || module->nmd > UINT32_MAX - 4u)
        return 0;
    uint32_t kind_value_id, tag_value_id, stride_value_id;
    if (!append_fixture_module_integer(module, kind_type, 12, &kind_value_id) ||
        !append_fixture_module_integer(module, kind_type, 1, &tag_value_id) ||
        !append_fixture_module_integer(module, kind_type, stride, &stride_value_id)) return 0;
    uint32_t metadata_base = module->nmd;
    dxil_md *metadata = realloc(module->md, (size_t)(module->nmd + 4u) * sizeof *metadata);
    if (!metadata) return 0;
    module->md = metadata;
    module->md[metadata_base] = (dxil_md){ .kind = DXM_VALUE, .value = kind_value_id };
    module->md[metadata_base + 1u] = (dxil_md){ .kind = DXM_VALUE, .value = tag_value_id };
    module->md[metadata_base + 2u] = (dxil_md){ .kind = DXM_VALUE, .value = stride_value_id };
    module->md[metadata_base + 3u] = (dxil_md){ .kind = DXM_NODE, .n = 2,
        .ops = calloc(2u, sizeof *module->md[metadata_base + 3u].ops) };
    if (!module->md[metadata_base + 3u].ops) return 0;
    module->md[metadata_base + 3u].ops[0] = metadata_base + 2u;
    module->md[metadata_base + 3u].ops[1] = metadata_base + 3u;
    module->nmd += 4u;
    dxil_md *resource = &module->md[resource_index];
    if (!resource->ops || resource->n <= properties_index) return 0;
    resource->ops[6] = metadata_base + 1u;
    resource->ops[properties_index] = metadata_base + 4u;
    return 1;
}

static int set_typed_srv_kind_to_structured(dxil_module *module, uint32_t stride) {
    return set_typed_resource_kind_to_structured(module, DXIL_SRV, stride);
}

static int set_typed_uav_kind_to_structured(dxil_module *module, uint32_t stride) {
    return set_typed_resource_kind_to_structured(module, DXIL_UAV, stride);
}

static int remove_fixture_typed_buffer_store(dxil_module *module) {
    if (!module) return 0;
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            if (dxil_dxop(module, function, &function->insts[ii]) != 69) continue;
            free(function->insts[ii].ops);
            memmove(&function->insts[ii], &function->insts[ii + 1u],
                    (size_t)(function->ninsts - ii - 1u) * sizeof *function->insts);
            function->ninsts--;
            return 1;
        }
    }
    return 0;
}

static int set_raw_buffer_properties_null(dxil_module *module, uint32_t resource_class) {
    const dxil_named_md *named = dxil_named(module, "dx.resources");
    if (!named || named->n != 1 || !named->nodes || named->nodes[0] >= module->nmd) return 0;
    dxil_md *lists = &module->md[named->nodes[0]];
    dxil_md *resources = mutable_metadata_operand(module, lists, resource_class);
    int changed = 0;
    uint32_t properties_index = resource_class == DXIL_SRV ? 8u : 10u;
    for (uint32_t i = 0; resources && i < resources->n; i++) {
        dxil_md *resource = mutable_metadata_operand(module, resources, i);
        const dxil_md *kind = dxil_md_op(module, resource, 6);
        int64_t kind_value;
        if (!resource || !dxil_md_int(module, kind, &kind_value) || kind_value != 11 ||
            resource->n <= properties_index || !resource->ops) continue;
        resource->ops[properties_index] = 0;
        changed++;
    }
    return changed;
}

static int set_raw_srv_properties_null(dxil_module *module) {
    return set_raw_buffer_properties_null(module, DXIL_SRV);
}

static int set_raw_uav_properties_null(dxil_module *module) {
    return set_raw_buffer_properties_null(module, DXIL_UAV);
}

static int set_raw_buffer_load_byte_offset(dxil_module *module, uint32_t byte_offset) {
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) != 68 || instruction->n != 5) continue;
            const dxil_value *index = dxil_value_at(module, function, instruction->ops[3]);
            if (!index) return 0;
            uint32_t byte_offset_id = append_int_local(module, function, index->type, byte_offset);
            if (byte_offset_id == UINT32_MAX) return 0;
            instruction->ops[3] = byte_offset_id;
            return 1;
        }
    }
    return 0;
}

static int set_buffer_store_index(dxil_module *module, uint32_t element_index) {
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) != 69 || instruction->n != 10) continue;
            const dxil_value *index = dxil_value_at(module, function, instruction->ops[3]);
            if (!index) return 0;
            uint32_t index_id = append_int_local(module, function, index->type, element_index);
            if (index_id == UINT32_MAX) return 0;
            instruction->ops[3] = index_id;
            return 1;
        }
    }
    return 0;
}

static int set_buffer_load_index(dxil_module *module, uint32_t element_index) {
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) != 68 || instruction->n != 5) continue;
            const dxil_value *index = dxil_value_at(module, function, instruction->ops[3]);
            if (!index || index->type >= module->ntypes) return 0;
            uint32_t index_id = append_int_local(module, function, index->type, element_index);
            if (index_id == UINT32_MAX) return 0;
            instruction->ops[3] = index_id;
            return 1;
        }
    }
    return 0;
}

static int set_buffer_load_result_field(dxil_module *module, uint32_t field) {
    if (!module || field >= 4) return 0;
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        uint32_t load_result = UINT32_MAX;
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) == 68 && instruction->n == 5) {
                load_result = instruction->result;
                break;
            }
        }
        if (load_result == UINT32_MAX) continue;
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (instruction->op != DXI_EXTRACTVAL || instruction->n != 2 || instruction->nvals != 1 ||
                !instruction->ops || instruction->ops[0] != load_result || instruction->ops[1] != 0)
                continue;
            instruction->ops[1] = field;
            return 1;
        }
    }
    return 0;
}

static int set_structured_load_byte_offset(dxil_module *module, uint32_t byte_offset) {
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) != 68 || instruction->n != 5) continue;
            const dxil_value *index = dxil_value_at(module, function, instruction->ops[3]);
            if (!index || index->type >= module->ntypes) return 0;
            uint32_t offset_id = append_int_local(module, function, index->type, byte_offset);
            if (offset_id == UINT32_MAX) return 0;
            instruction->ops[4] = offset_id;
            return 1;
        }
    }
    return 0;
}

static int use_output_buffer_for_dimensions(dxil_module *module) {
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        uint32_t output_handle = UINT32_MAX;
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) != 57 || instruction->n < 3) continue;
            const dxil_value *resource_class = dxil_value_at(module, function, instruction->ops[2]);
            if (resource_class && resource_class->kind == DXV_CONST_INT && resource_class->u.i == DXIL_UAV) {
                output_handle = instruction->result;
                break;
            }
        }
        if (output_handle == UINT32_MAX) continue;
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) == 72 && instruction->n == 4) {
                instruction->ops[2] = output_handle;
                return 1;
            }
        }
    }
    return 0;
}

static int convert_buffer_load_to_raw_buffer_load(dxil_module *module, uint32_t mask_value,
                                                  uint32_t result_field);
static int convert_float_buffer_load_to_raw_buffer_load(dxil_module *module, uint32_t mask_value,
                                                        uint32_t result_field);
static int convert_buffer_store_to_raw_buffer_store(dxil_module *module, const char *overload,
                                                    uint32_t byte_offset, uint32_t mask_value,
                                                    uint32_t alignment_value, int use_dynamic_offset);
static int convert_buffer_load_to_raw_buffer_load_with_type(dxil_module *module, const char *overload,
                                                            uint32_t mask_value, uint32_t result_field);
static int check_raw_buffer_load_rejection(int use_raw_resource, uint32_t mask_value,
                                           uint32_t result_field, const char *label);

static int translate_raw_buffer_common_fixture_variant(const char *output_path, int use_raw_buffer_load,
                                                       uint32_t byte_offset, uint32_t raw_mask,
                                                       uint32_t raw_field, uint32_t expected_value,
                                                       int execute_runtime) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/typed_buffer_cs.dxil", &module)) return 1;
    int raw_kind = set_typed_srv_kind_to_raw(&module);
    int raw_properties = set_raw_srv_properties_null(&module);
    int raw_offset = set_raw_buffer_load_byte_offset(&module, byte_offset);
    int output_dimensions = use_output_buffer_for_dimensions(&module);
    int changed = raw_kind == 1 && raw_properties == 1 && raw_offset && output_dimensions;
    if (changed && use_raw_buffer_load)
        changed = convert_buffer_load_to_raw_buffer_load(&module, raw_mask, raw_field);
    else if (changed && raw_field != 0)
        changed = set_buffer_load_result_field(&module, raw_field);
    gpu_ir_shader *shader = NULL;
    int rc = changed ? dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader) : GPU_IR_INVALID;
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: raw-buffer lowering returned %d for %s\n", rc,
                use_raw_buffer_load ? "RawBufferLoad opcode 139" : "ByteAddressBuffer.Load opcode 68");
        return check("scalar raw-buffer load did not lower to common IR");
    }
    uint32_t shift_count = 0, component_add_count = 0;
    uint32_t access_count = 0, load_count = 0, store_count = 0, length_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("raw-buffer IR lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_SHIFT_RIGHT_LOGICAL &&
            instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL && instruction->operand_count == 2)
            shift_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_ADD &&
            instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL && instruction->source_opcode == DXI_CALL &&
            instruction->operand_count == 2)
            component_add_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_ACCESS_CHAIN) access_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_LOAD) load_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_STORE) store_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_ARRAY_LENGTH) length_count++;
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
    char *msl = NULL;
    size_t msl_size = 0;
    if (layout) rc = gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size);
    else rc = GPU_IR_INVALID;
    gpu_ir_pipeline_layout_release(layout);
    int valid = rc == GPU_IR_OK && msl && msl_size && shift_count == 1 &&
        component_add_count == (raw_field != 0 ? 1u : 0u) && access_count == 2 &&
        load_count == 1 && store_count == 1 && length_count == 1 &&
        strstr(msl, " >> uint(0x00000002u)") &&
        (byte_offset == 0 || strstr(msl, "0x00000004u")) &&
        strstr(msl, "vx_buffer_ranges[0]") && strstr(msl, "[[buffer(0)]]") &&
        strstr(msl, "[[buffer(1)]]");
    if (!valid) {
        fprintf(stderr, "dxil_ir: raw-buffer common-MSL diagnostics rc=%d shifts=%u component_adds=%u access=%u loads=%u stores=%u lengths=%u msl=%zu\n",
                rc, shift_count, component_add_count, access_count, load_count, store_count, length_count, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        gpu_ir_shader_release(shader);
        return check("ByteAddressBuffer.Load byte offsets did not become bounded word-index reads in common MSL");
    }
    int runtime_ok = !execute_runtime || execute_raw_buffer_load_shader_on_metal(shader, expected_value);
    if (runtime_ok && execute_runtime && raw_field != 0) {
        const uint32_t input_words[4] = { 11u, 22u, 33u, 44u };
        const uint32_t out_of_range_words[4] = { 0u, 4u, 0u, 0u };
        runtime_ok = execute_raw_buffer_shader_on_metal_with_ranges(shader, input_words,
                                                                     out_of_range_words,
                                                                     (size_t)raw_field * 4u, 16u);
    }
    gpu_ir_shader_release(shader);
    if (!runtime_ok) { free(msl); return check("ByteAddressBuffer.Load common-IR Metal readback was incorrect"); }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) { free(msl); return check("could not write the raw-buffer common-MSL fixture"); }
    }
    free(msl);
    if (!use_raw_buffer_load && raw_field == 0) {
        puts("dxil_ir: scalar ByteAddressBuffer.Load lowers to bounded common GPU IR/MSL: ok");
    } else if (!use_raw_buffer_load) {
        static const char *const lanes[] = { "x", "y", "z", "w" };
        printf("dxil_ir: ByteAddressBuffer.Load %s field uses bounded common GPU IR/MSL: ok\n",
               raw_field < sizeof lanes / sizeof lanes[0] ? lanes[raw_field] : "unknown");
    } else if (raw_field == 0) {
        puts("dxil_ir: scalar RawBufferLoad opcode 139 lowers to bounded common GPU IR/MSL: ok");
    } else {
        static const char *const lanes[] = { "x", "y", "z", "w" };
        printf("dxil_ir: RawBufferLoad opcode 139 %s lane uses bounded common GPU IR/MSL: ok\n",
               raw_field < sizeof lanes / sizeof lanes[0] ? lanes[raw_field] : "unknown");
    }
    return 0;
}

static int translate_raw_buffer_float_common_fixture_variant(int use_raw_buffer_load, uint32_t raw_field,
                                                             const uint32_t input_words[4],
                                                             uint32_t expected_value, int execute_runtime,
                                                             const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/typed_float_buffer_cs.dxil", &module)) return 1;
    int changed = set_typed_srv_kind_to_raw(&module) == 1 &&
        set_raw_srv_properties_null(&module) == 1 &&
        set_raw_buffer_load_byte_offset(&module, 0) &&
        use_output_buffer_for_dimensions(&module) &&
        set_buffer_store_index(&module, 1);
    if (changed && use_raw_buffer_load)
        changed = convert_float_buffer_load_to_raw_buffer_load(&module, 15, raw_field);
    else if (raw_field != 0)
        changed = set_buffer_load_result_field(&module, raw_field);
    gpu_ir_shader *shader = NULL;
    int rc = changed ? dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader) : GPU_IR_INVALID;
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: float %s lowering returned %d for lane %u\n",
                use_raw_buffer_load ? "RawBufferLoad" : "bufferLoad", rc, raw_field);
        return check("float RawBufferLoad did not lower to common IR");
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
    char *msl = NULL;
    size_t msl_size = 0;
    rc = layout ? gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size) : GPU_IR_INVALID;
    gpu_ir_pipeline_layout_release(layout);
    uint32_t bitcast_count = 0, load_count = 0;
    for (uint32_t i = 0; rc == GPU_IR_OK && i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { rc = GPU_IR_INVALID; break; }
        if (instruction->opcode == GPU_IR_SHADER_OP_CONVERT &&
            instruction->operation == GPU_IR_SHADER_CONVERT_BITCAST) bitcast_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_LOAD) load_count++;
    }
    if (rc != GPU_IR_OK || !msl || !msl_size || bitcast_count != 2 || load_count != 1 ||
        !strstr(msl, "as_type<float>(")) {
        fprintf(stderr, "dxil_ir: float %s lane %u MSL diagnostics rc=%d bitcasts=%u loads=%u\n",
                use_raw_buffer_load ? "RawBufferLoad" : "bufferLoad", raw_field, rc, bitcast_count, load_count);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        gpu_ir_shader_release(shader);
        return check("float RawBufferLoad did not preserve its 32-bit representation in common MSL");
    }
    if (output_path) {
        FILE *output = fopen(output_path, "wb");
        int failed = !output || fwrite(msl, 1, msl_size, output) != msl_size;
        if (output && fclose(output)) failed = 1;
        if (failed) {
            free(msl);
            gpu_ir_shader_release(shader);
            return check("could not write the float byte-address load lane fixture");
        }
    }
    free(msl);
    const uint32_t expected_words[4] = { 0u, expected_value, 0u, 0u };
    int runtime_ok = !execute_runtime || execute_raw_buffer_shader_on_metal(shader, input_words, expected_words);
    if (runtime_ok && execute_runtime && raw_field != 0) {
        const uint32_t out_of_range_words[4] = { 0u, 0x3f800000u, 0u, 0u };
        runtime_ok = execute_raw_buffer_shader_on_metal_with_ranges(shader, input_words,
                                                                     out_of_range_words,
                                                                     (size_t)raw_field * 4u, 16u);
    }
    gpu_ir_shader_release(shader);
    if (!runtime_ok) return check("float RawBufferLoad common-IR Metal readback was incorrect");
    static const char *const lanes[] = { "x", "y", "z", "w" };
    printf("dxil_ir: float %s %s lane bitcast and common IR/MSL%s: ok\n",
           use_raw_buffer_load ? "RawBufferLoad opcode 139" : "ByteAddressBuffer.Load.f32",
           raw_field < sizeof lanes / sizeof lanes[0] ? lanes[raw_field] : "unknown",
           execute_runtime ? " and Metal readback" : "");
    return 0;
}

static int translate_raw_buffer_float_common_fixture(void) {
    const uint32_t input_words[4] = { 0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u };
    const uint32_t expected_values[4] = { 0x40200000u, 0x40800000u, 0x40b00000u, 0x40e00000u };
    if (translate_raw_buffer_float_common_fixture_variant(0, 0, input_words, expected_values[0], 1, NULL)) return 1;
    for (uint32_t lane = 0; lane < 4; lane++)
        if (translate_raw_buffer_float_common_fixture_variant(1, lane, input_words, expected_values[lane], 1, NULL))
            return 1;
    return 0;
}

static int translate_raw_buffer_dimensions_fixture_variant(const char *overload, int query_uav) {
    if (!overload || (strcmp(overload, "i32") && strcmp(overload, "f32"))) return 1;
    int is_float = !strcmp(overload, "f32");
    const char *fixture = is_float ? "tests/shaders12/typed_float_buffer_cs.dxil" :
        "tests/shaders12/typed_buffer_cs.dxil";
    dxil_module module = { 0 };
    if (read_dxil_fixture(fixture, &module)) return 1;
    int changed;
    if (query_uav) {
        changed = set_typed_uav_kind_to_raw(&module) == 1 &&
            set_raw_uav_properties_null(&module) == 1 &&
            convert_buffer_store_to_raw_buffer_store(&module, overload, 0, 1, 4, 0) &&
            use_output_buffer_for_dimensions(&module);
    } else {
        changed = set_typed_srv_kind_to_raw(&module) == 1 &&
            set_raw_srv_properties_null(&module) == 1 &&
            set_raw_buffer_load_byte_offset(&module, is_float ? 0u : 4u);
    }
    gpu_ir_shader *shader = NULL;
    int rc = changed ? dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader) : GPU_IR_INVALID;
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: raw %s GetDimensions lowering returned %d\n", query_uav ? "UAV" : "SRV", rc);
        return check("raw-buffer GetDimensions did not lower to common IR");
    }
    uint32_t array_length_count = 0, byte_scale_count = 0, access_count = 0;
    uint32_t load_count = 0, store_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("raw GetDimensions IR lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_ARRAY_LENGTH) array_length_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_MULTIPLY &&
            instruction->source_opcode == DXI_EXTRACTVAL)
            byte_scale_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ACCESS_CHAIN) access_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_LOAD) load_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_STORE) store_count++;
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
    char *msl = NULL;
    size_t msl_size = 0;
    rc = layout ? gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size) : GPU_IR_INVALID;
    gpu_ir_pipeline_layout_release(layout);
    int valid = rc == GPU_IR_OK && msl && msl_size && array_length_count == 1 && byte_scale_count == 1 &&
        access_count == 2 && load_count == 1 && store_count == 1 &&
        strstr(msl, "vx_buffer_ranges[0]") && strstr(msl, "0x00000004u");
    if (!valid) {
        fprintf(stderr, "dxil_ir: raw %s GetDimensions diagnostics rc=%d lengths=%u scales=%u access=%u loads=%u stores=%u msl=%zu\n",
                query_uav ? "UAV" : "SRV", rc, array_length_count, byte_scale_count,
                access_count, load_count, store_count, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        gpu_ir_shader_release(shader);
        return check("raw-buffer GetDimensions did not produce byte lengths in common MSL");
    }
    const uint32_t input_words[4] = {
        is_float ? 0x3f800000u : 11u,
        is_float ? 0x40000000u : 22u,
        is_float ? 0x40400000u : 33u,
        is_float ? 0x40800000u : 44u
    };
    uint32_t expected_words[4] = { 0u, 0u, 0u, 0u };
    uint32_t result_word = is_float ? 0x40600000u : 30u;
    expected_words[query_uav || is_float ? 0u : 1u] = result_word;
    int runtime_ok = execute_raw_buffer_shader_on_metal_with_ranges(shader, input_words, expected_words, 8u, 8u);
    free(msl);
    gpu_ir_shader_release(shader);
    if (!runtime_ok) return check("raw-buffer GetDimensions Metal readback was incorrect");
    printf("dxil_ir: %s raw-buffer GetDimensions returns byte length for %s common-IR shader and Metal readback: ok\n",
           query_uav ? "RWByteAddressBuffer" : "ByteAddressBuffer", overload);
    return 0;
}

static int translate_structured_buffer_dimensions_fixture_variant(uint32_t stride) {
    if (stride < 4u || stride % 4u) return check("structured-buffer test stride must be a multiple of four");
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/typed_buffer_cs.dxil", &module)) return 1;
    int changed = set_typed_uav_kind_to_structured(&module, stride) &&
        use_output_buffer_for_dimensions(&module);
    if (changed && stride > 4u) changed = remove_fixture_typed_buffer_store(&module);
    gpu_ir_shader *shader = NULL;
    int rc = changed ? dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader) : GPU_IR_INVALID;
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: structured GetDimensions stride=%u lowering returned %d\n", stride, rc);
        return check("structured-buffer GetDimensions did not lower to common IR");
    }
    uint32_t array_length_count = 0, stride_divide_count = 0;
    uint32_t access_count = 0, load_count = 0, store_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("structured GetDimensions IR lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_ARRAY_LENGTH) array_length_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_DIVIDE &&
            instruction->source_opcode == DXI_EXTRACTVAL && instruction->operand_count == 2)
            stride_divide_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ACCESS_CHAIN) access_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_LOAD) load_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_STORE) store_count++;
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
    char *msl = NULL;
    size_t msl_size = 0;
    rc = layout ? gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size) : GPU_IR_INVALID;
    gpu_ir_pipeline_layout_release(layout);
    char stride_constant[40];
    snprintf(stride_constant, sizeof stride_constant, "uint(0x%08xu)", stride / 4u);
    int expected_store_count = stride == 4u ? 1 : 0;
    int valid = rc == GPU_IR_OK && msl && msl_size && array_length_count == 1 &&
        stride_divide_count == 1 && access_count == (stride == 4u ? 2u : 1u) &&
        load_count == 1 && store_count == (uint32_t)expected_store_count &&
        strstr(msl, "vx_buffer_ranges[0]") && strstr(msl, stride_constant);
    if (!valid) {
        fprintf(stderr, "dxil_ir: structured GetDimensions stride=%u diagnostics rc=%d lengths=%u divisions=%u access=%u loads=%u stores=%u msl=%zu\n",
                stride, rc, array_length_count, stride_divide_count, access_count, load_count, store_count, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        gpu_ir_shader_release(shader);
        return check("structured-buffer GetDimensions did not divide view length by its stride");
    }
    const uint32_t input_words[4] = { 11u, 22u, 33u, 44u };
    uint32_t expected_words[4] = { 0u, 0u, 0u, 0u };
    if (stride == 4u) expected_words[1] = 24u;
    int runtime_ok = execute_raw_buffer_shader_on_metal_with_ranges(shader, input_words, expected_words, 8u, 8u);
    free(msl);
    gpu_ir_shader_release(shader);
    if (!runtime_ok) return check("structured-buffer GetDimensions Metal execution returned incorrect data");
    printf("dxil_ir: structured-buffer GetDimensions stride=%u lowers through common IR and Metal: ok\n", stride);
    return 0;
}

static int translate_structured_buffer_load_fixture_variant(uint32_t stride, uint32_t element_index,
                                                            uint32_t byte_offset, uint32_t result_field,
                                                            size_t input_range, uint32_t expected_result) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/typed_buffer_cs.dxil", &module)) return 1;
    int changed = set_typed_srv_kind_to_structured(&module, stride) &&
        set_buffer_load_index(&module, element_index) &&
        set_structured_load_byte_offset(&module, byte_offset) &&
        set_buffer_load_result_field(&module, result_field);
    gpu_ir_shader *shader = NULL;
    int rc = changed ? dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader) : GPU_IR_INVALID;
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: structured BufferLoad stride=%u element=%u byte_offset=%u returned %d\n",
                stride, element_index, byte_offset, rc);
        return check("structured BufferLoad did not lower to common IR");
    }
    uint32_t array_length_count = 0, dimension_divide_count = 0, structured_multiply_count = 0;
    uint32_t structured_offset_add_count = 0, access_count = 0, load_count = 0, store_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("structured BufferLoad IR lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_ARRAY_LENGTH) array_length_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_DIVIDE && instruction->source_opcode == DXI_EXTRACTVAL)
            dimension_divide_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_MULTIPLY && instruction->source_opcode == DXI_CALL)
            structured_multiply_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_ADD && instruction->source_opcode == DXI_CALL)
            structured_offset_add_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_ACCESS_CHAIN) access_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_LOAD) load_count++;
        if (instruction->opcode == GPU_IR_SHADER_OP_STORE) store_count++;
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
    char *msl = NULL;
    size_t msl_size = 0;
    rc = layout ? gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size) : GPU_IR_INVALID;
    gpu_ir_pipeline_layout_release(layout);
    char stride_constant[40];
    char offset_word_constant[40];
    snprintf(stride_constant, sizeof stride_constant, "uint(0x%08xu)", stride / 4u);
    uint32_t offset_words = byte_offset / 4u + result_field;
    snprintf(offset_word_constant, sizeof offset_word_constant, "uint(0x%08xu)", offset_words);
    int valid = rc == GPU_IR_OK && msl && msl_size && array_length_count == 1 &&
        dimension_divide_count == 1 && structured_multiply_count == 1 &&
        structured_offset_add_count == (byte_offset || result_field ? 1u : 0u) && access_count == 2 &&
        load_count == 1 && store_count == 1 && strstr(msl, stride_constant) &&
        (!offset_words || strstr(msl, offset_word_constant));
    if (!valid) {
        fprintf(stderr, "dxil_ir: structured BufferLoad diagnostics stride=%u rc=%d lengths=%u dimension_divides=%u multiplies=%u offset_adds=%u access=%u loads=%u stores=%u msl=%zu\n",
                stride, rc, array_length_count, dimension_divide_count, structured_multiply_count,
                structured_offset_add_count, access_count, load_count, store_count, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        gpu_ir_shader_release(shader);
        return check("structured BufferLoad did not use element stride and byte offset in common MSL");
    }
    const uint32_t input_words[4] = { 11u, 22u, 33u, 44u };
    uint32_t expected_words[4] = { 0u, expected_result, 0u, 0u };
    int runtime_ok = execute_raw_buffer_shader_on_metal_with_ranges(shader, input_words, expected_words,
                                                                     input_range, 8u);
    free(msl);
    gpu_ir_shader_release(shader);
    if (!runtime_ok) return check("structured BufferLoad Metal execution returned incorrect data");
    printf("dxil_ir: structured BufferLoad stride=%u element=%u byte_offset=%u field=%u returns %u through common IR/Metal: ok\n",
           stride, element_index, byte_offset, result_field, expected_result);
    return 0;
}

static int check_structured_buffer_load_rejects_field_past_stride(void) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/typed_buffer_cs.dxil", &module)) return 1;
    int changed = set_typed_srv_kind_to_structured(&module, 8u) &&
        set_structured_load_byte_offset(&module, 0u) && set_buffer_load_result_field(&module, 2u);
    gpu_ir_shader *shader = NULL;
    int rc = changed ? dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader) : GPU_IR_INVALID;
    int rejected = changed && rc == GPU_IR_UNSUPPORTED && !shader;
    gpu_ir_shader_release(shader);
    dxil_free(&module);
    if (!rejected) {
        fprintf(stderr, "dxil_ir: structured BufferLoad accepted field 2 with an 8-byte stride (rc=%d)\n", rc);
        return check("structured BufferLoad crossed a structured element boundary");
    }
    puts("dxil_ir: structured BufferLoad rejects components beyond the element stride: ok");
    return 0;
}

static int translate_structured_float_buffer_load_fixture(void) {
    dxil_module module = { 0 };
    if (read_typed_buffer_fixture(&module)) return 1;
    int changed = make_typed_buffer_fixture_float(&module) &&
        set_typed_srv_kind_to_structured(&module, 16u) &&
        set_buffer_load_index(&module, 0u) && set_structured_load_byte_offset(&module, 0u) &&
        set_buffer_load_result_field(&module, 3u);
    gpu_ir_shader *shader = NULL;
    int rc = changed ? dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader) : GPU_IR_INVALID;
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: structured float BufferLoad field 3 returned %d\n", rc);
        return check("structured float BufferLoad did not lower to common IR");
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
    char *msl = NULL;
    size_t msl_size = 0;
    rc = layout ? gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size) : GPU_IR_INVALID;
    gpu_ir_pipeline_layout_release(layout);
    int valid = rc == GPU_IR_OK && msl && msl_size && strstr(msl, "as_type<float>(");
    if (!valid) {
        fprintf(stderr, "dxil_ir: structured float BufferLoad MSL diagnostics rc=%d msl=%zu\n", rc, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        gpu_ir_shader_release(shader);
        return check("structured float BufferLoad did not preserve its 32-bit payload in common MSL");
    }
    const uint32_t input_words[4] = { 0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u };
    const uint32_t expected_words[4] = { 0u, 0x41000000u, 0u, 0u };
    int runtime_ok = execute_raw_buffer_shader_on_metal_with_ranges(shader, input_words, expected_words, 16u, 8u);
    free(msl);
    gpu_ir_shader_release(shader);
    if (!runtime_ok) return check("structured float BufferLoad Metal readback was incorrect");
    puts("dxil_ir: structured float BufferLoad field 3 bitcast and Metal readback: ok");
    return 0;
}

static int translate_raw_buffer_store_common_fixture_variant(const char *overload, uint32_t byte_offset,
                                                             uint32_t mask, uint32_t alignment,
                                                             size_t output_range, int use_dynamic_offset) {
    if (!overload || (strcmp(overload, "i32") && strcmp(overload, "f32"))) return 1;
    int is_float = !strcmp(overload, "f32");
    const char *fixture = is_float ? "tests/shaders12/typed_float_buffer_cs.dxil" :
        "tests/shaders12/typed_buffer_cs.dxil";
    dxil_module module = { 0 };
    if (read_dxil_fixture(fixture, &module)) return 1;
    int changed = set_typed_uav_kind_to_raw(&module) == 1 &&
        set_raw_uav_properties_null(&module) == 1 &&
        convert_buffer_store_to_raw_buffer_store(&module, overload, byte_offset, mask, alignment,
                                                 use_dynamic_offset);
    gpu_ir_shader *shader = NULL;
    int rc = changed ? dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader) : GPU_IR_INVALID;
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: RawBufferStore %s mask=%u alignment=%u lowering returned %d\n",
                overload, mask, alignment, rc);
        return check("RawBufferStore did not lower to common IR");
    }
    uint32_t lane_count = 0;
    while (lane_count < 4 && (mask & (1u << lane_count))) lane_count++;
    uint32_t shift_count = 0, component_add_count = 0, access_count = 0;
    uint32_t load_count = 0, store_count = 0, length_count = 0, bitcast_count = 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction) { gpu_ir_shader_release(shader); return check("RawBufferStore IR lost an instruction"); }
        if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_SHIFT_RIGHT_LOGICAL &&
            instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL && instruction->operand_count == 2)
            shift_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_ARITHMETIC &&
            instruction->operation == GPU_IR_SHADER_ARITH_ADD &&
            instruction->source_api == GPU_IR_SHADER_SOURCE_DXIL && instruction->source_opcode == DXI_CALL &&
            instruction->operand_count == 2)
            component_add_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_ACCESS_CHAIN) access_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_LOAD) load_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_STORE) store_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_ARRAY_LENGTH) length_count++;
        else if (instruction->opcode == GPU_IR_SHADER_OP_CONVERT &&
                 instruction->operation == GPU_IR_SHADER_CONVERT_BITCAST) bitcast_count++;
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
    char *msl = NULL;
    size_t msl_size = 0;
    rc = layout ? gpu_ir_msl_emit_compute_with_layout(shader, layout, &msl, &msl_size) : GPU_IR_INVALID;
    gpu_ir_pipeline_layout_release(layout);
    uint32_t expected_store_count = lane_count;
    uint32_t expected_bitcast_count = is_float ? lane_count + 1u : 0u;
    int dynamic_offset_seen = !use_dynamic_offset;
    if (use_dynamic_offset && msl) {
        for (const char *line = msl; *line;) {
            const char *end = strchr(line, '\n');
            size_t line_length = end ? (size_t)(end - line) : strlen(line);
            if (line_length < 512) {
                char line_copy[512];
                memcpy(line_copy, line, line_length);
                line_copy[line_length] = '\0';
                if (strstr(line_copy, "vx_value_") && strstr(line_copy, " >> uint(0x00000002u)"))
                    dynamic_offset_seen = 1;
            }
            if (!end) break;
            line = end + 1;
        }
    }
    int valid = rc == GPU_IR_OK && msl && msl_size && shift_count == 1 &&
        component_add_count == (lane_count ? lane_count - 1u : 0u) &&
        access_count == lane_count + 1u && load_count == 1 && store_count == expected_store_count &&
        length_count == 1 && bitcast_count == expected_bitcast_count &&
        strstr(msl, "vx_buffer_ranges[0]") && strstr(msl, "[[buffer(0)]]") &&
        strstr(msl, "[[buffer(1)]]") && (!is_float || strstr(msl, "as_type<uint>(")) &&
        dynamic_offset_seen;
    if (!valid) {
        fprintf(stderr, "dxil_ir: RawBufferStore %s mask=%u diagnostics rc=%d lanes=%u shifts=%u adds=%u access=%u loads=%u stores=%u lengths=%u bitcasts=%u msl=%zu\n",
                overload, mask, rc, lane_count, shift_count, component_add_count, access_count,
                load_count, store_count, length_count, bitcast_count, msl_size);
        if (msl) fwrite(msl, 1, msl_size, stderr);
        free(msl);
        gpu_ir_shader_release(shader);
        return check("RawBufferStore lanes did not emit bounded common MSL");
    }
    const uint32_t input_words[4] = {
        is_float ? 0x3f800000u : 11u,
        is_float ? 0x40000000u : use_dynamic_offset ? 0u : 22u,
        is_float ? 0x40400000u : 33u,
        is_float ? 0x40800000u : 44u
    };
    const uint32_t stored_value = is_float ? 0x40200000u : use_dynamic_offset ? 4u : 26u;
    uint32_t expected_words[4] = { 0u, 0u, 0u, 0u };
    uint32_t effective_byte_offset = use_dynamic_offset ? stored_value : byte_offset;
    uint32_t base_word = effective_byte_offset / 4u;
    for (uint32_t lane = 0; lane < lane_count; lane++) {
        uint32_t word = base_word + lane;
        if ((uint64_t)(word + 1u) * 4u <= output_range) expected_words[word] = stored_value;
    }
    int runtime_ok = execute_raw_buffer_shader_on_metal_with_ranges(shader, input_words, expected_words,
                                                                    sizeof input_words, output_range);
    free(msl);
    gpu_ir_shader_release(shader);
    if (!runtime_ok) return check("RawBufferStore common-IR Metal readback was incorrect");
    printf("dxil_ir: RawBufferStore opcode 140 %s %soffset %u mask 0x%x respects %zu-byte view range on Metal: ok\n",
           overload, use_dynamic_offset ? "dynamic " : "", effective_byte_offset, mask, output_range);
    return 0;
}

static int check_raw_buffer_store_rejection(const char *overload, uint32_t mask,
                                            uint32_t alignment, const char *label) {
    int is_float = overload && !strcmp(overload, "f32");
    const char *fixture = is_float ? "tests/shaders12/typed_float_buffer_cs.dxil" :
        "tests/shaders12/typed_buffer_cs.dxil";
    dxil_module module = { 0 };
    if (!overload || read_dxil_fixture(fixture, &module)) return 1;
    int changed = set_typed_uav_kind_to_raw(&module) == 1 &&
        set_raw_uav_properties_null(&module) == 1 &&
            convert_buffer_store_to_raw_buffer_store(&module, overload, 0, mask, alignment, 0);
    gpu_ir_shader *shader = NULL;
    int rc = changed ? dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader) : GPU_IR_INVALID;
    int rejected = changed && rc == GPU_IR_UNSUPPORTED && !shader;
    gpu_ir_shader_release(shader);
    dxil_free(&module);
    if (!rejected) {
        fprintf(stderr, "dxil_ir: RawBufferStore accepted unsupported %s (rc=%d)\n", label, rc);
        return check("RawBufferStore accepted an unsupported mask or alignment");
    }
    return 0;
}

static int translate_byteaddress_load_lane_fixtures(const char *output_path, int execute_runtime) {
    static const uint32_t expected_values[] = { 15, 26, 37, 48 };
    for (uint32_t field = 0; field < 4; field++) {
        const char *lane_output = field == 3 ? output_path : NULL;
        if (translate_raw_buffer_common_fixture_variant(lane_output, 0, 0, 0, field,
                                                         expected_values[field], execute_runtime)) return 1;
    }
    const uint32_t float_input_words[4] = { 0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u };
    const uint32_t float_expected_values[4] = { 0x40200000u, 0x40800000u, 0x40b00000u, 0x40e00000u };
    for (uint32_t field = 0; field < 4; field++) {
        if (translate_raw_buffer_float_common_fixture_variant(0, field, float_input_words,
                                                              float_expected_values[field], execute_runtime,
                                                              field == 3 ? output_path : NULL))
            return 1;
    }
    static const struct { uint32_t mask, field; } prefix_masks[] = {
        { 3u, 1u }, { 7u, 2u }, { 15u, 3u }
    };
    for (uint32_t i = 0; i < sizeof prefix_masks / sizeof prefix_masks[0]; i++) {
        uint32_t field = prefix_masks[i].field;
        if (translate_raw_buffer_common_fixture_variant(NULL, 1, 0, prefix_masks[i].mask,
                                                         field, expected_values[field], execute_runtime))
            return 1;
    }
    return 0;
}

static int translate_raw_buffer_common_fixture(const char *output_path) {
    if (translate_raw_buffer_common_fixture_variant(NULL, 0, 4, 1, 0, 26, 1)) return 1;
    if (translate_byteaddress_load_lane_fixtures(NULL, 1)) return 1;
    if (translate_raw_buffer_common_fixture_variant(output_path, 1, 4, 1, 0, 26, 1)) return 1;
    if (translate_raw_buffer_common_fixture_variant(NULL, 1, 0, 2, 1, 26, 1)) return 1;
    if (translate_raw_buffer_common_fixture_variant(NULL, 1, 0, 4, 2, 37, 1)) return 1;
    if (translate_raw_buffer_common_fixture_variant(NULL, 1, 0, 8, 3, 48, 1)) return 1;
    if (translate_raw_buffer_float_common_fixture()) return 1;
    if (translate_raw_buffer_dimensions_fixture_variant("i32", 0)) return 1;
    if (translate_raw_buffer_dimensions_fixture_variant("f32", 0)) return 1;
    if (translate_raw_buffer_dimensions_fixture_variant("i32", 1)) return 1;
    if (translate_raw_buffer_dimensions_fixture_variant("f32", 1)) return 1;
    if (translate_structured_buffer_dimensions_fixture_variant(4u)) return 1;
    if (translate_structured_buffer_dimensions_fixture_variant(8u)) return 1;
    if (translate_structured_buffer_load_fixture_variant(4u, 1u, 0u, 0u, 8u, 24u)) return 1;
    if (translate_structured_buffer_load_fixture_variant(8u, 0u, 4u, 0u, 8u, 23u)) return 1;
    if (translate_structured_buffer_load_fixture_variant(8u, 1u, 0u, 0u, 8u, 1u)) return 1;
    if (translate_structured_buffer_load_fixture_variant(8u, 0u, 0u, 1u, 8u, 23u)) return 1;
    if (translate_structured_buffer_load_fixture_variant(16u, 0u, 4u, 2u, 16u, 45u)) return 1;
    if (check_structured_buffer_load_rejects_field_past_stride()) return 1;
    if (translate_structured_float_buffer_load_fixture()) return 1;
    static const uint32_t store_masks[] = { 1u, 3u, 7u, 15u };
    for (uint32_t i = 0; i < sizeof store_masks / sizeof store_masks[0]; i++) {
        if (translate_raw_buffer_store_common_fixture_variant("i32", 0, store_masks[i], 4, 16, 0)) return 1;
        if (translate_raw_buffer_store_common_fixture_variant("f32", 0, store_masks[i], 4, 16, 0)) return 1;
    }
    if (translate_raw_buffer_store_common_fixture_variant("i32", 4, 3, 4, 8, 0)) return 1;
    if (translate_raw_buffer_store_common_fixture_variant("f32", 8, 1, 4, 8, 0)) return 1;
    if (translate_raw_buffer_store_common_fixture_variant("i32", 0, 1, 4, 8, 1)) return 1;
    if (check_raw_buffer_store_rejection("i32", 5, 4, "non-prefix write mask")) return 1;
    if (check_raw_buffer_store_rejection("i32", 1, 8, "non-four-byte alignment")) return 1;
    if (check_raw_buffer_load_rejection(0, 1, 0, "typed resource")) return 1;
    if (check_raw_buffer_load_rejection(1, 16, 0, "out-of-range mask")) return 1;
    if (check_raw_buffer_load_rejection(1, 1, 1, "masked-out y lane")) return 1;
    puts("dxil_ir: RawBufferLoad rejects typed resources, invalid masks and masked-out fields: ok");
    return 0;
}

static int convert_typed_buffer_load_to_texture_load(dxil_module *module) {
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) != 68 || instruction->n != 5) continue;
            const dxil_value *opcode = dxil_value_at(module, function, instruction->ops[1]);
            const dxil_value *index = dxil_value_at(module, function, instruction->ops[3]);
            if (!opcode || !index || opcode->type >= module->ntypes || index->type >= module->ntypes) return 0;
            uint32_t opcode_type = opcode->type, index_type = index->type;
            char *callee_name = strdup("dx.op.textureLoad.i32");
            if (!callee_name) return 0;
            uint32_t texture_opcode = append_int_local(module, function, opcode_type, 66);
            uint32_t undef = append_undef_local(module, function, index_type);
            uint32_t *operands = calloc(10, sizeof *operands);
            if (texture_opcode == UINT32_MAX || undef == UINT32_MAX || !operands) {
                free(callee_name);
                free(operands);
                return 0;
            }
            operands[0] = instruction->ops[0];
            operands[1] = texture_opcode;
            operands[2] = instruction->ops[2];
            operands[3] = undef;              // A typed buffer has no mip level.
            operands[4] = instruction->ops[3]; // Buffer element index is coord0.
            operands[5] = operands[6] = operands[7] = operands[8] = operands[9] = undef;
            free(instruction->ops);
            instruction->ops = operands;
            instruction->n = 10;
            if (instruction->ops[0] >= module->nmodule_values) {
                free(callee_name);
                return 0;
            }
            dxil_value *callee = &module->values[instruction->ops[0]];
            free((char *)callee->name);
            callee->name = callee_name;
            return 1;
        }
    }
    return 0;
}

static int translate_direct_typed_buffer_texture_load_mapping_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/typed_buffer_cs.dxil", &module)) return 1;
    int changed = convert_typed_buffer_load_to_texture_load(&module);
    dxil_msl direct = { 0 };
    int failed = !changed || dxil_to_msl(&module, &direct) != 0;
    dxil_free(&module);
    if (!failed && (direct.uses_component_mapping_unsupported_read ||
                    !strstr(direct.msl, "? vx_desc_swizzle(uint4(") ||
                    !strstr(direct.msl, ": uint4(0)"))) {
        fprintf(stderr, "DXIL typed-buffer TextureLoad MSL did not map the loaded SRV vector or preserve the zero fallback\n%s\n",
                direct.msl ? direct.msl : "");
        failed = 1;
    }
    if (!failed && output_path) {
        FILE *output = fopen(output_path, "wb");
        size_t size = strlen(direct.msl);
        int write_failed = !output || fwrite(direct.msl, 1, size, output) != size;
        if (output && fclose(output)) write_failed = 1;
        if (write_failed) failed = check("could not write the direct typed-buffer TextureLoad fixture");
    }
    if (failed) fprintf(stderr, "dxil_ir: typed-buffer TextureLoad component-mapping lowering failed: %s\n", direct.error);
    dxil_msl_free(&direct);
    return failed;
}

static int convert_buffer_load_to_raw_buffer_load_with_type(dxil_module *module, const char *overload,
                                                            uint32_t mask_value, uint32_t result_field) {
    if (!module || !overload || (strcmp(overload, "i32") && strcmp(overload, "f32"))) return 0;
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) != 68 || instruction->n != 5) continue;
            if (instruction->ops[0] >= module->nmodule_values) return 0;
            dxil_value *callee = &module->values[instruction->ops[0]];
            const char prefix[] = "dx.op.rawBufferLoad.";
            size_t name_size = sizeof prefix + strlen(overload);
            char *name = malloc(name_size);
            if (!name) return 0;
            snprintf(name, name_size, "%s%s", prefix, overload);
            free((char *)callee->name);
            callee->name = name;

            const dxil_value *opcode = dxil_value_at(module, function, instruction->ops[1]);
            if (!opcode || opcode->type >= module->ntypes) return 0;
            uint32_t opcode_type = opcode->type;
            uint32_t mask_type = UINT32_MAX;
            for (uint32_t ti = 0; ti < module->ntypes; ti++)
                if (module->types[ti].kind == DXT_INT && module->types[ti].bits == 8) { mask_type = ti; break; }
            if (mask_type == UINT32_MAX) {
                uint32_t count = module->ntypes;
                dxil_type *types = realloc(module->types, (size_t)(count + 1) * sizeof *types);
                if (!types) return 0;
                module->types = types;
                module->types[count] = (dxil_type){ .kind = DXT_INT, .bits = 8 };
                module->ntypes++;
                mask_type = count;
            }
            dxil_type *signature = function_signature(module, callee);
            if (!signature || signature->count != 4 || !signature->members) return 0;
            uint32_t *parameters = realloc(signature->members, 6u * sizeof *parameters);
            if (!parameters) return 0;
            signature->members = parameters;
            signature->members[4] = mask_type;
            signature->members[5] = opcode_type;
            signature->count = 6;
            uint32_t raw_opcode = append_int_local(module, function, opcode_type, 139);
            uint32_t mask = append_int_local(module, function, mask_type, mask_value);
            uint32_t alignment = append_int_local(module, function, opcode_type, 4);
            if (raw_opcode == UINT32_MAX || mask == UINT32_MAX || alignment == UINT32_MAX) return 0;
            uint32_t *operands = realloc(instruction->ops, 7 * sizeof *operands);
            if (!operands) return 0;
            instruction->ops = operands;
            instruction->ops[1] = raw_opcode;
            instruction->ops[5] = mask;
            instruction->ops[6] = alignment;
            instruction->n = 7;
            if (result_field >= 4) return 0;
            for (uint32_t extract_index = 0; extract_index < function->ninsts; extract_index++) {
                dxil_inst *extract = &function->insts[extract_index];
                if (extract->op != DXI_EXTRACTVAL || extract->n != 2 || !extract->ops ||
                    extract->ops[0] != instruction->result || extract->ops[1] != 0) continue;
                extract->ops[1] = result_field;
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

static int convert_buffer_load_to_raw_buffer_load(dxil_module *module, uint32_t mask_value,
                                                  uint32_t result_field) {
    return convert_buffer_load_to_raw_buffer_load_with_type(module, "i32", mask_value, result_field);
}

static int convert_float_buffer_load_to_raw_buffer_load(dxil_module *module, uint32_t mask_value,
                                                        uint32_t result_field) {
    return convert_buffer_load_to_raw_buffer_load_with_type(module, "f32", mask_value, result_field);
}

static int convert_buffer_store_to_raw_buffer_store(dxil_module *module, const char *overload,
                                                    uint32_t byte_offset, uint32_t mask_value,
                                                    uint32_t alignment_value, int use_dynamic_offset) {
    if (!module || !overload || (strcmp(overload, "i32") && strcmp(overload, "f32")) ||
        (use_dynamic_offset && strcmp(overload, "i32"))) return 0;
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) != 69) continue;
            if (instruction->n != 10 || instruction->result != UINT32_MAX ||
                instruction->ops[0] >= module->nmodule_values) return 0;
            dxil_value *callee = &module->values[instruction->ops[0]];
            const dxil_value *opcode = dxil_value_at(module, function, instruction->ops[1]);
            const dxil_value *index = dxil_value_at(module, function, instruction->ops[3]);
            const dxil_value *first_value = dxil_value_at(module, function, instruction->ops[5]);
            const dxil_value *mask = dxil_value_at(module, function, instruction->ops[9]);
            if (!callee || callee->kind != DXV_FUNCTION || !opcode || opcode->type >= module->ntypes ||
                !index || index->type >= module->ntypes || !first_value || first_value->type >= module->ntypes ||
                (use_dynamic_offset && first_value->type != index->type) ||
                !mask || mask->type >= module->ntypes)
                return 0;
            uint32_t opcode_type = opcode->type, index_type = index->type, mask_type = mask->type;
            const dxil_type *index_type_desc = &module->types[index_type];
            const dxil_type *mask_type_desc = &module->types[mask_type];
            if (index_type_desc->kind != DXT_INT || index_type_desc->bits != 32 ||
                mask_type_desc->kind != DXT_INT || mask_type_desc->bits != 8)
                return 0;
            dxil_type *signature = function_signature(module, callee);
            if (!signature || signature->count != 9 || !signature->members) return 0;
            uint32_t *parameters = realloc(signature->members, 10u * sizeof *parameters);
            if (!parameters) return 0;
            signature->members = parameters;
            signature->members[9] = opcode_type;
            signature->count = 10;
            uint32_t raw_opcode = append_int_local(module, function, opcode_type, 140);
            uint32_t raw_offset = use_dynamic_offset ? instruction->ops[5] :
                append_int_local(module, function, index_type, byte_offset);
            uint32_t raw_mask = append_int_local(module, function, mask_type, mask_value);
            uint32_t alignment = append_int_local(module, function, opcode_type, alignment_value);
            if (raw_opcode == UINT32_MAX || raw_offset == UINT32_MAX || raw_mask == UINT32_MAX ||
                alignment == UINT32_MAX)
                return 0;
            uint32_t *operands = realloc(instruction->ops, 11u * sizeof *operands);
            if (!operands) return 0;
            instruction->ops = operands;
            const char prefix[] = "dx.op.rawBufferStore.";
            size_t name_size = sizeof prefix + strlen(overload);
            char *name = malloc(name_size);
            if (!name) return 0;
            snprintf(name, name_size, "%s%s", prefix, overload);
            free((char *)callee->name);
            callee->name = name;
            instruction->ops[1] = raw_opcode;
            instruction->ops[3] = raw_offset;
            instruction->ops[9] = raw_mask;
            instruction->ops[10] = alignment;
            instruction->n = 11;
            return 1;
        }
    }
    return 0;
}

static int check_raw_buffer_load_rejection(int use_raw_resource, uint32_t mask_value,
                                          uint32_t result_field, const char *label) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/typed_buffer_cs.dxil", &module)) return 1;
    int changed = 1;
    if (use_raw_resource)
        changed = set_typed_srv_kind_to_raw(&module) == 1 && set_raw_srv_properties_null(&module) == 1;
    if (changed) changed = convert_buffer_load_to_raw_buffer_load(&module, mask_value, result_field);
    gpu_ir_shader *shader = NULL;
    int rc = changed ? dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader) : GPU_IR_INVALID;
    int rejected = changed && rc == GPU_IR_UNSUPPORTED && !shader;
    gpu_ir_shader_release(shader);
    dxil_free(&module);
    if (!rejected) {
        fprintf(stderr, "dxil_ir: RawBufferLoad accepted unsupported %s (rc=%d)\n", label, rc);
        return check("RawBufferLoad accepted an unsupported resource shape or mask");
    }
    return 0;
}

static int translate_direct_buffer_mapping_shape_fixture(const char *output_path, uint32_t kind, const char *label) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/typed_buffer_cs.dxil", &module)) return 1;
    int changed = remap_typed_srv_metadata_to_kind(&module, kind);
    if (kind == 11) changed = changed && convert_buffer_load_to_raw_buffer_load(&module, 15, 0);
    if (kind == 12) changed = changed && set_structured_load_byte_offset_zero(&module);
    dxil_msl direct = { 0 };
    int failed = !changed || dxil_to_msl(&module, &direct) != 0;
    if (!failed && (direct.uses_component_mapping_unsupported_read ||
                    !strstr(direct.msl, "vx_desc_swizzle(vx_ld4("))) {
        fprintf(stderr, "DXIL %s-buffer MSL did not apply SRV component mapping to its loaded vector\n%s\n",
                label, direct.msl ? direct.msl : "");
        failed = 1;
    }
    if (!failed && output_path) {
        FILE *output = fopen(output_path, "wb");
        size_t size = strlen(direct.msl);
        int write_failed = !output || fwrite(direct.msl, 1, size, output) != size;
        if (output && fclose(output)) write_failed = 1;
        if (write_failed) failed = check("could not write the direct buffer-load fixture");
    }
    if (failed) fprintf(stderr, "dxil_ir: direct %s-buffer component-mapping lowering failed: %s\n", label, direct.error);
    dxil_msl_free(&direct);
    dxil_free(&module);
    return failed;
}

static int translate_direct_raw_buffer_mapping_fixture(const char *output_path) {
    return translate_direct_buffer_mapping_shape_fixture(output_path, 11, "raw");
}

static int translate_direct_structured_buffer_mapping_fixture(const char *output_path) {
    return translate_direct_buffer_mapping_shape_fixture(output_path, 12, "structured");
}

static int isolate_derivatives_from_samplegrad(dxil_module *module) {
    int converted = 0;
    for (uint32_t fi = 0; fi < module->nfunctions; fi++) {
        dxil_function *function = &module->functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            if (dxil_dxop(module, function, instruction) != 63) continue;
            if (instruction->n < 18 || instruction->ops[0] >= module->nmodule_values) return 0;
            const dxil_value *opcode = dxil_value_at(module, function, instruction->ops[1]);
            if (!opcode || opcode->type >= module->ntypes) return 0;
            uint32_t opcode_type = opcode->type;
            uint32_t *operands = malloc(12 * sizeof *operands);
            if (!operands) return 0;
            memcpy(operands, instruction->ops, 11 * sizeof *operands);
            operands[11] = instruction->ops[instruction->n - 1];
            uint32_t sample_opcode = append_int_local(module, function, opcode_type, 60);
            if (sample_opcode == UINT32_MAX) {
                free(operands);
                return 0;
            }
            operands[1] = sample_opcode;
            dxil_value *callee = &module->values[instruction->ops[0]];
            char *sample_name = strdup("dx.op.sample.f32");
            if (!sample_name) {
                free(operands);
                return 0;
            }
            free((char *)callee->name);
            callee->name = sample_name;
            free(instruction->ops);
            instruction->ops = operands;
            instruction->n = 12;
            converted++;
        }
    }
    return converted != 0;
}

static int translate_derivative_modes_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/samplegrad_ps.dxil", &module)) return 1;
    if (!isolate_derivatives_from_samplegrad(&module)) {
        dxil_free(&module);
        return check("could not isolate derivative operations in samplegrad fixture");
    }
    int fine_x = 0, fine_y = 0;
    for (uint32_t fi = 0; fi < module.nfunctions; fi++) {
        dxil_function *function = &module.functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *instruction = &function->insts[ii];
            int opcode = dxil_dxop(&module, function, instruction);
            if ((opcode != 83 || fine_x) && (opcode != 84 || fine_y)) continue;
            if (instruction->n != 3) continue;
            const dxil_value *value = dxil_value_at(&module, function, instruction->ops[1]);
            if (!value || value->type >= module.ntypes) continue;
            uint32_t replacement = append_int_local(&module, function, value->type, opcode == 83 ? 85 : 86);
            if (replacement == UINT32_MAX) {
                dxil_free(&module);
                return check("could not synthesize fine derivative fixture");
            }
            instruction->ops[1] = replacement;
            if (opcode == 83) fine_x = 1; else fine_y = 1;
        }
    }
    dxil_msl direct = { 0 };
    int failed = !fine_x || !fine_y || dxil_to_msl(&module, &direct) != 0;
    dxil_free(&module);
    if (!failed && (!strstr(direct.msl, "quad_shuffle(") || !strstr(direct.msl, "ushort(0u)") ||
                    !strstr(direct.msl, "ushort(1u)") || !strstr(direct.msl, "ushort(2u)") ||
                    !strstr(direct.msl, "quad_shuffle_xor(") ||
                    !strstr(direct.msl, "thread_index_in_simdgroup"))) {
        fprintf(stderr, "DXIL coarse/fine derivatives did not preserve their distinct MSL lowering\n%s\n",
                direct.msl ? direct.msl : "");
        failed = 1;
    }
    if (!failed && output_path) {
        FILE *output = fopen(output_path, "wb");
        size_t size = strlen(direct.msl);
        int write_failed = !output || fwrite(direct.msl, 1, size, output) != size;
        if (output && fclose(output)) write_failed = 1;
        if (write_failed) failed = check("could not write the DXIL derivative-mode fixture");
    }
    if (failed) fprintf(stderr, "dxil_ir: coarse/fine derivative lowering failed: %s\n", direct.error);
    dxil_msl_free(&direct);
    return failed;
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

static int translate_integer_minmax_dxil_fixture(const char *output_path) {
    static const uint32_t expected_operations[] = {
        GPU_IR_SHADER_ARITH_MINIMUM, GPU_IR_SHADER_ARITH_MAXIMUM,
        GPU_IR_SHADER_ARITH_MINIMUM, GPU_IR_SHADER_ARITH_MAXIMUM
    };
    static const uint32_t expected_flags[] = {
        0, 0, GPU_IR_SHADER_FLAG_SIGNED, GPU_IR_SHADER_FLAG_SIGNED
    };
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/integer_minmax_buffer_cs.dxil", &module)) return 1;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: integer-minmax lowering returned %d\n", rc);
        return check("DXIL integer min/max buffer shader did not lower to common IR");
    }
    uint32_t seen = 0;
    int valid = 1;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC ||
            (instruction->operation != GPU_IR_SHADER_ARITH_MINIMUM &&
             instruction->operation != GPU_IR_SHADER_ARITH_MAXIMUM)) continue;
        if (seen >= sizeof expected_operations / sizeof expected_operations[0] ||
            instruction->operation != expected_operations[seen] || instruction->flags != expected_flags[seen] ||
            instruction->operand_count != 2) {
            valid = 0;
            break;
        }
        seen++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    if (valid && seen == sizeof expected_operations / sizeof expected_operations[0])
        rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    else rc = GPU_IR_INVALID;
    gpu_ir_shader_release(shader);
    valid = valid && rc == GPU_IR_OK && msl && msl_size &&
        strstr(msl, "min(vx_value_") && strstr(msl, "max(vx_value_") &&
        strstr(msl, "as_type<uint>(min(as_type<int>(") &&
        strstr(msl, "as_type<uint>(max(as_type<int>(");
    if (valid && output_path) {
        FILE *output = fopen(output_path, "wb");
        valid = output && fwrite(msl, 1, msl_size, output) == msl_size;
        if (output && fclose(output)) valid = 0;
    }
    free(msl);
    if (!valid) return check("DXIL IMin/IMax/UMin/UMax did not preserve opcode signedness in common MSL");
    puts("dxil_ir: DXIL IMin/IMax/UMin/UMax lower to signedness-correct common MSL: ok");
    return 0;
}

static int translate_integer_mad_dxil_fixture(const char *output_path) {
    static const uint32_t expected_flags[] = { GPU_IR_SHADER_FLAG_SIGNED, 0 };
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/integer_mad_buffer_cs.dxil", &module)) return 1;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: integer-MAD lowering returned %d\n", rc);
        return check("DXIL IMad/UMad buffer shader did not lower to common IR");
    }
    uint32_t seen = 0;
    int valid = 1;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC ||
            instruction->operation != GPU_IR_SHADER_ARITH_INTEGER_MAD) continue;
        const gpu_ir_shader_type *type = NULL;
        for (uint32_t type_index = 0; type_index < gpu_ir_shader_type_count(shader); type_index++) {
            const gpu_ir_shader_type *candidate = gpu_ir_shader_type_at(shader, type_index);
            if (candidate && candidate->id == instruction->result_type_id) type = candidate;
        }
        if (seen >= sizeof expected_flags / sizeof expected_flags[0] ||
            instruction->flags != expected_flags[seen] || instruction->operand_count != 3 ||
            instruction->source_api != GPU_IR_SHADER_SOURCE_DXIL || !type ||
            type->kind != GPU_IR_SHADER_TYPE_INT || type->bit_width != 32) {
            valid = 0;
            break;
        }
        seen++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    if (valid && seen == sizeof expected_flags / sizeof expected_flags[0])
        rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    else rc = GPU_IR_INVALID;
    gpu_ir_shader_release(shader);
    valid = valid && rc == GPU_IR_OK && msl && msl_size &&
        strstr(msl, "uint(vx_value_") && strstr(msl, " * uint(vx_value_") &&
        strstr(msl, " + uint(vx_value_");
    if (valid && output_path) {
        FILE *output = fopen(output_path, "wb");
        valid = output && fwrite(msl, 1, msl_size, output) == msl_size;
        if (output && fclose(output)) valid = 0;
    }
    free(msl);
    if (!valid) return check("DXIL IMad/UMad did not retain signedness and modulo-32-bit common MSL semantics");
    puts("dxil_ir: DXIL IMad/UMad lower to signedness-preserving modulo-32-bit common MSL: ok");
    return 0;
}

static int check_vector_integer_mad_adapter(void) {
    uint32_t entry_parameters[] = { 2, 2, 2, 3, 3, 3 };
    uint32_t vector2_parameters[] = { 1, 2, 2, 2 };
    uint32_t vector4_parameters[] = { 1, 3, 3, 3 };
    dxil_type types[] = {
        { .kind = DXT_VOID },
        { .kind = DXT_INT, .bits = 32 },
        { .kind = DXT_VECTOR, .elem = 1, .count = 2 },
        { .kind = DXT_VECTOR, .elem = 1, .count = 4 },
        { .kind = DXT_FUNCTION, .elem = 0, .count = 6, .members = entry_parameters },
        { .kind = DXT_FUNCTION, .elem = 2, .count = 4, .members = vector2_parameters },
        { .kind = DXT_FUNCTION, .elem = 3, .count = 4, .members = vector4_parameters }
    };
    dxil_value values[] = {
        { .kind = DXV_FUNCTION, .type = 4, .name = "main" },
        { .kind = DXV_FUNCTION, .type = 5, .name = "dx.op.tertiary.v2i32" },
        { .kind = DXV_FUNCTION, .type = 6, .name = "dx.op.tertiary.v4i32" },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 48 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 49 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 4 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 },
        { .kind = DXV_CONST_INT, .type = 1, .u.i = 1 }
    };
    dxil_value locals[] = {
        { .kind = DXV_ARG, .type = 2, .u.index = 0 },
        { .kind = DXV_ARG, .type = 2, .u.index = 1 },
        { .kind = DXV_ARG, .type = 2, .u.index = 2 },
        { .kind = DXV_ARG, .type = 3, .u.index = 3 },
        { .kind = DXV_ARG, .type = 3, .u.index = 4 },
        { .kind = DXV_ARG, .type = 3, .u.index = 5 },
        { .kind = DXV_INST, .type = 2, .u.index = 0 },
        { .kind = DXV_INST, .type = 3, .u.index = 1 }
    };
    uint32_t vector2_operands[] = { 1, 3, 9, 10, 11 };
    uint32_t vector4_operands[] = { 2, 4, 12, 13, 14 };
    dxil_inst instructions[] = {
        { .op = DXI_CALL, .type = 2, .result = 15, .block = 0, .n = 5, .ops = vector2_operands },
        { .op = DXI_CALL, .type = 3, .result = 16, .block = 0, .n = 5, .ops = vector4_operands },
        { .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function functions[] = {
        { .type = 4, .value = 0, .first_arg = 9, .nblocks = 1, .ninsts = 3,
          .insts = instructions, .nlocals = 8, .locals = locals },
        { .type = 5, .value = 1, .is_declaration = 1 },
        { .type = 6, .value = 2, .is_declaration = 1 }
    };
    dxil_md metadata[8] = { 0 };
    uint32_t dimensions[] = { 3, 4, 5 };
    uint32_t properties[] = { 2, 6 };
    uint32_t entry_ops[] = { 1, 0, 0, 0, 7 };
    uint32_t entry_list[] = { 7 };
    metadata[0] = (dxil_md){ .kind = DXM_VALUE, .value = 0 };
    metadata[1] = (dxil_md){ .kind = DXM_VALUE, .value = 5 };
    metadata[2] = (dxil_md){ .kind = DXM_VALUE, .value = 6 };
    metadata[3] = (dxil_md){ .kind = DXM_VALUE, .value = 7 };
    metadata[4] = (dxil_md){ .kind = DXM_VALUE, .value = 8 };
    metadata[5] = (dxil_md){ .kind = DXM_NODE, .n = 3, .ops = dimensions };
    metadata[6] = (dxil_md){ .kind = DXM_NODE, .n = 2, .ops = properties };
    metadata[7] = (dxil_md){ .kind = DXM_NODE, .n = 5, .ops = entry_ops };
    dxil_named_md named = { .name = "dx.entryPoints", .n = 1, .nodes = entry_list };
    dxil_module module = { .shader_kind = 5, .ntypes = 7, .types = types,
        .nmodule_values = 9, .values = values, .nfunctions = 3, .functions = functions,
        .nmd = 8, .md = metadata, .nnamed = 1, .named = &named };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: vector IMad/UMad lowering returned %d\n", rc);
        return check("DXIL vector IMad/UMad did not lower to common IR");
    }
    const uint32_t expected_flags[] = { GPU_IR_SHADER_FLAG_SIGNED, 0 };
    const uint32_t expected_lanes[] = { 2, 4 };
    uint32_t seen = 0;
    int valid = 1;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC ||
            instruction->operation != GPU_IR_SHADER_ARITH_INTEGER_MAD) continue;
        const gpu_ir_shader_type *type = gpu_ir_shader_find_type(shader, instruction->result_type_id);
        const gpu_ir_shader_type *element = type ? gpu_ir_shader_find_type(shader, type->element_type_id) : NULL;
        if (seen >= sizeof expected_lanes / sizeof expected_lanes[0] ||
            instruction->flags != expected_flags[seen] || instruction->operand_count != 3 ||
            instruction->source_api != GPU_IR_SHADER_SOURCE_DXIL || !type ||
            type->kind != GPU_IR_SHADER_TYPE_VECTOR || type->element_count != expected_lanes[seen] ||
            !element || element->kind != GPU_IR_SHADER_TYPE_INT || element->bit_width != 32) {
            valid = 0;
            break;
        }
        seen++;
    }
    gpu_ir_shader_release(shader);
    if (!valid || seen != sizeof expected_lanes / sizeof expected_lanes[0])
        return check("DXIL vector IMad/UMad lost opcode signedness or lane shape in common IR");
    puts("dxil_ir: DXIL vector IMad/UMad preserve signedness and 2/4-lane shape in common IR: ok");
    return 0;
}

static int check_vector_integer_mad_msl(const char *output_path) {
    const gpu_ir_shader_type types[] = {
        { .id = 1, .kind = GPU_IR_SHADER_TYPE_VOID },
        { .id = 2, .kind = GPU_IR_SHADER_TYPE_INT, .bit_width = 32, .signedness = 1 },
        { .id = 3, .kind = GPU_IR_SHADER_TYPE_VECTOR, .element_type_id = 2, .element_count = 2 },
        { .id = 4, .kind = GPU_IR_SHADER_TYPE_FUNCTION, .element_type_id = 1 },
        { .id = 5, .kind = GPU_IR_SHADER_TYPE_INT, .bit_width = 32 },
        { .id = 6, .kind = GPU_IR_SHADER_TYPE_VECTOR, .element_type_id = 5, .element_count = 4 }
    };
    const gpu_ir_shader_function functions[] = {
        { .id = 10, .result_type_id = 1, .function_type_id = 4, .first_block = 0,
          .block_count = 1, .first_instruction = 24, .instruction_count = 2 },
        { .id = 12, .result_type_id = 1, .function_type_id = 4, .first_block = 1,
          .block_count = 1, .first_instruction = 26, .instruction_count = 2 }
    };
    const gpu_ir_shader_block blocks[] = {
        { .function_index = 0, .label_id = 11, .first_instruction = 24, .instruction_count = 2,
          .terminator_opcode = GPU_IR_SHADER_OP_RETURN },
        { .function_index = 1, .label_id = 13, .first_instruction = 26, .instruction_count = 2,
          .terminator_opcode = GPU_IR_SHADER_OP_RETURN }
    };
    const gpu_ir_shader_operand operands[] = {
        { GPU_IR_SHADER_OPERAND_ID, 30 }, { GPU_IR_SHADER_OPERAND_ID, 31 },
        { GPU_IR_SHADER_OPERAND_ID, 32 }, { GPU_IR_SHADER_OPERAND_ID, 33 },
        { GPU_IR_SHADER_OPERAND_ID, 34 }, { GPU_IR_SHADER_OPERAND_ID, 35 },
        { GPU_IR_SHADER_OPERAND_ID, 80 }, { GPU_IR_SHADER_OPERAND_ID, 81 },
        { GPU_IR_SHADER_OPERAND_ID, 82 }, { GPU_IR_SHADER_OPERAND_ID, 83 },
        { GPU_IR_SHADER_OPERAND_ID, 84 }, { GPU_IR_SHADER_OPERAND_ID, 85 },
        { GPU_IR_SHADER_OPERAND_ID, 86 }, { GPU_IR_SHADER_OPERAND_ID, 87 },
        { GPU_IR_SHADER_OPERAND_ID, 88 }, { GPU_IR_SHADER_OPERAND_ID, 89 },
        { GPU_IR_SHADER_OPERAND_ID, 90 }, { GPU_IR_SHADER_OPERAND_ID, 91 },
        { GPU_IR_SHADER_OPERAND_ID, 70 }, { GPU_IR_SHADER_OPERAND_ID, 71 },
        { GPU_IR_SHADER_OPERAND_ID, 72 }, { GPU_IR_SHADER_OPERAND_ID, 100 },
        { GPU_IR_SHADER_OPERAND_ID, 101 }, { GPU_IR_SHADER_OPERAND_ID, 102 }
    };
    const uint32_t literal_words[] = {
        UINT32_MAX, 0x7fffffffu, 2, 3, 1, 1,
        UINT32_MAX, 1, 0x80000000u, 0x7fffffffu,
        2, 3, 3, 2, 1, 2, 1, 3
    };
    gpu_ir_shader_instruction instructions[28] = { 0 };
    for (uint32_t i = 0; i < 6; i++) {
        instructions[i] = (gpu_ir_shader_instruction){
            .source_api = GPU_IR_SHADER_SOURCE_DXIL, .opcode = GPU_IR_SHADER_OP_CONSTANT,
            .operation = GPU_IR_SHADER_CONSTANT_SCALAR, .source_opcode = 0,
            .result_id = 30u + i, .result_type_id = 2,
            .function_index = UINT32_MAX, .block_index = UINT32_MAX,
            .first_literal = i, .literal_word_count = 1
        };
    }
    for (uint32_t i = 0; i < 3; i++) {
        instructions[6 + i] = (gpu_ir_shader_instruction){
            .source_api = GPU_IR_SHADER_SOURCE_DXIL, .opcode = GPU_IR_SHADER_OP_CONSTANT,
            .operation = GPU_IR_SHADER_CONSTANT_COMPOSITE, .source_opcode = 0,
            .result_id = 70u + i, .result_type_id = 3,
            .function_index = UINT32_MAX, .block_index = UINT32_MAX,
            .first_operand = i * 2u, .operand_count = 2
        };
    }
    for (uint32_t i = 0; i < 12; i++) {
        instructions[9 + i] = (gpu_ir_shader_instruction){
            .source_api = GPU_IR_SHADER_SOURCE_DXIL, .opcode = GPU_IR_SHADER_OP_CONSTANT,
            .operation = GPU_IR_SHADER_CONSTANT_SCALAR, .source_opcode = 0,
            .result_id = 80u + i, .result_type_id = 5,
            .function_index = UINT32_MAX, .block_index = UINT32_MAX,
            .first_literal = 6u + i, .literal_word_count = 1
        };
    }
    for (uint32_t i = 0; i < 3; i++) {
        instructions[21 + i] = (gpu_ir_shader_instruction){
            .source_api = GPU_IR_SHADER_SOURCE_DXIL, .opcode = GPU_IR_SHADER_OP_CONSTANT,
            .operation = GPU_IR_SHADER_CONSTANT_COMPOSITE, .source_opcode = 0,
            .result_id = 100u + i, .result_type_id = 6,
            .function_index = UINT32_MAX, .block_index = UINT32_MAX,
            .first_operand = 6u + i * 4u, .operand_count = 4
        };
    }
    instructions[24] = (gpu_ir_shader_instruction){
        .source_api = GPU_IR_SHADER_SOURCE_DXIL, .opcode = GPU_IR_SHADER_OP_ARITHMETIC,
        .operation = GPU_IR_SHADER_ARITH_INTEGER_MAD, .source_opcode = 48,
        .flags = GPU_IR_SHADER_FLAG_SIGNED, .result_id = 110, .result_type_id = 3,
        .function_index = 0, .block_index = 0, .first_operand = 18, .operand_count = 3
    };
    instructions[25] = (gpu_ir_shader_instruction){
        .source_api = GPU_IR_SHADER_SOURCE_DXIL, .opcode = GPU_IR_SHADER_OP_RETURN,
        .source_opcode = 1, .function_index = 0, .block_index = 0
    };
    instructions[26] = (gpu_ir_shader_instruction){
        .source_api = GPU_IR_SHADER_SOURCE_DXIL, .opcode = GPU_IR_SHADER_OP_ARITHMETIC,
        .operation = GPU_IR_SHADER_ARITH_INTEGER_MAD, .source_opcode = 49,
        .result_id = 111, .result_type_id = 6,
        .function_index = 1, .block_index = 1, .first_operand = 21, .operand_count = 3
    };
    instructions[27] = (gpu_ir_shader_instruction){
        .source_api = GPU_IR_SHADER_SOURCE_DXIL, .opcode = GPU_IR_SHADER_OP_RETURN,
        .source_opcode = 1, .function_index = 1, .block_index = 1
    };
    const gpu_ir_shader_desc description = {
        .source_api = GPU_IR_SHADER_SOURCE_DXIL, .stage_mask = GPU_IR_STAGE_COMPUTE,
        .entry_function_id = 10, .threadgroup_size = { 1, 1, 1 },
        .types = types, .type_count = sizeof types / sizeof types[0],
        .functions = functions, .function_count = sizeof functions / sizeof functions[0],
        .blocks = blocks, .block_count = sizeof blocks / sizeof blocks[0],
        .instructions = instructions, .instruction_count = sizeof instructions / sizeof instructions[0],
        .operands = operands, .operand_count = sizeof operands / sizeof operands[0],
        .literal_words = literal_words, .literal_word_count = sizeof literal_words / sizeof literal_words[0]
    };
    gpu_ir_shader *shader = gpu_ir_shader_create(&description);
    if (!shader) return check("common IR rejected vector integer MAD with signed 32-bit lanes");
    char *msl = NULL;
    size_t msl_size = 0;
    int rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    gpu_ir_shader_release(shader);
    int valid = rc == GPU_IR_OK && msl && msl_size &&
        strstr(msl, "as_type<int2>(as_type<uint2>(") &&
        strstr(msl, " * as_type<uint2>(") && strstr(msl, " + as_type<uint2>(") &&
        strstr(msl, "vx_value_111 = uint4(") && strstr(msl, " * uint4(") && strstr(msl, " + uint4(");
    if (valid && output_path) {
        FILE *output = fopen(output_path, "wb");
        valid = output && fwrite(msl, 1, msl_size, output) == msl_size;
        if (output && fclose(output)) valid = 0;
    }
    if (!valid)
        fprintf(stderr, "dxil_ir: vector integer MAD emitter rc=%d size=%zu\n%s",
                rc, msl_size, msl ? msl : "(no MSL output)\n");
    free(msl);
    if (!valid) return check("common MSL did not preserve signed vector MAD modulo-32-bit semantics");
    puts("dxil_ir: common MSL emits signed vector IMad through unsigned lanes with bit-preserving results: ok");
    return 0;
}

static int translate_float_mad_dxil_fixture(const char *output_path) {
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/float_mad_buffer_cs.dxil", &module)) return 1;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: float-MAD lowering returned %d\n", rc);
        return check("DXIL FMad buffer shader did not lower to common IR");
    }
    uint32_t seen = 0;
    int valid = 1;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC ||
            instruction->operation != GPU_IR_SHADER_ARITH_FLOAT_MAD) continue;
        const gpu_ir_shader_type *type = gpu_ir_shader_find_type(shader, instruction->result_type_id);
        if (seen || instruction->flags != GPU_IR_SHADER_FLAG_FLOAT || instruction->operand_count != 3 ||
            instruction->source_api != GPU_IR_SHADER_SOURCE_DXIL || !type ||
            type->kind != GPU_IR_SHADER_TYPE_FLOAT || type->bit_width != 32) {
            valid = 0;
            break;
        }
        seen++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    if (valid && seen == 1) rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    else rc = GPU_IR_INVALID;
    gpu_ir_shader_release(shader);
    valid = valid && rc == GPU_IR_OK && msl && msl_size &&
        strstr(msl, "volatile float vx_fmad_product_") &&
        strstr(msl, " = vx_value_") && strstr(msl, " * vx_value_") &&
        strstr(msl, " + vx_value_");
    if (valid && output_path) {
        FILE *output = fopen(output_path, "wb");
        valid = output && fwrite(msl, 1, msl_size, output) == msl_size;
        if (output && fclose(output)) valid = 0;
    }
    free(msl);
    if (!valid) return check("DXIL FMad did not lower to separately rounded common MSL arithmetic");
    puts("dxil_ir: precise DXIL FMad lowers to separately rounded common MSL arithmetic: ok");
    return 0;
}

static int translate_float_minmax_dxil_fixture(const char *output_path) {
    static const uint32_t expected_operations[] = {
        GPU_IR_SHADER_ARITH_MINIMUM, GPU_IR_SHADER_ARITH_MAXIMUM,
        GPU_IR_SHADER_ARITH_MINIMUM, GPU_IR_SHADER_ARITH_MAXIMUM,
        GPU_IR_SHADER_ARITH_MINIMUM, GPU_IR_SHADER_ARITH_MAXIMUM,
        GPU_IR_SHADER_ARITH_MINIMUM, GPU_IR_SHADER_ARITH_MAXIMUM,
        GPU_IR_SHADER_ARITH_MINIMUM, GPU_IR_SHADER_ARITH_MAXIMUM,
        GPU_IR_SHADER_ARITH_MINIMUM, GPU_IR_SHADER_ARITH_MAXIMUM
    };
    const uint32_t expected_flags = GPU_IR_SHADER_FLAG_FLOAT |
        GPU_IR_SHADER_FLAG_MINMAX_ORDERED_SELECT | GPU_IR_SHADER_FLAG_MINMAX_IGNORE_SINGLE_NAN |
        GPU_IR_SHADER_FLAG_MINMAX_FLUSH_DENORMS;
    dxil_module module = { 0 };
    if (read_dxil_fixture("tests/shaders12/float_minmax_buffer_cs.dxil", &module)) return 1;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: float-minmax lowering returned %d\n", rc);
        return check("DXIL float min/max buffer shader did not lower to common IR");
    }
    uint32_t seen = 0;
    int valid = 1;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (!instruction || instruction->opcode != GPU_IR_SHADER_OP_ARITHMETIC ||
            (instruction->operation != GPU_IR_SHADER_ARITH_MINIMUM &&
             instruction->operation != GPU_IR_SHADER_ARITH_MAXIMUM)) continue;
        if (seen >= sizeof expected_operations / sizeof expected_operations[0] ||
            instruction->operation != expected_operations[seen] || instruction->flags != expected_flags ||
            instruction->operand_count != 2) {
            valid = 0;
            break;
        }
        seen++;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    if (valid && seen == sizeof expected_operations / sizeof expected_operations[0])
        rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    else rc = GPU_IR_INVALID;
    gpu_ir_shader_release(shader);
    valid = valid && rc == GPU_IR_OK && msl && msl_size &&
        strstr(msl, "isnan(") && strstr(msl, "0x80000000u") &&
        strstr(msl, "vx_minmax_cmp_a_") && strstr(msl, "vx_minmax_cmp_b_");
    if (valid && output_path) {
        FILE *output = fopen(output_path, "wb");
        valid = output && fwrite(msl, 1, msl_size, output) == msl_size;
        if (output && fclose(output)) valid = 0;
    }
    free(msl);
    if (!valid) return check("DXIL FMin/FMax did not preserve NaN, operand-order and denormal comparison rules");
    puts("dxil_ir: DXIL FMin/FMax lower with NaN, operand-order and denormal comparison semantics: ok");
    return 0;
}

static int translate_dxil_atomic_add(const char *output_path, const char *ops_output_path,
                                     const char *compare_exchange_output_path) {
    static const struct {
        uint32_t dxil_code, common_operation;
        const char *intrinsic, *atomic_type, *value_type, *kernel_name;
    } atomic_cases[] = {
        { 0, GPU_IR_SHADER_ATOMIC_INTEGER_ADD, "atomic_fetch_add_explicit", "atomic_uint", "uint", "vx_atomic_add" },
        { 1, GPU_IR_SHADER_ATOMIC_INTEGER_AND, "atomic_fetch_and_explicit", "atomic_uint", "uint", "vx_atomic_and" },
        { 2, GPU_IR_SHADER_ATOMIC_INTEGER_OR, "atomic_fetch_or_explicit", "atomic_uint", "uint", "vx_atomic_or" },
        { 3, GPU_IR_SHADER_ATOMIC_INTEGER_XOR, "atomic_fetch_xor_explicit", "atomic_uint", "uint", "vx_atomic_xor" },
        { 4, GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MINIMUM, "atomic_fetch_min_explicit", "atomic_int", "int", "vx_atomic_smin" },
        { 5, GPU_IR_SHADER_ATOMIC_INTEGER_SIGNED_MAXIMUM, "atomic_fetch_max_explicit", "atomic_int", "int", "vx_atomic_smax" },
        { 6, GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MINIMUM, "atomic_fetch_min_explicit", "atomic_uint", "uint", "vx_atomic_umin" },
        { 7, GPU_IR_SHADER_ATOMIC_INTEGER_UNSIGNED_MAXIMUM, "atomic_fetch_max_explicit", "atomic_uint", "uint", "vx_atomic_umax" },
        { 8, GPU_IR_SHADER_ATOMIC_INTEGER_EXCHANGE, "atomic_exchange_explicit", "atomic_uint", "uint", "vx_atomic_exchange" }
    };
    static const uint32_t create_handle_parameters[] = { 1, 2, 1, 1, 3 };
    static const uint32_t atomic_add_parameters[] = { 1, 4, 1, 1, 1, 1, 1 };
    static uint32_t resource_record_ops[11];
    static uint32_t property_ops[2];
    static uint32_t uav_list_ops[1];
    static uint32_t resource_lists_ops[4];
    static uint32_t dimensions_ops[3];
    static uint32_t entry_properties_ops[2];
    static uint32_t entry_ops[5];
    static uint32_t create_handle_ops[] = { 1, 3, 4, 5, 6, 7 };
    static uint32_t atomic_add_ops[] = { 2, 8, 18, 9, 10, 11, 12, 13 };
    static uint32_t atomic_add_old_value_ops[] = { 2, 8, 18, 9, 13, 11, 12, 19 };
    static dxil_type types[8];
    static dxil_value values[18];
    static dxil_value locals[3];
    static dxil_inst instructions[4];
    static dxil_function functions[3];
    static dxil_md metadata[15];
    static uint32_t resource_nodes[] = { 7 };
    static uint32_t entry_nodes[] = { 13 };
    static dxil_named_md named[2];

    memset(types, 0, sizeof types);
    memset(values, 0, sizeof values);
    memset(locals, 0, sizeof locals);
    memset(instructions, 0, sizeof instructions);
    memset(functions, 0, sizeof functions);
    memset(metadata, 0, sizeof metadata);
    memset(named, 0, sizeof named);
    memset(resource_record_ops, 0, sizeof resource_record_ops);
    memset(property_ops, 0, sizeof property_ops);
    memset(uav_list_ops, 0, sizeof uav_list_ops);
    memset(resource_lists_ops, 0, sizeof resource_lists_ops);
    memset(dimensions_ops, 0, sizeof dimensions_ops);
    memset(entry_properties_ops, 0, sizeof entry_properties_ops);
    memset(entry_ops, 0, sizeof entry_ops);

    types[0] = (dxil_type){ .kind = DXT_VOID };
    types[1] = (dxil_type){ .kind = DXT_INT, .bits = 32 };
    types[2] = (dxil_type){ .kind = DXT_INT, .bits = 8 };
    types[3] = (dxil_type){ .kind = DXT_INT, .bits = 1 };
    types[4] = (dxil_type){ .kind = DXT_STRUCT, .name = "dx.types.Handle" };
    types[5] = (dxil_type){ .kind = DXT_FUNCTION, .elem = 0 };
    types[6] = (dxil_type){ .kind = DXT_FUNCTION, .elem = 4, .count = 5,
                            .members = (uint32_t *)create_handle_parameters };
    types[7] = (dxil_type){ .kind = DXT_FUNCTION, .elem = 1, .count = 7,
                            .members = (uint32_t *)atomic_add_parameters };

    values[0] = (dxil_value){ .kind = DXV_FUNCTION, .type = 5, .u.index = 0, .name = "main" };
    values[1] = (dxil_value){ .kind = DXV_FUNCTION, .type = 6, .u.index = 1,
                              .name = "dx.op.createHandle" };
    values[2] = (dxil_value){ .kind = DXV_FUNCTION, .type = 7, .u.index = 2,
                              .name = "dx.op.atomicBinOp.i32" };
    values[3] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 57 };
    values[4] = (dxil_value){ .kind = DXV_CONST_INT, .type = 2, .u.i = 1 };
    values[5] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 0 };
    values[6] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 0 };
    values[7] = (dxil_value){ .kind = DXV_CONST_INT, .type = 3, .u.i = 0 };
    values[8] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 78 };
    values[9] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 0 };
    values[10] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 0 };
    values[11] = (dxil_value){ .kind = DXV_CONST_UNDEF, .type = 1 };
    values[12] = (dxil_value){ .kind = DXV_CONST_UNDEF, .type = 1 };
    values[13] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 1 };
    values[14] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 5 };
    values[15] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 10 };
    values[16] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 4 };
    values[17] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 0 };
    locals[0] = (dxil_value){ .kind = DXV_INST, .type = 4, .u.index = 0 };
    locals[1] = (dxil_value){ .kind = DXV_INST, .type = 1, .u.index = 1 };
    instructions[0] = (dxil_inst){ .op = DXI_CALL, .type = 4, .result = 18,
                                   .block = 0, .n = 6, .ops = create_handle_ops };
    instructions[1] = (dxil_inst){ .op = DXI_CALL, .type = 1, .result = 19,
                                   .block = 0, .n = 8, .ops = atomic_add_ops };
    locals[2] = (dxil_value){ .kind = DXV_INST, .type = 1, .u.index = 2 };
    instructions[2] = (dxil_inst){ .op = DXI_CALL, .type = 1, .result = 20,
                                   .block = 0, .n = 8, .ops = atomic_add_old_value_ops };
    instructions[3] = (dxil_inst){ .op = DXI_RET, .type = 0, .result = UINT32_MAX, .block = 0 };
    functions[0] = (dxil_function){ .name = "main", .type = 5, .value = 0,
        .nblocks = 1, .ninsts = 4, .insts = instructions, .nlocals = 3, .locals = locals };
    functions[1] = (dxil_function){ .name = "dx.op.createHandle", .type = 6, .value = 1,
        .is_declaration = 1 };
    functions[2] = (dxil_function){ .name = "dx.op.atomicBinOp.i32", .type = 7, .value = 2,
        .is_declaration = 1 };

    resource_record_ops[3] = 1;
    resource_record_ops[4] = 1;
    resource_record_ops[5] = 2;
    resource_record_ops[6] = 3;
    resource_record_ops[10] = 5;
    property_ops[0] = 1;
    property_ops[1] = 4;
    uav_list_ops[0] = 6;
    resource_lists_ops[1] = 7;
    dimensions_ops[0] = dimensions_ops[1] = dimensions_ops[2] = 11;
    entry_properties_ops[0] = 10;
    entry_properties_ops[1] = 12;
    entry_ops[0] = 9;
    entry_ops[4] = 13;
    metadata[0] = (dxil_md){ .kind = DXM_VALUE, .value = 5 };
    metadata[1] = (dxil_md){ .kind = DXM_VALUE, .value = 13 };
    metadata[2] = (dxil_md){ .kind = DXM_VALUE, .value = 15 };
    metadata[3] = (dxil_md){ .kind = DXM_VALUE, .value = 14 };
    metadata[4] = (dxil_md){ .kind = DXM_NODE, .n = 2, .ops = property_ops };
    metadata[5] = (dxil_md){ .kind = DXM_NODE, .n = 11, .ops = resource_record_ops };
    metadata[6] = (dxil_md){ .kind = DXM_NODE, .n = 1, .ops = uav_list_ops };
    metadata[7] = (dxil_md){ .kind = DXM_NODE, .n = 4, .ops = resource_lists_ops };
    metadata[8] = (dxil_md){ .kind = DXM_VALUE, .value = 0 };
    metadata[9] = (dxil_md){ .kind = DXM_VALUE, .value = 16 };
    metadata[10] = (dxil_md){ .kind = DXM_VALUE, .value = 13 };
    metadata[11] = (dxil_md){ .kind = DXM_NODE, .n = 3, .ops = dimensions_ops };
    metadata[12] = (dxil_md){ .kind = DXM_NODE, .n = 2, .ops = entry_properties_ops };
    metadata[13] = (dxil_md){ .kind = DXM_NODE, .n = 5, .ops = entry_ops };
    named[0] = (dxil_named_md){ .name = "dx.resources", .n = 1, .nodes = resource_nodes };
    named[1] = (dxil_named_md){ .name = "dx.entryPoints", .n = 1, .nodes = entry_nodes };
    dxil_module module = { .shader_kind = 5, .ntypes = 8, .types = types,
        .nmodule_values = 18, .values = values, .nfunctions = 3, .functions = functions,
        .nmd = 14, .md = metadata, .nnamed = 2, .named = named };

    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        fprintf(stderr, "dxil_ir: atomic-add lowering returned %d\n", rc);
        return check("DXIL typed R32_UINT AtomicBinOp Add did not lower to common IR");
    }
    const gpu_ir_shader_instruction *atomic = NULL;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->opcode == GPU_IR_SHADER_OP_ATOMIC) atomic = instruction;
    }
    int valid = atomic && atomic->source_api == GPU_IR_SHADER_SOURCE_DXIL &&
        atomic->operation == GPU_IR_SHADER_ATOMIC_INTEGER_ADD && atomic->operand_count == 4 &&
        atomic->result_id && atomic->result_type_id;
    if (valid) {
        const gpu_ir_shader_operand *scope_operand = gpu_ir_shader_operand_at(shader, atomic->first_operand + 1u);
        const gpu_ir_shader_operand *semantics_operand = gpu_ir_shader_operand_at(shader, atomic->first_operand + 2u);
        const gpu_ir_shader_instruction *scope = NULL, *semantics = NULL;
        for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
            const gpu_ir_shader_instruction *candidate = gpu_ir_shader_instruction_at(shader, i);
            if (!candidate) continue;
            if (scope_operand && scope_operand->kind == GPU_IR_SHADER_OPERAND_ID &&
                candidate->result_id == scope_operand->value) scope = candidate;
            if (semantics_operand && semantics_operand->kind == GPU_IR_SHADER_OPERAND_ID &&
                candidate->result_id == semantics_operand->value) semantics = candidate;
        }
        valid = scope && semantics && scope->opcode == GPU_IR_SHADER_OP_CONSTANT &&
            semantics->opcode == GPU_IR_SHADER_OP_CONSTANT &&
            gpu_ir_shader_literal_word_at(shader, scope->first_literal) == 1u &&
            gpu_ir_shader_literal_word_at(shader, semantics->first_literal) == 0u;
    }
    char *msl = NULL;
    size_t msl_size = 0;
    if (valid) rc = gpu_ir_msl_emit_compute(shader, &msl, &msl_size);
    else rc = GPU_IR_INVALID;
    valid = valid && rc == GPU_IR_OK && msl && msl_size &&
        strstr(msl, "atomic_fetch_add_explicit(reinterpret_cast<device atomic_uint*>") &&
        strstr(msl, "memory_order_relaxed");
    if (valid && output_path) {
        FILE *output = fopen(output_path, "wb");
        valid = output && fwrite(msl, 1, msl_size, output) == msl_size;
        if (output && fclose(output)) valid = 0;
    }
    free(msl);
    if (!valid) return check("DXIL atomic add did not preserve device scope and relaxed unsigned semantics in common MSL");
    gpu_ir_shader_release(shader);

    FILE *ops_output = NULL;
    if (ops_output_path) {
        ops_output = fopen(ops_output_path, "wb");
        if (!ops_output || fputs("#include <metal_stdlib>\nusing namespace metal;\n\n", ops_output) == EOF) {
            if (ops_output) fclose(ops_output);
            return check("could not create the DXIL atomic operation MSL fixture");
        }
    }
    int ops_valid = 1;
    for (size_t i = 0; i < sizeof atomic_cases / sizeof atomic_cases[0]; i++) {
        values[9].u.i = atomic_cases[i].dxil_code;
        rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
        if (rc != GPU_IR_OK || !shader) {
            fprintf(stderr, "dxil_ir: atomic code %u failed common-IR build with %d\n",
                    atomic_cases[i].dxil_code, rc);
            gpu_ir_shader_release(shader);
            ops_valid = 0;
            break;
        }
        const gpu_ir_shader_instruction *candidate = NULL;
        for (uint32_t j = 0; j < gpu_ir_shader_instruction_count(shader); j++) {
            const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, j);
            if (instruction && instruction->opcode == GPU_IR_SHADER_OP_ATOMIC) candidate = instruction;
        }
        char *operation_msl = NULL;
        size_t operation_msl_size = 0;
        int emit_rc = candidate && candidate->operation == atomic_cases[i].common_operation ?
            gpu_ir_msl_emit_compute(shader, &operation_msl, &operation_msl_size) : GPU_IR_INVALID;
        if (!candidate || candidate->operation != atomic_cases[i].common_operation ||
            emit_rc != GPU_IR_OK || !operation_msl || !operation_msl_size) {
            fprintf(stderr, "dxil_ir: atomic code %u lowered to %u (expected %u), MSL result %d\n",
                    atomic_cases[i].dxil_code, candidate ? candidate->operation : 0,
                    atomic_cases[i].common_operation, emit_rc);
            ops_valid = 0;
        }
        char atomic_fragment[128];
        snprintf(atomic_fragment, sizeof atomic_fragment,
                 "as_type<uint>(%s(reinterpret_cast<device %s*>",
                 atomic_cases[i].intrinsic, atomic_cases[i].atomic_type);
        if (ops_valid && (!strstr(operation_msl, atomic_fragment) ||
            !strstr(operation_msl, "memory_order_relaxed"))) {
            fprintf(stderr, "dxil_ir: atomic code %u MSL omitted %s\n", atomic_cases[i].dxil_code,
                    atomic_fragment);
            ops_valid = 0;
        }
        char value_fragment[64];
        snprintf(value_fragment, sizeof value_fragment, "as_type<%s>(uint(", atomic_cases[i].value_type);
        if (ops_valid && !strstr(operation_msl, value_fragment)) {
            fprintf(stderr, "dxil_ir: atomic code %u MSL omitted value cast %s\n", atomic_cases[i].dxil_code,
                    value_fragment);
            ops_valid = 0;
        }
        if (ops_valid && !output_path && !ops_output_path && !compare_exchange_output_path &&
            !execute_atomic_operation_on_metal(shader, atomic_cases[i].common_operation)) {
            fprintf(stderr, "dxil_ir: DXIL AtomicBinOp code %u failed concurrent Metal readback\n",
                    atomic_cases[i].dxil_code);
            ops_valid = 0;
        }
        if (ops_valid && ops_output) {
            const char *entry = strstr(operation_msl, "kernel void vx_main");
            const char *entry_name = entry ? strstr(entry, "vx_main") : NULL;
            if (!entry || !entry_name ||
                fwrite(entry, 1, (size_t)(entry_name - entry), ops_output) != (size_t)(entry_name - entry) ||
                fputs(atomic_cases[i].kernel_name, ops_output) == EOF ||
                fwrite(entry_name + strlen("vx_main"), 1,
                       operation_msl_size - (size_t)(entry_name + strlen("vx_main") - operation_msl),
                       ops_output) != operation_msl_size -
                       (size_t)(entry_name + strlen("vx_main") - operation_msl) ||
                fputc('\n', ops_output) == EOF) ops_valid = 0;
        }
        free(operation_msl);
        gpu_ir_shader_release(shader);
        if (!ops_valid) break;
    }
    if (ops_output && fclose(ops_output)) ops_valid = 0;
    values[9].u.i = 0;
    if (!ops_valid) return check("DXIL AtomicBinOp lowering or common MSL emission failed for a supported operation");

    values[9].u.i = 9;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    values[9].u.i = 0;
    if (rc != GPU_IR_UNSUPPORTED) return check("invalid DXIL AtomicBinOp code was accepted by common IR");
    values[14].u.i = 4;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    int signed_resource_supported = rc == GPU_IR_OK && shader != NULL;
    gpu_ir_shader_release(shader);
    values[14].u.i = 5;
    if (!signed_resource_supported) return check("DXIL typed R32_SINT atomic was rejected by common IR");
    values[14].u.i = 9;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    values[14].u.i = 5;
    if (rc != GPU_IR_UNSUPPORTED) return check("DXIL typed floating-point atomic escaped the integer common-IR subset");
    values[11].kind = DXV_CONST_INT;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    gpu_ir_shader_release(shader);
    values[11].kind = DXV_CONST_UNDEF;
    if (rc != GPU_IR_UNSUPPORTED) return check("DXIL typed-buffer atomic accepted an extra coordinate");

    values[2].name = "dx.op.atomicCompareExchange.i32";
    functions[2].name = "dx.op.atomicCompareExchange.i32";
    values[8].u.i = 79;
    values[12] = (dxil_value){ .kind = DXV_CONST_INT, .type = 1, .u.i = 41 };
    atomic_add_ops[4] = atomic_add_ops[5] = 11;
    atomic_add_old_value_ops[3] = 13;
    atomic_add_old_value_ops[4] = atomic_add_old_value_ops[5] = 11;
    atomic_add_old_value_ops[6] = 9;
    rc = dxil_build_gpu_ir_shader(&module, 0, &shader);
    if (rc != GPU_IR_OK || !shader) {
        fprintf(stderr, "dxil_ir: AtomicCompareExchange lowering returned %d\n", rc);
        gpu_ir_shader_release(shader);
        return check("DXIL typed R32_UINT AtomicCompareExchange did not lower to common IR");
    }
    const gpu_ir_shader_instruction *compare_exchange = NULL;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *instruction = gpu_ir_shader_instruction_at(shader, i);
        if (instruction && instruction->opcode == GPU_IR_SHADER_OP_ATOMIC &&
            instruction->operation == GPU_IR_SHADER_ATOMIC_INTEGER_COMPARE_EXCHANGE)
            compare_exchange = instruction;
    }
    char *compare_exchange_msl = NULL;
    size_t compare_exchange_msl_size = 0;
    rc = compare_exchange ? gpu_ir_msl_emit_compute(shader, &compare_exchange_msl,
                                                     &compare_exchange_msl_size) : GPU_IR_INVALID;
    int compare_exchange_valid = rc == GPU_IR_OK && compare_exchange &&
        compare_exchange->operand_count == 6 && compare_exchange_msl && compare_exchange_msl_size &&
        strstr(compare_exchange_msl, "atomic_compare_exchange_weak_explicit(") &&
        strstr(compare_exchange_msl, "while (vx_expected_") &&
        strstr(compare_exchange_msl, "memory_order_relaxed, memory_order_relaxed") &&
        strstr(compare_exchange_msl, "vx_expected_");
    if (compare_exchange_valid && compare_exchange_output_path) {
        FILE *output = fopen(compare_exchange_output_path, "wb");
        compare_exchange_valid = output &&
            fwrite(compare_exchange_msl, 1, compare_exchange_msl_size, output) == compare_exchange_msl_size;
        if (output && fclose(output)) compare_exchange_valid = 0;
    }
    if (compare_exchange_valid && !output_path && !ops_output_path && !compare_exchange_output_path)
        compare_exchange_valid = execute_atomic_compare_exchange_on_metal(shader);
    free(compare_exchange_msl);
    gpu_ir_shader_release(shader);
    values[2].name = "dx.op.atomicBinOp.i32";
    functions[2].name = "dx.op.atomicBinOp.i32";
    values[8].u.i = 78;
    values[12].kind = DXV_CONST_UNDEF;
    atomic_add_ops[4] = 10;
    atomic_add_old_value_ops[3] = 9;
    atomic_add_old_value_ops[4] = 13;
    atomic_add_old_value_ops[6] = 12;
    if (!compare_exchange_valid)
        return check("DXIL typed R32_UINT AtomicCompareExchange did not preserve old-value semantics");
    puts("dxil_ir: typed R32_SINT/R32_UINT AtomicBinOp integer operations lower to common device-scope relaxed atomics; 64 concurrent Metal invocations verify every operation; unsupported forms rejected: ok");
    puts("dxil_ir: typed R32_UINT AtomicCompareExchange preserves the returned old value through common IR and Metal: ok");
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--check-dxil-target0"))
        return check_dxil_target0_common_ir(NULL);
    if (argc == 3 && !strcmp(argv[1], "--write-dxil-target0"))
        return check_dxil_target0_common_ir(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--check-dxil-vertex-position"))
        return check_dxil_vertex_position_output(NULL);
    if (argc == 3 && !strcmp(argv[1], "--write-dxil-vertex-position"))
        return check_dxil_vertex_position_output(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--check-dxil-vertex-input"))
        return check_dxil_vertex_input(NULL);
    if (argc == 3 && !strcmp(argv[1], "--write-dxil-vertex-input"))
        return check_dxil_vertex_input(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--check-dxil-vertex-system-values"))
        return check_dxil_vertex_system_values(NULL);
    if (argc == 3 && !strcmp(argv[1], "--write-dxil-vertex-system-values"))
        return check_dxil_vertex_system_values(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--check-vector-integer-mad")) {
        if (check_vector_integer_mad_adapter()) return 1;
        return check_vector_integer_mad_msl(argv[2]);
    }
    if (argc == 3 && !strcmp(argv[1], "--write-fragment-discard"))
        return check_discard_fragment_msl(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--check-fragment-discard"))
        return check_discard_dxil_lowering() || check_discard_fragment_msl(NULL);
    if (argc == 2 && !strcmp(argv[1], "--test-common-wave-queries"))
        return translate_compute_subgroup_queries(NULL);
    if (argc == 3 && !strcmp(argv[1], "--write-common-wave-queries"))
        return translate_compute_subgroup_queries(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--check-byteaddress-load-lanes"))
        return translate_byteaddress_load_lane_fixtures(NULL, 0);
    if (argc == 3 && !strcmp(argv[1], "--write-byteaddress-load-lanes"))
        return translate_byteaddress_load_lane_fixtures(argv[2], 0);
    const char *system_values_output = NULL;
    const char *core_compute_output = NULL;
    const char *bit_intrinsics_output = NULL;
    const char *typed_buffer_output = NULL;
    const char *raw_buffer_common_output = NULL;
    const char *typed_sint_buffer_output = NULL;
    const char *typed_float_buffer_output = NULL;
    const char *typed_float_dxil_output = NULL;
    const char *texture_dimensions_output = NULL;
    const char *texture_dimensions_lod1_output = NULL;
    const char *texture_dimensions_array_output = NULL;
    const char *texture_dimensions_msaa_output = NULL;
    const char *texture_load_msaa_output = NULL;
    const char *texture_load_msaa_array_output = NULL;
    const char *texture_load_output = NULL;
    const char *texture_load_array_output = NULL;
    const char *texture_load_array_dxil_output = NULL;
    const char *texture_load_sint_dxil_output = NULL;
    const char *texture_load_array_sint_dxil_output = NULL;
    const char *texture_load_float_dxil_output = NULL;
    const char *texture_load_float_channel_dxil_output = NULL;
    const char *texture_load_float_channel_z_dxil_output = NULL;
    const char *texture_load_float_channel_w_dxil_output = NULL;
    const char *direct_texture_load_mapping_output = NULL;
    const char *direct_texture_load_mapping_uint_output = NULL;
    const char *direct_texture_load_mapping_sint_output = NULL;
    const char *direct_typed_buffer_mapping_output = NULL;
    const char *direct_typed_buffer_texture_load_mapping_output = NULL;
    const char *direct_raw_buffer_mapping_output = NULL;
    const char *direct_structured_buffer_mapping_output = NULL;
    const char *derivative_modes_output = NULL;
    const char *signed_conversion_output = NULL;
    const char *unary_intrinsics_output = NULL;
    const char *integer_minmax_output = NULL;
    const char *integer_mad_output = NULL;
    const char *float_mad_output = NULL;
    const char *float_minmax_output = NULL;
    const char *atomic_add_output = NULL;
    const char *atomic_ops_output = NULL;
    const char *atomic_compare_exchange_output = NULL;
    if (argc == 3 && !strcmp(argv[1], "--write-system-values")) system_values_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-core-compute")) core_compute_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-bit-intrinsics")) bit_intrinsics_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-typed-buffer")) typed_buffer_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-raw-buffer-common")) raw_buffer_common_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-typed-sint-buffer")) typed_sint_buffer_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-typed-float-buffer")) typed_float_buffer_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-typed-float-dxil")) typed_float_dxil_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-dimensions")) texture_dimensions_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-dimensions-lod1"))
        texture_dimensions_lod1_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-dimensions-array"))
        texture_dimensions_array_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-dimensions-msaa"))
        texture_dimensions_msaa_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load-msaa"))
        texture_load_msaa_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load-msaa-array"))
        texture_load_msaa_array_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load")) texture_load_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load-array")) texture_load_array_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load-array-dxil"))
        texture_load_array_dxil_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load-sint-dxil"))
        texture_load_sint_dxil_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load-array-sint-dxil"))
        texture_load_array_sint_dxil_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load-float-dxil"))
        texture_load_float_dxil_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load-float-channel-dxil"))
        texture_load_float_channel_dxil_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load-float-channel-z-dxil"))
        texture_load_float_channel_z_dxil_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-texture-load-float-channel-w-dxil"))
        texture_load_float_channel_w_dxil_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-direct-texture-load-mapping"))
        direct_texture_load_mapping_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-direct-texture-load-mapping-uint"))
        direct_texture_load_mapping_uint_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-direct-texture-load-mapping-sint"))
        direct_texture_load_mapping_sint_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-direct-typed-buffer-mapping"))
        direct_typed_buffer_mapping_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-direct-typed-buffer-texture-load-mapping"))
        direct_typed_buffer_texture_load_mapping_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-direct-raw-buffer-mapping"))
        direct_raw_buffer_mapping_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-direct-structured-buffer-mapping"))
        direct_structured_buffer_mapping_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-derivative-modes")) derivative_modes_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-signed-conversions")) signed_conversion_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-unary-intrinsics")) unary_intrinsics_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-integer-minmax")) integer_minmax_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-integer-mad")) integer_mad_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-float-mad")) float_mad_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-float-minmax")) float_minmax_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-dxil-atomic-add")) atomic_add_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-dxil-atomic-ops")) atomic_ops_output = argv[2];
    else if (argc == 3 && !strcmp(argv[1], "--write-dxil-atomic-compare-exchange"))
        atomic_compare_exchange_output = argv[2];
    else if (argc != 1) return check("unknown command-line option");
    if (raw_buffer_common_output)
        return translate_raw_buffer_common_fixture(raw_buffer_common_output);
    if (texture_dimensions_lod1_output)
        return translate_texture_dimensions_fixture(texture_dimensions_lod1_output, 1, 0);
    if (texture_dimensions_array_output)
        return translate_texture_dimensions_fixture(texture_dimensions_array_output, 0, 1);
    if (texture_dimensions_msaa_output)
        return translate_texture_dimensions_msaa_fixture(texture_dimensions_msaa_output);
    if (texture_load_msaa_output) return translate_texture_load_msaa_fixture(texture_load_msaa_output);
    if (texture_dimensions_output)
        return translate_texture_dimensions_fixture(texture_dimensions_output, 0, 0);
    if (texture_load_output) return translate_texture_load_fixture(texture_load_output, 0);
    if (texture_load_array_output) return translate_texture_load_fixture(texture_load_array_output, 1);
    if (texture_load_array_dxil_output) return translate_texture_load_array_dxil_fixture(texture_load_array_dxil_output);
    if (texture_load_sint_dxil_output)
        return translate_texture_load_sint_dxil_fixture(texture_load_sint_dxil_output);
    if (texture_load_array_sint_dxil_output)
        return translate_texture_load_array_sint_dxil_fixture(texture_load_array_sint_dxil_output);
    if (texture_load_float_dxil_output)
        return translate_texture_load_float_dxil_fixture(texture_load_float_dxil_output);
    if (texture_load_float_channel_dxil_output)
        return translate_texture_load_float_channel_dxil_fixture(texture_load_float_channel_dxil_output, 1);
    if (texture_load_float_channel_z_dxil_output)
        return translate_texture_load_float_channel_dxil_fixture(texture_load_float_channel_z_dxil_output, 2);
    if (texture_load_float_channel_w_dxil_output)
        return translate_texture_load_float_channel_dxil_fixture(texture_load_float_channel_w_dxil_output, 3);
    if (direct_texture_load_mapping_output)
        return translate_direct_texture_load_mapping_fixture("tests/shaders12/texload_float_bounds_cs.dxil",
                                                              "float4", direct_texture_load_mapping_output);
    if (direct_texture_load_mapping_uint_output)
        return translate_direct_texture_load_mapping_fixture("tests/shaders12/texload_bounds_cs.dxil",
                                                              "uint4", direct_texture_load_mapping_uint_output);
    if (direct_texture_load_mapping_sint_output)
        return translate_direct_texture_load_mapping_fixture("tests/shaders12/texload_sint_bounds_cs.dxil",
                                                              "int4", direct_texture_load_mapping_sint_output);
    if (direct_typed_buffer_mapping_output)
        return translate_direct_typed_buffer_mapping_fixture(direct_typed_buffer_mapping_output);
    if (direct_typed_buffer_texture_load_mapping_output)
        return translate_direct_typed_buffer_texture_load_mapping_fixture(direct_typed_buffer_texture_load_mapping_output);
    if (direct_raw_buffer_mapping_output)
        return translate_direct_raw_buffer_mapping_fixture(direct_raw_buffer_mapping_output);
    if (direct_structured_buffer_mapping_output)
        return translate_direct_structured_buffer_mapping_fixture(direct_structured_buffer_mapping_output);
    if (derivative_modes_output) return translate_derivative_modes_fixture(derivative_modes_output);
    if (check_dxil_target0_common_ir(NULL)) return 1;
    if (check_dxil_vertex_position_output(NULL)) return 1;
    if (check_dxil_vertex_system_values(NULL)) return 1;
    if (translate_core_compute(core_compute_output)) return 1;
    if (translate_bit_intrinsics(bit_intrinsics_output)) return 1;
    if (translate_vector_bit_intrinsics()) return 1;
    if (translate_compute_system_values(system_values_output)) return 1;
    if (translate_compute_subgroup_queries(NULL)) return 1;
    if (translate_cfg_and_phi()) return 1;
    if (translate_vector_compare_and_select()) return 1;
    if (translate_vector_compare_predicates()) return 1;
    if (translate_vector_classification_adapter()) return 1;
    if (translate_typed_buffer_fixture(typed_buffer_output)) return 1;
    if (translate_raw_buffer_common_fixture(raw_buffer_common_output)) return 1;
    if (translate_typed_sint_buffer_fixture(typed_sint_buffer_output)) return 1;
    if (translate_typed_float_buffer_fixture(typed_float_buffer_output)) return 1;
    if (translate_typed_float_dxil_fixture(typed_float_dxil_output)) return 1;
    if (translate_texture_load_fixture(NULL, 0)) return 1;
    if (translate_texture_load_fixture(NULL, 1)) return 1;
    if (translate_texture_load_array_dxil_fixture(NULL)) return 1;
    if (translate_texture_load_sint_dxil_fixture(NULL)) return 1;
    if (translate_texture_load_array_sint_dxil_fixture(NULL)) return 1;
    if (translate_texture_load_float_dxil_fixture(NULL)) return 1;
    if (translate_texture_load_float_channel_dxil_fixture(NULL, 1)) return 1;
    if (translate_texture_load_float_channel_dxil_fixture(NULL, 2)) return 1;
    if (translate_texture_load_float_channel_dxil_fixture(NULL, 3)) return 1;
    if (translate_texture_load_msaa_fixture(NULL)) return 1;
    if (translate_texture_load_msaa_array_fixture(texture_load_msaa_array_output)) return 1;
    if (translate_direct_texture_load_mapping_fixture("tests/shaders12/texload_bounds_cs.dxil", "uint4", NULL)) return 1;
    if (translate_direct_texture_load_mapping_fixture("tests/shaders12/texload_sint_bounds_cs.dxil", "int4", NULL)) return 1;
    if (translate_direct_texture_load_mapping_fixture("tests/shaders12/texload_float_bounds_cs.dxil", "float4", NULL)) return 1;
    if (translate_direct_typed_buffer_mapping_fixture(NULL)) return 1;
    if (translate_direct_typed_buffer_texture_load_mapping_fixture(NULL)) return 1;
    if (translate_direct_raw_buffer_mapping_fixture(NULL)) return 1;
    if (translate_direct_structured_buffer_mapping_fixture(NULL)) return 1;
    if (translate_signed_conversion_fixture(signed_conversion_output)) return 1;
    if (translate_unary_intrinsics(unary_intrinsics_output)) return 1;
    if (translate_integer_minmax_dxil_fixture(integer_minmax_output)) return 1;
    if (translate_integer_mad_dxil_fixture(integer_mad_output)) return 1;
    if (translate_float_mad_dxil_fixture(float_mad_output)) return 1;
    if (translate_float_minmax_dxil_fixture(float_minmax_output)) return 1;
    if (translate_dxil_atomic_add(atomic_add_output, atomic_ops_output, atomic_compare_exchange_output)) return 1;
    if (check_discard_dxil_lowering()) return 1;
    if (check_discard_fragment_msl(NULL)) return 1;
    if (reject_wave_shader_unsupported()) return 1;
    if (test_quad_intrinsic_validation()) return 1;
    if (test_wave_query_validation()) return 1;
    if (reject_unsupported_intrinsic()) return 1;
    return 0;
}
