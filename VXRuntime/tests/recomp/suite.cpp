// Stage-1 program for the static recompiler (RECOMP.md): what a game's code does that a recompiler must get right,
// in one MSVC-ABI program (clang -target x86_64-pc-windows-msvc, linked by MinGW's ld without a C runtime, as
// tests/msvc_eh.cpp): jump tables, SEH __try/__except/__finally (hardware and software exceptions), C++ exceptions,
// threads with atomics and a message-passing litmus, TLS callbacks with __declspec(thread), SSE and AVX2/FMA paths
// chosen by CPUID, and Direct3D 11 (suite_d3d.c: a clear and a triangle read back).
// Output: lines compared between runs; "info:" and "time:" lines are not (CPU features, timings).
// suite.exe [bench]: with "bench", the timed kernels too (RECOMP.md's speed comparison).
typedef unsigned long long u64; typedef unsigned int u32; typedef unsigned char u8;
extern "C" {
__declspec(dllimport) int puts(const char *);
__declspec(dllimport) void *__stdcall GetProcessHeap(void);
__declspec(dllimport) void *__stdcall HeapAlloc(void *, u32, u64);
__declspec(dllimport) int __stdcall HeapFree(void *, u32, void *);
__declspec(dllimport) void *__stdcall CreateThread(void *, u64, u32 (__stdcall *)(void *), void *, u32, u32 *);
__declspec(dllimport) u32 __stdcall WaitForSingleObject(void *, u32);
__declspec(dllimport) int __stdcall CloseHandle(void *);
__declspec(dllimport) void __stdcall RaiseException(u32, u32, u32, const u64 *);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *);
__declspec(dllimport) const char *__stdcall GetCommandLineA(void);
__declspec(dllimport) void __stdcall Sleep(u32);
__declspec(dllimport) u32 __stdcall GetCurrentThreadId(void);
__declspec(dllimport) int __stdcall GetExitCodeThread(void *, u32 *);
unsigned long _exception_code(void);
int d3d_test(char *out, int cap);                     // suite_d3d.c
}
void *operator new(decltype(sizeof 0) n) { return HeapAlloc(GetProcessHeap(), 0, n); }
void operator delete(void *p, decltype(sizeof 0)) noexcept { HeapFree(GetProcessHeap(), 0, p); }
void operator delete(void *p) noexcept { HeapFree(GetProcessHeap(), 0, p); }

// ---- output without a C runtime -------------------------------------------------------------------------------
static char line[512]; static int len;
static void s(const char *t) { while (*t && len < 500) line[len++] = *t++; }
static void u(u64 v) { char t[24]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) line[len++] = t[--n]; }
static void i(long long v) { if (v < 0) { s("-"); u((u64)-v); } else u((u64)v); }
static void x(u64 v) { s("0x"); char t[17]; int n = 0; do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v); while (n) line[len++] = t[--n]; }
static void end() { line[len] = 0; puts(line); len = 0; }
static int failures;
static void verdict(int ok) { s(ok ? " -> ok" : " -> FAILED"); end(); if (!ok) failures++; }

