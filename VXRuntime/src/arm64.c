// The ARM64 build of the runtime (phase 4, TRANSLATOR.md): the program's x86-64 code runs translated by the engine
// (translator/engine.c) in this process, and this file joins the two sides.
//
// x86 code calling the runtime: every function with the Windows calling convention has a thunk generated from its C
// signature (tools/vx_thunks.py, included at the end of each source file); the engine turns a call to such an
// address into a call of its thunk, which reads the arguments from the x86 registers and stack.
//
// The runtime calling x86 code (entry points, window procedures, thread start routines, callbacks): x86 code is
// never mapped executable for ARM64, so calling it faults on the first instruction; the signal handler sends the
// call to vx_cb_tramp (context_arm64.S), which runs it on the engine with x0-x7 as RCX RDX R8 R9 and the stack. The
// sources pass only integers and pointers to such calls (tools/vx_thunks.py checks every call through an ms_abi
// function pointer), so no signature is needed on this side.
#ifdef __aarch64__
#include "vx.h"
#include "arm64.h"
#include "../translator/engine.h"
#include "../translator/vcpu.h"
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <execinfo.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

__thread const char *vx_va_slots;
static xe *g_engine;
uint64_t vx_low_base;                                         // (vx.h: the vCPU's memory below 4 GB)
uint8_t vx_low_map[1 << 16] __attribute__((aligned(64))) = { 0 };
enum { X86_RETURN = 0xFFFFFFFFFFFF0000ull };                // where x86 code called from here returns to (never code)

// ---- thunks, by function address ------------------------------------------------------------------------------
enum { THUNK_BITS = 16 };
static vx_thunk g_thunks[1u << THUNK_BITS];
static uint8_t g_fixed[1u << THUNK_BITS];                     // set by vx_thunks_override: kept over generated ones
static uint32_t thunk_slot(uint64_t fn) { return (uint32_t)((fn >> 2) * 0x9E3779B97F4A7C15ull >> (64 - THUNK_BITS)); }
static void add(const vx_thunk *t, size_t n, int fixed) {     // (constructors: before main, one thread)
    for (size_t i = 0; i < n; i++) {
        uint32_t s = thunk_slot((uint64_t)(uintptr_t)t[i].fn);
        while (g_thunks[s].fn && g_thunks[s].fn != t[i].fn) s = (s + 1) & ((1u << THUNK_BITS) - 1);
        if (g_fixed[s] && !fixed) continue;
        g_thunks[s] = t[i]; g_fixed[s] |= (uint8_t)fixed;
    }
}
void vx_thunks_add(const vx_thunk *t, size_t n) { add(t, n, 0); }
void vx_thunks_override(const vx_thunk *t, size_t n) { add(t, n, 1); }
void (*vx_thunk_find(uint64_t fn))(x86_ctx *) {
    for (uint32_t s = thunk_slot(fn); g_thunks[s].fn; s = (s + 1) & ((1u << THUNK_BITS) - 1))
        if ((uint64_t)(uintptr_t)g_thunks[s].fn == fn) return g_thunks[s].thunk;
    return NULL;
}

// ---- calls the vCPU guest may record (vx_defer_add) -------------------------------------------------------------
enum { DEFER_BITS = 10 };
static struct { const void *fn; xv_guest_fn handler; void *d; } g_defer[1u << DEFER_BITS];
static void guest_add(const void *fn, xv_guest_fn handler, void *d) {
    uint32_t s = thunk_slot((uint64_t)(uintptr_t)fn) & ((1u << DEFER_BITS) - 1);
    while (g_defer[s].fn) s = (s + 1) & ((1u << DEFER_BITS) - 1);
    g_defer[s].fn = fn; g_defer[s].handler = handler; g_defer[s].d = d;
}
// A native function guest.c answers itself (desc: its argument, copied to the heap the guest may have).
void vx_guest_add(const void *fn, xv_guest_fn handler, const void *desc, size_t size) {
    void *d = NULL;
    if (size) { d = calloc(1, size); memcpy(d, desc, size); }
    guest_add(fn, handler, d);
}
void vx_defer_add(const vx_defer *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint32_t s = thunk_slot((uint64_t)(uintptr_t)d[i].fn) & ((1u << DEFER_BITS) - 1);
        while (g_defer[s].fn) s = (s + 1) & ((1u << DEFER_BITS) - 1);
        xv_defer *x = calloc(1, sizeof *x);                     // (heap: memory the guest may have)
        x->nargs = d[i].nargs; x->ncopy = d[i].ncopy;
        memcpy(x->copy, d[i].copy, sizeof x->copy);
        g_defer[s].fn = d[i].fn; g_defer[s].handler = (xv_guest_fn)xv_guest_defer; g_defer[s].d = x;
    }
}
static int guest_find(uint64_t fn, uint64_t *handler, const void **desc) {
    for (uint32_t s = thunk_slot(fn) & ((1u << DEFER_BITS) - 1); g_defer[s].fn; s = (s + 1) & ((1u << DEFER_BITS) - 1))
        if ((uint64_t)(uintptr_t)g_defer[s].fn == fn) {
            if (g_defer[s].handler == (xv_guest_fn)xv_guest_defer) {   // (a recorded call: its thunk, run later)
                xv_defer *x = g_defer[s].d;
                if (!x->thunk) x->thunk = (uint64_t)(uintptr_t)vx_thunk_find(fn);
                if (!x->thunk) return 0;
            }
            *handler = (uint64_t)(uintptr_t)g_defer[s].handler; *desc = g_defer[s].d;
            return 1;
        }
    return 0;
}

