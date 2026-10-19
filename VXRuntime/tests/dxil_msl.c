#include "dxil_msl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TYPE_FLOAT, TYPE_INT, TYPE_FLOAT4, TYPE_INT4, TYPE_VOID, TYPE_COUNT };
enum { SHUFFLE_RESULT = 26, LOCAL_COUNT = 27, INSERT_COUNT = 8, INST_COUNT = 10 };

static int translate_mask(int raw_data) {
    dxil_type types[TYPE_COUNT] = { 0 };
    types[TYPE_FLOAT].kind = DXT_FLOAT;
    types[TYPE_INT].kind = DXT_INT;
    types[TYPE_INT].bits = 32;
    types[TYPE_FLOAT4] = (dxil_type){ .kind = DXT_VECTOR, .count = 4, .elem = TYPE_FLOAT };
    types[TYPE_INT4] = (dxil_type){ .kind = DXT_VECTOR, .count = 4, .elem = TYPE_INT };
    types[TYPE_VOID].kind = DXT_VOID;

    dxil_value *locals = calloc(LOCAL_COUNT, sizeof *locals);
    dxil_inst *insts = calloc(INST_COUNT, sizeof *insts);
    uint32_t operands[INST_COUNT][3] = { 0 };
    uint32_t aggregate_ids[4] = { 12, 13, 14, 15 };
    uint64_t raw_mask[4] = { 4, 1, UINT32_MAX, 7 };
    if (!locals || !insts) { free(locals); free(insts); return 1; }

    for (uint32_t i = 0; i < 8; i++) {
        locals[i].kind = DXV_CONST_FLOAT;
        locals[i].type = TYPE_FLOAT;
        locals[i].u.f = (double)i + 1.0;
    }
    for (uint32_t i = 0; i < 4; i++) {
        locals[8 + i].kind = DXV_CONST_INT;
        locals[8 + i].type = TYPE_INT;
        locals[8 + i].u.i = i;
    }
    const int64_t aggregate_mask[4] = { 2, 0, 9, 5 };
    for (uint32_t i = 0; i < 4; i++) {
        locals[12 + i].kind = DXV_CONST_INT;
        locals[12 + i].type = TYPE_INT;
        locals[12 + i].u.i = aggregate_mask[i];
    }
    if (!raw_data) {
        locals[13].kind = DXV_CONST_UNDEF;
        locals[13].type = TYPE_INT;
    }
    locals[16].kind = raw_data ? DXV_CONST_DATA : DXV_CONST_AGGREGATE;
    locals[16].type = TYPE_INT4;
    locals[16].u.list.n = 4;
    if (raw_data) {
        locals[16].data = raw_mask;
    } else {
        locals[16].u.list.ids = aggregate_ids;
    }
    locals[17].kind = DXV_CONST_UNDEF;
    locals[17].type = TYPE_FLOAT4;

    for (uint32_t i = 0; i < INSERT_COUNT; i++) {
        uint32_t result = 18 + i;
        uint32_t source = i < 4 ? (i ? result - 1 : 17) : (i == 4 ? 17 : result - 1);
        operands[i][0] = source;
        operands[i][1] = i;
        operands[i][2] = 8 + (i & 3);
        insts[i] = (dxil_inst){ .op = DXI_INSERTELT, .type = TYPE_FLOAT4, .result = result,
            .block = 0, .n = 3, .ops = operands[i] };
        locals[result].kind = DXV_INST;
        locals[result].type = TYPE_FLOAT4;
        locals[result].u.index = i;
    }
    operands[8][0] = 21;
    operands[8][1] = 25;
    operands[8][2] = 16;
    insts[8] = (dxil_inst){ .op = DXI_SHUFFLEVEC, .type = TYPE_FLOAT4, .result = SHUFFLE_RESULT,
        .block = 0, .n = 3, .ops = operands[8] };
    locals[SHUFFLE_RESULT].kind = DXV_INST;
    locals[SHUFFLE_RESULT].type = TYPE_FLOAT4;
    locals[SHUFFLE_RESULT].u.index = 8;
    insts[9] = (dxil_inst){ .op = DXI_RET, .type = TYPE_VOID, .result = UINT32_MAX,
        .block = 0, .n = 0 };

    dxil_function function = { .name = "main", .nblocks = 1, .ninsts = INST_COUNT,
        .insts = insts, .nlocals = LOCAL_COUNT, .locals = locals };
    dxil_module module = { .shader_kind = 5, .ntypes = TYPE_COUNT, .types = types,
        .nfunctions = 1, .functions = &function };
    dxil_msl result = { 0 };
    int failed = dxil_to_msl(&module, &result) != 0;
    const char *expected = raw_data
        ? "v26 = float4(v25[0], v21[1], 0, v25[3]);"
        : "v26 = float4(v21[2], 0, 0, v25[1]);";
    if (!failed && !strstr(result.msl, expected)) {
        fprintf(stderr, "DXIL vector shuffle MSL missing: %s\nMSL:\n%s\n", expected, result.msl);
        failed = 1;
    }
    if (failed) fprintf(stderr, "DXIL vector shuffle translation failed: %s\n", result.error);
    dxil_msl_free(&result);
    free(insts);
    free(locals);
    return failed;
}

static int bindless_resource_heap_class(void) {
    FILE *file = fopen("tests/shaders12/bindless_ps.dxil", "rb");
    if (!file) { fprintf(stderr, "could not open bindless DXIL fixture\n"); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) { fclose(file); return 1; }
    size_t read = fread(bytes, 1, (size_t)length, file);
    fclose(file);

    dxil_module module = { 0 };
    dxil_msl result = { 0 };
    int failed = read != (size_t)length || dxil_read(bytes, read, &module) != 0;
    free(bytes);
    if (!failed) failed = dxil_to_msl(&module, &result) != 0;
    if (!failed && (!result.uses_heap || result.heap_resource_classes != (1u << DXIL_SRV))) {
        fprintf(stderr, "bindless DXIL heap classes were %#x, expected SRV only\n", result.heap_resource_classes);
        failed = 1;
    }
    if (failed) fprintf(stderr, "bindless DXIL class translation failed: %s\n", result.error);
    dxil_msl_free(&result);
    dxil_free(&module);
    return failed;
}

static int translate_store_only_uav_fixture(void) {
    FILE *file = fopen("tests/shaders12/sum_cs.dxil", "rb");
    if (!file) { fprintf(stderr, "could not open UAV access DXIL fixture\n"); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) { fclose(file); return 1; }
    size_t read = fread(bytes, 1, (size_t)length, file);
    fclose(file);

    dxil_module module = { 0 };
    dxil_msl result = { 0 };
    int failed = read != (size_t)length || dxil_read(bytes, read, &module) != 0;
    free(bytes);
    if (!failed) failed = dxil_to_msl(&module, &result) != 0;
    int found_texture_uav = 0;
    for (uint32_t i = 0; !failed && i < result.nslots; i++) {
        const dxil_msl_slot *slot = &result.slots[i];
        if (slot->cls != DXIL_UAV || slot->kind != 2) continue;
        found_texture_uav = 1;
        if (slot->uav_access != DXIL_MSL_ACCESS_WRITE) failed = 1;
    }
    if (!failed && (!found_texture_uav || !strstr(result.msl, "texture2d<float, access::write>"))) failed = 1;
    if (failed) fprintf(stderr, "store-only texture UAV was not lowered with access::write: %s\nMSL:\n%s\n",
                        result.error, result.msl ? result.msl : "(none)");
    dxil_msl_free(&result);
    dxil_free(&module);
    return failed;
}

static int translate_sample_mask_fixture(const char *write_path) {
    FILE *file = fopen("tests/shaders12/cmp_ps.dxil", "rb");
    if (!file) { fprintf(stderr, "could not open sample-mask DXIL fixture\n"); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) { fclose(file); return 1; }
    size_t read = fread(bytes, 1, (size_t)length, file);
    fclose(file);

    dxil_module module = { 0 };
    dxil_msl result = { 0 };
    int failed = read != (size_t)length || dxil_read(bytes, read, &module) != 0;
    free(bytes);
    if (!failed) failed = dxil_to_msl_with_sample_mask(&module, 0x5au, &result) != 0;
    if (!failed && (!strstr(result.msl, "uint vx_sample_mask [[sample_mask]];") ||
                    !strstr(result.msl, "vx_out.vx_sample_mask = 0x0000005au;"))) {
        fprintf(stderr, "DXIL PSO sample-mask lowering was missing from MSL:\n%s\n", result.msl);
        failed = 1;
    }
    if (!failed && write_path) {
        FILE *out = fopen(write_path, "wb");
        if (!out || fwrite(result.msl, 1, strlen(result.msl), out) != strlen(result.msl)) failed = 1;
        if (out && fclose(out)) failed = 1;
    }
    if (failed) fprintf(stderr, "DXIL sample-mask translation failed: %s\n", result.error);
    dxil_msl_free(&result);
    dxil_free(&module);
    return failed;
}

static int write_msl(const char *path, const dxil_msl *result);

