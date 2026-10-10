// Half-Life: Alyx's boot sequence, shared by the Mac driver (mains/m_alyx.c)
// and the visionOS app (visionos/Sources/kl_app.c): the import hooks that put
// the OpenVR shim and the native LuaJIT in front of the glibc personality, the
// guest C++ runtime, Valve's launcher argv, and the jump into the launcher's
// ELF entry point (whose __libc_start_main runs main -> Source2Main).
//
// Everything here works from translated dylibs (KL_DYLIB_DIR) as well as from
// the ELF tree: on the headset only the translations exist, so nothing below
// reads a guest library as a file.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../klepton.h"
#include "../kl_env.h"
#include "../kl_fault.h"
#include "kl_glibc.h"
#include "kl_alyx.h"

#ifdef KL_OPENVR_SHIM
int klvr_klepton_claims(const char *soname);
void *klvr_klepton_lookup(const char *name);
#else
static int klvr_klepton_claims(const char *soname) { (void)soname; return 0; }
static void *klvr_klepton_lookup(const char *name) { (void)name; return NULL; }
#endif

// The glibc personality, with the OpenVR shim in front of it: libopenvr_api.so
// is "served" (never mapped), and its imports resolve to the shim.
int kl_alyx_serves_soname(const char *s) {
    return klvr_klepton_claims(s) || kl_glibc_serves_soname(s);
}
// libvr.so also imports VRControlPanel (SteamVR's private IVRControlPanel
// accessor) and null-checks every call: the shim's, or NULL.
static void *alyx_no_control_panel(void) { return NULL; }
void *kl_alyx_lookup(const char *name) {
    void *p = name[0] == 'V' && name[1] == 'R' && name[2] == '_' ? klvr_klepton_lookup(name) : NULL;
    if (!p && !strcmp(name, "VRControlPanel")) {
        p = klvr_klepton_lookup(name);
        if (!p) p = (void *)alyx_no_control_panel;
    }
    return p ? p : kl_glibc_lookup(name);
}

void kl_alyx_install_hooks(void) {
    { extern void kl_fossilize_install(void); kl_fossilize_install(); }   // KL_VK_FOZ_RECORD / _REPLAY
    kl_soname_served_hook = kl_alyx_serves_soname;
    kl_shim_override = kl_alyx_lookup;
    { extern void *kl_lua_preload(const char *); kl_preload_hook = kl_lua_preload; }
    // A translated library answers stat/access only where its directory
    // exists (Source 2 finds its root by stat()ing up the tree).
    setenv("KL_DYLIB_STAT_NEEDS_DIR", "1", 0);
}

int kl_alyx_run(const char *game, const char *sysroot, int gargc, char **gargv) {
    static char bin[1024], hbin[1024], exe[1024];
    snprintf(bin, sizeof bin, "%s/bin/linuxsteamrtarm64", game);
    snprintf(hbin, sizeof hbin, "%s/hlvr/bin/linuxsteamrtarm64", game);
    snprintf(exe, sizeof exe, "%s/hlvr", bin);
    kl_set_library_path(bin);
    kl_add_library_path(sysroot);
    kl_add_library_path(hbin);
    kl_alyx_install_hooks();

    char p[1024];
    snprintf(p, sizeof p, "%s/libgcc_s.so.1", sysroot);
    kl_image *gcc = kl_load_recursive(p);
    snprintf(p, sizeof p, "%s/libstdc++.so.6", sysroot);
    kl_image *cxx = kl_load_recursive(p);
    if (!gcc || !cxx) { fprintf(stderr, "[alyx] C++ runtime: %s\n", kl_error()); return 1; }
    kl_glibc_set_cxx_images(cxx, gcc);

    static char *args[72];
    int n = 0;
    args[n++] = exe;
    // In VR, Alyx pauses after every map load (hlvr_pause_after_realmap,
    // hlvr_pause_after_loadsavegame) until the player acts, and while paused
    // its client sleeps ~107 ms a frame: 9 fps. Nothing unpauses it without an
    // input path, so turn the pause off; KL_ALYX_LOAD_PAUSE=1 keeps Valve's
    // behaviour. First, so the same convars on the command line still win.
    if (!kl_env_on("KL_ALYX_LOAD_PAUSE", 0)) {
        static char *nopause[] = { "+hlvr_pause_after_realmap", "0", "+hlvr_pause_after_loadsavegame", "0" };
        for (int i = 0; i < 4; i++) args[n++] = nopause[i];
    }
    // The one intermittent crash seen on the Mac (3 logs, 0 of 40 launches
    // when looked for) is a worker in CEntitySystem::BuildEntityNetworking's
    // parallel build: V_stricmp_fast on a pointer of 0x36a..0x742, under
    // libnetworksystem+0x20f944. It is a ~30 ms step run once per launch, so
    // it is built serially by default (the engine's own switch);
    // KL_ALYX_NET_PARALLEL=1 restores the parallel build.
    if (!kl_env_on("KL_ALYX_NET_PARALLEL", 0)) args[n++] = "-net_no_parallel_build";
    for (int i = 0; i < gargc && n < 71; i++) args[n++] = gargv[i];
    args[n] = NULL;
    kl_glibc_set_process(n, args, exe);
    if (chdir(game) != 0) perror("[alyx] chdir");

    kl_image *img = kl_load_recursive(exe);
    if (!img) { fprintf(stderr, "[alyx] launcher: %s\n", kl_error()); return 1; }
    // e_entry from the image's own ELF header, not the file: on the headset
    // only the translation exists (its __klelf starts with the header).
    uint64_t entry = *(const uint64_t *)(kl_base(img) + 0x18);
    fprintf(stderr, "[alyx] launcher mapped at %p, entry +%#llx\n", kl_base(img), (unsigned long long)entry);
    void (*start)(void *) = (void (*)(void *))(kl_base(img) + entry);
    start(NULL);                        // does not return: __libc_start_main exits
    return 0;
}
