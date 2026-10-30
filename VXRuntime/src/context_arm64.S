// The ARM64 build's assembly (src/arm64.c). vx_cb_tramp: where a call from this runtime into x86 code goes (the
// fault handler sends it here with the x86 address in x9): x0-x7 and the caller's stack arguments to
// vx_x86_invoke, which runs the code on the engine; its RAX comes back in x0.
#ifdef __aarch64__
    .text
    .globl _vx_cb_tramp
    .p2align 2
_vx_cb_tramp:
    stp x29, x30, [sp, #-80]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    mov x0, x9
    add x1, sp, #16
    add x2, sp, #80
    bl _vx_x86_invoke
    ldp x29, x30, [sp], #80
    ret

// vx_cb_tramp_tls: the same, for the visionOS build's calls through VX_X86FN (src/vx.h): the x86 address was pushed
// on this thread's list by vx_x86_fn, which handed out this address in its place (no fault to send it here there).
    .globl _vx_cb_tramp_tls
    .p2align 2
_vx_cb_tramp_tls:
    stp x29, x30, [sp, #-80]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    bl _vx_x86_fn_take
    mov x9, x0
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldp x29, x30, [sp], #80
    b _vx_cb_tramp
#endif
