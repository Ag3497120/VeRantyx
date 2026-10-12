// Structured exception handling, x64 Windows style.
//
// Exceptions are found and unwound with the program's own unwind data (.pdata RUNTIME_FUNCTIONs pointing at
// .xdata UNWIND_INFOs), the way Windows does it: RtlLookupFunctionEntry + RtlVirtualUnwind walk the frames,
// language handlers (__C_specific_handler, GCC's __gxx_personality_seh0, MSVC's __CxxFrameHandler) decide, and
// RtlUnwindEx unwinds to the frame that takes the exception. CPU faults arrive as macOS signals and are turned
// into the same dispatch on the faulting thread's own stack.
//
// This runtime's own functions are native code without Windows unwind data, so every walk starts from a
// context in the program's code: RaiseException and RtlUnwindEx record their caller's state on entry
// (context.S), and an unwind started from inside a handler restarts from the context of the exception being
// dispatched.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ucontext.h>
#include <execinfo.h>
#include "vx.h"

// ---- structures --------------------------------------------------------------------------------------------------
typedef struct { uint64_t lo, hi; } M128A;
typedef struct __attribute__((aligned(16))) {
    uint64_t home[6];
    DWORD flags, mxcsr;
    uint16_t cs, ds, es, fs, gs, ss;
    DWORD eflags;
    uint64_t dr[6];
    uint64_t gpr[16];       // Rax Rcx Rdx Rbx Rsp Rbp Rsi Rdi R8..R15, in the unwind codes' register order
    uint64_t rip;
    uint8_t  fltsave_head[0xa0];
    M128A    xmm[16];
    uint8_t  rest[0x4d0 - 0x2a0];
} CONTEXT;
_Static_assert(sizeof(CONTEXT) == 0x4d0, "CONTEXT size");
_Static_assert(__builtin_offsetof(CONTEXT, gpr) == 0x78, "CONTEXT gpr");
_Static_assert(__builtin_offsetof(CONTEXT, rip) == 0xf8, "CONTEXT rip");
_Static_assert(__builtin_offsetof(CONTEXT, xmm) == 0x1a0, "CONTEXT xmm");
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

typedef struct EXCEPTION_RECORD {
    DWORD code, flags;
    struct EXCEPTION_RECORD *next;
    void *address;
    DWORD nparams, pad;
    uint64_t info[15];
} EXCEPTION_RECORD;
typedef struct { EXCEPTION_RECORD *rec; CONTEXT *ctx; } EXCEPTION_POINTERS;
typedef struct { DWORD begin, end, unwind; } RUNTIME_FUNCTION;
typedef struct {
    uint64_t control_pc, image_base; RUNTIME_FUNCTION *function; uint64_t establisher_frame, target_ip;
    CONTEXT *context; void *language_handler, *handler_data, *history_table; DWORD scope_index, fill0;
} DISPATCHER_CONTEXT;
typedef int (WINAPI *language_handler)(EXCEPTION_RECORD *, uint64_t frame, CONTEXT *, DISPATCHER_CONTEXT *);

enum { EXCEPTION_NONCONTINUABLE = 1, EXCEPTION_UNWINDING = 2, EXCEPTION_EXIT_UNWIND = 4, EXCEPTION_TARGET_UNWIND = 0x20 };
enum { ExceptionContinueExecution = 0, ExceptionContinueSearch = 1 };
enum { UNW_FLAG_EHANDLER = 1, UNW_FLAG_UHANDLER = 2, UNW_FLAG_CHAININFO = 4 };

void vx_restore_context(const CONTEXT *ctx) __attribute__((noreturn));

// ---- function lookup ------------------------------------------------------------------------------------------------
static int pc_in_image(uint64_t pc) { return vx_image_at(pc) != NULL; }

