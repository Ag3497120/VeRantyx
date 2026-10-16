// The differential test (TRANSLATOR.md, phase 2): random instructions of every form the translator handles, each run
// with random registers, flags and memory twice: for real, by ref_x86 under Rosetta 2, and translated by xlat.c and run
// natively. Registers, the defined flags, memory and faults must match.
//
//   build/difftest build/ref_x86 [cases per form] [seed]
#include "../x86dec.h"
#include "../xlat.h"
#include "difftest.h"
#include "../engine.h"
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern char **environ;
static uint64_t rng_state;
static uint64_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17; return rng_state; }
static uint64_t rnd_value(void) {                   // edge values half the time
    static const uint64_t edge[] = { 0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000,
                                     0xFFFFFFFF, 0x100000000ull, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, ~0ull, ~0ull - 1 };
    uint64_t r = rnd();
    if (r & 1) return edge[(r >> 1) % (sizeof edge / sizeof edge[0])];
    switch ((r >> 1) & 3) { case 0: return rnd() & 0xFF; case 1: return rnd() & 0xFFFF; case 2: return rnd() & 0xFFFFFFFF; }
    return rnd();
}

// Floating-point lanes with the values that differ between x86 and ARM: zeros, infinities, quiet and signalling
// NaNs (both signs, payloads), denormals, values at the integer conversion limits.
static uint32_t rnd_f32(void) {
    static const uint32_t edge[] = { 0, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000,
                                     0x7FA00001, 0xFF812345, 0x7FC12345, 0x00000001, 0x807FFFFF, 0x00400000, 0x4F000000, 0xCF000000,
                                     0x4EFFFFFF, 0xCF000001, 0x5F000000, 0x3F000000, 0x3FC00000, 0x40200000, 0x7F7FFFFF, 0x00800000 };
    uint64_t r = rnd();
    if (r & 1) return edge[(r >> 1) % (sizeof edge / sizeof edge[0])];
    if (r & 2) { float f = (float)((int64_t)(rnd() % 2000000) - 1000000) / (float)(1 + rnd() % 1000); uint32_t u; memcpy(&u, &f, 4); return u; }
    return (uint32_t)rnd();
}
static uint64_t rnd_f64(void) {
    static const uint64_t edge[] = { 0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x7FF0000000000000ull,
                                     0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000000ull, 0x7FF4000000000001ull,
                                     0xFFF0000000012345ull, 0x7FF8000000012345ull, 1, 0x800FFFFFFFFFFFFFull, 0x41E0000000000000ull,
                                     0xC1E0000000000000ull, 0x41DFFFFFFFC00000ull, 0x41DFFFFFFFE00000ull, 0xC1E0000000200000ull,
                                     0x43E0000000000000ull, 0xC3E0000000000000ull, 0x3FE0000000000000ull, 0x7FEFFFFFFFFFFFFFull };
    uint64_t r = rnd();
    if (r & 1) return edge[(r >> 1) % (sizeof edge / sizeof edge[0])];
    if (r & 2) { double f = (double)((int64_t)(rnd() % 2000000000) - 1000000000) / (double)(1 + rnd() % 100000); uint64_t u; memcpy(&u, &f, 8); return u; }
    return rnd();
}
static void rnd_xmm(uint8_t *x) {                     // 16 bytes: floats, doubles or bits
    int k = (int)(rnd() % 3);
    for (int i = 0; i < 16; i += k == 1 ? 8 : 4) {
        if (k == 0) { uint32_t v = rnd_f32(); memcpy(x + i, &v, 4); }
        else if (k == 1) { uint64_t v = rnd_f64(); memcpy(x + i, &v, 8); }
        else { uint32_t v = (uint32_t)rnd(); memcpy(x + i, &v, 4); }
    }
}

// ---- the forms ------------------------------------------------------------------------------------------------------------------
typedef struct { const char *name; int map; uint8_t op; int group; int imm; int no_mem; int no_byte_ops; int special; int prefix; } form;
enum { SP_MOFFS = 1, SP_XLAT, SP_LEAVE, SP_STRING, SP_LOCK };   // special: how the case is set up; prefix: F3 / F2
// imm: 0 none, 1 imm8, 2 imm16/32 (operand size), 4 imm for MOV r, imm (64 with REX.W); group: ModRM.reg fixed (-1: any)
static const form forms[] = {
#define ALU(nm, base) { nm " r/m8, r8", 0, base, -1, 0, 0, 0 }, { nm " r/m, r", 0, base + 1, -1, 0, 0, 0 }, \
                      { nm " r8, r/m8", 0, base + 2, -1, 0, 0, 0 }, { nm " r, r/m", 0, base + 3, -1, 0, 0, 0 }, \
                      { nm " al, imm8", 0, base + 4, -2, 1, 0, 0 }, { nm " eax, imm", 0, base + 5, -2, 2, 0, 0 }
    ALU("add", 0x00), ALU("or", 0x08), ALU("adc", 0x10), ALU("sbb", 0x18), ALU("and", 0x20), ALU("sub", 0x28), ALU("xor", 0x30), ALU("cmp", 0x38),
#undef ALU
    { "grp1 r/m8, imm8", 0, 0x80, -1, 1, 0, 0 }, { "grp1 r/m, imm", 0, 0x81, -1, 2, 0, 0 }, { "grp1 r/m, imm8", 0, 0x83, -1, 1, 0, 0 },
    { "test r/m8, r8", 0, 0x84, -1, 0, 0, 0 }, { "test r/m, r", 0, 0x85, -1, 0, 0, 0 }, { "test al, imm8", 0, 0xA8, -2, 1, 0, 0 }, { "test eax, imm", 0, 0xA9, -2, 2, 0, 0 },
    { "xchg r/m8, r8", 0, 0x86, -1, 0, 0, 0 }, { "xchg r/m, r", 0, 0x87, -1, 0, 0, 0 },
    { "mov r/m8, r8", 0, 0x88, -1, 0, 0, 0 }, { "mov r/m, r", 0, 0x89, -1, 0, 0, 0 }, { "mov r8, r/m8", 0, 0x8A, -1, 0, 0, 0 }, { "mov r, r/m", 0, 0x8B, -1, 0, 0, 0 },
    { "lea", 0, 0x8D, -1, 0, -1, 0 },
    { "mov r/m8, imm8", 0, 0xC6, 0, 1, 0, 0 }, { "mov r/m, imm", 0, 0xC7, 0, 2, 0, 0 },
    { "mov r8, imm8", 0, 0xB0, -3, 1, 0, 0 }, { "mov r, imm", 0, 0xB8, -3, 4, 0, 0 },
    { "movsxd", 0, 0x63, -1, 0, 0, 0 }, { "imul r, r/m, imm", 0, 0x69, -1, 2, 0, 0 }, { "imul r, r/m, imm8", 0, 0x6B, -1, 1, 0, 0 },
    { "xchg rax, r", 0, 0x90, -3, 0, 0, 0 }, { "cbw/cwde/cdqe", 0, 0x98, -2, 0, 0, 0 }, { "cwd/cdq/cqo", 0, 0x99, -2, 0, 0, 0 },
    { "test r/m8, imm8", 0, 0xF6, 0, 1, 0, 0 }, { "not r/m8", 0, 0xF6, 2, 0, 0, 0 }, { "neg r/m8", 0, 0xF6, 3, 0, 0, 0 },
    { "mul r/m8", 0, 0xF6, 4, 0, 0, 0 }, { "imul r/m8", 0, 0xF6, 5, 0, 0, 0 }, { "div r/m8", 0, 0xF6, 6, 0, 0, 0 }, { "idiv r/m8", 0, 0xF6, 7, 0, 0, 0 },
    { "test r/m, imm", 0, 0xF7, 0, 2, 0, 0 }, { "not r/m", 0, 0xF7, 2, 0, 0, 0 }, { "neg r/m", 0, 0xF7, 3, 0, 0, 0 },
    { "mul r/m", 0, 0xF7, 4, 0, 0, 0 }, { "imul r/m", 0, 0xF7, 5, 0, 0, 0 }, { "div r/m", 0, 0xF7, 6, 0, 0, 0 }, { "idiv r/m", 0, 0xF7, 7, 0, 0, 0 },
    { "inc r/m8", 0, 0xFE, 0, 0, 0, 0 }, { "dec r/m8", 0, 0xFE, 1, 0, 0, 0 }, { "inc r/m", 0, 0xFF, 0, 0, 0, 0 }, { "dec r/m", 0, 0xFF, 1, 0, 0, 0 },
#define SH(g, nm) { nm " r/m8, imm8", 0, 0xC0, g, 1, 0, 0 }, { nm " r/m, imm8", 0, 0xC1, g, 1, 0, 0 }, { nm " r/m8, 1", 0, 0xD0, g, 0, 0, 0 }, \
                  { nm " r/m, 1", 0, 0xD1, g, 0, 0, 0 }, { nm " r/m8, cl", 0, 0xD2, g, 0, 0, 0 }, { nm " r/m, cl", 0, 0xD3, g, 0, 0, 0 }
    SH(0, "rol"), SH(1, "ror"), SH(4, "shl"), SH(5, "shr"), SH(7, "sar"),
#undef SH
    { "push r", 0, 0x50, -3, 0, 0, 0 }, { "pop r", 0, 0x58, -3, 0, 0, 0 }, { "push imm", 0, 0x68, -2, 2, 0, 0 }, { "push imm8", 0, 0x6A, -2, 1, 0, 0 },
    { "clc", 0, 0xF8, -2, 0, 0, 0 }, { "stc", 0, 0xF9, -2, 0, 0, 0 }, { "cmc", 0, 0xF5, -2, 0, 0, 0 },
    { "pushfq", 0, 0x9C, -2, 0, 0, 0 }, { "popfq", 0, 0x9D, -2, 0, 0, 0 }, { "movnti", 1, 0xC3, -1, 0, -1, 1 },
    { "lahf", 0, 0x9F, -2, 0, 0, 0 }, { "sahf", 0, 0x9E, -2, 0, 0, 0 },
    { "cmovcc", 1, 0x40, -4, 0, 0, 0 }, { "setcc", 1, 0x90, -5, 0, 0, 0 },
    { "movzx r, r/m8", 1, 0xB6, -1, 0, 0, 0 }, { "movzx r, r/m16", 1, 0xB7, -1, 0, 0, 0 },
    { "movsx r, r/m8", 1, 0xBE, -1, 0, 0, 0 }, { "movsx r, r/m16", 1, 0xBF, -1, 0, 0, 0 },
    { "imul r, r/m", 1, 0xAF, -1, 0, 0, 0 }, { "bswap", 1, 0xC8, -3, 0, 0, 1 },
    { "bsf", 1, 0xBC, -1, 0, 0, 0 }, { "bsr", 1, 0xBD, -1, 0, 0, 0 },
    { "tzcnt", 1, 0xBC, -6, 0, 0, 0 }, { "lzcnt", 1, 0xBD, -6, 0, 0, 0 }, { "popcnt", 1, 0xB8, -6, 0, 0, 0 },
    { "bt r/m, r", 1, 0xA3, -1, 0, 0, 0 }, { "bts r/m, r", 1, 0xAB, -1, 0, 0, 0 }, { "btr r/m, r", 1, 0xB3, -1, 0, 0, 0 }, { "btc r/m, r", 1, 0xBB, -1, 0, 0, 0 },
    { "bt r/m, imm8", 1, 0xBA, 4, 1, 0, 0 }, { "bts r/m, imm8", 1, 0xBA, 5, 1, 0, 0 }, { "btr r/m, imm8", 1, 0xBA, 6, 1, 0, 0 }, { "btc r/m, imm8", 1, 0xBA, 7, 1, 0, 0 },
    { "xadd r/m8, r8", 1, 0xC0, -1, 0, 0, 0 }, { "xadd r/m, r", 1, 0xC1, -1, 0, 0, 0 },
    { "cmpxchg r/m8, r8", 1, 0xB0, -1, 0, 0, 0 }, { "cmpxchg r/m, r", 1, 0xB1, -1, 0, 0, 0 },
    // phase 2b
#define SH(g, nm) { nm " r/m8, imm8", 0, 0xC0, g, 1, 0, 0 }, { nm " r/m, imm8", 0, 0xC1, g, 1, 0, 0 }, { nm " r/m8, 1", 0, 0xD0, g, 0, 0, 0 }, \
                  { nm " r/m, 1", 0, 0xD1, g, 0, 0, 0 }, { nm " r/m8, cl", 0, 0xD2, g, 0, 0, 0 }, { nm " r/m, cl", 0, 0xD3, g, 0, 0, 0 }
    SH(2, "rcl"), SH(3, "rcr"),
#undef SH
    { "shld r/m, r, imm8", 1, 0xA4, -1, 1, 0, 1 }, { "shld r/m, r, cl", 1, 0xA5, -1, 0, 0, 1 },
    { "shrd r/m, r, imm8", 1, 0xAC, -1, 1, 0, 1 }, { "shrd r/m, r, cl", 1, 0xAD, -1, 0, 0, 1 },
    { "push r/m", 0, 0xFF, 6, 0, 0, 1 }, { "pop r/m", 0, 0x8F, 0, 0, 0, 1 },
    { "leave", 0, 0xC9, -2, 0, 0, 1, SP_LEAVE },
    { "mov al, moffs", 0, 0xA0, -2, 0, 0, 0, SP_MOFFS }, { "mov rax, moffs", 0, 0xA1, -2, 0, 0, 0, SP_MOFFS },
    { "mov moffs, al", 0, 0xA2, -2, 0, 0, 0, SP_MOFFS }, { "mov moffs, rax", 0, 0xA3, -2, 0, 0, 0, SP_MOFFS },
    { "xlat", 0, 0xD7, -2, 0, 0, 0, SP_XLAT },
#define STR(nm, o, pfx) { nm, 0, o, -2, 0, 0, 0, SP_STRING, pfx }
    STR("movsb", 0xA4, 0), STR("movs", 0xA5, 0), STR("rep movsb", 0xA4, 0xF3), STR("rep movs", 0xA5, 0xF3),
    STR("cmpsb", 0xA6, 0), STR("cmps", 0xA7, 0), STR("repe cmpsb", 0xA6, 0xF3), STR("repe cmps", 0xA7, 0xF3),
    STR("repne cmpsb", 0xA6, 0xF2), STR("repne cmps", 0xA7, 0xF2),
    STR("stosb", 0xAA, 0), STR("stos", 0xAB, 0), STR("rep stosb", 0xAA, 0xF3), STR("rep stos", 0xAB, 0xF3),
    STR("lodsb", 0xAC, 0), STR("lods", 0xAD, 0), STR("rep lodsb", 0xAC, 0xF3), STR("rep lods", 0xAD, 0xF3),
    STR("scasb", 0xAE, 0), STR("scas", 0xAF, 0), STR("repe scasb", 0xAE, 0xF3), STR("repe scas", 0xAF, 0xF3),
    STR("repne scasb", 0xAE, 0xF2), STR("repne scas", 0xAF, 0xF2),
#undef STR
#define LK(nm, map, o, g, imm, byteonly) { "lock " nm, map, o, g, imm, 0, 0, SP_LOCK }
    LK("add m, r", 0, 0x01, -1, 0, 0), LK("or m, r", 0, 0x09, -1, 0, 0), LK("adc m, r", 0, 0x11, -1, 0, 0), LK("sbb m, r", 0, 0x19, -1, 0, 0),
    LK("and m, r", 0, 0x21, -1, 0, 0), LK("sub m, r", 0, 0x29, -1, 0, 0), LK("xor m, r", 0, 0x31, -1, 0, 0), LK("add m8, r8", 0, 0x00, -1, 0, 0),
    LK("add m, imm", 0, 0x81, 0, 2, 0), LK("sub m, imm8", 0, 0x83, 5, 1, 0), LK("and m, imm8", 0, 0x83, 4, 1, 0),
    LK("not m", 0, 0xF7, 2, 0, 0), LK("neg m", 0, 0xF7, 3, 0, 0), LK("inc m", 0, 0xFF, 0, 0, 0), LK("dec m", 0, 0xFF, 1, 0, 0),
    LK("bts m, r", 1, 0xAB, -1, 0, 0), LK("btr m, r", 1, 0xB3, -1, 0, 0), LK("btc m, r", 1, 0xBB, -1, 0, 0), LK("bts m, imm8", 1, 0xBA, 5, 1, 0),
    LK("xadd m, r", 1, 0xC1, -1, 0, 0), LK("cmpxchg m, r", 1, 0xB1, -1, 0, 0), LK("cmpxchg m8, r8", 1, 0xB0, -1, 0, 0),
#undef LK
};
// group: -1 any ModRM, -2 no ModRM, -3 register in the opcode, -4 cc in the opcode (ModRM), -5 cc (ModRM, r/m8),
// -6 F3 prefix (ModRM)

static int is_byte_op(const form *f) {
    if (f->map == 0) {
        uint8_t o = f->op;
        if (o < 0x40) return !(o & 1);
        if (f->special == SP_STRING) return !(o & 1);
        return o == 0x80 || o == 0x84 || o == 0x86 || o == 0x88 || o == 0x8A || o == 0xC6 || o == 0xB0 || o == 0xA8 ||
               o == 0xF6 || o == 0xFE || o == 0xC0 || o == 0xD0 || o == 0xD2 || o == 0xA0 || o == 0xA2 || o == 0xD7;
    }
    return f->op == 0xC0 || f->op == 0xB0 || f->group == -5;
}

static uint64_t last_addr;                           // the memory operand's address of the last case (0: none)
// Builds one instance of a form into c (bytes and a state whose memory operand points into the test memory).
static void make_case(const form *f, dt_case *c, int *size_out) {
    memset(c, 0, sizeof *c);
    last_addr = 0;
    for (int i = 0; i < 16; i++) { rnd_xmm(c->in.xmm[i]); rnd_xmm(c->in.ymm_hi[i]); }
    c->in.mxcsr = 0x1F80;
    for (int i = 0; i < 16; i++) c->in.gpr[i] = rnd_value();
    for (int i = 0; i < DT_BUF_SIZE; i++) c->mem[i] = (uint8_t)rnd();
    c->in.gpr[4] = DT_STACK;
    c->in.flags = (rnd() & 0x8D5) | 0x202;
    uint8_t *p = c->code;
    int byteop = is_byte_op(f);
    int sizesel = (int)(rnd() % 3);                         // 0: 32-bit, 1: 16-bit (66), 2: 64-bit (REX.W)
    if (byteop || f->no_byte_ops) sizesel = sizesel == 1 ? 0 : sizesel;
    if (f->op == 0x68 || f->op == 0x6A || f->op == 0x50 || f->op == 0x58 || (!f->map && (f->op == 0x9C || f->op == 0x9D))) sizesel = 0;   // (64-bit stack operations)
    if (f->special == SP_LOCK) *p++ = 0xF0;
    if (f->prefix) *p++ = (uint8_t)f->prefix;
    if (f->op == 0xFF && f->group == 6) sizesel = 0;                 // (PUSH/POP r/m: 64-bit)
    if (f->op == 0x8F || f->special == SP_LEAVE || f->special == SP_XLAT) sizesel = 0;
    if (f->group == -6) *p++ = 0xF3;
    if (sizesel == 1) *p++ = 0x66;
    int rex = 0x40 | (sizesel == 2 ? 8 : 0);
    int reg = (int)(rnd() % 16), rm = (int)(rnd() % 16);
    while (reg == 4) reg = (int)(rnd() % 16);
    while (rm == 4) rm = (int)(rnd() % 16);
    int use_rex = (rex != 0x40) || (rnd() & 1);              // (without REX, byte registers 4-7 are AH CH DH BH)
    int has_modrm = f->group >= -1 || f->group == -4 || f->group == -5 || f->group == -6;
    int mem = has_modrm && !f->no_mem && (f->no_mem == -1 || f->special == SP_LOCK || (rnd() % 3 == 0));
    if (f->no_mem == -1) mem = 1;                            // (LEA needs a memory operand)
    int base = 0, index = -1, mod = 3, scale = 0;
    if (mem) {
        do base = (int)(rnd() % 16); while (base == 4 || base == 5 || base == 12 || base == 13 || base == reg);
        index = (rnd() & 1) ? (int)(rnd() % 16) : -1;
        if (index == 4 || index == base || index == reg) index = -1;
        scale = (int)(rnd() % 4);
        mod = (int)(rnd() % 3);
        rm = base;
    }
    if (!use_rex) { reg &= 7; rm &= 7; if (reg == 4) reg = 6; if (rm == 4) rm = 7; if (mem) { base &= 7; if (base == 4 || base == 5) base = 3; rm = base; if (index >= 0) index &= 7; if (index == 4 || index == base || index == reg) index = -1; } }
    if (f->group == -3) { rm = (int)(rnd() % 16); if (rm == 4) rm = 1; if (!use_rex) rm &= 7; if (rm == 4) rm = 0; }
    int rexb = (f->group == -3) ? (rm >> 3) : (mem ? (base >> 3) : (rm >> 3));
    if (use_rex) { rex |= (reg >> 3) << 2 | (index >= 0 ? (index >> 3) << 1 : 0) | rexb; *p++ = (uint8_t)rex; }
    if (f->map == 1) *p++ = 0x0F;
    uint8_t op = f->op;
    if (f->group == -3) op = (uint8_t)(op | (rm & 7));
    if (f->group == -4 || f->group == -5) op = (uint8_t)(op | (rnd() % 16));
    *p++ = op;
    if (has_modrm) {
        int r = f->group >= 0 ? f->group : reg;
        if (!mem) *p++ = (uint8_t)(0xC0 | (r & 7) << 3 | (rm & 7));
        else {
            int disp = mod == 1 ? (int)(rnd() % 64) - 32 : mod == 2 ? (int)(rnd() % 512) - 256 : 0;
            if (index >= 0) { *p++ = (uint8_t)(mod << 6 | (r & 7) << 3 | 4); *p++ = (uint8_t)(scale << 6 | (index & 7) << 3 | (base & 7)); }
            else *p++ = (uint8_t)(mod << 6 | (r & 7) << 3 | (base & 7));
            if (mod == 1) *p++ = (uint8_t)disp;
            if (mod == 2) { memcpy(p, &disp, 4); p += 4; }
            // The address lands in the middle of the test memory, below the stack.
            int64_t idx_v = index >= 0 ? (int64_t)(rnd() % 32) : 0;
            last_addr = DT_BUF + 1024 + (rnd() % 512);
            c->in.gpr[base] = last_addr - (uint64_t)((int64_t)disp + (idx_v << scale));
            if (index >= 0) c->in.gpr[index] = (uint64_t)idx_v;
            // BT with a register bit offset reaches offset/8 bytes away: keep it small.
            if (f->map == 1 && (f->op == 0xA3 || f->op == 0xAB || f->op == 0xB3 || f->op == 0xBB)) c->in.gpr[reg] = (rnd() % 2048) - 1024;
        }
    }
    int size = byteop ? 1 : sizesel == 1 ? 2 : sizesel == 2 ? 8 : 4;
    if (f->op == 0x68 || f->op == 0x6A || f->op == 0x50 || f->op == 0x58) size = 8;
    int shld16 = f->map == 1 && (f->op == 0xA4 || f->op == 0xA5 || f->op == 0xAC || f->op == 0xAD) && size == 2;
    if (shld16 && (f->op == 0xA5 || f->op == 0xAD)) c->in.gpr[1] = (c->in.gpr[1] & ~0xFFull) | (rnd() % 17);   // (16-bit: count <= 16)
    if (shld16 && f->imm == 1) *p++ = (uint8_t)(rnd() % 17);
    else if (f->imm == 1) *p++ = (uint8_t)rnd_value();
    else if (f->imm == 2) { int n = size == 2 ? 2 : 4; uint32_t v = (uint32_t)rnd_value(); memcpy(p, &v, (size_t)n); p += n; }
    else if (f->imm == 4) { int n = size == 2 ? 2 : size == 8 ? 8 : 4; uint64_t v = rnd_value(); memcpy(p, &v, (size_t)n); p += n; }
    // Division: half the time a divisor and dividend that do not fault.
    if (f->group >= 6 && (f->op == 0xF6 || f->op == 0xF7) && (rnd() & 1)) {
        c->in.gpr[2] = 0;
        if (!mem) c->in.gpr[rm] |= 1; else c->mem[1024] |= 1;
    }
    if (!f->map && f->op == 0x9D) {                                          // (POPFQ: no TF, or the next instruction traps)
        uint64_t v; memcpy(&v, c->mem + (DT_STACK - DT_BUF), 8); v &= ~0x100ull; memcpy(c->mem + (DT_STACK - DT_BUF), &v, 8);
    }
    if (f->special == SP_MOFFS) { uint64_t a = DT_BUF + 1024 + rnd() % 1024; memcpy(p, &a, 8); p += 8; }
    if (f->special == SP_XLAT) c->in.gpr[3] = DT_BUF + 1024 + rnd() % 1024;
    if (f->special == SP_LEAVE) c->in.gpr[5] = DT_STACK - 8 * (rnd() % 32);
    if (f->special == SP_STRING) {
        c->in.gpr[6] = DT_BUF + 512 + rnd() % 256; c->in.gpr[7] = DT_BUF + 1536 + rnd() % 256;
        if (f->prefix) c->in.gpr[1] = rnd() % 20;
        if (rnd() & 1) c->in.flags |= 0x400;                               // DF
        if (f->op == 0xA6 || f->op == 0xA7) memcpy(c->mem + 1536, c->mem + 512, 512);   // (equal runs for REPE)
        if ((f->op == 0xAE || f->op == 0xAF) && (rnd() & 1)) c->in.gpr[0] = c->mem[1600];
    }
    if (f->op == 0xA0 || f->op == 0xA1 || f->op == 0xA2 || f->op == 0xA3) size = byteop ? 1 : size;
    c->len = (uint32_t)(p - c->code);
    *size_out = size;
}

