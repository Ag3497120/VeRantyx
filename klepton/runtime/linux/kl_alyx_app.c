// The Alyx app's own files, read before the guest starts: kl_app.c on the headset, and m_alyx on the Mac when
// KL_APP_DOCS is set (the same code against a folder laid out like the app's container).
//
// Each comes from Documents when a test script pushed one there (tools/alyx-test/morning.sh), else from the
// defaults bundled in the app (<bundle>/alyx-demo: the locked DEMO settings), so an install with nothing pushed
// plays at DEMO and our scripts keep working unchanged:
//   alyx-env.txt      KEY=VALUE lines, applied before any default ('#' starts a comment). In a value "@demo/"
//                     is the bundled folder and "@docs/" Documents, e.g. KL_VK_FOZ_REPLAY=@demo/demo.foz:@docs/klfz
//   alyx-convars.lua  stands in for the game's (empty) game/gameinit.lua (KL_LUA_INJECT, kl_lua.c): the render
//                     convars, set after each spawn
// Two files the game reads from its own folder are written there from the bundle once per version of the bundled
// file: a stamp beside each (<file>.glovebox) holds the version (a hash of the bundled bytes) last written, so
//   - no file yet: ours is written;
//   - stamp matches the bundle: the file is kept, with whatever the player changed since (the game saves Options
//     into video2.txt);
//   - no stamp, or an older one: ours replaces it, the old file kept beside it as <file>.before-update. This is the
//     case of every install before the stamps: the game had written its own video2.txt at its first boot (its
//     automatic setup, at the Steam Frame's low settings) and the bundled one was never installed over it. A
//     replaced video2.txt keeps the old file's VendorID/DeviceID (the GPU the game wrote it for).
//   - a stamp reading "keep" (a test script's pushed copy): never replaced while a test run is on (below).
// The game keeps video2.txt only while its VendorID and DeviceID equal the running GPU's (both read with default -1,
// so a missing line fails too); otherwise it replaces the whole file with its automatic setup at boot (measured on
// the Mac, 2026-10-11). The bundled file was a Mac's, so every headset threw it away at its first boot. So at every
// start video2.txt gets the GPU's own ids (kl_vulkan_gpu_ids: MoltenVK's, which also change with an OS update).
//   hlvr/vr_fidelity_levels_override.csv   the fidelity table (libvr loads it before its own)
//   hlvr/cfg/deckard/video2.txt            the video settings (the game saves the player's changes here)
// On the headset (kl_app.c calls kl_alyx_app_test_marker_required) every pushed file is a test run's: it counts only
// while Documents/alyx-env.txt carries the line GLOVEBOX_TEST=1 (tools/alyx-test/morning.sh, device-batch.sh) and was
// pushed for this install (kl_alyx_app_test_run). Otherwise alyx-env.txt and alyx-convars.lua are set
// aside as <name>.ignored, the bundle's apply and a "keep" stamp counts as an old one, so a test's files end with the
// next install and never carry into the build a player plays: one left behind made the headset skip the launcher (no
// cloud sync) and play the test's settings (2026-10-12). The Mac's m_alyx takes any pushed file, as before.
// A new player's defaults: while hlvr/save/personal.cfg (the player's options, which the game writes at every save
// and restores at every load) does not exist, KL_ALYX_NEWPLAYER_ARGS (bundled env: the continuous movement we
// test with) is added to the game's arguments, so the New Game panel starts there. Once the game has saved the
// player's own choices nothing is added again.
// And the soname names Valve's hlvr.sh links at every launch (libavcodec.so.58 -> libavcodec.so.58.91.100, ...)
// are made as empty files when nothing is at that name (lstat: a link the launcher made, even a dangling one, is
// kept). The engine needs the names to exist (it crashes early in its module loading without them); a download
// of the depot does not carry them, since hlvr.sh makes them, and on the device every ELF is an empty placeholder
// anyway (the code is the bundle's signed translation).
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kl_alyx_app.h"

static char g_res[1024], g_docs[1024];

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }
static int named(const char *p) { struct stat st; return lstat(p, &st) == 0; }   // the name itself, links included

