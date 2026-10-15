// x86-64 instruction decoder (64-bit mode only). See x86dec.h.
#include "x86dec.h"
#include <string.h>

// One-byte map: what follows the opcode. M: ModRM; immediates: B 1 byte, W 2, Z 2 or 4 (operand size), V 2, 4 or 8
// (MOV r, imm), A address-size offset (MOV moffs), E ENTER (2 + 1); X: invalid in 64-bit mode; P: prefix (handled
// before the table).
enum { N = 0, M = 1, B = 2, W = 4, Z = 8, V = 16, A = 32, E = 64, X = 128 };
static const uint8_t one[256] = {
    /*00*/ M,M,M,M,B,Z,X,X, M,M,M,M,B,Z,X,0,
    /*10*/ M,M,M,M,B,Z,X,X, M,M,M,M,B,Z,X,X,
    /*20*/ M,M,M,M,B,Z,0,X, M,M,M,M,B,Z,0,X,
    /*30*/ M,M,M,M,B,Z,0,X, M,M,M,M,B,Z,0,X,
    /*40*/ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    /*50*/ N,N,N,N,N,N,N,N, N,N,N,N,N,N,N,N,
    /*60*/ X,X,X,M,0,0,0,0, Z,M|Z,B,M|B,N,N,N,N,
    /*70*/ B,B,B,B,B,B,B,B, B,B,B,B,B,B,B,B,
    /*80*/ M|B,M|Z,X,M|B,M,M,M,M, M,M,M,M,M,M,M,M,
    /*90*/ N,N,N,N,N,N,N,N, N,N,X,N,N,N,N,N,
    /*A0*/ A,A,A,A,N,N,N,N, B,Z,N,N,N,N,N,N,
    /*B0*/ B,B,B,B,B,B,B,B, V,V,V,V,V,V,V,V,
    /*C0*/ M|B,M|B,W,N,X,X,M|B,M|Z, E,N,W,N,N,B,X,N,
    /*D0*/ M,M,M,M,X,X,X,N, M,M,M,M,M,M,M,M,
    /*E0*/ B,B,B,B,B,B,B,B, Z,Z,X,B,N,N,N,N,
    /*F0*/ 0,N,0,0,N,N,M,M, N,N,N,N,N,N,M,M,
};
// Two-byte map (0F xx). 0F 38 and 0F 3A are maps of their own; 0F 0F is 3DNow!.
static const uint8_t two[256] = {
    /*00*/ M,M,M,M,X,N,N,N, N,N,X,N,X,M,N,M|B,
    /*10*/ M,M,M,M,M,M,M,M, M,M,M,M,M,M,M,M,
    /*20*/ M,M,M,M,X,X,X,X, M,M,M,M,M,M,M,M,
    /*30*/ N,N,N,N,N,N,X,N, 0,X,0,X,X,X,X,X,
    /*40*/ M,M,M,M,M,M,M,M, M,M,M,M,M,M,M,M,
    /*50*/ M,M,M,M,M,M,M,M, M,M,M,M,M,M,M,M,
    /*60*/ M,M,M,M,M,M,M,M, M,M,M,M,M,M,M,M,
    /*70*/ M|B,M|B,M|B,M|B,M,M,M,N, M,M,X,X,M,M,M,M,
    /*80*/ Z,Z,Z,Z,Z,Z,Z,Z, Z,Z,Z,Z,Z,Z,Z,Z,
    /*90*/ M,M,M,M,M,M,M,M, M,M,M,M,M,M,M,M,
    /*A0*/ N,N,N,M,M|B,M,X,X, N,N,N,M,M|B,M,M,M,
    /*B0*/ M,M,M,M,M,M,M,M, M,M,M|B,M,M,M,M,M,
    /*C0*/ M,M,M|B,M,M|B,M|B,M|B,M, N,N,N,N,N,N,N,N,
    /*D0*/ M,M,M,M,M,M,M,M, M,M,M,M,M,M,M,M,
    /*E0*/ M,M,M,M,M,M,M,M, M,M,M,M,M,M,M,M,
    /*F0*/ M,M,M,M,M,M,M,M, M,M,M,M,M,M,M,M,
};