// EFLAGS bits an instruction leaves undefined (they are not compared), from the instruction and its inputs.
static uint64_t undefined_flags(const x86_insn *d, const dt_state *in, int size) {
    const uint64_t CF = 1, PF = 4, AF = 16, ZF = 64, SF = 128, OF = 2048;
    int op = d->opcode, g = d->reg & 7;
    if (d->map == X86_MAP_1) {
        if ((op < 0x40 && ((op >> 3) == 1 || (op >> 3) == 4 || (op >> 3) == 6)) || ((op >= 0x80 && op <= 0x83) && (g == 1 || g == 4 || g == 6)) ||
            op == 0x84 || op == 0x85 || op == 0xA8 || op == 0xA9 || ((op == 0xF6 || op == 0xF7) && g == 0)) return AF;
        if ((op == 0xF6 || op == 0xF7) && (g == 4 || g == 5)) return SF | ZF | AF | PF;
        if ((op == 0xF6 || op == 0xF7) && (g == 6 || g == 7)) return CF | OF | SF | ZF | AF | PF;
        if (op == 0x69 || op == 0x6B) return SF | ZF | AF | PF;
        if ((op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) && (g == 2 || g == 3)) {   // RCL RCR: OF for 1 only
            int count = (op >= 0xD2 ? (int)(in->gpr[1] & 0xFF) : op >= 0xD0 ? 1 : (int)(uint8_t)d->imm) & (size == 8 ? 63 : 31);
            return count == 1 ? 0 : OF;
        }
        if (op == 0xC0 || op == 0xC1 || (op >= 0xD0 && op <= 0xD3)) {
            int count = op >= 0xD2 ? (int)(in->gpr[1] & 0xFF) : op >= 0xD0 ? 1 : (int)(uint8_t)d->imm;
            count &= size == 8 ? 63 : 31;
            uint64_t u = 0;
            if (count != 1) u |= OF;
            if (g >= 4) u |= AF;
            if ((g == 4 || g == 5 || g == 6) && count >= size * 8) u |= CF;
            return count ? u : 0;
        }
    } else if (d->map == X86_MAP_0F) {
        if (op == 0xAF) return SF | ZF | AF | PF;
        if (op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) {
            int count = (op == 0xA5 || op == 0xAD ? (int)(in->gpr[1] & 0xFF) : (int)(uint8_t)d->imm) & (size == 8 ? 63 : 31);
            return count == 0 ? 0 : count == 1 ? AF : AF | OF;
        }
        if ((op == 0xBC || op == 0xBD) && d->mandatory != 0xF3) return CF | OF | SF | AF | PF;
        if ((op == 0xBC || op == 0xBD) && d->mandatory == 0xF3) return OF | SF | AF | PF;
        if (op == 0xA3 || op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xBA) return OF | SF | AF | PF;
    }
    return 0;
}

// ---- programs: blocks, branches, calls, loops -------------------------------------------------------------------------------
// Random programs that always end: branches go forward, loops count down from at most 4, the one function does not
// call. Registers used: RAX RCX RDX RSI RDI R8-R11; RBX points at test memory (never written), RSP at the stack.
static uint8_t *pp; static uint8_t *pp0;
static const int pool[] = { 0, 1, 2, 6, 7, 8, 9, 10, 11 };
static int preg(int no_rcx) { int r; do r = pool[rnd() % 9]; while (no_rcx && r == 1); return r; }
static void rex_rr(int w, int reg, int rm, int force) {
    int rex = 0x40 | w << 3 | (reg >> 3) << 2 | (rm >> 3);
    if (rex != 0x40 || force) *pp++ = (uint8_t)rex;
}
static int flags_clean;                              // the flags conditions read are all defined
// Scalar SSE: MOVSS / MOVSD loads, arithmetic on registers and memory, XORPS x, x, register moves, and forms that
// write the whole register (MOVAPS, ADDPS, CVTSS2SD): the engine tracks which registers are zero above lane 0.
static void sse_insn(void) {
    static const int xr[] = { 0, 1, 2, 9 };              // (few registers: loads, arithmetic and moves meet often)
    int x = xr[rnd() % 4], y = xr[rnd() % 4], pf = (rnd() & 1) ? 0xF3 : 0xF2;
    static const uint8_t arith[] = { 0x58, 0x59, 0x5C, 0x5E, 0x51, 0x5D, 0x5F };
    if (rnd() % 3 == 0) {                                // a chain on one register: a load, then arithmetic on it
        *pp++ = (uint8_t)pf; rex_rr(0, x, 3, 0); *pp++ = 0x0F; *pp++ = 0x10; *pp++ = (uint8_t)(0x43 | (x & 7) << 3); *pp++ = (uint8_t)(rnd() % 120);
        for (int m = 1 + (int)(rnd() % 3); m > 0; m--) {
            int p2 = (rnd() % 4) ? pf : pf ^ 0xF3 ^ 0xF2, o = arith[rnd() % 7];
            if (rnd() % 5 == 0) { rex_rr(0, x, y, 0); *pp++ = 0x0F; *pp++ = 0x28; *pp++ = (uint8_t)(0xC0 | (x & 7) << 3 | (y & 7)); }   // (MOVAPS: unknown again)
            *pp++ = (uint8_t)p2;
            if (rnd() & 1) { rex_rr(0, x, y, 0); *pp++ = 0x0F; *pp++ = (uint8_t)o; *pp++ = (uint8_t)(0xC0 | (x & 7) << 3 | (y & 7)); }
            else { rex_rr(0, x, 3, 0); *pp++ = 0x0F; *pp++ = (uint8_t)o; *pp++ = (uint8_t)(0x43 | (x & 7) << 3); *pp++ = (uint8_t)(rnd() % 120); }
        }
        return;
    }
    switch (rnd() % 8) {
    case 0: case 1: *pp++ = (uint8_t)pf; rex_rr(0, x, 3, 0); *pp++ = 0x0F; *pp++ = 0x10; *pp++ = (uint8_t)(0x43 | (x & 7) << 3); *pp++ = (uint8_t)(rnd() % 120); break;
    case 2: case 3: *pp++ = (uint8_t)pf; rex_rr(0, x, y, 0); *pp++ = 0x0F; *pp++ = arith[rnd() % 7]; *pp++ = (uint8_t)(0xC0 | (x & 7) << 3 | (y & 7)); break;
    case 4: *pp++ = (uint8_t)pf; rex_rr(0, x, 3, 0); *pp++ = 0x0F; *pp++ = arith[rnd() % 7]; *pp++ = (uint8_t)(0x43 | (x & 7) << 3); *pp++ = (uint8_t)(rnd() % 120); break;
    case 5: rex_rr(0, x, x, 0); *pp++ = 0x0F; *pp++ = 0x57; *pp++ = (uint8_t)(0xC0 | (x & 7) << 3 | (x & 7)); break;
    case 6: *pp++ = (uint8_t)pf; rex_rr(0, x, y, 0); *pp++ = 0x0F; *pp++ = (rnd() & 1) ? 0x10 : 0x11; *pp++ = (uint8_t)(0xC0 | (x & 7) << 3 | (y & 7)); break;
    case 7: { static const uint8_t whole[] = { 0x28, 0x58, 0x5A }; int o = whole[rnd() % 3];
              if (o == 0x5A) *pp++ = (uint8_t)pf;
              rex_rr(0, x, y, 0); *pp++ = 0x0F; *pp++ = (uint8_t)o; *pp++ = (uint8_t)(0xC0 | (x & 7) << 3 | (y & 7)); break; }
    }
}
static void body_insn(int no_rcx) {
    if (rnd() % 4 == 0) { sse_insn(); return; }
    int k = (int)(rnd() % 13), w = (int)(rnd() & 1), a = preg(no_rcx), c = preg(no_rcx);
    static const uint8_t alu[] = { 0x01, 0x09, 0x11, 0x19, 0x21, 0x29, 0x31, 0x39, 0x85 };
    switch (k) {
    case 0: case 1: rex_rr(w, c, a, 0); *pp++ = alu[rnd() % 9]; *pp++ = (uint8_t)(0xC0 | (c & 7) << 3 | (a & 7)); flags_clean = 1; break;
    case 2: rex_rr(w, 0, a, 0); *pp++ = 0x83; *pp++ = (uint8_t)(0xC0 | (rnd() % 8) << 3 | (a & 7)); *pp++ = (uint8_t)rnd(); flags_clean = 1; break;
    case 3: rex_rr(w, 0, a, 0); *pp++ = (uint8_t)(0xB8 | (a & 7)); { uint64_t v = rnd_value(); memcpy(pp, &v, w ? 8 : 4); pp += w ? 8 : 4; } break;
    case 4: rex_rr(w, a, 3, 0); *pp++ = (rnd() & 1) ? 0x8B : 0x89; *pp++ = (uint8_t)(0x43 | (a & 7) << 3); *pp++ = (uint8_t)(rnd() % 120); break;
    case 5: rex_rr(w, 0, a, 0); *pp++ = 0xC1; *pp++ = (uint8_t)(0xC0 | (int[]){ 0, 1, 4, 5, 7 }[rnd() % 5] << 3 | (a & 7));
            *pp++ = (uint8_t)(1 + rnd() % 30); flags_clean = 0; break;
    case 6: rex_rr(w, a, c, 0); *pp++ = 0x0F; *pp++ = 0xAF; *pp++ = (uint8_t)(0xC0 | (a & 7) << 3 | (c & 7)); flags_clean = 0; break;
    case 7: rex_rr(w, 0, a, 0); { int g = (int)(rnd() % 4); *pp++ = g < 2 ? 0xFF : 0xF7; *pp++ = (uint8_t)(0xC0 | g << 3 | (a & 7)); if (g != 2) flags_clean = 1; } break;
    case 8: if (!flags_clean) break; rex_rr(0, 0, a, 1); *pp++ = 0x0F; *pp++ = (uint8_t)(0x90 | (rnd() % 16)); *pp++ = (uint8_t)(0xC0 | (a & 7)); break;
    case 9: if (!flags_clean) break; rex_rr(w, a, c, 0); *pp++ = 0x0F; *pp++ = (uint8_t)(0x40 | (rnd() % 16)); *pp++ = (uint8_t)(0xC0 | (a & 7) << 3 | (c & 7)); break;
    case 10: { int x = preg(no_rcx);                                 // LEA a, [c + x*s + disp8]
             *pp++ = (uint8_t)(0x40 | w << 3 | (a >> 3) << 2 | (x >> 3) << 1 | (c >> 3)); *pp++ = 0x8D; *pp++ = (uint8_t)(0x44 | (a & 7) << 3);
             *pp++ = (uint8_t)((rnd() % 4) << 6 | (x & 7) << 3 | (c & 7)); *pp++ = (uint8_t)rnd(); break; }
    case 11: rex_rr(0, 0, a, 0); *pp++ = (uint8_t)(0x50 | (a & 7)); if (rnd() & 1) body_insn(no_rcx); rex_rr(0, 0, c, 0); *pp++ = (uint8_t)(0x58 | (c & 7)); break;
    case 12: rex_rr(w, a, c, 0); *pp++ = 0x87; *pp++ = (uint8_t)(0xC0 | (a & 7) << 3 | (c & 7)); break;
    }
}
static void clean_flags(void) {                      // CMP r, r: every flag defined
    if (flags_clean) return;
    int a = preg(0), c = preg(0);
    rex_rr(1, c, a, 0); *pp++ = 0x39; *pp++ = (uint8_t)(0xC0 | (c & 7) << 3 | (a & 7)); flags_clean = 1;
}
// A CMP / TEST / SUB / AND / OR / XOR of 8 to 64 bits (registers, AH-BH, an immediate, RBX-based memory) for a Jcc right
// after it: the engine takes such a Jcc's condition from the operands when nothing else reads the flags.
static uint8_t *g_mem;                               // (the case's memory: RBX + d is g_mem[1024 + d])
static void fusable(void) {
    int a = preg(0), c = preg(0), k = (int)(rnd() % 6), sz = (int)(rnd() % 4);   // (k: CMP TEST SUB AND OR XOR; sz: 8 16 32 64)
    static const uint8_t rr[] = { 0x38, 0x84, 0x28, 0x20, 0x08, 0x30 }, grp[] = { 7, 0, 5, 4, 1, 6 }, acc[] = { 0x3C, 0xA8, 0x2C, 0x24, 0x0C, 0x34 };
    int imm = sz == 0 ? 1 : sz == 1 ? 2 : 4;
    if (sz == 1) *pp++ = 0x66;
    if (rnd() % 6 == 0) {                                            // (U)COMISS/SD: registers or memory (special floats there)
        static const int xr[] = { 0, 1, 2, 9 };
        int x = xr[rnd() % 4], y = xr[rnd() % 4], dbl = (int)(rnd() & 1);
        if (dbl) *pp++ = 0x66;
        if (rnd() & 1) { rex_rr(0, x, y, 0); *pp++ = 0x0F; *pp++ = (uint8_t)(0x2E | (rnd() & 1)); *pp++ = (uint8_t)(0xC0 | (x & 7) << 3 | (y & 7)); }
        else { rex_rr(0, x, 3, 0); *pp++ = 0x0F; *pp++ = (uint8_t)(0x2E | (rnd() & 1)); *pp++ = (uint8_t)(0x43 | (x & 7) << 3); *pp++ = (uint8_t)(rnd() % 120); }
        flags_clean = 1;
        return;
    }
    switch (rnd() % 5) {
    case 4: {                                                        // CMP memory, imm: 0 (the memory zero or not), the
        int op = sz == 0 ? 0x80 : (rnd() & 1) ? 0x83 : 0x81, d = (int)(rnd() % 112), n = op == 0x81 ? imm : 1;   //  value there, any
        int bytes = sz == 0 ? 1 : sz == 1 ? 2 : sz == 2 ? 4 : 8;
        uint8_t *m = g_mem + 1024 + d;
        uint64_t v = 0;
        int how = (int)(rnd() % 3);
        if (how == 0 && (rnd() & 1)) memset(m, 0, (size_t)bytes);
        if (how == 1) {                                              // (equal: an immediate that sign-extends to the value)
            if (n == 1) { int8_t s8 = (int8_t)rnd(); memset(m, s8 < 0 ? 0xFF : 0, (size_t)bytes); m[0] = (uint8_t)s8; v = (uint64_t)(int64_t)s8; }
            else { int32_t s32 = (int32_t)rnd(); memset(m, s32 < 0 ? 0xFF : 0, (size_t)bytes); memcpy(m, &s32, (size_t)(bytes < 4 ? bytes : 4)); v = (uint64_t)(int64_t)s32; }
        } else if (how == 2) v = rnd();
        rex_rr(sz == 3, 0, 3, 0); *pp++ = (uint8_t)op; *pp++ = (uint8_t)(0x40 | 7 << 3 | 3); *pp++ = (uint8_t)d;
        for (int i = 0; i < n; i++) *pp++ = (uint8_t)(v >> (8 * i));
        break; }
    case 0:                                                          // r/m, r
        if (sz == 0 && k < 2 && (rnd() & 1)) { *pp++ = rr[k]; *pp++ = (uint8_t)(0xC0 | (rnd() % 8) << 3 | (rnd() % 8)); break; }   // (AH-BH)
        rex_rr(sz == 3, c, a, sz == 0); *pp++ = (uint8_t)(rr[k] | (sz ? 1 : 0)); *pp++ = (uint8_t)(0xC0 | (c & 7) << 3 | (a & 7)); break;
    case 1: {                                                        // r/m, imm (group 1, F6/F7 TEST)
        int op = k == 1 ? (sz ? 0xF7 : 0xF6) : sz == 0 ? 0x80 : (rnd() & 1) ? 0x83 : 0x81;
        rex_rr(sz == 3, 0, a, sz == 0); *pp++ = (uint8_t)op; *pp++ = (uint8_t)(0xC0 | grp[k] << 3 | (a & 7));
        int n = op == 0x83 || op == 0x80 || op == 0xF6 ? 1 : imm;
        for (int i = 0; i < n; i++) *pp++ = (uint8_t)rnd();
        break; }
    case 2:                                                          // AL / AX / EAX / RAX, imm
        if (sz == 3) *pp++ = 0x48;
        *pp++ = (uint8_t)(acc[k] | (sz ? 1 : 0));
        for (int i = 0; i < imm; i++) *pp++ = (uint8_t)rnd();
        break;
    case 3:                                                          // CMP / TEST with memory (either side for CMP)
        rex_rr(sz == 3, c, 3, sz == 0);
        *pp++ = (uint8_t)((k == 1 ? 0x84 : (rnd() & 1) ? 0x3A : 0x38) | (sz ? 1 : 0));
        *pp++ = (uint8_t)(0x43 | (c & 7) << 3); *pp++ = (uint8_t)(rnd() % 120);
        break;
    }
    flags_clean = 1;
}
// 0-2 instructions without flags between such a CMP and its Jcc: LEA, MOV, MOVZX (fused across), a load (not fused)
static void between(void) {
    for (int m = (int)(rnd() % 3); m > 0; m--) {
        int a = preg(0), c = preg(0), w = (int)(rnd() & 1);
        switch (rnd() % 5) {
        case 0: { int x = preg(0); *pp++ = (uint8_t)(0x40 | w << 3 | (a >> 3) << 2 | (x >> 3) << 1 | (c >> 3)); *pp++ = 0x8D;
                  *pp++ = (uint8_t)(0x44 | (a & 7) << 3); *pp++ = (uint8_t)((rnd() % 4) << 6 | (x & 7) << 3 | (c & 7)); *pp++ = (uint8_t)rnd(); break; }
        case 1: rex_rr(w, c, a, 0); *pp++ = 0x89; *pp++ = (uint8_t)(0xC0 | (c & 7) << 3 | (a & 7)); break;
        case 2: rex_rr(0, 0, a, 0); *pp++ = (uint8_t)(0xB8 | (a & 7)); { uint32_t v = (uint32_t)rnd(); memcpy(pp, &v, 4); pp += 4; } break;
        case 3: rex_rr(w, a, c, 1); *pp++ = 0x0F; *pp++ = (uint8_t)(0xB6 | (rnd() & 1) << 3 | (rnd() & 1)); *pp++ = (uint8_t)(0xC0 | (a & 7) << 3 | (c & 7)); break;
        case 4: rex_rr(w, a, 3, 0); *pp++ = 0x8B; *pp++ = (uint8_t)(0x43 | (a & 7) << 3); *pp++ = (uint8_t)(rnd() % 120); break;
        }
    }
}
typedef struct { uint8_t *at; int target; } fixup;      // a rel32 at `at` to block `target` (-1: the function, -2: the end)
static void make_program(dt_case *c) {
    memset(c, 0, sizeof *c);
    c->kind = 1;
    for (int i = 0; i < 16; i++) { rnd_xmm(c->in.xmm[i]); rnd_xmm(c->in.ymm_hi[i]); }
    c->in.mxcsr = 0x1F80;
    for (int i = 0; i < 16; i++) c->in.gpr[i] = rnd_value();
    for (int i = 0; i < DT_BUF_SIZE; i++) c->mem[i] = (uint8_t)rnd();
    for (int k = 0; k < 24; k++) {                       // (special floats where the loads read: [RBX + 0..127])
        static const uint32_t f32[] = { 0, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00001u, 0x7F800003u, 0x3F800000u, 0x00000005u, 0xFFC00000u };
        static const uint64_t f64[] = { 0, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000001ull, 0x7FF0000000000007ull, 0x3FF0000000000000ull, 5 };
        int at = 1024 + (int)(rnd() % 120);
        if (rnd() & 1) memcpy(c->mem + at, &f32[rnd() % 9], 4); else memcpy(c->mem + at, &f64[rnd() % 8], 8);
    }
    c->in.gpr[4] = DT_STACK; c->in.gpr[3] = DT_BUF + 1024;
    c->in.flags = (rnd() & 0x8D5) | 0x202;
    flags_clean = 1;
    g_mem = c->mem;
    int func_flags = (int)(rnd() & 1);                   // (the function ends with flags set: callers may read them)
    pp = pp0 = c->prog;
    int nb = 2 + (int)(rnd() % 6);
    uint8_t *start[8]; fixup fx[64]; int nfx = 0;
    for (int i = 0; i < nb; i++) {
        start[i] = pp;
        if (rnd() & 1) { int a = preg(0), c = preg(0); rex_rr(1, c, a, 0); *pp++ = 0x29 | (rnd() & 1) << 4; *pp++ = (uint8_t)(0xC0 | (c & 7) << 3 | (a & 7)); }   // (SUB / CMP first: flags dead on the way in)
        int n = 1 + (int)(rnd() % 4);
        for (int k = 0; k < n; k++) body_insn(0);
        int kind = (int)(rnd() % 12);
        if (getenv("DT_KIND")) kind = atoi(getenv("DT_KIND"));      // (one kind of block only: a debugging aid)
        if (i == nb - 1) kind = 0;
        clean_flags();                                       // (every block ends with defined flags: any path may jump on)
        switch (kind) {
        case 1: clean_flags(); *pp++ = 0x0F; *pp++ = (uint8_t)(0x80 | (rnd() % 16)); fx[nfx++] = (fixup){ pp, i + 1 + (int)(rnd() % (unsigned)(nb - i)) }; pp += 4; break;
        case 2: *pp++ = 0xE9; fx[nfx++] = (fixup){ pp, i + 1 + (int)(rnd() % (unsigned)(nb - i)) }; pp += 4; break;
        case 3: *pp++ = 0xE8; fx[nfx++] = (fixup){ pp, -1 }; pp += 4; flags_clean = func_flags; break;
        case 4: { int t = i + 1 + (int)(rnd() % (unsigned)(nb - i));                     // LEA R11, [RIP+x]; JMP R11
                  *pp++ = 0x4C; *pp++ = 0x8D; *pp++ = 0x1D; fx[nfx++] = (fixup){ pp, t }; pp += 4; *pp++ = 0x41; *pp++ = 0xFF; *pp++ = 0xE3; break; }
        case 5: *pp++ = 0x4C; *pp++ = 0x8D; *pp++ = 0x1D; fx[nfx++] = (fixup){ pp, -1 }; pp += 4;  // LEA R11, [RIP+f]; CALL R11
                *pp++ = 0x41; *pp++ = 0xFF; *pp++ = 0xD3; flags_clean = func_flags; break;
        case 6: { *pp++ = 0xB9; uint32_t k = 1 + (uint32_t)(rnd() % 4); memcpy(pp, &k, 4); pp += 4;   // MOV ECX, k; L: ...; LOOP L
                  uint8_t *top = pp; int m = 1 + (int)(rnd() % 2);
                  for (int j = 0; j < m; j++) body_insn(1);
                  clean_flags();                         // (the back edge too: the next round may read them)
                  *pp++ = 0xE2; *pp = (uint8_t)(int8_t)(top - (pp + 1)); pp++; break; }
        case 7: { clean_flags(); *pp++ = (uint8_t)(0x70 | (rnd() % 16)); uint8_t *rel = pp++; uint8_t *from = pp;   // Jcc rel8 over one
                  body_insn(0); *rel = (uint8_t)(pp - from); flags_clean = 0; clean_flags(); break; }
        case 9: case 10: fusable(); between(); *pp++ = 0x0F; *pp++ = (uint8_t)(0x80 | (rnd() % 16)); fx[nfx++] = (fixup){ pp, i + 1 + (int)(rnd() % (unsigned)(nb - i)) }; pp += 4; break;
        case 11: {                                       // CALL (its flags maybe pending), then a writer, a JMP to the next
            *pp++ = 0xE8; fx[nfx++] = (fixup){ pp, -1 }; pp += 4;   //  instruction, and a SETcc there reading the writer's
            int a = preg(0), w = (int)(rnd() & 1), shift = (int)(rnd() & 1);   //  flags first in its block (lazy flags)
            if (shift) { rex_rr(w, 0, a, 0); *pp++ = 0xC1; *pp++ = (uint8_t)(0xC0 | 4 << 3 | (a & 7)); *pp++ = (uint8_t)(1 + rnd() % 30); }   // SHL a, n
            else { int c2 = preg(0); rex_rr(w, c2, a, 0); *pp++ = (uint8_t)((uint8_t[]){ 0x01, 0x29, 0x21, 0x09, 0x31 })[rnd() % 5]; *pp++ = (uint8_t)(0xC0 | (c2 & 7) << 3 | (a & 7)); }
            *pp++ = 0xEB; *pp++ = 0;                     // JMP to the next instruction
            static const uint8_t defined[] = { 2, 3, 4, 5, 8, 9, 10, 11 };   // (after SHL n: CF ZF SF PF; OF and AF not)
            int r = preg(0), cc = shift ? defined[rnd() % 8] : (int)(rnd() % 16);
            rex_rr(0, 0, r, 1); *pp++ = 0x0F; *pp++ = (uint8_t)(0x90 | cc); *pp++ = (uint8_t)(0xC0 | (r & 7));
            flags_clean = 0; clean_flags();              // (CMP: what follows reads only these, so the writer computes its
            break; }                                     //  flags, through merge_flags, rather than recording them)
        case 8: { clean_flags(); *pp++ = (uint8_t)(0xE0 + rnd() % 4); uint8_t *rel = pp++; uint8_t *from = pp;   // LOOPcc/JRCXZ over one
                  body_insn(0); *rel = (uint8_t)(pp - from); flags_clean = 0; clean_flags(); break; }
        }
    }
    clean_flags();
    uint8_t *end_jmp = pp; *pp++ = 0xE9; fx[nfx++] = (fixup){ pp, -2 }; pp += 4;
    (void)end_jmp;
    uint8_t *func = pp;                                  // the function: a few instructions, RET
    int n = 1 + (int)(rnd() % 3);
    for (int k = 0; k < n; k++) body_insn(0);
    if (func_flags) fusable();                           // (its flags: the caller's after the return)
    *pp++ = 0xC3;
    for (int i = 0; i < nfx; i++) {
        uint8_t *to = fx[i].target == -1 ? func : fx[i].target == -2 ? (uint8_t *)(uintptr_t)0 : fx[i].target >= nb ? end_jmp : start[fx[i].target];
        int64_t rel = fx[i].target == -2 ? (int64_t)(DT_END - (DT_PROG + (uint64_t)(fx[i].at + 4 - pp0)))
                                         : (int64_t)(to - (fx[i].at + 4));
        int32_t r32 = (int32_t)rel; memcpy(fx[i].at, &r32, 4);
    }
    c->prog_len = (uint32_t)(pp - pp0);
}

