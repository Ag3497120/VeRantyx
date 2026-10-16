// SSE / SSE2 (phase 3a), included by xlat.c. XMM n is Vn; V16-V23 are scratch.
//
// Floating point follows x86, not ARM, where they differ:
// - NaNs: x86 returns the first operand's NaN (quieted) if it has one, else the second's; an invalid operation
//   (inf - inf, 0 * inf, 0 / 0, sqrt of a negative) gives the "indefinite" NaN, sign set (0xFFC00000). ARM prefers
//   signalling NaNs and its default NaN has the sign clear. The ARM result is checked for NaNs (one compare); only
//   then are the lanes rebuilt the x86 way.
// - MIN/MAX: x86 returns the second operand unless the first is strictly less (greater): a compare and a select,
//   which is also what it does with NaNs and signed zeros.
// - Conversions to integers: x86 gives 0x80000000 (0x8000000000000000) for NaN and out-of-range values; ARM
//   saturates. The result is checked and replaced.
// MXCSR is taken as its default (round to nearest, nothing flushed); its exception flags are not kept (yet).

enum { V_B = 16, V_R = 17, V_T = 18, V_U = 19, V_W = 20, V_X = 21 };

static uint32_t a64_dup2d(int d, int n) { return 0x4E080C00u | (uint32_t)n << 5 | (uint32_t)d; }
static uint32_t a64_ins_h(int d, int i, int n, int j) { return 0x6E000400u | (uint32_t)((i << 2) | 2) << 16 | (uint32_t)(j << 1) << 11 | (uint32_t)n << 5 | (uint32_t)d; }
static void patch_bcond(xlat_buf *b, size_t at, int cond) { b->code[at] = a64_bcond(cond, (int32_t)(b->n - at)); }
// The code after the branch at `at` (kind 0: taken on V, 1: on S0 zero), to here, moves after the block (xlat_cold_flush):
// it must not branch nor hold a site.
static void cold_from(xlat_buf *b, size_t at, int kind) {
    if (b->ncold == b->ccold) { b->ccold = b->ccold ? b->ccold * 2 : 16; b->cold = realloc(b->cold, b->ccold * sizeof *b->cold); }
    size_t n = b->n - (at + 1);
    uint32_t *w = malloc(n * 4 + 4);
    memcpy(w, b->code + at + 1, n * 4);
    b->cold[b->ncold++] = (struct cold){ at, kind, w, n };
    b->n = at + 1;
}

// The r/m operand as an XMM register number: the register itself, or V_B loaded from memory (size bytes: 2, 4, 8, 16).
static int xmm_rm(xlat_buf *b, const x86_insn *d, int size, uint64_t next) {
    if (d->mod == 3) return d->rm;
    if (size >= 4 && !(d->prefixes & X86_P_VEX)) {                       // (VEX code may count on T_ADDR after)
        int32_t off; int n = ea_fold(b, d, next, size, &off);
        E(b, a64_ldv_off(size, V_B, n, off));
        return V_B;
    }
    ea(b, d, next, 0);
    E(b, size == 16 ? a64_ldrq(V_B, T_ADDR, 0) : size == 8 ? a64_ldrd(V_B, T_ADDR, 0) : size == 2 ? a64_ldrh_v(V_B, T_ADDR, 0) : a64_ldrs(V_B, T_ADDR, 0));
    return V_B;
}

// x86 NaN results for r = op(ops[0], ops[1], ...): the first operand that is a NaN (quieted), else the indefinite NaN.
// dbl: 64-bit lanes; scalar: only lane 0 is checked.
static void nan_fixn(xlat_buf *b, int dbl, int scalar, const int *ops, int n, int r) {
    size_t skip = here(b);
    if (scalar) { E(b, dbl ? a64_fcmpd(r, r) : a64_fcmps(r, r)); skip = here(b); E(b, 0); }
    else {
        E(b, dbl ? a64_fcmeq2d(V_T, r, r) : a64_fcmeq4s(V_T, r, r)); E(b, a64_uminv4s(V_T, V_T)); E(b, a64_fmov_ws(S0, V_T));
        skip = here(b); E(b, 0);
    }
    if (dbl) { mov_imm(b, S0, 0xFFF8000000000000ull); E(b, a64_dup2d(V_U, S0)); mov_imm(b, S0, 0x0008000000000000ull); E(b, a64_dup2d(V_W, S0)); }
    else { mov_imm(b, S0, 0xFFC00000u); E(b, a64_dup4s(V_U, S0)); mov_imm(b, S0, 0x00400000u); E(b, a64_dup4s(V_W, S0)); }
    // (scalar: lane 0 compared only, so that the other lanes raise no flags)
    #define ISNAN(dst, x) do { E(b, scalar ? a64_fcmp_s(0, dbl, dst, x, x) : dbl ? a64_fcmeq2d(dst, x, x) : a64_fcmeq4s(dst, x, x)); E(b, a64_vmvn(dst, dst)); } while (0)
    ISNAN(V_X, r); E(b, a64_vbsl(V_X, V_U, r));                         // invalid: indefinite
    for (int i = n - 1; i >= 0; i--) {                                    // (the first operand last: it wins)
        ISNAN(V_T, ops[i]); E(b, a64_vorr(V_U, ops[i], V_W)); E(b, a64_vbsl(V_T, V_U, V_X)); E(b, a64_vmov(V_X, V_T));
    }
    #undef ISNAN
    if (scalar) E(b, dbl ? a64_fmov_dd(r, V_X) : a64_fmov_ss(r, V_X));   // (lane 0; the rest zero, as known when r is the
    else E(b, a64_vmov(r, V_X));                                          //  destination itself: xlat_buf's zhi)
    if (b->defer_cold) { cold_from(b, skip, scalar ? 0 : 1); return; }
    if (scalar) patch_bcond(b, skip, VC); else patch_cbz(b, skip, 1, S0);
}
// r = a op src (src < 0: unary)
static void nan_fix(xlat_buf *b, int dbl, int scalar, int a, int src, int r) {
    int ops[2] = { a, src };
    nan_fixn(b, dbl, scalar, ops, src >= 0 ? 2 : 1, r);
}

// Float to integer, x86 style: the ARM conversion (truncating, or rounded as MXCSR says) of lane 0 of v into general
// register t (size 4 or 8), and 0x80000000... for NaN and out of range. The conversion's own flags are x86's (invalid
// out of range, inexact otherwise when inexact).
static void cvt_to_int(xlat_buf *b, int t, int v, int dbl, int size, int trunc) {
    if (!trunc) {                                                         // MXCSR's rounding (no flags), then for the flags
        E(b, dbl ? a64_frinti_d(V_U, v) : a64_frinti_s(V_U, v));         //   only a conversion of the value as it was if the
        int e = size == 8 ? 63 : 31;                                      //   rounded one fits (inexact), else of the rounded
        mov_imm(b, S0, dbl ? (uint64_t)(1023 + e) << 52 : (uint64_t)(127 + e) << 23);   //   one (invalid alone, as x86)
        E(b, dbl ? a64_fmov_dx(V_W, S0) : a64_fmov_sw(V_W, S0));         // 2^e
        E(b, dbl ? a64_fcmpd(V_U, V_W) : a64_fcmps(V_U, V_W)); E(b, a64_fcsel(dbl, V_X, V_U, v, GE));
        E(b, a64_fneg(dbl, V_W, V_W));                                     // -2^e (vector FNEG: lane 0 is all that is read)
        E(b, dbl ? a64_fcmpd(V_U, V_W) : a64_fcmps(V_U, V_W)); E(b, a64_fcsel(dbl, V_X, V_U, V_X, LT));
        E(b, size == 4 ? (dbl ? a64_fcvtzs_wd(XZR, V_X) : a64_fcvtzs_ws(XZR, V_X)) : (dbl ? a64_fcvtzs_xd(XZR, V_X) : a64_fcvtzs_xs(XZR, V_X)));
        v = V_U;
    }
    mov_imm(b, S1, size == 8 ? 0x8000000000000000ull : 0x80000000ull);
    if (size == 4) {                                                      // ARM saturates: x86 wants 0x80000000 at 2^31 and up
        E(b, dbl ? a64_fcvtzs_wd(t, v) : a64_fcvtzs_ws(t, v));
        mov_imm(b, S0, dbl ? 0x41E0000000000000ull : 0x4F000000u);        // 2^31
        E(b, dbl ? a64_fmov_dx(V_W, S0) : a64_fmov_sw(V_W, S0)); E(b, dbl ? a64_fcmpd(v, V_W) : a64_fcmps(v, V_W));
        E(b, a64_csel(t, S1, t, GE));
    } else {
        E(b, dbl ? a64_fcvtzs_xd(t, v) : a64_fcvtzs_xs(t, v));
        mov_imm(b, S0, 0x7FFFFFFFFFFFFFFFull); E(b, a64_cmp(t, S0)); E(b, a64_csel(t, S1, t, EQ));   // saturated high
    }
    E(b, dbl ? a64_fcmpd(v, v) : a64_fcmps(v, v)); E(b, a64_csel(t, S1, t, VS));                     // NaN
}

// The same for 4 lanes: x86 gives 0x80000000 where the value is NaN or not in [-2^31, 2^31).
static void cvt4_to_int(xlat_buf *b, int dst, int v, int trunc) {
    if (trunc) E(b, a64_fcvtzs4s(V_R, v));
    else { E(b, a64_frint(4, 0, V_R, v)); E(b, a64_fcvtzs4s(V_R, V_R)); E(b, a64_fcvtzs4s(V_T, v)); }   // (the last: flags, as above)
    mov_imm(b, S0, 0x4F000000u); E(b, a64_dup4s(V_U, S0));                  // 2^31
    mov_imm(b, S0, 0xCF000000u); E(b, a64_dup4s(V_W, S0));                  // -2^31
    E(b, a64_fcmgt4s(V_T, V_U, v)); E(b, a64_fcmge4s(V_X, v, V_W)); E(b, a64_vand(V_T, V_T, V_X));   // in range (not NaN)
    mov_imm(b, S0, 0x80000000u); E(b, a64_dup4s(V_U, S0));
    E(b, a64_vbsl(V_T, V_R, V_U)); E(b, a64_vmov(dst, V_T));
}

// SHUFPS / PSHUFD: four 32-bit lanes picked by imm from lo (lanes 0, 1) and hi (lanes 2, 3).
static void shuf4(xlat_buf *b, int dst, int lo, int hi, int imm) {
    if (lo == hi) {                                                      // (one source: the common patterns in one instruction)
        if (imm == 0x00 || imm == 0x55 || imm == 0xAA || imm == 0xFF) { E(b, a64_dup_s(dst, lo, imm & 3)); return; }
        if (imm == 0x44 || imm == 0xEE) { E(b, a64_dup_d(dst, lo, imm == 0xEE)); return; }
        if (imm == 0x4E) { E(b, a64_ext(dst, lo, lo, 8)); return; }
        if (imm == 0xB1) { E(b, a64_rev64_4s(dst, lo)); return; }
        if (imm == 0xE4) { if (dst != lo) E(b, a64_vmov(dst, lo)); return; }
    }
    for (int i = 0; i < 4; i++) E(b, a64_ins_s(V_R, i, i < 2 ? lo : hi, imm >> (2 * i) & 3));
    E(b, a64_vmov(dst, V_R));
}

// RCPPS / RSQRTPS into r. x86 leaves the exact values to the processor (relative error at most 1.5 * 2^-12; Intel,
// AMD and Rosetta 2 all differ): here the ARM estimate and one Newton step (about 2^-16), with x86's edges: denormal
// inputs count as zero, reciprocals too small for a normal number are zero, negative numbers have the indefinite NaN
// as reciprocal square root.
static void estimate(xlat_buf *b, int r, int v, int rsqrt) {
    enum { V_Y = 22 };
    mov_imm(b, S0, 0x7F800000u); E(b, a64_dup4s(V_T, S0)); E(b, a64_vand(V_T, v, V_T)); E(b, a64_cmeq0(2, V_T, V_T));
    mov_imm(b, S0, 0x007FFFFFu); E(b, a64_dup4s(V_U, S0)); E(b, a64_vand(V_T, V_T, V_U)); E(b, a64_vbic(V_U, v, V_T));   // V_U: denormals as zeros
    if (!rsqrt) {
        E(b, a64_frecpe4s(V_W, V_U)); E(b, a64_frecps4s(V_X, V_U, V_W)); E(b, a64_fmul4s(V_W, V_W, V_X));
        mov_imm(b, S0, 0x7F800000u); E(b, a64_dup4s(V_T, S0)); E(b, a64_vand(V_T, V_W, V_T)); E(b, a64_cmeq0(2, V_T, V_T));
        mov_imm(b, S0, 0x7FFFFFFFu); E(b, a64_dup4s(V_X, S0)); E(b, a64_vand(V_T, V_T, V_X)); E(b, a64_vbic(r, V_W, V_T));
        return;
    }
    E(b, a64_frsqrte4s(V_W, V_U)); E(b, a64_fmul4s(V_X, V_U, V_W)); E(b, a64_frsqrts4s(V_X, V_X, V_W)); E(b, a64_fmul4s(V_X, V_W, V_X));
    E(b, a64_movi0(V_T)); E(b, a64_fcmgt4s(V_T, V_U, V_T));                               // the step where 0 < v < inf;
    mov_imm(b, S0, 0x7F800000u); E(b, a64_dup4s(V_Y, S0)); E(b, a64_fcmgt4s(V_Y, V_Y, V_U));   // zeros, infinities, NaNs
    E(b, a64_vand(V_T, V_T, V_Y)); E(b, a64_vbsl(V_T, V_X, V_W));                          // keep the estimate
    E(b, a64_movi0(V_Y)); E(b, a64_fcmgt4s(V_Y, V_Y, V_U));
    mov_imm(b, S0, 0xFFC00000u); E(b, a64_dup4s(V_X, S0)); E(b, a64_vbsl(V_Y, V_X, V_T)); E(b, a64_vmov(r, V_Y));
}