// ---- the threads' x86 state -------------------------------------------------------------------------------------
void *vx_x86_return_address(void);
static __thread x86_ctx *t_ctx;
x86_ctx *vx_x86_ctx(void) {
    if (t_ctx) return t_ctx;
    uint8_t *teb = vx_teb();                                  // (its x86 stack: vx_thread_attach)
    x86_ctx *c = calloc(1, sizeof *c);
    c->gs_base = (uint64_t)(uintptr_t)teb;
    c->gpr[4] = (*(uint64_t *)(teb + TEB_STACK_BASE) - 64) & ~15ull;
    c->mxcsr = 0x1F80; c->fcw = 0x037F; c->flags = 0x202;
    c->ras = xe_ring_new(g_engine);
    enum { BATCH = 256 << 10 };                               // (vCPU: calls recorded in the guest)
    uint8_t *batch = mmap(NULL, BATCH, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    c->batch_base = c->batch_ptr = (uint64_t)(uintptr_t)batch; c->batch_end = c->batch_base + BATCH;
    t_ctx = c;
    return c;
}

// Calls from here into x86 code nest (a window procedure called from DispatchMessage called by the program ...).
// Each level keeps where its x86 frames begin (top: they are below) and a jmp_buf: an exception handled in a frame
// of level L resumes L's engine loop, and the native frames above it are left, as the x86 build leaves them by
// moving RSP over them.
typedef struct level { struct level *outer; uint64_t top; jmp_buf jb; void *seh; int xv_depth; } level;
static __thread level *t_level;
void *vx_x86_return_address(void) {
    return t_ctx && t_ctx->gpr[4] ? (void *)(uintptr_t)*(uint64_t *)(uintptr_t)t_ctx->gpr[4] : NULL;
}
void vx_arm64_thread_exit(void) { free(t_ctx); t_ctx = NULL; t_level = NULL; }
void *vx_seh_level_begin(const x86_ctx *outer);
void vx_seh_level_end(void *level);
void vx_seh_fault(DWORD code, DWORD n, const uint64_t *params) __attribute__((noreturn));
static uint64_t read_fpcr(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static void write_fpcr(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }

// The program's code stopped at an exception the engine found (ctx->fault, RIP at the instruction).
static void __attribute__((noreturn)) engine_fault(x86_ctx *c, int r) {
    if (r < 0) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)c->rip;
        vx_log("arm64: an instruction the translator does not know at %#llx: %02x %02x %02x %02x %02x %02x", (unsigned long long)c->rip, p[0], p[1], p[2], p[3], p[4], p[5]);
        _exit(3);
    }
    c->fault = 0;
    switch (r) {
    case X86_FAULT_DE: vx_seh_fault(0xC0000094, 0, NULL);                 // STATUS_INTEGER_DIVIDE_BY_ZERO
    case X86_FAULT_BP: vx_seh_fault(0x80000003, 0, NULL);                 // STATUS_BREAKPOINT
    case X86_FAULT_UD: vx_seh_fault(0xC000001D, 0, NULL);                 // STATUS_ILLEGAL_INSTRUCTION
    case X86_FAULT_GP: vx_seh_fault(0xC0000096, 0, NULL);                 // STATUS_PRIVILEGED_INSTRUCTION
    case X86_FAULT_PF: {                                                  // (vCPU: an access fault)
        uint64_t p[2] = { c->fault_info >> 63, c->fault_info & ~(1ull << 63) };
        vx_seh_fault(0xC0000005, 2, p);
    }
    default:
        if (r == X86_FAULT_INT && c->fault_info == 0x29) {             // __fastfail: ends the process
            vx_log("__fastfail(%llu) at %#llx", (unsigned long long)c->gpr[1], (unsigned long long)c->rip);
            _exit(0xC0000409 & 0xff);
        }
        { uint64_t p[2] = { 0, ~0ull }; vx_seh_fault(0xC0000005, 2, p); }   // INT n: an access violation, as on Windows
    }
}