// ---- running the translation ----------------------------------------------------------------------------------------------
static uint32_t *jit;
static sigjmp_buf jb;
static volatile int signaled;
static void on_signal(int sig) { signaled = sig; siglongjmp(jb, 1); }

static int run_translated(const dt_case *c, const x86_insn *d, dt_result *r) {
    xlat_buf b = { 0 };
    xlat_prologue(&b);
    if (xlat_insn(&b, d, c->code, 0x1000) < 0) { free(b.code); return -1; }
    xlat_epilogue(&b);
    pthread_jit_write_protect_np(0);
    memcpy(jit, b.code, b.n * 4);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(jit, b.n * 4);
    free(b.code);
    static x86_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memcpy(ctx.gpr, c->in.gpr, sizeof ctx.gpr);
    memcpy(ctx.xmm, c->in.xmm, sizeof ctx.xmm); memcpy(ctx.ymm_hi, c->in.ymm_hi, sizeof ctx.ymm_hi);
    ctx.flags = c->in.flags; ctx.mxcsr = c->in.mxcsr;
    ctx.fcw = c->in.fcw; ctx.fsw = c->in.fsw; ctx.ftag = c->in.ftag;
    for (int i = 0; i < 8; i++) memcpy(ctx.st[(ctx.fsw >> 11 & 7) + i & 7], c->in.st[i], 10);
    memcpy((void *)DT_BUF, c->mem, DT_BUF_SIZE);
    signaled = 0;
    if (!sigsetjmp(jb, 1)) ((void (*)(x86_ctx *))jit)(&ctx);
    memcpy(r->out.gpr, ctx.gpr, sizeof ctx.gpr);
    memcpy(r->out.xmm, ctx.xmm, sizeof ctx.xmm); memcpy(r->out.ymm_hi, ctx.ymm_hi, sizeof ctx.ymm_hi);
    r->out.mxcsr = ctx.mxcsr;
    r->out.fcw = ctx.fcw; r->out.fsw = ctx.fsw; r->out.ftag = ctx.ftag;
    for (int i = 0; i < 8; i++) memcpy(r->out.st[i], ctx.st[(ctx.fsw >> 11 & 7) + i & 7], 10);
    r->out.flags = ctx.flags;
    r->out.fault = signaled ? 2 : ctx.fault == X86_FAULT_DE ? 1 : ctx.fault ? 2 : 0;   // (other x86 exceptions: signals in the reference)
    if (r->out.fault) { memcpy(r->out.gpr, c->in.gpr, sizeof ctx.gpr); r->out.flags = c->in.flags; }
    memcpy(r->mem, (void *)DT_BUF, DT_BUF_SIZE);
    return 0;
}

// ---- SSE forms -----------------------------------------------------------------------------------------------------------
typedef struct { const char *name; uint8_t pf, op; uint64_t kind; int group; int mem; int map; } sse_form;
#define K_MODEL (1ull << 32)   // Rosetta 2 lacks it (SIGILL): checked against model() instead
#define K_RAND (1ull << 33)    // a random value (RDRAND, RDSEED): its register is not compared
#define K_RAW (1ull << 34)     // fixed bytes: group = length, map = the bytes (little-endian)
#define K_TSC (1ull << 35)     // RDTSC(P): RAX, RDX (and RCX) are the time, not compared
#define K_CPUID (1ull << 36)   // CPUID: EAX a leaf Rosetta 2 knows (or one past), ECX a subleaf
// kind bits: reg / rm are general registers, an imm8, register-only, REX.W variants, 16-byte aligned memory;
// VEX: encoded with VEX, VEX.vvvv not used (1111), used only between registers, 256 bits allowed, only 256 bits,
// no ModRM, imm8[7:4] a register
enum { K_GREG = 1, K_GRM = 2, K_IMM = 4, K_REGONLY = 8, K_W = 16, K_ALIGN = 32,
       K_VEX = 64, K_NOV = 128, K_VREG = 256, K_L1 = 512, K_L1ONLY = 1024, K_NOMODRM = 2048, K_MASK4 = 4096, K_KEEPF = 8192,
       K_SHCNT = 16384, K_APPROX = 32768, K_W1 = 65536, K_NANSIGN = 131072, K_CNT32 = 262144, K_MEMONLY = 524288, K_BOTHF = 1048576, K_FMA = 2097152, K_X87 = 4194304, K_ALIAS = 8388608, K_TRANS = 16777216,
       K_VGPR = 1 << 25, K_UAP = 1 << 26, K_UAPS = 1 << 27, K_LOCKOPT = 1 << 28, K_CMPX = 1 << 29, K_GATHER = 1 << 30, K_STR = 1u << 31 };