static int translate_buffer_update_counter(const char *write_path) {
    enum { I8_TYPE, I32_TYPE, I1_TYPE, HANDLE_TYPE, HANDLE_PROPERTIES_TYPE, VOID_TYPE, TYPE_COUNT };
    dxil_type types[TYPE_COUNT] = { 0 };
    types[I8_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 8 };
    types[I32_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 32 };
    types[I1_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 1 };
    types[HANDLE_TYPE] = (dxil_type){ .kind = DXT_STRUCT, .name = "dx.types.Handle" };
    types[HANDLE_PROPERTIES_TYPE] = (dxil_type){ .kind = DXT_STRUCT, .name = "dx.types.HandleProperties" };
    types[VOID_TYPE].kind = DXT_VOID;

    dxil_value module_values[3] = {
        { .kind = DXV_FUNCTION, .name = "dx.op.createHandleFromHeap" },
        { .kind = DXV_FUNCTION, .name = "dx.op.annotateHandle" },
        { .kind = DXV_FUNCTION, .name = "dx.op.bufferUpdateCounter" }
    };
    dxil_value locals[11] = { 0 };
    locals[0] = (dxil_value){ .kind = DXV_CONST_INT, .type = I32_TYPE, .u.i = 218 };
    locals[1] = (dxil_value){ .kind = DXV_CONST_INT, .type = I32_TYPE, .u.i = 0 };
    locals[2] = (dxil_value){ .kind = DXV_CONST_INT, .type = I1_TYPE, .u.i = 0 };
    locals[3] = (dxil_value){ .kind = DXV_INST, .type = HANDLE_TYPE, .u.index = 0 };
    locals[4] = (dxil_value){ .kind = DXV_CONST_INT, .type = I32_TYPE, .u.i = 216 };
    locals[5] = (dxil_value){ .kind = DXV_CONST_INT, .type = I32_TYPE, .u.i = 12 | (1 << 12) };
    locals[6] = (dxil_value){ .kind = DXV_CONST_INT, .type = I32_TYPE, .u.i = 16 };
    uint32_t property_ids[] = { 8, 9 };
    locals[7] = (dxil_value){ .kind = DXV_CONST_AGGREGATE, .type = HANDLE_PROPERTIES_TYPE,
        .u.list = { .ids = property_ids, .n = 2 } };
    locals[8] = (dxil_value){ .kind = DXV_INST, .type = HANDLE_TYPE, .u.index = 1 };
    locals[9] = (dxil_value){ .kind = DXV_CONST_INT, .type = I32_TYPE, .u.i = 70 };
    locals[10] = (dxil_value){ .kind = DXV_CONST_INT, .type = I8_TYPE, .u.i = -1 };

    uint32_t create_handle_ops[] = { 0, 3, 4, 5 };
    uint32_t annotate_handle_ops[] = { 1, 7, 6, 10 };
    uint32_t update_counter_ops[] = { 2, 12, 11, 13 };
    dxil_inst insts[] = {
        { .op = DXI_CALL, .type = HANDLE_TYPE, .result = 6, .block = 0, .n = 4, .ops = create_handle_ops },
        { .op = DXI_CALL, .type = HANDLE_TYPE, .result = 11, .block = 0, .n = 4, .ops = annotate_handle_ops },
        { .op = DXI_CALL, .type = VOID_TYPE, .result = UINT32_MAX, .block = 0, .n = 4, .ops = update_counter_ops },
        { .op = DXI_RET, .type = VOID_TYPE, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function function = { .name = "main", .nblocks = 1, .ninsts = 4, .insts = insts,
        .nlocals = 11, .locals = locals };
    dxil_module module = { .shader_kind = 5, .ntypes = TYPE_COUNT, .types = types,
        .nmodule_values = 3, .values = module_values, .nfunctions = 1, .functions = &function };
    dxil_msl result = { 0 };
    int failed = dxil_to_msl(&module, &result) != 0;
    if (!failed && (!result.uses_heap || !result.uses_counters ||
        !strstr(result.msl, "->flags & 2u") ||
        !strstr(result.msl, "atomic_fetch_add_explicit((device atomic_uint *)v8->res") ||
        !strstr(result.msl, "memory_order_relaxed"))) {
        fprintf(stderr, "DXIL BufferUpdateCounter lowering omitted counter metadata, guard, or atomic update\nMSL:\n%s\n",
                result.msl);
        failed = 1;
    }
    if (!failed) failed = write_msl(write_path, &result);
    if (failed) fprintf(stderr, "DXIL BufferUpdateCounter translation failed: %s\n", result.error);
    dxil_msl_free(&result);
    return failed;
}

static int translate_resource_atomics(uint8_t kind, uint8_t component, uint32_t atomic_operation, const char *write_path) {
    enum { VOID_TYPE, INT_TYPE, BOOL_TYPE, HANDLE_TYPE, PROPERTIES_TYPE, TYPE_COUNT };
    dxil_type types[TYPE_COUNT] = { 0 };
    types[VOID_TYPE].kind = DXT_VOID;
    types[INT_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 32 };
    types[BOOL_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 1 };
    types[HANDLE_TYPE] = (dxil_type){ .kind = DXT_STRUCT, .name = "dx.types.Handle" };
    uint32_t property_members[] = { INT_TYPE, INT_TYPE };
    types[PROPERTIES_TYPE] = (dxil_type){ .kind = DXT_STRUCT, .count = 2, .members = property_members,
        .name = "dx.types.HandleProperties" };

    enum {
        FN_CREATE, FN_ANNOTATE, FN_ATOMIC, FN_COMPARE,
        V_ZERO, V_ONE, V_KIND, V_COMPONENT, V_TAG_STRIDE, V_CREATE_OPCODE, V_UAV,
        V_ANNOTATE_OPCODE, V_BASIC_PROPERTIES, V_BIN_OPCODE, V_COMPARE_OPCODE, V_ADD,
        V_COORD, V_VALUE, V_COMPARE_VALUE, V_NEW_VALUE, MODULE_VALUE_COUNT
    };
    dxil_value module_values[MODULE_VALUE_COUNT] = {
        [FN_CREATE] = { .kind = DXV_FUNCTION, .name = "dx.op.createHandle" },
        [FN_ANNOTATE] = { .kind = DXV_FUNCTION, .name = "dx.op.annotateHandle" },
        [FN_ATOMIC] = { .kind = DXV_FUNCTION, .name = "dx.op.atomicBinOp.i32" },
        [FN_COMPARE] = { .kind = DXV_FUNCTION, .name = "dx.op.atomicCompareExchange.i32" },
        [V_ZERO] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 0 },
        [V_ONE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 1 },
        [V_KIND] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = kind },
        [V_COMPONENT] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = component },
        [V_TAG_STRIDE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 1 },
        [V_CREATE_OPCODE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 57 },
        [V_UAV] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = DXIL_UAV },
        [V_ANNOTATE_OPCODE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 216 },
        [V_BASIC_PROPERTIES] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = kind | (1u << 12) },
        [V_BIN_OPCODE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 78 },
        [V_COMPARE_OPCODE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 79 },
        [V_ADD] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = atomic_operation },
        [V_COORD] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 0 },
        [V_VALUE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 2 },
        [V_COMPARE_VALUE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 2 },
        [V_NEW_VALUE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 3 }
    };

    dxil_md metadata[14] = { 0 };
    metadata[0] = (dxil_md){ .kind = DXM_VALUE, .value = V_ZERO };
    metadata[1] = (dxil_md){ .kind = DXM_VALUE, .value = V_ONE };
    metadata[2] = (dxil_md){ .kind = DXM_VALUE, .value = V_KIND };
    metadata[3] = (dxil_md){ .kind = DXM_VALUE, .value = V_ZERO };
    metadata[4] = (dxil_md){ .kind = DXM_VALUE, .value = V_COMPONENT };
    metadata[5] = (dxil_md){ .kind = DXM_VALUE, .value = V_TAG_STRIDE };
    metadata[6] = (dxil_md){ .kind = DXM_VALUE, .value = V_ZERO };
    uint32_t ext_ops[] = { 4, 5, 6, 7 };
    metadata[7] = (dxil_md){ .kind = DXM_NODE, .n = 4, .ops = ext_ops };
    uint32_t resource_ops[11] = { 0 };
    resource_ops[3] = 1; resource_ops[4] = 1; resource_ops[5] = 2;
    resource_ops[6] = 3; resource_ops[8] = 1; resource_ops[10] = 8;
    metadata[8] = (dxil_md){ .kind = DXM_NODE, .n = 11, .ops = resource_ops };
    metadata[9] = (dxil_md){ .kind = DXM_NODE };
    uint32_t uav_ops[] = { 9 };
    metadata[10] = (dxil_md){ .kind = DXM_NODE, .n = 1, .ops = uav_ops };
    metadata[11] = (dxil_md){ .kind = DXM_NODE };
    metadata[12] = (dxil_md){ .kind = DXM_NODE };
    uint32_t resources_ops[] = { 10, 11, 12, 13 };
    metadata[13] = (dxil_md){ .kind = DXM_NODE, .n = 4, .ops = resources_ops };
    uint32_t resource_node[] = { 13 };
    dxil_named_md named_resources = { .name = "dx.resources", .n = 1, .nodes = resource_node };

    dxil_value locals[5] = { 0 };
    uint32_t property_ids[] = { V_BASIC_PROPERTIES, V_COMPONENT };
    locals[0] = (dxil_value){ .kind = DXV_INST, .type = HANDLE_TYPE, .u.index = 0 };
    locals[1] = (dxil_value){ .kind = DXV_CONST_AGGREGATE, .type = PROPERTIES_TYPE,
        .u.list = { .n = 2, .ids = property_ids } };
    locals[2] = (dxil_value){ .kind = DXV_INST, .type = HANDLE_TYPE, .u.index = 1 };
    locals[3] = (dxil_value){ .kind = DXV_INST, .type = INT_TYPE, .u.index = 2 };
    locals[4] = (dxil_value){ .kind = DXV_INST, .type = INT_TYPE, .u.index = 3 };

    uint32_t create_ops[] = { FN_CREATE, V_CREATE_OPCODE, V_UAV, V_ZERO, V_ZERO, V_ZERO };
    uint32_t annotate_ops[] = { FN_ANNOTATE, V_ANNOTATE_OPCODE, MODULE_VALUE_COUNT, MODULE_VALUE_COUNT + 1 };
    uint32_t atomic_ops[] = { FN_ATOMIC, V_BIN_OPCODE, MODULE_VALUE_COUNT + 2, V_ADD,
        V_COORD, V_COORD, V_COORD, V_VALUE };
    uint32_t compare_ops[] = { FN_COMPARE, V_COMPARE_OPCODE, MODULE_VALUE_COUNT + 2,
        V_COORD, V_COORD, V_COORD, V_COMPARE_VALUE, V_NEW_VALUE };
    dxil_inst insts[] = {
        { .op = DXI_CALL, .type = HANDLE_TYPE, .result = MODULE_VALUE_COUNT, .block = 0, .n = 6, .ops = create_ops },
        { .op = DXI_CALL, .type = HANDLE_TYPE, .result = MODULE_VALUE_COUNT + 2, .block = 0, .n = 4, .ops = annotate_ops },
        { .op = DXI_CALL, .type = INT_TYPE, .result = MODULE_VALUE_COUNT + 3, .block = 0, .n = 8, .ops = atomic_ops },
        { .op = DXI_CALL, .type = INT_TYPE, .result = MODULE_VALUE_COUNT + 4, .block = 0, .n = 8, .ops = compare_ops },
        { .op = DXI_RET, .type = VOID_TYPE, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function function = { .name = "main", .nblocks = 1, .ninsts = 5, .insts = insts,
        .nlocals = 5, .locals = locals };
    dxil_module module = { .shader_kind = 5, .ntypes = TYPE_COUNT, .types = types,
        .nmodule_values = MODULE_VALUE_COUNT, .values = module_values, .nfunctions = 1, .functions = &function,
        .nmd = 14, .md = metadata, .nnamed = 1, .named = &named_resources };
    dxil_msl result = { 0 };
    int failed = dxil_to_msl(&module, &result) != 0;
    if (!failed && ((atomic_operation == 0 && !strstr(result.msl, "atomic_fetch_add")) ||
        ((atomic_operation == 4 || atomic_operation == 6) &&
         (!strstr(result.msl, "atomic_load") || !strstr(result.msl, "while (!vx_done)"))) ||
        !strstr(result.msl, "atomic_compare_exchange_weak") || !strstr(result.msl, "while (!vx_ok && vx_e.x =="))) {
        fprintf(stderr, "DXIL atomic MSL omitted typed atomic behavior for kind %u\nMSL:\n%s\n", kind, result.msl);
        failed = 1;
    }
    if (!failed && kind == 10) {
        char texture_type[64];
        snprintf(texture_type, sizeof texture_type, "texture_buffer<%s, access::read_write>", component == 4 ? "int" : "uint");
        if (!strstr(result.msl, texture_type) || !strstr(result.msl, "->size")) {
            fprintf(stderr, "DXIL typed-buffer atomic MSL omitted its resource type or view bounds\nMSL:\n%s\n", result.msl);
            failed = 1;
        }
    }
    if (!failed && kind != 10) {
        const char *element_type = component == 4 ? "int" : "uint";
        char texture_type[80];
        snprintf(texture_type, sizeof texture_type, kind == 1 ? "texture1d<%s, access::read_write>" :
            kind == 6 ? "texture1d_array<%s, access::read_write>" :
            kind == 2 ? "texture2d<%s, access::read_write>" :
            kind == 7 ? "texture2d_array<%s, access::read_write>" : "texture3d<%s, access::read_write>", element_type);
        const char *bounds[] = { "get_width()", "get_height()", "get_depth()", "get_array_size()" };
        if (!strstr(result.msl, texture_type)) failed = 1;
        if (kind == 1 || kind == 2 || kind == 4 || kind == 6 || kind == 7)
            if (!strstr(result.msl, bounds[0])) failed = 1;
        if (kind == 2 || kind == 4 || kind == 7)
            if (!strstr(result.msl, bounds[1])) failed = 1;
        if (kind == 4 && !strstr(result.msl, bounds[2])) failed = 1;
        if ((kind == 6 || kind == 7) && !strstr(result.msl, bounds[3])) failed = 1;
        if (failed) fprintf(stderr, "DXIL texture atomic MSL omitted shape %u or coordinate bounds\nMSL:\n%s\n", kind, result.msl);
    }
    if (!failed) failed = write_msl(write_path, &result);
    if (failed) fprintf(stderr, "DXIL resource atomic translation failed: %s\n", result.error);
    dxil_msl_free(&result);
    return failed;
}

static uint32_t test_le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int shader_feature_flag_from_container(void) {
    FILE *file = fopen("tests/shaders12/bindless_ps.dxil", "rb");
    if (!file) { fprintf(stderr, "could not open DXIL feature-info fixture\n"); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long length = ftell(file);
    if (length < 32 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) { fclose(file); return 1; }
    size_t read = fread(bytes, 1, (size_t)length, file);
    fclose(file);
    int failed = read != (size_t)length;
    uint32_t parts = !failed && !memcmp(bytes, "DXBC", 4) ? test_le32(bytes + 28) : 0;
    int found = 0;
    uint32_t feature_index = UINT32_MAX, dxil_index = UINT32_MAX;
    if (!failed && parts > ((size_t)length - 32) / 4) failed = 1;
    for (uint32_t i = 0; !failed && i < parts; i++) {
        uint32_t offset = test_le32(bytes + 32 + (size_t)i * 4);
        if ((size_t)offset + 8 > (size_t)length) continue;
        if (!memcmp(bytes + offset, "DXIL", 4)) dxil_index = i;
        if (memcmp(bytes + offset, "SFI0", 4)) continue;
        if ((size_t)offset + 16 > (size_t)length || test_le32(bytes + offset + 4) != sizeof(uint64_t)) continue;
        uint64_t flags = test_le32(bytes + offset + 8) | (uint64_t)test_le32(bytes + offset + 12) << 32;
        flags |= DXIL_FEATURE_WAVE_OPS_INCLUDE_HELPER_LANES;
        for (unsigned b = 0; b < 8; b++) bytes[offset + 8 + b] = (unsigned char)(flags >> (b * 8));
        found = 1;
        feature_index = i;
    }
    if (!failed && !found) {
        fprintf(stderr, "DXIL fixture has no 64-bit SFI0 feature-info part\n");
        failed = 1;
    }
    if (!failed && dxil_index != UINT32_MAX && feature_index != UINT32_MAX) {
        size_t sfi_slot = 32 + (size_t)feature_index * 4, dxil_slot = 32 + (size_t)dxil_index * 4;
        unsigned char tmp[4];
        memcpy(tmp, bytes + sfi_slot, sizeof tmp);
        memcpy(bytes + sfi_slot, bytes + dxil_slot, sizeof tmp);
        memcpy(bytes + dxil_slot, tmp, sizeof tmp);
    }
    dxil_module module = { 0 };
    if (!failed && dxil_read(bytes, read, &module)) failed = 1;
    if (!failed && !(module.shader_feature_flags & DXIL_FEATURE_WAVE_OPS_INCLUDE_HELPER_LANES)) {
        fprintf(stderr, "DXIL reader dropped the SFI0 helper-lane feature flag\n");
        failed = 1;
    }
    if (failed) fprintf(stderr, "DXIL shader feature-info parsing failed: %s\n", module.error);
    dxil_free(&module);
    free(bytes);
    return failed;
}

static int translate_wave_prefix(unsigned opcode, unsigned wave_op, int float_type, const char *write_path) {
    enum { VOID_TYPE, INT_TYPE, FLOAT_TYPE, BOOL_TYPE, TYPE_COUNT };
    dxil_type types[TYPE_COUNT] = { 0 };
    types[VOID_TYPE].kind = DXT_VOID;
    types[INT_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 32 };
    types[FLOAT_TYPE].kind = DXT_FLOAT;
    types[BOOL_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 1 };

    dxil_value module_values[1] = { { .kind = DXV_FUNCTION, .name = opcode == 136 ? "dx.op.wavePrefixBitCount.i32" : "dx.op.wavePrefixOp.i32" } };
    dxil_value locals[5] = { 0 };
    locals[0] = (dxil_value){ .kind = DXV_CONST_INT, .type = float_type ? FLOAT_TYPE : opcode == 136 ? BOOL_TYPE : INT_TYPE, .u.i = 7 };
    if (float_type) { locals[0].kind = DXV_CONST_FLOAT; locals[0].u.f = 7.0; }
    locals[1] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = opcode };
    locals[2] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = wave_op };
    locals[3] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = float_type ? 1 : 0 };
    locals[4] = (dxil_value){ .kind = DXV_INST, .type = INT_TYPE, .u.index = 0 };
    if (float_type && opcode != 136) locals[4].type = FLOAT_TYPE;

    uint32_t call_ops[] = { 0, 2, 1, 3, 4 };
    dxil_inst insts[2] = {
        { .op = DXI_CALL, .type = locals[4].type, .result = 5, .block = 0,
          .n = opcode == 136 ? 3u : 5u, .ops = call_ops },
        { .op = DXI_RET, .type = VOID_TYPE, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function function = { .name = "main", .nblocks = 1, .ninsts = 2, .insts = insts,
        .nlocals = 5, .locals = locals };
    dxil_module module = { .shader_kind = 5, .ntypes = TYPE_COUNT, .types = types,
        .nmodule_values = 1, .values = module_values, .nfunctions = 1, .functions = &function };
    dxil_msl result = { 0 };
    int failed = dxil_to_msl(&module, &result) != 0;
    if (!failed && opcode == 121 && !float_type &&
        (!strstr(result.msl, "simd_ballot(true)") ||
         !strstr(result.msl, "simd_shuffle(as_type<int>(7u), ushort(vx_wp_source))") ||
         !strstr(result.msl, "v4 = as_type<uint>(vx_wp_acc);") ||
         !strstr(result.msl, "uint vx_lane [[thread_index_in_simdgroup]]") ||
         !strstr(result.msl, "uint vx_lanes [[threads_per_simdgroup]]"))) {
        fprintf(stderr, "DXIL signed wave prefix min lowering is incomplete\nMSL:\n%s\n", result.msl);
        failed = 1;
    }
    if (!failed && opcode == 121 && float_type &&
        (!strstr(result.msl, "isnan(vx_wp_acc)") ||
         !strstr(result.msl, "vx_wp_acc >= vx_wp_value ? vx_wp_acc : vx_wp_value"))) {
        fprintf(stderr, "DXIL floating wave prefix max lowering is incomplete\nMSL:\n%s\n", result.msl);
        failed = 1;
    }
    if (!failed && opcode == 136 &&
        (!strstr(result.msl, "simd_ballot((true) && bool(true))") ||
         !strstr(result.msl, "vx_wbc_count += uint((vx_wbc_true >> vx_wbc_i) & 1ul)"))) {
        fprintf(stderr, "DXIL wave prefix bit count lowering is incomplete\nMSL:\n%s\n", result.msl);
        failed = 1;
    }
    if (!failed && write_path) {
        FILE *file = fopen(write_path, "wb");
        int write_failed = !file;
        if (file) {
            if (fwrite(result.msl, 1, strlen(result.msl), file) != strlen(result.msl)) write_failed = 1;
            if (fclose(file)) write_failed = 1;
        }
        if (write_failed) {
            fprintf(stderr, "could not write generated MSL to %s\n", write_path);
            failed = 1;
        }
    }
    if (failed) fprintf(stderr, "DXIL wave prefix translation failed: %s\n", result.error);
    dxil_msl_free(&result);
    return failed;
}

static int write_msl(const char *path, const dxil_msl *result) {
    if (!path) return 0;
    FILE *file = fopen(path, "wb");
    int failed = !file;
    if (file) {
        if (fwrite(result->msl, 1, strlen(result->msl), file) != strlen(result->msl)) failed = 1;
        if (fclose(file)) failed = 1;
    }
    if (failed) fprintf(stderr, "could not write generated MSL to %s\n", path);
    return failed;
}

static dxil_value *mutable_value(dxil_module *module, dxil_function *function, uint32_t id) {
    if (id < module->nmodule_values) return &module->values[id];
    id -= module->nmodule_values;
    return id < function->nlocals ? &function->locals[id] : NULL;
}

static int set_instruction_opcode(dxil_module *module, dxil_function *function, dxil_inst *inst, int opcode) {
    const dxil_value *old = dxil_value_at(module, function, inst->ops[1]);
    if (!old || old->kind != DXV_CONST_INT) return 0;
    uint32_t type = old->type;
    dxil_value *locals = realloc(function->locals, (size_t)(function->nlocals + 1) * sizeof *locals);
    if (!locals) return 0;
    function->locals = locals;
    locals[function->nlocals] = (dxil_value){ .kind = DXV_CONST_INT, .type = type, .u.i = opcode };
    inst->ops[1] = module->nmodule_values + function->nlocals;
    function->nlocals++;
    return 1;
}

static uint32_t append_int_constant(dxil_module *module, dxil_function *function, uint32_t type, int64_t value) {
    dxil_value *locals = realloc(function->locals, (size_t)(function->nlocals + 1) * sizeof *locals);
    if (!locals) return UINT32_MAX;
    function->locals = locals;
    locals[function->nlocals] = (dxil_value){ .kind = DXV_CONST_INT, .type = type, .u.i = value };
    return module->nmodule_values + function->nlocals++;
}

static uint32_t find_undef_value(const dxil_module *module, const dxil_function *function, uint32_t type) {
    uint32_t count = module->nmodule_values + function->nlocals;
    for (uint32_t id = 0; id < count; id++) {
        const dxil_value *value = dxil_value_at(module, function, id);
        if (value && value->kind == DXV_CONST_UNDEF && value->type == type) return id;
    }
    return UINT32_MAX;
}

static int reject_1d_explicit_sample_options(void) {
    static const int opcodes[] = { 60, 61, 62, 63 };
    FILE *file = fopen("tests/shaders12/samplegrad_ps.dxil", "rb");
    if (!file) { fprintf(stderr, "could not open 1D sample-options DXIL fixture\n"); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) { fclose(file); return 1; }
    size_t read = fread(bytes, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) { free(bytes); return 1; }

    int failed = 0;
    for (size_t i = 0; i < sizeof opcodes / sizeof opcodes[0] && !failed; i++) {
        dxil_module module = { 0 };
        dxil_msl result = { 0 };
        if (dxil_read(bytes, read, &module)) {
            fprintf(stderr, "could not read 1D sample-options fixture: %s\n", module.error);
            failed = 1;
        }
        int changed = 0;
        for (uint32_t fi = 0; !failed && fi < module.nfunctions; fi++) {
            dxil_function *function = &module.functions[fi];
            for (uint32_t ii = 0; ii < function->ninsts; ii++) {
                dxil_inst *inst = &function->insts[ii];
                if (dxil_dxop(&module, function, inst) != 63) continue;
                dxil_value *opcode = mutable_value(&module, function, inst->ops[1]);
                if (!opcode || opcode->kind != DXV_CONST_INT) { failed = 1; break; }
                opcode->u.i = opcodes[i];
                changed++;
            }
        }
        if (!failed && !changed) {
            fprintf(stderr, "1D sample-options fixture contains no SampleGrad instruction\n");
            failed = 1;
        }
        if (!failed && dxil_to_msl(&module, &result) == 0) {
            fprintf(stderr, "1D texture sampling opcode %d was silently translated\n", opcodes[i]);
            failed = 1;
        }
        const char *expected = opcodes[i] == 60 ? "sampler min-LOD clamp" : "explicit bias, LOD, or gradients";
        if (!failed && !strstr(result.error, expected)) {
            fprintf(stderr, "1D texture sampling opcode %d had an unexpected diagnostic: %s\n", opcodes[i], result.error);
            failed = 1;
        }
        dxil_msl_free(&result);
        dxil_free(&module);
    }
    if (!failed) {
        dxil_module module = { 0 };
        dxil_msl result = { 0 };
        if (dxil_read(bytes, read, &module)) failed = 1;
        int found = 0;
        for (uint32_t fi = 0; !failed && fi < module.nfunctions && !found; fi++) {
            dxil_function *function = &module.functions[fi];
            for (uint32_t ii = 0; ii < function->ninsts; ii++) {
                dxil_inst *inst = &function->insts[ii];
                if (dxil_dxop(&module, function, inst) != 63 || inst->n <= 11) continue;
                const dxil_value *gradient = dxil_value_at(&module, function, inst->ops[11]);
                const dxil_value *offset = dxil_value_at(&module, function, inst->ops[8]);
                uint32_t undef = gradient ? find_undef_value(&module, function, gradient->type) : UINT32_MAX;
                uint32_t nonzero = offset ? append_int_constant(&module, function, offset->type, 1) : UINT32_MAX;
                if (undef == UINT32_MAX || nonzero == UINT32_MAX || !set_instruction_opcode(&module, function, inst, 60)) {
                    failed = 1;
                    break;
                }
                inst->ops[8] = nonzero;
                inst->ops[11] = undef;
                found = 1;
                break;
            }
        }
        if (!failed && !found) {
            fprintf(stderr, "1D texel-offset fixture contains no SampleGrad instruction\n");
            failed = 1;
        }
        if (!failed && dxil_to_msl(&module, &result) == 0) {
            fprintf(stderr, "1D sample with a nonzero texel offset was silently translated\n");
            failed = 1;
        }
        if (!failed && !strstr(result.error, "nonzero or dynamic texel offsets")) {
            fprintf(stderr, "1D sample offset had an unexpected diagnostic: %s\n", result.error);
            failed = 1;
        }
        dxil_msl_free(&result);
        dxil_free(&module);
    }
    free(bytes);
    return failed;
}

static int translate_cube_array_min_lod(const char *write_path) {
    FILE *file = fopen("tests/shaders12/samplegrad_ps.dxil", "rb");
    if (!file) { fprintf(stderr, "could not open SampleGrad DXIL fixture for min-LOD test\n"); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) { fclose(file); return 1; }
    size_t read = fread(bytes, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) { free(bytes); return 1; }

    dxil_module module = { 0 };
    dxil_msl result = { 0 };
    int failed = dxil_read(bytes, read, &module) != 0;
    free(bytes);
    if (failed) {
        fprintf(stderr, "could not read SampleGrad DXIL fixture: %s\n", module.error);
        dxil_free(&module);
        return 1;
    }

    dxil_function *sample_functions[16] = { 0 };
    dxil_inst *sample_insts[16] = { 0 };
    size_t sample_count = 0;
    for (uint32_t fi = 0; fi < module.nfunctions; fi++) {
        dxil_function *function = &module.functions[fi];
        for (uint32_t ii = 0; ii < function->ninsts; ii++) {
            dxil_inst *inst = &function->insts[ii];
            if (dxil_dxop(&module, function, inst) != 63) continue;
            if (sample_count == sizeof sample_insts / sizeof sample_insts[0]) { failed = 1; break; }
            sample_functions[sample_count] = function;
            sample_insts[sample_count++] = inst;
        }
    }
    if (sample_count != 8) {
        fprintf(stderr, "SampleGrad fixture contained %zu calls, expected 8\n", sample_count);
        failed = 1;
    }
    for (size_t i = 0; !failed && i < sample_count; i++) {
        dxil_function *function = sample_functions[i];
        dxil_inst *inst = sample_insts[i];
        // The fixture's first two calls are Texture1D/Texture1DArray; make them ordinary Sample calls so
        // the remaining supported-dimensional SampleGrad cases can reach the final CubeArray check.
        if (i < 2) {
            const dxil_value *gradient = inst->n > 11 ? dxil_value_at(&module, function, inst->ops[11]) : NULL;
            uint32_t undef = gradient ? find_undef_value(&module, function, gradient->type) : UINT32_MAX;
            if (undef == UINT32_MAX || !set_instruction_opcode(&module, function, inst, 60)) { failed = 1; break; }
            inst->ops[11] = undef;
        } else if (i == 7 && !set_instruction_opcode(&module, function, inst, 60)) {
            failed = 1;
        }
    }
    if (!failed && dxil_to_msl(&module, &result)) {
        fprintf(stderr, "CubeArray min-LOD translation failed: %s\n", result.error);
        failed = 1;
    }
    if (!failed) {
        dxil_inst *cube_array = sample_insts[7];
        uint32_t clamp_id = cube_array->ops[11];
        char expected[128];
        if (clamp_id < module.nmodule_values) {
            fprintf(stderr, "CubeArray min-LOD fixture clamp unexpectedly uses a module constant\n");
            failed = 1;
        } else {
            snprintf(expected, sizeof expected, "max(vx_desc_min_lod((device const VxDesc *)v%u), v%u)",
                     (unsigned)(cube_array->ops[2] - module.nmodule_values),
                     clamp_id - module.nmodule_values);
            if (!strstr(result.msl, "texturecube_array") || !strstr(result.msl, "#if defined(VXRT_HAS_MIN_LOD_CLAMP)") ||
                !strstr(result.msl, expected) || !result.uses_min_lod_clamp) {
                fprintf(stderr, "CubeArray min-LOD lowering missing %s\nMSL:\n%s\n", expected, result.msl);
                failed = 1;
            }
        }
    }
    if (!failed) failed = write_msl(write_path, &result);
    dxil_msl_free(&result);
    dxil_free(&module);
    return failed;
}

static int translate_depth_cube_comparison(const char *write_path) {
    FILE *file = fopen("tests/shaders12/cmp_ps.dxil", "rb");
    if (!file) { fprintf(stderr, "could not open comparison-sampling DXIL fixture\n"); return 1; }
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    unsigned char *bytes = malloc((size_t)length);
    if (!bytes) { fclose(file); return 1; }
    size_t read = fread(bytes, 1, (size_t)length, file);
    fclose(file);
    if (read != (size_t)length) { free(bytes); return 1; }

    dxil_module module = { 0 };
    dxil_msl result = { 0 };
    int failed = dxil_read(bytes, read, &module) != 0;
    free(bytes);
    if (failed) {
        fprintf(stderr, "could not read comparison-sampling fixture: %s\n", module.error);
        dxil_free(&module);
        return 1;
    }

    int converted = 0;
    const dxil_named_md *resources = dxil_named(&module, "dx.resources");
    const dxil_md *lists = resources && resources->n ? &module.md[resources->nodes[0]] : NULL;
    const dxil_md *srv_ranges = dxil_md_op(&module, lists, DXIL_SRV);
    for (uint32_t ri = 0; !failed && srv_ranges && ri < srv_ranges->n; ri++) {
        const dxil_md *resource = dxil_md_op(&module, srv_ranges, ri);
        const dxil_md *kind_md = dxil_md_op(&module, resource, 6);
        if (!kind_md || kind_md->kind != DXM_VALUE || kind_md->value >= module.nmodule_values) continue;
        dxil_value *kind = &module.values[kind_md->value];
        if (kind->kind != DXV_CONST_INT) continue;
        uint32_t old_kind = (uint32_t)kind->u.i;
        uint32_t new_kind = old_kind == 2 ? 5u : old_kind == 7 ? 9u : 0u;
        if (!new_kind) continue;

        uint32_t replacement_id = UINT32_MAX;
        for (uint32_t vi = 0; vi < module.nmodule_values; vi++) {
            const dxil_value *candidate = &module.values[vi];
            if (candidate->kind == DXV_CONST_INT && candidate->type == kind->type && candidate->u.i == new_kind) {
                replacement_id = vi;
                break;
            }
        }
        if (replacement_id != UINT32_MAX) {
            size_t md_id = (size_t)(kind_md - module.md);
            module.md[md_id].value = replacement_id;
        } else {
            kind->u.i = new_kind;
        }
        converted++;
    }
    if (!failed && converted != 2) {
        fprintf(stderr, "comparison fixture contained %d 2D depth handles, expected 2\n", converted);
        failed = 1;
    }
    if (!failed && dxil_to_msl(&module, &result)) {
        fprintf(stderr, "cube depth comparison translation failed: %s\n", result.error);
        failed = 1;
    }
    if (!failed && (!strstr(result.msl, "depthcube<float>") ||
                    !strstr(result.msl, "depthcube_array<float>") ||
                    !strstr(result.msl, "sample_compare(") ||
                    !strstr(result.msl, "vx_desc_min_lod((device const VxDesc *)"))) {
        fprintf(stderr, "cube depth comparison lowering is incomplete\nMSL:\n%s\n", result.msl);
        failed = 1;
    }
    if (!failed) failed = write_msl(write_path, &result);
    dxil_msl_free(&result);
    dxil_free(&module);
    return failed;
}

static int translate_unordered_equal(const char *write_path) {
    enum { VOID_TYPE, FLOAT_TYPE, BOOL_TYPE, FLOAT4_TYPE, BOOL4_TYPE, TYPE_COUNT };
    dxil_type types[TYPE_COUNT] = { 0 };
    types[VOID_TYPE].kind = DXT_VOID;
    types[FLOAT_TYPE].kind = DXT_FLOAT;
    types[BOOL_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 1 };
    types[FLOAT4_TYPE] = (dxil_type){ .kind = DXT_VECTOR, .count = 4, .elem = FLOAT_TYPE };
    types[BOOL4_TYPE] = (dxil_type){ .kind = DXT_VECTOR, .count = 4, .elem = BOOL_TYPE };

    dxil_value locals[6] = { 0 };
    locals[0] = (dxil_value){ .kind = DXV_CONST_FLOAT, .type = FLOAT_TYPE, .u.f = __builtin_nan("") };
    locals[1] = (dxil_value){ .kind = DXV_CONST_FLOAT, .type = FLOAT_TYPE, .u.f = 1.0 };
    locals[2] = (dxil_value){ .kind = DXV_CONST_NULL, .type = FLOAT4_TYPE };
    locals[3] = (dxil_value){ .kind = DXV_CONST_NULL, .type = FLOAT4_TYPE };
    locals[4] = (dxil_value){ .kind = DXV_INST, .type = BOOL_TYPE, .u.index = 0 };
    locals[5] = (dxil_value){ .kind = DXV_INST, .type = BOOL4_TYPE, .u.index = 1 };
    uint32_t scalar_ops[] = { 0, 1 }, vector_ops[] = { 2, 3 };
    dxil_inst insts[] = {
        { .op = DXI_CMP, .sub = 9, .type = BOOL_TYPE, .result = 4, .block = 0,
          .n = 2, .ops = scalar_ops },
        { .op = DXI_CMP, .sub = 9, .type = BOOL4_TYPE, .result = 5, .block = 0,
          .n = 2, .ops = vector_ops },
        { .op = DXI_RET, .type = VOID_TYPE, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function function = { .name = "main", .nblocks = 1, .ninsts = 3, .insts = insts,
        .nlocals = 6, .locals = locals };
    dxil_module module = { .shader_kind = 5, .ntypes = TYPE_COUNT, .types = types,
        .nfunctions = 1, .functions = &function };
    dxil_msl result = { 0 };
    int failed = dxil_to_msl(&module, &result) != 0;
    const char *required[] = {
        "(as_type<float>(0x7fc00000u) == 1.0f) | (isnan(as_type<float>(0x7fc00000u)) | isnan(1.0f))",
        "(float4(0) == float4(0)) | (isnan(float4(0)) | isnan(float4(0)))"
    };
    for (size_t i = 0; !failed && i < sizeof required / sizeof required[0]; i++) {
        if (!strstr(result.msl, required[i])) {
            fprintf(stderr, "DXIL unordered-equal MSL missing scalar/vector NaN handling: %s\nMSL:\n%s\n",
                    required[i], result.msl);
            failed = 1;
        }
    }
    if (!failed) failed = write_msl(write_path, &result);
    if (failed) fprintf(stderr, "DXIL unordered-equal translation failed: %s\n", result.error);
    dxil_msl_free(&result);
    return failed;
}

static int translate_texture_gathers(const char *write_path) {
    enum { VOID_TYPE, INT_TYPE, FLOAT_TYPE, HANDLE_TYPE, PROPERTIES_TYPE, RESRET_TYPE, TYPE_COUNT };
    dxil_type types[TYPE_COUNT] = { 0 };
    uint32_t properties_members[] = { INT_TYPE, INT_TYPE };
    uint32_t resret_members[] = { FLOAT_TYPE, FLOAT_TYPE, FLOAT_TYPE, FLOAT_TYPE, INT_TYPE };
    types[VOID_TYPE].kind = DXT_VOID;
    types[INT_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 32 };
    types[FLOAT_TYPE].kind = DXT_FLOAT;
    types[HANDLE_TYPE] = (dxil_type){ .kind = DXT_STRUCT, .name = "dx.types.Handle" };
    types[PROPERTIES_TYPE] = (dxil_type){ .kind = DXT_STRUCT, .count = 2, .members = properties_members,
        .name = "dx.types.HandleProperties" };
    types[RESRET_TYPE] = (dxil_type){ .kind = DXT_STRUCT, .count = 5, .members = resret_members,
        .name = "dx.types.ResRet.f32" };

    enum {
        FN_CREATE, FN_ANNOTATE, FN_GATHER, FN_GATHER_CMP,
        V_CREATE_OPCODE, V_ANNOTATE_OPCODE, V_GATHER_OPCODE, V_GATHER_CMP_OPCODE,
        V_ZERO, V_ONE, V_NEG_ONE, V_SAMPLER_HEAP, V_COMPONENT_FLOAT,
        V_KIND_2D, V_KIND_2D_ARRAY, V_KIND_CUBE, V_KIND_CUBE_ARRAY,
        V_UV0, V_UV1, V_UV2, V_UV3, V_CHANNEL, V_DEPTH_CHANNEL, V_COMPARE,
        V_PROPS_2D, V_PROPS_2D_ARRAY, V_PROPS_CUBE, V_PROPS_CUBE_ARRAY, MODULE_VALUE_COUNT
    };
    dxil_value module_values[MODULE_VALUE_COUNT] = {
        [FN_CREATE] = { .kind = DXV_FUNCTION, .name = "dx.op.createHandleFromHeap" },
        [FN_ANNOTATE] = { .kind = DXV_FUNCTION, .name = "dx.op.annotateHandle" },
        [FN_GATHER] = { .kind = DXV_FUNCTION, .name = "dx.op.textureGather.f32" },
        [FN_GATHER_CMP] = { .kind = DXV_FUNCTION, .name = "dx.op.textureGatherCmp.f32" },
        [V_CREATE_OPCODE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 218 },
        [V_ANNOTATE_OPCODE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 216 },
        [V_GATHER_OPCODE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 73 },
        [V_GATHER_CMP_OPCODE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 74 },
        [V_ZERO] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 0 },
        [V_ONE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 1 },
        [V_NEG_ONE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = -1 },
        [V_SAMPLER_HEAP] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 1 },
        [V_COMPONENT_FLOAT] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 9 },
        [V_KIND_2D] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 2 },
        [V_KIND_2D_ARRAY] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 7 },
        [V_KIND_CUBE] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 5 },
        [V_KIND_CUBE_ARRAY] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 9 },
        [V_UV0] = { .kind = DXV_CONST_FLOAT, .type = FLOAT_TYPE, .u.f = 0.25 },
        [V_UV1] = { .kind = DXV_CONST_FLOAT, .type = FLOAT_TYPE, .u.f = 0.75 },
        [V_UV2] = { .kind = DXV_CONST_FLOAT, .type = FLOAT_TYPE, .u.f = 0.5 },
        [V_UV3] = { .kind = DXV_CONST_FLOAT, .type = FLOAT_TYPE, .u.f = 0.125 },
        [V_CHANNEL] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 2 },
        [V_DEPTH_CHANNEL] = { .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 0 },
        [V_COMPARE] = { .kind = DXV_CONST_FLOAT, .type = FLOAT_TYPE, .u.f = 0.5 }
    };
    uint32_t prop_ids[4][2] = {
        { V_KIND_2D, V_COMPONENT_FLOAT }, { V_KIND_2D_ARRAY, V_COMPONENT_FLOAT },
        { V_KIND_CUBE, V_COMPONENT_FLOAT }, { V_KIND_CUBE_ARRAY, V_COMPONENT_FLOAT }
    };
    module_values[V_PROPS_2D] = (dxil_value){ .kind = DXV_CONST_AGGREGATE, .type = PROPERTIES_TYPE,
        .u.list = { .n = 2, .ids = prop_ids[0] } };
    module_values[V_PROPS_2D_ARRAY] = (dxil_value){ .kind = DXV_CONST_AGGREGATE, .type = PROPERTIES_TYPE,
        .u.list = { .n = 2, .ids = prop_ids[1] } };
    module_values[V_PROPS_CUBE] = (dxil_value){ .kind = DXV_CONST_AGGREGATE, .type = PROPERTIES_TYPE,
        .u.list = { .n = 2, .ids = prop_ids[2] } };
    module_values[V_PROPS_CUBE_ARRAY] = (dxil_value){ .kind = DXV_CONST_AGGREGATE, .type = PROPERTIES_TYPE,
        .u.list = { .n = 2, .ids = prop_ids[3] } };

    enum { CREATE_BASE = 0, TEXTURE_BASE = 4, SAMPLER_LOCAL = 8, GATHER_BASE = 9, GATHER_CMP_BASE = 13, LOCAL_COUNT = 17 };
    uint32_t create_ops[4][4], annotate_ops[4][4], gather_ops[4][11], gather_cmp_ops[4][12];
    uint32_t sampler_ops[] = { FN_CREATE, V_CREATE_OPCODE, V_ZERO, V_SAMPLER_HEAP };
    dxil_inst insts[18] = { 0 };
    dxil_value locals[LOCAL_COUNT] = { 0 };
    static const uint32_t properties_ids[] = { V_PROPS_2D, V_PROPS_2D_ARRAY, V_PROPS_CUBE, V_PROPS_CUBE_ARRAY };
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t created = MODULE_VALUE_COUNT + CREATE_BASE + i;
        uint32_t texture = MODULE_VALUE_COUNT + TEXTURE_BASE + i;
        uint32_t gathered = MODULE_VALUE_COUNT + GATHER_BASE + i;
        uint32_t gathered_cmp = MODULE_VALUE_COUNT + GATHER_CMP_BASE + i;
        create_ops[i][0] = FN_CREATE; create_ops[i][1] = V_CREATE_OPCODE;
        create_ops[i][2] = V_ZERO; create_ops[i][3] = V_ZERO;
        annotate_ops[i][0] = FN_ANNOTATE; annotate_ops[i][1] = V_ANNOTATE_OPCODE;
        annotate_ops[i][2] = created; annotate_ops[i][3] = properties_ids[i];
        gather_ops[i][0] = FN_GATHER; gather_ops[i][1] = V_GATHER_OPCODE;
        gather_ops[i][2] = texture; gather_ops[i][3] = MODULE_VALUE_COUNT + SAMPLER_LOCAL;
        gather_ops[i][4] = V_UV0; gather_ops[i][5] = V_UV1; gather_ops[i][6] = V_UV2; gather_ops[i][7] = V_UV3;
        gather_ops[i][8] = i < 2 ? V_ONE : V_ZERO;
        gather_ops[i][9] = i < 2 ? V_NEG_ONE : V_ZERO;
        gather_ops[i][10] = V_CHANNEL;
        gather_cmp_ops[i][0] = FN_GATHER_CMP; gather_cmp_ops[i][1] = V_GATHER_CMP_OPCODE;
        gather_cmp_ops[i][2] = texture; gather_cmp_ops[i][3] = MODULE_VALUE_COUNT + SAMPLER_LOCAL;
        gather_cmp_ops[i][4] = V_UV0; gather_cmp_ops[i][5] = V_UV1;
        gather_cmp_ops[i][6] = V_UV2; gather_cmp_ops[i][7] = V_UV3;
        gather_cmp_ops[i][8] = i < 2 ? V_ONE : V_ZERO;
        gather_cmp_ops[i][9] = i < 2 ? V_NEG_ONE : V_ZERO;
        gather_cmp_ops[i][10] = V_DEPTH_CHANNEL; gather_cmp_ops[i][11] = V_COMPARE;
        locals[CREATE_BASE + i] = (dxil_value){ .kind = DXV_INST, .type = HANDLE_TYPE, .u.index = CREATE_BASE + i };
        locals[TEXTURE_BASE + i] = (dxil_value){ .kind = DXV_INST, .type = HANDLE_TYPE, .u.index = TEXTURE_BASE + i };
        locals[GATHER_BASE + i] = (dxil_value){ .kind = DXV_INST, .type = RESRET_TYPE, .u.index = GATHER_BASE + i };
        locals[GATHER_CMP_BASE + i] = (dxil_value){ .kind = DXV_INST, .type = RESRET_TYPE, .u.index = GATHER_CMP_BASE + i };
        insts[CREATE_BASE + i] = (dxil_inst){ .op = DXI_CALL, .type = HANDLE_TYPE, .result = created,
            .block = 0, .n = 4, .ops = create_ops[i] };
        insts[TEXTURE_BASE + i] = (dxil_inst){ .op = DXI_CALL, .type = HANDLE_TYPE, .result = texture,
            .block = 0, .n = 4, .ops = annotate_ops[i] };
        insts[GATHER_BASE + i] = (dxil_inst){ .op = DXI_CALL, .type = RESRET_TYPE, .result = gathered,
            .block = 0, .n = 11, .ops = gather_ops[i] };
        insts[GATHER_CMP_BASE + i] = (dxil_inst){ .op = DXI_CALL, .type = RESRET_TYPE, .result = gathered_cmp,
            .block = 0, .n = 12, .ops = gather_cmp_ops[i] };
    }
    insts[8] = (dxil_inst){ .op = DXI_CALL, .type = HANDLE_TYPE, .result = MODULE_VALUE_COUNT + SAMPLER_LOCAL,
        .block = 0, .n = 4, .ops = sampler_ops };
    locals[SAMPLER_LOCAL] = (dxil_value){ .kind = DXV_INST, .type = HANDLE_TYPE, .u.index = 8 };
    insts[17] = (dxil_inst){ .op = DXI_RET, .type = VOID_TYPE, .result = UINT32_MAX, .block = 0 };
    dxil_function function = { .name = "main", .nblocks = 1, .ninsts = 18, .insts = insts,
        .nlocals = LOCAL_COUNT, .locals = locals };
    dxil_module module = { .shader_kind = 5, .ntypes = TYPE_COUNT, .types = types,
        .nmodule_values = MODULE_VALUE_COUNT, .values = module_values, .nfunctions = 1, .functions = &function };
    dxil_msl result = { 0 };
    int failed = dxil_to_msl(&module, &result) != 0;
    if (!failed && !result.uses_gather) {
        fprintf(stderr, "DXIL gather shader metadata omitted its use of TextureGather\n");
        failed = 1;
    }
    const char *required[] = { "texture2d<float>", "texture2d_array<float>", "texturecube<float>",
        "texturecube_array<float>", "depth2d<float>", "depth2d_array<float>", "depthcube<float>",
        "depthcube_array<float>", ".gather(", ".gather_compare(", "component::z", "int2(1, -1)" };
    for (size_t i = 0; !failed && i < sizeof required / sizeof required[0]; i++) {
        if (!strstr(result.msl, required[i])) {
            fprintf(stderr, "DXIL texture gather MSL missing %s\nMSL:\n%s\n", required[i], result.msl);
            failed = 1;
        }
    }
    if (!failed) {
        module_values[V_ONE].u.i = 8;
        dxil_msl rejected = { 0 };
        if (!dxil_to_msl(&module, &rejected) || !strstr(rejected.error, "offsets must be constants in [-8, 7]")) {
            fprintf(stderr, "DXIL texture gather accepted an offset that Metal cannot represent\n");
            failed = 1;
        }
        dxil_msl_free(&rejected);
        module_values[V_ONE].u.i = 1;
    }
    if (!failed) {
        insts[GATHER_BASE].n--;
        dxil_msl malformed = { 0 };
        if (!dxil_to_msl(&module, &malformed) ||
            !strstr(malformed.error, "malformed texture gather signature")) {
            fprintf(stderr, "DXIL texture gather accepted a truncated intrinsic signature\n");
            failed = 1;
        }
        dxil_msl_free(&malformed);
        insts[GATHER_BASE].n++;
    }
    if (!failed) failed = write_msl(write_path, &result);
    if (failed) fprintf(stderr, "DXIL texture gather translation failed: %s\n", result.error);
    dxil_msl_free(&result);
    return failed;
}

