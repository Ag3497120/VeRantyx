// ELF TLS for glibc guests — see kl_tls.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/mman.h>
#include "kl_tls.h"
#include "kl_env.h"

#define KL_TLS_MAX_MODULES 512
#define KL_TLS_BLOCK_BYTES (1u << 20)      // mmap'd per thread, committed lazily
#define KL_TLS_BLOCK_HDR   64              // block header (list links, atexit list)

struct kl_tls_module {
    const void *init;
    size_t filesz, memsz, align;
    uint64_t off;          // offset in the dynamic block, or in the fixed region
    int fixed;
    char name[96];
};

struct atexit_rec { struct atexit_rec *next; void (*dtor)(void *); void *obj; };
struct block { struct block *next, *prev; struct atexit_rec *atexit; uint64_t pad[5]; };

static kl_tls_module g_mods[KL_TLS_MAX_MODULES];
static unsigned g_nmods;
static uint64_t g_block_used = KL_TLS_BLOCK_HDR;
static uint64_t g_fixed_used;
static struct block *g_blocks;
static unsigned g_nblocks, g_blocks_peak;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_ready;                // 0 unclaimed, 1 ok, -1 failed
static char g_err[256];

static void err(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vsnprintf(g_err, sizeof g_err, fmt, a); va_end(a);
}
const char *kl_tls_error(void) { return g_err; }

static inline uint64_t *tsd(void) {
    uint64_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    return (uint64_t *)tp;
}
static inline uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// Claim one TSD slot as a real pthread key (never squat: see kl_x18.h). Darwin
// hands keys out lowest-free-first and never reissues one that is held, so
// creating keys until it yields `slot` walks up to it; the ones collected on
// the way are released again.
static int claim_slot(unsigned slot, void (*dtor)(void *)) {
    static pthread_key_t spare[768];
    unsigned nspare = 0;
    int got = 0;
    for (unsigned i = 0; i < 768; i++) {
        pthread_key_t k;
        if (pthread_key_create(&k, (unsigned)k == slot ? dtor : NULL) != 0) break;
        if ((unsigned)k == slot) {
            // Created without the destructor above (k was unknown before the
            // call); recreate with it when one is wanted.
            if (dtor) {
                pthread_key_delete(k);
                if (pthread_key_create(&k, dtor) != 0 || (unsigned)k != slot) break;
            }
            got = 1; break;
        }
        if ((unsigned)k > slot) { spare[nspare++] = k; break; }
        spare[nspare++] = k;
    }
    for (unsigned i = 0; i < nspare; i++) pthread_key_delete(spare[i]);
    return got ? 0 : -1;
}

static void block_destroy(void *p) {
    struct block *b = (struct block *)p;
    // Thread-exit destructors, most recently registered first, exactly as
    // glibc runs them. A destructor may register another; the list is drained.
    for (;;) {
        pthread_mutex_lock(&g_mu);
        struct atexit_rec *r = b->atexit;
        if (r) b->atexit = r->next;
        pthread_mutex_unlock(&g_mu);
        if (!r) break;
        r->dtor(r->obj);
        free(r);
    }
    pthread_mutex_lock(&g_mu);
    if (b->prev) b->prev->next = b->next; else g_blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    g_nblocks--;
    pthread_mutex_unlock(&g_mu);
    munmap(b, KL_TLS_BLOCK_BYTES);
}

static int init_once(void) {
    if (g_ready) return g_ready > 0 ? 0 : -1;
    uint64_t tp = (uint64_t)(uintptr_t)tsd();
    if (tp & 7) { err("thread pointer %#llx is not 8-aligned", (unsigned long long)tp); g_ready = -1; return -1; }
    if (claim_slot(KL_TLS_BLOCK_SLOT, block_destroy) != 0) {
        err("TSD slot %d (TLS block) is not available in this process", KL_TLS_BLOCK_SLOT);
        g_ready = -1; return -1;
    }
    for (unsigned i = 0; i < KL_TLS_FIXED_SLOTS; i++)
        if (claim_slot(KL_TLS_FIXED_SLOT + i, NULL) != 0) {
            err("TSD slot %u (fixed TLS) is not available in this process", KL_TLS_FIXED_SLOT + i);
            g_ready = -1; return -1;
        }
    g_ready = 1;
    return 0;
}

