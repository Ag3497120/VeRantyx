// Translated code in a Hypervisor.framework guest (vcpu.c, engine.c's xe_new_vcpu).
#pragma once
#include "xlat.h"
int xv_init(void);                                         // the VM (once per process); 0: none
// Runs the guest on the calling thread (its own vCPU) from `enter` with x0 = c until the exit (HVC #1); helpers
// and native functions run on the host meanwhile. 0, or -1 when the guest stopped where nothing can go on.
int xv_enter(x86_ctx *c, uint64_t enter, uint64_t vectors);
// Makes host memory [va, va + len) the guest's at the same address (else it is mapped when first touched).
int xv_map(uint64_t va, uint64_t len);