static int translate_wave_match_multiprefix(const char *write_path, const char *pixel_write_path) {
    enum { VOID_TYPE, INT_TYPE, BOOL_TYPE, INT2_TYPE, UINT4_TYPE, TYPE_COUNT };
    dxil_type types[TYPE_COUNT] = { 0 };
    types[VOID_TYPE].kind = DXT_VOID;
    types[INT_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 32 };
    types[BOOL_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 1 };
    types[INT2_TYPE] = (dxil_type){ .kind = DXT_VECTOR, .count = 2, .elem = INT_TYPE };
    types[UINT4_TYPE] = (dxil_type){ .kind = DXT_VECTOR, .count = 4, .elem = INT_TYPE };

    dxil_value module_values[3] = {
        { .kind = DXV_FUNCTION, .name = "dx.op.waveMatch.v2i32" },
        { .kind = DXV_FUNCTION, .name = "dx.op.waveMultiPrefixOp.i32" },
        { .kind = DXV_FUNCTION, .name = "dx.op.waveMultiPrefixBitCount" }
    };
    dxil_value locals[30] = { 0 };
    locals[0] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 165 };
    locals[1] = (dxil_value){ .kind = DXV_CONST_NULL, .type = INT2_TYPE };
    locals[2] = (dxil_value){ .kind = DXV_INST, .type = UINT4_TYPE, .u.index = 0 };
    locals[3] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 166 };
    locals[4] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 7 };
    for (uint32_t i = 5; i <= 8; i++) locals[i] = (dxil_value){
        .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = UINT32_MAX
    };
    locals[9] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 0 };
    locals[10] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 0 };
    locals[11] = (dxil_value){ .kind = DXV_INST, .type = INT_TYPE, .u.index = 1 };
    locals[12] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 167 };
    locals[13] = (dxil_value){ .kind = DXV_CONST_INT, .type = BOOL_TYPE, .u.i = 1 };
    for (uint32_t i = 14; i <= 17; i++) locals[i] = (dxil_value){
        .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = UINT32_MAX
    };
    locals[18] = (dxil_value){ .kind = DXV_INST, .type = INT_TYPE, .u.index = 2 };
    locals[19] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 165 };
    locals[20] = (dxil_value){ .kind = DXV_CONST_INT, .type = BOOL_TYPE, .u.i = 1 };
    locals[21] = (dxil_value){ .kind = DXV_INST, .type = UINT4_TYPE, .u.index = 3 };
    for (uint32_t i = 0; i < 4; i++) locals[22 + i] = (dxil_value){
        .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = i + 1
    };
    for (uint32_t i = 0; i < 4; i++) locals[26 + i] = (dxil_value){
        .kind = DXV_INST, .type = INT_TYPE, .u.index = 4 + i
    };

    uint32_t match_ops[] = { 0, 3, 4 };
    uint32_t multiprefix_ops[] = { 1, 6, 7, 8, 9, 10, 11, 12, 13 };
    uint32_t bitcount_ops[] = { 2, 15, 16, 17, 18, 19, 20 };
    uint32_t bool_match_ops[] = { 0, 22, 23 };
    uint32_t multiprefix_and_ops[] = { 1, 6, 7, 8, 9, 10, 11, 25, 13 };
    uint32_t multiprefix_or_ops[] = { 1, 6, 7, 8, 9, 10, 11, 26, 13 };
    uint32_t multiprefix_xor_ops[] = { 1, 6, 7, 8, 9, 10, 11, 27, 13 };
    uint32_t multiprefix_product_ops[] = { 1, 6, 7, 8, 9, 10, 11, 28, 13 };
    dxil_inst insts[9] = {
        { .op = DXI_CALL, .type = UINT4_TYPE, .result = 5, .block = 0,
          .n = 3, .ops = match_ops },
        { .op = DXI_CALL, .type = INT_TYPE, .result = 14, .block = 0,
          .n = 9, .ops = multiprefix_ops },
        { .op = DXI_CALL, .type = INT_TYPE, .result = 29, .block = 0,
          .n = 9, .ops = multiprefix_and_ops },
        { .op = DXI_CALL, .type = INT_TYPE, .result = 30, .block = 0,
          .n = 9, .ops = multiprefix_or_ops },
        { .op = DXI_CALL, .type = INT_TYPE, .result = 31, .block = 0,
          .n = 9, .ops = multiprefix_xor_ops },
        { .op = DXI_CALL, .type = INT_TYPE, .result = 32, .block = 0,
          .n = 9, .ops = multiprefix_product_ops },
        { .op = DXI_CALL, .type = INT_TYPE, .result = 21, .block = 0,
          .n = 7, .ops = bitcount_ops },
        { .op = DXI_CALL, .type = UINT4_TYPE, .result = 24, .block = 0,
          .n = 3, .ops = bool_match_ops },
        { .op = DXI_RET, .type = VOID_TYPE, .result = UINT32_MAX, .block = 0 }
    };
    dxil_function function = { .name = "main", .nblocks = 1, .ninsts = 9, .insts = insts,
        .nlocals = 30, .locals = locals };
    dxil_module module = { .shader_kind = 5, .ntypes = TYPE_COUNT, .types = types,
        .nmodule_values = 3, .values = module_values, .nfunctions = 1, .functions = &function };
    dxil_msl result = { 0 };
    int failed = dxil_to_msl(&module, &result) != 0;
    const char *required[] = {
        "simd_shuffle(uint2(0), ushort(vx_wm_i))",
        "all(vx_wm_value == uint2(0))",
        "simd_shuffle(uint(true), ushort(vx_wm_i))",
        "vx_wm_mask[vx_wm_i >> 5] |= 1u << (vx_wm_i & 31u)",
        "for (uint vx_mp_i = 0; vx_mp_i < vx_lanes; ++vx_mp_i)",
        "bool vx_mp_include = vx_mp_i < vx_lane && ((vx_mp_active >> vx_mp_i) & ulong(1)) != 0ul",
        "vx_mp_lane_mask == vx_mp_target_mask",
        "vx_mp_acc = vx_mp_acc + vx_mp_value",
        "vx_mp_acc = vx_mp_acc & vx_mp_value",
        "vx_mp_acc = vx_mp_acc | vx_mp_value",
        "vx_mp_acc = vx_mp_acc ^ vx_mp_value",
        "vx_mp_acc = vx_mp_acc * vx_mp_value",
        "vx_mp_acc += vx_mp_value != 0u ? 1u : 0u"
    };
    for (size_t i = 0; !failed && i < sizeof required / sizeof required[0]; i++) {
        if (!strstr(result.msl, required[i])) {
            fprintf(stderr, "DXIL wave-match/prefix MSL missing: %s\nMSL:\n%s\n", required[i], result.msl);
            failed = 1;
        }
    }
    if (!failed) failed = write_msl(write_path, &result);
    if (failed) fprintf(stderr, "DXIL wave-match/prefix translation failed: %s\n", result.error);
    dxil_msl_free(&result);

    if (!failed) {
        module.shader_kind = 0;
        result = (dxil_msl){ 0 };
        failed = dxil_to_msl(&module, &result) != 0 ||
            !strstr(result.msl, "simd_ballot(!simd_is_helper_thread())");
        if (failed) fprintf(stderr, "DXIL pixel-wave helpers were not excluded: %s\n", result.error);
        if (!failed) failed = write_msl(pixel_write_path, &result);
        dxil_msl_free(&result);
    }
    return failed;
}

