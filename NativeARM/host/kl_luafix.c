// Valve's LuaJIT arena below 2 GB, on a platform that has no memory there.
//
// libvscript installs an mmap hook into its LuaJIT (lua_VALVE_OVERRIDE_MMAP)
// that reserves a 2 GB-window arena through tier0's
// Plat_VirtualAlloc(addr, size, type, prot, maxAddress) with maxAddress =
// 0x80000000, and then refuses any result above 0x80000000 - size. tier0
// serves that by probing mmap hints from 0x10000 up in 64 KB steps. On arm64
// macOS the kernel keeps the first 4 GB unmappable (a 4 GB __PAGEZERO is
// required at exec and vm_map_min is 4 GB), so every probe lands high and the
// VM creation fails ("Lua allocation of 1048576 failed").
//
// This LuaJIT is the arm64 build, which is GC64: it addresses 47 bits and has
// no use for the window itself. So: Plat_VirtualAlloc is interposed to drop
// the maxAddress constraint, and the hook's range check (one conditional
// branch) is turned into a nop in memory, verified by its surrounding
// instruction pattern before it is touched.
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include "klepton.h"
#include "kl_luafix.h"

typedef void *(*valloc_fn)(void *, size_t, unsigned, unsigned, uintptr_t);
static valloc_fn g_real;
static int g_patched;

// Reservations made under a maxAddress (the Lua arenas). The hook commits
// them with prot 7 (RWX); this platform has no RWX for non-JIT pages and the
// GC64 interpreter's heap has no reason to execute, so the commit becomes RW.
// (LuaJIT's trace mcode asks for executable memory separately and, refused,
// stays in the interpreter.)
typedef struct { uintptr_t lo, hi; } range_t;
static range_t g_arenas[4096];
static int g_narenas;
static int in_arena(uintptr_t a) {
    for (int i = 0; i < g_narenas; i++) if (a >= g_arenas[i].lo && a < g_arenas[i].hi) return 1;
    return 0;
}

// mov x1,#0x80000000 ; stp x21,x22,[sp,#0x20] ; mov x21,x0 ; sub x1,x1,x19 ;
// cmp x0,x1 ; b.hi <fail>
static const uint32_t k_pattern[] = { 0xd2b00001, 0xa9025bf5, 0xaa0003f5, 0xcb130021, 0xeb01001f };
#define K_BRANCH_MASK 0xff00001f   // b.cond with cond HI (0x8): 0x54......8
#define K_BRANCH_VAL  0x54000008

static void patch_range_check(void) {
    if (g_patched) return;
    kl_image *img = kl_find_image("libvscript.so");
    uint8_t *base = img ? (uint8_t *)kl_base(img) : NULL;
    if (!base) return;                 // tier0's own early 2 GB reservation; try again later
    g_patched = 1;
    // The pattern sits early in the image; scan the first 2 MB of text.
    for (size_t off = 0; off + sizeof k_pattern + 4 <= 0x200000; off += 4) {
        const uint32_t *p = (const uint32_t *)(base + off);
        if (memcmp(p, k_pattern, sizeof k_pattern) != 0) continue;
        uint32_t *br = (uint32_t *)(base + off + sizeof k_pattern);
        if ((*br & K_BRANCH_MASK) != K_BRANCH_VAL) continue;
        uintptr_t page = (uintptr_t)br & ~(uintptr_t)0x3fff;
        if (mprotect((void *)page, 0x4000, PROT_READ | PROT_WRITE) != 0) {
            fprintf(stderr, "  [luafix] mprotect RW failed; range check left alone\n"); return;
        }
        *br = 0xd503201f;                                   // nop
        mprotect((void *)page, 0x4000, PROT_READ | PROT_EXEC);
        sys_icache_invalidate(br, 4);
        fprintf(stderr, "  [luafix] libvscript+%#zx: Lua arena 2 GB range check disabled\n", off + sizeof k_pattern);
        return;
    }
    fprintf(stderr, "  [luafix] Lua arena range check not found in libvscript (different build?)\n");
}

