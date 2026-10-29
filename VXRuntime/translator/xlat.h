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

typedef struct x86_ctx_s {
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
    uint16_t fcw, fsw;           // x87 control and status words (TOP: FSW bits 11-13)
    uint8_t ftag;                // x87: bit r set while physical register r holds a value (FXSAVE's abridged tags)
    uint8_t pad3[3];
    uint8_t st[8][16];           // x87 physical registers R0-R7: 64-bit significand, then sign and exponent (x87.c)
    uint64_t link;               // engine.c: the branch that left translated code for a block not yet linked (0: none)
    uint64_t entry;              // engine.c: where translated code is entered
    uint64_t fault_info;         // with fault: the INT n vector, or RCX for __fastfail (INT 29h)
    uint64_t ras;                // engine.c: the return-address ring's next entry (in X29 while translated code runs)
    uint64_t jump;               // set by a native function called from translated code (xlat_block_native): go on at
                                 //   rip (or stop at fault) instead of returning to the caller
    uint64_t stale;              // set by a checked block (xlat_buf.check_bytes) whose code changed: translate it again
    uint64_t single;             // engine.c: run the instruction at rip alone, translated just for this once (it writes
                                 //   code that comes after it: what follows is translated after the write)
    uint64_t batch_base, batch_ptr, batch_end;   // vcpu.c: calls recorded in the guest, run at its next exit
    uint64_t pend_a, pend_b, pend_r;   // flags not computed yet (x27 bit 3, TRANSLATOR.md "Rules"): the operands and the
    uint64_t pend_info;                //   result of the instruction that set them, its kind * 4 + log2(size)
} x86_ctx;
enum { CTX_FLAGS = 128, CTX_RIP = 136, CTX_FS = 144, CTX_GS = 152, CTX_FAULT = 160, CTX_HELPER_OP = 168, CTX_HELPER_VAL = 176,
       CTX_XMM = 192, CTX_YMMH = 448, CTX_MXCSR = 704, CTX_FCW = 712,
       CTX_LINK = 848, CTX_ENTRY = 856, CTX_FAULT_INFO = 864, CTX_RAS = 872, CTX_JUMP = 880, CTX_STALE = 888, CTX_SINGLE = 896, CTX_PEND = 928 };
_Static_assert(__builtin_offsetof(x86_ctx, pend_a) == CTX_PEND, "CTX_PEND");
// Flags pending (x27 bit 3, reserved in EFLAGS): the six arithmetic flags in EFLAGS form, from the record (any other x27
// bits as they are).
uint64_t xlat_flags_resolve(uint64_t x27, const x86_ctx *c);
// The return-address ring (xlat_block_linked, engine.c): 2^XLAT_RAS_BITS bytes aligned to that, entries of 16 bytes: the x86
// return address and the translated code to go back to. CALL adds one, RET takes one and returns there when it matches.
enum { XLAT_RAS_BITS = 12 };
// x86_ctx.fault: 0, or an x86 exception (vector + 1), with ctx->rip at the instruction
enum { X86_FAULT_DE = 1, X86_FAULT_BP = 4, X86_FAULT_UD = 7, X86_FAULT_GP = 14, X86_FAULT_PF = 15 /* fault_info: address | write << 63 */,
       X86_FAULT_INT = 0x100 /* INT n: fault_info */ };
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
    int ordered;                 // memory accesses that may be shared: LDAPR / STLR (in-process, no TSO)
    uint32_t dead_flags;         // this instruction's flags (EFLAGS bits) that are written again before any read: not computed
    uint64_t stop;               // xlat_block_linked: where the engine returns to C (the flags are live there)
    int fuse_kind, fuse_size, fuse_in_reg, keep_operands;
    int fuse_in_reg_zero;            // (X_COND holds a value whose zero means "taken", not the condition itself)
    int fp_fuse;                     // (U)COMIS[SD] with a JA/JAE/JB/JBE/JP/JNP right after it (its cc + 1): FCMP's NZCV only
    int check_bytes;
    int lazy_flags;              // xlat_block_linked: the engine has a pending-flags stub (XR_FLAGS sites call it)
    int lazy_record;             //   this instruction's flags are recorded, not computed (flags_arith)
    int lazy_done;               //   (flags_arith did record them)
    int aot;                     // made ahead of time (translator/aot.c): host addresses as XR_ABS sites, not immediates
    int vcpu;                    // the code runs in a Hypervisor.framework guest (vcpu.c): calls of host code are HVC #2
                                 //   (xlat_helper) and #3 (a native function: its thunk in S0)             // xlat_block_linked: compare the block's x86 bytes first (code on a page written often)
    // xlat_block_linked: rarely taken code (the NaN fixes) goes after the block, out of the straight path
    int defer_cold;
    struct cold { size_t at; int kind; uint32_t *words; size_t n; } *cold;
    size_t ncold, ccold;
    uint16_t zhi[2];
    // xlat_block_linked: where each x86 instruction's code starts (for faults: the host pc's x86 RIP)
    struct { uint32_t at; int32_t sp; uint64_t rip; } *imap;   // (sp: RSP is x4 + sp there, b->sp_delta)
    size_t nimap, cimap;             // XMM registers whose bits above 32 [0] / 64 [1] are known zero (xlat_block_linked)
    int sp_delta;                // xlat_block_linked: RSP = x4 + sp_delta (pushes and pops not applied to x4 yet)
    int nfused, nskipped;        // (counts: Jccs fused, instructions whose flags were not computed)    // the Jcc after a CMP / TEST / SUB / ... whose flags are dead after it: its condition
                                 // from the operands left in the scratch registers (1: subtraction, 2: logic; 0: none)
    struct { uint32_t at, kind; } *relocs;   // xlat_block_linked's sites
    size_t nrelocs, crelocs;
} xlat_buf;

