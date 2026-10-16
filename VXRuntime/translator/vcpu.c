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
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>

enum { POOL_IPA = 0x10000000, POOL_BYTES = 64 << 20, DATA_IPA = 0x40000000, HOST_PAGE = 16384 };
enum { SET_BITS = 22 };                                     // host pages in the guest (open addressing, 4M)

static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;   // (hv_vm_map may free: the release hook)
static uint8_t *g_pool;                                     // page tables: host side of [POOL_IPA, + POOL_BYTES)
static size_t g_pool_used;
static uint64_t *g_l0, g_l0_ipa;
static uint64_t g_next_ipa = DATA_IPA, g_max_ipa;
static uint64_t *g_mapped, *g_ipa;                          // host pages in the guest (0: empty, page | 1: gone) and where
static uint64_t g_low_base;                                 // the guest's low 4 GB: the host's at this offset (xv_set_low)
static volatile uint64_t g_gen;                             // page tables changed for the worse: TLBs to flush
static uint64_t g_tlbi;                                     // guest code: TLBI VMALLE1; DSB; ISB; HVC #4 (xv_set_tlbi)
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
    g_ipa = mmap(NULL, sizeof(uint64_t) << SET_BITS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
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
static int64_t set_find(uint64_t page) {                    // (with g_lock)
    for (uint32_t s = set_slot(page); g_mapped[s]; s = (s + 1) & ((1u << SET_BITS) - 1)) if (g_mapped[s] == page) return s;
    return -1;
}
static uint64_t *pte(uint64_t va) {                          // the last-level entry of va, NULL when there is none
    uint64_t *t = g_l0;
    for (int level = 0; level < 3; level++) {
        uint64_t e = t[va >> (39 - 9 * level) & 511];
        if (!(e & 1)) return NULL;
        t = table_at(e & 0x0000FFFFFFFFF000ull);
    }
    return &t[va >> 12 & 511];
}
static int (*g_write_hook)(uint64_t pc, uint64_t addr);
void xv_set_write_hook(int (*hook)(uint64_t, uint64_t)) { g_write_hook = hook; }
void xv_set_tlbi(uint64_t stub) { g_tlbi = stub; }
// Host memory going away (src/vm_hooks.c): out of the guest. Its guest physical addresses are never used again, so
// a translation a TLB still holds faults (stage 2) instead of reaching other memory; the TLBs are flushed after.
static void unmap_slot(uint64_t page, int64_t s) {         // (with g_lock)
    for (int k = 0; k < HOST_PAGE / 4096; k++) { uint64_t *p = pte(page + 4096 * (uint64_t)k); if (p) *p = 0; }
    hv_vm_unmap(g_ipa[s], HOST_PAGE);
    g_mapped[s] = page | 1;
    g_gen++;
}
void xv_unmap(uint64_t va, uint64_t len) {
    if (!g_ok) return;
    pthread_mutex_lock(&g_lock);
    for (uint64_t page = va & ~(uint64_t)(HOST_PAGE - 1); page < va + len; page += HOST_PAGE) {
        int64_t s = set_find(page);
        if (s >= 0) unmap_slot(page, s);
        if (g_low_base && page - g_low_base < (1ull << 32) && (s = set_find(page - g_low_base)) >= 0) unmap_slot(page - g_low_base, s);
    }
    pthread_mutex_unlock(&g_lock);
}
void xv_set_low(uint64_t base) { g_low_base = base; }
// Write-protects (or not) a host page's guest mappings (SMC: the engine's pages translated code came from).
void xv_protect(uint64_t page, int read_only) {
    pthread_mutex_lock(&g_lock);
    page &= ~(uint64_t)(HOST_PAGE - 1);
    if (set_find(page) >= 0) {
        for (int k = 0; k < HOST_PAGE / 4096; k++) {
            uint64_t *p = pte(page + 4096 * (uint64_t)k);
            if (p && (*p & 1)) *p = read_only ? *p | 1u << 7 : *p & ~(uint64_t)(1u << 7);
        }
        g_gen++;                                                    // (either way: a TLB holds the old permission)
    }
    pthread_mutex_unlock(&g_lock);
}

// The host page holding va into the guest, if this process has it mapped. 1: in the guest (now or already).
// A copy of a host page the guest may only read, of its 4 KB parts that can be read (bit k of *parts: part k).
static void *snapshot(uint64_t page, uint32_t *parts) {
    uint8_t *copy = mmap(NULL, HOST_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (copy == MAP_FAILED) return NULL;
    *parts = 0;
    for (int k = 0; k < HOST_PAGE / 4096; k++) {
        mach_vm_size_t got = 0;                                           // (no SIGBUS: a file's page past its end fails)
        if (mach_vm_read_overwrite(mach_task_self(), page + 4096 * (uint64_t)k, 4096, (mach_vm_address_t)(uintptr_t)(copy + 4096 * k), &got) == KERN_SUCCESS && got == 4096)
            *parts |= 1u << k;
    }
    if (!*parts) { munmap(copy, HOST_PAGE); return NULL; }
    return copy;
}
// A guest page below 4 GB is the host's page at g_low_base + its address (the guest's page tables say so; the host
// cannot map there).
static int map_host_page(uint64_t va) {
    uint64_t page = va & ~(uint64_t)(HOST_PAGE - 1), host = page;
    if (!(page >> 32)) { if (!g_low_base) return 0; host = g_low_base + page; }
    pthread_mutex_lock(&g_lock);
    if (set_find(page) >= 0) { pthread_mutex_unlock(&g_lock); return 1; }
    uint32_t s = set_slot(page);
    while (g_mapped[s] && !(g_mapped[s] & 1)) s = (s + 1) & ((1u << SET_BITS) - 1);   // (an empty slot or a gone one)
    mach_vm_address_t a = host; mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info; mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &obj) != KERN_SUCCESS ||
        a > host || !(info.protection & VM_PROT_READ) || g_next_ipa + HOST_PAGE > g_max_ipa) {
        pthread_mutex_unlock(&g_lock); return 0;
    }
    // (writable by its maximum protection: the SMC watch makes code pages read-only for a while, and the guest's own
    //  page tables say what it may do)
    int writable = (info.max_protection & VM_PROT_WRITE) != 0;
    uint64_t ipa = g_next_ipa;
    uint32_t parts = (1u << (HOST_PAGE / 4096)) - 1;
    void *copy = NULL;
    // An executable host page never goes into the VM (macOS panics, SPTM): one that can never be written (an image's
    // __TEXT: constants guest.c's memcpy or the program may read) goes in as a copy, read-only. So does a read-only
    // page the VM refuses (a file's view whose last page passes the file's end).
    int ok = 0;
    if (!(info.protection & VM_PROT_EXECUTE))
        ok = hv_vm_map((void *)(uintptr_t)host, ipa, HOST_PAGE, HV_MEMORY_READ | (writable ? HV_MEMORY_WRITE : 0) | HV_MEMORY_EXEC) == HV_SUCCESS;
    if (!ok && !(info.protection & VM_PROT_WRITE) && (!(info.protection & VM_PROT_EXECUTE) || !writable) && (copy = snapshot(host, &parts))) {
        writable = 0;
        ok = hv_vm_map(copy, ipa, HOST_PAGE, HV_MEMORY_READ | HV_MEMORY_EXEC) == HV_SUCCESS;
    }
    if (!ok) { if (copy) munmap(copy, HOST_PAGE); pthread_mutex_unlock(&g_lock); return 0; }
    g_next_ipa += HOST_PAGE;
    for (int k = 0; k < HOST_PAGE / 4096; k++) if (parts >> k & 1) map4k(page + 4096 * (uint64_t)k, ipa + 4096 * (uint64_t)k, writable);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g_mapped[s] = page; g_ipa[s] = ipa;
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
static __thread volatile int t_in_run, t_interrupt;                 // (xv_interrupt)
static void (*g_interrupt_hook)(x86_ctx *, const xv_regs *);
static __thread uint64_t t_gen, t_resume;
static __thread uint64_t t_fault_pc, t_fault_addr;
static __thread int t_fault_write;
void xv_fault(uint64_t *pc, uint64_t *addr, int *write) { *pc = t_fault_pc; *addr = t_fault_addr; *write = t_fault_write; }
int xv_depth(void) { return t_depth; }
void xv_set_depth(int depth) { t_depth = depth; }
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
    hv_vcpu_set_vtimer_offset(t_vcpu, 0);                                   // (CNTVCT = mach_absolute_time: guest.c's time)
    uint8_t *stack = mmap(NULL, 256 << 10, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);   // (guest.c's calls)
    hv_vcpu_set_sys_reg(t_vcpu, HV_SYS_REG_SP_EL1, (uint64_t)(uintptr_t)(stack + (256 << 10) - 64));
    t_have = 1;
    return 1;
}