static int g_need_marker, g_test_run = 1;   // headset: pushed files only for a test run; one is on
void kl_alyx_app_test_marker_required(int on) { g_need_marker = on; }

// Nanoseconds of a stat time (the field names differ between Darwin and Linux).
#ifdef __APPLE__
#define KL_NS(st, t) ((long long)(st).st_##t##timespec.tv_sec * 1000000000LL + (st).st_##t##timespec.tv_nsec)
#else
#define KL_NS(st, t) ((long long)(st).st_##t##tim.tv_sec * 1000000000LL + (st).st_##t##tim.tv_nsec)
#endif

// 1 when <path> has the line GLOVEBOX_TEST=1 (surrounding blanks allowed).
static int test_marker(const char *path) {
    char line[1024];
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *k = line; while (*k == ' ' || *k == '\t') k++;
        char *e = k + strcspn(k, "\r\n"); while (e > k && (e[-1] == ' ' || e[-1] == '\t')) e--;
        found = (size_t)(e - k) == 15 && !strncmp(k, "GLOVEBOX_TEST=1", 15);
    }
    fclose(f);
    return found;
}

// A test's files belong to the install that first sees them: Documents/alyx-env.txt.install names that install (the app
// folder, which an install makes new and nothing changes after) and that push of the file (its inode, modification time
// and size, which a new push changes; the launcher's backup mark at every start changes only its change time). The
// same push seen by a later install is an old test's. Both the launcher's gate and kl_alyx_app_files ask, the gate
// first, so the first asks claims.
int kl_alyx_app_test_run(const char *res, const char *docs) {
    char p[1100], claim[1200], want[256], have[256] = "";
    struct stat app, env;
    snprintf(p, sizeof p, "%s/alyx-env.txt", docs);
    if (!test_marker(p) || stat(res, &app) != 0 || stat(p, &env) != 0) return 0;
    int n = snprintf(want, sizeof want, "app %llu %lld\n", (unsigned long long)app.st_ino, KL_NS(app, c));
    const char *push = want + n;
    snprintf(want + n, sizeof want - n, "push %llu %lld %lld\n", (unsigned long long)env.st_ino, KL_NS(env, m),
             (long long)env.st_size);
    snprintf(claim, sizeof claim, "%s.install", p);
    FILE *f = fopen(claim, "r");
    if (f) { size_t r = fread(have, 1, sizeof have - 1, f); have[r] = 0; fclose(f); }
    if (!strcmp(have, want)) return 1;                              // this install's test
    const char *had = strstr(have, "push ");
    if (had && !strcmp(had, push)) return 0;                        // the same push, an earlier install's: an old test
    f = fopen(claim, "w");                                          // a new push: this install's
    if (f) { fputs(want, f); fclose(f); }
    return 1;
}

// Documents/<name> when pushed, else the bundled alyx-demo/<name>; NULL when neither. *from says which.
static const char *pick(const char *name, char *buf, size_t n, const char **from) {
    snprintf(buf, n, "%s/%s", g_docs, name);
    if (exists(buf)) { *from = "Documents"; return buf; }
    snprintf(buf, n, "%s/alyx-demo/%s", g_res, name);
    if (exists(buf)) { *from = "bundle"; return buf; }
    *from = "none";
    return NULL;
}

// "@demo/" and "@docs/" anywhere in an env value, as the folders they name.
static void expand(const char *v, char *out, size_t n) {
    size_t o = 0;
    out[0] = 0;
    while (*v && o + 1 < n) {
        const char *base = !strncmp(v, "@demo/", 6) ? g_res : !strncmp(v, "@docs/", 6) ? g_docs : NULL;
        if (base) {
            o += (size_t)snprintf(out + o, n - o, base == g_res ? "%s/alyx-demo/" : "%s/", base);
            v += 6;
        } else {
            out[o++] = *v++;
            out[o] = 0;
        }
    }
    if (o >= n) out[n - 1] = 0;
}

