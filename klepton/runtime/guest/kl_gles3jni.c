// kl_gles3jni — see kl_gles3jni.h. The GLES3JNILib call sequence, lifted from
// kl_jkxr (which owns the JKXR-specific data layout) and made engine-agnostic:
// the native prefix is discovered from the engine's own exports, and there is no
// asset copying — the data tree is staged as-is.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "klepton.h"
#include "kl_gles3jni.h"
#include "kl_native.h"
#include "kl_jni.h"
#include "kl_ndk.h"
#include "kl_egl.h"
#include "kl_openxr.h"
#include "kl_env.h"

static char        g_libdir[1024];
static char        g_entry[256];         // "libxash.so"
static char        g_prefix[128];        // "Java_com_drbeef_lambda1vr_GLES3JNILib_"
static kl_image   *g_engine;
static long long   g_handle;
static char        g_error[256];

static int g3_fail(const char *msg) { snprintf(g_error, sizeof g_error, "%s", msg); return -1; }
const char *kl_gles3jni_error(void) { return g_error[0] ? g_error : "(none)"; }

int kl_gles3jni_configure(const char *libdir, const char *entry_lib, FILE *out) {
    if (!libdir || !entry_lib || !*entry_lib) return g3_fail("no library directory / entry");
    snprintf(g_libdir, sizeof g_libdir, "%s", libdir);
    snprintf(g_entry,  sizeof g_entry,  "%s.so", entry_lib);
    kl_jni_set_native_lib_dir(libdir);   // nativeLibraryDir / dlopen-by-name resolve here
    kl_engine_setenv(libdir, entry_lib, out);   // XASH3D_* the engine getenv's (was strdup(NULL))
    if (out) { fprintf(out, "  [g3] %s / %s\n", g_libdir, g_entry); fflush(out); }
    return kl_native_configure(libdir, entry_lib, out) == 0 ? 0 : g3_fail(kl_native_error());
}

typedef long long g3_jlong;
typedef g3_jlong (*g3_fn_create)(void *env, void *cls, void *activity, void *cmdline);
typedef void (*g3_fn_h)(void *env, void *cls, g3_jlong h);
typedef void (*g3_fn_ho)(void *env, void *cls, g3_jlong h, void *obj);

static void *g3_native(const char *name) {
    if (!g_engine || !g_prefix[0]) return NULL;
    char sym[256];
    snprintf(sym, sizeof sym, "%s%s", g_prefix, name);
    return kl_sym(g_engine, sym);
}

// Find the Java_com_drbeef_<pkg>_GLES3JNILib_ prefix the engine actually exports,
// by probing onCreate under each known Team Beef package token. Keeps the driver
// independent of which title this is.
static int g3_detect_prefix(FILE *out) {
    static const char *const PKGS[] = { "lambda1vr", "jkxr" };
    for (size_t i = 0; i < sizeof PKGS / sizeof *PKGS; i++) {
        char sym[256];
        snprintf(sym, sizeof sym, "Java_com_drbeef_%s_GLES3JNILib_onCreate", PKGS[i]);
        if (kl_sym(g_engine, sym)) {
            snprintf(g_prefix, sizeof g_prefix, "Java_com_drbeef_%s_GLES3JNILib_", PKGS[i]);
            // The Activity the engine keeps a global ref to and calls back into
            // (haptics, keyboard) — its own package's GLES3JNIActivity.
            char act[128];
            snprintf(act, sizeof act, "com/drbeef/%s/GLES3JNIActivity", PKGS[i]);
            kl_jni_set_activity_class(act);
            if (out) { fprintf(out, "  [g3] GLES3JNILib package: com.drbeef.%s\n", PKGS[i]); fflush(out); }
            return 0;
        }
    }
    return g3_fail("no Java_com_drbeef_*_GLES3JNILib_onCreate export found");
}

int kl_gles3jni_load(FILE *out) {
    if (kl_native_load(out) != 0) return g3_fail(kl_native_error());   // maps graph + engine JNI_OnLoad
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_libdir, g_entry);
    g_engine = kl_find_image(path);
    if (!g_engine) return g3_fail("the engine library is not in the image registry");
    return g3_detect_prefix(out);   // sets g_prefix and the activity class
}

unsigned kl_gles3jni_gap(FILE *out) { return kl_native_gap(out); }

