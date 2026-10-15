// Translated code in a Hypervisor.framework guest (phase 4, TRANSLATOR.md): the engine's code runs on a vCPU of
// this process's VM, one per thread, with x86's memory order in hardware (ACTLR_EL1.EnTSO) and page tables of our
// own (4 KB pages, so the program's address space can be what Windows has, low 4 GB included).
//
// Memory: the guest sees host memory at the same addresses. A page is put in the guest when the guest first
// touches it: the fault goes to the guest's vectors, which leave with HVC; the host checks that the address is
// mapped in this process, maps that 16 KB host page at a guest physical address of its own and writes the page
// table entries, and the guest goes on. The guest leaves with HVC for everything else too: the engine's exit
// (#1), a helper (#2: xlat_helper), a native function (#3: its thunk in x16).
#include "vcpu.h"
#include <Hypervisor/Hypervisor.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

enum { POOL_IPA = 0x10000000, POOL_BYTES = 64 << 20, DATA_IPA = 0x40000000, HOST_PAGE = 16384 };
enum { SET_BITS = 22 };                                     // host pages in the guest (open addressing, 4M)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *g_pool;                                     // page tables: host side of [POOL_IPA, + POOL_BYTES)
static size_t g_pool_used;
static uint64_t *g_l0, g_l0_ipa;
static uint64_t g_next_ipa = DATA_IPA, g_max_ipa;
static uint64_t *g_mapped;                                  // host pages in the guest (0: empty)
static int g_ok;

static uint64_t *table_new(uint64_t *ipa) {
    if (g_pool_used + 4096 > POOL_BYTES) { fprintf(stderr, "[vxrt] vcpu: out of page tables\n"); abort(); }
    uint64_t *t = (uint64_t *)(g_pool + g_pool_used);
    *ipa = POOL_IPA + g_pool_used;
    g_pool_used += 4096;
    memset(t, 0, 4096);
    return t;
}
static uint64_t *table_at(uint64_t ipa) { return (uint64_t *)(g_pool + (ipa - POOL_IPA)); }

int xv_init(void) {
    static int done;
    pthread_mutex_lock(&g_lock);
    if (done) { pthread_mutex_unlock(&g_lock); return g_ok; }
    done = 1;
    hv_vm_config_t cfg = hv_vm_config_create();
    uint32_t bits = 0;
    hv_vm_config_get_max_ipa_size(&bits);
    hv_vm_config_set_ipa_size(cfg, bits);
    g_max_ipa = 1ull << bits;
    if (hv_vm_create(cfg) != HV_SUCCESS) { pthread_mutex_unlock(&g_lock); return 0; }
    g_pool = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    g_mapped = mmap(NULL, sizeof(uint64_t) << SET_BITS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_pool == MAP_FAILED || g_mapped == MAP_FAILED || hv_vm_map(g_pool, POOL_IPA, POOL_BYTES, HV_MEMORY_READ | HV_MEMORY_WRITE) != HV_SUCCESS) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    g_l0 = table_new(&g_l0_ipa);
    uint8_t *parity = mmap(NULL, HOST_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);   // (the binary's
    memcpy(parity, xlat_parity, 256);                                                                     //  own pages stay out)
    xlat_guest_parity = parity;
    g_ok = 1;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

// A 4 KB guest page at va -> ipa (writable or read-only for the guest).
static void map4k(uint64_t va, uint64_t ipa, int writable) {
    uint64_t *t = g_l0;
    for (int level = 0; level < 3; level++) {
        uint64_t *e = &t[va >> (39 - 9 * level) & 511];
        if (!(*e & 1)) { uint64_t tipa; table_new(&tipa); *e = tipa | 3; }
        t = table_at(*e & 0x0000FFFFFFFFF000ull);
    }
    // valid page | AF | inner shareable | AttrIndx 0 (normal, write-back) | AP[2]: read-only
    t[va >> 12 & 511] = ipa | 3 | 1u << 10 | 3u << 8 | (writable ? 0 : 1u << 7);
}
static uint32_t set_slot(uint64_t page) { return (uint32_t)((page >> 14) * 0x9E3779B97F4A7C15ull >> (64 - SET_BITS)); }

// The host page holding va into the guest, if this process has it mapped. 1: in the guest (now or already).
static int map_host_page(uint64_t va) {
    uint64_t page = va & ~(uint64_t)(HOST_PAGE - 1);
    pthread_mutex_lock(&g_lock);
    uint32_t s = set_slot(page);
    while (g_mapped[s] && g_mapped[s] != page) s = (s + 1) & ((1u << SET_BITS) - 1);
    if (g_mapped[s]) { pthread_mutex_unlock(&g_lock); return 1; }
    mach_vm_address_t a = page; mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj) != KERN_SUCCESS ||
        a > page || !(info.protection & VM_PROT_READ) || (info.protection & VM_PROT_EXECUTE) || g_next_ipa + HOST_PAGE > g_max_ipa) {
        // (never an executable host page: macOS panics, SPTM)
        pthread_mutex_unlock(&g_lock); return 0;
    }
    int writable = (info.protection & VM_PROT_WRITE) != 0;
    uint64_t ipa = g_next_ipa;
    hv_memory_flags_t f = HV_MEMORY_READ | (writable ? HV_MEMORY_WRITE : 0);
    if (hv_vm_map((void *)(uintptr_t)page, ipa, HOST_PAGE, f | HV_MEMORY_EXEC) != HV_SUCCESS) { pthread_mutex_unlock(&g_lock); return 0; }
    g_next_ipa += HOST_PAGE;
    for (int k = 0; k < HOST_PAGE / 4096; k++) map4k(page + 4096 * (uint64_t)k, ipa + 4096 * (uint64_t)k, writable);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_mapped[s] = page;
    pthread_mutex_unlock(&g_lock);
    return 1;
}
int xv_map(uint64_t va, uint64_t len) {
    for (uint64_t p = va & ~(uint64_t)(HOST_PAGE - 1); p < va + len; p += HOST_PAGE) if (!map_host_page(p)) return 0;
    return 1;
}

