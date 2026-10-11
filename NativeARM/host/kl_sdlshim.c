// SDL2 for a windowless engine.
//
// tier0's Plat_CreateWindow asks SDL for a Vulkan-capable window and the
// render system then asks SDL for the Vulkan surface. SDL's dummy video driver
// (the only one with no X11/Wayland here) has no Vulkan, so these entry points
// — which the engine resolves by dlsym on libSDL2 — are interposed: the window
// is created without the Vulkan flag, and the surface comes from Klepton's
// synthetic swapchain (vkCreateXcbSurfaceKHR in its Vulkan shim).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "klepton.h"
#include "kl_sdlshim.h"

#define SDL_WINDOW_OPENGL 0x00000002u
#define SDL_WINDOW_VULKAN 0x10000000u
#define SDL_WINDOW_METAL  0x20000000u

typedef void *(*create_window_fn)(const char *, int, int, int, int, uint32_t);
typedef void *(*gipa_fn)(void *, const char *);

static void *sdl_real(const char *name) {
    kl_image *sdl = kl_find_image("libSDL2-2.0.so.0");
    if (!sdl) sdl = kl_find_image("libSDL2.so");
    return sdl ? kl_sym(sdl, name) : NULL;
}

static void *shim_SDL_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags) {
    create_window_fn real = (create_window_fn)sdl_real("SDL_CreateWindow");
    if (!real) { fprintf(stderr, "  [sdl] SDL_CreateWindow: libSDL2 not loaded\n"); return NULL; }
    uint32_t f = flags & ~(SDL_WINDOW_VULKAN | SDL_WINDOW_OPENGL | SDL_WINDOW_METAL);
    void *win = real(title, x, y, w, h, f);
    fprintf(stderr, "  [sdl] SDL_CreateWindow(\"%s\", %dx%d, flags %#x -> %#x) -> %p\n", title ? title : "", w, h, flags, f, win);
    return win;
}
static int shim_SDL_Vulkan_LoadLibrary(const char *path) { (void)path; return 0; }
static void shim_SDL_Vulkan_UnloadLibrary(void) {}
static void *shim_SDL_Vulkan_GetVkGetInstanceProcAddr(void) {
    void *p = kl_shim_lookup("vkGetInstanceProcAddr");
    if (!p) fprintf(stderr, "  [sdl] no vkGetInstanceProcAddr to hand out\n");
    return p;
}
static int shim_SDL_Vulkan_GetInstanceExtensions(void *window, unsigned *count, const char **names) {
    (void)window;
    static const char *ext[] = { "VK_KHR_surface", "VK_KHR_xcb_surface" };
    if (!count) return 0;
    if (!names) { *count = 2; return 1; }
    unsigned n = *count < 2 ? *count : 2;
    for (unsigned i = 0; i < n; i++) names[i] = ext[i];
    *count = n;
    return n == 2;
}
static int shim_SDL_Vulkan_CreateSurface(void *window, void *instance, void **surface) {
    (void)window;
    gipa_fn gipa = (gipa_fn)kl_shim_lookup("vkGetInstanceProcAddr");
    typedef int (*create_fn)(void *, const void *, const void *, void **);
    create_fn create = gipa ? (create_fn)gipa(instance, "vkCreateXcbSurfaceKHR") : NULL;
    if (!create) { fprintf(stderr, "  [sdl] SDL_Vulkan_CreateSurface: no vkCreateXcbSurfaceKHR\n"); return 0; }
    struct { uint32_t sType; const void *pNext; uint32_t flags; void *connection; uint32_t window; } ci = { 1000005000, NULL, 0, NULL, 1 };
    int r = create(instance, &ci, NULL, surface);
    fprintf(stderr, "  [sdl] SDL_Vulkan_CreateSurface(instance %p) -> %d, surface %p\n", instance, r, surface ? *surface : NULL);
    return r == 0;
}
static void shim_SDL_Vulkan_GetDrawableSize(void *window, int *w, int *h) {
    (void)window;
    const char *ew = getenv("KL_VK_WIDTH"), *eh = getenv("KL_VK_HEIGHT");
    if (w) *w = ew ? atoi(ew) : 1280;
    if (h) *h = eh ? atoi(eh) : 720;
}
// SDL_SysWMinfo: version (3 bytes, padded), subsystem (int at 4), union info.
// Reported as X11 with placeholder handles; the surface never touches them.
static int shim_SDL_GetWindowWMInfo(void *window, uint8_t *info) {
    (void)window;
    if (!info) return 0;
    *(int32_t *)(info + 4) = 2;                       // SDL_SYSWM_X11
    *(void **)(info + 8) = (void *)0x1;               // Display *
    *(unsigned long *)(info + 16) = 1;                // Window
    return 1;
}

void kl_sdlshim_install(void) {
    kl_interpose("SDL_CreateWindow", (void *)shim_SDL_CreateWindow);
    kl_interpose("SDL_Vulkan_LoadLibrary", (void *)shim_SDL_Vulkan_LoadLibrary);
    kl_interpose("SDL_Vulkan_UnloadLibrary", (void *)shim_SDL_Vulkan_UnloadLibrary);
    kl_interpose("SDL_Vulkan_GetVkGetInstanceProcAddr", (void *)shim_SDL_Vulkan_GetVkGetInstanceProcAddr);
    kl_interpose("SDL_Vulkan_GetInstanceExtensions", (void *)shim_SDL_Vulkan_GetInstanceExtensions);
    kl_interpose("SDL_Vulkan_CreateSurface", (void *)shim_SDL_Vulkan_CreateSurface);
    kl_interpose("SDL_Vulkan_GetDrawableSize", (void *)shim_SDL_Vulkan_GetDrawableSize);
    kl_interpose("SDL_GetWindowWMInfo", (void *)shim_SDL_GetWindowWMInfo);
}
