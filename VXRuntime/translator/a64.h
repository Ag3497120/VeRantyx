// ARM64 instruction encoders for the translator's code emitter (the A64 encodings in the Arm ARM, 64-bit forms unless
// a name ends in w). Each returns one 32-bit instruction; the emitter appends them to a code buffer.
#pragma once
#include <stdint.h>

enum { XZR = 31, SP = 31 };
enum a64_cond { EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };

// Moves
static inline uint32_t a64_movz(int d, uint16_t imm, int shift) { return 0xD2800000u | (uint32_t)(shift / 16) << 21 | (uint32_t)imm << 5 | (uint32_t)d; }
static inline uint32_t a64_movk(int d, uint16_t imm, int shift) { return 0xF2800000u | (uint32_t)(shift / 16) << 21 | (uint32_t)imm << 5 | (uint32_t)d; }
static inline uint32_t a64_movn(int d, uint16_t imm, int shift) { return 0x92800000u | (uint32_t)(shift / 16) << 21 | (uint32_t)imm << 5 | (uint32_t)d; }
static inline uint32_t a64_mov(int d, int m) { return 0xAA0003E0u | (uint32_t)m << 16 | (uint32_t)d; }       // ORR d, xzr, m
static inline uint32_t a64_movw(int d, int m) { return 0x2A0003E0u | (uint32_t)m << 16 | (uint32_t)d; }      // zero-extends

