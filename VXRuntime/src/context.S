// Register-level pieces of exception handling, x86-64 (AT&T syntax, Mach-O).
//
// CONTEXT offsets (Windows x64): ContextFlags 0x30, MxCsr 0x34, EFlags 0x44, Rax 0x78, Rcx 0x80, Rdx 0x88,
// Rbx 0x90, Rsp 0x98, Rbp 0xa0, Rsi 0xa8, Rdi 0xb0, R8..R15 0xb8..0xf0, Rip 0xf8, FltSave 0x100
// (its MxCsr at 0x118), Xmm0..Xmm15 0x1a0..0x290. sizeof(CONTEXT) = 0x4d0.

#define CTX_SIZE 0x4d0

// Fills the CONTEXT at %r11 with the registers as they are now, except Rsp/Rip, which describe the state the
// caller will see once this function returns (Rsp = caller's rsp after the return, Rip = return address).
// Expects: %r11 = context, 8(%rsp)+ the extra stack this macro's user pushed = see each use.
.macro SAVE_GPRS ctx
    movq %rax, 0x78(\ctx)
    movq %rcx, 0x80(\ctx)
    movq %rdx, 0x88(\ctx)
    movq %rbx, 0x90(\ctx)
    movq %rbp, 0xa0(\ctx)
    movq %rsi, 0xa8(\ctx)
    movq %rdi, 0xb0(\ctx)
    movq %r8,  0xb8(\ctx)
    movq %r9,  0xc0(\ctx)
    movq %r10, 0xc8(\ctx)
    movq %r12, 0xd8(\ctx)
    movq %r13, 0xe0(\ctx)
    movq %r14, 0xe8(\ctx)
    movq %r15, 0xf0(\ctx)
.endm

.macro SAVE_FP ctx
    stmxcsr 0x34(\ctx)
    stmxcsr 0x118(\ctx)
    movdqu %xmm0,  0x1a0(\ctx)
    movdqu %xmm1,  0x1b0(\ctx)
    movdqu %xmm2,  0x1c0(\ctx)
    movdqu %xmm3,  0x1d0(\ctx)
    movdqu %xmm4,  0x1e0(\ctx)
    movdqu %xmm5,  0x1f0(\ctx)
    movdqu %xmm6,  0x200(\ctx)
    movdqu %xmm7,  0x210(\ctx)
    movdqu %xmm8,  0x220(\ctx)
    movdqu %xmm9,  0x230(\ctx)
    movdqu %xmm10, 0x240(\ctx)
    movdqu %xmm11, 0x250(\ctx)
    movdqu %xmm12, 0x260(\ctx)
    movdqu %xmm13, 0x270(\ctx)
    movdqu %xmm14, 0x280(\ctx)
    movdqu %xmm15, 0x290(\ctx)
    movl $0x10001f, 0x30(\ctx)          // CONTEXT_AMD64 | CONTROL | INTEGER | SEGMENTS | FLOATING_POINT
.endm

    .text

// void RtlCaptureContext(CONTEXT *ctx)   -- Windows calling convention, rcx = ctx
    .globl _vx_RtlCaptureContext
    .p2align 4
_vx_RtlCaptureContext:
    pushfq
    popq 0x44(%rcx)                       // EFlags (writes 8 bytes; the next field is Dr0, unused)
    SAVE_GPRS %rcx
    movq %r11, 0xd0(%rcx)
    leaq 8(%rsp), %rax                    // the caller's rsp after this call returns
    movq %rax, 0x98(%rcx)
    movq (%rsp), %rax                     // return address
    movq %rax, 0xf8(%rcx)
    movq 0x78(%rcx), %rax                 // restore rax
    SAVE_FP %rcx
    ret

// The two Windows entry points below capture the caller's state into a CONTEXT on their own stack, then call
// into C (System V convention). The CONTEXT sits at 0x10(%rsp), 16-byte aligned: on entry rsp = 8 mod 16, and
// 0x4e8 more keeps the call below aligned.
#define FRAME 0x4e8

// void RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args)   (rcx, edx, r8d, r9)
//   -> vx_raise(code, flags, n, args, ctx)   (never returns: the context is restored or the process ends)
    .globl _vx_RaiseException
    .p2align 4