static WINAPI RUNTIME_FUNCTION *RtlLookupFunctionEntry(uint64_t pc, uint64_t *base, void *history) {
    (void)history;
    const vx_image *m = vx_image_at(pc);
    if (!m || !m->pdata_size) return NULL;
    *base = (uint64_t)m->base;
    RUNTIME_FUNCTION *table = (RUNTIME_FUNCTION *)(m->base + m->pdata_rva);
    size_t lo = 0, hi = m->pdata_size / sizeof(RUNTIME_FUNCTION);
    DWORD rva = (DWORD)(pc - *base);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < table[mid].begin) hi = mid;
        else if (rva >= table[mid].end) lo = mid + 1;
        else return &table[mid];
    }
    return NULL;
}

static WINAPI void *RtlPcToFileHeader(uint64_t pc, void **base) {
    const vx_image *m = vx_image_at(pc);
    *base = m ? m->base : NULL;
    return *base;
}

// ---- virtual unwind ------------------------------------------------------------------------------------------------------
typedef struct { uint8_t offset; uint8_t op_info; } unwind_code;
typedef struct { uint8_t version_flags, prolog, count, frame; unwind_code codes[]; } unwind_info;

static int code_slots(const unwind_code *c) {
    switch (c->op_info & 0xf) {
    case 1: return (c->op_info >> 4) ? 3 : 2;   // ALLOC_LARGE
    case 4: case 8: return 2;                     // SAVE_NONVOL, SAVE_XMM128
    case 5: case 9: return 3;                     // the _FAR forms
    default: return 1;
    }
}

// Is pc inside an epilogue (add/lea rsp; pops; ret or a jump out of the function)? Wine's check.
static int in_epilog(const uint8_t *pc, uint64_t base, const RUNTIME_FUNCTION *fn) {
    if ((pc[0] & 0xf8) == 0x48) {
        switch (pc[1]) {
        case 0x81: if (pc[0] == 0x48 && pc[2] == 0xc4) { pc += 7; break; } return 0;
        case 0x83: if (pc[0] == 0x48 && pc[2] == 0xc4) { pc += 4; break; } return 0;
        case 0x8d:
            if (pc[0] & 0x06) return 0;
            if (((pc[2] >> 3) & 7) != 4 || (pc[2] & 7) == 4) return 0;
            if ((pc[2] >> 6) == 1) { pc += 4; break; }
            if ((pc[2] >> 6) == 2) { pc += 7; break; }
            return 0;
        }
    }
    for (;;) {
        if ((*pc & 0xf0) == 0x40) pc++;
        switch (*pc) {
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: pc++; continue;
        case 0xc2: case 0xc3: return 1;
        case 0xe9: { uint64_t t = (uint64_t)pc + 5 + *(int32_t *)(pc + 1);
                     return t < base + fn->begin || t >= base + fn->end; }
        case 0xeb: { uint64_t t = (uint64_t)pc + 2 + (int8_t)pc[1];
                     return t < base + fn->begin || t >= base + fn->end; }
        case 0xf3: return pc[1] == 0xc3;
        case 0xff: return pc[1] == 0x25;
        default: return 0;
        }
    }
}

static void run_epilog(const uint8_t *pc, CONTEXT *c) {
    if ((pc[0] & 0xf8) == 0x48) {
        switch (pc[1]) {
        case 0x81: c->gpr[RSP] += *(int32_t *)(pc + 3); pc += 7; break;
        case 0x83: c->gpr[RSP] += (int8_t)pc[3]; pc += 4; break;
        case 0x8d: {
            int reg = (pc[2] & 7) | ((pc[0] & 1) << 3);
            if ((pc[2] >> 6) == 1) { c->gpr[RSP] = c->gpr[reg] + (int8_t)pc[3]; pc += 4; }
            else { c->gpr[RSP] = c->gpr[reg] + *(int32_t *)(pc + 3); pc += 7; }
            break;
        }
        }
    }
    for (;;) {
        int rex = (*pc & 0xf0) == 0x40 ? *pc++ & 0xf : 0;
        if (*pc >= 0x58 && *pc <= 0x5f) {
            c->gpr[(*pc - 0x58) | ((rex & 1) << 3)] = *(uint64_t *)c->gpr[RSP];
            c->gpr[RSP] += 8; pc++; continue;
        }
        break;   // ret / jmp: the return address is popped below
    }
    c->rip = *(uint64_t *)c->gpr[RSP];
    c->gpr[RSP] += 8;
}

