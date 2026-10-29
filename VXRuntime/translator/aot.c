// Running statically recompiled modules (aot.h, RECOMP.md). A module's route-B code is the translator's: it keeps the
// x86 state in registers as the engine's blocks do, enters through the module's own prologue (enter) and leaves
// through its own exit stub with the next RIP in ctx->rip. Leaving happens for an address the module has no block
// for: another module's, a native function's (called from here as the engine's native blocks call it), or code
// nobody recompiled, which the engine translates (the fallback: counted, and an error with VXRT_AOT_STRICT=1, as it
// would be where code cannot be generated at run time).
#include "aot.h"
#include <ctype.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#ifdef __aarch64__
void vx_log(const char *fmt, ...);
enum { MAX_MODULES = 64 };
static const xa_desc *g_mod[MAX_MODULES];
static int g_nmod;
static uint64_t g_fallback, g_fallback_rips, g_native, g_entries, g_cfn_calls, g_stubs, g_splits;
int vx_builtin_stub(uint64_t rip, uint64_t *target);   // (src/loader.c)
const char *vx_image_name_at(uint64_t pc);            // (src/loader.c: an image's name with the offset, or NULL)
static int g_strict = -1;
int xa_any(void) { return g_nmod > 0; }

// Where a module comes from. On the Mac, a dylib (dir/<name>-<hash>[.p<k>].dylib, made by tools/aot_build.sh). On
// visionOS an app may load no code made after it was signed, so the modules are linked into it and the app's
// registry (xros/app, generated with the modules: vxrt_aot_static) answers for those same file names.
#if VX_XROS
typedef struct { const char *name; const xa_desc *desc; } xa_static;
extern const xa_static vxrt_aot_static[];          // ({ NULL, NULL } at the end)
static const xa_desc *static_module(const char *path) {
    const char *slash = strrchr(path, '/'), *name = slash ? slash + 1 : path;
    for (const xa_static *m = vxrt_aot_static; m->name; m++) if (!strcasecmp(m->name, name)) return m->desc;
    return NULL;
}
int xa_module_exists(const char *path) { return static_module(path) != NULL; }
static const xa_desc *open_module(const char *path) {
    const xa_desc *d = static_module(path);
    if (!d) vx_log("aot: %s is not linked into this app", path);
    return d;
}
#else
int xa_module_exists(const char *path) { return !access(path, R_OK); }
static const xa_desc *open_module(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { vx_log("aot: %s: %s", path, dlerror()); return NULL; }
    return dlsym(h, "vxrt_aot_module");
}
#endif

int xa_load(const char *path, uint64_t base, uint64_t size, uint64_t hash) {
    const xa_desc *d = open_module(path);
    if (!d) return -1;
    if (!d || d->magic != XA_MAGIC || d->version != XA_VERSION) { vx_log("aot: %s is not a module of this version", path); return -1; }
    if (d->image_base != base || d->image_size != size || d->file_hash != hash) {
        vx_log("aot: %s was made for another image (base %#llx, hash %016llx; this one %#llx, %016llx): not used", path,
               (unsigned long long)d->image_base, (unsigned long long)d->file_hash, (unsigned long long)base, (unsigned long long)hash);
        return -1;
    }
    // (the code made without FPCR.AH carries the NaN fixes; the engine's own code must agree: XE_AFP before it translates)
    if (!(d->flags & XA_AFP)) setenv("XE_AFP", "0", 1);
    if (d->next >= XA_NABS) {
        d->ext[XA_HELPER] = (uint64_t)(uintptr_t)xlat_helper;
        d->ext[XA_SPLIT_LOCK] = (uint64_t)(uintptr_t)xlat_split_lock_addr();
        d->ext[XA_PARITY] = (uint64_t)(uintptr_t)xlat_parity;
    }
    if (g_nmod == MAX_MODULES) return -1;
    g_mod[g_nmod++] = d;
    vx_log("aot: %s: %llu blocks, %llu C functions, %llu KB of code for the image at %#llx (part %d of %d: %#llx-%#llx)", path,
           (unsigned long long)d->nblocks, (unsigned long long)d->ncfns, (unsigned long long)((d->code_end - d->code) >> 10),
           (unsigned long long)base, d->part, d->nparts, (unsigned long long)d->part_lo, (unsigned long long)d->part_hi);
    return 0;
}

