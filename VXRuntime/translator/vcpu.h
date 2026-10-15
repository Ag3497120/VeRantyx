// Translated code in a Hypervisor.framework guest (vcpu.c, engine.c's xe_new_vcpu).
#pragma once
#include "xlat.h"
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
void xv_unmap(uint64_t va, uint64_t len);                   // host memory going away: out of the guest
void xv_protect(uint64_t page, int read_only);             // a host page's guest mappings read-only (SMC) or not
void xv_set_tlbi(uint64_t stub);                            // guest code: TLBI, DSB, ISB, HVC #XV_HVC_TLBI
// Makes host memory [va, va + len) the guest's at the same address (else it is mapped when first touched).
int xv_map(uint64_t va, uint64_t len);
