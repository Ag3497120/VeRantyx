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
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <execinfo.h>
#include "vx.h"
#include <dlfcn.h>
#ifdef __aarch64__
#include "arm64.h"
#endif

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
#define STATUS_UNWIND_CONSOLIDATE 0x80000029u
enum { UNW_FLAG_EHANDLER = 1, UNW_FLAG_UHANDLER = 2, UNW_FLAG_CHAININFO = 4 };

void vx_restore_context(const CONTEXT *ctx) __attribute__((noreturn));

// ---- function lookup ------------------------------------------------------------------------------------------------
// Code generated at run time (JIT compilers such as V8) registers its unwind data in tables of its own:
// RtlAddFunctionTable, the growable tables of Windows 8, or a callback for a range.
typedef WINAPI RUNTIME_FUNCTION *(*table_callback)(uint64_t pc, void *context);
typedef struct dyn_table {
    RUNTIME_FUNCTION *entries; DWORD count, max; uint64_t base, lo, hi;   // code between lo and hi
    table_callback callback; void *context;
    struct dyn_table *next;
} dyn_table;
static dyn_table *g_tables;
static pthread_rwlock_t g_tables_lock = PTHREAD_RWLOCK_INITIALIZER;
static dyn_table *table_at(uint64_t pc) {           // with g_tables_lock held
    for (dyn_table *t = g_tables; t; t = t->next) if (pc >= t->lo && pc < t->hi) return t;
    return NULL;
}
static int pc_in_image(uint64_t pc) {
    if (vx_image_at(pc)) return 1;
    pthread_rwlock_rdlock(&g_tables_lock);
    int in = table_at(pc) != NULL;
    pthread_rwlock_unlock(&g_tables_lock);
    return in;
}
static RUNTIME_FUNCTION *search(RUNTIME_FUNCTION *table, size_t n, DWORD rva) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rva < table[mid].begin) hi = mid;
        else if (rva >= table[mid].end) lo = mid + 1;
        else return &table[mid];
    }
    return NULL;
}
static dyn_table *add_table(RUNTIME_FUNCTION *entries, DWORD count, DWORD max, uint64_t base, uint64_t lo, uint64_t hi) {
    dyn_table *t = calloc(1, sizeof *t);
    t->entries = entries; t->count = count; t->max = max; t->base = base; t->lo = lo; t->hi = hi;
    pthread_rwlock_wrlock(&g_tables_lock);
    t->next = g_tables; g_tables = t;
    pthread_rwlock_unlock(&g_tables_lock);
    return t;
}
static void remove_table(dyn_table *gone) {
    pthread_rwlock_wrlock(&g_tables_lock);
    for (dyn_table **pp = &g_tables; *pp; pp = &(*pp)->next) if (*pp == gone) { *pp = gone->next; free(gone); break; }
    pthread_rwlock_unlock(&g_tables_lock);
}
static WINAPI BOOL RtlAddFunctionTable(RUNTIME_FUNCTION *entries, DWORD count, uint64_t base) {
    DWORD lo = ~0u, hi = 0;
    for (DWORD i = 0; i < count; i++) { if (entries[i].begin < lo) lo = entries[i].begin; if (entries[i].end > hi) hi = entries[i].end; }
    if (count) add_table(entries, count, count, base, base + lo, base + hi);
    return 1;
}
static WINAPI BOOL RtlDeleteFunctionTable(RUNTIME_FUNCTION *entries) {
    pthread_rwlock_rdlock(&g_tables_lock);
    dyn_table *t = g_tables;
    while (t && t->entries != entries) t = t->next;
    pthread_rwlock_unlock(&g_tables_lock);
    if (t) remove_table(t);
    return t != NULL;
}
static WINAPI DWORD RtlAddGrowableFunctionTable(void **handle, RUNTIME_FUNCTION *entries, DWORD count, DWORD max, uint64_t lo, uint64_t hi) {
    *handle = add_table(entries, count, max, lo, lo, hi);
    return 0;
}
static WINAPI void RtlGrowFunctionTable(void *handle, DWORD count) {
    pthread_rwlock_wrlock(&g_tables_lock);
    dyn_table *t = handle;
    if (count <= t->max) t->count = count;
    pthread_rwlock_unlock(&g_tables_lock);
}
static WINAPI void RtlDeleteGrowableFunctionTable(void *handle) { remove_table(handle); }
// The identifier's low two bits are 3 for a callback table (Windows' convention); base..base+length is its code.
static WINAPI BOOL RtlInstallFunctionTableCallback(uint64_t id, uint64_t base, DWORD length, table_callback cb, void *context, const WCHAR *dll) {
    (void)dll;
    if ((id & 3) != 3) return 0;
    dyn_table *t = add_table(NULL, 0, 0, base, base, base + length);
    t->callback = cb; t->context = context;
    return 1;
}

