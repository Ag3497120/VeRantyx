// Bounds-checked ECMA-335 CIL instruction framing and branch-target decoding.
#ifndef VX_DOTNET_IL_H
#define VX_DOTNET_IL_H
#include <stddef.h>
#include <stdint.h>

typedef enum {
    VX_IL_OPERAND_NONE,
    VX_IL_OPERAND_I1,
    VX_IL_OPERAND_I2,
    VX_IL_OPERAND_I4,
    VX_IL_OPERAND_I8,
    VX_IL_OPERAND_R4,
    VX_IL_OPERAND_R8,
    VX_IL_OPERAND_TOKEN,
    VX_IL_OPERAND_BRANCH1,
    VX_IL_OPERAND_BRANCH4,
    VX_IL_OPERAND_SWITCH
} vx_il_operand_kind;

typedef struct {
    uint32_t offset;
    uint32_t next_offset;
    uint16_t opcode;
    vx_il_operand_kind operand_kind;
    const uint8_t *operand;
    uint32_t operand_size;
} vx_dotnet_instruction;

// Returns 1 for an instruction, 0 at end of code, or -1 for malformed/unknown IL.
int vx_dotnet_il_next(const uint8_t *code, uint32_t code_size, uint32_t *cursor,
                      vx_dotnet_instruction *out);

// Decodes a branch or switch destination as a code offset. Switch target is
// indexed by arm and is relative to the end of its complete instruction.
int vx_dotnet_il_branch_target(const vx_dotnet_instruction *instruction,
                               uint32_t arm, uint32_t code_size, uint32_t *target);

#endif
