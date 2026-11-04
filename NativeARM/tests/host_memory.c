// Exercise the actual Linux memory queries, without a guest, Steam or GPU.
#include "klepton.h"
#include "kl_glibc.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

struct linux_sysinfo {
    int64_t uptime;
    uint64_t loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
};
_Static_assert(sizeof(struct linux_sysinfo) == 112, "Linux arm64 sysinfo ABI");

int main(int argc, char **argv) {
    assert(argc == 2);
    uint64_t host = 0; size_t size = sizeof host;
    assert(sysctlbyname("hw.memsize", &host, &size, NULL, 0) == 0);
    uint64_t expected = host;
    if (!strcmp(argv[1], "--limited")) {
        setenv("KL_MEM_TOTAL_MB", "2048", 1);
        if (expected > (2048ull << 20)) expected = 2048ull << 20;
    } else { assert(!strcmp(argv[1], "--default")); unsetenv("KL_MEM_TOTAL_MB"); }
    kl_glibc_install("synthetic-memory-test");
    int (*guest_sysinfo)(void *) = kl_shim_lookup("sysinfo");
    long (*guest_sysconf)(int) = kl_shim_lookup("sysconf");
    void *(*guest_fopen)(const char *, const char *) = kl_shim_lookup("fopen");
    int (*guest_fclose)(void *) = kl_shim_lookup("fclose");
    char *(*guest_fgets)(char *, int, void *) = kl_shim_lookup("fgets");
    assert(guest_sysinfo && guest_sysconf && guest_fopen && guest_fclose && guest_fgets);
    struct { struct linux_sysinfo info; uint64_t guard; } out = {.guard = 0x123456789abcdef0ull};
    assert(guest_sysinfo(&out.info) == 0 && out.guard == 0x123456789abcdef0ull);
    uint64_t total = out.info.totalram * out.info.mem_unit;
    printf("Linux sysinfo: %llu MiB, Mac: %llu MiB, expected: %llu MiB\n",
           total >> 20, host >> 20, expected >> 20);
    assert(total == expected && out.info.freeram * out.info.mem_unit <= total);
    assert((uint64_t)guest_sysconf(85) * guest_sysconf(30) == total);
    long available = guest_sysconf(86);
    assert(available >= 0 && available <= guest_sysconf(85));
    void *file = guest_fopen("/proc/meminfo", "r"); assert(file);
    char line[256]; uint64_t procTotal = 0;
    while (guest_fgets(line, sizeof line, file)) {
        unsigned long long kb;
        if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) procTotal = kb * 1024;
    }
    assert(guest_fclose(file) == 0 && procTotal == total);
    puts("Native host: sysinfo, /proc/meminfo and glibc page counts agree on the Mac budget");
}