// v = all ones in the 32-bit lanes whose bit is set in bits (4 bits), or the 64-bit lanes (dbl, 2 bits).
static void lane_mask(xlat_buf *b, int v, int bits, int dbl) {
    uint64_t lo, hi;
    if (dbl) { lo = bits & 1 ? ~0ull : 0; hi = bits & 2 ? ~0ull : 0; }
    else { lo = (bits & 1 ? 0xFFFFFFFFull : 0) | (bits & 2 ? 0xFFFFFFFF00000000ull : 0); hi = (bits & 4 ? 0xFFFFFFFFull : 0) | (bits & 8 ? 0xFFFFFFFF00000000ull : 0); }
    mov_imm(b, S0, lo); mov_imm(b, S1, hi); E(b, a64_fmov_dx(v, S0)); E(b, a64_ins_gd(v, 1, S1));
}

// ZF (no bit of V_T set) and CF (no bit of V_U set); AF OF PF SF cleared (PTEST, VTESTPS/PD).
static void test_flags(xlat_buf *b) {
    E(b, a64_movz(ACC, 0, 0));
    for (int k = 0; k < 2; k++) {
        int v = k ? V_U : V_T;
        E(b, a64_umov_d(T_A, v, 0)); E(b, a64_umov_d(T_B, v, 1)); E(b, a64_orr(T_A, T_A, T_B));
        E(b, a64_cmp(T_A, XZR)); E(b, a64_cset(S1, EQ)); put_flag(b, S1, k ? 0 : 6);
    }
    merge_flags(b, F_ARITH);
}

static int xlat_sse4(xlat_buf *b, const x86_insn *d, uint64_t next);
// CVTPD2DQ / CVTTPD2DQ of s into the lower half of r (upper half zero): 0x80000000 where the value does not fit.
static void pd2dq(xlat_buf *b, int r, int s, int trunc) {
    E(b, a64_frint(trunc ? 3 : 4, 1, V_R, s)); E(b, a64_fcvtzs2d(V_R, V_R));   // (rounded first: no flags yet)
    E(b, a64_xtn_lo(2, V_T, V_R)); E(b, a64_sxtl_lo(2, V_U, V_T)); E(b, a64_vcmeq(3, V_U, V_U, V_R));   // fits in 32 bits
    E(b, a64_vand(V_W, s, V_U)); E(b, a64_fcvtzs2d(V_W, V_W));             // (inexact from the lanes that fit)
    E(b, a64_fcmeq2d(V_W, s, s)); E(b, a64_vand(V_U, V_U, V_W)); E(b, a64_xtn_lo(2, V_U, V_U));       // and is not NaN
    E(b, a64_umov_d(S1, V_U, 0)); E(b, a64_addi(S1, S1, 1)); size_t ok = here(b); E(b, 0);   // (all fit: ~0 + 1 = 0)
    mov_imm(b, S0, 0x41F0000000000000ull); E(b, a64_fmov_dx(V_W, S0)); E(b, a64_fcvtzs_wd(XZR, V_W));   // invalid, as x86 raises
    patch_cbz(b, ok, 0, S1);
    mov_imm(b, S0, 0x80000000u); E(b, a64_dup4s(V_W, S0)); E(b, a64_vbsl(V_U, V_T, V_W)); E(b, a64_insg(3, V_U, 1, XZR));
    E(b, a64_vmov(r, V_U));
}