// ---- vCPUs ------------------------------------------------------------------------------------------------------
static __thread hv_vcpu_t t_vcpu;
static __thread hv_vcpu_exit_t *t_exit;
static __thread int t_have, t_depth;
static int vcpu_setup(uint64_t vectors) {
    if (hv_vcpu_create(&t_vcpu, &t_exit, NULL) != HV_SUCCESS) return 0;
    hv_vcpu_set_sys_reg(t_vcpu, HV_SYS_REG_MAIR_EL1, 0xFF);                 // attribute 0: normal memory, write-back
    // T0SZ 16 (48-bit addresses), walks write-back inner shareable, 4 KB granule, no TTBR1 walks, 40-bit IPS
    hv_vcpu_set_sys_reg(t_vcpu, HV_SYS_REG_TCR_EL1, 16 | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23 | 2ull << 32);
    hv_vcpu_set_sys_reg(t_vcpu, HV_SYS_REG_TTBR0_EL1, g_l0_ipa);
    hv_vcpu_set_sys_reg(t_vcpu, HV_SYS_REG_SCTLR_EL1, 0x30D00800u | 1 | 4 | 0x1000);   // MMU, data and instruction caches
    hv_vcpu_set_sys_reg(t_vcpu, HV_SYS_REG_CPACR_EL1, 3u << 20);           // FP and SIMD at EL1
    hv_vcpu_set_sys_reg(t_vcpu, HV_SYS_REG_VBAR_EL1, vectors);
    if (!xv_map(vectors, 2048)) return 0;                                   // (else a fault would find no vectors: a loop)
    if (__builtin_available(macOS 15.0, *)) {                               // EnTSO: x86's memory order
        uint64_t actlr = 0;
        hv_vcpu_get_sys_reg(t_vcpu, HV_SYS_REG_ACTLR_EL1, &actlr);
        hv_vcpu_set_sys_reg(t_vcpu, HV_SYS_REG_ACTLR_EL1, actlr | 2);
    }
    hv_vcpu_set_vtimer_mask(t_vcpu, true);
    t_have = 1;
    return 1;
}

