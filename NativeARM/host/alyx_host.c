// alyx_host: the Linux ARM64 Half-Life: Alyx engine, loaded on macOS.
//
// Valve's hlvr launcher is 26 KB: it dlopens libengine2.so and calls
// Source2Main. This is the same program with Klepton's loader (and the glibc
// layer in kl_glibc.c) in place of the Linux dynamic linker.
//
//   alyx_host --libdir DIR --probe lib.so [lib.so …]   load, run constructors, report
//   alyx_host --libdir DIR --game DIR [-- engine args]  run the engine
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <limits.h>
#include <errno.h>
#include "klepton.h"
#include "kl_tls.h"
#include "kl_glibc.h"
#include "kl_fault.h"
#include "kl_enginelog.h"
#include "kl_sdlshim.h"
#include "kl_luafix.h"
#include "kl_sdlaudio.h"
#include "kl_steambridge.h"
#include "kl_x18.h"
#include <sys/stat.h>

// The OpenVR runtime (../vrruntime), served to the guest as vrclient.so.
extern void *vrrt_dlsym(const char *name);
extern void vrrt_init(const char *gameDir);

// openvr_api finds its runtime through a JSON path registry; point it (via
// VR_PATHREG_OVERRIDE) at one naming a directory whose vrclient.so is ours.
static void install_vr_path_registry(void) {
    const char *home = getenv("HOME");
    char dir[PATH_MAX], sub[PATH_MAX], file[PATH_MAX], reg[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/Verantyx_VR_Drive/verantyx_openvr", home ? home : "/tmp");
    snprintf(sub, sizeof sub, "%s/bin/linuxarm64", dir);
    snprintf(file, sizeof file, "%s/vrclient.so", sub);
    snprintf(reg, sizeof reg, "%s/openvrpaths.vrpath", dir);
    char mk[PATH_MAX + 16]; snprintf(mk, sizeof mk, "%s/bin", dir);
    mkdir(dir, 0755); mkdir(mk, 0755); mkdir(sub, 0755);
    FILE *f = fopen(file, "a"); if (f) fclose(f);       // dlopen of this path is answered by the host module
    f = fopen(reg, "w");
    if (f) {
        fprintf(f, "{\n\t\"config\" : [ \"%s/config\" ],\n\t\"external_drivers\" : null,\n\t\"jsonid\" : \"vrpathreg\",\n"
                   "\t\"log\" : [ \"%s/logs\" ],\n\t\"runtime\" : [ \"%s\" ],\n\t\"version\" : 1\n}\n", dir, dir, dir);
        fclose(f);
    }
    setenv("VR_PATHREG_OVERRIDE", reg, 1);
    kl_dl_add_host_module("/vrclient.so", vrrt_dlsym);
}

static const char *g_libdir;

struct source2_args { void *fn; uint64_t hinstance; const char *cmdline; const char *exedir; };
static struct source2_args g_args;
static long source2_tramp(void *p) {
    struct source2_args *a = p;
    int (*main_fn)(uint64_t, uint64_t, const char *, int, const char *, const char *) =
        (int (*)(uint64_t, uint64_t, const char *, int, const char *, const char *))a->fn;
    return (long)main_fn(a->hinstance, 0, a->cmdline, 0, a->exedir, "hlvr");
}

static void report_image(const char *soname) {
    kl_image *img = kl_find_image(soname);
    if (!img) { printf("  %-32s not loaded\n", soname); return; }
    const kl_stats *st = kl_get_stats(img);
    unsigned nm = 0;
    const char *const *miss = kl_missing_imports(img, &nm);
    printf("  %-32s relocs %u (tlsdesc %u tprel %u)  imports bound %u  UNRESOLVED %u%s\n",
           soname, st->relative + st->abs64 + st->glob_dat + st->jump_slot,
           st->tls_desc, st->tls_tprel, st->imports_bound, nm,
           st->tls_refused ? "  TLS sites refused!" : "");
    for (unsigned i = 0; i < nm; i++) printf("      %s\n", miss[i]);
}

// Dependencies first, like the loader, so the report reads in load order.
static void report_tree(const char *path, char seen[][128], int *nseen) {
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    for (int i = 0; i < *nseen; i++) if (!strcmp(seen[i], b)) return;
    if (*nseen < 256) snprintf(seen[(*nseen)++], 128, "%s", b);
    char names[32][128];
    int n = kl_list_needed(path, names, 32);
    for (int i = 0; i < n; i++) {
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", g_libdir, names[i]);
        if (access(full, R_OK) == 0) report_tree(full, seen, nseen);
    }
    report_image(b);
}

static int probe(int n, char **libs) {
    int bad = 0;
    for (int i = 0; i < n; i++) {
        char path[PATH_MAX];
        if (strchr(libs[i], '/')) snprintf(path, sizeof path, "%s", libs[i]);
        else snprintf(path, sizeof path, "%s/%s", g_libdir, libs[i]);
        printf("=== %s ===\n", path);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        kl_image *img = kl_load_recursive(path);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (!img) { printf("  FAILED: %s\n", kl_error()); bad++; continue; }
        printf("  loaded with dependencies and constructors run in %.0f ms\n",
               (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
        static char seen[256][128]; int nseen = 0;
        report_tree(path, seen, &nseen);
    }
    kl_tls_report(stdout);
    kl_glibc_report(stdout);
    return bad ? 1 : 0;
}

// KL_GLIBC_TRACE_EXIT=1: an exit() that reached Darwin's libc without passing
// through the guest's exit (host code, or a bound-early import).
static int g_host_trace_exit;
static void host_atexit(void) {
    if (!g_host_trace_exit) return;
    fprintf(stderr, "  [host] atexit: process is exiting via exit()\n");
    kl_fault_print_frames(stderr, __builtin_frame_address(0));
}
int main(int argc, char **argv) {
    { const char *e = getenv("KL_GLIBC_TRACE_EXIT"); g_host_trace_exit = e && *e && *e != '0'; }
    atexit(host_atexit);
    const char *game = NULL;
    int do_probe = 0, first_lib = 0, engine_args = argc;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--libdir") && i + 1 < argc) g_libdir = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--probe")) { do_probe = 1; first_lib = i + 1; break; }
        else if (!strcmp(argv[i], "--")) { engine_args = i + 1; break; }
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    // The engine's console output goes through the guest's printf to our stdout
    // and the process ends with _exit(); unbuffered, nothing is lost.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (!g_libdir) g_libdir = getenv("ALYX_LIBDIR");
    if (!g_libdir) { fprintf(stderr, "--libdir DIR (or ALYX_LIBDIR) is required\n"); return 2; }
    static char libdir_abs[PATH_MAX];
    if (realpath(g_libdir, libdir_abs)) g_libdir = libdir_abs;     // the engine chdirs

    // glibc libraries keep string tables beside functions; the loader's
    // "looks like data" test refused a real thread-pointer read in libtier0.
    setenv("KL_TLS_REWRITE_FORCE", "1", 0);
    kl_glibc_install(argv[0]);
    kl_set_library_path(g_libdir);
    kl_thread_init();
    kl_fault_install();            // faults are reported as image+offset
    kl_fault_add_reporter(kl_x18_report);   // ...with the x18 encodings the veneer pass refused
#ifdef KLEPTON_MVK_DIR
    setenv("KL_MVK_DIR", KLEPTON_MVK_DIR, 0);     // MoltenVK, by absolute path: the engine chdirs
#endif
    // The engine's desktop window becomes a synthetic swapchain; keep it small.
    setenv("KL_VK_WIDTH", "1280", 0);
    setenv("KL_VK_HEIGHT", "720", 0);
    // SDL2's dummy video driver: no X11, no Wayland. The HMD is our OpenVR runtime.
    setenv("SDL_VIDEODRIVER", "dummy", 0);
    setenv("SDL_AUDIODRIVER", "dummy", 0);

    if (do_probe) return probe(argc - first_lib, argv + first_lib);

    if (!game) { fprintf(stderr, "--game DIR (the Half-Life Alyx directory holding game/) is required to run\n"); return 2; }
    char exedir[PATH_MAX], exe[PATH_MAX];
    snprintf(exedir, sizeof exedir, "%s/game/bin/linuxsteamrtarm64", game);
    snprintf(exe, sizeof exe, "%s/hlvr", exedir);
    kl_glibc_set_exe_path(exe);
    argv[engine_args - 1] = exe;                 // argv[0] as hlvr would see it
    kl_glibc_set_cmdline(argc - engine_args + 1, argv + engine_args - 1);
    if (chdir(exedir) != 0) fprintf(stderr, "chdir %s: %s\n", exedir, strerror(errno));

    kl_enginelog_install();        // the engine's console, to stderr
    kl_sdlshim_install();          // SDL window without X11; Vulkan surface from the shim
    kl_sdlaudio_install();         // SDL2 audio -> Core Audio (Linux SDL has no macOS backend)
    kl_luafix_install();           // LuaJIT arena above 4 GB (no low memory on arm64 macOS)
    install_vr_path_registry();
    kl_steambridge_install();      // steam_api -> the macOS Steam client
    // fontconfig wants a writable cache; keep it beside the game root.
    { char cache[PATH_MAX]; snprintf(cache, sizeof cache, "%s/cache", game); mkdir(cache, 0755); setenv("XDG_CACHE_HOME", cache, 0); }
    char hlvrdir[PATH_MAX];
    snprintf(hlvrdir, sizeof hlvrdir, "%s/game/hlvr", game);
    vrrt_init(hlvrdir);

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/libengine2.so", g_libdir);
    kl_image *engine = kl_load_recursive(path);
    if (!engine) { fprintf(stderr, "libengine2.so: %s\n", kl_error()); return 1; }
    kl_enginelog_bind(kl_find_image("libtier0.so"));
    void *entry = kl_sym(engine, "Source2Main");
    if (!entry) { fprintf(stderr, "libengine2.so exports no Source2Main\n"); return 1; }

    // hlvr: the command line is argv[1..] joined with spaces, the "exe dir" is
    // the launcher's own directory, and Source2Main(1, 0, cmdline, 0, exedir,
    // "hlvr") runs on a thread tier0 sets up (Plat_HookSpecialThread, with a
    // 512 KB argument and the thread name "Source2Main").
    size_t len = 1;
    for (int i = engine_args; i < argc; i++) len += strlen(argv[i]) + 1;
    char *cmdline = calloc(len, 1);
    for (int i = engine_args; i < argc; i++) { if (i > engine_args) strcat(cmdline, " "); strcat(cmdline, argv[i]); }
    g_args.fn = entry; g_args.hinstance = 1; g_args.cmdline = cmdline; g_args.exedir = exedir;
    fprintf(stderr, "Source2Main(1, 0, \"%s\", 0, \"%s\", \"hlvr\")\n", cmdline, exedir);

    kl_image *tier0 = kl_find_image("libtier0.so");
    long (*hook)(int, long (*)(void *), void *, size_t, const char *, void *) =
        tier0 ? (long (*)(int, long (*)(void *), void *, size_t, const char *, void *))kl_sym(tier0, "Plat_HookSpecialThread") : NULL;
    long rc;
    if (hook && !getenv("ALYX_NO_SPECIAL_THREAD")) rc = hook(0, source2_tramp, &g_args, 0x80000, "Source2Main", NULL);
    else rc = source2_tramp(&g_args);
    fprintf(stderr, "Source2Main returned %ld\n", rc);
    // The engine may have redirected fd 2 by then; say it where it is seen.
    { const char *rf = getenv("ALYX_RESULT_FILE");
      FILE *f = rf ? fopen(rf, "a") : NULL;
      if (f) { fprintf(f, "Source2Main returned %ld\n", rc); fclose(f); } }
    kl_x18_report(stderr);
    kl_tls_report(stderr);
    return (int)rc;
}
