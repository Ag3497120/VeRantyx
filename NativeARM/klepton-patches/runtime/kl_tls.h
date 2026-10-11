// ELF thread-local storage for guests that use it (glibc-built libraries).
//
// Android guests reach TLS through emutls or bionic's slot table, so Klepton's
// loader had no PT_TLS handling and knew no TLS relocation. A glibc guest is
// different: every `thread_local` and `__thread` lands in a PT_TLS segment and
// is reached through R_AARCH64_TLSDESC (a resolver call) or, for the
// initial-exec model, R_AARCH64_TLS_TPREL64 (a fixed offset from the thread
// pointer). Both are answered here.
//
// The thread pointer the guest reads is Darwin's TSD base (TPIDRRO_EL0, after
// the loader's tpidr_el0 rewrite), so a fixed TP-relative offset has to land
// inside the TSD array. A handful of slots (KL_TLS_FIXED_SLOT ...) is claimed
// for the few modules that need that; everything else lives in a per-thread
// block allocated on first touch, whose address sits in KL_TLS_BLOCK_SLOT, and
// the TLSDESC resolver returns (block + offset - TP).
#ifndef KL_TLS_H
#define KL_TLS_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct kl_tls_module kl_tls_module;

// Darwin TSD slots, claimed as real pthread keys the way kl_x18.h claims 500.
#define KL_TLS_BLOCK_SLOT   501
#define KL_TLS_FIXED_SLOT   504
#define KL_TLS_FIXED_SLOTS  8        // 64 bytes of TP-relative TLS, for TPREL64 modules

// Register a module's PT_TLS: its initialisation image, how many bytes of it
// are in the file, the full size and alignment. `fixed` asks for TP-relative
// placement (needed when the module has TPREL64 relocations). NULL on failure;
// kl_tls_error() says why.
kl_tls_module *kl_tls_register(const char *path, const void *init, size_t filesz,
                               size_t memsz, size_t align, int fixed);
const char *kl_tls_error(void);

// The value of an R_AARCH64_TLS_TPREL64 slot: the TP-relative offset of
// `off` bytes into the module's block. Fails for a module that was not
// registered as fixed.
int  kl_tls_tprel(kl_tls_module *m, uint64_t off, uint64_t *tp_off);
// Fill an R_AARCH64_TLSDESC GOT pair (resolver, argument) for `off` bytes into
// the module's block.
void kl_tls_desc(kl_tls_module *m, uint64_t off, uint64_t slot[2]);

// Per-thread: initialise the fixed slots for the calling thread. Called from
// kl_thread_init (every guest thread, and the host's main thread).
void kl_tls_thread_init(void);
// glibc's __cxa_thread_atexit_impl: run `dtor(obj)` when the calling thread
// exits. Returns 0.
int  kl_tls_thread_atexit(void (*dtor)(void *), void *obj, void *dso);
// The dynamic block of the calling thread, created if needed (test aid).
void *kl_tls_block(void);
void kl_tls_report(FILE *f);

// The resolvers the GOT pairs point at (kl_tls.S).
extern void kl_tlsdesc_static(void);
extern void kl_tlsdesc_dynamic(void);
#endif
