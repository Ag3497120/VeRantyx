// Probe (TRANSLATOR.md, phase 0): can a plain process run code on a Hypervisor.framework vCPU in TSO mode (ACTLR_EL1.EnTSO,
// macOS 15+), and what does one guest -> host -> guest round trip cost? Needs the com.apple.security.hypervisor
// entitlement (ad-hoc signing is enough): make build/hvprobe && build/hvprobe
#include <Hypervisor/Hypervisor.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define CHECK(x) do { hv_return_t r_ = (x); if (r_ != HV_SUCCESS) { printf("%s failed: %#x\n", #x, r_); return 1; } } while (0)
int main(void) {
    CHECK(hv_vm_create(NULL));
    size_t sz = 0x4000;
    uint32_t *code = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    // Guest code at guest physical 0x10000 (no MMU: EL1 with SCTLR.M = 0): loop { hvc #0 }.
    code[0] = 0xd4000002;          // hvc #0
    code[1] = 0x17ffffff;          // b . - 4
    CHECK(hv_vm_map(code, 0x10000, sz, HV_MEMORY_READ | HV_MEMORY_EXEC));
    hv_vcpu_t vcpu; hv_vcpu_exit_t *exit;
    CHECK(hv_vcpu_create(&vcpu, &exit, NULL));
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_PC, 0x10000));
    CHECK(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5));                 // EL1h, interrupts masked
    uint64_t actlr = 0;
    CHECK(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ACTLR_EL1, &actlr));
    printf("ACTLR_EL1 before: %#llx\n", actlr);
    CHECK(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_ACTLR_EL1, actlr | 2));   // EnTSO
    CHECK(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ACTLR_EL1, &actlr));
    printf("ACTLR_EL1 after:  %#llx (EnTSO %s)\n", actlr, (actlr & 2) ? "on" : "off");
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    int n = 200000, exits = 0;
    uint64_t t0 = mach_absolute_time();
    for (int i = 0; i < n; i++) {
        CHECK(hv_vcpu_run(vcpu));
        if (exit->reason == HV_EXIT_REASON_EXCEPTION) exits++;
        else { printf("exit reason %d\n", exit->reason); break; }
    }
    uint64_t t1 = mach_absolute_time();
    double ns = (double)(t1 - t0) * tb.numer / tb.denom / n;
    printf("%d exits, %.0f ns per hvc round trip (guest -> host -> guest)\n", exits, ns);
    return 0;
}
