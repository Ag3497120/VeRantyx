// Translator coverage over real programs: every function an x86-64 PE lists in .pdata, decoded instruction by
// instruction and translated (xlat_block, one instruction each). Prints the totals, then one line per kind of
// instruction the translator does not take yet: count, the decoder's key and the first instance's bytes.
// usage: coverage <PE files...>   (coverage.py adds the names and sums over many files)
#include "xlat.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { uint32_t key; long n; uint8_t bytes[15]; uint8_t len; } miss;
static miss misses[4096];
static int nmiss;
static long total, ok, undecodable;

static uint32_t key_of(const x86_insn *d) {                              // map, opcode, prefix, VEX, group (ModRM.reg)
    uint32_t pf = d->mandatory == 0x66 ? 1 : d->mandatory == 0xF3 ? 2 : d->mandatory == 0xF2 ? 3 : 0;
    uint32_t vex = d->prefixes & X86_P_EVEX ? 2 : d->prefixes & X86_P_VEX ? 1 : 0;
    return (uint32_t)d->map << 24 | (uint32_t)d->opcode << 16 | pf << 12 | vex << 10 | (uint32_t)(d->has_modrm ? (d->reg & 7) : 8) << 4 |
           (uint32_t)(d->has_modrm && d->mod == 3) << 3;
}

static void file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    struct stat st; fstat(fd, &st);
    uint8_t *p = mmap(NULL, (size_t)st.st_size + 16, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED || st.st_size < 0x200 || p[0] != 'M' || p[1] != 'Z') return;
    uint32_t pe = *(uint32_t *)(p + 0x3C);
    if (pe + 0x108 > (uint64_t)st.st_size || memcmp(p + pe, "PE\0\0", 4) || *(uint16_t *)(p + pe + 4) != 0x8664) goto done;
    uint16_t nsec = *(uint16_t *)(p + pe + 6), optsz = *(uint16_t *)(p + pe + 20);
    const uint8_t *opt = p + pe + 24, *sec = opt + optsz;
    uint32_t pdata = *(uint32_t *)(opt + 112 + 3 * 8), pdsize = *(uint32_t *)(opt + 112 + 3 * 8 + 4);
    #define RVA(r) ({ uint32_t r_ = (r); const uint8_t *res_ = NULL; for (int i_ = 0; i_ < nsec; i_++) { const uint8_t *s_ = sec + 40 * i_; \
        uint32_t va_ = *(uint32_t *)(s_ + 12), vs_ = *(uint32_t *)(s_ + 8), raw_ = *(uint32_t *)(s_ + 20), rs_ = *(uint32_t *)(s_ + 16); \
        if (r_ >= va_ && r_ < va_ + (vs_ > rs_ ? vs_ : rs_) && r_ - va_ < rs_) res_ = p + raw_ + (r_ - va_); } res_; })
    const uint32_t *rf = (const uint32_t *)RVA(pdata);
    if (!rf) goto done;
    for (uint32_t i = 0; i + 12 <= pdsize; i += 12, rf += 3) {
        uint32_t begin = rf[0], end = rf[1];
        const uint8_t *code = RVA(begin);
        if (!code || end <= begin || end - begin > (1u << 24) || code + (end - begin) > p + st.st_size) continue;
        for (uint32_t off = 0; off < end - begin; ) {
            x86_insn d;
            int len = x86_decode(code + off, end - begin - off, &d);
            total++;
            if (!len) { undecodable++; off++; continue; }
            xlat_buf b = { 0 };
            if (xlat_block(&b, (uint64_t)(uintptr_t)(code + off), 1) == 0) ok++;
            else {
                uint32_t k = key_of(&d);
                int j = 0;
                while (j < nmiss && misses[j].key != k) j++;
                if (j == nmiss && nmiss < 4096) { misses[j].key = k; memcpy(misses[j].bytes, code + off, (size_t)len); misses[j].len = (uint8_t)len; nmiss++; }
                if (j < nmiss) misses[j].n++;
            }
            free(b.code);
            off += (uint32_t)len;
        }
    }
done:
    munmap(p, (size_t)st.st_size + 16);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) file(argv[i]);
    printf("total %ld translated %ld undecodable %ld\n", total, ok, undecodable);
    for (int j = 0; j < nmiss; j++) {
        printf("miss %08x %ld", misses[j].key, misses[j].n);
        for (int k = 0; k < misses[j].len; k++) printf(" %02x", misses[j].bytes[k]);
        printf("\n");
    }
    return 0;
}