// K_STR: strings (PCMPxSTRx): bytes from a few values, zeros among them; EAX / EDX small lengths half the time
// K_VGPR: VEX.vvvv a general register (never RSP); K_UAP / K_UAPS: AF PF (and SF) undefined; K_LOCKOPT: LOCK now and
// then; K_CMPX: memory often equal to RDX:RAX (CMPXCHG8B/16B); K_GATHER: VSIB addressing (index lanes small)
// K_TRANS: an x87 transcendental: x86 allows an error (Intel: about 1 ulp); results within 1 ulp of Rosetta's are taken as equal
// (TRANSLATOR.md: ours are correctly rounded, checked against exact values separately)
// K_ALIAS: an undocumented x87 alias (FSTP1, FCOM2, FXCH4, FFREEP, ...): x86 has them, Rosetta 2 rejects them (counted)
// K_X87: an x87 instruction (op D8-DF, group: ModRM.reg, map: a whole ModRM byte for the register forms that have one)
// K_FMA: ARM raises invalid for 0 * inf + QNaN, x86 does not (ours: counted, flags only)
// K_BOTHF: Rosetta 2 raises the flags of both the addition and the subtraction in every lane (FMADDSUB, FMSUBADD)
// K_CNT32: Rosetta 2 reads only the low 32 bits of VPSRLVQ / VPSLLVQ's 64-bit counts (x86: all 64)
// K_NANSIGN: Rosetta 2 changes the sign of a subtracted NaN (x86 keeps it, as in SUBPS): HSUB, ADDSUB
// K_SHCNT: shift counts (the r/m operand) often small; K_APPROX: an estimate (RCP/RSQRT, see approx_ok); K_W1: VEX.W = 1
// K_KEEPF: Rosetta 2 leaves AF OF PF SF as they were where x86 clears them (VTESTPS/PD, VPTEST, PTEST)
static const sse_form sse_forms[] = {
    { "movups", 0, 0x10, 0, -1, 16 }, { "movupd", 0x66, 0x10, 0, -1, 16 }, { "movss", 0xF3, 0x10, 0, -1, 4 }, { "movsd", 0xF2, 0x10, 0, -1, 8 },
    { "movups st", 0, 0x11, 0, -1, 16 }, { "movss st", 0xF3, 0x11, 0, -1, 4 }, { "movsd st", 0xF2, 0x11, 0, -1, 8 },
    { "movaps", 0, 0x28, K_ALIGN, -1, 16 }, { "movapd", 0x66, 0x28, K_ALIGN, -1, 16 }, { "movaps st", 0, 0x29, K_ALIGN, -1, 16 },
    { "movdqa", 0x66, 0x6F, K_ALIGN, -1, 16 }, { "movdqu", 0xF3, 0x6F, 0, -1, 16 }, { "movdqa st", 0x66, 0x7F, K_ALIGN, -1, 16 }, { "movdqu st", 0xF3, 0x7F, 0, -1, 16 },
    { "movlps/movhlps", 0, 0x12, 0, -1, 8 }, { "movhps/movlhps", 0, 0x16, 0, -1, 8 }, { "movlpd", 0x66, 0x12, 0, -2, 8 }, { "movhpd", 0x66, 0x16, 0, -2, 8 },
    { "movlps st", 0, 0x13, 0, -2, 8 }, { "movhps st", 0, 0x17, 0, -2, 8 },
    { "unpcklps", 0, 0x14, K_ALIGN, -1, 16 }, { "unpckhps", 0, 0x15, K_ALIGN, -1, 16 }, { "unpcklpd", 0x66, 0x14, K_ALIGN, -1, 16 }, { "unpckhpd", 0x66, 0x15, K_ALIGN, -1, 16 },
    { "shufps", 0, 0xC6, K_IMM | K_ALIGN, -1, 16 }, { "shufpd", 0x66, 0xC6, K_IMM | K_ALIGN, -1, 16 },
    { "andps", 0, 0x54, K_ALIGN, -1, 16 }, { "andnps", 0, 0x55, K_ALIGN, -1, 16 }, { "orps", 0, 0x56, K_ALIGN, -1, 16 }, { "xorps", 0, 0x57, K_ALIGN, -1, 16 },
    { "andpd", 0x66, 0x54, K_ALIGN, -1, 16 }, { "xorpd", 0x66, 0x57, K_ALIGN, -1, 16 },
#define FP(nm, o) { nm "ps", 0, o, K_ALIGN, -1, 16 }, { nm "pd", 0x66, o, K_ALIGN, -1, 16 }, { nm "ss", 0xF3, o, 0, -1, 4 }, { nm "sd", 0xF2, o, 0, -1, 8 }
    FP("add", 0x58), FP("mul", 0x59), FP("sub", 0x5C), FP("div", 0x5E), FP("min", 0x5D), FP("max", 0x5F), FP("sqrt", 0x51),
#undef FP
    { "cmpps", 0, 0xC2, K_IMM | K_ALIGN, -1, 16 }, { "cmppd", 0x66, 0xC2, K_IMM | K_ALIGN, -1, 16 }, { "cmpss", 0xF3, 0xC2, K_IMM, -1, 4 }, { "cmpsd", 0xF2, 0xC2, K_IMM, -1, 8 },
    { "ucomiss", 0, 0x2E, 0, -1, 4 }, { "comiss", 0, 0x2F, 0, -1, 4 }, { "ucomisd", 0x66, 0x2E, 0, -1, 8 }, { "comisd", 0x66, 0x2F, 0, -1, 8 },
    { "cvtsi2ss", 0xF3, 0x2A, K_GRM | K_W, -1, 4 }, { "cvtsi2sd", 0xF2, 0x2A, K_GRM | K_W, -1, 4 },
    { "cvttss2si", 0xF3, 0x2C, K_GREG | K_W, -1, 4 }, { "cvtss2si", 0xF3, 0x2D, K_GREG | K_W, -1, 4 },
    { "cvttsd2si", 0xF2, 0x2C, K_GREG | K_W, -1, 8 }, { "cvtsd2si", 0xF2, 0x2D, K_GREG | K_W, -1, 8 },
    { "cvtps2pd", 0, 0x5A, 0, -1, 8 }, { "cvtpd2ps", 0x66, 0x5A, K_ALIGN, -1, 16 }, { "cvtss2sd", 0xF3, 0x5A, 0, -1, 4 }, { "cvtsd2ss", 0xF2, 0x5A, 0, -1, 8 },
    { "cvtdq2ps", 0, 0x5B, K_ALIGN, -1, 16 }, { "cvtps2dq", 0x66, 0x5B, K_ALIGN, -1, 16 }, { "cvttps2dq", 0xF3, 0x5B, K_ALIGN, -1, 16 }, { "cvtdq2pd", 0xF3, 0xE6, 0, -1, 8 },
    { "movmskps", 0, 0x50, K_GREG | K_REGONLY, -1, 0 }, { "movmskpd", 0x66, 0x50, K_GREG | K_REGONLY, -1, 0 },
    { "movd/q xmm, r/m", 0x66, 0x6E, K_GRM | K_W, -1, 4 }, { "movd/q r/m, xmm", 0x66, 0x7E, K_GRM | K_W, -1, 4 },
    { "movq xmm, xmm/m64", 0xF3, 0x7E, 0, -1, 8 }, { "movq xmm/m64, xmm", 0x66, 0xD6, 0, -1, 8 },
    { "pshufd", 0x66, 0x70, K_IMM | K_ALIGN, -1, 16 }, { "pshuflw", 0xF2, 0x70, K_IMM | K_ALIGN, -1, 16 }, { "pshufhw", 0xF3, 0x70, K_IMM | K_ALIGN, -1, 16 },
    { "psrlw imm", 0x66, 0x71, K_IMM | K_REGONLY, 2, 0 }, { "psraw imm", 0x66, 0x71, K_IMM | K_REGONLY, 4, 0 }, { "psllw imm", 0x66, 0x71, K_IMM | K_REGONLY, 6, 0 },
    { "psrld imm", 0x66, 0x72, K_IMM | K_REGONLY, 2, 0 }, { "psrad imm", 0x66, 0x72, K_IMM | K_REGONLY, 4, 0 }, { "pslld imm", 0x66, 0x72, K_IMM | K_REGONLY, 6, 0 },
    { "psrlq imm", 0x66, 0x73, K_IMM | K_REGONLY, 2, 0 }, { "psllq imm", 0x66, 0x73, K_IMM | K_REGONLY, 6, 0 },
    { "psrldq", 0x66, 0x73, K_IMM | K_REGONLY, 3, 0 }, { "pslldq", 0x66, 0x73, K_IMM | K_REGONLY, 7, 0 },
#define PI(nm, o) { nm, 0x66, o, K_ALIGN, -1, 16 }
    PI("paddb", 0xFC), PI("paddw", 0xFD), PI("paddd", 0xFE), PI("paddq", 0xD4), PI("psubb", 0xF8), PI("psubw", 0xF9), PI("psubd", 0xFA), PI("psubq", 0xFB),
    PI("pand", 0xDB), PI("pandn", 0xDF), PI("por", 0xEB), PI("pxor", 0xEF), PI("pcmpeqb", 0x74), PI("pcmpeqw", 0x75), PI("pcmpeqd", 0x76),
    PI("pcmpgtb", 0x64), PI("pcmpgtw", 0x65), PI("pcmpgtd", 0x66), PI("punpcklbw", 0x60), PI("punpcklwd", 0x61), PI("punpckldq", 0x62),
    PI("punpcklqdq", 0x6C), PI("punpckhbw", 0x68), PI("punpckhwd", 0x69), PI("punpckhdq", 0x6A), PI("punpckhqdq", 0x6D), PI("pmaddwd", 0xF5), PI("pmullw", 0xD5),
#undef PI
    // AVX (VEX; 128 and 256 bits)
#define V K_VEX
#define VN (K_VEX | K_NOV)
    { "ldmxcsr", 0, 0xAE, K_MEMONLY, 2, 4 }, { "stmxcsr", 0, 0xAE, K_MEMONLY, 3, 4 }, { "vldmxcsr", 0, 0xAE, VN | K_MEMONLY, 2, 4 }, { "vstmxcsr", 0, 0xAE, VN | K_MEMONLY, 3, 4 },
    { "movntps", 0, 0x2B, K_ALIGN, -2, 16 }, { "movntdq", 0x66, 0xE7, K_ALIGN, -2, 16 },
    { "vmovups", 0, 0x10, VN | K_L1, -1, 16 }, { "vmovupd", 0x66, 0x10, VN | K_L1, -1, 16 }, { "vmovss", 0xF3, 0x10, V | K_VREG, -1, 4 }, { "vmovsd", 0xF2, 0x10, V | K_VREG, -1, 8 },
    { "vmovups st", 0, 0x11, VN | K_L1, -1, 16 }, { "vmovss st", 0xF3, 0x11, V | K_VREG, -1, 4 }, { "vmovsd st", 0xF2, 0x11, V | K_VREG, -1, 8 },
    { "vmovaps", 0, 0x28, VN | K_L1 | K_ALIGN, -1, 16 }, { "vmovapd", 0x66, 0x28, VN | K_L1 | K_ALIGN, -1, 16 }, { "vmovaps st", 0, 0x29, VN | K_L1 | K_ALIGN, -1, 16 },
    { "vmovdqa", 0x66, 0x6F, VN | K_L1 | K_ALIGN, -1, 16 }, { "vmovdqu", 0xF3, 0x6F, VN | K_L1, -1, 16 }, { "vmovdqa st", 0x66, 0x7F, VN | K_L1 | K_ALIGN, -1, 16 },
    { "vmovdqu st", 0xF3, 0x7F, VN | K_L1, -1, 16 }, { "vmovntps", 0, 0x2B, VN | K_L1 | K_ALIGN, -2, 16 }, { "vmovntdq", 0x66, 0xE7, VN | K_L1 | K_ALIGN, -2, 16 },
    { "vmovlps/vmovhlps", 0, 0x12, V, -1, 8 }, { "vmovhps/vmovlhps", 0, 0x16, V, -1, 8 }, { "vmovlpd", 0x66, 0x12, V, -2, 8 }, { "vmovhpd", 0x66, 0x16, V, -2, 8 },
    { "vmovlps st", 0, 0x13, VN, -2, 8 }, { "vmovhps st", 0, 0x17, VN, -2, 8 },
    { "vunpcklps", 0, 0x14, V | K_L1, -1, 16 }, { "vunpckhps", 0, 0x15, V | K_L1, -1, 16 }, { "vunpcklpd", 0x66, 0x14, V | K_L1, -1, 16 }, { "vunpckhpd", 0x66, 0x15, V | K_L1, -1, 16 },
    { "vshufps", 0, 0xC6, V | K_L1 | K_IMM, -1, 16 }, { "vshufpd", 0x66, 0xC6, V | K_L1 | K_IMM, -1, 16 },
    { "vandps", 0, 0x54, V | K_L1, -1, 16 }, { "vandnps", 0, 0x55, V | K_L1, -1, 16 }, { "vorps", 0, 0x56, V | K_L1, -1, 16 }, { "vxorps", 0, 0x57, V | K_L1, -1, 16 },
    { "vandpd", 0x66, 0x54, V | K_L1, -1, 16 }, { "vxorpd", 0x66, 0x57, V | K_L1, -1, 16 },
#define VFP(nm, o) { "v" nm "ps", 0, o, V | K_L1, -1, 16 }, { "v" nm "pd", 0x66, o, V | K_L1, -1, 16 }, { "v" nm "ss", 0xF3, o, V, -1, 4 }, { "v" nm "sd", 0xF2, o, V, -1, 8 }
    VFP("add", 0x58), VFP("mul", 0x59), VFP("sub", 0x5C), VFP("div", 0x5E), VFP("min", 0x5D), VFP("max", 0x5F),
#undef VFP
    { "vsqrtps", 0, 0x51, VN | K_L1, -1, 16 }, { "vsqrtpd", 0x66, 0x51, VN | K_L1, -1, 16 }, { "vsqrtss", 0xF3, 0x51, V, -1, 4 }, { "vsqrtsd", 0xF2, 0x51, V, -1, 8 },
    { "vcmpps", 0, 0xC2, V | K_L1 | K_IMM, -1, 16 }, { "vcmppd", 0x66, 0xC2, V | K_L1 | K_IMM, -1, 16 }, { "vcmpss", 0xF3, 0xC2, V | K_IMM, -1, 4 }, { "vcmpsd", 0xF2, 0xC2, V | K_IMM, -1, 8 },
    { "vucomiss", 0, 0x2E, VN, -1, 4 }, { "vcomiss", 0, 0x2F, VN, -1, 4 }, { "vucomisd", 0x66, 0x2E, VN, -1, 8 }, { "vcomisd", 0x66, 0x2F, VN, -1, 8 },
    { "vcvtsi2ss", 0xF3, 0x2A, V | K_GRM | K_W, -1, 4 }, { "vcvtsi2sd", 0xF2, 0x2A, V | K_GRM | K_W, -1, 4 },
    { "vcvttss2si", 0xF3, 0x2C, VN | K_GREG | K_W, -1, 4 }, { "vcvtss2si", 0xF3, 0x2D, VN | K_GREG | K_W, -1, 4 },
    { "vcvttsd2si", 0xF2, 0x2C, VN | K_GREG | K_W, -1, 8 }, { "vcvtsd2si", 0xF2, 0x2D, VN | K_GREG | K_W, -1, 8 },
    { "vcvtps2pd", 0, 0x5A, VN | K_L1, -1, 8 }, { "vcvtpd2ps", 0x66, 0x5A, VN | K_L1, -1, 16 }, { "vcvtss2sd", 0xF3, 0x5A, V, -1, 4 }, { "vcvtsd2ss", 0xF2, 0x5A, V, -1, 8 },
    { "vcvtdq2ps", 0, 0x5B, VN | K_L1, -1, 16 }, { "vcvtps2dq", 0x66, 0x5B, VN | K_L1, -1, 16 }, { "vcvttps2dq", 0xF3, 0x5B, VN | K_L1, -1, 16 },
    { "vcvtdq2pd", 0xF3, 0xE6, VN | K_L1, -1, 8 },
    { "vmovmskps", 0, 0x50, VN | K_L1 | K_GREG | K_REGONLY, -1, 0 }, { "vmovmskpd", 0x66, 0x50, VN | K_L1 | K_GREG | K_REGONLY, -1, 0 },
    { "vmovd/q xmm, r/m", 0x66, 0x6E, VN | K_GRM | K_W, -1, 4 }, { "vmovd/q r/m, xmm", 0x66, 0x7E, VN | K_GRM | K_W, -1, 4 },
    { "vmovq xmm, xmm/m64", 0xF3, 0x7E, VN, -1, 8 }, { "vmovq xmm/m64, xmm", 0x66, 0xD6, VN, -1, 8 },
    { "vpshufd", 0x66, 0x70, VN | K_L1 | K_IMM, -1, 16 }, { "vpshuflw", 0xF2, 0x70, VN | K_L1 | K_IMM, -1, 16 }, { "vpshufhw", 0xF3, 0x70, VN | K_L1 | K_IMM, -1, 16 },
    { "vpsrlw imm", 0x66, 0x71, V | K_L1 | K_IMM | K_REGONLY, 2, 0 }, { "vpsraw imm", 0x66, 0x71, V | K_L1 | K_IMM | K_REGONLY, 4, 0 },
    { "vpsllw imm", 0x66, 0x71, V | K_L1 | K_IMM | K_REGONLY, 6, 0 }, { "vpsrld imm", 0x66, 0x72, V | K_L1 | K_IMM | K_REGONLY, 2, 0 },
    { "vpsrad imm", 0x66, 0x72, V | K_L1 | K_IMM | K_REGONLY, 4, 0 }, { "vpslld imm", 0x66, 0x72, V | K_L1 | K_IMM | K_REGONLY, 6, 0 },
    { "vpsrlq imm", 0x66, 0x73, V | K_L1 | K_IMM | K_REGONLY, 2, 0 }, { "vpsllq imm", 0x66, 0x73, V | K_L1 | K_IMM | K_REGONLY, 6, 0 },
    { "vpsrldq", 0x66, 0x73, V | K_L1 | K_IMM | K_REGONLY, 3, 0 }, { "vpslldq", 0x66, 0x73, V | K_L1 | K_IMM | K_REGONLY, 7, 0 },
#define VPI(nm, o) { "v" nm, 0x66, o, V | K_L1, -1, 16 }
    VPI("paddb", 0xFC), VPI("paddw", 0xFD), VPI("paddd", 0xFE), VPI("paddq", 0xD4), VPI("psubb", 0xF8), VPI("psubw", 0xF9), VPI("psubd", 0xFA), VPI("psubq", 0xFB),
    VPI("pand", 0xDB), VPI("pandn", 0xDF), VPI("por", 0xEB), VPI("pxor", 0xEF), VPI("pcmpeqb", 0x74), VPI("pcmpeqw", 0x75), VPI("pcmpeqd", 0x76),
    VPI("pcmpgtb", 0x64), VPI("pcmpgtw", 0x65), VPI("pcmpgtd", 0x66), VPI("punpcklbw", 0x60), VPI("punpcklwd", 0x61), VPI("punpckldq", 0x62),
    VPI("punpcklqdq", 0x6C), VPI("punpckhbw", 0x68), VPI("punpckhwd", 0x69), VPI("punpckhdq", 0x6A), VPI("punpckhqdq", 0x6D), VPI("pmaddwd", 0xF5), VPI("pmullw", 0xD5),
#undef VPI
    { "vzeroupper", 0, 0x77, VN | K_NOMODRM, -1, 0 }, { "vzeroall", 0, 0x77, VN | K_NOMODRM | K_L1ONLY, -1, 0 },
    { "vbroadcastss", 0x66, 0x18, VN | K_L1, -1, 4, 2 }, { "vbroadcastsd", 0x66, 0x19, VN | K_L1ONLY, -1, 8, 2 }, { "vbroadcastf128", 0x66, 0x1A, VN | K_L1ONLY, -2, 16, 2 },
    { "vinsertf128", 0x66, 0x18, V | K_L1ONLY | K_IMM, -1, 16, 3 }, { "vextractf128", 0x66, 0x19, VN | K_L1ONLY | K_IMM, -1, 16, 3 },
    { "vperm2f128", 0x66, 0x06, V | K_L1ONLY | K_IMM, -1, 32, 3 },
    { "vtestps", 0x66, 0x0E, VN | K_L1 | K_KEEPF, -1, 16, 2 }, { "vtestpd", 0x66, 0x0F, VN | K_L1 | K_KEEPF, -1, 16, 2 }, { "vptest", 0x66, 0x17, VN | K_L1 | K_KEEPF, -1, 16, 2 },
    { "vmaskmovps", 0x66, 0x2C, V | K_L1, -2, 16, 2 }, { "vmaskmovpd", 0x66, 0x2D, V | K_L1, -2, 16, 2 },
    { "vmaskmovps st", 0x66, 0x2E, V | K_L1, -2, 16, 2 }, { "vmaskmovpd st", 0x66, 0x2F, V | K_L1, -2, 16, 2 },
    { "vpermilps", 0x66, 0x0C, V | K_L1, -1, 16, 2 }, { "vpermilpd", 0x66, 0x0D, V | K_L1, -1, 16, 2 },
    { "vpermilps imm", 0x66, 0x04, VN | K_L1 | K_IMM, -1, 16, 3 }, { "vpermilpd imm", 0x66, 0x05, VN | K_L1 | K_IMM, -1, 16, 3 },
    { "vblendps", 0x66, 0x0C, V | K_L1 | K_IMM, -1, 16, 3 }, { "vblendpd", 0x66, 0x0D, V | K_L1 | K_IMM, -1, 16, 3 },
    { "vblendvps", 0x66, 0x4A, V | K_L1 | K_MASK4, -1, 16, 3 }, { "vblendvpd", 0x66, 0x4B, V | K_L1 | K_MASK4, -1, 16, 3 },
    // SSE / SSE2 rest, SSE3
    { "rsqrtps", 0, 0x52, K_ALIGN | K_APPROX, -1, 16 }, { "rsqrtss", 0xF3, 0x52, K_APPROX, -1, 4 },
    { "rcpps", 0, 0x53, K_ALIGN | K_APPROX, -1, 16 }, { "rcpss", 0xF3, 0x53, K_APPROX, -1, 4 },
    { "haddps", 0xF2, 0x7C, K_ALIGN, -1, 16 }, { "haddpd", 0x66, 0x7C, K_ALIGN, -1, 16 }, { "hsubps", 0xF2, 0x7D, K_ALIGN | K_NANSIGN, -1, 16 }, { "hsubpd", 0x66, 0x7D, K_ALIGN | K_NANSIGN, -1, 16 },
    { "addsubps", 0xF2, 0xD0, K_ALIGN | K_NANSIGN, -1, 16 }, { "addsubpd", 0x66, 0xD0, K_ALIGN | K_NANSIGN, -1, 16 },
    { "movddup", 0xF2, 0x12, 0, -1, 8 }, { "movsldup", 0xF3, 0x12, K_ALIGN, -1, 16 }, { "movshdup", 0xF3, 0x16, K_ALIGN, -1, 16 }, { "lddqu", 0xF2, 0xF0, 0, -2, 16 },
    { "cvtpd2dq", 0xF2, 0xE6, K_ALIGN, -1, 16 }, { "cvttpd2dq", 0x66, 0xE6, K_ALIGN, -1, 16 },
    { "pinsrw", 0x66, 0xC4, K_GRM | K_IMM, -1, 2 }, { "pextrw", 0x66, 0xC5, K_GREG | K_REGONLY | K_IMM, -1, 0 }, { "pmovmskb", 0x66, 0xD7, K_GREG | K_REGONLY, -1, 0 },
#define PI2(nm, o, k) { nm, 0x66, o, K_ALIGN | (k), -1, 16 }, { "v" nm, 0x66, o, V | K_L1 | (k), -1, 16 }
    PI2("packsswb", 0x63, 0), PI2("packuswb", 0x67, 0), PI2("packssdw", 0x6B, 0), PI2("psubusb", 0xD8, 0), PI2("psubusw", 0xD9, 0),
    PI2("paddusb", 0xDC, 0), PI2("paddusw", 0xDD, 0), PI2("psubsb", 0xE8, 0), PI2("psubsw", 0xE9, 0), PI2("paddsb", 0xEC, 0), PI2("paddsw", 0xED, 0),
    PI2("pminub", 0xDA, 0), PI2("pmaxub", 0xDE, 0), PI2("pminsw", 0xEA, 0), PI2("pmaxsw", 0xEE, 0), PI2("pavgb", 0xE0, 0), PI2("pavgw", 0xE3, 0),
    PI2("pmulhuw", 0xE4, 0), PI2("pmulhw", 0xE5, 0), PI2("pmuludq", 0xF4, 0), PI2("psadbw", 0xF6, 0),
    PI2("psrlw", 0xD1, K_SHCNT), PI2("psrld", 0xD2, K_SHCNT), PI2("psrlq", 0xD3, K_SHCNT), PI2("psraw", 0xE1, K_SHCNT), PI2("psrad", 0xE2, K_SHCNT),
    PI2("psllw", 0xF1, K_SHCNT), PI2("pslld", 0xF2, K_SHCNT), PI2("psllq", 0xF3, K_SHCNT),
#undef PI2
    { "vrsqrtps", 0, 0x52, VN | K_L1 | K_APPROX, -1, 16 }, { "vrsqrtss", 0xF3, 0x52, V | K_APPROX, -1, 4 },
    { "vrcpps", 0, 0x53, VN | K_L1 | K_APPROX, -1, 16 }, { "vrcpss", 0xF3, 0x53, V | K_APPROX, -1, 4 },
    { "vhaddps", 0xF2, 0x7C, V | K_L1, -1, 16 }, { "vhaddpd", 0x66, 0x7C, V | K_L1, -1, 16 }, { "vhsubps", 0xF2, 0x7D, V | K_L1 | K_NANSIGN, -1, 16 }, { "vhsubpd", 0x66, 0x7D, V | K_L1 | K_NANSIGN, -1, 16 },
    { "vaddsubps", 0xF2, 0xD0, V | K_L1 | K_NANSIGN, -1, 16 }, { "vaddsubpd", 0x66, 0xD0, V | K_L1 | K_NANSIGN, -1, 16 },
    { "vmovddup", 0xF2, 0x12, VN | K_L1, -1, 8 }, { "vmovsldup", 0xF3, 0x12, VN | K_L1, -1, 16 }, { "vmovshdup", 0xF3, 0x16, VN | K_L1, -1, 16 },
    { "vlddqu", 0xF2, 0xF0, VN | K_L1, -2, 16 }, { "vcvtpd2dq", 0xF2, 0xE6, VN | K_L1, -1, 16 }, { "vcvttpd2dq", 0x66, 0xE6, VN | K_L1, -1, 16 },
    { "vpinsrw", 0x66, 0xC4, V | K_GRM | K_IMM, -1, 2 }, { "vpextrw", 0x66, 0xC5, VN | K_GREG | K_REGONLY | K_IMM, -1, 0 },
    { "vpmovmskb", 0x66, 0xD7, VN | K_L1 | K_GREG | K_REGONLY, -1, 0 },
    // SSSE3, SSE4.1 (legacy and VEX)
#define S4(nm, o, map, k, vk, mem) { nm, 0x66, o, (k), -1, mem, map }, { "v" nm, 0x66, o, (vk), -1, mem, map }
    S4("pshufb", 0x00, 2, K_ALIGN, V | K_L1, 16), S4("phaddw", 0x01, 2, K_ALIGN, V | K_L1, 16), S4("phaddd", 0x02, 2, K_ALIGN, V | K_L1, 16),
    S4("phaddsw", 0x03, 2, K_ALIGN, V | K_L1, 16), S4("pmaddubsw", 0x04, 2, K_ALIGN, V | K_L1, 16), S4("phsubw", 0x05, 2, K_ALIGN, V | K_L1, 16),
    S4("phsubd", 0x06, 2, K_ALIGN, V | K_L1, 16), S4("phsubsw", 0x07, 2, K_ALIGN, V | K_L1, 16), S4("psignb", 0x08, 2, K_ALIGN, V | K_L1, 16),
    S4("psignw", 0x09, 2, K_ALIGN, V | K_L1, 16), S4("psignd", 0x0A, 2, K_ALIGN, V | K_L1, 16), S4("pmulhrsw", 0x0B, 2, K_ALIGN, V | K_L1, 16),
    { "pblendvb", 0x66, 0x10, K_ALIGN, -1, 16, 2 }, { "blendvps", 0x66, 0x14, K_ALIGN, -1, 16, 2 }, { "blendvpd", 0x66, 0x15, K_ALIGN, -1, 16, 2 },
    { "ptest", 0x66, 0x17, K_ALIGN | K_KEEPF, -1, 16, 2 },
    S4("pabsb", 0x1C, 2, K_ALIGN, VN | K_L1, 16), S4("pabsw", 0x1D, 2, K_ALIGN, VN | K_L1, 16), S4("pabsd", 0x1E, 2, K_ALIGN, VN | K_L1, 16),
    S4("pmovsxbw", 0x20, 2, 0, VN | K_L1, 8), S4("pmovsxbd", 0x21, 2, 0, VN | K_L1, 4), S4("pmovsxbq", 0x22, 2, 0, VN | K_L1, 2),
    S4("pmovsxwd", 0x23, 2, 0, VN | K_L1, 8), S4("pmovsxwq", 0x24, 2, 0, VN | K_L1, 4), S4("pmovsxdq", 0x25, 2, 0, VN | K_L1, 8),
    S4("pmovzxbw", 0x30, 2, 0, VN | K_L1, 8), S4("pmovzxbd", 0x31, 2, 0, VN | K_L1, 4), S4("pmovzxbq", 0x32, 2, 0, VN | K_L1, 2),
    S4("pmovzxwd", 0x33, 2, 0, VN | K_L1, 8), S4("pmovzxwq", 0x34, 2, 0, VN | K_L1, 4), S4("pmovzxdq", 0x35, 2, 0, VN | K_L1, 8),
    S4("pmuldq", 0x28, 2, K_ALIGN, V | K_L1, 16), S4("pcmpeqq", 0x29, 2, K_ALIGN, V | K_L1, 16), S4("packusdw", 0x2B, 2, K_ALIGN, V | K_L1, 16),
    { "movntdqa", 0x66, 0x2A, K_ALIGN, -2, 16, 2 }, { "vmovntdqa", 0x66, 0x2A, VN | K_L1 | K_ALIGN, -2, 16, 2 },
    S4("pcmpgtq", 0x37, 2, K_ALIGN, V | K_L1, 16), S4("pminsb", 0x38, 2, K_ALIGN, V | K_L1, 16), S4("pminsd", 0x39, 2, K_ALIGN, V | K_L1, 16),
    S4("pminuw", 0x3A, 2, K_ALIGN, V | K_L1, 16), S4("pminud", 0x3B, 2, K_ALIGN, V | K_L1, 16), S4("pmaxsb", 0x3C, 2, K_ALIGN, V | K_L1, 16),
    S4("pmaxsd", 0x3D, 2, K_ALIGN, V | K_L1, 16), S4("pmaxuw", 0x3E, 2, K_ALIGN, V | K_L1, 16), S4("pmaxud", 0x3F, 2, K_ALIGN, V | K_L1, 16),
    S4("pmulld", 0x40, 2, K_ALIGN, V | K_L1, 16),
    S4("roundps", 0x08, 3, K_ALIGN | K_IMM, VN | K_L1 | K_IMM, 16), S4("roundpd", 0x09, 3, K_ALIGN | K_IMM, VN | K_L1 | K_IMM, 16),
    S4("roundss", 0x0A, 3, K_IMM, V | K_IMM, 4), S4("roundsd", 0x0B, 3, K_IMM, V | K_IMM, 8),
    { "blendps", 0x66, 0x0C, K_ALIGN | K_IMM, -1, 16, 3 }, { "blendpd", 0x66, 0x0D, K_ALIGN | K_IMM, -1, 16, 3 },
    S4("pblendw", 0x0E, 3, K_ALIGN | K_IMM, V | K_L1 | K_IMM, 16), S4("palignr", 0x0F, 3, K_ALIGN | K_IMM, V | K_L1 | K_IMM, 16),
    S4("pextrb", 0x14, 3, K_GRM | K_IMM, VN | K_GRM | K_IMM, 1), S4("pextrw m", 0x15, 3, K_GRM | K_IMM, VN | K_GRM | K_IMM, 2),
    S4("pextrd/q", 0x16, 3, K_GRM | K_W | K_IMM, VN | K_GRM | K_W | K_IMM, 4), S4("extractps", 0x17, 3, K_GRM | K_IMM, VN | K_GRM | K_IMM, 4),
    S4("pinsrb", 0x20, 3, K_GRM | K_IMM, V | K_GRM | K_IMM, 1), S4("insertps", 0x21, 3, K_IMM, V | K_IMM, 4),
    S4("pinsrd/q", 0x22, 3, K_GRM | K_W | K_IMM, V | K_GRM | K_W | K_IMM, 4),
    S4("dpps", 0x40, 3, K_ALIGN | K_IMM, V | K_L1 | K_IMM, 16), S4("dppd", 0x41, 3, K_ALIGN | K_IMM, V | K_IMM, 16),
    S4("pcmpestrm", 0x60, 3, K_IMM | K_STR | K_W, VN | K_IMM | K_STR | K_W, 16), S4("pcmpestri", 0x61, 3, K_IMM | K_STR | K_W, VN | K_IMM | K_STR | K_W, 16),
    S4("pcmpistrm", 0x62, 3, K_IMM | K_STR, VN | K_IMM | K_STR, 16), S4("pcmpistri", 0x63, 3, K_IMM | K_STR, VN | K_IMM | K_STR, 16),
    S4("aeskeygenassist", 0xDF, 3, K_ALIGN | K_IMM, VN | K_IMM, 16),
    { "crc32 r, r/m8", 0xF2, 0xF0, K_GREG | K_GRM | K_W, -1, 1, 2 }, { "crc32 r, r/m", 0xF2, 0xF1, K_GREG | K_GRM | K_W, -1, 4, 2 },
    { "movbe r, m", 0, 0xF0, K_GREG | K_GRM | K_MEMONLY | K_W, -1, 4, 2 }, { "movbe m, r", 0, 0xF1, K_GREG | K_GRM | K_MEMONLY | K_W, -1, 4, 2 },
    { "int3", 0, 0, K_RAW, 1, 0, 0xCC }, { "int 29h", 0, 0, K_RAW, 2, 0, 0x29CD }, { "ud2", 0, 0, K_RAW, 2, 0, 0x0B0F },
    { "cpuid", 0, 0, K_RAW | K_CPUID, 2, 0, 0xA20F }, { "xgetbv", 0, 0, K_RAW, 3, 0, 0xD0010F },
    { "rdtsc", 0, 0, K_RAW | K_TSC, 2, 0, 0x310F }, { "rdtscp", 0, 0, K_RAW | K_TSC, 3, 0, 0xF9010F },
    { "rdrand", 0, 0xC7, K_GRM | K_REGONLY | K_W | K_RAND, 6, 0 }, { "rdseed", 0, 0xC7, K_GRM | K_REGONLY | K_W | K_RAND, 7, 0 },
    { "adcx", 0x66, 0xF6, K_GREG | K_GRM | K_W | K_MODEL, -1, 4, 2 }, { "adox", 0xF3, 0xF6, K_GREG | K_GRM | K_W | K_MODEL, -1, 4, 2 },
    S4("aesimc", 0xDB, 2, K_ALIGN, VN, 16), S4("aesenc", 0xDC, 2, K_ALIGN, V, 16), S4("aesenclast", 0xDD, 2, K_ALIGN, V, 16),
    S4("aesdec", 0xDE, 2, K_ALIGN, V, 16), S4("aesdeclast", 0xDF, 2, K_ALIGN, V, 16), S4("pclmulqdq", 0x44, 3, K_ALIGN | K_IMM, V | K_IMM, 16),
#undef S4
    // BMI1 / BMI2
#define VG (K_VEX | K_GRM | K_W | K_VGPR)
    { "andn", 0, 0xF2, VG | K_GREG | K_UAP, -1, 8, 2 }, { "blsr", 0, 0xF3, VG | K_UAP, 1, 8, 2 }, { "blsmsk", 0, 0xF3, VG | K_UAP, 2, 8, 2 },
    { "blsi", 0, 0xF3, VG | K_UAP, 3, 8, 2 }, { "bzhi", 0, 0xF5, VG | K_GREG | K_UAP, -1, 8, 2 }, { "pdep", 0xF2, 0xF5, VG | K_GREG, -1, 8, 2 },
    { "pext", 0xF3, 0xF5, VG | K_GREG, -1, 8, 2 }, { "mulx", 0xF2, 0xF6, VG | K_GREG, -1, 8, 2 }, { "bextr", 0, 0xF7, VG | K_GREG | K_UAPS, -1, 8, 2 },
    { "shlx", 0x66, 0xF7, VG | K_GREG, -1, 8, 2 }, { "sarx", 0xF3, 0xF7, VG | K_GREG, -1, 8, 2 }, { "shrx", 0xF2, 0xF7, VG | K_GREG, -1, 8, 2 },
    { "rorx", 0xF2, 0xF0, K_VEX | K_NOV | K_GRM | K_W | K_GREG | K_IMM, -1, 8, 3 },
#undef VG
    { "cmpxchg8b", 0, 0xC7, K_MEMONLY | K_LOCKOPT | K_CMPX, 1, 8 }, { "cmpxchg16b", 0, 0xC7, K_MEMONLY | K_LOCKOPT | K_CMPX | K_W1 | K_ALIGN, 1, 16 },
    // AVX2 gathers, F16C
    { "vpgatherdd", 0x66, 0x90, V | K_L1 | K_GATHER, -1, 4, 2 }, { "vpgatherdq", 0x66, 0x90, V | K_L1 | K_GATHER | K_W1, -1, 8, 2 },
    { "vpgatherqd", 0x66, 0x91, V | K_L1 | K_GATHER, -1, 4, 2 }, { "vpgatherqq", 0x66, 0x91, V | K_L1 | K_GATHER | K_W1, -1, 8, 2 },
    { "vgatherdps", 0x66, 0x92, V | K_L1 | K_GATHER, -1, 4, 2 }, { "vgatherdpd", 0x66, 0x92, V | K_L1 | K_GATHER | K_W1, -1, 8, 2 },
    { "vgatherqps", 0x66, 0x93, V | K_L1 | K_GATHER, -1, 4, 2 }, { "vgatherqpd", 0x66, 0x93, V | K_L1 | K_GATHER | K_W1, -1, 8, 2 },
    { "vcvtph2ps", 0x66, 0x13, VN | K_L1, -1, 8, 2 }, { "vcvtps2ph", 0x66, 0x1D, VN | K_L1 | K_IMM, -1, 8, 3 },
    { "vpblendvb", 0x66, 0x4C, V | K_L1 | K_MASK4, -1, 16, 3 }, { "vpblendd", 0x66, 0x02, V | K_L1 | K_IMM, -1, 16, 3 },
    // FMA
#define FMA3(nm, o, k) { "vf" nm "132", 0x66, 0x90 + o, (k) | K_FMA, -1, 16, 2 }, { "vf" nm "213", 0x66, 0xA0 + o, (k) | K_FMA, -1, 16, 2 }, { "vf" nm "231", 0x66, 0xB0 + o, (k) | K_FMA, -1, 16, 2 }
    FMA3("maddsub", 6, V | K_L1 | K_W | K_BOTHF), FMA3("msubadd", 7, V | K_L1 | K_W | K_BOTHF), FMA3("madd p", 8, V | K_L1 | K_W), FMA3("madd s", 9, V | K_W),
    FMA3("msub p", 0xA, V | K_L1 | K_W), FMA3("msub s", 0xB, V | K_W), FMA3("nmadd p", 0xC, V | K_L1 | K_W), FMA3("nmadd s", 0xD, V | K_W),
    FMA3("nmsub p", 0xE, V | K_L1 | K_W), FMA3("nmsub s", 0xF, V | K_W),
#undef FMA3
    // AVX2
    { "vpbroadcastb", 0x66, 0x78, VN | K_L1, -1, 1, 2 }, { "vpbroadcastw", 0x66, 0x79, VN | K_L1, -1, 2, 2 },
    { "vpbroadcastd", 0x66, 0x58, VN | K_L1, -1, 4, 2 }, { "vpbroadcastq", 0x66, 0x59, VN | K_L1, -1, 8, 2 },
    { "vbroadcasti128", 0x66, 0x5A, VN | K_L1ONLY, -2, 16, 2 }, { "vinserti128", 0x66, 0x38, V | K_L1ONLY | K_IMM, -1, 16, 3 },
    { "vextracti128", 0x66, 0x39, VN | K_L1ONLY | K_IMM, -1, 16, 3 }, { "vperm2i128", 0x66, 0x46, V | K_L1ONLY | K_IMM, -1, 32, 3 },
    { "vpermq", 0x66, 0x00, VN | K_L1ONLY | K_IMM | K_W1, -1, 32, 3 }, { "vpermpd", 0x66, 0x01, VN | K_L1ONLY | K_IMM | K_W1, -1, 32, 3 },
    { "vpermd", 0x66, 0x36, V | K_L1ONLY, -1, 32, 2 }, { "vpermps", 0x66, 0x16, V | K_L1ONLY, -1, 32, 2 },
    { "vpsrlvd/q", 0x66, 0x45, V | K_L1 | K_W | K_SHCNT | K_CNT32, -1, 16, 2 }, { "vpsravd", 0x66, 0x46, V | K_L1 | K_SHCNT, -1, 16, 2 },
    { "vpsllvd/q", 0x66, 0x47, V | K_L1 | K_W | K_SHCNT | K_CNT32, -1, 16, 2 },
    { "vpmaskmovd/q", 0x66, 0x8C, V | K_L1 | K_W, -2, 16, 2 }, { "vpmaskmovd/q st", 0x66, 0x8E, V | K_L1 | K_W, -2, 16, 2 },
    // x87
    { "fadd m32", 0, 0xD8, K_X87 | K_MEMONLY, 0, 4, 0x00 },
    { "fmul m32", 0, 0xD8, K_X87 | K_MEMONLY, 1, 4, 0x00 },
    { "fcom m32", 0, 0xD8, K_X87 | K_MEMONLY, 2, 4, 0x00 },
    { "fcomp m32", 0, 0xD8, K_X87 | K_MEMONLY, 3, 4, 0x00 },
    { "fsub m32", 0, 0xD8, K_X87 | K_MEMONLY, 4, 4, 0x00 },
    { "fsubr m32", 0, 0xD8, K_X87 | K_MEMONLY, 5, 4, 0x00 },
    { "fdiv m32", 0, 0xD8, K_X87 | K_MEMONLY, 6, 4, 0x00 },
    { "fdivr m32", 0, 0xD8, K_X87 | K_MEMONLY, 7, 4, 0x00 },
    { "fadd m64", 0, 0xDC, K_X87 | K_MEMONLY, 0, 8, 0x00 },
    { "fmul m64", 0, 0xDC, K_X87 | K_MEMONLY, 1, 8, 0x00 },
    { "fcom m64", 0, 0xDC, K_X87 | K_MEMONLY, 2, 8, 0x00 },
    { "fcomp m64", 0, 0xDC, K_X87 | K_MEMONLY, 3, 8, 0x00 },
    { "fsub m64", 0, 0xDC, K_X87 | K_MEMONLY, 4, 8, 0x00 },
    { "fsubr m64", 0, 0xDC, K_X87 | K_MEMONLY, 5, 8, 0x00 },
    { "fdiv m64", 0, 0xDC, K_X87 | K_MEMONLY, 6, 8, 0x00 },
    { "fdivr m64", 0, 0xDC, K_X87 | K_MEMONLY, 7, 8, 0x00 },
    { "fiadd m32int", 0, 0xDA, K_X87 | K_MEMONLY, 0, 4, 0x00 },
    { "fimul m32int", 0, 0xDA, K_X87 | K_MEMONLY, 1, 4, 0x00 },
    { "ficom m32int", 0, 0xDA, K_X87 | K_MEMONLY, 2, 4, 0x00 },
    { "ficomp m32int", 0, 0xDA, K_X87 | K_MEMONLY, 3, 4, 0x00 },
    { "fisub m32int", 0, 0xDA, K_X87 | K_MEMONLY, 4, 4, 0x00 },
    { "fisubr m32int", 0, 0xDA, K_X87 | K_MEMONLY, 5, 4, 0x00 },
    { "fidiv m32int", 0, 0xDA, K_X87 | K_MEMONLY, 6, 4, 0x00 },
    { "fidivr m32int", 0, 0xDA, K_X87 | K_MEMONLY, 7, 4, 0x00 },
    { "fiadd m16int", 0, 0xDE, K_X87 | K_MEMONLY, 0, 2, 0x00 },
    { "fimul m16int", 0, 0xDE, K_X87 | K_MEMONLY, 1, 2, 0x00 },
    { "ficom m16int", 0, 0xDE, K_X87 | K_MEMONLY, 2, 2, 0x00 },
    { "ficomp m16int", 0, 0xDE, K_X87 | K_MEMONLY, 3, 2, 0x00 },
    { "fisub m16int", 0, 0xDE, K_X87 | K_MEMONLY, 4, 2, 0x00 },
    { "fisubr m16int", 0, 0xDE, K_X87 | K_MEMONLY, 5, 2, 0x00 },
    { "fidiv m16int", 0, 0xDE, K_X87 | K_MEMONLY, 6, 2, 0x00 },
    { "fidivr m16int", 0, 0xDE, K_X87 | K_MEMONLY, 7, 2, 0x00 },
    { "fld m32", 0, 0xD9, K_X87 | K_MEMONLY, 0, 4, 0x00 },
    { "fst m32", 0, 0xD9, K_X87 | K_MEMONLY, 2, 4, 0x00 },
    { "fstp m32", 0, 0xD9, K_X87 | K_MEMONLY, 3, 4, 0x00 },
    { "fldenv", 0, 0xD9, K_X87 | K_MEMONLY, 4, 28, 0x00 },
    { "fldcw", 0, 0xD9, K_X87 | K_MEMONLY, 5, 2, 0x00 },
    { "fnstenv", 0, 0xD9, K_X87 | K_MEMONLY, 6, 28, 0x00 },
    { "fnstcw", 0, 0xD9, K_X87 | K_MEMONLY, 7, 2, 0x00 },
    { "fild m32", 0, 0xDB, K_X87 | K_MEMONLY, 0, 4, 0x00 },
    { "fisttp m32", 0, 0xDB, K_X87 | K_MEMONLY, 1, 4, 0x00 },
    { "fist m32", 0, 0xDB, K_X87 | K_MEMONLY, 2, 4, 0x00 },
    { "fistp m32", 0, 0xDB, K_X87 | K_MEMONLY, 3, 4, 0x00 },
    { "fld m80", 0, 0xDB, K_X87 | K_MEMONLY, 5, 10, 0x00 },
    { "fstp m80", 0, 0xDB, K_X87 | K_MEMONLY, 7, 10, 0x00 },
    { "fld m64", 0, 0xDD, K_X87 | K_MEMONLY, 0, 8, 0x00 },
    { "fisttp m64", 0, 0xDD, K_X87 | K_MEMONLY, 1, 8, 0x00 },
    { "fst m64", 0, 0xDD, K_X87 | K_MEMONLY, 2, 8, 0x00 },
    { "fstp m64", 0, 0xDD, K_X87 | K_MEMONLY, 3, 8, 0x00 },
    { "frstor", 0, 0xDD, K_X87 | K_MEMONLY, 4, 108, 0x00 },
    { "fnsave", 0, 0xDD, K_X87 | K_MEMONLY, 6, 108, 0x00 },
    { "fnstsw m16", 0, 0xDD, K_X87 | K_MEMONLY, 7, 2, 0x00 },
    { "fild m16", 0, 0xDF, K_X87 | K_MEMONLY, 0, 2, 0x00 },
    { "fisttp m16", 0, 0xDF, K_X87 | K_MEMONLY, 1, 2, 0x00 },
    { "fist m16", 0, 0xDF, K_X87 | K_MEMONLY, 2, 2, 0x00 },
    { "fistp m16", 0, 0xDF, K_X87 | K_MEMONLY, 3, 2, 0x00 },
    { "fbld", 0, 0xDF, K_X87 | K_MEMONLY, 4, 10, 0x00 },
    { "fild m64", 0, 0xDF, K_X87 | K_MEMONLY, 5, 8, 0x00 },
    { "fbstp", 0, 0xDF, K_X87 | K_MEMONLY, 6, 10, 0x00 },
    { "fistp m64", 0, 0xDF, K_X87 | K_MEMONLY, 7, 8, 0x00 },
    { "fadd st, st(i)", 0, 0xD8, K_X87 | K_REGONLY, 0, 0, 0x00 },
    { "fmul st, st(i)", 0, 0xD8, K_X87 | K_REGONLY, 1, 0, 0x00 },
    { "fcom st(i)", 0, 0xD8, K_X87 | K_REGONLY, 2, 0, 0x00 },
    { "fcomp st(i)", 0, 0xD8, K_X87 | K_REGONLY, 3, 0, 0x00 },
    { "fsub st, st(i)", 0, 0xD8, K_X87 | K_REGONLY, 4, 0, 0x00 },
    { "fsubr st, st(i)", 0, 0xD8, K_X87 | K_REGONLY, 5, 0, 0x00 },
    { "fdiv st, st(i)", 0, 0xD8, K_X87 | K_REGONLY, 6, 0, 0x00 },
    { "fdivr st, st(i)", 0, 0xD8, K_X87 | K_REGONLY, 7, 0, 0x00 },
    { "fadd st(i), st", 0, 0xDC, K_X87 | K_REGONLY, 0, 0, 0x00 },
    { "fmul st(i), st", 0, 0xDC, K_X87 | K_REGONLY, 1, 0, 0x00 },
    { "fcom2", 0, 0xDC, K_X87 | K_REGONLY | K_ALIAS, 2, 0, 0x00 },
    { "fcomp3", 0, 0xDC, K_X87 | K_REGONLY | K_ALIAS, 3, 0, 0x00 },
    { "fsubr st(i), st", 0, 0xDC, K_X87 | K_REGONLY, 4, 0, 0x00 },
    { "fsub st(i), st", 0, 0xDC, K_X87 | K_REGONLY, 5, 0, 0x00 },
    { "fdivr st(i), st", 0, 0xDC, K_X87 | K_REGONLY, 6, 0, 0x00 },
    { "fdiv st(i), st", 0, 0xDC, K_X87 | K_REGONLY, 7, 0, 0x00 },
    { "fld st(i)", 0, 0xD9, K_X87 | K_REGONLY, 0, 0, 0x00 },
    { "fxch", 0, 0xD9, K_X87 | K_REGONLY, 1, 0, 0x00 },
    { "fstp1", 0, 0xD9, K_X87 | K_REGONLY | K_ALIAS, 3, 0, 0x00 },
    { "fnop", 0, 0xD9, K_X87 | K_REGONLY, 2, 0, 0xD0 },
    { "fchs", 0, 0xD9, K_X87 | K_REGONLY, 4, 0, 0xE0 },
    { "fabs", 0, 0xD9, K_X87 | K_REGONLY, 4, 0, 0xE1 },
    { "ftst", 0, 0xD9, K_X87 | K_REGONLY, 4, 0, 0xE4 },
    { "fxam", 0, 0xD9, K_X87 | K_REGONLY, 4, 0, 0xE5 },
    { "fld1", 0, 0xD9, K_X87 | K_REGONLY, 5, 0, 0xE8 },
    { "fldl2t", 0, 0xD9, K_X87 | K_REGONLY, 5, 0, 0xE9 },
    { "fldl2e", 0, 0xD9, K_X87 | K_REGONLY, 5, 0, 0xEA },
    { "fldpi", 0, 0xD9, K_X87 | K_REGONLY, 5, 0, 0xEB },
    { "fldlg2", 0, 0xD9, K_X87 | K_REGONLY, 5, 0, 0xEC },
    { "fldln2", 0, 0xD9, K_X87 | K_REGONLY, 5, 0, 0xED },
    { "fldz", 0, 0xD9, K_X87 | K_REGONLY, 5, 0, 0xEE },
    { "fxtract", 0, 0xD9, K_X87 | K_REGONLY, 6, 0, 0xF4 },
    { "fprem1", 0, 0xD9, K_X87 | K_REGONLY, 6, 0, 0xF5 },
    { "fdecstp", 0, 0xD9, K_X87 | K_REGONLY, 6, 0, 0xF6 },
    { "fincstp", 0, 0xD9, K_X87 | K_REGONLY, 6, 0, 0xF7 },
    { "fprem", 0, 0xD9, K_X87 | K_REGONLY, 7, 0, 0xF8 },
    { "fsqrt", 0, 0xD9, K_X87 | K_REGONLY, 7, 0, 0xFA },
    { "frndint", 0, 0xD9, K_X87 | K_REGONLY, 7, 0, 0xFC },
    { "fscale", 0, 0xD9, K_X87 | K_REGONLY, 7, 0, 0xFD },
    { "fcmovb", 0, 0xDA, K_X87 | K_REGONLY, 0, 0, 0x00 },
    { "fcmove", 0, 0xDA, K_X87 | K_REGONLY, 1, 0, 0x00 },
    { "fcmovbe", 0, 0xDA, K_X87 | K_REGONLY, 2, 0, 0x00 },
    { "fcmovu", 0, 0xDA, K_X87 | K_REGONLY, 3, 0, 0x00 },
    { "fucompp", 0, 0xDA, K_X87 | K_REGONLY, 5, 0, 0xE9 },
    { "fcmovnb", 0, 0xDB, K_X87 | K_REGONLY, 0, 0, 0x00 },
    { "fcmovne", 0, 0xDB, K_X87 | K_REGONLY, 1, 0, 0x00 },
    { "fcmovnbe", 0, 0xDB, K_X87 | K_REGONLY, 2, 0, 0x00 },
    { "fcmovnu", 0, 0xDB, K_X87 | K_REGONLY, 3, 0, 0x00 },
    { "fnclex", 0, 0xDB, K_X87 | K_REGONLY, 4, 0, 0xE2 },
    { "fninit", 0, 0xDB, K_X87 | K_REGONLY, 4, 0, 0xE3 },
    { "fucomi", 0, 0xDB, K_X87 | K_REGONLY, 5, 0, 0x00 },
    { "fcomi", 0, 0xDB, K_X87 | K_REGONLY, 6, 0, 0x00 },
    { "ffree", 0, 0xDD, K_X87 | K_REGONLY, 0, 0, 0x00 },
    { "fxch4", 0, 0xDD, K_X87 | K_REGONLY | K_ALIAS, 1, 0, 0x00 },
    { "fst st(i)", 0, 0xDD, K_X87 | K_REGONLY, 2, 0, 0x00 },
    { "fstp st(i)", 0, 0xDD, K_X87 | K_REGONLY, 3, 0, 0x00 },
    { "fucom", 0, 0xDD, K_X87 | K_REGONLY, 4, 0, 0x00 },
    { "fucomp", 0, 0xDD, K_X87 | K_REGONLY, 5, 0, 0x00 },
    { "faddp", 0, 0xDE, K_X87 | K_REGONLY, 0, 0, 0x00 },
    { "fmulp", 0, 0xDE, K_X87 | K_REGONLY, 1, 0, 0x00 },
    { "fcomp5", 0, 0xDE, K_X87 | K_REGONLY | K_ALIAS, 2, 0, 0x00 },
    { "fsubrp", 0, 0xDE, K_X87 | K_REGONLY, 4, 0, 0x00 },
    { "fsubp", 0, 0xDE, K_X87 | K_REGONLY, 5, 0, 0x00 },
    { "fdivrp", 0, 0xDE, K_X87 | K_REGONLY, 6, 0, 0x00 },
    { "fdivp", 0, 0xDE, K_X87 | K_REGONLY, 7, 0, 0x00 },
    { "fcompp", 0, 0xDE, K_X87 | K_REGONLY, 3, 0, 0xD9 },
    { "ffreep", 0, 0xDF, K_X87 | K_REGONLY | K_ALIAS, 0, 0, 0x00 },
    { "fxch7", 0, 0xDF, K_X87 | K_REGONLY | K_ALIAS, 1, 0, 0x00 },
    { "fstp8", 0, 0xDF, K_X87 | K_REGONLY | K_ALIAS, 2, 0, 0x00 },
    { "fstp9", 0, 0xDF, K_X87 | K_REGONLY | K_ALIAS, 3, 0, 0x00 },
    { "fucomip", 0, 0xDF, K_X87 | K_REGONLY, 5, 0, 0x00 },
    { "fcomip", 0, 0xDF, K_X87 | K_REGONLY, 6, 0, 0x00 },
    { "fnstsw ax", 0, 0xDF, K_X87 | K_REGONLY, 4, 0, 0xE0 },
    { "fsin", 0, 0xD9, K_X87 | K_REGONLY | K_TRANS, 7, 0, 0xFE }, { "fcos", 0, 0xD9, K_X87 | K_REGONLY | K_TRANS, 7, 0, 0xFF },
    { "fptan", 0, 0xD9, K_X87 | K_REGONLY | K_TRANS, 6, 0, 0xF2 }, { "fsincos", 0, 0xD9, K_X87 | K_REGONLY | K_TRANS, 7, 0, 0xFB },
    { "fpatan", 0, 0xD9, K_X87 | K_REGONLY | K_TRANS, 6, 0, 0xF3 }, { "f2xm1", 0, 0xD9, K_X87 | K_REGONLY | K_TRANS, 6, 0, 0xF0 },
    { "fyl2x", 0, 0xD9, K_X87 | K_REGONLY | K_TRANS, 6, 0, 0xF1 }, { "fyl2xp1", 0, 0xD9, K_X87 | K_REGONLY | K_TRANS, 7, 0, 0xF9 },
    { "fxsave", 0, 0xAE, K_MEMONLY | K_ALIGN, 0, 512 }, { "fxrstor", 0, 0xAE, K_MEMONLY | K_ALIGN, 1, 512 },
#undef V
#undef VN
};

