// x86-64 -> ARM64: general-purpose integer instructions (phase 2). See xlat.h for the register convention.
#include "xlat.h"
#include "a64.h"
#include "x87.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_A = 19, T_B = 20, T_R = 21, T_T = 22, ACC = 23, T_ADDR = 24, S0 = 16, S1 = 17, XP = 26, XF = 27, XC = 28 };
enum { F_CF = 1, F_PF = 4, F_AF = 16, F_ZF = 64, F_SF = 128, F_DF = 1024, F_OF = 2048, F_ARITH = 0x8D5 };

const uint8_t xlat_parity[256] = {
#define P2(n) n, n ^ 1, n ^ 1, n
#define P4(n) P2(n), P2(n ^ 1), P2(n ^ 1), P2(n)
#define P6(n) P4(n), P4(n ^ 1), P4(n ^ 1), P4(n)
    P6(1), P6(0), P6(0), P6(1)
#undef P2
#undef P4
#undef P6
};

static void E(xlat_buf *b, uint32_t insn) {
    if (b->n == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 256;
        b->code = realloc(b->code, b->cap * sizeof *b->code);
    }
    b->code[b->n++] = insn;
}
static size_t here(xlat_buf *b) { return b->n; }
static void patch_cbz(xlat_buf *b, size_t at, int nonzero, int reg) {     // a forward CBZ/CBNZ at `at` to here
    int32_t words = (int32_t)(b->n - at);
    b->code[at] = nonzero ? a64_cbnz(reg, words) : a64_cbz(reg, words);
}
static void patch_b(xlat_buf *b, size_t at) { b->code[at] = a64_b((int32_t)(b->n - at)); }

static void mov_imm(xlat_buf *b, int d, uint64_t v) {
    int first = 1;
    for (int sh = 0; sh < 64; sh += 16) {
        uint16_t part = (uint16_t)(v >> sh);
        if (!part) continue;
        E(b, first ? a64_movz(d, part, sh) : a64_movk(d, part, sh));
        first = 0;
    }
    if (first) E(b, a64_movz(d, 0, 0));
}

// ---- operands ---------------------------------------------------------------------------------------------------------------
static int bits(int size) { return size * 8; }
static int log2s_(int size) { return size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0; }
static void zext(xlat_buf *b, int d, int n, int size) { E(b, size == 8 ? a64_mov(d, n) : a64_ubfx(d, n, 0, bits(size))); }
static void sext(xlat_buf *b, int d, int n, int size) { E(b, size == 8 ? a64_mov(d, n) : a64_sbfx(d, n, 0, bits(size))); }
static int high8(const x86_insn *d, int reg, int size) { return size == 1 && !d->rex && reg >= 4 && reg <= 7; }

static void rd_reg(xlat_buf *b, const x86_insn *d, int t, int reg, int size) {
    if (high8(d, reg, size)) E(b, a64_ubfx(t, reg - 4, 8, 8));
    else zext(b, t, reg, size);
}
static void wr_reg(xlat_buf *b, const x86_insn *d, int reg, int t, int size) {
    if (high8(d, reg, size)) E(b, a64_bfi(reg - 4, t, 8, 8));
    else if (size == 1) E(b, a64_bfi(reg, t, 0, 8));
    else if (size == 2) E(b, a64_bfi(reg, t, 0, 16));
    else if (size == 4) E(b, a64_movw(reg, t));                        // (32-bit writes clear the upper half)
    else E(b, a64_mov(reg, t));
}

// The memory operand's address into T_ADDR (segment base added unless lea).
static void ea(xlat_buf *b, const x86_insn *d, uint64_t next, int for_lea) {
    if (d->rip_relative) mov_imm(b, T_ADDR, next + (uint64_t)(int64_t)d->disp);
    else {
        int have = 0;
        if (d->base != 0xFF) { E(b, a64_mov(T_ADDR, d->base)); have = 1; }
        if (d->index != 0xFF) {
            int sh = d->scale == 8 ? 3 : d->scale == 4 ? 2 : d->scale == 2 ? 1 : 0;
            if (have) E(b, a64_add_lsl(T_ADDR, T_ADDR, d->index, sh));
            else { E(b, a64_lsli(T_ADDR, d->index, sh)); have = 1; }
        }
        if (!have) mov_imm(b, T_ADDR, (uint64_t)(int64_t)d->disp);
        else if (d->disp > 0 && d->disp < 4096) E(b, a64_addi(T_ADDR, T_ADDR, (uint32_t)d->disp));
        else if (d->disp < 0 && d->disp > -4096) E(b, a64_subi(T_ADDR, T_ADDR, (uint32_t)-d->disp));
        else if (d->disp) { mov_imm(b, S0, (uint64_t)(int64_t)d->disp); E(b, a64_add(T_ADDR, T_ADDR, S0)); }
    }
    if (d->prefixes & X86_P_ADDRSIZE) E(b, a64_ubfx(T_ADDR, T_ADDR, 0, 32));
    if (!for_lea && (d->prefixes & (X86_P_SEG_FS | X86_P_SEG_GS))) {
        E(b, a64_ldr(8, S0, XC, (d->prefixes & X86_P_SEG_FS) ? CTX_FS : CTX_GS));
        E(b, a64_add(T_ADDR, T_ADDR, S0));
    }
}
// ModRM's r/m operand: read into t (zero-extended); its address, if memory, stays in T_ADDR for wr_rm.
static void rd_rm(xlat_buf *b, const x86_insn *d, int t, int size, uint64_t next) {
    if (d->mod == 3) rd_reg(b, d, t, d->rm, size);
    else { ea(b, d, next, 0); E(b, a64_ldr(size, t, T_ADDR, 0)); }
}
// Locked accesses to memory that is not naturally aligned (x86 allows them; ARM atomics fault on them) go through one
// lock shared by all translated code: taken with SWPAL, released with STLR. x25 holds its address meanwhile.
static uint64_t xlat_split_lock;
enum { X_LOCK = 25 };
static void split_lock_take(xlat_buf *b) {
    mov_imm(b, X_LOCK, (uint64_t)(uintptr_t)&xlat_split_lock);
    E(b, a64_movz(S1, 1, 0)); E(b, a64_swpal(8, S1, S1, X_LOCK)); E(b, a64_cbnz(S1, -2));
}
static void split_lock_give(xlat_buf *b) { E(b, a64_stlr(XZR, X_LOCK)); }
// A forward branch taken when T_ADDR is not aligned to size (S0 used); returns its position for patch_cbz.
static size_t if_misaligned(xlat_buf *b, int size) {
    E(b, a64_ubfx(S0, T_ADDR, 0, log2s_(size)));
    size_t at = here(b); E(b, 0);
    return at;
}

static void wr_rm(xlat_buf *b, const x86_insn *d, int t, int size) {
    if (d->mod == 3) wr_reg(b, d, d->rm, t, size);
    else if (b->lock) {                                  // LOCK: compare-and-swap with the value read, else start over
        size_t slow = size > 1 ? if_misaligned(b, size) : 0;
        E(b, a64_mov(S1, b->lock_expect));
        E(b, a64_casal(size, S1, t, T_ADDR));
        E(b, a64_cmp(S1, b->lock_expect));
        E(b, a64_bcond(NE, (int32_t)b->lock_head - (int32_t)b->n));
        if (size > 1) {
            size_t done = here(b); E(b, 0);
            patch_cbz(b, slow, 1, S0);                   // misaligned: the same, under the split lock
            split_lock_take(b);
            E(b, a64_ldr(size, S1, T_ADDR, 0)); E(b, a64_cmp(S1, b->lock_expect));
            E(b, a64_bcond(EQ, 3)); split_lock_give(b); E(b, a64_b((int32_t)b->lock_head - (int32_t)b->n));
            E(b, a64_str(size, t, T_ADDR, 0)); split_lock_give(b);
            patch_b(b, done);
        }
    }
    else E(b, a64_str(size, t, T_ADDR, 0));
}

// ---- flags ---------------------------------------------------------------------------------------------------------------
static void put_flag(xlat_buf *b, int bitreg, int pos) { E(b, a64_orr_lsl(ACC, ACC, bitreg, pos)); }
static void flag_bit(xlat_buf *b, int d, int n, int pos) { E(b, a64_ubfx(d, n, pos, 1)); }
// Merges ACC into EFLAGS: the bits in mask are replaced.
static void merge_flags(xlat_buf *b, uint32_t mask) {
    mov_imm(b, S0, mask);
    E(b, a64_bic(XF, XF, S0));
    E(b, a64_orr(XF, XF, ACC));
}
// ZF, SF and PF of T_R (size) into ACC.
static void flags_zsp(xlat_buf *b, int size) {
    E(b, a64_ubfx(S0, T_R, 0, 8));
    E(b, a64_ldrb_reg(S0, XP, S0));
    put_flag(b, S0, 2);
    if (size == 8) E(b, a64_cmp(T_R, XZR));
    else { E(b, a64_ubfx(S0, T_R, 0, bits(size))); E(b, a64_cmp(S0, XZR)); }
    E(b, a64_cset(S0, EQ));
    put_flag(b, S0, 6);
    flag_bit(b, S0, T_R, bits(size) - 1);
    put_flag(b, S0, 7);
}
enum { K_ADD, K_SUB, K_LOGIC };
// All six arithmetic flags of T_R = T_A op T_B (+/- carry), from the operands' bits; keep_cf: INC/DEC.
static void flags_arith(xlat_buf *b, int kind, int size, int keep_cf) {
    int msb = bits(size) - 1;
    E(b, a64_movz(ACC, 0, 0));
    if (kind == K_ADD && !keep_cf) {                 // carry out of the top: (a & b) | ((a | b) & ~r)
        E(b, a64_and(T_T, T_A, T_B)); E(b, a64_orr(S0, T_A, T_B)); E(b, a64_bic(S0, S0, T_R)); E(b, a64_orr(T_T, T_T, S0));
        flag_bit(b, ACC, T_T, msb);
    } else if (kind == K_SUB && !keep_cf) {          // borrow: (~a & b) | (~(a ^ b) & r)
        E(b, a64_bic(T_T, T_B, T_A)); E(b, a64_eor(S0, T_A, T_B)); E(b, a64_bic(S0, T_R, S0)); E(b, a64_orr(T_T, T_T, S0));
        flag_bit(b, ACC, T_T, msb);
    }
    flags_zsp(b, size);
    if (kind != K_LOGIC) {
        E(b, a64_eor(S0, T_A, T_B)); E(b, a64_eor(S0, S0, T_R)); flag_bit(b, S0, S0, 4); put_flag(b, S0, 4);
        if (kind == K_ADD) { E(b, a64_eor(S0, T_A, T_R)); E(b, a64_eor(S1, T_B, T_R)); }
        else { E(b, a64_eor(S0, T_A, T_B)); E(b, a64_eor(S1, T_A, T_R)); }
        E(b, a64_and(S0, S0, S1)); flag_bit(b, S0, S0, msb); put_flag(b, S0, 11);
    }
    merge_flags(b, keep_cf ? F_ARITH & ~F_CF : F_ARITH);
}

