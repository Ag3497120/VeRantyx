#include "dotnet_exec.h"
#include "dotnet_il.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIL_STEP_LIMIT 1000000u

typedef enum { SV_I4, SV_I8, SV_REF } stack_kind;
typedef struct { uint64_t bits; stack_kind kind; } stack_value;

static uint32_t read32(const uint8_t *p) { uint32_t v; memcpy(&v, p, sizeof v); return v; }
static int32_t read_i32(const uint8_t *p) { int32_t v; memcpy(&v, p, sizeof v); return v; }
static int64_t read_i64(const uint8_t *p) { int64_t v; memcpy(&v, p, sizeof v); return v; }

static void fail(char *error, size_t capacity, const char *fmt, ...) {
    if (!error || !capacity) return;
    va_list args; va_start(args, fmt);
    vsnprintf(error, capacity, fmt, args);
    va_end(args);
}

static int compressed_u32(const uint8_t *p, size_t size, uint32_t *value, size_t *used) {
    if (!size) return 0;
    if (!(p[0] & 0x80)) { *value = p[0]; *used = 1; return 1; }
    if ((p[0] & 0xc0) == 0x80) {
        if (size < 2) return 0;
        *value = ((uint32_t)(p[0] & 0x3f) << 8) | p[1]; *used = 2; return 1;
    }
    if ((p[0] & 0xe0) == 0xc0) {
        if (size < 4) return 0;
        *value = ((uint32_t)(p[0] & 0x1f) << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8) | p[3];
        if (*value > 0x1fffffffu) return 0;
        *used = 4; return 1;
    }
    return 0;
}

static int signature_result(const vx_dotnet_metadata *md, const vx_dotnet_method *method,
                            uint8_t *result_type, char *error, size_t capacity) {
    const uint8_t *sig; uint32_t length;
    if (!vx_dotnet_blob(md, method->signature_index, &sig, &length) || length < 3) {
        fail(error, capacity, "MethodDef %08x has a malformed signature blob", method->token); return 0;
    }
    uint8_t callconv = sig[0];
    if ((callconv & 0x0f) != 0 || (callconv & 0x60) || (callconv & 0x10)) {
        fail(error, capacity, "MethodDef %08x uses an unsupported calling convention 0x%02x", method->token, callconv); return 0;
    }
    size_t pos = 1, used; uint32_t parameters;
    if (!compressed_u32(sig + pos, length - pos, &parameters, &used)) {
        fail(error, capacity, "MethodDef %08x has a malformed parameter count", method->token); return 0;
    }
    pos += used;
    if (parameters || pos >= length) {
        fail(error, capacity, "MethodDef %08x must have a zero-argument signature", method->token); return 0;
    }
    *result_type = sig[pos];
    if (*result_type != 0x01 && *result_type != 0x02 && *result_type != 0x03 &&
        *result_type != 0x04 && *result_type != 0x05 &&
        *result_type != 0x06 && *result_type != 0x07 && *result_type != 0x08 &&
        *result_type != 0x09 && *result_type != 0x0a && *result_type != 0x0b &&
        *result_type != 0x18 && *result_type != 0x19) {
        fail(error, capacity, "MethodDef %08x returns unsupported CLI element type 0x%02x", method->token, *result_type);
        return 0;
    }
    return 1;
}

static int pop(stack_value *stack, uint32_t *depth, stack_value *value) {
    if (!*depth) return 0;
    *value = stack[--*depth];
    return 1;
}

static int push(stack_value *stack, uint32_t *depth, uint32_t limit, uint64_t bits, stack_kind kind) {
    if (*depth >= limit) return 0;
    stack[(*depth)++] = (stack_value){ .bits = bits, .kind = kind };
    return 1;
}