static WINAPI RUNTIME_FUNCTION *RtlLookupFunctionEntry(uint64_t pc, uint64_t *base, void *history) {
    (void)history;
    const vx_image *m = vx_image_at(pc);
    if (!m) {
        pthread_rwlock_rdlock(&g_tables_lock);
        dyn_table *t = table_at(pc);
        RUNTIME_FUNCTION *fn = NULL;
        table_callback cb = NULL; void *ctx = NULL;
        if (t) {
            *base = t->base;
            if (t->callback) { cb = t->callback; ctx = t->context; }
            else fn = search(t->entries, t->count, (DWORD)(pc - t->base));
        }
        pthread_rwlock_unlock(&g_tables_lock);
        return cb ? cb(pc, ctx) : fn;
    }
    if (!m->pdata_size) return NULL;
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
// RtlVirtualUnwind: one frame back, from Microsoft's documented formats ("x64 exception handling", "x64 prolog and
// epilog"). The unwind codes describe the prolog; an epilog has none, so a pc inside one is recognised by its bytes.
typedef struct { uint8_t offset, op_info; } unwind_code;            // op: low 4 bits, its info: high 4
typedef struct {
    uint8_t version_flags, prolog_size, count, frame;      // frame: register (low 4 bits), offset / 16 (high 4)
    unwind_code code[];
} unwind_info;
enum { UWOP_PUSH_NONVOL, UWOP_ALLOC_LARGE, UWOP_ALLOC_SMALL, UWOP_SET_FPREG, UWOP_SAVE_NONVOL, UWOP_SAVE_NONVOL_FAR,
       UWOP_SAVE_XMM128 = 8, UWOP_SAVE_XMM128_FAR, UWOP_PUSH_MACHFRAME };

static uint64_t load64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static int64_t imm(const uint8_t *p, int wide) { int32_t v; if (!wide) return (int8_t)*p; memcpy(&v, p, 4); return v; }
// The 16- or 32-bit operand held in the slots after a code.
static uint32_t operand(const unwind_code *u, int wide) {
    uint32_t v = u[1].offset | u[1].op_info << 8;
    return wide ? v | (uint32_t)(u[2].offset | u[2].op_info << 8) << 16 : v;
}
static int code_slots(const unwind_code *u) {
    switch (u->op_info & 15) {
    case UWOP_ALLOC_LARGE: return u->op_info >> 4 ? 3 : 2;
    case UWOP_SAVE_NONVOL: case UWOP_SAVE_XMM128: return 2;
    case UWOP_SAVE_NONVOL_FAR: case UWOP_SAVE_XMM128_FAR: return 3;
    default: return 1;
    }
}
// What follows the codes (kept to an even number of slots): a chained RUNTIME_FUNCTION or the handler's RVA and data.
static const uint8_t *info_tail(const unwind_info *ui) { return (const uint8_t *)&ui->code[(ui->count + 1) & ~1]; }

// Undoes one prolog step. SAVE_* read relative to the frame (the frame register's value less its offset, or the
// stack pointer the walk started from).
static void undo_code(const unwind_code *u, CONTEXT *c, uint64_t frame, uint64_t *frame_ret, int *machframe) {
    uint64_t *g = c->gpr;
    int reg = u->op_info >> 4;
    switch (u->op_info & 15) {
    case UWOP_PUSH_NONVOL: g[reg] = load64(g[RSP]); g[RSP] += 8; break;
    case UWOP_ALLOC_LARGE: g[RSP] += reg ? operand(u, 1) : operand(u, 0) * 8; break;
    case UWOP_ALLOC_SMALL: g[RSP] += reg * 8 + 8; break;
    case UWOP_SET_FPREG: g[RSP] = *frame_ret = frame; break;
    case UWOP_SAVE_NONVOL: g[reg] = load64(frame + operand(u, 0) * 8); break;
    case UWOP_SAVE_NONVOL_FAR: g[reg] = load64(frame + operand(u, 1)); break;
    case UWOP_SAVE_XMM128: memcpy(&c->xmm[reg], (const void *)(uintptr_t)(frame + operand(u, 0) * 16), 16); break;
    case UWOP_SAVE_XMM128_FAR: memcpy(&c->xmm[reg], (const void *)(uintptr_t)(frame + operand(u, 1)), 16); break;
    case UWOP_PUSH_MACHFRAME:                                      // an interrupt's frame: rip, cs, eflags, rsp, ss
        if (reg) g[RSP] += 8;                                      // (above an error code)
        c->rip = load64(g[RSP]);
        g[RSP] = load64(g[RSP] + 24);
        *machframe = 1;
        break;
    }
}

// The rest of an epilog from pc: `add rsp, imm` or `lea rsp, [reg + disp]`, pops, then ret or a jump out of the
// function (a tail call, which leaves the return address on the stack like ret).
typedef struct {
    enum { ADJUST_NONE, ADJUST_ADD, ADJUST_LEA } adjust;
    int base, npops;                                               // lea's base register
    int64_t disp;                                                  // add's immediate, lea's displacement
    uint8_t pop[64];                                               // (no compiler pops more than the 16 registers)
} epilog;
static int decode_epilog(const uint8_t *p, uint64_t begin, uint64_t end, epilog *e) {
    e->adjust = ADJUST_NONE; e->npops = 0;
    if ((p[0] & 0xf8) == 0x48) {                                   // REX.W: perhaps the stack adjustment
        if (p[1] == 0x81 || p[1] == 0x83) {
            if (p[0] != 0x48 || p[2] != 0xc4) return 0;            // add to a register other than rsp
            e->adjust = ADJUST_ADD;
            e->disp = imm(p + 3, p[1] == 0x81);
            p += p[1] == 0x81 ? 7 : 4;
        } else if (p[1] == 0x8d) {
            int modrm = p[2], mod = modrm >> 6;
            if (p[0] & 6 || (modrm >> 3 & 7) != RSP || (modrm & 7) == 4 || mod == 0 || mod == 3) return 0;
            e->adjust = ADJUST_LEA;
            e->base = (modrm & 7) | (p[0] & 1) << 3;
            e->disp = imm(p + 3, mod == 2);
            p += mod == 2 ? 7 : 4;
        }
    }
    for (;;) {
        int rex = (*p & 0xf0) == 0x40 ? *p++ : 0;
        switch (*p) {
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
            if (e->npops == (int)sizeof e->pop) return 0;
            e->pop[e->npops++] = (*p++ - 0x58) | (rex & 1) << 3;
            continue;
        case 0xc3: case 0xc2: return 1;                            // ret (ret imm16's imm is not applied)
        case 0xf3: return p[1] == 0xc3;                            // rep ret
        case 0xff: return p[1] == 0x25;                            // jmp [rip + disp32]
        case 0xe9: case 0xeb: {                                    // jmp rel32 / rel8: an epilog if it leaves
            int wide = *p == 0xe9;
            uint64_t target = (uint64_t)(uintptr_t)(p + (wide ? 5 : 2)) + imm(p + 1, wide);
            return target < begin || target >= end;
        }
        default: return 0;
        }
    }
}
static void run_epilog(const epilog *e, CONTEXT *c) {
    uint64_t *g = c->gpr;
    if (e->adjust == ADJUST_ADD) g[RSP] += e->disp;
    else if (e->adjust == ADJUST_LEA) g[RSP] = g[e->base] + e->disp;
    for (int i = 0; i < e->npops; i++) { g[e->pop[i]] = load64(g[RSP]); g[RSP] += 8; }
    c->rip = load64(g[RSP]);
    g[RSP] += 8;
}

// Unwinds c one frame (its function's entry and the entries chained to it). Returns the language handler of the
// kind `type` asks for, with its data in *data, unless pc is in a prolog or epilog, where the function's handler
// does not apply yet or any more. *frame_ret: the establisher frame.
static WINAPI void *RtlVirtualUnwind(DWORD type, uint64_t base, uint64_t pc, RUNTIME_FUNCTION *fn, CONTEXT *c,
                                     void **data, uint64_t *frame_ret, void *ctx_pointers) {
    (void)ctx_pointers;
    uint64_t *g = c->gpr, frame = g[RSP];
    const unwind_info *ui;
    int machframe = 0, in_prolog;
    *frame_ret = frame;
    for (;;) {
        ui = (const unwind_info *)(uintptr_t)(base + fn->unwind);
        uint64_t begin = base + fn->begin;
        if (ui->frame & 15) frame = g[ui->frame & 15] - (ui->frame >> 4) * 16;
        in_prolog = pc >= begin && pc < begin + ui->prolog_size;
        epilog e;
        if (!in_prolog && decode_epilog((const uint8_t *)(uintptr_t)pc, begin, base + fn->end, &e)) {
            *frame_ret = frame;
            run_epilog(&e, c);
            return NULL;
        }
        // In the prolog, only the steps already taken (those whose instruction ends at or before pc) are undone.
        for (int i = 0; i < ui->count; i += code_slots(&ui->code[i]))
            if (!in_prolog || ui->code[i].offset <= pc - begin)
                undo_code(&ui->code[i], c, frame, frame_ret, &machframe);
        if (!(ui->version_flags >> 3 & UNW_FLAG_CHAININFO)) break;
        fn = (RUNTIME_FUNCTION *)info_tail(ui);
    }
    if (!machframe) { c->rip = load64(g[RSP]); g[RSP] += 8; }
    if (!(ui->version_flags >> 3 & type) || in_prolog) return NULL;
    uint32_t handler;
    memcpy(&handler, info_tail(ui), 4);
    *data = (void *)(info_tail(ui) + 4);
    return (void *)(uintptr_t)(base + handler);
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
    uint64_t at;                // where it sits on the program's stack (deeper entries: lower)
} vx_entry;
static __thread vx_entry *t_entries;
// x86 build: the entry itself is on the program's stack (the runtime runs on it too). ARM64: the runtime has a
// stack of its own, so just below the program's stack pointer at the entry.
#ifdef __aarch64__
#define ENTRY(next, ctx) { (next), (ctx), NULL, (ctx)->gpr[RSP] - 8 }
#define ENTRY_SET_AT(e)
#else
#define ENTRY(next, ctx) { (next), (ctx), NULL, 0 }
#define ENTRY_SET_AT(e) ((e).at = (uint64_t)&(e))
#endif

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
        *c = unwinding && e->unwinding_frame ? *e->unwinding_frame : *e->ctx;
        // Entries deeper on the stack than where the walk now is belong to frames already unwound (a catch
        // block runs on top of them): they are not part of this walk.
        e = e->next;
        while (e && e->at < c->gpr[RSP]) e = e->next;
        *cursor = e;
    }
    return NULL;
}

