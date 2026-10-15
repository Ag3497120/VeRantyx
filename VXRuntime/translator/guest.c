// Code that runs inside the vCPU guest (vcpu.c copies the __vx_guest section into the guest's code). Built on its
// own (Makefile: freestanding, no builtins, no stack protector): it may call nothing and read no globals, as the
// host's own code and data are never put in the guest.
//
// xv_guest_defer: a call of a native function that returns nothing (Direct3D 11's immediate context: setters,
// draws) is recorded instead of leaving the guest: its arguments and the arrays they point to (copied now, as the
// program may reuse them at once) go into the thread's batch; the host runs the batch, in order, at the guest's next
// exit (any call it must answer comes after them). 1: the batch is full or the call does not fit: call it now.
#include "vcpu.h"

__attribute__((section("__TEXT,__vx_guest"), used))
int xv_guest_defer(x86_ctx *c, const xv_defer *d) {
    uint64_t args[XV_DEFER_ARGS];
    const uint64_t *stack = (const uint64_t *)(uintptr_t)c->gpr[4];
    uint32_t n = d->nargs;
    for (uint32_t i = 0; i < n; i++) args[i] = i == 0 ? c->gpr[1] : i == 1 ? c->gpr[2] : i == 2 ? c->gpr[8] : i == 3 ? c->gpr[9] : stack[5 + (i - 4)];
    uint64_t size = 8 * (3 + (uint64_t)n), bytes[4];
    for (uint32_t k = 0; k < d->ncopy; k++) {
        const xv_copy *cp = &d->copy[k];
        uint64_t count = cp->count_arg >= 0 ? (uint32_t)args[cp->count_arg] : cp->count;
        if (count > cp->max) return 1;                           // (D3D11_KEEP_..., or more than fits)
        bytes[k] = args[cp->arg] ? count * cp->elem : 0;
        size += (bytes[k] + 7) & ~7ull;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)c->batch_ptr;
    if (!p || p + size > (uint8_t *)(uintptr_t)c->batch_end) return 1;
    uint64_t off = 8 * (3 + (uint64_t)n);
    for (uint32_t k = 0; k < d->ncopy; k++) {
        if (!bytes[k]) continue;
        const uint8_t *from = (const uint8_t *)(uintptr_t)args[d->copy[k].arg];
        volatile uint8_t *to = p + off;                          // (volatile: no memcpy call made of it)
        for (uint64_t b = 0; b < bytes[k]; b++) to[b] = from[b];
        args[d->copy[k].arg] = (uint64_t)(uintptr_t)(p + off);   // (the copy, at the same address for the host)
        off += (bytes[k] + 7) & ~7ull;
    }
    uint64_t *r = (uint64_t *)p;
    r[0] = size; r[1] = d->thunk; r[2] = n;
    for (uint32_t i = 0; i < n; i++) r[3 + i] = args[i];
    c->batch_ptr = (uint64_t)(uintptr_t)(p + size);
    return 0;
}
