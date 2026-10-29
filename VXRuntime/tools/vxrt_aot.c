// vxrt-aot: the static recompiler's tool (RECOMP.md). A native ARM64 program for this Mac, built with the
// translator's own sources.
//
//   vxrt-aot analyze <pe> [--roots-only]  stage 0: where the code is. Roots from the file (entry, exports, TLS callbacks,
//                                         .pdata, relocated pointers into code, exception scope tables, GCC landing
//                                         pads, CDPR-style address lists with --addresses), recursive descent with the
//                                         translator's decoder, jump tables recovered, and what of the code sections
//                                         that leaves unexplained. Statistics only: nothing is translated.
//   vxrt-aot gen <pe> <out.s>             route B: every block the analysis finds, translated by xlat_block_linked
//                                         as the engine would, written out as assembly for a module (aot.h)
//   vxrt-aot name <pe>                    the module's file name the runtime looks for (VXRT_AOT_DIR)
//
// VXRT_AOT_BASE=<hex>: the image as loaded at that address (relocated), for a DLL the runtime cannot map at its
// preferred base; the runtime then maps the file where its module says (src/loader.c).
//
// The image is mapped at its preferred base in this process, as the runtime maps it: the translator reads code at the
// addresses it will run at.
#include "../translator/xlat.h"
#include "../translator/x86dec.h"
#include "../translator/aot.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#include <mach/mach_vm.h>
#include <time.h>

// ---- the image -----------------------------------------------------------------------------------------------------
typedef struct { char name[9]; uint32_t va, vsize, raw_ptr, raw_size, flags; } section;
typedef struct {
    uint8_t *file; size_t len; uint64_t hash;
    uint64_t base; uint32_t size, entry;
    uint8_t *mem;                                   // mapped at base
    section sec[96]; int nsec;
    uint32_t dir_rva[16], dir_size[16];
    const char *path, *name;
} image;
static uint16_t r16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t r32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t r64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void die(const char *fmt, ...) { va_list a; va_start(a, fmt); fprintf(stderr, "vxrt-aot: "); vfprintf(stderr, fmt, a); fprintf(stderr, "\n"); va_end(a); exit(1); }

