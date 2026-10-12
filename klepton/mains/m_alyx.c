// Host driver for Half-Life: Alyx's Linux ARM64 build (glibc guest).
//
//   ./build/m_alyx <game dir> <sysroot dir> [--gap]
//
// <game dir> is the depot's game/ folder; <sysroot dir> holds the guest C++
// runtime (Debian libstdc++.so.6, libgcc_s.so.1). --gap maps and relocates
// every library and prints the unresolved imports without running anything.
//
// Built with the OpenVR shim (KL_OPENVR_SHIM, see the Makefile), the game's
// libopenvr_api.so is never loaded: its VR_* entry points bind to the shim,
// so a run without -novr gets a headset. On the Mac there is none, and the
// shim's host mode stands in (fixed head, still hands, the game's own window
// as the picture); see shim/openvr/README.md in the project repo.
#include <dirent.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include "../runtime/klepton.h"
#include "../runtime/kl_fault.h"
#include "../runtime/linux/kl_glibc.h"

#include "../runtime/linux/kl_alyx.h"
#include "../runtime/linux/kl_alyx_app.h"
#define alyx_serves_soname kl_alyx_serves_soname
#define alyx_lookup kl_alyx_lookup

static const char *g_dirs[4];
static int g_ndirs;

// Guest files we never load: replaced by our own implementation or not needed.
static int skipped(const char *b) {
    static const char *skip[] = {
        "libopenvr_api.so",           // replaced by the OpenVR shim (WS6)
        "libvulkan_freedreno_hlvr.so", // Steam Frame's Adreno driver; MoltenVK instead
        "libmimalloc.so",             // initial-exec TLS; system malloc instead
    };
    for (unsigned i = 0; i < sizeof skip / sizeof skip[0]; i++) if (!strcmp(b, skip[i])) return 1;
    return 0;
}

static int find_lib(const char *name, char *out, size_t cap) {
    for (int i = 0; i < g_ndirs; i++) {
        snprintf(out, cap, "%s/%s", g_dirs[i], name);
        if (access(out, R_OK) == 0) return 1;
    }
    // Sonames the depot ships only under their full version (libavcodec.so.58
    // is the file libavcodec.so.58.91.100): the first "<soname>.*" match.
    size_t nl = strlen(name);
    for (int i = 0; i < g_ndirs; i++) {
        DIR *d = opendir(g_dirs[i]);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!strncmp(e->d_name, name, nl) && e->d_name[nl] == '.') {
                snprintf(out, cap, "%s/%s", g_dirs[i], e->d_name);
                closedir(d);
                return 1;
            }
        }
        if (d) closedir(d);
    }
    return 0;
}

// Missing DT_NEEDED sonames, collected for the report.
static char g_missing_so[64][128];
static int g_nmissing_so;

static char g_inprog[64][128];
static int g_ninprog;

static kl_image *load(const char *path, int run_init) {
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    kl_image *have = kl_find_image(b);
    if (have) return have;
    // DT_NEEDED cycles exist (libfreetype <-> libharfbuzz): a library already
    // being loaded further up the stack is skipped; its imports bind lazily
    // through the global registry once it lands.
    char real[1024];
    if (!realpath(path, real)) snprintf(real, sizeof real, "%s", path);
    const char *rb = strrchr(real, '/'); rb = rb ? rb + 1 : real;
    for (int i = 0; i < g_ninprog; i++) if (!strcmp(g_inprog[i], rb)) return (kl_image *)1;
    if ((have = kl_find_image(rb))) { kl_register_image(b, have); return have; }
    if (g_ninprog < 64) snprintf(g_inprog[g_ninprog++], 128, "%s", rb);
    char names[48][128];
    int nn = kl_list_needed(path, names, 48);
    for (int i = 0; i < nn; i++) {
        if (alyx_serves_soname(names[i]) || skipped(names[i])) continue;
        char full[1024];
        if (!find_lib(names[i], full, sizeof full)) {
            int dup = 0;
            for (int j = 0; j < g_nmissing_so; j++) dup |= !strcmp(g_missing_so[j], names[i]);
            if (!dup && g_nmissing_so < 64) snprintf(g_missing_so[g_nmissing_so++], 128, "%s", names[i]);
            continue;
        }
        if (!load(full, run_init)) return NULL;
    }
    kl_image *img = kl_load_auto(real);
    g_ninprog--;
    if (!img) { fprintf(stderr, "load %s: %s\n", path, kl_error()); return NULL; }
    kl_register_image(rb, img);
    if (strcmp(rb, b)) kl_register_image(b, img);
    // The DT_NEEDED spelling too (libavcodec.so.58), so later loads dedupe.
    char sn[128]; const char *dot = strstr(b, ".so.");
    if (dot) { const char *v2 = strchr(dot + 4, '.'); if (v2) { snprintf(sn, sizeof sn, "%.*s", (int)(v2 - b), b); if (!kl_find_image(sn)) kl_register_image(sn, img); } }
    if (run_init) kl_run_init(img);
    return img;
}