typedef void (*native_fn)(x86_ctx *);
void xv_guest_code(const uint8_t **code, size_t *size) {
    extern const struct mach_header_64 *_NSGetMachExecuteHeader(void);
    unsigned long n = 0;
    *code = getsectiondata(_NSGetMachExecuteHeader(), "__TEXT", "__vx_guest", &n);
    *size = n;
}
// The calls recorded in the guest (guest.c), in order: each with a context holding its arguments.
static void drain(x86_ctx *c) {
    if (!c->batch_base || c->batch_ptr == c->batch_base) return;
    uint8_t *p = (uint8_t *)(uintptr_t)c->batch_base, *end = (uint8_t *)(uintptr_t)c->batch_ptr;
    c->batch_ptr = c->batch_base;
    static __thread x86_ctx *f;
    if (!f) f = calloc(1, sizeof *f);
    uint64_t stack[5 + XV_DEFER_ARGS];
    static const int reg[4] = { 1, 2, 8, 9 };
    while (p < end) {
        const uint64_t *r = (const uint64_t *)p;
        for (uint64_t i = 0; i < r[2]; i++) { if (i < 4) f->gpr[reg[i]] = r[3 + i]; else stack[5 + (i - 4)] = r[3 + i]; }
        f->gpr[4] = (uint64_t)(uintptr_t)stack;
        ((native_fn)(uintptr_t)r[1])(f);
        p += r[0];
    }
}
// XV_STATS=1: exits counted by kind and native function, printed at exit.
#include <dlfcn.h>
static uint64_t g_counts[8], g_fn[512], g_fn_n[512];
static void count_fn(uint64_t fn) {
    uint32_t s = (uint32_t)(fn >> 2) & 511;
    for (int k = 0; k < 512; k++, s = (s + 1) & 511) {
        if (g_fn[s] == fn) { __atomic_fetch_add(&g_fn_n[s], 1, __ATOMIC_RELAXED); return; }
        if (!g_fn[s]) { g_fn[s] = fn; g_fn_n[s] = 1; return; }
    }
}
static void print_stats(void) {
    fprintf(stderr, "[vxrt] vcpu exits: engine %llu, helper %llu, native %llu, fault %llu, tlbi %llu\n", (unsigned long long)g_counts[1],
            (unsigned long long)g_counts[2], (unsigned long long)g_counts[3], (unsigned long long)g_counts[5], (unsigned long long)g_counts[4]);
    for (int round = 0; round < 25; round++) {
        int best = -1;
        for (int i = 0; i < 512; i++) if (g_fn[i] && (best < 0 || g_fn_n[i] > g_fn_n[best])) best = i;
        if (best < 0 || !g_fn_n[best]) break;
        Dl_info di; const char *name = dladdr((void *)(uintptr_t)g_fn[best], &di) && di.dli_sname ? di.dli_sname : "?";
        fprintf(stderr, "   %10llu  %s\n", (unsigned long long)g_fn_n[best], name);
        g_fn_n[best] = 0;
    }
}
static int g_stats = -1;
#define DBG(...) do { if (getenv("XV_DEBUG")) { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
void xv_set_interrupt_hook(void (*hook)(x86_ctx *, const xv_regs *)) { g_interrupt_hook = hook; }
int xv_interrupt(void) {
    if (!t_have || !t_in_run || !g_interrupt_hook) return 0;
    t_interrupt = 1;
    hv_vcpus_exit(&t_vcpu, 1);                                             // (hv_vcpu_run returns: canceled)
    return 1;
}
static void interrupted(x86_ctx *c) {                                     // (after a run: the guest's registers)
    xv_regs r;
    for (int i = 0; i < 31; i++) hv_vcpu_get_reg(t_vcpu, (hv_reg_t)(HV_REG_X0 + i), &r.x[i]);
    hv_vcpu_get_reg(t_vcpu, HV_REG_PC, &r.pc);
    if (t_resume) r.pc = t_resume;                                         // (in the TLB flush on the way back)
    for (int i = 0; i < 16; i++) {
        hv_simd_fp_uchar16_t v; hv_vcpu_get_simd_fp_reg(t_vcpu, (hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0 + i), &v);
        memcpy(r.q[i], &v, 16);
    }
    g_interrupt_hook(c, &r);
}
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
    int stage2 = 0;
    for (;;) {
        DBG("run");
        if (t_gen != g_gen && g_tlbi && !t_resume) {                       // (page tables changed: this vCPU's TLB first)
            t_gen = g_gen;
            hv_vcpu_get_reg(t_vcpu, HV_REG_PC, &t_resume);
            hv_vcpu_set_reg(t_vcpu, HV_REG_PC, g_tlbi);
        }
        t_in_run = 1;
        hv_return_t ran = hv_vcpu_run(t_vcpu);
        t_in_run = 0;
        if (t_interrupt) { t_interrupt = 0; interrupted(c); }               // (SuspendThread: stopped there till resumed)
        if (ran != HV_SUCCESS) { fprintf(stderr, "[vxrt] vcpu: run failed\n"); break; }
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
            if (g_stats < 0) { g_stats = getenv("XV_STATS") != NULL; if (g_stats) atexit(print_stats); }
            if (g_stats) __atomic_fetch_add(&g_counts[imm < 5 ? imm : 5], 1, __ATOMIC_RELAXED);
            if (imm != XV_HVC_HELPER && imm != XV_HVC_TLBI) drain(c);    // (what the guest recorded comes first)
            if (imm == XV_HVC_EXIT) { result = 0; break; }
            if (imm == XV_HVC_TLBI) { hv_vcpu_set_reg(t_vcpu, HV_REG_PC, t_resume); t_resume = 0; continue; }
            if (imm == XV_HVC_HELPER) { xlat_helper(c); continue; }
            if (imm == XV_HVC_NATIVE) {
                uint64_t fn; hv_vcpu_get_reg(t_vcpu, HV_REG_X16, &fn);
                if (g_stats) { count_fn(fn); if (g_counts[3] % 1000000 == 0) print_stats(); }
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
                if (abort_ && (fsc & 0x3C) == 0x0C) {                          // permission: a stale TLB entry (the page
                    pthread_mutex_lock(&g_lock);                               //  table allows it now)? flushed, again.
                    uint64_t *p = pte(far);                                    // (on a readable page only a write faults:
                    int allowed = p && (*p & 1) && !(*p & 1u << 7);            //  atomics may say WnR 0)
                    pthread_mutex_unlock(&g_lock);
                    if (allowed) { t_gen = g_gen - 1; hv_vcpu_set_reg(t_vcpu, HV_REG_PC, elr); hv_vcpu_set_reg(t_vcpu, HV_REG_CPSR, spsr); continue; }
                }
                int write = ((esr >> 6) & 1) || (fsc & 0x3C) == 0x0C;
                if (abort_ && write && g_write_hook && g_write_hook(elr, far)) {   // (guest.c's write to a watched page)
                    t_gen = g_gen - 1; hv_vcpu_set_reg(t_vcpu, HV_REG_PC, elr); hv_vcpu_set_reg(t_vcpu, HV_REG_CPSR, spsr);
                    continue;
                }
                if (abort_ && (eec == 0x24 || eec == 0x25)) {                  // the program's access fault: its state
                    for (int r = 0; r < 16; r++) {
                        hv_vcpu_get_reg(t_vcpu, (hv_reg_t)(HV_REG_X0 + r), &c->gpr[r]);
                        hv_simd_fp_uchar16_t v; hv_vcpu_get_simd_fp_reg(t_vcpu, (hv_simd_fp_reg_t)(HV_SIMD_FP_REG_Q0 + r), &v);
                        memcpy(c->xmm[r], &v, 16);
                    }
                    hv_vcpu_get_reg(t_vcpu, HV_REG_X27, &c->flags); c->flags = xlat_flags_resolve(c->flags, c);
                    hv_vcpu_get_reg(t_vcpu, HV_REG_X29, &c->ras);
                    t_fault_pc = elr; t_fault_addr = far; t_fault_write = write;
                    result = XV_FAULT;
                    break;
                }
                fprintf(stderr, "[vxrt] vcpu: exception in the guest: vector %u, ESR %#llx, FAR %#llx, ELR %#llx\n",
                        imm - XV_HVC_VECTOR, (unsigned long long)esr, (unsigned long long)far, (unsigned long long)elr);
                break;
            }
        }
        if ((ec == 0x20 || ec == 0x24) && stage2++ < 4) { t_gen = g_gen - 1; continue; }   // (a page gone: a stale TLB entry)
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
