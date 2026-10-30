// Static recompilation (RECOMP.md): x86-64 code of a PE image translated before the program runs, into a module
// (a dylib) the ARM64 runtime loads with the image. Two ways to make one (tools/vxrt_aot.c): route B, the
// translator's own code for each block (xlat_block_linked) written out as assembly, and route A, each function as C
// compiled by clang (decided against for now: RECOMP.md). Modules describe themselves with xa_desc; aot.c runs them.
#pragma once
#include <stdint.h>
#include "xlat.h"
#include "engine.h"

enum { XA_MAGIC = 0x52435856u /* "VXCR" */, XA_VERSION = 2 };
enum { XA_ORDERED = 1, XA_AFP = 2, XA_ROUTE_A = 4 };
typedef struct { uint64_t rip; int64_t off; } xa_block;               // a block's x86 address and its code (from code)
typedef struct { uint32_t off; int32_t sp; uint64_t rip; } xa_imap;   // host code offset -> x86 instruction (rip 0: none)
typedef struct { uint64_t rip; void (*fn)(x86_ctx *); } xa_cfn;        // route A: a function made of C
typedef struct {
    uint32_t magic, version;
    uint64_t image_base, image_size, file_hash, flags;
    const uint8_t *code, *code_end;                  // route B's code (blocks, stubs)
    void (*enter)(x86_ctx *);                        // route B: the x86 state into registers, on at ctx->entry
    const xa_block *blocks; uint64_t nblocks;        // sorted by rip
    const xa_imap *imap; uint64_t nimap;             // sorted by off
    uint64_t *ext; uint64_t next;                    // host addresses the code loads (XA_HELPER ...), set by xa_load
    const xa_cfn *cfns; uint64_t ncfns;              // route A (not built: RECOMP.md): sorted by rip
    // Parts (version 2): an image with more code than B/BL reach (±128 MB) is split into dylibs, each for the x86
    // addresses [part_lo, part_hi) (one part: the whole image). xa_link fills the slots once all parts are loaded.
    uint64_t part_lo, part_hi; int32_t part, nparts;
    const void *lookup;                              // this part's lookup stub (x17: the x86 address)
    uint64_t *plk;                                   // [nparts]: each part's lookup (0: not loaded, the exit)
    const uint64_t *xrip; uint64_t *xslot; uint64_t nx;  // other parts' blocks this part branches to, and their code
} xa_desc;

// The generator and the loader agree on the file: FNV-1a 64 of the PE file's bytes.
static inline uint64_t xa_hash(const uint8_t *p, uint64_t n) {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
#ifdef __aarch64__
// Loads a module for the image mapped at base (its file's bytes: hash); 0 when it is used.
int xa_load(const char *path, uint64_t base, uint64_t size, uint64_t hash);
// The parts of the image with this hash branch to each other (after all of them are loaded); the number linked.
int xa_link(uint64_t hash);
int xa_any(void);
int xa_module_exists(const char *path);             // a module of that file name (Mac: a dylib; visionOS: linked in)
// The base the module for this file (dir/<name lowercased>-<hash>[.p0].dylib) was made for; 0 when there is none.
int xa_reserve(void);                              // VXRT_AOT_DIR: at start (aot.c); 0: the range is taken
int xa_in_arena(uint64_t a, uint64_t n);
uint64_t xa_arena_base(void), xa_arena_size(void);   // (where the arena is or will be)
uint64_t xa_module_base(const char *dir, const char *path, uint64_t hash);
// Runs the program from ctx->rip until stop like xe_run: in a module's code when the address is one of its blocks,
// a native function (find) called from here, anything else on the engine (counted: the fallback; VXRT_AOT_STRICT=1
// makes it an error instead).
int xa_run(xe *e, x86_ctx *c, uint64_t stop, void (*(*native)(uint64_t))(x86_ctx *));

int xa_owns_rip(uint64_t rip);                     // an address some module has code for
int xa_owns(uint64_t pc);                          // host pc in a module's code
uint64_t xa_rip_sp_at(uint64_t pc, int64_t *sp);   // the x86 instruction there (0: none), RSP's offset from x4
int xa_fix_split(void *uc);                        // an ordered access across 16 bytes in a module's code: done here
void xa_report(void);
#endif
