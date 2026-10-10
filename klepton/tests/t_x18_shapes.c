// EXECUTES the adr/blr/casp veneer shapes added for the Alyx corpus, under
// preemption pressure (the kernel zeroes x18 on every exception return, so an
// unveneered x18 read after the spin loop reads 0).
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "kl_x18.h"

extern char f_adr[], f_adr_end[], f_blr[], f_blr_end[], f_casp[], f_casp_end[], f_spwb[], f_spwb_end[];

static void *spin(void *a) { volatile uint64_t x = 0; for (;;) x++; return a; }
static uint64_t callee(uint64_t v) { return v * 2; }

// Copy [s,e) into a fresh JIT page, veneer it, return its executable address.
static void *install(char *s, char *e, kl_x18_stats *st) {
    size_t n = (size_t)(e - s), cap = 1 << 16;
    uint8_t *m = mmap(NULL, cap * 2, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return NULL; }
    pthread_jit_write_protect_np(0);
    memcpy(m, s, n);
    size_t used = 0;
    kl_x18_emit(m, n, (uint64_t)m, m + cap, cap, (uint64_t)(m + cap), st, &used);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(m, cap * 2);
    return m;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (kl_x18_init()) { fprintf(stderr, "kl_x18_init failed\n"); return 1; }
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    pthread_t t;
    for (long i = 0; i < ncpu * 2; i++) pthread_create(&t, 0, spin, 0);

    int fail = 0;
    kl_x18_stats st;
    uint64_t (*adr)(void) = install(f_adr, f_adr_end, &st);
    printf("adr : sites %u patched %u refused %u\n", st.sites, st.patched, st.refused);
    uint64_t want_adr = (uint64_t)adr + (uint64_t)(f_adr_end - f_adr) - 4;
    uint64_t (*blr)(uint64_t (*)(uint64_t), uint64_t) = install(f_blr, f_blr_end, &st);
    printf("blr : sites %u patched %u refused %u\n", st.sites, st.patched, st.refused);
    void (*casp)(uint64_t *, uint64_t *, uint64_t, uint64_t, uint64_t, uint64_t) =
        install(f_casp, f_casp_end, &st);
    printf("casp: sites %u patched %u refused %u\n", st.sites, st.patched, st.refused);

    uint64_t (*spwb)(uint64_t) = install(f_spwb, f_spwb_end, &st);
    printf("spwb: sites %u patched %u refused %u\n", st.sites, st.patched, st.refused);
    for (int i = 0; i < 2000 && !fail; i++) {
        uint64_t v = spwb(0x5000 + (uint64_t)i);
        if (v != 0x5000 + (uint64_t)i) { printf("spwb FAIL iter %d: %#llx\n", i, v); fail = 1; }
        uint64_t a = adr();
        if (a != want_adr) { printf("adr FAIL iter %d: %#llx want %#llx\n", i, a, want_adr); fail = 1; }
        uint64_t b = blr(callee, (uint64_t)i);
        if (b != (uint64_t)i * 2 + 1) { printf("blr FAIL iter %d: %llu\n", i, b); fail = 1; }
        _Alignas(16) uint64_t mem[2] = {0x1111, 0x2222};
        uint64_t out[4] = {0};
        casp(mem, out, 0x1111, 0x2222, 0xaaaa + i, 0xbbbb);
        // first casp: expected matched -> Rs gets old (0x1111,0x2222), mem = new
        // second casp: Rs=(new) matches -> Rs gets old new, mem = Rt = (exp)
        if (out[0] != 0x1111 || out[1] != 0x2222 || out[2] != 0xaaaa + (uint64_t)i ||
            out[3] != 0xbbbb || mem[0] != 0x1111 || mem[1] != 0x2222) {
            printf("casp FAIL iter %d: out %llx %llx %llx %llx mem %llx %llx\n", i,
                   out[0], out[1], out[2], out[3], mem[0], mem[1]);
            fail = 1;
        }
    }
    printf(fail ? "FAIL\n" : "PASS: adr/blr/casp/sp-writeback veneers correct over 2000 preempted iterations\n");
    _exit(fail);
}