// 1: not an SSE instruction; 0: translated; -1: not supported (yet).
static int xlat_sse(xlat_buf *b, const x86_insn *d, uint64_t next) {
    if (d->map == X86_MAP_0F38 || d->map == X86_MAP_0F3A) return xlat_sse4(b, d, next);
    if (d->map != X86_MAP_0F) return 1;
    int op = d->opcode, pf = d->mandatory, x = d->reg, dbl = pf == 0x66 || pf == 0xF2;
    int scalar = pf == 0xF3 || pf == 0xF2, esz = scalar ? (pf == 0xF2 ? 8 : 4) : 16;
    if (op == 0xAE && d->mod == 3 && (d->reg & 7) >= 5 && !(d->prefixes & X86_P_VEX)) { E(b, 0xD5033BBFu); return 0; }   // LFENCE MFENCE SFENCE: DMB ISH
    if (op == 0xAE && d->mod != 3 && pf == 0 && (d->reg & 7) <= 1 && !(d->prefixes & X86_P_VEX)) {   // FXSAVE FXRSTOR
        if (!(d->reg & 7)) mxcsr_sync(b);
        ea(b, d, next, 0);
        call_helper(b, (d->reg & 7) ? H_FXRSTOR : H_FXSAVE, T_ADDR);
        if (d->reg & 7) { E(b, a64_ldr(4, T_A, XC, CTX_MXCSR)); mxcsr_to_fpcr(b, T_B, T_A); E(b, a64_msr_fpcr(T_B)); E(b, a64_msr_fpsr(XZR)); }
        return 0;
    }
    if (op == 0xAE && d->mod != 3 && pf == 0 && ((d->reg & 7) == 2 || (d->reg & 7) == 3)) {
        if ((d->reg & 7) == 2) {                                          // LDMXCSR: FPCR from it, flags from it
            ea(b, d, next, 0); E(b, a64_ldr(4, T_A, T_ADDR, 0)); E(b, a64_str(4, T_A, XC, CTX_MXCSR));
            mxcsr_to_fpcr(b, T_B, T_A); E(b, a64_msr_fpcr(T_B)); E(b, a64_msr_fpsr(XZR));
        } else { mxcsr_sync(b); ea(b, d, next, 0); E(b, a64_str(4, T_A, T_ADDR, 0)); }   // STMXCSR
        return 0;
    }
    int sse_op = (op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2F) || (op >= 0x50 && op <= 0x7F) || op == 0xC2 ||
                 op == 0xC4 || op == 0xC5 || op == 0xC6 || op >= 0xD0;
    if (!sse_op) return 1;
    if (op >= 0x60 && pf == 0 && !(op >= 0x50 && op <= 0x5F) && op != 0xC2 && op != 0xC6) return -1;   // (MMX)

    switch (op) {
    case 0x10: case 0x11: {                                               // MOVUPS/UPD/SS/SD
        if (op == 0x10) {
            if (!scalar) { int s = xmm_rm(b, d, 16, next); E(b, a64_vmov(x, s)); }
            else if (d->mod == 3) E(b, pf == 0xF2 ? a64_ins_d(x, 0, d->rm, 0) : a64_ins_s(x, 0, d->rm, 0));
            else if (!(d->prefixes & X86_P_VEX)) { int32_t off; int n = ea_fold(b, d, next, esz, &off); E(b, a64_ldv_off(esz, x, n, off)); }   // (from memory:
            else { ea(b, d, next, 0); E(b, esz == 8 ? a64_ldrd(x, T_ADDR, 0) : a64_ldrs(x, T_ADDR, 0)); }                                 //  the rest zeroed)
        } else if (d->mod == 3) {
            if (!scalar) E(b, a64_vmov(d->rm, x));
            else E(b, pf == 0xF2 ? a64_ins_d(d->rm, 0, x, 0) : a64_ins_s(d->rm, 0, x, 0));
        } else if (!(d->prefixes & X86_P_VEX)) { int32_t off; int n = ea_fold(b, d, next, esz, &off); E(b, a64_stv_off(esz, x, n, off)); }
        else { ea(b, d, next, 0); E(b, esz == 16 ? a64_strq(x, T_ADDR, 0) : esz == 8 ? a64_strd(x, T_ADDR, 0) : a64_strs(x, T_ADDR, 0)); }
        return 0;
    }
    case 0x28: { int s = xmm_rm(b, d, 16, next); E(b, a64_vmov(x, s)); return 0; }                 // MOVAPS/PD
    case 0x29: if (d->mod == 3) E(b, a64_vmov(d->rm, x));
        else if (!(d->prefixes & X86_P_VEX)) { int32_t off; int n = ea_fold(b, d, next, 16, &off); E(b, a64_stv_off(16, x, n, off)); }
        else { ea(b, d, next, 0); E(b, a64_strq(x, T_ADDR, 0)); }
        return 0;
    case 0x2B: case 0xE7:                                                 // MOVNTPS/PD MOVNTDQ (the hint is dropped)
        if (d->mod == 3 || scalar || (op == 0xE7 && pf != 0x66)) return -1;
        ea(b, d, next, 0); E(b, a64_strq(x, T_ADDR, 0)); return 0;
    case 0x6F: if (pf != 0x66 && pf != 0xF3) return -1; { int s = xmm_rm(b, d, 16, next); E(b, a64_vmov(x, s)); } return 0;   // MOVDQA/U
    case 0x7F: if (pf != 0x66 && pf != 0xF3) return -1;
        if (d->mod == 3) E(b, a64_vmov(d->rm, x)); else { ea(b, d, next, 0); E(b, a64_strq(x, T_ADDR, 0)); } return 0;
    case 0x12: case 0x13: case 0x16: case 0x17: {                         // MOVLPS/PD MOVHLPS MOVHPS/PD MOVLHPS
        if (op == 0x12 && pf == 0xF2) { int s = xmm_rm(b, d, 8, next); E(b, a64_dup_d(x, s, 0)); return 0; }      // MOVDDUP
        if ((op == 0x12 || op == 0x16) && pf == 0xF3) {                   // MOVSLDUP MOVSHDUP
            int s = xmm_rm(b, d, 16, next);
            E(b, op == 0x12 ? a64_trn1(2, x, s, s) : a64_trn2(2, x, s, s));
            return 0;
        }
        if (pf != 0 && pf != 0x66) return -1;
        int high = op >= 0x16;
        if (op == 0x12 || op == 0x16) {
            if (d->mod == 3) { if (pf) return -1; E(b, a64_ins_d(x, high ? 1 : 0, d->rm, high ? 0 : 1)); }   // MOVLHPS / MOVHLPS
            else { int s = xmm_rm(b, d, 8, next); E(b, a64_ins_d(x, high, s, 0)); }
        } else {
            if (d->mod == 3) return -1;
            ea(b, d, next, 0); E(b, a64_umov_d(T_A, x, high)); E(b, a64_str(8, T_A, T_ADDR, 0));
        }
        return 0;
    }
    case 0x14: case 0x15: {                                               // UNPCKLPS/PD UNPCKHPS/PD
        if (scalar) return -1;
        int s = xmm_rm(b, d, 16, next);
        E(b, op == 0x14 ? (dbl ? a64_zip1_2d(x, x, s) : a64_zip1_4s(x, x, s)) : (dbl ? a64_zip2_2d(x, x, s) : a64_zip2_4s(x, x, s)));
        return 0;
    }
    case 0xC6: {                                                          // SHUFPS / SHUFPD
        if (scalar) return -1;
        int s = xmm_rm(b, d, 16, next), imm = (int)(uint8_t)d->imm;
        if (dbl) { E(b, a64_ins_d(V_R, 0, x, imm & 1)); E(b, a64_ins_d(V_R, 1, s, imm >> 1 & 1)); E(b, a64_vmov(x, V_R)); }
        else shuf4(b, x, x, s, imm);
        return 0;
    }
    case 0x54: case 0x55: case 0x56: case 0x57: {                         // ANDPS ANDNPS ORPS XORPS (and PD)
        if (scalar) return -1;
        int s = xmm_rm(b, d, 16, next);
        if (d->mod == 3 && s == x && (op == 0x55 || op == 0x57)) { E(b, a64_movi0(x)); return 0; }   // (zero: as for PXOR below)
        E(b, op == 0x54 ? a64_vand(x, x, s) : op == 0x55 ? a64_vbic(x, s, x) : op == 0x56 ? a64_vorr(x, x, s) : a64_veor(x, x, s));
        return 0;
    }
    case 0x58: case 0x59: case 0x5C: case 0x5E: case 0x51: {              // ADD MUL SUB DIV SQRT (ps pd ss sd)
        int s = xmm_rm(b, d, esz, next);
        if (op != 0x51 && afp(b)) {                                       // (FPCR.AH: ARM's NaNs are x86's, mxcsr_to_fpcr)
            int r = scalar && (b->zhi[dbl] >> x & 1) ? x : V_R;          // (the rest of x known zero: straight into it)
            switch (op) {
            case 0x58: E(b, scalar ? (dbl ? a64_faddd(r, x, s) : a64_fadds(r, x, s)) : dbl ? a64_fadd2d(x, x, s) : a64_fadd4s(x, x, s)); break;
            case 0x59: E(b, scalar ? (dbl ? a64_fmuld(r, x, s) : a64_fmuls(r, x, s)) : dbl ? a64_fmul2d(x, x, s) : a64_fmul4s(x, x, s)); break;
            case 0x5C: E(b, scalar ? (dbl ? a64_fsubd(r, x, s) : a64_fsubs(r, x, s)) : dbl ? a64_fsub2d(x, x, s) : a64_fsub4s(x, x, s)); break;
            case 0x5E: E(b, scalar ? (dbl ? a64_fdivd(r, x, s) : a64_fdivs(r, x, s)) : dbl ? a64_fdiv2d(x, x, s) : a64_fdiv4s(x, x, s)); break;
            }
            if (scalar && r != x) E(b, dbl ? a64_ins_d(x, 0, V_R, 0) : a64_ins_s(x, 0, V_R, 0));
            return 0;
        }
        if (op == 0x51) {
            if (scalar) E(b, dbl ? a64_fsqrtd(V_R, s) : a64_fsqrts(V_R, s));   // (scalar instructions: no flags from other lanes)
            else E(b, dbl ? a64_fsqrt2d(V_R, s) : a64_fsqrt4s(V_R, s));
            E(b, a64_vmov(22, s));                                        // (nan_fix uses V_T to V_X)
            nan_fix(b, dbl, scalar, 22, -1, V_R);
        } else if (scalar && (b->zhi[dbl] >> x & 1)) {                 // (the rest of x known zero: the result straight into
            E(b, a64_vmov(V_R, x));                                       //  it, a copy of x for NaNs off the dependency chain)
            switch (op) {
            case 0x58: E(b, dbl ? a64_faddd(x, x, s) : a64_fadds(x, x, s)); break;
            case 0x59: E(b, dbl ? a64_fmuld(x, x, s) : a64_fmuls(x, x, s)); break;
            case 0x5C: E(b, dbl ? a64_fsubd(x, x, s) : a64_fsubs(x, x, s)); break;
            case 0x5E: E(b, dbl ? a64_fdivd(x, x, s) : a64_fdivs(x, x, s)); break;
            }
            nan_fix(b, dbl, scalar, V_R, s == x ? V_R : s, x);
            return 0;
        } else if (scalar) {
            switch (op) {
            case 0x58: E(b, dbl ? a64_faddd(V_R, x, s) : a64_fadds(V_R, x, s)); break;
            case 0x59: E(b, dbl ? a64_fmuld(V_R, x, s) : a64_fmuls(V_R, x, s)); break;
            case 0x5C: E(b, dbl ? a64_fsubd(V_R, x, s) : a64_fsubs(V_R, x, s)); break;
            case 0x5E: E(b, dbl ? a64_fdivd(V_R, x, s) : a64_fdivs(V_R, x, s)); break;
            }
            nan_fix(b, dbl, scalar, x, s, V_R);
        } else {
            switch (op) {
            case 0x58: E(b, dbl ? a64_fadd2d(V_R, x, s) : a64_fadd4s(V_R, x, s)); break;
            case 0x59: E(b, dbl ? a64_fmul2d(V_R, x, s) : a64_fmul4s(V_R, x, s)); break;
            case 0x5C: E(b, dbl ? a64_fsub2d(V_R, x, s) : a64_fsub4s(V_R, x, s)); break;
            case 0x5E: E(b, dbl ? a64_fdiv2d(V_R, x, s) : a64_fdiv4s(V_R, x, s)); break;
            }
            nan_fix(b, dbl, scalar, x, s, V_R);
        }
        if (scalar) E(b, dbl ? a64_ins_d(x, 0, V_R, 0) : a64_ins_s(x, 0, V_R, 0)); else E(b, a64_vmov(x, V_R));
        return 0;
    }
    case 0x52: case 0x53: {                                               // RSQRTPS/SS RCPPS/SS (estimate())
        if (pf == 0x66 || pf == 0xF2) return -1;
        int s = xmm_rm(b, d, esz, next);
        E(b, a64_mrs_fpsr(T_B));                                         // (x86 raises no exceptions for these)
        estimate(b, V_R, s, op == 0x52);
        E(b, a64_msr_fpsr(T_B));
        if (scalar) E(b, a64_ins_s(x, 0, V_R, 0)); else E(b, a64_vmov(x, V_R));
        return 0;
    }
    case 0x7C: case 0x7D: {                                               // HADDPS/PD HSUBPS/PD: pairs of the two
        if (pf != 0x66 && pf != 0xF2) return -1;
        int s = xmm_rm(b, d, 16, next), sz = dbl && pf == 0x66 ? 3 : 2, dd = sz == 3;
        E(b, a64_uzp1(sz, 22, x, s)); E(b, a64_uzp2(sz, 23, x, s));
        E(b, op == 0x7C ? (dd ? a64_fadd2d(V_R, 22, 23) : a64_fadd4s(V_R, 22, 23)) : (dd ? a64_fsub2d(V_R, 22, 23) : a64_fsub4s(V_R, 22, 23)));
        nan_fix(b, dd, 0, 22, 23, V_R);
        E(b, a64_vmov(x, V_R));
        return 0;
    }
    case 0xD0: {                                                          // ADDSUBPS/PD: even lanes subtract, odd lanes add
        if (pf != 0x66 && pf != 0xF2) return -1;
        int s = xmm_rm(b, d, 16, next), dd = pf == 0x66;                 // (x - s is x + -s exactly: one addition, one
        lane_mask(b, V_T, dd ? 1 : 5, dd);                                //   set of flags)
        mov_imm(b, S0, dd ? 0x8000000000000000ull : 0x8000000080000000ull); E(b, a64_dup2d(V_U, S0)); E(b, a64_vand(V_T, V_T, V_U));
        E(b, a64_veor(22, s, V_T)); E(b, dd ? a64_fadd2d(V_R, x, 22) : a64_fadd4s(V_R, x, 22));
        nan_fix(b, dd, 0, x, s, V_R);
        E(b, a64_vmov(x, V_R));
        return 0;
    }
    case 0xF0:                                                            // LDDQU (VEX: maybe vex_sse's registers)
        if (pf != 0xF2 || (d->mod == 3 && !(d->prefixes & X86_P_VEX))) return -1;
        { int s = xmm_rm(b, d, 16, next); E(b, a64_vmov(x, s)); }
        return 0;
    case 0xC4: {                                                          // PINSRW
        if (pf != 0x66) return -1;
        rd_rm(b, d, T_A, 2, next); E(b, a64_insg(1, x, (int)(d->imm & 7), T_A));
        return 0;
    }
    case 0xC5:                                                            // PEXTRW r32, xmm
        if (pf != 0x66 || d->mod != 3) return -1;
        E(b, a64_umovg(1, T_A, d->rm, (int)(d->imm & 7))); wr_reg(b, d, d->reg, T_A, 8);
        return 0;
    case 0xD7: {                                                          // PMOVMSKB: the bytes' sign bits
        if (pf != 0x66 || d->mod != 3) return -1;
        E(b, a64_sshr(8, V_T, d->rm, 7)); mov_imm(b, S0, 0x8040201008040201ull); E(b, a64_dup2d(V_U, S0)); E(b, a64_vand(V_T, V_T, V_U));
        E(b, a64_addv8b(V_U, V_T)); E(b, a64_ext(V_W, V_T, V_T, 8)); E(b, a64_addv8b(V_W, V_W));
        E(b, a64_umov_b0(T_A, V_U)); E(b, a64_umov_b0(T_B, V_W)); E(b, a64_orr_lsl(T_A, T_A, T_B, 8));
        wr_reg(b, d, d->reg, T_A, 8);
        return 0;
    }
    case 0x5D: case 0x5F: {                                               // MIN / MAX: a < b ? a : b, a > b ? a : b
        int s = xmm_rm(b, d, esz, next);
        int lt_a = op == 0x5D ? s : x, lt_b = op == 0x5D ? x : s;
        E(b, scalar ? a64_fcmp_s(2, dbl, V_T, lt_a, lt_b) : dbl ? a64_fcmgt2d(V_T, lt_a, lt_b) : a64_fcmgt4s(V_T, lt_a, lt_b));
        E(b, a64_vbsl(V_T, x, s));
        // (with DAZ the result is the flushed operand: FCMEQ sees FZ's zeros, which keep their sign)
        E(b, a64_movi0(V_U)); E(b, scalar ? a64_fcmp_s(0, dbl, V_U, V_T, V_U) : dbl ? a64_fcmeq2d(V_U, V_T, V_U) : a64_fcmeq4s(V_U, V_T, V_U));
        mov_imm(b, S0, dbl ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFu); E(b, dbl ? a64_dup2d(V_W, S0) : a64_dup4s(V_W, S0));
        E(b, a64_vand(V_U, V_U, V_W)); E(b, a64_vbic(V_T, V_T, V_U));
        if (scalar) E(b, dbl ? a64_ins_d(x, 0, V_T, 0) : a64_ins_s(x, 0, V_T, 0)); else E(b, a64_vmov(x, V_T));
        return 0;
    }
    case 0xC2: {                                                          // CMPPS/PD/SS/SD
        // The predicates: 32 with VEX (imm & 31; the upper 16 differ only in signalling, so imm & 15 here). Without VEX the SDM has 8 and
        // reserves the rest; Rosetta 2 takes imm & 7 for CMPPS/CMPPD but imm & 31 for CMPSS/CMPSD (bit 4: whether a QNaN
        // signals, as with VEX), and so does this.
        int s = xmm_rm(b, d, esz, next), imm5 = (int)(d->imm & ((d->prefixes & X86_P_VEX) || scalar ? 31 : 7)), p = imm5 & 15;
        #define EQ_(dd, a_, b_) E(b, scalar ? a64_fcmp_s(0, dbl, dd, a_, b_) : dbl ? a64_fcmeq2d(dd, a_, b_) : a64_fcmeq4s(dd, a_, b_))
        #define GT_(dd, a_, b_) E(b, scalar ? a64_fcmp_s(2, dbl, dd, a_, b_) : dbl ? a64_fcmgt2d(dd, a_, b_) : a64_fcmgt4s(dd, a_, b_))
        #define GE_(dd, a_, b_) E(b, scalar ? a64_fcmp_s(1, dbl, dd, a_, b_) : dbl ? a64_fcmge2d(dd, a_, b_) : a64_fcmge4s(dd, a_, b_))
        // Flags: ARM's EQ is quiet (raises invalid for SNaNs only), GT/GE signal (any NaN), as most predicates want. The
        // others: an extra compare for its flags (signalling EQ, the constants), or ordered operands for a quiet order.
        int sig = (p == 1 || p == 2 || p == 5 || p == 6 || p == 9 || p == 10 || p == 13 || p == 14) ^ (imm5 >= 16);
        int ordering = p == 1 || p == 2 || p == 5 || p == 6 || p == 9 || p == 10 || p == 13 || p == 14;
        if (sig && !ordering) GT_(V_U, x, s);                             // (EQ_OS ... TRUE_US: a signalling compare)
        else if (!sig && (p == 11 || p == 15)) EQ_(V_U, x, s);            // (FALSE_OQ, TRUE_UQ: invalid for SNaNs)
        int qx = x, qs = s;
        if (!sig && ordering) {                                           // LT_OQ ... GT_OQ: NaN lanes compared as zeros,
            EQ_(V_W, x, x); EQ_(V_X, s, s);                               //   then their result decided by the predicate
            E(b, a64_vand(V_W, V_W, V_X));
            E(b, a64_vand(22, x, V_W)); E(b, a64_vand(23, s, V_W)); qx = 22; qs = 23;
        }
        #define x qx
        #define s qs
        #define ORD_(dd) do { EQ_(dd, x, x); EQ_(V_U, s, s); E(b, a64_vand(dd, dd, V_U)); } while (0)
        switch (p) {
        case 0: EQ_(V_R, x, s); break;                                    // EQ_OQ
        case 1: GT_(V_R, s, x); break;                                    // LT_OS
        case 2: GE_(V_R, s, x); break;                                    // LE_OS
        case 3: ORD_(V_R); E(b, a64_vmvn(V_R, V_R)); break;               // UNORD_Q
        case 4: EQ_(V_R, x, s); E(b, a64_vmvn(V_R, V_R)); break;          // NEQ_UQ
        case 5: GT_(V_R, s, x); E(b, a64_vmvn(V_R, V_R)); break;          // NLT_US
        case 6: GE_(V_R, s, x); E(b, a64_vmvn(V_R, V_R)); break;          // NLE_US
        case 7: ORD_(V_R); break;                                         // ORD_Q
        case 8: ORD_(V_T); EQ_(V_R, x, s); E(b, a64_vmvn(V_T, V_T)); E(b, a64_vorr(V_R, V_R, V_T)); break;   // EQ_UQ
        case 9: GE_(V_R, x, s); E(b, a64_vmvn(V_R, V_R)); break;          // NGE_US
        case 10: GT_(V_R, x, s); E(b, a64_vmvn(V_R, V_R)); break;         // NGT_US
        case 11: E(b, a64_movi0(V_R)); break;                             // FALSE_OQ
        case 12: ORD_(V_T); EQ_(V_R, x, s); E(b, a64_vbic(V_R, V_T, V_R)); break;   // NEQ_OQ: ordered and not equal
        case 13: GE_(V_R, x, s); break;                                   // GE_OS
        case 14: GT_(V_R, x, s); break;                                   // GT_OS
        case 15: E(b, a64_movi0(V_R)); E(b, a64_vmvn(V_R, V_R)); break;   // TRUE_UQ
        }
        #undef x
        #undef s
        if (!sig && ordering) {                                           // (unordered lanes: false, or true for the N.. ones)
            if (p == 5 || p == 6 || p == 9 || p == 10) E(b, a64_vorn(V_R, V_R, V_W)); else E(b, a64_vand(V_R, V_R, V_W));
        }
        #undef EQ_
        #undef GT_
        #undef GE_
        #undef ORD_
        if (scalar) E(b, dbl ? a64_ins_d(x, 0, V_R, 0) : a64_ins_s(x, 0, V_R, 0)); else E(b, a64_vmov(x, V_R));
        return 0;
    }
    case 0x2E: case 0x2F: {                                               // UCOMISS/SD COMISS/SD: ZF PF CF
        if (pf != 0 && pf != 0x66) return -1;
        int s = xmm_rm(b, d, pf ? 8 : 4, next);
        E(b, op == 0x2F ? (pf ? a64_fcmped(x, s) : a64_fcmpes(x, s)) : (pf ? a64_fcmpd(x, s) : a64_fcmps(x, s)));   // (COMIS: QNaNs signal too)
        if (b->fp_fuse) { b->fuse_kind = 6; b->fuse_size = b->fp_fuse - 1; b->fp_fuse = 0; return 0; }   // (rules: the Jcc reads NZCV)
        E(b, a64_movz(ACC, 0, 0));
        E(b, a64_cset(S1, VS));                                           // unordered: ZF PF CF all set
        E(b, a64_cset(S0, EQ)); E(b, a64_orr(S0, S0, S1)); put_flag(b, S0, 6);
        put_flag(b, S1, 2);
        E(b, a64_cset(S0, MI)); E(b, a64_orr(S0, S0, S1)); put_flag(b, S0, 0);
        merge_flags(b, F_ARITH);
        return 0;
    }
    case 0x2A: {                                                          // CVTSI2SS / CVTSI2SD
        if (!scalar) return -1;
        int w = d->opsize == 8;
        rd_rm(b, d, T_A, w ? 8 : 4, next);
        if (pf == 0xF3) E(b, w ? a64_scvtf_sx(V_R, T_A) : a64_scvtf_sw(V_R, T_A)); else E(b, w ? a64_scvtf_dx(V_R, T_A) : a64_scvtf_dw(V_R, T_A));
        E(b, pf == 0xF2 ? a64_ins_d(x, 0, V_R, 0) : a64_ins_s(x, 0, V_R, 0));
        return 0;
    }
    case 0x2C: case 0x2D: {                                               // CVTTSS2SI CVTSS2SI (SD)
        if (!scalar) return -1;
        int s = xmm_rm(b, d, esz, next), sz = d->opsize == 8 ? 8 : 4;
        cvt_to_int(b, T_A, s, pf == 0xF2, sz, op == 0x2C);
        wr_reg(b, d, d->reg, T_A, sz);
        return 0;
    }
    case 0x5A: {                                                          // CVTPS2PD CVTPD2PS CVTSS2SD CVTSD2SS
        int s = xmm_rm(b, d, pf == 0 ? 8 : esz, next);
        if (pf == 0) E(b, a64_fcvtl2d(x, s));
        else if (pf == 0x66) E(b, a64_fcvtn2s(x, s));
        else if (pf == 0xF3) { E(b, a64_fcvt_ds(V_R, s)); E(b, a64_ins_d(x, 0, V_R, 0)); }
        else { E(b, a64_fcvt_sd(V_R, s)); E(b, a64_ins_s(x, 0, V_R, 0)); }
        return 0;
    }
    case 0x5B: {                                                          // CVTDQ2PS CVTPS2DQ CVTTPS2DQ
        int s = xmm_rm(b, d, 16, next);
        if (pf == 0) E(b, a64_scvtf4s(x, s));
        else if (pf == 0x66 || pf == 0xF3) cvt4_to_int(b, x, s, pf == 0xF3);
        else return -1;
        return 0;
    }
    case 0xE6: {                                                          // CVTDQ2PD CVTPD2DQ CVTTPD2DQ
        if (pf == 0xF2 || pf == 0x66) { int s = xmm_rm(b, d, 16, next); pd2dq(b, x, s, pf == 0x66); return 0; }
        if (pf != 0xF3) return -1;
        int s = xmm_rm(b, d, 8, next);
        E(b, a64_sxtl2d(V_R, s)); E(b, a64_scvtf2d(x, V_R));
        return 0;
    }
    case 0x50: {                                                          // MOVMSKPS / MOVMSKPD
        if (d->mod != 3 || scalar) return -1;
        int n = dbl ? 2 : 4;
        E(b, a64_movz(T_A, 0, 0));
        for (int i = 0; i < n; i++) {
            if (dbl) { E(b, a64_umov_d(S0, d->rm, i)); E(b, a64_lsri(S0, S0, 63)); }
            else { E(b, a64_umov_s(S0, d->rm, i)); E(b, a64_ubfx(S0, S0, 31, 1)); }
            E(b, a64_orr_lsl(T_A, T_A, S0, i));
        }
        wr_reg(b, d, d->reg, T_A, 4);
        return 0;
    }
    case 0x6E: case 0x7E: {                                               // MOVD / MOVQ with general registers, MOVQ xmm
        if (op == 0x7E && pf == 0xF3) { int s = xmm_rm(b, d, 8, next); E(b, a64_movi0(V_R)); E(b, a64_ins_d(V_R, 0, s, 0)); E(b, a64_vmov(x, V_R)); return 0; }
        if (pf != 0x66) return -1;
        int w = d->opsize == 8 ? 8 : 4;
        if (op == 0x6E) { rd_rm(b, d, T_A, w, next); E(b, w == 8 ? a64_fmov_dx(x, T_A) : a64_fmov_sw(x, T_A)); }
        else {
            if (d->mod == 3) { E(b, w == 8 ? a64_umov_d(d->rm, x, 0) : a64_umov_s(d->rm, x, 0)); return 0; }   // (32 bits: the upper half cleared)
            E(b, w == 8 ? a64_umov_d(T_A, x, 0) : a64_umov_s(T_A, x, 0));
            if (d->mod != 3) ea(b, d, next, 0);
            wr_rm(b, d, T_A, w);
        }
        return 0;
    }
    case 0xD6: {                                                          // MOVQ xmm/m64, xmm
        if (pf != 0x66) return -1;
        if (d->mod == 3) { E(b, a64_movi0(V_R)); E(b, a64_ins_d(V_R, 0, x, 0)); E(b, a64_vmov(d->rm, V_R)); }
        else { ea(b, d, next, 0); E(b, a64_strd(x, T_ADDR, 0)); }
        return 0;
    }
    case 0x70: {                                                          // PSHUFD PSHUFLW PSHUFHW
        int s = xmm_rm(b, d, 16, next), imm = (int)(uint8_t)d->imm;
        if (pf == 0x66) shuf4(b, x, s, s, imm);
        else if (pf == 0xF2 || pf == 0xF3) {
            int base = pf == 0xF3 ? 4 : 0;
            E(b, a64_vmov(V_R, s));
            for (int i = 0; i < 4; i++) E(b, a64_ins_h(V_R, base + i, s, base + (imm >> (2 * i) & 3)));
            E(b, a64_vmov(x, V_R));
        } else return -1;
        return 0;
    }
    case 0x71: case 0x72: case 0x73: {                                    // PSRLW/D/Q PSRAW/D PSLLW/D/Q PSRLDQ PSLLDQ by imm8
        if (pf != 0x66 || d->mod != 3) return -1;
        int g = d->reg & 7, v = d->rm, c = (int)(uint8_t)d->imm;
        int es = op == 0x71 ? 16 : op == 0x72 ? 32 : 64;
        if (op == 0x73 && (g == 3 || g == 7)) {                           // byte shifts of the whole register
            if (c >= 16) { E(b, a64_movi0(v)); return 0; }
            if (!c) return 0;
            E(b, a64_movi0(V_T));
            E(b, g == 3 ? a64_ext(v, v, V_T, c) : a64_ext(v, V_T, v, 16 - c));
            return 0;
        }
        if (g == 2) { if (c >= es) E(b, a64_movi0(v)); else if (c) E(b, a64_ushr(es, v, v, c)); }
        else if (g == 4 && op != 0x73) { if (c) E(b, a64_sshr(es, v, v, c >= es ? es : c)); }   // (past the width: the sign)
        else if (g == 6) { if (c >= es) E(b, a64_movi0(v)); else if (c) E(b, a64_shl(es, v, v, c)); }
        else return -1;
        return 0;
    }
    }
    // Integer SIMD with the 66 prefix (MMX forms without it are not supported)
    if (pf != 0x66) return -1;
    int s;
    switch (op) {
    case 0xFC: case 0xFD: case 0xFE: case 0xD4: case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xDB: case 0xDF: case 0xEB: case 0xEF: case 0x74: case 0x75: case 0x76: case 0x64: case 0x65: case 0x66:
    case 0x60: case 0x61: case 0x62: case 0x6C: case 0x68: case 0x69: case 0x6A: case 0x6D: case 0xF5: case 0xD5:
    case 0x63: case 0x67: case 0x6B: case 0xD8: case 0xD9: case 0xDC: case 0xDD: case 0xE8: case 0xE9: case 0xEC: case 0xED:
    case 0xDA: case 0xDE: case 0xEA: case 0xEE: case 0xE0: case 0xE3: case 0xE4: case 0xE5: case 0xF4: case 0xF6:
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3:
        s = xmm_rm(b, d, 16, next);
        break;
    default: return -1;
    }
    if ((op >= 0xD1 && op <= 0xD3) || op == 0xE1 || op == 0xE2 || (op >= 0xF1 && op <= 0xF3)) {
        // shifts by the low 64 bits of s, the same for all lanes; counts from the width on give 0 (or the sign)
        int sz = (op & 3) == 1 ? 1 : (op & 3) == 2 ? 2 : 3, es = 8 << sz;
        E(b, a64_umov_d(T_A, s, 0)); mov_imm(b, S0, (uint64_t)es); E(b, a64_cmp(T_A, S0)); E(b, a64_csel(T_A, S0, T_A, HI));
        if (op < 0xF0) E(b, a64_sub(T_A, XZR, T_A));                    // (right: a negative count)
        E(b, a64_dupg(sz, V_T, T_A));
        E(b, op == 0xE1 || op == 0xE2 ? a64_sshl(sz, x, x, V_T) : a64_ushl(sz, x, x, V_T));
        return 0;
    }
    // Zero idioms (XOR, ANDN, SUB, PCMPGT of a register with itself): MOVI #0, which the Apple cores rename without reading
    // the register. EOR v, v, v waits for v's last value: chains of a loop that start from such a zero (an accumulator
    // per output) then run one after another instead of side by side (matmul 10.1 -> 4.5 ms on an M1 Max).
    if (d->mod == 3 && s == x && (op == 0xEF || op == 0xDF || (op >= 0xF8 && op <= 0xFB) || (op >= 0x64 && op <= 0x66))) { E(b, a64_movi0(x)); return 0; }
    switch (op) {
    case 0xFC: E(b, a64_add16b(x, x, s)); break;   case 0xFD: E(b, a64_add8h(x, x, s)); break;
    case 0xFE: E(b, a64_add4s(x, x, s)); break;    case 0xD4: E(b, a64_add2d(x, x, s)); break;
    case 0xF8: E(b, a64_sub16b(x, x, s)); break;   case 0xF9: E(b, a64_sub8h(x, x, s)); break;
    case 0xFA: E(b, a64_sub4s(x, x, s)); break;    case 0xFB: E(b, a64_sub2d(x, x, s)); break;
    case 0xDB: E(b, a64_vand(x, x, s)); break;     case 0xDF: E(b, a64_vbic(x, s, x)); break;
    case 0xEB: E(b, a64_vorr(x, x, s)); break;     case 0xEF: E(b, a64_veor(x, x, s)); break;
    case 0x74: E(b, a64_cmeq16b(x, x, s)); break;  case 0x75: E(b, a64_cmeq8h(x, x, s)); break;
    case 0x76: E(b, a64_cmeq4s(x, x, s)); break;
    case 0x64: E(b, a64_cmgt16b(x, x, s)); break;  case 0x65: E(b, a64_cmgt8h(x, x, s)); break;
    case 0x66: E(b, a64_cmgt4s(x, x, s)); break;
    case 0x60: E(b, a64_zip1_16b(x, x, s)); break; case 0x68: E(b, a64_zip2_16b(x, x, s)); break;
    case 0x61: E(b, a64_zip1_8h(x, x, s)); break;  case 0x69: E(b, a64_zip2_8h(x, x, s)); break;
    case 0x62: E(b, a64_zip1_4s(x, x, s)); break;  case 0x6A: E(b, a64_zip2_4s(x, x, s)); break;
    case 0x6C: E(b, a64_zip1_2d(x, x, s)); break;  case 0x6D: E(b, a64_zip2_2d(x, x, s)); break;
    case 0xD5: E(b, a64_mul8h(x, x, s)); break;
    case 0xF5: E(b, a64_smull4s(V_T, x, s)); E(b, a64_smull2_4s(V_U, x, s)); E(b, a64_addp4s(x, V_T, V_U)); break;   // PMADDWD
    case 0x63: E(b, a64_sqxtn_lo(0, V_T, x)); E(b, a64_sqxtn_hi(0, V_T, s)); E(b, a64_vmov(x, V_T)); break;           // PACKSSWB
    case 0x6B: E(b, a64_sqxtn_lo(1, V_T, x)); E(b, a64_sqxtn_hi(1, V_T, s)); E(b, a64_vmov(x, V_T)); break;           // PACKSSDW
    case 0x67: E(b, a64_sqxtun_lo(0, V_T, x)); E(b, a64_sqxtun_hi(0, V_T, s)); E(b, a64_vmov(x, V_T)); break;         // PACKUSWB
    case 0xD8: E(b, a64_uqsub(0, x, x, s)); break;  case 0xD9: E(b, a64_uqsub(1, x, x, s)); break;
    case 0xDC: E(b, a64_uqadd(0, x, x, s)); break;  case 0xDD: E(b, a64_uqadd(1, x, x, s)); break;
    case 0xE8: E(b, a64_sqsub(0, x, x, s)); break;  case 0xE9: E(b, a64_sqsub(1, x, x, s)); break;
    case 0xEC: E(b, a64_sqadd(0, x, x, s)); break;  case 0xED: E(b, a64_sqadd(1, x, x, s)); break;
    case 0xDA: E(b, a64_umin(0, x, x, s)); break;   case 0xDE: E(b, a64_umax(0, x, x, s)); break;
    case 0xEA: E(b, a64_smin(1, x, x, s)); break;   case 0xEE: E(b, a64_smax(1, x, x, s)); break;
    case 0xE0: E(b, a64_urhadd(0, x, x, s)); break; case 0xE3: E(b, a64_urhadd(1, x, x, s)); break;                  // PAVGB/W
    case 0xE4: case 0xE5:                                                                                            // PMULHUW PMULHW
        E(b, op == 0xE4 ? a64_umull_lo(1, V_T, x, s) : a64_smull_lo(1, V_T, x, s));
        E(b, op == 0xE4 ? a64_umull_hi(1, V_U, x, s) : a64_smull_hi(1, V_U, x, s));
        E(b, a64_uzp2(1, x, V_T, V_U)); break;
    case 0xF4: E(b, a64_xtn_lo(2, V_T, x)); E(b, a64_xtn_lo(2, V_U, s)); E(b, a64_umull_lo(2, x, V_T, V_U)); break;  // PMULUDQ
    case 0xF6: E(b, a64_uabd(0, V_T, x, s)); E(b, a64_uaddlp(0, V_T, V_T)); E(b, a64_uaddlp(1, V_T, V_T)); E(b, a64_uaddlp(2, x, V_T)); break;   // PSADBW
    }
    return 0;
}