static WINAPI void *RtlVirtualUnwind(DWORD type, uint64_t base, uint64_t pc, RUNTIME_FUNCTION *fn, CONTEXT *c,
                                     void **data, uint64_t *frame_ret, void *ctx_pointers) {
    (void)ctx_pointers;
    uint64_t frame = c->gpr[RSP];
    *frame_ret = frame;
    const unwind_info *info;
    DWORD prolog_offset;
    int mach_frame = 0;
    for (;;) {
        info = (const unwind_info *)(base + fn->unwind);
        int frame_reg = info->frame & 0xf;
        if (frame_reg) frame = c->gpr[frame_reg] - (info->frame >> 4) * 16;
        if (pc >= base + fn->begin && pc < base + fn->begin + info->prolog) {
            prolog_offset = (DWORD)(pc - base - fn->begin);
        } else {
            prolog_offset = ~0u;
            if (in_epilog((const uint8_t *)pc, base, fn)) {
                *frame_ret = frame;
                run_epilog((const uint8_t *)pc, c);
                return NULL;
            }
        }
        for (int i = 0; i < info->count; i += code_slots(&info->codes[i])) {
            const unwind_code *uc = &info->codes[i];
            if (prolog_offset < uc->offset) continue;
            int op = uc->op_info & 0xf, opinfo = uc->op_info >> 4;
            switch (op) {
            case 0: c->gpr[opinfo] = *(uint64_t *)c->gpr[RSP]; c->gpr[RSP] += 8; break;                       // PUSH_NONVOL
            case 1: c->gpr[RSP] += opinfo ? *(uint32_t *)&info->codes[i + 1] : *(uint16_t *)&info->codes[i + 1] * 8u; break;
            case 2: c->gpr[RSP] += (opinfo + 1) * 8; break;                                                     // ALLOC_SMALL
            case 3: c->gpr[RSP] = *frame_ret = frame; break;                                                    // SET_FPREG
            case 4: c->gpr[opinfo] = *(uint64_t *)(frame + *(uint16_t *)&info->codes[i + 1] * 8); break;
            case 5: c->gpr[opinfo] = *(uint64_t *)(frame + *(uint32_t *)&info->codes[i + 1]); break;
            case 8: memcpy(&c->xmm[opinfo], (void *)(frame + *(uint16_t *)&info->codes[i + 1] * 16), 16); break;
            case 9: memcpy(&c->xmm[opinfo], (void *)(frame + *(uint32_t *)&info->codes[i + 1]), 16); break;
            case 10:                                                                                            // PUSH_MACHFRAME
                if (opinfo) c->gpr[RSP] += 8;
                c->rip = *(uint64_t *)c->gpr[RSP];
                c->gpr[RSP] = *(uint64_t *)(c->gpr[RSP] + 24);
                mach_frame = 1;
                break;
            }
        }
        if (!(info->version_flags >> 3 & UNW_FLAG_CHAININFO)) break;
        fn = (RUNTIME_FUNCTION *)&info->codes[(info->count + 1) & ~1];
    }
    if (!mach_frame) { c->rip = *(uint64_t *)c->gpr[RSP]; c->gpr[RSP] += 8; }
    if (!((info->version_flags >> 3) & type) || prolog_offset != ~0u) return NULL;
    const DWORD *handler_rva = (const DWORD *)&info->codes[(info->count + 1) & ~1];
    *data = (void *)(handler_rva + 1);
    return (void *)(base + *handler_rva);
}

