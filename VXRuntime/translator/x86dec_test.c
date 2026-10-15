// The decoder on instructions whose lengths and parts are known (Intel SDM; cross-checked with llvm-mc). Part of make check.
#include "x86dec.h"
#include <stdio.h>
#include <string.h>

static const struct { const char *what; uint8_t bytes[16]; int n, len; } cases[] = {
    { "push rbp", {0x55}, 1, 1 },
    { "mov rax, [rip+disp32]", {0x48,0x8B,0x05,0x10,0x20,0x30,0x40}, 7, 7 },
    { "mov rax, imm64", {0x48,0xB8,1,2,3,4,5,6,7,8}, 10, 10 },
    { "mov ax, imm16", {0x66,0xB8,1,2}, 4, 4 },
    { "mov eax, [moffs64]", {0xA1,1,2,3,4,5,6,7,8}, 9, 9 },
    { "lea rcx, [rsp+rax*8+0x10]", {0x48,0x8D,0x4C,0xC4,0x10}, 5, 5 },
    { "mov eax, [rbp+disp32] (SIB base 5, mod 2)", {0x8B,0x84,0x25,0,1,0,0}, 7, 7 },
    { "mov eax, [disp32] (SIB no base)", {0x8B,0x04,0x25,0,1,0,0}, 7, 7 },
    { "test byte [rax], imm8 (F6 /0)", {0xF6,0x00,0x7F}, 3, 3 },
    { "not byte [rax] (F6 /2)", {0xF6,0x10}, 2, 2 },
    { "test dword [rax], imm32 (F7 /0)", {0xF7,0x00,1,2,3,4}, 6, 6 },
    { "test word [rax], imm16", {0x66,0xF7,0x00,1,2}, 5, 5 },
    { "enter 0x20, 0", {0xC8,0x20,0x00,0x00}, 4, 4 },
    { "ret imm16", {0xC2,0x08,0x00}, 3, 3 },
    { "call rel32 (66 ignored)", {0x66,0xE8,1,2,3,4}, 6, 6 },
    { "jne rel32", {0x0F,0x85,1,2,3,4}, 6, 6 },
    { "lock cmpxchg [rcx], rdx", {0xF0,0x48,0x0F,0xB1,0x11}, 5, 5 },
    { "rep movsb", {0xF3,0xA4}, 2, 2 },
    { "rex ignored before 66: nop", {0x43,0x66,0x90}, 3, 3 },
    { "pshufd xmm0, xmm1, imm8", {0x66,0x0F,0x70,0xC1,0x1B}, 5, 5 },
    { "pshufb xmm0, [rax] (0F 38)", {0x66,0x0F,0x38,0x00,0x00}, 5, 5 },
    { "roundss xmm0, xmm1, 4 (0F 3A)", {0x66,0x0F,0x3A,0x0A,0xC1,0x04}, 6, 6 },
    { "vmovups ymm0, [rax] (VEX2)", {0xC5,0xFC,0x10,0x00}, 4, 4 },
    { "vfmadd231ps ymm0, ymm1, [rax+8] (VEX3)", {0xC4,0xE2,0x75,0xB8,0x40,0x08}, 6, 6 },
    { "vshufps ymm0, ymm1, ymm2, imm8", {0xC5,0xF4,0xC6,0xC2,0x44}, 5, 5 },
    { "vzeroupper (no ModRM)", {0xC5,0xF8,0x77}, 3, 3 },
    { "vaddps zmm0, zmm1, zmm2 (EVEX)", {0x62,0xF1,0x74,0x48,0x58,0xC2}, 6, 6 },
    { "fld qword [rax] (x87)", {0xDD,0x00}, 2, 2 },
    { "cpuid", {0x0F,0xA2}, 2, 2 },
    { "imul eax, ecx, imm8", {0x6B,0xC1,0x10}, 3, 3 },
    { "shld eax, ecx, imm8", {0x0F,0xA4,0xC8,0x04}, 4, 4 },
    { "bt dword [rax], imm8 (0F BA)", {0x0F,0xBA,0x20,0x05}, 4, 4 },
    { "3DNow pfadd", {0x0F,0x0F,0xC1,0x9E}, 4, 4 },
    { "push imm32 (sign-extended)", {0x68,0xFF,0xFF,0xFF,0xFF}, 5, 5 },
    { "imul rsp, [rdi+0x68], imm32 (66 + REX.W: imm32)", {0x66,0x48,0x69,0x67,0x68,1,2,3,4}, 9, 9 },
    { "XOP vprotd xmm, xmm, imm8 (map 8)", {0x8F,0xE8,0x78,0xC2,0xEC,0x0E}, 6, 6 },
    { "XOP vprotq xmm, xmm, imm8 (map 8)", {0x8F,0x48,0x78,0xC3,0xC8,0x38}, 6, 6 },
    { "XOP vfrczps (map 9, no immediate)", {0x8F,0xE9,0x78,0x80,0xC1}, 5, 5 },
    { "pop qword [rax] (8F /0, not XOP)", {0x8F,0x00}, 2, 2 },
    { "invalid in 64-bit: push es", {0x06}, 1, 0 },
    { "cut off", {0x48,0x8B}, 2, 0 },
};

int main(void) {
    int bad = 0, n = (int)(sizeof cases / sizeof cases[0]);
    for (int i = 0; i < n; i++) {
        x86_insn d;
        int len = x86_decode(cases[i].bytes, (size_t)cases[i].n, &d);
        if (len != cases[i].len) { printf("  %s: length %d, want %d\n", cases[i].what, len, cases[i].len); bad++; }
    }
    // Parts: the memory operand of lea rcx, [rsp+rax*8+0x10] and the immediate of mov rax, imm64.
    x86_insn d;
    x86_decode(cases[5].bytes, 5, &d);
    if (!(d.base == 4 && d.index == 0 && d.scale == 8 && d.disp == 0x10 && d.reg == 1 && d.opsize == 8)) { printf("  lea operand parts wrong\n"); bad++; }
    x86_decode(cases[2].bytes, 10, &d);
    if (d.imm != 0x0807060504030201LL) { printf("  imm64 wrong: %llx\n", (unsigned long long)d.imm); bad++; }
    x86_decode(cases[23].bytes, 6, &d);
    if (!(d.map == X86_MAP_0F38 && d.opcode == 0xB8 && d.vex_l == 1 && d.vex_vvvv == 1 && d.mandatory == 0x66 && d.disp == 8)) { printf("  VEX parts wrong\n"); bad++; }
    printf("x86 decoder: %d/%d cases -> %s\n", n - bad, n, bad ? "FAILED" : "ok");
    return bad != 0;
}
