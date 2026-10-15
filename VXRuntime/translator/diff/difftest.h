// The differential test's shared definitions: one x86 instruction, the machine state before it, and after.
#pragma once
#include <stdint.h>

#define DT_BUF ((uint64_t)0x300000000ull)   // test memory, at this address in both processes
#define DT_BUF_SIZE 4096
#define DT_STACK (DT_BUF + 3584)            // RSP points here (the reference runner keeps EFLAGS below it)

typedef struct {
    uint64_t gpr[16];
    uint64_t flags;
    uint64_t fault;                         // 0, or 1 (#DE), 2 (other signal)
} dt_state;

typedef struct {
    uint8_t code[16];
    uint32_t len;
    uint32_t pad;
    dt_state in;
    uint8_t mem[DT_BUF_SIZE];
} dt_case;

typedef struct {
    dt_state out;
    uint8_t mem[DT_BUF_SIZE];
} dt_result;