static void env_file(const char *path, char *line_out, size_t outsz) {
    char line[1024], val[2048];
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t used = strlen(line_out);
    while (fgets(line, sizeof line, f)) {
        char *e = line + strcspn(line, "\r\n"); *e = 0;
        char *k = line; while (*k == ' ' || *k == '\t') k++;
        char *eq = strchr(k, '=');
        if (*k == '#' || !eq || eq == k) continue;
        *eq = 0;
        expand(eq + 1, val, sizeof val);
        setenv(k, val, 1);
        if (used < outsz) used += (size_t)snprintf(line_out + used, outsz - used, " %s=%s", k, val);
    }
    fclose(f);
}

// mkdir -p of <path>'s parent.
static void parents(const char *path) {
    char p[1200];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
}

// A whole small file into memory (NUL-terminated); NULL when it can't be read. *len gets its size.
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 65536, n = 0;
    char *b = malloc(cap + 1);
    size_t r;
    while (b && (r = fread(b + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap) { char *g = realloc(b, (cap *= 2) + 1); if (!g) { free(b); b = NULL; } else b = g; }
    }
    fclose(f);
    if (!b) return NULL;
    b[n] = 0;
    *len = n;
    return b;
}

// The value of a "key" "value" line of a KeyValues file (video2.txt), into out; 0 when there is none.
static int kv_get(const char *text, const char *key, char *out, size_t n) {
    char q[64];
    snprintf(q, sizeof q, "\"%s\"", key);
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
        const char *k = l;
        while (*k == ' ' || *k == '\t') k++;
        if (strncmp(k, q, strlen(q))) continue;
        const char *v = strchr(k + strlen(q), '"');
        const char *e = v ? strchr(v + 1, '"') : NULL;
        if (!e || memchr(v, '\n', (size_t)(e - v))) return 0;
        snprintf(out, n, "%.*s", (int)(e - v - 1), v + 1);
        return 1;
    }
    return 0;
}

// <text> with the value of its first "key" line set to val; a new malloc'd string.
static char *kv_set(const char *text, const char *key, const char *val) {
    char q[64], cur[256];
    snprintf(q, sizeof q, "\"%s\"", key);
    if (!kv_get(text, key, cur, sizeof cur)) return strdup(text);
    size_t out_n = strlen(text) + strlen(val) + 1;
    char *out = malloc(out_n);
    if (!out) return NULL;
    out[0] = 0;
    int done = 0;
    for (const char *l = text; *l;) {
        const char *nl = strchr(l, '\n');
        size_t len = nl ? (size_t)(nl - l + 1) : strlen(l);
        const char *k = l;
        while (*k == ' ' || *k == '\t') k++;
        if (!done && !strncmp(k, q, strlen(q))) {
            const char *v = strchr(k + strlen(q), '"');
            const char *e = v ? strchr(v + 1, '"') : NULL;
            if (e && e < l + len) {
                strncat(out, l, (size_t)(v - l + 1));
                strcat(out, val);
                strncat(out, e, (size_t)(l + len - e));
                l += len;
                done = 1;
                continue;
            }
        }
        strncat(out, l, len);
        l += len;
    }
    return out;
}

// <data> written to <dst> through a temporary file and a rename; 0 on success.
static int put(const char *dst, const char *data, size_t n) {
    char tmp[1300];
    parents(dst);
    snprintf(tmp, sizeof tmp, "%s.klnew", dst);
    FILE *out = fopen(tmp, "wb");
    if (!out) return -1;
    int bad = fwrite(data, 1, n, out) != n;
    if (fclose(out) != 0 || bad || rename(tmp, dst) != 0) { unlink(tmp); return -1; }
    return 0;
}

// The GPU's Vulkan ids, from kl_vulkan.c where it is linked (the app, m_alyx); 0 without it.
__attribute__((weak)) int kl_vulkan_gpu_ids(unsigned *vendor, unsigned *device) {
    (void)vendor; (void)device;
    return 0;
}