// x87 registers that hold values (the others' contents mean nothing: Rosetta 2 leaves the indefinite NaN in a popped one).
static int x87_regs_equal(const dt_state *w, const dt_state *g) {
    int t = w->fsw >> 11 & 7;
    for (int i = 0; i < 8; i++) if ((w->ftag >> ((t + i) & 7) & 1) && memcmp(w->st[i], g->st[i], 10)) return 0;
    return 1;
}
// K_TRANS: registers in use that differ by one unit in the last place (both finite, same sign) are taken as equal, and
// then C1 (rounded up or not) too. 1 if any did. *far: one differs by more (FYL2XP1: Rosetta 2 loses precision there).
static int ulp_apart(const uint8_t *a, const uint8_t *b, int n) {     // within n units in the last place
    uint64_t ma, mb; uint16_t ea, eb; memcpy(&ma, a, 8); memcpy(&ea, a + 8, 2); memcpy(&mb, b, 8); memcpy(&eb, b + 8, 2);
    if ((ea ^ eb) & 0x8000 || (ea & 0x7FFF) == 0x7FFF || (eb & 0x7FFF) == 0x7FFF) return 0;
    // as 79-bit integers (exponent, then the significand without its J bit): adjacent values differ by 1
    unsigned __int128 ia = (unsigned __int128)(ea & 0x7FFF) << 63 | (ma & ~(1ull << 63)), ib = (unsigned __int128)(eb & 0x7FFF) << 63 | (mb & ~(1ull << 63));
    if ((ea & 0x7FFF) && !(ma >> 63)) return 0;                                // (unnormals: no)
    if ((eb & 0x7FFF) && !(mb >> 63)) return 0;
    return ia - ib <= (unsigned)n || ib - ia <= (unsigned)n;
}
static int trans_close(dt_result *w, const dt_result *g, int *far, int directed) {   // (directed rounding: Rosetta 2 is off by up to 2)
    int t = w->out.fsw >> 11 & 7, any = 0;
    *far = 0;
    if ((w->out.fsw ^ g->out.fsw) & ~0x200 || w->out.ftag != g->out.ftag) return 0;
    for (int i = 0; i < 8; i++) {                                           // (first look: patch only if all are close)
        if (!(w->out.ftag >> ((t + i) & 7) & 1) || !memcmp(w->out.st[i], g->out.st[i], 10)) continue;
        if (!ulp_apart(w->out.st[i], g->out.st[i], directed ? 2 : 1)) { *far = 1; return 0; }
        any = 1;
    }
    for (int i = 0; i < 8; i++) memcpy(w->out.st[i], g->out.st[i], 10);
    w->out.fsw = g->out.fsw;                                                // (C1: which way each rounded, Rosetta's or ours)
    return any;
}

