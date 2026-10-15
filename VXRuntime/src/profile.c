// VXRT_PROFILE=file: a sampling profiler that sees through Rosetta. Every millisecond of CPU time (SIGPROF) the
// interrupted x86-64 instruction pointer is recorded with its thread; a thread of its own counts them, and every 10 s
// (and at exit) writes the hottest functions to the file: a Windows image's function by its .pdata entry
// ("hlvr.dll!0x1234"), anything else by dladdr (the runtime as "vxrt+0x5678": `atos -o build/vxrt -l <base>` names it).
// Separately: the samples taken while the thread held a keyed mutex (d3d11_shared.m), where a game's frame
// serializes with SteamVR's compositor.
#include "vx.h"
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct { uint64_t pc, thread; uint32_t held, pad; } prof_sample;
#define RING (1u << 18)
static prof_sample g_ring[RING];
static volatile uint64_t g_head, g_tail;
static pthread_key_t g_held_key;
static int g_on;

void vx_profile_held(int delta) {                  // keyed mutex taken (+1) or released (-1) by this thread
    if (!g_on) return;
    intptr_t n = (intptr_t)pthread_getspecific(g_held_key) + delta;
    pthread_setspecific(g_held_key, (void *)(n < 0 ? 0 : n));
}

static void on_prof(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = ucv;
    uint64_t i = __atomic_fetch_add(&g_head, 1, __ATOMIC_RELAXED);
    if (i - g_tail >= RING) return;                // (the counting thread fell behind: dropped)
    prof_sample *s = &g_ring[i & (RING - 1)];
#ifdef __aarch64__
    s->pc = uc ? uc->uc_mcontext->__ss.__pc : 0;
#else
    s->pc = uc ? uc->uc_mcontext->__ss.__rip : 0;
#endif
    s->thread = (uint64_t)pthread_self();
    s->held = pthread_getspecific(g_held_key) != NULL;
}

// Counts by instruction address (open addressing), then by function when written out.
typedef struct { uint64_t pc; uint32_t all, held; } pc_count;
#define SLOTS (1u << 20)
static pc_count *g_counts;
static uint64_t g_total, g_total_held;
typedef struct { uint64_t thread; uint32_t all; char name[32]; } thread_count;
static thread_count g_threads[64];

static void count(const prof_sample *s) {
    uint64_t h = (s->pc * 0x9E3779B97F4A7C15ull) >> 44;
    for (uint32_t k = 0; k < SLOTS; k++) {
        pc_count *c = &g_counts[(h + k) & (SLOTS - 1)];
        if (c->pc && c->pc != s->pc) continue;
        c->pc = s->pc; c->all++; c->held += s->held;
        break;
    }
    g_total++; g_total_held += s->held;
    for (int t = 0; t < 64; t++) {
        if (g_threads[t].thread && g_threads[t].thread != s->thread) continue;
        if (!g_threads[t].thread) {
            g_threads[t].thread = s->thread;
            pthread_getname_np((pthread_t)s->thread, g_threads[t].name, sizeof g_threads[t].name);
        }
        g_threads[t].all++;
        break;
    }
}

typedef struct { char name[96]; uint32_t all, held; } fn_count;
static void function_name(uint64_t pc, char *out, size_t n) {
    const vx_image *m = vx_image_at(pc);
    if (m) {
        uint32_t rva = (uint32_t)(pc - (uint64_t)m->base), start = rva;
        if (m->pdata_size) {                       // the RUNTIME_FUNCTION (begin, end, unwind) holding it
            const uint32_t *f = (const uint32_t *)(m->base + m->pdata_rva);
            size_t lo = 0, hi = m->pdata_size / 12;
            while (lo < hi) { size_t mid = (lo + hi) / 2; if (f[mid * 3 + 1] <= rva) lo = mid + 1; else hi = mid; }
            if (lo < m->pdata_size / 12 && f[lo * 3] <= rva) start = f[lo * 3];
        }
        snprintf(out, n, "%s!%#x", m->name, start);
        return;
    }
    Dl_info info;
    if (dladdr((void *)pc, &info) && info.dli_fname) {
        const char *file = strrchr(info.dli_fname, '/');
        file = file ? file + 1 : info.dli_fname;
        if (info.dli_fbase == (void *)_dyld_get_image_header(0))   // the runtime itself: by offset, for atos
            snprintf(out, n, "vxrt+%#llx (%s)", (unsigned long long)(pc - (uint64_t)info.dli_fbase), info.dli_sname ? info.dli_sname : "?");
        else snprintf(out, n, "%s:%s", file, info.dli_sname ? info.dli_sname : "?");
        return;
    }
    snprintf(out, n, "? (translated code, %#llx)", (unsigned long long)pc);
}
static int by_all(const void *a, const void *b) { return (int)((const fn_count *)b)->all - (int)((const fn_count *)a)->all; }
static int by_held(const void *a, const void *b) { return (int)((const fn_count *)b)->held - (int)((const fn_count *)a)->held; }
static int thread_by_all(const void *a, const void *b) { return (int)((const thread_count *)b)->all - (int)((const thread_count *)a)->all; }

