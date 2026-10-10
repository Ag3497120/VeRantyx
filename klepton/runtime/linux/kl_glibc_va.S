// glibc-only variadic entry points, same shape as runtime/libc/kl_va_thunks.S:
// spill x0-x7/q0-q7, build an AAPCS64 va_list over them, and call a C handler
// with &va as the argument after the named ones.
.text
.macro KLG_VTHUNK name, handler, ngp
    .globl _\name
    .p2align 2
_\name:
    stp     x29, x30, [sp, #-16]!
    mov     x29, sp
    sub     sp, sp, #224
    stp     q0, q1, [sp, #0]
    stp     q2, q3, [sp, #32]
    stp     q4, q5, [sp, #64]
    stp     q6, q7, [sp, #96]
    stp     x0, x1, [sp, #128]
    stp     x2, x3, [sp, #144]
    stp     x4, x5, [sp, #160]
    stp     x6, x7, [sp, #176]
    add     x9,  sp, #192
    add     x10, x29, #16
    str     x10, [x9, #0]
    add     x10, sp, #192
    str     x10, [x9, #8]
    add     x10, sp, #128
    str     x10, [x9, #16]
    mov     w11, #(-64 + (\ngp * 8))
    str     w11, [x9, #24]
    mov     w11, #-128
    str     w11, [x9, #28]
    mov     x\ngp, x9
    bl      _\handler
    add     sp, sp, #224
    ldp     x29, x30, [sp], #16
    ret
.endm

//         guest-facing entry        C handler               named GP args
KLG_VTHUNK klg_printf_chk,           klgh_printf_chk,        2
KLG_VTHUNK klg_fprintf_chk,          klgh_fprintf_chk,       3
KLG_VTHUNK klg_swscanf,              klgh_swscanf,           2
KLG_VTHUNK klg_syslog_chk,           klgh_syslog_chk,        3
KLG_VTHUNK klg_asprintf_chk,         klgh_asprintf_chk,      3
KLG_VTHUNK klg_err,                  klgh_err,               2
KLG_VTHUNK klg_errx,                 klgh_errx,              2
KLG_VTHUNK klg_warn,                 klgh_warn,              1
KLG_VTHUNK klg_devwarning,           klgh_devwarning,        1
KLG_VTHUNK klg_lua_pushfstring,      klgh_lua_pushfstring,   2
KLG_VTHUNK klg_luaL_error,           klgh_luaL_error,        2
