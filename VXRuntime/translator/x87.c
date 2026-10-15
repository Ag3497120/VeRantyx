// x87 for translated code (phase 3c, TRANSLATOR.md): the 80-bit floating-point unit in software. Translated code calls
// xlat_x87 through the helper interface with the instruction's opcode, ModRM byte and memory address. Exact as the SDM
// describes it: 64-bit significands, precision and rounding control, the stack with its faults, the exception flags
// and condition codes, every load, store and conversion. Unmasked exceptions only set ES and B (as masked otherwise:
// #MF is not raised).
#include "x87.h"
#include <string.h>

typedef unsigned __int128 u128;
typedef struct { uint64_t m; uint16_t se; } f80;
enum { CL_ZERO, CL_FIN, CL_INF, CL_QNAN, CL_SNAN, CL_BAD };
typedef struct { int sign, cls; int32_t exp; u128 sig; } ux;        // finite: sig * 2^(exp - 127), bit 127 of sig set
enum { FE_I = 1, FE_D = 2, FE_Z = 4, FE_O = 8, FE_U = 16, FE_P = 32, FE_SF = 64 };
enum { SW_C0 = 0x100, SW_C1 = 0x200, SW_C2 = 0x400, SW_C3 = 0x4000, SW_CC = 0x4700 };
static const f80 INDEF = { 0xC000000000000000ull, 0xFFFF };

static int clz128(u128 v) { uint64_t h = (uint64_t)(v >> 64); return h ? __builtin_clzll(h) : 64 + __builtin_clzll((uint64_t)v); }

// ---- formats ----------------------------------------------------------------------------------------------------------
static ux unpack(f80 v) {
    ux u = { v.se >> 15, CL_FIN, 0, 0 };
    int e = v.se & 0x7FFF;
    if (e == 0x7FFF) {
        if (!(v.m >> 63)) u.cls = CL_BAD;                                  // pseudo-infinity, pseudo-NaN
        else if (!(v.m << 1)) u.cls = CL_INF;
        else u.cls = (v.m >> 62 & 1) ? CL_QNAN : CL_SNAN;
        u.sig = (u128)v.m << 64;
        return u;
    }
    if (e == 0) {
        if (!v.m) { u.cls = CL_ZERO; return u; }
        int lz = __builtin_clzll(v.m);                                     // denormal (or pseudo-denormal: J set)
        u.sig = (u128)v.m << (64 + lz); u.exp = -16382 - lz;
        return u;
    }
    if (!(v.m >> 63)) { u.cls = CL_BAD; return u; }                        // unnormal
    u.sig = (u128)v.m << 64; u.exp = e - 16383;
    return u;
}
static int is_denormal(f80 v) { return !(v.se & 0x7FFF) && v.m; }
static int is_nan(ux u) { return u.cls == CL_QNAN || u.cls == CL_SNAN; }
static f80 quiet(f80 v) { v.m |= 1ull << 62; return v; }
static f80 make_inf(int s) { f80 v = { 1ull << 63, (uint16_t)(s << 15 | 0x7FFF) }; return v; }
static f80 make_zero(int s) { f80 v = { 0, (uint16_t)(s << 15) }; return v; }

typedef struct { int p; int32_t emin, emax; } fmt;                     // significand bits, normal exponents
static const fmt F80 = { 64, -16382, 16383 }, F64 = { 53, -1022, 1023 }, F32 = { 24, -126, 127 };

// sig >> sh (1 <= sh), rounded by rc: the kept bits (they may carry into one more bit); inexact and rounded up out.
static uint64_t rnd(int sign, u128 sig, int sticky, int sh, int rc, int *inexact, int *up) {
    uint64_t kept; int guard, rest;
    if (sh >= 129) { kept = 0; guard = 0; rest = sig != 0 || sticky; }
    else if (sh == 128) { kept = 0; guard = (int)(sig >> 127); rest = (sig << 1) != 0 || sticky; }
    else { kept = (uint64_t)(sig >> sh); guard = (int)(sig >> (sh - 1) & 1); rest = (sig & (((u128)1 << (sh - 1)) - 1)) != 0 || sticky; }
    *inexact = guard || rest;
    int inc = rc == 0 ? guard && (rest || (kept & 1)) : rc == 1 ? sign && *inexact : rc == 2 ? !sign && *inexact : 0;
    *up = inc;
    return kept + (uint64_t)inc;
}

typedef struct { int sign; int32_t e; uint64_t kept; int cls; } rounded;   // cls: CL_FIN (kept: p bits), CL_ZERO, CL_INF
// A finite nonzero value rounded to format f: flags (PE, UE, OE) and C1 (rounded up) into *fl and *c1.
static rounded round_to(int sign, int32_t exp, u128 sig, int sticky, fmt f, int rc, int *fl, int *c1) {
    rounded r = { sign, exp, 0, CL_FIN };
    int inexact, up, sh = 128 - f.p;
    uint64_t k = rnd(sign, sig, sticky, sh, rc, &inexact, &up);
    int32_t e = exp;
    if (f.p == 64 ? k == 0 && up : k >> f.p) { k = f.p == 64 ? 1ull << 63 : k >> 1; e++; }   // (carry out of the significand)
    int tiny = e < f.emin;                                                  // (after rounding, as the SDM says)
    if (exp < f.emin) {                                                     // denormal: fewer bits
        int extra = f.emin - exp;
        k = rnd(sign, sig, sticky, sh + (extra > 200 ? 200 : extra), rc, &inexact, &up);
        e = f.emin;
        if (!k) r.cls = CL_ZERO;
    } else if (e > f.emax) {                                                // overflow
        *fl |= FE_O | FE_P;
        int to_inf = rc == 0 || (rc == 1 && sign) || (rc == 2 && !sign);
        if (to_inf) { r.cls = CL_INF; *c1 = 1; } else { r.e = f.emax; r.kept = f.p == 64 ? ~0ull : (1ull << f.p) - 1; *c1 = 0; }
        return r;
    }
    if (inexact) { *fl |= FE_P; if (tiny) *fl |= FE_U; }
    *c1 = inexact && up;
    r.e = e; r.kept = k;
    return r;
}
static f80 pack80(rounded r, int p) {
    if (r.cls == CL_ZERO) return make_zero(r.sign);
    if (r.cls == CL_INF) return make_inf(r.sign);
    f80 v; v.m = p == 64 ? r.kept : r.kept << (64 - p);
    v.se = (uint16_t)(r.sign << 15 | (v.m >> 63 ? r.e + 16383 : 0));
    return v;
}
static uint64_t pack_bits(rounded r, fmt f, int ebits) {             // IEEE single / double
    uint64_t s = (uint64_t)r.sign << (f.p + ebits - 1), emax = (1ull << ebits) - 1;
    if (r.cls == CL_ZERO) return s;
    if (r.cls == CL_INF) return s | emax << (f.p - 1);
    uint64_t E = r.kept >> (f.p - 1) ? (uint64_t)(r.e + (int32_t)(emax >> 1)) : 0;
    return s | E << (f.p - 1) | (r.kept & ((1ull << (f.p - 1)) - 1));
}

// ---- the machine ------------------------------------------------------------------------------------------------------
typedef struct { x86_ctx *c; int fl; int c1; } fpu;
static int top(fpu *u) { return u->c->fsw >> 11 & 7; }
static void set_top(fpu *u, int t) { u->c->fsw = (uint16_t)((u->c->fsw & ~0x3800) | (t & 7) << 11); }
static int phys(fpu *u, int i) { return (top(u) + i) & 7; }
static f80 rget(fpu *u, int r) { f80 v; memcpy(&v.m, u->c->st[r], 8); memcpy(&v.se, u->c->st[r] + 8, 2); return v; }
static void rput(fpu *u, int r, f80 v) { memcpy(u->c->st[r], &v.m, 8); memcpy(u->c->st[r] + 8, &v.se, 2); u->c->ftag |= (uint8_t)(1 << r); }
static int empty(fpu *u, int i) { return !(u->c->ftag >> phys(u, i) & 1); }
static int rc_of(fpu *u) { return u->c->fcw >> 10 & 3; }
static int pc_bits(fpu *u) { int pc = u->c->fcw >> 8 & 3; return pc == 0 ? 24 : pc == 2 ? 53 : 64; }
// ST(i), or (empty) a stack underflow: IE, SF, C1 = 0 and the indefinite NaN. *bad set then.
static f80 st(fpu *u, int i, int *bad) {
    if (empty(u, i)) { u->fl |= FE_I | FE_SF; u->c1 = 0; if (bad) *bad = 1; return INDEF; }
    return rget(u, phys(u, i));
}
static void set_st(fpu *u, int i, f80 v) { rput(u, phys(u, i), v); }
static void pop(fpu *u) { u->c->ftag &= (uint8_t)~(1 << phys(u, 0)); set_top(u, top(u) + 1); }
static void push(fpu *u, f80 v) {
    int t = (top(u) - 1) & 7;
    if (u->c->ftag >> t & 1) { u->fl |= FE_I | FE_SF; u->c1 = 1; v = INDEF; }   // stack overflow
    set_top(u, t); rput(u, t, v);
}
// The flags into FSW (ES and B when one is unmasked); C1 as the instruction left it.
static void finish(fpu *u, int set_c1) {
    x86_ctx *c = u->c;
    c->fsw |= (uint16_t)(u->fl & 0x7F);
    if (c->fsw & ~c->fcw & 0x3F) c->fsw |= 0x8080;
    if (set_c1) c->fsw = (uint16_t)((c->fsw & ~SW_C1) | (u->c1 ? SW_C1 : 0));
}

