// Half-Life: Alyx's boot sequence (kl_alyx.c).
#pragma once
int   kl_alyx_serves_soname(const char *soname);
void *kl_alyx_lookup(const char *name);
void  kl_alyx_install_hooks(void);
// Runs Valve's launcher; returns only on a failure to start it. <game> is the
// depot's game/ folder, <sysroot> where libstdc++.so.6 / libgcc_s.so.1 are
// named from (translations are resolved by name through KL_DYLIB_DIR).
int   kl_alyx_run(const char *game, const char *sysroot, int argc, char **argv);