static int translate_pixel_wave_helpers(const char *write_path) {
    enum { VOID_TYPE, INT_TYPE, BOOL_TYPE, UINT4_TYPE, TYPE_COUNT };
    dxil_type types[TYPE_COUNT] = { 0 };
    types[VOID_TYPE].kind = DXT_VOID;
    types[INT_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 32 };
    types[BOOL_TYPE] = (dxil_type){ .kind = DXT_INT, .bits = 1 };
    types[UINT4_TYPE] = (dxil_type){ .kind = DXT_VECTOR, .count = 4, .elem = INT_TYPE };
    dxil_value module_values[1] = { { .kind = DXV_FUNCTION, .name = "dx.op.wave" } };
    dxil_value locals[29] = { 0 };
    const uint32_t opcodes[12] = { 110, 113, 114, 115, 116, 119, 120, 117, 118, 135, 136, 121 };
    for (uint32_t i = 0; i < 12; i++) locals[i] = (dxil_value){
        .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = opcodes[i]
    };
    locals[12] = (dxil_value){ .kind = DXV_CONST_INT, .type = BOOL_TYPE, .u.i = 1 };
    locals[13] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 7 };
    locals[14] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 0 };
    locals[15] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 0 };
    locals[16] = (dxil_value){ .kind = DXV_CONST_INT, .type = INT_TYPE, .u.i = 0 };

    const uint32_t op0[] = { 0, 1 };
    const uint32_t op1[] = { 0, 2, 13 };
    const uint32_t op2[] = { 0, 3, 13 };
    const uint32_t op3[] = { 0, 4, 13 };
    const uint32_t op4[] = { 0, 5, 13 };
    const uint32_t op5[] = { 0, 6, 14, 15, 16 };
    const uint32_t op6[] = { 0, 7, 14, 15 };
    const uint32_t op7[] = { 0, 8, 13, 17 };
    const uint32_t op8[] = { 0, 9, 14 };
    const uint32_t op9[] = { 0, 10, 13 };
    const uint32_t op10[] = { 0, 11, 13 };
    const uint32_t op11[] = { 0, 12, 14, 15, 16 };
    const uint32_t *operands[12] = { op0, op1, op2, op3, op4, op5, op6, op7, op8, op9, op10, op11 };
    const uint32_t counts[12] = { 2, 3, 3, 3, 3, 5, 4, 4, 3, 3, 3, 5 };
    const uint32_t result_types[12] = { BOOL_TYPE, BOOL_TYPE, BOOL_TYPE, BOOL_TYPE, UINT4_TYPE,
                                        INT_TYPE, INT_TYPE, BOOL_TYPE, INT_TYPE, INT_TYPE, INT_TYPE, INT_TYPE };
    dxil_inst insts[13] = { 0 };
    for (uint32_t i = 0; i < 12; i++) {
        uint32_t result = 18 + i;
        insts[i] = (dxil_inst){ .op = DXI_CALL, .type = result_types[i], .result = result,
            .block = 0, .n = counts[i], .ops = (uint32_t *)operands[i] };
        locals[result - 1].kind = DXV_INST;
        locals[result - 1].type = result_types[i];
        locals[result - 1].u.index = i;
    }
    insts[12] = (dxil_inst){ .op = DXI_RET, .type = VOID_TYPE, .result = UINT32_MAX, .block = 0 };
    dxil_function function = { .name = "main", .nblocks = 1, .ninsts = 13, .insts = insts,
        .nlocals = 29, .locals = locals };
    dxil_module module = { .shader_kind = 0, .ntypes = TYPE_COUNT, .types = types,
        .nmodule_values = 1, .values = module_values, .nfunctions = 1, .functions = &function };
    dxil_msl result = { 0 };
    int failed = dxil_to_msl(&module, &result) != 0;
    const char *required[] = {
        "simd_ballot(!simd_is_helper_thread())",
        "simd_ballot((!simd_is_helper_thread()) && bool(true))",
        "simd_sum(vx_wo_value)",
        "simd_prefix_exclusive_sum(vx_wo_value)",
        "simd_and(vx_wb_value)",
        "simd_shuffle(uint(true), ushort(vx_we_first)) != 0u",
        "simd_shuffle(uint(true), ushort(0u)) != 0u",
        "simd_shuffle(7u, ushort(vx_wf_first))",
        "vx_wbc_count += uint((vx_wbc_true >> vx_wbc_i) & 1ul)"
    };
    for (size_t i = 0; !failed && i < sizeof required / sizeof required[0]; i++) {
        if (!strstr(result.msl, required[i])) {
            fprintf(stderr, "DXIL pixel wave MSL missing: %s\nMSL:\n%s\n", required[i], result.msl);
            failed = 1;
        }
    }
    if (!failed) failed = write_msl(write_path, &result);
    if (failed) fprintf(stderr, "DXIL pixel wave helper translation failed: %s\n", result.error);
    dxil_msl_free(&result);

    if (!failed) {
        module.shader_feature_flags = DXIL_FEATURE_WAVE_OPS_INCLUDE_HELPER_LANES;
        result = (dxil_msl){ 0 };
        failed = dxil_to_msl(&module, &result) != 0 || !strstr(result.msl, "simd_ballot(true)") ||
            strstr(result.msl, "simd_is_helper_thread()") != NULL;
        if (failed) fprintf(stderr, "DXIL WaveOpsIncludeHelperLanes flag was not honored: %s\n", result.error);
        dxil_msl_free(&result);
    }
    return failed;
}