// SSSE3 and SSE4.1 (maps 0F38 and 0F3A, prefix 66). 1: not one of them (CRC32, MOVBE: general instructions).
static const uint8_t pmov_size[6] = { 8, 4, 2, 8, 4, 8 };                // PMOVSX/ZX: bytes read, by opcode & 7
static const uint8_t pmov_from[6] = { 0, 0, 0, 1, 1, 2 }, pmov_to[6] = { 1, 2, 3, 2, 3, 3 };
static void pmov(xlat_buf *b, int dst, int s, int k, int zx) {           // the extensions, one step per size
    for (int sz = pmov_from[k]; sz < pmov_to[k]; sz++) {
        int r = sz + 1 == pmov_to[k] ? dst : V_T;
        E(b, zx ? a64_uxtl_lo(sz, r, s) : a64_sxtl_lo(sz, r, s));
        s = V_T;
    }
}
static int xlat_sse4(xlat_buf *b, const x86_insn *d, uint64_t next) {
    int op = d->opcode, x = d->reg, imm = (int)(uint8_t)d->imm;
    if (d->mandatory != 0x66) return d->prefixes & X86_P_VEX ? -1 : 1;
    if (d->map == X86_MAP_0F38 && op >= 0xF0) return 1;                  // (ADCX, MOVBE m16: general instructions)
    if (d->map == X86_MAP_0F38) {
        int pm = (op >= 0x20 && op <= 0x25) || (op >= 0x30 && op <= 0x35);
        if (op == 0x2A && d->mod == 3 && !(d->prefixes & X86_P_VEX)) return -1;
        int known = op <= 0x0B || op == 0x10 || op == 0x14 || op == 0x15 || op == 0x17 || (op >= 0x1C && op <= 0x1E) || pm ||
                    (op >= 0x28 && op <= 0x2B) || (op >= 0x37 && op <= 0x40) || (op >= 0xDB && op <= 0xDF);
        if (!known) return -1;
        int s = xmm_rm(b, d, pm ? pmov_size[op & 7] : 16, next);
        switch (op) {
        case 0x00: mov_imm(b, S0, 0x8F); E(b, a64_dupg(0, V_T, S0)); E(b, a64_vand(V_T, s, V_T)); E(b, a64_tbl(x, x, V_T)); break;   // PSHUFB
        case 0x01: case 0x02: case 0x03: case 0x05: case 0x06: case 0x07: {                      // PHADD/PHSUB W D SW
            int sz = (op & 3) == 2 ? 2 : 1;
            E(b, a64_uzp1(sz, V_T, x, s)); E(b, a64_uzp2(sz, V_U, x, s));
            E(b, op == 0x03 ? a64_sqadd(1, x, V_T, V_U) : op == 0x07 ? a64_sqsub(1, x, V_T, V_U) : op < 0x04 ? a64_vadd(sz, x, V_T, V_U) : a64_vsub(sz, x, V_T, V_U));
            break;
        }
        case 0x04:                                                        // PMADDUBSW: unsigned x signed bytes, pairs added
            E(b, a64_uxtl_lo(0, V_T, x)); E(b, a64_sxtl_lo(0, V_U, s)); E(b, a64_vmul(1, V_T, V_T, V_U));
            E(b, a64_uxtl_hi(0, V_U, x)); E(b, a64_sxtl_hi(0, V_W, s)); E(b, a64_vmul(1, V_U, V_U, V_W));
            E(b, a64_uzp1(1, V_W, V_T, V_U)); E(b, a64_uzp2(1, V_X, V_T, V_U)); E(b, a64_sqadd(1, x, V_W, V_X));
            break;
        case 0x08: case 0x09: case 0x0A: {                                // PSIGNB/W/D
            int sz = op - 0x08;
            E(b, a64_vneg(sz, V_T, x)); E(b, a64_cmlt0(sz, V_U, s)); E(b, a64_vbsl(V_U, V_T, x));
            E(b, a64_cmeq0(sz, V_W, s)); E(b, a64_vbic(x, V_U, V_W));
            break;
        }
        case 0x0B:                                                        // PMULHRSW (SQRDMULH, but -1 * -1 wraps to 0x8000)
            E(b, a64_sqrdmulh(1, V_T, x, s)); mov_imm(b, S0, 0x8000); E(b, a64_dupg(1, V_U, S0));
            E(b, a64_vcmeq(1, V_W, x, V_U)); E(b, a64_vcmeq(1, V_X, s, V_U)); E(b, a64_vand(V_W, V_W, V_X));
            E(b, a64_vbsl(V_W, V_U, V_T)); E(b, a64_vmov(x, V_W));
            break;
        case 0x10: case 0x14: case 0x15: {                                // PBLENDVB BLENDVPS BLENDVPD: by XMM0's sign bits
            int es = op == 0x10 ? 8 : op == 0x14 ? 32 : 64;
            E(b, a64_sshr(es, V_T, 0, es - 1)); E(b, a64_vbsl(V_T, s, x)); E(b, a64_vmov(x, V_T));
            break;
        }
        case 0x17: E(b, a64_vand(V_T, s, x)); E(b, a64_vbic(V_U, s, x)); test_flags(b); break;   // PTEST
        case 0x1C: case 0x1D: case 0x1E: E(b, a64_vabs(op - 0x1C, x, s)); break;                // PABSB/W/D
        case 0x28: E(b, a64_xtn_lo(2, V_T, x)); E(b, a64_xtn_lo(2, V_U, s)); E(b, a64_smull_lo(2, x, V_T, V_U)); break;   // PMULDQ
        case 0x29: E(b, a64_vcmeq(3, x, x, s)); break;                    // PCMPEQQ
        case 0x37: E(b, a64_vcmgt(3, x, x, s)); break;                    // PCMPGTQ
        case 0x2A: E(b, a64_vmov(x, s)); break;                           // MOVNTDQA
        case 0x2B: E(b, a64_sqxtun_lo(1, V_T, x)); E(b, a64_sqxtun_hi(1, V_T, s)); E(b, a64_vmov(x, V_T)); break;   // PACKUSDW
        case 0x38: E(b, a64_smin(0, x, x, s)); break; case 0x39: E(b, a64_smin(2, x, x, s)); break;
        case 0x3A: E(b, a64_umin(1, x, x, s)); break; case 0x3B: E(b, a64_umin(2, x, x, s)); break;
        case 0x3C: E(b, a64_smax(0, x, x, s)); break; case 0x3D: E(b, a64_smax(2, x, x, s)); break;
        case 0x3E: E(b, a64_umax(1, x, x, s)); break; case 0x3F: E(b, a64_umax(2, x, x, s)); break;
        case 0x40: E(b, a64_vmul(2, x, x, s)); break;                     // PMULLD
        case 0xDB: E(b, a64_aes(3, x, s)); break;                         // AESIMC
        case 0xDC: case 0xDD: case 0xDE: case 0xDF: {                     // AESENC(LAST) AESDEC(LAST): ARM's rounds start with
            int dec = op >= 0xDE;                                          //   the key, x86's end with it: a zero key, then XOR
            E(b, a64_movi0(V_T)); E(b, a64_vmov(V_U, x)); E(b, a64_aes(dec, V_U, V_T));
            if (!(op & 1)) E(b, a64_aes(dec ? 3 : 2, V_U, V_U));
            E(b, a64_veor(x, V_U, s));
            break;
        }
        default: pmov(b, x, s, op & 7, op >= 0x30); break;                // PMOVSX / PMOVZX
        }
        return 0;
    }
    if (d->map != X86_MAP_0F3A) return 1;
    switch (op) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: {                         // ROUNDPS/PD/SS/SD (imm bit 2: MXCSR's mode)
        int dbl = op & 1, scalar = op >= 0x0A, mode = imm & 4 ? 4 : imm & 3;
        int s = xmm_rm(b, d, scalar ? (dbl ? 8 : 4) : 16, next);
        if (scalar) { E(b, a64_movi0(V_T)); E(b, dbl ? a64_ins_d(V_T, 0, s, 0) : a64_ins_s(V_T, 0, s, 0)); s = V_T; }   // (lane 0 alone: no flags from the rest)
        E(b, a64_frint(mode, dbl, V_R, s));
        if (!(imm & 8)) E(b, a64_frint(5, dbl, V_U, s));                  // (precision not suppressed: inexact if not integral)
        if (scalar) E(b, dbl ? a64_ins_d(x, 0, V_R, 0) : a64_ins_s(x, 0, V_R, 0)); else E(b, a64_vmov(x, V_R));
        return 0;
    }
    case 0x0C: case 0x0D: case 0x0E: {                                    // BLENDPS BLENDPD PBLENDW
        int s = xmm_rm(b, d, 16, next), n = op == 0x0C ? 4 : op == 0x0D ? 2 : 8, sz = op == 0x0C ? 2 : op == 0x0D ? 3 : 1;
        for (int i = 0; i < n; i++) if (imm >> i & 1) E(b, a64_insv(sz, x, i, s, i));
        return 0;
    }
    case 0x0F: {                                                          // PALIGNR: (x:s) >> imm bytes
        int s = xmm_rm(b, d, 16, next);
        if (imm >= 32) E(b, a64_movi0(x));
        else if (imm >= 16) { E(b, a64_movi0(V_T)); E(b, a64_ext(x, x, V_T, imm - 16)); }
        else E(b, a64_ext(x, s, x, imm));
        return 0;
    }
    case 0x14: case 0x15: case 0x16: case 0x17: {                         // PEXTRB/W/D/Q EXTRACTPS to r/m
        int size = op == 0x14 ? 1 : op == 0x15 ? 2 : op == 0x16 && d->opsize == 8 ? 8 : 4, lg = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
        E(b, a64_umovg(lg, T_A, x, imm & (16 / size - 1)));
        if (d->mod == 3) wr_reg(b, d, d->rm, T_A, 8); else { ea(b, d, next, 0); E(b, a64_str(size, T_A, T_ADDR, 0)); }
        return 0;
    }
    case 0x20: case 0x22: {                                               // PINSRB PINSRD/Q
        int size = op == 0x20 ? 1 : d->opsize == 8 ? 8 : 4, lg = size == 1 ? 0 : size == 4 ? 2 : 3;
        rd_rm(b, d, T_A, d->mod == 3 && size == 1 ? 4 : size, next);   // (a register: its low byte, never AH-BH)
        E(b, a64_insg(lg, x, imm & (16 / size - 1), T_A));
        return 0;
    }
    case 0x21: {                                                          // INSERTPS: one lane in, then lanes zeroed
        int s = xmm_rm(b, d, 4, next);
        E(b, a64_ins_s(x, imm >> 4 & 3, s, d->mod == 3 ? imm >> 6 : 0));
        for (int i = 0; i < 4; i++) if (imm >> i & 1) E(b, a64_insg(2, x, i, XZR));
        return 0;
    }
    case 0x44: {                                                          // PCLMULQDQ: the quadwords imm picks, carry-less
        int s = xmm_rm(b, d, 16, next);
        E(b, a64_dup_d(V_T, x, imm & 1)); E(b, a64_dup_d(V_U, s, imm >> 4 & 1)); E(b, a64_pmull1q(x, V_T, V_U));
        return 0;
    }
    case 0x60: case 0x61: case 0x62: case 0x63: {                         // PCMPESTRI/M PCMPISTRI/M (in C: xlat.c's pcmpstr())
        if (d->mod != 3) ea(b, d, next, 0);
        call_helper(b, H_PCMPSTR | ((uint32_t)(op & 3) | (uint32_t)(d->mod != 3) << 2 | (uint32_t)((d->rex & 8) || ((d->prefixes & X86_P_VEX) && d->vex_w)) << 3 |
                                    (uint32_t)imm << 4 | (uint32_t)(x & 15) << 12 | (uint32_t)(d->mod == 3 ? d->rm & 15 : 0) << 16) << 8,
                    d->mod != 3 ? T_ADDR : XZR);
        return 0;
    }
    case 0xDF: {                                                          // AESKEYGENASSIST: SubWord, RotWord, round constant
        // AESE with a zero key gives ShiftRows(SubBytes(s)); a table picks the bytes SubWord / RotWord want from it
        int s = xmm_rm(b, d, 16, next);
        E(b, a64_movi0(V_T)); E(b, a64_vmov(V_U, s)); E(b, a64_aes(0, V_U, V_T));
        uint8_t want[16], at[16];                                         // want: source byte per result byte; at: where
        static const int src_dw[4] = { 1, 1, 3, 3 };                      //   ShiftRows put SubBytes(s[i])
        for (int lane = 0; lane < 4; lane++)
            for (int k = 0; k < 4; k++) want[4 * lane + k] = (uint8_t)(4 * src_dw[lane] + (lane & 1 ? (k + 1) & 3 : k));
        for (int i = 0; i < 16; i++) { int r = i & 3, cc = i >> 2; at[i] = (uint8_t)(r + 4 * ((cc - r) & 3)); }
        uint64_t lo = 0, hi = 0;
        for (int k = 0; k < 8; k++) { lo |= (uint64_t)at[want[k]] << (8 * k); hi |= (uint64_t)at[want[8 + k]] << (8 * k); }
        mov_imm(b, S0, lo); mov_imm(b, S1, hi); E(b, a64_fmov_dx(V_W, S0)); E(b, a64_ins_gd(V_W, 1, S1));
        E(b, a64_tbl(V_R, V_U, V_W));
        uint64_t rc = (uint64_t)imm << 32;                                // (round constant in lanes 1 and 3)
        mov_imm(b, S0, rc); E(b, a64_dup2d(V_X, S0)); E(b, a64_veor(x, V_R, V_X));
        return 0;
    }
    case 0x40: case 0x41: {                                               // DPPS DPPD: (p0 + p1) + (p2 + p3), as x86 adds them
        int dbl = op == 0x41, s = xmm_rm(b, d, 16, next);
        lane_mask(b, V_T, imm >> 4, dbl); E(b, a64_vand(22, x, V_T)); E(b, a64_vand(23, s, V_T));   // (products left out: +0 * +0)
        E(b, dbl ? a64_fmul2d(V_R, 22, 23) : a64_fmul4s(V_R, 22, 23)); nan_fix(b, dbl, 0, 22, 23, V_R);
        if (!dbl) {
            E(b, a64_uzp1(2, 22, V_R, V_R)); E(b, a64_uzp2(2, 23, V_R, V_R)); E(b, a64_fadd4s(V_R, 22, 23)); nan_fix(b, 0, 0, 22, 23, V_R);
        }
        E(b, dbl ? a64_dup_d(22, V_R, 0) : a64_dup_s(22, V_R, 0)); E(b, dbl ? a64_dup_d(23, V_R, 1) : a64_dup_s(23, V_R, 1));
        E(b, dbl ? a64_fadd2d(V_R, 22, 23) : a64_fadd4s(V_R, 22, 23)); nan_fix(b, dbl, 0, 22, 23, V_R);
        lane_mask(b, V_T, imm & 15, dbl); E(b, a64_vand(x, V_R, V_T));
        return 0;
    }
    }
    return -1;
}

