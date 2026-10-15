// Checks the decoder against LLVM: reads `llvm-objdump -d --no-show-raw-insn <pe>` on stdin and decodes the PE's bytes
// at every address LLVM shows an instruction at; the length LLVM implies (the distance to the next instruction) must
// be ours. Prints the totals and the mismatches by mnemonic.
//   xcrun llvm-objdump -d --no-show-raw-insn game.dll | build/x86dec_check game.dll
#include "x86dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t va, size; uint64_t off; } section;
static section secs[96]; static int nsec;
static uint8_t *img; static size_t img_size;

static const uint8_t *at(uint64_t va, size_t *avail) {
    for (int i = 0; i < nsec; i++)
        if (va >= secs[i].va && va < secs[i].va + secs[i].size) {
            uint64_t o = secs[i].off + (va - secs[i].va);
            if (o >= img_size) return NULL;
            *avail = (size_t)(secs[i].va + secs[i].size - va);
            if (*avail > img_size - o) *avail = img_size - o;
            return img + o;
        }
    return NULL;
}

typedef struct { char name[24]; long n; char example[64]; } miss;
static miss misses[512]; static int nmiss;
static void note(const char *m, uint64_t va, const uint8_t *p, int want, int got) {
    int i;
    for (i = 0; i < nmiss && strcmp(misses[i].name, m); i++) {}
    if (i == nmiss) {
        if (nmiss == 512) return;
        snprintf(misses[i].name, sizeof misses[i].name, "%s", m);
        int k = snprintf(misses[i].example, sizeof misses[i].example, "%llx want %d got %d:", (unsigned long long)va, want, got);
        for (int j = 0; j < (want > got ? want : got) && j < 8 && k < 60; j++) k += snprintf(misses[i].example + k, sizeof misses[i].example - k, " %02x", p[j]);
        nmiss++;
    }
    misses[i].n++;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: llvm-objdump -d --no-show-raw-insn <pe> | x86dec_check <pe>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); img_size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    img = malloc(img_size);
    if (fread(img, 1, img_size, f) != img_size) return 2;
    fclose(f);
    uint32_t pe; memcpy(&pe, img + 0x3C, 4);
    uint16_t ns; memcpy(&ns, img + pe + 6, 2);
    uint16_t opt; memcpy(&opt, img + pe + 20, 2);
    uint64_t base; memcpy(&base, img + pe + 24 + 24, 8);
    const uint8_t *sh = img + pe + 24 + opt;
    for (int i = 0; i < ns && nsec < 96; i++, sh += 40) {
        uint32_t vs, va, rs, ro; memcpy(&vs, sh + 8, 4); memcpy(&va, sh + 12, 4); memcpy(&rs, sh + 16, 4); memcpy(&ro, sh + 20, 4);
        secs[nsec++] = (section){ base + va, rs < vs ? rs : vs, ro };
    }
    // All of LLVM's lines first: (address, mnemonic); a prefix LLVM shows on a line of its own is merged into the
    // instruction after it.
    typedef struct { uint64_t va; char m[24]; uint8_t gap; } entry;
    size_t cap = 1 << 20, n = 0;
    entry *e = malloc(cap * sizeof *e);
    char line[512];
    static const char *prefix_lines[] = { "lock", "rep", "repne", "repe", "xacquire", "xrelease", "data16", "data32",
                                          "addr32", "notrack", "bnd", "cs", "ds", "ss", "es", "fs", "gs", NULL };
    int gap = 1;
    while (fgets(line, sizeof line, stdin)) {
        char *colon = strchr(line, ':'), *s = line;
        while (*s == ' ') s++;
        char *t = colon ? colon + 1 : NULL;
        while (t && *t == ' ') t++;
        if (!colon || s == colon || !t || *t != '\t' || strchr(" <", *s)) { gap = 1; continue; }   // (labels, headers, "...")
        if (n == cap) e = realloc(e, (cap *= 2) * sizeof *e);
        e[n].va = strtoull(s, NULL, 16);
        sscanf(t + 1, "%23s", e[n].m);
        e[n].gap = (uint8_t)gap; gap = 0;
        int is_prefix = !strncmp(e[n].m, "rex", 3);
        for (int i = 0; prefix_lines[i]; i++) if (!strcmp(e[n].m, prefix_lines[i])) is_prefix = 1;
        const char *rest = strstr(t + 1, e[n].m) + strlen(e[n].m);
        while (*rest == ' ' || *rest == '\t') rest++;
        if (*rest && *rest != '\n') is_prefix = 0;           // ("rep movsb" on one line: an instruction)
        if (n && !e[n].gap && e[n - 1].m[0] == '+') {        // (the instruction after a prefix line: keep the prefix's address)
            uint64_t va = e[n - 1].va; uint8_t g = e[n - 1].gap;
            e[n - 1] = e[n]; e[n - 1].va = va; e[n - 1].gap = g;
            if (is_prefix) e[n - 1].m[0] = '+';
            continue;
        }
        if (is_prefix) e[n].m[0] = '+';                      // (marks a pending prefix)
        n++;
    }
    long total = 0, same = 0, undecodable = 0, data = 0, apx = 0;
    for (size_t i = 0; i + 1 < n; i++) {
        if (e[i + 1].gap || e[i].m[0] == '+') continue;
        uint64_t va = e[i].va, next = e[i + 1].va;
        if (next <= va || next - va > 15) continue;
        size_t avail; const uint8_t *p = at(va, &avail);
        if (!p) continue;
        x86_insn d; int len = x86_decode(p, avail, &d);
        total++;
        if (len == (int)(next - va)) same++;
        else if (!strcmp(e[i].m, "<unknown>")) { same++; data++; }   // (LLVM: not an instruction; data in the code section)
        else if (!len && (p[0] == 0xD5 || p[0] == 0x62)) apx++;              // (LLVM's APX/AVX-512 reading of data)
        else { if (!len) undecodable++; note(e[i].m, va, p, (int)(next - va), len); }
    }
    long real = total - apx;
    printf("%s: %ld instructions, %ld as LLVM (%.4f%%; %ld data bytes LLVM refuses), %ld undecodable; %ld APX/AVX-512 readings of data left out\n",
           argv[1], real, same, real ? 100.0 * same / real : 0, data, undecodable, apx);
    for (int i = 0; i < nmiss; i++) printf("  %-16s %8ld  e.g. %s\n", misses[i].name, misses[i].n, misses[i].example);
    return same == real ? 0 : 1;
}