uint64_t vx_x86_call(uint64_t fn, int n, const uint64_t *args) {
    x86_ctx *c = vx_x86_ctx();
    x86_ctx saved = *c;                                       // (the x86 code this call interrupts, if any)
    int nstack = n > 4 ? n - 4 : 0;
    nstack += nstack & 1;
    uint64_t sp = ((c->gpr[4] - 128) & ~15ull) - 8 * (uint64_t)(4 + nstack) - 8;   // (RSP + 8 aligned at the entry)
    uint64_t *s = (uint64_t *)(uintptr_t)sp;
    s[0] = X86_RETURN;
    for (int i = 0; i < 4 + nstack; i++) s[1 + i] = i >= 4 && i < n ? args[i] : 0;
    static const int reg[4] = { 1, 2, 8, 9 };
    for (int i = 0; i < 4; i++) c->gpr[reg[i]] = i < n ? args[i] : 0;
    c->gpr[4] = sp;
    c->rip = fn; c->jump = 0; c->fault = 0;
    level l = { t_level, sp + 8, {0}, t_level ? vx_seh_level_begin(&saved) : NULL, xv_depth() };
    t_level = &l;
    uint64_t fpcr = read_fpcr();
    if (setjmp(l.jb)) { t_level = &l; write_fpcr(fpcr); xv_set_depth(l.xv_depth); }   // (resumed in this level: the
                                                                                     //  context is the handler's)
    int r = xe_run(g_engine, c, X86_RETURN);
    if (r) engine_fault(c, r);
    t_level = l.outer;
    if (l.seh) vx_seh_level_end(l.seh);
    uint64_t ret = c->gpr[0];
    *c = saved;
    return ret;
}
// Resumes the program in this context (an exception handled): in the innermost level whose frames hold its stack.
void __attribute__((noreturn)) vx_arm64_resume(const uint64_t *gpr, uint64_t rip, uint64_t eflags, uint32_t mxcsr, const void *xmm) {
    x86_ctx *c = vx_x86_ctx();
    memcpy(c->gpr, gpr, sizeof c->gpr);
    c->rip = rip; c->flags = (eflags & 0xFD5) | 2; c->mxcsr = mxcsr;
    memcpy(c->xmm, xmm, sizeof c->xmm);
    c->jump = 0; c->fault = 0;
    level *l = t_level;
    while (l && gpr[4] >= l->top) {                       // (levels left: their native frames are not returned to)
        level *outer = l->outer;
        if (l->seh) vx_seh_level_end(l->seh);
        l = outer;
    }
    if (!l) { vx_log("arm64: resuming at %#llx: no call of the program's code holds stack %#llx", (unsigned long long)rip, (unsigned long long)gpr[4]); _exit(3); }
    longjmp(l->jb, 1);
}
// From vx_cb_tramp: a call into x86 code at `fn`, its eight argument registers, then the caller's stack arguments.
uint64_t vx_x86_invoke(uint64_t fn, const uint64_t *regs, const uint64_t *stack) {
    uint64_t args[12];
    memcpy(args, regs, 8 * sizeof *args);
    memcpy(args + 8, stack, 4 * sizeof *args);
    return vx_x86_call(fn, 12, args);
}
void vx_cb_tramp(void);