// x87's NaN rule for two operands: a QNaN over an SNaN; of two of a kind, the larger significand (quieted).
static f80 nan2(fpu *u, f80 a, ux ua, f80 b, ux ub) {
    if (ua.cls == CL_SNAN || ub.cls == CL_SNAN) u->fl |= FE_I;
    if (!is_nan(ub)) return quiet(a);
    if (!is_nan(ua)) return quiet(b);
    if (ua.cls != ub.cls) return ua.cls == CL_QNAN ? a : b;
    uint64_t ma = a.m << 1, mb = b.m << 1;
    if (ma != mb) return quiet(ma > mb ? a : b);
    return quiet(!(a.se >> 15) ? a : b);                                    // (equal: the positive one)
}

// ---- arithmetic -------------------------------------------------------------------------------------------------------
enum { OP_ADD, OP_MUL, OP_SUB, OP_DIV };
static f80 finite_result(fpu *u, int sign, int32_t exp, u128 sig, int sticky, int p) {
    rounded r = round_to(sign, exp, sig, sticky, (fmt){ p, F80.emin, F80.emax }, rc_of(u), &u->fl, &u->c1);
    return pack80(r, p);
}
static f80 add_ux(fpu *u, ux a, ux b, int p) {                         // finite, nonzero; b may be negated by the caller
    if (a.exp < b.exp || (a.exp == b.exp && a.sig < b.sig)) { ux t = a; a = b; b = t; }
    u128 sa = a.sig >> 1, sb = b.sig >> 1;                                  // (room for a carry)
    int d = a.exp - b.exp, sticky = 0;
    if (d >= 127) { sticky = sb != 0; sb = 0; }
    else if (d) { sticky = (sb & (((u128)1 << d) - 1)) != 0; sb >>= d; }
    u128 s;
    if (a.sign == b.sign) s = sa + sb;
    else {
        s = sa - sb - (sticky ? 1 : 0);                                     // (the sticky bit: something below, borrowed)
        if (!s && !sticky) return make_zero(rc_of(u) == 1);                 // exact zero: -0 only rounding down
    }
    int lz = clz128(s);
    return finite_result(u, a.sign, a.exp + 1 - lz, s << lz, sticky, p);
}
static f80 arith(fpu *u, int op, f80 a, f80 b, int p, int den) {       // a op b (SUB: a - b, DIV: a / b); den: an operand was a
                                                                        //   denormal single or double
    ux ua = unpack(a), ub = unpack(b);
    if (ua.cls == CL_BAD || ub.cls == CL_BAD) { u->fl |= FE_I; return INDEF; }   // (the SDM's order: unsupported, NaNs,
    if (is_nan(ua) || is_nan(ub)) return nan2(u, a, ua, b, ub);                //   other invalid and divide by zero,
    if (den) u->fl |= FE_D;                                                     //   denormal last; a denormal single or
    int za = ua.cls == CL_ZERO, zb = ub.cls == CL_ZERO, ia = ua.cls == CL_INF, ib = ub.cls == CL_INF;   //   double: when loaded)
    if (op == OP_SUB) { ub.sign ^= 1; op = OP_ADD; }
    int invalid = op == OP_ADD ? ia && ib && ua.sign != ub.sign : op == OP_MUL ? (ia && zb) || (za && ib) : (ia && ib) || (za && zb);
    if (invalid) { u->fl |= FE_I; return INDEF; }
    if (op == OP_DIV && zb && !ia) { u->fl |= FE_Z; return make_inf(ua.sign ^ ub.sign); }
    if (is_denormal(a) || is_denormal(b)) u->fl |= FE_D;
    if (op == OP_ADD) {
        if (ia || ib) return make_inf(ia ? ua.sign : ub.sign);
        if (za && zb) return make_zero(ua.sign == ub.sign ? ua.sign : rc_of(u) == 1);
        if (za || zb) {                                                     // (x + 0: x, rounded to the precision)
            ux x = za ? ub : ua;
            return finite_result(u, x.sign, x.exp, x.sig, 0, p);
        }
        return add_ux(u, ua, ub, p);
    }
    int sign = ua.sign ^ ub.sign;
    if (op == OP_MUL) {
        if (ia || ib) return make_inf(sign);
        if (za || zb) return make_zero(sign);
        u128 pr = (u128)(uint64_t)(ua.sig >> 64) * (uint64_t)(ub.sig >> 64);
        if (pr >> 127) return finite_result(u, sign, ua.exp + ub.exp + 1, pr, 0, p);
        return finite_result(u, sign, ua.exp + ub.exp, pr << 1, 0, p);
    }
    // DIV
    if (ia) return make_inf(sign);
    if (ib || za) return make_zero(sign);
    uint64_t x = (uint64_t)(ua.sig >> 64), y = (uint64_t)(ub.sig >> 64);
    uint64_t q0 = x / y, r = x % y;
    u128 n1 = (u128)r << 64; uint64_t q1 = (uint64_t)(n1 / y); r = (uint64_t)(n1 % y);
    u128 n2 = (u128)r << 64; uint64_t q2 = (uint64_t)(n2 / y); r = (uint64_t)(n2 % y);
    if (q0) return finite_result(u, sign, ua.exp - ub.exp, (u128)1 << 127 | (u128)q1 << 63 | q2 >> 1, (q2 & 1) || r, p);
    return finite_result(u, sign, ua.exp - ub.exp - 1, (u128)q1 << 64 | q2, r != 0, p);
}
static f80 fsqrt(fpu *u, f80 a) {
    ux ua = unpack(a);
    if (ua.cls == CL_BAD) { u->fl |= FE_I; return INDEF; }
    if (is_nan(ua)) { if (ua.cls == CL_SNAN) u->fl |= FE_I; return quiet(a); }
    if (ua.cls == CL_ZERO) return a;
    if (ua.sign) { u->fl |= FE_I; return INDEF; }
    if (ua.cls == CL_INF) return a;
    if (is_denormal(a)) u->fl |= FE_D;
    // value = m * 2^e (m: 64 bits); with e even, sqrt = isqrt(m << 126) * 2^((e - 126) / 2)
    uint64_t m = (uint64_t)(ua.sig >> 64);
    int32_t e = ua.exp - 63;
    uint64_t n[3];                                                          // the radicand, 192 bits (n[2] highest)
    u128 mm = e & 1 ? (u128)m << 1 : m; if (e & 1) e--;
    u128 lo = mm << 126; n[0] = (uint64_t)lo; n[1] = (uint64_t)(lo >> 64); n[2] = (uint64_t)(mm >> 2);
    u128 root = 0, rem = 0;
    for (int i = 95; i >= 0; i--) {                                        // two bits at a time, from the top
        int bit = 2 * i;
        uint64_t two = n[bit / 64] >> (bit % 64) & 3;
        rem = rem << 2 | two;
        u128 trial = root << 2 | 1;
        if (rem >= trial) { rem -= trial; root = root << 1 | 1; } else root <<= 1;
    }
    int lz = clz128(root);
    return finite_result(u, 0, (e - 126) / 2 + 127 - lz, root << lz, rem != 0, pc_bits(u));
}

