// The TLSDESC resolver (see kl_elftls.h). In: x0 = &desc[0]. Out: x0 = address
// of the variable minus TPIDRRO_EL0. Preserves every other register (the ABI's
// promise), including all 32 full vector registers on the slow path.
    .text
    .p2align 2
    .globl _kl_tlsdesc_resolver
_kl_tlsdesc_resolver:
    ldr  x0, [x0, #8]                   // kl_tlsdesc_arg *
    stp  x1, x2, [sp, #-32]!
    str  x3, [sp, #16]
    mrs  x1, tpidrro_el0
    adrp x2, _g_kl_tls_key@PAGE
    ldr  x2, [x2, _g_kl_tls_key@PAGEOFF]
    ldr  x2, [x1, x2, lsl #3]           // this thread's DTV
    cbz  x2, 1f
    ldr  x3, [x0]                       // module
    ldr  x2, [x2, x3, lsl #3]           // block
    cbz  x2, 1f
    ldr  x3, [x0, #8]                   // offset
    add  x0, x2, x3
    sub  x0, x0, x1
    ldr  x3, [sp, #16]
    ldp  x1, x2, [sp], #32
    ret
1:  ldr  x3, [sp, #16]
    ldp  x1, x2, [sp], #32
    stp  x29, x30, [sp, #-16]!
    mov  x29, sp
    sub  sp, sp, #656                   // x1..x17 (+pad) and q0..q31
    stp  x1, x2,  [sp, #0]
    stp  x3, x4,  [sp, #16]
    stp  x5, x6,  [sp, #32]
    stp  x7, x8,  [sp, #48]
    stp  x9, x10, [sp, #64]
    stp  x11, x12, [sp, #80]
    stp  x13, x14, [sp, #96]
    stp  x15, x16, [sp, #112]
    str  x17, [sp, #128]
    stp  q0, q1,   [sp, #144]
    stp  q2, q3,   [sp, #176]
    stp  q4, q5,   [sp, #208]
    stp  q6, q7,   [sp, #240]
    stp  q8, q9,   [sp, #272]
    stp  q10, q11, [sp, #304]
    stp  q12, q13, [sp, #336]
    stp  q14, q15, [sp, #368]
    stp  q16, q17, [sp, #400]
    stp  q18, q19, [sp, #432]
    stp  q20, q21, [sp, #464]
    stp  q22, q23, [sp, #496]
    stp  q24, q25, [sp, #528]
    stp  q26, q27, [sp, #560]
    stp  q28, q29, [sp, #592]
    stp  q30, q31, [sp, #624]
    bl   _kl_tls_desc_slow
    mrs  x1, tpidrro_el0
    sub  x0, x0, x1
    ldp  x1, x2,  [sp, #0]
    ldp  x3, x4,  [sp, #16]
    ldp  x5, x6,  [sp, #32]
    ldp  x7, x8,  [sp, #48]
    ldp  x9, x10, [sp, #64]
    ldp  x11, x12, [sp, #80]
    ldp  x13, x14, [sp, #96]
    ldp  x15, x16, [sp, #112]
    ldr  x17, [sp, #128]
    ldp  q0, q1,   [sp, #144]
    ldp  q2, q3,   [sp, #176]
    ldp  q4, q5,   [sp, #208]
    ldp  q6, q7,   [sp, #240]
    ldp  q8, q9,   [sp, #272]
    ldp  q10, q11, [sp, #304]
    ldp  q12, q13, [sp, #336]
    ldp  q14, q15, [sp, #368]
    ldp  q16, q17, [sp, #400]
    ldp  q18, q19, [sp, #432]
    ldp  q20, q21, [sp, #464]
    ldp  q22, q23, [sp, #496]
    ldp  q24, q25, [sp, #528]
    ldp  q26, q27, [sp, #560]
    ldp  q28, q29, [sp, #592]
    ldp  q30, q31, [sp, #624]
    mov  sp, x29
    ldp  x29, x30, [sp], #16
    ret