// KL_SIGWATCH=1: report whenever the SIGBUS/SIGSEGV disposition changes, and
// who installed the new one. A guest (Breakpad, V8, SDL) replacing Klepton's
// fault reporter is otherwise invisible: the next crash just exits silently.
#include <signal.h>
#include <pthread.h>
#include <dlfcn.h>
static void *sigwatch(void *a) {
    (void)a;
    void *last[2] = {0, 0};
    int sigs[2] = {SIGBUS, SIGSEGV};
    for (;;) {
        for (int i = 0; i < 2; i++) {
            struct sigaction cur;
            sigaction(sigs[i], NULL, &cur);
            void *h = (cur.sa_flags & SA_SIGINFO) ? (void *)cur.sa_sigaction : (void *)cur.sa_handler;
            if (h != last[i]) {
                Dl_info di; size_t off = 0;
                const char *img = kl_addr_image(h, &off);
                if (!img && dladdr(h, &di)) { img = di.dli_sname ? di.dli_sname : di.dli_fname; off = (size_t)((char *)h - (char *)(di.dli_sname ? di.dli_saddr : di.dli_fbase)); }
                fprintf(stderr, "[sigwatch] %s handler -> %p (%s+%#zx) flags %#x\n", i ? "SIGSEGV" : "SIGBUS", h, img ? img : "?", off, cur.sa_flags);
                last[i] = h;
            }
        }
        usleep(2000);
    }
    return NULL;
}

void kl_sample_dump_all(FILE *out, const char *why);
static void *stall_test(void *arg) {
    sleep((unsigned)(intptr_t)arg);
    kl_sample_dump_all(stderr, "KL_STALL_TEST_S");
    return NULL;
}

// KL_STACKDUMP=<seconds>: every so often, suspend the main thread and print
// its frame-pointer chain as image+offset. For "the loop runs but nothing
// progresses" diagnosis without a debugger.
static mach_port_t g_main_thread;
static void *stackdump(void *a) {
    int secs = (int)(intptr_t)a;
    for (;;) {
        sleep(secs);
        arm_thread_state64_t st;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_suspend(g_main_thread) != KERN_SUCCESS) continue;
        uintptr_t pcs[48]; int n = 0;
        if (thread_get_state(g_main_thread, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) == KERN_SUCCESS) {
            pcs[n++] = (uintptr_t)arm_thread_state64_get_pc(st);
            pcs[n++] = (uintptr_t)arm_thread_state64_get_lr(st);
            uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(st);
            while (fp && !(fp & 7) && n < 48) {
                uintptr_t *f = (uintptr_t *)fp;
                uintptr_t lr = f[1] & 0x0000ffffffffffffull;
                if (!lr) break;
                pcs[n++] = lr;
                if (f[0] <= fp) break;
                fp = f[0];
            }
        }
        thread_resume(g_main_thread);
        fprintf(stderr, "[stackdump] main thread:\n");
        for (int i = 0; i < n; i++) {
            size_t off = 0; Dl_info di;
            const char *img = kl_addr_image((void *)pcs[i], &off);
            if (!img && dladdr((void *)pcs[i], &di)) {
                img = di.dli_sname ? di.dli_sname : di.dli_fname;
                off = pcs[i] - (uintptr_t)(di.dli_sname ? di.dli_saddr : di.dli_fbase);
            }
            const char *b = img ? strrchr(img, '/') : NULL;
            fprintf(stderr, "  #%-2d %s+%#zx\n", i, img ? (b ? b + 1 : img) : "?", off);
        }
    }
    return NULL;
}

// Klepton finds MoltenVK relative to the working directory by default, and the launcher changes it: pin it to this
// build's vendored copy. Before the app files too, which read the GPU's ids through it.
static void pin_mvk(void) {
    if (getenv("KL_MVK_DIR")) return;
    char mvk[1024], cwd[1024];
    if (getcwd(cwd, sizeof cwd)) {
        snprintf(mvk, sizeof mvk, "%s/vendor-moltenvk/out/macos", cwd);
        setenv("KL_MVK_DIR", mvk, 1);
    }
}

