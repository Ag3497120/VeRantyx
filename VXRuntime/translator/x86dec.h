// x86-64 instruction decoder: the first stage of VXRuntime's own x86-64 -> ARM64 translator (TRANSLATOR.md).
// It finds an instruction's length and parts (prefixes, opcode map and byte, ModRM operand, immediate) without
// tables of mnemonics; what an instruction does is the translator's business, which keys on (map, opcode, prefix).
#pragma once
#include <stddef.h>
#include <stdint.h>

enum x86_map { X86_MAP_1 = 0, X86_MAP_0F, X86_MAP_0F38, X86_MAP_0F3A, X86_MAP_3DNOW, X86_MAP_XOP8, X86_MAP_XOP9, X86_MAP_XOPA };

enum x86_prefix {                    // bits of x86_insn.prefixes
    X86_P_LOCK = 1, X86_P_REP = 2, X86_P_REPNE = 4, X86_P_OPSIZE = 8, X86_P_ADDRSIZE = 16,
    X86_P_SEG_FS = 32, X86_P_SEG_GS = 64, X86_P_SEG_OTHER = 128,
    X86_P_VEX = 256, X86_P_EVEX = 512,
};

typedef struct {
    uint8_t len;                     // total length in bytes (1..15); 0: not decodable
    uint8_t map;                     // enum x86_map
    uint8_t opcode;                  // the opcode byte within its map
    uint16_t prefixes;               // enum x86_prefix
    uint8_t mandatory;               // SIMD mandatory prefix: 0 none, 0x66, 0xF3, 0xF2 (legacy or VEX.pp/EVEX.pp)
    uint8_t rex;                     // 0x40-0x4F, or 0
    uint8_t vex_l;                   // VEX.L / EVEX.L'L (vector length: 0 = 128, 1 = 256, 2 = 512)
    uint8_t vex_w;
    uint8_t vex_vvvv;                // the extra source register (already inverted), 0-31
    uint8_t has_modrm;
    uint8_t modrm, mod, reg, rm;     // reg and rm include REX/VEX extension bits
    uint8_t has_sib;
    uint8_t base, index, scale;      // memory operand (base 0xFF: none; index 0xFF: none)
    uint8_t rip_relative;
    int32_t disp;
    uint8_t disp_size;
    uint8_t imm_size;                // bytes of immediate data (two immediates, e.g. ENTER: sum)
    int64_t imm;                     // the first immediate, sign-extended
    uint8_t opsize;                  // operand size in bytes for general instructions: 1, 2, 4 or 8
} x86_insn;

// Decodes the instruction at code[0..avail). Returns its length (0 if it cannot be decoded: invalid in 64-bit mode
// or cut off).
int x86_decode(const uint8_t *code, size_t avail, x86_insn *out);
