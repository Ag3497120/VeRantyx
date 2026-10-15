// x87 in software for translated code (x87.c).
#pragma once
#include "xlat.h"

// Whether x87.c does the x87 instruction with this opcode (D8-DF) and ModRM byte (transcendentals: not yet).
int x87_supported(int op, int modrm);
// Runs it: a is the memory operand's address (memory forms).
void xlat_x87(x86_ctx *c, uint32_t op, uint32_t modrm, uint64_t a);
// FXSAVE (restore 0) / FXRSTOR (restore 1) at a: the 416 bytes the processor defines.
void xlat_fxsave(x86_ctx *c, uint64_t a, int restore);
