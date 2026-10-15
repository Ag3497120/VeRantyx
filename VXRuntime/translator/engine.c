// The execution engine (phase 4a): runs x86-64 code translated block by block (xlat_block_linked), with the x86
// registers kept in ARM registers from block to block. Blocks are entered once through a stub that loads the state
// and leave it through one that stores it; between them they jump to each other directly. A direct branch first goes
// through the link stub (to C when its target is not translated yet), and is then patched into a branch to the
// target's code; an indirect one (RET, JMP/CALL r/m) looks its target up in a table the generated code reads.
#include "engine.h"
#include "a64.h"
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { S0 = 16, S1 = 17, T_T = 22, XC = 28, LR = 30 };
enum { CODE_BYTES = 64 << 20, TABLE_BITS = 16, MAP_BITS = 20 };

typedef struct { uint64_t rip, host; } entry;
struct xe {
    uint32_t *code;                 // MAP_JIT: stubs first, then blocks
    size_t used, cap;               // (in instructions)
    uint32_t *enter, *exit, *link, *lookup, *link_call;
    uint64_t *ring;                 // the return-address ring (x86_ctx.ras points into it)
    uint64_t ring_at;               //   where it was when xe_run last returned
    entry *table;                   // read by the lookup stub: direct-mapped
    entry *map;                     // every block: open addressing (rip 0: empty)
    int ordered;
    xe_stats stats;
};

static size_t emit(uint32_t *buf, size_t n, uint32_t insn) { buf[n] = insn; return n + 1; }
static size_t mov64(uint32_t *buf, size_t n, int d, uint64_t v) {
    n = emit(buf, n, a64_movz(d, (uint16_t)v, 0));
    for (int s = 16; s < 64; s += 16) if (v >> s & 0xFFFF) n = emit(buf, n, a64_movk(d, (uint16_t)(v >> s), s));
    return n;
}
static void writable(int w) { pthread_jit_write_protect_np(!w); }
static uint32_t idx(uint64_t rip, int bits) { return (uint32_t)((rip ^ rip >> 14 ^ rip >> 29) & ((1u << bits) - 1)); }

static void stubs(xe *e) {
    xlat_buf b = { 0 };
    uint32_t *c = e->code;
    size_t n = 0;
    // enter(ctx): save the host's registers, load the x86 state, go to ctx->entry
    e->enter = c + n;
    xlat_prologue(&b);
    memcpy(c + n, b.code, b.n * 4); n += b.n; b.n = 0;
    n = emit(c, n, a64_ldr(8, S0, XC, CTX_ENTRY));
    n = emit(c, n, a64_br(S0));
    // exit: S1 is the next RIP; store the state, back to the host
    e->exit = c + n;
    n = emit(c, n, a64_str(8, S1, XC, CTX_RIP));
    xlat_epilogue(&b);
    memcpy(c + n, b.code, b.n * 4); n += b.n; b.n = 0;
    free(b.code);
    // lookup: S1 is the target; its code from the table, else exit (ctx->link 0: nothing to patch)
    e->lookup = c + n;
    n = emit(c, n, a64_str(8, XZR, XC, CTX_LINK));
    uint32_t *body = c + n;
    n = emit(c, n, a64_eor_lsr(S0, S1, S1, 14));
    n = emit(c, n, a64_eor_lsr(S0, S0, S1, 29));
    n = emit(c, n, a64_ubfx(S0, S0, 0, TABLE_BITS));
    n = mov64(c, n, LR, (uint64_t)(uintptr_t)e->table);
    n = emit(c, n, a64_add_lsl(LR, LR, S0, 4));
    n = emit(c, n, a64_ldp(T_T, LR, LR, 0));
    n = emit(c, n, a64_cmp(T_T, S1));
    n = emit(c, n, a64_bcond(NE, (int32_t)(e->exit - (c + n))));
    n = emit(c, n, a64_br(LR));
    // link: entered by BL from a site, the target in the 8 bytes after it; ctx->link = the site, then as lookup
    e->link = c + n;
    n = emit(c, n, a64_ldr(8, S1, LR, 0));
    n = emit(c, n, a64_subi(LR, LR, 4));
    n = emit(c, n, a64_str(8, LR, XC, CTX_LINK));
    n = emit(c, n, a64_b((int32_t)(body - (c + n))));
    // call-link: entered by BL from a call site (LR: the site + 4, the target 12 bytes on); the same
    e->link_call = c + n;
    n = emit(c, n, a64_ld_off(8, S1, LR, 12));
    n = emit(c, n, a64_subi(T_T, LR, 4));
    n = emit(c, n, a64_str(8, T_T, XC, CTX_LINK));
    n = emit(c, n, a64_b((int32_t)(body - (c + n))));
    e->used = n;
}
// Forgets the ring's entries (their code is gone): x86 address ~0, which no return matches.
static void ring_clear(xe *e) { for (size_t i = 0; i < (1u << XLAT_RAS_BITS) / 8; i += 2) { e->ring[i] = ~0ull; e->ring[i + 1] = 0; } }

