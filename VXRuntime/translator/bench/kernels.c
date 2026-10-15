// Benchmark kernels for the translator (phase 4a): plain C with no library calls, built once for x86-64 and run on
// the engine (bench.c) and under Rosetta 2 (rosetta_main.c), and for ARM64 as the native reference. Each takes an
// iteration count and returns a checksum (the three must agree).
#include <stdint.h>

static uint8_t buf[65536];
static float ma[64 * 64], mb[64 * 64], mc[64 * 64];
static int32_t arr[20000];
typedef struct node { struct node *l, *r; uint32_t key; } node;
static node pool[16384];
typedef struct { float x, y, z, vx, vy, vz; } particle;
static particle ps[4096];
static uint32_t seed = 12345;
static uint32_t lcg(void) { seed = seed * 1664525u + 1013904223u; return seed; }

uint64_t k_crc(uint64_t n) {                                     // bitwise CRC-32: integer ALU, a tight loop
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (uint8_t)lcg();
    uint32_t c = ~0u;
    for (uint64_t it = 0; it < n; it++)
        for (int i = 0; i < 4096; i++) { c ^= buf[(i + it) & 0xFFFF]; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & -(c & 1)); }
    return ~c;
}

uint64_t k_matmul(uint64_t n) {                                  // floats (SSE), vectorized by the compiler
    for (int i = 0; i < 64 * 64; i++) { ma[i] = (float)(lcg() % 100) / 50.0f; mb[i] = (float)(lcg() % 100) / 50.0f; }
    double s = 0;
    for (uint64_t it = 0; it < n; it++) {
        for (int i = 0; i < 64; i++)
            for (int j = 0; j < 64; j++) { float acc = 0; for (int k = 0; k < 64; k++) acc += ma[i * 64 + k] * mb[k * 64 + j]; mc[i * 64 + j] = acc; }
        s += mc[(it * 7) & 4095];
        ma[it & 4095] += 0.25f;
    }
    return (uint64_t)(s * 1000);
}

static void qs(int32_t *a, int lo, int hi) {                     // recursion, branches, memory
    while (lo < hi) {
        int32_t p = a[(lo + hi) / 2]; int i = lo, j = hi;
        while (i <= j) { while (a[i] < p) i++; while (a[j] > p) j--; if (i <= j) { int32_t t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; } }
        if (j - lo < hi - i) { qs(a, lo, j); lo = i; } else { qs(a, i, hi); hi = j; }
    }
}
uint64_t k_sort(uint64_t n) {
    uint64_t s = 0;
    for (uint64_t it = 0; it < n; it++) {
        for (int i = 0; i < 20000; i++) arr[i] = (int32_t)(lcg() >> 1);
        qs(arr, 0, 19999);
        s += (uint64_t)arr[it % 20000] + (uint64_t)arr[10000];
    }
    return s;
}

uint64_t k_tree(uint64_t n) {                                    // pointer chasing: a binary search tree
    uint64_t found = 0;
    for (uint64_t it = 0; it < n; it++) {
        node *root = 0; int used = 0;
        for (int i = 0; i < 16384; i++) {
            uint32_t k = lcg(); node **p = &root;
            while (*p) p = k < (*p)->key ? &(*p)->l : &(*p)->r;
            node *x = &pool[used++]; x->key = k; x->l = x->r = 0; *p = x;
        }
        for (int i = 0; i < 16384; i++) {
            uint32_t k = pool[(i * 7919) & 16383].key; node *x = root;
            while (x && x->key != k) x = k < x->key ? x->l : x->r;
            found += x != 0;
        }
    }
    return found;
}

static uint64_t fib(uint64_t x) { return x < 2 ? x : fib(x - 1) + fib(x - 2); }
uint64_t k_fib(uint64_t n) { uint64_t s = 0; for (uint64_t it = 0; it < n; it++) s += fib(24); return s; }   // calls and returns

uint64_t k_physics(uint64_t n) {                                 // vector math with branches, as a game's update
    for (int i = 0; i < 4096; i++) { particle *p = &ps[i]; p->x = (float)(lcg() % 1000); p->y = (float)(lcg() % 1000); p->z = 0;
        p->vx = (float)(lcg() % 200) / 10 - 10; p->vy = (float)(lcg() % 200) / 10 - 10; p->vz = 0; }
    float e = 0;
    for (uint64_t it = 0; it < n; it++)
        for (int i = 0; i < 4096; i++) {
            particle *p = &ps[i];
            p->vy -= 0.98f * 0.016f;
            p->x += p->vx * 0.016f; p->y += p->vy * 0.016f; p->z += p->vz * 0.016f;
            if (p->x < 0 || p->x > 1000) p->vx = -p->vx * 0.9f;
            if (p->y < 0) { p->y = 0; p->vy = -p->vy * 0.8f; }
            e += p->vx * p->vx + p->vy * p->vy;
        }
    return (uint64_t)e;
}

uint64_t k_interp(uint64_t n) {                                  // a bytecode interpreter: a switch (indirect jumps)
    uint8_t prog[256];
    for (int i = 0; i < 256; i++) prog[i] = (uint8_t)(lcg() % 8);
    int64_t acc = 0, r1 = 1, r2 = 3;
    for (uint64_t it = 0; it < n; it++)
        for (int pc = 0; pc < 256; pc++) {
            switch (prog[pc]) {
            case 0: acc += r1; break;
            case 1: acc -= r2; break;
            case 2: r1 = acc ^ r2; break;
            case 3: r2 += 7; break;
            case 4: acc *= 3; break;
            case 5: acc >>= 1; break;
            case 6: r1 = r2 - acc; break;
            default: acc += pc; break;
            }
        }
    return (uint64_t)(acc + r1 + r2);
}

uint64_t k_string(uint64_t n) {                                  // byte loops: length, compare, search
    for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (uint8_t)(lcg() % 26 + 'a');
    for (int i = 255; i < (int)sizeof buf; i += 256) buf[i] = 0;
    uint64_t s = 0;
    for (uint64_t it = 0; it < n; it++)
        for (int b = 0; b < 255; b++) {
            const uint8_t *p = buf + 256 * b, *q = buf + 256 * ((b + 1) & 255);
            int len = 0; while (p[len]) len++;
            int k = 0; while (k < len && p[k] == q[k]) k++;
            const uint8_t *z = p; while (*z && *z != 'q') z++;
            s += (uint64_t)len + (uint64_t)k + (uint64_t)(z - p);
        }
    return s;
}

void start(void) {}                                              // (entry for the static image; never run)