static int vex_imm8(int map, uint8_t op) {            // VEX/EVEX instructions with an 8-bit immediate
    if (map == X86_MAP_0F3A) return 1;
    if (map == X86_MAP_0F) return (op >= 0x70 && op <= 0x73) || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6;
    return 0;
}

// ModRM, SIB and displacement at p; fills the memory operand. Returns the bytes used, or -1 if cut off.
static int modrm(const uint8_t *p, const uint8_t *end, x86_insn *d, int rex_r, int rex_x, int rex_b) {
    if (p >= end) return -1;
    const uint8_t *s = p;
    d->has_modrm = 1;
    d->modrm = *p++;
    d->mod = d->modrm >> 6;
    d->reg = ((d->modrm >> 3) & 7) | (rex_r << 3);
    int rm = d->modrm & 7;
    d->rm = rm | (rex_b << 3);
    d->base = d->index = 0xFF;
    if (d->mod == 3) return 1;
    if (rm == 4) {
        if (p >= end) return -1;
        d->has_sib = 1;
        uint8_t sib = *p++;
        d->scale = (uint8_t)(1 << (sib >> 6));
        int idx = ((sib >> 3) & 7) | (rex_x << 3);
        if (idx != 4) d->index = (uint8_t)idx;
        int base = sib & 7;
        if (base == 5 && d->mod == 0) d->disp_size = 4;
        else d->base = (uint8_t)(base | (rex_b << 3));
    } else if (rm == 5 && d->mod == 0) {
        d->rip_relative = 1;
        d->disp_size = 4;
    } else {
        d->base = d->rm;
    }
    if (d->mod == 1) d->disp_size = 1;
    else if (d->mod == 2) d->disp_size = 4;
    if (p + d->disp_size > end) return -1;
    if (d->disp_size == 1) d->disp = (int8_t)*p;
    else if (d->disp_size == 4) { int32_t v; memcpy(&v, p, 4); d->disp = v; }
    p += d->disp_size;
    return (int)(p - s);
}

static int64_t read_imm(const uint8_t *p, int size) {
    switch (size) {
    case 1: return (int8_t)p[0];
    case 2: { int16_t v; memcpy(&v, p, 2); return v; }
    case 4: { int32_t v; memcpy(&v, p, 4); return v; }
    case 8: { int64_t v; memcpy(&v, p, 8); return v; }
    }
    return 0;
}

