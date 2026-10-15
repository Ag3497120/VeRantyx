// Code that runs inside the vCPU guest (vcpu.c copies the __vx_guest section into the guest's code). Built on its
// own (Makefile: freestanding, no builtins, no stack protector): it may call nothing and read no globals, as the
// host's own code and data are never put in the guest.
//
// xv_guest_defer: a call of a native function that returns nothing (Direct3D 11's immediate context: setters,
// draws) is recorded instead of leaving the guest: its arguments and the arrays they point to (copied now, as the
// program may reuse them at once) go into the thread's batch; the host runs the batch, in order, at the guest's next
// exit (any call it must answer comes after them). 1: the batch is full or the call does not fit: call it now.
#include "vcpu.h"
#include "xvheap.h"

__attribute__((section("__TEXT,__vx_guest"), used))
int xv_guest_defer(x86_ctx *c, const xv_defer *d) {
    uint64_t args[XV_DEFER_ARGS];
    const uint64_t *stack = (const uint64_t *)(uintptr_t)c->gpr[4];
    uint32_t n = d->nargs;
    for (uint32_t i = 0; i < n; i++) args[i] = i == 0 ? c->gpr[1] : i == 1 ? c->gpr[2] : i == 2 ? c->gpr[8] : i == 3 ? c->gpr[9] : stack[5 + (i - 4)];
    uint64_t size = 8 * (3 + (uint64_t)n), bytes[4];
    for (uint32_t k = 0; k < d->ncopy; k++) {
        const xv_copy *cp = &d->copy[k];
        uint64_t count = cp->count_arg >= 0 ? (uint32_t)args[cp->count_arg] : cp->count;
        if (count > cp->max) return 1;                           // (D3D11_KEEP_..., or more than fits)
        bytes[k] = args[cp->arg] ? count * cp->elem : 0;
        size += (bytes[k] + 7) & ~7ull;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)c->batch_ptr;
    if (!p || p + size > (uint8_t *)(uintptr_t)c->batch_end) return 1;
    uint64_t off = 8 * (3 + (uint64_t)n);
    for (uint32_t k = 0; k < d->ncopy; k++) {
        if (!bytes[k]) continue;
        const uint8_t *from = (const uint8_t *)(uintptr_t)args[d->copy[k].arg];
        volatile uint8_t *to = p + off;                          // (volatile: no memcpy call made of it)
        for (uint64_t b = 0; b < bytes[k]; b++) to[b] = from[b];
        args[d->copy[k].arg] = (uint64_t)(uintptr_t)(p + off);   // (the copy, at the same address for the host)
        off += (bytes[k] + 7) & ~7ull;
    }
    uint64_t *r = (uint64_t *)p;
    r[0] = size; r[1] = d->thunk; r[2] = n;
    for (uint32_t i = 0; i < n; i++) r[3 + i] = args[i];
    c->batch_ptr = (uint64_t)(uintptr_t)(p + size);
    return 0;
}