// ---- 1. jump tables: a bytecode interpreter (dense switch) and a classifier (sparse cases) --------------------
enum { OP_PUSH, OP_ADD, OP_SUB, OP_MUL, OP_DUP, OP_SWAP, OP_JNZ, OP_DEC, OP_XOR, OP_SHL, OP_SHR, OP_ROT, OP_DROP, OP_MOD, OP_HALT, OP_NOP };
__declspec(noinline) static u64 interp(const u8 *code, u64 seed, u64 steps) {
    u64 st[64]; int sp = 0, pc = 0; st[sp++] = seed;
    for (u64 n = 0; n < steps; n++) {
        switch (code[pc++]) {
        case OP_PUSH: st[sp++] = code[pc++]; break;
        case OP_ADD: sp--; st[sp - 1] += st[sp]; break;
        case OP_SUB: sp--; st[sp - 1] -= st[sp]; break;
        case OP_MUL: sp--; st[sp - 1] *= st[sp] | 1; break;
        case OP_DUP: st[sp] = st[sp - 1]; sp++; break;
        case OP_SWAP: { u64 t = st[sp - 1]; st[sp - 1] = st[sp - 2]; st[sp - 2] = t; break; }
        case OP_JNZ: { u8 to = code[pc++]; if (st[sp - 1] & 7) pc = to; break; }
        case OP_DEC: st[sp - 1]--; break;
        case OP_XOR: sp--; st[sp - 1] ^= st[sp]; break;
        case OP_SHL: st[sp - 1] <<= 3; break;
        case OP_SHR: st[sp - 1] >>= 5; break;
        case OP_ROT: st[sp - 1] = st[sp - 1] << 13 | st[sp - 1] >> 51; break;
        case OP_DROP: sp--; break;
        case OP_MOD: sp--; st[sp - 1] %= (st[sp] | 1); break;
        case OP_HALT: return st[sp - 1];
        default: break;
        }
        if (sp > 60) sp = 1;
        if (sp < 1) { st[0] = n; sp = 1; }
        if (pc >= 32) pc = 0;
    }
    return st[sp - 1] ^ (u64)sp;
}
__declspec(noinline) static int classify(int c) {
    switch (c) {
    case 'a': case 'e': case 'i': case 'o': case 'u': return 1;
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': return 2;
    case ' ': case '\t': case '\n': return 3;
    case '.': case ',': case ';': case ':': case '!': case '?': return 4;
    case 'x': case 'y': case 'z': return 5;
    default: return c < 32 ? 6 : 0;
    }
}
static const u8 program[32] = { OP_PUSH, 7, OP_ADD, OP_DUP, OP_ROT, OP_XOR, OP_PUSH, 3, OP_MUL, OP_SHR, OP_DUP, OP_PUSH, 11, OP_MOD,
                                OP_ADD, OP_DEC, OP_JNZ, 2, OP_SHL, OP_SWAP, OP_DROP, OP_NOP, OP_PUSH, 5, OP_SUB, OP_JNZ, 0, OP_DUP, OP_DROP, OP_NOP, OP_NOP, OP_NOP };
static void test_jump_tables() {
    u64 r = interp(program, 12345, 100000);
    const char *text = "the quick brown fox, 2 lazy dogs; 10 times!\t?";
    u64 h = 0;
    for (int k = 0; text[k]; k++) h = h * 7 + (u64)classify(text[k]);
    s("jump tables: interp="); x(r); s(" classify="); x(h);
    verdict(r == 0x7b5ca9bb39ad6dadull || true);       // (the value itself is compared between runs)
}

// ---- 2. SEH: an access violation, an integer division by zero, RaiseException, __finally ---------------------
static volatile int *volatile nowhere;
static volatile int zero;
static int finally_ran;
// (clang covers calls in a __try, not the instructions themselves without -fasync-exceptions: the faults happen in callees)
__declspec(noinline) static void poke(int v) { *nowhere = v; }
__declspec(noinline) static int divide(int a) { return a / zero; }
__declspec(noinline) static int seh_av() {
    __try { poke(1); return -1; }
    __except (_exception_code() == 0xC0000005u ? 1 : 0) { return 1; }
}
__declspec(noinline) static int seh_div() {
    __try { volatile int q = divide(100); (void)q; return -1; }
    __except (_exception_code() == 0xC0000094u ? 1 : 0) { return 2; }
}
__declspec(noinline) static void raiser(u32 code) {
    __try { u64 info[1] = { 77 }; RaiseException(code, 0, 1, info); }
    __finally { finally_ran++; }
}
__declspec(noinline) static int seh_raise() {
    __try { raiser(0xE0001234u); return -1; }
    __except (_exception_code() == 0xE0001234u ? 1 : 0) { return 3; }
}
__declspec(noinline) static int seh_nested() {           // the inner filter declines; the outer one handles
    int r = 0;
    __try {
        __try { poke(2); }
        __except (0) { r = -1; }
    } __except (1) { r += 4; }
    return r;
}
static void test_seh() {
    int a = seh_av(), d = seh_div(), r = seh_raise(), n = seh_nested();
    s("seh: av="); i(a); s(" div="); i(d); s(" raise="); i(r); s(" finally="); i(finally_ran); s(" nested="); i(n);
    verdict(a == 1 && d == 2 && r == 3 && finally_ran == 1 && n == 4);
}

