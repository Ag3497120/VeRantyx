// The engine on visionOS (VISIONOS_PORT.md), in place of engine.c: an app there may not make code at run time (no
// MAP_JIT, no dynamic-codesigning), so nothing is translated. All x86 code runs from the statically recompiled
// modules linked into the app (translator/aot.c); an address that has none stops the program here, with the address
// and its image in the log, instead of the translator's fallback on the Mac.
#include "engine.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void vx_log(const char *fmt, ...);
const char *vx_image_name_at(uint64_t pc);   // (src/loader.c)
void xa_report(void);

struct xe {
    pthread_mutex_t lock;
    void (*(*native)(uint64_t))(x86_ctx *);
    int (*foreign)(uint64_t rip);
    xe_stats stats;
};

xe *xe_new(int ordered) {
    (void)ordered;
    xe *e = calloc(1, sizeof *e);
    if (e) pthread_mutex_init(&e->lock, NULL);
    vx_log("engine: visionOS build: no run-time translation (no JIT); x86 code runs only from linked AOT modules");
    return e;
}
xe *xe_new_vcpu(void) { vx_log("engine: no Hypervisor.framework on visionOS: no vCPU"); return NULL; }
void xe_set_native(xe *e, void (*(*find)(uint64_t))(x86_ctx *)) { e->native = find; }
void xe_set_foreign(xe *e, int (*owns)(uint64_t)) { e->foreign = owns; }
void xe_set_guest(xe *e, int (*find)(uint64_t, uint64_t *, const void **)) { (void)e; (void)find; }
// The return-address ring is memory the modules' code reads and writes (xlat.h): the same as the engine's.
uint64_t xe_ring_new(xe *e) {
    (void)e;
    uint64_t *r;
    if (posix_memalign((void **)&r, 1u << XLAT_RAS_BITS, 1u << XLAT_RAS_BITS)) return 0;
    for (size_t i = 0; i < (1u << XLAT_RAS_BITS) / 8; i += 2) { r[i] = ~0ull; r[i + 1] = 0; }
    return (uint64_t)(uintptr_t)r;
}
uint64_t xe_rip_at(const xe *e, uint64_t pc) { (void)e; (void)pc; return 0; }
uint64_t xe_rip_sp_at(const xe *e, uint64_t pc, int64_t *sp) { (void)e; (void)pc; if (sp) *sp = 0; return 0; }
void xe_set_smc(xe *e, int on) { (void)e; (void)on; }
int xe_smc_write(xe *e, uint64_t addr) { (void)e; (void)addr; return 0; }
void xe_forget(xe *e, uint64_t addr, uint64_t len) { (void)e; (void)addr; (void)len; }
int xe_fix_split(xe *e, void *uc) { (void)e; (void)uc; return 0; }
int xe_owns(const xe *e, uint64_t pc) { (void)e; (void)pc; return 0; }
int xe_in_guest(const xe *e, uint64_t pc) { (void)e; (void)pc; return 0; }
void xe_flush(xe *e) { (void)e; }
xe_stats xe_get_stats(const xe *e) { return e->stats; }

// Reached for x86 code no module has (xa_run's fallback, or a program run without modules): the end of the program.
int xe_run(xe *e, x86_ctx *c, uint64_t stop) {
    (void)stop;
    if (e->foreign && e->foreign(c->rip)) return XE_FOREIGN;
    const char *img = vx_image_name_at(c->rip);
    vx_log("engine: no recompiled code for %#llx (%s) and no JIT on visionOS: the program stops (VISIONOS_PORT.md: add it to the image's AOT roots)",
           (unsigned long long)c->rip, img ? img : "no image");
    xa_report();
    _exit(3);
}