// ---- AVX (phase 3b) ------------------------------------------------------------------------------------------------------
// YMM n is Vn (its lower half) and ctx.ymm_hi[n] (its upper half: V24-V31 could not hold sixteen). A 256-bit operation
// loads the upper halves it reads into V25/V26 and stores its result's; a VEX instruction with a 128-bit result clears
// its destination's upper half (two stores). Most VEX forms are SSE forms with a separate first source (VEX.vvvv): they
// run the SSE translation above on a copy of the instruction whose operands are registers chosen here, once per half.
enum { V_HX = 24, V_HA = 25, V_HS = 26, V_LO = 27 };
static void hi_ld(xlat_buf *b, int v, int n) { E(b, a64_ldrq(v, XC, CTX_YMMH + 16 * n)); }
static void hi_st(xlat_buf *b, int v, int n) { E(b, a64_strq(v, XC, CTX_YMMH + 16 * n)); }
static void hi_zero(xlat_buf *b, int n) { E(b, a64_str(8, XZR, XC, CTX_YMMH + 16 * n)); E(b, a64_str(8, XZR, XC, CTX_YMMH + 16 * n + 8)); }

// The r/m operand as a register (or V_B from memory), and its upper half in V_HS when L.
static int ymm_src(xlat_buf *b, const x86_insn *d, int L, uint64_t next) {
    if (d->mod == 3) { if (L) hi_ld(b, V_HS, d->rm); return d->rm; }
    ea(b, d, next, 0);
    E(b, a64_ldrq(V_B, T_ADDR, 0));
    if (L) E(b, a64_ldrq(V_HS, T_ADDR, 16));
    return V_B;
}

