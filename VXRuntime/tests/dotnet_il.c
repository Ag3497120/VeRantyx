#include "dotnet_il.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "dotnet IL test failed: %s\n", m); exit(1); } } while (0)

int main(void) {
    const uint8_t main_body[] = {
        0x73, 0xae, 0x01, 0x00, 0x06, 0x25,
        0x6f, 0xac, 0x01, 0x00, 0x06,
        0x6f, 0xeb, 0x01, 0x00, 0x0a,
        0x26, 0x2a
    };
    uint32_t cursor = 0;
    vx_dotnet_instruction ins;
    const uint16_t expected[] = { 0x73, 0x25, 0x6f, 0x6f, 0x26, 0x2a };
    const uint32_t tokens[] = { 0x060001ae, 0, 0x060001ac, 0x0a0001eb, 0, 0 };
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        CHECK(vx_dotnet_il_next(main_body, sizeof main_body, &cursor, &ins) == 1,
              "entry method instruction was not decoded");
        CHECK(ins.opcode == expected[i], "entry method opcode or instruction boundary is wrong");
        if (tokens[i]) {
            uint32_t token;
            CHECK(ins.operand_kind == VX_IL_OPERAND_TOKEN && ins.operand_size == 4,
                  "method call token has the wrong operand kind");
            memcpy(&token, ins.operand, sizeof token);
            CHECK(token == tokens[i], "method token was not retained");
        }
    }
    CHECK(vx_dotnet_il_next(main_body, sizeof main_body, &cursor, &ins) == 0,
          "instruction iterator did not stop at end of code");

    const uint8_t short_branch[] = { 0x2b, 0xfe, 0x2a };
    cursor = 0;
    CHECK(vx_dotnet_il_next(short_branch, sizeof short_branch, &cursor, &ins) == 1 &&
          ins.operand_kind == VX_IL_OPERAND_BRANCH1, "short branch was not decoded");
    uint32_t target;
    CHECK(vx_dotnet_il_branch_target(&ins, 0, sizeof short_branch, &target) && target == 0,
          "signed short branch target was decoded incorrectly");
    CHECK(vx_dotnet_il_next(short_branch, sizeof short_branch, &cursor, &ins) == 1 && ins.opcode == 0x2a,
          "short branch did not end at the next instruction");

    uint8_t switch_code[] = { 0x45, 1, 0, 0, 0, 0, 0, 0, 0, 0x2a };
    cursor = 0;
    CHECK(vx_dotnet_il_next(switch_code, sizeof switch_code, &cursor, &ins) == 1 &&
          ins.operand_kind == VX_IL_OPERAND_SWITCH, "switch operand was not decoded");
    CHECK(vx_dotnet_il_branch_target(&ins, 0, sizeof switch_code, &target) && target == 9,
          "switch target was not relative to the end of the table");

    const uint8_t truncated_token[] = { 0x28, 1, 2 };
    cursor = 0;
    CHECK(vx_dotnet_il_next(truncated_token, sizeof truncated_token, &cursor, &ins) == -1,
          "truncated metadata token accepted");
    const uint8_t truncated_switch[] = { 0x45, 0xff, 0xff, 0xff, 0x7f };
    cursor = 0;
    CHECK(vx_dotnet_il_next(truncated_switch, sizeof truncated_switch, &cursor, &ins) == -1,
          "oversized switch table accepted");
    const uint8_t invalid_opcode[] = { 0x24 };
    cursor = 0;
    CHECK(vx_dotnet_il_next(invalid_opcode, sizeof invalid_opcode, &cursor, &ins) == -1,
          "reserved opcode accepted");
    puts("CIL instruction framing, metadata tokens, branches and malformed operands: ok");
    return 0;
}