static void *shim_Plat_VirtualAlloc(void *addr, size_t size, unsigned type, unsigned prot, uintptr_t max_addr) {
    if (!g_real) {
        kl_image *t0 = kl_find_image("libtier0.so");
        g_real = t0 ? (valloc_fn)kl_sym(t0, "Plat_VirtualAlloc") : NULL;
        if (!g_real) { fprintf(stderr, "  [luafix] tier0 Plat_VirtualAlloc not found\n"); return NULL; }
    }
    static int said;
    if (max_addr) {
        patch_range_check();
        if (said < 8) { said++; fprintf(stderr, "  [luafix] Plat_VirtualAlloc(size %zu, type %u, prot %u, max %#lx): max address dropped\n",
                                        size, type, prot, (unsigned long)max_addr); }
        void *p = g_real(addr, size, type, prot, 0);
        if (p && g_narenas < (int)(sizeof g_arenas / sizeof g_arenas[0]))
            g_arenas[g_narenas++] = (range_t){ (uintptr_t)p, (uintptr_t)p + size };
        return p;
    }
    if ((type & 2) && (prot & 4) && addr && in_arena((uintptr_t)addr)) {
        static int said_x;
        if (!said_x++) fprintf(stderr, "  [luafix] Lua arena commit asked for RWX; committed RW\n");
        prot &= ~4u;
    }
    // Linux pages are 4 KB, Apple Silicon's 16 KB: a commit of one 4 KB page
    // inside a reservation (panorama's allocator does this) is an mprotect
    // the kernel refuses as unaligned. The commit is widened to the 16 KB
    // pages it touches (the neighbours become accessible early, never less).
    if ((type & 3) == 2 && addr && (((uintptr_t)addr & 0x3fff) || (size & 0x3fff))) {
        uintptr_t a = (uintptr_t)addr & ~(uintptr_t)0x3fff;
        uintptr_t e = ((uintptr_t)addr + size + 0x3fff) & ~(uintptr_t)0x3fff;
        void *r = g_real((void *)a, e - a, type, prot, max_addr);
        return r ? addr : NULL;
    }
    void *r = g_real(addr, size, type, prot, max_addr);
    if (!r) {
        static int nfail;
        if (nfail++ < 16) fprintf(stderr, "  [valloc] Plat_VirtualAlloc(%p, %zu, type %u, prot %u) FAILED (errno %d)\n",
                                  addr, size, type, prot, errno);
    }
    return r;
}

// Plat_VirtualFree(addr, size, type): type 4 decommits (mprotect PROT_NONE),
// 8 releases. A decommit is shrunk to the 16 KB pages it fully covers, so a
// 4 KB decommit never revokes a neighbour that is still in use; one that
// covers no whole page succeeds without touching anything.
typedef int (*vfree_fn)(void *, size_t, unsigned);
static vfree_fn g_real_free;
static int shim_Plat_VirtualFree(void *addr, size_t size, unsigned type) {
    if (!g_real_free) {
        kl_image *t0 = kl_find_image("libtier0.so");
        g_real_free = t0 ? (vfree_fn)kl_sym(t0, "Plat_VirtualFree") : NULL;
        if (!g_real_free) return 0;
    }
    if (type == 4 && (((uintptr_t)addr & 0x3fff) || (size & 0x3fff))) {
        uintptr_t a = ((uintptr_t)addr + 0x3fff) & ~(uintptr_t)0x3fff;
        uintptr_t e = ((uintptr_t)addr + size) & ~(uintptr_t)0x3fff;
        if (e <= a) return 1;
        return g_real_free((void *)a, e - a, type);
    }
    return g_real_free(addr, size, type);
}

void kl_luafix_install(void) {
    kl_interpose("Plat_VirtualAlloc", (void *)shim_Plat_VirtualAlloc);
    kl_interpose("Plat_VirtualFree", (void *)shim_Plat_VirtualFree);
}