// Ordered accesses across 16 bytes: LDAPR / STLR fault there (LSE2 allows other misaligned ones). The first such
// fault patches the instruction into a branch to a few instructions of its own: DMB ISH, the plain access, DMB ISH,
// back. (FEX-Emu handles x86 ordering in-process the same way.) Other signals go to the handler before ours.
static xe *fault_engine;
static struct sigaction prev_bus, prev_segv;
static void on_fault(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    xe *e = fault_engine;
    uint32_t *pc = (uint32_t *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
    if (e && pc >= e->code && pc < e->code + e->used && e->used + 8 < e->cap) {
        uint32_t insn = *pc, sz = insn >> 30, t = insn & 31, n = insn >> 5 & 31;
        int load = (insn & 0x3FFFFC00u) == 0x38BFC000u, store = (insn & 0x3FFFFC00u) == 0x089FFC00u;
        if (load || store) {
            static const int bytes[4] = { 1, 2, 4, 8 };
            uint32_t *stub = e->code + e->used;
            writable(1);
            stub[0] = 0xD5033BBFu;                                        // DMB ISH
            stub[1] = load ? a64_ldr(bytes[sz], (int)t, (int)n, 0) : a64_str(bytes[sz], (int)t, (int)n, 0);
            stub[2] = 0xD5033BBFu;
            stub[3] = a64_b((int32_t)(pc + 1 - (stub + 3)));
            *pc = a64_b((int32_t)(stub - pc));
            writable(0);
            sys_icache_invalidate(stub, 16); sys_icache_invalidate(pc, 4);
            e->used += 4; e->stats.split_patches++;
            return;                                                      // (again: through the patch)
        }
    }
    struct sigaction *p = sig == SIGBUS ? &prev_bus : &prev_segv;
    if (p->sa_flags & SA_SIGINFO) p->sa_sigaction(sig, si, uc_);
    else if (p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN) p->sa_handler(sig);
    else { signal(sig, SIG_DFL); raise(sig); }
}

xe *xe_new(int ordered) {
    xe *e = calloc(1, sizeof *e);
    e->code = mmap(NULL, CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (e->code == MAP_FAILED) { free(e); return NULL; }
    e->cap = CODE_BYTES / 4;
    e->table = calloc(1u << TABLE_BITS, sizeof *e->table);
    e->map = calloc(1u << MAP_BITS, sizeof *e->map);
    e->ordered = ordered;
    if (posix_memalign((void **)&e->ring, 1u << XLAT_RAS_BITS, 1u << XLAT_RAS_BITS)) { free(e); return NULL; }
    ring_clear(e);
    e->ring_at = (uint64_t)(uintptr_t)e->ring;
    writable(1); stubs(e); writable(0);
    sys_icache_invalidate(e->code, e->used * 4);
    if (ordered) {
        fault_engine = e;
        struct sigaction sa = { 0 };
        sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGBUS, &sa, &prev_bus); sigaction(SIGSEGV, &sa, &prev_segv);
    }
    return e;
}

void xe_flush(xe *e) {
    ring_clear(e);
    memset(e->table, 0, (sizeof *e->table) << TABLE_BITS);
    memset(e->map, 0, (sizeof *e->map) << MAP_BITS);
    writable(1); stubs(e); writable(0);
    sys_icache_invalidate(e->code, e->used * 4);
}

static uint64_t find(xe *e, uint64_t rip) {
    for (uint32_t i = idx(rip, MAP_BITS); e->map[i].rip; i = (i + 1) & ((1u << MAP_BITS) - 1))
        if (e->map[i].rip == rip) return e->map[i].host;
    return 0;
}

// The block at rip, translated now if it is not yet. 0: it cannot be.
static uint64_t block(xe *e, uint64_t rip, uint64_t stop) {
    uint64_t host = find(e, rip);
    if (host) return host;
    xlat_buf b = { 0 };
    b.ordered = e->ordered; b.stop = stop;
    uint64_t end;
    if (xlat_block_linked(&b, rip, 64, &end) < 0) { free(b.code); free(b.relocs); return 0; }
    if (e->used + b.n > e->cap || e->stats.blocks + 1 >= (1u << (MAP_BITS - 1))) {   // (full: start over)
        xe_flush(e);
        e->stats.flushes++;
    }
    uint32_t *at = e->code + e->used;
    writable(1);
    memcpy(at, b.code, b.n * 4);
    for (size_t i = 0; i < b.nrelocs; i++) {
        uint32_t *p = at + b.relocs[i].at;
        if (b.relocs[i].kind == XR_LOOKUP) *p = a64_b((int32_t)(e->lookup - p));
        else if (b.relocs[i].kind == XR_EXIT) *p = a64_b((int32_t)(e->exit - p));
        else if (b.relocs[i].kind == XR_LOOKUP_CALL) *p = a64_bl((int32_t)(e->lookup - p));
        else if (b.relocs[i].kind == XR_CALL) {                          // (the return's site, 3 words, then the target)
            uint64_t target = (uint64_t)p[4] | (uint64_t)p[5] << 32, t = find(e, target);
            *p = a64_bl(t ? (int32_t)((uint32_t *)(uintptr_t)t - p) : (int32_t)(e->link_call - p));
            if (t) e->stats.linked++;
        } else {
            uint64_t target = (uint64_t)p[1] | (uint64_t)p[2] << 32, t = find(e, target);
            *p = t ? a64_b((int32_t)((uint32_t *)(uintptr_t)t - p)) : a64_bl((int32_t)(e->link - p));
            if (t) e->stats.linked++;
        }
    }
    writable(0);
    sys_icache_invalidate(at, b.n * 4);
    static FILE *dump; static int dump_checked;                         // (XE_DUMP=<file>: each block's x86 range and code)
    if (!dump_checked) { dump_checked = 1; const char *p = getenv("XE_DUMP"); if (p) dump = fopen(p, "w"); }
    if (dump) {
        fprintf(dump, "block %llx-%llx at %p:", (unsigned long long)rip, (unsigned long long)end, (void *)at);
        for (size_t i = 0; i < b.n; i++) fprintf(dump, " %08x", at[i]);
        fprintf(dump, "\n"); fflush(dump);
    }
    e->used += b.n;
    host = (uint64_t)(uintptr_t)at;
    uint32_t i = idx(rip, MAP_BITS);
    while (e->map[i].rip) i = (i + 1) & ((1u << MAP_BITS) - 1);
    e->map[i].rip = rip; e->map[i].host = host;
    e->table[idx(rip, TABLE_BITS)] = (entry){ rip, host };
    e->stats.blocks++; e->stats.x86_bytes += end - rip; e->stats.arm_insns += b.n;
    e->stats.flags_skipped += (uint64_t)b.nskipped; e->stats.fused += (uint64_t)b.nfused;
    free(b.code); free(b.relocs);
    return host;
}

static int run(xe *e, x86_ctx *c, uint64_t stop);
int xe_run(xe *e, x86_ctx *c, uint64_t stop) {
    c->link = 0;
    c->ras = e->ring_at;
    int r = run(e, c, stop);
    e->ring_at = c->ras;
    return r;
}
static int run(xe *e, x86_ctx *c, uint64_t stop) {
    for (;;) {
        if (c->fault) return (int)c->fault;
        if (c->rip == stop) return 0;
        uint64_t host = block(e, c->rip, stop);
        if (!host) return -1;
        if (c->link) {                                                    // (patch the site that came here: a call's stays BL)
            uint32_t *p = (uint32_t *)(uintptr_t)c->link;
            int call = (*p & 0xFC000000u) == 0x94000000u && p + ((int32_t)(*p << 6) >> 6) == e->link_call;
            int32_t words = (int32_t)((uint32_t *)(uintptr_t)host - p);
            writable(1); *p = call ? a64_bl(words) : a64_b(words); writable(0);
            sys_icache_invalidate(p, 4);
            e->stats.linked++;
            c->link = 0;
        }
        e->stats.entries++;
        c->entry = host;
        ((void (*)(x86_ctx *))(void *)e->enter)(c);
    }
}

xe_stats xe_get_stats(const xe *e) { return e->stats; }
