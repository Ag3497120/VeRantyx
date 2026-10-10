// See kl_present.h for what this is and why the mode is observed rather than
// configured. The implementation is deliberately tiny: two facts in, one
// derived answer out, and a generation counter so a consumer can act on a
// transition exactly once.
#include <stdio.h>
#include "kl_present.h"
#include "kl_env.h"

static int      g_have_window;
static int      g_win_w, g_win_h;
static int      g_have_eyes;
static int      g_have_xr_layer;
static unsigned g_generation;
static kl_present_mode g_mode = KL_PRESENT_NONE;

// The whole policy, in one place. Stereo beats mono because it is the more
// specific fact: every Android guest creates a window surface, including Unity,
// so "has a window" cannot distinguish the two on its own — but an eye pair OR an
// XR composition layer can: both are the guest presenting through the compositor.
// (Xash's VR menus sit on a quad layer with no eye textures yet — the layer is
// what marks them immersive while in the menu.)
static kl_present_mode derive(void) {
    // KL_PRESENT_MONO=1 forces the 2D flat shell even for a guest that also set
    // up eyes / an XR layer. A Source-VR port (portal) renders its MENU in mono to
    // the guest window (captured by kl_glfb_present on eglSwapBuffers) while it
    // submits EMPTY OpenXR eye/projection layers — so the auto policy shows the
    // empty immersive view and the mono menu is never seen. This override surfaces
    // the guest's flat picture (the menu) so it can be read and navigated; drop it
    // once the guest actually renders stereo content (in a map).
    if (g_have_window && kl_env_on("KL_PRESENT_MONO", 0)) return KL_PRESENT_MONO;
    if (g_have_eyes || g_have_xr_layer) return KL_PRESENT_STEREO;
    if (g_have_window) return KL_PRESENT_MONO;
    return KL_PRESENT_NONE;
}

static void settle(void) {
    kl_present_mode m = derive();
    if (m == g_mode) return;
    static const char *const NAME[] = { "none", "mono (window)", "stereo (immersive)" };
    fprintf(stderr, "  [present] %s -> %s (generation %u)\n",
            NAME[g_mode], NAME[m], g_generation + 1);
    g_mode = m;
    g_generation++;
}

kl_present_mode kl_present_mode_now(void) { return g_mode; }
unsigned        kl_present_generation(void) { return g_generation; }

void kl_present_mono_size(int *w, int *h) {
    if (w) *w = g_win_w;
    if (h) *h = g_win_h;
}

void kl_present_note_window_surface(int w, int h) {
    if (w > 0 && h > 0) { g_win_w = w; g_win_h = h; }
    g_have_window = 1;
    settle();
}

void kl_present_note_eye_texture(void) {
    g_have_eyes = 1;
    settle();
}

void kl_present_note_xr_layer(void) {
    g_have_xr_layer = 1;
    settle();
}

void kl_present_clear_eye_textures(void) {
    g_have_eyes = 0;
    settle();
}