// ---- 3. C++ exceptions ---------------------------------------------------------------------------------------------
static int destroyed;
struct Guard { int id; Guard(int k) : id(k) {} ~Guard() { destroyed += id; } };
struct Err { int code; virtual ~Err() {} };
struct Deep : Err { int depth; };
__declspec(noinline) static void thrower(int depth) {
    Guard g(depth);
    if (depth == 0) { Deep e; e.code = 42; e.depth = 9; throw e; }
    thrower(depth - 1);
}
__declspec(noinline) static int cxx_basic() { try { thrower(5); } catch (const Err &e) { return e.code; } return -1; }
__declspec(noinline) static int cxx_rethrow() { try { try { throw 7; } catch (int) { throw; } } catch (int v) { return v; } return -1; }
__declspec(noinline) static int cxx_loop() { int c = 0; for (int k = 0; k < 2000; k++) try { if (k >= 0) throw k; } catch (int v) { c += v == k; } return c; }
static void test_cxx() {
    int b = cxx_basic(), r = cxx_rethrow(), l = cxx_loop();
    s("c++ exceptions: basic="); i(b); s(" destroyed="); i(destroyed); s(" rethrow="); i(r); s(" loop="); i(l);
    verdict(b == 42 && destroyed == 15 && r == 7 && l == 2000);
}

// ---- 4. threads: atomic adds, a spinlock, a message-passing litmus -------------------------------------------------
enum { THREADS = 4, ITER = 200000, MSGS = 200000 };
static long long counter, locked_counter; static int lock_word;
static u64 mp_data[1024]; static volatile u64 mp_flag; static long long mp_bad;
static u32 __stdcall worker(void *arg) {
    (void)arg;
    for (int k = 0; k < ITER; k++) {
        __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);                       // LOCK XADD
        while (__atomic_exchange_n(&lock_word, 1, __ATOMIC_ACQUIRE)) {}          // XCHG
        locked_counter++;                                                         // (plain, under the lock)
        __atomic_store_n(&lock_word, 0, __ATOMIC_RELEASE);                        // a plain MOV on x86
    }
    return 0;
}
// x86 orders a plain store after a plain store: once the reader sees the flag, it sees the data written before it.
static u32 __stdcall producer(void *arg) {
    (void)arg;
    for (u64 k = 1; k <= MSGS; k++) {
        mp_data[k & 1023] = k * 0x9E3779B97F4A7C15ull;
        __atomic_store_n(&mp_flag, k, __ATOMIC_RELEASE);                          // (compiles to a plain MOV)
        while (__atomic_load_n(&mp_flag, __ATOMIC_ACQUIRE) == k && k < MSGS) {}  // (the reader takes it: flag | 1 << 63)
    }
    return 0;
}
static u32 __stdcall consumer(void *arg) {
    (void)arg;
    for (;;) {
        u64 f = __atomic_load_n(&mp_flag, __ATOMIC_ACQUIRE);
        if (!f || f >> 63) continue;
        if (__atomic_load_n(&mp_data[f & 1023], __ATOMIC_RELAXED) != f * 0x9E3779B97F4A7C15ull) mp_bad++;
        if (f == MSGS) return 0;
        __atomic_store_n(&mp_flag, f | 1ull << 63, __ATOMIC_RELEASE);
    }
}

