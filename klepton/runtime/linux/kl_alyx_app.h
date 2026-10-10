// The Alyx app's own files, read before the guest starts (kl_alyx_app.c).
#pragma once
#include <stddef.h>
// <res>: the app bundle (its alyx-demo/ folder holds the bundled defaults); <docs>: the app's Documents, with
// the game under alyx/assets. Applies the env file, points KL_LUA_INJECT at the convars, writes the game's
// fidelity table and video settings when missing and makes the soname placeholders. <envline> gets what the env
// file set and <filesline> where each file came from, both for the boot log.
void kl_alyx_app_files(const char *res, const char *docs, char *envline, size_t envsz,
                       char *filesline, size_t filessz);