static char **g_argv;
static void load(image *im, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s", path);
    fseek(f, 0, SEEK_END); im->len = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    // (the image's range is taken before the file's copy is allocated, which could land there: a large file's did)
    uint8_t h[4096] = { 0 };
    size_t hn = fread(h, 1, sizeof h, f); fseek(f, 0, SEEK_SET);
    uint32_t hpe = hn >= 0x40 ? r32(h + 0x3c) : 0;
    if (hpe && hpe + 24 + 60 <= hn && !memcmp(h + hpe, "PE\0\0", 4) && r16(h + hpe + 24) == 0x20b) {
        uint64_t b = getenv("VXRT_AOT_BASE") ? strtoull(getenv("VXRT_AOT_BASE"), NULL, 16) : r64(h + hpe + 24 + 24);
        uint32_t sz = r32(h + hpe + 24 + 56);
        // (exactly there, never over a mapping: mach_vm_allocate fixed; mmap takes some ranges' hints as no hint)
        mach_vm_address_t a = b;
        if (mach_vm_allocate(mach_task_self(), &a, sz, VM_FLAGS_FIXED) == KERN_SUCCESS) im->mem = (uint8_t *)(uintptr_t)a;
        else if (!getenv("VXRT_AOT_RESTARTS") || atoi(getenv("VXRT_AOT_RESTARTS")) < 8) {
            // (the system's randomized regions cover it this time: they fall elsewhere in a new process)
            char n[8], self[4096]; uint32_t len = sizeof self;
            snprintf(n, sizeof n, "%d", getenv("VXRT_AOT_RESTARTS") ? atoi(getenv("VXRT_AOT_RESTARTS")) + 1 : 1);
            setenv("VXRT_AOT_RESTARTS", n, 1);
            if (!_NSGetExecutablePath(self, &len)) execv(self, g_argv);
        }
    }
    im->file = malloc(im->len);
    if (fread(im->file, 1, im->len, f) != im->len) die("cannot read %s", path);
    fclose(f);
    im->path = path; im->name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    im->hash = xa_hash(im->file, im->len);
    const uint8_t *d = im->file;
    uint32_t pe = r32(d + 0x3c);
    if (im->len < pe + 24 || memcmp(d + pe, "PE\0\0", 4) || r16(d + pe + 4) != 0x8664) die("%s: not an x86-64 PE", path);
    uint32_t opt = pe + 24, nsec = r16(d + pe + 6), optsz = r16(d + pe + 20);
    if (r16(d + opt) != 0x20b) die("%s: not PE32+", path);
    im->entry = r32(d + opt + 16); im->base = r64(d + opt + 24); im->size = r32(d + opt + 56);
    uint64_t pref = im->base;
    if (getenv("VXRT_AOT_BASE")) im->base = strtoull(getenv("VXRT_AOT_BASE"), NULL, 16);
    uint32_t hdr = r32(d + opt + 60), ndirs = r32(d + opt + 108);
    for (uint32_t i = 0; i < 16 && i < ndirs; i++) { im->dir_rva[i] = r32(d + opt + 112 + 8 * i); im->dir_size[i] = r32(d + opt + 116 + 8 * i); }
    uint32_t so = opt + optsz;
    for (uint32_t i = 0; i < nsec && i < 96; i++, so += 40) {
        section *s = &im->sec[im->nsec++];
        memcpy(s->name, d + so, 8); s->name[8] = 0;
        s->vsize = r32(d + so + 8); s->va = r32(d + so + 12); s->raw_size = r32(d + so + 16); s->raw_ptr = r32(d + so + 20); s->flags = r32(d + so + 36);
    }
    if (!im->mem) im->mem = mmap((void *)(uintptr_t)im->base, im->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (im->mem == MAP_FAILED || (uint64_t)(uintptr_t)im->mem != im->base) die("cannot map %s at its base %#llx", path, (unsigned long long)im->base);
    memcpy(im->mem, d, hdr < im->len ? hdr : im->len);
    for (int i = 0; i < im->nsec; i++) {
        section *s = &im->sec[i];
        uint32_t n = s->raw_size < s->vsize || !s->vsize ? s->raw_size : s->vsize;
        if (n && s->raw_ptr + n <= im->len) memcpy(im->mem + s->va, d + s->raw_ptr, n);
    }
    // VXRT_AOT_BASE: a module for the image loaded there instead (a DLL whose preferred base the process cannot have:
    // macOS's shared cache is at 0x180000000), relocated as src/loader.c does it
    int64_t delta = (int64_t)(im->base - pref);
    if (delta && im->dir_size[5]) {
        for (uint8_t *b = im->mem + im->dir_rva[5]; b < im->mem + im->dir_rva[5] + im->dir_size[5];) {
            uint32_t page = r32(b), block = r32(b + 4);
            if (!block) break;
            for (uint32_t k = 8; k < block; k += 2) {
                uint16_t e = r16(b + k);
                if ((e >> 12) == 10) { uint64_t *q = (uint64_t *)(im->mem + page + (e & 0xfff)); *q += (uint64_t)delta; }
                else if ((e >> 12) == 3) { uint32_t *q = (uint32_t *)(im->mem + page + (e & 0xfff)); *q += (uint32_t)delta; }
            }
            b += block;
        }
    } else if (delta) die("%s: VXRT_AOT_BASE given and the image has no relocations", path);
}
static int exec_rva(const image *im, uint64_t rva) {
    for (int i = 0; i < im->nsec; i++)
        if ((im->sec[i].flags & 0x20000000) && rva >= im->sec[i].va && rva < (uint64_t)im->sec[i].va + im->sec[i].vsize) return 1;
    return 0;
}
static int exec_va(const image *im, uint64_t va) { return va >= im->base && exec_rva(im, va - im->base); }
static int in_image(const image *im, uint64_t va) { return va >= im->base && va - im->base < im->size; }
static const char *cstr(const image *im, uint32_t rva) { return rva && rva < im->size ? (const char *)im->mem + rva : ""; }

// Imports: the IAT slot's address -> the function's name (for exception handlers reached through a thunk).
typedef struct { uint64_t slot; char name[96]; } import;
static import *g_imp; static int g_nimp;
static void read_imports(const image *im) {
    if (!im->dir_size[1]) return;
    for (const uint8_t *d = im->mem + im->dir_rva[1]; r32(d + 12); d += 20) {
        uint32_t ilt = r32(d) ? r32(d) : r32(d + 16), iat = r32(d + 16);
        for (uint32_t k = 0;; k++) {
            uint64_t v = r64(im->mem + ilt + 8 * k);
            if (!v) break;
            g_imp = realloc(g_imp, (size_t)(g_nimp + 1) * sizeof *g_imp);
            g_imp[g_nimp].slot = im->base + iat + 8 * k;
            snprintf(g_imp[g_nimp].name, sizeof g_imp[g_nimp].name, "%s", v >> 63 ? "#ordinal" : cstr(im, (uint32_t)(v & 0x7fffffff) + 2));
            g_nimp++;
        }
    }
}
static const char *import_at(uint64_t slot) { for (int i = 0; i < g_nimp; i++) if (g_imp[i].slot == slot) return g_imp[i].name; return NULL; }

// ---- stage 0: roots and recursive descent --------------------------------------------------------------------------
enum { M_CODE = 1, M_LEADER = 2, M_FUNC = 4, M_SEEN = 8, M_TABLE = 16 };
enum { R_ENTRY, R_EXPORT, R_TLS, R_PDATA, R_HANDLER, R_RELOC, R_RVA, R_SCOPE, R_LANDING, R_FH3, R_FH4, R_LIST, R_CALL, R_LEA, R_IMM, R_TABLE, R_BRANCH, R_NKINDS };
static const char *root_names[R_NKINDS] = { "entry", "exports", "TLS callbacks", ".pdata", "exception handlers", "relocated pointers",
    "image-relative (RVA) references to aligned code", "__C_specific_handler scopes",
    "GCC landing pads", "__CxxFrameHandler3 funclets", "__CxxFrameHandler4 funclets, continuations", "address list", "direct call targets", "lea [rip] into code", "imm64 into code", "jump table targets", "direct branch targets" };
static uint8_t *g_len, *g_mark;                 // per image byte: instruction length at a start; marks
static uint64_t g_roots[R_NKINDS], g_roots_new[R_NKINDS];
static uint64_t *g_work; static size_t g_nwork, g_cwork;
static uint64_t g_insns, g_bad, g_jmp_reg, g_tables, g_table_entries, g_unbounded, g_jmp_mem, g_call_ind;
typedef struct { uint64_t at; } jsite;
static uint64_t *g_jsites; static size_t g_njsites, g_cjsites;
static const image *G;

static void push(uint64_t va, int kind) {
    if (!exec_va(G, va)) return;
    uint32_t r = (uint32_t)(va - G->base);
    g_roots[kind]++;
    if (!(g_mark[r] & M_LEADER)) g_roots_new[kind]++;
    g_mark[r] |= M_LEADER;
    if (kind <= R_LIST || kind == R_CALL) g_mark[r] |= M_FUNC;
    if (g_mark[r] & M_SEEN) return;
    if (g_nwork == g_cwork) { g_cwork = g_cwork ? 2 * g_cwork : 4096; g_work = realloc(g_work, g_cwork * sizeof *g_work); }
    g_work[g_nwork++] = va;
}

// The instruction ending at va, decoded earlier (its start), or 0.
static uint64_t insn_before(uint64_t va) {
    for (int k = 1; k <= 15; k++) {
        uint64_t s = va - (uint64_t)k;
        if (s < G->base) break;
        uint32_t r = (uint32_t)(s - G->base);
        if (g_len[r] == k) return s;
    }
    return 0;
}
static int decode_at(uint64_t va, x86_insn *d) { return x86_decode(G->mem + (va - G->base), 15, d) > 0; }
static int is_mem(const x86_insn *d) { return d->has_modrm && d->mod != 3; }

// A jump table before `jmp reg` at site: the table's address, its entries' size and signedness, and the base they are
// added to, from the instructions before it (in this block or the one falling into it). Entries are read while they
// are in bounds (a CMP with the index and a JA/JAE before the jump) or, without one, while their targets are code of
// the same section: candidates only, as the code's own lookup decides at run time (a wrong one is dead code).
static void jump_table(uint64_t site) {
    x86_insn j; if (!decode_at(site, &j)) return;
    int reg = j.rm;
    uint64_t at = site;
    int64_t table = -1, add_base = 0; int esize = 0, sign = 0, have_add = 0, add_reg = -1, idx = -1, load_base = -1; int32_t disp = 0;
    int64_t bound = -1;
    // (back through the code before it in address order, to the function's start at most: a table's base is often
    //  loaded once before the loop that dispatches through it)
    for (int n = 0; n < 256 && (at = insn_before(at)); n++) {
        x86_insn d; if (!decode_at(at, &d)) break;
        if (g_mark[at - G->base] & M_FUNC) n = 256 - 1;   // (this instruction, then stop)
        int op = d.opcode;
        if (d.map == X86_MAP_1 && (op == 0x01 || op == 0x03) && d.mod == 3 && !have_add) {        // ADD reg, X
            int dst = op == 0x01 ? d.rm : d.reg, src = op == 0x01 ? d.reg : d.rm;
            if (dst == reg) { have_add = 1; add_reg = src; continue; }
        }
        if (d.map == X86_MAP_1 && (op == 0x63 || op == 0x8B) && is_mem(&d) && d.reg == reg && esize == 0 && d.scale == 4 - 0 && d.index != 0xFF) {
            esize = 4; sign = op == 0x63; idx = d.index; load_base = d.base; disp = d.disp; continue;
        }
        if (d.map == X86_MAP_1 && op == 0x8B && is_mem(&d) && d.reg == reg && esize == 0 && d.scale == 8 && d.index != 0xFF && d.base == 0xFF) {
            esize = 8; idx = d.index; disp = d.disp; table = (int64_t)(int32_t)d.disp; continue;    // jmp through absolute pointers
        }
        if (d.map == X86_MAP_1 && op == 0x8D && d.rip_relative) {                                // LEA r, [rip + x]
            int64_t v = (int64_t)(at + d.len) + d.disp;
            if (esize == 4 && d.reg == load_base && table < 0) table = v + disp;
            if (have_add && d.reg == add_reg) add_base = v;
            continue;
        }
        if (idx >= 0 && d.map == X86_MAP_1 && (op == 0x83 || op == 0x81 || op == 0x3D) && ((op == 0x3D && (idx & 15) == 0) || (d.mod == 3 && (d.reg & 7) == 7 && (d.rm & 15) == (idx & 15)))) {
            // CMP idx, imm: then the JA / JAE after it (the first conditional branch we passed)
            if (bound < 0 && n < 16) bound = d.imm + 1;
            if (table >= 0 && (!have_add || add_base)) break;
            continue;
        }
    }
    if (esize == 4 && have_add && (table < 0 || add_base == 0)) {
        // (the base's LEA not on the straight line before it: the last LEA of that register from the function's start,
        //  in address order, a loop's table base hoisted above the loop)
        uint64_t f = site;
        for (int k = 0; k < (1 << 16) && f > G->base && !(g_mark[f - G->base] & M_FUNC); k++) f--;
        int64_t last[16]; for (int k = 0; k < 16; k++) last[k] = -1;
        for (uint64_t a = f; a < site; a++) {
            if (!(g_mark[a - G->base] & M_SEEN)) continue;
            x86_insn d; if (!decode_at(a, &d)) continue;
            if (d.map == X86_MAP_1 && d.opcode == 0x8D && d.rip_relative && d.reg < 16) last[d.reg] = (int64_t)(a + d.len) + d.disp;
            a += d.len - 1u;
        }
        if (table < 0 && load_base >= 0 && load_base < 16 && last[load_base] >= 0) table = last[load_base] + disp;
        if (!add_base && add_reg >= 0 && add_reg < 16 && last[add_reg] >= 0) add_base = last[add_reg];
    }
    if (esize == 4 && (!have_add || table < 0 || add_base == 0)) return;
    if (esize == 0 || table < 0) return;
    g_tables++;
    if (bound < 0) g_unbounded++;
    int64_t limit = bound >= 0 && bound <= 65536 ? bound : 4096;
    for (int64_t k = 0; k < limit; k++) {
        uint64_t ea = (uint64_t)table + (uint64_t)(k * esize);
        if (!in_image(G, ea) || !in_image(G, ea + (uint64_t)esize - 1)) break;
        uint64_t t;
        if (esize == 8) t = r64(G->mem + (ea - G->base));
        else { uint32_t v = r32(G->mem + (ea - G->base)); t = (uint64_t)add_base + (sign ? (uint64_t)(int64_t)(int32_t)v : (uint64_t)v); }
        if (!exec_va(G, t)) { if (bound < 0) break; else continue; }
        if (bound < 0) { x86_insn tt; if (!decode_at(t, &tt)) break; }
        for (int b = 0; b < esize; b++) g_mark[ea - G->base + (uint64_t)b] |= M_TABLE;
        g_table_entries++;
        push(t, R_TABLE);
    }
}

static void descend(void) {
    while (g_nwork) {
        uint64_t va = g_work[--g_nwork];
        for (;;) {
            uint32_t r = (uint32_t)(va - G->base);
            if (!exec_va(G, va)) break;
            if (g_mark[r] & M_SEEN) break;
            x86_insn d;
            if (!decode_at(va, &d)) { g_bad++; break; }
            g_mark[r] |= M_SEEN;
            g_len[r] = d.len;
            for (int k = 0; k < d.len && r + (uint32_t)k < G->size; k++) g_mark[r + (uint32_t)k] |= M_CODE;
            g_insns++;
            uint64_t next = va + d.len;
            int op = d.opcode, ext = d.reg & 7;
            if (d.map == X86_MAP_1 && op == 0x8D && d.rip_relative) push(next + (uint64_t)(int64_t)d.disp, R_LEA);
            if (d.map == X86_MAP_1 && op >= 0xB8 && op <= 0xBF && (d.rex & 8) && exec_va(G, (uint64_t)d.imm)) push((uint64_t)d.imm, R_IMM);
            if (d.map == X86_MAP_1 && ((op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3))) {
                push(next + (uint64_t)d.imm, R_BRANCH);
                if (exec_va(G, next)) g_mark[next - G->base] |= M_LEADER;
                va = next; continue;
            }
            if (d.map == X86_MAP_0F && op >= 0x80 && op <= 0x8F) {
                push(next + (uint64_t)d.imm, R_BRANCH);
                if (exec_va(G, next)) g_mark[next - G->base] |= M_LEADER;
                va = next; continue;
            }
            if (d.map == X86_MAP_1 && (op == 0xEB || op == 0xE9)) { push(next + (uint64_t)d.imm, R_BRANCH); break; }
            if (d.map == X86_MAP_1 && op == 0xE8) {
                push(next + (uint64_t)d.imm, R_CALL);
                if (exec_va(G, next)) g_mark[next - G->base] |= M_LEADER;
                va = next; continue;
            }
            if (d.map == X86_MAP_1 && op == 0xFF && (ext == 2 || ext == 3)) {
                g_call_ind++;
                if (exec_va(G, next)) g_mark[next - G->base] |= M_LEADER;
                va = next; continue;
            }
            if (d.map == X86_MAP_1 && op == 0xFF && (ext == 4 || ext == 5)) {
                if (d.mod == 3) {
                    g_jmp_reg++;
                    if (g_njsites == g_cjsites) { g_cjsites = g_cjsites ? 2 * g_cjsites : 256; g_jsites = realloc(g_jsites, g_cjsites * sizeof *g_jsites); }
                    g_jsites[g_njsites++] = va;
                } else g_jmp_mem++;
                break;
            }
            if (d.map == X86_MAP_1 && (op == 0xC3 || op == 0xC2 || op == 0xCB || op == 0xCA || op == 0xCC || op == 0xF4 || op == 0xCF)) break;
            if (d.map == X86_MAP_1 && op == 0xCD && (d.imm & 0xFF) == 0x29) break;          // __fastfail
            if (d.map == X86_MAP_0F && (op == 0x0B || op == 0xFF || op == 0xB9)) break;     // UD2 / UD0 / UD1
            va = next;
        }
        if (!g_nwork && g_njsites) {                       // (then the jump tables, with what decoded before them)
            size_t n = g_njsites; uint64_t *sites = malloc(n * sizeof *sites);
            memcpy(sites, g_jsites, n * sizeof *sites); g_njsites = 0;
            for (size_t i = 0; i < n; i++) jump_table(sites[i]);
            free(sites);
        }
    }
}

// Exception handlers' data: __C_specific_handler's scope table (filters, __except targets, __finally handlers),
// __CxxFrameHandler3's FuncInfo (catch and unwind funclets), __gxx_personality_seh0's LSDA (landing pads).
static uint64_t g_scopes, g_lsda, g_fh3, g_fh4, g_other_handlers;
static uint64_t uleb(const uint8_t **p) { uint64_t v = 0; int s = 0; for (;;) { uint8_t b = *(*p)++; v |= (uint64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) return v; } }
static uint64_t enc_read(const uint8_t **p, uint8_t enc) {
    uint64_t v = 0;
    switch (enc & 0x0f) {
    case 0x01: return uleb(p);
    case 0x03: v = r32(*p); *p += 4; return v;
    case 0x0b: v = (uint64_t)(int64_t)(int32_t)r32(*p); *p += 4; return v;
    case 0x04: v = r64(*p); *p += 8; return v;
    case 0x02: v = r16(*p); *p += 2; return v;
    default: v = r32(*p); *p += 4; return v;
    }
}
static const char *handler_name(uint64_t va) {
    x86_insn d;
    if (!exec_va(G, va) || !decode_at(va, &d)) return NULL;
    if (d.map == X86_MAP_1 && d.opcode == 0xFF && (d.reg & 7) == 4 && d.rip_relative) return import_at(va + d.len + (uint64_t)(int64_t)d.disp);
    return NULL;
}
// The handlers' data, parsed by kind. live 0: only checked (a statically linked handler is recognized by the shape of
// all its functions' data, below); 1: its code addresses are roots. Each returns whether the data has that shape.
static int rd_ok(uint32_t rva, uint32_t n) { return rva && (uint64_t)rva + n <= G->size; }
static int code_rva(uint32_t rva) { return rva && exec_rva(G, rva); }
static int scope_table(uint32_t data, int live) {
    // __C_specific_handler (also __GSHandlerCheck_SEH's first part): count, then {begin, end, handler or filter, target}
    if (!rd_ok(data, 4)) return 0;
    uint32_t n = r32(G->mem + data);
    if (!n || n > 256 || !rd_ok(data, 4 + 16 * n)) return 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t *e = G->mem + data + 4 + 16 * k;
        uint32_t b = r32(e), en = r32(e + 4), filter = r32(e + 8), target = r32(e + 12);
        if (!code_rva(b) || !code_rva(en - 1) || en <= b || en - b > (1u << 24)) return 0;
        if (filter > 1 && !code_rva(filter)) return 0;
        if (target && !code_rva(target)) return 0;
        if (!target && filter <= 1) return 0;                     // (a __finally has its handler there)
    }
    if (live) for (uint32_t k = 0; k < n; k++) {
        const uint8_t *e = G->mem + data + 4 + 16 * k;
        uint32_t filter = r32(e + 8), target = r32(e + 12);
        if (filter > 1) push(G->base + filter, R_SCOPE);
        if (target) push(G->base + target, R_SCOPE);
        g_scopes++;
    }
    return 1;
}
// __CxxFrameHandler4's FuncInfo4 (ehdata4_export.h): compressed unsigned numbers, int32 displacements (RVAs).
typedef struct { const uint8_t *p, *end; int bad; } rd4;
static uint32_t u4(rd4 *r) {
    if (r->p >= r->end) { r->bad = 1; return 0; }
    uint8_t b = r->p[0];
    int len = !(b & 1) ? 1 : (b & 3) == 1 ? 2 : (b & 7) == 3 ? 3 : (b & 15) == 7 ? 4 : 5;
    if (r->p + len > r->end) { r->bad = 1; return 0; }
    uint32_t v;
    if (len == 5) v = r32(r->p + 1);
    else { v = 0; for (int i = len - 1; i >= 0; i--) v = v << 8 | r->p[i]; v >>= len; }
    r->p += len;
    return v;
}
static uint32_t i4(rd4 *r) { if (r->p + 4 > r->end) { r->bad = 1; return 0; } uint32_t v = r32(r->p); r->p += 4; return v; }
static rd4 at4(uint32_t rva) { rd4 r = { G->mem + rva, G->mem + G->size, !rd_ok(rva, 1) }; return r; }
static int fh4(uint32_t data, uint32_t func, int live) {
    if (!rd_ok(data, 4)) return 0;
    uint32_t fi = r32(G->mem + data);
    if (!rd_ok(fi, 1) || exec_rva(G, fi)) return 0;
    rd4 r = at4(fi);
    uint8_t h = *r.p++;
    int is_catch = h & 1, bbt = h & 4, has_uw = h & 8, has_try = h & 16;
    if (h & 0x80) return 0;                                       // (h & 2, separated code: the IP-to-state maps differ, not read)
    if (bbt) u4(&r);
    uint32_t uw = has_uw ? i4(&r) : 0, tb = has_try ? i4(&r) : 0, ip = i4(&r);
    if (is_catch) u4(&r);
    if (r.bad || (has_uw && !rd_ok(uw, 1)) || (has_try && !rd_ok(tb, 1)) || !rd_ok(ip, 1) || exec_rva(G, ip)) return 0;
    uint32_t roots[4096]; int nr = 0;
    if (has_uw) {
        rd4 u = at4(uw);
        uint32_t n = u4(&u);
        if (n > 100000) return 0;
        for (uint32_t k = 0; k < n && !u.bad; k++) {
            uint32_t t = u4(&u), type = t & 3;
            if (type) { uint32_t a = i4(&u); if (!code_rva(a)) return 0; if (nr < 4096) roots[nr++] = a; }
            if (type == 1 || type == 2) u4(&u);
        }
        if (u.bad) return 0;
    }
    if (has_try) {
        rd4 t = at4(tb);
        uint32_t n = u4(&t);
        if (n > 10000) return 0;
        for (uint32_t k = 0; k < n && !t.bad; k++) {
            u4(&t); u4(&t); u4(&t);
            uint32_t ha = i4(&t);
            if (t.bad || !rd_ok(ha, 1)) return 0;
            rd4 c = at4(ha);
            uint32_t nh = u4(&c);
            if (nh > 256) return 0;
            for (uint32_t j = 0; j < nh && !c.bad; j++) {
                uint8_t hh = *c.p++;
                if (hh & 0xC0) return 0;
                if (hh & 1) u4(&c);                                  // adjectives
                if (hh & 2) i4(&c);                                  // type descriptor
                if (hh & 4) u4(&c);                                  // catch object's frame offset
                uint32_t fn = i4(&c);
                if (!code_rva(fn)) return 0;
                if (nr < 4096) roots[nr++] = fn;
                for (int q = 0; q < ((hh >> 4) & 3); q++) {          // continuations: RVAs, or offsets from the function
                    uint32_t a = (hh & 8) ? i4(&c) : func + u4(&c);
                    if (c.bad || !code_rva(a)) return 0;
                    if (nr < 4096) roots[nr++] = a;
                }
            }
            if (c.bad) return 0;
        }
        if (t.bad) return 0;
    }
    if (live) { for (int k = 0; k < nr; k++) push(G->base + roots[k], R_FH4); g_fh4++; }
    return 1;
}
static int fh3(uint32_t data, int live) {
    if (!rd_ok(data, 4)) return 0;
    uint32_t f = r32(G->mem + data);
    if (!rd_ok(f, 40)) return 0;
    const uint8_t *fi = G->mem + f;
    uint32_t magic = r32(fi);
    if (magic < 0x19930520 || magic > 0x19930522) return 0;
    uint32_t nunwind = r32(fi + 4), unwind = r32(fi + 8), ntry = r32(fi + 12), tries = r32(fi + 16);
    if ((nunwind && !rd_ok(unwind, 8 * nunwind)) || (ntry && !rd_ok(tries, 20 * ntry))) return 0;
    if (!live) return 1;
    for (uint32_t k = 0; k < nunwind && k < 4096; k++) { uint32_t a = r32(G->mem + unwind + 8 * k + 4); if (code_rva(a)) push(G->base + a, R_FH3); }
    for (uint32_t t = 0; t < ntry && t < 1024; t++) {
        const uint8_t *tb = G->mem + tries + 20 * t;
        uint32_t ncatch = r32(tb + 12), arr = r32(tb + 16);
        for (uint32_t c = 0; c < ncatch && c < 64 && rd_ok(arr, 20 * (c + 1)); c++) { uint32_t h = r32(G->mem + arr + 20 * c + 12); if (code_rva(h)) push(G->base + h, R_FH3); }
    }
    g_fh3++;
    return 1;
}
// Handlers without an imported name (a statically linked CRT, as Cyberpunk's): each kept with its functions' data until
// all .pdata is read, then recognized by shape: the kind whose parse fits (nearly) all of the handler's functions.
typedef struct { uint32_t handler, begin, data; } hsite;
static hsite *g_hs; static size_t g_nhs, g_chs;
static uint64_t g_shape[5];                                    // handlers recognized: C scopes, FH4, FH3, GCC, none
static void exception_data(uint32_t begin, uint32_t uw) {
    const uint8_t *u = G->mem + uw;
    uint8_t flags = u[0] >> 3, ncodes = u[2];
    if (!(flags & 3) || (flags & 4)) return;
    uint32_t at = uw + 4 + 2 * ((ncodes + 1u) & ~1u);
    uint32_t handler = r32(G->mem + at), data = at + 4;
    push(G->base + handler, R_HANDLER);                         // (the runtime calls it: a root of its own)
    const char *name = handler_name(G->base + handler);
    if (name && !strcmp(name, "__C_specific_handler")) scope_table(data, 1);
    else if (name && !strcmp(name, "__gxx_personality_seh0")) {
        const uint8_t *p = G->mem + r32(G->mem + data);
        uint8_t lpenc = *p++;
        uint64_t lpstart = G->base + begin;
        if (lpenc != 0xff) lpstart = enc_read(&p, lpenc);
        uint8_t ttenc = *p++;
        if (ttenc != 0xff) uleb(&p);
        uint8_t csenc = *p++;
        uint64_t cslen = uleb(&p);
        const uint8_t *end = p + cslen;
        while (p < end) {
            enc_read(&p, csenc); enc_read(&p, csenc);
            uint64_t lp = enc_read(&p, csenc);
            uleb(&p);
            if (lp) { push(lpstart + lp, R_LANDING); g_lsda++; }
        }
    } else if (name && (!strcmp(name, "__CxxFrameHandler3") || !strcmp(name, "__CxxFrameHandler"))) fh3(data, 1);
    else if (name && !strcmp(name, "__CxxFrameHandler4")) fh4(data, begin, 1);
    else {
        if (g_nhs == g_chs) { g_chs = g_chs ? 2 * g_chs : 4096; g_hs = realloc(g_hs, g_chs * sizeof *g_hs); }
        g_hs[g_nhs++] = (hsite){ handler, begin, data };
    }
}
static int cmp_hs(const void *a, const void *b) { uint32_t x = ((const hsite *)a)->handler, y = ((const hsite *)b)->handler; return x < y ? -1 : x > y; }
static void handlers_by_shape(void) {
    qsort(g_hs, g_nhs, sizeof *g_hs, cmp_hs);
    for (size_t i = 0; i < g_nhs;) {
        size_t j = i; while (j < g_nhs && g_hs[j].handler == g_hs[i].handler) j++;
        size_t n = j - i, c = 0, f4 = 0, f3 = 0;
        for (size_t k = i; k < j; k++) { c += scope_table(g_hs[k].data, 0); f4 += fh4(g_hs[k].data, g_hs[k].begin, 0); f3 += fh3(g_hs[k].data, 0); }
        // (most of them: a function's data the parse rejects is still read where it can be; ties go to the stricter FH4)
        int kind = 4; size_t best = 0;
        if (f4 > best) { kind = 1; best = f4; }
        if (f3 > best) { kind = 2; best = f3; }
        if (c > best) { kind = 0; best = c; }
        if (best * 10 < n * 9) kind = 4;
        if (getenv("VXRT_AOT_HANDLERS"))
            fprintf(stderr, "handler %#llx: %zu functions; shapes: C scopes %zu, FH4 %zu, FH3 %zu -> %s\n", (unsigned long long)(G->base + g_hs[i].handler),
                    n, c, f4, f3, (const char *[]){ "C scopes", "FH4", "FH3", "GCC", "none" }[kind]);
        for (size_t k = i; k < j; k++) {
            if (kind == 0) scope_table(g_hs[k].data, 1);
            else if (kind == 1) fh4(g_hs[k].data, g_hs[k].begin, 1);
            else if (kind == 2) fh3(g_hs[k].data, 1);
            else g_other_handlers++;
        }
        g_shape[kind]++;
        i = j;
    }
    g_nhs = 0;
}