#define G3_CALL(out, name, type, ...)                                          \
    do {                                                                        \
        type fn__ = (type)g3_native(name);                                       \
        if (!fn__) { if (out) fprintf(out, "  [g3] %s — not exported, skipped\n", name); } \
        else { if (out) { fprintf(out, "  [g3] %s\n", name); fflush(out); }      \
               fn__(kl_jni_env(), NULL, ##__VA_ARGS__); }                        \
    } while (0)

// The android.view.Surface the Activity hands over — one object for the run; the
// guest turns it into the runtime's single synthetic window via ANativeWindow_fromSurface.
static void *g3_surface(void) {
    static void *s;
    if (!s) s = kl_jni_new_object("android/view/Surface");
    return s;
}

int kl_gles3jni_create(FILE *out) {
    if (!g_engine) return g3_fail("the engine was never loaded");
    g3_fn_create create = (g3_fn_create)g3_native("onCreate");
    if (!create) return g3_fail("the engine exports no GLES3JNILib_onCreate");
    // Command line: the engine tokenizes this string into argv and also reads
    // commandline.txt itself, so args here are supplementary. It must NOT be empty:
    // lambda1vr's onCreate guards its FIRST strdup against an empty/NULL commandline
    // (leaving the pointer NULL) but then strdup()s that pointer a second time
    // UNGUARDED — an empty string segfaults in strdup->strlen(NULL). Hand it a
    // benign argv[0] program name (the engine ignores argv[0]) so the token is
    // never empty; KL_GLES3JNI_ARGS overrides for real args.
    const char *extra = kl_env_str("KL_GLES3JNI_ARGS", "xash3d");
    // The engine selects its game from the argv `-game <mod>` parm; XASH3D_GAMEDIR
    // env alone was not enough (it kept falling back to valve). commandline.txt
    // holds the real `-game HL_Gold_HD`, but the guest doesn't tokenize it into the
    // argv the game-selection reads — so pass it here. Only when the driver resolved
    // a non-valve gamedir (kl_native.c's -game parse -> XASH3D_GAMEDIR) and the
    // caller hasn't already put a -game in KL_GLES3JNI_ARGS.
    char argbuf[512];
    const char *gd = getenv("XASH3D_GAMEDIR");
    if (gd && *gd && strcmp(gd, "valve") != 0 && !strstr(extra, "-game")) {
        snprintf(argbuf, sizeof argbuf, "%s -game %s", extra, gd);
        extra = argbuf;
    }
    if (out) { fprintf(out, "  [g3] onCreate(\"%s\")\n", extra); fflush(out); }
    kl_jni_local_frame_push();
    g_handle = create(kl_jni_env(), NULL, kl_jni_activity(), kl_jni_new_string(extra));
    kl_jni_local_frame_pop();
    if (out) { fprintf(out, "  [g3] handle 0x%llx\n", (unsigned long long)g_handle); fflush(out); }
    if (!g_handle) return g3_fail("GLES3JNILib_onCreate returned a zero handle");
    return 0;
}

void kl_gles3jni_start(FILE *out) {
    if (!g_handle) return;
    G3_CALL(out, "onStart", g3_fn_ho, g_handle, kl_jni_activity());
    G3_CALL(out, "onResume", g3_fn_h, g_handle);
    G3_CALL(out, "onSurfaceCreated", g3_fn_ho, g_handle, g3_surface());
    G3_CALL(out, "onSurfaceChanged", g3_fn_ho, g_handle, g3_surface());
}

double kl_gles3jni_pump(double seconds, const volatile int *quit) {
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    double elapsed = 0;
    for (unsigned t = 0; (quit ? !*quit : 1) && (seconds < 0 || elapsed < seconds); t++) {
        kl_ndk_pump_looper(100);
        if ((t + 1) % 10 == 0) kl_jni_drain_ui_tasks();
        if ((t + 1) % 20 == 0) kl_xash_tail_enginelog();   // stream engine.log -> our log
        kl_jni_tick_choreographer();
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (double)(now.tv_sec - t0.tv_sec) + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return elapsed;
}

// Inject a console command string into the running Xash engine via its exported
// Cbuf_AddText (the command buffer, executed next server frame). Used to drive the
// guest from Klepton — e.g. the two-grip gesture that kicks the stuck intro-guard
// sequence. No-op for any guest that isn't the Xash engine (Cbuf_AddText absent).
void kl_xash_console_cmd(const char *cmd) {
    if (!g_engine || !cmd || !*cmd) return;
    static void (*addtext)(const char *) = NULL;
    static int resolved = 0;
    if (!resolved) { resolved = 1; addtext = (void (*)(const char *))kl_sym(g_engine, "Cbuf_AddText"); }
    if (addtext) { addtext(cmd); fprintf(stderr, "  [xash] console: %s", cmd); }
}

void kl_gles3jni_report(FILE *out) {
    if (!out) return;
    kl_openxr_report(out);
    kl_egl_report(out);
    fprintf(out, "\n=== JNI surface ===\n");
    kl_jni_report(out);
    fflush(out);
}