// a <=> b: 0 greater, 1 less, 2 equal, 3 unordered; IE for any NaN (signal) or SNaNs only.
static int compare(fpu *u, f80 a, f80 b, int signal, int den) {
    ux ua = unpack(a), ub = unpack(b);
    if (ua.cls == CL_BAD || ub.cls == CL_BAD) { u->fl |= FE_I; return 3; }
    if (is_nan(ua) || is_nan(ub)) { if (signal || ua.cls == CL_SNAN || ub.cls == CL_SNAN) u->fl |= FE_I; return 3; }
    if (is_denormal(a) || is_denormal(b) || den) u->fl |= FE_D;
    if (ua.cls == CL_ZERO && ub.cls == CL_ZERO) return 2;
    // ordering of sign-magnitude values: via ordered keys
    int sa = ua.cls == CL_ZERO ? 0 : ua.sign ? -1 : 1, sb = ub.cls == CL_ZERO ? 0 : ub.sign ? -1 : 1;
    if (sa != sb) return sa > sb ? 0 : 1;
    // same sign, both nonzero: magnitude compare (infinity largest)
    int32_t ea = ua.cls == CL_INF ? 1 << 20 : ua.exp, eb = ub.cls == CL_INF ? 1 << 20 : ub.exp;
    u128 ma = ua.cls == CL_INF ? 0 : ua.sig, mb = ub.cls == CL_INF ? 0 : ub.sig;
    int mag = ea != eb ? (ea > eb ? 1 : -1) : ma != mb ? (ma > mb ? 1 : -1) : 0;
    if (!mag) return 2;
    return (mag > 0) ^ (sa < 0) ? 0 : 1;
}
static void set_cc(fpu *u, int r) {                                      // C3 C2 C0 from a comparison
    static const uint16_t cc[4] = { 0, SW_C0, SW_C3, SW_C3 | SW_C2 | SW_C0 };
    u->c->fsw = (uint16_t)((u->c->fsw & ~(SW_C0 | SW_C2 | SW_C3)) | cc[r]);
}
static void set_eflags(fpu *u, int r) {                                  // FCOMI: ZF PF CF; OF SF AF cleared
    static const uint64_t fl[4] = { 0, 1, 0x40, 0x45 };
    u->c->flags = (u->c->flags & ~0x8D5ull) | fl[r];
}

// ---- conversions ------------------------------------------------------------------------------------------------------
// Single / double to 80 bits, exactly. For FLD (den == NULL) an SNaN is quieted with IE and a denormal raises DE; for
// an arithmetic operand an SNaN stays one (the NaN rules see it) and *den says whether it was denormal.
static f80 from_ieee(fpu *u, uint64_t bits, fmt f, int ebits, int *den) {
    int sign = (int)(bits >> (f.p + ebits - 1) & 1), emax = (1 << ebits) - 1;
    int E = (int)(bits >> (f.p - 1) & (uint64_t)emax);
    uint64_t frac = bits & ((1ull << (f.p - 1)) - 1);
    if (E == emax) {
        if (!frac) return make_inf(sign);
        f80 v = { 1ull << 63 | frac << (64 - f.p), (uint16_t)(sign << 15 | 0x7FFF) };
        if (!den && !(frac >> (f.p - 2))) { u->fl |= FE_I; v = quiet(v); }  // SNaN
        return v;
    }
    if (!E) {
        if (!frac) return make_zero(sign);
        if (den) *den = 1; else u->fl |= FE_D;
        int lz = __builtin_clzll(frac);
        // value = frac * 2^(emin - (p - 1)); frac << lz has its top bit at 63: exponent emin - (p - 1) + 63 - lz
        f80 v = { frac << lz, (uint16_t)(sign << 15 | (f.emin - (f.p - 1) + 63 - lz + 16383)) };
        return v;
    }
    f80 v = { 1ull << 63 | frac << (64 - f.p), (uint16_t)(sign << 15 | (E - (emax >> 1) + 16383)) };
    return v;
}
static uint64_t to_ieee(fpu *u, f80 a, fmt f, int ebits) {               // rounded by RC (not PC)
    ux ua = unpack(a);
    uint64_t sbit = (uint64_t)ua.sign << (f.p + ebits - 1), emax = (1ull << ebits) - 1;
    if (ua.cls == CL_BAD) { u->fl |= FE_I; ua = unpack(INDEF); sbit = 1ull << (f.p + ebits - 1); a = INDEF; }
    if (is_nan(ua)) {
        if (ua.cls == CL_SNAN) u->fl |= FE_I;
        uint64_t frac = (a.m << 1) >> (64 - f.p + 1) | 1ull << (f.p - 2);    // (top fraction bits, quieted)
        return sbit | emax << (f.p - 1) | frac;
    }
    if (ua.cls == CL_ZERO) return sbit;
    if (ua.cls == CL_INF) return sbit | emax << (f.p - 1);
    rounded r = round_to(ua.sign, ua.exp, ua.sig, 0, f, rc_of(u), &u->fl, &u->c1);
    return pack_bits(r, f, ebits);
}
static f80 from_int(int64_t v) {
    if (!v) return make_zero(0);
    int s = v < 0; uint64_t m = s ? 0 - (uint64_t)v : (uint64_t)v;
    int lz = __builtin_clzll(m);
    f80 r = { m << lz, (uint16_t)(s << 15 | (16383 + 63 - lz)) };
    return r;
}
// To an integer of size bytes (trunc: toward zero, else RC): out of range or NaN gives IE and the integer indefinite.
static int64_t to_int_lim(fpu *u, f80 a, int64_t lim, int trunc, int *invalid);
static int64_t to_int(fpu *u, f80 a, int size, int trunc, int *invalid) {
    return to_int_lim(u, a, size == 2 ? 0x7FFF : size == 4 ? 0x7FFFFFFF : 0x7FFFFFFFFFFFFFFFll, trunc, invalid);
}
// The same with any limit (|result| <= lim, or -lim - 1): inexact (PE, C1) only when the result fits.
static int64_t to_int_lim(fpu *u, f80 a, int64_t lim, int trunc, int *invalid) {
    ux ua = unpack(a);
    *invalid = 0;
    if (ua.cls == CL_BAD || is_nan(ua) || ua.cls == CL_INF) { u->fl |= FE_I; *invalid = 1; return -lim - 1; }
    if (ua.cls == CL_ZERO) return 0;
    int rc = trunc ? 3 : rc_of(u), inexact, up;                             // (no #D for integer stores)
    uint64_t mag;
    if (ua.exp >= 64) { u->fl |= FE_I; *invalid = 1; return -lim - 1; }
    mag = rnd(ua.sign, ua.sig, 0, 127 - ua.exp, rc, &inexact, &up);       // (127 - exp: the bits below the point)
    if (ua.exp == 63 && (ua.sig >> 64) && up && !mag) { u->fl |= FE_I; *invalid = 1; return -lim - 1; }   // (carry out)
    if (ua.sign ? mag > (uint64_t)lim + 1 : mag > (uint64_t)lim) { u->fl |= FE_I; *invalid = 1; return -lim - 1; }   // (no PE then)
    if (inexact) { u->fl |= FE_P; u->c1 = up; }
    return ua.sign ? (int64_t)(0 - mag) : (int64_t)mag;
}

// ---- the instructions -------------------------------------------------------------------------------------------------
static const struct { u128 sig; int32_t exp; } constants[7] = {            // FLD1 FLDL2T FLDL2E FLDPI FLDLG2 FLDLN2 FLDZ
    { (u128)1 << 127, 0 },
    { (u128)0xD49A784BCD1B8AFEull << 64 | 0x492BF6FF4DAFDB4Cull, 1 },
    { (u128)0xB8AA3B295C17F0BBull << 64 | 0xBE87FED0691D3E88ull, 0 },
    { (u128)0xC90FDAA22168C234ull << 64 | 0xC4C6628B80DC1CD1ull, 1 },
    { (u128)0x9A209A84FBCFF798ull << 64 | 0x8F8959AC0B7C9178ull, -2 },
    { (u128)0xB17217F7D1CF79ABull << 64 | 0xC9E3B39803F2F6AFull, -1 },
    { 0, 0 },
};
static f80 load_const(fpu *u, int k) {
    if (k == 6) return make_zero(0);
    // rounded as RC says (no flags), from 128 bits (Intel's documentation speaks of 66; Rosetta 2 rounds as from more)
    u128 sig = constants[k].sig;
    int fl = 0, c1 = 0;
    rounded r = round_to(0, constants[k].exp, sig, 0, F80, rc_of(u), &fl, &c1);
    return pack80(r, 64);
}

static f80 rndint(fpu *u, f80 a) {
    ux ua = unpack(a);
    if (ua.cls == CL_BAD) { u->fl |= FE_I; return INDEF; }
    if (is_nan(ua)) { if (ua.cls == CL_SNAN) u->fl |= FE_I; return quiet(a); }
    if (ua.cls != CL_FIN) return a;
    if (is_denormal(a)) u->fl |= FE_D;
    if (ua.exp >= 63) return a;
    int inexact, up;
    uint64_t m = rnd(ua.sign, ua.sig, 0, 127 - ua.exp, rc_of(u), &inexact, &up);
    if (inexact) { u->fl |= FE_P; u->c1 = up; }
    if (!m) return make_zero(ua.sign);
    f80 r = from_int((int64_t)m); r.se |= (uint16_t)(ua.sign << 15);
    return r;
}
static f80 fscale(fpu *u, f80 a, f80 b) {
    ux ua = unpack(a), ub = unpack(b);
    if (ua.cls == CL_BAD || ub.cls == CL_BAD) { u->fl |= FE_I; return INDEF; }
    if (is_nan(ua) || is_nan(ub)) return nan2(u, a, ua, b, ub);
    if (is_denormal(a) || is_denormal(b)) u->fl |= FE_D;
    if (ub.cls == CL_INF) {
        if (!ub.sign) { if (ua.cls == CL_ZERO) { u->fl |= FE_I; return INDEF; } return ua.cls == CL_FIN ? make_inf(ua.sign) : a; }
        if (ua.cls == CL_INF) { u->fl |= FE_I; return INDEF; }
        return make_zero(ua.sign);
    }
    if (ua.cls != CL_FIN) return a;
    int64_t n = 0;                                                          // trunc(b), held to a range past any exponent
    if (ub.cls == CL_FIN && ub.exp >= 0) n = ub.exp > 20 ? 1 << 20 : (int64_t)(ub.sig >> (127 - ub.exp));
    if (ub.sign) n = -n;
    return finite_result(u, ua.sign, ua.exp + (int32_t)n, ua.sig, 0, 64);
}