// Rosetta 2 keeps C1 where the SDM says it is cleared (loads): 1 if that is the only difference (then ours is taken).
static int c1_kept(const dt_case *c, dt_result *w, const dt_result *g) {
    if (((w->out.fsw ^ g->out.fsw) != 0x200) || ((w->out.fsw ^ c->in.fsw) & 0x200)) return 0;
    w->out.fsw = g->out.fsw;
    return 1;
}

// What x86 does, for the instructions Rosetta 2 lacks (ADCX, ADOX): into want.
static void model(const sse_form *f, const dt_case *c, const x86_insn *d, dt_result *want) {
    memcpy(&want->out, &c->in, sizeof c->in);
    memcpy(want->mem, c->mem, DT_BUF_SIZE);
    int sz = d->rex & 8 ? 8 : 4, bit = f->pf == 0x66 ? 0 : 11;
    uint64_t a = c->in.gpr[d->reg], bv;
    if (d->mod == 3) bv = c->in.gpr[d->rm];
    else { uint64_t at = c->in.gpr[d->base] + (uint64_t)(int64_t)d->disp - DT_BUF; bv = 0; memcpy(&bv, c->mem + at, (size_t)sz); }
    uint64_t cin = c->in.flags >> bit & 1;
    unsigned __int128 sum;
    if (sz == 4) sum = (unsigned __int128)(uint32_t)a + (uint32_t)bv + cin; else sum = (unsigned __int128)a + bv + cin;
    want->out.gpr[d->reg] = sz == 4 ? (uint32_t)sum : (uint64_t)sum;
    int carry = (int)(sz == 4 ? (uint64_t)(sum >> 32) & 1 : (uint64_t)(sum >> 64) & 1);
    want->out.flags = (c->in.flags & ~(1ull << bit)) | (uint64_t)carry << bit;
    want->out.fault = 0;
}

// The r/m operand's 32 bytes (the upper 16 from YMM, or memory past the first 16).
static void rm_bytes(const dt_case *c, const x86_insn *d, uint8_t out[32]) {
    if (d->mod == 3) { memcpy(out, c->in.xmm[d->rm], 16); memcpy(out + 16, c->in.ymm_hi[d->rm], 16); }
    else memcpy(out, c->mem + (c->in.gpr[d->base] + (uint64_t)(int64_t)d->disp - DT_BUF), 32);
}
// K_CNT32: a 64-bit count (VEX.W) that is below 64 in its low 32 bits but not in all 64.
static int cnt32_case(const dt_case *c, const x86_insn *d) {
    if (!d->vex_w) return 0;
    uint8_t src[32]; rm_bytes(c, d, src);
    for (int i = 0; i < (d->vex_l ? 4 : 2); i++) { uint64_t v; memcpy(&v, src + 8 * i, 8); if ((v >> 32) && (uint32_t)v < 64) return 1; }
    return 0;
}

// K_NANSIGN: the destination's lanes differ only in the sign of NaNs (Rosetta's then taken as ours). 1 if any did.
static int nan_signs_only(const sse_form *f, const x86_insn *d, dt_result *want, const dt_result *got) {
    int dbl = f->pf == 0x66, es = dbl ? 8 : 4, any = 0;
    for (int h = 0; h < 2; h++) {
        uint8_t *wp = h ? want->out.ymm_hi[d->reg] : want->out.xmm[d->reg];
        const uint8_t *gp = h ? got->out.ymm_hi[d->reg] : got->out.xmm[d->reg];
        for (int i = 0; i < 16; i += es) {
            uint64_t wv = 0, gv = 0, sign = dbl ? 1ull << 63 : 1ull << 31, abs_max = dbl ? 0x7FF0000000000000ull : 0x7F800000ull;
            memcpy(&wv, wp + i, (size_t)es); memcpy(&gv, gp + i, (size_t)es);
            if (wv == gv) continue;
            if ((wv ^ gv) != sign || (wv & ~sign) <= abs_max) return 0;
            any = 1;
        }
    }
    if (any) { memcpy(want->out.xmm[d->reg], got->out.xmm[d->reg], 16); memcpy(want->out.ymm_hi[d->reg], got->out.ymm_hi[d->reg], 16); }
    return any;
}

// Ours, known: with FTZ, ARM flushes a result that is below the smallest normal number before rounding; x86 decides
// after rounding. A result that rounds up to exactly +-2^-126 (2^-1022) is +-0 here. 1 if the case differs only so.
static long ftz_boundary, ue_tiny, fma_ie, trans_ulp, trans_undef, modeled;
// Rosetta 2's deviations from x86, by kind (DT_DEVIATIONS=1 prints them)
enum { DV_LOCKNEG, DV_TESTF, DV_NANSIGN, DV_CNT32, DV_BOTHF, DV_ALIAS, DV_FBLD, DV_C1, DV_TAGS, DV_YL2XP1, DV_F2XM1PE, DV_ATANPE, DV_SQRT, DV_PREMDEN, DV_PH, DV_N };
static const char *dv_name[DV_N] = { "16-bit LOCK NEG across 16 bytes: no store", "PTEST/VTEST keep AF OF PF SF",
    "HSUB/ADDSUB: subtracted NaN's sign", "VPSRLVQ/VPSLLVQ: low 32 bits of the count", "FMADDSUB/FMSUBADD: both operations' flags",
    "undocumented x87 aliases rejected", "FBLD: no stack overflow check", "x87 loads keep C1", "tags after FXRSTOR: in use = zero",
    "FYL2XP1: imprecise (and its DE, UE, PE, C1)", "F2XM1: no PE (UE) for small arguments",
    "FPATAN: no PE (UE) for tiny ratios", "FSQRT: exact squares inexact when rounding up/down", "FPREM/FPREM1: denormal divisor, exponent difference 63",
    "VCVTPS2PH: imm bits 7-3 swap rounding up and down" };
static long dv[DV_N];   // (ue_tiny: UE alone differs: ARM finds results tiny before rounding, x86 after)
static int ftz_boundary_only(const dt_case *c, dt_result *want, const dt_result *got) {
    if (!(c->in.mxcsr & 0x8040)) return 0;                                    // (FZ: FTZ or DAZ set)
    int any = 0;
    for (int r = 0; r < 32; r++) {
        uint8_t *wp = r < 16 ? want->out.xmm[r] : want->out.ymm_hi[r - 16];
        const uint8_t *gp = r < 16 ? got->out.xmm[r] : got->out.ymm_hi[r - 16];
        if (!memcmp(wp, gp, 16)) continue;
        int ok32 = 1, ok64 = 1;
        for (int i = 0; i < 16; i += 4) { uint32_t w, g; memcpy(&w, wp + i, 4); memcpy(&g, gp + i, 4); if (w != g && !((w & 0x7FFFFFFF) == 0x00800000 && g == (w & 0x80000000))) ok32 = 0; }
        for (int i = 0; i < 16; i += 8) { uint64_t w, g; memcpy(&w, wp + i, 8); memcpy(&g, gp + i, 8); if (w != g && !((w & ~(1ull << 63)) == 0x0010000000000000ull && g == (w & (1ull << 63)))) ok64 = 0; }
        if (!ok32 && !ok64) return 0;
        any = 1;
    }
    if (any) { memcpy(want->out.xmm, got->out.xmm, sizeof want->out.xmm); memcpy(want->out.ymm_hi, got->out.ymm_hi, sizeof want->out.ymm_hi); }
    return any;
}

// Ours, known: one of FTZ and DAZ alone runs as both (ARM's FZ). That can only matter with a denormal source (FTZ
// alone: it is flushed too) or a denormal result (DAZ alone: x86 keeps it). 1 if the case has one: its differences are
// counted as that, not compared.
static long fz_single;
static int denormal_in(const uint8_t *p, int n) {
    for (int i = 0; i + 4 <= n; i += 4) { uint32_t v; memcpy(&v, p + i, 4); if (!(v & 0x7F800000) && (v & 0x7FFFFF)) return 1; }
    for (int i = 0; i + 8 <= n; i += 8) { uint64_t v; memcpy(&v, p + i, 8); if (!(v & 0x7FF0000000000000ull) && (v & 0xFFFFFFFFFFFFFull)) return 1; }
    return 0;
}
static int fz_single_case(const dt_case *c, const x86_insn *d, const dt_result *want, const dt_result *got) {
    uint32_t m = c->in.mxcsr & 0x8040;
    if ((m != 0x8000 && m != 0x0040) || !d->has_modrm || (d->mod != 3 && (d->base == 0xFF || d->index != 0xFF))) return 0;
    if (m == 0x0040) return denormal_in(want->out.xmm[d->reg & 15], 16) || denormal_in(want->out.ymm_hi[d->reg & 15], 16) ||
                            (!memcmp(want->out.xmm, got->out.xmm, sizeof want->out.xmm) && !memcmp(want->out.ymm_hi, got->out.ymm_hi, sizeof want->out.ymm_hi) &&
                             !((want->out.mxcsr ^ got->out.mxcsr) & ~0x32u) && (got->out.mxcsr & (want->out.mxcsr ^ got->out.mxcsr)) == (want->out.mxcsr ^ got->out.mxcsr));
                            // (or a partial result flushed: the same values, ours with UE PE more)
    uint8_t src[32];
    if (denormal_in(c->in.xmm[d->reg & 15], 16) || denormal_in(c->in.ymm_hi[d->reg & 15], 16)) return 1;
    if ((d->prefixes & X86_P_VEX) && (denormal_in(c->in.xmm[d->vex_vvvv & 15], 16) || denormal_in(c->in.ymm_hi[d->vex_vvvv & 15], 16))) return 1;
    if (d->mod == 3) return denormal_in(c->in.xmm[d->rm & 15], 16) || denormal_in(c->in.ymm_hi[d->rm & 15], 16);
    rm_bytes(c, d, src);
    return denormal_in(src, 32);
}

// RCP / RSQRT (K_APPROX): x86 leaves the values to the processor within a relative error of 1.5 * 2^-12, with tiny
// results flushed to zero and denormal inputs taken as zero. Each lane of both results (Rosetta's and ours) is checked
// against the exact value; then ours is copied into Rosetta's, so that everything else is still compared exactly.
static int approx_lane(uint32_t xb, uint32_t rb, int rsqrt) {
    if ((xb & 0x7FFFFFFF) > 0x7F800000) return rb == (xb | 0x400000);                 // NaN: quieted
    if (!(xb & 0x7F800000)) xb &= 0x80000000;                                          // denormal: zero
    float x, r; memcpy(&x, &xb, 4); memcpy(&r, &rb, 4);
    if (rsqrt && (xb >> 31) && (xb & 0x7FFFFFFF)) return rb == 0xFFC00000;             // negative: indefinite
    if (!(xb & 0x7FFFFFFF)) return rb == ((xb & 0x80000000) | 0x7F800000);             // zero: infinity
    if ((xb & 0x7FFFFFFF) == 0x7F800000) return rb == (xb & 0x80000000);               // infinity: zero
    double e = rsqrt ? 1.0 / sqrt((double)x) : 1.0 / (double)x, tol = 1.5 / 4096, tiny = 0x1p-126;
    if ((rb >> 31) != (xb >> 31)) return 0;
    if (fabs(e) < tiny * (1 + tol) && !(rb & 0x7FFFFFFF)) return 1;                   // (flushed)
    if (fabs(e) < tiny * (1 - tol)) return !(rb & 0x7FFFFFFF);
    return fabs((double)r - e) <= tol * fabs(e);
}
// 1: both within the bound; 0: ours is not; -1: Rosetta's is not.
static int approx_ok(const sse_form *f, const dt_case *c, const x86_insn *d, dt_result *want, const dt_result *got) {
    int scalar = f->pf == 0xF3, n = scalar ? 1 : 4, L = d->vex_l && !scalar, rsqrt = f->op == 0x52;
    uint8_t src[32] = { 0 };
    if (d->mod == 3) { memcpy(src, c->in.xmm[d->rm], 16); memcpy(src + 16, c->in.ymm_hi[d->rm], 16); }
    else memcpy(src, c->mem + (c->in.gpr[d->base] + (uint64_t)(int64_t)d->disp - DT_BUF), L ? 32 : scalar ? 4 : 16);
    for (int h = 0; h <= L; h++)
        for (int i = 0; i < n; i++) {
            uint8_t *wp = (h ? want->out.ymm_hi[d->reg] : want->out.xmm[d->reg]) + 4 * i;
            const uint8_t *gp = (h ? got->out.ymm_hi[d->reg] : got->out.xmm[d->reg]) + 4 * i;
            uint32_t xb, wb, gb; memcpy(&xb, src + 16 * h + 4 * i, 4); memcpy(&wb, wp, 4); memcpy(&gb, gp, 4);
            if (!approx_lane(xb, gb, rsqrt)) return 0;
            if (!approx_lane(xb, wb, rsqrt)) return -1;
            memcpy(wp, gp, 4);
        }
    return 1;
}

