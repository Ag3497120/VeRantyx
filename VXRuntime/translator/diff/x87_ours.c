// x87_exact.py's side for VXRuntime: the same records as x87_ref.c, through the translator's x87 (x87.c), natively.
#include "x87.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef struct { uint64_t m; uint16_t e; } __attribute__((packed)) f80;
typedef struct { uint32_t op; f80 x, y; } __attribute__((packed)) rec;
typedef struct { f80 r0, r1; uint16_t sw; } __attribute__((packed)) out;
static x86_ctx c;
static void fld(f80 v) { int t = ((c.fsw >> 11) - 1) & 7; c.fsw = (uint16_t)((c.fsw & ~0x3800) | t << 11); memcpy(c.st[t], &v, 10); c.ftag |= (uint8_t)(1 << t); }
static f80 fstp(void) { int t = c.fsw >> 11 & 7; f80 v; memcpy(&v, c.st[t], 10); c.ftag &= (uint8_t)~(1 << t); c.fsw = (uint16_t)((c.fsw & ~0x3800) | ((t + 1) & 7) << 11); return v; }
int main(void) { rec r; out o;
    static const int mod[8] = { 0xFE, 0xFF, 0xF2, 0xF3, 0xF0, 0xF1, 0xF9, 0xFB };
    while (fread(&r, sizeof r, 1, stdin) == 1) {
        memset(&c, 0, sizeof c); c.fcw = (uint16_t)(0x037F | (getenv("RC") ? atoi(getenv("RC")) : 0) << 10);
        if (r.op == 3 || r.op == 5 || r.op == 6) { fld(r.y); fld(r.x); } else fld(r.x);
        xlat_x87(&c, 0xD9, (uint32_t)mod[r.op], 0);
        o.sw = c.fsw; memset(&o.r1, 0, sizeof o.r1);
        if (r.op == 2 || r.op == 7) { o.r1 = fstp(); o.r0 = fstp(); } else o.r0 = fstp();
        fwrite(&o, sizeof o, 1, stdout);
    }
}
