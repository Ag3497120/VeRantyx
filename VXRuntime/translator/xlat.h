// x86-64 -> ARM64 translation of one instruction (phase 2: general-purpose integer instructions). See TRANSLATOR.md.
//
// Register convention of translated code:
//   x0-x15   the x86 registers RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8-R15 (in x86's encoding order)
//   x27      EFLAGS as x86 has it (CF bit 0, PF 2, AF 4, ZF 6, SF 7, DF 10, OF 11)
//   x26      the parity table (256 bytes, 1 where a byte has an even number of set bits)
//   x28      the thread's x86_ctx (helpers, FS/GS bases, faults)
//   x16-x17, x19-x24  scratch
//   v0-v15   XMM0-XMM15; v16-v23 scratch (v31 too)
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
    uint64_t pad;
    uint8_t xmm[16][16];         // XMM0-15 (in V0-V15 while translated code runs)
    uint8_t ymm_hi[16][16];      // the upper halves of YMM0-15 (AVX, phase 3b)
    uint32_t mxcsr;
    uint32_t pad2;
} x86_ctx;
enum { CTX_FLAGS = 128, CTX_RIP = 136, CTX_FS = 144, CTX_GS = 152, CTX_FAULT = 160, CTX_HELPER_OP = 168, CTX_HELPER_VAL = 176,
       CTX_XMM = 192, CTX_YMMH = 448, CTX_MXCSR = 704 };
enum { X86_FAULT_DE = 1 };
// Registers of translated code that hold more than scratch values: x25 is the string instructions' step.

typedef struct {
    uint32_t *code;
    size_t n, cap;
    int unsupported;             // set when an instruction cannot be translated (yet)
    // LOCK on a read-modify-write of memory: the store becomes a compare-and-swap that starts the instruction over
    // (from lock_head) if memory changed since it was read; lock_expect holds the value read.
    int lock;
    size_t lock_head;
    int lock_expect;
} xlat_buf;

extern const uint8_t xlat_parity[256];

// Translates the instruction d (decoded from code at rip). Returns 0, or -1 if it is not supported (yet).
int xlat_insn(xlat_buf *b, const x86_insn *d, const uint8_t *code, uint64_t rip);
// Translates the basic block at rip (its x86 code read from memory at that address) into a function
// void block(x86_ctx *): it runs the block and leaves the next instruction's address in ctx->rip (and a fault, if
// one happened, in ctx->fault with ctx->rip at the faulting instruction). Stops after a control transfer or max_insns.
// Returns 0, or -1 if an instruction is not supported (yet).
int xlat_block(xlat_buf *b, uint64_t rip, int max_insns);
enum { X86_FAULT_UD = 3 };
// Helpers translated code calls for rare or intricate instructions (one-operand MUL/IMUL/DIV/IDIV, RCL/RCR).
void xlat_helper(x86_ctx *ctx);
// A test harness's entry and exit: loads x86_ctx (pointer in x0) into the registers above, and stores it back.
void xlat_prologue(xlat_buf *b);
void xlat_epilogue(xlat_buf *b);
