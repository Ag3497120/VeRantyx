// The Alyx app's own files, read before the guest starts (kl_alyx_app.c).
#pragma once
#include <stddef.h>
// <res>: the app bundle (its alyx-demo/ folder holds the bundled defaults); <docs>: the app's Documents, with
// the game under alyx/assets. Applies the env file, points KL_LUA_INJECT at the convars, writes the game's
// fidelity table and video settings once per bundled version and makes the soname placeholders. <envline> gets what the env
// file set and <filesline> where each file came from, both for the boot log.
void kl_alyx_app_files(const char *res, const char *docs, char *envline, size_t envsz,
                       char *filesline, size_t filessz);
// On the headset: files pushed into Documents count only for a test run (kl_alyx_app_test_run); otherwise they are
// set aside (kl_alyx_app.c). Call before kl_alyx_app_files. The Mac's m_alyx doesn't.
void kl_alyx_app_test_marker_required(int on);
// 1 when <docs>/alyx-env.txt has the line GLOVEBOX_TEST=1 and was pushed for the app at <res> as installed now (no
// install since it was first seen): a test script's files for this install (tools/alyx-test/morning.sh).
int kl_alyx_app_test_run(const char *res, const char *docs);
