// The execution engine (engine.c): translated blocks, linked to each other, with a cache.
#pragma once
#include "xlat.h"

typedef struct xe xe;
typedef struct {
    uint64_t blocks, linked, entries, flushes;      // blocks translated, branches patched, entries from C, cache flushes
    uint64_t x86_bytes, arm_insns;                  // code translated
    uint64_t split_patches;                         // ordered accesses across 16 bytes patched (engine.c)
    uint64_t flags_skipped, fused;                  // instructions whose flags nobody reads, Jccs fused with a CMP / TEST
    uint64_t smc_pages;                             // pages whose translations went because the code was written
} xe_stats;

// ordered: memory accesses that may be shared as LDAPR / STLR (in-process, without TSO); installs a SIGBUS / SIGSEGV
// handler for those across 16 bytes (call it after the program's own handlers: others go to them).
xe *xe_new(int ordered);
// Addresses that are native functions translated code may call (the ARM64 runtime's thunks, src/arm64.c): find
// returns the thunk for one, else NULL; the engine then makes that address's block a call of the thunk.
void xe_set_native(xe *e, void (*(*find)(uint64_t rip))(x86_ctx *));
// A return-address ring for a thread's context (x86_ctx.ras); without one, contexts share the engine's.
uint64_t xe_ring_new(xe *e);
// The x86 instruction whose translation holds host address pc (0: none, or not an x86 instruction's code).
uint64_t xe_rip_at(const xe *e, uint64_t pc);
// Code that changes: with smc on, the pages translations come from are write-protected; the program's fault
// handler calls xe_smc_write for a write fault (1: it was such a page, its translations are gone and it is
// writable again: retry the write). xe_forget: memory freed or replaced (VirtualFree), its translations go.
void xe_set_smc(xe *e, int on);
int xe_smc_write(xe *e, uint64_t addr);
void xe_forget(xe *e, uint64_t addr, uint64_t len);
// Ordered mode: an LDAPR / STLR across 16 bytes faulted (uc: the signal's ucontext_t); 1: patched (DMB, the plain
// access, DMB), the access is retried. xe_new installs a handler that calls this; a program with its own calls it.
int xe_fix_split(xe *e, void *uc);
// Whether pc is in the engine's translated code.
int xe_owns(const xe *e, uint64_t pc);
// Forgets every translation (the code it came from changed). Not while another thread runs translated code.
void xe_flush(xe *e);
// Runs from ctx->rip until the code reaches stop (0), an x86 exception (ctx->fault, RIP at the instruction), or an
// instruction that cannot be translated (-1).
int xe_run(xe *e, x86_ctx *ctx, uint64_t stop);
xe_stats xe_get_stats(const xe *e);