// ---- transcendental instructions ---------------------------------------------------------------------------------------
// Computed with 128-bit significands (each operation off by about 2^-127; the results by a few times that), then rounded
// once as RC says: correctly rounded but for values within some 2^-120 of a rounding boundary. FSIN, FCOS, FSINCOS and
// FPTAN reduce their argument by Intel's 66-bit value of pi, exactly, as x86 processors do (and Rosetta 2).
typedef struct { int s; int32_t e; u128 m; } bf;                       // m * 2^(e - 127), bit 127 of m set (or m = 0)
static bf bf_norm(int s, int32_t e, u128 m) { if (!m) return (bf){ s, 0, 0 }; int lz = clz128(m); return (bf){ s, e - lz, m << lz }; }
static bf bf_of(f80 v) { ux u = unpack(v); return (bf){ u.sign, u.exp, u.sig }; }   // (finite)
static bf bf_int(int64_t v) { return v < 0 ? bf_norm(1, 127, (u128)(uint64_t)-v) : bf_norm(0, 127, (u128)v); }
static bf bf_neg(bf a) { a.s ^= 1; return a; }
static u128 mul_hi(u128 a, u128 b) {
    uint64_t ah = (uint64_t)(a >> 64), al = (uint64_t)a, bh = (uint64_t)(b >> 64), bl = (uint64_t)b;
    u128 hh = (u128)ah * bh, hl = (u128)ah * bl, lh = (u128)al * bh, ll = (u128)al * bl;
    u128 mid = (ll >> 64) + (uint64_t)hl + (uint64_t)lh;
    return hh + (hl >> 64) + (lh >> 64) + (mid >> 64);
}
static bf bf_mul(bf a, bf b) { if (!a.m || !b.m) return (bf){ a.s ^ b.s, 0, 0 }; return bf_norm(a.s ^ b.s, a.e + b.e + 1, mul_hi(a.m, b.m)); }
static bf bf_add(bf a, bf b) {
    if (!a.m) return b;
    if (!b.m) return a;
    if (a.e < b.e || (a.e == b.e && a.m < b.m)) { bf t = a; a = b; b = t; }
    int d = a.e - b.e;
    u128 am = a.m >> 1, bm = d >= 127 ? 0 : b.m >> (d + 1);
    return bf_norm(a.s, a.e + 1, a.s == b.s ? am + bm : am - bm);
}
static bf bf_sub(bf a, bf b) { return bf_add(a, bf_neg(b)); }
static bf bf_div(bf a, bf b) {
    if (!a.m) return a;
    u128 r = a.m, d = b.m, q = 0; int top = 0;
    for (int i = 0; i < 128; i++) {
        q <<= 1;
        if (top || r >= d) { r -= d; q |= 1; }
        top = (int)(r >> 127); r <<= 1;
    }
    return bf_norm(a.s ^ b.s, a.e - b.e, q);
}
static const bf BF_ONE = { 0, 0, (u128)1 << 127 };
static const bf BF_PI = { 0, 1, (u128)0xC90FDAA22168C234ull << 64 | 0xC4C6628B80DC1CD1ull };
static const bf BF_LN2 = { 0, -1, (u128)0xB17217F7D1CF79ABull << 64 | 0xC9E3B39803F2F6AFull };
static const bf BF_LOG2E = { 0, 0, (u128)0xB8AA3B295C17F0BBull << 64 | 0xBE87FED0691D3E89ull };
static const bf ATAN16[17] = {                                          // atan(k / 16)
    { 0, 0, 0 },
    { 0, -5, (u128)0xFFAADDB967EF4E36ull << 64 | 0xCB2792DC0E2E0D51ull }, { 0, -4, (u128)0xFEADD4D5617B6E32ull << 64 | 0xC897989F3E888EF8ull },
    { 0, -3, (u128)0xBDCBDA5E72D81134ull << 64 | 0x7B0B4F881C9C7488ull }, { 0, -3, (u128)0xFADBAFC96406EB15ull << 64 | 0x6DC79EF5F7A217E6ull },
    { 0, -2, (u128)0x9B13B9B83F5E5E69ull << 64 | 0xC5ABB498D27AF328ull }, { 0, -2, (u128)0xB7B0CA0F26F78473ull << 64 | 0x8AA32122DCFE4483ull },
    { 0, -2, (u128)0xD327761E611FE5B6ull << 64 | 0x427C95E9001E7136ull }, { 0, -2, (u128)0xED63382B0DDA7B45ull << 64 | 0x6FE445ECBC3A8D03ull },
    { 0, -1, (u128)0x832BF4A6D9867E2Aull << 64 | 0x4B6A09CB61A515C1ull }, { 0, -1, (u128)0x8F005D5EF7F59F9Bull << 64 | 0x5C835E1665C43748ull },
    { 0, -1, (u128)0x9A2F80E671BDDA20ull << 64 | 0x4226F8E2204FF3BDull }, { 0, -1, (u128)0xA4BC7D1934F70924ull << 64 | 0x19A87F2A457DAC9Full },
    { 0, -1, (u128)0xAEAC4C38B4D8C080ull << 64 | 0x14725E2F3E52070Aull }, { 0, -1, (u128)0xB8053E2BC2319E73ull << 64 | 0xCB2DA55210A4443Dull },
    { 0, -1, (u128)0xC0CE85B8AC526640ull << 64 | 0x89DD62C46E92FA25ull }, { 0, -1, (u128)0xC90FDAA22168C234ull << 64 | 0xC4C6628B80DC1CD1ull },
};
static f80 bf_round(fpu *u, bf a) {                                     // rounded as RC says; never exact (PE)
    if (!a.m) return make_zero(a.s);
    return finite_result(u, a.s, a.e, a.m, 1, 64);
}
// sin and cos of r, |r| <= pi/4 (Taylor series: the terms fall by r^2 / (n (n + 1)) each)
static void bf_sincos(bf r, bf *sn, bf *cs) {
    bf r2 = bf_mul(r, r), t = r, s = r, c = BF_ONE, tc = BF_ONE;
    for (int n = 1; n < 60; n += 2) {
        t = bf_neg(bf_div(bf_mul(t, r2), bf_int((int64_t)(n + 1) * (n + 2))));
        tc = bf_neg(bf_div(bf_mul(tc, r2), bf_int((int64_t)n * (n + 1))));
        s = bf_add(s, t); c = bf_add(c, tc);
        if ((!t.m || t.e < s.e - 132) && (!tc.m || tc.e < c.e - 132)) break;
    }
    *sn = s; *cs = c;
}
// x (finite, |x| < 2^63) reduced by pi66/2 exactly: r in [-pi66/4, pi66/4] and the quadrant.
static bf reduce66(ux x, int *quad) {
    static const u128 P = (u128)0x3ull << 64 | 0x243F6A8885A308D3ull;   // pi * 2^64, rounded to 66 bits
    // |x| = M * 2^E (M: 64 bits), pi66 / 2 = P * 2^-65: in units of 2^-65, X = M * 2^(E + 65)
    uint64_t M = (uint64_t)(x.sig >> 64);
    int32_t E = x.exp - 63;
    *quad = 0;
    if (E + 65 < 0) return (bf){ x.sign, x.exp, x.sig };               // (tiny: no reduction)
    u128 rem = M; unsigned q = 0;
    if (rem >= P) { rem -= P; q = 1; }
    for (int32_t i = 0; i < E + 65; i++) { rem <<= 1; q <<= 1; if (rem >= P) { rem -= P; q |= 1; } }
    int flip = rem << 1 > P;                                             // (beyond pi66 / 4: from the next multiple)
    if (flip) { rem = P - rem; q++; }
    *quad = (int)(q & 3);
    bf r = bf_norm(flip, -65 + 127, rem);                                // rem * 2^-65
    if (x.sign) { r.s ^= 1; *quad = (4 - *quad) & 3; }
    return r;
}
// atan(z) for z >= 0
static bf bf_atan(bf z) {
    int inv = z.m && z.e >= 0 && !(z.e == 0 && z.m == BF_ONE.m);        // (z > 1: pi/2 - atan(1/z))
    if (inv) z = bf_div(BF_ONE, z);
    // nearest k/16, then atan(z) = atan(k/16) + atan((z - k/16) / (1 + z k/16))
    int k = 0;
    if (z.m) { int32_t sh = 122 - z.e; k = sh >= 128 ? 0 : (int)(((uint64_t)(z.m >> sh) + 1) >> 1); if (k > 16) k = 16; }   // round(16 z)
    bf c = bf_div(bf_int(k), bf_int(16));
    bf t = k ? bf_div(bf_sub(z, c), bf_add(BF_ONE, bf_mul(z, c))) : z;
    bf t2 = bf_mul(t, t), term = t, sum = t;
    for (int n = 3; n < 200; n += 2) {
        term = bf_neg(bf_mul(term, t2));
        bf add = bf_div(term, bf_int(n));
        sum = bf_add(sum, add);
        if (!add.m || add.e < sum.e - 132) break;
    }
    bf r = bf_add(ATAN16[k], sum);
    if (inv) r = bf_sub(bf_mul(BF_PI, (bf){ 0, -1, (u128)1 << 127 }), r);
    return r;
}
// ln(m) for m in [1/sqrt 2, sqrt 2]: 2 atanh((m - 1) / (m + 1))
static bf bf_ln_near1(bf m) {
    bf z = bf_div(bf_sub(m, BF_ONE), bf_add(m, BF_ONE)), z2 = bf_mul(z, z), term = z, sum = z;
    for (int n = 3; n < 400; n += 2) {
        term = bf_mul(term, z2);
        bf add = bf_div(term, bf_int(n));
        sum = bf_add(sum, add);
        if (!add.m || add.e < sum.e - 132) break;
    }
    return bf_add(sum, sum);
}
// log2(x), x > 0 finite
static bf bf_log2(bf x) {
    int32_t e = x.e; bf m = { 0, 0, x.m };                                // x = m * 2^e, m in [1, 2)
    if (m.m > ((u128)0xB504F333F9DE6484ull << 64)) { m.e = -1; e++; }    // (m > sqrt 2: halve it)
    return bf_add(bf_int(e), bf_mul(bf_ln_near1(m), BF_LOG2E));
}
// 2^x - 1, |x| <= 1 (or a little more): e^t - 1 with t = x ln 2, by its series (relative precision kept for small x)
static bf bf_exp2m1(bf x) {
    bf t = bf_mul(x, BF_LN2), term = t, sum = t;
    for (int n = 2; n < 100; n++) {
        term = bf_div(bf_mul(term, t), bf_int(n));
        sum = bf_add(sum, term);
        if (!term.m || term.e < sum.e - 132) break;
    }
    return sum;
}

