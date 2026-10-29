// Synthetic calls into the actual host shim; no ELF guest, Steam, GPU or encoder.
#include "klepton.h"
#include "kl_glibc.h"
#include "kl_steambridge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

int main(int argc, char **argv) {
    assert(argc == 2);
    char path[PATH_MAX], actual[PATH_MAX], guest[PATH_MAX];
    snprintf(path, sizeof path, "%s/open-mode", argv[1]);
    setenv("VERANTYX_STEAM_STATE", argv[1], 1);
    const char *home = getenv("HOME"); assert(home);
    snprintf(guest, sizeof guest, "%s/.steam/test-service", home);
    snprintf(actual, sizeof actual, "%s/test-service", argv[1]);
    assert(!strcmp(kl_steambridge_path(guest), actual));
    snprintf(guest, sizeof guest, "%s/.steam-other", home);
    assert(!strcmp(kl_steambridge_path(guest), guest));
    kl_glibc_install("synthetic-file-test");
    int (*guest_open)(const char *, int, int) = kl_shim_lookup("open");
    int (*guest_openat)(int, const char *, int, int) = kl_shim_lookup("openat");
    int (*guest_creat)(const char *, unsigned) = kl_shim_lookup("creat");
    assert(guest_open && guest_openat && guest_creat);
    mode_t mask = umask(0);
    int fd = guest_open(path, 01 | 0100 | 0200, 0640); assert(fd >= 0); close(fd);
    struct stat st; assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640);
    unlink(path);
    fd = guest_openat(-100, path, 01 | 0100 | 0200, 0600); assert(fd >= 0); close(fd);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600); unlink(path);
    fd = guest_creat(path, 0644); assert(fd >= 0); close(fd);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0644); unlink(path);
    umask(mask);
    puts("Native host: Linux open/openat/creat ABI, file modes and private Steam path mapping passed");
    return 0;
}
