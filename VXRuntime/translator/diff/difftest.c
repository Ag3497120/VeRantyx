// The differential test (TRANSLATOR.md, phase 2): random instructions of every form the translator handles, each run
// with random registers, flags and memory twice: for real, by ref_x86 under Rosetta 2, and translated by xlat.c and run
// natively. Registers, the defined flags, memory and faults must match.
//
//   build/difftest build/ref_x86 [cases per form] [seed]
#include "../x86dec.h"
#include "../xlat.h"
#include "difftest.h"
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <spawn.h>
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
    for (int i = 0; i < 16; i++) c->in.gpr[i] = rnd_value();
    for (int i = 0; i < DT_BUF_SIZE; i++) c->mem[i] = (uint8_t)rnd();
    c->in.gpr[4] = DT_STACK;
    c->in.flags = (rnd() & 0x8D5) | 0x202;
    uint8_t *p = c->code;
    int byteop = is_byte_op(f);
    int sizesel = (int)(rnd() % 3);                         // 0: 32-bit, 1: 16-bit (66), 2: 64-bit (REX.W)
    if (byteop || f->no_byte_ops) sizesel = sizesel == 1 ? 0 : sizesel;
    if (f->op == 0x68 || f->op == 0x6A || f->op == 0x50 || f->op == 0x58) sizesel = 0;   // (64-bit stack operations)
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
    ctx.flags = c->in.flags;
    memcpy((void *)DT_BUF, c->mem, DT_BUF_SIZE);
    signaled = 0;
    if (!sigsetjmp(jb, 1)) ((void (*)(x86_ctx *))jit)(&ctx);
    memcpy(r->out.gpr, ctx.gpr, sizeof ctx.gpr);
    r->out.flags = ctx.flags;
    r->out.fault = signaled ? 2 : ctx.fault == X86_FAULT_DE ? 1 : 0;
    if (r->out.fault) { memcpy(r->out.gpr, c->in.gpr, sizeof ctx.gpr); r->out.flags = c->in.flags; }
    memcpy(r->mem, (void *)DT_BUF, DT_BUF_SIZE);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: difftest <ref_x86> [cases per form] [seed]\n"); return 2; }
    int per = argc > 2 ? atoi(argv[2]) : 2000;
    const char *only = argc > 4 ? argv[4] : NULL;            // (a part of the forms' names: only those)
    rng_state = argc > 3 ? strtoull(argv[3], NULL, 0) : 0x9E3779B97F4A7C15ull;
    jit = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    void *buf = mmap((void *)DT_BUF, DT_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (jit == MAP_FAILED || buf != (void *)DT_BUF) { fprintf(stderr, "difftest: mmap failed\n"); return 1; }
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);

    int to_ref[2], from_ref[2];
    if (pipe(to_ref) || pipe(from_ref)) return 1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to_ref[0], 0);
    posix_spawn_file_actions_adddup2(&fa, from_ref[1], 1);
    posix_spawn_file_actions_addclose(&fa, to_ref[1]);
    posix_spawn_file_actions_addclose(&fa, from_ref[0]);
    pid_t pid;
    char *args[] = { "/usr/bin/arch", "-x86_64", argv[1], NULL };
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
        char first[400] = "";
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
            if (f->special == SP_LOCK && f->op == 0xF7 && f->group == 3 && size == 2 && (last_addr & 15) == 15) { rosetta_deviations++; continue; }
            uint64_t mask = ~undefined_flags(&d, &c.in, size) & 0x8D5;
            int diff = want.out.fault != got.out.fault;
            if (!diff && !want.out.fault) {
                // (the reference stores EFLAGS just below the final RSP: those 8 bytes are its own)
                uint64_t sp = want.out.gpr[4] - 8 - DT_BUF;
                if (sp + 8 <= DT_BUF_SIZE) memcpy(want.mem + sp, got.mem + sp, 8);
                diff = (want.out.flags & mask) != (got.out.flags & mask) || memcmp(want.mem, got.mem, DT_BUF_SIZE);
                for (int i = 0; i < 16; i++) diff |= want.out.gpr[i] != got.out.gpr[i];
            }
            if (diff) {
                if (!bad) {
                    int n = snprintf(first, sizeof first, "bytes");
                    for (uint32_t i = 0; i < c.len; i++) n += snprintf(first + n, sizeof first - (size_t)n, " %02x", c.code[i]);
                    n += snprintf(first + n, sizeof first - (size_t)n, "; fault %llu/%llu flags %03llx/%03llx (in %03llx)",
                                  (unsigned long long)want.out.fault, (unsigned long long)got.out.fault,
                                  (unsigned long long)(want.out.flags & mask), (unsigned long long)(got.out.flags & mask), (unsigned long long)(c.in.flags & 0x8D5));
                    for (int i = 0; i < 16 && n < 360; i++)
                        if (want.out.gpr[i] != got.out.gpr[i])
                            n += snprintf(first + n, sizeof first - (size_t)n, "; r%d %llx/%llx (in %llx)", i,
                                          (unsigned long long)want.out.gpr[i], (unsigned long long)got.out.gpr[i], (unsigned long long)c.in.gpr[i]);
                    if (memcmp(want.mem, got.mem, DT_BUF_SIZE)) snprintf(first + n, sizeof first - (size_t)n, "; memory differs");
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
    fclose(out); fclose(in);
    printf("x86 -> ARM64 vs Rosetta: %d forms, %ld cases: %ld forms match, %ld differ (%ld cases)%s; %ld cases skipped where Rosetta deviates from x86\n",
           nforms, total, forms_ok, forms_bad, bad_total, unsupported ? " (some not translated)" : "", rosetta_deviations);
    return forms_bad ? 1 : 0;
}
