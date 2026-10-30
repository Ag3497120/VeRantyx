// Steam for the Linux build: the real Steam client, no emulation.
//
// Valve's Linux ARM64 libsteam_api.so looks for a running Steam client through
// ~/.steam/steam.pid and then dlopens ~/.steam/sdkarm64/steamclient.so, the
// client-side half of the Steamworks IPC. There is no Linux Steam client on this
// Mac, but there is a macOS one, and its steamclient.dylib ships an arm64 slice:
// the same C++ interfaces (Itanium ABI on both sides), talking to the macOS Steam
// process over its own IPC. So that dlopen is answered with the macOS library:
// the game's steam_api binds to it and Steam sees the game as it should.
//
// Requirements: macOS Steam.app running and signed in by the user (owner of the
// game); nothing here bypasses or imitates Steam.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <signal.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include "klepton.h"
#include "kl_steambridge.h"

static void *g_steamclient;
static char g_dylib[PATH_MAX];

// The macOS steamclient's Breakpad: Breakpad_SteamMiniDumpInit takes the
// task's Mach exception port, so every fault in the process (including the
// ones Klepton's own reporter explains) becomes a minidump, a crash_report_sender
// launch and a silent _exit(1). Crash reporting is not part of reaching Steam;
// the Breakpad_* calls are answered with no-ops and faults stay ours.
static long breakpad_noop(void) { return 0; }

static void *steamclient_sym(const char *name) {
    if (!g_steamclient) return NULL;
    if (!strncmp(name, "Breakpad_", 9)) {
        static int said;
        if (!said++) fprintf(stderr, "  [steam] steamclient.so %s and the other Breakpad_* calls -> no-op "
                                     "(Steam's crash handler would take the Mach exception port)\n", name);
        return (void *)breakpad_noop;
    }
    void *p = dlsym(g_steamclient, name);
    static int n; if (n++ < 8) fprintf(stderr, "  [steam] steamclient.so dlsym(%s) -> %p\n", name, p);
    return p;
}

// The pid of the macOS Steam client, or 0.
static pid_t steam_pid(void) {
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) != 0) return 0;
    struct kinfo_proc *procs = malloc(len);
    if (!procs || sysctl(mib, 4, procs, &len, NULL, 0) != 0) { free(procs); return 0; }
    pid_t found = 0;
    for (size_t i = 0; i < len / sizeof *procs; i++)
        if (!strcmp(procs[i].kp_proc.p_comm, "steam_osx")) { found = procs[i].kp_proc.p_pid; break; }
    free(procs);
    return found;
}

static const char *find_dylib(void) {
    const char *env = getenv("VERANTYX_STEAMCLIENT_DYLIB");
    if (env && *env && access(env, R_OK) == 0) return env;
    const char *home = getenv("HOME");
    snprintf(g_dylib, sizeof g_dylib, "%s/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamclient.dylib", home ? home : "");
    if (access(g_dylib, R_OK) == 0) return g_dylib;
    // steamcmd ships the same universal library; useful until Steam.app has bootstrapped.
    FILE *p = popen("ls -d /opt/homebrew/Caskroom/steamcmd/*/MacOS/steamclient.dylib 2>/dev/null | head -1", "r");
    if (p) { if (fgets(g_dylib, sizeof g_dylib, p)) g_dylib[strcspn(g_dylib, "\n")] = 0; pclose(p); if (g_dylib[0] && access(g_dylib, R_OK) == 0) return g_dylib; }
    return NULL;
}

const char *kl_steambridge_path(const char *path) {
    const char *state = getenv("VERANTYX_STEAM_STATE"), *home = getenv("HOME");
    if (!path || !state || !home) return path;
    size_t n = strlen(home);
    if (strncmp(path, home, n) || strncmp(path + n, "/.steam", 7) || (path[n + 7] && path[n + 7] != '/')) return path;
    static _Thread_local char mapped[PATH_MAX];
    snprintf(mapped, sizeof mapped, "%s%s", state, path + n + 7);
    return mapped;
}

void kl_steambridge_install(void) {
    if (getenv("VERANTYX_NO_STEAM")) return;
    const char *dylib = find_dylib();
    const char *home = getenv("HOME");
    if (!home) return;
    char dir[PATH_MAX], sub[PATH_MAX], f[PATH_MAX];
    const char *state = getenv("VERANTYX_STEAM_STATE");
    if (state) snprintf(dir, sizeof dir, "%s", state);
    else snprintf(dir, sizeof dir, "%s/.steam", home);
    snprintf(sub, sizeof sub, "%s/sdkarm64", dir);
    mkdir(dir, 0755); mkdir(sub, 0755);
    pid_t pid = steam_pid();
    if (!pid) { fprintf(stderr, "  [steam] Steam.app must be running and signed in\n"); return; }
    snprintf(f, sizeof f, "%s/steam.pid", dir);
    FILE *fp = fopen(f, "w");
    if (fp) { fprintf(fp, "%d\n", (int)pid); fclose(fp); }
    snprintf(f, sizeof f, "%s/steamclient.so", sub);
    fp = fopen(f, "a"); if (fp) fclose(fp);             // the path must exist; dlopen is answered here
    if (dylib) {
        g_steamclient = dlopen(dylib, RTLD_NOW | RTLD_LOCAL);
        fprintf(stderr, "  [steam] macOS steamclient %s: %s; Steam.app %s\n", dylib,
                g_steamclient ? "loaded" : dlerror(), pid ? "running" : "NOT running (sign in to macOS Steam for the game to see Steam)");
    } else {
        fprintf(stderr, "  [steam] no macOS steamclient.dylib found (install and start Steam.app, or set VERANTYX_STEAMCLIENT_DYLIB)\n");
    }
    setenv("SteamAppId", "546560", 0);
    setenv("SteamGameId", "546560", 0);
    kl_dl_add_host_module("/steamclient.so", steamclient_sym);
}