// ---- dispatch ---------------------------------------------------------------------------------------------------------------
typedef int32_t (WINAPI *vectored_handler)(EXCEPTION_POINTERS *);
static vectored_handler g_vectored[32];
static int g_vectored_n;
static vectored_handler g_unhandled_filter;

static WINAPI void *AddVectoredExceptionHandler(DWORD first, vectored_handler h) {
    if (g_vectored_n >= 32) return NULL;
    if (first) { memmove(g_vectored + 1, g_vectored, sizeof(g_vectored[0]) * g_vectored_n); g_vectored[0] = h; }
    else g_vectored[g_vectored_n] = h;
    g_vectored_n++;
    return (void *)h;
}
static WINAPI DWORD RemoveVectoredExceptionHandler(void *h) {
    for (int i = 0; i < g_vectored_n; i++)
        if ((void *)g_vectored[i] == h) { memmove(g_vectored + i, g_vectored + i + 1, sizeof(g_vectored[0]) * (--g_vectored_n - i)); return 1; }
    return 0;
}
static WINAPI vectored_handler SetUnhandledExceptionFilter(vectored_handler f) { vectored_handler o = g_unhandled_filter; g_unhandled_filter = f; return o; }
static WINAPI int32_t UnhandledExceptionFilter(EXCEPTION_POINTERS *ep) { (void)ep; return 1; }

// ---- walking the program's stack -----------------------------------------------------------------------------------------
// The program's frames are walked with its unwind data. Between them sit this runtime's own native frames (an
// exception being dispatched, an unwind in progress, a handler called from either), which have no Windows
// unwind data. Every entry into the runtime that can call back into the program records where the program
// was, and a walk that reaches native code jumps to that record, the way Windows walks through the frames of
// its own dispatcher. An unwind that meets another unwind in progress continues from the frame that unwind was
// processing (a "collided unwind"), so no frame is unwound twice. This is what MinGW's C++ exceptions need:
// their cleanup handlers raise a new exception from inside an unwind and start another unwind from there.
typedef struct vx_entry {
    struct vx_entry *next;      // the next older entry on this thread
    CONTEXT *ctx;               // where the program called into the runtime (or faulted)
    CONTEXT *unwinding_frame;   // for an unwind: the frame whose handler is running, else NULL
} vx_entry;
static __thread vx_entry *t_entries;

// Moves c to the next frame that has unwind data and returns its function entry; NULL at the bottom of the stack.
static RUNTIME_FUNCTION *next_function(CONTEXT *c, vx_entry **cursor, int unwinding, uint64_t *base) {
    for (int n = 0; n < 65536; n++) {
        RUNTIME_FUNCTION *fn = RtlLookupFunctionEntry(c->rip, base, NULL);
        if (fn) return fn;
        if (pc_in_image(c->rip)) {                // a leaf function: the return address is on top of the stack
            c->rip = *(uint64_t *)c->gpr[RSP];
            c->gpr[RSP] += 8;
            continue;
        }
        vx_entry *e = *cursor;                     // native code: skip to where the program entered it
        if (!e) return NULL;
        *cursor = e->next;
        *c = unwinding && e->unwinding_frame ? *e->unwinding_frame : *e->ctx;
    }
    return NULL;
}

// Resumes the program at ctx. Runtime entries deeper than the target stack are finished.
static void __attribute__((noreturn)) resume(CONTEXT *ctx) {
    while (t_entries && (uint64_t)t_entries < ctx->gpr[RSP]) t_entries = t_entries->next;
    vx_restore_context(ctx);
}

