// Variadic entry points of glibc's _FORTIFY_SOURCE family. Same shape as
// Klepton's kl_va_thunks.S (MIT): a guest call arrives with the first 8 integer
// arguments in x0-x7 and the first 8 FP arguments in v0-v7 (Linux AAPCS64 puts
// variadic arguments in registers; Darwin puts them on the stack), so the
// register file is spilled, an AAPCS64 va_list is built over it and a C
// handler re-marshals into Darwin's layout.
//
//   sp+  0 : q0-q7 save area (128B)      vr_top = sp+128
//   sp+128 : x0-x7 save area ( 64B)      gr_top = sp+192
//   sp+192 : kl_va                (32B)
//
// ngp = number of named integer parameters (the kl_va* becomes parameter ngp).
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

    add     x9,  sp, #192               // &va
    add     x10, x29, #16               // incoming stack args
    str     x10, [x9, #0]               // va.stack
    add     x10, sp, #192
    str     x10, [x9, #8]               // va.gr_top
    add     x10, sp, #128
    str     x10, [x9, #16]              // va.vr_top
    mov     w11, #(-64 + (\ngp * 8))
    str     w11, [x9, #24]              // va.gr_offs
    mov     w11, #-128
    str     w11, [x9, #28]              // va.vr_offs

    mov     x\ngp, x9
    bl      _\handler

    add     sp, sp, #224
    ldp     x29, x30, [sp], #16
    ret
.endm

//         guest symbol                 C handler                   named GP args
KLG_VTHUNK klg___printf_chk,            klgh_printf_chk,            2
KLG_VTHUNK klg___fprintf_chk,           klgh_fprintf_chk,           3
KLG_VTHUNK klg___sprintf_chk,           klgh_sprintf_chk,           4
KLG_VTHUNK klg___snprintf_chk,          klgh_snprintf_chk,          5
KLG_VTHUNK klg___asprintf_chk,          klgh_asprintf_chk,          3
KLG_VTHUNK klg___dprintf_chk,           klgh_dprintf_chk,           3
KLG_VTHUNK klg___syslog_chk,            klgh_syslog_chk,            3
KLG_VTHUNK klg_error,                   klgh_error,                 3
KLG_VTHUNK klg_err,                     klgh_err,                   2
KLG_VTHUNK klg_errx,                    klgh_errx,                  2
KLG_VTHUNK klg_warn,                    klgh_warn,                  1
KLG_VTHUNK klg_warnx,                   klgh_warnx,                 1
KLG_VTHUNK klg_syscall,                 klgh_syscall,               1
// tier0 console output (kl_enginelog.c)
KLG_VTHUNK klg_LoggingSystem_Log,       klgh_LoggingSystem_Log,     3
KLG_VTHUNK klg_LoggingSystem_LogColor,  klgh_LoggingSystem_LogColor, 4
KLG_VTHUNK klg_Msg,                     klgh_Msg,                   1
KLG_VTHUNK klg_Warning,                 klgh_Warning,               1
