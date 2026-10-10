// ELF TLS for glibc-built guests. See kl_elftls.h.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kl_elftls.h"

#define KL_TLS_MAX_MODS 256

typedef struct { const void *init; size_t filesz, memsz, align; } tls_mod;
static tls_mod g_mods[KL_TLS_MAX_MODS];
static unsigned g_nmods;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

// Read by the assembly fast path: the TSD slot holding this thread's DTV.
uint64_t g_kl_tls_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void dtv_free(void *p) {
    void **dtv = p;
    for (unsigned i = 1; i < KL_TLS_MAX_MODS; i++) free(dtv[i]);
    free(dtv);
}
static void key_init(void) {
    pthread_key_t k;
    if (pthread_key_create(&k, dtv_free) != 0) {
        fprintf(stderr, "[klepton] ELF TLS: pthread_key_create failed\n");
        abort();
    }
    g_kl_tls_key = (uint64_t)k;
}

unsigned kl_tls_register(const void *init, size_t filesz, size_t memsz, size_t align) {
    pthread_once(&g_once, key_init);
    pthread_mutex_lock(&g_lock);
    unsigned id = 0;
    if (g_nmods + 1 < KL_TLS_MAX_MODS) {
        id = ++g_nmods;
        g_mods[id] = (tls_mod){init, filesz, memsz, align ? align : 16};
    }
    pthread_mutex_unlock(&g_lock);
    if (!id) fprintf(stderr, "[klepton] ELF TLS: more than %d TLS modules\n", KL_TLS_MAX_MODS - 1);
    return id;
}

void *kl_tls_addr(uint64_t mod, uint64_t off) {
    pthread_once(&g_once, key_init);
    void **dtv = pthread_getspecific((pthread_key_t)g_kl_tls_key);
    if (!dtv) {
        dtv = calloc(KL_TLS_MAX_MODS, sizeof(void *));
        pthread_setspecific((pthread_key_t)g_kl_tls_key, dtv);
    }
    if (mod == 0 || mod >= KL_TLS_MAX_MODS) {
        fprintf(stderr, "[klepton] ELF TLS: bad module %llu\n", (unsigned long long)mod);
        abort();
    }
    if (!dtv[mod]) {
        pthread_mutex_lock(&g_lock);
        tls_mod m = g_mods[mod];
        pthread_mutex_unlock(&g_lock);
        size_t al = m.align < 16 ? 16 : m.align;
        void *blk = NULL;
        if (posix_memalign(&blk, al, m.memsz ? m.memsz : 1) != 0) abort();
        if (m.filesz) memcpy(blk, m.init, m.filesz);
        memset((char *)blk + m.filesz, 0, m.memsz - m.filesz);
        dtv[mod] = blk;
    }
    return (char *)dtv[mod] + off;
}

// Slow path of the assembly resolver: every caller-saved register is already
// spilled by the caller of this function.
void *kl_tls_desc_slow(const kl_tlsdesc_arg *a) { return kl_tls_addr(a->mod, a->off); }

void *kl_tls_get_addr(const kl_tlsdesc_arg *ti) { return kl_tls_addr(ti->mod, ti->off); }