// The arena: 512 MB at 0xf00000000 (VXRT_AOT_ARENA=<hex>), where modules' images are made for (vxrt-aot with
// VXRT_AOT_BASE: the exe relocated too), reserved at start. Below 0x1000000000 (a macOS 14 process's end) the system
// has already placed randomized regions by main (malloc's 128 MB chunks, two 12 GB ones, the runtime's own dylibs near
// 0x140000000 as ASLR falls); this range is free about 9 times in 10, and main starts again when it is not.
// An image whose base is in the arena is mapped over the reservation.
static uint64_t g_arena_lo, g_arena_hi;
int xa_reserve(void) {
    if (g_arena_hi) return 1;                                  // (visionOS: the app reserves it first, xros/app)
    if (getenv("VXRT_AOT_MAPDUMP")) {
        mach_vm_address_t r = 0x100000000ull; mach_vm_size_t rs; vm_region_basic_info_data_64_t info; mach_port_t o;
        for (;;) {
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
            if (mach_vm_region(mach_task_self(), &r, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &o) || r >= 0x1000000000ull) break;
            if (rs >= (16 << 20)) vx_log("map %#llx-%#llx %llu MB", (unsigned long long)r, (unsigned long long)(r + rs), (unsigned long long)(rs >> 20));
            r += rs;
        }
    }
    mach_vm_address_t a = getenv("VXRT_AOT_ARENA") ? strtoull(getenv("VXRT_AOT_ARENA"), NULL, 16) : 0xf00000000ull;
    mach_vm_size_t n = 0x20000000ull;
    if (mach_vm_allocate(mach_task_self(), &a, n, VM_FLAGS_FIXED) == KERN_SUCCESS) { g_arena_lo = a; g_arena_hi = a + n; return 1; }
    else {
        vx_log("aot: the image arena at %#llx is taken: modules there are not used", (unsigned long long)a);
        mach_vm_address_t r = a; mach_vm_size_t rs; vm_region_basic_info_data_64_t info; mach_port_t o;
        for (int i = 0; i < 8; i++) {
            mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
            if (mach_vm_region(mach_task_self(), &r, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &o) || r >= a + n) break;
            vx_log("aot:   %#llx-%#llx is mapped", (unsigned long long)r, (unsigned long long)(r + rs));
            r += rs;
        }
    }
    return 0;
}
int xa_in_arena(uint64_t a, uint64_t n) { return g_arena_hi && a >= g_arena_lo && a + n <= g_arena_hi; }

uint64_t xa_module_base(const char *dir, const char *path, uint64_t hash) {
    const char *slash = strrchr(path, '/');
    char lower[256], mod[4096];
    size_t k = 0;
    for (const char *n = slash ? slash + 1 : path; *n && k + 1 < sizeof lower; n++) lower[k++] = (char)tolower((unsigned char)*n);
    lower[k] = 0;
    snprintf(mod, sizeof mod, "%s/%s-%016llx.dylib", dir, lower, (unsigned long long)hash);
    if (!xa_module_exists(mod)) snprintf(mod, sizeof mod, "%s/%s-%016llx.p0.dylib", dir, lower, (unsigned long long)hash);
    if (!xa_module_exists(mod)) return 0;
    const xa_desc *d = open_module(mod);                       // (a dylib kept open: xa_load opens it again)
    return d && d->magic == XA_MAGIC && d->version == XA_VERSION && d->file_hash == hash ? d->image_base : 0;
}