// ---- small functions answered in the guest (registered by src/arm64.c's vx_guest_add) ------------------------------
// Each: 0 with the result in RAX (the call is done), or 1 (the host's own function is called instead).
#define GUEST __attribute__((section("__TEXT,__vx_guest"), used))
static inline __attribute__((always_inline)) uint64_t garg(const x86_ctx *c, int i) {
    return i == 0 ? c->gpr[1] : i == 1 ? c->gpr[2] : i == 2 ? c->gpr[8] : i == 3 ? c->gpr[9] : ((const uint64_t *)(uintptr_t)c->gpr[4])[5 + (i - 4)];
}
enum { TEB_LAST_ERROR = 0x68, TEB_TLS_SLOTS = 0x1480 };
GUEST int xv_g_tls_get(x86_ctx *c, const void *d) {
    (void)d;
    uint32_t i = (uint32_t)garg(c, 0);
    if (i >= 64) return 1;
    *(volatile uint32_t *)(uintptr_t)(c->gs_base + TEB_LAST_ERROR) = 0;
    c->gpr[0] = ((volatile uint64_t *)(uintptr_t)(c->gs_base + TEB_TLS_SLOTS))[i];
    return 0;
}
GUEST int xv_g_tls_set(x86_ctx *c, const void *d) {
    (void)d;
    uint32_t i = (uint32_t)garg(c, 0);
    if (i >= 64) return 1;
    ((volatile uint64_t *)(uintptr_t)(c->gs_base + TEB_TLS_SLOTS))[i] = garg(c, 1);
    c->gpr[0] = 1;
    return 0;
}
GUEST int xv_g_get_last_error(x86_ctx *c, const void *d) { (void)d; c->gpr[0] = *(volatile uint32_t *)(uintptr_t)(c->gs_base + TEB_LAST_ERROR); return 0; }
GUEST int xv_g_set_last_error(x86_ctx *c, const void *d) { (void)d; *(volatile uint32_t *)(uintptr_t)(c->gs_base + TEB_LAST_ERROR) = (uint32_t)garg(c, 0); return 0; }
// Time: the guest's virtual counter is the host's mach_absolute_time (vtimer offset 0); d: the timebase, as
// kernel32.c's QueryPerformanceCounter computes nanoseconds.
static inline __attribute__((always_inline)) uint64_t gnow(const void *d) {
    uint64_t t; __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
    const uint32_t *tb = d;
    return t * tb[0] / tb[1];
}
GUEST int xv_g_qpc(x86_ctx *c, const void *d) { *(volatile int64_t *)(uintptr_t)garg(c, 0) = (int64_t)gnow(d); c->gpr[0] = 1; return 0; }
GUEST int xv_g_qpf(x86_ctx *c, const void *d) { (void)d; *(volatile int64_t *)(uintptr_t)garg(c, 0) = 1000000000; c->gpr[0] = 1; return 0; }
GUEST int xv_g_tick64(x86_ctx *c, const void *d) { c->gpr[0] = gnow(d) / 1000000; return 0; }
GUEST int xv_g_tick(x86_ctx *c, const void *d) { c->gpr[0] = (uint32_t)(gnow(d) / 1000000); return 0; }
// Memory and strings (the C runtime's): results as the host's (memcmp: the difference of the first unequal bytes).
GUEST int xv_g_memcpy(x86_ctx *c, const void *d) {
    (void)d;
    uint8_t *to = (uint8_t *)(uintptr_t)garg(c, 0); const uint8_t *from = (const uint8_t *)(uintptr_t)garg(c, 1);
    uint64_t n = garg(c, 2), i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; __builtin_memcpy(&w, from + i, 8); __builtin_memcpy(to + i, &w, 8); }
    for (; i < n; i++) to[i] = from[i];
    c->gpr[0] = (uint64_t)(uintptr_t)to;
    return 0;
}
GUEST int xv_g_memmove(x86_ctx *c, const void *d) {
    uint8_t *to = (uint8_t *)(uintptr_t)garg(c, 0); const uint8_t *from = (const uint8_t *)(uintptr_t)garg(c, 1);
    uint64_t n = garg(c, 2);
    if (to <= from || to >= from + n) return xv_g_memcpy(c, d);
    for (uint64_t i = n; i >= 8; i -= 8) { uint64_t w; __builtin_memcpy(&w, from + i - 8, 8); __builtin_memcpy(to + i - 8, &w, 8); n = i - 8; }
    while (n) { n--; to[n] = from[n]; }
    c->gpr[0] = (uint64_t)(uintptr_t)to;
    return 0;
}
GUEST int xv_g_memset(x86_ctx *c, const void *d) {
    (void)d;
    uint8_t *to = (uint8_t *)(uintptr_t)garg(c, 0);
    uint64_t v = (uint8_t)garg(c, 1), n = garg(c, 2), i = 0;
    v *= 0x0101010101010101ull;
    for (; i + 8 <= n; i += 8) __builtin_memcpy(to + i, &v, 8);
    for (; i < n; i++) to[i] = (uint8_t)v;
    c->gpr[0] = (uint64_t)(uintptr_t)to;
    return 0;
}
GUEST int xv_g_memcmp(x86_ctx *c, const void *d) {
    (void)d;
    const uint8_t *a = (const uint8_t *)(uintptr_t)garg(c, 0), *b = (const uint8_t *)(uintptr_t)garg(c, 1);
    uint64_t n = garg(c, 2);
    int r = 0;
    for (uint64_t i = 0; i < n; i++) if (a[i] != b[i]) { r = (int)a[i] - (int)b[i]; break; }
    c->gpr[0] = (uint64_t)(int64_t)r;
    return 0;
}
// (8 bytes at a time from an aligned address: a read never crosses into a page the string does not reach)
GUEST int xv_g_strlen(x86_ctx *c, const void *d) {
    (void)d;
    const char *s = (const char *)(uintptr_t)garg(c, 0), *p = s;
    while ((uintptr_t)p & 7) { if (!*p) { c->gpr[0] = (uint64_t)(p - s); return 0; } p++; }
    for (;; p += 8) {
        uint64_t w = *(const volatile uint64_t *)p;
        uint64_t z = (w - 0x0101010101010101ull) & ~w & 0x8080808080808080ull;
        if (z) { c->gpr[0] = (uint64_t)(p - s) + (uint64_t)__builtin_ctzll(z) / 8; return 0; }
    }
}
// A function that always answers the same (GetProcessHeap; d: the value).
GUEST int xv_g_const(x86_ctx *c, const void *d) { c->gpr[0] = *(const uint64_t *)d; return 0; }
GUEST int xv_g_wcslen(x86_ctx *c, const void *d) {
    (void)d;
    const uint16_t *s = (const uint16_t *)(uintptr_t)garg(c, 0);
    uint64_t n = 0;
    while (s[n]) n++;
    c->gpr[0] = n;
    return 0;
}
// The heap (translator/xvheap.h; d: the heap's address): blocks of its range only; a larger one, or a pointer the
// host's malloc gave, is the host's.
static inline __attribute__((always_inline)) xh_heap *gheap(const void *d) { return *(xh_heap *const *)d; }
static inline __attribute__((always_inline)) int galloc(x86_ctx *c, const void *d, uint64_t n, int zero) {
    void *p = xh_alloc(gheap(d), n);
    if (!p) return 1;
    if (zero) xh_zero(p, xh_block_size(p));
    c->gpr[0] = (uint64_t)(uintptr_t)p;
    return 0;
}
GUEST int xv_g_malloc(x86_ctx *c, const void *d) { return galloc(c, d, garg(c, 0), 0); }
GUEST int xv_g_calloc(x86_ctx *c, const void *d) {
    uint64_t a = garg(c, 0), b = garg(c, 1);
    if (a > 0xFFFFFFFFull || b > 0xFFFFFFFFull) return 1;
    return galloc(c, d, a * b, 1);
}
GUEST int xv_g_free(x86_ctx *c, const void *d) {
    void *p = (void *)(uintptr_t)garg(c, 0);
    return p && !xh_free(gheap(d), p);
}
// realloc(p, n) (HeapReAlloc: i = 2, its flags must not ask to stay in place): a block grown out of its class moves
// within the range.
static inline __attribute__((always_inline)) int grealloc(x86_ctx *c, const void *d, int i) {
    xh_heap *h = gheap(d);
    void *p = (void *)(uintptr_t)garg(c, i);
    uint64_t n = garg(c, i + 1);
    if (!p) return i ? 1 : galloc(c, d, n, 0);
    if (!n || !xh_owns(h, p)) return 1;
    uint32_t have = xh_block_size(p);
    if (n > have) {
        void *q = xh_alloc(h, n);
        if (!q) return 1;
        xh_copy(q, p, have);
        xh_free(h, p);
        p = q;
    }
    c->gpr[0] = (uint64_t)(uintptr_t)p;
    return 0;
}
GUEST int xv_g_realloc(x86_ctx *c, const void *d) { return grealloc(c, d, 0); }
GUEST int xv_g_msize(x86_ctx *c, const void *d) {
    const void *p = (const void *)(uintptr_t)garg(c, 0);
    if (!xh_owns(gheap(d), p)) return 1;
    c->gpr[0] = xh_block_size(p);
    return 0;
}
enum { HEAP_ZERO_MEMORY = 0x8, HEAP_REALLOC_IN_PLACE_ONLY = 0x10, LMEM_ZEROINIT = 0x40 };
GUEST int xv_g_heap_alloc(x86_ctx *c, const void *d) { return galloc(c, d, garg(c, 2), (garg(c, 1) & HEAP_ZERO_MEMORY) != 0); }
GUEST int xv_g_heap_free(x86_ctx *c, const void *d) {
    void *p = (void *)(uintptr_t)garg(c, 2);
    if (p && !xh_free(gheap(d), p)) return 1;
    c->gpr[0] = 1;
    return 0;
}
GUEST int xv_g_heap_size(x86_ctx *c, const void *d) {
    const void *p = (const void *)(uintptr_t)garg(c, 2);
    if (!xh_owns(gheap(d), p)) return 1;
    c->gpr[0] = xh_block_size(p);
    return 0;
}
GUEST int xv_g_heap_realloc(x86_ctx *c, const void *d) {
    uint64_t flags = garg(c, 1);
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) return 1;
    // (HEAP_ZERO_MEMORY: what lies past the old size is zeroed; the old size is the block's, so a block that stays
    // has nothing past it, and one that moves is the host's to zero.)
    const void *p = (const void *)(uintptr_t)garg(c, 2);
    if ((flags & HEAP_ZERO_MEMORY) && (!xh_owns(gheap(d), p) || garg(c, 3) > xh_block_size(p))) return 1;
    return grealloc(c, d, 2);
}
GUEST int xv_g_local_alloc(x86_ctx *c, const void *d) { return galloc(c, d, garg(c, 1), (garg(c, 0) & LMEM_ZEROINIT) != 0); }
GUEST int xv_g_local_free(x86_ctx *c, const void *d) {
    void *p = (void *)(uintptr_t)garg(c, 0);
    if (p && !xh_free(gheap(d), p)) return 1;
    c->gpr[0] = 0;
    return 0;
}

