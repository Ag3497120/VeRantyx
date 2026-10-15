// The reference side of the differential test: built for x86-64 and run under Rosetta 2, it executes one instruction
// at a time for real and reports the state after it. Reads dt_case records on stdin, writes dt_result records.
// It writes x86 machine code at run time (a stub that loads the state, the instruction, a stub that stores it):
// test tooling of the translator (TRANSLATOR.md).
#include "difftest.h"
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint8_t *code, *stub;
static size_t at;
static uint64_t host_rsp;
static dt_state st_in, st_out;
static sigjmp_buf jb;
static volatile int signaled;

static void b(uint8_t x) { code[at++] = x; }
static void q(uint64_t v) { memcpy(code + at, &v, 8); at += 8; }
static void d32(uint32_t v) { memcpy(code + at, &v, 4); at += 4; }
static void mov_rax_imm(uint64_t v) { b(0x48); b(0xB8); q(v); }
// mov reg, [rax + disp32]  /  mov [rax + disp32], reg
static void ld(int reg, int32_t disp) { b(0x48 | (reg >= 8 ? 4 : 0)); b(0x8B); b(0x80 | (reg & 7) << 3); d32((uint32_t)disp); }
static void st(int reg, int32_t disp) { b(0x48 | (reg >= 8 ? 4 : 0)); b(0x89); b(0x80 | (reg & 7) << 3); d32((uint32_t)disp); }

static void on_signal(int sig) { signaled = sig; siglongjmp(jb, 1); }

int main(void) {
    code = stub = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t *buf = mmap((void *)DT_BUF, DT_BUF_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    uint8_t *prog = mmap((void *)DT_PROG, DT_PROG_MAX + 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (code == MAP_FAILED || buf != (uint8_t *)DT_BUF || prog != (uint8_t *)DT_PROG) { fprintf(stderr, "ref_x86: mmap failed\n"); return 1; }
    // (signals run on a stack of their own: the test's RSP points into the test memory)
    static uint8_t altstack[65536];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, NULL);
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER | SA_ONSTACK;
    sigaction(SIGFPE, &sa, NULL); sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    static dt_case c;
    static dt_result r;
    while (fread(&c, sizeof c, 1, stdin) == 1) {
        // Rosetta 2 keeps translations of code it has run: the pages are made writable (and executable again
        // afterwards), so it sees the new instruction instead of reusing the last one's translation.
        mprotect(stub, 4096, PROT_READ | PROT_WRITE);
        mprotect(prog, DT_PROG_MAX + 4096, PROT_READ | PROT_WRITE);
        // The loading stub: host registers saved, the state loaded, then the instruction (and the storing stub after
        // it), or a jump to the program at DT_PROG (and the storing stub at DT_END, where the program ends).
        code = stub; at = 0;
        b(0x53); b(0x55); b(0x41); b(0x54); b(0x41); b(0x55); b(0x41); b(0x56); b(0x41); b(0x57);   // push rbx rbp r12-r15
        mov_rax_imm((uint64_t)&host_rsp); b(0x48); b(0x89); b(0x20);                              // mov [rax], rsp
        mov_rax_imm((uint64_t)&st_in);
        b(0xFF); b(0xB0); d32(128); b(0x9D);                                                      // push [rax+128]; popfq
        for (int reg = 1; reg < 16; reg++) ld(reg, reg * 8);
        for (int x = 0; x < 16; x++) {                                                            // movdqu xmmN, [rax+off]
            b(0xF3); if (x >= 8) b(0x44); b(0x0F); b(0x6F); b(0x80 | (x & 7) << 3); d32((uint32_t)(DT_XMM + 16 * x));
        }
        for (int x = 0; x < 16; x++) {                                                            // vinsertf128 ymmN, ymmN, [rax+off], 1
            b(0xC4); b((uint8_t)((x < 8 ? 0x80 : 0) | 0x63)); b((uint8_t)((~x & 15) << 3 | 5)); b(0x18);
            b(0x80 | (x & 7) << 3); d32((uint32_t)(DT_YMMH + 16 * x)); b(1);
        }
        b(0x0F); b(0xAE); b(0x90); d32(DT_MXCSR);                                                 // ldmxcsr [rax+off]
        b(0x48); b(0x8B); b(0x00);                                                                // mov rax, [rax]
        if (c.kind == 1) {
            memcpy(prog, c.prog, c.prog_len);
            b(0xFF); b(0x25); d32(0); q(DT_PROG);                                                 // jmp [rip+0] -> DT_PROG
            code = prog; at = DT_PROG_MAX;
        } else {
            memcpy(code + at, c.code, c.len); at += c.len;                                        // the instruction
        }
        b(0x48); b(0xA3); q((uint64_t)&st_out.gpr[0]);                                            // mov [moffs], rax
        b(0x9C);                                                                                  // pushfq (test stack)
        mov_rax_imm((uint64_t)&st_out);
        b(0x8F); b(0x80); d32(128);                                                               // pop [rax+128]
        for (int reg = 1; reg < 16; reg++) st(reg, reg * 8);
        for (int x = 0; x < 16; x++) {                                                            // movdqu [rax+off], xmmN
            b(0xF3); if (x >= 8) b(0x44); b(0x0F); b(0x7F); b(0x80 | (x & 7) << 3); d32((uint32_t)(DT_XMM + 16 * x));
        }
        for (int x = 0; x < 16; x++) {                                                            // vextractf128 [rax+off], ymmN, 1
            b(0xC4); b((uint8_t)((x < 8 ? 0x80 : 0) | 0x63)); b(0x7D); b(0x19);
            b(0x80 | (x & 7) << 3); d32((uint32_t)(DT_YMMH + 16 * x)); b(1);
        }
        b(0xC5); b(0xF8); b(0x77);                                                                // vzeroupper
        b(0x0F); b(0xAE); b(0x98); d32(DT_MXCSR);                                                 // stmxcsr [rax+off]
        mov_rax_imm((uint64_t)&host_rsp); b(0x48); b(0x8B); b(0x20);                              // mov rsp, [rax]
        b(0x41); b(0x5F); b(0x41); b(0x5E); b(0x41); b(0x5D); b(0x41); b(0x5C); b(0x5D); b(0x5B); b(0xC3);
        mprotect(stub, 4096, PROT_READ | PROT_EXEC);
        mprotect(prog, DT_PROG_MAX + 4096, PROT_READ | PROT_EXEC);
        st_in = c.in;
        memset(&st_out, 0, sizeof st_out);
        memcpy(buf, c.mem, DT_BUF_SIZE);
        signaled = 0;
        alarm(2);                                              // (a program that does not end)
        if (!sigsetjmp(jb, 1)) ((void (*)(void))stub)();
        alarm(0);
        r.out = signaled ? st_in : st_out;
        r.out.fault = signaled == SIGFPE ? 1 : signaled == SIGALRM ? 4 : signaled ? 2 : 0;
        memcpy(r.mem, buf, DT_BUF_SIZE);
        if (fwrite(&r, sizeof r, 1, stdout) != 1) return 1;
        fflush(stdout);
    }
    return 0;
}
