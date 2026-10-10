// kl_gles3jni — drbeef's GLES3JNILib front door, generalized.
//
// Team Beef's VR ports (JKXR, and Lambda1VR = Half-Life on Xash3D) put their
// natives as static Java_com_drbeef_<pkg>_GLES3JNILib_* exports on a plain
// Activity: onCreate builds the engine and returns a jlong handle, and every
// lifecycle callback after it is keyed on that handle. kl_jkxr drives exactly
// this for JKXR but is bound to that title's data layout (JK3/base, pk3 copying,
// ja/jo tokens); this file is the engine-agnostic core for the ones that just
// need the call sequence — hl1 (com.drbeef.lambda1vr), whose data is the staged
// xash/valve tree and needs no copying. The library graph is mapped by the
// shared kl_native loader.
#ifndef KL_GLES3JNI_H
#define KL_GLES3JNI_H

#include <stdio.h>

int         kl_gles3jni_configure(const char *libdir, const char *entry_lib, FILE *out);
const char *kl_gles3jni_error(void);
int         kl_gles3jni_load(FILE *out);   // map graph + engine JNI_OnLoad
unsigned    kl_gles3jni_gap(FILE *out);
int         kl_gles3jni_create(FILE *out); // GLES3JNILib_onCreate -> handle
void        kl_gles3jni_start(FILE *out);  // onStart/onResume/onSurfaceCreated/Changed
double      kl_gles3jni_pump(double seconds, const volatile int *quit);
void        kl_gles3jni_report(FILE *out);

#endif