static uint64_t g_npdata, g_pdata_chained;
static void roots(const char *addresses) {
    push(G->base + G->entry, R_ENTRY);
    if (G->dir_size[0]) {                                                   // exports
        const uint8_t *e = G->mem + G->dir_rva[0];
        uint32_t nfun = r32(e + 20), funcs = r32(e + 28);
        for (uint32_t k = 0; k < nfun; k++) {
            uint32_t rva = r32(G->mem + funcs + 4 * k);
            if (rva && !(rva >= G->dir_rva[0] && rva < G->dir_rva[0] + G->dir_size[0])) push(G->base + rva, R_EXPORT);
        }
    }
    if (G->dir_size[9]) {                                                   // TLS callbacks
        uint64_t cbs = r64(G->mem + G->dir_rva[9] + 24);
        for (uint64_t a = cbs; in_image(G, a) && r64(G->mem + (a - G->base)); a += 8) push(r64(G->mem + (a - G->base)), R_TLS);
    }
    for (uint32_t k = 0; k < G->dir_size[3] / 12; k++) {                    // .pdata, and the handlers' data
        const uint8_t *p = G->mem + G->dir_rva[3] + 12 * k;
        uint32_t b = r32(p), uw = r32(p + 8);
        g_npdata++;
        if (uw & 1) continue;
        if ((G->mem[uw] >> 3) & 4) g_pdata_chained++;
        push(G->base + b, R_PDATA);
        exception_data(b, uw);
    }
    handlers_by_shape();
    if (G->dir_size[5]) {                                                   // base relocations: pointers into code
        for (const uint8_t *b = G->mem + G->dir_rva[5]; b < G->mem + G->dir_rva[5] + G->dir_size[5];) {
            uint32_t page = r32(b), size = r32(b + 4);
            if (size < 8) break;
            for (uint32_t k = 8; k < size; k += 2) {
                uint16_t e = r16(b + k);
                if ((e >> 12) != 10) continue;
                uint32_t slot = page + (e & 0xfff);
                if (exec_rva(G, slot)) continue;                            // (a pointer inside code: data there)
                push(r64(G->mem + slot), R_RELOC);
            }
            b += size;
        }
    }
    // Image-relative references (x64 MSVC metadata: ThrowInfo's destructor, CatchableType's copy constructor, RTTI, ...
    // are RVAs, which no relocation names): 32-bit words in data sections that are the RVA of 16-byte aligned code or of
    // a .pdata function. Candidates only (a wrong one is dead code).
    for (int i = 0; i < G->nsec; i++) {
        const section *s = &G->sec[i];
        if (s->flags & 0x20000000) continue;
        if (s->va == G->dir_rva[3] || s->va == G->dir_rva[5]) continue;          // (.pdata: read above; .reloc)
        for (uint32_t r = s->va; r + 4 <= s->va + s->vsize && r + 4 <= G->size; r += 4) {
            uint32_t v = r32(G->mem + r);
            if (v && exec_rva(G, v) && (!(v & 15) || (g_mark[v] & M_FUNC))) push(G->base + v, R_RVA);
        }
    }
    if (addresses) {                                                        // a list of addresses: "0001:<offset>" or hex RVAs
        FILE *f = fopen(addresses, "r");
        if (!f) die("cannot open %s", addresses);
        char buf[1 << 16]; size_t n;
        char tail[32] = { 0 };
        // (CDPR's JSON: "offset": "0001:0018971c", the first section's offset; read as a stream)
        while ((n = fread(buf, 1, sizeof buf - 1, f)) > 0) {
            buf[n] = 0;
            for (char *p = buf; (p = strstr(p, "\"0001:")); p += 6) {
                if (p + 15 > buf + n) break;
                uint32_t off = (uint32_t)strtoul(p + 6, NULL, 16);
                push(G->base + G->sec[0].va + off, R_LIST);
            }
            (void)tail;
        }
        fclose(f);
    }
}