// Condition cc (0-15) of EFLAGS into S0 as 0 or 1.
static void cond(xlat_buf *b, int cc) {
    switch (cc >> 1) {
    case 0: flag_bit(b, S0, XF, 11); break;                                            // O
    case 1: flag_bit(b, S0, XF, 0); break;                                             // B
    case 2: flag_bit(b, S0, XF, 6); break;                                             // Z
    case 3: flag_bit(b, S0, XF, 0); flag_bit(b, S1, XF, 6); E(b, a64_orr(S0, S0, S1)); break;   // BE
    case 4: flag_bit(b, S0, XF, 7); break;                                             // S
    case 5: flag_bit(b, S0, XF, 2); break;                                             // P
    case 6: flag_bit(b, S0, XF, 7); flag_bit(b, S1, XF, 11); E(b, a64_eor(S0, S0, S1)); break;  // L
    case 7: flag_bit(b, S0, XF, 7); flag_bit(b, S1, XF, 11); E(b, a64_eor(S0, S0, S1));        // LE
            flag_bit(b, S1, XF, 6); E(b, a64_orr(S0, S0, S1)); break;
    }
    if (cc & 1) { E(b, a64_movz(S1, 1, 0)); E(b, a64_sub(S0, S1, S0)); }
}

// ---- helpers (C) -------------------------------------------------------------------------------------------------------------
// Calls xlat_helper(ctx) with op and the value in `val`: the x86 registers and flags go through the context.
// (op_reg >= 0: the op is in that register, computed at run time)
static void call_helper_r(xlat_buf *b, uint32_t op, int op_reg, int val) {
    E(b, a64_str(8, val, XC, CTX_HELPER_VAL));
    if (op_reg >= 0) E(b, a64_str(8, op_reg, XC, CTX_HELPER_OP));
    for (int r = 0; r < 16; r += 2) E(b, a64_stp(r, r + 1, XC, r * 8));
    for (int r = 0; r < 16; r += 2) E(b, a64_stpq(r, r + 1, XC, CTX_XMM + r * 16));
    E(b, a64_str(8, XF, XC, CTX_FLAGS));
    if (op_reg < 0) { mov_imm(b, S0, op); E(b, a64_str(8, S0, XC, CTX_HELPER_OP)); }
    E(b, a64_mov(0, XC));
    mov_imm(b, S0, (uint64_t)(uintptr_t)xlat_helper);
    E(b, a64_blr(S0));
    for (int r = 0; r < 16; r += 2) E(b, a64_ldp(r, r + 1, XC, r * 8));
    for (int r = 0; r < 16; r += 2) E(b, a64_ldpq(r, r + 1, XC, CTX_XMM + r * 16));
    E(b, a64_ldr(8, XF, XC, CTX_FLAGS));
}
static void call_helper(xlat_buf *b, uint32_t op, int val) { call_helper_r(b, op, -1, val); }
enum { H_MUL = 1, H_IMUL, H_DIV, H_IDIV, H_RCL, H_RCR, H_X87, H_FXSAVE, H_FXRSTOR, H_PDEP, H_PEXT, H_GATHER, H_PCMPSTR, H_RAND };   // op = kind | size << 8 | count << 16 (x87: opcode, ModRM)
// AVX2 gathers (VPGATHERDD/DQ/QD/QQ, VGATHERDPS/DPD/QPS/QPD): op = opcode | W << 8 | L << 9 | dst << 10 | mask << 14 |
// index << 18 | log2 scale << 22; base: the base register plus displacement. Each element whose mask has its sign bit set
// is loaded from base + index * scale; the mask ends all zero, and what the instruction does not gather in the
// destination is zero (VEX: the upper half too when 128 bits).
static void gather(x86_ctx *c, uint32_t op, uint64_t base) {
    int opc = (int)(op & 0xFF), w = (int)(op >> 8 & 1), L = (int)(op >> 9 & 1), dst = (int)(op >> 10 & 15), mk = (int)(op >> 14 & 15);
    int idx = (int)(op >> 18 & 15), sc = (int)(op >> 22 & 3), qidx = opc & 1, es = w ? 8 : 4;
    int n = (qidx || w) ? (L ? 4 : 2) : (L ? 8 : 4);
    uint8_t d[32], m[32], ix[32];
    #define LOAD32(v, r) do { memcpy(v, c->xmm[r], 16); memcpy(v + 16, c->ymm_hi[r], 16); } while (0)
    LOAD32(d, dst); LOAD32(m, mk); LOAD32(ix, idx);
    #undef LOAD32
    for (int i = 0; i < n; i++) {
        uint64_t mv = 0; memcpy(&mv, m + es * i, (size_t)es);
        if (!(mv >> (es * 8 - 1))) continue;
        int64_t iv;
        if (qidx) { int64_t q; memcpy(&q, ix + 8 * i, 8); iv = q; } else { int32_t dd; memcpy(&dd, ix + 4 * i, 4); iv = dd; }
        memcpy(d + es * i, (const void *)(uintptr_t)(base + (uint64_t)(iv << sc)), (size_t)es);
    }
    int keep = n * es;                                                    // (bytes gathered: the rest of the destination is 0)
    memset(d + keep, 0, (size_t)(32 - keep));
    if (!L) memset(d + 16, 0, 16);
    memcpy(c->xmm[dst], d, 16); memcpy(c->ymm_hi[dst], d + 16, 16);
    memset(c->xmm[mk], 0, 16); memset(c->ymm_hi[mk], 0, 16);
}
// PCMPESTRI/ESTRM/ISTRI/ISTRM (SSE4.2): op = opcode & 3 | memory << 2 | REX.W << 3 | imm << 4 | reg << 12 | rm << 16 (24 bits:
// the helper call has them); a: the memory operand's address. The SDM's algorithm: element format, aggregation,
// polarity, index or mask out.
static void pcmpstr(x86_ctx *c, uint32_t op, uint64_t a) {
    int opc = 0x60 | (int)(op & 3), mem = (int)(op >> 2 & 1), w = (int)(op >> 3 & 1), imm = (int)(op >> 4 & 0xFF), reg = (int)(op >> 12 & 15);
    int rm = (int)(op >> 16 & 15), words = imm & 1, sgn = imm >> 1 & 1, agg = imm >> 2 & 3, pol = imm >> 4 & 3, n = words ? 8 : 16;
    uint8_t A[16], B[16];
    memcpy(A, c->xmm[reg], 16);
    if (mem) memcpy(B, (const void *)(uintptr_t)a, 16); else memcpy(B, c->xmm[rm], 16);
    int64_t ea_[16], eb[16];
    for (int i = 0; i < n; i++) {
        if (words) { uint16_t x, y; memcpy(&x, A + 2 * i, 2); memcpy(&y, B + 2 * i, 2); ea_[i] = sgn ? (int16_t)x : x; eb[i] = sgn ? (int16_t)y : y; }
        else { ea_[i] = sgn ? (int8_t)A[i] : A[i]; eb[i] = sgn ? (int8_t)B[i] : B[i]; }
    }
    int la = n, lb = n;
    if (opc <= 0x61) {                                                    // explicit lengths: |EAX|, |EDX| (RAX, RDX with REX.W)
        int64_t x = w ? (int64_t)c->gpr[0] : (int32_t)c->gpr[0], y = w ? (int64_t)c->gpr[2] : (int32_t)c->gpr[2];
        uint64_t ax = x < 0 ? 0 - (uint64_t)x : (uint64_t)x, ay = y < 0 ? 0 - (uint64_t)y : (uint64_t)y;
        la = ax < (uint64_t)n ? (int)ax : n; lb = ay < (uint64_t)n ? (int)ay : n;
    } else {                                                              // implicit: up to the first null element
        for (la = 0; la < n && ea_[la]; la++) {}
        for (lb = 0; lb < n && eb[lb]; lb++) {}
    }
    unsigned res = 0;
    for (int j = 0; j < n; j++) {
        int r = 0;
        if (agg == 0) { if (j < lb) for (int i = 0; i < la; i++) r |= ea_[i] == eb[j]; }                       // equal any
        else if (agg == 1) { if (j < lb) for (int i = 0; i + 1 < la; i += 2) r |= ea_[i] <= eb[j] && eb[j] <= ea_[i + 1]; }   // ranges
        else if (agg == 2) r = j >= la && j >= lb ? 1 : j >= la || j >= lb ? 0 : ea_[j] == eb[j];               // equal each
        else {                                                            // equal ordered
            r = 1;
            for (int i = 0; i < n - j && r; i++) {
                if (i >= la) break;
                if (j + i >= lb) { r = 0; break; }
                r = ea_[i] == eb[j + i];
            }
        }
        res |= (unsigned)r << j;
    }
    unsigned all = (1u << n) - 1;
    if (pol == 1) res ^= all; else if (pol == 3) res ^= (1u << lb) - 1;
    res &= all;
    if (!(opc & 1)) {                                                     // ...STRM (60, 62): XMM0, bits or whole elements
        uint8_t m[16] = { 0 };
        if (imm & 0x40) for (int j = 0; j < n; j++) { if (res >> j & 1) memset(m + (words ? 2 * j : j), 0xFF, words ? 2 : 1); }
        else { m[0] = (uint8_t)res; m[1] = (uint8_t)(res >> 8); }
        memcpy(c->xmm[0], m, 16);
    } else {                                                              // ...STRI (61, 63): ECX, the least or most significant bit
        int idx = n;
        if (res) idx = (imm & 0x40) ? 31 - __builtin_clz(res) : __builtin_ctz(res);
        c->gpr[1] = (uint32_t)idx;
    }
    uint64_t fl = (res ? 1u : 0) | (lb < n ? 0x40u : 0) | (la < n ? 0x80u : 0) | (res & 1 ? 0x800u : 0);
    c->flags = (c->flags & ~0x8D5ull) | fl;
}
void xlat_helper(x86_ctx *c) {
    int kind = (int)(c->helper_op & 0xFF), size = (int)(c->helper_op >> 8 & 0xFF), n = size * 8;
    uint64_t mask = n == 64 ? ~0ull : (1ull << n) - 1, v = c->helper_val & mask;
    uint64_t *ax = &c->gpr[0], *dx = &c->gpr[2];
    #define SET(reg, val) do { if (size == 4) *(reg) = (uint32_t)(val); else *(reg) = (*(reg) & ~mask) | ((val) & mask); } while (0)
    if (kind == H_PDEP || kind == H_PEXT) {                             // (source in helper_val, mask in pad; result in helper_val)
        uint64_t src = c->helper_val, mask = c->pad, r = 0;
        if (size == 4) { src = (uint32_t)src; mask = (uint32_t)mask; }
        for (uint64_t bit = 1, k = 0; mask; mask &= mask - 1) {
            uint64_t m = mask & -mask;
            if (kind == H_PDEP) { if (src >> k & 1) r |= m; }
            else if (src & m) r |= bit << k;
            k++;
        }
        c->helper_val = r;
        return;
    }
    if (kind == H_GATHER) { gather(c, (uint32_t)(c->helper_op >> 8), c->helper_val); return; }
    if (kind == H_PCMPSTR) { pcmpstr(c, (uint32_t)(c->helper_op >> 8), c->helper_val); return; }
    if (kind == H_RAND) { arc4random_buf(&c->helper_val, 8); return; }   // RDRAND / RDSEED: the system's random numbers
    if (kind == H_X87) { xlat_x87(c, (uint32_t)(c->helper_op >> 8 & 0xFF), (uint32_t)(c->helper_op >> 16 & 0xFF), c->helper_val); return; }
    if (kind == H_FXSAVE || kind == H_FXRSTOR) { xlat_fxsave(c, c->helper_val, kind == H_FXRSTOR); return; }
    if (kind == H_MUL || kind == H_IMUL) {
        if (size == 1) {
            uint16_t r = kind == H_MUL ? (uint16_t)((uint8_t)*ax * (uint8_t)v) : (uint16_t)((int8_t)*ax * (int8_t)v);
            *ax = (*ax & ~0xFFFFull) | r;
            int over = kind == H_MUL ? (r >> 8) != 0 : (int16_t)r != (int8_t)r;
            c->flags = (c->flags & ~(uint64_t)(F_CF | F_OF)) | (over ? F_CF | F_OF : 0);
            return;
        }
        unsigned __int128 p; int over;
        if (kind == H_MUL) { p = (unsigned __int128)(*ax & mask) * v; over = (p >> n) != 0; }
        else {
            __int128 a = n == 64 ? (int64_t)*ax : n == 32 ? (int32_t)*ax : (int16_t)*ax;
            __int128 bb = n == 64 ? (int64_t)v : n == 32 ? (int32_t)v : (int16_t)v;
            __int128 s = a * bb; p = (unsigned __int128)s;
            __int128 lo = n == 64 ? (int64_t)(uint64_t)p : n == 32 ? (int32_t)(uint32_t)p : (int16_t)(uint16_t)p;
            over = lo != s;
        }
        SET(ax, (uint64_t)p); SET(dx, (uint64_t)(p >> n));
        c->flags = (c->flags & ~(uint64_t)(F_CF | F_OF)) | (over ? F_CF | F_OF : 0);
        return;
    }
    if (kind == H_DIV || kind == H_IDIV) {
        if (!v) { c->fault = X86_FAULT_DE; return; }
        if (size == 1) {
            uint16_t num = (uint16_t)*ax;
            if (kind == H_DIV) {
                unsigned q = num / (uint8_t)v, r = num % (uint8_t)v;
                if (q > 0xFF) { c->fault = X86_FAULT_DE; return; }
                *ax = (*ax & ~0xFFFFull) | (r << 8) | q;
            } else {
                int q = (int16_t)num / (int8_t)v, r = (int16_t)num % (int8_t)v;
                if (q > 127 || q < -128) { c->fault = X86_FAULT_DE; return; }
                *ax = (*ax & ~0xFFFFull) | ((uint64_t)(uint8_t)r << 8) | (uint8_t)q;
            }
            return;
        }
        unsigned __int128 num = ((unsigned __int128)(*dx & mask) << n) | (*ax & mask);
        if (kind == H_DIV) {
            unsigned __int128 q = num / v, r = num % v;
            if (q >> n) { c->fault = X86_FAULT_DE; return; }
            SET(ax, (uint64_t)q); SET(dx, (uint64_t)r);
        } else {
            __int128 sn = n == 64 ? (__int128)num : n == 32 ? (__int128)(int64_t)(uint64_t)num : (__int128)(int32_t)(uint32_t)num;
            __int128 sd = n == 64 ? (int64_t)v : n == 32 ? (int32_t)v : (int16_t)v;
            if (n == 64 && sn == (__int128)((unsigned __int128)1 << 127) && sd == -1) { c->fault = X86_FAULT_DE; return; }
            __int128 q = sn / sd, r = sn % sd;
            __int128 lim = (__int128)1 << (n - 1);
            if (q >= lim || q < -lim) { c->fault = X86_FAULT_DE; return; }
            SET(ax, (uint64_t)q); SET(dx, (uint64_t)r);
        }
        return;
    }
    if (kind == H_RCL || kind == H_RCR) {                // through CF; count already masked to 5 or 6 bits
        int cnt = (int)(c->helper_op >> 16 & 0xFF);
        if (size == 1) cnt %= 9; else if (size == 2) cnt %= 17;
        if (!cnt) { c->helper_val = v; return; }
        uint64_t x = v, cf = c->flags & 1, of;
        if (kind == H_RCL) {
            for (int i = 0; i < cnt; i++) { uint64_t out = x >> (n - 1) & 1; x = ((x << 1) | cf) & mask; cf = out; }
            of = (x >> (n - 1) & 1) ^ cf;
        } else {
            of = (x >> (n - 1) & 1) ^ cf;
            for (int i = 0; i < cnt; i++) { uint64_t out = x & 1; x = (x >> 1) | (cf << (n - 1)); cf = out; }
        }
        c->helper_val = x;
        c->flags = (c->flags & ~(uint64_t)(F_CF | F_OF)) | cf | (of ? F_OF : 0);
        return;
    }
    #undef SET
}