// The game folder's <rel> (video2.txt) stamped with this GPU's VendorID and DeviceID, so the game keeps it; returns
// what happened, for the boot log (into buf).
static const char *match_gpu(const char *rel, char *buf, size_t bufsz) {
    char dst[1200], have[64], want[2][24];
    unsigned id[2];
    snprintf(dst, sizeof dst, "%s/alyx/assets/%s", g_docs, rel);
    if (!named(dst)) return "no file";
    if (!kl_vulkan_gpu_ids(&id[0], &id[1])) return "gpu unknown";
    size_t n;
    char *text = slurp(dst, &n);
    if (!text) return "unreadable";
    static const char *const keys[] = {"VendorID", "DeviceID"};
    int changed = 0;
    for (int i = 0; i < 2; i++) {
        snprintf(want[i], sizeof want[i], "%u", id[i]);
        if (kv_get(text, keys[i], have, sizeof have) && !strcmp(have, want[i])) continue;
        char *t = kv_set(text, keys[i], want[i]);
        if (!t || !strcmp(t, text)) { free(t); free(text); return "NO ID LINE"; }
        free(text);
        text = t;
        changed = 1;
    }
    int bad = changed && put(dst, text, strlen(text)) != 0;
    free(text);
    snprintf(buf, bufsz, "%s %s/%s", bad ? "WRITE FAILED" : changed ? "set to" : "match", want[0], want[1]);
    return buf;
}

// The bundled <name> into the game folder's <rel>, once per version of the bundled file (the stamp rules at the
// top); returns what happened.
static const char *install(const char *name, const char *rel) {
    char src[1200], dst[1200], stamp[1300], old[1300], ver[24], have[64] = "";
    snprintf(dst, sizeof dst, "%s/alyx/assets/%s", g_docs, rel);
    snprintf(stamp, sizeof stamp, "%s.glovebox", dst);
    snprintf(src, sizeof src, "%s/alyx-demo/%s", g_res, name);
    size_t n, sn;
    char *ours = slurp(src, &n);
    if (!ours) return named(dst) ? "kept (none bundled)" : "not bundled";
    unsigned long long h = 1469598103934665603ULL;   // FNV-1a 64 of the bundled bytes: the version
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)ours[i]) * 1099511628211ULL;
    snprintf(ver, sizeof ver, "%016llx", h);
    char *st = slurp(stamp, &sn);
    if (st) { snprintf(have, sizeof have, "%.*s", (int)strcspn(st, "\r\n"), st); free(st); }
    const char *what = "written";
    if (named(dst)) {
        if (!strcmp(have, ver)) { free(ours); return "kept"; }
        if (!strcmp(have, "keep") && g_test_run) { free(ours); return "kept (pushed)"; }
        size_t on;
        char *prev = slurp(dst, &on);
        if (!prev) { free(ours); return "kept (unreadable)"; }
        // The GPU the old file was written for, so the game takes ours as its own.
        static const char *const ids[] = {"VendorID", "DeviceID"};
        for (size_t i = 0; i < 2; i++) {
            char v[64], b[64];
            if (!kv_get(prev, ids[i], v, sizeof v) || !kv_get(ours, ids[i], b, sizeof b) || !strcmp(v, b)) continue;
            char *t = kv_set(ours, ids[i], v);
            if (t) { free(ours); ours = t; n = strlen(ours); }
        }
        snprintf(old, sizeof old, "%s.before-update", dst);
        int saved = put(old, prev, on) == 0;
        free(prev);
        if (!saved) { free(ours); return "kept (backup failed)"; }
        what = !strcmp(have, "keep") ? "replaced a test copy (old one saved)"
             : have[0] ? "updated (old one saved)" : "replaced (old one saved)";
    }
    int bad = put(dst, ours, n) != 0;
    free(ours);
    if (bad) return "WRITE FAILED";
    if (put(stamp, ver, strlen(ver)) != 0) return "written (STAMP FAILED)";
    return what;
}

// hlvr.sh's links for linuxsteamrtarm64, and the SDL2 name the engine opens.
static const char *const k_sonames[] = {
    "libavcodec.so.58", "libavcodec.so", "libavformat.so.58", "libavformat.so", "libavresample.so.4",
    "libavresample.so", "libavutil.so.56", "libavutil.so", "libfontconfig.so.1", "libfontconfig.so",
    "libfreetype.so.6", "libfreetype.so", "libharfbuzz-gobject.so.0", "libharfbuzz-gobject.so",
    "libharfbuzz-icu.so.0", "libharfbuzz-icu.so", "libharfbuzz.so.0", "libharfbuzz.so",
    "libharfbuzz-subset.so.0", "libharfbuzz-subset.so", "libpango-1.0.so.0", "libpango-1.0.so",
    "libpangoft2-1.0.so.0", "libpangoft2-1.0.so", "libswscale.so.5", "libswscale.so", "libSDL2-2.0.so.0",
};