// x87 values: zeros, infinities, NaNs, denormals, unnormals now and then, small integers and halves, and normal numbers
// with exponents near 1, near the limits and anywhere.
static void rnd_f80(uint8_t *p) {
    uint64_t m; uint32_t se; int k = (int)(rnd() % 20);
    if (k == 0) { m = 0; se = 0; }
    else if (k == 1) { m = 1ull << 63; se = 0x7FFF; }
    else if (k == 2) { m = 0xC000000000000000ull | (rnd() & 0xFFFF); se = 0x7FFF; }
    else if (k == 3) { m = 0x8000000000000000ull | ((rnd() & 0x3FFFFFFFFFFFFFFFull) | 1); se = 0x7FFF; }
    else if (k == 4) { m = (rnd() >> (rnd() % 64)) & ~(1ull << 63); se = 0; }
    else if (k == 5 && (rnd() & 3) == 0) { m = rnd() & ~(1ull << 63); se = 1 + (uint32_t)(rnd() % 0x7FFE); }   // (unnormal)
    else if (k <= 8) {
        int64_t v = (int64_t)(rnd() % 4001) - 2000;
        if (!v) v = 1;
        uint64_t a = v < 0 ? (uint64_t)-v : (uint64_t)v; int lz = __builtin_clzll(a);
        m = a << lz; se = (uint32_t)(16383 + 63 - lz - (k == 8)) | (v < 0 ? 0x8000u : 0);   // (k 8: halves)
        memcpy(p, &m, 8); uint16_t s16 = (uint16_t)se; memcpy(p + 8, &s16, 2); return;
    } else {
        m = rnd() | 1ull << 63;
        int r = (int)(rnd() % 4);
        se = r == 0 ? 16383 - 70 + (uint32_t)(rnd() % 140) : r == 1 ? 1 + (uint32_t)(rnd() % 80) : r == 2 ? 0x7FFE - (uint32_t)(rnd() % 80) : 1 + (uint32_t)(rnd() % 0x7FFE);
        if (rnd() % 4 == 0) m &= ~0ull << (rnd() % 64);                    // (short significands)
    }
    se |= (rnd() & 1) ? 0x8000u : 0;
    memcpy(p, &m, 8); uint16_t s16 = (uint16_t)se; memcpy(p + 8, &s16, 2);
}
static uint16_t rnd_fcw(void) {                                         // all exceptions masked; PC and RC anything
    static const int pcs[8] = { 0, 2, 3, 3, 3, 2, 0, 1 };
    return (uint16_t)(0x0040 | 0x3F | pcs[rnd() % 8] << 8 | (rnd() % 4) << 10);
}
static void rnd_x87(dt_state *st) {
    st->fcw = rnd_fcw();
    int top = (int)(rnd() % 8);
    st->fsw = (uint16_t)((rnd() & 0x4700) | (rnd() % 3 == 0 ? rnd() & 0x3F : 0) | top << 11);
    int t = (int)(rnd() % 8);
    st->ftag = t == 0 ? 0 : t == 1 ? 0xFF : (uint8_t)(rnd() | rnd());
    for (int i = 0; i < 8; i++) rnd_f80(st->st[i]);
}
// The memory operand of an x87 form (size bytes at p), as its instruction reads it.
static void x87_operand(const sse_form *f, uint8_t *p) {
    int op = f->op, g = f->group;
    if (op == 0xD8 || (op == 0xD9 && g == 0)) { uint32_t v = rnd_f32(); memcpy(p, &v, 4); }
    else if (op == 0xDC || (op == 0xDD && g == 0)) { uint64_t v = rnd_f64(); memcpy(p, &v, 8); }
    else if ((op == 0xDB && g == 5)) rnd_f80(p);
    else if (op == 0xDA || op == 0xDE || (op == 0xDB && g == 0) || (op == 0xDF && (g == 0 || g == 5))) { uint64_t v = rnd_value(); memcpy(p, &v, 8); }
    else if (op == 0xDF && g == 4) {                                       // BCD: 18 digits and a sign
        for (int i = 0; i < 9; i++) p[i] = (uint8_t)(rnd() % 10 | (rnd() % 10) << 4);
        p[9] = (rnd() & 1) ? 0x80 : 0;
    } else if (op == 0xD9 && g == 5) { uint16_t v = rnd_fcw(); memcpy(p, &v, 2); }
    else if ((op == 0xD9 && g == 4) || (op == 0xDD && g == 4)) {           // FLDENV / FRSTOR: an environment
        dt_state t; rnd_x87(&t);
        uint32_t env[7] = { t.fcw | 0xFFFF0000u, t.fsw | 0xFFFF0000u, 0, 0, 0, 0, 0xFFFF0000u };
        for (int r = 0; r < 8; r++) env[2] |= (t.ftag >> r & 1 ? (uint32_t)(rnd() % 3) : 3u) << (2 * r);
        env[2] |= 0xFFFF0000u;
        memcpy(p, env, 28);
        if (g == 4 && op == 0xDD) for (int i = 0; i < 8; i++) memcpy(p + 28 + 10 * i, t.st[i], 10);
    }
}
static void make_sse_case(const sse_form *f, dt_case *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 16; i++) { c->in.gpr[i] = rnd_value(); rnd_xmm(c->in.xmm[i]); rnd_xmm(c->in.ymm_hi[i]); }
    for (int i = 0; i < DT_BUF_SIZE; i += 16) rnd_xmm(c->mem + i);
    c->in.gpr[4] = DT_STACK;
    c->in.flags = (rnd() & 0x8D5) | 0x202;
    // MXCSR: mostly the default, also the other rounding modes, FTZ+DAZ, and flags already set
    static const uint32_t modes[] = { 0x1F80, 0x1F80, 0x1F80, 0x3F80, 0x5F80, 0x7F80, 0x9FC0, 0xDFC0, 0x9F80, 0x1FC0 };
    c->in.mxcsr = modes[rnd() % 10] | (rnd() % 4 == 0 ? (uint32_t)(rnd() & 0x3F) : 0);
    if (getenv("DT_MXCSR")) c->in.mxcsr = (uint32_t)strtoul(getenv("DT_MXCSR"), NULL, 0);   // (one mode only)
    rnd_x87(&c->in);
    if (f->kind & K_RAW) {                                                  // (fixed bytes)
        for (int i = 0; i < f->group; i++) c->code[i] = (uint8_t)(f->map >> (8 * i));
        c->len = (uint32_t)f->group;
        if (f->kind & K_CPUID) {
            static const uint32_t leaves[] = { 0, 1, 2, 4, 6, 7, 0xB, 0xD, 0x80000000u, 0x80000001u, 0x80000002u, 0x80000003u, 0x80000004u, 0x80000006u, 0x80000008u };
            c->in.gpr[0] = rnd() % 8 ? leaves[rnd() % 15] : (uint32_t)rnd() % 0x20;
            c->in.gpr[1] = rnd() % 4;
        }
        if (c->code[1] == 0x01 && c->code[2] == 0xD0 && rnd() % 4) c->in.gpr[1] = 0;   // (XGETBV: mostly XCR0)
        return;
    }
    if (f->kind & K_STR) {                                                  // (strings: a few letters, zeros, small lengths)
        static const uint8_t letters[8] = { 0, 'a', 'b', 'c', 'd', 0x80, 0xFF, 'a' };
        for (int i = 0; i < 16; i++) for (int k = 0; k < 16; k++) c->in.xmm[i][k] = rnd() % 4 ? letters[1 + rnd() % 4] : letters[rnd() % 8];
        for (int i = 0; i < DT_BUF_SIZE; i++) c->mem[i] = rnd() % 4 ? letters[1 + rnd() % 4] : letters[rnd() % 8];
        if (rnd() & 1) { c->in.gpr[0] = (uint64_t)(int64_t)((int)(rnd() % 41) - 20); c->in.gpr[2] = (uint64_t)(int64_t)((int)(rnd() % 41) - 20); }
    }
    if (f->kind & K_X87) {                                                  // [REX] op ModRM [disp8]
        uint8_t *q = c->code;
        int base = 0, mem = (f->kind & K_MEMONLY) != 0;
        if (mem) { do base = (int)(rnd() % 16); while (base == 4 || base == 5 || base == 12 || base == 13); }
        if (base >= 8 || (rnd() % 4 == 0)) *q++ = (uint8_t)(0x40 | (base >> 3));
        *q++ = f->op;
        if (!mem) *q++ = f->map ? (uint8_t)f->map : (uint8_t)(0xC0 | f->group << 3 | (int)(rnd() % 8));
        else {
            int disp = (int)(rnd() % 64) - 32;
            *q++ = (uint8_t)(0x40 | f->group << 3 | (base & 7)); *q++ = (uint8_t)disp;
            uint64_t a = DT_BUF + 1024 + rnd() % 1024;
            c->in.gpr[base] = a - (uint64_t)(int64_t)disp;
            x87_operand(f, c->mem + (a - DT_BUF));
        }
        c->len = (uint32_t)(q - c->code);
        return;
    }
    uint8_t *p = c->code;
    int vex = f->kind & K_VEX;
    if (f->pf && !vex) *p++ = f->pf;
    int w = ((f->kind & K_W) && (rnd() & 1)) || (f->kind & K_W1);
    int reg = (int)(rnd() % 16), rm = (int)(rnd() % 16);
    if (f->kind & K_GREG) while (reg == 4) reg = (int)(rnd() % 16);
    if (f->group >= 0) reg = f->group;
    int mem = (f->kind & K_MEMONLY) || (!(f->kind & K_REGONLY) && (f->group == -2 || rnd() % 3 == 0));
    if (mem && f->op == 0x12 && !f->pf) mem = rnd() & 1;                      // (MOVLPS or MOVHLPS)
    if (!mem && (f->kind & K_GRM)) while (rm == 4) rm = (int)(rnd() % 16);
    int base = 0;
    if (mem) { do base = (int)(rnd() % 16); while (base == 4 || base == 5 || base == 12 || base == 13 || ((f->kind & K_GREG) && base == reg)); rm = base; }
    int L = (f->kind & K_L1ONLY) || ((f->kind & K_L1) && (rnd() & 1));
    if (vex) {
        // C5 (two bytes) when it can say it: map 0F, W0, no REX.X/B; else C4
        int v = (f->kind & K_NOV) || ((f->kind & K_VREG) && mem) ? 0 : (int)(rnd() % 16);
        if (f->kind & K_VGPR) while (v == 4) v = (int)(rnd() % 16);
        if (f->kind & K_GATHER) {                                         // dst, mask (vvvv), index all different
            mem = 1;
            do v = (int)(rnd() % 16); while (v == reg);
        }
        int pp = f->pf == 0x66 ? 1 : f->pf == 0xF3 ? 2 : f->pf == 0xF2 ? 3 : 0, map = f->map ? f->map : 1;
        int R = reg >> 3 & 1, B = rm >> 3 & 1;
        if (map == 1 && !w && !B && (rnd() & 1)) { *p++ = 0xC5; *p++ = (uint8_t)(!R << 7 | (~v & 15) << 3 | L << 2 | pp); }
        else { *p++ = 0xC4; *p++ = (uint8_t)(!R << 7 | 1 << 6 | !B << 5 | map); *p++ = (uint8_t)(w << 7 | (~v & 15) << 3 | L << 2 | pp); }
        *p++ = f->op;
        if (f->kind & K_NOMODRM) { c->len = (uint32_t)(p - c->code); return; }
        if (f->kind & K_GATHER) {                                         // VSIB: [base + index lanes * scale + disp8]
            int ix; do ix = (int)(rnd() % 16); while (ix == reg || ix == v);
            int bs; do bs = (int)(rnd() % 16); while (bs == 4 || bs == 5 || bs == 12 || bs == 13);
            int sc = (int)(rnd() % 4), disp = (int)(rnd() % 64) - 32;
            uint8_t *v1 = c->code + 1;                                    // (C4 form: R X B mmmmm)
            if (c->code[0] == 0xC5) return make_sse_case(f, c);           // (start over: gathers need the 3-byte form)
            *v1 = (uint8_t)(!(reg >> 3 & 1) << 7 | !(ix >> 3) << 6 | !(bs >> 3) << 5 | f->map);
            *p++ = (uint8_t)(0x44 | (reg & 7) << 3);                      // mod 01, rm 100 (SIB)
            *p++ = (uint8_t)(sc << 6 | (ix & 7) << 3 | (bs & 7));
            *p++ = (uint8_t)disp;
            uint64_t a = DT_BUF + 1024 + (rnd() % 512) & ~7ull;
            c->in.gpr[bs] = a - (uint64_t)(int64_t)disp;
            for (int i = 0; i < 8; i++) {                                 // index lanes: 0..63 (signed: a few negative)
                int32_t iv = (int32_t)(rnd() % 64) - (rnd() % 8 == 0 ? 32 : 0);
                int64_t qv = iv;
                if (f->op & 1) { if (i < 4) { memcpy(i < 2 ? c->in.xmm[ix] + 8 * i : c->in.ymm_hi[ix] + 8 * (i - 2), &qv, 8); } }
                else memcpy(i < 4 ? c->in.xmm[ix] + 4 * i : c->in.ymm_hi[ix] + 4 * (i - 4), &iv, 4);
            }
            c->len = (uint32_t)(p - c->code);
            return;
        }
    } else {
        if ((f->kind & K_LOCKOPT) && (rnd() & 1)) { memmove(c->code + 1, c->code, (size_t)(p - c->code)); c->code[0] = 0xF0; p++; }
        int rex = 0x40 | w << 3 | (reg >> 3 & 1) << 2 | (rm >> 3);
        if (rex != 0x40 || (rnd() & 1)) *p++ = (uint8_t)rex;
        *p++ = 0x0F;
        if (f->map) *p++ = f->map == 2 ? 0x38 : 0x3A;
        *p++ = f->op;
    }
    if (!mem) *p++ = (uint8_t)(0xC0 | (reg & 7) << 3 | (rm & 7));
    else {
        int disp = (int)(rnd() % 64) - 32;
        *p++ = (uint8_t)(0x40 | (reg & 7) << 3 | (rm & 7)); *p++ = (uint8_t)disp;
        uint64_t a = DT_BUF + 1024 + rnd() % 1024;
        if (f->kind & K_ALIGN) a &= L ? ~31ull : ~15ull;
        c->in.gpr[base] = a - (uint64_t)(int64_t)disp;
        if ((f->kind & K_SHCNT) && (rnd() & 1)) for (int i = 0; i < 8; i++) { uint32_t v = (uint32_t)(rnd() % 70); memcpy(c->mem + (a - DT_BUF) + 4 * i, &v, 4); }
    }
    if (!mem && (f->kind & K_SHCNT) && (rnd() & 1))
        for (int i = 0; i < 4; i++) { uint32_t v = (uint32_t)(rnd() % 70); memcpy(c->in.xmm[rm] + 4 * i, &v, 4); memcpy(c->in.ymm_hi[rm] + 4 * i, &v, 4); }
    if (mem && f->op == 0xAE && f->group == 1 && !(f->kind & K_VEX)) {      // (FXRSTOR: a valid image)
        uint8_t *img = c->mem + (c->in.gpr[base] + (uint64_t)(int64_t)(int8_t)p[-1] - DT_BUF);
        dt_state t; rnd_x87(&t);
        memcpy(img, &t.fcw, 2); memcpy(img + 2, &t.fsw, 2); img[4] = t.ftag; img[5] = 0;
        uint32_t mx = 0x1F80 | (uint32_t)(rnd() & 0x3F), mm = 0xFFFF; memcpy(img + 24, &mx, 4); memcpy(img + 28, &mm, 4);
        for (int i = 0; i < 8; i++) { memcpy(img + 32 + 16 * i, t.st[i], 10); memset(img + 42 + 16 * i, 0, 6); }
    }
    if (mem && (f->kind & K_CMPX) && (rnd() & 1)) {                          // (CMPXCHG8B/16B: equal half the time)
        uint8_t *m = c->mem + (c->in.gpr[base] + (uint64_t)(int64_t)(int8_t)p[-1] - DT_BUF);
        if (w) { memcpy(m, &c->in.gpr[0], 8); memcpy(m + 8, &c->in.gpr[2], 8); }
        else { memcpy(m, &c->in.gpr[0], 4); memcpy(m + 4, &c->in.gpr[2], 4); }
    }
    if (mem && f->op == 0xAE && f->group == 2) {                             // (LDMXCSR: a valid value, exceptions masked)
        static const uint32_t modes[] = { 0x1F80, 0x3F80, 0x5F80, 0x7F80, 0x9FC0, 0xDFC0 };
        uint32_t v = modes[rnd() % 6] | (uint32_t)(rnd() & 0x3F);
        memcpy(c->mem + (c->in.gpr[base] + (uint64_t)(int64_t)(int8_t)p[-1] - DT_BUF), &v, 4);
    }
    if (f->kind & K_IMM) *p++ = (uint8_t)(f->group >= 0 && (rnd() & 1) ? rnd() % 20 : rnd());   // (shifts: counts near the width)
    if (f->kind & K_MASK4) *p++ = (uint8_t)rnd();
    c->len = (uint32_t)(p - c->code);
}

