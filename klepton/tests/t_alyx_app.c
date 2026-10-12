// The Alyx app's settings files, installed once per bundled version (runtime/linux/kl_alyx_app.c). `make alyx-app`,
// in `make check`.
//
// The case this gate exists for: every headset installed before the stamps ran the game's own first-boot video
// settings (its automatic setup at the Steam Frame's low values: trilinear filtering, fog 48), because the bundled
// video2.txt was written only when missing and the game had written its own first. The update must put ours in
// once, keep the old file, and then leave the player's later changes alone. And the game keeps video2.txt only while
// its VendorID/DeviceID equal the running GPU's, so every start stamps the GPU's own ids into it (a fresh install,
// an OS update that changes MoltenVK's device id). On the headset a test's pushed files count only with the line
// GLOVEBOX_TEST=1: one left behind made a headset skip the launcher and play the test's settings.
//
// Plain files in a temporary folder laid out like the app's (bundle alyx-demo/ + Documents/alyx/assets/hlvr).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../runtime/linux/kl_alyx_app.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char root[512], res[600], docs[600], vid[800], csv[800];

// The GPU as kl_vulkan.c would report it (this test links no MoltenVK): replaces kl_alyx_app.c's weak default.
static unsigned g_vendor = 4203, g_device = 452984842;
static int g_have_gpu = 1;
int kl_vulkan_gpu_ids(unsigned *vendor, unsigned *device) {
    if (!g_have_gpu) return 0;
    *vendor = g_vendor;
    *device = g_device;
    return 1;
}

static void put(const char *p, const char *s) {
    char d[900];
    snprintf(d, sizeof d, "%s", p);
    for (char *c = d + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(d, 0755); *c = '/'; }
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); exit(2); }
    fputs(s, f);
    fclose(f);
}

