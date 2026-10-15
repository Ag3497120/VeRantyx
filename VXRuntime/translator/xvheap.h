// The program's small heap blocks in the ARM64 runtime, one allocator for the vCPU guest (guest.c answers malloc,
// HeapAlloc ... without leaving the guest) and the host (src/heap_arm64.c, the same functions when called there):
// size classes up to 2 KB carved from one large reserved range (pages come when touched), each class a lock-free
// list of freed blocks (a 16-bit tag against ABA in the pointer's top bits). Larger blocks are the host's malloc;
// what is in the range decides which one a pointer belongs to. Inline only: each user keeps the code in its own
// section.
#ifndef XVHEAP_H
#define XVHEAP_H
#include <stdint.h>

enum { XH_CLASSES = 24, XH_MAX = 2048, XH_HEADER = 16, XH_MAGIC = 0x48454150 };
typedef struct {
    uint64_t head[XH_CLASSES];       // freed blocks: pointer | tag << 48
    uint64_t cursor, limit, base;    // carving: the next free byte, the end of the range, its start
} xh_heap;

#define XH_INLINE static inline __attribute__((always_inline))
// (Arithmetic, not a table: the guest's code may read no constants of the host's.) 16..128 by 16, then four steps
// in each doubling: 160, 192, 224, 256, 320 ... 2048.
XH_INLINE uint32_t xh_size(uint32_t k) {
    if (k < 8) return 16 * (k + 1);
    uint32_t base = 128u << ((k - 8) / 4);
    return base + ((k - 8) % 4 + 1) * (base / 4);
}
XH_INLINE uint32_t xh_class(uint64_t n) {
    if (n <= 128) return n ? (uint32_t)(n - 1) / 16 : 0;
    uint32_t g = 63 - (uint32_t)__builtin_clzll(n - 1) - 7, base = 128u << g;
    return 8 + 4 * g + (uint32_t)(n - base - 1) / (base / 4);
}
XH_INLINE int xh_owns(const xh_heap *h, const void *p) {
    return (uint64_t)(uintptr_t)p >= h->base && (uint64_t)(uintptr_t)p < h->limit;
}
// A block of n bytes (16-byte aligned), or NULL: too large, or the range is used up (the host's malloc then).
XH_INLINE void *xh_alloc(xh_heap *h, uint64_t n) {
    if (n > XH_MAX) return 0;
    uint32_t k = xh_class(n);
    uint64_t old = __atomic_load_n(&h->head[k], __ATOMIC_ACQUIRE);
    while (old & 0xFFFFFFFFFFFFull) {
        uint64_t *p = (uint64_t *)(uintptr_t)(old & 0xFFFFFFFFFFFFull);
        uint64_t next = (*(volatile uint64_t *)p & 0xFFFFFFFFFFFFull) | ((old >> 48) + 1) << 48;
        if (__atomic_compare_exchange_n(&h->head[k], &old, next, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return p;
    }
    uint64_t at = __atomic_fetch_add(&h->cursor, XH_HEADER + xh_size(k), __ATOMIC_RELAXED);
    if (at + XH_HEADER + xh_size(k) > h->limit) return 0;
    uint32_t *hdr = (uint32_t *)(uintptr_t)at;
    hdr[0] = k; hdr[1] = XH_MAGIC;
    return (uint8_t *)(uintptr_t)at + XH_HEADER;
}
XH_INLINE uint32_t xh_block_size(const void *p) { return xh_size(((const uint32_t *)p - 4)[0]); }
// Frees a block of the range (1), or 0: not one of ours.
XH_INLINE int xh_free(xh_heap *h, void *p) {
    if (!xh_owns(h, p)) return 0;
    uint32_t k = ((const uint32_t *)p - 4)[0];
    uint64_t old = __atomic_load_n(&h->head[k], __ATOMIC_ACQUIRE);
    for (;;) {
        *(volatile uint64_t *)p = old & 0xFFFFFFFFFFFFull;
        uint64_t neu = (uint64_t)(uintptr_t)p | ((old >> 48) + 1) << 48;
        if (__atomic_compare_exchange_n(&h->head[k], &old, neu, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
    }
}
XH_INLINE void xh_zero(void *p, uint64_t n) {
    volatile uint64_t *w = p;
    for (uint64_t i = 0; i < (n + 7) / 8; i++) w[i] = 0;
}
XH_INLINE void xh_copy(void *to, const void *from, uint64_t n) {
    volatile uint8_t *t = to; const uint8_t *f = from;
    for (uint64_t i = 0; i < n; i++) t[i] = f[i];
}
#endif