static int analyze(const image *im, const char *addresses, int quiet) {
    G = im;
    g_len = calloc(im->size, 1); g_mark = calloc(im->size, 1);
    read_imports(im);
    clock_t t0 = clock();
    roots(addresses);
    descend();
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    if (quiet) return 0;
    printf("%s: base %#llx, %u bytes, hash %016llx\n", im->name, (unsigned long long)im->base, im->size, (unsigned long long)im->hash);
    printf("roots (found / not reached from earlier roots):\n");
    for (int k = 0; k < R_NKINDS; k++) if (g_roots[k]) printf("  %-30s %9llu %9llu\n", root_names[k], (unsigned long long)g_roots[k], (unsigned long long)g_roots_new[k]);
    uint64_t leaders = 0, funcs = 0, code = 0, table = 0, pad = 0, unknown = 0, xbytes = 0;
    for (int i = 0; i < im->nsec; i++) {
        const section *s = &im->sec[i];
        if (!(s->flags & 0x20000000)) continue;
        for (uint32_t r = s->va; r < s->va + s->vsize && r < im->size; r++) {
            xbytes++;
            uint8_t m = g_mark[r], b = im->mem[r];
            leaders += (m & M_LEADER) && (m & M_SEEN); funcs += (m & M_FUNC) && (m & M_SEEN);
            if (m & M_CODE) code++;
            else if (m & M_TABLE) table++;
            else if (b == 0xCC || b == 0x90 || b == 0x00) pad++;
            else unknown++;
        }
    }
    printf("descent: %llu instructions decoded, %llu undecodable starts, %llu block leaders, %llu function starts (%.2f s)\n",
           (unsigned long long)g_insns, (unsigned long long)g_bad, (unsigned long long)leaders, (unsigned long long)funcs, secs);
    printf("indirect: %llu calls, %llu jmp [mem], %llu jmp reg; jump tables %llu (%llu without a bound found), %llu entries\n",
           (unsigned long long)g_call_ind, (unsigned long long)g_jmp_mem, (unsigned long long)g_jmp_reg, (unsigned long long)g_tables,
           (unsigned long long)g_unbounded, (unsigned long long)g_table_entries);
    printf("handlers: %llu C scopes, %llu GCC landing pads, %llu FH3 functions, %llu FH4 functions, %llu others; handlers without a name"
           " recognized by shape: %llu C, %llu FH4, %llu FH3, %llu not\n",
           (unsigned long long)g_scopes, (unsigned long long)g_lsda, (unsigned long long)g_fh3, (unsigned long long)g_fh4, (unsigned long long)g_other_handlers,
           (unsigned long long)g_shape[0], (unsigned long long)g_shape[1], (unsigned long long)g_shape[2], (unsigned long long)g_shape[4]);
    printf(".pdata: %llu entries (%llu chained)\n", (unsigned long long)g_npdata, (unsigned long long)g_pdata_chained);
    printf("code sections: %llu bytes: %.2f%% instructions, %.2f%% jump tables, %.2f%% padding (CC/90/00), %.2f%% unexplained\n",
           (unsigned long long)xbytes, 100.0 * (double)code / (double)xbytes, 100.0 * (double)table / (double)xbytes,
           100.0 * (double)pad / (double)xbytes, 100.0 * (double)unknown / (double)xbytes);
    return 0;
}