// ---- instructions --------------------------------------------------------------------------------------------------------------
static int log2s(int size) { return size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0; }
static uint64_t szmask(int size) { return size == 8 ? ~0ull : (1ull << bits(size)) - 1; }

// ADD OR ADC SBB AND SUB XOR CMP: T_A (destination value), T_B (source value) loaded; the result written unless CMP.
static void alu(xlat_buf *b, const x86_insn *d, int op, int size, int dest_is_rm, int dest_reg) {
    int kind = K_LOGIC;
    switch (op) {
    case 0: E(b, a64_add(T_R, T_A, T_B)); kind = K_ADD; break;
    case 1: E(b, a64_orr(T_R, T_A, T_B)); break;
    case 2: E(b, a64_add(T_R, T_A, T_B)); flag_bit(b, S0, XF, 0); E(b, a64_add(T_R, T_R, S0)); kind = K_ADD; break;
    case 3: E(b, a64_sub(T_R, T_A, T_B)); flag_bit(b, S0, XF, 0); E(b, a64_sub(T_R, T_R, S0)); kind = K_SUB; break;
    case 4: E(b, a64_and(T_R, T_A, T_B)); break;
    case 5: case 7: E(b, a64_sub(T_R, T_A, T_B)); kind = K_SUB; break;
    case 6: E(b, a64_eor(T_R, T_A, T_B)); break;
    }
    if (op != 7) { if (dest_is_rm) wr_rm(b, d, T_R, size); else wr_reg(b, d, dest_reg, T_R, size); }
    flags_arith(b, kind, size, 0);
}

static void shift(xlat_buf *b, const x86_insn *d, int op, int size, int count_from_cl, int count_imm, uint64_t next) {
    int n = bits(size), msb = n - 1;
    rd_rm(b, d, T_A, size, next);
    if (count_from_cl) E(b, a64_ubfx(T_B, 1, 0, size == 8 ? 6 : 5));
    else {
        int c = count_imm & (size == 8 ? 63 : 31);
        if (!c) {                                                        // count 0: nothing changes, but a 32-bit register
            if (size == 4 && d->mod == 3) wr_reg(b, d, d->rm, T_A, 4);   // destination is still written (upper half cleared)
            return;
        }
        mov_imm(b, T_B, (uint64_t)c);
    }
    size_t skip = here(b); E(b, 0);                                      // CBZ count -> count 0
    E(b, a64_movz(ACC, 0, 0));
    uint32_t mask = F_ARITH;
    switch (op) {
    case 4: case 6:                                                      // SHL
        E(b, a64_lslv(T_R, T_A, T_B));
        mov_imm(b, S0, (uint64_t)n); E(b, a64_sub(S0, S0, T_B)); E(b, a64_lsrv(S0, T_A, S0)); flag_bit(b, ACC, S0, 0);
        flag_bit(b, S1, T_R, msb); E(b, a64_eor(S1, S1, ACC)); put_flag(b, S1, 11);
        break;
    case 5:                                                              // SHR
        E(b, a64_lsrv(T_R, T_A, T_B));
        E(b, a64_subi(S0, T_B, 1)); E(b, a64_lsrv(S0, T_A, S0)); flag_bit(b, ACC, S0, 0);
        flag_bit(b, S1, T_A, msb); put_flag(b, S1, 11);
        break;
    case 7:                                                              // SAR
        sext(b, T_T, T_A, size);
        E(b, a64_asrv(T_R, T_T, T_B));
        E(b, a64_subi(S0, T_B, 1)); E(b, a64_asrv(S0, T_T, S0)); flag_bit(b, ACC, S0, 0);
        break;
    case 0: case 1:                                                      // ROL, ROR: only CF and OF change
        mask = F_CF | F_OF;
        if (size < 4) E(b, a64_ubfx(T_T, T_B, 0, size == 1 ? 3 : 4)); else E(b, a64_mov(T_T, T_B));
        mov_imm(b, S0, (uint64_t)n); E(b, a64_sub(S0, S0, T_T));         // S0 = width - count
        if (size == 8) E(b, a64_rorv(T_R, T_A, op == 0 ? S0 : T_T));      // (ROL n = ROR 64 - n)
        else {
            if (op == 0) { E(b, a64_lslv(T_R, T_A, T_T)); E(b, a64_lsrv(S0, T_A, S0)); }
            else { E(b, a64_lsrv(T_R, T_A, T_T)); E(b, a64_lslv(S0, T_A, S0)); }
            E(b, a64_orr(T_R, T_R, S0));
        }
        if (op == 0) { flag_bit(b, ACC, T_R, 0); flag_bit(b, S1, T_R, msb); E(b, a64_eor(S1, S1, ACC)); }
        else { flag_bit(b, ACC, T_R, msb); flag_bit(b, S1, T_R, msb - 1); flag_bit(b, S0, T_R, msb); E(b, a64_eor(S1, S1, S0)); }
        put_flag(b, S1, 11);
        break;
    }
    wr_rm(b, d, T_R, size);
    if (mask == F_ARITH) flags_zsp(b, size);
    merge_flags(b, mask);
    size_t end = here(b); E(b, 0);
    patch_cbz(b, skip, 0, T_B);                                          // count 0 (from CL): as above
    if (size == 4 && d->mod == 3) wr_reg(b, d, d->rm, T_A, 4);
    patch_b(b, end);
}