extern const uint8_t xlat_parity[256];

// Translates the instruction d (decoded from code at rip). Returns 0, or -1 if it is not supported (yet).
int xlat_insn(xlat_buf *b, const x86_insn *d, const uint8_t *code, uint64_t rip);
// Translates the basic block at rip (its x86 code read from memory at that address) into a function
// void block(x86_ctx *): it runs the block and leaves the next instruction's address in ctx->rip (and a fault, if
// one happened, in ctx->fault with ctx->rip at the faulting instruction). Stops after a control transfer or max_insns.
// Returns 0, or -1 if an instruction is not supported (yet).
int xlat_block(xlat_buf *b, uint64_t rip, int max_insns);
// The same for the engine (engine.c): no prologue or epilogue (registers stay in the x86 convention between blocks);
// a block leaves through sites the engine links or patches, listed in b->relocs. *end gets the x86 address after the
// block's last instruction. Returns 0, or -1 if an instruction is not supported (yet).
enum { XR_LINK = 1, XR_LOOKUP, XR_EXIT,        // sites: BL link stub (then the target as 8 bytes), B lookup stub, B exit stub,
       XR_CALL, XR_LOOKUP_CALL,                 //   BL call-link stub (the return's site, then the target), BL lookup stub,
       XR_FLAGS,                                //   BL the pending-flags stub (xlat_flags_stub),
       XR_ABS };                                //   (b->aot only) a host address: 4 words, the first XA_ABS_WORD(which, reg)
// Host addresses the code holds (b->aot: the static recompiler, translator/aot.c, puts loads from its module's table
// there): which one, and the register it goes to.
enum { XA_HELPER = 0, XA_SPLIT_LOCK, XA_PARITY, XA_NABS };
#define XA_ABS_WORD(which, reg) ((uint32_t)(which) << 5 | (uint32_t)(reg))
uint64_t *xlat_split_lock_addr(void);
// The pending-flags stub (engine.c makes one): computes the flags a record describes into x27 (x16, x17 kept).
void xlat_flags_stub(xlat_buf *b);
int xlat_block_linked(xlat_buf *b, uint64_t rip, int max_insns, uint64_t *end);
// The "block" at a native function's address (engine.c, the ARM64 runtime): the x86 state into the context, fn(ctx)
// (a thunk: it reads the call's arguments there and writes the result), the state back, then RET, or on at
// ctx->rip when the function set ctx->jump.
void xlat_block_native(xlat_buf *b, void (*fn)(x86_ctx *));
// The same in the vCPU, first guest_fn(ctx, desc) in the guest (guest.c: an answer, or the call recorded); only
// when that returns 1 does the call leave the guest.
void xlat_block_native_defer(xlat_buf *b, void (*fn)(x86_ctx *), uint64_t desc, uint64_t guest_defer);
// The guest's entry and exit (vcpu.c): x0 = the context on entry, then on at ctx->entry; the exit (S1: the next
// RIP) stores the state and leaves with HVC #1.
void xlat_guest_enter(xlat_buf *b);
extern const uint8_t *xlat_guest_parity;
void xlat_guest_exit(xlat_buf *b);
enum { XV_HVC_EXIT = 1, XV_HVC_HELPER, XV_HVC_NATIVE, XV_HVC_TLBI, XV_HVC_VECTOR = 0x10 };
// Helpers translated code calls for rare or intricate instructions (one-operand MUL/IMUL/DIV/IDIV, RCL/RCR).
void xlat_helper(x86_ctx *ctx);
// A test harness's entry and exit: loads x86_ctx (pointer in x0) into the registers above, and stores it back.
void xlat_prologue(xlat_buf *b);
void xlat_epilogue(xlat_buf *b);
