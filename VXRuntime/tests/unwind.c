// RtlVirtualUnwind on hand-made unwind data: nothing here is executed. The "image" is a byte array holding function
// bytes, UNWIND_INFOs and RUNTIME_FUNCTIONs; the "stack" is an array of known values. Each case unwinds one frame
// and compares every register, Rip, Xmm0-15, the returned handler, *HandlerData and *EstablisherFrame.
#include <windows.h>
#include <stdio.h>
#include <string.h>

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { PUSH_NONVOL, ALLOC_LARGE, ALLOC_SMALL, SET_FPREG, SAVE_NONVOL, SAVE_NONVOL_FAR, OP6, OP7, SAVE_XMM128,
       SAVE_XMM128_FAR, PUSH_MACHFRAME };
#define CODE(off, op, info) (WORD)((off) | (op) << 8 | (info) << 12)
#define HANDLER 0x7f00                              // a handler RVA (never called)

static BYTE img[0x8000] __attribute__((aligned(4096)));
static DWORD64 stk[0x4000];                         // large: the ALLOC_LARGE / _FAR cases use offsets past 64 KB
static DWORD next_rva = 0x10;
static DWORD tail;                                  // RVA just after the last info's (even-padded) codes
static int cases, failed;

static DWORD64 S(int i) { return stk[i]; }
static DWORD64 A(int i) { return (DWORD64)&stk[i]; }
static DWORD64 *G(CONTEXT *c) { return &c->Rax; }
static M128A *X(CONTEXT *c) { return c->FltSave.XmmRegisters; }
static DWORD64 at(DWORD rva) { return (DWORD64)img + rva; }

static DWORD alloc(DWORD n) { DWORD r = next_rva; next_rva = (next_rva + n + 15) & ~15u; return r; }
// Function bytes at begin+offset, in a function [begin, begin+size).
static RUNTIME_FUNCTION func(DWORD size, DWORD offset, const BYTE *code, int n, DWORD unwind) {
    DWORD b = alloc(size);
    memset(img + b, 0x90, size);
    if (n) memcpy(img + b + offset, code, n);
    return (RUNTIME_FUNCTION){ .BeginAddress = b, .EndAddress = b + size, .UnwindData = unwind };
}
// An UNWIND_INFO; after the codes, a chained entry (flags 4) or a handler RVA and 4 bytes of handler data.
static DWORD info(int flags, int prolog, int fr, int froff, int n, const WORD *codes, const RUNTIME_FUNCTION *chain) {
    DWORD r = alloc(4 + 2 * (n + 1) + 16);
    img[r] = 1 | flags << 3; img[r + 1] = prolog; img[r + 2] = n; img[r + 3] = fr | froff << 4;
    memcpy(img + r + 4, codes, 2 * n);
    tail = r + 4 + 2 * ((n + 1) & ~1);
    if (flags & UNW_FLAG_CHAININFO) memcpy(img + tail, chain, sizeof *chain);
    else { DWORD h = HANDLER; memcpy(img + tail, &h, 4); memcpy(img + tail + 4, "data", 4); }
    return r;
}

static void init(CONTEXT *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 16; i++) G(c)[i] = 0xC0DE000000000000ull | (DWORD64)i << 8;
    for (int i = 0; i < 16; i++) { X(c)[i].Low = 0xEEEE0000ull + i; X(c)[i].High = ~(0xEEEE0000ll + i); }
    c->Rsp = A(4);
    c->Rbp = A(20);
    c->Rbx = A(10);
    c->R13 = A(30);
}

static void *unwind(DWORD type, DWORD64 pc, RUNTIME_FUNCTION *fn, CONTEXT *c, void **data, DWORD64 *frame) {
    *data = (void *)0xDA7A; *frame = 0xF8A3E;
    c->Rip = pc;
    return (void *)RtlVirtualUnwind(type, (DWORD64)img, pc, fn, c, data, frame, NULL);
}