static void write_report(const char *path) {
    // Group the addresses by function (a sorted array of names; few thousand distinct functions).
    size_t cap = 8192, n = 0;
    fn_count *fns = calloc(cap, sizeof *fns);
    for (uint32_t i = 0; i < SLOTS; i++) {
        pc_count *c = &g_counts[i];
        if (!c->pc) continue;
        char name[96];
        function_name(c->pc, name, sizeof name);
        size_t k = 0;
        while (k < n && strcmp(fns[k].name, name)) k++;
        if (k == n) {
            if (n == cap) continue;
            snprintf(fns[n].name, sizeof fns[n].name, "%s", name);
            n++;
        }
        fns[k].all += c->all; fns[k].held += c->held;
    }
    FILE *f = fopen(path, "w");
    if (!f) { free(fns); return; }
    fprintf(f, "%llu samples (1 ms of CPU each), %llu while holding a keyed mutex; runtime loaded at %p\n\n",
            (unsigned long long)g_total, (unsigned long long)g_total_held, (void *)_dyld_get_image_header(0));
    thread_count threads[64];
    memcpy(threads, g_threads, sizeof threads);
    qsort(threads, 64, sizeof *threads, thread_by_all);
    fprintf(f, "Threads:\n");
    for (int t = 0; t < 12 && threads[t].thread; t++)
        fprintf(f, "  %6.2f%%  %s\n", 100.0 * threads[t].all / (g_total ? g_total : 1), threads[t].name[0] ? threads[t].name : "(unnamed)");
    qsort(fns, n, sizeof *fns, by_all);
    fprintf(f, "\nAll samples:\n");
    for (size_t k = 0; k < n && k < 60; k++) fprintf(f, "  %6.2f%%  %s\n", 100.0 * fns[k].all / (g_total ? g_total : 1), fns[k].name);
    qsort(fns, n, sizeof *fns, by_held);
    fprintf(f, "\nWhile holding a keyed mutex:\n");
    for (size_t k = 0; k < n && k < 60 && fns[k].held; k++) fprintf(f, "  %6.2f%%  %s\n", 100.0 * fns[k].held / (g_total_held ? g_total_held : 1), fns[k].name);
    fclose(f);
    free(fns);
}

static const char *g_path;
static pthread_mutex_t g_report_lock = PTHREAD_MUTEX_INITIALIZER;
static void drain(void) {
    uint64_t head = __atomic_load_n(&g_head, __ATOMIC_ACQUIRE);
    if (head - g_tail > RING) g_tail = head - RING;
    while (g_tail < head) {
        prof_sample s = g_ring[g_tail & (RING - 1)];
        if (s.pc) count(&s);
        __atomic_store_n(&g_tail, g_tail + 1, __ATOMIC_RELEASE);
    }
}
static void *counter(void *arg) {
    (void)arg;
    pthread_setname_np("vxrt profiler");
    for (int tick = 1;; tick++) {
        usleep(200000);
        pthread_mutex_lock(&g_report_lock);
        drain();
        if (tick % 50 == 0) write_report(g_path);
        pthread_mutex_unlock(&g_report_lock);
    }
    return NULL;
}
void vx_profile_finish(void) {
    if (!g_on) return;
    pthread_mutex_lock(&g_report_lock);
    drain();
    write_report(g_path);
    pthread_mutex_unlock(&g_report_lock);
}

void vx_profile_start(void) {
    g_path = getenv("VXRT_PROFILE");
    if (!g_path || !*g_path) return;
    g_counts = calloc(SLOTS, sizeof *g_counts);
    pthread_key_create(&g_held_key, NULL);
    g_on = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, counter, NULL);
    pthread_detach(t);
    atexit(vx_profile_finish);
    struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
    setitimer(ITIMER_PROF, &it, NULL);
    vx_log("profiling to %s", g_path);
}

#ifdef __aarch64__
#include "thunks/profile.c.inc"
#endif