// ---- route B: the translator's blocks as assembly ------------------------------------------------------------------
typedef struct { uint64_t rip; uint32_t *code; size_t n; xlat_buf b; } tblock;
static tblock *g_blocks; static size_t g_nblocks, g_cblocks;
static uint64_t *g_bmap; static size_t g_bcap;              // rip -> index + 1 (open addressing, 2 words per entry)
static uint64_t g_failed, g_failed_unique;
static size_t bmap_find(uint64_t rip) {
    size_t mask = g_bcap - 1, i = (size_t)((rip * 0x9E3779B97F4A7C15ull) >> 20) & mask;
    while (g_bmap[2 * i] && g_bmap[2 * i] != rip) i = (i + 1) & mask;
    return i;
}
enum { X86_RETURN_STOP = 0 };
static void gen_blocks(const image *im, uint64_t stop) {
    g_bcap = 1 << 16;
    while (g_bcap < 4 * (size_t)g_insns) g_bcap <<= 1;
    g_bmap = calloc(2 * g_bcap, sizeof *g_bmap);
    uint64_t *work = NULL; size_t nwork = 0, cwork = 0;
    #define WPUSH(v) do { if (nwork == cwork) { cwork = cwork ? 2 * cwork : 4096; work = realloc(work, cwork * sizeof *work); } work[nwork++] = (v); } while (0)
    for (uint32_t r = 0; r < im->size; r++) if ((g_mark[r] & M_LEADER) && (g_mark[r] & M_SEEN)) WPUSH(im->base + r);
    while (nwork) {
        uint64_t rip = work[--nwork];
        if (!exec_va(im, rip)) continue;
        size_t slot = bmap_find(rip);
        if (g_bmap[2 * slot]) continue;
        g_bmap[2 * slot] = rip;
        xlat_buf b = { 0 };
        b.ordered = 1; b.stop = stop; b.lazy_flags = 1; b.aot = 1;
        uint64_t end = rip + 1;
        if (xlat_block_linked(&b, rip, 64, &end) < 0) {
            g_failed++; g_failed_unique++;
            if (getenv("VXRT_AOT_REFUSED")) fprintf(stderr, "refused %#llx\n", (unsigned long long)rip);
            g_bmap[2 * slot + 1] = 0;
            free(b.code); free(b.relocs); free(b.imap);
            continue;
        }
        if (g_nblocks == g_cblocks) { g_cblocks = g_cblocks ? 2 * g_cblocks : 4096; g_blocks = realloc(g_blocks, g_cblocks * sizeof *g_blocks); }
        g_blocks[g_nblocks] = (tblock){ rip, b.code, b.n, b };
        g_bmap[2 * slot + 1] = ++g_nblocks;
        for (size_t i = 0; i < b.nrelocs; i++) {
            uint32_t *p = b.code + b.relocs[i].at;
            if (b.relocs[i].kind == XR_LINK) WPUSH((uint64_t)p[1] | (uint64_t)p[2] << 32);
            else if (b.relocs[i].kind == XR_CALL) WPUSH((uint64_t)p[4] | (uint64_t)p[5] << 32);
        }
    }
    free(work);
}
static int64_t block_index(uint64_t rip) { size_t s = bmap_find(rip); return g_bmap[2 * s] == rip && g_bmap[2 * s + 1] ? (int64_t)g_bmap[2 * s + 1] - 1 : -1; }
static int cmp_blk(const void *a, const void *b) { uint64_t x = ((const tblock *)a)->rip, y = ((const tblock *)b)->rip; return x < y ? -1 : x > y; }

