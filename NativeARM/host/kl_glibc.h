// glibc symbol layer on top of Klepton's bionic shim — see kl_glibc.c.
#ifndef KL_GLIBC_H
#define KL_GLIBC_H
#include <stdio.h>

// Install as Klepton's kl_shim_override: glibc-only names are answered here,
// everything else falls through to Klepton's bionic → Darwin tables. argv0
// feeds __progname / program_invocation_name.
void kl_glibc_install(const char *argv0);
void kl_glibc_report(FILE *f);
// What readlink("/proc/self/exe") answers (the launcher path tier0 expects).
void kl_glibc_set_exe_path(const char *path);
// What /proc/self/cmdline reads (argv, NUL separated).
void kl_glibc_set_cmdline(int argc, char **argv);
#endif
