#include "dotnet_il.h"
#include <string.h>

static uint32_t read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static int32_t read_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static int range_ok(uint32_t size, uint32_t offset, uint32_t length) {
    return offset <= size && length <= size - offset;
}

static int opcode_operand(uint16_t op, vx_il_operand_kind *kind, uint32_t *size) {
    *kind = VX_IL_OPERAND_NONE; *size = 0;
    if (op <= 0x000d || (op >= 0x0014 && op <= 0x001e) ||
        op == 0x0025 || op == 0x0026 || op == 0x002a ||
        (op >= 0x0046 && op <= 0x006e) || op == 0x0076 || op == 0x007a ||
        (op >= 0x0082 && op <= 0x008b) || op == 0x008e ||
        (op >= 0x0090 && op <= 0x00a2) ||
        (op >= 0x00b3 && op <= 0x00ba) || op == 0x00c3 ||
        (op >= 0x00d1 && op <= 0x00dc) || (op >= 0x00df && op <= 0x00e0)) return 1;
    if ((op >= 0x000e && op <= 0x0013) || op == 0x001f ||
        (op >= 0x002b && op <= 0x0037) || op == 0x00de) {
        *kind = (op >= 0x002b && op <= 0x0037) || op == 0x00de
            ? VX_IL_OPERAND_BRANCH1 : VX_IL_OPERAND_I1;
        *size = 1; return 1;
    }
    if (op == 0x0020 || (op >= 0x0038 && op <= 0x0044) || op == 0x00dd) {
        *kind = (op >= 0x0038 && op <= 0x0044) || op == 0x00dd
            ? VX_IL_OPERAND_BRANCH4 : VX_IL_OPERAND_I4;
        *size = 4; return 1;
    }
    if (op == 0x0021) { *kind = VX_IL_OPERAND_I8; *size = 8; return 1; }
    if (op == 0x0022) { *kind = VX_IL_OPERAND_R4; *size = 4; return 1; }
    if (op == 0x0023) { *kind = VX_IL_OPERAND_R8; *size = 8; return 1; }
    if (op == 0x0027 || op == 0x0028 || op == 0x0029 || op == 0x006f ||
        (op >= 0x0070 && op <= 0x0075) || op == 0x0077 ||
        (op >= 0x007b && op <= 0x0081) || (op >= 0x008c && op <= 0x008d) ||
        op == 0x008f || op == 0x00a3 || op == 0x00a4 || op == 0x00a5 ||
        op == 0x00c2 || op == 0x00c6 || op == 0x00d0) {
        *kind = VX_IL_OPERAND_TOKEN;
        *size = 4; return 1;
    }
    if (op == 0x0045) { *kind = VX_IL_OPERAND_SWITCH; return 1; }
    if (op >= 0xfe00 && op <= 0xfe1e) {
        uint8_t sub = (uint8_t)op;
        if (sub <= 0x05 || sub == 0x0f || sub == 0x11 ||
            (sub >= 0x13 && sub <= 0x14) || (sub >= 0x17 && sub <= 0x18) ||
            sub == 0x1a || (sub >= 0x1d && sub <= 0x1e)) return 1;
        if (sub >= 0x09 && sub <= 0x0e) { *kind = VX_IL_OPERAND_I2; *size = 2; return 1; }
        if (sub == 0x12 || sub == 0x19) { *kind = VX_IL_OPERAND_I1; *size = 1; return 1; }
        if (sub == 0x06 || sub == 0x07 || sub == 0x15 || sub == 0x16 || sub == 0x1c) {
            *kind = VX_IL_OPERAND_TOKEN; *size = 4; return 1;
        }
    }
    return 0;
}

int vx_dotnet_il_next(const uint8_t *code, uint32_t code_size, uint32_t *cursor,
                      vx_dotnet_instruction *out) {
    if (!code || !cursor || !out || *cursor > code_size) return -1;
    if (*cursor == code_size) return 0;
    uint32_t at = *cursor;
    uint16_t opcode = code[at++];
    if (opcode == 0xfe) {
        if (!range_ok(code_size, at, 1)) return -1;
        opcode = (uint16_t)(0xfe00 | code[at++]);
    }
    vx_il_operand_kind kind;
    uint32_t operand_size;
    if (!opcode_operand(opcode, &kind, &operand_size)) return -1;
    if (kind == VX_IL_OPERAND_SWITCH) {
        if (!range_ok(code_size, at, 4)) return -1;
        uint32_t count = read32(code + at);
        if (count > (code_size - at - 4) / 4) return -1;
        operand_size = 4 + count * 4;
    }
    if (!range_ok(code_size, at, operand_size)) return -1;
    *out = (vx_dotnet_instruction){ .offset = *cursor, .next_offset = at + operand_size,
        .opcode = opcode, .operand_kind = kind, .operand = code + at, .operand_size = operand_size };
    *cursor = out->next_offset;
    return 1;
}

int vx_dotnet_il_branch_target(const vx_dotnet_instruction *ins,
                               uint32_t arm, uint32_t code_size, uint32_t *target) {
    if (!ins || !target || !ins->operand) return 0;
    int64_t relative;
    if (ins->operand_kind == VX_IL_OPERAND_BRANCH1) {
        if (ins->operand_size != 1) return 0;
        int8_t v; memcpy(&v, ins->operand, sizeof v); relative = v;
    } else if (ins->operand_kind == VX_IL_OPERAND_BRANCH4) {
        if (ins->operand_size != 4) return 0;
        relative = read_i32(ins->operand);
    } else if (ins->operand_kind == VX_IL_OPERAND_SWITCH) {
        if (ins->operand_size < 4) return 0;
        uint32_t count = read32(ins->operand);
        if (arm >= count || ins->operand_size != 4 + count * 4) return 0;
        relative = read_i32(ins->operand + 4 + arm * 4);
    } else return 0;
    int64_t destination = (int64_t)ins->next_offset + relative;
    if (destination < 0 || destination >= code_size) return 0;
    *target = (uint32_t)destination;
    return 1;
}