// Run mode: Valve's own launcher (bin/linuxsteamrtarm64/hlvr) through the
// loader. Its _start calls __libc_start_main (served by the glibc layer),
// which calls main(); main finds its bin dir via /proc/self/exe, dlopens
// libtier0 and libengine2, and calls Source2Main.
static int run_launcher(const char *game, const char *sysroot, int gargc, char **gargv) {
    kl_fault_install();
    if (getenv("KL_SIGWATCH")) { pthread_t t; pthread_create(&t, NULL, sigwatch, NULL); }
    // KL_STALL_TEST_S=<s>: the headset's guest-stall thread dump (kl_sample_dump_all), once, s seconds in, so its
    // output can be checked on the Mac.
    if (getenv("KL_STALL_TEST_S")) {
        extern void kl_sample_dump_all(FILE *out, const char *why);
        pthread_t t;
        pthread_create(&t, NULL, stall_test, (void *)(intptr_t)atoi(getenv("KL_STALL_TEST_S")));
        pthread_detach(t);
    }
    if (getenv("KL_STACKDUMP")) {
        g_main_thread = mach_thread_self();
        pthread_t t; pthread_create(&t, NULL, stackdump, (void *)(intptr_t)atoi(getenv("KL_STACKDUMP")));
    }
    pin_mvk();
    return kl_alyx_run(game, sysroot, gargc, gargv);
}

