// The execution engine (engine.c): translated blocks, linked to each other, with a cache.
#pragma once
#include "xlat.h"

typedef struct xe xe;
typedef struct {
    uint64_t blocks, linked, entries, flushes;      // blocks translated, branches patched, entries from C, cache flushes
    uint64_t x86_bytes, arm_insns;                  // code translated
    uint64_t split_patches;                         // ordered accesses across 16 bytes patched (engine.c)
    uint64_t flags_skipped, fused;                  // instructions whose flags nobody reads, Jccs fused with a CMP / TEST
} xe_stats;

// ordered: memory accesses that may be shared as LDAPR / STLR (in-process, without TSO); installs a SIGBUS / SIGSEGV
// handler for those across 16 bytes (call it after the program's own handlers: others go to them).
xe *xe_new(int ordered);
// Forgets every translation (the code it came from changed).
void xe_flush(xe *e);
// Runs from ctx->rip until the code reaches stop (0), an x86 exception (ctx->fault, RIP at the instruction), or an
// instruction that cannot be translated (-1).
int xe_run(xe *e, x86_ctx *ctx, uint64_t stop);
xe_stats xe_get_stats(const xe *e);
