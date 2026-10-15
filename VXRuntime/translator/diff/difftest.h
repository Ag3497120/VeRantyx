// The differential test's shared definitions: one x86 instruction, the machine state before it, and after.
#pragma once
#include <stdint.h>

#define DT_BUF ((uint64_t)0x300000000ull)   // test memory, at this address in both processes
#define DT_BUF_SIZE 4096
#define DT_STACK (DT_BUF + 3584)            // RSP points here (the reference runner keeps EFLAGS below it)

typedef struct {
    uint64_t gpr[16];
    uint64_t flags;
    uint64_t fault;                         // 0, or 1 (#DE), 2 (other signal), 4 (did not end in time)
    uint8_t xmm[16][16];
    uint32_t mxcsr;
    uint32_t pad;
} dt_state;
enum { DT_XMM = 144, DT_MXCSR = 400 };      // offsets in dt_state

// Programs (several blocks, branches, calls) run from this address in both processes and end by jumping to DT_END.
#define DT_PROG ((uint64_t)0x310000000ull)
#define DT_PROG_MAX 3072
#define DT_END (DT_PROG + DT_PROG_MAX)

typedef struct {
    uint8_t code[16];                       // kind 0: one instruction
    uint32_t len;
    uint32_t kind;                          // 0: one instruction; 1: a program in prog
    dt_state in;
    uint8_t mem[DT_BUF_SIZE];
    uint32_t prog_len;
    uint8_t prog[DT_PROG_MAX];
} dt_case;

typedef struct {
    dt_state out;
    uint8_t mem[DT_BUF_SIZE];
} dt_result;