static int branch_if(uint16_t op, stack_value a, stack_value b, int *result) {
    if ((op == 0x002c || op == 0x002d || op == 0x0039 || op == 0x003a)) {
        *result = (op == 0x002c || op == 0x0039) ? a.bits == 0 : a.bits != 0;
        return 1;
    }
    if (a.kind != b.kind || (a.kind != SV_I4 && a.kind != SV_I8 && op != 0x002e && op != 0x003b &&
                              op != 0x0033 && op != 0x0040)) return 0;
    uint64_t ua = a.kind == SV_I4 ? (uint32_t)a.bits : a.bits;
    uint64_t ub = b.kind == SV_I4 ? (uint32_t)b.bits : b.bits;
    int64_t sa = a.kind == SV_I4 ? (int32_t)a.bits : (int64_t)a.bits;
    int64_t sb = b.kind == SV_I4 ? (int32_t)b.bits : (int64_t)b.bits;
    switch (op) {
        case 0x002e: case 0x003b: *result = ua == ub; return 1;            // beq
        case 0x002f: case 0x003c: *result = sa >= sb; return 1;
        case 0x0030: case 0x003d: *result = sa > sb; return 1;
        case 0x0031: case 0x003e: *result = sa <= sb; return 1;
        case 0x0032: case 0x003f: *result = sa < sb; return 1;
        case 0x0033: case 0x0040: *result = ua != ub; return 1;         // bne.un
        case 0x0034: case 0x0041: *result = ua >= ub; return 1;         // bge.un
        case 0x0035: case 0x0042: *result = ua > ub; return 1;          // bgt.un
        case 0x0036: case 0x0043: *result = ua <= ub; return 1;         // ble.un
        case 0x0037: case 0x0044: *result = ua < ub; return 1;          // blt.un
        default: return 0;
    }
}

static int binary_integer(uint16_t op, stack_value a, stack_value b, stack_value *out) {
    if (a.kind != b.kind || (a.kind != SV_I4 && a.kind != SV_I8)) return 0;
    uint32_t is32 = a.kind == SV_I4;
    uint64_t ua = is32 ? (uint32_t)a.bits : a.bits;
    uint64_t ub = is32 ? (uint32_t)b.bits : b.bits;
    int64_t sa = is32 ? (int32_t)a.bits : (int64_t)a.bits;
    int64_t sb = is32 ? (int32_t)b.bits : (int64_t)b.bits;
    uint32_t shift_mask = is32 ? 31 : 63;
    uint64_t value;
    switch (op) {
        case 0x0058: value = ua + ub; break;
        case 0x0059: value = ua - ub; break;
        case 0x005a: value = ua * ub; break;
        case 0x005b:
            if (!ub || (is32 ? ((int32_t)ua == INT32_MIN && (int32_t)ub == -1) : (sa == INT64_MIN && sb == -1))) return 0;
            value = (uint64_t)(sa / sb); break;
        case 0x005c: if (!ub) return 0; value = ua / ub; break;
        case 0x005d:
            if (!ub || (is32 ? ((int32_t)ua == INT32_MIN && (int32_t)ub == -1) : (sa == INT64_MIN && sb == -1))) return 0;
            value = (uint64_t)(sa % sb); break;
        case 0x005e: if (!ub) return 0; value = ua % ub; break;
        case 0x005f: value = ua & ub; break;
        case 0x0060: value = ua | ub; break;
        case 0x0061: value = ua ^ ub; break;
        case 0x0062: value = ua << (ub & shift_mask); break;
        case 0x0063: value = (uint64_t)(sa >> (ub & shift_mask)); break;
        case 0x0064: value = ua >> (ub & shift_mask); break;
        default: return 0;
    }
    out->kind = a.kind;
    out->bits = is32 ? (uint32_t)value : value;
    return 1;
}