static void __attribute__((noreturn)) dispatch(EXCEPTION_RECORD *rec, CONTEXT *ctx) {
    vx_entry self = { t_entries, ctx, NULL };
    t_entries = &self;
    EXCEPTION_POINTERS ep = { rec, ctx };
    for (int i = 0; i < g_vectored_n; i++)
        if (g_vectored[i](&ep) == -1 /* EXCEPTION_CONTINUE_EXECUTION */) resume(ctx);
    CONTEXT c = *ctx;
    vx_entry *cursor = self.next;
    for (int depth = 0; depth < 65536; depth++) {
        uint64_t base;
        RUNTIME_FUNCTION *fn = next_function(&c, &cursor, 0, &base);
        if (!fn) break;
        CONTEXT before = c;
        void *data = NULL; uint64_t frame = 0;
        language_handler h = RtlVirtualUnwind(UNW_FLAG_EHANDLER, base, c.rip, fn, &c, &data, &frame, NULL);
        if (!h) continue;
        DISPATCHER_CONTEXT dc = { before.rip, base, fn, frame, 0, &before, (void *)h, data, NULL, 0, 0 };
        if (h(rec, frame, ctx, &dc) == ExceptionContinueExecution) {
            if (rec->flags & EXCEPTION_NONCONTINUABLE) { vx_log("a handler continued a noncontinuable exception"); _exit(3); }
            resume(ctx);
        }
    }
    if (g_unhandled_filter && g_unhandled_filter(&ep) == -1) resume(ctx);
    vx_log("unhandled exception %#x at %p", rec->code, rec->address);
    fflush(stdout);
    _exit((int)rec->code);
}

// Called by RaiseException (context.S) with the caller's context.
void __attribute__((noreturn)) vx_raise(DWORD code, DWORD flags, DWORD n, const uint64_t *args, CONTEXT *ctx) {
    EXCEPTION_RECORD rec = { code, flags & EXCEPTION_NONCONTINUABLE, NULL, (void *)ctx->rip, n > 15 ? 15 : n, 0, {0} };
    if (args) memcpy(rec.info, args, rec.nparams * 8);
    dispatch(&rec, ctx);
}

// Called by RtlUnwindEx/RtlUnwind (context.S) with the caller's context. Unwinds frame by frame, calling
// termination handlers, until the frame `target_frame`, then resumes it at target_ip with rax = retval.
// The caller's CONTEXT argument is not used as scratch space: it may be the record of an exception still
// being dispatched.
void __attribute__((noreturn)) vx_unwind(uint64_t target_frame, uint64_t target_ip, EXCEPTION_RECORD *rec,
                                         uint64_t retval, CONTEXT *unused, CONTEXT *captured) {
    (void)unused;
    vx_entry self = { t_entries, captured, NULL };
    t_entries = &self;
    EXCEPTION_RECORD local = { 0xC0000027 /* STATUS_UNWIND */, 0, NULL, NULL, 0, 0, {0} };
    if (!rec) rec = &local;
    rec->flags |= EXCEPTION_UNWINDING;
    if (!target_frame) rec->flags |= EXCEPTION_EXIT_UNWIND;
    CONTEXT out;
    CONTEXT c = *captured;
    vx_entry *cursor = self.next;
    for (int depth = 0; depth < 65536; depth++) {
        uint64_t base;
        RUNTIME_FUNCTION *fn = next_function(&c, &cursor, 1, &base);
        if (!fn) break;
        CONTEXT before = c;
        void *data = NULL; uint64_t frame = 0;
        language_handler h = RtlVirtualUnwind(UNW_FLAG_UHANDLER, base, c.rip, fn, &c, &data, &frame, NULL);
        int at_target = frame == target_frame;
        if (at_target) rec->flags |= EXCEPTION_TARGET_UNWIND;
        out = before;
        if (h) {
            DISPATCHER_CONTEXT dc = { before.rip, base, fn, frame, target_ip, &out, (void *)h, data, NULL, 0, 0 };
            self.unwinding_frame = &before;
            h(rec, frame, &out, &dc);
            self.unwinding_frame = NULL;
        }
        if (at_target) {
            out.rip = target_ip;
            out.gpr[RAX] = retval;
            resume(&out);
        }
    }
    vx_log(target_frame ? "unwind did not reach its target frame %#llx" : "exit unwind reached the end of the stack",
           (unsigned long long)target_frame);
    fflush(stdout);
    _exit(3);
}