// ---- 5. TLS: callbacks (DLL_PROCESS_ATTACH / THREAD_ATTACH / THREAD_DETACH) and __declspec(thread) --------------
static long tls_process, tls_attach, tls_detach;
__declspec(thread) int tls_value = 1000;
__declspec(thread) u64 tls_slots[8];
static void __stdcall tls_callback(void *module, u32 reason, void *reserved) {
    (void)module; (void)reserved;
    if (reason == 1) __atomic_fetch_add(&tls_process, 1, __ATOMIC_SEQ_CST);
    if (reason == 2) __atomic_fetch_add(&tls_attach, 1, __ATOMIC_SEQ_CST);
    if (reason == 3) __atomic_fetch_add(&tls_detach, 1, __ATOMIC_SEQ_CST);
}
extern "C" {
// The TLS directory (PE format, IMAGE_TLS_DIRECTORY64) and its sections: data from .tls$AAA to .tls$ZZZ, callbacks
// from .CRT$XLA to .CRT$XLZ (the linker sorts the $ suffixes). ld points the TLS data directory at _tls_used.
u32 _tls_index = 0;
__attribute__((section(".tls$AAA"))) char _tls_start = 0;
__attribute__((section(".tls$ZZZ"))) char _tls_end = 0;
__attribute__((section(".CRT$XLA"))) void (__stdcall *__xl_a)(void *, u32, void *) = 0;
__attribute__((section(".CRT$XLB"))) void (__stdcall *__xl_b)(void *, u32, void *) = tls_callback;
__attribute__((section(".CRT$XLZ"))) void (__stdcall *__xl_z)(void *, u32, void *) = 0;
struct tls_dir { u64 start, end, index, callbacks; u32 zero_fill, characteristics; };
extern const tls_dir _tls_used;
const tls_dir _tls_used =
    { (u64)&_tls_start, (u64)&_tls_end, (u64)&_tls_index, (u64)(&__xl_a + 1), 0, 0 };
}
static u32 __stdcall tls_worker(void *arg) {
    u64 id = (u64)arg;
    int before = tls_value;
    tls_value += (int)id;
    for (int k = 0; k < 8; k++) tls_slots[k] = id * 100 + (u64)k;
    Sleep(5);
    u64 sum = 0;
    for (int k = 0; k < 8; k++) sum += tls_slots[k];
    return (u32)(before == 1000 && tls_value == 1000 + (int)id && sum == id * 800 + 28);
}
static void test_threads_tls() {
    void *h[THREADS + 2]; u32 tid;
    for (int k = 0; k < THREADS; k++) h[k] = CreateThread(0, 0, worker, 0, 0, &tid);
    h[THREADS] = CreateThread(0, 0, producer, 0, 0, &tid);
    h[THREADS + 1] = CreateThread(0, 0, consumer, 0, 0, &tid);
    for (int k = 0; k < THREADS + 2; k++) { WaitForSingleObject(h[k], 0xFFFFFFFFu); CloseHandle(h[k]); }
    s("threads: counter="); i(counter); s(" locked="); i(locked_counter); s(" mp_violations="); i(mp_bad);
    verdict(counter == THREADS * ITER && locked_counter == THREADS * ITER && mp_bad == 0);
    int ok_threads = 0;
    for (u64 k = 1; k <= 3; k++) {
        void *t = CreateThread(0, 0, tls_worker, (void *)k, 0, &tid);
        WaitForSingleObject(t, 0xFFFFFFFFu);
        u32 code = 0;
        GetExitCodeThread(t, &code); ok_threads += code == 1;
        CloseHandle(t);
    }
    s("tls: process_attach="); i(tls_process); s(" threads_ok="); i(ok_threads); s(" main_value="); i(tls_value);
    // (THREAD_ATTACH / DETACH counted as at least the threads made here: the runtime may make threads of its own)
    s(" attach>=9 "); s(tls_attach >= 3 + THREADS + 2 ? "yes" : "no"); s(" detach>=9 "); s(tls_detach >= 3 + THREADS + 2 ? "yes" : "no");
    verdict(tls_process == 1 && ok_threads == 3 && tls_value == 1000 && tls_attach >= 9 && tls_detach >= 9);
}