static int classify_tag(f80 v) {                                         // the 2-bit tag: 0 valid, 1 zero, 2 special
    ux uv = unpack(v);
    if (uv.cls == CL_ZERO) return 1;
    if (uv.cls == CL_FIN && !is_denormal(v)) return 0;
    return 2;
}
static uint16_t full_tags(fpu *u) {
    uint16_t t = 0;
    for (int r = 0; r < 8; r++) t |= (uint16_t)((u->c->ftag >> r & 1 ? classify_tag(rget(u, r)) : 3) << (2 * r));
    return t;
}

static void rd(void *dst, uint64_t a, size_t n) { memcpy(dst, (const void *)(uintptr_t)a, n); }
static void wr(uint64_t a, const void *src, size_t n) { memcpy((void *)(uintptr_t)a, src, n); }

// FNSTENV / FLDENV (28 bytes, the 32-bit protected-mode layout); instruction and data pointers are not kept (0).
static void store_env(fpu *u, uint64_t a) {
    uint32_t env[7] = { u->c->fcw, u->c->fsw, full_tags(u), 0, 0, 0, 0 };   // (the reserved halves: 0, as Rosetta 2 writes them)
    wr(a, env, 28);
}
static void load_env(fpu *u, uint64_t a) {
    uint32_t env[7]; rd(env, a, 28);
    u->c->fcw = (uint16_t)env[0]; u->c->fsw = (uint16_t)env[1];
    uint8_t t = 0; for (int r = 0; r < 8; r++) if ((env[2] >> (2 * r) & 3) != 3) t |= (uint8_t)(1 << r);
    u->c->ftag = t;
}

static f80 load_bcd(uint64_t a) {
    uint8_t b[10]; rd(b, a, 10);
    uint64_t v = 0;
    for (int i = 8; i >= 0; i--) v = v * 100 + (uint64_t)(b[i] >> 4) * 10 + (b[i] & 15);
    f80 r = from_int((int64_t)v);
    if (b[9] & 0x80) r.se |= 0x8000;
    return r;
}
static void store_bcd(fpu *u, uint64_t a, f80 x) {
    ux ux_ = unpack(x);
    uint8_t b[10] = { 0 };
    int invalid = 0;
    int64_t v = 0;
    if (ux_.cls == CL_BAD || is_nan(ux_) || ux_.cls == CL_INF) { invalid = 1; u->fl |= FE_I; }
    else { v = to_int_lim(u, x, 999999999999999999ll, 0, &invalid); if (v == -999999999999999999ll - 1) invalid = 1; }
    if (invalid) {
        static const uint8_t indef[10] = { 0, 0, 0, 0, 0, 0, 0, 0xC0, 0xFF, 0xFF };
        wr(a, indef, 10); return;
    }
    uint64_t m = v < 0 ? (uint64_t)-v : (uint64_t)v;
    for (int i = 0; i < 9; i++) { b[i] = (uint8_t)(m % 10 | (m / 10 % 10) << 4); m /= 100; }
    if (v < 0 || ((x.se >> 15) && !v)) b[9] = 0x80;
    wr(a, b, 10);
}

static int cond_true(x86_ctx *c, int k) {                               // FCMOVcc: B E BE U, negated with 4
    uint64_t f = c->flags;
    int r = k == 0 ? (int)(f & 1) : k == 1 ? (int)(f >> 6 & 1) : k == 2 ? (int)((f & 1) | (f >> 6 & 1)) : (int)(f >> 2 & 1);
    return r;
}

// FPATAN: atan(y / x) in the quadrant of (x, y), with the SDM's table for zeros and infinities.
static f80 fpatan(fpu *u, f80 y, ux uy, f80 x, ux ux_) {
    (void)y; (void)x;
    int sy = uy.sign, sx = ux_.sign;
    bf half = { 0, -1, (u128)1 << 127 }, quarter = { 0, -2, (u128)1 << 127 };
    bf r;
    if (is_denormal(x) || is_denormal(y)) u->fl |= FE_D;                  // (whatever the other operand)
    if (uy.cls == CL_ZERO || (ux_.cls == CL_INF && uy.cls != CL_INF)) {   // y = 0, or x infinite: 0 or pi
        if (!sx) return make_zero(sy);
        r = BF_PI;
    } else if (uy.cls == CL_INF) {                                       // y infinite: pi/2, or pi/4 / 3pi/4 against infinities
        if (ux_.cls == CL_INF) r = sx ? bf_mul(BF_PI, (bf){ 0, -1, (u128)3 << 126 }) : bf_mul(BF_PI, quarter);
        else r = bf_mul(BF_PI, half);
    } else if (ux_.cls == CL_ZERO) r = bf_mul(BF_PI, half);              // x = 0: pi/2
    else {
        bf by = bf_of(y), bx = bf_of(x);
        by.s = bx.s = 0;
        r = bf_atan(bf_div(by, bx));
        if (sx) r = bf_sub(BF_PI, r);
    }
    r.s = sy;
    return bf_round(u, r);
}
// FYL2X: y * log2(x); FYL2XP1: y * log2(x + 1). Invalid, divide by zero and infinities as the SDM's tables say.
static f80 flog(fpu *u, f80 y, ux uy, f80 x, ux ux_, int p1) {
    int sy = uy.sign;
    // the logarithm's class: negative (invalid), -inf (x = 0, or -1 for P1), 0 (x = 1, or 0 for P1), +inf, finite
    int lzero = p1 ? ux_.cls == CL_ZERO : (ux_.cls == CL_FIN && !ux_.sign && ux_.exp == 0 && ux_.sig == (u128)1 << 127);
    if (!p1 && ux_.cls != CL_ZERO && ux_.sign) { u->fl |= FE_I; return INDEF; }
    if (p1 && ux_.sign && ux_.cls == CL_INF) { u->fl |= FE_I; return INDEF; }
    if (!p1 && ux_.cls == CL_ZERO) {                                     // log2 0 = -inf
        if (uy.cls == CL_ZERO) { u->fl |= FE_I; return INDEF; }
        if (uy.cls != CL_INF) u->fl |= FE_Z;                               // (divide by zero: before any DE)
        return make_inf(!sy);
    }
    if (ux_.cls == CL_INF) {                                             // log2 +inf = +inf
        if (uy.cls == CL_ZERO) { u->fl |= FE_I; return INDEF; }
        return make_inf(sy);
    }
    if (lzero) {                                                         // log = 0 (x = 1, or x = 0 for P1)
        if (uy.cls == CL_INF) { u->fl |= FE_I; return INDEF; }
        if (is_denormal(y)) u->fl |= FE_D;
        return make_zero(sy ^ (p1 ? ux_.sign : 0));
    }
    if (uy.cls == CL_INF || uy.cls == CL_ZERO) {                         // (the logarithm's sign decides: no DE)
        int pos = p1 ? !ux_.sign : (ux_.exp > 0 || (ux_.exp == 0 && ux_.sig > (u128)1 << 127));
        return uy.cls == CL_INF ? make_inf(sy ^ !pos) : make_zero(sy ^ !pos);
    }
    if (is_denormal(x) || is_denormal(y)) u->fl |= FE_D;
    // exact logarithms (x, or 1 + x, a power of two): y times an integer, rounded once like any product
    bf arg = bf_of(x);
    if (p1) arg = ux_.cls == CL_FIN && ux_.exp > -64 && ux_.exp < 64 ? bf_add(BF_ONE, arg) : (bf){ 0, 0, 0 };
    if (arg.m == (u128)1 << 127 && !arg.s && uy.cls == CL_FIN) return arith(u, OP_MUL, y, from_int(arg.e), 64, 0);
    bf l;
    if (p1) {                                                            // ln(1 + x) = 2 atanh(x / (2 + x)): exact for small x
        bf xx = bf_of(x), z = bf_div(xx, bf_add(bf_int(2), xx)), z2 = bf_mul(z, z), term = z, sum = z;
        for (int n = 3; n < 400; n += 2) { term = bf_mul(term, z2); bf add = bf_div(term, bf_int(n)); sum = bf_add(sum, add); if (!add.m || add.e < sum.e - 132) break; }
        l = bf_mul(bf_add(sum, sum), BF_LOG2E);
    } else l = bf_log2(bf_of(x));
    if (uy.cls == CL_INF) return make_inf(sy ^ l.s);
    if (uy.cls == CL_ZERO) return make_zero(sy ^ l.s);
    return bf_round(u, bf_mul(bf_of(y), l));
}

