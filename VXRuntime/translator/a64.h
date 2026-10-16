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
// An offset the encoding cannot hold is a translator bug: stop at once (it would wrap and address other memory).
static inline uint32_t a64_off_(int ok) { if (!ok) __builtin_trap(); return 0; }
static inline uint32_t a64_stp(int t1, int t2, int n, int imm) { return a64_off_(imm >= -512 && imm <= 504 && !(imm & 7)) | 0xA9000000u | ((uint32_t)(imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static inline uint32_t a64_ldp(int t1, int t2, int n, int imm) { return a64_off_(imm >= -512 && imm <= 504 && !(imm & 7)) | 0xA9400000u | ((uint32_t)(imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static inline uint32_t a64_add_lsl(int d, int n, int m, int sh) { return 0x8B000000u | (uint32_t)m << 16 | (uint32_t)sh << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_addi(int d, int n, uint32_t imm12) { return 0x91000000u | (imm12 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_subi(int d, int n, uint32_t imm12) { return 0xD1000000u | (imm12 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_cmp(int n, int m) { return a64_subs(XZR, n, m); }
static inline uint32_t a64_addsi(int d, int n, uint32_t imm12) { return 0xB1000000u | (imm12 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_subsi(int d, int n, uint32_t imm12) { return 0xF1000000u | (imm12 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_cmpi(int n, uint32_t imm12) { return 0xF100001Fu | (imm12 & 0xFFF) << 10 | (uint32_t)n << 5; }
static inline uint32_t a64_cmni(int n, uint32_t imm12) { return 0xB100001Fu | (imm12 & 0xFFF) << 10 | (uint32_t)n << 5; }

// Bitfields: UBFM / SBFM / BFM (64-bit, N = 1)
static inline uint32_t a64_ubfm(int d, int n, int immr, int imms) { return 0xD3400000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_sbfm(int d, int n, int immr, int imms) { return 0x93400000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_bfm(int d, int n, int immr, int imms) { return 0xB3400000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_ubfm_w(int d, int n, int immr, int imms) { return 0x53000000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_sbfm_w(int d, int n, int immr, int imms) { return 0x13000000u | (uint32_t)immr << 16 | (uint32_t)imms << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_w32(uint32_t insn) { return insn & 0x7FFFFFFFu; }   // (the 32-bit form: ADD SUB AND ORR EOR, register or immediate)
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
    return a64_off_(!(off % (uint32_t)size) && off / (uint32_t)size < 4096) | op[size] | (off / (uint32_t)size & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t;
}
static inline uint32_t a64_str(int size, int t, int n, uint32_t off) {
    static const uint32_t op[9] = { 0, 0x39000000u, 0x79000000u, 0, 0xB9000000u, 0, 0, 0, 0xF9000000u };
    return a64_off_(!(off % (uint32_t)size) && off / (uint32_t)size < 4096) | op[size] | (off / (uint32_t)size & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t;
}
// Loads and stores at [n + off]: LDR / STR's scaled unsigned offset when it fits, else LDUR / STUR's signed 9 bits (the
// caller checks off is one of the two). sx: sign-extending load (LDRSB LDRSH LDRSW) to 64 bits (wide) or 32.
static inline int a64_off_scaled(int32_t off, int size) { return off >= 0 && off % size == 0 && off / size < 4096; }
static inline uint32_t a64_ld_off(int size, int t, int n, int32_t off) {
    static const uint32_t lg[9] = { 0, 0, 1, 0, 2, 0, 0, 0, 3 };
    if (a64_off_scaled(off, size)) return a64_ldr(size, t, n, (uint32_t)off);
    return 0x38400000u | lg[size] << 30 | ((uint32_t)off & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t;
}
static inline uint32_t a64_st_off(int size, int t, int n, int32_t off) {
    static const uint32_t lg[9] = { 0, 0, 1, 0, 2, 0, 0, 0, 3 };
    if (a64_off_scaled(off, size)) return a64_str(size, t, n, (uint32_t)off);
    return 0x38000000u | lg[size] << 30 | ((uint32_t)off & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t;
}
static inline uint32_t a64_ldsx_off(int size, int wide, int t, int n, int32_t off) {   // size 1 2 4 (4: wide only)
    uint32_t lg = size == 4 ? 2 : size == 2 ? 1 : 0, opc = wide ? 0x00800000u : 0x00C00000u;
    if (a64_off_scaled(off, size)) return 0x39000000u | lg << 30 | opc | ((uint32_t)off / (uint32_t)size & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t;
    return 0x38000000u | lg << 30 | opc | ((uint32_t)off & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t;
}
static inline uint32_t a64_ldv_off(int size, int t, int n, int32_t off) {   // S D Q
    if (a64_off_scaled(off, size)) return (size == 4 ? 0xBD400000u : size == 8 ? 0xFD400000u : 0x3DC00000u) | ((uint32_t)off / (uint32_t)size & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t;
    return (size == 4 ? 0xBC400000u : size == 8 ? 0xFC400000u : 0x3CC00000u) | ((uint32_t)off & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t;
}
static inline uint32_t a64_stv_off(int size, int t, int n, int32_t off) {
    if (a64_off_scaled(off, size)) return (size == 4 ? 0xBD000000u : size == 8 ? 0xFD000000u : 0x3D800000u) | ((uint32_t)off / (uint32_t)size & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t;
    return (size == 4 ? 0xBC000000u : size == 8 ? 0xFC000000u : 0x3C800000u) | ((uint32_t)off & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t;
}
static inline uint32_t a64_str8_pre(int t, int n, int imm9) { return 0xF8000C00u | ((uint32_t)imm9 & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t; }   // [n, #imm]!
static inline uint32_t a64_ldr8_post(int t, int n, int imm9) { return 0xF8400400u | ((uint32_t)imm9 & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t; }  // [n], #imm
static inline uint32_t a64_ld_post(int size, int t, int n, int imm9) {     // [n], #imm (zero-extending)
    static const uint32_t op[9] = { 0, 0x38400400u, 0x78400400u, 0, 0xB8400400u, 0, 0, 0, 0xF8400400u };
    return op[size] | ((uint32_t)imm9 & 0x1FF) << 12 | (uint32_t)n << 5 | (uint32_t)t;
}
static inline uint32_t a64_ldrb_reg(int t, int n, int m) { return 0x38606800u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_stp_pre(int t1, int t2, int n, int imm) { return 0xA9800000u | ((uint32_t)(imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static inline uint32_t a64_ldp_post(int t1, int t2, int n, int imm) { return 0xA8C00000u | ((uint32_t)(imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }

// Atomics (LSE), acquire + release: CASAL s(expected; returns the old value), t(new), [n]; SWPAL s(new), t(old), [n]
static inline uint32_t a64_lse_size(int size) { return (uint32_t)(size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0) << 30; }
static inline uint32_t a64_casal(int size, int s, int t, int n) { return a64_lse_size(size) | 0x08E0FC00u | (uint32_t)s << 16 | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_swpal(int size, int s, int t, int n) { return a64_lse_size(size) | 0x38E08000u | (uint32_t)s << 16 | (uint32_t)n << 5 | (uint32_t)t; }

static inline uint32_t a64_stlr(int t, int n) { return 0xC89FFC00u | (uint32_t)n << 5 | (uint32_t)t; }   // store-release, 64-bit

// SIMD helpers for population count: FMOV d, x / CNT v.8b / ADDV b / UMOV w, v.b[0]
static inline uint32_t a64_fmov_dx(int d, int n) { return 0x9E670000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_cnt8b(int d, int n) { return 0x0E205800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_addv8b(int d, int n) { return 0x0E31B800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_umov_b0(int d, int n) { return 0x0E013C00u | (uint32_t)n << 5 | (uint32_t)d; }

// Control flow
static inline uint32_t a64_ret(void) { return 0xD65F03C0u; }
static inline uint32_t a64_ret_r(int n) { return 0xD65F0000u | (uint32_t)n << 5; }
static inline uint32_t a64_adr(int d, int32_t bytes) { return 0x10000000u | ((uint32_t)bytes & 3) << 29 | ((uint32_t)bytes >> 2 & 0x7FFFF) << 5 | (uint32_t)d; }
static inline uint32_t a64_bfxil(int d, int n, int lsb, int width) { return a64_bfm(d, n, lsb, lsb + width - 1); }
static inline uint32_t a64_blr(int n) { return 0xD63F0000u | (uint32_t)n << 5; }
static inline uint32_t a64_b(int32_t words) { return 0x14000000u | ((uint32_t)words & 0x3FFFFFF); }
static inline uint32_t a64_bcond(int cond, int32_t words) { return 0x54000000u | ((uint32_t)words & 0x7FFFF) << 5 | (uint32_t)cond; }
static inline uint32_t a64_cbz(int t, int32_t words) { return 0xB4000000u | ((uint32_t)words & 0x7FFFF) << 5 | (uint32_t)t; }
static inline uint32_t a64_cbnz(int t, int32_t words) { return 0xB5000000u | ((uint32_t)words & 0x7FFFF) << 5 | (uint32_t)t; }

// ---- SIMD and floating point (phase 3) -------------------------------------------------------------------------------
#define A64_V3(name, op) static inline uint32_t a64_##name(int d, int n, int m) { return (op) | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }
// vector, 4S / 2D / 16B / 8H
A64_V3(fadd4s, 0x4E20D400u) A64_V3(fadd2d, 0x4E60D400u) A64_V3(fsub4s, 0x4EA0D400u) A64_V3(fsub2d, 0x4EE0D400u)
A64_V3(fmul4s, 0x6E20DC00u) A64_V3(fmul2d, 0x6E60DC00u) A64_V3(fdiv4s, 0x6E20FC00u) A64_V3(fdiv2d, 0x6E60FC00u)
A64_V3(fcmeq4s, 0x4E20E400u) A64_V3(fcmeq2d, 0x4E60E400u) A64_V3(fcmgt4s, 0x6EA0E400u) A64_V3(fcmgt2d, 0x6EE0E400u)
A64_V3(fcmge4s, 0x6E20E400u) A64_V3(fcmge2d, 0x6E60E400u)
A64_V3(vand, 0x4E201C00u) A64_V3(vorr, 0x4EA01C00u) A64_V3(veor, 0x6E201C00u) A64_V3(vbic, 0x4E601C00u)
A64_V3(vbsl, 0x6E601C00u)                                         // d = (d & n) | (~d & m)
A64_V3(add16b, 0x4E208400u) A64_V3(add8h, 0x4E608400u) A64_V3(add4s, 0x4EA08400u) A64_V3(add2d, 0x4EE08400u)
A64_V3(sub16b, 0x6E208400u) A64_V3(sub8h, 0x6E608400u) A64_V3(sub4s, 0x6EA08400u) A64_V3(sub2d, 0x6EE08400u)
A64_V3(cmeq16b, 0x6E208C00u) A64_V3(cmeq8h, 0x6E608C00u) A64_V3(cmeq4s, 0x6EA08C00u)
A64_V3(cmgt16b, 0x4E203400u) A64_V3(cmgt8h, 0x4E603400u) A64_V3(cmgt4s, 0x4EA03400u)
A64_V3(zip1_4s, 0x4E803800u) A64_V3(zip2_4s, 0x4E807800u) A64_V3(zip1_2d, 0x4EC03800u) A64_V3(zip2_2d, 0x4EC07800u)
A64_V3(zip1_16b, 0x4E003800u) A64_V3(zip2_16b, 0x4E007800u) A64_V3(zip1_8h, 0x4E403800u) A64_V3(zip2_8h, 0x4E407800u)
A64_V3(smull4s, 0x0E60C000u) A64_V3(smull2_4s, 0x4E60C000u) A64_V3(addp4s, 0x4EA0BC00u)
A64_V3(mul8h, 0x4E609C00u) A64_V3(mul4s, 0x4EA09C00u)
// scalar s / d
A64_V3(fadds, 0x1E202800u) A64_V3(faddd, 0x1E602800u) A64_V3(fsubs, 0x1E203800u) A64_V3(fsubd, 0x1E603800u)
A64_V3(fmuls, 0x1E200800u) A64_V3(fmuld, 0x1E600800u) A64_V3(fdivs, 0x1E201800u) A64_V3(fdivd, 0x1E601800u)
#undef A64_V3
static inline uint32_t a64_vmov(int d, int n) { return a64_vorr(d, n, n); }
static inline uint32_t a64_vmvn(int d, int n) { return 0x6E205800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_movi0(int d) { return 0x6F00E400u | (uint32_t)d; }                   // MOVI Vd.2D, #0
static inline uint32_t a64_fsqrt4s(int d, int n) { return 0x6EA1F800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fsqrt2d(int d, int n) { return 0x6EE1F800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fsqrts(int d, int n) { return 0x1E21C000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fsqrtd(int d, int n) { return 0x1E61C000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcmps(int n, int m) { return 0x1E202000u | (uint32_t)m << 16 | (uint32_t)n << 5; }
static inline uint32_t a64_fcmpd(int n, int m) { return 0x1E602000u | (uint32_t)m << 16 | (uint32_t)n << 5; }
// lanes: INS Vd.S[i], Vn.S[j] / Vd.D[i], Vn.D[j]; to and from general registers
static inline uint32_t a64_ins_s(int d, int i, int n, int j) { return 0x6E000400u | (uint32_t)((i << 3) | 4) << 16 | (uint32_t)(j << 2) << 11 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_ins_d(int d, int i, int n, int j) { return 0x6E000400u | (uint32_t)((i << 4) | 8) << 16 | (uint32_t)(j << 3) << 11 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fmov_ss(int d, int n) { return 0x1E204000u | (uint32_t)n << 5 | (uint32_t)d; }   // (zeroes the rest)
static inline uint32_t a64_fmov_dd(int d, int n) { return 0x1E604000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fmov_sw(int d, int n) { return 0x1E270000u | (uint32_t)n << 5 | (uint32_t)d; }   // S <- W (zeroes the rest)
static inline uint32_t a64_fmov_ws(int d, int n) { return 0x1E260000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fmov_xd(int d, int n) { return 0x9E660000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_umov_s(int d, int n, int i) { return 0x0E003C00u | (uint32_t)((i << 3) | 4) << 16 | (uint32_t)n << 5 | (uint32_t)d; }   // W <- Vn.S[i]
static inline uint32_t a64_umov_d(int d, int n, int i) { return 0x4E003C00u | (uint32_t)((i << 4) | 8) << 16 | (uint32_t)n << 5 | (uint32_t)d; }   // X <- Vn.D[i]
static inline uint32_t a64_ins_gs(int d, int i, int n) { return 0x4E001C00u | (uint32_t)((i << 3) | 4) << 16 | (uint32_t)n << 5 | (uint32_t)d; }  // Vd.S[i] <- W
static inline uint32_t a64_ins_gd(int d, int i, int n) { return 0x4E001C00u | (uint32_t)((i << 4) | 8) << 16 | (uint32_t)n << 5 | (uint32_t)d; }  // Vd.D[i] <- X
static inline uint32_t a64_dup4s(int d, int n) { return 0x4E040C00u | (uint32_t)n << 5 | (uint32_t)d; }                                          // Vd.4S <- W
static inline uint32_t a64_ext(int d, int n, int m, int imm) { return 0x6E000000u | (uint32_t)m << 16 | (uint32_t)imm << 11 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_uminv4s(int d, int n) { return 0x6EB1A800u | (uint32_t)n << 5 | (uint32_t)d; }
// shifts by immediate: 4S (n 1..32 right, 0..31 left), 8H, 2D
static inline uint32_t a64_ushr(int esize, int d, int n, int sh) { return 0x6F000400u | (uint32_t)(2 * esize - sh) << 16 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_sshr(int esize, int d, int n, int sh) { return 0x4F000400u | (uint32_t)(2 * esize - sh) << 16 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_shl(int esize, int d, int n, int sh) { return 0x4F005400u | (uint32_t)(esize + sh) << 16 | (uint32_t)n << 5 | (uint32_t)d; }
// conversions
static inline uint32_t a64_scvtf_sw(int d, int n) { return 0x1E220000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_scvtf_sx(int d, int n) { return 0x9E220000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_scvtf_dw(int d, int n) { return 0x1E620000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_scvtf_dx(int d, int n) { return 0x9E620000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtzs_ws(int d, int n) { return 0x1E380000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtzs_xs(int d, int n) { return 0x9E380000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtzs_wd(int d, int n) { return 0x1E780000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtzs_xd(int d, int n) { return 0x9E780000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtns_ws(int d, int n) { return 0x1E200000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtns_xs(int d, int n) { return 0x9E200000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtns_wd(int d, int n) { return 0x1E600000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtns_xd(int d, int n) { return 0x9E600000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvt_ds(int d, int n) { return 0x1E22C000u | (uint32_t)n << 5 | (uint32_t)d; }       // D <- S
static inline uint32_t a64_fcvt_sd(int d, int n) { return 0x1E624000u | (uint32_t)n << 5 | (uint32_t)d; }       // S <- D
static inline uint32_t a64_scvtf4s(int d, int n) { return 0x4E21D800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtzs4s(int d, int n) { return 0x4EA1B800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtns4s(int d, int n) { return 0x4E21A800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtl2d(int d, int n) { return 0x0E617800u | (uint32_t)n << 5 | (uint32_t)d; }       // 2D <- low 2S
static inline uint32_t a64_fcvtn2s(int d, int n) { return 0x0E616800u | (uint32_t)n << 5 | (uint32_t)d; }       // low 2S <- 2D (upper zeroed)
static inline uint32_t a64_sxtl2d(int d, int n) { return 0x0F20A400u | (uint32_t)n << 5 | (uint32_t)d; }        // 2D <- low 2S, signed
static inline uint32_t a64_scvtf2d(int d, int n) { return 0x4E61D800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_dup_s(int d, int n, int i) { return 0x4E000400u | (uint32_t)((i << 3) | 4) << 16 | (uint32_t)n << 5 | (uint32_t)d; }   // Vd.4S <- Vn.S[i]
static inline uint32_t a64_dup_d(int d, int n, int i) { return 0x4E000400u | (uint32_t)((i << 4) | 8) << 16 | (uint32_t)n << 5 | (uint32_t)d; }   // Vd.2D <- Vn.D[i]
static inline uint32_t a64_fcvtl2_2d(int d, int n) { return 0x4E617800u | (uint32_t)n << 5 | (uint32_t)d; }     // 2D <- high 2S
static inline uint32_t a64_fcvtn2_4s(int d, int n) { return 0x4E616800u | (uint32_t)n << 5 | (uint32_t)d; }     // high 2S <- 2D (low kept)
static inline uint32_t a64_sxtl2_2d(int d, int n) { return 0x4F20A400u | (uint32_t)n << 5 | (uint32_t)d; }      // 2D <- high 2S, signed
static inline uint32_t a64_tbl(int d, int n, int m) { return 0x4E000000u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }       // Vd.16B <- {Vn.16B}[Vm.16B]
static inline uint32_t a64_cmeq2d(int d, int n, int m) { return 0x6EE08C00u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_tbz(int t, int bit, int32_t words) { return 0x36000000u | (uint32_t)(bit >> 5) << 31 | (uint32_t)(bit & 31) << 19 | ((uint32_t)words & 0x3FFF) << 5 | (uint32_t)t; }
// size-generic vector forms (size: 0 bytes, 1 halfwords, 2 words, 3 doublewords; Q = 1 unless the name ends in _lo)
#define A64_VS3(name, op) static inline uint32_t a64_##name(int size, int d, int n, int m) { return (op) | (uint32_t)size << 22 | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }
#define A64_VS2(name, op) static inline uint32_t a64_##name(int size, int d, int n) { return (op) | (uint32_t)size << 22 | (uint32_t)n << 5 | (uint32_t)d; }
A64_VS3(sqadd, 0x4E200C00u) A64_VS3(uqadd, 0x6E200C00u) A64_VS3(sqsub, 0x4E202C00u) A64_VS3(uqsub, 0x6E202C00u)
A64_VS3(smin, 0x4E206C00u) A64_VS3(smax, 0x4E206400u) A64_VS3(umin, 0x6E206C00u) A64_VS3(umax, 0x6E206400u)
A64_VS3(urhadd, 0x6E201400u) A64_VS3(ushl, 0x6E204400u) A64_VS3(sshl, 0x4E204400u) A64_VS3(uabd, 0x6E207400u)
A64_VS3(uzp1, 0x4E001800u) A64_VS3(uzp2, 0x4E005800u) A64_VS3(trn1, 0x4E002800u) A64_VS3(trn2, 0x4E006800u)
A64_VS3(vcmgt, 0x4E203400u) A64_VS3(vcmeq, 0x6E208C00u) A64_VS3(vmul, 0x4E209C00u) A64_VS3(vadd, 0x4E208400u) A64_VS3(vsub, 0x6E208400u)
A64_VS3(sqrdmulh, 0x6E20B400u)
A64_VS3(smull_lo, 0x0E20C000u) A64_VS3(smull_hi, 0x4E20C000u) A64_VS3(umull_lo, 0x2E20C000u) A64_VS3(umull_hi, 0x6E20C000u)   // (size: of the sources)
A64_VS2(vneg, 0x6E20B800u) A64_VS2(vabs, 0x4E20B800u) A64_VS2(cmlt0, 0x4E20A800u) A64_VS2(cmeq0, 0x4E209800u) A64_VS2(uaddlp, 0x6E202800u)
A64_VS2(sqxtn_lo, 0x0E214800u) A64_VS2(sqxtn_hi, 0x4E214800u) A64_VS2(sqxtun_lo, 0x2E212800u) A64_VS2(sqxtun_hi, 0x6E212800u) A64_VS2(xtn_lo, 0x0E212800u)   // (size: of the result)
#undef A64_VS3
#undef A64_VS2
// SXTL/UXTL (SSHLL/USHLL #0) of the lower (_lo) or upper (_hi) half, size: of the sources
static inline uint32_t a64_sxtl_lo(int size, int d, int n) { return 0x0F00A400u | (0x80000u << size) | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_sxtl_hi(int size, int d, int n) { return 0x4F00A400u | (0x80000u << size) | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_uxtl_lo(int size, int d, int n) { return 0x2F00A400u | (0x80000u << size) | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_uxtl_hi(int size, int d, int n) { return 0x6F00A400u | (0x80000u << size) | (uint32_t)n << 5 | (uint32_t)d; }
// floating point, dbl: 2D (else 4S)
static inline uint32_t a64_frint(int mode, int dbl, int d, int n) {    // mode: 0 nearest even, 1 down, 2 up, 3 toward zero, 4 FPCR's, 5 FPCR's, inexact raised
    static const uint32_t op[6] = { 0x4E218800u, 0x4E219800u, 0x4EA18800u, 0x4EA19800u, 0x6EA19800u, 0x6E219800u };
    return op[mode] | (uint32_t)dbl << 22 | (uint32_t)n << 5 | (uint32_t)d;
}
static inline uint32_t a64_frecpe4s(int d, int n) { return 0x4EA1D800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_frsqrte4s(int d, int n) { return 0x6EA1D800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_frecps4s(int d, int n, int m) { return 0x4E20FC00u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_frsqrts4s(int d, int n, int m) { return 0x4EA0FC00u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtns2d(int d, int n) { return 0x4E61A800u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvtzs2d(int d, int n) { return 0x4EE1B800u | (uint32_t)n << 5 | (uint32_t)d; }
// lanes from and to general registers, by size
static inline uint32_t a64_dupg(int size, int d, int n) { return 0x4E000C00u | (1u << size) << 16 | (uint32_t)n << 5 | (uint32_t)d; }      // all lanes <- Rn
static inline uint32_t a64_insg(int size, int d, int i, int n) { return 0x4E001C00u | (uint32_t)((i << (size + 1)) | (1 << size)) << 16 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_umovg(int size, int d, int n, int i) { return (size == 3 ? 0x4E003C00u : 0x0E003C00u) | (uint32_t)((i << (size + 1)) | (1 << size)) << 16 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_insv(int size, int d, int i, int n, int j) { return 0x6E000400u | (uint32_t)((i << (size + 1)) | (1 << size)) << 16 | (uint32_t)(j << size) << 11 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fmla(int dbl, int d, int n, int m) { return (dbl ? 0x4E60CC00u : 0x4E20CC00u) | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }   // d += n * m, fused
static inline uint32_t a64_fmls(int dbl, int d, int n, int m) { return (dbl ? 0x4EE0CC00u : 0x4EA0CC00u) | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }   // d -= n * m, fused
static inline uint32_t a64_fneg(int dbl, int d, int n) { return (dbl ? 0x6EE0F800u : 0x6EA0F800u) | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_tbl2(int d, int n, int m) { return 0x4E002000u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }      // table {Vn, Vn+1}
static inline uint32_t a64_cmhi2d(int d, int n, int m) { return 0x6EE03400u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_ldrh_v(int t, int n, uint32_t off) { return 0x7D400000u | (off / 2 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_aes(int op, int d, int n) { return (0x4E284800u | (uint32_t)op << 12) | (uint32_t)n << 5 | (uint32_t)d; }   // op: 0 AESE 1 AESD 2 AESMC 3 AESIMC
static inline uint32_t a64_pmull1q(int d, int n, int m) { return 0x0EE0E000u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }   // 128 <- low 64 x low 64
static inline uint32_t a64_msr_fpcr(int t) { return 0xD51B4400u | (uint32_t)t; }
static inline uint32_t a64_mrs_fpcr(int t) { return 0xD53B4400u | (uint32_t)t; }
static inline uint32_t a64_msr_fpsr(int t) { return 0xD51B4420u | (uint32_t)t; }
static inline uint32_t a64_mrs_fpsr(int t) { return 0xD53B4420u | (uint32_t)t; }
static inline uint32_t a64_fcmpes(int n, int m) { return 0x1E202010u | (uint32_t)m << 16 | (uint32_t)n << 5; }   // signalling compares
static inline uint32_t a64_fcmped(int n, int m) { return 0x1E602010u | (uint32_t)m << 16 | (uint32_t)n << 5; }
static inline uint32_t a64_frinti_s(int d, int n) { return 0x1E27C000u | (uint32_t)n << 5 | (uint32_t)d; }      // FPCR's rounding mode
static inline uint32_t a64_frinti_d(int d, int n) { return 0x1E67C000u | (uint32_t)n << 5 | (uint32_t)d; }
// scalar compares (lane 0; the rest of d zeroed), FRINTX (FPCR's mode, inexact raised), scalar FMADD/FMSUB
static inline uint32_t a64_fcmp_s(int op, int dbl, int d, int n, int m) {   // op: 0 EQ, 1 GE, 2 GT
    static const uint32_t c[3] = { 0x5E20E400u, 0x7E20E400u, 0x7EA0E400u };
    return c[op] | (uint32_t)dbl << 22 | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d;
}
static inline uint32_t a64_vorn(int d, int n, int m) { return 0x4EE01C00u | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }   // n | ~m
static inline uint32_t a64_fcsel(int dbl, int d, int n, int m, int cond) { return (dbl ? 0x1E600C00u : 0x1E200C00u) | (uint32_t)m << 16 | (uint32_t)cond << 12 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_frintx_s(int d, int n) { return 0x1E274000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_frintx_d(int d, int n) { return 0x1E674000u | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fmadd_sc(int dbl, int d, int n, int m, int a) { return (dbl ? 0x1F400000u : 0x1F000000u) | (uint32_t)m << 16 | (uint32_t)a << 10 | (uint32_t)n << 5 | (uint32_t)d; }   // a + n*m
static inline uint32_t a64_caspal(int s, int t, int n) { return 0x4860FC00u | (uint32_t)s << 16 | (uint32_t)n << 5 | (uint32_t)t; }   // 128 bits: (s, s+1) vs [n], (t, t+1) stored
static inline uint32_t a64_shiftw(int kind, int d, int n, int m) { return (0x1AC02000u | (uint32_t)kind << 10) | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d; }   // LSLV LSRV ASRV RORV (kind 0-3), 32 bits
static inline uint32_t a64_extr(int sf, int d, int n, int m, int lsb) { return (sf ? 0x93C00000u : 0x13800000u) | (uint32_t)m << 16 | (uint32_t)lsb << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_fcvt_h(int op, int d, int n) {             // 0 FCVTL 4S<-4H, 1 FCVTL2, 2 FCVTN 4H<-4S, 3 FCVTN2
    static const uint32_t c[4] = { 0x0E217800u, 0x4E217800u, 0x0E216800u, 0x4E216800u };
    return c[op] | (uint32_t)n << 5 | (uint32_t)d;
}
static inline uint32_t a64_ldapr(int size, int t, int n) { return (size == 8 ? 0xF8BFC000u : size == 4 ? 0xB8BFC000u : size == 2 ? 0x78BFC000u : 0x38BFC000u) | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_stlr_sz(int size, int t, int n) { return (size == 8 ? 0xC89FFC00u : size == 4 ? 0x889FFC00u : size == 2 ? 0x489FFC00u : 0x089FFC00u) | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_eor_lsr(int d, int n, int m, int sh) { return 0xCA400000u | (uint32_t)m << 16 | (uint32_t)sh << 10 | (uint32_t)n << 5 | (uint32_t)d; }
static inline uint32_t a64_bl(int32_t words) { return 0x94000000u | ((uint32_t)words & 0x3FFFFFF); }
static inline uint32_t a64_hvc(int imm16) { return 0xD4000002u | ((uint32_t)imm16 & 0xFFFF) << 5; }
static inline uint32_t a64_br(int n) { return 0xD61F0000u | (uint32_t)n << 5; }
static inline uint32_t a64_crc32c(int size, int d, int n, int m) {     // CRC32CB/H/W/X
    return (size == 8 ? 0x9AC05C00u : 0x1AC05000u | (uint32_t)(size == 1 ? 0 : size == 2 ? 1 : 2) << 10) | (uint32_t)m << 16 | (uint32_t)n << 5 | (uint32_t)d;
}
// SIMD memory: [n + imm], scaled
static inline uint32_t a64_ldrq(int t, int n, uint32_t off) { return 0x3DC00000u | (off / 16 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_strq(int t, int n, uint32_t off) { return 0x3D800000u | (off / 16 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_ldrd(int t, int n, uint32_t off) { return 0xFD400000u | (off / 8 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_strd(int t, int n, uint32_t off) { return 0xFD000000u | (off / 8 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_ldrs(int t, int n, uint32_t off) { return 0xBD400000u | (off / 4 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_strs(int t, int n, uint32_t off) { return 0xBD000000u | (off / 4 & 0xFFF) << 10 | (uint32_t)n << 5 | (uint32_t)t; }
static inline uint32_t a64_ldpq(int t1, int t2, int n, int imm) { return a64_off_(imm >= -1024 && imm <= 1008 && !(imm & 15)) | 0xAD400000u | ((uint32_t)(imm / 16) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static inline uint32_t a64_stpq(int t1, int t2, int n, int imm) { return a64_off_(imm >= -1024 && imm <= 1008 && !(imm & 15)) | 0xAD000000u | ((uint32_t)(imm / 16) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static inline uint32_t a64_stpd(int t1, int t2, int n, int imm) { return a64_off_(imm >= -512 && imm <= 504 && !(imm & 7)) | 0x6D000000u | ((uint32_t)(imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
static inline uint32_t a64_ldpd(int t1, int t2, int n, int imm) { return 0x6D400000u | ((uint32_t)(imm / 8) & 0x7F) << 15 | (uint32_t)t2 << 10 | (uint32_t)n << 5 | (uint32_t)t1; }