static int xlat_one(xlat_buf *b, const x86_insn *d, uint64_t rip);
int xlat_insn(xlat_buf *b, const x86_insn *d, const uint8_t *code, uint64_t rip) {
    (void)code;
    b->lock = (d->prefixes & X86_P_LOCK) && d->has_modrm && d->mod != 3;
    b->lock_head = b->n;
    b->lock_expect = T_A;
    int r = xlat_one(b, d, rip);
    b->lock = 0;
    return r;
}

// String instructions (MOVS CMPS STOS LODS SCAS), with REP / REPE / REPNE: RSI, RDI, RCX, the direction flag.
static void string_op(xlat_buf *b, const x86_insn *d, int op) {
    int size = (op & 1) ? d->opsize : 1, kind = (op - 0xA4) >> 1;   // 0 MOVS, 1 CMPS, 3 STOS, 4 LODS, 5 SCAS
    int rep = (d->prefixes & (X86_P_REP | X86_P_REPNE)) != 0, cmp = kind == 1 || kind == 5;
    int repne = (d->prefixes & X86_P_REPNE) && cmp;
    enum { STEP = 25 };
    mov_imm(b, STEP, (uint64_t)size);                                    // step: +size, or -size with DF set
    flag_bit(b, S0, XF, 10); E(b, a64_sub(S1, XZR, STEP)); E(b, a64_cmp(S0, XZR)); E(b, a64_csel(STEP, S1, STEP, NE));
    size_t top = here(b), done = 0;
    if (rep) { done = here(b); E(b, 0); }                                // CBZ RCX -> end
    switch (kind) {
    case 0: E(b, a64_ldr(size, T_A, 6, 0)); E(b, a64_str(size, T_A, 7, 0)); break;
    case 1: E(b, a64_ldr(size, T_A, 6, 0)); E(b, a64_ldr(size, T_B, 7, 0)); break;
    case 3: E(b, a64_str(size, 0, 7, 0)); break;
    case 4: E(b, a64_ldr(size, T_A, 6, 0)); wr_reg(b, d, 0, T_A, size); break;
    case 5: rd_reg(b, d, T_A, 0, size); E(b, a64_ldr(size, T_B, 7, 0)); break;
    }
    if (kind == 0 || kind == 1 || kind == 4) E(b, a64_add(6, 6, STEP));
    if (kind != 4) E(b, a64_add(7, 7, STEP));
    if (cmp) { E(b, a64_sub(T_R, T_A, T_B)); flags_arith(b, K_SUB, size, 0); }
    if (rep) {
        E(b, a64_subi(1, 1, 1));
        if (cmp) {                                                       // REPE: on while ZF; REPNE: while not ZF
            flag_bit(b, S0, XF, 6);
            E(b, repne ? a64_cbnz(S0, 2) : a64_cbz(S0, 2));
            E(b, a64_b((int32_t)top - (int32_t)b->n));
        } else E(b, a64_b((int32_t)top - (int32_t)b->n));
        patch_cbz(b, done, 0, 1);
    }
}

// SHLD / SHRD: the destination shifted, filled from the source.
static void shld(xlat_buf *b, const x86_insn *d, int right, int count_from_cl, int count_imm, uint64_t next) {
    int size = d->opsize, n = bits(size), msb = n - 1;
    rd_rm(b, d, T_A, size, next); rd_reg(b, d, T_B, d->reg, size);
    if (count_from_cl) E(b, a64_ubfx(T_T, 1, 0, size == 8 ? 6 : 5));
    else {
        int c = count_imm & (size == 8 ? 63 : 31);
        if (!c) { if (size == 4 && d->mod == 3) wr_reg(b, d, d->rm, T_A, 4); return; }
        mov_imm(b, T_T, (uint64_t)c);
    }
    size_t zero = here(b); E(b, 0);                                      // CBZ count
    if (size == 8) {
        mov_imm(b, S0, 64); E(b, a64_sub(S0, S0, T_T));
        if (!right) { E(b, a64_lslv(T_R, T_A, T_T)); E(b, a64_lsrv(S1, T_B, S0)); }
        else { E(b, a64_lsrv(T_R, T_A, T_T)); E(b, a64_lslv(S1, T_B, S0)); }
        E(b, a64_orr(T_R, T_R, S1));
    } else if (!right) {                                                 // ((dest:src) << count) >> n
        E(b, a64_lsli(S1, T_A, n)); E(b, a64_orr(S1, S1, T_B)); E(b, a64_lslv(S1, S1, T_T)); E(b, a64_lsri(T_R, S1, n));
    } else {                                                             // (src:dest) >> count
        E(b, a64_lsli(S1, T_B, n)); E(b, a64_orr(S1, S1, T_A)); E(b, a64_lsrv(T_R, S1, T_T));
    }
    E(b, a64_movz(ACC, 0, 0));
    if (!right) { mov_imm(b, S0, (uint64_t)n); E(b, a64_sub(S0, S0, T_T)); E(b, a64_lsrv(S0, T_A, S0)); }
    else { E(b, a64_subi(S0, T_T, 1)); E(b, a64_lsrv(S0, T_A, S0)); }
    flag_bit(b, ACC, S0, 0);                                             // CF: the last bit out of the destination
    E(b, a64_eor(S0, T_R, T_A)); flag_bit(b, S0, S0, msb); put_flag(b, S0, 11);   // OF: the sign changed
    wr_rm(b, d, T_R, size);
    flags_zsp(b, size);
    merge_flags(b, F_ARITH);
    size_t end = here(b); E(b, 0);
    patch_cbz(b, zero, 0, T_T);
    if (size == 4 && d->mod == 3) wr_reg(b, d, d->rm, T_A, 4);
    patch_b(b, end);
}

// BMI1 / BMI2 (VEX-encoded general-purpose instructions): ANDN BEXTR BLSR BLSMSK BLSI BZHI PDEP PEXT MULX SHLX SHRX SARX
// RORX. 1: not one of them.
static int xlat_bmi(xlat_buf *b, const x86_insn *d, uint64_t next) {
    int op = d->opcode, pf = d->mandatory, sz = d->vex_w ? 8 : 4, bits_ = sz * 8, v = d->vex_vvvv;
    if (d->map == X86_MAP_0F3A) {
        if (op != 0xF0 || pf != 0xF2) return 1;
        rd_rm(b, d, T_A, sz, next);                                       // RORX: no flags
        E(b, a64_extr(sz == 8, T_R, T_A, T_A, (int)(d->imm & (bits_ - 1))));
        wr_reg(b, d, d->reg, T_R, sz);
        return 0;
    }
    if (d->map != X86_MAP_0F38 || op < 0xF2 || op > 0xF7 || op == 0xF4) return 1;
    if (d->vex_l) return -1;
    #define ZS_CF(cf_reg) do { E(b, a64_movz(ACC, 0, 0)); flags_zsp(b, sz); if ((cf_reg) >= 0) put_flag(b, cf_reg, 0); merge_flags(b, F_ARITH); } while (0)
    if (op == 0xF2 && pf == 0) {                                          // ANDN: ~vvvv & r/m; ZF SF, CF OF 0
        rd_rm(b, d, T_A, sz, next); rd_reg(b, d, T_B, v, sz);
        E(b, a64_bic(T_R, T_A, T_B)); ZS_CF(-1); wr_reg(b, d, d->reg, T_R, sz);
        return 0;
    }
    if (op == 0xF3 && pf == 0) {                                          // BLSR BLSMSK BLSI: into VEX.vvvv
        int g = d->reg & 7;
        if (g < 1 || g > 3) return -1;
        rd_rm(b, d, T_A, sz, next);
        if (g == 3) E(b, a64_sub(T_B, XZR, T_A)); else E(b, a64_subi(T_B, T_A, 1));
        E(b, g == 1 ? a64_and(T_R, T_A, T_B) : g == 2 ? a64_eor(T_R, T_A, T_B) : a64_and(T_R, T_A, T_B));
        if (sz == 4) E(b, a64_movw(T_R, T_R));
        E(b, a64_cmp(T_A, XZR)); E(b, a64_cset(T_T, g == 3 ? NE : EQ));    // CF: source zero (BLSI: not zero)
        ZS_CF(T_T);
        wr_reg(b, d, v, T_R, sz);
        return 0;
    }
    if (op == 0xF5 && pf == 0) {                                          // BZHI: bits from the index (vvvv[7:0]) up cleared
        rd_rm(b, d, T_A, sz, next); rd_reg(b, d, T_B, v, sz);
        E(b, a64_ubfx(T_B, T_B, 0, 8));
        mov_imm(b, S0, ~0ull); E(b, a64_lslv(S0, S0, T_B)); E(b, a64_bic(T_R, T_A, S0));
        mov_imm(b, S1, (uint64_t)bits_); E(b, a64_cmp(T_B, S1));
        E(b, a64_csel(T_R, T_A, T_R, CS)); E(b, a64_cset(T_T, CS));       // (index past the size: the source, CF 1)
        ZS_CF(T_T); wr_reg(b, d, d->reg, T_R, sz);
        return 0;
    }
    if (op == 0xF5 && (pf == 0xF2 || pf == 0xF3)) {                       // PDEP PEXT (in C)
        rd_reg(b, d, T_A, v, sz); rd_rm(b, d, T_B, sz, next);
        E(b, a64_str(8, T_B, XC, CTX_HELPER_VAL + 8));
        call_helper(b, (pf == 0xF2 ? H_PDEP : H_PEXT) | (uint32_t)sz << 8, T_A);
        E(b, a64_ldr(8, T_R, XC, CTX_HELPER_VAL)); wr_reg(b, d, d->reg, T_R, sz);
        return 0;
    }
    if (op == 0xF6 && pf == 0xF2) {                                       // MULX: rdx * r/m; low into vvvv, then high into reg
        rd_rm(b, d, T_A, sz, next); rd_reg(b, d, T_B, 2, sz);
        if (sz == 8) { E(b, a64_mul(T_R, T_A, T_B)); E(b, a64_umulh(T_T, T_A, T_B)); }
        else { E(b, a64_mul(T_R, T_A, T_B)); E(b, a64_lsri(T_T, T_R, 32)); }
        wr_reg(b, d, v, T_R, sz); wr_reg(b, d, d->reg, T_T, sz);
        return 0;
    }
    if (op == 0xF7) {                                                     // BEXTR SHLX SARX SHRX
        rd_rm(b, d, T_A, sz, next); rd_reg(b, d, T_B, v, sz);
        if (pf == 0) {                                                    // BEXTR: start = vvvv[7:0], length = vvvv[15:8]
            E(b, a64_ubfx(S0, T_B, 0, 8)); E(b, a64_ubfx(S1, T_B, 8, 8));
            E(b, a64_lsrv(T_R, T_A, S0)); mov_imm(b, T_T, (uint64_t)bits_); E(b, a64_cmp(S0, T_T)); E(b, a64_csel(T_R, XZR, T_R, CS));
            mov_imm(b, S0, ~0ull); E(b, a64_lslv(S0, S0, S1)); E(b, a64_cmp(S1, T_T)); E(b, a64_csel(S0, XZR, S0, CS));
            E(b, a64_bic(T_R, T_R, S0));
            ZS_CF(-1); wr_reg(b, d, d->reg, T_R, sz);
            return 0;
        }
        int kind = pf == 0x66 ? 0 : pf == 0xF2 ? 1 : 2;                   // SHLX SHRX SARX: no flags
        if (sz == 8) E(b, kind == 0 ? a64_lslv(T_R, T_A, T_B) : kind == 1 ? a64_lsrv(T_R, T_A, T_B) : a64_asrv(T_R, T_A, T_B));
        else E(b, a64_shiftw(kind, T_R, T_A, T_B));
        wr_reg(b, d, d->reg, T_R, sz);
        return 0;
    }
    #undef ZS_CF
    return -1;
}