// Critical sections and SRW locks (src/sync.c's lock words, the same protocol): a free lock taken, a lock released
// that no one sleeps on; anything else is the host's (it waits, it wakes).
enum { TEB_THREAD_ID = 0x48, CS_RECURSION = 12, CS_OWNER = 16, CS_WORD = 24 };
#define GFIELD(p, off, type) (*(volatile type *)((uint8_t *)(p) + (off)))
static inline __attribute__((always_inline)) uint64_t gtid(const x86_ctx *c) { return *(volatile uint32_t *)(uintptr_t)(c->gs_base + TEB_THREAD_ID); }
static inline __attribute__((always_inline)) int gcs_try(x86_ctx *c, void *cs) {
    uint32_t tid = (uint32_t)gtid(c), *w = (uint32_t *)((uint8_t *)cs + CS_WORD);
    if (__atomic_load_n(w, __ATOMIC_RELAXED) && GFIELD(cs, CS_OWNER, uint32_t) == tid) { GFIELD(cs, CS_RECURSION, int32_t)++; return 1; }
    uint32_t free_ = 0;
    if (!__atomic_compare_exchange_n(w, &free_, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
    GFIELD(cs, CS_RECURSION, int32_t) = 1; GFIELD(cs, CS_OWNER, uint64_t) = tid;
    return 1;
}
GUEST int xv_g_cs_enter(x86_ctx *c, const void *d) { (void)d; return !gcs_try(c, (void *)(uintptr_t)garg(c, 0)); }
GUEST int xv_g_cs_try(x86_ctx *c, const void *d) { (void)d; c->gpr[0] = gcs_try(c, (void *)(uintptr_t)garg(c, 0)); return 0; }
GUEST int xv_g_cs_leave(x86_ctx *c, const void *d) {
    (void)d;
    void *cs = (void *)(uintptr_t)garg(c, 0);
    int32_t r = GFIELD(cs, CS_RECURSION, int32_t);
    if (r > 1) { GFIELD(cs, CS_RECURSION, int32_t) = r - 1; return 0; }
    uint32_t *w = (uint32_t *)((uint8_t *)cs + CS_WORD), held = 1;
    if (__atomic_load_n(w, __ATOMIC_RELAXED) != 1) return 1;          // (sleepers: the host wakes one)
    uint64_t owner = GFIELD(cs, CS_OWNER, uint64_t);
    GFIELD(cs, CS_RECURSION, int32_t) = 0; GFIELD(cs, CS_OWNER, uint64_t) = 0;
    if (__atomic_compare_exchange_n(w, &held, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return 0;
    GFIELD(cs, CS_RECURSION, int32_t) = r; GFIELD(cs, CS_OWNER, uint64_t) = owner;   // (one came to sleep meanwhile)
    return 1;
}
static inline __attribute__((always_inline)) int gsrw_try(uint32_t *w, int shared) {
    uint32_t cur = __atomic_load_n(w, __ATOMIC_RELAXED);
    if (cur & 1) return 0;
    if (!shared && cur >> 2) return 0;
    return __atomic_compare_exchange_n(w, &cur, shared ? cur + 4 : cur | 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
// (an acquire: done, or the host's to wait; a try: answered either way)
GUEST int xv_g_srw_acquire_exclusive(x86_ctx *c, const void *d) { (void)d; return !gsrw_try((uint32_t *)(uintptr_t)garg(c, 0), 0); }
GUEST int xv_g_srw_acquire_shared(x86_ctx *c, const void *d) { (void)d; return !gsrw_try((uint32_t *)(uintptr_t)garg(c, 0), 1); }
GUEST int xv_g_srw_try_exclusive(x86_ctx *c, const void *d) { (void)d; c->gpr[0] = gsrw_try((uint32_t *)(uintptr_t)garg(c, 0), 0); return 0; }
GUEST int xv_g_srw_try_shared(x86_ctx *c, const void *d) { (void)d; c->gpr[0] = gsrw_try((uint32_t *)(uintptr_t)garg(c, 0), 1); return 0; }
static inline __attribute__((always_inline)) int gsrw_release(uint32_t *w, int shared) {
    uint32_t cur = __atomic_load_n(w, __ATOMIC_RELAXED);
    for (;;) {
        if (cur & 2) return 1;                                        // (sleepers: the host)
        uint32_t neu = shared ? cur - 4 : cur & ~1u;
        if (__atomic_compare_exchange_n(w, &cur, neu, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return 0;
    }
}
GUEST int xv_g_srw_release_exclusive(x86_ctx *c, const void *d) { (void)d; return gsrw_release((uint32_t *)(uintptr_t)garg(c, 0), 0); }
GUEST int xv_g_srw_release_shared(x86_ctx *c, const void *d) { (void)d; return gsrw_release((uint32_t *)(uintptr_t)garg(c, 0), 1); }