static void check(const char *name, CONTEXT *got, CONTEXT *want, void *h, void *want_h, void *data, void *want_data,
                  DWORD64 frame, DWORD64 want_frame) {
    static const char *reg[] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11",
                                 "r12", "r13", "r14", "r15", "rip" };
    int bad = 0;
    cases++;
    for (int i = 0; i < 17; i++)                                    // (Rip follows R15)
        if (G(got)[i] != G(want)[i])
            bad = printf("unwind %s: %s %#llx, expected %#llx\n", name, reg[i], G(got)[i], G(want)[i]);
    for (int i = 0; i < 16; i++)
        if (memcmp(&X(got)[i], &X(want)[i], 16)) bad = printf("unwind %s: xmm%d differs\n", name, i);
    if (h != want_h) bad = printf("unwind %s: handler %p, expected %p\n", name, h, want_h);
    if (data != want_data) bad = printf("unwind %s: handler data %p, expected %p\n", name, data, want_data);
    if (frame != want_frame) bad = printf("unwind %s: frame %#llx, expected %#llx\n", name, frame, want_frame);
    if (bad) failed++;
}

// ---- epilogs ----
// The function's unwind codes (add rsp 8; push rbx) give a different result from any epilog below, and its handler
// shows the codes were used.
static const WORD body_codes[] = { CODE(0, ALLOC_SMALL, 0), CODE(0, PUSH_NONVOL, RBX) };
static DWORD body_info, body_tail, body_fp_info;
static void body_result(CONTEXT *w) { w->Rsp = A(7); w->Rbx = S(5); w->Rip = S(6); }