// CMPXCHG8B / CMPXCHG16B m: EDX:EAX (RDX:RAX) against memory; equal: ECX:EBX (RCX:RBX) stored, ZF 1; else the memory
// into EDX:EAX (RDX:RAX), ZF 0. Atomic either way (CASAL / CASPAL; 8 bytes across 16: the split lock). 16 bytes must
// be aligned (x86: #GP; here the alignment fault).
static void cmpxchg8_16(xlat_buf *b, const x86_insn *d, uint64_t next) {
    b->lock = 0;
    ea(b, d, next, 0);
    if (d->opsize == 8) {
        E(b, a64_mov(S0, 0)); E(b, a64_mov(S1, 2)); E(b, a64_mov(T_B, 3)); E(b, a64_mov(T_R, 1));
        E(b, a64_mov(T_A, S0)); E(b, a64_mov(T_T, S1));
        E(b, a64_caspal(S0, T_B, T_ADDR));                                // (S0, S1: what memory held)
        E(b, a64_eor(T_A, T_A, S0)); E(b, a64_eor(T_T, T_T, S1)); E(b, a64_orr(T_A, T_A, T_T));
        E(b, a64_cmp(T_A, XZR)); E(b, a64_cset(T_T, EQ)); E(b, a64_bfi(XF, T_T, 6, 1));
        E(b, a64_csel(0, 0, S0, EQ)); E(b, a64_csel(2, 2, S1, EQ));
        return;
    }
    // 8 bytes: (edx << 32 | eax) and (ecx << 32 | ebx)
    E(b, a64_movw(T_B, 0)); E(b, a64_orr_lsl(T_B, T_B, 2, 32));
    E(b, a64_movw(T_R, 3)); E(b, a64_orr_lsl(T_R, T_R, 1, 32));
    size_t slow = if_misaligned(b, 8);
    E(b, a64_mov(T_A, T_B)); E(b, a64_casal(8, T_A, T_R, T_ADDR));       // T_A: what memory held
    size_t done = here(b); E(b, 0);
    patch_cbz(b, slow, 1, S0);
    split_lock_take(b);
    E(b, a64_ldr(8, T_A, T_ADDR, 0)); E(b, a64_cmp(T_A, T_B));
    E(b, a64_bcond(NE, 2)); E(b, a64_str(8, T_R, T_ADDR, 0));
    split_lock_give(b);
    patch_b(b, done);
    E(b, a64_cmp(T_A, T_B)); E(b, a64_cset(T_T, EQ)); E(b, a64_bfi(XF, T_T, 6, 1));
    size_t eq = here(b); E(b, 0);
    E(b, a64_movw(0, T_A)); E(b, a64_lsri(2, T_A, 32));                 // not equal: EDX:EAX (upper halves cleared)
    b->code[eq] = a64_bcond(EQ, (int32_t)(b->n - eq));
}

// MXCSR on ARM: its rounding control and FTZ/DAZ are FPCR's RMode and FZ (FZ flushes inputs and results alike, so
// one of FTZ and DAZ alone is taken as both: TRANSLATOR.md); its exception flags are FPSR's cumulative bits, read
// back when the program reads MXCSR (STMXCSR) and when translated code is left.
static void mxcsr_to_fpcr(xlat_buf *b, int dst, int mx) {               // (S0, S1 used)
    E(b, a64_ubfx(S0, mx, 13, 1)); E(b, a64_lsli(S0, S0, 23));         // x86 RC 01 (down) / 10 (up) are ARM's 10 / 01
    E(b, a64_ubfx(S1, mx, 14, 1)); E(b, a64_orr_lsl(S0, S0, S1, 22));
    E(b, a64_ubfx(S1, mx, 15, 1)); E(b, a64_ubfx(dst, mx, 6, 1)); E(b, a64_orr(S1, S1, dst));
    E(b, a64_orr_lsl(dst, S0, S1, 24));
}
static void fpsr_to_mxcsr(xlat_buf *b, int dst, int fpsr) {             // IOC DZC OFC UFC IXC -> IE ZE OE UE PE (S0 used)
    E(b, a64_ubfx(dst, fpsr, 0, 1));
    E(b, a64_ubfx(S0, fpsr, 1, 4)); E(b, a64_orr_lsl(dst, dst, S0, 2));
    E(b, a64_ubfx(S0, fpsr, 3, 1)); E(b, a64_orr_lsl(dst, dst, S0, 5));  // (x86 reports an underflow only when inexact)
}
// The flags so far into ctx->mxcsr (in T_A afterwards); FPSR cleared.
static void mxcsr_sync(xlat_buf *b) {
    E(b, a64_mrs_fpsr(T_B)); fpsr_to_mxcsr(b, T_R, T_B);
    E(b, a64_ldr(4, T_A, XC, CTX_MXCSR)); E(b, a64_orr(T_A, T_A, T_R)); E(b, a64_str(4, T_A, XC, CTX_MXCSR));
    E(b, a64_msr_fpsr(XZR));
}
#include "xlat_sse.inc"