// Allocate (if needed) the calling thread's dynamic block, with every module
// registered so far initialised. Caller holds g_mu.
static struct block *block_get_locked(void) {
    uint64_t *t = tsd();
    struct block *b = (struct block *)t[KL_TLS_BLOCK_SLOT];
    if (b) return b;
    b = mmap(NULL, KL_TLS_BLOCK_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (b == MAP_FAILED) return NULL;
    for (unsigned i = 0; i < g_nmods; i++)
        if (!g_mods[i].fixed && g_mods[i].filesz)
            memcpy((char *)b + g_mods[i].off, g_mods[i].init, g_mods[i].filesz);
    b->next = g_blocks; b->prev = NULL;
    if (g_blocks) g_blocks->prev = b;
    g_blocks = b;
    g_nblocks++;
    if (g_nblocks > g_blocks_peak) g_blocks_peak = g_nblocks;
    // pthread_setspecific writes the same slot and lets the key's destructor
    // see the value at thread exit.
    pthread_setspecific((pthread_key_t)KL_TLS_BLOCK_SLOT, b);
    return b;
}

static void fixed_init_thread_locked(void) {
    char *base = (char *)tsd() + KL_TLS_FIXED_SLOT * 8;
    for (unsigned i = 0; i < g_nmods; i++) {
        if (!g_mods[i].fixed) continue;
        if (g_mods[i].filesz) memcpy(base + g_mods[i].off, g_mods[i].init, g_mods[i].filesz);
        memset(base + g_mods[i].off + g_mods[i].filesz, 0, g_mods[i].memsz - g_mods[i].filesz);
    }
}

kl_tls_module *kl_tls_register(const char *path, const void *init, size_t filesz,
                               size_t memsz, size_t align, int fixed) {
    pthread_mutex_lock(&g_mu);
    if (init_once() != 0) { pthread_mutex_unlock(&g_mu); return NULL; }
    if (g_nmods == KL_TLS_MAX_MODULES) { err("too many TLS modules (%u)", g_nmods); pthread_mutex_unlock(&g_mu); return NULL; }
    if (align < 8) align = 8;
    if (filesz > memsz) filesz = memsz;
    kl_tls_module *m = &g_mods[g_nmods];
    memset(m, 0, sizeof *m);
    m->init = init; m->filesz = filesz; m->memsz = memsz; m->align = align; m->fixed = fixed;
    const char *b = path ? strrchr(path, '/') : NULL;
    snprintf(m->name, sizeof m->name, "%s", b ? b + 1 : (path ? path : "?"));
    if (fixed) {
        if (align > 16) { err("%s: TP-relative TLS with alignment %zu (max 16)", m->name, align); pthread_mutex_unlock(&g_mu); return NULL; }
        uint64_t off = align_up(g_fixed_used, align);
        if (off + memsz > KL_TLS_FIXED_SLOTS * 8) {
            err("%s: TP-relative TLS of %zu bytes does not fit the %d fixed slots (%llu used)",
                m->name, memsz, KL_TLS_FIXED_SLOTS * 8, (unsigned long long)g_fixed_used);
            pthread_mutex_unlock(&g_mu); return NULL;
        }
        m->off = off; g_fixed_used = off + memsz;
        g_nmods++;
        // This thread's copy now; every other thread's at its kl_thread_init.
        char *base = (char *)tsd() + KL_TLS_FIXED_SLOT * 8;
        if (filesz) memcpy(base + off, init, filesz);
        memset(base + off + filesz, 0, memsz - filesz);
    } else {
        uint64_t off = align_up(g_block_used, align);
        if (off + memsz > KL_TLS_BLOCK_BYTES) { err("%s: TLS block exhausted", m->name); pthread_mutex_unlock(&g_mu); return NULL; }
        m->off = off; g_block_used = off + memsz;
        g_nmods++;
        // Threads that already have a block get this module's initial image
        // too: nothing of the module has run yet, so the region is untouched.
        if (filesz)
            for (struct block *blk = g_blocks; blk; blk = blk->next)
                memcpy((char *)blk + off, init, filesz);
    }
    if (kl_env_on("KL_TRACE_TLS", 0))
        fprintf(stderr, "  [tls] %s: %zu bytes (%zu initialised, align %zu) at %s+%#llx\n",
                m->name, memsz, filesz, align, fixed ? "TP" : "block",
                (unsigned long long)(fixed ? KL_TLS_FIXED_SLOT * 8 + m->off : m->off));
    pthread_mutex_unlock(&g_mu);
    return m;
}

int kl_tls_tprel(kl_tls_module *m, uint64_t off, uint64_t *tp_off) {
    if (!m || !m->fixed) { err("TPREL64 against a module without TP-relative placement"); return -1; }
    *tp_off = KL_TLS_FIXED_SLOT * 8 + m->off + off;
    return 0;
}

void kl_tls_desc(kl_tls_module *m, uint64_t off, uint64_t slot[2]) {
    if (m->fixed) {
        slot[0] = (uint64_t)(uintptr_t)kl_tlsdesc_static;
        slot[1] = KL_TLS_FIXED_SLOT * 8 + m->off + off;
    } else {
        slot[0] = (uint64_t)(uintptr_t)kl_tlsdesc_dynamic;
        slot[1] = m->off + off;
    }
}

// The slow path of kl_tlsdesc_dynamic (kl_tls.S): this thread has no block yet.
uint64_t kl_tls_slow(const uint64_t *desc) {
    pthread_mutex_lock(&g_mu);
    struct block *b = block_get_locked();
    pthread_mutex_unlock(&g_mu);
    if (!b) {
        fprintf(stderr, "  [tls] out of memory for a thread's TLS block\n");
        abort();
    }
    return (uint64_t)(uintptr_t)((char *)b + desc[1]) - (uint64_t)(uintptr_t)tsd();
}

void *kl_tls_block(void) {
    pthread_mutex_lock(&g_mu);
    if (init_once() != 0) { pthread_mutex_unlock(&g_mu); return NULL; }
    struct block *b = block_get_locked();
    pthread_mutex_unlock(&g_mu);
    return b;
}

void kl_tls_thread_init(void) {
    pthread_mutex_lock(&g_mu);
    if (g_ready > 0) fixed_init_thread_locked();
    pthread_mutex_unlock(&g_mu);
}

int kl_tls_thread_atexit(void (*dtor)(void *), void *obj, void *dso) {
    (void)dso;
    struct atexit_rec *r = malloc(sizeof *r);
    if (!r) return -1;
    r->dtor = dtor; r->obj = obj;
    pthread_mutex_lock(&g_mu);
    struct block *b = g_ready > 0 || init_once() == 0 ? block_get_locked() : NULL;
    if (!b) { pthread_mutex_unlock(&g_mu); free(r); return -1; }
    r->next = b->atexit; b->atexit = r;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

void kl_tls_report(FILE *f) {
    pthread_mutex_lock(&g_mu);
    fprintf(f, "  [tls] %u modules, %llu bytes dynamic, %llu bytes TP-relative, %u live blocks (peak %u)\n",
            g_nmods, (unsigned long long)g_block_used, (unsigned long long)g_fixed_used,
            g_nblocks, g_blocks_peak);
    pthread_mutex_unlock(&g_mu);
}