static char *get(const char *p) {
    static char b[8][8192];
    static int i;
    char *o = b[i++ % 8];
    o[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    size_t n = fread(o, 1, 8191, f);
    o[n] = 0;
    fclose(f);
    return o;
}

static char *with(const char *p, const char *suffix) {
    static char b[4][900];
    static int i;
    char *o = b[i++ % 4];
    snprintf(o, 900, "%s%s", p, suffix);
    return o;
}

// One app start; returns the boot log's files line.
static const char *boot(void) {
    static char env[2048], files[512];
    kl_alyx_app_files(res, docs, env, sizeof env, files, sizeof files);
    return files;
}

#define OURS(filt, dev) \
    "\"config\"\n{\n\t\"VendorID\"\t\t\"4203\"\n\t\"DeviceID\"\t\t\"" dev "\"\n" \
    "\t\"setting.r_texturefilteringquality\"\t\t\"" filt "\"\n}\n"

int main(void) {
    snprintf(root, sizeof root, "/tmp/t_alyx_app.%d", (int)getpid());
    snprintf(res, sizeof res, "%s/bundle", root);
    snprintf(docs, sizeof docs, "%s/docs", root);
    snprintf(vid, sizeof vid, "%s/alyx/assets/hlvr/cfg/deckard/video2.txt", docs);
    snprintf(csv, sizeof csv, "%s/alyx/assets/hlvr/vr_fidelity_levels_override.csv", docs);
    char p[900];
    snprintf(p, sizeof p, "%s/alyx-demo/video2.txt", res);
    put(p, OURS("4", "452985354"));
    snprintf(p, sizeof p, "%s/alyx-demo/vr_fidelity_levels_override.csv", res);
    put(p, "level,scale\n0,1.0\n");
    snprintf(p, sizeof p, "%s/alyx-demo/alyx-env.txt", res);
    put(p, "KL_T_BUNDLED=1\n");
    snprintf(p, sizeof p, "%s/alyx-demo/alyx-convars.lua", res);
    put(p, "-- bundled convars\n");

    // 1. An install from before the stamps: the game's own low file, written for this headset's GPU.
    put(vid, OURS("1", "452984842"));
    put(csv, "level,scale\n0,0.8\n");
    const char *l = boot();
    CHECK(strstr(l, "fidelity table replaced (old one saved)") && strstr(l, "video settings replaced (old one saved)")
          && strstr(l, "(gpu ids match 4203/452984842)"), "first boot after the update: %s", l);
    CHECK(get(vid) && strstr(get(vid), "r_texturefilteringquality\"\t\t\"4\""), "ours is in: %s", get(vid));
    CHECK(strstr(get(vid), "\"DeviceID\"\t\t\"452984842\""), "the old file's GPU id is kept: %s", get(vid));
    CHECK(get(with(vid, ".before-update")) && !strcmp(get(with(vid, ".before-update")), OURS("1", "452984842")),
          "the old file is saved beside it");
    CHECK(!strcmp(get(csv), "level,scale\n0,1.0\n"), "fidelity table updated: %s", get(csv));

    // 2. Next boot: kept. Then the player changes Options (the game rewrites the file): still kept.
    l = boot();
    CHECK(strstr(l, "fidelity table kept") && strstr(l, "video settings kept"), "second boot: %s", l);
    put(vid, OURS("5", "452984842"));
    l = boot();
    CHECK(strstr(l, "video settings kept") && strstr(get(vid), "quality\"\t\t\"5\""), "player's change kept: %s", l);

    // 3. A later update changes the bundled settings: ours again, once, with the player's file saved.
    snprintf(p, sizeof p, "%s/alyx-demo/video2.txt", res);
    put(p, OURS("3", "452985354"));
    l = boot();
    CHECK(strstr(l, "video settings updated (old one saved)"), "bundle changed: %s", l);
    CHECK(strstr(get(vid), "quality\"\t\t\"3\"") && strstr(get(vid), "452984842"), "new bundle in, id kept: %s", get(vid));
    CHECK(strstr(get(with(vid, ".before-update")), "quality\"\t\t\"5\""), "player's file saved");
    l = boot();
    CHECK(strstr(l, "video settings kept"), "then kept: %s", l);

    // 4. A fresh install (no file): ours, with this GPU's ids rather than the bundle's (a Mac's).
    unlink(vid);
    unlink(with(vid, ".glovebox"));
    l = boot();
    CHECK(strstr(l, "video settings written (gpu ids set to 4203/452984842)") && !strcmp(get(vid), OURS("3", "452984842")),
          "fresh: %s / %s", l, get(vid));

    // 4b. An OS update changes MoltenVK's device id: the ids follow, the settings (and the player's) stay.
    put(vid, OURS("5", "452984842"));
    g_device = 469762058;
    l = boot();
    CHECK(strstr(l, "video settings kept (gpu ids set to 4203/469762058)") && !strcmp(get(vid), OURS("5", "469762058")),
          "os update: %s / %s", l, get(vid));

    // 4c. No GPU answer (no MoltenVK): the file is left as it is.
    g_have_gpu = 0;
    g_device = 1;
    l = boot();
    CHECK(strstr(l, "(gpu ids gpu unknown)") && !strcmp(get(vid), OURS("5", "469762058")), "no gpu: %s", l);
    g_have_gpu = 1;

    // 5. A test script's pushed copy, stamped "keep": never replaced.
    put(vid, OURS("0", "1"));
    put(with(vid, ".glovebox"), "keep\n");
    l = boot();
    CHECK(strstr(l, "video settings kept (pushed)") && !strcmp(get(vid), OURS("0", "1")), "pushed: %s", l);

    // 5b. The headset, with a test's files left behind after the test (no GLOVEBOX_TEST=1 line; a look-alike doesn't
    // count): both set aside as .ignored, the bundle's apply, and the pushed video settings give way to ours, once.
    kl_alyx_app_test_marker_required(1);
    char denv[700], dcv[700], cmd[3200];
    snprintf(denv, sizeof denv, "%s/alyx-env.txt", docs);
    snprintf(dcv, sizeof dcv, "%s/alyx-convars.lua", docs);
    put(denv, "KL_T_LEFT_BEHIND=1\nGLOVEBOX_TEST=10\n");
    put(dcv, "-- a test's convars\n");
    l = boot();
    CHECK(strstr(l, "env bundle") && strstr(l, "convars bundle")
          && strstr(l, "video settings replaced a test copy (old one saved)") && strstr(l, "set aside (.ignored)"),
          "left behind: %s", l);
    CHECK(!get(denv) && !get(dcv) && get(with(denv, ".ignored")) && !strcmp(get(with(denv, ".ignored")), "KL_T_LEFT_BEHIND=1\nGLOVEBOX_TEST=10\n")
          && get(with(dcv, ".ignored")), "set aside, kept beside: %s", get(with(denv, ".ignored")));
    CHECK(strstr(get(vid), "quality\"\t\t\"3\"") && strstr(get(with(vid, ".before-update")), "quality\"\t\t\"0\""),
          "ours in, the test's copy saved: %s", get(vid));
    l = boot();
    CHECK(strstr(l, "env bundle") && strstr(l, "video settings kept") && !strstr(l, "set aside"), "then as usual: %s", l);

    // 5c. The headset, a test run (the line GLOVEBOX_TEST=1, blanks around it allowed, pushed for this install): its
    // files count, as on the Mac, and keep counting at the next start, after the launcher's backup mark (a change of
    // the file's metadata, not a new push).
    put(denv, "KL_T_RUN=1\n  GLOVEBOX_TEST=1 \r\n");
    put(vid, OURS("0", "1"));
    put(with(vid, ".glovebox"), "keep\n");
    CHECK(kl_alyx_app_test_run(res, docs) == 1, "the launcher's gate: a test run");
    l = boot();
    CHECK(strstr(l, "env Documents") && strstr(l, "video settings kept (pushed)") && !strstr(l, "set aside")
          && get(denv) && !strcmp(get(vid), OURS("0", "1")), "test run: %s", l);
    chmod(denv, 0600);
    l = boot();
    CHECK(strstr(l, "env Documents") && strstr(l, "video settings kept (pushed)"), "test run, next start: %s", l);

    // 5d. A new install (the app folder made new) without a new push: the test is over. Launcher, files set aside,
    // ours in for the pushed video settings.
    snprintf(cmd, sizeof cmd, "mv '%s' '%s.old' && cp -r '%s.old' '%s' && rm -rf '%s.old'", res, res, res, res, res);
    if (system(cmd) != 0) return 2;
    CHECK(kl_alyx_app_test_run(res, docs) == 0, "the launcher's gate after a new install: no test run");
    l = boot();
    CHECK(strstr(l, "env bundle") && strstr(l, "video settings replaced a test copy (old one saved)")
          && strstr(l, "set aside (.ignored)") && !get(denv) && !get(with(denv, ".install")), "new install: %s", l);

    // 5e. A test on the new install pushes its files again: a test run again.
    put(denv, "GLOVEBOX_TEST=1\n");
    CHECK(kl_alyx_app_test_run(res, docs) == 1, "pushed again: a test run");
    kl_alyx_app_test_marker_required(0);

    // 6. No game folder yet (still downloading): nothing written.
    snprintf(cmd, sizeof cmd, "rm -rf '%s/alyx'", docs);
    if (system(cmd) != 0) return 2;
    l = boot();
    CHECK(strstr(l, "video settings no game folder"), "no game folder: %s", l);

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0) return 2;
    printf("%s\n", fails ? "alyx app files: FAILED" : "alyx app files: 13 cases ok");
    return fails != 0;
}