// dst = a op s as the SSE form e (s: e's r/m operand; a < 0: no first source), on both halves when L. s128: the r/m
// operand is 128 bits and both halves use it (shift counts).
static int vex_sse(xlat_buf *b, const x86_insn *d, x86_insn e, int dst, int a, int L, int s128, uint64_t next) {
    if (L) {
        if (s128) { int s = xmm_rm(b, &e, 16, next); E(b, a64_vmov(V_HS, s)); e.rm = (uint8_t)s; e.mod = 3; }
        else if (e.mod != 3) { e.rm = (uint8_t)ymm_src(b, d, 1, next); e.mod = 3; }
        else hi_ld(b, V_HS, e.rm);
        if (a >= 0) hi_ld(b, V_HA, a);
    }
    int x = dst;
    if (a >= 0 && a != dst) { if (e.mod == 3 && e.rm == dst) x = V_HX; E(b, a64_vmov(x, a)); }
    e.reg = (uint8_t)x;
    if (xlat_sse(b, &e, next)) return -1;
    if (x != dst) E(b, a64_vmov(dst, x));
    if (!L) { hi_zero(b, dst); return 0; }
    e.rm = V_HS; e.reg = a >= 0 ? V_HA : V_HX;
    if (e.map == X86_MAP_0F && e.opcode == 0xC6 && e.mandatory == 0x66) e.imm >>= 2;   // (VSHUFPD: the upper half has bits 2 and 3)
    if (xlat_sse(b, &e, next)) return -1;
    hi_st(b, e.reg, dst);
    return 0;
}

// VMOVMSKPS/PD's bits of v (lanes of 4 or 8 bytes) into T_A from bit `at`.
static void movmsk(xlat_buf *b, int v, int dbl, int at) {
    for (int i = 0; i < (dbl ? 2 : 4); i++) {
        if (dbl) { E(b, a64_umov_d(S0, v, i)); E(b, a64_lsri(S0, S0, 63)); }
        else { E(b, a64_umov_s(S0, v, i)); E(b, a64_ubfx(S0, S0, 31, 1)); }
        E(b, a64_orr_lsl(T_A, T_A, S0, at + i));
    }
}

// FMA3: VFMADD VFMSUB VFNMADD VFNMSUB VFMADDSUB VFMSUBADD, 132 / 213 / 231, ps pd ss sd: one rounding, as x86.
// NaNs as Rosetta 2 has them (the SDM's rule is still to be checked): the first NaN of the first multiplicand, the second,
// the addend, whatever the form's operand order, with the sign changed where the form negates that operand (the
// addend, or the first multiplicand for -(a*b)).
static int vex_fma(xlat_buf *b, const x86_insn *d, uint64_t next) {
    int op = d->opcode, kind = op & 15, order = op >> 4, dbl = d->vex_w, x = d->reg, a = d->vex_vvvv;
    int scalar = kind >= 9 && (kind & 1), L = scalar ? 0 : d->vex_l;
    int s = scalar ? xmm_rm(b, d, dbl ? 8 : 4, next) : ymm_src(b, d, L, next);
    if (L) { hi_ld(b, V_HA, a); hi_ld(b, 28, x); }
    for (int h = 0; h <= L; h++) {
        int X = h ? 28 : x, A = h ? V_HA : a, S = h ? V_HS : s, R = h ? V_HX : V_LO;
        int m1 = order == 9 ? X : A, m2 = order == 0xA ? X : S, c = order == 9 ? A : order == 0xA ? S : X;
        int neg_p = kind >= 0xC && kind != 0xA && kind != 0xB;            // -(a*b): FNMADD FNMSUB
        if (kind == 6 || kind == 7) {                                     // c negated in the lanes that subtract (FMADDSUB:
            E(b, a64_fneg(dbl, 22, c));                                   //   even ones; FMSUBADD: odd ones)
            lane_mask(b, V_T, dbl ? (kind == 6 ? 1 : 2) : (kind == 6 ? 5 : 10), dbl);
            E(b, a64_vbsl(V_T, 22, c)); E(b, a64_vmov(22, V_T));
            c = 22;
        } else if (kind == 0xA || kind == 0xB || kind >= 0xE) { E(b, a64_fneg(dbl, 22, c)); c = 22; }   // a*b - c, -(a*b) - c
        if (neg_p) { E(b, a64_fneg(dbl, 29, m1)); m1 = 29; }              // -(a*b) as (-a)*b
        if (scalar) E(b, a64_fmadd_sc(dbl, R, m1, m2, c));                // (scalar: no flags from the other lanes)
        else { E(b, a64_vmov(R, c)); E(b, a64_fmla(dbl, R, m1, m2)); }
        int ops[3] = { m1, m2, c };
        nan_fixn(b, dbl, scalar, ops, 3, R);
    }
    if (scalar) E(b, dbl ? a64_ins_d(x, 0, V_LO, 0) : a64_ins_s(x, 0, V_LO, 0)); else E(b, a64_vmov(x, V_LO));
    if (L) hi_st(b, V_HX, x); else hi_zero(b, x);
    return 0;
}