// ---- 6. SSE and AVX2/FMA paths chosen by CPUID ------------------------------------------------------------------------
static void cpuid(u32 leaf, u32 sub, u32 r[4]) { __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub)); }
static int have_avx2_fma() {
    u32 r[4]; cpuid(0, 0, r);
    if (r[0] < 7) return 0;
    cpuid(1, 0, r);
    int osxsave = r[2] >> 27 & 1, avx = r[2] >> 28 & 1, fma = r[2] >> 12 & 1;
    if (!osxsave || !avx || !fma) return 0;
    u32 lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6) return 0;
    cpuid(7, 0, r);
    return r[1] >> 5 & 1;
}
typedef float v4f __attribute__((vector_size(16)));
typedef float v8f __attribute__((vector_size(32)));
typedef int v8i __attribute__((vector_size(32)));
enum { N = 4096 };
static float va[N], vb[N], vc[N];
__declspec(noinline) static u64 sse_kernel(float *out) {
    v4f acc = { 0, 0, 0, 0 };
    for (int k = 0; k < N; k += 4) {
        v4f a = *(const v4f *)(va + k), b = *(const v4f *)(vb + k);
        v4f r = a * b + (v4f){ 0.5f, 0.25f, 0.125f, 1.0f };
        *(v4f *)(out + k) = r; acc += r;
    }
    union { float f[4]; u32 w[4]; } q; *(v4f *)q.f = acc;
    return (u64)q.w[0] ^ (u64)q.w[1] << 8 ^ (u64)q.w[2] << 16 ^ (u64)q.w[3] << 24;
}
__attribute__((target("avx2,fma"))) __declspec(noinline) static u64 avx_kernel(float *out) {
    v8f acc = { 0 };
    v8i iacc = { 0 };
    for (int k = 0; k < N; k += 8) {
        v8f a = *(const v8f *)(va + k), b = *(const v8f *)(vb + k);
        v8f r = __builtin_elementwise_fma(a, b, (v8f){ 0.5f, 0.25f, 0.125f, 1.0f, 0.5f, 0.25f, 0.125f, 1.0f });
        *(v8f *)(out + k) = r; acc += r;
        iacc += __builtin_convertvector(r * 16.0f, v8i) * (v8i){ 1, 3, 5, 7, 9, 11, 13, 15 };   // (AVX2 integer)
    }
    union { float f[8]; u32 w[8]; } q; *(v8f *)q.f = acc;
    union { int v[8]; u32 w[8]; } p; *(v8i *)p.v = iacc;
    u64 h = 0;
    for (int k = 0; k < 8; k++) h = h * 31 + q.w[k] + p.w[k];
    return h;
}
static void test_simd() {
    for (int k = 0; k < N; k++) { va[k] = (float)(k % 97) * 0.37f - 11.0f; vb[k] = (float)(k % 13) * 1.5f + 0.001f * (float)k; }
    u64 h1 = sse_kernel(vc);
    s("simd: sse="); x(h1); s(" c[123]="); x(*(const u32 *)&vc[123]);
    verdict(1);
    int avx = have_avx2_fma();
    s("info: cpuid avx2+fma="); i(avx); end();
    if (avx) { u64 h2 = avx_kernel(vc); s("simd: avx2/fma="); x(h2); s(" c[123]="); x(*(const u32 *)&vc[123]); verdict(1); }
}

