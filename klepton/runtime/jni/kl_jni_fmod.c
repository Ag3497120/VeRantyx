// org.fmod.FMOD — FMOD's Android Java helper class.
//
// FMOD's native core (libfmod.so) does not open the audio device itself on
// Android through raw syscalls; it goes through a small Java shim, org.fmod.FMOD
// (from fmod.jar), which owns the Android Context and answers the platform
// queries the native mixer needs — output sample rate, block size, whether the
// low-latency / AAudio fast paths are available. FMOD's JNI_OnLoad resolves this
// class and org.fmod.{AudioDevice,MediaCodec}; the native side then calls
// FMOD.checkInit() before it will bring the core up. With no host implementation
// the checkInit() call aborted (vampire), so FMOD::getGlobals / System::create
// never ran and there was no audio.
//
// There is no real Java here, so this answers the class synthetically. The
// output itself is native — FMOD picks OpenSL (kl_opensl) once it believes its
// Java env is up — so the only job of these methods is to tell FMOD "your Java
// side is initialised, here is a sane output format". checkInit() true is the
// gate; the format getters match what kl_audio resamples to (48 kHz), and AAudio
// is reported UNsupported so FMOD stays on the OpenSL path Klepton actually
// serves rather than the AAudio one.
#include <stdint.h>
#include "kl_jni.h"
#include "kl_jni_int.h"

#define FMODCLS "org/fmod/FMOD"

static klj_val klf_true(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 1};
}
static klj_val klf_false(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 0};
}
static klj_val klf_void(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 0};
}
// The Android Context FMOD stashes and hands back to its own code. The activity
// is the context here, exactly as GameActivity is elsewhere.
static klj_val klf_getContext(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n;
    return (klj_val){.l = kl_jni_activity()};
}
// The device's output format, as the native mixer configures itself from it.
// 48 kHz is what kl_audio's CoreAudio unit runs at; a mismatch just makes FMOD
// resample, but matching avoids a second resample on top of ours. 512 frames is
// a conventional Android output block.
static klj_val klf_sampleRate(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 48000};
}
static klj_val klf_blockSize(void *env, void *self, const klj_val *a, int n) {
    (void)env; (void)self; (void)a; (void)n; return (klj_val){.j = 512};
}

const klj_binding klj_bind_fmod[] = {
    // The gate the native side checks before creating the core system.
    {FMODCLS, "checkInit",           "()Z",                    klf_true},
    // init(context)/close() — the Java side would store/clear the context; there
    // is nothing to store here (getContext answers from the activity), so no-ops.
    {FMODCLS, "init",                "(Landroid/content/Context;)V", klf_void},
    {FMODCLS, "init",                "(Ljava/lang/Object;)V",  klf_void},
    {FMODCLS, "close",               "()V",                    klf_void},
    {FMODCLS, "getContext",          "()Landroid/content/Context;", klf_getContext},
    {FMODCLS, "getContext",          "()Ljava/lang/Object;",   klf_getContext},
    // Output format + fast-path capabilities the native mixer queries.
    {FMODCLS, "getOutputSampleRate", "()I",                    klf_sampleRate},
    {FMODCLS, "getOutputBlockSize",  "()I",                    klf_blockSize},
    {FMODCLS, "supportsLowLatency",  "()Z",                    klf_true},
    // AAudio reported UNsupported so FMOD stays on the OpenSL output Klepton
    // serves (kl_opensl); its AAudio backend is not wired here.
    {FMODCLS, "supportsAAudio",      "()Z",                    klf_false},
    {0}
};