static const xa_block *block_at(const xa_desc *d, uint64_t rip);
int xa_link(uint64_t hash) {
    const xa_desc *part[MAX_MODULES] = { 0 };
    int n = 0, total = 0;
    for (int i = 0; i < g_nmod; i++)
        if (g_mod[i]->file_hash == hash && g_mod[i]->part < MAX_MODULES) { part[g_mod[i]->part] = g_mod[i]; total = g_mod[i]->nparts; n++; }
    if (total <= 1) return n;
    for (int p = 0; p < total && p < MAX_MODULES; p++) {
        const xa_desc *d = part[p];
        if (!d) { vx_log("aot: part %d of the image with hash %016llx is missing: its addresses go to the translator", p, (unsigned long long)hash); continue; }
        for (int q = 0; q < total && q < MAX_MODULES; q++) d->plk[q] = part[q] ? (uint64_t)(uintptr_t)part[q]->lookup : 0;
        uint64_t linked = 0;
        for (uint64_t k = 0; k < d->nx; k++) {
            uint64_t rip = d->xrip[k];
            for (int q = 0; q < total && q < MAX_MODULES; q++) {
                const xa_desc *e = part[q];
                if (!e || rip < e->part_lo || rip >= e->part_hi) continue;
                const xa_block *b = block_at(e, rip);
                if (b) { d->xslot[k] = (uint64_t)(uintptr_t)(e->code + b->off); linked++; }
                break;
            }
        }
        if (linked != d->nx) vx_log("aot: part %d: %llu of %llu branches to other parts linked", p, (unsigned long long)linked, (unsigned long long)d->nx);
    }
    return n;
}

static const xa_desc *module_for(uint64_t rip) {
    for (int i = 0; i < g_nmod; i++)
        if (rip >= g_mod[i]->part_lo && rip < g_mod[i]->part_hi) return g_mod[i];
    return NULL;
}
static const xa_block *block_at(const xa_desc *d, uint64_t rip) {
    size_t lo = 0, hi = d->nblocks;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (d->blocks[mid].rip < rip) lo = mid + 1; else hi = mid; }
    return lo < d->nblocks && d->blocks[lo].rip == rip ? &d->blocks[lo] : NULL;
}
static void (*cfn_at(const xa_desc *d, uint64_t rip))(x86_ctx *) {
    size_t lo = 0, hi = d->ncfns;
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (d->cfns[mid].rip < rip) lo = mid + 1; else hi = mid; }
    return lo < d->ncfns && d->cfns[lo].rip == rip ? d->cfns[lo].fn : NULL;
}
int xa_owns_rip(uint64_t rip) {
    const xa_desc *d = module_for(rip);
    return d && (block_at(d, rip) || cfn_at(d, rip));
}
int xa_owns(uint64_t pc) {
    for (int i = 0; i < g_nmod; i++) if (pc >= (uint64_t)(uintptr_t)g_mod[i]->code && pc < (uint64_t)(uintptr_t)g_mod[i]->code_end) return 1;
    return 0;
}
uint64_t xa_rip_sp_at(uint64_t pc, int64_t *sp) {
    for (int i = 0; i < g_nmod; i++) {
        const xa_desc *d = g_mod[i];
        if (pc < (uint64_t)(uintptr_t)d->code || pc >= (uint64_t)(uintptr_t)d->code_end) continue;
        uint64_t off = pc - (uint64_t)(uintptr_t)d->code;
        size_t lo = 0, hi = d->nimap;
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (d->imap[mid].off <= off) lo = mid + 1; else hi = mid; }
        if (sp) *sp = lo ? d->imap[lo - 1].sp : 0;
        return lo ? d->imap[lo - 1].rip : 0;
    }
    if (sp) *sp = 0;
    return 0;
}