static int sonames(void) {
    char dir[1200], p[1300];
    snprintf(dir, sizeof dir, "%s/alyx/assets/bin/linuxsteamrtarm64", g_docs);
    if (!exists(dir)) return -1;
    int made = 0;
    for (size_t i = 0; i < sizeof k_sonames / sizeof k_sonames[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", dir, k_sonames[i]);
        if (named(p)) continue;
        // O_EXCL: never through a link, never over a file that appeared meanwhile.
        int fd = open(p, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) { close(fd); made++; }
    }
    return made;
}

void kl_alyx_app_files(const char *res, const char *docs, char *envline, size_t envsz,
                       char *filesline, size_t filessz) {
    snprintf(g_res, sizeof g_res, "%s", res);
    snprintf(g_docs, sizeof g_docs, "%s", docs);
    char p[1200];
    const char *env_from, *cv_from;
    envline[0] = 0;
    int aside = 0;
    g_test_run = !g_need_marker || kl_alyx_app_test_run(g_res, g_docs);
    if (!g_test_run) {
        static const char *const pushed[] = {"alyx-env.txt", "alyx-convars.lua"};
        for (size_t i = 0; i < 2; i++) {
            char from[1200], to[1300];
            snprintf(from, sizeof from, "%s/%s", g_docs, pushed[i]);
            snprintf(to, sizeof to, "%s.ignored", from);
            if (named(from) && rename(from, to) == 0) aside++;
        }
        snprintf(p, sizeof p, "%s/alyx-env.txt.install", g_docs);
        unlink(p);
    }
    const char *env = pick("alyx-env.txt", p, sizeof p, &env_from);
    if (env) {
        snprintf(envline, envsz, "  env file  :");
        env_file(env, envline, envsz);
        size_t u = strlen(envline);
        if (u + 1 < envsz) snprintf(envline + u, envsz - u, "\n");
    }
    // The convars, unless a hand-given KL_LUA_INJECT (a Mac test) names its own.
    static char spec[1300];
    const char *cv = pick("alyx-convars.lua", p, sizeof p, &cv_from);
    if (cv && !getenv("KL_LUA_INJECT")) {
        snprintf(spec, sizeof spec, "game/gameinit=%s", cv);
        setenv("KL_LUA_INJECT", spec, 1);
    }
    char hlvr[1200];
    snprintf(hlvr, sizeof hlvr, "%s/alyx/assets/hlvr", g_docs);
    const char *fid = "no game folder", *vid = "no game folder", *opt = "no game folder", *gpu = "-";
    char gpubuf[80];
    if (exists(hlvr)) {
        fid = install("vr_fidelity_levels_override.csv", "hlvr/vr_fidelity_levels_override.csv");
        vid = install("video2.txt", "hlvr/cfg/deckard/video2.txt");
        gpu = match_gpu("hlvr/cfg/deckard/video2.txt", gpubuf, sizeof gpubuf);
        char pc[1200];
        snprintf(pc, sizeof pc, "%s/save/personal.cfg", hlvr);
        const char *np = getenv("KL_ALYX_NEWPLAYER_ARGS");
        if (named(pc)) opt = "the player's (kept)";
        else if (!np || !*np) opt = "none (game defaults)";
        else {
            static char args[2048];
            const char *a = getenv("KL_ALYX_ARGS");
            snprintf(args, sizeof args, "%s%s%s", a && *a ? a : "", a && *a ? " " : "", np);
            setenv("KL_ALYX_ARGS", args, 1);
            opt = "new player defaults";
        }
    }
    int so = sonames();
    snprintf(filesline, filessz,
             "  app files : env %s, convars %s, fidelity table %s, video settings %s (gpu ids %s), player options %s, "
             "soname placeholders %s%d%s\n",
             env_from, cv_from, fid, vid, gpu, opt, so < 0 ? "(no bin folder) " : "made ", so < 0 ? 0 : so,
             aside ? ", an old test's files set aside (.ignored)" : "");
}
