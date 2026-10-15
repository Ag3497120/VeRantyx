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

// The r/m operand as an XMM register number: the register itself, or V_B loaded from memory (size bytes: 4, 8, 16).
static int xmm_rm(xlat_buf *b, const x86_insn *d, int size, uint64_t next) {
    if (d->mod == 3) return d->rm;
    ea(b, d, next, 0);
    E(b, size == 16 ? a64_ldrq(V_B, T_ADDR, 0) : size == 8 ? a64_ldrd(V_B, T_ADDR, 0) : a64_ldrs(V_B, T_ADDR, 0));
    return V_B;
}

// x86 NaN results for r = a op src (src < 0: unary). dbl: 64-bit lanes; scalar: only lane 0 is checked.
static void nan_fix(xlat_buf *b, int dbl, int scalar, int a, int src, int r) {
    size_t skip = here(b);
    if (scalar) { E(b, dbl ? a64_fcmpd(r, r) : a64_fcmps(r, r)); skip = here(b); E(b, 0); }
    else {
        E(b, dbl ? a64_fcmeq2d(V_T, r, r) : a64_fcmeq4s(V_T, r, r)); E(b, a64_uminv4s(V_T, V_T)); E(b, a64_fmov_ws(S0, V_T));
        skip = here(b); E(b, 0);
    }
    if (dbl) { mov_imm(b, S0, 0xFFF8000000000000ull); E(b, a64_dup2d(V_U, S0)); mov_imm(b, S0, 0x0008000000000000ull); E(b, a64_dup2d(V_W, S0)); }
    else { mov_imm(b, S0, 0xFFC00000u); E(b, a64_dup4s(V_U, S0)); mov_imm(b, S0, 0x00400000u); E(b, a64_dup4s(V_W, S0)); }
    #define ISNAN(dst, x) do { E(b, dbl ? a64_fcmeq2d(dst, x, x) : a64_fcmeq4s(dst, x, x)); E(b, a64_vmvn(dst, dst)); } while (0)
    ISNAN(V_X, r); E(b, a64_vbsl(V_X, V_U, r));                         // invalid: indefinite
    if (src >= 0) { ISNAN(V_T, src); E(b, a64_vorr(V_U, src, V_W)); E(b, a64_vbsl(V_T, V_U, V_X)); E(b, a64_vmov(V_X, V_T)); }
    ISNAN(V_T, a); E(b, a64_vorr(V_U, a, V_W)); E(b, a64_vbsl(V_T, V_U, V_X)); E(b, a64_vmov(r, V_T));
    #undef ISNAN
    if (scalar) patch_bcond(b, skip, VC); else patch_cbz(b, skip, 1, S0);
}

// Float to integer, x86 style: the ARM conversion (truncating or nearest) of lane 0 of v into general register t
// (size 4 or 8), and 0x80000000... for NaN and out of range.
static void cvt_to_int(xlat_buf *b, int t, int v, int dbl, int size, int trunc) {
    if (dbl) E(b, trunc ? a64_fcvtzs_xd(t, v) : a64_fcvtns_xd(t, v)); else E(b, trunc ? a64_fcvtzs_xs(t, v) : a64_fcvtns_xs(t, v));
    mov_imm(b, S1, size == 8 ? 0x8000000000000000ull : 0x80000000ull);
    if (size == 4) { E(b, a64_sbfx(S0, t, 0, 32)); E(b, a64_cmp(S0, t)); E(b, a64_csel(t, t, S1, EQ)); }   // out of 32-bit range
    else { mov_imm(b, S0, 0x7FFFFFFFFFFFFFFFull); E(b, a64_cmp(t, S0)); E(b, a64_csel(t, S1, t, EQ)); }   // saturated high
    E(b, dbl ? a64_fcmpd(v, v) : a64_fcmps(v, v)); E(b, a64_csel(t, S1, t, VS));                     // NaN
}

// The same for 4 lanes: x86 gives 0x80000000 where the value is NaN or not in [-2^31, 2^31).
static void cvt4_to_int(xlat_buf *b, int dst, int v, int trunc) {
    E(b, trunc ? a64_fcvtzs4s(V_R, v) : a64_fcvtns4s(V_R, v));
    mov_imm(b, S0, 0x4F000000u); E(b, a64_dup4s(V_U, S0));                  // 2^31
    mov_imm(b, S0, 0xCF000000u); E(b, a64_dup4s(V_W, S0));                  // -2^31
    E(b, a64_fcmgt4s(V_T, V_U, v)); E(b, a64_fcmge4s(V_X, v, V_W)); E(b, a64_vand(V_T, V_T, V_X));   // in range (not NaN)
    mov_imm(b, S0, 0x80000000u); E(b, a64_dup4s(V_U, S0));
    E(b, a64_vbsl(V_T, V_R, V_U)); E(b, a64_vmov(dst, V_T));
}

// SHUFPS / PSHUFD: four 32-bit lanes picked by imm from lo (lanes 0, 1) and hi (lanes 2, 3).
static void shuf4(xlat_buf *b, int dst, int lo, int hi, int imm) {
    for (int i = 0; i < 4; i++) E(b, a64_ins_s(V_R, i, i < 2 ? lo : hi, imm >> (2 * i) & 3));
    E(b, a64_vmov(dst, V_R));
}