_vx_RaiseException:
    subq $FRAME, %rsp
    leaq 0x10(%rsp), %r11
    SAVE_GPRS %r11
    pushfq
    popq %rax
    movq %rax, 0x44(%r11)
    leaq FRAME+8(%rsp), %rax
    movq %rax, 0x98(%r11)
    movq FRAME(%rsp), %rax
    movq %rax, 0xf8(%r11)
    SAVE_FP %r11
    movl %ecx, %edi
    movl %edx, %esi
    movl %r8d, %edx
    movq %r9, %rcx
    movq %r11, %r8
    call _vx_raise
    ud2

// void _CxxThrowException(void *object, const ThrowInfo *info)   (rcx, rdx)
//   -> vx_cxx_throw(object, info, ctx)   (never returns)
    .globl _vx_CxxThrowException
    .p2align 4
_vx_CxxThrowException:
    subq $FRAME, %rsp
    leaq 0x10(%rsp), %r11
    SAVE_GPRS %r11
    pushfq
    popq %rax
    movq %rax, 0x44(%r11)
    leaq FRAME+8(%rsp), %rax
    movq %rax, 0x98(%r11)
    movq FRAME(%rsp), %rax
    movq %rax, 0xf8(%r11)
    SAVE_FP %r11
    movq %rcx, %rdi
    movq %rdx, %rsi
    movq %r11, %rdx
    call _vx_cxx_throw
    ud2

// void RtlUnwindEx(void *frame, void *target_ip, EXCEPTION_RECORD *rec, void *retval, CONTEXT *ctx, void *history)
//   rcx, rdx, r8, r9, [rsp+0x28], [rsp+0x30]
//   -> vx_unwind(frame, target_ip, rec, retval, ctx, captured)   (never returns)
    .globl _vx_RtlUnwindEx
    .p2align 4
_vx_RtlUnwindEx:
    subq $FRAME, %rsp
    leaq 0x10(%rsp), %r11
    SAVE_GPRS %r11
    pushfq
    popq %rax
    movq %rax, 0x44(%r11)
    leaq FRAME+8(%rsp), %rax
    movq %rax, 0x98(%r11)
    movq FRAME(%rsp), %rax
    movq %rax, 0xf8(%r11)
    SAVE_FP %r11
    movq %rcx, %rdi
    movq %rdx, %rsi
    movq %r8, %rdx
    movq %r9, %rcx
    movq FRAME+0x28(%rsp), %r8
    movq %r11, %r9
    call _vx_unwind
    ud2

// void RtlUnwind(void *frame, void *target_ip, EXCEPTION_RECORD *rec, void *retval)
    .globl _vx_RtlUnwind
    .p2align 4
_vx_RtlUnwind:
    subq $FRAME, %rsp
    leaq 0x10(%rsp), %r11
    SAVE_GPRS %r11
    pushfq
    popq %rax
    movq %rax, 0x44(%r11)
    leaq FRAME+8(%rsp), %rax
    movq %rax, 0x98(%r11)
    movq FRAME(%rsp), %rax
    movq %rax, 0xf8(%r11)
    SAVE_FP %r11
    movq %rcx, %rdi
    movq %rdx, %rsi
    movq %r8, %rdx
    movq %r9, %rcx
    xorl %r8d, %r8d
    movq %r11, %r9
    call _vx_unwind
    ud2

// void vx_restore_context(const CONTEXT *ctx)   -- System V, rdi = ctx. Resumes execution at ctx->Rip.
// rip, eflags and rdi are pushed just below the target rsp, then popped as the last step.
    .globl _vx_restore_context
    .p2align 4
