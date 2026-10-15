// The kernels as an ordinary program: under Rosetta 2 (x86-64) or natively (ARM64). Prints ns per call.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
uint64_t k_crc(uint64_t), k_matmul(uint64_t), k_sort(uint64_t), k_tree(uint64_t), k_fib(uint64_t), k_physics(uint64_t), k_interp(uint64_t), k_string(uint64_t);
static const struct { const char *name; uint64_t (*f)(uint64_t); uint64_t n; } K[] = {
    { "crc", k_crc, 40 }, { "matmul", k_matmul, 40 }, { "sort", k_sort, 20 }, { "tree", k_tree, 10 },
    { "fib", k_fib, 40 }, { "physics", k_physics, 400 }, { "interp", k_interp, 40000 }, { "string", k_string, 400 },
};
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv) {
    int reps = argc > 1 ? atoi(argv[1]) : 3;
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) {
        uint64_t r = K[i].f(1); double best = 1e9;
        for (int k = 0; k < reps; k++) { double t0 = now(); r = K[i].f(K[i].n); double t = now() - t0; if (t < best) best = t; }
        printf("%-8s %10.3f ms  %llu\n", K[i].name, best * 1e3, (unsigned long long)r);
    }
}
