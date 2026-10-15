// Translated code in a Hypervisor.framework guest (vcpu.c, engine.c's xe_new_vcpu).
#pragma once
#include "xlat.h"
// A native function whose calls may be recorded in the guest and run at its next exit (no result): its thunk,
// its argument count, and the arrays its arguments point to (argument `arg`, `count` elements of `elem` bytes, the
// count in argument count_arg when >= 0; more than max: not recorded).
enum { XV_DEFER_ARGS = 12 };
typedef struct { int8_t arg, count_arg; uint16_t elem; uint32_t count, max; } xv_copy;
typedef struct { uint64_t thunk; uint32_t nargs, ncopy; xv_copy copy[4]; } xv_defer;
int xv_guest_defer(x86_ctx *c, const xv_defer *d);          // (guest.c: runs in the guest)
typedef int (*xv_guest_fn)(x86_ctx *c, const void *desc);   // guest.c's answers: 0 done (RAX), 1 the host's instead
int xv_g_tls_get(x86_ctx *, const void *); int xv_g_tls_set(x86_ctx *, const void *);
int xv_g_get_last_error(x86_ctx *, const void *); int xv_g_set_last_error(x86_ctx *, const void *);
int xv_g_qpc(x86_ctx *, const void *); int xv_g_qpf(x86_ctx *, const void *);
int xv_g_tick64(x86_ctx *, const void *); int xv_g_tick(x86_ctx *, const void *);
int xv_g_memcpy(x86_ctx *, const void *); int xv_g_memmove(x86_ctx *, const void *); int xv_g_memset(x86_ctx *, const void *);
int xv_g_memcmp(x86_ctx *, const void *); int xv_g_strlen(x86_ctx *, const void *); int xv_g_wcslen(x86_ctx *, const void *);
int xv_g_malloc(x86_ctx *, const void *); int xv_g_calloc(x86_ctx *, const void *); int xv_g_free(x86_ctx *, const void *);
int xv_g_realloc(x86_ctx *, const void *); int xv_g_msize(x86_ctx *, const void *); int xv_g_heap_alloc(x86_ctx *, const void *);
int xv_g_heap_free(x86_ctx *, const void *); int xv_g_heap_size(x86_ctx *, const void *); int xv_g_heap_realloc(x86_ctx *, const void *);
int xv_g_cs_enter(x86_ctx *, const void *); int xv_g_cs_try(x86_ctx *, const void *); int xv_g_cs_leave(x86_ctx *, const void *);
int xv_g_srw_acquire_exclusive(x86_ctx *, const void *); int xv_g_srw_acquire_shared(x86_ctx *, const void *);
int xv_g_srw_try_exclusive(x86_ctx *, const void *); int xv_g_srw_try_shared(x86_ctx *, const void *);
int xv_g_srw_release_exclusive(x86_ctx *, const void *); int xv_g_srw_release_shared(x86_ctx *, const void *);
int xv_g_const(x86_ctx *, const void *); int xv_g_local_alloc(x86_ctx *, const void *); int xv_g_local_free(x86_ctx *, const void *);
void xv_guest_code(const uint8_t **code, size_t *size);     // guest.c's code, to copy into the guest
int xv_init(void);                                         // the VM (once per process); 0: none
// Runs the guest on the calling thread (its own vCPU) from `enter` with x0 = c until the exit (HVC #1); helpers
// and native functions run on the host meanwhile. 0, or -1 when the guest stopped where nothing can go on.
int xv_enter(x86_ctx *c, uint64_t enter, uint64_t vectors);
// XV_FAULT: the guest touched memory the process does not have (or may not write): the x86 registers are in c, and
// xv_fault gives the host address of the instruction, the address touched and whether it wrote.
enum { XV_FAULT = -3 };
void xv_fault(uint64_t *pc, uint64_t *addr, int *write);
// After a longjmp out of nested runs (an exception resumed further out): the nesting depth as it is again.
int xv_depth(void);
void xv_set_depth(int depth);
void xv_set_low(uint64_t base);                              // guest addresses below 4 GB: the host's at base + them
void xv_unmap(uint64_t va, uint64_t len);                   // host memory going away: out of the guest
void xv_protect(uint64_t page, int read_only);             // a host page's guest mappings read-only (SMC) or not
// SuspendThread (src/sync.c's signal, on the thread itself): a thread in the guest leaves it, and the hook gets the
// guest's registers (between runs: r NULL, the context holds the state); xv_interrupt from the signal handler: 1 if
// the thread was in the guest (the hook runs as it leaves), 0 (stop it where it is).
typedef struct { uint64_t x[31], pc; uint8_t q[16][16]; } xv_regs;
void xv_set_interrupt_hook(void (*hook)(x86_ctx *c, const xv_regs *r));
int xv_interrupt(void);
void xv_set_write_hook(int (*hook)(uint64_t pc, uint64_t addr));   // a write fault the program did not make: 1 handled
void xv_set_tlbi(uint64_t stub);                            // guest code: TLBI, DSB, ISB, HVC #XV_HVC_TLBI
// Makes host memory [va, va + len) the guest's at the same address (else it is mapped when first touched).
int xv_map(uint64_t va, uint64_t len);