_vx_restore_context:
    ldmxcsr 0x34(%rdi)
    movdqu 0x1a0(%rdi), %xmm0
    movdqu 0x1b0(%rdi), %xmm1
    movdqu 0x1c0(%rdi), %xmm2
    movdqu 0x1d0(%rdi), %xmm3
    movdqu 0x1e0(%rdi), %xmm4
    movdqu 0x1f0(%rdi), %xmm5
    movdqu 0x200(%rdi), %xmm6
    movdqu 0x210(%rdi), %xmm7
    movdqu 0x220(%rdi), %xmm8
    movdqu 0x230(%rdi), %xmm9
    movdqu 0x240(%rdi), %xmm10
    movdqu 0x250(%rdi), %xmm11
    movdqu 0x260(%rdi), %xmm12
    movdqu 0x270(%rdi), %xmm13
    movdqu 0x280(%rdi), %xmm14
    movdqu 0x290(%rdi), %xmm15
    movq 0x98(%rdi), %rax                 // target rsp
    subq $24, %rax
    movq 0xf8(%rdi), %rcx                 // rip
    movq %rcx, 16(%rax)
    movl 0x44(%rdi), %ecx                 // eflags
    movq %rcx, 8(%rax)
    movq 0xb0(%rdi), %rcx                 // rdi
    movq %rcx, (%rax)
    movq %rax, %r11                       // r11 = new stack top (r11 is restored last from the context)
    movq 0x80(%rdi), %rcx
    movq 0x88(%rdi), %rdx
    movq 0x90(%rdi), %rbx
    movq 0xa0(%rdi), %rbp
    movq 0xa8(%rdi), %rsi
    movq 0xb8(%rdi), %r8
    movq 0xc0(%rdi), %r9
    movq 0xc8(%rdi), %r10
    movq 0xd8(%rdi), %r12
    movq 0xe0(%rdi), %r13
    movq 0xe8(%rdi), %r14
    movq 0xf0(%rdi), %r15
    movq 0x78(%rdi), %rax
    movq %r11, %rsp
    movq 0xd0(%rdi), %r11
    popq %rdi
    popfq
    ret

// __intrinsic_setjmp(jmp_buf buf, void *frame) and longjmp(jmp_buf buf, int value), Windows convention.
// _JUMP_BUFFER: Frame 0, Rbx 8, Rsp 0x10, Rbp 0x18, Rsi 0x20, Rdi 0x28, R12 0x30, R13 0x38, R14 0x40, R15 0x48,
// Rip 0x50, MxCsr 0x58, FpCsr 0x5c, Spare 0x5e, Xmm6..Xmm15 0x60..0xf0. longjmp restores the registers
// directly (termination handlers between the two frames are not run yet).
    .globl _vx_setjmp
    .p2align 4
_vx_setjmp:
    movq %rdx, 0x00(%rcx)
    movq %rbx, 0x08(%rcx)
    leaq 8(%rsp), %rax
    movq %rax, 0x10(%rcx)
    movq %rbp, 0x18(%rcx)
    movq %rsi, 0x20(%rcx)
    movq %rdi, 0x28(%rcx)
    movq %r12, 0x30(%rcx)
    movq %r13, 0x38(%rcx)
    movq %r14, 0x40(%rcx)
    movq %r15, 0x48(%rcx)
    movq (%rsp), %rax
    movq %rax, 0x50(%rcx)
    stmxcsr 0x58(%rcx)
    fnstcw 0x5c(%rcx)
    movdqu %xmm6,  0x60(%rcx)
    movdqu %xmm7,  0x70(%rcx)
    movdqu %xmm8,  0x80(%rcx)
    movdqu %xmm9,  0x90(%rcx)
    movdqu %xmm10, 0xa0(%rcx)
    movdqu %xmm11, 0xb0(%rcx)
    movdqu %xmm12, 0xc0(%rcx)
    movdqu %xmm13, 0xd0(%rcx)
    movdqu %xmm14, 0xe0(%rcx)
    movdqu %xmm15, 0xf0(%rcx)
    xorl %eax, %eax
    ret

    .globl _vx_longjmp
    .p2align 4
_vx_longjmp:
    movl %edx, %eax
    testl %eax, %eax
    jnz 1f
    movl $1, %eax