// Resumes the program at ctx. Runtime entries deeper than the target stack are finished.
static void __attribute__((noreturn)) resume(CONTEXT *ctx) {
    while (t_entries && t_entries->at < ctx->gpr[RSP]) t_entries = t_entries->next;
    vx_restore_context(ctx);
}

static int seh_trace(void) {                       // VXRT_TRACE_SEH=1: every raise, and what runs when nothing handles one
    static int on = -1;
    if (on < 0) { const char *e = getenv("VXRT_TRACE_SEH"); on = e && *e == '1'; }
    return on;
}
static void __attribute__((noreturn)) dispatch(EXCEPTION_RECORD *rec, CONTEXT *ctx) {
    vx_entry self = ENTRY(t_entries, ctx);
    ENTRY_SET_AT(self);
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
    if (g_unhandled_filter) {
        // VXRT_TRACE_SEH=1: the program's unhandled-exception filter is about to run (it may write a dump and wait)
        if (seh_trace()) {
            const vx_image *m = vx_image_at(ctx->rip);
            vx_log("SEH: no handler for %#x raised at %s+%#llx: the program's unhandled filter runs", rec->code,
                   m ? m->name : "?", m ? (unsigned long long)(ctx->rip - (uint64_t)(uintptr_t)m->base) : (unsigned long long)ctx->rip);
            CONTEXT walk = *ctx; vx_entry *at = self.next;
            for (int depth = 0; depth < 12; depth++) {
                const vx_image *f = vx_image_at(walk.rip);
                vx_log("   #%d %s+%#llx", depth, f ? f->name : "?", f ? (unsigned long long)(walk.rip - (uint64_t)(uintptr_t)f->base) : (unsigned long long)walk.rip);
                uint64_t base; RUNTIME_FUNCTION *fn = next_function(&walk, &at, 0, &base);
                if (!fn) break;
                void *data = NULL; uint64_t frame = 0;
                RtlVirtualUnwind(0, base, walk.rip, fn, &walk, &data, &frame, NULL);
                if (!walk.rip) break;
            }
        }
        if (g_unhandled_filter(&ep) == -1) resume(ctx);
    }
    vx_log("unhandled exception %#x at %p", rec->code, rec->address);
    CONTEXT walk = *ctx;                          // the program's frames, unwound
    vx_entry *at = self.next;
    for (int depth = 0; depth < 20; depth++) {
        const vx_image *m = vx_image_at(walk.rip);
        vx_log("   #%d %s+%#llx", depth, m ? m->name : "?", m ? (unsigned long long)(walk.rip - (uint64_t)(uintptr_t)m->base) : (unsigned long long)walk.rip);
        uint64_t base;
        RUNTIME_FUNCTION *fn = next_function(&walk, &at, 0, &base);
        if (!fn) break;
        void *data = NULL; uint64_t frame = 0;
        RtlVirtualUnwind(0, base, walk.rip, fn, &walk, &data, &frame, NULL);
        if (!walk.rip) break;
    }
    vx_jobs_process_exit();
    fflush(stdout);
    _exit((int)rec->code);
}

