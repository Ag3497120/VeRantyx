// The glibc personality: what a Linux (glibc, libstdc++) guest imports that
// Klepton's Bionic-shaped shim table does not answer, or answers with the wrong
// ABI. Installed as kl_shim_override, so it is consulted FIRST.
#ifndef KL_GLIBC_H
#define KL_GLIBC_H
#include "klepton.h"

// Answer a glibc-guest import, or NULL to fall through to Klepton's shims.
void *kl_glibc_lookup(const char *name);

// The guest's own C++ runtime images (Debian libstdc++.so.6 / libgcc_s.so.1).
// C++ ABI and unwinder names bind to these before any host shim, so guest
// exceptions never meet Darwin's libc++abi.
void kl_glibc_set_cxx_images(kl_image *libstdcxx, kl_image *libgcc_s);

// The guest process identity: argv for __libc_start_main, and the path
// readlink("/proc/self/exe") answers (the launcher finds its bin dir by it).
void kl_glibc_set_process(int argc, char **argv, const char *exe_path);

// Sonames the personality serves itself (libc.so.6, libpthread.so.0, ...).
int kl_glibc_serves_soname(const char *soname);
#endif