static uint32_t *jit_prog;
static xe *engine;                                                         // (programs run on the engine: engine.c)
static int run_program(const dt_case *c, dt_result *r) {
    memcpy((void *)DT_PROG, c->prog, c->prog_len);
    static x86_ctx ctx;
    memset(&ctx, 0, sizeof ctx);
    memcpy(ctx.gpr, c->in.gpr, sizeof ctx.gpr);
    memcpy(ctx.xmm, c->in.xmm, sizeof ctx.xmm); memcpy(ctx.ymm_hi, c->in.ymm_hi, sizeof ctx.ymm_hi);
    ctx.flags = c->in.flags; ctx.mxcsr = c->in.mxcsr;
    ctx.fcw = c->in.fcw; ctx.fsw = c->in.fsw; ctx.ftag = c->in.ftag;
    for (int i = 0; i < 8; i++) memcpy(ctx.st[(ctx.fsw >> 11 & 7) + i & 7], c->in.st[i], 10);
    ctx.rip = DT_PROG;
    memcpy((void *)DT_BUF, c->mem, DT_BUF_SIZE);
    signaled = 0;
    xe_flush(engine);                                                      // (each program is new code at DT_PROG)
    if (!sigsetjmp(jb, 1)) { if (xe_run(engine, &ctx, DT_END) < 0) return -1; }
    else __asm__ volatile("msr fpcr, xzr");                               // (left translated code by a signal)
    memcpy(r->out.gpr, ctx.gpr, sizeof ctx.gpr);
    memcpy(r->out.xmm, ctx.xmm, sizeof ctx.xmm); memcpy(r->out.ymm_hi, ctx.ymm_hi, sizeof ctx.ymm_hi);
    r->out.mxcsr = ctx.mxcsr;
    r->out.fcw = ctx.fcw; r->out.fsw = ctx.fsw; r->out.ftag = ctx.ftag;
    for (int i = 0; i < 8; i++) memcpy(r->out.st[i], ctx.st[(ctx.fsw >> 11 & 7) + i & 7], 10);
    r->out.flags = ctx.flags;
    r->out.fault = signaled ? 2 : ctx.fault == X86_FAULT_DE ? 1 : ctx.rip != DT_END ? 4 : 0;
    if (r->out.fault) { memcpy(r->out.gpr, c->in.gpr, sizeof ctx.gpr); r->out.flags = c->in.flags; }
    memcpy(r->mem, (void *)DT_BUF, DT_BUF_SIZE);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: difftest <ref_x86> [cases per form] [seed]\n"); return 2; }
    setvbuf(stdout, NULL, _IOLBF, 0);                         // (results as they come)
    int per = argc > 2 ? atoi(argv[2]) : 2000;
    const char *only = argc > 4 ? argv[4] : NULL;            // (a part of the forms' names: only those)
    rng_state = argc > 3 ? strtoull(argv[3], NULL, 0) : 0x9E3779B97F4A7C15ull;
    jit = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    void *buf = mmap((void *)DT_BUF, DT_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    void *progmem = mmap((void *)DT_PROG, DT_PROG_MAX + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    jit_prog = mmap(NULL, 4u << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (progmem != (void *)DT_PROG || jit_prog == MAP_FAILED) { fprintf(stderr, "difftest: mmap failed\n"); return 1; }
    if (jit == MAP_FAILED || buf != (void *)DT_BUF) { fprintf(stderr, "difftest: mmap failed\n"); return 1; }
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
    engine = getenv("DT_VCPU") ? xe_new_vcpu() : xe_new(getenv("DT_ORDERED") != NULL);   // (after: its handler passes the rest on)
    if (!engine) { fprintf(stderr, "difftest: no engine (DT_VCPU: the hypervisor entitlement?)\n"); return 1; }

    int to_ref[2], from_ref[2];
    if (pipe(to_ref) || pipe(from_ref)) return 1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to_ref[0], 0);
    posix_spawn_file_actions_adddup2(&fa, from_ref[1], 1);
    posix_spawn_file_actions_addclose(&fa, to_ref[1]);
    posix_spawn_file_actions_addclose(&fa, from_ref[0]);
    pid_t pid;
    char *args[] = { "/usr/bin/arch", "-x86_64", "-e", "ROSETTA_ADVERTISE_AVX=1", argv[1], NULL };   // (its CPUID: ours)
    if (posix_spawn(&pid, "/usr/bin/arch", &fa, NULL, args, environ)) { perror("spawn"); return 1; }
    close(to_ref[0]); close(from_ref[1]);
    FILE *out = fdopen(to_ref[1], "wb"), *in = fdopen(from_ref[0], "rb");

    static dt_case c;
    static dt_result want, got;
    long total = 0, bad_total = 0, forms_ok = 0, forms_bad = 0, unsupported = 0, rosetta_deviations = 0;
    int nforms = (int)(sizeof forms / sizeof forms[0]);
    for (int fi = 0; fi < nforms; fi++) {
        const form *f = &forms[fi];
        if (only && !strstr(f->name, only)) continue;
        long bad = 0, ran = 0;
        char first[900] = "";
        for (int k = 0; k < per; k++) {
            int size;
            make_case(f, &c, &size);
            x86_insn d;
            if (x86_decode(c.code, c.len, &d) != (int)c.len) { snprintf(first, sizeof first, "decoder: length %d, want %u", d.len, c.len); bad++; break; }
            if (run_translated(&c, &d, &got) < 0) { unsupported++; snprintf(first, sizeof first, "not translated"); bad++; break; }
            if (fwrite(&c, sizeof c, 1, out) != 1 || fflush(out) || fread(&want, sizeof want, 1, in) != 1) { fprintf(stderr, "ref_x86 stopped\n"); return 1; }
            ran++;
            // Rosetta 2 deviates from x86 here (the translator does what x86 does): a 16-bit LOCK NEG across a 16-byte
            // boundary updates the flags but does not store the result.
            if (f->special == SP_LOCK && f->op == 0xF7 && f->group == 3 && size == 2 && (last_addr & 15) == 15) { rosetta_deviations++; dv[DV_LOCKNEG]++; continue; }
            uint64_t mask = ~undefined_flags(&d, &c.in, size) & 0x8D5;
            int diff = want.out.fault != got.out.fault;
            if (!diff && !want.out.fault) {
                // (the reference stores EFLAGS just below the final RSP: those 8 bytes are its own)
                uint64_t sp = want.out.gpr[4] - 8 - DT_BUF;
                if (sp + 8 <= DT_BUF_SIZE) memcpy(want.mem + sp, got.mem + sp, 8);
                diff = (want.out.flags & mask) != (got.out.flags & mask) || memcmp(want.mem, got.mem, DT_BUF_SIZE) ||
                       memcmp(want.out.xmm, got.out.xmm, sizeof want.out.xmm) || memcmp(want.out.ymm_hi, got.out.ymm_hi, sizeof want.out.ymm_hi) || (want.out.mxcsr & ~0x3Fu) != (got.out.mxcsr & ~0x3Fu);
                for (int i = 0; i < 16; i++) diff |= want.out.gpr[i] != got.out.gpr[i];
            }
            if (diff) {
                if (!bad) {
                    int n = snprintf(first, sizeof first, "bytes");
                    for (uint32_t i = 0; i < c.len; i++) n += snprintf(first + n, sizeof first - (size_t)n, " %02x", c.code[i]);
                    n += snprintf(first + n, sizeof first - (size_t)n, "; fault %llu/%llu flags %03llx/%03llx (in %03llx)",
                                  (unsigned long long)want.out.fault, (unsigned long long)got.out.fault,
                                  (unsigned long long)(want.out.flags & mask), (unsigned long long)(got.out.flags & mask), (unsigned long long)(c.in.flags & 0x8D5));
                    for (int i = 0; i < 16 && n < 760; i++)
                        if (want.out.gpr[i] != got.out.gpr[i])
                            n += snprintf(first + n, sizeof first - (size_t)n, "; r%d %llx/%llx (in %llx)", i,
                                          (unsigned long long)want.out.gpr[i], (unsigned long long)got.out.gpr[i], (unsigned long long)c.in.gpr[i]);
                    for (int i = 0; i < 16 && n < 760; i++)
                        if (memcmp(want.out.xmm[i], got.out.xmm[i], 16)) {
                            uint64_t w0, w1, g0, g1, i0, i1;
                            memcpy(&w0, want.out.xmm[i], 8); memcpy(&w1, want.out.xmm[i] + 8, 8); memcpy(&g0, got.out.xmm[i], 8); memcpy(&g1, got.out.xmm[i] + 8, 8);
                            memcpy(&i0, c.in.xmm[i], 8); memcpy(&i1, c.in.xmm[i] + 8, 8);
                            n += snprintf(first + n, sizeof first - (size_t)n, "; xmm%d %016llx%016llx/%016llx%016llx (in %016llx%016llx)", i,
                                          (unsigned long long)w1, (unsigned long long)w0, (unsigned long long)g1, (unsigned long long)g0, (unsigned long long)i1, (unsigned long long)i0);
                        }
                    if (memcmp(want.mem, got.mem, DT_BUF_SIZE)) n += snprintf(first + n, sizeof first - (size_t)n, "; memory differs");
                    if ((want.out.mxcsr & ~0x3Fu) != (got.out.mxcsr & ~0x3Fu)) snprintf(first + n, sizeof first - (size_t)n, "; mxcsr %x/%x", want.out.mxcsr, got.out.mxcsr);
                    if (getenv("DT_VERBOSE"))
                        for (int i = 0; i < DT_BUF_SIZE; i++) if (want.mem[i] != got.mem[i]) fprintf(stderr, "    mem[%d] in %02x want %02x got %02x\n", i, c.mem[i], want.mem[i], got.mem[i]);
                    if (getenv("DT_VERBOSE"))
                        for (int i = 0; i < 16; i++) fprintf(stderr, "    r%-2d in %016llx want %016llx got %016llx\n", i, (unsigned long long)c.in.gpr[i],
                                                             (unsigned long long)want.out.gpr[i], (unsigned long long)got.out.gpr[i]);
                }
                bad++;
            }
        }
        total += ran; bad_total += bad;
        if (bad) { forms_bad++; printf("  FAIL %-22s %ld/%ld  %s\n", f->name, bad, ran, first); }
        else forms_ok++;
    }
    // SSE / SSE2 forms
    int nsse = (int)(sizeof sse_forms / sizeof sse_forms[0]);
    for (int fi = 0; fi < nsse && (!only || 1); fi++) {
        const sse_form *f = &sse_forms[fi];
        if (only && !strstr(f->name, only)) continue;
        long bad = 0, ran = 0; char first[900] = "";
        for (int k = 0; k < per; k++) {
            make_sse_case(f, &c);
            if (getenv("DT_TRACE")) fprintf(stderr, "%s case %d: mxcsr %04x\n", f->name, k, c.in.mxcsr);
            x86_insn d;
            if (x86_decode(c.code, c.len, &d) != (int)c.len) { snprintf(first, sizeof first, "decoder: length %d, want %u", d.len, c.len); bad++; break; }
            if (run_translated(&c, &d, &got) < 0) { unsupported++; snprintf(first, sizeof first, "not translated (bytes %02x %02x %02x %02x %02x)", c.code[0], c.code[1], c.code[2], c.code[3], c.code[4]); bad++; break; }
            if (fwrite(&c, sizeof c, 1, out) != 1 || fflush(out) || fread(&want, sizeof want, 1, in) != 1) { fprintf(stderr, "ref_x86 stopped\n"); return 1; }
            ran++;
            if ((f->kind & K_APPROX) && !want.out.fault && !got.out.fault && approx_ok(f, &c, &d, &want, &got) <= 0) {
                if (!bad) snprintf(first, sizeof first, "estimate out of x86's bound (bytes %02x %02x %02x %02x %02x)", c.code[0], c.code[1], c.code[2], c.code[3], c.code[4]);
                bad++; continue;
            }
            if ((f->kind & K_NANSIGN) && nan_signs_only(f, &d, &want, &got)) { rosetta_deviations++; dv[DV_NANSIGN]++; }
            if ((f->kind & K_CNT32) && cnt32_case(&c, &d)) { rosetta_deviations++; dv[DV_CNT32]++; continue; }
            if (!want.out.fault && !got.out.fault && ftz_boundary_only(&c, &want, &got)) ftz_boundary++;
            if (!want.out.fault && ((want.out.mxcsr ^ got.out.mxcsr) & ~2u) == 0x10) { ue_tiny++; want.out.mxcsr = got.out.mxcsr; }
            if ((f->kind & K_FMA) && !want.out.fault && (got.out.mxcsr & ~(want.out.mxcsr | 2u)) == 1) {
                fma_ie++; want.out.mxcsr |= 1;                            // (IE: ours alone)
            }
            if ((f->kind & K_BOTHF) && !want.out.fault && want.out.mxcsr != got.out.mxcsr) {
                uint32_t ours = got.out.mxcsr, tiny = (ours & ~want.out.mxcsr) == 0x10 ? 0x10 : 0;   // (with ours' UE as well:
                if ((want.out.mxcsr & (ours & ~tiny)) == (ours & ~tiny)) {                         //  both known, in two lanes)
                    rosetta_deviations++; dv[DV_BOTHF]++; if (tiny) ue_tiny++; want.out.mxcsr = got.out.mxcsr;   // (Rosetta's flags:
                }                                                                                  //  ours and more)
            }
            if ((f->kind & K_MODEL) && want.out.fault == 2) { model(f, &c, &d, &want); modeled++; }
            if ((f->kind & K_RAND) && want.out.fault == 2) {                // (RDSEED: Rosetta 2 has none: x86's result, the value aside)
                memcpy(&want.out, &c.in, sizeof c.in); memcpy(want.mem, c.mem, DT_BUF_SIZE);
                want.out.flags = (c.in.flags & ~0x8D5ull) | 1; want.out.fault = 0; modeled++;
                if (!(d.rex & 8)) want.out.gpr[d.rm] = (uint32_t)got.out.gpr[d.rm];
            }
            if ((f->kind & K_RAND) && !want.out.fault) want.out.gpr[d.rm] = got.out.gpr[d.rm];
            if ((f->kind & K_TSC) && !want.out.fault) { want.out.gpr[0] = got.out.gpr[0]; want.out.gpr[2] = got.out.gpr[2]; if (c.len == 3) want.out.gpr[1] = got.out.gpr[1]; }
            int excusable = !want.out.fault && !got.out.fault && fz_single_case(&c, &d, &want, &got);
            if ((f->kind & K_ALIAS) && want.out.fault == 2 && !got.out.fault) { rosetta_deviations++; dv[DV_ALIAS]++; continue; }
            if ((f->kind & K_X87) && f->op == 0xDF && f->group == 4 && (c.in.ftag >> (((c.in.fsw >> 11) - 1) & 7) & 1)) { rosetta_deviations++; dv[DV_FBLD]++; continue; }   // (FBLD: Rosetta 2 does not check for a stack overflow)
            if ((f->kind & K_X87) && !want.out.fault && !got.out.fault && c1_kept(&c, &want, &got)) { rosetta_deviations++; dv[DV_C1]++; }
            if (f->op == 0x1D && f->map == 3 && (f->kind & K_VEX)) {         // (VCVTPS2PH: imm[7:3] are to be ignored)
                int imm = c.code[c.len - 1];
                if ((imm & 0xF8) && !(imm & 4) && ((imm & 3) == 1 || (imm & 3) == 2) && memcmp(&want, &got, sizeof want)) { rosetta_deviations++; dv[DV_PH]++; continue; }
            }
            if ((f->kind & K_X87) && f->map == 0xFA && !want.out.fault && !(got.out.fsw & 0x20) && (want.out.fsw & 0x20) && (c.in.fcw >> 10 & 3)) {
                int far; dt_result w2 = want; w2.out.fsw &= (uint16_t)~0x20;                // (FSQRT: ours exact, Rosetta 1 ulp off with PE)
                if (trans_close(&w2, &got, &far, 1)) { rosetta_deviations++; dv[DV_SQRT]++; want = w2; }
            }
            if ((f->kind & K_X87) && (f->map == 0xF5 || f->map == 0xF8) && !want.out.fault) {   // (FPREM: a denormal divisor, or
                uint16_t e0, e1; uint64_t m0, m1;                                              //   an exponent difference of 63)
                memcpy(&e0, c.in.st[0] + 8, 2); memcpy(&m0, c.in.st[0], 8); memcpy(&e1, c.in.st[1] + 8, 2); memcpy(&m1, c.in.st[1], 8);
                int x0 = (e0 & 0x7FFF) ? (e0 & 0x7FFF) : 1 - (m0 ? __builtin_clzll(m0) : 0), x1 = (e1 & 0x7FFF) ? (e1 & 0x7FFF) : 1 - (m1 ? __builtin_clzll(m1) : 0);
                if (((!(e1 & 0x7FFF) && m1) || x0 - x1 == 63) && memcmp(&want, &got, sizeof want)) { rosetta_deviations++; dv[DV_PREMDEN]++; continue; }
            }
            if ((f->kind & K_TRANS) && !want.out.fault && !got.out.fault) {
                int far, t0 = c.in.fsw >> 11 & 7;
                uint16_t e0; memcpy(&e0, c.in.st[0] + 8, 2);
                uint64_t m0; memcpy(&m0, c.in.st[0], 8);
                int f2xm1_nan = (e0 & 0x7FFF) == 0x7FFF && (m0 << 1);
                if (f->map == 0xF0 && (c.in.ftag >> t0 & 1) && (e0 & 0x7FFF) >= 0x3FFF && !f2xm1_nan &&
                    !((e0 & 0x7FFF) == 0x3FFF && m0 == 1ull << 63)) { trans_undef++; continue; }   // (F2XM1 outside [-1, 1], infinities too: undefined)
                uint16_t fx = (uint16_t)((want.out.fsw ^ got.out.fsw) & ~0x200);
                if (f->map == 0xF0 && (got.out.fsw & 0x20) && (fx == 0x10 || fx == 0x20 || fx == 0x30) && (got.out.fsw & fx) == fx) {
                    rosetta_deviations++; dv[DV_F2XM1PE]++; want.out.fsw |= fx;   // (inexact, as it always is: x86 raises PE, and UE if tiny)
                }
                if (f->map == 0xF3 && (got.out.fsw & 0x20) && (fx == 0x20 || fx == 0x30) && (got.out.fsw & fx) == fx) {
                    rosetta_deviations++; dv[DV_ATANPE]++; want.out.fsw |= fx;      // (atan t is never t: PE)
                }
                if (trans_close(&want, &got, &far, (c.in.fcw >> 10 & 3) != 0)) trans_ulp++;
                else if (f->map == 0xF9 && (!x87_regs_equal(&want.out, &got.out) || !((want.out.fsw ^ got.out.fsw) & ~0x232))) {
                    rosetta_deviations++; dv[DV_YL2XP1]++; continue;      // (values, or DE / PE / C1)
                }
            }
            if (!want.out.fault && d.mod != 3 && ((f->kind & K_X87) || f->op == 0xAE)) {   // (instruction and data pointers: not kept)
                uint64_t at = c.in.gpr[d.base] + (uint64_t)(int64_t)d.disp - DT_BUF;
                int envs = (f->op == 0xD9 && f->group == 6) || (f->op == 0xDD && f->group == 6);
                if (envs && at + 28 <= DT_BUF_SIZE) {
                    memcpy(want.mem + at + 12, got.mem + at + 12, 16);
                    // Rosetta 2 tags every register in use "zero" after FXRSTOR (which starts each case): its tag word
                    // says only which are empty. Ours is taken where it agrees on that.
                    uint16_t wt, gt; memcpy(&wt, want.mem + at + 8, 2); memcpy(&gt, got.mem + at + 8, 2);
                    int same_empty = 1;
                    for (int r = 0; r < 8; r++) same_empty &= ((wt >> 2 * r & 3) == 3) == ((gt >> 2 * r & 3) == 3) && ((wt >> 2 * r & 3) == 3 || (wt >> 2 * r & 3) == 1);
                    if (same_empty && wt != gt) { memcpy(want.mem + at + 8, got.mem + at + 8, 2); rosetta_deviations++; dv[DV_TAGS]++; }
                }
                if (f->op == 0xAE && f->group == 0 && at + 512 <= DT_BUF_SIZE) { memcpy(want.mem + at + 6, got.mem + at + 6, 18); memcpy(want.mem + at + 416, got.mem + at + 416, 96); }
            }
            if ((f->kind & K_KEEPF) && (want.out.flags & 0x894) == (c.in.flags & 0x894) && (got.out.flags & 0x894) == 0 && (c.in.flags & 0x894)) {
                rosetta_deviations++; dv[DV_TESTF]++;                // (x86 clears them: the translator does)
                want.out.flags = (want.out.flags & ~0x894ull) | (got.out.flags & 0x894);
            }
            int diff = want.out.fault != got.out.fault;
            if (!diff && !want.out.fault) {
                uint64_t sp = want.out.gpr[4] - 8 - DT_BUF;
                if (sp + 8 <= DT_BUF_SIZE) memcpy(want.mem + sp, got.mem + sp, 8);
                uint64_t fm = 0x8D5 & ~((f->kind & K_UAP) ? 0x14ull : 0) & ~((f->kind & K_UAPS) ? 0x94ull : 0);
                diff = (want.out.flags & fm) != (got.out.flags & fm) || memcmp(want.mem, got.mem, DT_BUF_SIZE) ||
                       memcmp(want.out.xmm, got.out.xmm, sizeof want.out.xmm) || memcmp(want.out.ymm_hi, got.out.ymm_hi, sizeof want.out.ymm_hi) ||
                       (want.out.mxcsr & ~2u) != (got.out.mxcsr & ~2u) ||   // (DE is not kept: TRANSLATOR.md)
                       want.out.fcw != got.out.fcw || want.out.fsw != got.out.fsw || want.out.ftag != got.out.ftag ||
                       !x87_regs_equal(&want.out, &got.out);
                for (int i = 0; i < 16; i++) diff |= want.out.gpr[i] != got.out.gpr[i];
            }
            if (diff && excusable) { fz_single++; diff = 0; }
            if (diff && getenv("DT_DUMPALL")) {                              // (every failing case: case, Rosetta's, ours)
                FILE *df = fopen(getenv("DT_DUMPALL"), "ab");
                if (df) { fwrite(&c, sizeof c, 1, df); fwrite(&want, sizeof want, 1, df); fwrite(&got, sizeof got, 1, df); fclose(df); }
            }
            if (diff) {
                if (!bad) {
                    int n = snprintf(first, sizeof first, "bytes");
                    for (uint32_t i = 0; i < c.len; i++) n += snprintf(first + n, sizeof first - (size_t)n, " %02x", c.code[i]);
                    n += snprintf(first + n, sizeof first - (size_t)n, "; fault %llu/%llu flags %03llx/%03llx (in %03llx)", (unsigned long long)want.out.fault,
                                  (unsigned long long)got.out.fault, (unsigned long long)(want.out.flags & 0x8D5), (unsigned long long)(got.out.flags & 0x8D5),
                                  (unsigned long long)(c.in.flags & 0x8D5));
                    for (int i = 0; i < 16 && n < 760; i++)
                        if (want.out.gpr[i] != got.out.gpr[i]) n += snprintf(first + n, sizeof first - (size_t)n, "; r%d %llx/%llx", i, (unsigned long long)want.out.gpr[i], (unsigned long long)got.out.gpr[i]);
                    for (int i = 0; i < 16 && n < 760; i++)
                        if (memcmp(want.out.xmm[i], got.out.xmm[i], 16)) {
                            uint64_t w0, w1, g0, g1, i0, i1;
                            memcpy(&w0, want.out.xmm[i], 8); memcpy(&w1, want.out.xmm[i] + 8, 8); memcpy(&g0, got.out.xmm[i], 8); memcpy(&g1, got.out.xmm[i] + 8, 8);
                            memcpy(&i0, c.in.xmm[i], 8); memcpy(&i1, c.in.xmm[i] + 8, 8);
                            n += snprintf(first + n, sizeof first - (size_t)n, "; xmm%d %016llx%016llx/%016llx%016llx (in %016llx%016llx)", i,
                                          (unsigned long long)w1, (unsigned long long)w0, (unsigned long long)g1, (unsigned long long)g0, (unsigned long long)i1, (unsigned long long)i0);
                        }
                    for (int i = 0; i < 16 && n < 760; i++)
                        if (memcmp(want.out.ymm_hi[i], got.out.ymm_hi[i], 16)) {
                            uint64_t w0, w1, g0, g1;
                            memcpy(&w0, want.out.ymm_hi[i], 8); memcpy(&w1, want.out.ymm_hi[i] + 8, 8); memcpy(&g0, got.out.ymm_hi[i], 8); memcpy(&g1, got.out.ymm_hi[i] + 8, 8);
                            n += snprintf(first + n, sizeof first - (size_t)n, "; ymm%d.hi %016llx%016llx/%016llx%016llx", i,
                                          (unsigned long long)w1, (unsigned long long)w0, (unsigned long long)g1, (unsigned long long)g0);
                        }
                    if (want.out.mxcsr != got.out.mxcsr) n += snprintf(first + n, sizeof first - (size_t)n, "; mxcsr %04x/%04x (in %04x)", want.out.mxcsr, got.out.mxcsr, c.in.mxcsr);
                    if (want.out.fcw != got.out.fcw || want.out.fsw != got.out.fsw || want.out.ftag != got.out.ftag)
                        n += snprintf(first + n, sizeof first - (size_t)n, "; fcw %04x/%04x fsw %04x/%04x (in %04x) tag %02x/%02x (in %02x)", want.out.fcw, got.out.fcw,
                                      want.out.fsw, got.out.fsw, c.in.fsw, want.out.ftag, got.out.ftag, c.in.ftag);
                    for (int i = 0; i < 8 && n < 760; i++)
                        if (memcmp(want.out.st[i], got.out.st[i], 10)) {
                            uint64_t wm, gm; uint16_t we, ge; memcpy(&wm, want.out.st[i], 8); memcpy(&we, want.out.st[i] + 8, 2); memcpy(&gm, got.out.st[i], 8); memcpy(&ge, got.out.st[i] + 8, 2);
                            n += snprintf(first + n, sizeof first - (size_t)n, "; st%d %04x.%016llx/%04x.%016llx", i, we, (unsigned long long)wm, ge, (unsigned long long)gm);
                        }
                    if (memcmp(want.mem, got.mem, DT_BUF_SIZE)) snprintf(first + n, sizeof first - (size_t)n, "; memory differs");
                    if (getenv("DT_DUMP")) {                                  // (DT_DUMP.<form>: each form's first)
                        char path[1024]; snprintf(path, sizeof path, "%s.%d", getenv("DT_DUMP"), fi);
                        FILE *df = fopen(path, "wb"); if (df) { fwrite(&c, sizeof c, 1, df); fclose(df); }
                    }
                    if (getenv("DT_VERBOSE"))
                        for (int i = 0, k = 0; i < DT_BUF_SIZE && k < 40; i++) if (want.mem[i] != got.mem[i]) { k++; fprintf(stderr, "    mem[%d] in %02x want %02x got %02x\n", i, c.mem[i], want.mem[i], got.mem[i]); }
                    if (getenv("DT_RECHECK")) {
                        static dt_result again; run_translated(&c, &d, &again);
                        fprintf(stderr, "    recheck: translated again %s; the reference's memory at the operand %s ours\n",
                                memcmp(again.out.xmm, got.out.xmm, sizeof got.out.xmm) ? "DIFFERS" : "the same", memcmp(want.mem, got.mem, DT_BUF_SIZE) ? "differs from" : "equals");
                        uint64_t a[2]; memcpy(a, c.mem + 0x700, 16);
                        fprintf(stderr, "    case mem[0x700] %016llx %016llx\n", (unsigned long long)a[1], (unsigned long long)a[0]);
                    }
                }
                bad++;
            }
        }
        total += ran; bad_total += bad; nforms++;
        if (bad) { forms_bad++; printf("  FAIL %-22s %ld/%ld  %s\n", f->name, bad, ran, first); } else forms_ok++;
    }

    // Programs: blocks, branches, calls, loops.
    if (!only || strstr("programs", only)) {
        long pbad = 0, pran = 0; char first[900] = "";
        int nprog = per * 4;
        for (int k = 0; k < nprog; k++) {
            make_program(&c);
            if (getenv("DT_PROG_FILE") && !k) { FILE *pf = fopen(getenv("DT_PROG_FILE"), "wb"); if (pf) { fwrite(c.prog, 1, c.prog_len, pf); fclose(pf); } }
            if (run_program(&c, &got) < 0) { snprintf(first, sizeof first, "not translated"); pbad++; break; }
            if (fwrite(&c, sizeof c, 1, out) != 1 || fflush(out) || fread(&want, sizeof want, 1, in) != 1) { fprintf(stderr, "ref_x86 stopped\n"); return 1; }
            pran++;
            int diff = want.out.fault != got.out.fault;
            if (!diff && !want.out.fault) {
                uint64_t sp = want.out.gpr[4] - 8 - DT_BUF;
                if (sp + 8 <= DT_BUF_SIZE) memcpy(want.mem + sp, got.mem + sp, 8);
                diff = (want.out.flags & 0x8D5) != (got.out.flags & 0x8D5) || memcmp(want.mem, got.mem, DT_BUF_SIZE) ||
                       memcmp(want.out.xmm, got.out.xmm, sizeof want.out.xmm) || memcmp(want.out.ymm_hi, got.out.ymm_hi, sizeof want.out.ymm_hi);
                for (int i = 0; i < 16; i++) diff |= want.out.gpr[i] != got.out.gpr[i];
            }
            if (diff) {
                if (!pbad) {
                    int n = snprintf(first, sizeof first, "program %d (%u bytes): fault %llu/%llu flags %03llx/%03llx", k, c.prog_len,
                                     (unsigned long long)want.out.fault, (unsigned long long)got.out.fault,
                                     (unsigned long long)(want.out.flags & 0x8D5), (unsigned long long)(got.out.flags & 0x8D5));
                    for (int i = 0; i < 16 && n < 760; i++)
                        if (want.out.gpr[i] != got.out.gpr[i])
                            n += snprintf(first + n, sizeof first - (size_t)n, "; r%d %llx/%llx", i, (unsigned long long)want.out.gpr[i], (unsigned long long)got.out.gpr[i]);
                    if (getenv("DT_VERBOSE")) { FILE *pf = fopen("/tmp/difftest_prog.bin", "wb"); if (pf) { fwrite(c.prog, 1, c.prog_len, pf); fclose(pf); } }
                }
                pbad++;
            }
        }
        total += pran; bad_total += pbad;
        if (pbad) { forms_bad++; printf("  FAIL %-22s %ld/%ld  %s\n", "programs", pbad, pran, first); } else forms_ok++;
        nforms++;
    }
    // The pending-flags record computed in C (xlat_flags_resolve: a fault or SuspendThread while flags are pending): ADD
    // SUB CMP AND OR XOR TEST of each size on the reference, its six flags against the record's (AF only where defined).
    if (!only || strstr("flags record", only)) {
        static const struct { uint8_t op; int kind, logic; } ops[] = {
            { 0x01, 0, 0 }, { 0x29, 1, 0 }, { 0x39, 1, 0 }, { 0x21, 2, 1 }, { 0x09, 2, 1 }, { 0x31, 2, 1 }, { 0x85, 2, 1 } };
        long rbad = 0, rran = 0; char first[300] = "";
        for (int k = 0; k < per * 2; k++) {
            int o = (int)(rnd() % 7), z = (int)(rnd() % 4), size = 1 << z;
            memset(&c, 0, sizeof c);
            for (int i = 0; i < 16; i++) c.in.gpr[i] = rnd_value();
            c.in.mxcsr = 0x1F80; c.in.gpr[4] = DT_STACK; c.in.flags = (rnd() & 0x8D5) | 0x202;
            uint8_t *q = c.code;
            if (size == 2) *q++ = 0x66;
            if (size == 8) *q++ = 0x48;
            *q++ = (uint8_t)(size == 1 ? ops[o].op - 1 : ops[o].op); *q++ = 0xC8;    // op rax/eax/ax/al, rcx/ecx/cx/cl
            c.len = (uint32_t)(q - c.code);
            if (fwrite(&c, sizeof c, 1, out) != 1 || fflush(out) || fread(&want, sizeof want, 1, in) != 1) { fprintf(stderr, "ref_x86 stopped\n"); return 1; }
            rran++;
            uint64_t m = size == 8 ? ~0ull : (1ull << (8 * size)) - 1, a = c.in.gpr[0] & m, bb = c.in.gpr[1] & m;
            uint64_t r = ops[o].kind == 0 ? a + bb : ops[o].kind == 1 ? a - bb : ops[o].op == 0x09 ? a | bb : ops[o].op == 0x31 ? a ^ bb : a & bb;
            x86_ctx ctx; memset(&ctx, 0, sizeof ctx); ctx.pend_a = a; ctx.pend_b = bb; ctx.pend_r = r; ctx.pend_info = (uint64_t)(ops[o].kind * 4 + z);
            uint64_t x27 = 8 | 0x202;
            uint64_t got_f = xlat_flags_resolve(x27, &ctx), mask = ops[o].logic ? 0x8C5 : 0x8D5;
            if ((got_f & mask) != (want.out.flags & mask) || (got_f & 8)) {
                if (!rbad) snprintf(first, sizeof first, "op %02x size %d a %llx b %llx: flags %03llx/%03llx (fault %llu, rax %llx)", ops[o].op, size,
                                    (unsigned long long)a, (unsigned long long)bb, (unsigned long long)(want.out.flags & mask), (unsigned long long)(got_f & mask),
                                    (unsigned long long)want.out.fault, (unsigned long long)want.out.gpr[0]);
                rbad++;
            }
        }
        total += rran; bad_total += rbad; nforms++;
        if (rbad) { forms_bad++; printf("  FAIL %-22s %ld/%ld  %s\n", "flags record", rbad, rran, first); } else forms_ok++;
    }
    fclose(out); fclose(in);
    if (getenv("DT_ENGINE")) {
        xe_stats st = xe_get_stats(engine);
        printf("  engine: %llu blocks, %llu branches linked, %llu entries from C, %llu flushes, %llu x86 bytes -> %llu ARM instructions, %llu split accesses patched, "
               "flags not computed for %llu instructions, %llu Jccs fused\n",
               (unsigned long long)st.blocks, (unsigned long long)st.linked, (unsigned long long)st.entries, (unsigned long long)st.flushes,
               (unsigned long long)st.x86_bytes, (unsigned long long)st.arm_insns, (unsigned long long)st.split_patches,
               (unsigned long long)st.flags_skipped, (unsigned long long)st.fused);
    }
    if (getenv("DT_DEVIATIONS")) for (int k = 0; k < DV_N; k++) if (dv[k]) printf("  Rosetta 2 deviation: %-45s %ld\n", dv_name[k], dv[k]);
    printf("x86 -> ARM64 vs Rosetta: %d forms, %ld cases: %ld forms match, %ld differ (%ld cases)%s; %ld cases skipped where Rosetta deviates from x86; ours known to: %ld (FTZ at 2^-126), %ld (UE, tiny before rounding), %ld (FMA's invalid for 0 * inf + QNaN), %ld (FTZ or DAZ alone, with denormals); x87 transcendentals 1 ulp from Rosetta's (or C1): %ld, undefined inputs: %ld; checked against a model (Rosetta lacks them): %ld\n",
           nforms, total, forms_ok, forms_bad, bad_total, unsupported ? " (some not translated)" : "", rosetta_deviations, ftz_boundary, ue_tiny, fma_ie, fz_single, trans_ulp, trans_undef, modeled);
    return forms_bad ? 1 : 0;
}