// ---- __C_specific_handler (the C runtime's handler for __try/__except/__finally) ----------------------------------------------
typedef struct { DWORD begin, end, handler, target; } scope_record;
typedef struct { DWORD count; scope_record r[]; } scope_table;
void vx_rtl_unwind_ex(uint64_t, uint64_t, EXCEPTION_RECORD *, uint64_t, CONTEXT *, void *) __asm__("_vx_RtlUnwindEx");

static WINAPI int __C_specific_handler(EXCEPTION_RECORD *rec, uint64_t frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc) {
    const scope_table *t = dc->handler_data;
    DWORD pc = (DWORD)(dc->control_pc - dc->image_base);
    if (rec->flags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        DWORD target = (DWORD)(dc->target_ip - dc->image_base);
        for (DWORD i = dc->scope_index; i < t->count; i++) {
            const scope_record *s = &t->r[i];
            if (pc < s->begin || pc >= s->end) continue;
            if ((rec->flags & EXCEPTION_TARGET_UNWIND) && target >= s->begin && target < s->end) break;
            if (s->target == 0) {                         // __finally
                dc->scope_index = i + 1;
                ((void (WINAPI *)(int, uint64_t))(dc->image_base + s->handler))(1, frame);
            } else if ((rec->flags & EXCEPTION_TARGET_UNWIND) && dc->target_ip == dc->image_base + s->target) {
                break;
            }
        }
        return ExceptionContinueSearch;
    }
    EXCEPTION_POINTERS ep = { rec, ctx };
    for (DWORD i = dc->scope_index; i < t->count; i++) {
        const scope_record *s = &t->r[i];
        if (pc < s->begin || pc >= s->end || s->target == 0) continue;
        int32_t r = s->handler == 1 ? 1 : ((int32_t (WINAPI *)(EXCEPTION_POINTERS *, uint64_t))(dc->image_base + s->handler))(&ep, frame);
        if (r < 0) return ExceptionContinueExecution;
        if (r > 0) vx_rtl_unwind_ex(frame, dc->image_base + s->target, rec, rec->code, dc->context, dc->history_table);
    }
    return ExceptionContinueSearch;
}

// ---- CPU faults: macOS signals become Windows exceptions -------------------------------------------------------------------
static void signal_dispatch(EXCEPTION_RECORD *rec, CONTEXT *ctx) { dispatch(rec, ctx); }