// ---- functions that work on the x86 state itself ---------------------------------------------------------------------
// __chkstk: the stacks are mapped whole; the probe returns with every register as it was.
void vx_chkstk(void) { abort(); }
static void th_chkstk(x86_ctx *c) { (void)c; }
// _setjmp(jmp_buf, frame) / longjmp(jmp_buf, value): _JUMP_BUFFER as context.S documents it.
int vx_setjmp(void *buf, void *frame) { (void)buf; (void)frame; abort(); }
void vx_longjmp(void *buf, int v) { (void)buf; (void)v; abort(); }
static void th_setjmp(x86_ctx *c) {
    uint8_t *b = (uint8_t *)(uintptr_t)c->gpr[1];
    uint64_t rsp = c->gpr[4];
    *(uint64_t *)(b + 0x00) = c->gpr[2];
    *(uint64_t *)(b + 0x08) = c->gpr[3];
    *(uint64_t *)(b + 0x10) = rsp + 8;
    *(uint64_t *)(b + 0x18) = c->gpr[5];
    *(uint64_t *)(b + 0x20) = c->gpr[6];
    *(uint64_t *)(b + 0x28) = c->gpr[7];
    for (int r = 12; r < 16; r++) *(uint64_t *)(b + 0x30 + 8 * (r - 12)) = c->gpr[r];
    *(uint64_t *)(b + 0x50) = *(uint64_t *)(uintptr_t)rsp;
    *(uint32_t *)(b + 0x58) = c->mxcsr;
    *(uint16_t *)(b + 0x5c) = c->fcw;
    memcpy(b + 0x60, c->xmm[6], 10 * 16);
    c->gpr[0] = 0;
}
static void th_longjmp(x86_ctx *c) {
    const uint8_t *b = (const uint8_t *)(uintptr_t)c->gpr[1];
    uint32_t v = (uint32_t)c->gpr[2];
    c->gpr[0] = v ? v : 1;
    c->gpr[3] = *(const uint64_t *)(b + 0x08);
    c->gpr[4] = *(const uint64_t *)(b + 0x10);
    c->gpr[5] = *(const uint64_t *)(b + 0x18);
    c->gpr[6] = *(const uint64_t *)(b + 0x20);
    c->gpr[7] = *(const uint64_t *)(b + 0x28);
    for (int r = 12; r < 16; r++) c->gpr[r] = *(const uint64_t *)(b + 0x30 + 8 * (r - 12));
    c->rip = *(const uint64_t *)(b + 0x50);
    memcpy(c->xmm[6], b + 0x60, 10 * 16);
    c->jump = 1;
}
// Not yet on ARM64 (exceptions, fibers, call tracing): named when reached.
#define NOT_YET(name) void name(void) { vx_log("arm64: %s is not implemented yet", #name); _exit(3); }
NOT_YET(vx_fiber_switch) NOT_YET(vx_trace_return)
NOT_YET(vx_call_on_stack)
uint8_t vx_trace_stubs[16];
static const vx_thunk g_special[] = {
    { (const void *)vx_chkstk, th_chkstk }, { (const void *)vx_setjmp, th_setjmp }, { (const void *)vx_longjmp, th_longjmp },
};

// The thread's x86 state where a signal stopped it (SuspendThread's GetThreadContext): in translated code, the
// registers it keeps the state in (a stub between blocks: the next RIP in x17); in this runtime, called by the
// program, as just returned from that call. 0: the thread has not run x86 code.
int vx_arm64_state_at(const void *ucv, void *outv) {
    const ucontext_t *uc = ucv;
    x86_ctx *out = outv;
    if (!t_ctx) return 0;
    *out = *t_ctx;
    const _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    if (xe_owns(g_engine, ss->__pc)) {
        uint64_t rip = xe_rip_at(g_engine, ss->__pc);
        for (int i = 0; i < 16; i++) { out->gpr[i] = ss->__x[i]; memcpy(out->xmm[i], &uc->uc_mcontext->__ns.__v[i], 16); }
        out->flags = xlat_flags_resolve(ss->__x[27], t_ctx);   // (pending flags: computed)
        out->rip = rip ? rip : ss->__x[17];
        return 1;
    }
    uint64_t rsp = out->gpr[4];
    if (rsp) { out->rip = *(const uint64_t *)(uintptr_t)rsp; out->gpr[4] = rsp + 8; }
    return 1;
}