// 1: not an SSE instruction; 0: translated; -1: not supported (yet).
static int xlat_sse(xlat_buf *b, const x86_insn *d, uint64_t next) {
    if (d->map != X86_MAP_0F) return 1;
    int op = d->opcode, pf = d->mandatory, x = d->reg, dbl = pf == 0x66 || pf == 0xF2;
    int scalar = pf == 0xF3 || pf == 0xF2, esz = scalar ? (pf == 0xF2 ? 8 : 4) : 16;
    if (op == 0xAE && d->mod == 3 && (d->reg & 7) >= 5) { E(b, 0xD5033BBFu); return 0; }   // LFENCE MFENCE SFENCE: DMB ISH
    int sse_op = (op >= 0x10 && op <= 0x17) || (op >= 0x28 && op <= 0x2F) || (op >= 0x50 && op <= 0x7F) || op == 0xC2 ||
                 op == 0xC6 || op >= 0xD0;
    if (!sse_op) return 1;
    if (op >= 0x60 && pf == 0 && !(op >= 0x50 && op <= 0x5F) && op != 0xC2 && op != 0xC6) return -1;   // (MMX)

    switch (op) {
    case 0x10: case 0x11: {                                               // MOVUPS/UPD/SS/SD
        if (op == 0x10) {
            if (!scalar) { int s = xmm_rm(b, d, 16, next); E(b, a64_vmov(x, s)); }
            else if (d->mod == 3) E(b, pf == 0xF2 ? a64_ins_d(x, 0, d->rm, 0) : a64_ins_s(x, 0, d->rm, 0));
            else { int s = xmm_rm(b, d, esz, next); E(b, a64_vmov(x, s)); }   // (from memory: upper lanes zeroed)
        } else if (d->mod == 3) {
            if (!scalar) E(b, a64_vmov(d->rm, x));
            else E(b, pf == 0xF2 ? a64_ins_d(d->rm, 0, x, 0) : a64_ins_s(d->rm, 0, x, 0));
        } else { ea(b, d, next, 0); E(b, esz == 16 ? a64_strq(x, T_ADDR, 0) : esz == 8 ? a64_strd(x, T_ADDR, 0) : a64_strs(x, T_ADDR, 0)); }
        return 0;
    }
    case 0x28: { int s = xmm_rm(b, d, 16, next); E(b, a64_vmov(x, s)); return 0; }                 // MOVAPS/PD
    case 0x29: if (d->mod == 3) E(b, a64_vmov(d->rm, x)); else { ea(b, d, next, 0); E(b, a64_strq(x, T_ADDR, 0)); } return 0;
    case 0x6F: if (pf != 0x66 && pf != 0xF3) return -1; { int s = xmm_rm(b, d, 16, next); E(b, a64_vmov(x, s)); } return 0;   // MOVDQA/U
    case 0x7F: if (pf != 0x66 && pf != 0xF3) return -1;
        if (d->mod == 3) E(b, a64_vmov(d->rm, x)); else { ea(b, d, next, 0); E(b, a64_strq(x, T_ADDR, 0)); } return 0;
    case 0x12: case 0x13: case 0x16: case 0x17: {                         // MOVLPS/PD MOVHLPS MOVHPS/PD MOVLHPS
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
        E(b, op == 0x54 ? a64_vand(x, x, s) : op == 0x55 ? a64_vbic(x, s, x) : op == 0x56 ? a64_vorr(x, x, s) : a64_veor(x, x, s));
        return 0;
    }
    case 0x58: case 0x59: case 0x5C: case 0x5E: case 0x51: {              // ADD MUL SUB DIV SQRT (ps pd ss sd)
        int s = xmm_rm(b, d, esz, next);
        if (op == 0x51) {
            if (scalar) { E(b, a64_vmov(V_T, x)); E(b, dbl ? a64_ins_d(V_T, 0, s, 0) : a64_ins_s(V_T, 0, s, 0)); s = V_T; }
            E(b, dbl ? a64_fsqrt2d(V_R, s) : a64_fsqrt4s(V_R, s));
            E(b, a64_vmov(22, s));                                        // (nan_fix uses V_T to V_X)
            nan_fix(b, dbl, scalar, 22, -1, V_R);
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
    case 0x5D: case 0x5F: {                                               // MIN / MAX: a < b ? a : b, a > b ? a : b
        int s = xmm_rm(b, d, esz, next);
        if (op == 0x5D) E(b, dbl ? a64_fcmgt2d(V_T, s, x) : a64_fcmgt4s(V_T, s, x));
        else E(b, dbl ? a64_fcmgt2d(V_T, x, s) : a64_fcmgt4s(V_T, x, s));
        E(b, a64_vbsl(V_T, x, s));
        if (scalar) E(b, dbl ? a64_ins_d(x, 0, V_T, 0) : a64_ins_s(x, 0, V_T, 0)); else E(b, a64_vmov(x, V_T));
        return 0;
    }
    case 0xC2: {                                                          // CMPPS/PD/SS/SD
        // The predicates: 32 with VEX (imm & 31; the upper 16 differ only in signalling). Without VEX the SDM has 8 and
        // reserves the rest; Rosetta 2 takes imm & 7 for CMPPS/CMPPD but imm & 15 for CMPSS/CMPSD, and so does this.
        int s = xmm_rm(b, d, esz, next), p = (int)(d->imm & (scalar ? 15 : 7));
        #define EQ_(dd, a_, b_) E(b, dbl ? a64_fcmeq2d(dd, a_, b_) : a64_fcmeq4s(dd, a_, b_))
        #define GT_(dd, a_, b_) E(b, dbl ? a64_fcmgt2d(dd, a_, b_) : a64_fcmgt4s(dd, a_, b_))
        #define GE_(dd, a_, b_) E(b, dbl ? a64_fcmge2d(dd, a_, b_) : a64_fcmge4s(dd, a_, b_))
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
        E(b, pf ? a64_fcmpd(x, s) : a64_fcmps(x, s));
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
    case 0xE6: {                                                          // CVTDQ2PD (others later)
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
        s = xmm_rm(b, d, 16, next);
        break;
    default: return -1;
    }
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
    }
    return 0;
}
