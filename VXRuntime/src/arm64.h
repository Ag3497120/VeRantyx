// The ARM64 build's side of the boundary with translated x86 code (src/arm64.c): the program's registers as the
// engine keeps them (x86_ctx), and what the generated thunks (tools/vx_thunks.py) use to read the arguments of a
// Windows x64 call and to return its result.
#ifndef VX_ARM64_H
#define VX_ARM64_H
#include <stdint.h>
#include <string.h>
#include "../translator/xlat.h"

typedef struct { const void *fn; void (*thunk)(x86_ctx *); } vx_thunk;
void vx_thunks_add(const vx_thunk *t, size_t n);
void vx_thunks_override(const vx_thunk *t, size_t n);       // thunks of their own for functions that work on the x86 state
void (*vx_thunk_find(uint64_t fn))(x86_ctx *);       // NULL: not a function translated code may call
// vCPU: calls of these native functions (no result) may be recorded in the guest and run at its next exit; for
// each, its argument count and the arrays its arguments point to (translator/vcpu.h's xv_copy).
#include "../translator/vcpu.h"
typedef struct { const void *fn; uint32_t nargs, ncopy; xv_copy copy[4]; } vx_defer;
void vx_defer_add(const vx_defer *d, size_t n);
#define VX_COPY(arg, count_arg, elem, max) { arg, count_arg, elem, 0, max }
#define VX_COPY_N(arg, n, elem) { arg, -1, elem, n, n }

// Argument i of the call the program just made (RSP at its return address): RCX RDX R8 R9, then the stack above the
// 32-byte home area. Floating-point arguments 0-3 are in XMM0-3 instead.
static inline uint64_t vx_arg(const x86_ctx *c, int i) {
    static const int reg[4] = { 1, 2, 8, 9 };
    return i < 4 ? c->gpr[reg[i]] : ((const uint64_t *)(uintptr_t)c->gpr[4])[5 + (i - 4)];
}
static inline float vx_arg_f(const x86_ctx *c, int i) {
    float f; uint64_t s = vx_arg(c, i);
    memcpy(&f, i < 4 ? (const void *)c->xmm[i] : (const void *)&s, 4);
    return f;
}
static inline double vx_arg_d(const x86_ctx *c, int i) {
    double d; uint64_t s = vx_arg(c, i);
    memcpy(&d, i < 4 ? (const void *)c->xmm[i] : (const void *)&s, 8);
    return d;
}
static inline void vx_ret_f(x86_ctx *c, float f) { memset(c->xmm[0], 0, 16); memcpy(c->xmm[0], &f, 4); }
static inline void vx_ret_d(x86_ctx *c, double d) { memset(c->xmm[0], 0, 16); memcpy(c->xmm[0], &d, 8); }

// A variadic function's arguments after its `fixed` ones: the four register arguments go to their home slots (x64
// callers pass floating-point varargs in both kinds of register), so all of them are 8-byte slots in a row.
extern __thread const char *vx_va_slots;
static inline void vx_va_begin(x86_ctx *c, int fixed) {
    uint64_t *s = (uint64_t *)(uintptr_t)c->gpr[4];
    s[1] = c->gpr[1]; s[2] = c->gpr[2]; s[3] = c->gpr[8]; s[4] = c->gpr[9];
    vx_va_slots = (const char *)(s + 1 + fixed);
}

// The calling thread's x86 registers (made, with its x86 stack, on first use).
x86_ctx *vx_x86_ctx(void);
// Calls x86 code at `fn` with up to 12 integer arguments, the Windows x64 way; returns RAX.
uint64_t vx_x86_call(uint64_t fn, int n, const uint64_t *args);
#endif