// Parts: a module's code must reach its own blocks with B/BL (±128 MB), so an image with more code than that is
// split into parts, each a dylib of its own covering a range of x86 addresses (blocks in address order). A branch to a
// block in another part goes through a veneer here that loads the block's host address from a slot (xa_load fills
// it); the lookup sends an address of another part's range to that part's lookup (its address in a slot too).
static int *g_part;                                         // block index -> part
static uint64_t *g_part_lo; static int g_nparts;            // each part's first x86 address (part 0: the image's base)
static int g_cur;                                           // the part being written
typedef struct { uint64_t *rip; size_t n, cap; uint64_t *set; size_t setcap; } xrefs;
static xrefs g_x;
static void xref_add(uint64_t rip) {
    size_t mask = g_x.setcap - 1, i = (size_t)((rip * 0x9E3779B97F4A7C15ull) >> 20) & mask;
    while (g_x.set[i] && g_x.set[i] != rip) i = (i + 1) & mask;
    if (g_x.set[i]) return;
    g_x.set[i] = rip;
    if (g_x.n == g_x.cap) { g_x.cap = g_x.cap ? 2 * g_x.cap : 4096; g_x.rip = realloc(g_x.rip, g_x.cap * sizeof *g_x.rip); }
    g_x.rip[g_x.n++] = rip;
}
// The words: runs of plain words in .inst lines of 8 (the file is several GB for a large image otherwise).
static void emit_words(FILE *o, const uint32_t *w, size_t n) {
    for (size_t i = 0; i < n; i += 8) {
        fprintf(o, "\t.inst 0x%08x", w[i]);
        for (size_t j = i + 1; j < n && j < i + 8; j++) fprintf(o, ",0x%08x", w[j]);
        fputc('\n', o);
    }
}
static void emit_branch(FILE *o, const char *op, uint64_t to, const char *miss) {
    int64_t k = block_index(to);
    if (k < 0) fprintf(o, "\tbl %s\n", miss);
    else if (g_part[k] == g_cur) fprintf(o, "\t%s LB%llx\n", op, (unsigned long long)to);
    else { xref_add(to); fprintf(o, "\t%s LV%llx\n", op, (unsigned long long)to); }
}
static void emit_code2(FILE *o, const xlat_buf *b, uint64_t rip) {
    size_t ri = 0, mi = 0, run = 0;
    for (size_t k = 0; k <= b->n; k++) {
        int site = 0, label = rip && mi < b->nimap && b->imap[mi].at == k;
        while (ri < b->nrelocs && b->relocs[ri].at < k) ri++;
        if (k < b->n && ri < b->nrelocs && b->relocs[ri].at == k) site = 1;
        if (site || label || k == b->n) { emit_words(o, b->code + run, k - run); run = k; }
        if (k == b->n) break;
        while (rip && mi < b->nimap && b->imap[mi].at == k) { fprintf(o, "LI%llx_%zu:\n", (unsigned long long)rip, mi); mi++; }
        if (!site) continue;
        uint32_t kind = b->relocs[ri].kind; const uint32_t *p = b->code + k;
        if (kind == XR_LINK) emit_branch(o, "b", (uint64_t)p[1] | (uint64_t)p[2] << 32, "Lmiss_link");
        else if (kind == XR_CALL) emit_branch(o, "bl", (uint64_t)p[4] | (uint64_t)p[5] << 32, "Lmiss_call");
        else if (kind == XR_LOOKUP) fprintf(o, "\tb Llookup\n");
        else if (kind == XR_LOOKUP_CALL) fprintf(o, "\tbl Llookup\n");
        else if (kind == XR_EXIT) fprintf(o, "\tb Lexit\n");
        else if (kind == XR_FLAGS) fprintf(o, "\tbl Lflags\n");
        else if (kind == XR_ABS) {
            int which = (int)(p[0] >> 5), reg = (int)(p[0] & 31);
            fprintf(o, "\tadrp x%d, Lext%d@PAGE\n\tldr x%d, [x%d, Lext%d@PAGEOFF]\n\tnop\n\tnop\n", reg, which, reg, reg, which);
            k += 3;
        } else die("unknown site kind %u", kind);
        ri++;
        run = k + 1;
    }
}
static void mov64(FILE *o, int reg, uint64_t v) {
    fprintf(o, "\tmovz x%d, #%#llx\n", reg, (unsigned long long)(v & 0xffff));
    for (int s = 16; s < 64; s += 16) if ((v >> s) & 0xffff) fprintf(o, "\tmovk x%d, #%#llx, lsl #%d\n", reg, (unsigned long long)((v >> s) & 0xffff), s);
}