static int xlat_one(xlat_buf *b, const x86_insn *d, uint64_t rip) {
    uint64_t next = rip + d->len;
    int op = d->opcode, size = d->opsize, reg = d->reg;
    if (d->prefixes & X86_P_EVEX) return -1;
    if (d->prefixes & X86_P_VEX) return xlat_vex(b, d, next);         // AVX (xlat_sse.inc)
    if (d->map == X86_MAP_1) {
        if (op < 0x40 && (op & 7) < 6) {                                 // ADD .. CMP, six forms each
            int aop = op >> 3, form = op & 7, sz = (form & 1) ? size : 1;
            if (form == 0 || form == 1) { rd_rm(b, d, T_A, sz, next); rd_reg(b, d, T_B, reg, sz); alu(b, d, aop, sz, 1, 0); }
            else if (form == 2 || form == 3) { rd_reg(b, d, T_A, reg, sz); rd_rm(b, d, T_B, sz, next); alu(b, d, aop, sz, 0, reg); }
            else { rd_reg(b, d, T_A, 0, sz); mov_imm(b, T_B, (uint64_t)d->imm & szmask(sz)); alu(b, d, aop, sz, 0, 0); }
            return 0;
        }
        if (op >= 0x80 && op <= 0x83 && op != 0x82) {                   // group 1: r/m, imm
            int sz = op == 0x80 ? 1 : size;
            rd_rm(b, d, T_A, sz, next); mov_imm(b, T_B, (uint64_t)d->imm & szmask(sz)); alu(b, d, reg & 7, sz, 1, 0);
            return 0;
        }
        if (op == 0x84 || op == 0x85 || op == 0xA8 || op == 0xA9) {     // TEST
            int sz = (op & 1) ? size : 1;
            if (op < 0xA0) { rd_rm(b, d, T_A, sz, next); rd_reg(b, d, T_B, reg, sz); }
            else { rd_reg(b, d, T_A, 0, sz); mov_imm(b, T_B, (uint64_t)d->imm & szmask(sz)); }
            E(b, a64_and(T_R, T_A, T_B)); flags_arith(b, K_LOGIC, sz, 0);
            return 0;
        }
        if (op >= 0x88 && op <= 0x8B) {                                 // MOV
            int sz = (op & 1) ? size : 1;
            if (op <= 0x89) { rd_reg(b, d, T_A, reg, sz); if (d->mod != 3) ea(b, d, next, 0); wr_rm(b, d, T_A, sz); }
            else { rd_rm(b, d, T_A, sz, next); wr_reg(b, d, reg, T_A, sz); }
            return 0;
        }
        if (op == 0x8D && d->mod != 3) {                                 // LEA
            ea(b, d, next, 1); wr_reg(b, d, reg, T_ADDR, size); return 0;
        }
        if (op == 0xC6 || op == 0xC7) {                                  // MOV r/m, imm
            if (reg & 7) return -1;
            int sz = op == 0xC6 ? 1 : size;
            if (d->mod != 3) ea(b, d, next, 0);
            mov_imm(b, T_A, (uint64_t)d->imm & szmask(sz)); wr_rm(b, d, T_A, sz);
            return 0;
        }
        if (op >= 0xB0 && op <= 0xBF) {                                  // MOV reg, imm
            int sz = op < 0xB8 ? 1 : size, r = (op & 7) | ((d->rex & 1) << 3);
            mov_imm(b, T_A, (uint64_t)d->imm & szmask(sz)); wr_reg(b, d, r, T_A, sz);
            return 0;
        }
        if (op == 0x69 || op == 0x6B) {                                  // IMUL reg, r/m, imm
            rd_rm(b, d, T_A, size, next); mov_imm(b, T_B, (uint64_t)d->imm & szmask(size));
            goto imul;
        }
        if (op == 0x63) {                                                // MOVSXD: sign-extended to 64 bits whatever the
            rd_rm(b, d, T_A, 4, next);                                   // operand size, as Rosetta 2 runs it
            sext(b, T_A, T_A, 4); wr_reg(b, d, reg, T_A, 8);
            return 0;
        }
        if ((op == 0x86 || op == 0x87) && d->mod != 3) {                 // XCHG with memory: always atomic
            int sz = op == 0x86 ? 1 : size;
            rd_reg(b, d, T_B, reg, sz); ea(b, d, next, 0);
            size_t slow = sz > 1 ? if_misaligned(b, sz) : 0;
            E(b, a64_swpal(sz, T_B, T_A, T_ADDR));
            if (sz > 1) {
                size_t done = here(b); E(b, 0);
                patch_cbz(b, slow, 1, S0);
                split_lock_take(b); E(b, a64_ldr(sz, T_A, T_ADDR, 0)); E(b, a64_str(sz, T_B, T_ADDR, 0)); split_lock_give(b);
                patch_b(b, done);
            }
            wr_reg(b, d, reg, T_A, sz);
            return 0;
        }
        if (op == 0x86 || op == 0x87) {                                  // XCHG r/m, reg
            int sz = op == 0x86 ? 1 : size;
            rd_rm(b, d, T_A, sz, next); rd_reg(b, d, T_B, reg, sz);
            wr_rm(b, d, T_B, sz); wr_reg(b, d, reg, T_A, sz);
            return 0;
        }
        if (op >= 0x90 && op <= 0x97) {                                  // XCHG rAX, reg (90: NOP)
            int r = (op & 7) | ((d->rex & 1) << 3);
            if (r == 0) return 0;
            rd_reg(b, d, T_A, 0, size); rd_reg(b, d, T_B, r, size);
            wr_reg(b, d, 0, T_B, size); wr_reg(b, d, r, T_A, size);
            return 0;
        }
        if (op == 0x98) {                                                // CBW / CWDE / CDQE
            int from = size / 2;
            sext(b, T_A, 0, from); wr_reg(b, d, 0, T_A, size);
            return 0;
        }
        if (op == 0x99) {                                                // CWD / CDQ / CQO
            E(b, a64_sbfx(T_A, 0, bits(size) - 1, 1)); wr_reg(b, d, 2, T_A, size);
            return 0;
        }
        if (op == 0xF6 || op == 0xF7) {                                  // group 3
            int sz = op == 0xF6 ? 1 : size, g = reg & 7;
            if (g == 0 || g == 1) {                                      // TEST r/m, imm
                rd_rm(b, d, T_A, sz, next); mov_imm(b, T_B, (uint64_t)d->imm & szmask(sz));
                E(b, a64_and(T_R, T_A, T_B)); flags_arith(b, K_LOGIC, sz, 0);
            } else if (g == 2) {                                         // NOT
                rd_rm(b, d, T_A, sz, next); E(b, a64_orn(T_R, XZR, T_A)); wr_rm(b, d, T_R, sz);
            } else if (g == 3) {                                         // NEG
                b->lock_expect = T_B;
                rd_rm(b, d, T_B, sz, next); E(b, a64_movz(T_A, 0, 0)); E(b, a64_sub(T_R, T_A, T_B));
                wr_rm(b, d, T_R, sz); flags_arith(b, K_SUB, sz, 0);
            } else {                                                     // MUL IMUL DIV IDIV (in C)
                rd_rm(b, d, T_B, sz, next);
                call_helper(b, (uint32_t)(g - 3) | (uint32_t)sz << 8, T_B);
            }
            return 0;
        }
        if (op == 0xFF && (reg & 7) == 6) {                              // PUSH r/m
            rd_rm(b, d, T_A, 8, next); E(b, a64_subi(4, 4, 8)); E(b, a64_str(8, T_A, 4, 0));
            return 0;
        }
        if (op == 0x8F && (reg & 7) == 0) {                              // POP r/m (the address after RSP moved)
            E(b, a64_ldr(8, T_A, 4, 0)); E(b, a64_addi(4, 4, 8));
            if (d->mod != 3) ea(b, d, next, 0);
            wr_rm(b, d, T_A, 8);
            return 0;
        }
        if (op == 0xC9) {                                                // LEAVE
            E(b, a64_mov(4, 5)); E(b, a64_ldr(8, 5, 4, 0)); E(b, a64_addi(4, 4, 8));
            return 0;
        }
        if (op >= 0xA0 && op <= 0xA3) {                                  // MOV AL/rAX <-> [moffs64]
            int sz = (op & 1) ? size : 1;
            mov_imm(b, T_ADDR, (uint64_t)d->imm);
            if (d->prefixes & (X86_P_SEG_FS | X86_P_SEG_GS)) { E(b, a64_ldr(8, S0, XC, (d->prefixes & X86_P_SEG_FS) ? CTX_FS : CTX_GS)); E(b, a64_add(T_ADDR, T_ADDR, S0)); }
            if (op <= 0xA1) { E(b, a64_ldr(sz, T_A, T_ADDR, 0)); wr_reg(b, d, 0, T_A, sz); }
            else E(b, a64_str(sz, 0, T_ADDR, 0));
            return 0;
        }
        if (op == 0xD7) {                                                // XLAT: AL = [RBX + AL]
            E(b, a64_ubfx(S0, 0, 0, 8)); E(b, a64_add(T_ADDR, 3, S0));
            E(b, a64_ldr(1, T_A, T_ADDR, 0)); E(b, a64_bfi(0, T_A, 0, 8));
            return 0;
        }
        if (op >= 0xA4 && op <= 0xAF && op != 0xA8 && op != 0xA9) {    // MOVS CMPS STOS LODS SCAS
            if (d->prefixes & (X86_P_ADDRSIZE | X86_P_SEG_FS | X86_P_SEG_GS)) return -1;
            string_op(b, d, op);
            return 0;
        }
        if (op == 0xFE || op == 0xFF) {                                  // INC DEC
            int sz = op == 0xFE ? 1 : size, g = reg & 7;
            if (g > 1) return -1;
            rd_rm(b, d, T_A, sz, next); E(b, a64_movz(T_B, 1, 0));
            E(b, g ? a64_sub(T_R, T_A, T_B) : a64_add(T_R, T_A, T_B));
            wr_rm(b, d, T_R, sz); flags_arith(b, g ? K_SUB : K_ADD, sz, 1);
            return 0;
        }
        if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) {    // shifts and rotates
            int sz = (op & 1) ? size : 1, g = reg & 7;
            if (g == 2 || g == 3) {                                      // RCL RCR (in C)
                rd_rm(b, d, T_A, sz, next);
                if (op >= 0xD2) E(b, a64_ubfx(T_B, 1, 0, sz == 8 ? 6 : 5));
                else mov_imm(b, T_B, (uint64_t)((op >= 0xD0 ? 1 : (int)d->imm) & (sz == 8 ? 63 : 31)));
                mov_imm(b, S1, (uint32_t)(g == 2 ? H_RCL : H_RCR) | (uint32_t)sz << 8);
                E(b, a64_orr_lsl(S1, S1, T_B, 16));
                call_helper_r(b, 0, S1, T_A);
                E(b, a64_ldr(8, T_R, XC, CTX_HELPER_VAL));
                wr_rm(b, d, T_R, sz);
                return 0;
            }
            shift(b, d, g, sz, op >= 0xD2, op >= 0xD0 ? 1 : (int)d->imm, next);
            return 0;
        }
        if (op >= 0x50 && op <= 0x57) {                                  // PUSH reg
            int r = (op & 7) | ((d->rex & 1) << 3);
            E(b, a64_mov(T_A, r)); E(b, a64_subi(4, 4, 8)); E(b, a64_str(8, T_A, 4, 0));
            return 0;
        }
        if (op >= 0x58 && op <= 0x5F) {                                  // POP reg
            int r = (op & 7) | ((d->rex & 1) << 3);
            E(b, a64_ldr(8, T_A, 4, 0)); E(b, a64_addi(4, 4, 8)); E(b, a64_mov(r, T_A));
            return 0;
        }
        if (op == 0x68 || op == 0x6A) {                                  // PUSH imm
            mov_imm(b, T_A, (uint64_t)d->imm); E(b, a64_subi(4, 4, 8)); E(b, a64_str(8, T_A, 4, 0));
            return 0;
        }
        if (op == 0x9C || op == 0x9D) {                                  // PUSHFQ POPFQ (no 16-bit forms)
            if (size == 2) return -1;
            if (op == 0x9C) { mov_imm(b, S0, 0x202); E(b, a64_orr(T_A, XF, S0)); E(b, a64_subi(4, 4, 8)); E(b, a64_str(8, T_A, 4, 0)); }
            else {                                                       // (what a program may change: the arithmetic flags,
                E(b, a64_ldr(8, T_A, 4, 0)); E(b, a64_addi(4, 4, 8));    //   TF DF NT AC ID; TF does not trap here)
                mov_imm(b, S0, 0x8D5 | 0x100 | 0x400 | 0x4000 | 0x40000 | 0x200000); E(b, a64_and(XF, T_A, S0));
            }
            return 0;
        }
        if (op == 0xF8 || op == 0xF9 || op == 0xF5) {                    // CLC STC CMC
            E(b, a64_movz(S0, 1, 0));
            E(b, op == 0xF8 ? a64_bic(XF, XF, S0) : op == 0xF9 ? a64_orr(XF, XF, S0) : a64_eor(XF, XF, S0));
            return 0;
        }
        if (op == 0xFC || op == 0xFD) {                                  // CLD STD
            mov_imm(b, S0, F_DF); E(b, op == 0xFC ? a64_bic(XF, XF, S0) : a64_orr(XF, XF, S0));
            return 0;
        }
        if (op == 0x9F) {                                                // LAHF
            mov_imm(b, S0, 0xD5); E(b, a64_and(T_A, XF, S0)); mov_imm(b, S0, 2); E(b, a64_orr(T_A, T_A, S0));
            E(b, a64_bfi(0, T_A, 8, 8));
            return 0;
        }
        if (op >= 0xD8 && op <= 0xDF) {                                  // x87: in software (x87.c)
            if (!x87_supported(op, d->modrm)) return -1;
            if (d->mod != 3) ea(b, d, next, 0);
            call_helper(b, H_X87 | (uint32_t)op << 8 | (uint32_t)d->modrm << 16, d->mod != 3 ? T_ADDR : XZR);
            return 0;
        }
        if (op == 0x9B) return 0;                                        // FWAIT (no #MF: nothing waits)
        if (op == 0x9E) {                                                // SAHF
            E(b, a64_ubfx(T_A, 0, 8, 8)); mov_imm(b, S0, 0xD5); E(b, a64_and(T_A, T_A, S0));
            E(b, a64_bic(XF, XF, S0)); E(b, a64_orr(XF, XF, T_A));
            return 0;
        }
        return -1;
    }
    if (d->map == X86_MAP_0F || d->map == X86_MAP_0F38 || d->map == X86_MAP_0F3A) {
        int r = xlat_sse(b, d, next);                                    // SSE / SSE2 (xlat_sse.inc); 1: not one
        if (r != 1) return r;
    }
    if (d->map == X86_MAP_0F38 && op >= 0xF0) {                          // CRC32 MOVBE ADCX ADOX
        int pf = d->mandatory;
        if ((op == 0xF0 || op == 0xF1) && pf == 0xF2) {                   // CRC32 r32/r64, r/m8/16/32/64 (Castagnoli: ARM's CRC32C)
            int sz = op == 0xF0 ? 1 : size;
            rd_rm(b, d, T_A, sz, next); rd_reg(b, d, T_B, reg, 4);
            E(b, a64_crc32c(sz, T_R, T_B, T_A)); wr_reg(b, d, reg, T_R, 4);
            return 0;
        }
        if ((op == 0xF0 || op == 0xF1) && d->mod != 3 && pf != 0xF3) {  // MOVBE: loads and stores byte-swapped
            if (op == 0xF0) { rd_rm(b, d, T_A, size, next); }
            else { rd_reg(b, d, T_A, reg, size); ea(b, d, next, 0); }
            E(b, size == 8 ? a64_rev(T_A, T_A) : a64_revw(T_A, T_A));
            if (size == 2) E(b, a64_lsri(T_A, T_A, 16));
            if (op == 0xF0) wr_reg(b, d, reg, T_A, size); else wr_rm(b, d, T_A, size);
            return 0;
        }
        if (op == 0xF6 && (pf == 0x66 || pf == 0xF3) && !(d->prefixes & X86_P_VEX)) {   // ADCX (CF) ADOX (OF): that flag only
            int bit = pf == 0x66 ? 0 : 11;
            size = d->rex & 8 ? 8 : 4;                                    // (ADCX's 66 is part of the opcode, not a size)
            rd_rm(b, d, T_B, size, next); rd_reg(b, d, T_A, reg, size);
            E(b, a64_ubfx(S0, XF, bit, 1)); E(b, a64_add(T_R, T_A, T_B)); E(b, a64_add(T_R, T_R, S0));
            if (size == 8) {                                              // carry out: T_R < T_A, or equal with a carry in
                E(b, a64_cmp(T_R, T_A)); E(b, a64_cset(T_T, CC)); E(b, a64_csel(T_T, S0, T_T, EQ));
            } else E(b, a64_lsri(T_T, T_R, 32));
            E(b, a64_bfi(XF, T_T, bit, 1));
            wr_reg(b, d, reg, T_R, size);
            return 0;
        }
        return -1;
    }
    if (d->map == X86_MAP_0F) {
        if (op >= 0x40 && op <= 0x4F) {                                 // CMOVcc
            rd_rm(b, d, T_B, size, next); rd_reg(b, d, T_A, reg, size);
            cond(b, op & 15); E(b, a64_cmp(S0, XZR)); E(b, a64_csel(T_R, T_B, T_A, NE));
            wr_reg(b, d, reg, T_R, size);
            return 0;
        }
        if (op >= 0x90 && op <= 0x9F) {                                 // SETcc
            if (d->mod != 3) ea(b, d, next, 0);
            cond(b, op & 15); wr_rm(b, d, S0, 1);
            return 0;
        }
        if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF) {     // MOVZX / MOVSX
            int from = (op & 1) ? 2 : 1;
            rd_rm(b, d, T_A, from, next);
            if (op >= 0xBE) sext(b, T_A, T_A, from);
            wr_reg(b, d, reg, T_A, size);
            return 0;
        }
        if (op == 0xAF) {                                                // IMUL reg, r/m
            rd_reg(b, d, T_A, reg, size); rd_rm(b, d, T_B, size, next);
            goto imul;
        }
        if (op >= 0xC8) {                                                // BSWAP
            int r = (op & 7) | ((d->rex & 1) << 3);
            if (size == 2) return -1;
            E(b, size == 8 ? a64_rev(r, r) : a64_revw(r, r));
            return 0;
        }
        if (op == 0xBC || op == 0xBD || op == 0xB8) {                   // BSF BSR / TZCNT LZCNT / POPCNT
            int f3 = d->mandatory == 0xF3;
            if (op == 0xB8 && !f3) return -1;
            rd_rm(b, d, T_B, size, next);
            E(b, a64_movz(ACC, 0, 0));
            if (op == 0xB8) {
                E(b, a64_fmov_dx(31, T_B)); E(b, a64_cnt8b(31, 31)); E(b, a64_addv8b(31, 31)); E(b, a64_umov_b0(T_R, 31));
                E(b, a64_cmp(T_B, XZR)); E(b, a64_cset(S0, EQ)); put_flag(b, S0, 6);
                wr_reg(b, d, reg, T_R, size); merge_flags(b, F_ARITH);
                return 0;
            }
            if (op == 0xBC) { E(b, a64_rbit(S0, T_B)); E(b, a64_clz(T_R, S0)); }
            else if (f3) { E(b, a64_clz(T_R, T_B)); if (size < 8) E(b, a64_subi(T_R, T_R, (uint32_t)(64 - bits(size)))); }
            else { mov_imm(b, S0, 63); E(b, a64_clz(S1, T_B)); E(b, a64_sub(T_R, S0, S1)); }
            if (f3) {                                                    // TZCNT / LZCNT: CF = (source == 0), ZF = (result == 0)
                if (op == 0xBC && size < 8) { E(b, a64_cmp(T_B, XZR)); mov_imm(b, S1, (uint64_t)bits(size)); E(b, a64_csel(T_R, S1, T_R, EQ)); }
                E(b, a64_cmp(T_B, XZR)); E(b, a64_cset(S0, EQ)); put_flag(b, S0, 0);
                E(b, a64_cmp(T_R, XZR)); E(b, a64_cset(S0, EQ)); put_flag(b, S0, 6);
                wr_reg(b, d, reg, T_R, size); merge_flags(b, F_ARITH);
                return 0;
            }
            E(b, a64_cmp(T_B, XZR)); E(b, a64_cset(S0, EQ)); put_flag(b, S0, 6);   // BSF / BSR: ZF = (source == 0),
            size_t skip = here(b); E(b, 0);                              // and then the destination is left alone
            wr_reg(b, d, reg, T_R, size);
            patch_cbz(b, skip, 0, T_B);
            merge_flags(b, F_ARITH);
            return 0;
        }
        if (op == 0xA3 || op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xBA) {   // BT BTS BTR BTC
            int kind = op == 0xBA ? (reg & 7) - 4 : op == 0xA3 ? 0 : op == 0xAB ? 1 : op == 0xB3 ? 2 : 3;
            if (kind < 0) return -1;
            int n = bits(size);
            if (op == 0xBA) mov_imm(b, T_B, (uint64_t)(d->imm & (n - 1)));
            else rd_reg(b, d, T_B, reg, size);
            if (d->mod == 3) rd_reg(b, d, T_A, d->rm, size);
            else {
                ea(b, d, next, 0);
                if (op != 0xBA) {                                        // (a register bit offset reaches beyond the operand)
                    sext(b, S1, T_B, size);
                    E(b, a64_asri(S1, S1, log2s(size) + 3)); E(b, a64_add_lsl(T_ADDR, T_ADDR, S1, log2s(size)));
                }
                E(b, a64_ldr(size, T_A, T_ADDR, 0));
            }
            E(b, a64_ubfx(T_B, T_B, 0, log2s(size) + 3));
            E(b, a64_lsrv(S0, T_A, T_B)); E(b, a64_movz(ACC, 0, 0)); flag_bit(b, ACC, S0, 0);
            if (kind) {
                E(b, a64_movz(S0, 1, 0)); E(b, a64_lslv(S0, S0, T_B));
                E(b, kind == 1 ? a64_orr(T_R, T_A, S0) : kind == 2 ? a64_bic(T_R, T_A, S0) : a64_eor(T_R, T_A, S0));
                wr_rm(b, d, T_R, size);
            }
            merge_flags(b, F_CF);
            return 0;
        }
        if (op == 0xC0 || op == 0xC1) {                                 // XADD
            int sz = op == 0xC0 ? 1 : size;
            rd_rm(b, d, T_A, sz, next); rd_reg(b, d, T_B, reg, sz);
            E(b, a64_add(T_R, T_A, T_B));
            if (b->lock) { wr_rm(b, d, T_R, sz); wr_reg(b, d, reg, T_A, sz); }   // (memory first: a retry rereads the source)
            else { wr_reg(b, d, reg, T_A, sz); wr_rm(b, d, T_R, sz); }
            flags_arith(b, K_ADD, sz, 0);
            return 0;
        }
        if ((op == 0xB0 || op == 0xB1) && b->lock) {                    // LOCK CMPXCHG: CASAL
            int sz = op == 0xB0 ? 1 : size;
            b->lock = 0;
            rd_reg(b, d, T_B, 0, sz); rd_reg(b, d, T_R, reg, sz); ea(b, d, next, 0);
            size_t slow = sz > 1 ? if_misaligned(b, sz) : 0;
            E(b, a64_mov(T_A, T_B)); E(b, a64_casal(sz, T_A, T_R, T_ADDR));    // T_A: the destination's old value
            if (sz > 1) {
                size_t done = here(b); E(b, 0);
                patch_cbz(b, slow, 1, S0);
                split_lock_take(b);
                E(b, a64_ldr(sz, T_A, T_ADDR, 0)); E(b, a64_cmp(T_A, T_B));
                E(b, a64_bcond(NE, 2)); E(b, a64_str(sz, T_R, T_ADDR, 0));
                split_lock_give(b);
                patch_b(b, done);
            }
            E(b, a64_sub(T_R, T_A, T_B)); flags_arith(b, K_SUB, sz, 0);        // (flags: destination - accumulator)
            flag_bit(b, S0, XF, 6);
            size_t ne = here(b); E(b, 0);
            if (sz == 4) wr_reg(b, d, 0, T_B, 4);                           // equal: (32-bit accumulator: upper half cleared)
            size_t end = here(b); E(b, 0);
            patch_cbz(b, ne, 0, S0);
            wr_reg(b, d, 0, T_A, sz);                                    // not equal: accumulator = destination
            patch_b(b, end);
            return 0;
        }
        if (op == 0xB0 || op == 0xB1) {                                 // CMPXCHG
            int sz = op == 0xB0 ? 1 : size;
            // Flags as for destination - accumulator: what Rosetta 2 gives (the SDM only says "the comparison").
            rd_rm(b, d, T_A, sz, next); rd_reg(b, d, T_B, 0, sz);
            E(b, a64_sub(T_R, T_A, T_B));
            flags_arith(b, K_SUB, sz, 0);
            E(b, a64_mov(T_T, T_A)); E(b, a64_mov(T_A, T_B)); E(b, a64_mov(T_B, T_T));   // (T_A: accumulator, T_B: destination)
            flag_bit(b, S0, XF, 6);
            size_t ne = here(b); E(b, 0);                                // CBZ ZF -> not equal
            if (sz == 4) wr_reg(b, d, 0, T_A, 4);                        // equal: a 32-bit accumulator's upper half cleared (as
            rd_reg(b, d, T_R, reg, sz); wr_rm(b, d, T_R, sz);            //  Rosetta 2 does), then destination = source
            size_t end = here(b); E(b, 0);
            patch_cbz(b, ne, 0, S0);
            wr_rm(b, d, T_B, sz);                                        // not equal: the destination is written back (a 32-bit register
            wr_reg(b, d, 0, T_B, sz);                                    //   too: upper half cleared), the accumulator gets it
            patch_b(b, end);
            return 0;
        }
        if (op == 0xA4 || op == 0xA5) { shld(b, d, 0, op == 0xA5, (int)d->imm, next); return 0; }   // SHLD
        if (op == 0xAC || op == 0xAD) { shld(b, d, 1, op == 0xAD, (int)d->imm, next); return 0; }   // SHRD
        if (op == 0xC7 && (reg & 7) == 1 && d->mod != 3) { cmpxchg8_16(b, d, next); return 0; }   // CMPXCHG8B / 16B
        if (op == 0xC7 && d->mod == 3 && ((reg & 7) == 6 || (reg & 7) == 7)) {   // RDRAND RDSEED: always a value (CF 1)
            call_helper(b, H_RAND, XZR);
            E(b, a64_ldr(8, T_A, XC, CTX_HELPER_VAL)); wr_reg(b, d, d->rm, T_A, size);
            mov_imm(b, S0, 0x8D5); E(b, a64_bic(XF, XF, S0)); E(b, a64_movz(S0, 1, 0)); E(b, a64_orr(XF, XF, S0));   // (OF SF ZF AF PF 0)
            return 0;
        }
        if (op == 0xC3 && d->mod != 3) { rd_reg(b, d, T_A, reg, size); ea(b, d, next, 0); wr_rm(b, d, T_A, size); return 0; }   // MOVNTI
        if (op == 0x1F || op == 0x0D || op == 0x18) return 0;            // NOP r/m, PREFETCH
        return -1;
    }
    return -1;