// VXRT_AOT_SPLITS=<file>: the x86 instructions whose ordered access crossed 16 bytes (each a signal here, where the
// engine patches its code once), written with each report for vxrt-aot to give those instructions plain accesses between barriers
enum { NSPLIT = 1 << 14 };
static uint64_t g_split_rip[NSPLIT];
static void note_split(uint64_t rip) {
    if (!rip) return;
    uint32_t i = (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 50);
    for (int k = 0; k < 32; k++, i = (i + 1) & (NSPLIT - 1)) {
        uint64_t v = __atomic_load_n(&g_split_rip[i], __ATOMIC_RELAXED);
        if (v == rip) return;
        if (!v && __atomic_compare_exchange_n(&g_split_rip[i], &v, rip, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
        if (v == rip) return;
    }
}
static void write_splits(void) {
    const char *f = getenv("VXRT_AOT_SPLITS");
    FILE *o = f ? fopen(f, "w") : NULL;          // (the whole set each time: reports come every 10 s)
    if (!o) return;
    for (int i = 0; i < NSPLIT; i++) if (g_split_rip[i]) fprintf(o, "%#llx\n", (unsigned long long)g_split_rip[i]);
    fclose(o);
}
// An LDAPR / STLR across 16 bytes faults (the engine patches its own code: engine.c); a module's code cannot be
// written, so the access is made here, between barriers, and the code goes on after it.
int xa_fix_split(void *uc_) {
    ucontext_t *uc = uc_;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    if (!xa_owns(pc)) return 0;
    uint32_t insn = *(const uint32_t *)(uintptr_t)pc, sz = insn >> 30, t = insn & 31, n = insn >> 5 & 31;
    int load = (insn & 0x3FFFFC00u) == 0x38BFC000u, store = (insn & 0x3FFFFC00u) == 0x089FFC00u;
    if (!load && !store) return 0;
    uint64_t addr = n == 31 ? uc->uc_mcontext->__ss.__sp : uc->uc_mcontext->__ss.__x[n];
    size_t bytes = (size_t)1 << sz;
    // (only an alignment fault of an access across 16 bytes: any other is the program's own, an access violation)
    if ((uc->uc_mcontext->__es.__esr & 0x3F) != 0x21 || (addr & 15) + bytes <= 16) return 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (load) {
        uint64_t v = 0;
        memcpy(&v, (const void *)(uintptr_t)addr, bytes);
        if (t != 31) uc->uc_mcontext->__ss.__x[t] = v;
    } else {
        uint64_t v = t == 31 ? 0 : uc->uc_mcontext->__ss.__x[t];
        memcpy((void *)(uintptr_t)addr, &v, bytes);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uc->uc_mcontext->__ss.__pc = pc + 4;
    __atomic_fetch_add(&g_splits, 1, __ATOMIC_RELAXED);
    if (getenv("VXRT_AOT_SPLITS")) note_split(xa_rip_sp_at(pc, NULL));
    return 1;
}

static void note_fallback(uint64_t rip) {
    enum { SEEN = 1 << 16 };                                  // (full: the addresses past it are counted, not listed)
    static uint64_t seen[SEEN]; static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    __atomic_fetch_add(&g_fallback, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&lock);
    uint32_t i = (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 48);
    int probes = 0;
    while (seen[i] && seen[i] != rip && probes++ < 64) i = (i + 1) & (SEEN - 1);
    if (!seen[i]) {
        seen[i] = rip; g_fallback_rips++;
        // VXRT_AOT_MISSES=<file>: each address once, with its image, for vxrt-aot (VXRT_AOT_ROOTS) or a module to add
        static FILE *misses; static int opened;
        if (!opened) { opened = 1; if (getenv("VXRT_AOT_MISSES")) misses = fopen(getenv("VXRT_AOT_MISSES"), "a"); }
        if (misses) { const char *im = vx_image_name_at(rip); fprintf(misses, "%#llx %s\n", (unsigned long long)rip, im ? im : "-"); fflush(misses); }
        if (g_fallback_rips <= 32 || getenv("VXRT_AOT_TRACE")) {
            const xa_desc *d = module_for(rip);
            const char *img = vx_image_name_at(rip);
            vx_log("aot: fallback to the translator at %#llx (%s)%s", (unsigned long long)rip, img ? img : "no image",
                   d ? ": in a recompiled image, a block the generator did not find" : "");
        }
    }
    pthread_mutex_unlock(&lock);
}

void xa_report(void);
static void xa_tick(void) {                                  // (every 4M entries: the counters each 10 s)
    static uint64_t last; struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec;
    if (now - last >= 10) { last = now; xa_report(); }
}
typedef struct { uint64_t rip, p; const xa_desc *d; } xa_cached;
enum { NCACHE = 1024 };
static __thread xa_cached t_cache[NCACHE];
// After a native function's thunk, as the engine's native block (xlat_block_native): the call made, RSP at its return
// address; then RET, or on at ctx->rip when the function says so. The return-address ring steps back as RET would.
static inline void native_return(x86_ctx *c) {
    if (c->jump) { c->jump = 0; return; }
    if (c->fault) return;
    uint64_t *sp = (uint64_t *)(uintptr_t)c->gpr[4];
    c->rip = *sp; c->gpr[4] += 8;
    if (c->ras) c->ras = (c->ras & ~(uint64_t)((1u << XLAT_RAS_BITS) - 1)) | ((c->ras - 16) & ((1u << XLAT_RAS_BITS) - 1));
}
int xa_run(xe *e, x86_ctx *c, uint64_t stop, void (*(*native)(uint64_t))(x86_ctx *)) {
    if (g_strict < 0) g_strict = getenv("VXRT_AOT_STRICT") && atoi(getenv("VXRT_AOT_STRICT"));
    for (;;) {
        if (c->fault) return (int)c->fault;
        uint64_t rip = c->rip;
        if (rip == stop) return 0;
        // (a thread's cache of the last addresses: a module's block, or a native function's thunk; most exits are
        // native calls and the returns from them, and the searches below cost more than the call)
        xa_cached *k = &t_cache[(rip ^ rip >> 12) & (NCACHE - 1)];
        if (k->rip == rip) {
            if (k->d) {
                if (!(++g_entries & ((1u << 22) - 1))) xa_tick();
                c->entry = k->p; c->link = 0; k->d->enter(c);
                continue;
            }
            g_native++;
            ((void (*)(x86_ctx *))k->p)(c);
            native_return(c);
            continue;
        }
        const xa_desc *d = module_for(rip);
        if (d) {
            void (*fn)(x86_ctx *) = d->ncfns ? cfn_at(d, rip) : NULL;
            if (fn) { g_cfn_calls++; fn(c); continue; }   // (route A: back with the next RIP)
            const xa_block *b = block_at(d, rip);
            if (b) {
                if (!(++g_entries & ((1u << 22) - 1))) xa_tick();
                c->entry = (uint64_t)(uintptr_t)(d->code + b->off);
                c->link = 0;
                k->rip = rip; k->p = c->entry; k->d = d;
                d->enter(c);
                continue;
            }
        }
        uint64_t to;
        if (vx_builtin_stub(rip, &to)) { g_stubs++; c->rip = to; continue; }   // (an x86 jmp [rip]: nothing else changes)
        void (*thunk)(x86_ctx *) = native ? native(rip) : NULL;
        if (thunk) {
            g_native++;
            k->rip = rip; k->p = (uint64_t)(uintptr_t)thunk; k->d = NULL;
            thunk(c);
            native_return(c);
            continue;
        }
        note_fallback(rip);
        if (g_strict) {
            vx_log("aot: VXRT_AOT_STRICT: %#llx has no recompiled code; stopping", (unsigned long long)rip);
            xa_report();
            _exit(3);
        }
        int r = xe_run(e, c, stop);
        if (r != XE_FOREIGN) return r;
    }
}

void xa_report(void) {
    if (!g_nmod) return;
    vx_log("aot: %llu entries into recompiled code, %llu C function calls, %llu native calls, %llu built-in export stubs followed, %llu fallbacks to the translator (%llu addresses), %llu ordered accesses across 16 bytes",
           (unsigned long long)g_entries, (unsigned long long)g_cfn_calls, (unsigned long long)g_native, (unsigned long long)g_stubs, (unsigned long long)g_fallback,
           (unsigned long long)g_fallback_rips, (unsigned long long)g_splits);
    write_splits();
}
#endif
