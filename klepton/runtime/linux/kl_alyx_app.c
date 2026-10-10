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
// Two files the game reads from its own folder are written there from the bundle only when missing, never over
// a pushed copy, one the game wrote itself or one a cloud sync brought:
//   hlvr/vr_fidelity_levels_override.csv   the fidelity table (libvr loads it before its own)
//   hlvr/cfg/deckard/video2.txt            the video settings (the game saves the player's changes here)
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

// The bundled <name> copied to the game folder's <rel> when nothing is there yet; returns what happened.
static const char *install(const char *name, const char *rel) {
    char src[1200], dst[1200], tmp[1300];
    snprintf(dst, sizeof dst, "%s/alyx/assets/%s", g_docs, rel);
    if (named(dst)) return "kept";
    snprintf(src, sizeof src, "%s/alyx-demo/%s", g_res, name);
    FILE *in = fopen(src, "rb");
    if (!in) return "not bundled";
    parents(dst);
    snprintf(tmp, sizeof tmp, "%s.klnew", dst);
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return "WRITE FAILED"; }
    char buf[65536];
    size_t n, bad = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) bad |= fwrite(buf, 1, n, out) != n;
    fclose(in);
    if (fclose(out) != 0 || bad || rename(tmp, dst) != 0) { unlink(tmp); return "WRITE FAILED"; }
    return "written";
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
    const char *fid = "no game folder", *vid = "no game folder", *opt = "no game folder";
    if (exists(hlvr)) {
        fid = install("vr_fidelity_levels_override.csv", "hlvr/vr_fidelity_levels_override.csv");
        vid = install("video2.txt", "hlvr/cfg/deckard/video2.txt");
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
             "  app files : env %s, convars %s, fidelity table %s, video settings %s, player options %s, "
             "soname placeholders %s%d\n",
             env_from, cv_from, fid, vid, opt, so < 0 ? "(no bin folder) " : "made ", so < 0 ? 0 : so);
}
