// Guest-shaped code for tests/t_x18_shapes.c. Uses x18 the way GCC-built Linux
// code does; the test copies each function into a MAP_JIT page, veneers it with
// kl_x18_emit, and runs it under preemption pressure.
    .text
    .p2align 2
// uint64_t f_adr(void): returns the address of its own label L1 via adr x18.
    .globl _f_adr
_f_adr:
    adr  x18, 1f
    mov  x9, #20000
2:  subs x9, x9, #1
    b.ne 2b
    mov  x0, x18
    ret
1:  nop
    .globl _f_adr_end
_f_adr_end:

// uint64_t f_blr(uint64_t (*fn)(uint64_t), uint64_t arg): calls fn through x18.
    .globl _f_blr
_f_blr:
    stp  x29, x30, [sp, #-16]!
    mov  x18, x0
    mov  x0, x1
    mov  x9, #20000
2:  subs x9, x9, #1
    b.ne 2b
    blr  x18
    add  x0, x0, #1
    ldp  x29, x30, [sp], #16
    ret
    .globl _f_blr_end
_f_blr_end:

// void f_casp(uint64_t mem[2] (16B aligned), uint64_t out[4],
//             uint64_t exp_lo, uint64_t exp_hi, uint64_t new_lo, uint64_t new_hi)
// Rs pair = x18:x19 (expected -> receives old), Rt pair = x4:x5 (new).
// Then a second casp with Rt pair = x18:x19 (new) and Rs = x6:x7.
    .globl _f_casp
_f_casp:
    stp  x19, x20, [sp, #-16]!
    mov  x18, x2
    mov  x19, x3
    mov  x9, #20000
2:  subs x9, x9, #1
    b.ne 2b
    caspal x18, x19, x4, x5, [x0]
    str  x18, [x1]
    str  x19, [x1, #8]
    // second: swap memory (now new_lo:new_hi) back to exp via Rt = x18:x19
    mov  x6, x4
    mov  x7, x5
    mov  x18, x2
    mov  x19, x3
    caspal x6, x7, x18, x19, [x0]
    str  x6, [x1, #16]
    str  x7, [x1, #24]
    ldp  x19, x20, [sp], #16
    ret
    .globl _f_casp_end
_f_casp_end:

// uint64_t f_spwb(uint64_t v): saves x18 with stp pre-index, clobbers it,
// restores with ldp post-index, returns x18 (must be v).
    .globl _f_spwb
_f_spwb:
    mov  x18, x0
    stp  x17, x18, [sp, #-16]!
    mov  x18, #7
    mov  x9, #20000
2:  subs x9, x9, #1
    b.ne 2b
    ldp  x17, x18, [sp], #16
    mov  x0, x18
    ret
    .globl _f_spwb_end
_f_spwb_end:
