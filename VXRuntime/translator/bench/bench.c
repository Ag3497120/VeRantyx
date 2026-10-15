// The kernels (kernels.c, built for x86-64 into a static image at a fixed address) run on the engine: each called as
// rosetta_main.c calls it, timed the same way. usage: bench <image> [reps] [mode: 0 in-process, 1 ordered, 2 vCPU]
#include "../engine.h"
#include <fcntl.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
enum { STOP = 0x7FF000 };                                       // (a return address nothing lives at)

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: bench <image> [reps] [ordered]\n"); return 2; }
    int reps = argc > 2 ? atoi(argv[2]) : 3, mode = argc > 3 ? atoi(argv[3]) : 0, ordered = mode == 1;   // (2: in a vCPU)
    int fd = open(argv[1], O_RDONLY); struct stat st; fstat(fd, &st);
    uint8_t *f = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    const struct mach_header_64 *mh = (const void *)f;
    const uint8_t *lc = f + sizeof *mh;
    const struct symtab_command *sym = NULL;
    uint64_t lo = ~0ull, hi = 0;                                 // (x86 segments sit on 4 KB pages, the host's are 16 KB:
    for (int pass = 0; pass < 2; pass++) {                       //   one mapping for all of them, then each copied in)
        lc = f + sizeof *mh;
        if (pass == 1) {
            uint64_t a = lo & ~0x3FFFull, z = (hi + 0x3FFF) & ~0x3FFFull;
            if (mmap((void *)a, z - a, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != (void *)a) { fprintf(stderr, "bench: cannot map the image\n"); return 1; }
        }
        for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
            const struct load_command *l = (const void *)lc;
            if (l->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *sg = (const void *)lc;
                if (!sg->vmsize || !strcmp(sg->segname, "__PAGEZERO")) continue;
                if (pass == 0) { if (sg->vmaddr < lo) lo = sg->vmaddr; if (sg->vmaddr + sg->vmsize > hi) hi = sg->vmaddr + sg->vmsize; }
                else memcpy((void *)sg->vmaddr, f + sg->fileoff, sg->filesize);
            } else if (l->cmd == LC_SYMTAB) sym = (const void *)lc;
        }
    }
    static const struct { const char *name; uint64_t n; } K[] = {
        { "_k_crc", 40 }, { "_k_matmul", 40 }, { "_k_sort", 20 }, { "_k_tree", 10 },
        { "_k_fib", 40 }, { "_k_physics", 400 }, { "_k_interp", 40000 }, { "_k_string", 400 },
    };
    uint64_t addr[8] = { 0 };
    const struct nlist_64 *nl = (const void *)(f + sym->symoff);
    const char *strs = (const char *)f + sym->stroff;
    for (uint32_t i = 0; i < sym->nsyms; i++)
        for (int k = 0; k < 8; k++) if (!strcmp(strs + nl[i].n_un.n_strx, K[k].name)) addr[k] = nl[i].n_value;
    uint8_t *stack = mmap(NULL, 8 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (getenv("XV_DEBUG")) fprintf(stderr, "making the engine\n");
    xe *e = mode == 2 ? xe_new_vcpu() : xe_new(ordered);
    if (getenv("XV_DEBUG")) fprintf(stderr, "engine %p\n", (void *)e);
    if (!e) { fprintf(stderr, "bench: no engine (vCPU: the hypervisor entitlement?)\n"); return 1; }
    x86_ctx *cp = calloc(1, sizeof *cp);                         // (heap: memory a vCPU guest may have)
#define c (*cp)
    for (int k = 0; k < 8; k++) {
        if (!addr[k]) { fprintf(stderr, "bench: no %s\n", K[k].name); return 1; }
        uint64_t r = 0; double best = 1e9;
        for (int rep = -1; rep < reps; rep++) {                  // (rep -1: the warm-up call with 1, as rosetta_main.c)
            memset(&c, 0, sizeof c);
            c.mxcsr = 0x1F80; c.fcw = 0x037F;
            c.gpr[4] = (uint64_t)(uintptr_t)(stack + (8 << 20) - 64) - 8;
            *(uint64_t *)(uintptr_t)c.gpr[4] = STOP;            // (the return address)
            c.gpr[7] = rep < 0 ? 1 : K[k].n;                     // (RDI: SysV, as the image was built)
            c.rip = addr[k];
            double t0 = now();
            int res = xe_run(e, &c, STOP);
            double t = now() - t0;
            if (res) { fprintf(stderr, "bench: %s stopped: %d at %llx\n", K[k].name, res, (unsigned long long)c.rip); return 1; }
            r = c.gpr[0];
            if (rep >= 0 && t < best) best = t;
        }
        printf("%-8s %10.3f ms  %llu\n", K[k].name + 3, best * 1e3, (unsigned long long)r);
    }
    xe_stats s = xe_get_stats(e);
    printf("engine: %llu blocks, %llu x86 bytes -> %llu ARM instructions (%.2f per x86 byte), %llu linked, %llu entries, %llu split, "
           "flags not computed for %llu instructions, %llu Jccs fused\n",
           (unsigned long long)s.blocks, (unsigned long long)s.x86_bytes, (unsigned long long)s.arm_insns, (double)s.arm_insns / (double)s.x86_bytes,
           (unsigned long long)s.linked, (unsigned long long)s.entries, (unsigned long long)s.split_patches,
           (unsigned long long)s.flags_skipped, (unsigned long long)s.fused);
    return 0;
}