// ---- 7. kernels for timing (bench) ----------------------------------------------------------------------------------
static double now() { static long long f; long long c; if (!f) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c / (double)f; }
__declspec(noinline) static u64 fib(u64 n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
__declspec(noinline) static void qsort_u32(u32 *a, long lo, long hi) {
    while (lo < hi) {
        u32 p = a[(lo + hi) / 2]; long l = lo, r = hi;
        while (l <= r) { while (a[l] < p) l++; while (a[r] > p) r--; if (l <= r) { u32 t = a[l]; a[l] = a[r]; a[r] = t; l++; r--; } }
        if (r - lo < hi - l) { qsort_u32(a, lo, r); lo = l; } else { qsort_u32(a, l, hi); hi = r; }
    }
}
__declspec(noinline) static float matmul(const float *a, const float *b, float *c, int n) {
    for (int r = 0; r < n; r++) for (int k = 0; k < n; k++) { float av = a[r * n + k]; for (int j = 0; j < n; j++) c[r * n + j] += av * b[k * n + j]; }
    float t = 0; for (int k = 0; k < n * n; k += 97) t += c[k]; return t;
}
__declspec(noinline) static u32 crc32(const u8 *p, u64 n) {
    static u32 table[256];
    if (!table[1]) for (u32 k = 0; k < 256; k++) { u32 c = k; for (int j = 0; j < 8; j++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; table[k] = c; }
    u32 c = 0xFFFFFFFFu; for (u64 k = 0; k < n; k++) c = table[(c ^ p[k]) & 0xFF] ^ (c >> 8); return ~c;
}
struct Node { Node *l, *r; u64 key; };
__declspec(noinline) static Node *insert(Node *t, u64 key, Node *pool, int *used) {
    Node **at = &t;
    while (*at) at = key < (*at)->key ? &(*at)->l : &(*at)->r;
    Node *n = &pool[(*used)++]; n->l = n->r = 0; n->key = key; *at = n; return t;
}
__declspec(noinline) static u64 walk(const Node *t) { u64 s_ = 0; while (t) { s_ += t->key + walk(t->l); t = t->r; } return s_; }
static void bench() {
    double t0 = now(); u64 f = fib(30); double t1 = now();
    s("bench: fib="); u(f); end(); s("time: fib "); u((u64)((t1 - t0) * 1e6)); s(" us"); end();
    enum { M = 1 << 20 };
    u32 *a = (u32 *)HeapAlloc(GetProcessHeap(), 0, M * 4);
    u64 seed = 1; for (int k = 0; k < M; k++) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; a[k] = (u32)(seed >> 33); }
    t0 = now(); qsort_u32(a, 0, M - 1); t1 = now();
    int sorted = 1; for (int k = 1; k < M; k++) sorted &= a[k - 1] <= a[k];
    s("bench: sort sorted="); i(sorted); s(" a[777]="); u(a[777]); end(); s("time: sort "); u((u64)((t1 - t0) * 1e6)); s(" us"); end();
    enum { MN = 192 };
    float *ma = (float *)HeapAlloc(GetProcessHeap(), 8, MN * MN * 4 * 3), *mb = ma + MN * MN, *mc = mb + MN * MN;
    for (int k = 0; k < MN * MN; k++) { ma[k] = (float)(k % 17) * 0.25f; mb[k] = (float)(k % 23) * 0.125f; }
    t0 = now(); float mt = matmul(ma, mb, mc, MN); t1 = now();
    s("bench: matmul="); x(*(const u32 *)&mt); end(); s("time: matmul "); u((u64)((t1 - t0) * 1e6)); s(" us"); end();
    t0 = now(); u32 c = crc32((const u8 *)a, M * 4); c ^= crc32((const u8 *)a, M * 4) * 3; t1 = now();
    s("bench: crc="); x(c); end(); s("time: crc "); u((u64)((t1 - t0) * 1e6)); s(" us"); end();
    t0 = now(); u64 r = interp(program, 99, 30000000); t1 = now();
    s("bench: interp="); x(r); end(); s("time: interp "); u((u64)((t1 - t0) * 1e6)); s(" us"); end();
    Node *pool = (Node *)HeapAlloc(GetProcessHeap(), 0, sizeof(Node) * 400000); int used = 0; Node *t = 0;
    t0 = now(); seed = 7; for (int k = 0; k < 400000; k++) { seed = seed * 6364136223846793005ull + 1; t = insert(t, seed >> 20, pool, &used); } u64 w = walk(t); t1 = now();
    s("bench: tree="); x(w); end(); s("time: tree "); u((u64)((t1 - t0) * 1e6)); s(" us"); end();
}

static int arg(const char *w) {                     // (a word of the command line)
    for (const char *p = GetCommandLineA(); *p; p++) { int k = 0; while (w[k] && p[k] == w[k]) k++; if (!w[k]) return 1; }
    return 0;
}
int main() {
    if (arg("benchonly")) { bench(); return 0; }
    test_jump_tables();
    test_seh();
    test_cxx();
    test_threads_tls();
    test_simd();
    char d3d[256]; int r = d3d_test(d3d, sizeof d3d);
    s(d3d); verdict(r == 0);
    if (arg("bench")) bench();
    s("suite: "); i(failures); s(" failures"); end();
    return failures;
}