// Maps 0F38 and 0F3A with VEX: AVX's own forms, AVX2's, FMA, and the SSSE3 / SSE4.1 forms with a first source.
static int vex_avx(xlat_buf *b, const x86_insn *d, uint64_t next) {
    int op = d->opcode, L = d->vex_l, a = d->vex_vvvv, x = d->reg, imm = (int)(uint8_t)d->imm, m38 = d->map == X86_MAP_0F38, w = d->vex_w;
    if (d->mandatory != 0x66) return -1;
    if (m38 && op >= 0x96 && op <= 0xBF && (op & 15) >= 6) return vex_fma(b, d, next);
    // (AVX2's integer forms that are AVX's float forms under another name)
    if (m38 && op == 0x5A) op = 0x1A;                                     // VBROADCASTI128
    if (!m38 && (op == 0x38 || op == 0x39 || op == 0x46)) op -= op == 0x46 ? 0x40 : 0x20;   // VINSERTI128 VEXTRACTI128 VPERM2I128
    if (m38 && (op == 0x8C || op == 0x8E)) op = op == 0x8C ? 0x2C + w : 0x2E + w, w = 0;    // VPMASKMOVD/Q
    if (m38 && op >= 0x90 && op <= 0x93) {                                // gathers (in C: xlat.c's gather())
        if (d->mod == 3 || !d->has_sib) return -1;
        int vidx = d->index == 0xFF ? 4 : d->index;                       // (VSIB: index 4 is XMM4, not "none")
        if (x == a || x == vidx || a == vidx) return -1;                  // (x86: #UD)
        x86_insn e = *d; e.index = 0xFF;
        ea(b, &e, next, 0);                                               // base + displacement (+ segment)
        uint32_t sc = d->scale == 8 ? 3 : d->scale == 4 ? 2 : d->scale == 2 ? 1 : 0;
        call_helper(b, H_GATHER | ((uint32_t)op | (uint32_t)w << 8 | (uint32_t)L << 9 | (uint32_t)x << 10 | (uint32_t)a << 14 |
                                  (uint32_t)vidx << 18 | sc << 22) << 8, T_ADDR);
        return 0;
    }
    if (m38 && op == 0x13) {                                              // VCVTPH2PS (F16C)
        if (w) return -1;
        int s = xmm_rm(b, d, L ? 16 : 8, next);
        if (L) E(b, a64_fcvt_h(1, V_HS, s));
        E(b, a64_fcvt_h(0, x, s));
        if (L) hi_st(b, V_HS, x); else hi_zero(b, x);
        return 0;
    }
    if (!m38 && op == 0x1D) {                                             // VCVTPS2PH (F16C): rounded as imm says, or MXCSR
        if (w) return -1;
        if (!(imm & 4)) {                                                 // (another mode: FPCR's RMode for this instruction)
            static const int arm_rm[4] = { 0, 2, 1, 3 };
            E(b, a64_mrs_fpcr(T_T)); mov_imm(b, S0, 3ull << 22); E(b, a64_bic(T_B, T_T, S0));
            mov_imm(b, S0, (uint64_t)arm_rm[imm & 3] << 22); E(b, a64_orr(T_B, T_B, S0)); E(b, a64_msr_fpcr(T_B));
        }
        E(b, a64_fcvt_h(2, V_R, x));
        if (L) { hi_ld(b, V_HS, x); E(b, a64_fcvt_h(3, V_R, V_HS)); }
        if (!(imm & 4)) E(b, a64_msr_fpcr(T_T));
        if (d->mod == 3) { E(b, a64_vmov(d->rm, V_R)); hi_zero(b, d->rm); }
        else { ea(b, d, next, 0); E(b, L ? a64_strq(V_R, T_ADDR, 0) : a64_strd(V_R, T_ADDR, 0)); }
        return 0;
    }
    if (m38 && (op == 0x18 || op == 0x19)) {                              // VBROADCASTSS / SD
        if (w) return -1;
        int s = xmm_rm(b, d, op == 0x18 ? 4 : 8, next);
        E(b, op == 0x18 ? a64_dup_s(x, s, 0) : a64_dup_d(x, s, 0));
        if (L) hi_st(b, x, x); else hi_zero(b, x);
        return 0;
    }
    if (m38 && (op == 0x58 || op == 0x59 || op == 0x78 || op == 0x79)) {  // VPBROADCASTD/Q/B/W
        if (w) return -1;
        int lg = op == 0x58 ? 2 : op == 0x59 ? 3 : op == 0x78 ? 0 : 1;
        if (d->mod == 3) E(b, a64_umovg(lg, S0, d->rm, 0)); else { ea(b, d, next, 0); E(b, a64_ldr(1 << lg, S0, T_ADDR, 0)); }
        E(b, a64_dupg(lg, x, S0));
        if (L) hi_st(b, x, x); else hi_zero(b, x);
        return 0;
    }
    if (m38 && op == 0x1A) {                                              // VBROADCASTF128
        if (d->mod == 3 || !L || w) return -1;
        int s = ymm_src(b, d, 0, next);
        E(b, a64_vmov(x, s)); hi_st(b, s, x);
        return 0;
    }
    if (!m38 && op == 0x18) {                                             // VINSERTF128
        if (!L || w) return -1;
        int s = xmm_rm(b, d, 16, next);
        if (imm & 1) { hi_st(b, s, x); if (a != x) E(b, a64_vmov(x, a)); }
        else { hi_ld(b, V_HA, a); E(b, a64_vmov(x, s)); hi_st(b, V_HA, x); }
        return 0;
    }
    if (!m38 && op == 0x19) {                                             // VEXTRACTF128 xmm/m128, ymm
        if (!L || w) return -1;
        int v = x;
        if (imm & 1) { hi_ld(b, V_HS, x); v = V_HS; }
        if (d->mod == 3) { E(b, a64_vmov(d->rm, v)); hi_zero(b, d->rm); }
        else { ea(b, d, next, 0); E(b, a64_strq(v, T_ADDR, 0)); }
        return 0;
    }
    if (!m38 && op == 0x06) {                                             // VPERM2F128: each half one of four, or zero
        if (!L || w) return -1;
        int s = ymm_src(b, d, 1, next);
        hi_ld(b, V_HA, a);
        int src[4] = { a, V_HA, s, V_HS };
        for (int h = 0; h < 2; h++) {
            int sel = imm >> (4 * h), r = h ? V_HX : V_LO;
            if (sel & 8) E(b, a64_movi0(r)); else E(b, a64_vmov(r, src[sel & 3]));
        }
        E(b, a64_vmov(x, V_LO)); hi_st(b, V_HX, x);
        return 0;
    }
    if (!m38 && (op == 0x00 || op == 0x01)) {                             // VPERMQ / VPERMPD: quadwords picked by imm
        if (!L || !w) return -1;
        int s = ymm_src(b, d, 1, next);
        int q[4][2] = { { s, 0 }, { s, 1 }, { V_HS, 0 }, { V_HS, 1 } };
        for (int i = 0; i < 4; i++) { int k = imm >> (2 * i) & 3; E(b, a64_insv(3, i < 2 ? V_LO : V_HX, i & 1, q[k][0], q[k][1])); }
        E(b, a64_vmov(x, V_LO)); hi_st(b, V_HX, x);
        return 0;
    }
    if (m38 && (op == 0x36 || op == 0x16)) {                              // VPERMD / VPERMPS: dwords picked by the first source
        if (!L || w) return -1;
        int s = ymm_src(b, d, 1, next);
        E(b, a64_vmov(29, s)); E(b, a64_vmov(30, V_HS));                  // (the table: two registers in a row)
        hi_ld(b, V_HA, a);
        for (int h = 0; h < 2; h++) {
            int A = h ? V_HA : a;
            mov_imm(b, S0, 7); E(b, a64_dup4s(V_T, S0)); E(b, a64_vand(V_T, A, V_T));
            mov_imm(b, S0, 0x04040404u); E(b, a64_dup4s(V_U, S0)); E(b, a64_vmul(2, V_T, V_T, V_U));
            mov_imm(b, S0, 0x03020100u); E(b, a64_dup4s(V_U, S0)); E(b, a64_vadd(2, V_T, V_T, V_U));
            E(b, a64_tbl2(h ? V_HX : V_LO, 29, V_T));
        }
        E(b, a64_vmov(x, V_LO)); hi_st(b, V_HX, x);
        return 0;
    }
    if (m38 && (op == 0x0E || op == 0x0F || op == 0x17)) {                // VTESTPS VTESTPD VPTEST: ZF, CF
        if (w) return -1;
        int s = ymm_src(b, d, L, next);
        E(b, a64_vand(V_T, s, x)); E(b, a64_vbic(V_U, s, x));             // ZF from s & x, CF from s & ~x
        if (L) {
            hi_ld(b, V_HA, x);
            E(b, a64_vand(V_W, V_HS, V_HA)); E(b, a64_vorr(V_T, V_T, V_W));
            E(b, a64_vbic(V_W, V_HS, V_HA)); E(b, a64_vorr(V_U, V_U, V_W));
        }
        if (op != 0x17) {                                                 // (VTESTPS/PD: only the sign bits)
            E(b, op == 0x0E ? a64_ushr(32, V_T, V_T, 31) : a64_ushr(64, V_T, V_T, 63));
            E(b, op == 0x0E ? a64_ushr(32, V_U, V_U, 31) : a64_ushr(64, V_U, V_U, 63));
        }
        test_flags(b);
        return 0;
    }
    if (m38 && op >= 0x2C && op <= 0x2F) {                                // VMASKMOVPS/PD: loads and stores of the lanes
        if (d->mod == 3 || w) return -1;                                  //   whose mask has the sign bit (no others touched)
        int dbl = op & 1, store = op >= 0x2E, n = dbl ? 2 : 4, es = dbl ? 8 : 4;
        ea(b, d, next, 0);
        if (L) { hi_ld(b, V_HA, a); if (store) hi_ld(b, V_HS, x); }
        if (!store) { E(b, a64_movi0(V_LO)); E(b, a64_movi0(V_HX)); }
        for (int h = 0; h <= L; h++)
            for (int i = 0; i < n; i++) {
                int mk = h ? V_HA : a, v = store ? (h ? V_HS : x) : (h ? V_HX : V_LO);
                E(b, dbl ? a64_umov_d(S0, mk, i) : a64_umov_s(S0, mk, i));
                size_t skip = here(b); E(b, 0);
                uint32_t off = (uint32_t)((h * n + i) * es);
                if (store) { E(b, dbl ? a64_umov_d(S1, v, i) : a64_umov_s(S1, v, i)); E(b, a64_str(es, S1, T_ADDR, off)); }
                else { E(b, a64_ldr(es, S1, T_ADDR, off)); E(b, dbl ? a64_ins_gd(v, i, S1) : a64_ins_gs(v, i, S1)); }
                b->code[skip] = a64_tbz(S0, es * 8 - 1, (int32_t)(b->n - skip));
            }
        if (!store) { E(b, a64_vmov(x, V_LO)); if (L) hi_st(b, V_HX, x); else hi_zero(b, x); }
        return 0;
    }
    int pm = m38 && ((op >= 0x20 && op <= 0x25) || (op >= 0x30 && op <= 0x35));
    if (pm && L) {                                                        // VPMOVSX/ZX ymm: twice the source
        int k = op & 7, s = xmm_rm(b, d, 2 * pmov_size[k], next);
        E(b, a64_ext(V_W, s, s, pmov_size[k])); pmov(b, V_HX, V_W, k, op >= 0x30);
        pmov(b, x, s, k, op >= 0x30); hi_st(b, V_HX, x);
        return 0;
    }
    // Forms done on each half: R = f(A, S), A the first source (VEX.vvvv), S the r/m operand.
    int half = m38 ? (op == 0x0C || op == 0x0D || ((op == 0x45 || op == 0x47) || (op == 0x46 && !w)))
                   : (op == 0x02 || op == 0x04 || op == 0x05 || op == 0x0C || op == 0x0D || op == 0x4A || op == 0x4B || op == 0x4C);
    if (half && !(m38 && op >= 0x45) && w) return -1;
    if (half) {
        int s = ymm_src(b, d, L, next);
        if (L) hi_ld(b, V_HA, a);
        for (int h = 0; h <= L; h++) {
            int A = h ? V_HA : a, S = h ? V_HS : s, R = h ? V_HX : V_LO;
            if (m38 && op == 0x0C) {                                      // VPERMILPS by a vector: lane i is A's lane S[i] & 3
                mov_imm(b, S0, 3); E(b, a64_dup4s(V_T, S0)); E(b, a64_vand(V_T, S, V_T));
                mov_imm(b, S0, 0x04040404u); E(b, a64_dup4s(V_U, S0)); E(b, a64_mul4s(V_T, V_T, V_U));
                mov_imm(b, S0, 0x03020100u); E(b, a64_dup4s(V_U, S0)); E(b, a64_add4s(V_T, V_T, V_U));
                E(b, a64_tbl(R, A, V_T));
            } else if (m38 && op == 0x0D) {                               // VPERMILPD by a vector: bit 1 of each lane
                mov_imm(b, S0, 2); E(b, a64_dup2d(V_U, S0)); E(b, a64_vand(V_T, S, V_U));
                E(b, a64_movi0(V_U)); E(b, a64_cmeq2d(V_T, V_T, V_U));
                E(b, a64_dup_d(V_U, A, 0)); E(b, a64_dup_d(V_W, A, 1)); E(b, a64_vbsl(V_T, V_U, V_W)); E(b, a64_vmov(R, V_T));
            } else if (m38) {                                             // VPSRLVD/Q VPSRAVD VPSLLVD/Q: counts per lane
                int sz = w ? 3 : 2, es = 8 << sz;
                mov_imm(b, S0, (uint64_t)es); E(b, a64_dupg(sz, V_U, S0));
                if (w) { E(b, a64_cmhi2d(V_W, S, V_U)); E(b, a64_vbsl(V_W, V_U, S)); } else E(b, a64_umin(2, V_W, S, V_U));   // (at most the width)
                if (op != 0x47) E(b, a64_vneg(sz, V_W, V_W));
                E(b, op == 0x46 ? a64_sshl(sz, R, A, V_W) : a64_ushl(sz, R, A, V_W));
            } else if (op == 0x04) shuf4(b, R, S, S, imm);                // VPERMILPS by imm (the r/m operand only)
            else if (op == 0x05) {                                        // VPERMILPD by imm: two bits per half
                E(b, a64_ins_d(R, 0, S, imm >> (2 * h) & 1)); E(b, a64_ins_d(R, 1, S, imm >> (2 * h + 1) & 1));
            } else if (op == 0x0C || op == 0x0D || op == 0x02) {          // VBLENDPS / VBLENDPD / VPBLENDD by imm
                int n = op == 0x0D ? 2 : 4;
                E(b, a64_vmov(R, A));
                for (int i = 0; i < n; i++)
                    if (imm >> (n * h + i) & 1) E(b, n == 4 ? a64_ins_s(R, i, S, i) : a64_ins_d(R, i, S, i));
            } else {                                                      // VBLENDVPS / VBLENDVPD / VPBLENDVB: the mask's sign bits
                int mk = imm >> 4 & 15, es = op == 0x4A ? 32 : op == 0x4B ? 64 : 8;
                if (h) { hi_ld(b, V_T, mk); mk = V_T; }
                E(b, a64_sshr(es, V_T, mk, es - 1));
                E(b, a64_vbsl(V_T, S, A)); E(b, a64_vmov(R, V_T));
            }
        }
        E(b, a64_vmov(x, V_LO));
        if (L) hi_st(b, V_HX, x); else hi_zero(b, x);
        return 0;
    }
    // SSSE3 / SSE4.1 forms (xlat_sse4)
    if (m38 && (op == 0x10 || op == 0x14 || op == 0x15)) return -1;      // (VEX has blends of its own: 4A-4C)
    x86_insn e = *d;
    e.opsize = w ? 8 : 4;
    if (!m38 && op >= 0x14 && op <= 0x17) return L ? -1 : (xlat_sse(b, &e, next) ? -1 : 0);   // to general registers or memory
    if (!m38 && op >= 0x60 && op <= 0x63) {                              // (flags, ECX or XMM0: its upper half cleared)
        if (L || xlat_sse(b, &e, next)) return -1;
        if (!(op & 1)) hi_zero(b, 0);
        return 0;
    }
    int one = m38 ? ((op >= 0x1C && op <= 0x1E) || op == 0x2A || op == 0xDB || pm) : (op == 0x08 || op == 0x09 || op == 0xDF);
    int only128 = m38 ? op >= 0xDB : (op == 0x0A || op == 0x0B || op == 0x20 || op == 0x21 || op == 0x22 || op == 0x41 || op == 0x44 || op == 0xDF);
    if (only128 && L && op != 0x0A && op != 0x0B) return -1;            // (VROUNDSS/SD: L is ignored)
    return vex_sse(b, d, e, x, one ? -1 : a, only128 ? 0 : L, 0, next);
}