int vx_dotnet_execute_static(const vx_dotnet_metadata *md, uint32_t method_token,
                             int64_t *return_value, char *error, size_t error_capacity) {
    if (error && error_capacity) error[0] = 0;
    if (!md || !return_value) { fail(error, error_capacity, "invalid CIL execution arguments"); return 0; }
    vx_dotnet_method method;
    if (!vx_dotnet_method_get(md, method_token, &method)) {
        fail(error, error_capacity, "entry token %08x is not a MethodDef in this image", method_token); return 0;
    }
    if (!(method.flags & 0x0010)) {
        fail(error, error_capacity, "entry method %s is not static", method.name); return 0;
    }
    uint8_t result_type;
    if (!signature_result(md, &method, &result_type, error, error_capacity)) return 0;
    vx_dotnet_method_body body;
    if (!vx_dotnet_method_body_get(md, &method, &body)) {
        fail(error, error_capacity, "method %s has no supported CIL method body", method.name); return 0;
    }
    if (body.local_signature_token || body.has_more_sections) {
        fail(error, error_capacity, "method %s uses local-signature or extra-section metadata not implemented yet", method.name);
        return 0;
    }

    vx_dotnet_instruction *instructions = calloc(body.code_size ? body.code_size : 1, sizeof *instructions);
    uint32_t *offset_map = malloc(((size_t)body.code_size + 1) * sizeof *offset_map);
    stack_value *stack = calloc(body.max_stack ? body.max_stack : 1, sizeof *stack);
    if (!instructions || !offset_map || !stack) {
        free(instructions); free(offset_map); free(stack);
        fail(error, error_capacity, "cannot allocate CIL frame for %s", method.name); return 0;
    }
    for (uint32_t i = 0; i <= body.code_size; i++) offset_map[i] = UINT32_MAX;
    uint32_t cursor = 0, count = 0;
    while (cursor < body.code_size) {
        vx_dotnet_instruction ins;
        int decoded = vx_dotnet_il_next(body.code, body.code_size, &cursor, &ins);
        if (decoded != 1 || count >= body.code_size) {
            fail(error, error_capacity, "invalid CIL instruction stream in %s at IL_%04x", method.name, cursor);
            free(instructions); free(offset_map); free(stack); return 0;
        }
        offset_map[ins.offset] = count;
        instructions[count++] = ins;
    }
    offset_map[body.code_size] = count;

    uint32_t pc = 0, depth = 0, steps = 0;
    int ok = 0;
    while (pc < count && steps++ < CIL_STEP_LIMIT) {
        vx_dotnet_instruction *ins = &instructions[pc];
        uint16_t op = ins->opcode;
        uint32_t next_pc = pc + 1;
        stack_value a = { 0 }, b = { 0 }, result = { 0 };
        if (op == 0x0000) { /* nop */ }
        else if (op == 0x0014) { /* ldnull */
            if (!push(stack, &depth, body.max_stack, 0, SV_REF)) goto stack_overflow;
        } else if (op == 0x0015 || (op >= 0x0016 && op <= 0x001e)) {
            int64_t v = op == 0x0015 ? -1 : (int64_t)(op - 0x0016);
            if (!push(stack, &depth, body.max_stack, (uint32_t)v, SV_I4)) goto stack_overflow;
        } else if (op == 0x001f) {
            int8_t v; memcpy(&v, ins->operand, 1);
            if (!push(stack, &depth, body.max_stack, (uint32_t)(int32_t)v, SV_I4)) goto stack_overflow;
        } else if (op == 0x0020) {
            int32_t v = read_i32(ins->operand);
            if (!push(stack, &depth, body.max_stack, (uint32_t)v, SV_I4)) goto stack_overflow;
        } else if (op == 0x0021) {
            int64_t v = read_i64(ins->operand);
            if (!push(stack, &depth, body.max_stack, (uint64_t)v, SV_I8)) goto stack_overflow;
        } else if (op == 0x0025) {
            if (!depth) goto stack_underflow;
            if (!push(stack, &depth, body.max_stack, stack[depth - 1].bits, stack[depth - 1].kind)) goto stack_overflow;
        } else if (op == 0x0026) {
            if (!pop(stack, &depth, &a)) goto stack_underflow;
        } else if (op == 0x002b || op == 0x0038 || op == 0x00de || op == 0x00dd) {
            if (!vx_dotnet_il_branch_target(ins, 0, body.code_size, &cursor) || offset_map[cursor] == UINT32_MAX)
                goto invalid_branch;
            if (op == 0x00de || op == 0x00dd) depth = 0;
            next_pc = offset_map[cursor];
        } else if ((op >= 0x002c && op <= 0x0037) || (op >= 0x0039 && op <= 0x0044)) {
            int take;
            if (op == 0x002c || op == 0x002d || op == 0x0039 || op == 0x003a) {
                if (!pop(stack, &depth, &a)) goto stack_underflow;
                if (!branch_if(op, a, (stack_value){ 0 }, &take)) goto unsupported;
            } else {
                if (!pop(stack, &depth, &b) || !pop(stack, &depth, &a)) goto stack_underflow;
                if (!branch_if(op, a, b, &take)) goto unsupported;
            }
            if (take) {
                if (!vx_dotnet_il_branch_target(ins, 0, body.code_size, &cursor) || offset_map[cursor] == UINT32_MAX)
                    goto invalid_branch;
                next_pc = offset_map[cursor];
            }
        } else if (op == 0x0045) {
            if (!pop(stack, &depth, &a)) goto stack_underflow;
            uint32_t arms = read32(ins->operand);
            if (a.kind != SV_I4) goto unsupported;
            uint32_t arm = (uint32_t)a.bits;
            if (arm < arms) {
                if (!vx_dotnet_il_branch_target(ins, arm, body.code_size, &cursor) || offset_map[cursor] == UINT32_MAX)
                    goto invalid_branch;
                next_pc = offset_map[cursor];
            }
        } else if (op == 0x0058 || (op >= 0x005b && op <= 0x0064)) {
            if (!pop(stack, &depth, &b) || !pop(stack, &depth, &a)) goto stack_underflow;
            if (!binary_integer(op, a, b, &result)) {
                fail(error, error_capacity, "integer operation %#x failed (division by zero or overflow) in %s at IL_%04x",
                     op, method.name, ins->offset); goto done;
            }
            if (!push(stack, &depth, body.max_stack, result.bits, result.kind)) goto stack_overflow;
        } else if (op == 0x0059 || op == 0x005a || op == 0x005f || op == 0x0060 || op == 0x0061) {
            if (!pop(stack, &depth, &b) || !pop(stack, &depth, &a)) goto stack_underflow;
            if (!binary_integer(op, a, b, &result)) goto unsupported;
            if (!push(stack, &depth, body.max_stack, result.bits, result.kind)) goto stack_overflow;
        } else if (op == 0x0065 || op == 0x0066) {
            if (!pop(stack, &depth, &a)) goto stack_underflow;
            if (a.kind != SV_I4 && a.kind != SV_I8) goto unsupported;
            result.kind = a.kind;
            result.bits = op == 0x0065 ? (uint64_t)(0 - a.bits) : ~a.bits;
            if (result.kind == SV_I4) result.bits = (uint32_t)result.bits;
            if (!push(stack, &depth, body.max_stack, result.bits, result.kind)) goto stack_overflow;
        } else if (op == 0xfe01 || op == 0xfe02 || op == 0xfe03 || op == 0xfe04 || op == 0xfe05) {
            if (!pop(stack, &depth, &b) || !pop(stack, &depth, &a)) goto stack_underflow;
            int comparison = 0;
            if (a.kind != b.kind) goto unsupported;
            if (op == 0xfe01) comparison = a.bits == b.bits;
            else if (a.kind != SV_I4 && a.kind != SV_I8) goto unsupported;
            else if (op == 0xfe02) comparison = (a.kind == SV_I4 ? (int32_t)a.bits : (int64_t)a.bits) >
                                                       (b.kind == SV_I4 ? (int32_t)b.bits : (int64_t)b.bits);
            else if (op == 0xfe03) comparison = (a.kind == SV_I4 ? (uint32_t)a.bits : a.bits) >
                                                       (b.kind == SV_I4 ? (uint32_t)b.bits : b.bits);
            else if (op == 0xfe04) comparison = (a.kind == SV_I4 ? (int32_t)a.bits : (int64_t)a.bits) <
                                                       (b.kind == SV_I4 ? (int32_t)b.bits : (int64_t)b.bits);
            else if (op == 0xfe05) comparison = (a.kind == SV_I4 ? (uint32_t)a.bits : a.bits) <
                                                       (b.kind == SV_I4 ? (uint32_t)b.bits : b.bits);
            if (!push(stack, &depth, body.max_stack, (uint32_t)comparison, SV_I4)) goto stack_overflow;
        } else if (op == 0x002a) {
            if (result_type == 0x01) {
                if (depth) goto invalid_return;
                *return_value = 0;
            } else {
                if (depth != 1 || !pop(stack, &depth, &a)) goto invalid_return;
                if (a.kind != SV_I4 && a.kind != SV_I8) goto invalid_return;
                if (result_type == 0x02 || result_type == 0x04 || result_type == 0x06 || result_type == 0x08)
                    *return_value = (int64_t)(int32_t)a.bits;
                else if (result_type == 0x03 || result_type == 0x05 || result_type == 0x07 || result_type == 0x09)
                    *return_value = (int64_t)(uint32_t)a.bits;
                else *return_value = (int64_t)a.bits;
            }
            ok = 1; goto done;
        } else {
unsupported:
            fail(error, error_capacity, "unsupported CIL opcode %#x in %s at IL_%04x", op, method.name, ins->offset);
            goto done;
        }
        pc = next_pc;
    }
    if (steps >= CIL_STEP_LIMIT) fail(error, error_capacity, "CIL instruction limit reached in %s", method.name);
    else fail(error, error_capacity, "CIL method %s fell off its end without ret", method.name);
    goto done;

stack_overflow:
    fail(error, error_capacity, "CIL stack exceeds maxstack in %s at IL_%04x", method.name, instructions[pc].offset); goto done;
stack_underflow:
    fail(error, error_capacity, "CIL stack underflow in %s at IL_%04x", method.name, instructions[pc].offset); goto done;
invalid_branch:
    fail(error, error_capacity, "CIL branch target is not an instruction boundary in %s at IL_%04x", method.name, instructions[pc].offset); goto done;
invalid_return:
    fail(error, error_capacity, "CIL return stack does not match method signature in %s at IL_%04x", method.name, instructions[pc].offset); goto done;
done:
    free(instructions); free(offset_map); free(stack);
    return ok;
}