// vCPU: a thread SuspendThread stopped in the guest (translator/vcpu.c), from the guest's registers as above; in
// guest.c's code (a call it answers), as just returned from that call (the call stored the state first).
void vx_thread_park_state(const void *state);                 // sync.c
static void vcpu_interrupted(x86_ctx *c, const xv_regs *r) {
    static __thread x86_ctx *s;
    if (!s) s = calloc(1, sizeof *s);
    *s = *c;
    if (xe_owns(g_engine, r->pc) && !xe_in_guest(g_engine, r->pc)) {
        uint64_t rip = xe_rip_at(g_engine, r->pc);
        for (int i = 0; i < 16; i++) { s->gpr[i] = r->x[i]; memcpy(s->xmm[i], r->q[i], 16); }
        s->flags = xlat_flags_resolve(r->x[27], c); s->ras = r->x[29];
        s->rip = rip ? rip : r->x[17];
    } else if (s->gpr[4]) {
        s->rip = *(const uint64_t *)(uintptr_t)s->gpr[4]; s->gpr[4] += 8;
    }
    vx_thread_park_state(s);
}
int vx_arm64_interrupt(void) { return xv_interrupt(); }

// ---- faults -----------------------------------------------------------------------------------------------------
static __thread uint64_t t_fault[2];                          // the access fault being dispatched: write?, address
static void __attribute__((noreturn)) fault_entry(void) { vx_seh_fault(0xC0000005, 2, t_fault); }
// The program wrote code it had run (SMC): its translations are gone; it goes on at the writing instruction, so
// that the rest of a block it may have changed is translated again too.
static void __attribute__((noreturn)) smc_restart(void) {
    x86_ctx *c = t_ctx;
    c->single = 1;                                            // (the write first, alone: then what follows, as written)
    level *l = t_level;
    while (l && c->gpr[4] >= l->top) l = l->outer;
    if (!l) { vx_log("arm64: no call of the program's code holds stack %#llx", (unsigned long long)c->gpr[4]); _exit(3); }
    longjmp(l->jb, 1);
}
// From the program's registers in a signal's machine context into its x86 state, at rip.
static void state_from_signal(x86_ctx *c, const ucontext_t *uc, uint64_t rip) {
    const _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    for (int i = 0; i < 16; i++) { c->gpr[i] = ss->__x[i]; memcpy(c->xmm[i], &uc->uc_mcontext->__ns.__v[i], 16); }
    c->flags = xlat_flags_resolve(ss->__x[27], c); c->ras = ss->__fp; c->rip = rip;
}
void vx_code_freed(void *addr, size_t len) { xe_forget(g_engine, (uint64_t)(uintptr_t)addr, len); }
uint64_t vx_arm64_x86_pc(uint64_t pc) {
    uint64_t rip = xe_owns(g_engine, pc) ? xe_rip_at(g_engine, pc) : 0;
    return rip ? rip : pc;
}
static void on_fault(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    uint64_t pc = ss->__pc;
    if ((sig == SIGSEGV || sig == SIGBUS) && xe_fix_split(g_engine, uc)) return;   // (ordered access across 16 bytes)
    // A write to a page whose code was translated (SMC). A permission fault on a readable page is a write, whatever
    // WnR says (LSE atomics, XCHG's SWPAL, may fault with WnR 0).
    uint64_t esr = uc->uc_mcontext->__es.__esr;
    int write = ((esr >> 6) & 1) || ((esr >> 26) == 0x24 && (esr & 0x3C) == 0x0C);
    if ((sig == SIGSEGV || sig == SIGBUS) && write && xe_smc_write(g_engine, (uint64_t)(uintptr_t)si->si_addr)) {
        uint64_t at = xe_owns(g_engine, pc) ? xe_rip_at(g_engine, pc) : 0;
        if (at && t_ctx) { state_from_signal(t_ctx, uc, at); ss->__pc = (uint64_t)(uintptr_t)smc_restart; ss->__lr = 0; }
        return;                                               // (native code: the write again, now allowed)
    }
    // An instruction fetch from a page that is not executable (native code always is): a call into x86 code.
    if ((sig == SIGSEGV || sig == SIGBUS) && (uint64_t)(uintptr_t)si->si_addr == pc && !xe_owns(g_engine, pc)) {
        ss->__x[9] = pc;                                      // a call into x86 code: on through the engine
        ss->__pc = (uint64_t)(uintptr_t)vx_cb_tramp;
        return;
    }
    uint64_t rip = xe_owns(g_engine, pc) ? xe_rip_at(g_engine, pc) : 0;
    if (rip && t_ctx && (sig == SIGSEGV || sig == SIGBUS) && getenv("VXRT_TRACE_FAULTS")) {   // (with the module, RSP and the thread's stack: a stack overflow shows as an address just below its limit)
        const vx_image *m = vx_image_at(rip); uint8_t *teb = vx_teb();
        vx_log("arm64: fault at x86 %#llx (%s+%#llx, host %#llx), address %p, ESR %#llx, rsp %#llx, stack %p-%p",
               (unsigned long long)rip, m ? m->name : "?", m ? (unsigned long long)(rip - (uint64_t)(uintptr_t)m->base) : 0ull,
               (unsigned long long)pc, si->si_addr, (unsigned long long)esr, (unsigned long long)ss->__x[4],
               *(void **)(teb + TEB_STACK_LIMIT), *(void **)(teb + TEB_STACK_BASE));
    }
    if (rip && t_ctx && (sig == SIGSEGV || sig == SIGBUS)) {
        // The program's access fault: its state from the registers the translation keeps it in, then on (out of
        // the signal) to the dispatch on this thread's stack, below the translated code's frame.
        state_from_signal(t_ctx, uc, rip);
        t_fault[0] = (uc->uc_mcontext->__es.__esr >> 6) & 1;             // (WnR: a write)
        t_fault[1] = (uint64_t)(uintptr_t)si->si_addr;
        ss->__pc = (uint64_t)(uintptr_t)fault_entry;
        ss->__lr = 0;
        return;
    }
    if (xe_owns(g_engine, pc))
        vx_log("arm64: signal %d in translated code (host pc %#llx, no x86 instruction there, address %p)", sig, (unsigned long long)pc, si->si_addr);
    else
        vx_log("signal %d at %#llx (address %p) in this runtime; native backtrace:", sig, (unsigned long long)pc, si->si_addr);
    void *frames[32];
    frames[0] = (void *)(uintptr_t)pc;
    int n = 1;
    for (uint64_t *fp = (uint64_t *)(uintptr_t)ss->__fp; fp && n < 32 && ((uint64_t)(uintptr_t)fp & 7) == 0; fp = (uint64_t *)(uintptr_t)fp[0]) {
        if (fp[1] < 4096) break;
        frames[n++] = (void *)(uintptr_t)fp[1];
        if (fp[0] <= (uint64_t)(uintptr_t)fp) break;
    }
    backtrace_symbols_fd(frames, n, 2);
    _exit(128 + sig);
}