static void write_part(const image *im, FILE *o, size_t first, size_t end, uint64_t stop) {
    (void)stop;
    size_t nb = end - first;
    g_x.n = 0; g_x.setcap = 1024; while (g_x.setcap < 4 * nb + 1024) g_x.setcap <<= 1;
    free(g_x.set); g_x.set = calloc(g_x.setcap, sizeof *g_x.set);
    uint64_t lo = g_part_lo[g_cur], hi = g_cur + 1 < g_nparts ? g_part_lo[g_cur + 1] : im->base + im->size;
    fprintf(o, "// Generated by vxrt-aot gen from %s (hash %016llx): route B, the translator's code for %zu blocks (part %d of %d,"
               " %#llx-%#llx).\n// Not to be shared: it is the program's code, translated (RECOMP.md).\n",
            im->name, (unsigned long long)im->hash, nb, g_cur, g_nparts, (unsigned long long)lo, (unsigned long long)hi);
    fprintf(o, "\t.section __TEXT,__text,regular,pure_instructions\n\t.p2align 4\nLcode:\nLenter:\n");
    xlat_buf s = { 0 }; s.aot = 1;
    xlat_prologue(&s);
    emit_code2(o, &s, 0);
    free(s.code); free(s.relocs); memset(&s, 0, sizeof s); s.aot = 1;
    fprintf(o, "\tldr x16, [x28, #%d]\n\tbr x16\n", CTX_ENTRY);
    fprintf(o, "Lexit:\n\t.inst 0x%08x\n\tbl Lflags\n\tstr x17, [x28, #%d]\n", 0x36180000u | 2u << 5 | 27, CTX_RIP);
    xlat_epilogue(&s);
    emit_code2(o, &s, 0);
    free(s.code); free(s.relocs); memset(&s, 0, sizeof s); s.aot = 1;
    fprintf(o, "Lexit_unlinked:\n\tstr xzr, [x28, #%d]\n\tb Lexit\n", CTX_LINK);
    fprintf(o, "Lmiss_link:\n\tldr x17, [x30]\n\tb Lexit_unlinked\n");
    fprintf(o, "Lmiss_call:\n\tldr x17, [x30, #12]\n\tb Lexit_unlinked\n");
    // lookup: x17 the target. Outside this part's range: the part whose range has it (its lookup from a slot), or the
    // exit; inside: its block from the table (open addressing, probing forward to an empty entry).
    size_t tsize = 16; while (tsize < 2 * nb) tsize <<= 1;
    fprintf(o, "Llookup:\n\tstr xzr, [x28, #%d]\n", CTX_LINK);
    if (g_nparts > 1) {
        mov64(o, 16, im->base + im->size); fprintf(o, "\tcmp x17, x16\n\tb.hs Lexit\n");
        for (int p = g_nparts - 1; p >= 0; p--) {
            mov64(o, 16, g_part_lo[p]); fprintf(o, "\tcmp x17, x16\n\tb.hs Lpart%d\n", p);
        }
        fprintf(o, "\tb Lexit\n");
        for (int p = 0; p < g_nparts; p++) {
            if (p == g_cur) continue;
            fprintf(o, "Lpart%d:\n\tadrp x16, Lplk@PAGE\n\tadd x16, x16, Lplk@PAGEOFF\n\tldr x16, [x16, #%d]\n\tcbz x16, Lexit\n\tbr x16\n", p, 8 * p);
        }
        fprintf(o, "Lpart%d:\n", g_cur);
    }
    fprintf(o, "\teor x16, x17, x17, lsr #14\n\teor x16, x16, x17, lsr #29\n\tand x16, x16, #%#llx\n"
               "\tadrp x30, _vxaot_table@PAGE\n\tadd x30, x30, _vxaot_table@PAGEOFF\n\tadd x30, x30, x16, lsl #4\n"
               "1:\n\tldp x22, x16, [x30], #16\n\tcmp x22, x17\n\tb.eq 2f\n\tcbnz x22, 1b\n\tb Lexit\n"
               "2:\n\tadrp x30, Lcode@PAGE\n\tadd x30, x30, Lcode@PAGEOFF\n\tadd x16, x30, x16\n\tbr x16\n",
            (unsigned long long)(tsize - 1));
    fprintf(o, "Lflags:\n");
    xlat_flags_stub(&s);
    emit_code2(o, &s, 0);
    free(s.code); free(s.relocs);
    for (size_t i = first; i < end; i++) {
        tblock *t = &g_blocks[i];
        fprintf(o, "LB%llx:\n", (unsigned long long)t->rip);
        emit_code2(o, &t->b, t->rip);
        fprintf(o, "LE%llx:\n", (unsigned long long)t->rip);
    }
    // veneers to other parts' blocks: their host address from the slot (until it is filled: the exit with the address)
    for (size_t k = 0; k < g_x.n; k++)
        fprintf(o, "LV%llx:\n\tadrp x16, LX%llx@PAGE\n\tldr x16, [x16, LX%llx@PAGEOFF]\n\tbr x16\n", (unsigned long long)g_x.rip[k],
                (unsigned long long)g_x.rip[k], (unsigned long long)g_x.rip[k]);
    for (size_t k = 0; k < g_x.n; k++) { fprintf(o, "LW%llx:\n", (unsigned long long)g_x.rip[k]); mov64(o, 17, g_x.rip[k]); fprintf(o, "\tb Lexit_unlinked\n"); }
    // (a symbol, not an L label: ADRP to a label is section + addend, and Mach-O addends stop at 8 MB)
    fprintf(o, "Lcode_end:\n\t.p2align 4\n_vxaot_table:\n");
    size_t tcap = tsize + nb + 1;
    uint64_t *tab = calloc(tcap, sizeof *tab);
    size_t tused = 0;
    for (size_t i = first; i < end; i++) {
        uint64_t rip = g_blocks[i].rip;
        size_t k = (size_t)((rip ^ rip >> 14 ^ rip >> 29) & (tsize - 1));
        while (tab[k]) k++;
        tab[k] = i + 1; if (k + 1 > tused) tused = k + 1;
    }
    for (size_t k = 0; k <= tused && k < tcap; k++) {
        if (tab[k]) fprintf(o, "\t.quad %#llx, LB%llx - Lcode\n", (unsigned long long)g_blocks[tab[k] - 1].rip, (unsigned long long)g_blocks[tab[k] - 1].rip);
        else fprintf(o, "\t.quad 0, 0\n");
    }
    free(tab);
    fprintf(o, "\t.p2align 3\nLblocks:\n");
    for (size_t i = first; i < end; i++) fprintf(o, "\t.quad %#llx, LB%llx - Lcode\n", (unsigned long long)g_blocks[i].rip, (unsigned long long)g_blocks[i].rip);
    fprintf(o, "Limap:\n");
    size_t nimap = 0;
    for (size_t i = first; i < end; i++) {
        tblock *t = &g_blocks[i];
        for (size_t mi = 0; mi < t->b.nimap; mi++, nimap++)
            fprintf(o, "\t.long LI%llx_%zu - Lcode, %d\n\t.quad %#llx\n", (unsigned long long)t->rip, mi, t->b.imap[mi].sp, (unsigned long long)t->b.imap[mi].rip);
        fprintf(o, "\t.long LE%llx - Lcode, 0\n\t.quad 0\n", (unsigned long long)t->rip); nimap++;
    }
    fprintf(o, "Lxrip:\n");
    for (size_t k = 0; k < g_x.n; k++) fprintf(o, "\t.quad %#llx\n", (unsigned long long)g_x.rip[k]);
    fprintf(o, "\t.quad 0\n\t.section __DATA,__data\n\t.p2align 3\n");
    for (int k = 0; k < XA_NABS; k++) fprintf(o, "Lext%d:\n\t.quad 0\n", k);
    fprintf(o, "Lplk:\n"); for (int p = 0; p < g_nparts; p++) fprintf(o, "\t.quad 0\n");
    fprintf(o, "Lxslot:\n");
    for (size_t k = 0; k < g_x.n; k++) fprintf(o, "LX%llx:\n\t.quad LW%llx\n", (unsigned long long)g_x.rip[k], (unsigned long long)g_x.rip[k]);
    fprintf(o, "\t.quad 0\n\t.globl _vxrt_aot_module\n_vxrt_aot_module:\n\t.long %#x, %d\n\t.quad %#llx, %#x, %#llx, %d\n"
               "\t.quad Lcode, Lcode_end, Lenter\n\t.quad Lblocks, %zu\n\t.quad Limap, %zu\n"
               "\t.quad Lext0, %d\n\t.quad 0, 0\n"
               "\t.quad %#llx, %#llx\n\t.long %d, %d\n\t.quad Llookup, Lplk\n\t.quad Lxrip, Lxslot, %zu\n",
            XA_MAGIC, XA_VERSION, (unsigned long long)im->base, im->size, (unsigned long long)im->hash, XA_ORDERED, nb, nimap, XA_NABS,
            (unsigned long long)lo, (unsigned long long)hi, g_cur, g_nparts, g_x.n);
}