// A VEX instruction (AVX). 0: translated; -1: not supported (yet).
static int xlat_vex(xlat_buf *b, const x86_insn *d, uint64_t next) {
    if (d->vex_vvvv > 15) return -1;
    int bmi = xlat_bmi(b, d, next);                                       // (general-purpose: BMI1 / BMI2)
    if (bmi != 1) return bmi;
    if (d->map == X86_MAP_0F38 || d->map == X86_MAP_0F3A) return vex_avx(b, d, next);
    if (d->map != X86_MAP_0F) return -1;
    int op = d->opcode, pf = d->mandatory, L = d->vex_l, a = d->vex_vvvv, m = d->mod == 3;
    int scalar = (pf == 0xF3 || pf == 0xF2) && ((op >= 0x10 && op <= 0x11) || (op >= 0x2A && op <= 0x2D) || (op >= 0x51 && op <= 0x53) ||
                                                (op >= 0x58 && op <= 0x5F && op != 0x5B) || op == 0xC2);
    x86_insn e = *d;
    e.opsize = d->vex_w ? 8 : 4;
    if (op == 0xAE) return d->mod != 3 && pf == 0 && !L && ((d->reg & 7) == 2 || (d->reg & 7) == 3) && !xlat_sse(b, &e, next) ? 0 : -1;   // VLDMXCSR VSTMXCSR
    if (op == 0x77) {                                                     // VZEROUPPER, VZEROALL
        E(b, a64_addi(S0, XC, CTX_YMMH));
        for (int i = 0; i < 16; i++) E(b, a64_stp(XZR, XZR, S0, 16 * i));
        if (L) for (int i = 0; i < 16; i++) E(b, a64_movi0(i));
        return 0;
    }
    // Stores to memory: the SSE form, then the upper half
    if (!m && (op == 0x11 || op == 0x29 || op == 0x7F || op == 0x2B || op == 0xE7 || op == 0x13 || op == 0x17 || op == 0xD6)) {
        if (xlat_sse(b, &e, next)) return -1;
        if (L && !scalar) { hi_ld(b, V_HS, d->reg); E(b, a64_strq(V_HS, T_ADDR, 16)); }
        return 0;
    }
    // No vector result: compares to flags, conversions and moves to general registers
    if (op == 0x2E || op == 0x2F || op == 0x2C || op == 0x2D || (op == 0x7E && pf == 0x66) || op == 0xC5 || ((op == 0x50 || op == 0xD7) && !L))
        return xlat_sse(b, &e, next) ? -1 : 0;
    if (op == 0x50 || op == 0xD7) {                                       // VMOVMSKPS/PD VPMOVMSKB ymm
        if (!m || scalar || (op == 0xD7 && pf != 0x66)) return -1;
        if (op == 0xD7) {                                                 // (the two halves' 16 bits each)
            x86_insn h = e; h.vex_l = 0;
            if (xlat_sse(b, &h, next)) return -1;
            E(b, a64_mov(T_R, T_A));
            hi_ld(b, V_HS, d->rm); h.rm = V_HS; h.reg = d->reg;
            if (xlat_sse(b, &h, next)) return -1;
            E(b, a64_orr_lsl(T_A, T_R, T_A, 16)); wr_reg(b, d, d->reg, T_A, 8);
            return 0;
        }
        E(b, a64_movz(T_A, 0, 0));
        movmsk(b, d->rm, pf == 0x66, 0);
        hi_ld(b, V_HS, d->rm); movmsk(b, V_HS, pf == 0x66, pf == 0x66 ? 2 : 4);
        wr_reg(b, d, d->reg, T_A, 4);
        return 0;
    }
    // Stores between registers are loads the other way round
    if (m && (op == 0x11 || op == 0x29 || op == 0x7F || (op == 0xD6 && pf == 0x66))) {
        e.reg = d->rm; e.rm = d->reg;
        e.opcode = op == 0x11 ? 0x10 : op == 0x29 ? 0x28 : op == 0x7F ? 0x6F : 0x7E;
        if (op == 0xD6) e.mandatory = 0xF3;
        return vex_sse(b, d, e, d->rm, op == 0x11 && scalar ? a : -1, scalar || op == 0xD6 ? 0 : L, 0, next);
    }
    if (op >= 0x71 && op <= 0x73) {                                       // shifts by imm: VEX.vvvv is the destination
        if (!m) return -1;
        int src = d->rm;
        if (L) hi_ld(b, V_HA, src);
        if (a != src) E(b, a64_vmov(a, src));
        e.rm = (uint8_t)a;
        if (xlat_sse(b, &e, next)) return -1;
        if (!L) { hi_zero(b, a); return 0; }
        e.rm = V_HA;
        if (xlat_sse(b, &e, next)) return -1;
        hi_st(b, V_HA, a);
        return 0;
    }
    if (L && op == 0x5A && pf == 0x66) {                                  // VCVTPD2PS xmm, ymm/m256
        int s = ymm_src(b, d, 1, next);
        E(b, a64_fcvtn2s(V_R, s)); E(b, a64_fcvtn2_4s(V_R, V_HS)); E(b, a64_vmov(d->reg, V_R)); hi_zero(b, d->reg);
        return 0;
    }
    if (L && op == 0xE6 && (pf == 0xF2 || pf == 0x66)) {                  // VCVTPD2DQ VCVTTPD2DQ xmm, ymm/m256
        int s = ymm_src(b, d, 1, next);
        pd2dq(b, V_LO, s, pf == 0x66); pd2dq(b, V_HX, V_HS, pf == 0x66);
        E(b, a64_ins_d(V_LO, 1, V_HX, 0)); E(b, a64_vmov(d->reg, V_LO)); hi_zero(b, d->reg);
        return 0;
    }
    if (L && ((op == 0x5A && pf == 0) || (op == 0xE6 && pf == 0xF3))) {   // VCVTPS2PD VCVTDQ2PD ymm, xmm/m128
        int s = xmm_rm(b, d, 16, next);
        if (op == 0x5A) { E(b, a64_fcvtl2_2d(V_HS, s)); E(b, a64_fcvtl2d(d->reg, s)); }
        else { E(b, a64_sxtl2_2d(V_HS, s)); E(b, a64_scvtf2d(V_HS, V_HS)); E(b, a64_sxtl2d(V_R, s)); E(b, a64_scvtf2d(d->reg, V_R)); }
        hi_st(b, V_HS, d->reg);
        return 0;
    }
    // The rest: SSE forms with a first source, except those without one (VEX.vvvv = 1111)
    int one = (op == 0x10 && (!scalar || !m)) || op == 0x28 || op == 0x6F || op == 0x5B || ((op >= 0x51 && op <= 0x53) && !scalar) ||
              (op == 0x5A && !scalar) || op == 0xE6 || op == 0x70 || (op == 0x7E && pf == 0xF3) || op == 0x6E || op == 0xF0 ||
              ((op == 0x12 || op == 0x16) && (pf == 0xF2 || pf == 0xF3));
    int only128 = scalar || op == 0x2A || op == 0x6E || op == 0x7E || op == 0xC4 || ((op == 0x12 || op == 0x16) && (pf == 0 || pf == 0x66));
    if (only128 && L && !scalar) return -1;
    int s128 = (op >= 0xD1 && op <= 0xD3) || op == 0xE1 || op == 0xE2 || (op >= 0xF1 && op <= 0xF3);   // (shift counts)
    return vex_sse(b, d, e, d->reg, one ? -1 : a, only128 ? 0 : L, s128, next);
}