typedef void (*expect_fn)(CONTEXT *);
// An epilog (or not) at function+0x10+pc_off, in a function of 0x40 bytes.
static void epilog_case(const char *name, const BYTE *code, int n, int pc_off, int is_epilog, expect_fn expect) {
    RUNTIME_FUNCTION fn = func(0x40, 0x10, code, n, body_info);
    CONTEXT c, w; init(&c); init(&w);
    void *data; DWORD64 frame;
    void *h = unwind(UNW_FLAG_EHANDLER, at(fn.BeginAddress + 0x10 + pc_off), &fn, &c, &data, &frame);
    if (is_epilog) { expect(&w); check(name, &c, &w, h, NULL, data, (void *)0xDA7A, frame, A(4)); }
    else { body_result(&w); check(name, &c, &w, h, (void *)at(HANDLER), data, (void *)at(body_tail + 4), frame, A(4)); }
}
#define EPILOG(name, pc_off, ...) \
    do { static const BYTE b[] = { __VA_ARGS__ }; epilog_case(#name, b, sizeof b, pc_off, 1, e_##name); } while (0)
#define NOT_EPILOG(name, ...) \
    do { static const BYTE b[] = { __VA_ARGS__ }; epilog_case(#name, b, sizeof b, 0, 0, NULL); } while (0)

static void e_add8(CONTEXT *w) { w->Rbx = S(9); w->Rbp = S(10); w->Rip = S(11); w->Rsp = A(12); }
static void e_add8_negative(CONTEXT *w) { w->Rbx = S(3); w->Rip = S(4); w->Rsp = A(5); }
static void e_add32(CONTEXT *w) { w->R14 = S(37); w->Rdi = S(38); w->Rip = S(39); w->Rsp = A(40); }
static void e_add32_negative(CONTEXT *w) { w->Rip = S(2); w->Rsp = A(3); }
static void e_lea_rbx_d8(CONTEXT *w) { w->Rbx = S(11); w->Rip = S(12); w->Rsp = A(13); }
static void e_lea_r13_d32(CONTEXT *w) { w->Rip = S(26); w->Rsp = A(27); }
static void e_pops_ret_imm(CONTEXT *w) { w->R12 = S(4); w->R13 = S(5); w->Rsi = S(6); w->Rip = S(7); w->Rsp = A(8); }
static void e_rex_w_pop(CONTEXT *w) { w->Rbx = S(4); w->Rip = S(5); w->Rsp = A(6); }
static void e_ret(CONTEXT *w) { w->Rip = S(4); w->Rsp = A(5); }
static void e_rex_ret(CONTEXT *w) { e_ret(w); }
static void e_rep_ret(CONTEXT *w) { e_ret(w); }
static void e_jmp32_out(CONTEXT *w) { w->Rbx = S(4); w->Rip = S(5); w->Rsp = A(6); }
static void e_jmp8_before(CONTEXT *w) { e_ret(w); }
static void e_jmp8_to_end(CONTEXT *w) { e_ret(w); }
static void e_jmp_rip(CONTEXT *w) { w->Rbp = S(4); w->Rip = S(5); w->Rsp = A(6); }

static void epilogs(void) {
    body_info = info(UNW_FLAG_EHANDLER, 0, 0, 0, 2, body_codes, NULL); body_tail = tail;
    body_fp_info = info(UNW_FLAG_EHANDLER, 0, RBP, 1, 2, body_codes, NULL);
    EPILOG(add8, 0, 0x48, 0x83, 0xC4, 0x28, 0x5B, 0x5D, 0xC3);                      // add rsp,28h; pop rbx; pop rbp
    EPILOG(add8_negative, 0, 0x48, 0x83, 0xC4, 0xF8, 0x5B, 0xC3);                   // add rsp,-8 (sign-extended)
    EPILOG(add32, 0, 0x48, 0x81, 0xC4, 0x08, 0x01, 0, 0, 0x41, 0x5E, 0x5F, 0xC3);   // add rsp,108h; pop r14; pop rdi
    EPILOG(add32_negative, 0, 0x48, 0x81, 0xC4, 0xF0, 0xFF, 0xFF, 0xFF, 0xC3);
    EPILOG(lea_rbx_d8, 0, 0x48, 0x8D, 0x63, 0x08, 0x5B, 0xC3);                      // lea rsp,[rbx+8]; pop rbx
    EPILOG(lea_r13_d32, 0, 0x49, 0x8D, 0xA5, 0xE0, 0xFF, 0xFF, 0xFF, 0xF3, 0xC3);   // lea rsp,[r13-20h]; rep ret
    // from the pops: pop r12; pop r13; pop rsi; ret 8
    EPILOG(pops_ret_imm, 4, 0x48, 0x83, 0xC4, 0x28, 0x41, 0x5C, 0x41, 0x5D, 0x5E, 0xC2, 0x08, 0x00);
    EPILOG(rex_w_pop, 0, 0x48, 0x5B, 0xC3);                                         // 48 starts no add/lea: a REX
    EPILOG(ret, 0, 0xC3);
    EPILOG(rex_ret, 0, 0x48, 0xC3);
    EPILOG(rep_ret, 0, 0xF3, 0xC3);
    EPILOG(jmp32_out, 0, 0x5B, 0xE9, 0x2A, 0, 0, 0);                                // to begin+40h: the function's end
    EPILOG(jmp8_before, 0, 0xEB, 0xED);                                             // to begin-1
    EPILOG(jmp8_to_end, 0, 0xEB, 0x2E);                                             // to begin+40h
    EPILOG(jmp_rip, 0, 0x5D, 0xFF, 0x25, 0, 0, 0, 0);                               // pop rbp; jmp [rip+0]

    // lea rsp,[rbp-10h] in a function with a frame register: the establisher frame is rbp - 16
    static const BYTE lea_rbp[] = { 0x48, 0x8D, 0x65, 0xF0, 0x5D, 0xC3 };
    RUNTIME_FUNCTION fn = func(0x40, 0x10, lea_rbp, sizeof lea_rbp, body_fp_info);
    CONTEXT c, w; init(&c); init(&w);
    void *data; DWORD64 frame;
    void *h = unwind(UNW_FLAG_EHANDLER, at(fn.BeginAddress + 0x10), &fn, &c, &data, &frame);
    w.Rbp = S(18); w.Rip = S(19); w.Rsp = A(20);
    check("lea_rbp_d8", &c, &w, h, NULL, data, (void *)0xDA7A, frame, A(18));

    NOT_EPILOG(jmp32_inside, 0xE9, 0, 0, 0, 0);                                     // to begin+15h
    NOT_EPILOG(jmp8_to_begin, 0xEB, 0xEE);
    NOT_EPILOG(jmp8_to_last_byte, 0xEB, 0x2D);
    NOT_EPILOG(call_rip, 0xFF, 0x15, 0, 0, 0, 0);
    NOT_EPILOG(rep_nop, 0xF3, 0x90);
    NOT_EPILOG(add_rax, 0x48, 0x83, 0xC0, 0x08, 0xC3);
    NOT_EPILOG(add_r12_imm8, 0x49, 0x83, 0xC4, 0x08, 0xC3);
    NOT_EPILOG(add_r12_imm32, 0x49, 0x81, 0xC4, 0x08, 0, 0, 0, 0xC3);
    NOT_EPILOG(add_rax_imm32, 0x48, 0x81, 0xC0, 0x08, 0, 0, 0, 0xC3);
    NOT_EPILOG(lea_rex_r, 0x4C, 0x8D, 0x65, 0xF0, 0xC3);
    NOT_EPILOG(lea_rex_x, 0x4A, 0x8D, 0x65, 0xF0, 0xC3);
    NOT_EPILOG(lea_sib, 0x48, 0x8D, 0x64, 0x24, 0x08, 0xC3);
    NOT_EPILOG(lea_rip, 0x48, 0x8D, 0x25, 0, 0, 0, 0, 0xC3);
    NOT_EPILOG(lea_mod3, 0x48, 0x8D, 0xE5, 0xC3);
    NOT_EPILOG(lea_rbp_not_rsp, 0x48, 0x8D, 0x6D, 0x08, 0xC3);
    NOT_EPILOG(mov_rbp_rsp, 0x48, 0x89, 0xE5, 0xC3);
    NOT_EPILOG(pop_nop, 0x5B, 0x90, 0xC3);
    NOT_EPILOG(two_rex, 0x41, 0x41, 0x5B, 0xC3);
}

// ---- prologs ----
// push rbp; push rbx; sub rsp,28h; lea rbp,[rsp+20h]; then the body (nops)
static void prologs(void) {
    static const BYTE code[] = { 0x55, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x8D, 0x6C, 0x24, 0x20 };
    static const WORD codes[] = { CODE(11, SET_FPREG, 0), CODE(6, ALLOC_SMALL, 4), CODE(2, PUSH_NONVOL, RBX),
                                  CODE(1, PUSH_NONVOL, RBP) };
    DWORD ui = info(UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER, 11, RBP, 2, 4, codes, NULL), t = tail;
    RUNTIME_FUNCTION fn = func(0x40, 0, code, sizeof code, ui);
    static const struct { int p; int rsp; int rbx, rbp, rip; } want[] = {   // stack slots; -1: unchanged
        { 0, 5, -1, -1, 4 }, { 1, 6, -1, 4, 5 }, { 2, 7, 4, 5, 6 }, { 6, 12, 9, 10, 11 }, { 10, 12, 9, 10, 11 } };
    for (int i = 0; i < 5; i++) {
        for (DWORD type = 1; type <= 2; type++) {
            CONTEXT c, w; init(&c); init(&w);
            void *data; DWORD64 frame;
            void *h = unwind(type, at(fn.BeginAddress + want[i].p), &fn, &c, &data, &frame);
            w.Rsp = A(want[i].rsp); w.Rip = S(want[i].rip);
            if (want[i].rbx >= 0) w.Rbx = S(want[i].rbx);
            if (want[i].rbp >= 0) w.Rbp = S(want[i].rbp);
            char name[32]; snprintf(name, sizeof name, "prolog+%d", want[i].p);
            check(name, &c, &w, h, NULL, data, (void *)0xDA7A, frame, A(4));
        }
    }
    for (DWORD type = 1; type <= 2; type++) {                         // the body: rbp - 32 is the frame
        CONTEXT c, w; init(&c); init(&w);
        void *data; DWORD64 frame;
        void *h = unwind(type, at(fn.BeginAddress + 11), &fn, &c, &data, &frame);
        w.Rbx = S(21); w.Rbp = S(22); w.Rip = S(23); w.Rsp = A(24);
        check("after prolog", &c, &w, h, (void *)at(HANDLER), data, (void *)at(t + 4), frame, A(16));
    }
    // Bytes in the prolog that read as an epilog (pop rbx; ret) are still the prolog.
    static const BYTE fake[] = { 0x5B, 0xC3 };
    static const WORD fake_codes[] = { CODE(1, PUSH_NONVOL, RBX) };
    ui = info(UNW_FLAG_EHANDLER, 2, 0, 0, 1, fake_codes, NULL);
    fn = func(0x20, 0, fake, sizeof fake, ui);
    CONTEXT c, w; init(&c); init(&w);
    void *data; DWORD64 frame;
    void *h = unwind(1, at(fn.BeginAddress), &fn, &c, &data, &frame);
    w.Rip = S(4); w.Rsp = A(5);
    check("epilog bytes in prolog", &c, &w, h, NULL, data, (void *)0xDA7A, frame, A(4));
}

// ---- every unwind code in a body ----
static void codes(void) {
    static const WORD all[] = {
        CODE(0x30, SAVE_XMM128, 6), 0x0101,                    // xmm6 at frame + 1010h
        CODE(0x30, SAVE_XMM128_FAR, 15), 0x005C, 0x0001,       // xmm15 at frame + 1005Ch (not 16-byte aligned)
        CODE(0x30, SAVE_NONVOL, RSI), 0x0101,                  // rsi at frame + 808h
        CODE(0x30, SAVE_NONVOL_FAR, R15), 0x0030, 0x0001,      // r15 at frame + 10030h
        CODE(0x30, OP6, 3), CODE(0x30, OP7, 5), CODE(0x30, 11, 0), CODE(0x30, 15, 15),   // one slot each, no effect
        CODE(0x20, ALLOC_LARGE, 0), 0x0102,                    // 810h
        CODE(0x10, ALLOC_LARGE, 1), 0x0010, 0x0001,            // 10010h
        CODE(8, ALLOC_SMALL, 1),                               // 10h
        CODE(2, PUSH_NONVOL, R12), CODE(1, PUSH_NONVOL, RBX),
    };
    int n = sizeof all / sizeof all[0];
    DWORD ui = info(UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER, 0x30, 0, 0, n, all, NULL), t = tail;
    RUNTIME_FUNCTION fn = func(0x80, 0, NULL, 0, ui);
    for (DWORD type = 0; type <= 4; type++) {
        CONTEXT c, w; init(&c); init(&w);
        void *data; DWORD64 frame;
        void *h = unwind(type, at(fn.BeginAddress + 0x40), &fn, &c, &data, &frame);
        w.Rsi = S(4 + 0x101); w.R15 = S(4 + 0x2006);
        w.Xmm6.Low = S(4 + 0x202); w.Xmm6.High = S(4 + 0x203);
        memcpy(&w.Xmm15, (BYTE *)A(4) + 0x1005C, 16);
        w.R12 = S(0x210A); w.Rbx = S(0x210B); w.Rip = S(0x210C); w.Rsp = A(0x210D);
        int match = (type & 3) != 0;
        char name[32]; snprintf(name, sizeof name, "all codes, type %lu", type);
        check(name, &c, &w, h, match ? (void *)at(HANDLER) : NULL, data, match ? (void *)at(t + 4) : (void *)0xDA7A,
              frame, A(4));
    }

    // A frame register with an offset (rbp - 48), and an odd number of slots before the handler.
    static const WORD fp[] = { CODE(8, SET_FPREG, 0), CODE(4, SAVE_NONVOL, RDI), 2 };
    ui = info(UNW_FLAG_EHANDLER, 8, RBP, 3, 3, fp, NULL); t = tail;
    fn = func(0x20, 0, NULL, 0, ui);
    CONTEXT c, w; init(&c); init(&w);
    c.Rbp = w.Rbp = A(40);
    void *data; DWORD64 frame;
    void *h = unwind(1, at(fn.BeginAddress + 0x10), &fn, &c, &data, &frame);
    w.Rdi = S(36); w.Rip = S(34); w.Rsp = A(35);
    check("frame register", &c, &w, h, (void *)at(HANDLER), data, (void *)at(t + 4), frame, A(34));

    // A machine frame with an error code (one slot: the handler follows two).
    static const WORD mf1[] = { CODE(0, PUSH_MACHFRAME, 1) };
    ui = info(UNW_FLAG_UHANDLER, 0, 0, 0, 1, mf1, NULL); t = tail;
    fn = func(0x20, 0, NULL, 0, ui);
    for (DWORD type = 1; type <= 2; type++) {
        init(&c); init(&w);
        h = unwind(type, at(fn.BeginAddress + 4), &fn, &c, &data, &frame);
        w.Rip = S(5); w.Rsp = S(8);
        check(type == 1 ? "machine frame, error code, type 1" : "machine frame, error code", &c, &w, h,
              type == 2 ? (void *)at(HANDLER) : NULL, data, type == 2 ? (void *)at(t + 4) : (void *)0xDA7A, frame,
              A(4));
    }
    // ... and without, after a push.
    static const WORD mf0[] = { CODE(0, PUSH_NONVOL, RBX), CODE(0, PUSH_MACHFRAME, 0) };
    ui = info(UNW_FLAG_EHANDLER, 0, 0, 0, 2, mf0, NULL); t = tail;
    fn = func(0x20, 0, NULL, 0, ui);
    init(&c); init(&w);
    h = unwind(1, at(fn.BeginAddress + 4), &fn, &c, &data, &frame);
    w.Rbx = S(4); w.Rip = S(5); w.Rsp = S(8);
    check("machine frame", &c, &w, h, (void *)at(HANDLER), data, (void *)at(t + 4), frame, A(4));
}

// ---- chained entries ----
static void chains(void) {
    CONTEXT c, w;
    void *data, *h; DWORD64 frame;
    static const WORD parent_codes[] = { CODE(4, ALLOC_SMALL, 2), CODE(1, PUSH_NONVOL, RBP) };
    DWORD pi = info(UNW_FLAG_EHANDLER, 4, 0, 0, 2, parent_codes, NULL), pt = tail;
    RUNTIME_FUNCTION parent = func(0x40, 0, NULL, 0, pi);
    static const BYTE push_r13[] = { 0x41, 0x55 };
    static const WORD primary_codes[] = { CODE(2, PUSH_NONVOL, R13) };
    RUNTIME_FUNCTION fn = func(0x20, 0, push_r13, 2, info(UNW_FLAG_CHAININFO, 2, 0, 0, 1, primary_codes, &parent));

    init(&c); init(&w);                                             // in the body: both entries' codes
    h = unwind(1, at(fn.BeginAddress + 8), &fn, &c, &data, &frame);
    w.R13 = S(4); w.Rbp = S(8); w.Rip = S(9); w.Rsp = A(10);
    check("chain", &c, &w, h, (void *)at(HANDLER), data, (void *)at(pt + 4), frame, A(4));

    init(&c); init(&w);                                             // in the primary's prolog: the handler still counts
    h = unwind(1, at(fn.BeginAddress), &fn, &c, &data, &frame);
    w.Rbp = S(7); w.Rip = S(8); w.Rsp = A(9);
    check("chain, primary prolog", &c, &w, h, (void *)at(HANDLER), data, (void *)at(pt + 4), frame, A(4));

    // The parent's prolog covers pc (the parent starts where the primary does): no handler.
    static const WORD p2_codes[] = { CODE(5, ALLOC_SMALL, 2), CODE(1, PUSH_NONVOL, RBP) };
    DWORD p2 = info(UNW_FLAG_EHANDLER, 6, 0, 0, 2, p2_codes, NULL);
    static const WORD q2_codes[] = { CODE(1, PUSH_NONVOL, R13) };
    RUNTIME_FUNCTION fn2 = func(0x20, 0, NULL, 0, 0);
    RUNTIME_FUNCTION par2 = { fn2.BeginAddress, fn2.BeginAddress + 0x20, p2 };
    fn2.UnwindData = info(UNW_FLAG_CHAININFO, 1, 0, 0, 1, q2_codes, &par2);
    init(&c); init(&w);
    h = unwind(1, at(fn2.BeginAddress + 3), &fn2, &c, &data, &frame);
    w.R13 = S(4); w.Rbp = S(5); w.Rip = S(6); w.Rsp = A(7);
    check("chain, parent prolog", &c, &w, h, NULL, data, (void *)0xDA7A, frame, A(4));

    // The primary's frame register sets the frame; the parent (no frame register) keeps it.
    static const WORD p3_codes[] = { CODE(0, SAVE_NONVOL, RSI), 3, CODE(0, PUSH_NONVOL, RBX) };
    RUNTIME_FUNCTION par3 = func(0x20, 0, NULL, 0, info(0, 0, 0, 0, 3, p3_codes, NULL));
    static const WORD q3_codes[] = { CODE(0, SET_FPREG, 0), CODE(0, PUSH_NONVOL, RBP) };
    RUNTIME_FUNCTION fn3 = func(0x20, 0, NULL, 0, info(UNW_FLAG_CHAININFO, 0, RBP, 1, 2, q3_codes, &par3));
    init(&c); init(&w);
    h = unwind(1, at(fn3.BeginAddress + 8), &fn3, &c, &data, &frame);
    w.Rbp = S(18); w.Rsi = S(21); w.Rbx = S(19); w.Rip = S(20); w.Rsp = A(21);
    check("chain, primary frame register", &c, &w, h, NULL, data, (void *)0xDA7A, frame, A(18));

    // The parent's frame register is read after the primary's codes restored it.
    static const WORD p4_codes[] = { CODE(0, SET_FPREG, 0) };
    DWORD p4 = info(UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER, 0, RBX, 1, 1, p4_codes, NULL), p4t = tail;
    RUNTIME_FUNCTION par4 = func(0x20, 0, NULL, 0, p4);
    static const WORD q4_codes[] = { CODE(0, PUSH_NONVOL, RBX) };
    RUNTIME_FUNCTION fn4 = func(0x20, 0, NULL, 0, info(UNW_FLAG_CHAININFO, 0, 0, 0, 1, q4_codes, &par4));
    DWORD64 saved = stk[4];
    stk[4] = A(30);
    init(&c); init(&w);
    h = unwind(2, at(fn4.BeginAddress + 8), &fn4, &c, &data, &frame);
    w.Rbx = A(30); w.Rip = S(28); w.Rsp = A(29);
    check("chain, parent frame register", &c, &w, h, (void *)at(HANDLER), data, (void *)at(p4t + 4), frame, A(28));
    stk[4] = saved;

    // A jump inside the primary is no epilog there, but leaves the parent: an epilog of the parent, simulated after the
    // primary's codes; the frame is the parent's (rbp - 0).
    static const WORD p5_codes[] = { CODE(1, PUSH_NONVOL, RBP) };
    RUNTIME_FUNCTION par5 = func(0x40, 0, NULL, 0, info(UNW_FLAG_EHANDLER, 1, RBP, 0, 1, p5_codes, NULL));
    static const BYTE jmp_back[] = { 0xEB, 0xF0 };                  // to begin+2
    RUNTIME_FUNCTION fn5 = func(0x20, 0x10, jmp_back, 2, info(UNW_FLAG_CHAININFO, 0, 0, 0, 1, primary_codes, &par5));
    init(&c); init(&w);
    h = unwind(1, at(fn5.BeginAddress + 0x10), &fn5, &c, &data, &frame);
    w.R13 = S(4); w.Rip = S(5); w.Rsp = A(6);
    check("chain, parent epilog", &c, &w, h, NULL, data, (void *)0xDA7A, frame, A(20));
}

int main(void) {
    for (int i = 0; i < (int)(sizeof stk / sizeof stk[0]); i++) stk[i] = 0x57AC000000000000ull | (DWORD64)i;
    epilogs();
    prologs();
    codes();
    chains();
    printf("unwind: %d cases, %d failed -> %s\n", cases, failed, failed ? "FAILED" : "ok");
    return failed ? 1 : 0;
}