int x87_supported(int op, int modrm) {
    int mod = modrm >> 6, reg = modrm >> 3 & 7;
    if (mod != 3) return !(op == 0xD9 && reg == 1);
    if (op == 0xD9) {
        if (reg <= 3) return reg != 2 || (modrm & 7) == 0;                  // FLD, FXCH, FNOP, (FSTP1)
        static const char ok[32] = { 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0,
                                     1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
        return ok[modrm - 0xE0];
    }
    if (op == 0xDA) return reg <= 3 || modrm == 0xE9;
    if (op == 0xDB) return reg <= 3 || (reg == 4 && (modrm & 7) <= 4) || reg == 5 || reg == 6;
    if (op == 0xDD) return reg != 7;
    if (op == 0xDE) return reg != 3 || modrm == 0xD9;
    if (op == 0xDF) return reg != 4 || modrm == 0xE0;
    return 1;                                                               // D8, DC
}

void xlat_x87(x86_ctx *c, uint32_t op, uint32_t modrm, uint64_t a) {
    fpu u_ = { c, 0, 0 }, *u = &u_;
    int mod = (int)(modrm >> 6), reg = (int)(modrm >> 3 & 7), rm = (int)(modrm & 7), bad = 0, c1 = 1;   // (c1: set C1 from u->c1)
    u->c1 = 0;
    if (mod != 3) {
        // ---- memory forms ----
        if (op == 0xD8 || op == 0xDC || op == 0xDA || op == 0xDE) {        // arithmetic with m32 / m64 / m32int / m16int
            f80 m, s0 = st(u, 0, &bad);                                   // (an empty ST(0): the indefinite NaN, as an operand)
            int den = 0;
            if (op == 0xD8) { uint32_t v; rd(&v, a, 4); m = from_ieee(u, v, F32, 8, &den); }
            else if (op == 0xDC) { uint64_t v; rd(&v, a, 8); m = from_ieee(u, v, F64, 11, &den); }
            else if (op == 0xDA) { int32_t v; rd(&v, a, 4); m = from_int(v); }
            else { int16_t v; rd(&v, a, 2); m = from_int(v); }
            if (reg == 2 || reg == 3) { set_cc(u, compare(u, s0, m, 1, den)); if (reg == 3) pop(u); finish(u, 1); return; }
            static const int ops[8] = { OP_ADD, OP_MUL, 0, 0, OP_SUB, OP_SUB, OP_DIV, OP_DIV };
            f80 r = reg == 5 || reg == 7 ? arith(u, ops[reg], m, s0, pc_bits(u), den) : arith(u, ops[reg], s0, m, pc_bits(u), den);
            set_st(u, 0, r);
            finish(u, 1); return;
        }
        if (op == 0xD9) {
            switch (reg) {
            case 0: { uint32_t v; rd(&v, a, 4); f80 x = from_ieee(u, v, F32, 8, NULL); push(u, x); break; }   // FLD m32
            case 2: case 3: { f80 x = st(u, 0, &bad); uint32_t v = (uint32_t)to_ieee(u, x, F32, 8); wr(a, &v, 4); if (reg == 3) pop(u); break; }
            case 4: load_env(u, a); return;                                 // FLDENV
            case 5: rd(&c->fcw, a, 2); return;                              // FLDCW
            case 6: store_env(u, a); c->fcw |= 0x3F; return;                // FNSTENV (exceptions masked afterwards)
            case 7: wr(a, &c->fcw, 2); return;                              // FNSTCW
            }
            finish(u, 1); return;
        }
        if (op == 0xDB) {
            switch (reg) {
            case 0: { int32_t v; rd(&v, a, 4); push(u, from_int(v)); break; }   // FILD m32
            case 1: case 2: case 3: {                                       // FISTTP FIST FISTP m32
                f80 x = st(u, 0, &bad); int inv; int32_t v = (int32_t)to_int(u, x, 4, reg == 1, &inv);
                if (bad) v = INT32_MIN;
                wr(a, &v, 4); if (reg != 2) pop(u); break;
            }
            case 5: { f80 x; rd(&x.m, a, 8); rd(&x.se, a + 8, 2); push(u, x); break; }   // FLD m80
            case 7: { f80 x = st(u, 0, &bad); wr(a, &x.m, 8); wr(a + 8, &x.se, 2); pop(u); break; }   // FSTP m80
            default: return;
            }
            finish(u, 1); return;
        }
        if (op == 0xDD) {
            switch (reg) {
            case 0: { uint64_t v; rd(&v, a, 8); push(u, from_ieee(u, v, F64, 11, NULL)); break; }   // FLD m64
            case 1: { f80 x = st(u, 0, &bad); int inv; int64_t v = to_int(u, x, 8, 1, &inv); if (bad) v = INT64_MIN; wr(a, &v, 8); pop(u); break; }
            case 2: case 3: { f80 x = st(u, 0, &bad); uint64_t v = to_ieee(u, x, F64, 11); wr(a, &v, 8); if (reg == 3) pop(u); break; }
            case 4: {                                                       // FRSTOR
                load_env(u, a);
                for (int i = 0; i < 8; i++) { f80 x; rd(&x.m, a + 28 + 10 * (uint64_t)i, 8); rd(&x.se, a + 36 + 10 * (uint64_t)i, 2);
                    int r = phys(u, i), full = c->ftag >> r & 1; rput(u, r, x); if (!full) c->ftag &= (uint8_t)~(1 << r); }
                return;
            }
            case 6: {                                                       // FNSAVE, then FNINIT
                store_env(u, a);
                for (int i = 0; i < 8; i++) { f80 x = rget(u, phys(u, i)); wr(a + 28 + 10 * (uint64_t)i, &x.m, 8); wr(a + 36 + 10 * (uint64_t)i, &x.se, 2); }
                c->fcw = 0x037F; c->fsw = 0; c->ftag = 0;
                return;
            }
            case 7: wr(a, &c->fsw, 2); return;                              // FNSTSW m16
            default: return;
            }
            finish(u, 1); return;
        }
        // DF
        switch (reg) {
        case 0: { int16_t v; rd(&v, a, 2); push(u, from_int(v)); break; }  // FILD m16
        case 1: case 2: case 3: {                                           // FISTTP FIST FISTP m16
            f80 x = st(u, 0, &bad); int inv; int16_t v = (int16_t)to_int(u, x, 2, reg == 1, &inv);
            if (bad) v = INT16_MIN;
            wr(a, &v, 2); if (reg != 2) pop(u); break;
        }
        case 4: push(u, load_bcd(a)); break;                                // FBLD
        case 5: { int64_t v; rd(&v, a, 8); push(u, from_int(v)); break; }  // FILD m64
        case 6: { f80 x = st(u, 0, &bad); store_bcd(u, a, x); pop(u); break; }   // FBSTP
        case 7: { f80 x = st(u, 0, &bad); int inv; int64_t v = to_int(u, x, 8, 0, &inv); if (bad) v = INT64_MIN; wr(a, &v, 8); pop(u); break; }
        }
        finish(u, 1); return;
    }
    // ---- register forms ----
    if (op == 0xD8 || op == 0xDC || op == 0xDE) {
        f80 s0 = st(u, 0, &bad), si = st(u, rm, &bad);
        if (reg == 2 || reg == 3) {                                         // FCOM(P) st(i) (DC/DE: aliases)
            if (op == 0xDE && reg == 3) {                                   // FCOMPP
                set_cc(u, compare(u, s0, st(u, 1, &bad), 1, 0)); pop(u); pop(u); finish(u, 1); return;
            }
            set_cc(u, compare(u, s0, si, 1, 0)); if (reg == 3 || op == 0xDE) pop(u); finish(u, 1); return;
        }
        // D8: st0 = st0 op st(i); DC / DE: st(i) = st(i) op st0 (DC E0 / DE E0: SUBR, E8: SUB; F0: DIVR, F8: DIV)
        static const int ops[8] = { OP_ADD, OP_MUL, 0, 0, OP_SUB, OP_SUB, OP_DIV, OP_DIV };
        int rev = op == 0xD8 ? (reg == 5 || reg == 7) : (reg == 4 || reg == 6);
        f80 x = op == 0xD8 ? s0 : si, y = op == 0xD8 ? si : s0;
        f80 r = rev ? arith(u, ops[reg], y, x, pc_bits(u), 0) : arith(u, ops[reg], x, y, pc_bits(u), 0);
        set_st(u, op == 0xD8 ? 0 : rm, r);
        if (op == 0xDE) pop(u);
        finish(u, 1); return;
    }
    if (op == 0xD9) {
        if (reg == 0) { f80 x = st(u, rm, &bad); push(u, x); finish(u, 1); return; }   // FLD st(i)
        if (reg == 1) {                                                     // FXCH
            f80 x = st(u, 0, &bad), y = st(u, rm, &bad);
            set_st(u, 0, y); set_st(u, rm, x); finish(u, 1); return;
        }
        if (reg == 2) return;                                               // FNOP
        if (reg == 3) { f80 x = st(u, 0, &bad); set_st(u, rm, x); pop(u); finish(u, 1); return; }   // FSTP1
        switch (modrm) {
        case 0xE0: case 0xE1: {                                             // FCHS FABS
            f80 x = st(u, 0, &bad);
            if (!bad) { if (modrm == 0xE0) x.se ^= 0x8000; else x.se &= 0x7FFF; }
            set_st(u, 0, x); finish(u, 1); return;
        }
        case 0xE4: { f80 x = st(u, 0, &bad); set_cc(u, compare(u, x, make_zero(0), 1, 0)); finish(u, 1); return; }   // FTST
        case 0xE5: {                                                        // FXAM: C3 C2 C0 the class, C1 the sign
            uint16_t cc;
            f80 x = rget(u, phys(u, 0));
            if (empty(u, 0)) cc = SW_C3 | SW_C0;
            else {
                ux ux_ = unpack(x);
                cc = ux_.cls == CL_BAD ? 0 : is_nan(ux_) ? SW_C0 : ux_.cls == CL_INF ? SW_C2 | SW_C0 : ux_.cls == CL_ZERO ? SW_C3 :
                     is_denormal(x) ? SW_C3 | SW_C2 : SW_C2;
            }
            if (x.se >> 15) cc |= SW_C1;
            c->fsw = (uint16_t)((c->fsw & ~SW_CC) | cc);
            return;
        }
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE:
            push(u, load_const(u, modrm - 0xE8)); finish(u, 1); return;
        case 0xF4: {                                                        // FXTRACT
            f80 x = st(u, 0, &bad), e, s;
            ux ux_ = unpack(x);
            if (!bad && (c->ftag >> ((top(u) - 1) & 7) & 1)) {             // (no room for the second: both indefinite, after
                if (ux_.cls == CL_ZERO) u->fl |= FE_Z;                      //   the operand's own exceptions)
                else if (ux_.cls == CL_SNAN || ux_.cls == CL_BAD) u->fl |= FE_I;
                else if (is_denormal(x)) u->fl |= FE_D;
                u->fl |= FE_I | FE_SF; set_st(u, 0, INDEF); push(u, INDEF); u->c1 = 1; finish(u, 1); return;
            }
            if (bad || ux_.cls == CL_BAD) { if (!bad) u->fl |= FE_I; e = s = INDEF; }
            else if (is_nan(ux_)) { if (ux_.cls == CL_SNAN) u->fl |= FE_I; e = s = quiet(x); }
            else if (ux_.cls == CL_ZERO) { u->fl |= FE_Z; e = make_inf(1); s = x; }
            else if (ux_.cls == CL_INF) { e = make_inf(0); s = x; }
            else { if (is_denormal(x)) u->fl |= FE_D; e = from_int(ux_.exp); s.m = (uint64_t)(ux_.sig >> 64); s.se = (uint16_t)(ux_.sign << 15 | 16383); }
            set_st(u, 0, e); push(u, s); finish(u, 1); return;
        }
        case 0xF5: case 0xF8: {                                             // FPREM1 FPREM
            f80 x = st(u, 0, &bad), y = st(u, 1, &bad), r;
            ux ua = unpack(x), ub = unpack(y);
            int near = modrm == 0xF5;
            c->fsw &= (uint16_t)~(SW_C0 | SW_C1 | SW_C2 | SW_C3);
            if (ua.cls == CL_BAD || ub.cls == CL_BAD) { u->fl |= FE_I; r = INDEF; }
            else if (is_nan(ua) || is_nan(ub)) r = nan2(u, x, ua, y, ub);
            else if (ua.cls == CL_INF || ub.cls == CL_ZERO) { u->fl |= FE_I; r = INDEF; }
            else {
                if (is_denormal(x) || is_denormal(y)) u->fl |= FE_D;
                r = x;
                c->fsw &= (uint16_t)~SW_C2;
                if (ua.cls == CL_FIN && ub.cls == CL_FIN) {
                    int32_t dexp = ua.exp - ub.exp, unit = ub.exp;              // (the remainder in units of 2^(unit - 63))
                    uint64_t ma = (uint64_t)(ua.sig >> 64), mb = (uint64_t)(ub.sig >> 64);
                    int partial = dexp >= 64, n = partial ? 32 + (int)(dexp % 32) : dexp;   // (partial: N, then d - N fewer)
                    if (partial) unit += dexp - n;
                    u128 R = 0, Y = mb; uint64_t q = 0; int sign = ua.sign, have = 1;
                    if (n >= 0) { u128 X = (u128)ma << n; q = (uint64_t)(X / Y); R = X % Y; }
                    else if (near && n == -1) { unit = ua.exp; R = ma; Y = (u128)mb << 1; }   // (x / y in [1/4, 1): 0 or 1)
                    else have = 0;                                          // |x| < |y| / 2: x
                    if (have && near && !partial && (R << 1 > Y || (R << 1 == Y && (q & 1)))) { R = Y - R; q++; sign ^= 1; }
                    if (have) {
                        if (!R) r = make_zero(ua.sign);
                        else { int lz = clz128(R); r = finite_result(u, sign, unit + 64 - lz, R << lz, 0, 64); }
                    }
                    if (partial) c->fsw |= SW_C2;
                    else c->fsw = (uint16_t)((c->fsw & ~(SW_C0 | SW_C1 | SW_C3)) | (q & 1 ? SW_C1 : 0) | (q & 2 ? SW_C3 : 0) | (q & 4 ? SW_C0 : 0));
                } else c->fsw &= (uint16_t)~(SW_C0 | SW_C1 | SW_C3);         // (x zero, or y infinite: x, quotient 0)
            }
            set_st(u, 0, r);
            finish(u, 0); return;
        }
        case 0xFE: case 0xFF: case 0xF2: case 0xFB: {                     // FSIN FCOS FPTAN FSINCOS
            f80 x = st(u, 0, &bad);
            ux ux_ = unpack(x);
            c->fsw &= (uint16_t)~SW_C2;
            int two = modrm == 0xF2 || modrm == 0xFB;                       // (a second result pushed)
            f80 r0, r1 = INDEF;                                             // r0: in ST(0)'s place; r1: pushed
            if (ux_.cls == CL_BAD) { u->fl |= FE_I; r0 = INDEF; }
            else if (is_nan(ux_)) { if (ux_.cls == CL_SNAN) u->fl |= FE_I; r0 = r1 = quiet(x); }
            else if (ux_.cls == CL_INF) { u->fl |= FE_I; r0 = INDEF; }
            else if (ux_.cls == CL_FIN && ux_.exp >= 63) { c->fsw |= SW_C2; finish(u, 1); return; }   // (out of range: unchanged)
            else if (ux_.cls == CL_ZERO) {
                f80 one = { 1ull << 63, 16383 };
                r0 = modrm == 0xFF ? one : x;                               // sin, tan: the zero; cos: 1
                r1 = modrm == 0xF2 ? one : one;                             // FPTAN pushes 1, FSINCOS cos 0 = 1
            } else {
                if (is_denormal(x)) u->fl |= FE_D;
                int q; bf r = reduce66(ux_, &q), sn, cs;
                bf_sincos(r, &sn, &cs);
                bf sinx = q == 0 ? sn : q == 1 ? cs : q == 2 ? bf_neg(sn) : bf_neg(cs);
                bf cosx = q == 0 ? cs : q == 1 ? bf_neg(sn) : q == 2 ? bf_neg(cs) : sn;
                if (modrm == 0xFE) r0 = bf_round(u, sinx);
                else if (modrm == 0xFF) r0 = bf_round(u, cosx);
                else if (modrm == 0xF2) { r0 = bf_round(u, bf_div(sinx, cosx)); r1 = (f80){ 1ull << 63, 16383 }; }
                else { r0 = bf_round(u, sinx); int c1s = u->c1; r1 = bf_round(u, cosx); u->c1 |= c1s; }
            }
            if (two && (c->ftag >> ((top(u) - 1) & 7) & 1)) {             // (no room: both indefinite)
                u->fl |= FE_I | FE_SF; set_st(u, 0, INDEF); push(u, INDEF); u->c1 = 1; finish(u, 1); return;
            }
            set_st(u, 0, r0);
            if (two) push(u, r1);
            finish(u, 1); return;
        }
        case 0xF3: case 0xF1: case 0xF9: {                                // FPATAN FYL2X FYL2XP1: into ST(1), then pop
            f80 x = st(u, 0, &bad), y = st(u, 1, &bad), r;
            ux ux_ = unpack(x), uy = unpack(y);
            if (ux_.cls == CL_BAD || uy.cls == CL_BAD) { u->fl |= FE_I; r = INDEF; }
            else if (is_nan(ux_) || is_nan(uy)) r = nan2(u, y, uy, x, ux_);
            else if (modrm == 0xF3) r = fpatan(u, y, uy, x, ux_);
            else r = flog(u, y, uy, x, ux_, modrm == 0xF9);
            set_st(u, 1, r); pop(u);
            finish(u, 1); return;
        }
        case 0xF0: {                                                        // F2XM1
            f80 x = st(u, 0, &bad), r;
            ux ux_ = unpack(x);
            if (ux_.cls == CL_BAD) { u->fl |= FE_I; r = INDEF; }
            else if (is_nan(ux_)) { if (ux_.cls == CL_SNAN) u->fl |= FE_I; r = quiet(x); }
            else if (ux_.cls == CL_ZERO) r = x;
            else if (ux_.cls == CL_INF) r = ux_.sign ? (f80){ 1ull << 63, 0xBFFF } : x;   // (2^-inf - 1 = -1)
            else if (ux_.exp == 0 && ux_.sig == (u128)1 << 127) r = ux_.sign ? (f80){ 1ull << 63, 0xBFFE } : x;   // (+-1: 1, -1/2 exactly)
            else { if (is_denormal(x)) u->fl |= FE_D; r = bf_round(u, bf_exp2m1(bf_of(x))); }
            set_st(u, 0, r);
            finish(u, 1); return;
        }
        case 0xF6: set_top(u, top(u) - 1); c->fsw &= (uint16_t)~SW_C1; return;   // FDECSTP
        case 0xF7: set_top(u, top(u) + 1); c->fsw &= (uint16_t)~SW_C1; return;   // FINCSTP
        case 0xFA: { f80 x = st(u, 0, &bad); set_st(u, 0, fsqrt(u, x)); finish(u, 1); return; }
        case 0xFC: { f80 x = st(u, 0, &bad); set_st(u, 0, rndint(u, x)); finish(u, 1); return; }
        case 0xFD: { f80 x = st(u, 0, &bad), y = st(u, 1, &bad); set_st(u, 0, fscale(u, x, y)); finish(u, 1); return; }
        }
        return;
    }
    if (op == 0xDA || op == 0xDB) {
        if (reg <= 3) {                                                     // FCMOVcc (DB: negated)
            f80 x = st(u, rm, &bad);                                     // (an empty ST(i): underflow whatever the condition)
            if (bad || (cond_true(c, reg) ^ (op == 0xDB))) set_st(u, 0, x);
            finish(u, 1); return;
        }
        if (op == 0xDA) {                                                   // FUCOMPP
            f80 x = st(u, 0, &bad), y = st(u, 1, &bad);
            set_cc(u, compare(u, x, y, 0, 0)); pop(u); pop(u); finish(u, 1); return;
        }
        if (modrm == 0xE2) { c->fsw &= 0x7F00; return; }                    // FNCLEX
        if (modrm == 0xE3) { c->fcw = 0x037F; c->fsw = 0; c->ftag = 0; return; }   // FNINIT
        if (reg == 4) return;                                               // FENI FDISI FSETPM (nothing on 387 and later)
        // FUCOMI (E8) FCOMI (F0)
        f80 x = st(u, 0, &bad), y = st(u, rm, &bad);
        int r = compare(u, x, y, reg == 6, 0);
        set_eflags(u, r); u->c1 = 0; finish(u, 1); return;
    }
    if (op == 0xDD) {
        switch (reg) {
        case 0: c->ftag &= (uint8_t)~(1 << phys(u, rm)); return;           // FFREE
        case 1: { f80 x = st(u, 0, &bad), y = st(u, rm, &bad); set_st(u, 0, y); set_st(u, rm, x); finish(u, 1); return; }   // FXCH4
        case 2: case 3: { f80 x = st(u, 0, &bad); set_st(u, rm, x); if (reg == 3) pop(u); finish(u, 1); return; }   // FST(P) st(i)
        case 4: case 5: {                                                   // FUCOM(P)
            f80 x = st(u, 0, &bad), y = st(u, rm, &bad);
            set_cc(u, compare(u, x, y, 0, 0)); if (reg == 5) pop(u); finish(u, 1); return;
        }
        }
        return;
    }
    // DF
    switch (reg) {
    case 0: c->ftag &= (uint8_t)~(1 << phys(u, rm)); set_top(u, top(u) + 1); return;   // FFREEP
    case 1: { f80 x = st(u, 0, &bad), y = st(u, rm, &bad); set_st(u, 0, y); set_st(u, rm, x); finish(u, 1); return; }   // FXCH7
    case 2: case 3: { f80 x = st(u, 0, &bad); set_st(u, rm, x); pop(u); finish(u, 1); return; }   // FSTP8 FSTP9
    case 4: c->gpr[0] = (c->gpr[0] & ~0xFFFFull) | c->fsw; return;         // FNSTSW AX
    case 5: case 6: {                                                       // FUCOMIP FCOMIP
        f80 x = st(u, 0, &bad), y = st(u, rm, &bad);
        int r = compare(u, x, y, reg == 6, 0);
        set_eflags(u, r); u->c1 = 0; pop(u); finish(u, 1); return;
    }
    }
    (void)c1;
}

// FXSAVE / FXRSTOR (64-bit layout): FCW FSW, abridged tags, the registers in stack order, MXCSR and XMM0-15.
void xlat_fxsave(x86_ctx *c, uint64_t a, int restore) {
    uint8_t img[512];
    if (!restore) {
        memset(img, 0, sizeof img);
        memcpy(img, &c->fcw, 2); memcpy(img + 2, &c->fsw, 2); img[4] = c->ftag;
        uint32_t mm = 0xFFFF; memcpy(img + 24, &c->mxcsr, 4); memcpy(img + 28, &mm, 4);
        int t = c->fsw >> 11 & 7;
        uint8_t ab = 0;
        for (int i = 0; i < 8; i++) { memcpy(img + 32 + 16 * i, c->st[(t + i) & 7], 10); }
        for (int r = 0; r < 8; r++) if (c->ftag >> r & 1) ab |= (uint8_t)(1 << r);
        img[4] = ab;
        memcpy(img + 160, c->xmm, 256);
        // (bytes 416-511 are the program's own: left as they are)
        memcpy((void *)(uintptr_t)a, img, 416);
        return;
    }
    memcpy(img, (const void *)(uintptr_t)a, 416);
    memcpy(&c->fcw, img, 2); memcpy(&c->fsw, img + 2, 2); c->ftag = img[4];
    memcpy(&c->mxcsr, img + 24, 4);
    int t = c->fsw >> 11 & 7;
    for (int i = 0; i < 8; i++) memcpy(c->st[(t + i) & 7], img + 32 + 16 * i, 10);
    memcpy(c->xmm, img + 160, 256);
}