// Called by RaiseException (context.S) with the caller's context.
void __attribute__((noreturn)) vx_raise(DWORD code, DWORD flags, DWORD n, const uint64_t *args, CONTEXT *ctx) {
    EXCEPTION_RECORD rec = { code, flags & EXCEPTION_NONCONTINUABLE, NULL, (void *)ctx->rip, n > 15 ? 15 : n, 0, {0} };
    if (args) memcpy(rec.info, args, rec.nparams * 8);
    if (seh_trace() || (vx_trace && code != 0xe06d7363 && code != 0x406d1388)) {   // (vx_trace: not C++ exceptions or thread names)
        const vx_image *m = vx_image_at(ctx->rip);
        vx_log("RaiseException %#x from %s+%#llx", code, m ? m->name : "?", m ? (unsigned long long)(ctx->rip - (uint64_t)m->base) : (unsigned long long)ctx->rip);
    }
    dispatch(&rec, ctx);
}

// Called by RtlUnwindEx/RtlUnwind (context.S) with the caller's context. Unwinds frame by frame, calling
// termination handlers, until the frame `target_frame`, then resumes it at target_ip with rax = retval.
// The caller's CONTEXT argument is not used as scratch space: it may be the record of an exception still
// being dispatched.
void __attribute__((noreturn)) vx_unwind(uint64_t target_frame, uint64_t target_ip, EXCEPTION_RECORD *rec,
                                         uint64_t retval, CONTEXT *unused, CONTEXT *captured) {
    (void)unused;
    vx_entry self = ENTRY(t_entries, captured);
    ENTRY_SET_AT(self);
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
            if (rec->code == STATUS_UNWIND_CONSOLIDATE && rec->nparams >= 1) {
                // The frames below the target are unwound but their stack is still intact: run the callback
                // (MSVC's runtime calls a catch block here) on it, as if called from the target frame, and
                // continue the target where the callback says.
                vx_entry consolidate = ENTRY(t_entries, &out);
                ENTRY_SET_AT(consolidate);
                t_entries = &consolidate;
                out.rip = (uint64_t)((void *(WINAPI *)(EXCEPTION_RECORD *))rec->info[0])(rec);
                t_entries = consolidate.next;
            }
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
// (the Windows entry: called with its convention, as a program would; a plain C call put the arguments elsewhere)
void WINAPI vx_rtl_unwind_ex(uint64_t, uint64_t, EXCEPTION_RECORD *, uint64_t, CONTEXT *, void *) __asm__("_vx_RtlUnwindEx");

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
#ifndef __aarch64__
static void signal_dispatch(EXCEPTION_RECORD *rec, CONTEXT *ctx) { dispatch(rec, ctx); }
#endif

// Signals are handled on a stack of their own, so that a thread that ran out of stack can still be reported.
void vx_signal_stack(void) {
    stack_t st = { .ss_size = 128 << 10, .ss_flags = 0 };
    st.ss_sp = mmap(NULL, st.ss_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (st.ss_sp != MAP_FAILED) sigaltstack(&st, NULL);
}

#ifndef __aarch64__
static void on_signal(int sig, siginfo_t *si, void *ucv) {
    ucontext_t *uc = ucv;
    _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    // Out of stack: the fault is just below the thread's stack (its guard page). Windows would raise
    // STATUS_STACK_OVERFLOW on what is left of the stack; here it is reported, with where it happened.
    uint8_t *teb;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(teb));          // the TEB's own slot (none on macOS's threads)
    if (teb && (sig == SIGSEGV || sig == SIGBUS)) {
        uint64_t limit = *(uint64_t *)(teb + TEB_STACK_LIMIT), a = (uint64_t)si->si_addr;
        if (a < limit && a + (64 << 10) >= limit) {
            const vx_image *m = vx_image_at(ss->__rip);
            Dl_info info;
            const char *sym = !m && dladdr((void *)ss->__rip, &info) && info.dli_sname ? info.dli_sname : NULL;
            vx_log("stack overflow on thread %u (stack %#llx-%#llx) at %s+%#llx%s%s, fault address %#llx", (unsigned)vx_current_tid(), (unsigned long long)limit,
                   *(unsigned long long *)(teb + TEB_STACK_BASE), m ? m->name : "?", m ? (unsigned long long)(ss->__rip - (uint64_t)m->base) : (unsigned long long)ss->__rip,
                   sym ? " in " : "", sym ? sym : "", (unsigned long long)a);
            // The program's frames nearest the fault: return addresses on the stack inside its images.
            const uint64_t *sp = (const uint64_t *)ss->__rsp;
            for (int i = 0, shown = 0; i < 16384 && shown < 40 && (uint64_t)(sp + i) < *(uint64_t *)(teb + TEB_STACK_BASE); i++) {
                const vx_image *f = vx_image_at(sp[i]);
                const uint8_t *r = (const uint8_t *)sp[i];         // (a return address follows a call instruction)
                int call = f && sp[i] - 6 > (uint64_t)f->base && (r[-5] == 0xe8 || (r[-6] == 0xff && (r[-5] & 0x38) == 0x10) ||
                                                                  (r[-2] == 0xff && (r[-1] & 0x38) == 0x10) || (r[-3] == 0xff && (r[-2] & 0x38) == 0x10));
                if (call) { vx_log("  stack: %s+%#llx", f->name, (unsigned long long)(sp[i] - (uint64_t)f->base)); shown++; }
            }
            _exit(128 + sig);
        }
    }
    // The program calling a bad address (a null function pointer): the fault is the program's, at the call. Its
    // caller is the frame to unwind from, as Windows treats the unknown address as a leaf function.
    uint64_t ret = 0;
    int bad_call = !pc_in_image(ss->__rip) && (sig == SIGSEGV || sig == SIGBUS) && (ss->__rip < 4096 || (uint64_t)si->si_addr == ss->__rip) &&
                   ss->__rsp && pc_in_image(ret = *(uint64_t *)ss->__rsp);
    if (bad_call) vx_log("the program called %#llx from %#llx", (unsigned long long)ss->__rip, (unsigned long long)ret);
    if (!pc_in_image(ss->__rip) && !bad_call) {
        // A fault in this runtime or in macOS code is a bug here, not the program's exception.
        // Print where (this runtime's frames) and exit at once rather than waiting for a macOS crash report.
        char tname[64] = "";
        pthread_getname_np(pthread_self(), tname, sizeof tname);
        uint8_t *top = pthread_get_stackaddr_np(pthread_self());
        vx_log("signal %d at %#llx (address %p) outside the program, thread %u '%s' (stack %p-%p, TEB %p); native backtrace:", sig,
               (unsigned long long)ss->__rip, si->si_addr, (unsigned)vx_current_tid(), tname, (void *)(top - pthread_get_stacksize_np(pthread_self())), (void *)top, (void *)teb);
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
    if (bad_call) { c->rip = ret; c->gpr[4] += 8; }             // unwound past the leaf "function" at the bad address
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
    case SIGTRAP:                                   // int3 (a trap): the saved Rip is already past the int3, where a
        rec->code = 0x80000003;                     // handler that continues without touching it resumes, as on
        rec->address = (void *)(c->rip - 1);        // Windows; only the reported address is the int3's own
        break;
    }
    uint64_t call_sp = (sp - 64) & ~(uint64_t)15;
    *(uint64_t *)(call_sp - 8) = 0;                 // fake return address
    ss->__rsp = call_sp - 8;
    ss->__rdi = (uint64_t)rec;
    ss->__rsi = (uint64_t)c;
    ss->__rip = (uint64_t)signal_dispatch;
}

// A Windows CONTEXT (all of it) from a signal's machine context: GetThreadContext of a thread SuspendThread stopped.
void vx_context_from_signal(void *out, const void *ucv) {
    const ucontext_t *uc = ucv;
    const _STRUCT_X86_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    CONTEXT *c = out;
    DWORD flags = c->flags;
    memset(c, 0, sizeof *c);
    c->flags = flags ? flags : 0x10001f;
    uint64_t regs[16] = { ss->__rax, ss->__rcx, ss->__rdx, ss->__rbx, ss->__rsp, ss->__rbp, ss->__rsi, ss->__rdi,
                          ss->__r8, ss->__r9, ss->__r10, ss->__r11, ss->__r12, ss->__r13, ss->__r14, ss->__r15 };
    memcpy(c->gpr, regs, sizeof regs);
    c->rip = ss->__rip;
    c->eflags = (DWORD)ss->__rflags;
    c->cs = 0x33; c->ss = 0x2b; c->ds = c->es = 0x2b; c->fs = 0x53; c->gs = 0x2b;
    c->mxcsr = uc->uc_mcontext->__fs.__fpu_mxcsr;
    memcpy(c->xmm, &uc->uc_mcontext->__fs.__fpu_xmm0, 16 * 16);
}

#else
// ARM64: the thread's x86 state where it stopped (src/arm64.c), as a Windows CONTEXT.
int vx_arm64_state_at(const void *uc, void *x86_state);
static void context_from(CONTEXT *c, const x86_ctx *x, uint64_t rip, uint64_t rsp);
void vx_context_from_signal(void *out, const void *ucv) {
    static __thread x86_ctx *x;
    if (!x) x = calloc(1, sizeof *x);
    CONTEXT *c = out;
    DWORD flags = c->flags;
    if (!vx_arm64_state_at(ucv, x)) { memset(c, 0, sizeof *c); c->flags = flags ? flags : 0x10001f; return; }
    context_from(c, x, x->rip, x->gpr[RSP]);
    if (flags) c->flags = flags;
}
// ... from an x86 state (a thread SuspendThread stopped in the vCPU guest: src/arm64.c).
void vx_context_from_state(void *out, const void *state) {
    const x86_ctx *x = state;
    CONTEXT *c = out;
    DWORD flags = c->flags;
    context_from(c, x, x->rip, x->gpr[RSP]);
    if (flags) c->flags = flags;
}
#endif

// abort() from macOS code (malloc's checks, libc++): where from, with the program's frames named (VXRT_TRACE).
static void on_abort(int sig) {
    void *frames[48];
    int n = backtrace(frames, 48);
    vx_log("abort: backtrace");
    backtrace_symbols_fd(frames, n, 2);
    for (int i = 0; i < n; i++) {
        const vx_image *m = vx_image_at((uint64_t)frames[i]);
        if (m) vx_log("   %s+%#llx", m->name, (unsigned long long)((uint8_t *)frames[i] - m->base));
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void vx_seh_install_signals(void) {
#ifndef __aarch64__                             // (ARM64: src/arm64.c's handler)
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
#endif
    signal(SIGABRT, on_abort);                  // (a native abort says where it came from)
}

// ---- exports -----------------------------------------------------------------------------------------------------------------
void vx_RtlCaptureContext(CONTEXT *);
void vx_RaiseException(DWORD, DWORD, DWORD, const uint64_t *);
#ifdef __aarch64__
void vx_RtlUnwindEx(uint64_t, uint64_t, EXCEPTION_RECORD *, uint64_t, CONTEXT *, void *);
void vx_RtlUnwind(uint64_t, uint64_t, EXCEPTION_RECORD *, uint64_t);
#else
void vx_RtlUnwindEx(void);
void vx_RtlUnwind(void);
#endif
int  vx_setjmp(void *, void *);
void vx_longjmp(void *, int);

static const vx_export seh_k32[] = {
    { "RtlCaptureContext", (void *)vx_RtlCaptureContext }, { "RaiseException", (void *)vx_RaiseException },
    { "RtlUnwindEx", (void *)vx_RtlUnwindEx }, { "RtlUnwind", (void *)vx_RtlUnwind },
    VX_EXPORT(RtlLookupFunctionEntry), VX_EXPORT(RtlVirtualUnwind), VX_EXPORT(RtlPcToFileHeader),
    VX_EXPORT(RtlAddFunctionTable), VX_EXPORT(RtlDeleteFunctionTable), VX_EXPORT(RtlAddGrowableFunctionTable),
    VX_EXPORT(RtlGrowFunctionTable), VX_EXPORT(RtlDeleteGrowableFunctionTable), VX_EXPORT(RtlInstallFunctionTableCallback),
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

#ifdef __aarch64__
// ---- ARM64: the program's state is the translator's (src/arm64.c) --------------------------------------------------
// A CONTEXT from the x86 registers, at rip / rsp.
static void context_from(CONTEXT *c, const x86_ctx *x, uint64_t rip, uint64_t rsp) {
    memset(c, 0, sizeof *c);
    c->flags = 0x10001f;
    c->mxcsr = x->mxcsr;
    c->cs = 0x33; c->ss = 0x2b; c->ds = c->es = 0x2b; c->fs = 0x53; c->gs = 0x2b;
    c->eflags = (DWORD)x->flags;
    memcpy(c->gpr, x->gpr, sizeof c->gpr);
    c->gpr[RSP] = rsp;
    c->rip = rip;
    memcpy(c->fltsave_head, &x->fcw, 2);                          // XSAVE_FORMAT: ControlWord, then MxCsr at 24
    memcpy(c->fltsave_head + 24, &x->mxcsr, 4);
    memcpy(c->xmm, x->xmm, sizeof c->xmm);
}
// As the program's caller of the runtime function now running sees it: just returned from the call.
static void context_at_return(CONTEXT *c, const x86_ctx *x) {
    uint64_t rsp = x->gpr[4];
    context_from(c, x, *(uint64_t *)(uintptr_t)rsp, rsp + 8);
}
void vx_arm64_resume(const uint64_t *gpr, uint64_t rip, uint64_t eflags, uint32_t mxcsr, const void *xmm) __attribute__((noreturn));
void vx_restore_context(const CONTEXT *c) {
    vx_arm64_resume(c->gpr, c->rip, c->eflags, c->mxcsr, c->xmm);
}

// The program's calls (thunks reading the x86 state) and this runtime's own (a context starting in native code:
// the walk goes on at the entries).
static void th_RtlCaptureContext(x86_ctx *x) { context_at_return((CONTEXT *)(uintptr_t)x->gpr[1], x); }
static void th_RaiseException(x86_ctx *x) {
    CONTEXT c; context_at_return(&c, x);
    vx_raise((DWORD)x->gpr[1], (DWORD)x->gpr[2], (DWORD)x->gpr[8], (const uint64_t *)(uintptr_t)x->gpr[9], &c);
}
void __attribute__((noreturn)) vx_cxx_throw(void *obj, const void *ti, void *ctx);
static void th_CxxThrowException(x86_ctx *x) {
    CONTEXT c; context_at_return(&c, x);
    vx_cxx_throw((void *)(uintptr_t)x->gpr[1], (const void *)(uintptr_t)x->gpr[2], &c);
}
static void th_RtlUnwindEx(x86_ctx *x) {
    CONTEXT c; context_at_return(&c, x);
    uint64_t *stack = (uint64_t *)(uintptr_t)x->gpr[4];
    vx_unwind(x->gpr[1], x->gpr[2], (EXCEPTION_RECORD *)(uintptr_t)x->gpr[8], x->gpr[9], (CONTEXT *)(uintptr_t)stack[5], &c);
}
static void th_RtlUnwind(x86_ctx *x) {
    CONTEXT c; context_at_return(&c, x);
    vx_unwind(x->gpr[1], x->gpr[2], (EXCEPTION_RECORD *)(uintptr_t)x->gpr[8], x->gpr[9], NULL, &c);
}
static void native_context(CONTEXT *c) {                          // (rip 0: no unwind data, so the entries)
    memset(c, 0, sizeof *c);
    c->gpr[RSP] = vx_x86_ctx()->gpr[4] - 8;
}
void vx_RtlCaptureContext(CONTEXT *c) { native_context(c); }
void vx_RaiseException(DWORD code, DWORD flags, DWORD n, const uint64_t *args) { CONTEXT c; native_context(&c); vx_raise(code, flags, n, args, &c); }
void vx_RtlUnwindEx(uint64_t frame, uint64_t ip, EXCEPTION_RECORD *rec, uint64_t ret, CONTEXT *ctx, void *history) {
    (void)history; CONTEXT c; native_context(&c); vx_unwind(frame, ip, rec, ret, ctx, &c);
}
void vx_RtlUnwind(uint64_t frame, uint64_t ip, EXCEPTION_RECORD *rec, uint64_t ret) { CONTEXT c; native_context(&c); vx_unwind(frame, ip, rec, ret, NULL, &c); }
void vx_CxxThrowException(void *obj, const void *ti) { CONTEXT c; native_context(&c); vx_cxx_throw(obj, ti, &c); }
static const vx_thunk seh_special[] = {
    { (const void *)vx_RtlCaptureContext, th_RtlCaptureContext }, { (const void *)vx_RaiseException, th_RaiseException },
    { (const void *)vx_RtlUnwindEx, th_RtlUnwindEx }, { (const void *)vx_RtlUnwind, th_RtlUnwind },
    { (const void *)vx_CxxThrowException, th_CxxThrowException },
};
__attribute__((constructor)) static void seh_special_add(void) { vx_thunks_add(seh_special, sizeof seh_special / sizeof *seh_special); }

// An exception of the program's code where translated code stopped (src/arm64.c): RIP at the instruction.
void __attribute__((noreturn)) vx_seh_fault(DWORD code, DWORD n, const uint64_t *params) {
    x86_ctx *x = vx_x86_ctx();
    // int3 (STATUS_BREAKPOINT) is a trap: the context resumes after the 0xCC (where the engine left Rip at it), so a
    // handler that continues without touching Rip runs on past the breakpoint; only the reported address is the int3.
    uint64_t rip = code == 0x80000003 ? x->rip + 1 : x->rip;
    CONTEXT c; context_from(&c, x, rip, x->gpr[4]);
    EXCEPTION_RECORD rec = { code, 0, NULL, (void *)(uintptr_t)x->rip, n, 0, {0} };
    if (params) memcpy(rec.info, params, n * 8);
    dispatch(&rec, &c);
}

// A call from this runtime into the program's code while the program is in a call into the runtime: the walk over
// the runtime's frames goes on where the program called it (as just returned from that call).
void *vx_seh_level_begin(const x86_ctx *outer) {
    struct { vx_entry e; CONTEXT c; } *l = malloc(sizeof *l);
    context_at_return(&l->c, outer);
    l->e = (vx_entry)ENTRY(t_entries, &l->c);
    t_entries = &l->e;
    return l;
}
void vx_seh_level_end(void *level) {
    vx_entry *e = level;
    if (t_entries == e) t_entries = e->next;
    free(level);
}
#endif

#ifdef __aarch64__
#include "thunks/seh.c.inc"
#endif