static int gen(const image *im, const char *out) {
    G = im;
    analyze(im, getenv("VXRT_AOT_ADDRESSES"), 1);
    setenv("XE_AFP", "0", 1);                                  // (portable: the NaN fixes in the code, not FPCR.AH)
    const uint64_t stop = 0xFFFFFFFFFFFF0000ull;               // (src/arm64.c's X86_RETURN, as the engine is given it)
    clock_t t0 = clock();
    gen_blocks(im, stop);
    qsort(g_blocks, g_nblocks, sizeof *g_blocks, cmp_blk);
    for (size_t i = 0; i < g_nblocks; i++) { size_t sl = bmap_find(g_blocks[i].rip); g_bmap[2 * sl + 1] = i + 1; }
    // parts: at most VXRT_AOT_PART_MB of code each (default 96: room for the veneers and stubs within 128 MB)
    uint64_t limit = (uint64_t)(getenv("VXRT_AOT_PART_MB") ? atof(getenv("VXRT_AOT_PART_MB")) * (1 << 20) : 96.0 * (1 << 20));
    if (limit < 4096) limit = 4096;
    g_part = malloc((g_nblocks + 1) * sizeof *g_part);
    size_t *starts = malloc((g_nblocks + 2) * sizeof *starts);
    g_part_lo = malloc((g_nblocks + 2) * sizeof *g_part_lo);
    uint64_t bytes = 0; g_nparts = 1; starts[0] = 0; g_part_lo[0] = im->base;
    for (size_t i = 0; i < g_nblocks; i++) {
        uint64_t sz = 4 * (uint64_t)g_blocks[i].n + 32;           // (+ a veneer, a miss stub: the worst case)
        if (bytes + sz > limit && i > starts[g_nparts - 1]) { starts[g_nparts] = i; g_part_lo[g_nparts] = g_blocks[i].rip; g_nparts++; bytes = 0; }
        bytes += sz;
        g_part[i] = g_nparts - 1;
    }
    starts[g_nparts] = g_nblocks;
    char path[4096];
    size_t ol = strlen(out); int has_s = ol > 2 && !strcmp(out + ol - 2, ".s");
    for (g_cur = 0; g_cur < g_nparts; g_cur++) {
        if (g_nparts == 1) snprintf(path, sizeof path, "%s", out);
        else snprintf(path, sizeof path, "%.*s.p%d.s", (int)(has_s ? ol - 2 : ol), out, g_cur);
        FILE *o = fopen(path, "w");
        if (!o) die("cannot write %s", path);
        static char buf[1 << 22]; setvbuf(o, buf, _IOFBF, sizeof buf);
        write_part(im, o, starts[g_cur], starts[g_cur + 1], stop);
        if (fclose(o)) die("cannot write %s", path);
        printf("%s\n", path);
    }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    uint64_t words = 0; for (size_t i = 0; i < g_nblocks; i++) words += g_blocks[i].n;
    fprintf(stderr, "vxrt-aot: %s: %zu blocks, %llu ARM64 words in %d part%s, %llu blocks the translator refused, %.2f s; module name %s-%016llx%s.dylib\n",
            im->name, g_nblocks, (unsigned long long)words, g_nparts, g_nparts > 1 ? "s" : "", (unsigned long long)g_failed, secs, im->name,
            (unsigned long long)im->hash, g_nparts > 1 ? ".p<k>" : "");
    return 0;
}

int main(int argc, char **argv) {
    g_argv = argv;
    if (argc < 3) { fprintf(stderr, "usage: vxrt-aot analyze|gen|name <pe> [out] [--addresses file]\n"); return 2; }
    image im = { 0 };
    load(&im, argv[2]);
    const char *addresses = NULL;
    for (int i = 3; i + 1 < argc; i++) if (!strcmp(argv[i], "--addresses")) addresses = argv[i + 1];
    if (!strcmp(argv[1], "name")) {
        char lower[256]; size_t k = 0;
        for (; im.name[k] && k + 1 < sizeof lower; k++) lower[k] = (char)tolower((unsigned char)im.name[k]);
        lower[k] = 0;
        printf("%s-%016llx.dylib\n", lower, (unsigned long long)im.hash);
        return 0;
    }
    if (!strcmp(argv[1], "analyze")) return analyze(&im, addresses, 0);
    if (!strcmp(argv[1], "gen") && argc >= 4) return gen(&im, argv[3]);
    fprintf(stderr, "vxrt-aot: unknown command %s\n", argv[1]);
    return 2;
}
