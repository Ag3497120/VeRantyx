// The execution engine (phase 4a): runs x86-64 code translated block by block (xlat_block_linked), with the x86
// registers kept in ARM registers from block to block. Blocks are entered once through a stub that loads the state
// and leave it through one that stores it; between them they jump to each other directly. A direct branch first goes
// through the link stub (to C when its target is not translated yet), and is then patched into a branch to the
// target's code; an indirect one (RET, JMP/CALL r/m) looks its target up in a table the generated code reads.
#include "engine.h"
#include "vcpu.h"
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
typedef struct { uint64_t rip, host; uint32_t id; } mentry;      // (map: host 0 with the rip kept, a block gone)
// A translated block: its x86 range, its code, and the branches patched to go to it (code index << 1 | call).
typedef struct { uint64_t rip, end, host; uint32_t *sites; uint32_t nsites, csites; int dead; } blk;
// A host page (16 KB) translated code came from, and the blocks made from it (SMC: code that changes).
enum { PAGE = 16384, PAGE_BITS = 14, PAGES = 1 << 14 };
typedef struct { uint64_t page; uint32_t *ids; uint32_t n, c; uint32_t faults; int protected, unprotected; } page_rec;
struct xe {
    uint32_t *code;                 // MAP_JIT: stubs first, then blocks
    size_t used, cap;               // (in instructions)
    uint32_t *enter, *exit, *link, *lookup, *link_call;
    uint64_t *ring;                 // the shared return-address ring (contexts without one of their own)
    uint64_t ring_at;               //   where it was when xe_run last returned
    uint64_t **rings; int nrings;   // every ring, cleared when the code goes
    void (*(*native)(uint64_t))(x86_ctx *);
    int (*guest)(uint64_t, uint64_t *, const void **);   // vCPU: a native function guest.c answers (xe_set_guest)
    uint64_t guest_base, guest_end;  //   guest.c's code: where the guest has it, and where it came from
    const uint8_t *guest_from;
    pthread_mutex_t lock;           // translating, linking, flushing (threads share the code)
    struct { uint64_t host, rip; } *imap;   // every translated instruction's code, in order (then rip 0: a block's end)
    size_t nimap, cimap;
    entry *table;                   // read by the lookup stub: direct-mapped
    mentry *map;                    // every block: open addressing (rip 0: empty)
    blk *blocks; size_t nblocks, cblocks;
    page_rec *pages;                // open addressing (page 0: empty)
    int smc;                        // xe_set_smc: write-protect the pages translated code came from
    int vcpu;                       // xe_new_vcpu: the code runs in a Hypervisor.framework guest (vcpu.c)
    uint32_t *vectors;              //   its exception vectors (each HVC #0x10 + n)
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

// vCPU: guest.c's code wrote to a page the SMC watch protects (memcpy into data beside the program's code): the page's
// code is forgotten as for the program's own write, and the write goes again (1).
static xe *g_vcpu_engine;
static int guest_write(uint64_t pc, uint64_t addr) {
    xe *e = g_vcpu_engine;
    return pc >= e->guest_base && pc < e->guest_end && xe_smc_write(e, addr);
}
static void stubs(xe *e) {
    xlat_buf b = { 0 };
    uint32_t *c = e->code;
    size_t n = 0;
    if (e->vcpu) {
        // In the guest (vcpu.c): enter with x0 = the context, exit with HVC #1 (the state stored)
        e->enter = c + n; xlat_guest_enter(&b); memcpy(c + n, b.code, b.n * 4); n += b.n; b.n = 0;
        e->exit = c + n; xlat_guest_exit(&b); memcpy(c + n, b.code, b.n * 4); n += b.n; b.n = 0;
    } else {
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
    }
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
    if (e->vcpu) {                                                        // the guest's exception vectors: HVC #0x10 + n
        n = (n + 511) & ~(size_t)511;                                     // (2 KB aligned, 16 entries of 128 bytes)
        e->vectors = c + n;
        for (int i = 0; i < 16; i++) { c[n + 32 * (size_t)i] = a64_hvc(XV_HVC_VECTOR + i); c[n + 32 * (size_t)i + 1] = a64_b(-1); }
        n += 512;
        xv_set_tlbi((uint64_t)(uintptr_t)(c + n));                      // TLBI VMALLE1; DSB ISH; ISB; HVC #4
        n = emit(c, n, 0xD508871Fu); n = emit(c, n, 0xD5033B9Fu); n = emit(c, n, 0xD5033FDFu); n = emit(c, n, a64_hvc(XV_HVC_TLBI));
        const uint8_t *gc; size_t gsize;                                   // guest.c's code (the section as it is)
        xv_guest_code(&gc, &gsize);
        if (gc && gsize) {
            n = (n + 3) & ~(size_t)3;
            memcpy(c + n, gc, gsize);
            e->guest_base = (uint64_t)(uintptr_t)(c + n); e->guest_from = gc;
            n += (gsize + 3) / 4;
            e->guest_end = (uint64_t)(uintptr_t)(c + n);
            g_vcpu_engine = e;
            xv_set_write_hook(guest_write);
        }
    }
    e->used = n;
}
// Forgets the rings' entries (their code is gone): x86 address ~0, which no return matches.
static void ring_clear(uint64_t *ring) { for (size_t i = 0; i < (1u << XLAT_RAS_BITS) / 8; i += 2) { ring[i] = ~0ull; ring[i + 1] = 0; } }
static uint64_t *ring_alloc(xe *e) {
    uint64_t *r;
    if (posix_memalign((void **)&r, 1u << XLAT_RAS_BITS, 1u << XLAT_RAS_BITS)) return NULL;
    ring_clear(r);
    e->rings = realloc(e->rings, (size_t)(e->nrings + 1) * sizeof *e->rings);
    e->rings[e->nrings++] = r;
    return r;
}
uint64_t xe_ring_new(xe *e) {
    pthread_mutex_lock(&e->lock);
    uint64_t *r = ring_alloc(e);
    pthread_mutex_unlock(&e->lock);
    return (uint64_t)(uintptr_t)r;
}
void xe_set_native(xe *e, void (*(*find)(uint64_t))(x86_ctx *)) { e->native = find; }
void xe_set_guest(xe *e, int (*find)(uint64_t, uint64_t *, const void **)) { e->guest = find; }
// A table entry, both words at once (with LSE2 an aligned STP is single-copy atomic): another thread's lookup stub
// sees the old entry or the new one.
static void put_entry(entry *t, uint64_t rip, uint64_t host) { __asm__ volatile("stp %1, %2, [%0]" :: "r"(t), "r"(rip), "r"(host) : "memory"); }

// Ordered accesses across 16 bytes: LDAPR / STLR fault there (LSE2 allows other misaligned ones). The first such
// fault patches the instruction into a branch to a few instructions of its own: DMB ISH, the plain access, DMB ISH,
// back. (FEX-Emu handles x86 ordering in-process the same way.) Other signals go to the handler before ours.
uint64_t xe_rip_at(const xe *e, uint64_t pc);
static xe *fault_engine;
static struct sigaction prev_bus, prev_segv;
int xe_fix_split(xe *e, void *uc_) {
    ucontext_t *uc = uc_;
    uint32_t *pc = (uint32_t *)(uintptr_t)uc->uc_mcontext->__ss.__pc;
    if (!e || pc < e->code || pc >= e->code + e->used) return 0;
    uint32_t insn = *pc, sz = insn >> 30, t = insn & 31, n = insn >> 5 & 31;
    int load = (insn & 0x3FFFFC00u) == 0x38BFC000u, store = (insn & 0x3FFFFC00u) == 0x089FFC00u;
    if (!load && !store) return 0;
    pthread_mutex_lock(&e->lock);                                         // (other threads translate meanwhile)
    if (*pc != insn) { pthread_mutex_unlock(&e->lock); return 1; }       // (patched by another thread already)
    if (e->used + 8 >= e->cap) { pthread_mutex_unlock(&e->lock); return 0; }
    static const int bytes[4] = { 1, 2, 4, 8 };
    uint32_t *stub = e->code + e->used;
    uint64_t rip = xe_rip_at(e, (uint64_t)(uintptr_t)pc);
    writable(1);
    stub[0] = 0xD5033BBFu;                                                // DMB ISH
    stub[1] = load ? a64_ldr(bytes[sz], (int)t, (int)n, 0) : a64_str(bytes[sz], (int)t, (int)n, 0);
    stub[2] = 0xD5033BBFu;
    stub[3] = a64_b((int32_t)(pc + 1 - (stub + 3)));
    *pc = a64_b((int32_t)(stub - pc));
    writable(0);
    sys_icache_invalidate(stub, 16); sys_icache_invalidate(pc, 4);
    if (e->nimap + 2 < e->cimap) {                                        // (a fault in the stub: still that instruction's)
        e->imap[e->nimap].host = (uint64_t)(uintptr_t)stub; e->imap[e->nimap].rip = rip;
        e->imap[e->nimap + 1].host = (uint64_t)(uintptr_t)(stub + 4); e->imap[e->nimap + 1].rip = 0;
        __atomic_store_n(&e->nimap, e->nimap + 2, __ATOMIC_RELEASE);
    }
    e->used += 4; e->stats.split_patches++;
    pthread_mutex_unlock(&e->lock);
    return 1;                                                             // (again: through the patch)
}
static void on_fault(int sig, siginfo_t *si, void *uc_) {
    if (xe_fix_split(fault_engine, uc_)) return;
    struct sigaction *p = sig == SIGBUS ? &prev_bus : &prev_segv;
    if (p->sa_flags & SA_SIGINFO) p->sa_sigaction(sig, si, uc_);
    else if (p->sa_handler != SIG_DFL && p->sa_handler != SIG_IGN) p->sa_handler(sig);
    else { signal(sig, SIG_DFL); raise(sig); }
}

static xe *make(int ordered, int vcpu);
xe *xe_new(int ordered) { return make(ordered, 0); }
xe *xe_new_vcpu(void) { return xv_init() ? make(0, 1) : NULL; }   // (the guest is TSO: plain accesses)
static xe *make(int ordered, int vcpu) {
    xe *e = calloc(1, sizeof *e);
    e->vcpu = vcpu;
    // (vcpu: plain memory the guest runs; the host never runs it, and an executable host page must not go into a VM:
    //  macOS panics, SPTM)
    e->code = vcpu ? mmap(NULL, CODE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
                   : mmap(NULL, CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (e->code == MAP_FAILED) { free(e); return NULL; }
    e->cap = CODE_BYTES / 4;
    e->table = calloc(1u << TABLE_BITS, sizeof *e->table);
    e->map = calloc(1u << MAP_BITS, sizeof *e->map);
    e->pages = calloc(PAGES, sizeof *e->pages);
    e->ordered = ordered;
    pthread_mutex_init(&e->lock, NULL);
    e->cimap = CODE_BYTES / 8;                                            // (reserved: read without the lock by fault handlers)
    e->imap = mmap(NULL, e->cimap * sizeof *e->imap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (!(e->ring = ring_alloc(e))) { free(e); return NULL; }
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
    e->nimap = 0;
    for (size_t i = 0; i < e->nblocks; i++) free(e->blocks[i].sites);
    e->nblocks = 0;
    for (uint32_t i = 0; i < PAGES; i++) {
        if (e->pages[i].protected) mprotect((void *)(uintptr_t)e->pages[i].page, PAGE, PROT_READ | PROT_WRITE);
        free(e->pages[i].ids);
    }
    memset(e->pages, 0, PAGES * sizeof *e->pages);
    for (int i = 0; i < e->nrings; i++) ring_clear(e->rings[i]);
    memset(e->table, 0, (sizeof *e->table) << TABLE_BITS);
    memset(e->map, 0, (sizeof *e->map) << MAP_BITS);
    writable(1); stubs(e); writable(0);
    sys_icache_invalidate(e->code, e->used * 4);
}

static mentry *find_entry(xe *e, uint64_t rip) {
    for (uint32_t i = idx(rip, MAP_BITS); e->map[i].rip; i = (i + 1) & ((1u << MAP_BITS) - 1))
        if (e->map[i].rip == rip && e->map[i].host) return &e->map[i];
    return NULL;
}
// A branch at p now goes to block id: remembered, to be put back to its stub if the block goes.
static void add_site(xe *e, uint32_t id, uint32_t *p, int call) {
    blk *b = &e->blocks[id];
    if (b->nsites == b->csites) { b->csites = b->csites ? 2 * b->csites : 4; b->sites = realloc(b->sites, b->csites * sizeof *b->sites); }
    b->sites[b->nsites++] = (uint32_t)(p - e->code) << 1 | (uint32_t)call;
}

// ---- code that changes (SMC) ------------------------------------------------------------------------------------
// The pages translated code came from are made read-only; a write there (xe_smc_write, from the fault handler)
// forgets every block made from the page and gives the page back. A page written again and again (data next to
// code) is left writable after a while, its translations no longer watched.
static page_rec *page_at(xe *e, uint64_t page, int make) {
    for (uint32_t i = (uint32_t)((page >> PAGE_BITS) * 0x9E3779B1u) & (PAGES - 1), n = 0; n < PAGES; i = (i + 1) & (PAGES - 1), n++) {
        if (e->pages[i].page == page) return &e->pages[i];
        if (!e->pages[i].page) { if (!make) return NULL; e->pages[i].page = page; return &e->pages[i]; }
    }
    return NULL;
}
static void forget_block(xe *e, uint32_t id) {
    blk *b = &e->blocks[id];
    if (b->dead) return;
    b->dead = 1;
    mentry *m = find_entry(e, b->rip);
    if (m && m->host == b->host) m->host = 0;
    entry *t = &e->table[idx(b->rip, TABLE_BITS)];
    if (t->rip == b->rip && t->host == b->host) put_entry(t, 0, 0);
    writable(1);
    for (uint32_t k = 0; k < b->nsites; k++) {                            // (branches to it: to their stubs again)
        uint32_t *p = e->code + (b->sites[k] >> 1);
        *p = (b->sites[k] & 1) ? a64_bl((int32_t)(e->link_call - p)) : a64_bl((int32_t)(e->link - p));
        sys_icache_invalidate(p, 4);
    }
    writable(0);
    b->nsites = 0;
}
static void watch_block(xe *e, uint32_t id) {
    blk *b = &e->blocks[id];
    for (uint64_t pg = b->rip & ~(uint64_t)(PAGE - 1); pg < b->end; pg += PAGE) {
        page_rec *r = page_at(e, pg, 1);
        if (!r) return;
        if (r->n == r->c) { r->c = r->c ? 2 * r->c : 8; r->ids = realloc(r->ids, r->c * sizeof *r->ids); }
        r->ids[r->n++] = id;
        if (!r->protected && !r->unprotected && !mprotect((void *)(uintptr_t)pg, PAGE, PROT_READ)) {
            r->protected = 1;
            if (e->vcpu) xv_protect(pg, 1);                               // (the guest's writes fault on its own page tables)
        }
    }
}
int xe_smc_write(xe *e, uint64_t addr) {
    if (!e || !e->smc) return 0;
    uint64_t pg = addr & ~(uint64_t)(PAGE - 1);
    pthread_mutex_lock(&e->lock);
    page_rec *r = page_at(e, pg, 0);
    if (!r) { pthread_mutex_unlock(&e->lock); return 0; }
    if (!r->protected) { pthread_mutex_unlock(&e->lock); return 1; }      // (another thread's write gave it back just now)
    for (uint32_t k = 0; k < r->n; k++) forget_block(e, r->ids[k]);
    r->n = 0;
    r->protected = 0;
    if (++r->faults == 16) r->unprotected = 1;                          // (written often: its blocks check their bytes)
    for (int i = 0; i < e->nrings; i++) ring_clear(e->rings[i]);
    mprotect((void *)(uintptr_t)pg, PAGE, PROT_READ | PROT_WRITE);
    if (e->vcpu) xv_protect(pg, 0);
    e->stats.smc_pages++;
    pthread_mutex_unlock(&e->lock);
    return 1;
}
void xe_forget(xe *e, uint64_t addr, uint64_t len) {
    if (!e || !e->smc || !len) return;
    pthread_mutex_lock(&e->lock);
    for (uint64_t pg = addr & ~(uint64_t)(PAGE - 1); pg < addr + len; pg += PAGE) {
        page_rec *r = page_at(e, pg, 0);
        if (!r) continue;
        for (uint32_t k = 0; k < r->n; k++) forget_block(e, r->ids[k]);
        r->n = 0;
        if (r->protected) mprotect((void *)(uintptr_t)pg, PAGE, PROT_READ | PROT_WRITE);
        r->protected = 0;
    }
    pthread_mutex_unlock(&e->lock);
}
void xe_set_smc(xe *e, int on) { e->smc = on; }

// The block at rip, translated now if it is not yet: its id (-1: it cannot be). once: the instruction at rip
// alone, not kept (not found by others, no page watched).
static int64_t block_ex(xe *e, uint64_t rip, uint64_t stop, int once) {
    mentry *found = once ? NULL : find_entry(e, rip);
    if (found) return found->id;
    xlat_buf b = { 0 };
    b.ordered = e->ordered; b.stop = stop; b.vcpu = e->vcpu;
    uint64_t end = rip + 1;
    void (*thunk)(x86_ctx *) = e->native ? e->native(rip) : NULL;
    uint64_t handler = 0; const void *desc = NULL;
    if (thunk && e->vcpu && e->guest && e->guest_base && e->guest(rip, &handler, &desc))
        xlat_block_native_defer(&b, thunk, (uint64_t)(uintptr_t)desc, e->guest_base + (handler - (uint64_t)(uintptr_t)e->guest_from));
    else if (thunk) xlat_block_native(&b, thunk);
    else {
        if (e->smc) {                                                     // (a page written often: a checked block)
            page_rec *r = page_at(e, rip & ~(uint64_t)(PAGE - 1), 0), *r2 = page_at(e, (rip + 1024) & ~(uint64_t)(PAGE - 1), 0);
            b.check_bytes = (r && r->unprotected) || (r2 && r2->unprotected);
        }
        if (xlat_block_linked(&b, rip, once ? 1 : 64, &end) < 0) { free(b.code); free(b.relocs); free(b.imap); return -1; }
    }
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
            mentry *t = find_entry(e, (uint64_t)p[4] | (uint64_t)p[5] << 32);
            *p = a64_bl(t ? (int32_t)((uint32_t *)(uintptr_t)t->host - p) : (int32_t)(e->link_call - p));
            if (t) { e->stats.linked++; add_site(e, t->id, p, 1); }
        } else {
            mentry *t = find_entry(e, (uint64_t)p[1] | (uint64_t)p[2] << 32);
            *p = t ? a64_b((int32_t)((uint32_t *)(uintptr_t)t->host - p)) : a64_bl((int32_t)(e->link - p));
            if (t) { e->stats.linked++; add_site(e, t->id, p, 0); }
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
    uint64_t host = (uint64_t)(uintptr_t)at;
    if (e->nblocks == e->cblocks) { e->cblocks = e->cblocks ? 2 * e->cblocks : 1024; e->blocks = realloc(e->blocks, e->cblocks * sizeof *e->blocks); }
    uint32_t id = (uint32_t)e->nblocks++;
    e->blocks[id] = (blk){ rip, end, host, NULL, 0, 0, 0 };
    if (!once) {
        uint32_t i = idx(rip, MAP_BITS);
        while (e->map[i].rip) i = (i + 1) & ((1u << MAP_BITS) - 1);
        e->map[i].rip = rip; e->map[i].host = host; e->map[i].id = id;
        if (e->smc && !thunk) watch_block(e, id);
    }
    if (!once) put_entry(&e->table[idx(rip, TABLE_BITS)], rip, host);
    for (size_t k = 0; k < b.nimap && e->nimap + 1 < e->cimap; k++) { e->imap[e->nimap].host = host + 4 * b.imap[k].at; e->imap[e->nimap++].rip = b.imap[k].rip; }
    if (e->nimap < e->cimap) { e->imap[e->nimap].host = host + 4 * b.n; e->imap[e->nimap].rip = 0; __atomic_store_n(&e->nimap, e->nimap + 1, __ATOMIC_RELEASE); }
    free(b.imap);
    e->stats.blocks++; e->stats.x86_bytes += end - rip; e->stats.arm_insns += b.n;
    e->stats.flags_skipped += (uint64_t)b.nskipped; e->stats.fused += (uint64_t)b.nfused;
    free(b.code); free(b.relocs);
    return id;
}

static int run(xe *e, x86_ctx *c, uint64_t stop);
int xe_run(xe *e, x86_ctx *c, uint64_t stop) {
    c->link = 0;
    if (c->ras) return run(e, c, stop);                                   // (a ring of its own)
    c->ras = e->ring_at;
    int r = run(e, c, stop);
    e->ring_at = c->ras;
    c->ras = 0;
    return r;
}
static int run(xe *e, x86_ctx *c, uint64_t stop) {
    for (;;) {
        if (c->fault) return (int)c->fault;
        if (c->rip == stop) return 0;
        pthread_mutex_lock(&e->lock);
        int once = c->single != 0;
        c->single = 0;
        int64_t id = block_ex(e, c->rip, stop, once);
        if (once) c->link = 0;                                            // (nothing is linked to a one-off block)
        if (id < 0) { pthread_mutex_unlock(&e->lock); return -1; }
        uint64_t host = e->blocks[id].host;
        if (c->link) {                                                    // (patch the site that came here: a call's stays BL)
            uint32_t *p = (uint32_t *)(uintptr_t)c->link;
            int call = (*p & 0xFC000000u) == 0x94000000u && p + ((int32_t)(*p << 6) >> 6) == e->link_call;
            int32_t words = (int32_t)((uint32_t *)(uintptr_t)host - p);
            writable(1); *p = call ? a64_bl(words) : a64_b(words); writable(0);
            add_site(e, (uint32_t)id, p, call);
            sys_icache_invalidate(p, 4);
            e->stats.linked++;
            c->link = 0;
        }
        pthread_mutex_unlock(&e->lock);
        e->stats.entries++;
        c->entry = host;
        if (e->vcpu) {
            int r = xv_enter(c, (uint64_t)(uintptr_t)e->enter, (uint64_t)(uintptr_t)e->vectors);
            if (r == XV_FAULT) {                                          // (an access fault of the program's: its x86 state)
                uint64_t pc, addr; int write;
                xv_fault(&pc, &addr, &write);
                uint64_t rip = xe_rip_at(e, pc);
                if (!rip) { fprintf(stderr, "[vxrt] engine: a fault at %#llx in no x86 instruction's code (address %#llx%s)\n", (unsigned long long)pc, (unsigned long long)addr, write ? ", a write" : ""); return -2; }
                c->rip = rip;
                if (write && xe_smc_write(e, addr)) c->single = 1;            // (code it had run: on at the write, alone)
                else { c->fault = X86_FAULT_PF; c->fault_info = addr | (uint64_t)write << 63; }
            } else if (r) return -2;
        }
        else ((void (*)(x86_ctx *))(void *)e->enter)(c);
        if (c->stale) {                                                   // (a checked block found its code changed)
            c->stale = 0;
            pthread_mutex_lock(&e->lock);
            mentry *m = find_entry(e, c->rip);
            if (m) forget_block(e, m->id);
            pthread_mutex_unlock(&e->lock);
        }
    }
}

xe_stats xe_get_stats(const xe *e) { return e->stats; }
uint64_t xe_rip_at(const xe *e, uint64_t pc) {
    size_t n = __atomic_load_n(&e->nimap, __ATOMIC_ACQUIRE), lo = 0, hi = n;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (e->imap[mid].host <= pc) lo = mid + 1; else hi = mid; }
    return lo ? e->imap[lo - 1].rip : 0;
}
int xe_owns(const xe *e, uint64_t pc) { return e && pc >= (uint64_t)(uintptr_t)e->code && pc < (uint64_t)(uintptr_t)(e->code + e->cap); }