imul: {
        // IMUL reg, r/m [, imm]: T_A, T_B loaded; CF = OF = the product does not fit.
        int msb_shift = bits(size);
        E(b, a64_movz(ACC, 0, 0));
        if (size == 8) {
            E(b, a64_mul(T_R, T_A, T_B)); E(b, a64_smulh(S1, T_A, T_B)); E(b, a64_asri(S0, T_R, 63));
            E(b, a64_cmp(S1, S0));
        } else {
            sext(b, T_A, T_A, size); sext(b, T_B, T_B, size);
            E(b, a64_mul(T_R, T_A, T_B)); E(b, a64_sbfx(S0, T_R, 0, msb_shift)); E(b, a64_cmp(S0, T_R));
        }
        E(b, a64_cset(S0, NE)); put_flag(b, S0, 0); put_flag(b, S0, 11);
        wr_reg(b, d, d->reg, T_R, size);
        merge_flags(b, F_ARITH);
        return 0;
    }
}

// ---- blocks and control flow ---------------------------------------------------------------------------------------
// The next RIP into S1 for a control transfer; returns 1 if d is one, 0 if not, -1 if unsupported.
static int control(xlat_buf *b, const x86_insn *d, uint64_t rip) {
    uint64_t next = rip + d->len, target = next + (uint64_t)d->imm;
    int op = d->opcode;
    if (d->map == X86_MAP_1) {
        if (op >= 0x70 && op <= 0x7F) goto jcc;
        if (op == 0xEB || op == 0xE9) { mov_imm(b, S1, target); return 1; }        // JMP rel
        if (op == 0xE8) {                                                          // CALL rel32
            mov_imm(b, T_A, next); E(b, a64_subi(4, 4, 8)); E(b, a64_str(8, T_A, 4, 0));
            mov_imm(b, S1, target); return 1;
        }
        if (op == 0xC3 || op == 0xC2) {                                            // RET [imm16]
            E(b, a64_ldr(8, S1, 4, 0));
            uint64_t pop = 8 + (op == 0xC2 ? (uint64_t)(uint16_t)d->imm : 0);
            if (pop < 4096) E(b, a64_addi(4, 4, (uint32_t)pop)); else { mov_imm(b, S0, pop); E(b, a64_add(4, 4, S0)); }
            return 1;
        }
        if (op == 0xFF && ((d->reg & 7) == 2 || (d->reg & 7) == 4)) {              // CALL / JMP r/m64
            rd_rm(b, d, T_A, 8, next);
            if ((d->reg & 7) == 2) { mov_imm(b, T_B, next); E(b, a64_subi(4, 4, 8)); E(b, a64_str(8, T_B, 4, 0)); }
            E(b, a64_mov(S1, T_A)); return 1;
        }
        if (op >= 0xE0 && op <= 0xE3) {                                            // LOOPNE LOOPE LOOP JRCXZ
            if (d->prefixes & X86_P_ADDRSIZE) return -1;
            if (op != 0xE3) E(b, a64_subi(1, 1, 1));
            E(b, a64_cmp(1, XZR)); E(b, a64_cset(S0, op == 0xE3 ? EQ : NE));
            if (op != 0xE2 && op != 0xE3) {                                        // (and ZF, or not ZF)
                flag_bit(b, S1, XF, 6);
                if (op == 0xE0) { E(b, a64_movz(T_T, 1, 0)); E(b, a64_sub(S1, T_T, S1)); }
                E(b, a64_and(S0, S0, S1));
            }
            goto select;
        }
        return 0;
    }
    if (d->map == X86_MAP_0F && op >= 0x80 && op <= 0x8F) goto jcc;
    return 0;
jcc:
    cond(b, op & 15);
select:
    mov_imm(b, S1, target); mov_imm(b, T_T, next);
    E(b, a64_cmp(S0, XZR)); E(b, a64_csel(S1, S1, T_T, NE));
    return 1;
}

