#ifndef KL_NATIVE_H
#define KL_NATIVE_H
#include <stdio.h>

// A plain Android NativeActivity + OpenXR guest: the app is entered through
// ANativeActivity_onCreate and runs its own loop on the thread that
// android_native_app_glue spawns for android_main, driving frames itself via
// OpenXR. No Unity, no Unreal, no engine-specific Java lifecycle — just the
// shared kl_nativeactivity harness. GTA Vice City VR (libmiamivr, a reVC/re3
// port) is the first guest through this door; other homebrew Quest ports of the
// same shape fit here without a new kind.
// Set the environment an SDL2/Xash/Source guest expects its Java launcher to
// have published (via nativeSetenv from manifest metadata) — the base/data/lib
// directories it reads with getenv() and would otherwise strdup(NULL) on. The
// engine family is chosen from `entry_lib`: libxash -> XASH3D_*, liblauncher ->
// Source's EXECUTABLE_PATH/APP_DATA_PATH. Safe to call for any entry (a no-op
// for names it does not recognise).
void        kl_engine_setenv(const char *libdir, const char *entry_lib, FILE *out);
// Live-tail the Xash -log (engine.log) into stderr; call periodically from a pump.
void        kl_xash_tail_enginelog(void);

int         kl_native_configure(const char *libdir, const char *entry_lib, FILE *out);
const char *kl_native_error(void);
int         kl_native_load(FILE *out);      // map + static init + JNI_OnLoad (if any)
unsigned    kl_native_gap(FILE *out);       // report unresolved imports
int         kl_native_create(FILE *out);    // ANativeActivity_onCreate
void        kl_native_start(FILE *out);     // onStart/onResume/onNativeWindowCreated
void        kl_native_stop(FILE *out);
double      kl_native_pump(double seconds, const volatile int *quit);
void        kl_native_report(FILE *out);
#endif