void vx_arm64_init(void) {
    // x86's memory order for accesses other threads may see (LDAPR / STLR, TRANSLATOR.md): VXRT_ORDERED=0 turns it off.
    // VXRT_VCPU=1: in a Hypervisor.framework vCPU with x86's order in hardware (translator/vcpu.c).
    if (getenv("VXRT_VCPU") && atoi(getenv("VXRT_VCPU"))) {
        g_engine = xe_new_vcpu();
        void vxvm_set_hook(void (*)(uint64_t, uint64_t));             // (src/vm_hooks.c: memory going away leaves the guest)
        vxvm_set_hook(xv_unmap);
        xv_set_interrupt_hook(vcpu_interrupted);
        // The guest's low 4 GB (LuaJIT 2.0 asks for memory below 2 GB): a reservation of the host's, no access until
        // handed out (kernel32.c's vx_virtual_alloc_below)
        mach_vm_address_t low = 0;
        if (mach_vm_allocate(mach_task_self(), &low, 1ull << 32, VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
            mach_vm_protect(mach_task_self(), low, 1ull << 32, 1, VM_PROT_NONE);
            mach_vm_protect(mach_task_self(), low, 1ull << 32, 0, VM_PROT_NONE);
            vx_low_base = low;
            xv_set_low(low);
        }
        if (!getenv("VXRT_VCPU_NO_GUEST")) xe_set_guest(g_engine, guest_find);
    }
    else g_engine = xe_new(!getenv("VXRT_ORDERED") || atoi(getenv("VXRT_ORDERED")) != 0);
    if (!g_engine) { fprintf(stderr, "vxrt: cannot map the translation cache\n"); exit(1); }
    xe_set_native(g_engine, vx_thunk_find);
    xe_set_smc(g_engine, 1);
    vx_thunks_override(g_special, sizeof g_special / sizeof *g_special);
    void vx_sync_arm64_init(void);
    vx_sync_arm64_init();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE };
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, NULL);
}
#endif