typedef void (*native_fn)(x86_ctx *);
#define DBG(...) do { if (getenv("XV_DEBUG")) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
int xv_enter(x86_ctx *c, uint64_t enter, uint64_t vectors) {
    DBG("xv_enter ctx %p enter %#llx vectors %#llx", (void *)c, (unsigned long long)enter, (unsigned long long)vectors);
    if (!t_have && !vcpu_setup(vectors)) { fprintf(stderr, "[vxrt] vcpu: cannot make a vCPU\n"); return -1; }
    // A call from the host into x86 code while the guest is in a call to the host (a callback): the guest's
    // registers of the outer run come back afterwards.
    uint64_t saved[33], fpcr = 0;
    int depth = t_depth++;
    if (depth) {
        for (int r = 0; r < 31; r++) hv_vcpu_get_reg(t_vcpu, (hv_reg_t)(HV_REG_X0 + r), &saved[r]);
        hv_vcpu_get_reg(t_vcpu, HV_REG_PC, &saved[31]); hv_vcpu_get_reg(t_vcpu, HV_REG_CPSR, &saved[32]);
        hv_vcpu_get_reg(t_vcpu, HV_REG_FPCR, &fpcr);
    }
    hv_vcpu_set_reg(t_vcpu, HV_REG_X0, (uint64_t)(uintptr_t)c);
    hv_vcpu_set_reg(t_vcpu, HV_REG_PC, enter);
    hv_vcpu_set_reg(t_vcpu, HV_REG_CPSR, 0x3c5);                           // EL1h, interrupts masked
    int result = -1;
    for (;;) {
        DBG("run");
        if (hv_vcpu_run(t_vcpu) != HV_SUCCESS) { fprintf(stderr, "[vxrt] vcpu: run failed\n"); break; }
        DBG("exit reason %d", (int)t_exit->reason);
        if (t_exit->reason != HV_EXIT_REASON_EXCEPTION) continue;            // (canceled, timer: on)
        uint64_t syndrome = t_exit->exception.syndrome, ec = syndrome >> 26;
        static int trace = -1, shown;
        if (trace < 0) trace = getenv("XV_TRACE") ? atoi(getenv("XV_TRACE")) : 0;
        if (trace && shown < trace) {
            uint64_t pc = 0, esr = 0, far = 0; hv_vcpu_get_reg(t_vcpu, HV_REG_PC, &pc);
            hv_vcpu_get_sys_reg(t_vcpu, HV_SYS_REG_ESR_EL1, &esr); hv_vcpu_get_sys_reg(t_vcpu, HV_SYS_REG_FAR_EL1, &far);
            fprintf(stderr, "vcpu exit: syndrome %#llx pc %#llx esr_el1 %#llx far %#llx\n", (unsigned long long)syndrome, (unsigned long long)pc, (unsigned long long)esr, (unsigned long long)far);
            shown++;
        }
        if (ec == 0x16) {                                                     // HVC
            uint32_t imm = (uint32_t)(syndrome & 0xFFFF);
            if (imm == XV_HVC_EXIT) { result = 0; break; }
            if (imm == XV_HVC_HELPER) { xlat_helper(c); continue; }
            if (imm == XV_HVC_NATIVE) {
                uint64_t fn; hv_vcpu_get_reg(t_vcpu, HV_REG_X16, &fn);
                ((native_fn)(uintptr_t)fn)(c);
                continue;
            }
            if (imm >= XV_HVC_VECTOR) {                                       // an exception in the guest
                uint64_t esr, far, elr, spsr;
                hv_vcpu_get_sys_reg(t_vcpu, HV_SYS_REG_ESR_EL1, &esr);
                hv_vcpu_get_sys_reg(t_vcpu, HV_SYS_REG_FAR_EL1, &far);
                hv_vcpu_get_sys_reg(t_vcpu, HV_SYS_REG_ELR_EL1, &elr);
                hv_vcpu_get_sys_reg(t_vcpu, HV_SYS_REG_SPSR_EL1, &spsr);
                uint64_t eec = esr >> 26, fsc = esr & 0x3F;
                int abort_ = eec == 0x20 || eec == 0x21 || eec == 0x24 || eec == 0x25;
                if (abort_ && (fsc & 0x3C) == 0x04 && map_host_page(far)) {   // translation fault: the page now
                    hv_vcpu_set_reg(t_vcpu, HV_REG_PC, elr);
                    hv_vcpu_set_reg(t_vcpu, HV_REG_CPSR, spsr);
                    continue;
                }
                fprintf(stderr, "[vxrt] vcpu: exception in the guest: vector %u, ESR %#llx, FAR %#llx, ELR %#llx\n",
                        imm - XV_HVC_VECTOR, (unsigned long long)esr, (unsigned long long)far, (unsigned long long)elr);
                break;
            }
        }
        uint64_t pc = 0; hv_vcpu_get_reg(t_vcpu, HV_REG_PC, &pc);
        fprintf(stderr, "[vxrt] vcpu: exit to the host: syndrome %#llx at %#llx, address %#llx\n",
                (unsigned long long)syndrome, (unsigned long long)pc, (unsigned long long)t_exit->exception.virtual_address);
        break;
    }
    if (depth) {
        for (int r = 0; r < 31; r++) hv_vcpu_set_reg(t_vcpu, (hv_reg_t)(HV_REG_X0 + r), saved[r]);
        hv_vcpu_set_reg(t_vcpu, HV_REG_PC, saved[31]); hv_vcpu_set_reg(t_vcpu, HV_REG_CPSR, saved[32]);
        hv_vcpu_set_reg(t_vcpu, HV_REG_FPCR, fpcr);
    }
    t_depth--;
    return result;
}