1:
    movq 0x08(%rcx), %rbx
    movq 0x18(%rcx), %rbp
    movq 0x20(%rcx), %rsi
    movq 0x28(%rcx), %rdi
    movq 0x30(%rcx), %r12
    movq 0x38(%rcx), %r13
    movq 0x40(%rcx), %r14
    movq 0x48(%rcx), %r15
    ldmxcsr 0x58(%rcx)
    fldcw 0x5c(%rcx)
    movdqu 0x60(%rcx), %xmm6
    movdqu 0x70(%rcx), %xmm7
    movdqu 0x80(%rcx), %xmm8
    movdqu 0x90(%rcx), %xmm9
    movdqu 0xa0(%rcx), %xmm10
    movdqu 0xb0(%rcx), %xmm11
    movdqu 0xc0(%rcx), %xmm12
    movdqu 0xd0(%rcx), %xmm13
    movdqu 0xe0(%rcx), %xmm14
    movdqu 0xf0(%rcx), %xmm15
    movq 0x10(%rcx), %rsp
    jmpq *0x50(%rcx)

// __chkstk (see desktop.c): stacks are mapped whole, so the probe only returns; every register is kept.
    .globl _vx_chkstk
_vx_chkstk:
    ret

// vx_fiber_switch(void **save_sp, void *new_sp) (System V): keeps this side's callee-saved registers, MXCSR and
// the x87 control word on its stack, saves the stack pointer in *save_sp, and resumes the other side. The
// Windows-only callee-saved registers (rsi, rdi, xmm6-15) are kept by SwitchToFiber's own frame, which calls
// this. A new fiber's stack is laid out as if it had called this, returning into its start routine.
    .globl _vx_fiber_switch
_vx_fiber_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    stmxcsr (%rsp)
    fnstcw 4(%rsp)
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    ldmxcsr (%rsp)
    fldcw 4(%rsp)
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

// ---- VXRT_TRACE_CALLS: a trampoline in front of each traced built-in function (calltrace.c) --------------------------------------
// Stub i (16 bytes apart) puts i in r11 and jumps to vx_trace_enter, which keeps the argument registers, has
// vx_trace_called log the call (and swap the return address for vx_trace_return), then jumps to the function.
    .globl _vx_trace_stubs
    .p2align 4
_vx_trace_stubs:
    .set stub_index, 0
    .rept 8192
    .p2align 4
    .byte 0x41, 0xbb
    .long stub_index
    .byte 0xe9
    .long _vx_trace_enter - (. + 4)
    .set stub_index, stub_index + 1
    .endr

_vx_trace_enter:
    pushq %rcx
    pushq %rdx
    pushq %r8
    pushq %r9
    subq $0x68, %rsp
    movdqu %xmm0, 0x20(%rsp)
    movdqu %xmm1, 0x30(%rsp)
    movdqu %xmm2, 0x40(%rsp)
    movdqu %xmm3, 0x50(%rsp)
    movl %r11d, %ecx
    leaq 0x88(%rsp), %rdx
    callq _vx_trace_called
    movq %rax, %r11
    movdqu 0x20(%rsp), %xmm0
    movdqu 0x30(%rsp), %xmm1
    movdqu 0x40(%rsp), %xmm2
    movdqu 0x50(%rsp), %xmm3
    addq $0x68, %rsp
    popq %r9
    popq %r8
    popq %rdx
    popq %rcx
    jmpq *%r11

    .globl _vx_trace_return
_vx_trace_return:
    subq $0x40, %rsp
    movq %rax, 0x20(%rsp)
    movdqu %xmm0, 0x30(%rsp)
    movq %rax, %rcx
    callq _vx_trace_returned
    movq %rax, %r11
    movq 0x20(%rsp), %rax
    movdqu 0x30(%rsp), %xmm0
    addq $0x40, %rsp
    jmpq *%r11

// uint64_t vx_call_on_stack(uint64_t (*fn)(void *), void *arg, void *stack_top): calls fn(arg) (System V) on
// another stack and returns its result on this one. Used to run Windows callbacks that macOS code makes on its own
// small-stacked threads (the Steam bridge).
    .globl _vx_call_on_stack
_vx_call_on_stack:
    pushq %rbp
    movq %rsp, %rbp
    movq %rdx, %rsp
    andq $-16, %rsp
    movq %rdi, %rax
    movq %rsi, %rdi
    callq *%rax
    movq %rbp, %rsp
    popq %rbp
    retq
