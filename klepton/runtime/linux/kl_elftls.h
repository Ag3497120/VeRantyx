// ELF TLS for glibc-built guests (Half-Life: Alyx's Linux ARM64 build).
//
// Android guests use emulated TLS, so upstream Klepton never needed this. GCC on
// aarch64-linux emits TLS descriptors (R_AARCH64_TLSDESC) for every shared-
// library TLS access:
//
//      adrp x0, :tlsdesc:var ; ldr x1, [x0, #lo12] ; add x0, x0, #lo12
//      blr  x1                          // resolver: x0 = &desc -> x0 = offset
//      mrs  x8, tpidr_el0               // rewritten to tpidrro_el0 at load
//      add  x0, x8, x0                  // the variable's address
//
// On Darwin the thread pointer the guest reads is TPIDRRO_EL0 (the TSD base),
// so the "offset" our resolver returns is simply (block address - tp), computed
// per thread. Each thread gets a lazily-allocated block per TLS module, found
// through a DTV hung off one pthread key (a Darwin TSD slot, read directly
// from tpidrro in the assembly fast path).
//
// Static-TLS relocations (R_AARCH64_TLS_TPREL64, initial-exec) need a constant
// offset from tp and cannot be served this way; they are refused by name.
#ifndef KL_ELFTLS_H
#define KL_ELFTLS_H
#include <stddef.h>
#include <stdint.h>

// One module per PT_TLS segment. Returns the module id (>= 1), or 0 on failure.
unsigned kl_tls_register(const void *init, size_t filesz, size_t memsz, size_t align);

// A TLS descriptor argument: what the resolver reads through desc[1].
typedef struct { uint64_t mod, off; } kl_tlsdesc_arg;

// The descriptor function the loader stores in desc[0]. Assembly; preserves
// every register but x0 and the flags, as the AArch64 TLSDESC ABI requires.
extern char kl_tlsdesc_resolver[];

// Address of (mod, off) in the calling thread's block, allocating it on first use.
void *kl_tls_addr(uint64_t mod, uint64_t off);

// glibc's __tls_get_addr, for general-dynamic code (tls_index = {mod, off}).
void *kl_tls_get_addr(const kl_tlsdesc_arg *ti);
#endif