int xlat_block(xlat_buf *b, uint64_t rip, int max_insns) {
    size_t exits[64]; int nexits = 0;                    // branches to the common exit (after a fault)
    xlat_prologue(b);
    for (int i = 0; i < max_insns; i++) {
        x86_insn d;
        if (!x86_decode((const uint8_t *)(uintptr_t)rip, 15, &d)) return -1;
        int c = control(b, &d, rip);
        if (c < 0) return -1;
        if (c) { E(b, a64_str(8, S1, XC, CTX_RIP)); break; }
        if (xlat_insn(b, &d, (const uint8_t *)(uintptr_t)rip, rip) < 0) return -1;
        if (d.map == X86_MAP_1 && (d.opcode == 0xF6 || d.opcode == 0xF7) && (d.reg & 7) >= 6 && nexits < 64) {
            // DIV / IDIV may raise #DE: then the block ends here, RIP at the instruction.
            E(b, a64_ldr(8, S0, XC, CTX_FAULT));
            size_t ok = here(b); E(b, 0);
            mov_imm(b, S1, rip); E(b, a64_str(8, S1, XC, CTX_RIP));
            exits[nexits++] = here(b); E(b, 0);
            patch_cbz(b, ok, 0, S0);
        }
        rip += d.len;
        if (i + 1 == max_insns) { mov_imm(b, S1, rip); E(b, a64_str(8, S1, XC, CTX_RIP)); }
    }
    for (int i = 0; i < nexits; i++) patch_b(b, exits[i]);
    xlat_epilogue(b);
    return 0;
}

void xlat_prologue(xlat_buf *b) {
    E(b, a64_stp_pre(29, 30, SP, -176));
    E(b, a64_stp(19, 20, SP, 16)); E(b, a64_stp(21, 22, SP, 32)); E(b, a64_stp(23, 24, SP, 48));
    E(b, a64_stp(25, 26, SP, 64)); E(b, a64_stp(27, 28, SP, 80));
    E(b, a64_stpd(8, 9, SP, 96)); E(b, a64_stpd(10, 11, SP, 112)); E(b, a64_stpd(12, 13, SP, 128)); E(b, a64_stpd(14, 15, SP, 144));
    E(b, a64_mov(XC, 0));
    mov_imm(b, XP, (uint64_t)(uintptr_t)xlat_parity);
    E(b, a64_ldr(8, XF, XC, CTX_FLAGS));
    for (int r = 0; r < 16; r += 2) E(b, a64_ldp(r, r + 1, XC, r * 8));
    for (int r = 0; r < 16; r += 2) E(b, a64_ldpq(r, r + 1, XC, CTX_XMM + r * 16));
    E(b, a64_mrs_fpcr(T_A)); E(b, a64_str(8, T_A, SP, 160));             // (the host's FPCR, back in the epilogue)
    E(b, a64_ldr(4, T_A, XC, CTX_MXCSR)); mxcsr_to_fpcr(b, T_B, T_A); E(b, a64_msr_fpcr(T_B)); E(b, a64_msr_fpsr(XZR));
}
void xlat_epilogue(xlat_buf *b) {
    for (int r = 0; r < 16; r += 2) E(b, a64_stp(r, r + 1, XC, r * 8));
    for (int r = 0; r < 16; r += 2) E(b, a64_stpq(r, r + 1, XC, CTX_XMM + r * 16));
    E(b, a64_str(8, XF, XC, CTX_FLAGS));
    mxcsr_sync(b);
    E(b, a64_ldr(8, T_A, SP, 160)); E(b, a64_msr_fpcr(T_A));
    E(b, a64_ldp(19, 20, SP, 16)); E(b, a64_ldp(21, 22, SP, 32)); E(b, a64_ldp(23, 24, SP, 48));
    E(b, a64_ldp(25, 26, SP, 64)); E(b, a64_ldp(27, 28, SP, 80));
    E(b, a64_ldpd(8, 9, SP, 96)); E(b, a64_ldpd(10, 11, SP, 112)); E(b, a64_ldpd(12, 13, SP, 128)); E(b, a64_ldpd(14, 15, SP, 144));
    E(b, a64_ldp_post(29, 30, SP, 176));
    E(b, a64_ret());
}