// Arithmetic and logic, shifted register (shift amount 0)
#define A64_RRR(name, op) static inline uint32_t a64_##name(int d, int n, int m) { return (op) | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }
A64_RRR(add, 0x8B000000u) A64_RRR(adds, 0xAB000000u) A64_RRR(sub, 0xCB000000u) A64_RRR(subs, 0xEB000000u)
A64_RRR(adc, 0x9A000000u) A64_RRR(adcs, 0xBA000000u) A64_RRR(sbc, 0xDA000000u) A64_RRR(sbcs, 0xFA000000u)
A64_RRR(and, 0x8A000000u) A64_RRR(ands, 0xEA000000u) A64_RRR(orr, 0xAA000000u) A64_RRR(eor, 0xCA000000u)
A64_RRR(bic, 0x8A200000u) A64_RRR(orn, 0xAA200000u)
A64_RRR(lslv, 0x9AC02000u) A64_RRR(lsrv, 0x9AC02400u) A64_RRR(asrv, 0x9AC02800u) A64_RRR(rorv, 0x9AC02C00u)
A64_RRR(udiv, 0x9AC00800u) A64_RRR(sdiv, 0x9AC00C00u)
A64_RRR(umulh, 0x9BC07C00u) A64_RRR(smulh, 0x9B407C00u)
A64_RRR(mul, 0x9B007C00u)                                         // MADD d, n, m, xzr
#undef A64_RRR
static inline uint32_t a64_msub(int d, int n, int m, int a) { return 0x9B008000u | (uint32_t)m << 16 | (uint32_t)a << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_orr_lsl(int d, int n, int m, int sh) { return 0xAA000000u | (uint32_t)m << 16 | (uint32_t)sh << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_stp(int t1, int t2, int n, int imm) { return 0xA9000000u | ((uint32_t)(imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static inline uint32_t a64_ldp(int t1, int t2, int n, int imm) { return 0xA9400000u | ((uint32_t)(imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static inline uint32_t a64_add_lsl(int d, int n, int m, int sh) { return 0x8B000000u | (uint32_t)m << 16 | (uint32_t)sh << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_addi(int d, int n, uint32_t imm12) { return 0x91000000u | (imm12 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_subi(int d, int n, uint32_t imm12) { return 0xD1000000u | (imm12 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_cmp(int n, int m) { return a64_subs(XZR, n, m); }

// Bitfields: UBFM / SBFM / BFM (64-bit, N = 1)
static inline uint32_t a64_ubfm(int d, int n, int immr, int imms) { return 0xD3400000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_sbfm(int d, int n, int immr, int imms) { return 0x93400000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_bfm(int d, int n, int immr, int imms) { return 0xB3400000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_ubfx(int d, int n, int lsb, int width) { return a64_ubfm(d, n, lsb, lsb + width - 1); }
static inline uint32_t a64_sbfx(int d, int n, int lsb, int width) { return a64_sbfm(d, n, lsb, lsb + width - 1); }
static inline uint32_t a64_bfi(int d, int n, int lsb, int width) { return a64_bfm(d, n, (64 - lsb) & 63, width - 1); }
static inline uint32_t a64_lsli(int d, int n, int sh) { return a64_ubfm(d, n, (64 - sh) & 63, 63 - sh); }
static inline uint32_t a64_lsri(int d, int n, int sh) { return a64_ubfm(d, n, sh, 63); }
static inline uint32_t a64_asri(int d, int n, int sh) { return a64_sbfm(d, n, sh, 63); }

// Bits and conditions
static inline uint32_t a64_clz(int d, int n) { return 0xDAC01000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_rbit(int d, int n) { return 0xDAC00000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_rev(int d, int n) { return 0xDAC00C00u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_revw(int d, int n) { return 0x5AC00800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_csel(int d, int n, int m, int cond) { return 0x9A800000u | (uint32_t)m << 16 | (uint32_t)cond << 12 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_csinc(int d, int n, int m, int cond) { return 0x9A800400u | (uint32_t)m << 16 | (uint32_t)cond << 12 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_cset(int d, int cond) { return a64_csinc(d, XZR, XZR, cond ^ 1); }
static inline uint32_t a64_mrs_nzcv(int t) { return 0xD53B4200u | (uint32_t)t; }
static inline uint32_t a64_msr_nzcv(int t) { return 0xD51B4200u | (uint32_t)t; }

// Memory: [n] with an unsigned, size-scaled 12-bit offset; register offset (LSL 0)
static inline uint32_t a64_ldr(int size, int t, int n, uint32_t off) {
    static const uint32_t op[9] = { 0, 0x39400000u, 0x79400000u, 0, 0xB9400000u, 0, 0, 0, 0xF9400000u };
    return op[size] | (off / (uint32_t)size & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t;
}
static inline uint32_t a64_str(int size, int t, int n, uint32_t off) {
    static const uint32_t op[9] = { 0, 0x39000000u, 0x79000000u, 0, 0xB9000000u, 0, 0, 0, 0xF9000000u };
    return op[size] | (off / (uint32_t)size & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t;
}
static inline uint32_t a64_ldrb_reg(int t, int n, int m) { return 0x38606800u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_stp_pre(int t1, int t2, int n, int imm) { return 0xA9800000u | ((uint32_t)(imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static inline uint32_t a64_ldp_post(int t1, int t2, int n, int imm) { return 0xA8C00000u | ((uint32_t)(imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }

// SIMD helpers for population count: FMOV d, x / CNT v.8b / ADDV b / UMOV w, v.b[0]
static inline uint32_t a64_fmov_dx(int d, int n) { return 0x9E670000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_cnt8b(int d, int n) { return 0x0E205800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_addv8b(int d, int n) { return 0x0E31B800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_umov_b0(int d, int n) { return 0x0E013C00u | (uint32_t)n << 5 | (uint32_t)d; }

// Control flow
static inline uint32_t a64_ret(void) { return 0xD65F03C0u; }
static inline uint32_t a64_blr(int n) { return 0xD63F0000u | (uint32_t)n << 5; }
static inline uint32_t a64_b(int32_t words) { return 0x14000000u | ((uint32_t)words & 0x3FFFFFF); }
static inline uint32_t a64_bcond(int cond, int32_t words) { return 0x54000000u | ((uint32_t)words & 0x7FFFF) << 5 | (uint32_t)cond; }
static inline uint32_t a64_cbz(int t, int32_t words) { return 0xB4000000u | ((uint32_t)words & 0x7FFFF) << 5 | (uint32_t)t; }
static inline uint32_t a64_cbnz(int t, int32_t words) { return 0xB5000000u | ((uint32_t)words & 0x7FFFF) << 5 | (uint32_t)t; }
