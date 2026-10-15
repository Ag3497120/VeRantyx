// x86-64 -> ARM64 translation of one instruction (phase 2: general-purpose integer instructions). See TRANSLATOR.md.
//
// Register convention of translated code:
//   x0-x15   the x86 registers RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8-R15 (in x86's encoding order)
//   x27      EFLAGS as x86 has it (CF bit 0, PF 2, AF 4, ZF 6, SF 7, DF 10, OF 11)
//   x26      the parity table (256 bytes, 1 where a byte has an even number of set bits)
//   x28      the thread's x86_ctx (helpers, FS/GS bases, faults)
//   x16-x17, x19-x24  scratch
// Flags are computed in full after every instruction that sets them (correct first; flag liveness comes later).
#pragma once
#include "x86dec.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t gpr[16];
    uint64_t flags;
    uint64_t rip;
    uint64_t fs_base, gs_base;
    uint64_t fault;              // 0, or the x86 exception the instruction raised (X86_FAULT_*)
    uint64_t helper_op;          // for helpers (xlat_helpers.c): what to do
    uint64_t helper_val;         //   and the operand they need
} x86_ctx;
enum { CTX_FLAGS = 128, CTX_RIP = 136, CTX_FS = 144, CTX_GS = 152, CTX_FAULT = 160, CTX_HELPER_OP = 168, CTX_HELPER_VAL = 176 };
enum { X86_FAULT_DE = 1 };

typedef struct {
    uint32_t *code;
    size_t n, cap;
    int unsupported;             // set when an instruction cannot be translated (yet)
} xlat_buf;

extern const uint8_t xlat_parity[256];

// Translates the instruction d (decoded from code at rip). Returns 0, or -1 if it is not supported (yet).
int xlat_insn(xlat_buf *b, const x86_insn *d, const uint8_t *code, uint64_t rip);
// Helpers translated code calls for rare or intricate instructions (one-operand MUL/IMUL/DIV/IDIV, RCL/RCR).
void xlat_helper(x86_ctx *ctx);
// A test harness's entry and exit: loads x86_ctx (pointer in x0) into the registers above, and stores it back.
void xlat_prologue(xlat_buf *b);
void xlat_epilogue(xlat_buf *b);