int x86_decode(const uint8_t *code, size_t avail, x86_insn *d) {
    memset(d, 0, sizeof *d);
    const uint8_t *p = code, *end = code + (avail < 15 ? avail : 15);
    uint8_t last_simd = 0;
    // Legacy prefixes (any order, any number while under 15 bytes), then REX right before the opcode.
    for (;;) {
        if (p >= end) return 0;
        uint8_t b = *p;
        if (b == 0xF0) d->prefixes |= X86_P_LOCK;
        else if (b == 0xF3) { d->prefixes |= X86_P_REP; last_simd = 0xF3; }
        else if (b == 0xF2) { d->prefixes |= X86_P_REPNE; last_simd = 0xF2; }
        else if (b == 0x66) { d->prefixes |= X86_P_OPSIZE; if (!last_simd) last_simd = 0x66; }
        else if (b == 0x67) d->prefixes |= X86_P_ADDRSIZE;
        else if (b == 0x64) d->prefixes |= X86_P_SEG_FS;
        else if (b == 0x65) d->prefixes |= X86_P_SEG_GS;
        else if (b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E) d->prefixes |= X86_P_SEG_OTHER;
        else if ((b & 0xF0) == 0x40 && p + 1 < end) {
            // A REX counts only right before the opcode: one followed by another prefix (or REX) is ignored, as the
            // processor does.
            uint8_t nx = p[1];
            if (!((nx & 0xF0) == 0x40 || nx == 0xF0 || nx == 0xF2 || nx == 0xF3 || nx == 0x66 || nx == 0x67 ||
                  nx == 0x64 || nx == 0x65 || nx == 0x26 || nx == 0x2E || nx == 0x36 || nx == 0x3E)) break;
        }
        else break;
        p++;
    }
    // (F2/F3 win over 66 as the mandatory prefix; between F2 and F3 the last one does)
    d->mandatory = last_simd;
    if (p < end && (*p & 0xF0) == 0x40) d->rex = *p++;
    if (p >= end) return 0;
    int rex_w = (d->rex >> 3) & 1, rex_r = (d->rex >> 2) & 1, rex_x = (d->rex >> 1) & 1, rex_b = d->rex & 1;
    uint8_t b = *p++;
    unsigned what;

    if (b == 0x8F && !d->rex && p + 1 < end && (p[0] & 0x1F) >= 8) {
        // AMD XOP (8F with a map number of 8 or more; below that it is POP r/m): like a three-byte VEX, maps 8
        // (imm8), 9 (none) and 0A (imm32). Only AMD processors run these; code reaches them after a CPUID check.
        uint8_t v1 = *p++, v2 = *p++;
        int map = v1 & 0x1F;
        if (map > 10) return 0;
        d->map = X86_MAP_3DNOW + (uint8_t)(map - 7);        // (XOP maps after the others: 8 -> 5, 9 -> 6, 10 -> 7)
        d->vex_w = v2 >> 7; d->vex_vvvv = (~v2 >> 3) & 15; d->vex_l = (v2 >> 2) & 1;
        d->prefixes |= X86_P_VEX;
        if (p >= end) return 0;
        d->opcode = *p++;
        int n = modrm(p, end, d, !(v1 >> 7), !((v1 >> 6) & 1), !((v1 >> 5) & 1));
        if (n < 0) return 0;
        p += n;
        d->imm_size = map == 8 ? 1 : map == 10 ? 4 : 0;
        d->opsize = 4;
    } else if ((b == 0xC4 || b == 0xC5 || b == 0x62) && !d->rex) {
        // VEX (C4: 3 bytes, C5: 2 bytes) and EVEX (62: 4 bytes); in 64-bit mode these bytes are always prefixes.
        int map, r, x, bb, w = 0, vvvv, l, pp;
        if (b == 0xC5) {
            if (p >= end) return 0;
            uint8_t v1 = *p++;
            r = !(v1 >> 7); x = 0; bb = 0; map = X86_MAP_0F;
            vvvv = (~v1 >> 3) & 15; l = (v1 >> 2) & 1; pp = v1 & 3;
            d->prefixes |= X86_P_VEX;
        } else if (b == 0xC4) {
            if (p + 1 >= end) return 0;
            uint8_t v1 = *p++, v2 = *p++;
            r = !(v1 >> 7); x = !((v1 >> 6) & 1); bb = !((v1 >> 5) & 1);
            int mm = v1 & 31;
            if (mm < 1 || mm > 3) return 0;
            map = mm;                                    // 1: 0F, 2: 0F38, 3: 0F3A (enum values match)
            w = v2 >> 7; vvvv = (~v2 >> 3) & 15; l = (v2 >> 2) & 1; pp = v2 & 3;
            d->prefixes |= X86_P_VEX;
        } else {
            if (p + 2 >= end) return 0;
            uint8_t e1 = *p++, e2 = *p++, e3 = *p++;
            r = !(e1 >> 7); x = !((e1 >> 6) & 1); bb = !((e1 >> 5) & 1);
            int mm = e1 & 7;
            if (mm < 1 || mm > 3) return 0;
            map = mm;
            w = e2 >> 7; vvvv = ((~e2 >> 3) & 15) | ((~e3 >> 3 & 1) << 4); pp = e2 & 3; l = (e3 >> 5) & 3;
            d->prefixes |= X86_P_EVEX;
        }
        d->map = (uint8_t)map; d->vex_w = (uint8_t)w; d->vex_vvvv = (uint8_t)vvvv; d->vex_l = (uint8_t)l;
        static const uint8_t ppmap[4] = { 0, 0x66, 0xF3, 0xF2 };
        d->mandatory = ppmap[pp];
        if (p >= end) return 0;
        d->opcode = *p++;
        if (!(map == X86_MAP_0F && d->opcode == 0x77)) {     // (VZEROUPPER / VZEROALL: no ModRM)
            int n = modrm(p, end, d, r, x, bb);
            if (n < 0) return 0;
            p += n;
        }
        d->imm_size = (uint8_t)(vex_imm8(map, d->opcode) ? 1 : 0);
        d->opsize = 4;
    } else {
        d->map = X86_MAP_1;
        if (b == 0x0F) {
            if (p >= end) return 0;
            uint8_t b2 = *p++;
            if (b2 == 0x38 || b2 == 0x3A) {
                if (p >= end) return 0;
                d->map = b2 == 0x38 ? X86_MAP_0F38 : X86_MAP_0F3A;
                d->opcode = *p++;
                what = M | (b2 == 0x3A ? B : 0);
            } else if (b2 == 0x0F) {
                d->map = X86_MAP_3DNOW;                 // 0F 0F ModRM imm8 (the imm8 is the operation)
                what = M | B;
            } else {
                d->map = X86_MAP_0F;
                d->opcode = b2;
                what = two[b2];
            }
        } else {
            d->opcode = b;
            what = one[b];
        }
        if (what & X) return 0;
        d->opsize = rex_w ? 8 : (d->prefixes & X86_P_OPSIZE) ? 2 : 4;
        if (d->map == X86_MAP_1 && ((b & 0xFE) == 0xF6 || b < 0x40 || (b >= 0x80 && b <= 0x8B) || b == 0xFE) && !(b & 1) && b != 0x63)
            d->opsize = 1;                               // (the byte forms: even opcodes in these rows)
        if (what & M) {
            int n = modrm(p, end, d, rex_r, rex_x, rex_b);
            if (n < 0) return 0;
            p += n;
        }
        int imm = 0;
        if (what & B) imm += 1;
        if (what & W) imm += 2;
        if (what & Z) imm += (d->prefixes & X86_P_OPSIZE) && !rex_w ? 2 : 4;   // (REX.W wins over 66)
        if (what & V) imm += rex_w ? 8 : (d->prefixes & X86_P_OPSIZE) ? 2 : 4;
        if (what & A) imm += (d->prefixes & X86_P_ADDRSIZE) ? 4 : 8;
        if (what & E) imm += 3;
        if (d->map == X86_MAP_1) {
            // TEST r/m, imm only for /0 and /1 of F6/F7; the rest of the group has no immediate.
            if (b == 0xF6 && d->reg % 8 < 2) imm += 1;
            if (b == 0xF7 && d->reg % 8 < 2) imm += (d->prefixes & X86_P_OPSIZE) && !rex_w ? 2 : 4;
            // Near branches and calls: rel32 whatever the operand-size prefix (64-bit mode, as Intel does).
            if (b == 0xE8 || b == 0xE9) imm = 4;
            // XBEGIN (C7 F8): rel16 with 66, else rel32 (the table's Z does that).
        } else if (d->map == X86_MAP_0F && d->opcode >= 0x80 && d->opcode <= 0x8F) {
            imm = 4;                                     // Jcc rel32
        }
        d->imm_size = (uint8_t)imm;
    }
    if (p + d->imm_size > end) return 0;
    int first = d->imm_size;
    if (d->map == X86_MAP_1 && (b == 0xC8)) first = 2;   // ENTER iw, ib
    if (first == 1 || first == 2 || first == 4 || first == 8) d->imm = read_imm(p, first);
    p += d->imm_size;
    d->len = (uint8_t)(p - code);
    return d->len;
}