static void on_signal(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    if (!pc_in_image(ss->__rip)) {
        // A fault in this runtime or in macOS code is a bug here, not the program's exception.
        // Print where (this runtime's frames) and exit at once rather than waiting for a macOS crash report.
        vx_log("signal %d at %#llx (address %p) outside the program; native backtrace:", sig, (unsigned long long)ss->__rip, si->si_addr);
        void *frames[32];
        frames[0] = (void *)ss->__rip;
        int n = 1;
        for (uint64_t *fp = (uint64_t *)ss->__rbp; fp && n < 32 && ((uint64_t)fp & 7) == 0; fp = (uint64_t *)fp[0]) {
            if (fp[1] < 4096) break;
            frames[n++] = (void *)fp[1];
            if (fp[0] <= (uint64_t)fp) break;
        }
        backtrace_symbols_fd(frames, n, 2);
        _exit(128 + sig);
    }
    // Build the record and context in a frame on the faulting thread's own stack, below its red zone, and make
    // the signal return into signal_dispatch there.
    uint64_t sp = (ss->__rsp - 256 - sizeof(CONTEXT) - sizeof(EXCEPTION_RECORD)) & ~(uint64_t)63;
    CONTEXT *c = (CONTEXT *)sp;
    EXCEPTION_RECORD *rec = (EXCEPTION_RECORD *)(sp + sizeof(CONTEXT));
    memset(c, 0, sizeof *c); memset(rec, 0, sizeof *rec);
    c->flags = 0x10001f;
    uint64_t regs[16] = { ss->__rax, ss->__rcx, ss->__rdx, ss->__rbx, ss->__rsp, ss->__rbp, ss->__rsi, ss->__rdi,
                          ss->__r8, ss->__r9, ss->__r10, ss->__r11, ss->__r12, ss->__r13, ss->__r14, ss->__r15 };
    memcpy(c->gpr, regs, sizeof regs);
    c->rip = ss->__rip;
    c->eflags = (DWORD)ss->__rflags;
    c->mxcsr = uc->uc_mcontext->__fs.__fpu_mxcsr;
    memcpy(c->xmm, &uc->uc_mcontext->__fs.__fpu_xmm0, 16 * 16);
    rec->address = (void *)ss->__rip;
    switch (sig) {
    case SIGSEGV: case SIGBUS:
        rec->code = 0xC0000005;                      // EXCEPTION_ACCESS_VIOLATION
        rec->nparams = 2;
        rec->info[0] = (uc->uc_mcontext->__es.__err & 2) ? 1 : 0;   // write?
        rec->info[1] = (uint64_t)si->si_addr;
        break;
    case SIGFPE:
        rec->code = si->si_code == FPE_INTDIV ? 0xC0000094 : si->si_code == FPE_INTOVF ? 0xC0000095 : 0xC0000090;
        break;
    case SIGILL: rec->code = 0xC000001D; break;    // EXCEPTION_ILLEGAL_INSTRUCTION
    case SIGTRAP:                                   // int3: Windows reports the breakpoint's own address
        rec->code = 0x80000003; c->rip--; rec->address = (void *)c->rip; break;
    }
    uint64_t call_sp = (sp - 64) & ~(uint64_t)15;
    *(uint64_t *)(call_sp - 8) = 0;                 // fake return address
    ss->__rsp = call_sp - 8;
    ss->__rdi = (uint64_t)rec;
    ss->__rsi = (uint64_t)c;
    ss->__rip = (uint64_t)signal_dispatch;
}

void vx_seh_install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
}

// ---- exports -----------------------------------------------------------------------------------------------------------------
void vx_RtlCaptureContext(CONTEXT *);
void vx_RaiseException(DWORD, DWORD, DWORD, const uint64_t *);
void vx_RtlUnwindEx(void);
void vx_RtlUnwind(void);
int  vx_setjmp(void *, void *);
void vx_longjmp(void *, int);

static const vx_export seh_k32[] = {
    { "RtlCaptureContext", (void *)vx_RtlCaptureContext }, { "RaiseException", (void *)vx_RaiseException },
    { "RtlUnwindEx", (void *)vx_RtlUnwindEx }, { "RtlUnwind", (void *)vx_RtlUnwind },
    VX_EXPORT(RtlLookupFunctionEntry), VX_EXPORT(RtlVirtualUnwind), VX_EXPORT(RtlPcToFileHeader),
    VX_EXPORT(AddVectoredExceptionHandler), VX_EXPORT(RemoveVectoredExceptionHandler),
    VX_EXPORT(SetUnhandledExceptionFilter), VX_EXPORT(UnhandledExceptionFilter),
};
static const vx_export seh_crt[] = {
    VX_EXPORT(__C_specific_handler),
    { "__intrinsic_setjmp", (void *)vx_setjmp }, { "_setjmp", (void *)vx_setjmp }, { "longjmp", (void *)vx_longjmp },
};

void vx_seh_init(void) {
    vx_register_module("kernel32.dll", seh_k32, sizeof seh_k32 / sizeof seh_k32[0]);
    vx_register_module("ntdll.dll", seh_k32, sizeof seh_k32 / sizeof seh_k32[0]);
    vx_register_module("ucrtbase.dll", seh_crt, sizeof seh_crt / sizeof seh_crt[0]);
    vx_register_module("msvcrt.dll", seh_crt, sizeof seh_crt / sizeof seh_crt[0]);
}