int main(int argc, char **argv) {
    int sample_mask_fixture = argc == 3 && !strcmp(argv[1], "--sample-mask");
    int fcmp_ueq_fixture = argc == 3 && !strcmp(argv[1], "--fcmp-ueq");
    int gather_fixture = argc == 3 && !strcmp(argv[1], "--texture-gather");
    int cube_array_min_lod_fixture = argc == 3 && !strcmp(argv[1], "--cube-array-min-lod");
    int depth_cube_compare_fixture = argc == 3 && !strcmp(argv[1], "--depth-cube-compare");
    if (argc != 1 && !sample_mask_fixture && !fcmp_ueq_fixture && !gather_fixture && !cube_array_min_lod_fixture &&
        !depth_cube_compare_fixture && argc != 6 && argc != 7 && argc != 8) {
        fprintf(stderr, "usage: dxil_msl_test [--sample-mask output.metal | --fcmp-ueq output.metal | --texture-gather output.metal | --cube-array-min-lod output.metal | --depth-cube-compare output.metal | signed-min-msl-path float-max-msl-path wave-match-multiprefix-msl-path pixel-wave-msl-path pixel-wave-helper-msl-path [counter-msl-path atomic-fixture-prefix]]\n");
        return 2;
    }
    const char *signed_path = argc == 6 ? argv[1] : NULL;
    const char *float_path = argc == 6 ? argv[2] : NULL;
    const char *counter_path = argc == 7 || argc == 8 ? argv[6] : NULL;
    const char *atomic_prefix = argc == 8 ? argv[7] : NULL;
    static const uint8_t atomic_kinds[] = { 10, 1, 6, 2, 7, 4 };
    static const char *atomic_names[] = { "typed_buffer", "texture_1d", "texture_1d_array",
        "texture_2d", "texture_2d_array", "texture_3d" };
    int failed = translate_mask(0) | translate_mask(1) | bindless_resource_heap_class() |
                 translate_store_only_uav_fixture() |
                 translate_sample_mask_fixture(sample_mask_fixture ? argv[2] : NULL) |
                 translate_buffer_update_counter(counter_path) |
                 translate_unordered_equal(fcmp_ueq_fixture ? argv[2] : NULL) |
                 reject_1d_explicit_sample_options() |
                 translate_cube_array_min_lod(cube_array_min_lod_fixture ? argv[2] : NULL) |
                 translate_depth_cube_comparison(depth_cube_compare_fixture ? argv[2] : NULL) |
                 translate_texture_gathers(gather_fixture ? argv[2] : NULL) |
                 shader_feature_flag_from_container() |
                 translate_wave_prefix(121, 2, 0, signed_path) |
                 translate_wave_prefix(121, 3, 1, float_path) |
                 translate_wave_prefix(136, 0, 0, NULL) |
                 translate_wave_match_multiprefix(argc == 6 ? argv[3] : NULL, argc == 6 ? argv[4] : NULL) |
                 translate_pixel_wave_helpers(argc == 6 ? argv[5] : NULL) |
                 0;
    for (size_t i = 0; i < sizeof atomic_kinds / sizeof atomic_kinds[0]; i++) {
        char path[512];
        const char *write_path = NULL;
        if (atomic_prefix) {
            snprintf(path, sizeof path, "%s_%s.metal", atomic_prefix, atomic_names[i]);
            write_path = path;
        }
        failed |= translate_resource_atomics(atomic_kinds[i], 5, 0, write_path);
    }
    char signedness_path[512];
    const char *signedness_write_path = NULL;
    if (atomic_prefix) {
        snprintf(signedness_path, sizeof signedness_path, "%s_typed_sint_umin.metal", atomic_prefix);
        signedness_write_path = signedness_path;
    }
    failed |= translate_resource_atomics(10, 4, 6, signedness_write_path);
    char mismatch_path[512];
    snprintf(mismatch_path, sizeof mismatch_path, "%s_typed_uint_imin.metal", atomic_prefix ? atomic_prefix : "");
    failed |= translate_resource_atomics(10, 5, 4, atomic_prefix ? mismatch_path : NULL);
    snprintf(mismatch_path, sizeof mismatch_path, "%s_texture_2d_uint_imin.metal", atomic_prefix ? atomic_prefix : "");
    failed |= translate_resource_atomics(2, 5, 4, atomic_prefix ? mismatch_path : NULL);
    snprintf(mismatch_path, sizeof mismatch_path, "%s_texture_2d_sint_umin.metal", atomic_prefix ? atomic_prefix : "");
    failed |= translate_resource_atomics(2, 4, 6, atomic_prefix ? mismatch_path : NULL);
    puts(failed ? "dxil_msl: FAILED" :
         "dxil_msl: unsupported 1D LOD requests rejected; unordered comparisons, shuffles, counters, gathers, wave ops and atomics: ok");
    return failed;
}
