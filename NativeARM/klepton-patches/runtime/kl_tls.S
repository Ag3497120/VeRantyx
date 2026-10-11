// TLSDESC resolvers — see kl_tls.h.
//
// The guest calls the resolver with x0 = the address of the GOT pair
// (resolver, argument) and expects, in x0, the offset of the variable from
// the thread pointer; every other register must come back untouched (the
// condition flags need not). Darwin's TSD base is the thread pointer.
.text

// Fixed, TP-relative placement: the argument IS the offset.
.globl _kl_tlsdesc_static
.p2align 2
_kl_tlsdesc_static:
    ldr     x0, [x0, #8]
    ret

// Dynamic placement: block + argument - TP, where the block pointer sits in
// Darwin TSD slot KL_TLS_BLOCK_SLOT (501); a thread without a block takes the
// slow path once.
.globl _kl_tlsdesc_dynamic
.p2align 2
_kl_tlsdesc_dynamic:
    stp     x1, x2, [sp, #-16]!
    mrs     x1, tpidrro_el0
    ldr     x2, [x1, #(501 * 8)]
    cbz     x2, Lslow
    ldr     x0, [x0, #8]
    add     x0, x0, x2
    sub     x0, x0, x1
    ldp     x1, x2, [sp], #16
    ret
Lslow:
    ldp     x1, x2, [sp], #16
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #672                // 18 GPRs (144, padded to 160) + 32 Q regs (512)
    stp     x1, x2, [sp, #0]
    stp     x3, x4, [sp, #16]
    stp     x5, x6, [sp, #32]
    stp     x7, x8, [sp, #48]
    stp     x9, x10, [sp, #64]
    stp     x11, x12, [sp, #80]
    stp     x13, x14, [sp, #96]
    stp     x15, x16, [sp, #112]
    stp     x17, x18, [sp, #128]
    add     x1, sp, #160
    stp     q0, q1, [x1, #0]
    stp     q2, q3, [x1, #32]
    stp     q4, q5, [x1, #64]
    stp     q6, q7, [x1, #96]
    stp     q8, q9, [x1, #128]
    stp     q10, q11, [x1, #160]
    stp     q12, q13, [x1, #192]
    stp     q14, q15, [x1, #224]
    stp     q16, q17, [x1, #256]
    stp     q18, q19, [x1, #288]
    stp     q20, q21, [x1, #320]
    stp     q22, q23, [x1, #352]
    stp     q24, q25, [x1, #384]
    stp     q26, q27, [x1, #416]
    stp     q28, q29, [x1, #448]
    stp     q30, q31, [x1, #480]
    bl      _kl_tls_slow
    add     x1, sp, #160
    ldp     q0, q1, [x1, #0]
    ldp     q2, q3, [x1, #32]
    ldp     q4, q5, [x1, #64]
    ldp     q6, q7, [x1, #96]
    ldp     q8, q9, [x1, #128]
    ldp     q10, q11, [x1, #160]
    ldp     q12, q13, [x1, #192]
    ldp     q14, q15, [x1, #224]
    ldp     q16, q17, [x1, #256]
    ldp     q18, q19, [x1, #288]
    ldp     q20, q21, [x1, #320]
    ldp     q22, q23, [x1, #352]
    ldp     q24, q25, [x1, #384]
    ldp     q26, q27, [x1, #416]
    ldp     q28, q29, [x1, #448]
    ldp     q30, q31, [x1, #480]
    ldp     x1, x2, [sp, #0]
    ldp     x3, x4, [sp, #16]
    ldp     x5, x6, [sp, #32]
    ldp     x7, x8, [sp, #48]
    ldp     x9, x10, [sp, #64]
    ldp     x11, x12, [sp, #80]
    ldp     x13, x14, [sp, #96]
    ldp     x15, x16, [sp, #112]
    ldp     x17, x18, [sp, #128]
    add     sp, sp, #672
    ldp     x29, x30, [sp], #16
    ret