// --app: run as the headset app does from its icon, against KL_APP_DOCS (a folder laid out like the app's
// Documents: the game in alyx/assets) and KL_APP_BUNDLE (the built .app, for its alyx-demo/ defaults). The
// app's own files come from that Documents folder or the bundle (kl_alyx_app.c, the code the app runs), the
// pipeline recorder's "auto" folder is <docs>/klfz as on the device, and Valve's argv is built as kl_driver.c
// builds it there (the fixed flags, then KL_ALYX_PROFILE_ARGS and KL_ALYX_ARGS). KL_APP_TEST_LUA=<file> is
// appended to the convars the app picked: a Mac check's probes (the device never sets it).
static int run_as_app(const char *sysroot) {
    const char *docs = getenv("KL_APP_DOCS"), *bundle = getenv("KL_APP_BUNDLE");
    if (!docs || !*docs || !bundle || !*bundle) { fprintf(stderr, "--app needs KL_APP_DOCS and KL_APP_BUNDLE\n"); return 2; }
    static char envline[4096], files[512], game[1200], fozdir[1200];
    snprintf(fozdir, sizeof fozdir, "%s/klfz", docs);
    setenv("KL_VK_FOZ_DIR", fozdir, 0);
    pin_mvk();
    kl_alyx_app_files(bundle, docs, envline, sizeof envline, files, sizeof files);
    fprintf(stderr, "=== as the app: %s, %s ===\n%s%s", bundle, docs, envline, files);
    // KL_PREWARM_ONLY=<device-info.json>: warm the shader cache as the launcher does before Play (kl_foz_prewarm), with
    // no game at all, and exit. The next run's own replay then shows whether every pipeline came from the cache.
    const char *pw = getenv("KL_PREWARM_ONLY");
    if (pw && *pw) { extern int kl_foz_prewarm(const char *); return kl_foz_prewarm(pw) ? 1 : 0; }
    // KL_PREWARM_FIRST=<device-info.json>: the prewarm, then the game in the same process, as the headset app runs
    // them (the launcher's "Preparing graphics" before Play).
    const char *pf = getenv("KL_PREWARM_FIRST");
    if (pf && *pf) { extern int kl_foz_prewarm(const char *); fprintf(stderr, "  prewarm first: %d\n", kl_foz_prewarm(pf)); }
    const char *tl = getenv("KL_APP_TEST_LUA");
    if (tl && *tl) {
        static char merged[1200], spec[1300];
        const char *inj = getenv("KL_LUA_INJECT"), *eq = inj ? strchr(inj, '=') : NULL;
        snprintf(merged, sizeof merged, "%s/kl-app-test-%d.lua", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", getpid());
        FILE *o = fopen(merged, "w");
        const char *parts[2] = { eq ? eq + 1 : NULL, tl };
        for (int i = 0; o && i < 2; i++) {
            FILE *f = parts[i] ? fopen(parts[i], "r") : NULL;
            char buf[8192]; size_t n;
            while (f && (n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, o);
            if (f) { fclose(f); fputc('\n', o); }
        }
        if (o) fclose(o);
        snprintf(spec, sizeof spec, "game/gameinit=%s", merged);
        setenv("KL_LUA_INJECT", spec, 1);
        fprintf(stderr, "  test lua  : %s + %s -> %s\n", eq ? eq + 1 : "(no convars)", tl, merged);
    }
    snprintf(game, sizeof game, "%s/alyx/assets", docs);
    static char *gargv[40];
    static char extra[2048];
    int n = 0;
    gargv[n++] = "-vr"; gargv[n++] = "-console"; gargv[n++] = "-condebug";
    gargv[n++] = "-vulkan_disable_ext_dynamic_state_3";
    const char *prof = getenv("KL_ALYX_PROFILE_ARGS"), *more = getenv("KL_ALYX_ARGS");
    snprintf(extra, sizeof extra, "%s %s", prof ? prof : "", more ? more : "");
    char *save = NULL;
    for (char *t = strtok_r(extra, " ", &save); t && n < 39; t = strtok_r(NULL, " ", &save)) gargv[n++] = t;
    gargv[n] = NULL;
    fprintf(stderr, "  game argv :");
    for (int i = 0; i < n; i++) fprintf(stderr, " %s", gargv[i]);
    fprintf(stderr, "\n");
    return run_launcher(game, sysroot, n, gargv);
}

int main(int argc, char **argv) {
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[2], "--app")) return run_as_app(argv[1]);
    if (argc < 3) { fprintf(stderr, "usage: m_alyx <game dir> <sysroot> [--gap | --run [game args...]]\n"
                                    "       m_alyx <sysroot> --app   (KL_APP_DOCS, KL_APP_BUNDLE)\n"); return 2; }
    if (argc > 3 && !strcmp(argv[3], "--run")) return run_launcher(argv[1], argv[2], argc - 4, argv + 4);
    int gap = argc > 3 && !strcmp(argv[3], "--gap");
    static char d0[1024], d1[1024];
    snprintf(d0, sizeof d0, "%s/bin/linuxsteamrtarm64", argv[1]);
    snprintf(d1, sizeof d1, "%s/hlvr/bin/linuxsteamrtarm64", argv[1]);
    g_dirs[g_ndirs++] = argv[2];              // guest C++ runtime first
    g_dirs[g_ndirs++] = d0;
    g_dirs[g_ndirs++] = d1;
    kl_set_library_path(d0);
    kl_fault_install();
    kl_soname_served_hook = alyx_serves_soname;
    kl_shim_override = alyx_lookup;

    // The C++ runtime first, so every later image's C++ ABI imports bind to it.
    char p[1024];
    snprintf(p, sizeof p, "%s/libgcc_s.so.1", argv[2]);
    kl_image *gcc = load(p, !gap);
    snprintf(p, sizeof p, "%s/libstdc++.so.6", argv[2]);
    kl_image *cxx = load(p, !gap);
    if (!gcc || !cxx) return 1;
    kl_glibc_set_cxx_images(cxx, gcc);

    // Everything the engine can load: every library in both bin dirs.
    unsigned total_missing = 0;
    for (int d = 1; d < g_ndirs; d++) {
        DIR *dir = opendir(g_dirs[d]);
        struct dirent *e;
        while (dir && (e = readdir(dir))) {
            const char *n = e->d_name;
            size_t l = strlen(n);
            if (!(l > 3 && !strcmp(n + l - 3, ".so")) || skipped(n)) continue;
            snprintf(p, sizeof p, "%s/%s", g_dirs[d], n);
            struct stat lst;
            if (lstat(p, &lst) == 0 && S_ISLNK(lst.st_mode)) continue;   // hlvr.sh's soname links
            char sz[8]; FILE *f = fopen(p, "rb"); size_t got = f ? fread(sz, 1, 4, f) : 0; if (f) fclose(f);
            if (got < 4 || memcmp(sz, "\x7f" "ELF", 4)) continue;        // empty dev symlink stand-ins
            if (!load(p, !gap)) return 1;
        }
        if (dir) closedir(dir);
    }
    if (gap) {
        // Union of unresolved names across images, with who wants them.
        extern kl_image *kl_registry_at(int i, const char **soname);
        const char *so;
        for (int i = 0; ; i++) {
            kl_image *img = kl_registry_at(i, &so);
            if (!img) break;
            unsigned n;
            const char *const *m = kl_missing_imports(img, &n);
            for (unsigned k = 0; k < n; k++) printf("MISSING %s %s\n", m[k], so);
            total_missing += n;
        }
        for (int j = 0; j < g_nmissing_so; j++) printf("MISSING_SONAME %s\n", g_missing_so[j]);
        printf("GAP: %u unresolved import sites (per image), %d missing sonames\n", total_missing, g_nmissing_so);
    }
    return 0;
}
