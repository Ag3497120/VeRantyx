// libvulkan.so over MoltenVK — see kl_vulkan.h for what this is and why.
//
// The real Vulkan headers are used rather than transcribed, which is the
// opposite of kl_openxr's choice next door, and the reason is that the headers
// are already a build dependency here: MoltenVK ships them in the same tarball
// as the dylib, and there is no Vulkan path at all without that dylib. So
// transcribing would add a whole class of silent layout bug (a 182-entry-point
// API, structs up to ~100 fields) to buy exactly nothing.
//
// The __has_include guard keeps `make check` honest on a bare checkout: with no
// MoltenVK vendored this file still compiles, claims nothing, and says so BY
// NAME the moment a guest asks for Vulkan. That is the same shape as ANGLE not
// being built — a missing optional dependency should read as a missing
// dependency, not as a guest that mysteriously fails to render.
#include <dlfcn.h>
#include <pthread.h>
#include <CommonCrypto/CommonDigest.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <TargetConditionals.h>

#include "kl_vulkan.h"
#include "kl_env.h"
#include "kl_driver.h"   // kl_driver_target_name() — per-target GPU identity spoof

// Matched on with a prefix test rather than equality: the guest asks by plain
// soname, but a path can arrive too (kl_dl.c hands through whatever the guest
// wrote, and BONELAB's plugin loader builds absolute paths).
static int name_is_vulkan(const char *p) {
    if (!p) return 0;
    const char *slash = strrchr(p, '/');
    const char *base = slash ? slash + 1 : p;
    return !strcmp(base, "libvulkan.so") || !strcmp(base, "libvulkan.so.1") ||
           !strcmp(base, "vulkan");
}

#if !__has_include(<vulkan/vulkan.h>)

// ---------------------------------------------------------------------------
// No MoltenVK vendored. Refuse by name, once, and let the guest take whatever
// path it takes when Vulkan is absent — which for BONELAB is the hardware
// requirements warning, i.e. exactly the pre-existing behaviour.
// ---------------------------------------------------------------------------
static void complain_once(void) {
    static int said;
    if (said++) return;
    fprintf(stderr,
            "  [vk] libvulkan.so requested, but MoltenVK is not vendored.\n"
            "  [vk] Run 'make mvk' (see BUILDING.md); this build has no Vulkan.\n");
}
int   kl_vulkan_claims(const char *n) { if (name_is_vulkan(n)) complain_once(); return 0; }
void *kl_vulkan_dlopen(const char *n) { if (name_is_vulkan(n)) complain_once(); return NULL; }
int   kl_vulkan_is_handle(const void *h) { (void)h; return 0; }
void *kl_vulkan_sym(const char *n) { (void)n; return NULL; }
void *kl_vulkan_lookup(const char *n) { (void)n; return NULL; }
int   kl_vulkan_available(void) { return 0; }
void  kl_vulkan_stats(unsigned *p, unsigned *c) { if (p) *p = 0; if (c) *c = 0; }
int   kl_vulkan_guest_active(void) { return 0; }
unsigned long long kl_vulkan_eye_image(int s, int e, unsigned w, unsigned h, int srgb) {
    (void)s; (void)e; (void)w; (void)h; (void)srgb; return 0;
}
// Was missing, and it is what kl_ovrp.c actually calls — so a checkout that has
// never run `make mvk` did not fail to *compile*, it failed to LINK, which is
// the one failure this whole `#if` exists to prevent. Every entry point in the
// header belongs here.
unsigned long long kl_vulkan_eye_image_layers(int s, int e, unsigned w, unsigned h,
                                             int srgb, int layers) {
    (void)s; (void)e; (void)w; (void)h; (void)srgb; (void)layers; return 0;
}
unsigned long long kl_vulkan_layer_image(int k, int s, int e, unsigned w, unsigned h,
                                         int srgb, int layers) {
    (void)k; (void)s; (void)e; (void)w; (void)h; (void)srgb; (void)layers; return 0;
}
void *kl_vulkan_layer_mtl_texture(int k, int s, int e, int *w, int *h) {
    (void)k; (void)s; (void)e; if (w) *w = 0; if (h) *h = 0; return NULL;
}
unsigned long long kl_vulkan_eye_depth_image(int s, int e, unsigned w, unsigned h) { (void)s; (void)e; (void)w; (void)h; return 0; }
unsigned long long kl_vulkan_eye_depth_half(int e, unsigned w, unsigned h) { (void)e; (void)w; (void)h; return 0; }
unsigned long long kl_vulkan_eye_depth_staging(int s, int e, unsigned long long b, void **m) { (void)s; (void)e; (void)b; if (m) *m = NULL; return 0; }
void kl_vulkan_set_eye_depth_map(int s, int e, float a, float b, unsigned w, unsigned h, unsigned p) { (void)s; (void)e; (void)a; (void)b; (void)w; (void)h; (void)p; }
void *kl_vulkan_eye_depth_mtl(int s, int e, int *w, int *h, float *a, float *b) { (void)s; (void)e; (void)w; (void)h; (void)a; (void)b; return NULL; }
void  kl_vulkan_layer_rekey(int old_key, int new_key) { (void)old_key; (void)new_key; }
void  kl_vulkan_capture_eyes(unsigned f, int s) { (void)f; (void)s; }
void  kl_vulkan_frame_done(int s) { (void)s; }
unsigned long long kl_vulkan_frame_serial(void) { return 0; }
// The OpenXR seam. Answering "not supported" here is what keeps
// XR_KHR_vulkan_enable OUT of the advertised extension list on a checkout with
// no MoltenVK, so a Vulkan OpenXR guest is refused at xrCreateInstance — where
// the guest's own log names the extension — instead of somewhere further in.
int   kl_vulkan_xr_supported(void) { return 0; }
const char *kl_vulkan_xr_instance_extensions(void) { return ""; }
const char *kl_vulkan_xr_device_extensions(void) { return ""; }
void *kl_vulkan_xr_physical_device(void *vi) { (void)vi; return NULL; }
void  kl_vulkan_xr_api_range(unsigned *mmaj, unsigned *mmin,
                             unsigned *xmaj, unsigned *xmin) {
    if (mmaj) *mmaj = 0; if (mmin) *mmin = 0;
    if (xmaj) *xmaj = 0; if (xmin) *xmin = 0;
}
unsigned long long kl_vulkan_xr_image(unsigned w, unsigned h, unsigned l,
                                      unsigned m, long long f, int depth) {
    (void)w; (void)h; (void)l; (void)m; (void)f; (void)depth; return 0;
}
void *kl_vulkan_xr_image_mtl(unsigned long long img) { (void)img; return NULL; }

#else  /* MoltenVK headers present */

#define VK_NO_PROTOTYPES
// The Android WSI half of the header, which is what `vkCreateAndroidSurfaceKHR`
// and its create-info live in. Safe to switch on with no Android SDK anywhere:
// vulkan_android.h forward-declares `struct ANativeWindow` and `struct
// AHardwareBuffer` itself and includes nothing. Transcribing the struct instead
// would have been four fields of avoidable risk.
#define VK_USE_PLATFORM_ANDROID_KHR 1
// ...and the Metal half, for `VK_EXT_metal_objects` — how the MTLTexture behind
// an eye VkImage is reached and handed to the compositor. Equally safe in plain
// C: vulkan_metal.h typedefs every Metal handle to `void *` unless __OBJC__ is
// defined, which it is not here.
#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan.h>
#include "kl_vk_pipe_hooks.h"   // the pipeline recorder/replayer add-on (kl_fossilize.cpp)
#include "kl_gpu_time.h"         // per-frame GPU time: each guest queue's MTLCommandQueue is handed over once
#include "kl_glfb.h"

#include <zlib.h>

#define KLVK_MAX_IMAGES 8

static int vk_trace(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_TRACE", 0);
    return on;
}
#define VKT(...) do { if (vk_trace()) fprintf(stderr, "  [vk] " __VA_ARGS__); } while (0)
#define VKI(...) fprintf(stderr, "  [vk] " __VA_ARGS__)

// ---------------------------------------------------------------------------
// Loading MoltenVK
// ---------------------------------------------------------------------------
//
// Two shapes, for the same reason ANGLE has two (kl_glfb.c's angle_dlopen): a
// bare dylib is what the host stages, and an app may only load code from inside
// its own bundle on visionOS, where it is a .framework. Try both rather than
// making the caller know which.
#define MVK_VENDORED_DIR "vendor-moltenvk/out/macos"

static void *mvk_try(const char *path) {
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) VKT("dlopen(%s): %s\n", path, dlerror());
    return h;
}

static void *mvk_open(void) {
    const char *explicit = kl_env_str("KL_MVK_DYLIB", NULL);
    if (explicit) return mvk_try(explicit);

    const char *dir = kl_env_str("KL_MVK_DIR", MVK_VENDORED_DIR);
    char path[1024];
    snprintf(path, sizeof path, "%s/libMoltenVK.dylib", dir);
    void *h = mvk_try(path);
    if (h) return h;
    snprintf(path, sizeof path, "%s/MoltenVK.framework/MoltenVK", dir);
    if ((h = mvk_try(path))) return h;
    // The bundle case: @rpath resolves to <exec dir>/Frameworks, which is where
    // an embedded framework lands and the ONLY place AMFI will accept code from.
    if ((h = mvk_try("@rpath/MoltenVK.framework/MoltenVK"))) return h;
    return mvk_try("MoltenVK.framework/MoltenVK");
}

static void *g_mvk;
static PFN_vkGetInstanceProcAddr real_gipa;

// Pipeline-object hooks (kl_vk_pipe_hooks.h): an add-on records every sampler, layout, module,
// render pass and pipeline the guest creates, and replays recorded ones at device creation.
static const klvk_pipe_hooks *g_pipe_hooks;
static __thread int t_pipe_replay;   // this thread belongs to the replayer
void klvk_set_pipe_hooks(const klvk_pipe_hooks *h) { g_pipe_hooks = h; }
void klvk_set_replay_thread(int on) { t_pipe_replay = on; }
#define PIPE_HOOK(fn, ...) do { if (g_pipe_hooks && g_pipe_hooks->fn && !t_pipe_replay) g_pipe_hooks->fn(__VA_ARGS__); } while (0)
static PFN_vkVoidFunction VKAPI_CALL klvk_GetDeviceProcAddr(VkDevice dev, const char *name);
static PFN_vkGetDeviceProcAddr   real_gdpa;
static int g_init_done, g_avail;

static void *mvk_sym(const char *n) { return g_mvk ? dlsym(g_mvk, n) : NULL; }

static void vk_init(void) {
    if (g_init_done) return;
    g_init_done = 1;

    // MoltenVK logs its whole device census at info level, which buries
    // everything this port prints. Same choice as tests/t_mvk.c, same escape.
    if (!kl_env_on("KL_MVK_VERBOSE", 0)) setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);

    // UE Vulkan titles (wanderer, wrath2) pay a long boot PSO-precompile wall:
    // every pipeline goes through MoltenVK's SPIRV->MSL compiler (~300 ms each,
    // hundreds of them, SERIALIZED) before the guest submits its first XR layer —
    // the headset shows black until warmup completes, and with no on-disk cache it
    // recompiles from scratch every boot. Let MoltenVK spread the compiles across
    // its concurrent-compilation pool (MVKConfiguration.shouldMaximizeConcurrent
    // Compilation). Render correctness is unchanged; global + env-overridable.
    if (kl_env_on("KL_MVK_CONCURRENT_COMPILE", 1))
        setenv("MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION", "1", 0);

    // wrath2: exactly one UE4 warmup pipeline (~#5251, a tessellation PSO) wedges
    // MoltenVK's Metal compiler forever — MVKMetalCompiler::compile blocks in
    // std::condition_variable::wait_until on a newRenderPipelineState completion
    // handler that never fires, so RenderThread never returns from vkCreateGraphics
    // Pipelines and the whole UE4 TaskGraph freezes (frozen conds, compositor
    // layers=0). metalCompileTimeout defaults to infinite; a finite bound makes the
    // doomed compile FAIL (NULL handle) instead of hanging, so boot proceeds with at
    // most one PSO missing rather than a dead loading screen. Nanoseconds; wrath2-
    // gated (env-overridable for other titles).
    {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        int def = (t && strcmp(t, "wrath2") == 0) ? 1 : 0;
        if (kl_env_on("KL_MVK_COMPILE_TIMEOUT", def))
            setenv("MVK_CONFIG_METAL_COMPILE_TIMEOUT", "10000000000", 0); // 10 s
    }

    // wanderer black world: a MoltenVK/SPIRV-Cross argument-buffer codegen bug. A
    // fragment material referencing TWO Material Parameter Collections (two dynamic
    // uniform buffers) makes SPIRV-Cross emit `spvDescriptorSetBuffer0` with only
    // MaterialCollection0 declared, then a dynamic-offset accessor for a sibling
    // MaterialCollection1 that is never in the struct — the MSL fails to compile
    // (VK_ERROR_INITIALIZATION_FAILED, "no member named 'MaterialCollection1'"),
    // the PSO is dropped, that master environment material never draws, and the
    // world stays black (the eye resolve itself is fine — the armed magenta probe
    // came back BLACK, not magenta). Forcing MoltenVK OFF the argument-buffer path
    // makes SPIRV-Cross emit discrete `buffer(N)` bindings, dodging the bug. The
    // texel-buffer emu already moves manual-fetch buffers to textures, so discrete
    // binding pressure should still fit Metal's per-stage limit. wanderer-gated,
    // env-overridable (KL_MVK_NO_ARGBUF).
    {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        int def = (t && strcmp(t, "wanderer") == 0) ? 1 : 0;
        if (kl_env_on("KL_MVK_NO_ARGBUF", def))
            setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "0", 0);  // 0 = NEVER
    }

    // (The former MVK_CONFIG_USE_MTLHEAP=2 forcing lived here. It existed only to
    // make the dead fork-B alias land at MTLBuffer offset 0 via MTLHeap placement.
    // The alias is gone, replaced by the 2D-image mirror below, which is entirely
    // independent of MTLHeap mode: it reads the guest pool's mapped CPU bytes and
    // writes the mirror image's mapped CPU bytes, both host-side, so nothing here
    // needs to perturb MoltenVK's memory backing. Left on the driver default so
    // the mirror is exercised against stock MoltenVK; a launcher may still set
    // MVK_CONFIG_USE_MTLHEAP explicitly if wanted.)

    g_mvk = mvk_open();
    if (!g_mvk) {
        VKI("MoltenVK could not be loaded — run 'make mvk' (BUILDING.md).\n");
        return;
    }
    real_gipa = (PFN_vkGetInstanceProcAddr)mvk_sym("vkGetInstanceProcAddr");
    real_gdpa = (PFN_vkGetDeviceProcAddr)mvk_sym("vkGetDeviceProcAddr");
    if (!real_gipa) { VKI("MoltenVK has no vkGetInstanceProcAddr\n"); return; }
    g_avail = 1;

    PFN_vkEnumerateInstanceVersion eiv =
        (PFN_vkEnumerateInstanceVersion)real_gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t ver = VK_API_VERSION_1_0;
    if (eiv) eiv(&ver);
    VKI("MoltenVK loaded — instance API %u.%u.%u\n", VK_VERSION_MAJOR(ver),
        VK_VERSION_MINOR(ver), VK_VERSION_PATCH(ver));
}

int kl_vulkan_available(void) { vk_init(); return g_avail; }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t magic;
    uint32_t w, h;
} klvk_surface;
#define KLVK_SURF_MAGIC 0x4b565355u   /* 'KVSU' */

// Everything a capture needs, resolved once per device. Held rather than
// re-resolved because vkQueuePresentKHR is on the frame path.
typedef struct {
    VkDevice          dev;
    VkPhysicalDevice  phys;
    uint32_t          queue_family;
    VkQueue           queue;          // for the acquire signal + the capture submit
    PFN_vkGetDeviceQueue              GetDeviceQueue;
    PFN_vkCreateImage                 CreateImage;
    PFN_vkDestroyImage                DestroyImage;
    PFN_vkGetImageMemoryRequirements  GetImageMemoryRequirements;
    PFN_vkAllocateMemory              AllocateMemory;
    PFN_vkFreeMemory                  FreeMemory;
    PFN_vkBindImageMemory             BindImageMemory;
    PFN_vkCreateBuffer                CreateBuffer;
    PFN_vkDestroyBuffer               DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkBindBufferMemory            BindBufferMemory;
    PFN_vkMapMemory                   MapMemory;
    PFN_vkUnmapMemory                 UnmapMemory;
    PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges;
    PFN_vkCreateCommandPool           CreateCommandPool;
    PFN_vkDestroyCommandPool          DestroyCommandPool;
    PFN_vkAllocateCommandBuffers      AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer          BeginCommandBuffer;
    PFN_vkEndCommandBuffer            EndCommandBuffer;
    PFN_vkResetCommandBuffer          ResetCommandBuffer;
    PFN_vkCmdPipelineBarrier          CmdPipelineBarrier;
    PFN_vkCmdCopyImageToBuffer        CmdCopyImageToBuffer;
    PFN_vkCmdClearColorImage          CmdClearColorImage;
    PFN_vkQueueSubmit                 QueueSubmit;
    PFN_vkCreateFence                 CreateFence;
    PFN_vkDestroyFence                DestroyFence;
    PFN_vkWaitForFences               WaitForFences;
    PFN_vkResetFences                 ResetFences;
    PFN_vkDeviceWaitIdle              DeviceWaitIdle;
    PFN_vkQueueWaitIdle               QueueWaitIdle;
    // Readback scratch, created on first use and shared by every capture path
    // (swapchain present and the OVRPlugin eye layer). Sized to the largest
    // image captured so far and grown as needed.
    VkBuffer        cap_buf;
    VkDeviceMemory  cap_mem;
    VkDeviceSize    cap_size;
    VkCommandPool   cap_pool;
    VkCommandBuffer cap_cmd;
    VkFence         cap_fence;
} klvk_device;

typedef struct {
    uint32_t       magic;
    klvk_device   *d;
    VkFormat       format;
    VkExtent2D     extent;
    uint32_t       n;
    VkImage        images[KLVK_MAX_IMAGES];
    VkDeviceMemory mem[KLVK_MAX_IMAGES];
    uint32_t       next;
} klvk_swapchain;
#define KLVK_SWAP_MAGIC 0x4b565343u   /* 'KVSC' */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static klvk_device *g_devs[8];
static int          g_ndev;
static VkInstance   g_instance;
static unsigned     g_presented, g_captured;
static void cache_display_luid(VkInstance inst); // GPU LUID snapshot for ovrp_GetDisplayAdapterId2
static VkPhysicalDevice klvk_sub_phys(const char *where); // live device for a NULL-handle guest
// Substitute the live physical device wherever the guest passes NULL; see live_phys0.
#define KLVK_PHYS_OR_SUB(pd, where) ((pd) ? (pd) : klvk_sub_phys(where))

void kl_vulkan_stats(unsigned *p, unsigned *c) {
    if (p) *p = g_presented;
    if (c) *c = g_captured;
}

static klvk_device *dev_find(VkDevice d) {
    for (int i = 0; i < g_ndev; i++) if (g_devs[i]->dev == d) return g_devs[i];
    return NULL;
}
static klvk_device *dev_of_queue(VkQueue q) {
    // Queues are opaque; the guest may use one we never fetched. With a single
    // device — which is every case here — the answer is unambiguous.
    (void)q;
    return g_ndev ? g_devs[0] : NULL;
}

// kl_foz_prewarm's device and instance, once its replay is done (runtime/linux/kl_fossilize.cpp): out of the layer's
// books and destroyed. The game's own device has to be g_devs[0], the one eye images, the compositor and queue lookups
// use, and nothing of the prewarm may be left for a handle of the game's to collide with.
void klvk_retire_device(VkInstance inst, VkDevice dev) {
    klvk_device *d = NULL;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_ndev; i++)
        if (g_devs[i]->dev == dev) {
            d = g_devs[i];
            memmove(&g_devs[i], &g_devs[i + 1], (size_t)(g_ndev - i - 1) * sizeof *g_devs);
            g_ndev--;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    PFN_vkDestroyDevice dd = (PFN_vkDestroyDevice)real_gipa(inst, "vkDestroyDevice");
    if (dd && dev) dd(dev, NULL);
    free(d);
    PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)real_gipa(inst, "vkDestroyInstance");
    if (di && inst) di(inst, NULL);
    if (g_instance == inst) g_instance = VK_NULL_HANDLE;
    VKI("prewarm device and instance retired; %d device(s) left\n", g_ndev);
}

// ---------------------------------------------------------------------------
// PNG (top-down; Vulkan images already are). kl_glfb.c has its own writer and
// it is NOT shared, deliberately: that one flips GL's bottom-up rows and is
// entangled with the exposure/gamma tonemap the GL capture applies. Sharing
// would mean either a flag that is wrong half the time or surgery on a
// 5000-line file on the critical rendering path.
// ---------------------------------------------------------------------------
static void be32w(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void png_chunk(FILE *f, const char *tag, const uint8_t *d, uint32_t n) {
    uint8_t h[4];
    be32w(h, n); fwrite(h, 1, 4, f); fwrite(tag, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uLong c = crc32(0, (const Bytef *)tag, 4);
    if (n) c = crc32(c, (const Bytef *)d, n);
    be32w(h, (uint32_t)c); fwrite(h, 1, 4, f);
}

static int png_write_rgba(const char *path, const uint8_t *px, int w, int h) {
    size_t stride = (size_t)w * 4, raw_n = (stride + 1) * (size_t)h;
    uint8_t *raw = malloc(raw_n);
    uLongf cn = compressBound((uLong)raw_n);
    uint8_t *cb = malloc(cn);
    if (!raw || !cb) { free(raw); free(cb); return 0; }
    for (int y = 0; y < h; y++) {
        raw[(stride + 1) * (size_t)y] = 0;
        memcpy(raw + (stride + 1) * (size_t)y + 1, px + stride * (size_t)y, stride);
    }
    int ok = compress(cb, &cn, raw, (uLong)raw_n) == Z_OK;
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        static const uint8_t sig[8] = {0x89,'P','N','G',13,10,26,10};
        fwrite(sig, 1, 8, f);
        uint8_t ih[13];
        be32w(ih, (uint32_t)w); be32w(ih + 4, (uint32_t)h);
        ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
        png_chunk(f, "IHDR", ih, 13);
        png_chunk(f, "IDAT", cb, (uint32_t)cn);
        png_chunk(f, "IEND", NULL, 0);
        fclose(f);
    } else ok = 0;
    free(raw); free(cb);
    return ok;
}

// Is this format's byte order BGRA rather than RGBA? The swapchain format is
// whatever the guest picked from what we advertised, and a channel swap is
// invisible in a grey test frame and glaring in a real one.
static int fmt_is_bgra(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB;
}

// ---------------------------------------------------------------------------
// Extension list surgery
// ---------------------------------------------------------------------------
//
// Every requested extension is checked against what MoltenVK actually
// advertises and silently-unsupported ones are DROPPED BY NAME rather than
// passed through. Passing them through fails the whole create call with
// VK_ERROR_EXTENSION_NOT_PRESENT, which tells the guest "no Vulkan" and tells
// us nothing about which of a dozen names was the problem.
//
// This is the same judgement the rest of the port makes about group answers:
// answering call-by-call would let the guest see a set that disagrees with
// itself.
typedef struct { const char *names[64]; uint32_t n; } extlist;

static void ext_add(extlist *l, const char *n) {
    if (l->n >= 64) return;
    for (uint32_t i = 0; i < l->n; i++) if (!strcmp(l->names[i], n)) return;
    l->names[l->n++] = n;
}

const char *kl_vk_extra_instance_exts[8];
int kl_vk_native_wsi;

static int ext_supported(const VkExtensionProperties *have, uint32_t nhave,
                         const char *want) {
    for (uint32_t i = 0; i < nhave; i++)
        if (!strcmp(have[i].extensionName, want)) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------
static VkResult klvk_EnumerateInstanceExtensionProperties(
        const char *layer, uint32_t *count, VkExtensionProperties *props) {
    vk_init();
    if (!g_avail) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceExtensionProperties real =
        (PFN_vkEnumerateInstanceExtensionProperties)
            real_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t n = 0;
    real(layer, &n, NULL);
    VKT("vkEnumerateInstanceExtensionProperties(%s): %u from MoltenVK\n", layer ? layer : "NULL", n);
    VkExtensionProperties *tmp = calloc(n + 2, sizeof *tmp);
    if (!tmp) return VK_ERROR_OUT_OF_HOST_MEMORY;
    real(layer, &n, tmp);

    // VK_KHR_android_surface is advertised because we implement it. Unity tests
    // for it before enabling it, and a Vulkan build that cannot find a surface
    // extension does not proceed.
    if (!layer && !ext_supported(tmp, n, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
        snprintf(tmp[n].extensionName, sizeof tmp[n].extensionName, "%s",
                 VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
        tmp[n].specVersion = 6;
        n++;
    }

    // A personality can advertise extra instance extensions its guest gates
    // on (a Linux guest wants a Linux WSI name before it will go on to create
    // a surface through SDL). They are dropped at vkCreateInstance, like any
    // name MoltenVK does not have.
    for (int i = 0; !layer && kl_vk_extra_instance_exts[i]; i++) {
        if (ext_supported(tmp, n, kl_vk_extra_instance_exts[i])) continue;
        tmp = realloc(tmp, (n + 1) * sizeof *tmp);
        memset(&tmp[n], 0, sizeof tmp[n]);
        snprintf(tmp[n].extensionName, sizeof tmp[n].extensionName, "%s", kl_vk_extra_instance_exts[i]);
        tmp[n].specVersion = 6;
        n++;
    }

    VkResult r = VK_SUCCESS;
    if (!props) *count = n;
    else {
        uint32_t give = *count < n ? *count : n;
        memcpy(props, tmp, give * sizeof *props);
        if (give < n) r = VK_INCOMPLETE;
        *count = give;
    }
    free(tmp);
    return r;
}

// ---- The guest's own instance and device settings, as it asked for them (KL_VK_DEVICE_INFO=<file>|auto) ----
// Pipelines compile to the same MSL only on a device made the same way, so a replay that runs before the game (the
// launcher warming the Metal cache with no guest at all: kl_foz_prewarm) needs these: written as JSON at
// vkCreateDevice, "auto" being device-info.json in the pipeline recorder's folder (KL_VK_FOZ_DIR, else
// $HOME/Documents/klfz). Feature structs go out as their bytes after the sType/pNext header; one this table does not
// know is listed by sType with size 0.
static struct { uint32_t api, app_ver, eng_ver; char app[64], eng[64]; char *ext[64]; uint32_t n; } g_inst_info;

static void devinfo_note_instance(const VkInstanceCreateInfo *ci) {
    if (!ci) return;
    for (uint32_t i = 0; i < g_inst_info.n; i++) free(g_inst_info.ext[i]);
    g_inst_info.n = 0;  // the latest instance's
    const VkApplicationInfo *a = ci->pApplicationInfo;
    if (a) {
        g_inst_info.api = a->apiVersion; g_inst_info.app_ver = a->applicationVersion; g_inst_info.eng_ver = a->engineVersion;
        snprintf(g_inst_info.app, sizeof g_inst_info.app, "%s", a->pApplicationName ? a->pApplicationName : "");
        snprintf(g_inst_info.eng, sizeof g_inst_info.eng, "%s", a->pEngineName ? a->pEngineName : "");
    }
    for (uint32_t i = 0; i < ci->enabledExtensionCount && g_inst_info.n < 64; i++)
        g_inst_info.ext[g_inst_info.n++] = strdup(ci->ppEnabledExtensionNames[i]);
}

static const struct { VkStructureType t; size_t size; } k_feature_structs[] = {
#define F(T, S) { VK_STRUCTURE_TYPE_##T, sizeof(S) }
    F(PHYSICAL_DEVICE_FEATURES_2, VkPhysicalDeviceFeatures2),
    F(PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, VkPhysicalDeviceVulkan11Features),
    F(PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, VkPhysicalDeviceVulkan12Features),
    F(PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, VkPhysicalDeviceVulkan13Features),
    F(PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures),
    F(PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures),
    F(PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features),
    F(PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures),
    F(PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures),
    F(PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures),
    F(PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures),
    F(PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures),
    F(PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, VkPhysicalDeviceImagelessFramebufferFeatures),
    F(PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, VkPhysicalDeviceUniformBufferStandardLayoutFeatures),
    F(PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures),
    F(PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures),
    F(PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VkPhysicalDeviceVulkanMemoryModelFeatures),
    F(PHYSICAL_DEVICE_MULTIVIEW_FEATURES, VkPhysicalDeviceMultiviewFeatures),
    F(PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, VkPhysicalDeviceSamplerYcbcrConversionFeatures),
    F(PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, VkPhysicalDeviceVariablePointersFeatures),
    F(PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, VkPhysicalDeviceShaderDrawParametersFeatures),
    F(PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, VkPhysicalDeviceProtectedMemoryFeatures),
    F(PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures),
    F(PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features),
    F(PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features),
    F(PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, VkPhysicalDeviceImageRobustnessFeatures),
    F(PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, VkPhysicalDeviceInlineUniformBlockFeatures),
    F(PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, VkPhysicalDevicePipelineCreationCacheControlFeatures),
    F(PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, VkPhysicalDevicePrivateDataFeatures),
    F(PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures),
    F(PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, VkPhysicalDeviceShaderIntegerDotProductFeatures),
    F(PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, VkPhysicalDeviceShaderTerminateInvocationFeatures),
    F(PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures),
    F(PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, VkPhysicalDeviceTextureCompressionASTCHDRFeatures),
    F(PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures),
    F(PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, VkPhysicalDeviceRobustness2FeaturesEXT),
    F(PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicStateFeaturesEXT),
    F(PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState2FeaturesEXT),
    F(PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, VkPhysicalDeviceCustomBorderColorFeaturesEXT),
    F(PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, VkPhysicalDeviceDepthClipEnableFeaturesEXT),
    F(PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT, VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT),
    F(PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, VkPhysicalDevice4444FormatsFeaturesEXT),
    F(PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT),
    F(PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT, VkPhysicalDeviceShaderAtomicFloatFeaturesEXT),
    // Alyx's own (Source 2 on MoltenVK, 2026-10-05): multiview, dynamic rendering, these two
    F(PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR, VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR),
    F(PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT, VkPhysicalDeviceHostImageCopyFeaturesEXT),
#ifdef VK_VERSION_1_4
    F(PHYSICAL_DEVICE_VULKAN_1_4_FEATURES, VkPhysicalDeviceVulkan14Features),
#endif
#undef F
};

static void devinfo_hex(FILE *f, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", ((const unsigned char *)p)[i]);
}

static void devinfo_write(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci) {
    const char *e = getenv("KL_VK_DEVICE_INFO");
    if (!e || !*e || !ci) return;
    char path[1024];
    const char *dir = getenv("KL_VK_FOZ_DIR");  // the pipeline recorder's folder, as kl_fossilize.cpp finds it
    if (!strcmp(e, "auto") && dir && *dir) snprintf(path, sizeof path, "%s/device-info.json", dir);
    else if (!strcmp(e, "auto")) snprintf(path, sizeof path, "%s/Documents/klfz/device-info.json", getenv("HOME") ? getenv("HOME") : ".");
    else snprintf(path, sizeof path, "%s", e);
    FILE *f = fopen(path, "w");
    if (!f) { VKI("device info: cannot write %s\n", path); return; }
    VkPhysicalDeviceProperties pr = {0};
    PFN_vkGetPhysicalDeviceProperties gp = (PFN_vkGetPhysicalDeviceProperties)real_gipa(g_instance, "vkGetPhysicalDeviceProperties");
    if (gp && phys) gp(phys, &pr);
    fprintf(f, "{\"v\": 1, \"gpu\": \"%s\", \"driver\": %u, \"gpu_api\": %u,\n", pr.deviceName, pr.driverVersion, pr.apiVersion);
    fprintf(f, " \"instance\": {\"api\": %u, \"app\": \"%s\", \"app_version\": %u, \"engine\": \"%s\", \"engine_version\": %u, \"extensions\": [",
            g_inst_info.api, g_inst_info.app, g_inst_info.app_ver, g_inst_info.eng, g_inst_info.eng_ver);
    for (uint32_t i = 0; i < g_inst_info.n; i++) fprintf(f, "%s\"%s\"", i ? ", " : "", g_inst_info.ext[i]);
    fprintf(f, "]},\n \"device\": {\"extensions\": [");
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) fprintf(f, "%s\"%s\"", i ? ", " : "", ci->ppEnabledExtensionNames[i]);
    fprintf(f, "],\n  \"queues\": [");
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
        fprintf(f, "%s{\"family\": %u, \"count\": %u, \"flags\": %u}", i ? ", " : "", ci->pQueueCreateInfos[i].queueFamilyIndex,
                ci->pQueueCreateInfos[i].queueCount, ci->pQueueCreateInfos[i].flags);
    fprintf(f, "],\n  \"enabled_features\": ");
    if (ci->pEnabledFeatures) { fputc('"', f); devinfo_hex(f, ci->pEnabledFeatures, sizeof *ci->pEnabledFeatures); fputc('"', f); }
    else fprintf(f, "null");
    fprintf(f, ",\n  \"pnext\": [");
    unsigned n = 0, unknown = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext, n++) {
        size_t size = 0;
        for (size_t k = 0; k < sizeof k_feature_structs / sizeof *k_feature_structs; k++)
            if (k_feature_structs[k].t == b->sType) { size = k_feature_structs[k].size; break; }
        fprintf(f, "%s\n   {\"sType\": %u, \"size\": %zu, \"data\": \"", n ? "," : "", (unsigned)b->sType, size);
        if (size > sizeof *b) devinfo_hex(f, (const char *)b + sizeof *b, size - sizeof *b);
        else unknown++;
        fputc('"', f); fputc('}', f);
    }
    fprintf(f, "]}}\n");
    fclose(f);
    VKI("device info: %u instance + %u device extension(s), %u pNext struct(s) (%u not in the table) -> %s\n",
        g_inst_info.n, ci->enabledExtensionCount, n, unknown, path);
}

static VkResult klvk_CreateInstance(const VkInstanceCreateInfo *ci,
                                    const VkAllocationCallbacks *alloc,
                                    VkInstance *out) {
    vk_init();
    if (!g_avail) return VK_ERROR_INITIALIZATION_FAILED;
    devinfo_note_instance(ci);
    PFN_vkCreateInstance real = (PFN_vkCreateInstance)real_gipa(NULL, "vkCreateInstance");
    PFN_vkEnumerateInstanceExtensionProperties enum_ext =
        (PFN_vkEnumerateInstanceExtensionProperties)
            real_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (!real || !enum_ext) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t nhave = 0;
    enum_ext(NULL, &nhave, NULL);
    VkExtensionProperties *have = calloc(nhave ? nhave : 1, sizeof *have);
    if (have) enum_ext(NULL, &nhave, have);

    extlist keep = {{0}, 0};
    for (uint32_t i = 0; ci && i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (!strcmp(e, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)) {
            VKT("instance ext %s -> ours (MoltenVK has no Android WSI)\n", e);
            continue;
        }
        // A personality-advertised WSI name stands for "a window surface":
        // the one this host has is Metal (what SDL's Vulkan surface uses).
        int extra = 0;
        for (int k = 0; kl_vk_extra_instance_exts[k]; k++) extra |= !strcmp(e, kl_vk_extra_instance_exts[k]);
        if (extra) {
            if (!have || ext_supported(have, nhave, "VK_EXT_metal_surface")) {
                int dup = 0;
                for (unsigned k = 0; k < keep.n; k++) dup |= !strcmp(keep.names[k], "VK_EXT_metal_surface");
                if (!dup) ext_add(&keep, "VK_EXT_metal_surface");
            }
            // ...and a headless one: SDL's offscreen video driver (the Alyx
            // app on visionOS, where there is no window to present to) makes
            // its Vulkan surface with VK_EXT_headless_surface.
            if (have && ext_supported(have, nhave, "VK_EXT_headless_surface")) {
                int dup = 0;
                for (unsigned k = 0; k < keep.n; k++) dup |= !strcmp(keep.names[k], "VK_EXT_headless_surface");
                if (!dup) ext_add(&keep, "VK_EXT_headless_surface");
            }
            VKT("instance ext %s -> VK_EXT_metal_surface (+ headless)\n", e);
            continue;
        }
        if (have && !ext_supported(have, nhave, e)) {
            VKI("instance ext %s NOT supported by MoltenVK — dropped\n", e);
            continue;
        }
        ext_add(&keep, e);
    }

    // MoltenVK is a portability driver: without this flag a conformant loader
    // enumerates ZERO physical devices and still returns VK_SUCCESS, which reads
    // exactly like "Metal is unavailable" and names nothing.
    VkInstanceCreateFlags flags = ci ? ci->flags : 0;
    if (!have || ext_supported(have, nhave, "VK_KHR_portability_enumeration")) {
        ext_add(&keep, "VK_KHR_portability_enumeration");
        flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkInstanceCreateInfo mine = ci ? *ci : (VkInstanceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    mine.flags = flags;
    mine.enabledExtensionCount = keep.n;
    mine.ppEnabledExtensionNames = keep.names;
    // Validation layers do not exist here; asking for one fails the create.
    mine.enabledLayerCount = 0;
    mine.ppEnabledLayerNames = NULL;

    // MoltenVK clamps every physical device's REPORTED apiVersion down to the
    // instance's VkApplicationInfo.apiVersion. A UE4 guest asks for a 1.0
    // instance, but its device-selection gate then rejects any GPU that reports
    // < 1.1 — so the Apple GPU comes back "api 1.0.357", the guest decides the
    // device has no usable Vulkan and puts up "This device does not support
    // Vulkan…" and quits (Wrath2, SIGKILL, no device ever created). Raise the
    // requested version to at least 1.1 (MoltenVK backs 1.4 here) so the device
    // reports >= 1.1 and clears the gate. A guest that only uses 1.0 features is
    // unaffected by the higher ceiling; UE5 guests already ask for 1.1 and are
    // untouched. This is also why olar (UE5) got a 1.1 device and rendered.
    VkApplicationInfo appinfo;
    const VkApplicationInfo *src = ci ? ci->pApplicationInfo : NULL;
    appinfo = src ? *src
                  : (VkApplicationInfo){ .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    if (appinfo.apiVersion < VK_API_VERSION_1_1) {
        VKI("instance apiVersion %u.%u -> raised to 1.1 (MoltenVK reports the "
            "device at the instance version; UE4's device gate needs >= 1.1)\n",
            VK_VERSION_MAJOR(appinfo.apiVersion), VK_VERSION_MINOR(appinfo.apiVersion));
        appinfo.apiVersion = VK_API_VERSION_1_1;
    }
    mine.pApplicationInfo = &appinfo;

    VkResult r = real(&mine, alloc, out);
    free(have);
    if (r == VK_SUCCESS) {
        g_instance = *out;
        cache_display_luid(*out); // grab the GPU LUID now — the guest may destroy
                                  // this instance before ovrp_GetDisplayAdapterId2
        VKI("vkCreateInstance ok — %u extension(s)%s\n", keep.n,
            ci && ci->enabledLayerCount ? ", layers dropped" : "");
    } else {
        VKI("vkCreateInstance FAILED %d\n", r);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
static void dev_resolve(klvk_device *d) {
#define R(f) d->f = (PFN_vk##f)real_gdpa(d->dev, "vk" #f)
    R(GetDeviceQueue); R(CreateImage); R(DestroyImage); R(GetImageMemoryRequirements);
    R(AllocateMemory); R(FreeMemory); R(BindImageMemory);
    R(CreateBuffer); R(DestroyBuffer); R(GetBufferMemoryRequirements);
    R(BindBufferMemory); R(MapMemory); R(UnmapMemory);
    R(InvalidateMappedMemoryRanges);
    R(CreateCommandPool); R(DestroyCommandPool); R(AllocateCommandBuffers);
    R(BeginCommandBuffer); R(EndCommandBuffer); R(ResetCommandBuffer);
    R(CmdPipelineBarrier); R(CmdCopyImageToBuffer); R(CmdClearColorImage); R(QueueSubmit);
    R(CreateFence); R(DestroyFence); R(WaitForFences); R(ResetFences);
    R(DeviceWaitIdle); R(QueueWaitIdle);
#undef R
}


// KL_VK_FAKE_MINMAX=1 — the headset's MoltenVK reports samplerFilterMinmax = 0
// and no SAMPLED_IMAGE_FILTER_MINMAX format bit (the Mac's reports both), and
// Source 2's render device init refuses the headset's GPU; that one feature is
// the only capability difference between the two (KL_VK_DUMP_CAPS diff). This
// advertises it and strips the reduction mode from the samplers that ask for
// it (they then filter as weighted average — the probe, not the final fix).
static int klvk_fake_minmax(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_VK_FAKE_MINMAX", 0);
    return on;
}
static void klvk_minmax_features(void *pnext) {
    if (!klvk_fake_minmax()) return;
    for (VkBaseOutStructure *b = pnext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            ((VkPhysicalDeviceVulkan12Features *)b)->samplerFilterMinmax = VK_TRUE;
}
static void klvk_minmax_props(void *pnext) {
    if (!klvk_fake_minmax()) return;
    for (VkBaseOutStructure *b = pnext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES) {
            ((VkPhysicalDeviceSamplerFilterMinmaxProperties *)b)->filterMinmaxSingleComponentFormats = VK_TRUE;
            ((VkPhysicalDeviceSamplerFilterMinmaxProperties *)b)->filterMinmaxImageComponentMapping = VK_TRUE;
        }
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
            ((VkPhysicalDeviceVulkan12Properties *)b)->filterMinmaxSingleComponentFormats = VK_TRUE;
            ((VkPhysicalDeviceVulkan12Properties *)b)->filterMinmaxImageComponentMapping = VK_TRUE;
        }
    }
}
static void klvk_minmax_format(VkFormatProperties *p) {
    if (!klvk_fake_minmax() || !p) return;
    if (p->optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        p->optimalTilingFeatures |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT;
}
// Unlink one struct type from a const pNext chain, in place.
static void klvk_unlink(const void *head_pnext_owner, VkStructureType t) {
    VkBaseOutStructure *prev = (VkBaseOutStructure *)head_pnext_owner;
    while (prev && prev->pNext) {
        if (prev->pNext->sType == t) { prev->pNext = prev->pNext->pNext; continue; }
        prev = prev->pNext;
    }
}
static VkResult klvk_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci,
                                  const VkAllocationCallbacks *alloc, VkDevice *out) {
    PFN_vkCreateDevice real = (PFN_vkCreateDevice)real_gipa(g_instance, "vkCreateDevice");
    PFN_vkEnumerateDeviceExtensionProperties enum_ext =
        (PFN_vkEnumerateDeviceExtensionProperties)
            real_gipa(g_instance, "vkEnumerateDeviceExtensionProperties");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    // The whole point of the null-substitution family: Unity reaches here with a
    // NULL physicalDevice (its GPU select left it null), and MoltenVK would make
    // a device on nothing. Give it the real GPU so it gets a working VkDevice.
    phys = KLVK_PHYS_OR_SUB(phys, "CreateDevice");
    // as the guest asked, before anything below edits it
    devinfo_write(phys, ci);
    if (ci && g_pipe_hooks && g_pipe_hooks->device_info) g_pipe_hooks->device_info(ci);
    if (klvk_fake_minmax() && ci)
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES &&
                ((VkPhysicalDeviceVulkan12Features *)b)->samplerFilterMinmax) {
                ((VkPhysicalDeviceVulkan12Features *)b)->samplerFilterMinmax = VK_FALSE;
                VKI("CreateDevice: samplerFilterMinmax requested — cleared (KL_VK_FAKE_MINMAX)\n");
            }

    uint32_t nhave = 0;
    VkExtensionProperties *have = NULL;
    if (enum_ext) {
        enum_ext(phys, NULL, &nhave, NULL);
        have = calloc(nhave ? nhave : 1, sizeof *have);
        if (have) enum_ext(phys, NULL, &nhave, have);
    }

    extlist keep = {{0}, 0};
    for (uint32_t i = 0; ci && i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (have && !ext_supported(have, nhave, e)) {
            VKI("device ext %s NOT supported by MoltenVK — dropped\n", e);
            continue;
        }
        ext_add(&keep, e);
    }
    // The spec REQUIRES this one to be enabled when the device advertises it,
    // and MoltenVK always does. Omitting it is undefined behaviour, not a
    // missing nicety.
    if (have && ext_supported(have, nhave, "VK_KHR_portability_subset"))
        ext_add(&keep, "VK_KHR_portability_subset");
    // ...and one the guest never asks for and we need: `vkExportMetalObjectsEXT`
    // is how the MTLTexture behind an eye VkImage reaches the compositor, and
    // calling it on a device that did not enable this is undefined. It adds no
    // behaviour of its own — nothing else in the device's operation changes —
    // and eye_mtl_texture falls back to MoltenVK's own deprecated entry point if
    // it is absent, so this is an upgrade rather than a requirement.
    if (have && ext_supported(have, nhave, "VK_EXT_metal_objects"))
        ext_add(&keep, "VK_EXT_metal_objects");

    VkDeviceCreateInfo mine = ci ? *ci : (VkDeviceCreateInfo){
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    mine.enabledExtensionCount = keep.n;
    mine.ppEnabledExtensionNames = keep.names;
    mine.enabledLayerCount = 0;
    mine.ppEnabledLayerNames = NULL;

    VkResult r = real(phys, &mine, alloc, out);
    free(have);
    if (r != VK_SUCCESS) { VKI("vkCreateDevice FAILED %d\n", r); return r; }

    klvk_device *d = calloc(1, sizeof *d);
    if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->dev = *out;
    d->phys = phys;
    d->queue_family = ci && ci->queueCreateInfoCount ? ci->pQueueCreateInfos[0].queueFamilyIndex : 0;
    dev_resolve(d);
    // A queue of our own, for the acquire signal and the capture submit. Taken
    // from the guest's own first queue family, which is the one it will present
    // on — see the note in klvk_AcquireNextImageKHR about the sharing.
    if (d->GetDeviceQueue) d->GetDeviceQueue(d->dev, d->queue_family, 0, &d->queue);

    pthread_mutex_lock(&g_lock);
    if (g_ndev < (int)(sizeof g_devs / sizeof *g_devs)) g_devs[g_ndev++] = d;
    pthread_mutex_unlock(&g_lock);

    VKI("vkCreateDevice ok — %u extension(s), queue family %u\n", keep.n, d->queue_family);
    if (g_pipe_hooks && g_pipe_hooks->device_created) g_pipe_hooks->device_created(d->dev, klvk_GetDeviceProcAddr);
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Surface — synthesized whole. There is no Android window here and nothing that
// has to reach a screen: this arc ends in a file on disk.
// ---------------------------------------------------------------------------
static VkResult klvk_CreateAndroidSurfaceKHR(VkInstance inst,
        const VkAndroidSurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *alloc,
        VkSurfaceKHR *out) {
    (void)inst; (void)ci; (void)alloc;
    klvk_surface *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->magic = KLVK_SURF_MAGIC;
    // The eye size the rest of the port already agreed on, so a Vulkan frame is
    // the same shape as the GLES one and the compositor seam does not have to
    // learn a second size later.
    s->w = (uint32_t)kl_env_int("KL_VK_WIDTH", 2064);
    s->h = (uint32_t)kl_env_int("KL_VK_HEIGHT", 2208);
    *out = (VkSurfaceKHR)s;
    VKI("vkCreateAndroidSurfaceKHR -> synthetic surface %ux%u\n", s->w, s->h);
    return VK_SUCCESS;
}

static klvk_surface *surf_of(VkSurfaceKHR h) {
    klvk_surface *s = (klvk_surface *)h;
    return (s && s->magic == KLVK_SURF_MAGIC) ? s : NULL;
}

static void klvk_DestroySurfaceKHR(VkInstance inst, VkSurfaceKHR surf,
                                   const VkAllocationCallbacks *alloc) {
    (void)inst; (void)alloc;
    klvk_surface *s = surf_of(surf);
    if (s) { s->magic = 0; free(s); }
}

static VkResult klvk_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice p, uint32_t fam,
        VkSurfaceKHR surf, VkBool32 *out) {
    (void)p; (void)fam; (void)surf;
    *out = VK_TRUE;
    return VK_SUCCESS;
}

static VkResult klvk_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p,
        VkSurfaceKHR surf, VkSurfaceCapabilitiesKHR *c) {
    (void)p;
    klvk_surface *s = surf_of(surf);
    uint32_t w = s ? s->w : 1024, h = s ? s->h : 1024;
    memset(c, 0, sizeof *c);
    c->minImageCount = 2;
    c->maxImageCount = KLVK_MAX_IMAGES;
    c->currentExtent.width = w;   c->currentExtent.height = h;
    c->minImageExtent = c->currentExtent;
    c->maxImageExtent = c->currentExtent;
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
                                 VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    // TRANSFER_SRC is not decoration: it is what lets the capture read the image
    // the guest presented. A guest that trims usage to what the surface reports
    // would otherwise hand us an image that cannot legally be copied from.
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                             VK_IMAGE_USAGE_SAMPLED_BIT |
                             VK_IMAGE_USAGE_STORAGE_BIT;
    return VK_SUCCESS;
}

static const VkFormat k_surf_formats[] = {
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_R8G8B8A8_SRGB,  VK_FORMAT_B8G8R8A8_SRGB,
};

static VkResult klvk_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice p,
        VkSurfaceKHR surf, uint32_t *count, VkSurfaceFormatKHR *out) {
    (void)p; (void)surf;
    uint32_t n = sizeof k_surf_formats / sizeof *k_surf_formats;
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t give = *count < n ? *count : n;
    for (uint32_t i = 0; i < give; i++) {
        out[i].format = k_surf_formats[i];
        out[i].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }
    *count = give;
    return give < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult klvk_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice p,
        VkSurfaceKHR surf, uint32_t *count, VkPresentModeKHR *out) {
    (void)p; (void)surf;
    static const VkPresentModeKHR modes[] = {
        VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    uint32_t n = sizeof modes / sizeof *modes;
    if (!out) { *count = n; return VK_SUCCESS; }
    uint32_t give = *count < n ? *count : n;
    memcpy(out, modes, give * sizeof *out);
    *count = give;
    return give < n ? VK_INCOMPLETE : VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Swapchain — ours end to end
// ---------------------------------------------------------------------------
static uint32_t mem_type(klvk_device *d, uint32_t bits, VkMemoryPropertyFlags want) {
    PFN_vkGetPhysicalDeviceMemoryProperties gmp =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp;
    memset(&mp, 0, sizeof mp);
    if (gmp) gmp(d->phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

// Forward decls: the swapchain path below registers its images, but the registry
// itself is defined further down beside klvk_image_alloc (which needs VkImage).
static void klvk_img_reg_add(VkImage img);
static int  klvk_img_reg_has(VkImage img);

static VkResult klvk_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci,
        const VkAllocationCallbacks *alloc, VkSwapchainKHR *out) {
    (void)alloc;
    klvk_device *d = dev_find(dev);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;

    klvk_swapchain *sc = calloc(1, sizeof *sc);
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->magic = KLVK_SWAP_MAGIC;
    sc->d = d;
    sc->format = ci->imageFormat;
    sc->extent = ci->imageExtent;
    sc->n = ci->minImageCount < 2 ? 2 : ci->minImageCount;
    if (sc->n > KLVK_MAX_IMAGES) sc->n = KLVK_MAX_IMAGES;

    for (uint32_t i = 0; i < sc->n; i++) {
        VkImageCreateInfo ic = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = sc->format,
            .extent = { sc->extent.width, sc->extent.height, 1 },
            .mipLevels = 1,
            .arrayLayers = ci->imageArrayLayers ? ci->imageArrayLayers : 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            // TRANSFER_SRC on top of whatever the guest asked for: the capture
            // copies out of exactly these images.
            .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = ci->imageSharingMode,
            .queueFamilyIndexCount = ci->queueFamilyIndexCount,
            .pQueueFamilyIndices = ci->pQueueFamilyIndices,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkResult r = d->CreateImage(dev, &ic, NULL, &sc->images[i]);
        if (r != VK_SUCCESS) { VKI("swapchain image %u: %d\n", i, r); goto fail; }
        klvk_img_reg_add(sc->images[i]);

        VkMemoryRequirements mr;
        d->GetImageMemoryRequirements(dev, sc->images[i], &mr);
        uint32_t mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == UINT32_MAX) mt = mem_type(d, mr.memoryTypeBits, 0);
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
        };
        r = d->AllocateMemory(dev, &ai, NULL, &sc->mem[i]);
        if (r != VK_SUCCESS) { VKI("swapchain memory %u: %d\n", i, r); goto fail; }
        r = d->BindImageMemory(dev, sc->images[i], sc->mem[i], 0);
        if (r != VK_SUCCESS) { VKI("swapchain bind %u: %d\n", i, r); goto fail; }
    }

    *out = (VkSwapchainKHR)sc;
    VKI("vkCreateSwapchainKHR -> %u image(s) %ux%u fmt %d (ours, no presentation engine)\n",
        sc->n, sc->extent.width, sc->extent.height, (int)sc->format);
    return VK_SUCCESS;
fail:
    free(sc);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static klvk_swapchain *swap_of(VkSwapchainKHR h) {
    klvk_swapchain *s = (klvk_swapchain *)h;
    return (s && s->magic == KLVK_SWAP_MAGIC) ? s : NULL;
}

static void klvk_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR h,
                                     const VkAllocationCallbacks *alloc) {
    (void)alloc;
    klvk_swapchain *sc = swap_of(h);
    if (!sc) return;
    klvk_device *d = sc->d;
    if (d->DeviceWaitIdle) d->DeviceWaitIdle(dev);
    for (uint32_t i = 0; i < sc->n; i++) {
        if (sc->images[i]) d->DestroyImage(dev, sc->images[i], NULL);
        if (sc->mem[i]) d->FreeMemory(dev, sc->mem[i], NULL);
    }
    // The readback scratch is the DEVICE's, not this swapchain's — it outlives
    // any one swapchain and is shared with the eye path.
    sc->magic = 0;
    free(sc);
}

static VkResult klvk_GetSwapchainImagesKHR(VkDevice dev, VkSwapchainKHR h,
        uint32_t *count, VkImage *out) {
    (void)dev;
    klvk_swapchain *sc = swap_of(h);
    if (!sc) return VK_ERROR_INITIALIZATION_FAILED;
    if (!out) { *count = sc->n; return VK_SUCCESS; }
    uint32_t give = *count < sc->n ? *count : sc->n;
    memcpy(out, sc->images, give * sizeof *out);
    *count = give;
    return give < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}

// Signal whatever the guest asked to be signalled, with no work attached.
//
// This is the one place a real presentation engine is genuinely being stood in
// for: an acquire hands back an image AND a signal that it is safe to use. With
// no engine there is nothing to wait for, so the signal is immediate — an empty
// submit is the only legal way to signal a binary semaphore from the host side.
//
// It goes on the guest's own queue, which is a shared resource: this is safe
// because acquire and submit are called from the same thread in every renderer
// that exists (Vulkan queues are externally synchronized and Unity's render
// thread owns this one). Worth naming, because a second presenting thread would
// make it a race with no error surface.
static VkResult klvk_AcquireNextImageKHR(VkDevice dev, VkSwapchainKHR h,
        uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *index) {
    (void)timeout;
    klvk_swapchain *sc = swap_of(h);
    if (!sc) return VK_ERROR_OUT_OF_DATE_KHR;
    klvk_device *d = sc->d;

    *index = sc->next;
    sc->next = (sc->next + 1) % sc->n;

    if (sem || fence) {
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .signalSemaphoreCount = sem ? 1u : 0u,
            .pSignalSemaphores = sem ? &sem : NULL,
        };
        VkResult r = d->QueueSubmit(d->queue, 1, &si, fence);
        if (r != VK_SUCCESS) VKT("acquire signal submit: %d\n", r);
    }
    (void)dev;
    return VK_SUCCESS;
}

// ---------------------------------------------------------------------------
// Present — and the capture
// ---------------------------------------------------------------------------
// Readback scratch big enough for `need` bytes, on the device rather than on a
// swapchain: the OVRPlugin eye path has no swapchain at all and needs the same
// machinery, and the two never capture at once.
static int cap_prepare(klvk_device *d, VkDeviceSize need) {
    if (d->cap_buf && d->cap_size >= need) return 1;
    if (d->cap_buf) {                         // grew — tear the old one down
        if (d->DeviceWaitIdle) d->DeviceWaitIdle(d->dev);
        d->DestroyBuffer(d->dev, d->cap_buf, NULL);
        d->FreeMemory(d->dev, d->cap_mem, NULL);
        d->cap_buf = VK_NULL_HANDLE; d->cap_mem = VK_NULL_HANDLE;
    }

    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = need,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (d->CreateBuffer(d->dev, &bi, NULL, &d->cap_buf) != VK_SUCCESS) return 0;
    d->cap_size = need;

    VkMemoryRequirements mr;
    d->GetBufferMemoryRequirements(d->dev, d->cap_buf, &mr);
    uint32_t mt = mem_type(d, mr.memoryTypeBits,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX)
        mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mt == UINT32_MAX) return 0;
    VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
    };
    if (d->AllocateMemory(d->dev, &ai, NULL, &d->cap_mem) != VK_SUCCESS) return 0;
    if (d->BindBufferMemory(d->dev, d->cap_buf, d->cap_mem, 0) != VK_SUCCESS) return 0;

    if (d->cap_pool) return 1;                // pool/cmd/fence are size-independent
    VkCommandPoolCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = d->queue_family,
    };
    if (d->CreateCommandPool(d->dev, &pi, NULL, &d->cap_pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = d->cap_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (d->AllocateCommandBuffers(d->dev, &cbi, &d->cap_cmd) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (d->CreateFence(d->dev, &fi, NULL, &d->cap_fence) != VK_SUCCESS) return 0;
    return 1;
}

static const char *cap_dir(void) { return kl_env_str("KL_VK_OUT", NULL); }

// Copy any image out and write it as a PNG.
//
// `cur` is the layout the guest left the image in, and getting it wrong is
// cheap here in a way it would not be on a real driver: **MoltenVK does not
// implement image layouts** — Metal has no such concept — so the barrier is in
// practice a memory barrier and the transition is bookkeeping. It is still
// written correctly, because this file is also the model for the device path.
// `layer` is the array slice to read. It is 0 for every 2D image here and 0/1
// for the two eyes of an Array-layout eye image, and it has to travel all the
// way into the copy region: a barrier and a copy that both say slice 0 read the
// LEFT eye twice and produce two identical PNGs, which reads exactly like a
// guest that renders one eye — the failure this capture exists to distinguish.
static int cap_image_layer(klvk_device *d, VkQueue q, VkImage img, uint32_t w, uint32_t h,
                           VkFormat fmt, VkImageLayout cur, uint32_t layer,
                           const char *path,
                           const VkSemaphore *waits, uint32_t nwait) {
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    if (!cap_prepare(d, need)) { VKI("capture setup failed\n"); return 0; }

    d->ResetCommandBuffer(d->cap_cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    d->BeginCommandBuffer(d->cap_cmd, &bi);

    VkImageMemoryBarrier to_src = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = cur,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 },
    };
    d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);

    VkBufferImageCopy region = {
        .bufferOffset = 0, .bufferRowLength = 0, .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { w, h, 1 },
    };
    d->CmdCopyImageToBuffer(d->cap_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            d->cap_buf, 1, &region);

    // Put it back where the guest left it, or its next render pass starts from
    // a layout it did not expect.
    VkImageMemoryBarrier back = to_src;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = cur;
    d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &back);
    d->EndCommandBuffer(d->cap_cmd);

    // Waiting on the caller's semaphores does double duty: it orders the copy
    // after the guest's rendering, and it CONSUMES signals that would otherwise
    // stay high forever — a binary semaphore nobody waits on is a validation
    // error and, on a real driver, a hang two frames later.
    VkPipelineStageFlags *stages = NULL;
    if (nwait) {
        stages = calloc(nwait, sizeof *stages);
        for (uint32_t i = 0; i < nwait; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
    VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = nwait,
        .pWaitSemaphores = nwait ? waits : NULL,
        .pWaitDstStageMask = stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &d->cap_cmd,
    };
    d->ResetFences(d->dev, 1, &d->cap_fence);
    VkResult r = d->QueueSubmit(q, 1, &si, d->cap_fence);
    free(stages);
    if (r != VK_SUCCESS) { VKI("capture submit: %d\n", r); return 0; }
    d->WaitForFences(d->dev, 1, &d->cap_fence, VK_TRUE, 2000000000ull);

    void *mapped = NULL;
    if (d->MapMemory(d->dev, d->cap_mem, 0, need, 0, &mapped) != VK_SUCCESS) return 0;
    VkMappedMemoryRange mmr = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = d->cap_mem, .offset = 0, .size = VK_WHOLE_SIZE,
    };
    if (d->InvalidateMappedMemoryRanges) d->InvalidateMappedMemoryRanges(d->dev, 1, &mmr);

    uint8_t *px = mapped;
    size_t n = (size_t)w * h;
    if (fmt_is_bgra(fmt))
        for (size_t i = 0; i < n; i++) {
            uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t;
        }
    // Alpha is not a coverage value here. A guest that never authors alpha
    // leaves it 0, and a correct RGB frame then
    // writes out as a fully TRANSPARENT PNG, which is visually identical to
    // black and has no error surface at all.
    for (size_t i = 0; i < n; i++) px[i * 4 + 3] = 255;

    // Is there anything in it? A dark frame and a never-written frame look the
    // same in a file browser, and this is the whole question the arc is asking.
    uint64_t sum = 0;
    size_t lit = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned v = px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2];
        sum += v;
        if (v) lit++;
    }

    int ok = png_write_rgba(path, px, (int)w, (int)h);
    d->UnmapMemory(d->dev, d->cap_mem);
    if (ok) {
        g_captured++;
        VKI("-> %s (%zu lit / %zu, mean %.1f)\n", path, lit, n,
            n ? (double)sum / (double)(n * 3) : 0.0);
    } else {
        VKI("PNG write failed (%s)\n", path);
    }
    return ok;
}

static int cap_image(klvk_device *d, VkQueue q, VkImage img, uint32_t w, uint32_t h,
                     VkFormat fmt, VkImageLayout cur, const char *path,
                     const VkSemaphore *waits, uint32_t nwait) {
    return cap_image_layer(d, q, img, w, h, fmt, cur, 0, path, waits, nwait);
}

static void cap_frame(klvk_swapchain *sc, VkQueue q, uint32_t idx,
                      const VkSemaphore *waits, uint32_t nwait) {
    char path[1024];
    snprintf(path, sizeof path, "%s/vk_%05u.png", cap_dir(), g_presented);
    cap_image(sc->d, q, sc->images[idx], sc->extent.width, sc->extent.height,
              sc->format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, path, waits, nwait);
}

// ---------------------------------------------------------------------------
// The OVRPlugin eye layer — the path this guest actually takes
// ---------------------------------------------------------------------------
//
// BONELAB never creates a WSI swapchain. Unity's Oculus XR path asks OVRPlugin
// for its eye textures instead (`ovrp_GetLayerTexture2`) and submits them to the
// Oculus compositor, so `VK_KHR_swapchain` above is exercised by nothing on this
// title — it stays because it is the general answer for a flat Vulkan guest and
// because the two share every line of the capture below.
//
// kl_ovrp.c owns that seam and has always answered with GL texture names. On the
// Vulkan path those names go straight into `vkCreateImageView` as VkImage
// handles, which is a segfault inside MoltenVK with a GL name for an address.
// So the eye textures have to be real VkImages, and this is where they are made.
// Two eye LAYOUTS reach here, and they differ in how many images exist rather
// than in anything about one image:
//
//   Stereo (ovrpLayout 0) — one 2D image per (stage, eye), array layer 0.
//   Array  (ovrpLayout 3) — ONE image per stage with two array layers, handed
//                           back for both eyes; the eye IS the layer index.
//
// The second is what Unity calls Single Pass Instanced / Multiview, and it is
// how this title ships on a Quest. `layers` is therefore a property of the
// stage, not of the eye: kl_ovrp.c passes 2 for Array and 1 for Stereo, and the
// slot for eye 1 is left empty under Array so nothing can hand out a second
// image that does not exist.
#define KLVK_EYE_STAGES 4
typedef struct {
    VkImage        img;
    VkDeviceMemory mem;
    uint32_t       w, h;
    uint32_t       layers;
    VkFormat       fmt;
} klvk_img_slot;

static klvk_img_slot g_eye[KLVK_EYE_STAGES][2];

// ...and the OTHER layers, which until now shared the table above.
//
// `ovrp_GetLayerTexture2` names a LAYER, and the storage was keyed on (stage,
// eye) alone — so every layer the guest set up was handed the eye layer's
// images. For a Unity guest that was invisible, because its only other layer is
// a 1x1 dummy nothing renders into. An Unreal guest has real ones: OculusHMD's
// FSplash and its IStereoLayers quads are how RE4 draws its intro logos, and
// they were being rendered straight into the corner of the eye image — a small
// logo in the top-left of an otherwise black eye, which reads as a broken
// projection rather than as a layer with no storage of its own.
//
// Small and fixed: a guest that runs out says so by name rather than silently
// aliasing again, which is the failure this replaces.
#define KLVK_MAX_LAYERS 64
static struct {
    int           key;                       // the guest's layer id; 0 = free
    unsigned      seq;                        // claim order, for recycling oldest
    klvk_img_slot s[KLVK_EYE_STAGES][2];
} g_layer_img[KLVK_MAX_LAYERS];
static VkImage g_wild_scratch;   // wild-handle stand-in (see klvk_wild_scratch); defined lazily
static VkImage g_content_rt; static uint32_t g_content_rt_w, g_content_rt_h, g_content_rt_area; // KL_RT_DEBUG: largest UI render target
#ifndef KLVK_SCRATCH_MIPS
#ifndef KLVK_SCRATCH_MIPS
#define KLVK_SCRATCH_MIPS   12u
#define KLVK_SCRATCH_LAYERS 2u
#endif
#endif
static unsigned g_wild_copy_redir, g_wild_blit_redir; // commands rerouted onto the scratch
static unsigned g_layer_img_seq;

int kl_vulkan_guest_active(void) { return g_avail && g_ndev > 0; }

// The LUID of the physical device the guest's instance sees, for ovrp_GetDisplay
// AdapterId2. Unity (OculusXR) asks OVRPlugin which GPU is the VR one, then walks
// vkEnumeratePhysicalDevices matching each device's VkPhysicalDeviceIDProperties.
// deviceLUID against the answer — and hands the MATCH to vkGetPhysicalDeviceQueue
// FamilyProperties. Answering a zero LUID (the old GetDeviceId2 stub) matched
// nothing once MoltenVK 1.4.2 began reporting a real, non-zero deviceLUID, so
// Unity selected a NULL VkPhysicalDevice and crashed inside MVKPhysicalDevice::
// getQueueFamilies. Report the real one.
//
// It must be CACHED at instance-create time, not queried on demand: Unity creates
// a throwaway instance to enumerate, DESTROYS it, and only then calls
// GetDisplayAdapterId2 — so by the time the id is asked for, g_instance points at
// a torn-down handle. The LUID is a property of the GPU (constant for the run), so
// caching the first live instance's answer is exactly right.
static uint8_t g_display_luid[8];
static int     g_display_luid_valid;

static void cache_display_luid(VkInstance inst) {
    if (g_display_luid_valid || !inst || !real_gipa) return;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)
        real_gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 gp2 = (PFN_vkGetPhysicalDeviceProperties2)
        real_gipa(inst, "vkGetPhysicalDeviceProperties2");
    if (!en || !gp2) return;
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (en(inst, &n, &pd) < 0 || !pd) return;
    VkPhysicalDeviceIDProperties idp;
    memset(&idp, 0, sizeof idp);
    idp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 p2;
    memset(&p2, 0, sizeof p2);
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &idp;
    gp2(pd, &p2);
    if (!idp.deviceLUIDValid) return;
    memcpy(g_display_luid, idp.deviceLUID, 8 /* VK_LUID_SIZE */);
    g_display_luid_valid = 1;
    VKI("cached display adapter LUID %02x%02x%02x%02x%02x%02x%02x%02x\n",
        g_display_luid[0], g_display_luid[1], g_display_luid[2], g_display_luid[3],
        g_display_luid[4], g_display_luid[5], g_display_luid[6], g_display_luid[7]);
}

// Returns 1 and fills out[8] once any Vulkan instance has existed and reported a
// valid LUID; 0 (out untouched) for a guest that never made a Vulkan instance
// (i.e. GLES), where the adapter id must stay empty.
int kl_vulkan_display_luid(uint8_t out[8]) {
    if (!out) return 0;
    if (!g_display_luid_valid) cache_display_luid(g_instance); // fallback if still live
    if (!g_display_luid_valid) return 0;
    memcpy(out, g_display_luid, 8);
    return 1;
}

// The live instance's first physical device — the substitute for a guest that
// reaches a vkGetPhysicalDevice*/vkCreateDevice call with a NULL handle. AC Nexus
// (Unity) does exactly that: its GfxDeviceVulkan runs a whole battery of device
// queries (QueueFamilyProperties, Features2, MemoryProperties, ...) and then
// vkCreateDevice on a physicalDevice that came out NULL, and MoltenVK dereferences
// null in each. There is exactly one GPU, so a NULL handle is unambiguous —
// substitute it everywhere rather than crash. Enumerated fresh off g_instance each
// time so it is always a handle valid for the LIVE instance (the guest may have
// destroyed the one it enumerated). Cheap: MoltenVK caches its physical devices.
static VkPhysicalDevice live_phys0(void) {
    if (!g_instance || !real_gipa) return VK_NULL_HANDLE;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)
        real_gipa(g_instance, "vkEnumeratePhysicalDevices");
    if (!en) return VK_NULL_HANDLE;
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    en(g_instance, &n, &pd);
    return pd;
}
// Substitute + one-time-per-callsite note. Evaluates to the usable handle.
static VkPhysicalDevice klvk_sub_phys(const char *where) {
    VkPhysicalDevice sub = live_phys0();
    static const char *last;
    if (last != where) { last = where;
        VKI("%s: guest passed NULL physicalDevice — substituting %p\n",
            where, (void *)sub); }
    return sub;
}

// ---------------------------------------------------------------------------
// The compositor seam — the MTLTexture behind an eye VkImage
// ---------------------------------------------------------------------------
//
// Both compositors (KleptonCompositor.swift on device, kl_view_mtl.m on the
// host) sample an `id<MTLTexture>` the guest rendered into, found through
// `kl_glfb_eye_mtl_texture`. On the GL path the host ALLOCATES that texture and
// ANGLE is told to use it as the eye's storage. On the Vulkan path the
// direction is the other way round and it is simpler: MoltenVK has already
// backed our VkImage with an MTLTexture, so there is nothing to import,
// negotiate a format for, or keep in step — we ask for the one that exists.
//
// **That asymmetry is the whole reason there is no provider call here.** An
// import (`VkImportMetalTextureInfoEXT`, or the deprecated `vkSetMTLTextureMVK`)
// would let the host choose the storage, and buys nothing: this eye image is
// never a drawable, the compositor only ever reads it, and a host-chosen
// texture would have to agree with the VkFormat exactly — the same
// "Incompatible format" class kl_glfb.h records for ANGLE, in an API where the
// failure is silent instead.
//
// Two entry points can answer, and they are tried in that order:
//
//   vkExportMetalObjectsEXT   VK_EXT_metal_objects, the standard one. Needs the
//                             extension ENABLED on the device, which is why
//                             klvk_CreateDevice adds it.
//   vkGetMTLTextureMVK        MoltenVK's own, deprecated but a plain dylib
//                             export that needs no extension — the fallback for
//                             a device created before this existed.
//
// Failure is not fatal anywhere: the capture path reads the VkImage directly
// and does not need this at all. A run with no compositor (every host run today)
// simply publishes a texture nobody samples.
static void *eye_mtl_texture(klvk_device *d, VkImage img) {
    if (!d || !img) return NULL;

    // vkGetMTLTextureMVK's own prototype lives in MoltenVK/mvk_deprecated_api.h,
    // which is an Objective-C header (`id<MTLTexture> *`) and cannot be included
    // from this plain-C file. The ABI is a pointer either way, so it is declared
    // here rather than dragging the translation unit into ObjC for one symbol.
    typedef void (*pfn_get_mtl_texture_mvk)(VkImage, void **);

    static PFN_vkExportMetalObjectsEXT export_ext;
    static pfn_get_mtl_texture_mvk     get_mvk;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        export_ext = (PFN_vkExportMetalObjectsEXT)
            (real_gdpa ? real_gdpa(d->dev, "vkExportMetalObjectsEXT") : NULL);
        get_mvk = (pfn_get_mtl_texture_mvk)mvk_sym("vkGetMTLTextureMVK");
        if (!export_ext && !get_mvk)
            VKI("no way to reach the MTLTexture behind an eye image — neither "
                "vkExportMetalObjectsEXT nor vkGetMTLTextureMVK resolves; the "
                "compositor will have nothing to sample (the capture is "
                "unaffected)\n");
    }

    if (export_ext) {
        VkExportMetalTextureInfoEXT tex = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
        .image = img,
            .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
        };
        VkExportMetalObjectsInfoEXT info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
            .pNext = &tex,
        };
        export_ext(d->dev, &info);
        if (tex.mtlTexture) return (void *)tex.mtlTexture;
    }
    if (get_mvk) {
        void *t = NULL;
        get_mvk(img, &t);
        return t;
    }
    return NULL;
}

// Tell kl_glfb which MTLTexture is which eye, so every compositor and readback
// finds the Vulkan eyes through the same accessor it already uses for the GL
// ones. Under the Array layout one texture serves both eyes and the eye is the
// SLICE, which is exactly what that table's `slice` field was added for.
static void eye_publish_mtl(int stage, int eye, int layers) {
    klvk_device *d = g_devs[0];
    VkImage img = g_eye[stage][eye].img;
    void *tex = eye_mtl_texture(d, img);
    if (!tex) return;
    int w = (int)g_eye[stage][eye].w, h = (int)g_eye[stage][eye].h;
    if (layers > 1) {
        kl_glfb_note_eye_mtl_texture(0, stage, tex, 0, w, h);
        kl_glfb_note_eye_mtl_texture(1, stage, tex, 1, w, h);
    } else {
        kl_glfb_note_eye_mtl_texture(eye, stage, tex, 0, w, h);
    }
}

// A registry of every VkImage this process actually created - the guest's via
// the vkCreateImage wrapper, ours via klvk_image_alloc, and the WSI swapchain's.
// The image-view wrapper consults it to refuse a handle no create ever produced:
// AC Nexus feeds vkCreateImageView a deterministic wild pointer (it lands in
// MoltenVK's own read-only data), and MoltenVK's MVKImageView constructor
// dereferences it and takes SIGBUS instead of returning an error. Refusing turns
// that into a clean failure. Locked - guest worker threads create both at once -
// and FAIL-OPEN if it ever fills, so a legitimate view is never wrongly refused.
#define KLVK_IMG_REG_MAX 16384
// An open-addressing set rather than a list: klvk_img_reg_has runs on every
// barrier and blit, and a linear scan of thousands of images under the lock
// was the layer's largest self cost in an Alyx profile (1.25% of samples).
#define KLVK_IMG_REG_CAP 32768                       // power of two, 2x MAX
#define KLVK_IMG_TOMB ((VkImage)(uintptr_t)1)
static VkImage g_img_set[KLVK_IMG_REG_CAP];
static int g_img_reg_n, g_img_tomb, g_img_full;
static pthread_mutex_t g_img_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned klvk_img_hash_slot(VkImage img) {
    uint64_t h = (uint64_t)(uintptr_t)img * 0x9E3779B97F4A7C15ull;
    return (unsigned)(h >> 49) & (KLVK_IMG_REG_CAP - 1);
}
static void klvk_img_rehash(void) {                  // lock held
    static VkImage old[KLVK_IMG_REG_CAP];
    memcpy(old, g_img_set, sizeof old);
    memset(g_img_set, 0, sizeof g_img_set);
    g_img_tomb = 0;
    for (int i = 0; i < KLVK_IMG_REG_CAP; i++) {
        if (!old[i] || old[i] == KLVK_IMG_TOMB) continue;
        unsigned j = klvk_img_hash_slot(old[i]);
        while (g_img_set[j]) j = (j + 1) & (KLVK_IMG_REG_CAP - 1);
        g_img_set[j] = old[i];
    }
}
static void klvk_img_reg_add(VkImage img) {
    if (!img || img == KLVK_IMG_TOMB) return;
    pthread_mutex_lock(&g_img_reg_lock);
    if (g_img_reg_n >= KLVK_IMG_REG_MAX) { g_img_full = 1; pthread_mutex_unlock(&g_img_reg_lock); return; }
    if (g_img_reg_n + g_img_tomb >= KLVK_IMG_REG_CAP * 3 / 4) klvk_img_rehash();
    unsigned j = klvk_img_hash_slot(img);
    int tomb = -1;
    for (;;) {
        VkImage v = g_img_set[j];
        if (v == img) break;                                   // already there
        if (v == KLVK_IMG_TOMB && tomb < 0) tomb = (int)j;
        if (!v) {
            if (tomb >= 0) { j = (unsigned)tomb; g_img_tomb--; }
            g_img_set[j] = img;
            g_img_reg_n++;
            break;
        }
        j = (j + 1) & (KLVK_IMG_REG_CAP - 1);
    }
    pthread_mutex_unlock(&g_img_reg_lock);
}
static void klvk_img_reg_del(VkImage img) {
    if (!img || img == KLVK_IMG_TOMB) return;
    pthread_mutex_lock(&g_img_reg_lock);
    for (unsigned j = klvk_img_hash_slot(img); g_img_set[j]; j = (j + 1) & (KLVK_IMG_REG_CAP - 1))
        if (g_img_set[j] == img) { g_img_set[j] = KLVK_IMG_TOMB; g_img_tomb++; g_img_reg_n--; break; }
    pthread_mutex_unlock(&g_img_reg_lock);
}

// ---------------------------------------------------------------------------
// KL_TRACE: one-build render dataflow tracer. Gated so it costs nothing off.
// ---------------------------------------------------------------------------
static int klvk_trace(void) { static int v=-1; if (v<0) v=kl_env_on("KL_TRACE",0); return v; }
static unsigned klvk_tid(void) { uint64_t t=0; pthread_threadid_np(NULL,&t); return (unsigned)t; }
#define klvk_trace_log(...) fprintf(stderr, "[TRACE] " __VA_ARGS__)
// Return 1 the first time a given (a,b) pair is seen, so per-frame-repeated
// render passes and copies each log ONCE instead of flooding the trace.
static int klvk_trace_once(unsigned long long a, unsigned long long b) {
    static struct { unsigned long long a, b; } seen[2048]; static int n;
    static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    int fresh = 1;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < n; i++) if (seen[i].a==a && seen[i].b==b) { fresh = 0; break; }
    if (fresh && n < 2048) { seen[n].a=a; seen[n].b=b; n++; }
    pthread_mutex_unlock(&lk);
    return fresh;
}
// Label an image: is it an eye layer, a menu overlay panel (returns its key), or
// a plain tracked/wild image.
static int klvk_is_eye_image(VkImage img) {
    if (!img) return 0;
    for (int st=0; st<KLVK_EYE_STAGES; st++) for (int e=0;e<2;e++)
        if (g_eye[st][e].img==img) return 1;
    return 0;
}
static int klvk_layer_image_key(VkImage img) {
    if (!img) return 0;
    for (int i=0;i<KLVK_MAX_LAYERS;i++) { if (!g_layer_img[i].key) continue;
        for (int st=0; st<KLVK_EYE_STAGES; st++) for (int e=0;e<2;e++)
            if (g_layer_img[i].s[st][e].img==img) return g_layer_img[i].key; }
    return 0;
}
// view -> image, so a framebuffer's attachment views resolve to their images.
#define KLVK_VIEW_MAP_MAX 8192
static struct { VkImageView view; VkImage img; } g_view_map[KLVK_VIEW_MAP_MAX];
static int g_view_map_n;
static pthread_mutex_t g_view_map_lock = PTHREAD_MUTEX_INITIALIZER;
static void klvk_view_map_add(VkImageView v, VkImage img) {
    if (!v) return;
    pthread_mutex_lock(&g_view_map_lock);
    if (g_view_map_n<KLVK_VIEW_MAP_MAX){ g_view_map[g_view_map_n].view=v; g_view_map[g_view_map_n].img=img; g_view_map_n++; }
    pthread_mutex_unlock(&g_view_map_lock);
}
static VkImage klvk_view_image(VkImageView v) {
    VkImage img=VK_NULL_HANDLE;
    pthread_mutex_lock(&g_view_map_lock);
    for (int i=g_view_map_n-1;i>=0;i--) if (g_view_map[i].view==v){ img=g_view_map[i].img; break; }
    pthread_mutex_unlock(&g_view_map_lock);
    return img;
}
// compact label for a log line
static const char *klvk_img_tag(VkImage img) {
    if (!img) return "nil";
    if (klvk_is_eye_image(img)) return "EYE";
    if (klvk_layer_image_key(img)) return "panel";
    if (!klvk_img_reg_has(img)) return "WILD";
    return "trk";
}
static int klvk_img_reg_has(VkImage img) {
    if (!img || img == KLVK_IMG_TOMB) return 0;
    int found = 0;
    pthread_mutex_lock(&g_img_reg_lock);
    if (g_img_full) found = 1;                        /* full -> fail open */
    else for (unsigned j = klvk_img_hash_slot(img); g_img_set[j]; j = (j + 1) & (KLVK_IMG_REG_CAP - 1))
        if (g_img_set[j] == img) { found = 1; break; }
    pthread_mutex_unlock(&g_img_reg_lock);
    return found;
}

// Adopt-on-use. A handle the guest presents that no create of ours logged is,
// in AC Nexus, a real Unity render target created through a Vulkan entry our
// dispatch did not intercept - NOT the wild overlay pointer the strict guard
// was built for (that source was removed upstream by controlling
// EnqueueSetupLayer2). Refusing such a handle makes MoltenVK reject Unity's
// render target, and Unity then renders nothing at all: it logs "temporary
// render texture not found" and every eye capture comes back 0-lit black. So
// The default is STRICT REFUSE. Adopting was tried and PROVEN unsafe: AC Nexus
// hands vkCreateImageView a handle (e.g. 0x..59d0, format 37 mips 0+2) right
// after it sets up its 2x2 Equirect placeholder layer - Unity's bogus native
// texture pointer for the loading skybox - and it points into a read-only
// mapping, so MVKImageView's constructor takes SIGBUS (signal 10) dereferencing
// it. Refusing turns that crash into a clean failure. KL_VK_IMG_ADOPT=1 opts
// back into adopting for a deliberate experiment, but it is known to crash on
// this title - do not enable it by default.
static int klvk_img_adopt_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("KL_VK_IMG_ADOPT");
        m = (e && e[0] == '1') ? 1 : 0;   /* default OFF - adopting SIGBUSes */
    }
    return m;
}
static int klvk_img_reg_ok(VkImage img) {
    if (!img) return 1;
    if (klvk_img_reg_has(img)) return 1;
    if (!klvk_img_adopt_mode()) return 0;
    klvk_img_reg_add(img);
    static int adopted;
    if (adopted < 64) {
        adopted++;
        VKI("vkImage %#llx ADOPTED - no create of ours logged it, but the guest "
            "holds it as a real resource; letting it through "
            "(KL_VK_IMG_ADOPT=0 refuses instead)\n",
            (unsigned long long)(uintptr_t)img);
    }
    return 1;
}

// Create an image and back it with device-local memory.
//
// Shared by the two callers that need one — the OVRPlugin eye images below and
// the OpenXR swapchain images (kl_vulkan_xr_image) — because the sequence is
// identical and only the description differs. Written out twice it would be two
// copies of the memory-type fallback, which is exactly the kind of thing that
// drifts and then differs on the path nobody ran.
static VkImage klvk_image_alloc(klvk_device *d, VkFormat fmt,
                                unsigned w, unsigned h, uint32_t layers,
                                uint32_t mips, VkImageUsageFlags usage,
                                VkDeviceMemory *mem_out) {
    if (mem_out) *mem_out = VK_NULL_HANDLE;
    VkImageCreateInfo ic = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { w, h, 1 },
        .mipLevels = mips ? mips : 1,
        .arrayLayers = layers ? layers : 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    // Unity's Vulkan renderer creates an alternate-format VIEW of a colour eye
    // texture (the sRGB<->UNORM pair), which is undefined unless the image was
    // told its format may vary. Without this AC Nexus faults inside MoltenVK's
    // MVKImageView constructor building that view; a same-format view (BONELAB)
    // is unaffected, so this only ever ADDS a capability.
    if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        ic.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImage img = VK_NULL_HANDLE;
    if (d->CreateImage(d->dev, &ic, NULL, &img) != VK_SUCCESS) {
        VKI("image %ux%u (%u layer(s)): vkCreateImage failed\n", w, h, layers);
        return VK_NULL_HANDLE;
    }
    VkMemoryRequirements mr;
    d->GetImageMemoryRequirements(d->dev, img, &mr);
    uint32_t mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = mem_type(d, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
    };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (d->AllocateMemory(d->dev, &ai, NULL, &mem) != VK_SUCCESS ||
        d->BindImageMemory(d->dev, img, mem, 0) != VK_SUCCESS) {
        VKI("image %ux%u: memory failed\n", w, h);
        d->DestroyImage(d->dev, img, NULL);
        return VK_NULL_HANDLE;
    }
    if (mem_out) *mem_out = mem;
    klvk_img_reg_add(img);
    return img;
}

// A non-eye layer's slot table, found or claimed by the guest's layer id.
// NULL when the table is full, which is named by the caller.
static klvk_img_slot *layer_slots_for(int key) {
    for (int i = 0; i < KLVK_MAX_LAYERS; i++)
        if (g_layer_img[i].key == key) return &g_layer_img[i].s[0][0];
    for (int i = 0; i < KLVK_MAX_LAYERS; i++)
        if (!g_layer_img[i].key) {
            g_layer_img[i].key = key;
            g_layer_img[i].seq = ++g_layer_img_seq;
            return &g_layer_img[i].s[0][0];
        }
    // Full: recycle the OLDEST-claimed slot rather than refusing. AC Nexus
    // re-creates its overlays across loading phases with a fresh layer id each
    // time and never frees the old ones, so a fixed table fills and
    // GetLayerTexture2 then failed for the rest - which is what kept the render
    // loop from ever finishing a frame. Evicting the earliest-claimed slot
    // keeps the newest (currently-visible) layers. The old slot's images are
    // left allocated (a compositor may sample them this instant) and are
    // resized/reused by kl_vulkan_layer_image for the new key.
    int oldest = 0;
    for (int i = 1; i < KLVK_MAX_LAYERS; i++)
        if (g_layer_img[i].seq < g_layer_img[oldest].seq) oldest = i;
    static unsigned evicted;
    if (evicted++ % 32 == 0)
        VKI("layer image table full, recycling oldest slot (key %d -> %d, %u so far)\n",
            g_layer_img[oldest].key, key, evicted);
    g_layer_img[oldest].key = key;
    g_layer_img[oldest].seq = ++g_layer_img_seq;
    return &g_layer_img[oldest].s[0][0];
}

// See kl_vulkan.h. The rename, not a copy: the table entry keeps its images
// and its age, only the key changes — so a compositor already sampling the
// MTLTexture behind it never sees the storage move.
void kl_vulkan_layer_rekey(int old_key, int new_key) {
    if (old_key == new_key || old_key == KLVK_EYE_LAYER || new_key == KLVK_EYE_LAYER)
        return;
    for (int i = 0; i < KLVK_MAX_LAYERS; i++) {
        if (g_layer_img[i].key != old_key) continue;
        g_layer_img[i].key = new_key;
        g_layer_img[i].seq = ++g_layer_img_seq;    // it is the NEWEST layer now
        static unsigned n;
        if (n++ % 32 == 0)
            VKI("layer %d re-keyed to %d — the recreated layer keeps its images "
                "(%u rekeys so far)\n", old_key, new_key, n);
        return;
    }
}

// Retired eye/layer images, destroyed once KLVK_RETIRE_FRAMES frames have been
// published since (klvk_retire_sweep, from kl_vulkan_frame_done).
#define KLVK_RETIRE_FRAMES 120
static uint64_t g_frame_serial;
static struct { klvk_device *d; VkImage img; VkDeviceMemory mem; VkBuffer buf; void *tex; uint64_t at; } g_retired[64];
static pthread_mutex_t g_retired_mu = PTHREAD_MUTEX_INITIALIZER;
static void klvk_retire_any(klvk_device *d, VkImage img, VkBuffer buf, VkDeviceMemory mem, void *tex) {
    if (!d || (!img && !buf && !tex)) return;
    pthread_mutex_lock(&g_retired_mu);
    int i = 0;
    while (i < 64 && (g_retired[i].img || g_retired[i].buf || g_retired[i].tex)) i++;
    if (i < 64) {
        g_retired[i].d = d; g_retired[i].img = img; g_retired[i].buf = buf; g_retired[i].mem = mem; g_retired[i].tex = tex;
        g_retired[i].at = __atomic_load_n(&g_frame_serial, __ATOMIC_ACQUIRE);
    } else {
        static int said;
        if (!said++) VKI("retired-image list full: an old eye image is kept\n");
    }
    pthread_mutex_unlock(&g_retired_mu);
}
static void klvk_retire(klvk_device *d, VkImage img, VkDeviceMemory mem) { klvk_retire_any(d, img, VK_NULL_HANDLE, mem, NULL); }
// A depth staging buffer and the texture view over it (KL_CP_DEPTH): the compositor may be sampling it this instant.
static void klvk_retire_buffer(klvk_device *d, VkBuffer buf, VkDeviceMemory mem, void *tex) {
    klvk_retire_any(d, VK_NULL_HANDLE, buf, mem, tex);
}
static void klvk_retire_sweep(void) {
    uint64_t now = __atomic_load_n(&g_frame_serial, __ATOMIC_ACQUIRE);
    static unsigned freed;
    pthread_mutex_lock(&g_retired_mu);
    for (int i = 0; i < 64; i++) {
        if ((!g_retired[i].img && !g_retired[i].buf && !g_retired[i].tex) || now < g_retired[i].at + KLVK_RETIRE_FRAMES) continue;
        klvk_device *d = g_retired[i].d;
        if (g_retired[i].tex) kl_mtl_release(g_retired[i].tex);
        if (g_retired[i].buf && d->DestroyBuffer) d->DestroyBuffer(d->dev, g_retired[i].buf, NULL);
        if (g_retired[i].img && d->DestroyImage) d->DestroyImage(d->dev, g_retired[i].img, NULL);
        if (g_retired[i].mem && d->FreeMemory) d->FreeMemory(d->dev, g_retired[i].mem, NULL);
        memset(&g_retired[i], 0, sizeof g_retired[i]);
        if (freed++ % 24 == 0) VKI("freed retired eye images: %u so far\n", freed);
    }
    pthread_mutex_unlock(&g_retired_mu);
}

unsigned long long kl_vulkan_layer_image(int layer_key, int stage, int eye,
                                         unsigned w, unsigned h, int srgb, int layers) {
    if (!kl_vulkan_guest_active()) return 0;
    if (stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1) return 0;
    if (!w || !h) return 0;
    if (layers < 1) layers = 1;
    if (layers > 2) layers = 2;
    // Under the Array layout there is one image for the stage and both eyes get
    // it; slot 0 owns it, so the eye argument selects a LAYER later and not a
    // second allocation here.
    if (layers > 1) eye = 0;
    klvk_device *d = g_devs[0];

    int is_eye = layer_key == KLVK_EYE_LAYER;
    klvk_img_slot *slots = is_eye ? &g_eye[0][0] : layer_slots_for(layer_key);
    if (!slots) {
        VKI("layer %d: all %d layer image slots are taken — refusing rather than "
            "handing back another layer's storage\n", layer_key, KLVK_MAX_LAYERS);
        return 0;
    }
    klvk_img_slot *slot = is_eye ? &g_eye[stage][eye] : &slots[stage * 2 + eye];

    VkFormat fmt = layer_key == KLVK_DEPTH_LAYER ? VK_FORMAT_R32_SFLOAT
                 : srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    // A cached image is only the right answer if it is the image that was
    // ASKED for. RE4 sets up its eye layer twice — 2290x2400, then 2748x2880
    // once UE4 applies its own pixel density — and the size was not consulted,
    // so the second layer was handed the first one's storage and the engine
    // rendered a 2748x2880 frame into a 2290x2400 image.
    //
    // The old image is not destroyed at once: a compositor or a capture may be
    // sampling it on another thread this instant. It used to be kept for good,
    // on the grounds that this happened once per stage at startup; with Source 2's
    // auto-fidelity changing the eye size every few seconds it leaked 6 images per
    // change (1.36 GB in 70 s on the headset on 2026-09-29, until the memory limit
    // ended the game). Now it is freed KLVK_RETIRE_FRAMES frames later (klvk_retire).
    if (slot->img && (slot->w != w || slot->h != h ||
                      slot->layers != (uint32_t)layers || slot->fmt != fmt)) {
        static unsigned said;
        if (said++ < 12)
            VKI("layer %d stage %d %s: was %ux%u (%u layer(s)), now %ux%u (%d) — "
                "allocating fresh; the old image is freed %d frames from now\n",
                layer_key, stage, eye ? "eye 1" : "eye 0",
                slot->w, slot->h, slot->layers, w, h, layers, KLVK_RETIRE_FRAMES);
        klvk_retire(d, slot->img, slot->mem);
        slot->img = VK_NULL_HANDLE;
        slot->mem = VK_NULL_HANDLE;
    }
    if (slot->img) return (uint64_t)(uintptr_t)slot->img;

    // The guest renders into it, samples it for its own post chain, and we copy
    // out of it. TRANSFER_DST is there because Unity clears some targets with
    // vkCmdClearColorImage rather than a load op.
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage img = klvk_image_alloc(d, fmt, w, h, (uint32_t)layers, 1,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT, &mem);
    if (img == VK_NULL_HANDLE) return 0;
    // KL_VK_EYE_TINT=1 pre-clears the image to a colour nothing in a rendered
    // scene produces, which is the A/B that separates the two ways an eye
    // texture can come back wrong. They are indistinguishable in the capture
    // otherwise: if the tint survives to the PNG, the guest never drew into
    // this image at all; if it does not, the guest drew and the content is
    // genuinely what it rendered. Off by default — it costs a submit per image
    // and it is a diagnostic, not a fix.
    if (kl_env_on("KL_VK_EYE_TINT", 0) && d->CmdClearColorImage &&
        cap_prepare(d, 4)) {
        VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        d->ResetCommandBuffer(d->cap_cmd, 0);
        d->BeginCommandBuffer(d->cap_cmd, &bi);
        VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
            // EVERY layer: under the Array layout the two eyes are layers of
            // this one image, and tinting only layer 0 would leave the right
            // eye's "did the guest draw?" question unanswered.
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, (uint32_t)layers },
        };
        d->CmdPipelineBarrier(d->cap_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        // A DIFFERENT colour per array layer, and that is the second question
        // this diagnostic answers. Under the Array layout the two eyes are two
        // slices of one image, and "the guest drew" and "the capture reads the
        // right slice" are separate failures that look identical in the files:
        // a capture that reads layer 0 twice produces two byte-identical PNGs,
        // which is exactly what a guest rendering only the left eye produces
        // too. Green for eye 0, blue for eye 1 — so a run says which.
        static const VkClearColorValue TINT[2] = {
            { .float32 = { 0.0f, 1.0f, 0.0f, 1.0f } },   // eye 0: green
            { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } },   // eye 1: blue
        };
        for (int ly = 0; ly < layers; ly++) {
            VkImageSubresourceRange rng =
                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, (uint32_t)ly, 1 };
            d->CmdClearColorImage(d->cap_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  &TINT[layers > 1 ? ly : (eye & 1)], 1, &rng);
        }
        d->EndCommandBuffer(d->cap_cmd);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .commandBufferCount = 1, .pCommandBuffers = &d->cap_cmd };
        d->ResetFences(d->dev, 1, &d->cap_fence);
        if (d->QueueSubmit(d->queue, 1, &si, d->cap_fence) == VK_SUCCESS)
            d->WaitForFences(d->dev, 1, &d->cap_fence, VK_TRUE, 2000000000ull);
        VKI("eye image stage %d pre-cleared: %s (KL_VK_EYE_TINT)\n", stage,
            layers > 1 ? "layer 0 GREEN, layer 1 BLUE"
                       : (eye ? "eye 1 BLUE" : "eye 0 GREEN"));
    }

    slot->img = img;
    slot->mem = mem;
    slot->w = w;
    slot->h = h;
    slot->layers = (uint32_t)layers;
    slot->fmt = fmt;
    // Only the EYE layer is published into kl_glfb's eye table. That table is
    // what every compositor and every readback samples, so a splash quad
    // announcing itself there would replace the eye on screen with a logo.
    if (is_eye) eye_publish_mtl(stage, eye, layers);
    VKI("%s stage %d %s = VkImage %p (%ux%u %s)\n",
        is_eye ? "eye image" : "layer image",
        stage,
        layers > 1 ? "both eyes (2 array layers)" :
                     (eye ? "eye 1" : "eye 0"),
        (void *)img, w, h, srgb ? "R8G8B8A8_SRGB" : "R8G8B8A8_UNORM");
    return (uint64_t)(uintptr_t)img;
}

unsigned long long kl_vulkan_eye_image_layers(int stage, int eye, unsigned w, unsigned h,
                                              int srgb, int layers) {
    return kl_vulkan_layer_image(KLVK_EYE_LAYER, stage, eye, w, h, srgb, layers);
}

void *kl_vulkan_layer_mtl_texture(int layer_key, int stage, int eye,
                                  int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!kl_vulkan_guest_active()) return NULL;
    if (stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1) return NULL;
    // Never the eye table: those go to a compositor through kl_glfb's eye table,
    // which is where every consumer already looks. This is only the OTHER
    // layers, which have no such table because nothing sampled them before.
    if (layer_key == KLVK_EYE_LAYER) return NULL;
    klvk_img_slot *slots = NULL;
    for (int i = 0; i < KLVK_MAX_LAYERS; i++)
        if (g_layer_img[i].key == layer_key) { slots = &g_layer_img[i].s[0][0]; break; }
    if (!slots) return NULL;
    klvk_img_slot *slot = &slots[stage * 2 + eye];
    // A layer allocated for eye 0 only (the common case: one texture serving
    // both eyes, with the per-eye ViewportRect selecting the part) answers eye 1
    // from slot 0 rather than with nothing.
    if (!slot->img && eye == 1) slot = &slots[stage * 2];
    if (!slot->img) return NULL;
    if (w) *w = (int)slot->w;
    if (h) *h = (int)slot->h;
    return eye_mtl_texture(g_devs[0], slot->img);
}

// ---- The game's depth for the compositor (KL_CP_DEPTH; see kl_vulkan.h) ----
static struct { VkBuffer buf; VkDeviceMemory mem; unsigned long long size; void *mapped; void *tex; unsigned tw, th, tpitch; } g_dstage[KLVK_EYE_STAGES][2];
static struct { float m22, m23; unsigned w, h, pitch; int valid; } g_dmap[KLVK_EYE_STAGES][2];
static pthread_mutex_t g_dstage_mu = PTHREAD_MUTEX_INITIALIZER;   // the game's thread grows, the compositor's makes views

// KL_CP_DEPTH at half resolution: one D32S8 image per eye (not per stage: it only lives between the blit that fills it
// and the copy that empties it, both in the same submit), the blit target for the game's depth before it goes to the
// half-size staging. A new size retires the old image KLVK_RETIRE_FRAMES later.
unsigned long long kl_vulkan_eye_depth_half(int eye, unsigned w, unsigned h) {
    static struct { VkImage img; VkDeviceMemory mem; unsigned w, h; } half[2];
    if (eye < 0 || eye > 1 || !w || !h || !g_devs[0]) return 0;
    klvk_device *d = g_devs[0];
    if (half[eye].img && (half[eye].w != w || half[eye].h != h)) {
        klvk_retire(d, half[eye].img, half[eye].mem);
        half[eye].img = VK_NULL_HANDLE; half[eye].mem = VK_NULL_HANDLE;
    }
    if (!half[eye].img) {
        half[eye].img = klvk_image_alloc(d, VK_FORMAT_D32_SFLOAT_S8_UINT, w, h, 1, 1,
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &half[eye].mem);
        half[eye].w = w; half[eye].h = h;
        if (half[eye].img) VKI("eye %d half-resolution depth image %ux%u (KL_CP_DEPTH_HALF)\n", eye, w, h);
    }
    return (uint64_t)(uintptr_t)half[eye].img;
}
unsigned long long kl_vulkan_eye_depth_image(int stage, int eye, unsigned w, unsigned h) {
    return kl_vulkan_layer_image(KLVK_DEPTH_LAYER, stage, eye, w, h, 0, 1);
}

static uint64_t klvk_depth_staging_locked(klvk_device *d, void *stv, int stage, int eye, unsigned long long bytes, void **mapped);
unsigned long long kl_vulkan_eye_depth_staging(int stage, int eye, unsigned long long bytes, void **mapped) {
    if (mapped) *mapped = NULL;
    if (!kl_vulkan_guest_active() || stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1 || !bytes) return 0;
    klvk_device *d = g_devs[0];
    typeof(g_dstage[0][0]) *st = &g_dstage[stage][eye];
    pthread_mutex_lock(&g_dstage_mu);
    uint64_t r = klvk_depth_staging_locked(d, st, stage, eye, bytes, mapped);
    pthread_mutex_unlock(&g_dstage_mu);
    return r;
}
static uint64_t klvk_depth_staging_locked(klvk_device *d, void *stv, int stage, int eye, unsigned long long bytes, void **mapped) {
    typeof(g_dstage[0][0]) *st = stv;
    if (st->buf && st->size >= bytes) { if (mapped) *mapped = st->mapped; return (uint64_t)(uintptr_t)st->buf; }
    if (st->buf) {                     // grown: the old buffer (and its view) go the retired images' way, later
        klvk_retire_buffer(d, st->buf, st->mem, st->tex);
        memset(st, 0, sizeof *st);
    }
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if (d->CreateBuffer(d->dev, &bi, NULL, &st->buf) != VK_SUCCESS) { st->buf = VK_NULL_HANDLE; return 0; }
    VkMemoryRequirements mr;
    d->GetBufferMemoryRequirements(d->dev, st->buf, &mr);
    // Host-visible: unified memory on Apple silicon, so it costs nothing, and it lets the Mac read depth back.
    uint32_t mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) mt = mem_type(d, mr.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    if (d->AllocateMemory(d->dev, &ai, NULL, &st->mem) != VK_SUCCESS ||
        d->BindBufferMemory(d->dev, st->buf, st->mem, 0) != VK_SUCCESS) {
        d->DestroyBuffer(d->dev, st->buf, NULL); memset(st, 0, sizeof *st); return 0;
    }
    st->size = bytes;
    if (d->MapMemory(d->dev, st->mem, 0, VK_WHOLE_SIZE, 0, &st->mapped) != VK_SUCCESS) st->mapped = NULL;
    if (mapped) *mapped = st->mapped;
    VKI("depth staging stage %d eye %d: %llu bytes%s\n", stage, eye, bytes, st->mapped ? " (host-visible)" : "");
    return (uint64_t)(uintptr_t)st->buf;
}

void kl_vulkan_set_eye_depth_map(int stage, int eye, float m22, float m23, unsigned w, unsigned h, unsigned pitch) {
    if (stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1) return;
    g_dmap[stage][eye].m22 = m22; g_dmap[stage][eye].m23 = m23;
    g_dmap[stage][eye].w = w; g_dmap[stage][eye].h = h; g_dmap[stage][eye].pitch = pitch;
    __atomic_store_n(&g_dmap[stage][eye].valid, 1, __ATOMIC_RELEASE);
}

void *kl_vulkan_eye_depth_mtl(int stage, int eye, int *w, int *h, float *m22, float *m23) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (stage < 0 || stage >= KLVK_EYE_STAGES || eye < 0 || eye > 1) return NULL;
    if (!__atomic_load_n(&g_dmap[stage][eye].valid, __ATOMIC_ACQUIRE)) return NULL;
    if (m22) *m22 = g_dmap[stage][eye].m22;
    if (m23) *m23 = g_dmap[stage][eye].m23;
    // pitch 0: the copy went on into the R32F layer image (KL_CP_DEPTH_IMAGE=1); otherwise sample the staging
    // buffer where the depth copy left it, through a texture view made once per buffer and size.
    unsigned dw = g_dmap[stage][eye].w, dh = g_dmap[stage][eye].h, dp = g_dmap[stage][eye].pitch;
    if (!dp) return kl_vulkan_layer_mtl_texture(KLVK_DEPTH_LAYER, stage, eye, w, h);
    typeof(g_dstage[0][0]) *st = &g_dstage[stage][eye];
    pthread_mutex_lock(&g_dstage_mu);
    if (!st->buf) { pthread_mutex_unlock(&g_dstage_mu); return NULL; }
    if (st->tex && (st->tw != dw || st->th != dh || st->tpitch != dp)) {
        klvk_retire_buffer(g_devs[0], VK_NULL_HANDLE, VK_NULL_HANDLE, st->tex);   // the view alone; see klvk_retire_any
        st->tex = NULL;
    }
    if (!st->tex) {
        typedef void (*pfn_get_mtl_buffer_mvk)(VkBuffer, void **);
        static pfn_get_mtl_buffer_mvk get_buf;
        if (!get_buf) get_buf = (pfn_get_mtl_buffer_mvk)mvk_sym("vkGetMTLBufferMVK");
        void *mb = NULL;
        if (get_buf) get_buf(st->buf, &mb);
        st->tex = mb ? kl_mtl_buffer_texture_r32f(mb, dw, dh, dp * 4) : NULL;
        st->tw = dw; st->th = dh; st->tpitch = dp;
        static int said;
        if (said++ < 4) VKI("depth view stage %d eye %d: %ux%u pitch %u over the staging buffer: %s\n",
                            stage, eye, dw, dh, dp, st->tex ? "ok" : "FAILED");
    }
    if (w) *w = (int)dw;
    if (h) *h = (int)dh;
    void *t = st->tex;
    pthread_mutex_unlock(&g_dstage_mu);
    return t;
}

// Diagnostic/bridge: the MTLTexture behind the wild-handle scratch — the single
// image AC Nexus actually composites its frame into (its real render target
// allocation fails, so it falls back to a wild pointer we substitute with this
// scratch, and the copies/blits/draws all land here). The compositor can draw
// this to SEE what the guest produced, and ultimately to present it.
static void *klvk_overlay_mtl_texture(VkImage img);
void *kl_vulkan_wild_scratch_mtl(int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!kl_vulkan_guest_active()) return NULL;
    // KL_RT_DEBUG=1: show the guest's latest large UI render target instead of the
    // (empty) composite scratch — the menu pieces the guest actually drew.
    if (kl_env_on("KL_RT_DEBUG", 0) && g_content_rt && klvk_img_reg_has(g_content_rt)
        && !klvk_is_eye_image(g_content_rt)) {
        void *t = klvk_overlay_mtl_texture(g_content_rt);
        if (t) { if (w) *w = (int)g_content_rt_w; if (h) *h = (int)g_content_rt_h; return t; }
    }
    if (!g_wild_scratch) return NULL;
    if (w) *w = 4096;
    if (h) *h = 4096;
    return eye_mtl_texture(g_devs[0], g_wild_scratch);
}

// Resolve the MTLTexture behind a live guest VkImage using ONLY vkGetMTLTextureMVK.
// The eye path (eye_mtl_texture) may use vkExportMetalObjectsEXT first, but that
// faults on any image the guest never marked exportable - which is every one of
// these overlay textures - so it is deliberately not used here (that was the first
// texL-resolver crash).
static void *klvk_overlay_mtl_texture(VkImage img) {
    typedef void (*pfn_get_mtl_texture_mvk)(VkImage, void **);
    static pfn_get_mtl_texture_mvk get_mvk;
    static int resolved;
    if (!resolved) { resolved = 1; get_mvk = (pfn_get_mtl_texture_mvk)mvk_sym("vkGetMTLTextureMVK"); }
    if (!get_mvk || !img) return NULL;
    void *t = NULL;
    get_mvk(img, &t);
    return t;
}

// ---------------------------------------------------------------------------
// Overlay SHADOW copies — the fix for "the panel textures are stale by the time
// the compositor samples them".
//
// The guest renders every menu panel and submits on ONE thread (the [thr]
// census: all createRT and 37/38 QueueSubmits share a tid), while the
// compositor samples those same images from ITS thread with no ordering — so a
// panel that had content right after its render reads black by sample time
// (recycled, re-rendered, or mid-write). KL_SYNC_GUEST would serialize the two
// but stalls presentation and the visionOS watchdog SIGKILLs the app.
//
// So synchronize the CONTENT instead of the threads: for each live image the
// overlay resolver samples, keep a same-size shadow image, and after every
// guest vkQueueSubmit append one command buffer (same queue, so Metal's hazard
// tracking orders it after the guest's renders) that copies live -> shadow.
// The compositor then samples the shadow, which always holds the last COMPLETE
// frame and is never written by anyone else. KL_OVERLAY_SHADOW=0 disables.
//
// Image dimensions/format come from a side table filled by klvk_CreateImage —
// the registry alone holds bare handles.
#define KLVK_IMG_INFO_MAX 4096
static struct { VkImage img; VkFormat fmt; uint32_t w, h; } g_img_info[KLVK_IMG_INFO_MAX];
static pthread_mutex_t g_img_info_lock = PTHREAD_MUTEX_INITIALIZER;
static void klvk_img_info_add(VkImage img, VkFormat fmt, uint32_t w, uint32_t h) {
    pthread_mutex_lock(&g_img_info_lock);
    for (int i = 0; i < KLVK_IMG_INFO_MAX; i++)
        if (!g_img_info[i].img || g_img_info[i].img == img) {
            g_img_info[i].img = img; g_img_info[i].fmt = fmt;
            g_img_info[i].w = w; g_img_info[i].h = h; break;
        }
    pthread_mutex_unlock(&g_img_info_lock);
}
static int klvk_img_info_get(VkImage img, VkFormat *fmt, uint32_t *w, uint32_t *h) {
    int ok = 0;
    pthread_mutex_lock(&g_img_info_lock);
    for (int i = 0; i < KLVK_IMG_INFO_MAX; i++)
        if (g_img_info[i].img == img) {
            if (fmt) *fmt = g_img_info[i].fmt;
            if (w) *w = g_img_info[i].w;
            if (h) *h = g_img_info[i].h;
            ok = 1; break;
        }
    pthread_mutex_unlock(&g_img_info_lock);
    return ok;
}
static void klvk_img_info_del(VkImage img) {
    pthread_mutex_lock(&g_img_info_lock);
    for (int i = 0; i < KLVK_IMG_INFO_MAX; i++)
        if (g_img_info[i].img == img) { g_img_info[i].img = VK_NULL_HANDLE; break; }
    pthread_mutex_unlock(&g_img_info_lock);
}

#define KLVK_SHADOW_MAX 64
static struct {
    VkImage  live;      // the guest's image (may die; checked each submit)
    VkImage  shadow;    // ours, same size+format, persistent
    void    *mtl;       // shadow's MTLTexture, cached
    uint32_t w, h;
    VkFormat fmt;
    int      copied;    // a copy has landed at least once -> safe to sample
    int      fromcopy;  // owned by capture-on-copy: the pump must not clobber it
} g_shadow[KLVK_SHADOW_MAX];
static int g_shadow_n;
static pthread_mutex_t g_shadow_lock = PTHREAD_MUTEX_INITIALIZER;

// Called from the overlay resolver (compositor thread): make sure `img` has a
// shadow slot. Creation of the Vulkan objects happens on the SUBMIT thread.
static void klvk_shadow_want(VkImage img) {
    if (!img) return;
    pthread_mutex_lock(&g_shadow_lock);
    for (int i = 0; i < g_shadow_n; i++)
        if (g_shadow[i].live == img) { pthread_mutex_unlock(&g_shadow_lock); return; }
    if (g_shadow_n < KLVK_SHADOW_MAX) {
        g_shadow[g_shadow_n].live = img;
        g_shadow[g_shadow_n].shadow = VK_NULL_HANDLE;
        g_shadow[g_shadow_n].mtl = NULL;
        g_shadow[g_shadow_n].copied = 0;
        g_shadow_n++;
    }
    pthread_mutex_unlock(&g_shadow_lock);
}

// The resolver's read side: the shadow's MTLTexture once a copy has landed.
static void *klvk_shadow_mtl(VkImage live) {
    void *t = NULL;
    pthread_mutex_lock(&g_shadow_lock);
    for (int i = 0; i < g_shadow_n; i++)
        if (g_shadow[i].live == live && g_shadow[i].copied && g_shadow[i].shadow) {
            if (!g_shadow[i].mtl)
                g_shadow[i].mtl = klvk_overlay_mtl_texture(g_shadow[i].shadow);
            t = g_shadow[i].mtl;
            break;
        }
    pthread_mutex_unlock(&g_shadow_lock);
    return t;
}

// A live image died: drop its slot so the submit path never copies from a
// stale handle (the crash the texL resolver already learned to avoid).
static void klvk_shadow_forget(VkImage live) {
    pthread_mutex_lock(&g_shadow_lock);
    for (int i = 0; i < g_shadow_n; i++)
        if (g_shadow[i].live == live) { g_shadow[i].live = VK_NULL_HANDLE; break; }
    pthread_mutex_unlock(&g_shadow_lock);
}

// Find-or-create a shadow for `live` ON THE GUEST THREAD (safe: this thread owns
// the queue). Returns the shadow image, or NULL when dims are unknown or the
// table is full. Used by the capture-on-copy path in klvk_CmdCopyImage.
static VkImage klvk_shadow_for_copy(VkImage live) {
    if (!live) return VK_NULL_HANDLE;
    klvk_device *d = g_devs[0];
    if (!d) return VK_NULL_HANDLE;
    VkImage sh = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_shadow_lock);
    int slot = -1;
    for (int i = 0; i < g_shadow_n; i++)
        if (g_shadow[i].live == live) { slot = i; break; }
    if (slot < 0 && g_shadow_n < KLVK_SHADOW_MAX) {
        slot = g_shadow_n++;
        g_shadow[slot].live = live; g_shadow[slot].shadow = VK_NULL_HANDLE;
        g_shadow[slot].mtl = NULL; g_shadow[slot].copied = 0;
    }
    if (slot >= 0) {
        if (!g_shadow[slot].shadow) {
            VkFormat fmt; uint32_t w, h;
            if (klvk_img_info_get(live, &fmt, &w, &h) && w && h) {
                g_shadow[slot].w = w; g_shadow[slot].h = h; g_shadow[slot].fmt = fmt;
                g_shadow[slot].shadow = klvk_image_alloc(d, fmt, w, h, 1, 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
            }
        }
        sh = g_shadow[slot].shadow;
        if (sh) { g_shadow[slot].copied = 1; g_shadow[slot].fromcopy = 1; }
    }
    pthread_mutex_unlock(&g_shadow_lock);
    return sh;
}

// The submit-side pump, called right after the guest's own vkQueueSubmit on the
// guest's thread. Creates shadow images on demand and records+submits ONE
// command buffer copying every wanted live image into its shadow. Layouts are
// GENERAL both sides: MoltenVK does not act on Vulkan layouts (Metal has none),
// and Metal's hazard tracking orders these copies after the guest's renders on
// the same queue. A private pool+cmdbuf+fence, reused; if the previous copy has
// not retired yet this round is skipped rather than blocked on.
static void klvk_shadow_pump(void) {
    static int on = -1;
    if (on < 0) on = kl_env_on("KL_OVERLAY_SHADOW", 1);
    if (!on) return;
    klvk_device *d = g_devs[0];
    if (!d || !g_shadow_n) return;

    static VkCommandPool   pool;
    static VkCommandBuffer cmd;
    static VkFence         fence;
    static int             pending;
    if (!pool) {
        VkCommandPoolCreateInfo pi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = d->queue_family,
        };
        if (d->CreateCommandPool(d->dev, &pi, NULL, &pool) != VK_SUCCESS) return;
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (d->AllocateCommandBuffers(d->dev, &ai, &cmd) != VK_SUCCESS) return;
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (d->CreateFence(d->dev, &fi, NULL, &fence) != VK_SUCCESS) return;
    }
    if (pending) {
        if (d->WaitForFences(d->dev, 1, &fence, VK_TRUE, 0) != VK_SUCCESS)
            return;                       // previous copy still in flight: skip
        d->ResetFences(d->dev, 1, &fence);
        pending = 0;
    }

    static PFN_vkCmdCopyImage real_copy;
    if (!real_copy) real_copy = (PFN_vkCmdCopyImage)mvk_sym("vkCmdCopyImage");
    if (!real_copy) return;

    // Snapshot the work under the lock; do Vulkan work outside it.
    struct { VkImage live, shadow; uint32_t w, h; int idx; } work[KLVK_SHADOW_MAX];
    int nwork = 0;
    pthread_mutex_lock(&g_shadow_lock);
    for (int i = 0; i < g_shadow_n; i++) {
        VkImage live = g_shadow[i].live;
        if (!live || !klvk_img_reg_has(live)) continue;
        // Capture-on-copy owns this shadow: it updates at the guest's own copy,
        // when content is provably present. A pump copy AFTER submit could land
        // on a recycled (black) live image and clobber the good capture.
        if (g_shadow[i].fromcopy) continue;
        if (!g_shadow[i].shadow) {
            VkFormat fmt; uint32_t w, h;
            if (!klvk_img_info_get(live, &fmt, &w, &h) || !w || !h) continue;
            g_shadow[i].w = w; g_shadow[i].h = h; g_shadow[i].fmt = fmt;
            g_shadow[i].shadow = klvk_image_alloc(d, fmt, w, h, 1, 1,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, NULL);
            if (!g_shadow[i].shadow) { g_shadow[i].live = VK_NULL_HANDLE; continue; }
            static int said; if (said < 12) { said++;
                VKI("shadow: %ux%u fmt %d for live %#llx -> %#llx\n", w, h, (int)fmt,
                    (unsigned long long)(uintptr_t)live,
                    (unsigned long long)(uintptr_t)g_shadow[i].shadow); }
        }
        work[nwork].live = live; work[nwork].shadow = g_shadow[i].shadow;
        work[nwork].w = g_shadow[i].w; work[nwork].h = g_shadow[i].h;
        work[nwork].idx = i; nwork++;
    }
    pthread_mutex_unlock(&g_shadow_lock);
    if (!nwork) return;

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (d->BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return;
    for (int i = 0; i < nwork; i++) {
        VkImageCopy rc = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .extent = { work[i].w, work[i].h, 1 },
        };
        real_copy(cmd, work[i].live, VK_IMAGE_LAYOUT_GENERAL,
                  work[i].shadow, VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
    }
    if (d->EndCommandBuffer(cmd) != VK_SUCCESS) return;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    if (d->QueueSubmit(d->queue, 1, &si, fence) == VK_SUCCESS) {
        pending = 1;
        pthread_mutex_lock(&g_shadow_lock);
        for (int i = 0; i < nwork; i++)
            if (g_shadow[work[i].idx].live == work[i].live)
                g_shadow[work[i].idx].copied = 1;
        pthread_mutex_unlock(&g_shadow_lock);
    }
}

// Overlay texL decoder + resolver. The compositor hands us the guest's per-layer
// texL pointer (stored from EnqueueSubmitLayer2). The tracer established the layout:
// the pointer's FIRST word is the guest VkImage this layer's UI was rendered into
// (it matches the per-frame copy sources every time; [+8]/[+16] carry its format).
// Those images are real, MoltenVK-backed, and tracked here - the menu pixels are
// literally in them - so we hand the compositor that image's MTLTexture instead of
// the empty layer swapchain image it was sampling. KL_OVERLAY_TEXL=0 restores the
// old NULL-return (falls back to the layer image) if this ever misbehaves.
void *kl_vulkan_mtl_for_handle(unsigned long long h) {
    if (!h) return NULL;
    // A handle is only a POINTER to a texture struct on the Vulkan path. On a
    // GLES guest the same compositor ladder hands this the layer's GL texture
    // NAME — a small integer — and dereferencing that read unmapped low memory
    // (ieytd: SIGSEGV at 0x74, i.e. GL tex 116 used as an address). No Vulkan
    // guest, or a value no mapped pointer can have, answers NULL and lets the
    // ladder fall through to kl_glfb_layer_mtl_texture, which owns GL names.
    if (!kl_vulkan_guest_active() || h < 0x10000ULL) return NULL;
    const unsigned long long *w = (const unsigned long long *)(uintptr_t)h;
    VkImage img = (VkImage)(uintptr_t)w[0];

    // Diagnostic struct dump, deduped to once per distinct handle (KL_TRACE only).
    if (klvk_trace()) {
        static unsigned long long seen[128]; static int seen_n;
        static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
        int fresh = 1;
        pthread_mutex_lock(&lk);
        for (int i = 0; i < seen_n; i++) if (seen[i] == h) { fresh = 0; break; }
        if (fresh && seen_n < 128) seen[seen_n++] = h;
        pthread_mutex_unlock(&lk);
        if (fresh) {
            klvk_trace_log("[texL] handle %#llx struct dump:\n", h);
            for (int i = 0; i < 12; i++) {
                unsigned long long v = w[i];
                const char *tag = klvk_img_tag((VkImage)(uintptr_t)v);
                int key = klvk_layer_image_key((VkImage)(uintptr_t)v);
                klvk_trace_log("[texL]   [+%2d] %#018llx  %s%s k=%d\n",
                    i * 8, v, tag, (v && strcmp(tag,"nil") && strcmp(tag,"WILD")) ? " <-- IMAGE" : "", key);
            }
        }
    }

    // Resolve on EVERY call (not just the first), so every frame samples fresh UI.
    if (!kl_env_on("KL_OVERLAY_TEXL", 1)) return NULL;
    // Only a live, tracked, non-eye image. A wild or already-destroyed handle here
    // is exactly the stale-MVKImage crash that destroy-tracking now prevents:
    // klvk_img_reg_has() is true only for an image still alive in our registry.
    int live = img && klvk_img_reg_has(img);
    int eye  = img && klvk_is_eye_image(img);
    // Register for shadow copies (klvk_shadow_pump) and sample the SHADOW once
    // one has landed: it holds the last complete frame, where the live image is
    // racing the guest's own re-render/recycle on another thread.
    void *mtl = NULL;
    if (live && !eye) {
        klvk_shadow_want(img);
        mtl = klvk_shadow_mtl(img);
        if (!mtl) mtl = klvk_overlay_mtl_texture(img);
    }
    // Bounded outcome log so we can tell a real HIT from a silent fall-through to
    // the empty layer image, and cross-reference the sampled image vs the render
    // targets. KL_TEXL_DIAG=1 (or KL_TRACE) turns it on; first 60 then every 600th.
    if (kl_env_on("KL_TEXL_DIAG", 0) || klvk_trace()) {
        static unsigned c;
        if (c < 60 || (c % 600) == 0)
            fprintf(stderr, "[TEXL] h=%#llx img=%#llx live=%d eye=%d -> mtl=%p %s\n",
                    h, (unsigned long long)(uintptr_t)img, live, eye, mtl,
                    mtl ? "HIT" : "miss(fallback to layer image)");
        c++;
    }
    return mtl;
}

unsigned long long kl_vulkan_eye_image(int stage, int eye, unsigned w, unsigned h,
                                       int srgb) {
    return kl_vulkan_eye_image_layers(stage, eye, w, h, srgb, 1);
}

// ---------------------------------------------------------------------------
// The OpenXR seam — XR_KHR_vulkan_enable. See kl_vulkan.h for why it exists.
// ---------------------------------------------------------------------------

// Whether the extension may be advertised. Deliberately NOT kl_vulkan_guest_active():
// that asks whether a device exists, and at xrCreateInstance time the guest has
// not created one yet — it cannot, because the whole point of the extension is
// that it asks US which physical device to create it on. So the question here
// is only whether MoltenVK is reachable at all.
int kl_vulkan_xr_supported(void) { return kl_vulkan_available(); }

// Empty, and true rather than lazy — klvk_CreateDevice already adds
// VK_EXT_metal_objects to every device the guest creates, so there is nothing
// left to require of the app. If something is ever genuinely required here,
// note that the spec wants a SPACE-delimited list in one buffer, not an array.
const char *kl_vulkan_xr_instance_extensions(void) { return ""; }
const char *kl_vulkan_xr_device_extensions(void)   { return ""; }

// The physical device the app must render with.
//
// The VkInstance is the APP's — it created one before asking, which is the
// ordering XR_KHR_vulkan_enable prescribes — so this enumerates on the handle it
// passes rather than on g_instance. Those are usually the same object here (the
// app's vkCreateInstance came through klvk_CreateInstance), but "usually" is not
// a thing to encode: the spec names the instance as a parameter precisely
// because the runtime is not entitled to assume it owns one.
//
// The FIRST device is the answer because MoltenVK exposes exactly one per Metal
// device, and picking by index rather than by score is honest about that. A
// machine that ever reports two would want a rule here, and would say so by
// this log line naming a count above 1.
void *kl_vulkan_xr_physical_device(void *vk_instance) {
    if (!kl_vulkan_available() || !vk_instance) return NULL;
    PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)
        real_gipa((VkInstance)vk_instance, "vkEnumeratePhysicalDevices");
    if (!enum_pd) { VKI("xr: no vkEnumeratePhysicalDevices on the app's instance\n"); return NULL; }
    uint32_t n = 0;
    if (enum_pd((VkInstance)vk_instance, &n, NULL) != VK_SUCCESS || n == 0) {
        // The trap BONELAB paid for, in the one place it can recur: without
        // VK_KHR_portability_enumeration this answers zero devices AND
        // VK_SUCCESS, so a bare count of 0 is not necessarily an empty machine.
        VKI("xr: vkEnumeratePhysicalDevices reports 0 devices — if the app's "
            "instance omitted VK_KHR_portability_enumeration, that is why\n");
        return NULL;
    }
    VkPhysicalDevice pd[8];
    if (n > 8) n = 8;
    if (enum_pd((VkInstance)vk_instance, &n, pd) != VK_SUCCESS || n == 0) return NULL;
    if (n > 1) VKI("xr: %u physical devices; taking the first\n", n);
    return (void *)pd[0];
}

// One OpenXR swapchain image.
//
// Deliberately NOT keyed on (stage, eye) the way the OVRPlugin eye images are:
// in OpenXR the runtime allocates a swapchain's images at xrCreateSwapchain,
// which knows a size and a format and nothing else — WHICH swapchain is an eye
// is only asserted later, at xrEndFrame, from the projection layer. That is a
// property of the API, not of a guest.
//
// So this hands back a bare image and keeps no table of its own; the eye
// association, and with it the MTLTexture publication, belongs at the assertion.
unsigned long long kl_vulkan_xr_image(unsigned w, unsigned h, unsigned layers,
                                      unsigned mips, long long vk_format, int depth) {
    if (!kl_vulkan_guest_active()) return 0;
    klvk_device *d = g_devs[0];
    if (!d || !w || !h) return 0;
    // A depth image is not a colour image with a different format: it needs the
    // depth/stencil attachment usage, and asking for COLOR_ATTACHMENT on a depth
    // format is a validation error rather than a harmless extra bit.
    VkImageUsageFlags usage = depth
        ? (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)
        : (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImage img = klvk_image_alloc(d, (VkFormat)vk_format, w, h,
                                   layers ? layers : 1, mips ? mips : 1, usage, &mem);
    if (img == VK_NULL_HANDLE) return 0;
    return (uint64_t)(uintptr_t)img;
}

// The MTLTexture behind an image this file created, for the compositor seam.
// NULL when the export is unavailable — the caller reports it, because only the
// caller knows which eye it was about.
void *kl_vulkan_xr_image_mtl(unsigned long long image) {
    if (!kl_vulkan_guest_active() || !image) return NULL;
    return eye_mtl_texture(g_devs[0], (VkImage)(uintptr_t)image);
}

// The Vulkan version range a session may be created against.
//
// Measured off the physical device, not asserted: MoltenVK's instance-level and
// device-level versions differ, and the one that governs the app's device is the
// device's. The floor is 1.0 because nothing here needs more, and overstating a
// floor is the dangerous direction — the same reasoning, and the same trap, as
// the GLES range next door (an app told it needs 1.2 asks for 1.2).
void kl_vulkan_xr_api_range(unsigned *min_major, unsigned *min_minor,
                            unsigned *max_major, unsigned *max_minor) {
    if (min_major) *min_major = 1;
    if (min_minor) *min_minor = 0;
    // A conservative ceiling until a device answers, so a failure to measure
    // cannot present as "this runtime supports nothing".
    if (max_major) *max_major = 1;
    if (max_minor) *max_minor = 0;
    if (!kl_vulkan_available()) return;

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    if (g_ndev > 0 && g_devs[0]) phys = g_devs[0]->phys;
    if (phys == VK_NULL_HANDLE && g_instance) {
        PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)
            real_gipa(g_instance, "vkEnumeratePhysicalDevices");
        uint32_t n = 1;
        VkPhysicalDevice one = VK_NULL_HANDLE;
        if (enum_pd && enum_pd(g_instance, &n, &one) >= 0 && n) phys = one;
    }
    if (phys == VK_NULL_HANDLE) return;

    PFN_vkGetPhysicalDeviceProperties props_fn = (PFN_vkGetPhysicalDeviceProperties)
        real_gipa(g_instance, "vkGetPhysicalDeviceProperties");
    if (!props_fn) return;
    VkPhysicalDeviceProperties p;
    memset(&p, 0, sizeof p);
    props_fn(phys, &p);
    if (max_major) *max_major = VK_VERSION_MAJOR(p.apiVersion);
    if (max_minor) *max_minor = VK_VERSION_MINOR(p.apiVersion);
}

// Diagnostic: dump each live non-eye layer image's lit-pixel count, so we can
// tell whether the guest actually RENDERED its menu UI into the overlay
// textures (lit > 0) or is submitting empty panels (0 lit). Gated on KL_VK_OUT
// like the eye capture; the PNGs go to the device but the '(N lit / M)' log
// line is the signal.
void kl_vulkan_capture_layers(void) {
    const char *dir = cap_dir();
    if (!dir || !kl_vulkan_guest_active()) return;
    klvk_device *d = g_devs[0];
    if (!d) return;
    for (int i = 0; i < KLVK_MAX_LAYERS; i++) {
        if (!g_layer_img[i].key) continue;
        klvk_img_slot *sl = &g_layer_img[i].s[0][0];   // stage 0, eye 0
        if (!sl->img) continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/vk_layer%02d.png", dir, g_layer_img[i].key);
        cap_image_layer(d, d->queue, sl->img, sl->w, sl->h, sl->fmt,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, path, NULL, 0);
    }
    VKI("wild-handle reroute census: %u copies + %u blits redirected, scratch=%#llx\n",
        g_wild_copy_redir, g_wild_blit_redir,
        (unsigned long long)(uintptr_t)g_wild_scratch);
    if (g_wild_scratch) {
        char path[1024];
        snprintf(path, sizeof path, "%s/vk_scratch.png", dir);
        cap_image_layer(d, d->queue, g_wild_scratch, 4096, 4096,
                        VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, path, NULL, 0);
    }
}

// One stage per call — the one the association NAMED — unless KL_VK_OUT_ALL=1
// captures every stage that has an image. The distinction is the whole tool:
// on the Vulkan path the stage association runs on the frame-counter fallback
// ("eye stage NOT observed" — both wrath2 and AC Nexus), so the named stage is
// a GUESS, and a capture of only the guess cannot show a wrong one. With ALL,
// one frame yields every stage and eye side by side, and the named stage
// carries a "_named" suffix — so a stale or misassociated stage is visible as
// "the fresh picture is in s1 but s0 is _named".
void kl_vulkan_capture_eyes(unsigned frame, int stage) {
    const char *dir = cap_dir();
    if (!dir || !kl_vulkan_guest_active()) return;
    int every = kl_env_int("KL_VK_OUT_EVERY", 1);
    if (every < 1) every = 1;
    if (frame % (unsigned)every) return;
    if (stage < 0 || stage >= KLVK_EYE_STAGES) return;
    // KL_VK_OUT_MAX: stop after this many captured frames (each is two PNGs),
    // so a long run cannot fill the device.
    static unsigned taken;
    int max = kl_env_int("KL_VK_OUT_MAX", 0);
    if (max > 0 && taken >= (unsigned)max) return;
    taken++;
    static int all = -1;
    if (all < 0) all = kl_env_on("KL_VK_OUT_ALL", 0);

    klvk_device *d = g_devs[0];
    for (int st = 0; st < KLVK_EYE_STAGES; st++) {
        if (!all && st != stage) continue;
        // Under the Array layout slot 0 holds the only image and the two eyes
        // are its layers, so the eye index selects a layer there and an image
        // here. The filename stays `eyeN` either way — it names what was
        // drawn, not where it was stored.
        int layered = g_eye[st][0].img && g_eye[st][0].layers > 1;
        for (int eye = 0; eye < 2; eye++) {
            int slot = layered ? 0 : eye;
            if (!g_eye[st][slot].img) continue;
            char path[1024];
            snprintf(path, sizeof path, "%s/vk_f%05u_s%d_eye%d%s.png", dir, frame,
                     st, eye, (all && st == stage) ? "_named" : "");
            // The guest submitted this to a compositor, so it left it readable
            // by one — SHADER_READ_ONLY_OPTIMAL. See cap_image on why a wrong
            // guess here is cheap on MoltenVK specifically.
            if (cap_image_layer(d, d->queue, g_eye[st][slot].img, g_eye[st][slot].w,
                                g_eye[st][slot].h, g_eye[st][slot].fmt,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                layered ? (uint32_t)eye : 0u, path, NULL, 0))
                g_presented++;
        }
    }
}

// ---------------------------------------------------------------------------
// The frame seam — "this eye texture is finished, a compositor may sample it"
// ---------------------------------------------------------------------------
//
// The GL path answers this with an MTLSharedEvent queued into ANGLE's own
// command stream (kl_glfb's klfb_gpu_frame_now): the compositor's command
// buffer waits on a value the guest's queue signals, and nothing ever stalls on
// the CPU. There is no equivalent here, and the reason is not an oversight —
// **the guest owns the VkQueue and we do not add work to its submissions.** A
// cross-queue GPU wait needs a shared event signalled *after* the guest's
// rendering, and the only ways to get one are to signal a timeline semaphore on
// the guest's queue (which needs `VK_KHR_timeline_semaphore` enabled on a device
// the GUEST creates) or to export the MTLCommandQueue and encode the signal
// ourselves in Objective-C, which this plain-C file cannot do.
//
// So this waits. `vkQueueWaitIdle` on the guest's own queue, at the moment the
// guest asserts the eye textures are drawn (ovrp_EndFrame4), and then a serial
// the compositor reads: past it, the picture is not merely submitted but
// COMPLETE, so no compositor-side ordering is needed at all.
//
// It is a real stall on the guest's frame thread and it is named as one. The
// capture path (KL_VK_OUT) already pays exactly this cost on every captured
// frame, and no device run has happened, so it buys correctness at a price
// nothing has yet measured. `KL_VK_FRAME_SYNC=0` removes the wait and keeps the
// serial — the A/B for "is the compositor showing a torn frame?", and the shape
// the timeline-semaphore version would have.

unsigned long long kl_vulkan_frame_serial(void) {
    return __atomic_load_n(&g_frame_serial, __ATOMIC_ACQUIRE);
}

// ---------------------------------------------------------------------------
// EYE SHADOW (KL_EYE_SHADOW) — candidate fix for a black EYE, mirroring the
// overlay SHADOW above but for the big eye array image.
//
// The overlay resolver samples a SHADOW copy (klvk_shadow_*) rather than the
// live guest image, because a Vulkan image the guest renders on its own thread
// and the compositor samples from ITS thread reads black by sample time. The
// eye image is EXCLUDED from that path (see `live && !eye` in
// kl_vulkan_mtl_for_handle) and reaches the compositor as the LIVE MTLTexture
// through kl_glfb's eye table. Its only ordering is the vkQueueWaitIdle below.
//
// This is the same treatment for the eye: at frame end (the guest's own
// assertion that the stage is drawn), copy the live eye image into a persistent
// per-stage shadow ON THE GUEST'S OWN QUEUE — so Metal's hazard tracking orders
// the copy after the guest's render — and republish the SHADOW's MTLTexture
// into kl_glfb for that stage. The compositor then samples a stable image that
// holds the last complete frame and is written by nobody else.
//
// OFF by default so it is a clean A/B against the live-sample path. The copy is
// the same cost the KL_VK_OUT capture already pays, so it is measured.
static VkImage        g_eye_shadow[KLVK_EYE_STAGES];
static VkDeviceMemory g_eye_shadow_mem[KLVK_EYE_STAGES];
static void          *g_eye_shadow_mtl[KLVK_EYE_STAGES];

static void klvk_eye_shadow_copy(klvk_device *d, int stage) {
    if (!d || stage < 0 || stage >= KLVK_EYE_STAGES) return;
    static PFN_vkCmdCopyImage real_copy;
    if (!real_copy) real_copy = (PFN_vkCmdCopyImage)mvk_sym("vkCmdCopyImage");
    if (!real_copy) return;

    static VkCommandPool   pool;
    static VkCommandBuffer cmd;
    static VkFence         fence;
    if (!pool) {
        VkCommandPoolCreateInfo pi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = d->queue_family,
        };
        if (d->CreateCommandPool(d->dev, &pi, NULL, &pool) != VK_SUCCESS) return;
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (d->AllocateCommandBuffers(d->dev, &ai, &cmd) != VK_SUCCESS) return;
        VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (d->CreateFence(d->dev, &fi, NULL, &fence) != VK_SUCCESS) return;
    }

    // wanderer (and every Array-layout guest) keeps ONE image with N array
    // layers in slot 0, handed out for both eyes. That is the case this fix
    // targets, so it shadows slot 0's image with its full layer count. A Stereo
    // guest (one 1-layer image per eye) is deliberately left on the live path
    // for now — it does not exhibit the black this A/B is chasing.
    klvk_img_slot *slot = &g_eye[stage][0];
    if (!slot->img || !slot->w || !slot->h) return;
    uint32_t layers = slot->layers ? slot->layers : 1;

    if (!g_eye_shadow[stage]) {
        g_eye_shadow[stage] = klvk_image_alloc(d, slot->fmt, slot->w, slot->h,
            layers, 1, VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT, &g_eye_shadow_mem[stage]);
        if (!g_eye_shadow[stage]) return;
        VKI("eye shadow: stage %d %ux%u fmt %d %u layer(s) live %#llx -> %#llx "
            "(KL_EYE_SHADOW)\n", stage, slot->w, slot->h, (int)slot->fmt, layers,
            (unsigned long long)(uintptr_t)slot->img,
            (unsigned long long)(uintptr_t)g_eye_shadow[stage]);
    }

    d->ResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (d->BeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return;
    VkImageCopy rc = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers },
        .extent = { slot->w, slot->h, 1 },
    };
    real_copy(cmd, slot->img, VK_IMAGE_LAYOUT_GENERAL,
              g_eye_shadow[stage], VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
    if (d->EndCommandBuffer(cmd) != VK_SUCCESS) return;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    d->ResetFences(d->dev, 1, &fence);
    if (d->QueueSubmit(d->queue, 1, &si, fence) != VK_SUCCESS) return;
    d->WaitForFences(d->dev, 1, &fence, VK_TRUE, 2000000000ull);

    if (!g_eye_shadow_mtl[stage])
        g_eye_shadow_mtl[stage] = eye_mtl_texture(d, g_eye_shadow[stage]);
    void *tex = g_eye_shadow_mtl[stage];
    if (!tex) return;
    int w = (int)slot->w, h = (int)slot->h;
    if (layers > 1) {
        kl_glfb_note_eye_mtl_texture(0, stage, tex, 0, w, h);
        kl_glfb_note_eye_mtl_texture(1, stage, tex, 1, w, h);
    } else {
        kl_glfb_note_eye_mtl_texture(0, stage, tex, 0, w, h);
    }
}

void kl_vulkan_frame_done(int stage) {
    if (!kl_vulkan_guest_active()) return;
    klvk_device *d = g_devs[0];
    static int eye_shadow = -1;
    // Default OFF. Tested ON for olar: the trailing (rain + menu) and grayscale
    // were UNCHANGED, so the eye-layer sampling race is NOT olar's problem — its
    // trailing is compositor/reprojection-side (it affects the overlay menu too)
    // and the grayscale is world-material/content. Not worth the per-frame copy
    // by default; KL_EYE_SHADOW=1 re-enables for the wanderer black-eye A/B it
    // was originally built for.
    if (eye_shadow < 0) eye_shadow = kl_env_on("KL_EYE_SHADOW", 0);
    if (eye_shadow) klvk_eye_shadow_copy(d, stage);
    if (kl_env_on("KL_VK_FRAME_SYNC", 1) && d && d->queue) {
        struct timespec w0, w1;
        clock_gettime(CLOCK_UPTIME_RAW, &w0);
        VkResult r = d->QueueWaitIdle ? d->QueueWaitIdle(d->queue)
                   : d->DeviceWaitIdle ? d->DeviceWaitIdle(d->dev)
                                       : VK_SUCCESS;
        clock_gettime(CLOCK_UPTIME_RAW, &w1);
        // The GPU-bound share of a synced frame, which the shim's "after"
        // otherwise lumps in with the game's CPU work.
        {
            static double sum, worst, since;
            static int n;
            double ms = (w1.tv_sec - w0.tv_sec) * 1e3 + (w1.tv_nsec - w0.tv_nsec) / 1e6;
            double now = w1.tv_sec + w1.tv_nsec / 1e9;
            sum += ms; n++; if (ms > worst) worst = ms;
            if (!since) since = now;
            if (now - since >= 5.0) {
                VKI("frame-end queue wait (GPU finishing the frame) avg %.1f / worst %.1f ms over %d frames\n",
                    sum / n, worst, n);
                sum = worst = 0; n = 0; since = now;
            }
        }
        if (r != VK_SUCCESS) {
            static int said;
            if (!said++)
                VKI("waiting for the guest's queue at frame end answered %d — a "
                    "compositor may sample an eye the GPU is still writing\n", (int)r);
        }
    }
    __atomic_add_fetch(&g_frame_serial, 1, __ATOMIC_RELEASE);
    klvk_retire_sweep();
}

static VkResult klvk_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    klvk_device *d = dev_of_queue(q);
    unsigned this_frame = g_presented++;

    int every = kl_env_int("KL_VK_OUT_EVERY", 1);
    if (every < 1) every = 1;
    const char *dir = cap_dir();
    int want = dir && (this_frame % (unsigned)every) == 0;

    for (uint32_t i = 0; pi && i < pi->swapchainCount; i++) {
        klvk_swapchain *sc = swap_of(pi->pSwapchains[i]);
        if (!sc) continue;
        if (want && i == 0) {
            cap_frame(sc, q, pi->pImageIndices[i], pi->pWaitSemaphores,
                      pi->waitSemaphoreCount);
        } else if (pi->waitSemaphoreCount && d) {
            // Not capturing, but the wait semaphores still have to be consumed.
            VkPipelineStageFlags *stages =
                calloc(pi->waitSemaphoreCount, sizeof *stages);
            for (uint32_t k = 0; k < pi->waitSemaphoreCount; k++)
                stages[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = pi->waitSemaphoreCount,
                .pWaitSemaphores = pi->pWaitSemaphores,
        .pWaitDstStageMask = stages,
            };
            d->QueueSubmit(q, 1, &si, VK_NULL_HANDLE);
            free(stages);
        }
        if (pi->pResults) pi->pResults[i] = VK_SUCCESS;
    }
    if (this_frame == 0) VKI("first vkQueuePresentKHR — the guest is rendering\n");
    return VK_SUCCESS;
}

// Forward decls: the wild-handle -> scratch substitution used by the copy/blit
// wrappers below, defined further down beside klvk_CreateImageView.
static VkImage klvk_wild_scratch(void);
static VkImage klvk_sub_wild(VkImage img);

// vkCmdCopyImage, guarded exactly like the image-view path: AC Nexus's broken
// overlay layers feed a wild VkImage handle here too (seen as x4 in the SIGSEGV
// inside MVKCmdCopyImage::setContent). If either image was never created, drop
// the copy rather than let MoltenVK dereference garbage. Resolved from MoltenVK
// by name (a command function needs no device handle to look up).
static void VKAPI_CALL klvk_CmdCopyImage(VkCommandBuffer cb, VkImage src,
        VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
        const VkImageCopy *regions) {
    static PFN_vkCmdCopyImage real;
    if (!real) real = (PFN_vkCmdCopyImage)mvk_sym("vkCmdCopyImage");
    if (klvk_trace() && klvk_trace_once((unsigned long long)(uintptr_t)src,(unsigned long long)(uintptr_t)dst)) {
        int sk=klvk_layer_image_key(src), dk=klvk_layer_image_key(dst);
        klvk_trace_log("[copy] src %#llx(%s k=%d) -> dst %#llx(%s k=%d) n=%u\n",
            (unsigned long long)(uintptr_t)src, klvk_img_tag(src), sk,
            (unsigned long long)(uintptr_t)dst, klvk_img_tag(dst), dk, n);
    }
    if ((src && !klvk_img_reg_has(src)) || (dst && !klvk_img_reg_has(dst))) {
        // DEFAULT: DROP the copy. Redirecting it onto the scratch was tried
        // (KL_VK_WILD_REDIRECT=1 restores it) and correlates with the render
        // loop stalling at pipeline depth ~8: a copy whose regions were sized
        // for the guest's own texture lands out of bounds on the 4096x4096
        // scratch, the command buffer dies, its fence never signals, and Unity
        // waits on frame 1 forever. A dropped copy costs a blank overlay; a
        // dead fence costs every frame after the eighth.
        if (kl_env_on("KL_VK_WILD_REDIRECT", 0)) {
            g_wild_copy_redir++;
            static int red; if (red < 8) { red++;
                VKI("vkCmdCopyImage: REDIRECTING wild handle to scratch "
                    "(src %#llx dst %#llx) so the guest's composite can land\n",
                    (unsigned long long)(uintptr_t)src, (unsigned long long)(uintptr_t)dst); }
            VkImage nsrc = klvk_sub_wild(src);
            VkImage ndst = klvk_sub_wild(dst);
            // The stall fix. The guest sized these regions for ITS OWN composite
            // texture; landed unchanged on the 4096x4096 scratch they run out of
            // bounds, MoltenVK kills the command buffer, its fence never signals,
            // and Unity waits on frame 1 forever. CLAMP every region to the
            // scratch's per-mip bounds and drop any that fall entirely outside,
            // so the composite lands as a live, safe copy instead of a dead fence.
            if (ndst == g_wild_scratch && n && regions) {
                // Region census: log where the guest wants each panel to land in
                // its composite, so we can see the composite's true coordinate
                // space (and whether it exceeds the 4096 scratch). KL_TRACE gates.
                if (kl_env_on("KL_TRACE", 0)) {
                    static int rl;
                    for (uint32_t i = 0; i < n && rl < 40; i++, rl++)
                        fprintf(stderr, "[TRACE] [region] src %#llx dstOff(%d,%d) "
                            "ext(%ux%u) mip %u layer %u\n",
                            (unsigned long long)(uintptr_t)nsrc,
                            regions[i].dstOffset.x, regions[i].dstOffset.y,
                            regions[i].extent.width, regions[i].extent.height,
                            regions[i].dstSubresource.mipLevel,
                            regions[i].dstSubresource.baseArrayLayer);
                }
                VkImageCopy *cr = calloc(n, sizeof *cr);
                uint32_t cn = 0;
                for (uint32_t i = 0; i < n && cr; i++) {
                    VkImageCopy rc = regions[i];
                    if (rc.dstSubresource.mipLevel >= KLVK_SCRATCH_MIPS) continue;
                    uint32_t md = 4096u >> rc.dstSubresource.mipLevel; if (!md) md = 1u;
                    if (rc.dstOffset.x < 0) rc.dstOffset.x = 0;
                    if (rc.dstOffset.y < 0) rc.dstOffset.y = 0;
                    if ((uint32_t)rc.dstOffset.x >= md || (uint32_t)rc.dstOffset.y >= md) continue;
                    uint32_t maxw = md - (uint32_t)rc.dstOffset.x;
                    uint32_t maxh = md - (uint32_t)rc.dstOffset.y;
                    if (rc.extent.width  > maxw) rc.extent.width  = maxw;
                    if (rc.extent.height > maxh) rc.extent.height = maxh;
                    if (rc.extent.width == 0u || rc.extent.height == 0u) continue;
                    if (rc.dstSubresource.baseArrayLayer >= KLVK_SCRATCH_LAYERS)
                        rc.dstSubresource.baseArrayLayer = 0;
                    cr[cn++] = rc;
                }
                if (cn && real) real(cb, nsrc, sl, ndst, dl, cn, cr);
                free(cr);
                return;
            }
            src = nsrc;
            dst = ndst;
        } else {
            // CAPTURE-ON-COPY — the deterministic replacement for the timing-
            // sensitive shadow pump. This copy is the guest reading a menu
            // panel into its composite, i.e. the ONE moment its content is
            // guaranteed present, on the guest's own thread and in its own
            // command order. The wild composite destination is unusable, so
            // retarget the copy into the panel's persistent SHADOW instead:
            // recorded into the SAME command buffer, so it executes exactly
            // when the guest's read would have. The compositor samples the
            // shadow, and what it shows no longer depends on when it looks.
            // KL_SHADOW_FROMCOPY=0 disables.
            static int fc = -1;
            if (fc < 0) fc = kl_env_on("KL_SHADOW_FROMCOPY", 1);
            if (fc && real && src && klvk_img_reg_has(src)
                && dst && !klvk_img_reg_has(dst) && n && regions) {
                VkImage sh = klvk_shadow_for_copy(src);
                uint32_t shw = 0, shh = 0; VkFormat shf;
                if (sh && klvk_img_info_get(src, &shf, &shw, &shh)) {
                    VkImageCopy *rew = calloc(n, sizeof *rew);
                    uint32_t rn = 0;
                    for (uint32_t i = 0; i < n && rew; i++) {
                        VkImageCopy rc = regions[i];
                        if (rc.srcSubresource.mipLevel != 0) continue;
                        rc.dstSubresource = rc.srcSubresource;
                        rc.dstSubresource.baseArrayLayer = 0;
                        rc.dstOffset = rc.srcOffset;   // same rect, panel-space
                        if ((uint32_t)rc.dstOffset.x >= shw ||
                            (uint32_t)rc.dstOffset.y >= shh) continue;
                        uint32_t mw = shw - (uint32_t)rc.dstOffset.x;
                        uint32_t mh = shh - (uint32_t)rc.dstOffset.y;
                        if (rc.extent.width  > mw) rc.extent.width  = mw;
                        if (rc.extent.height > mh) rc.extent.height = mh;
                        if (rc.extent.width && rc.extent.height) rew[rn++] = rc;
                    }
                    if (rn) real(cb, src, sl, sh, VK_IMAGE_LAYOUT_GENERAL, rn, rew);
                    free(rew);
                    static int said; if (said < 8) { said++;
                        VKI("vkCmdCopyImage: CAPTURED panel %#llx into shadow %#llx "
                            "(composite copy retargeted)\n",
                            (unsigned long long)(uintptr_t)src,
                            (unsigned long long)(uintptr_t)sh); }
                    return;
                }
            }
            static int dropped; if (dropped < 4) { dropped++;
                VKI("vkCmdCopyImage: DROPPING copy on wild handle "
                    "(src %#llx dst %#llx) - KL_VK_WILD_REDIRECT=1 redirects instead\n",
                    (unsigned long long)(uintptr_t)src, (unsigned long long)(uintptr_t)dst); }
            return;
        }
    }
    if (real) real(cb, src, sl, dst, dl, n, regions);
}

// The rest of the image-consuming commands, guarded the same way: if a handle
// was never created here (AC Nexus's unbacked video-overlay garbage), drop the
// command instead of letting MoltenVK dereference the wild pointer. All resolved
// from MoltenVK by name; a command function needs no device handle.
#define KLVK_IMG_BAD(img) ((img) && !klvk_img_reg_ok(img))
static void VKAPI_CALL klvk_CmdBlitImage(VkCommandBuffer cb, VkImage src,
        VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
        const VkImageBlit *r, VkFilter f) {
    static PFN_vkCmdBlitImage real;
    if (!real) real = (PFN_vkCmdBlitImage)mvk_sym("vkCmdBlitImage");
    if (KLVK_IMG_BAD(src) || KLVK_IMG_BAD(dst)) {
        if (!kl_env_on("KL_VK_WILD_REDIRECT", 0)) return;   // drop, as the copy above
        g_wild_blit_redir++;
        src = klvk_sub_wild(src);
        dst = klvk_sub_wild(dst);
    }
    if (real) real(cb, src, sl, dst, dl, n, r, f);
}
static void VKAPI_CALL klvk_CmdResolveImage(VkCommandBuffer cb, VkImage src,
        VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n,
        const VkImageResolve *r) {
    static PFN_vkCmdResolveImage real;
    if (!real) real = (PFN_vkCmdResolveImage)mvk_sym("vkCmdResolveImage");
    if (KLVK_IMG_BAD(src) || KLVK_IMG_BAD(dst)) {
        VKI("vkCmdResolveImage: REFUSING (src %#llx dst %#llx unknown)\n",
            (unsigned long long)(uintptr_t)src, (unsigned long long)(uintptr_t)dst);
        return;
    }
    if (real) real(cb, src, sl, dst, dl, n, r);
}
static void VKAPI_CALL klvk_CmdClearColorImage(VkCommandBuffer cb, VkImage img,
        VkImageLayout l, const VkClearColorValue *c, uint32_t n,
        const VkImageSubresourceRange *r) {
    static PFN_vkCmdClearColorImage real;
    if (!real) real = (PFN_vkCmdClearColorImage)mvk_sym("vkCmdClearColorImage");
    if (KLVK_IMG_BAD(img)) { VKI("vkCmdClearColorImage: REFUSING (img %#llx unknown)\n",
        (unsigned long long)(uintptr_t)img); return; }
    if (real) real(cb, img, l, c, n, r);
}
static int emu_on(void);    // defined below (target/env gate for the mirror + probes)
static int probe_on(void);
static int klvk_astc_sub_has(VkImage img);   // KL_VK_ASTC_RGBA8 test; defined below
// KL_VK_XFORM_TRACE=1 (diagnostic): the guest's RGBA32F images (the per-instance "transform texture" is one) and
// every buffer->image upload into them, region by region — is any part of the per-frame instance data mis-copied?
static VkImage g_xf_img[16]; static uint32_t g_xf_w[16], g_xf_h[16]; static int g_nxf;
static int xftrace_on(void) { static int v = -1; if (v < 0) v = kl_env_on("KL_VK_XFORM_TRACE", 0); return v; }
static int xf_find(VkImage img) { for (int i = 0; i < g_nxf; i++) if (g_xf_img[i] == img) return i; return -1; }
// KL_VK_EYEDRAW=1 (diagnostic, with KL_VK_XFORM_TRACE=1): for every draw with the eyeball pipeline, what it reads
// from the transform texture: its per-instance texel coordinate (vertex binding 1, half2) and that texel's header as
// the engine uploaded it (the latest upload into the transform texture recorded in the same command buffer).
static int eyedraw_on(void) { static int v = -1; if (v < 0) v = kl_env_on("KL_VK_EYEDRAW", 0); return v; }
#define KLED_CB 16384
static struct { void *cb; uint64_t pipe, vb1, vb1off, xbuf, xoff; uint32_t xrows; } g_ed_cb[KLED_CB];
static uint64_t g_ed_pipes[64]; static int g_ned_pipes;
static uint64_t g_ed_mod[8]; static int g_ned_mod;
#define KLED_DRAWS 512
static struct { uint64_t vb1, vb1off, xbuf, xoff; uint32_t xrows, first, count; } g_ed_draw[KLED_DRAWS];
static int g_ned_draw;
static int ed_slot(void *cb) {          // g_lock held; open addressing on the handle
    unsigned h = (unsigned)(((uintptr_t)cb >> 4) * 2654435761u) % KLED_CB;
    for (int n = 0; n < KLED_CB; n++, h = (h + 1) % KLED_CB) {
        if (g_ed_cb[h].cb == cb) return (int)h;
        if (!g_ed_cb[h].cb) { g_ed_cb[h].cb = cb; return (int)h; }
    }
    memset(&g_ed_cb[h], 0, sizeof g_ed_cb[h]); g_ed_cb[h].cb = cb; return (int)h;
}
#define KLEDC 8192
static struct { uint64_t dst, src, so, dof, sz; } g_edc[KLEDC]; static unsigned g_edc_n;
static struct { uint64_t dof, sz; uint8_t *data; } g_eds_inst[64]; static unsigned g_eds_n;   // recent instance-stream copies
static struct { uint32_t rows; float *data; } g_eds_xf;                   // latest transform-texture upload, rows x 1024 x 4
static int g_ed_near;                                                     // draws to print after an eye draw
static uint64_t g_ed_lastvb1off[8]; static int g_ed_nlast;                // the last few draws' instance offsets
static int ed_is_eye_pipe(uint64_t p) { for (int i = 0; i < g_ned_pipes; i++) if (g_ed_pipes[i] == p) return 1; return 0; }
static void VKAPI_CALL klvk_CmdCopyBufferToImage2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *ci) {
    static PFN_vkCmdCopyBufferToImage2 real;
    if (!real) { real = (PFN_vkCmdCopyBufferToImage2)mvk_sym("vkCmdCopyBufferToImage2");
                 if (!real) real = (PFN_vkCmdCopyBufferToImage2)mvk_sym("vkCmdCopyBufferToImage2KHR"); }
    if (xftrace_on() && ci) {
        int k = xf_find(ci->dstImage);
        static unsigned said, any;
        if (!any++) fprintf(stderr, "[xform] the guest uses vkCmdCopyBufferToImage2\n");
        if (k >= 0) for (uint32_t i = 0; i < ci->regionCount; i++) if (said < 400) { said++;
            const VkBufferImageCopy2 *r = &ci->pRegions[i];
            fprintf(stderr, "[xform] copy2 -> img#%d %ux%u: region %u/%u bufOff %llu rowLen %u imgH %u  at (%d,%d) extent %ux%u\n",
                k, g_xf_w[k], g_xf_h[k], i, ci->regionCount, (unsigned long long)r->bufferOffset, r->bufferRowLength,
                r->bufferImageHeight, r->imageOffset.x, r->imageOffset.y, r->imageExtent.width, r->imageExtent.height); }
    }
    if (real) real(cb, ci);
}


static void VKAPI_CALL klvk_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer buf,
        VkImage img, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
    static PFN_vkCmdCopyBufferToImage real;
    if (!real) real = (PFN_vkCmdCopyBufferToImage)mvk_sym("vkCmdCopyBufferToImage");
    if (KLVK_IMG_BAD(img)) { VKI("vkCmdCopyBufferToImage: REFUSING (img %#llx unknown)\n",
        (unsigned long long)(uintptr_t)img); return; }
    // KL_VK_XFORM_BARRIER=1 (diagnostic): a full barrier in front of every upload into the transform texture, so no
    // draw still reading the previous pass's per-instance data can overlap the overwrite (write-after-read)
    static int xfbar = -1; if (xfbar < 0) xfbar = kl_env_on("KL_VK_XFORM_BARRIER", 0);
    if (xfbar && xf_find(img) >= 0) {
        static PFN_vkCmdPipelineBarrier pb;
        if (!pb) pb = (PFN_vkCmdPipelineBarrier)mvk_sym("vkCmdPipelineBarrier");
        VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
        VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = l, .newLayout = l,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        if (pb) pb(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 1, &ib);
    }
    if (eyedraw_on() && r && n && xf_find(img) >= 0) {
        extern void klvk_ed_snap_xf(uint64_t buf, uint64_t off, uint32_t rows);
        klvk_ed_snap_xf((uint64_t)(uintptr_t)buf, r[0].bufferOffset, r[0].imageExtent.height);
        pthread_mutex_lock(&g_lock);
        int k = ed_slot(cb);
        g_ed_cb[k].xbuf = (uint64_t)(uintptr_t)buf; g_ed_cb[k].xoff = r[0].bufferOffset; g_ed_cb[k].xrows = r[0].imageExtent.height;
        pthread_mutex_unlock(&g_lock);
    }
    if (xftrace_on() && r) {
        int k = xf_find(img);
        if (k >= 0) {
            static unsigned said;
            for (uint32_t i = 0; i < n; i++) if (said < 400) { said++;
                fprintf(stderr, "[xform] copy -> img#%d %ux%u: region %u/%u bufOff %llu rowLen %u imgH %u  at (%d,%d) extent %ux%u mip %u layer %u\n",
                    k, g_xf_w[k], g_xf_h[k], i, n, (unsigned long long)r[i].bufferOffset, r[i].bufferRowLength, r[i].bufferImageHeight,
                    r[i].imageOffset.x, r[i].imageOffset.y, r[i].imageExtent.width, r[i].imageExtent.height,
                    r[i].imageSubresource.mipLevel, r[i].imageSubresource.baseArrayLayer); }
        }
    }
    // KL_VK_ASTC_RGBA8 test: this image was created RGBA8 in place of ASTC. The
    // guest's ASTC-block upload would overrun the RGBA8 store (4x the bytes), so SKIP
    // it and clear to solid GREEN. World turns green => the ASTC albedo reaches
    // surfaces with valid UVs => the gray was the ASTC content/decode. Stays gray =>
    // ASTC is not the visible albedo. The image is in TRANSFER_DST (l) here.
    if (klvk_astc_sub_has(img)) {
        static PFN_vkCmdClearColorImage clr;
        if (!clr) clr = (PFN_vkCmdClearColorImage)mvk_sym("vkCmdClearColorImage");
        if (clr) {
            VkClearColorValue green; green.float32[0] = 0.0f; green.float32[1] = 1.0f;
            green.float32[2] = 0.0f; green.float32[3] = 1.0f;
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                            VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
            clr(cb, img, l, &green, 1, &rng);
        }
        return;   // skip the real ASTC->RGBA8 copy (would overrun)
    }
    // PROBE (olar gray hunt): count texture uploads by target-image FORMAT. The gray
    // world materials are the open question — if the ASTC world textures (fmt
    // 165/171/172 = ASTC 6x6/8x8) never appear here, they are created but never
    // filled -> sampled empty -> flat gray. A per-format tally (logged once per new
    // format, then a running total) tells us whether the albedo textures get data.
    if (emu_on() || probe_on()) {
        static struct { int fmt, count; } hist[32];
        static int nhist, total;
        VkFormat f = 0; uint32_t w = 0, h = 0;
        klvk_img_info_get(img, &f, &w, &h);
        pthread_mutex_lock(&g_img_info_lock);
        total++;
        int seen = 0;
        for (int i = 0; i < nhist; i++) if (hist[i].fmt == (int)f) { hist[i].count++; seen = 1; break; }
        if (!seen && nhist < 32) {
            hist[nhist].fmt = (int)f; hist[nhist].count = 1; nhist++;
            VKI("[probe] CopyBufferToImage: first upload to format %d (%ux%u) — "
                "texture data IS being delivered to this format (total uploads %d)\n",
                (int)f, w, h, total);
        }
        if ((total & (total - 1)) == 0)   // powers of two: 1,2,4,8,... a cheap heartbeat
            VKI("[probe] CopyBufferToImage: %d uploads so far across %d distinct "
                "formats\n", total, nhist);
        pthread_mutex_unlock(&g_img_info_lock);
    }
    if (real) real(cb, buf, img, l, n, r);
    // DECISIVE DIAGNOSTIC (KL_VK_TINT_DEFAULTS=1): tint every 1x1 texture magenta
    // right after its upload. If the gray WORLD turns magenta, materials are
    // sampling 1x1 default placeholders (binding/upload-sync bug); if it stays gray,
    // the world samples its real textures and the bug is in sampling (UVs / ASTC).
    // The image is still in TRANSFER_DST here (l), and 1x1 defaults are copy targets,
    // so a clear needs no extra barrier. Diagnostic only; off by default.
    if (real && kl_env_on("KL_VK_TINT_DEFAULTS", 0)) {
        VkFormat f = 0; uint32_t w = 0, h = 0;
        // Only plain 8-bit color formats (37..50 = R8G8B8A8 / B8G8R8A8 variants) are
        // safe to color-clear — that is exactly what UE's default placeholder
        // textures are. Depth (124=D16_UNORM..130), compressed (>=131: BC/ETC/ASTC),
        // and float formats CANNOT be color-cleared (MoltenVK errors and the process
        // dies), so skip them.
        if (klvk_img_info_get(img, &f, &w, &h) && w <= 1 && h <= 1 &&
            (int)f >= 37 && (int)f <= 50) {
            static PFN_vkCmdClearColorImage clr;
            if (!clr) clr = (PFN_vkCmdClearColorImage)mvk_sym("vkCmdClearColorImage");
            if (clr) {
                VkClearColorValue magenta;
                magenta.float32[0] = 1.0f; magenta.float32[1] = 0.0f;
                magenta.float32[2] = 1.0f; magenta.float32[3] = 1.0f;
                VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                clr(cb, img, l, &magenta, 1, &rng);
            }
        }
    }
}
static void VKAPI_CALL klvk_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage img,
        VkImageLayout l, VkBuffer buf, uint32_t n, const VkBufferImageCopy *r) {
    static PFN_vkCmdCopyImageToBuffer real;
    if (!real) real = (PFN_vkCmdCopyImageToBuffer)mvk_sym("vkCmdCopyImageToBuffer");
    if (KLVK_IMG_BAD(img)) { VKI("vkCmdCopyImageToBuffer: REFUSING (img %#llx unknown)\n",
        (unsigned long long)(uintptr_t)img); return; }
    if (real) real(cb, img, l, buf, n, r);
}
// Pipeline barrier is special: it carries an ARRAY of image barriers, so rather
// than drop the whole call we filter out the ones on unknown images and forward
// the rest - the eye layer's own transitions must still go through.
static void VKAPI_CALL klvk_CmdPipelineBarrier(VkCommandBuffer cb,
        VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags df,
        uint32_t mbc, const VkMemoryBarrier *mb,
        uint32_t bbc, const VkBufferMemoryBarrier *bb,
        uint32_t ibc, const VkImageMemoryBarrier *ib) {
    static PFN_vkCmdPipelineBarrier real;
    if (!real) real = (PFN_vkCmdPipelineBarrier)mvk_sym("vkCmdPipelineBarrier");
    if (!real) return;
    int bad = 0;
    for (uint32_t i = 0; i < ibc; i++) if (KLVK_IMG_BAD(ib[i].image)) { bad = 1; break; }
    if (!bad) { real(cb, ss, ds, df, mbc, mb, bbc, bb, ibc, ib); return; }
    // A wild handle here is AC Nexus's menu/composite render target, which also
    // reaches vkCreateImageView and vkCmdCopyImage - both now SUBSTITUTE the
    // scratch for it. Its layout transitions must land on that SAME scratch or
    // the substituted image is used as a colour attachment while still in
    // UNDEFINED layout: MoltenVK then renders nothing and the guest's own
    // CommandBuffer reports "temporary render texture not found", which is the
    // menu never appearing. So REWRITE the barrier onto the scratch rather than
    // DROP it - the whole lifecycle of the wild handle stays on one real image.
    VkImageMemoryBarrier *rew = calloc(ibc ? ibc : 1, sizeof *rew);
    uint32_t redir = 0;
    if (rew) for (uint32_t i = 0; i < ibc; i++) {
        rew[i] = ib[i];
        if (KLVK_IMG_BAD(ib[i].image)) { rew[i].image = klvk_sub_wild(ib[i].image); redir++; }
    }
    static int said; if (said < 8) { said++;
        VKI("vkCmdPipelineBarrier: REROUTED %u of %u image barriers onto the "
            "scratch so the substituted render target transitions with it\n",
            redir, ibc); }
    real(cb, ss, ds, df, mbc, mb, bbc, bb, ibc, rew ? rew : ib);
    free(rew);
}

// A thin pass-through over MoltenVK's vkCreateImage that LOGS the handle it
// hands back and the RESULT. A guest that ignores a failed create keeps an
// uninitialised VkImage variable and later feeds it to vkCreateImageView, where
// MoltenVK dereferences the garbage and faults - so the question "was this image
// ever really created, and did the call succeed" is answered here. Our own
// klvk_image_alloc uses d->CreateImage (the real entry) and never reaches this,
// so this only ever sees the GUEST's images.
static int klvk_is_astc(VkFormat f);   // defined below (near format-properties)
// KL_VK_ASTC_RGBA8 test: hash set of images substituted ASTC->RGBA8, so
// CmdCopyBufferToImage can skip the (overrunning) ASTC upload and clear to green
// instead. Open-addressed, VkImage keys; sized for olar's ~500-2000 ASTC images.
#define KLVK_ASTC_SUB_N 4096
static VkImage g_astc_sub[KLVK_ASTC_SUB_N];
static pthread_mutex_t g_astc_sub_lock = PTHREAD_MUTEX_INITIALIZER;
static void klvk_astc_sub_add(VkImage img) {
    if (!img) return;
    uint64_t h = ((uint64_t)(uintptr_t)img >> 6) & (KLVK_ASTC_SUB_N - 1);
    pthread_mutex_lock(&g_astc_sub_lock);
    for (int i = 0; i < KLVK_ASTC_SUB_N; i++) {
        uint64_t s = (h + i) & (KLVK_ASTC_SUB_N - 1);
        if (!g_astc_sub[s] || g_astc_sub[s] == img) { g_astc_sub[s] = img; break; }
    }
    pthread_mutex_unlock(&g_astc_sub_lock);
}
static int klvk_astc_sub_has(VkImage img) {
    if (!img) return 0;
    uint64_t h = ((uint64_t)(uintptr_t)img >> 6) & (KLVK_ASTC_SUB_N - 1);
    int found = 0;
    pthread_mutex_lock(&g_astc_sub_lock);
    for (int i = 0; i < KLVK_ASTC_SUB_N; i++) {
        uint64_t s = (h + i) & (KLVK_ASTC_SUB_N - 1);
        if (!g_astc_sub[s]) break;
        if (g_astc_sub[s] == img) { found = 1; break; }
    }
    pthread_mutex_unlock(&g_astc_sub_lock);
    return found;
}
// KL_VK_MSAA_TRACE=1 (diagnostic, off by default): every multisampled image the guest creates (size, layers, format,
// samples, usage incl. TRANSIENT_ATTACHMENT 0x40) and the memory it is bound to (type index and property flags;
// LAZILY_ALLOCATED 0x10 is what MoltenVK backs with memoryless storage). The question it answers: does the game
// ask for tile-only MSAA attachments, and do they get real memory anyway.
static int g_msaa_trace = -1;
static int msaa_trace_on(void) { if (g_msaa_trace < 0) g_msaa_trace = kl_env_on("KL_VK_MSAA_TRACE", 0); return g_msaa_trace; }
enum { kMsaaImgs = 256, kMsaaMems = 8192 };
static struct { VkImage img; uint32_t w, h, layers, samples, fmt, usage; } g_msaa_imgs[kMsaaImgs];
static int g_msaa_nimg;
static struct { VkDeviceMemory mem; uint32_t type; VkDeviceSize size; } g_msaa_mems[kMsaaMems];
static int g_msaa_nmem;
static VkDeviceSize g_msaa_live;            // what the game holds in vkAllocateMemory blocks (trace only)
static pthread_mutex_t g_msaa_mu = PTHREAD_MUTEX_INITIALIZER;

// KL_VK_MSAA_MEMORYLESS=1: back the game's multisampled colour and depth attachments with Metal memoryless (tile-only)
// storage instead of real memory. Alyx's stereo scene pass clears them, resolves them inside the pass and stores
// neither (storeOp DONT_CARE; KL_VK_RP_TRACE), so their contents never need memory: 435 MB at 2784x2208 x2 x4x.
// The image is created as a TRANSIENT attachment (vkCreateImage above) and its dedicated allocation is moved to
// MoltenVK's LAZILY_ALLOCATED memory type, which MoltenVK backs with MTLStorageModeMemoryless. Memoryless storage
// only works inside one Metal encoder, so this relies on KL_VK_MERGE keeping the pass's suspend/resume chunks in one
// encoder; kl_gpu_time.m counts any chunk that is not (an "unmerged load", [ml] lines) and keeps it legal.
static void live_add(VkDeviceMemory mem, VkDeviceSize size);   // KL_VK_REPORT_USAGE (klvk_cap_heaps)
static void live_del(VkDeviceMemory mem);
static int g_msaa_ml = -1;
static int msaa_ml_on(void) { if (g_msaa_ml < 0) g_msaa_ml = kl_env_on("KL_VK_MSAA_MEMORYLESS", 0); return g_msaa_ml; }
enum { kMlImgs = 32 };
static VkImage g_ml_imgs[kMlImgs];
static int g_ml_nimg;
static void ml_add(VkImage img, VkImageUsageFlags was, const VkImageCreateInfo *ci) {
    pthread_mutex_lock(&g_msaa_mu);
    if (g_ml_nimg < kMlImgs) g_ml_imgs[g_ml_nimg++] = img;
    pthread_mutex_unlock(&g_msaa_mu);
    fprintf(stderr, "[ml] image %#llx %ux%u layers %u samples %u fmt %d: usage %#x -> %#x (transient)\n",
            (unsigned long long)(uintptr_t)img, ci->extent.width, ci->extent.height, ci->arrayLayers,
            (unsigned)ci->samples, (int)ci->format, was, ci->usage);
}
// The game does not use dedicated allocations for these: it allocates a block of exactly the image's size right after
// asking for its requirements. So the requirements are noted here and the next allocation of that exact size (whose
// type the image accepts) is the one moved to the memoryless type; the bind that follows is checked ([ml] bind).
static struct { VkImage img; VkDeviceSize size; uint32_t bits; int open; } g_ml_req[kMlImgs];
static int g_ml_nreq;
static void ml_note_req(VkDevice dev, VkImage img, const VkMemoryRequirements *mr);
static VkDeviceMemory g_ml_mem[kMlImgs];
static int g_ml_nmem;
static int ml_has(VkImage img) {
    int hit = 0;
    pthread_mutex_lock(&g_msaa_mu);
    for (int i = 0; i < g_ml_nimg; i++) if (g_ml_imgs[i] == img) { hit = 1; break; }
    pthread_mutex_unlock(&g_msaa_mu);
    return hit;
}

static VkResult VKAPI_CALL klvk_CreateImage(VkDevice dev, const VkImageCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkImage *out) {
    static PFN_vkCreateImage real;
    if (!real && real_gdpa) real = (PFN_vkCreateImage)real_gdpa(dev, "vkCreateImage");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    int did_astc_sub = 0;
    VkImageCreateInfo mod;
    // KL_VK_MSAA_MEMORYLESS: a multisampled colour/depth attachment becomes a transient attachment (its other usages
    // dropped), so its memory can be Metal memoryless storage (see ml_add / klvk_AllocateMemory).
    int ml_pending = 0; VkImageUsageFlags ml_usage_was = 0;
    if (ci && ci->samples > 1 && (ci->usage & 0x30u) && msaa_ml_on()) {
        if (ci != &mod) { mod = *ci; ci = &mod; }
        ml_usage_was = mod.usage;
        // colour or depth/stencil only, + TRANSIENT. KL_VK_MSAA_ML_INPUT=1 keeps INPUT_ATTACHMENT (MoltenVK then offers
        // no memoryless type for the image: types 0x1/0x3, measured 2026-10-03).
        mod.usage = (mod.usage & (0x10u | 0x20u | (kl_env_on("KL_VK_MSAA_ML_INPUT", 0) ? 0x80u : 0u))) | 0x40u;
        ml_pending = 1;
    }
    if (ci && (ci->usage & 0x80u) && kl_env_on("KL_STRIP_INPUT_ATT", 0)) {
        // No render pass in this guest uses an input attachment (census: inputAtts 0),
        // yet its temp render targets carry INPUT_ATTACHMENT usage — which makes
        // MoltenVK back them as memoryless/tile storage, so Unity's own temp-RT
        // bookkeeping cannot bind them ("temporary render texture not found") and
        // the menu compositing pass is skipped. Drop the unused bit so the RT is a
        // plain, fully-backed colour/depth target.
        mod = *ci;
        mod.usage &= ~0x80u;   // VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
        static int said; if (said < 8) { said++;
            VKI("vkCreateImage: stripped INPUT_ATTACHMENT usage %#x -> %#x (%ux%u fmt %d)\n",
                ci->usage, mod.usage, ci->extent.width, ci->extent.height, (int)ci->format); }
        ci = &mod;
    }
    // DECISIVE TEST (KL_VK_ASTC_RGBA8=1): create every ASTC image as RGBA8 instead.
    // The guest's ASTC upload then lands in an RGBA8 store (garbled bytes reinterpreted
    // as colour). If the gray WORLD turns GARBLED/COLOURFUL, the ASTC albedo was
    // reaching surfaces with valid UVs -> the gray was ASTC DECODE. If it stays gray,
    // ASTC is not the visible albedo. The matching view-format rewrite is in
    // klvk_CreateImageView (klvk_astc_sub_has). Diagnostic; off by default.
    if (ci && klvk_is_astc(ci->format) && kl_env_on("KL_VK_ASTC_RGBA8", 0)) {
        mod = *ci;
        mod.format = VK_FORMAT_R8G8B8A8_UNORM;
        // KEEP MUTABLE_FORMAT (flags 0x8): UE creates format-reinterpreted views on
        // these textures, and Metal asserts on a view unless the parent carries
        // pixelFormatView usage (which MoltenVK adds for MUTABLE images). Only drop
        // the format LIST (pNext) — it enumerates ASTC formats incompatible with the
        // RGBA8 store; a mutable image with no list allows any same-class view.
        mod.flags |= 0x8u;    // ensure MUTABLE so view creation is legal
        mod.pNext = NULL;
        ci = &mod;
        did_astc_sub = 1;
        static int said; if (said < 8) { said++;
            VKI("[test] ASTC fmt->RGBA8 substitute (%ux%u) — ASTC bytes will land as "
                "garbled colour; world colourful => gray was ASTC decode\n",
                ci->extent.width, ci->extent.height); }
    }
    // Simulator (or KL_VK_FORMAT_PRECHECK=1): the simulator's Metal device lacks
    // formats/usages the headset has, and MoltenVK then builds a texture
    // descriptor with MTLPixelFormatInvalid, which Metal ABORTS on instead of
    // returning an error (Alyx: "MTLTextureDescriptor has invalid pixelFormat
    // (0)" in MVKImagePlane::initSubresources). Ask first, and treat a "not
    // supported" as the rejection handled below — a depth format falls back to
    // plain D32, anything else to RGBA8. Off on device, where it costs a query.
    static int precheck = -1;
    if (precheck < 0) {
#if defined(TARGET_OS_SIMULATOR) && TARGET_OS_SIMULATOR
        precheck = kl_env_on("KL_VK_FORMAT_PRECHECK", 1);
#else
        precheck = kl_env_on("KL_VK_FORMAT_PRECHECK", 0);
#endif
    }
    if (precheck && ci && out) {
        klvk_device *pd = dev_find(dev);
        static PFN_vkGetPhysicalDeviceImageFormatProperties gifp;
        if (!gifp) gifp = (PFN_vkGetPhysicalDeviceImageFormatProperties)
                              real_gipa(g_instance, "vkGetPhysicalDeviceImageFormatProperties");
        VkImageFormatProperties ifp;
        VkResult fr = pd && gifp ? gifp(pd->phys, ci->format, ci->imageType, ci->tiling, ci->usage,
                                         ci->flags, &ifp) : VK_ERROR_INITIALIZATION_FAILED;
        // Too big for this device (the simulator stops at 8192; Alyx asks for
        // 8194 somewhere): clamp rather than let Metal abort.
        if (fr == VK_SUCCESS && (ci->extent.width > ifp.maxExtent.width ||
                                 ci->extent.height > ifp.maxExtent.height)) {
            static int said;
            if (said < 8) { said++;
                VKI("vkCreateImage: %ux%u (format %d) exceeds this device's %ux%u — clamped\n",
                    ci->extent.width, ci->extent.height, (int)ci->format,
                    ifp.maxExtent.width, ifp.maxExtent.height); fflush(stderr); }
            if (ci != &mod) { mod = *ci; ci = &mod; }
            if (mod.extent.width > ifp.maxExtent.width) mod.extent.width = ifp.maxExtent.width;
            if (mod.extent.height > ifp.maxExtent.height) mod.extent.height = ifp.maxExtent.height;
        }
        // A sample count the device cannot do (the simulator has no 2x MSAA):
        // single-sampled instead. Pipelines built for 2x will then mismatch,
        // which MoltenVK reports as an error rather than an abort.
        if (fr == VK_SUCCESS && !(ifp.sampleCounts & ci->samples)) {
            static int said;
            if (said < 4) { said++;
                VKI("vkCreateImage: %d samples not supported for format %d here — 1 sample\n",
                    (int)ci->samples, (int)ci->format); fflush(stderr); }
            if (ci != &mod) { mod = *ci; ci = &mod; }
            mod.samples = VK_SAMPLE_COUNT_1_BIT;
        }
        if (fr == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            static int said;
            int depth = ci->format >= VK_FORMAT_D16_UNORM && ci->format <= VK_FORMAT_D32_SFLOAT_S8_UINT;
            if (said < 16) { said++;
                VKI("vkCreateImage: format %d usage %#x flags %#x %ux%u not supported here — %s\n",
                    (int)ci->format, ci->usage, ci->flags, ci->extent.width, ci->extent.height,
                    depth ? "substituting D32_SFLOAT" : "RGBA8 fallback"); fflush(stderr); }
            if (depth) {
                mod = *ci;
                mod.format = VK_FORMAT_D32_SFLOAT;
                mod.pNext = NULL;
                mod.flags &= ~0x8u;
                ci = &mod;
            } else {
                *out = VK_NULL_HANDLE;
                goto rejected;
            }
        }
    }
    VkResult r = real(dev, ci, alloc, out);
    if (0) { rejected: r = VK_ERROR_FORMAT_NOT_SUPPORTED; }
    // MoltenVK rejects some formats outright (Wanderer: VK_FORMAT_ASTC_8x8_UNORM/
    // SRGB come back VK_ERROR_FEATURE_NOT_PRESENT) and leaves *out NULL. A null
    // image is not inert: the guest does not check, and the next call on it —
    // vkGetImageMemoryRequirements, vkBindImageMemory, vkCreateImageView — walks
    // a null `this` inside MoltenVK and crashes a render worker. Substitute a
    // real image of the same extent in a guaranteed format so the guest gets a
    // valid handle; its contents will be wrong (the ASTC upload lands in an RGBA8
    // store) but that is a wrong texture, not a dead process.
    if ((r != VK_SUCCESS || (out && !*out)) && out && ci) {
        VkImageCreateInfo fb = *ci;
        fb.pNext  = NULL;              // drop any format-list / external-memory chain
        fb.format = VK_FORMAT_R8G8B8A8_UNORM;
        fb.flags &= ~0x8u;            // MUTABLE_FORMAT: a format list would be needed
        if (fb.usage == 0) fb.usage = 0x4;                 // SAMPLED
        fb.usage &= ~0x80u;           // drop INPUT_ATTACHMENT (memoryless) just in case
        VkResult r2 = real(dev, &fb, alloc, out);
        if ((r2 != VK_SUCCESS || !*out)) {                 // last resort: single mip
            fb.mipLevels = 1;
            r2 = real(dev, &fb, alloc, out);
        }
        static int said;
        if (r2 == VK_SUCCESS && *out) {
            if (said < 16) { said++;
                VKI("vkCreateImage: format %d rejected (result %d) — substituted "
                    "RGBA8 %ux%u mips %u so the guest gets a real handle (texture "
                    "will be wrong, but no null-handle crash downstream)\n",
                    (int)ci->format, (int)r, ci->extent.width, ci->extent.height,
                    fb.mipLevels);
                fflush(stderr); }
            r = VK_SUCCESS;
        } else if (said < 16) { said++;
            VKI("vkCreateImage: format %d rejected (result %d) AND the RGBA8 "
                "substitute also failed (result %d, %ux%u) — the null-handle crash "
                "will follow; needs a closer look\n",
                (int)ci->format, (int)r, (int)r2, ci->extent.width, ci->extent.height);
            fflush(stderr);
        }
    }
    if (r == VK_SUCCESS && out) klvk_img_reg_add(*out);
    if (r == VK_SUCCESS && out && ml_pending) ml_add(*out, ml_usage_was, ci);
    if (r == VK_SUCCESS && out && ci && ci->samples > 1 && msaa_trace_on()) {
        pthread_mutex_lock(&g_msaa_mu);
        if (g_msaa_nimg < kMsaaImgs) {
            int k = g_msaa_nimg++;
            g_msaa_imgs[k].img = *out; g_msaa_imgs[k].w = ci->extent.width; g_msaa_imgs[k].h = ci->extent.height;
            g_msaa_imgs[k].layers = ci->arrayLayers; g_msaa_imgs[k].samples = ci->samples;
            g_msaa_imgs[k].fmt = (uint32_t)ci->format; g_msaa_imgs[k].usage = ci->usage;
        }
        pthread_mutex_unlock(&g_msaa_mu);
        fprintf(stderr, "[msaa] image %#llx %ux%u layers %u samples %u fmt %d usage %#x%s flags %#x\n",
                (unsigned long long)(uintptr_t)*out, ci->extent.width, ci->extent.height, ci->arrayLayers,
                (unsigned)ci->samples, (int)ci->format, ci->usage, (ci->usage & 0x40u) ? " (TRANSIENT)" : "", ci->flags);
    }
    if (r == VK_SUCCESS && out && xftrace_on() && ci && ci->format == VK_FORMAT_R32G32B32A32_SFLOAT &&
        ci->imageType == VK_IMAGE_TYPE_2D && g_nxf < 16) {
        g_xf_img[g_nxf] = *out; g_xf_w[g_nxf] = ci->extent.width; g_xf_h[g_nxf] = ci->extent.height;
        fprintf(stderr, "[xform] RGBA32F image #%d %ux%u mips %u layers %u usage %#x tiling %d\n", g_nxf, ci->extent.width,
                ci->extent.height, ci->mipLevels, ci->arrayLayers, ci->usage, (int)ci->tiling);
        g_nxf++;
    }
    if (r == VK_SUCCESS && out && ci)
        klvk_img_info_add(*out, ci->format, ci->extent.width, ci->extent.height);
    if (r == VK_SUCCESS && out && *out && did_astc_sub) klvk_astc_sub_add(*out);
    if (klvk_trace() && ci && (ci->extent.width >= 480 || ci->extent.height >= 480)
        && (ci->usage & 0x30u))   // colour or depth attachment == a render target
        fprintf(stderr, "[TRACE] [thr] createRT %#llx %ux%u fmt %d usage %#x tid=%u\n",
            (unsigned long long)(uintptr_t)(out?*out:0), ci->extent.width, ci->extent.height,
            (int)ci->format, ci->usage, klvk_tid());
    static int logged;
    if (logged < 4096 && (r != VK_SUCCESS || (ci && ci->mipLevels > 1) || logged < 64)) {
        logged++;
        VKI("vkCreateImage: -> %#llx result %d type %d format %d %ux%u mips %u layers %u "
            "usage %#x flags %#x\n",
            (unsigned long long)(uintptr_t)(out ? *out : 0), (int)r,
            ci ? (int)ci->imageType : -1, ci ? (int)ci->format : -1,
            ci ? ci->extent.width : 0, ci ? ci->extent.height : 0,
            ci ? ci->mipLevels : 0, ci ? ci->arrayLayers : 0,
            ci ? ci->usage : 0, ci ? ci->flags : 0);
    }
    return r;
}

// vkGetImageMemoryRequirements with a guard for VK_NULL_HANDLE. When a
// vkCreateImage fails (Wanderer: VK_FORMAT_ASTC_8x8_UNORM_BLOCK comes back
// VK_ERROR_FEATURE_NOT_PRESENT from MoltenVK), UE5 does not check the result and
// asks the null image for its requirements; MoltenVK then dereferences the null
// this (MVKImage::getMemoryRequirements, fault at +0x20). Answer a small dummy
// requirement instead of forwarding the null, so a rejected format degrades to a
// broken texture rather than a hard crash on a render worker.
static void VKAPI_CALL klvk_GetImageMemoryRequirements(VkDevice dev, VkImage img,
        VkMemoryRequirements *mr) {
    static PFN_vkGetImageMemoryRequirements real;
    if (!real && real_gdpa)
        real = (PFN_vkGetImageMemoryRequirements)real_gdpa(dev, "vkGetImageMemoryRequirements");
    if (!img) {
        if (mr) { mr->size = 256; mr->alignment = 256; mr->memoryTypeBits = ~0u; }
        static int said;
        if (said < 8) { said++;
            VKI("vkGetImageMemoryRequirements(NULL image) — a prior vkCreateImage "
                "failed (unsupported format?); returning a dummy requirement rather "
                "than letting MoltenVK dereference the null handle\n"); }
        return;
    }
    if (real) real(dev, img, mr);
    if (mr && msaa_ml_on() && g_ml_nimg && ml_has(img)) ml_note_req(dev, img, mr);
}
static int ml_lazy_type(VkDevice dev, uint32_t bits, int say) {
    static PFN_vkGetPhysicalDeviceMemoryProperties gmp;
    if (!gmp) gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    klvk_device *pd = dev_find(dev);
    if (!pd || !gmp) return -1;
    VkPhysicalDeviceMemoryProperties mp; gmp(pd->phys, &mp);
    int lazy = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (say) fprintf(stderr, "[ml] memory type %u: heap %u props %#x%s\n", i, mp.memoryTypes[i].heapIndex,
                         mp.memoryTypes[i].propertyFlags, (bits >> i) & 1 ? " (the image accepts it)" : "");
        if (lazy < 0 && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) && ((bits >> i) & 1)) lazy = (int)i;
    }
    return lazy;
}
static void ml_note_req(VkDevice dev, VkImage img, const VkMemoryRequirements *mr) {
    static int said;
    int lazy = ml_lazy_type(dev, mr->memoryTypeBits, !said++);
    pthread_mutex_lock(&g_msaa_mu);
    int k = -1;
    for (int i = 0; i < g_ml_nreq; i++) if (g_ml_req[i].img == img) k = i;
    if (k < 0 && g_ml_nreq < kMlImgs) k = g_ml_nreq++;
    if (k >= 0) { g_ml_req[k].img = img; g_ml_req[k].size = mr->size; g_ml_req[k].bits = mr->memoryTypeBits; g_ml_req[k].open = lazy >= 0; }
    pthread_mutex_unlock(&g_msaa_mu);
    fprintf(stderr, "[ml] image %#llx needs %.1f MB, types %#x: %s\n", (unsigned long long)(uintptr_t)img,
            mr->size / 1048576.0, mr->memoryTypeBits, lazy >= 0 ? "the next allocation of that size goes memoryless" :
            "no LAZILY_ALLOCATED type among them; stays in real memory");
}

// A thin pass-through over MoltenVK's vkCreateImageView that LOGS the guest's
// request first. The guest (Unity's Vulkan renderer) makes these on the eye
// images this file hands out through kl_ovrp; when MoltenVK faults building one,
// the only way to see WHICH view it asked for - format, type, subresource - is
// here, because nothing else on the path records it. It forwards verbatim to the
// real entry point resolved from MoltenVK and changes no behaviour of its own.
// A valid stand-in for the guest's WILD image handles - AC Nexus's loading
// video texture (an Android-surface import that cannot exist on visionOS). The
// guest builds a vkCreateImageView on it every frame; refusing returned an
// error that aborted the guest's whole frame, so it never reached EndFrame4 and
// the menu never rendered. Handing back a view on THIS image instead lets the
// frame finish - the video samples/renders black (the real video is shown by
// the host loading skybox), and the wild pointer never reaches MoltenVK. Big
// enough (4096, 12 mips, 2 layers, mutable format) that any view the guest asks
// for fits, and any framebuffer it sizes to its own texture is <= this.
#define KLVK_SCRATCH_MIPS   12u
#define KLVK_SCRATCH_LAYERS 2u
static VkImage klvk_wild_scratch(void) {
    if (!g_wild_scratch) {
        klvk_device *d = g_devs[0];
        if (!d) return VK_NULL_HANDLE;
        g_wild_scratch = klvk_image_alloc(d, VK_FORMAT_R8G8B8A8_UNORM,
            4096, 4096, KLVK_SCRATCH_LAYERS, KLVK_SCRATCH_MIPS,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
            NULL);
        if (g_wild_scratch)
            VKI("created 4096x4096 scratch image %#llx for wild-handle view "
                "substitution\n", (unsigned long long)(uintptr_t)g_wild_scratch);
    }
    return g_wild_scratch;
}

// Map a WILD (unregistered) image handle to the scratch image so a command
// referencing it succeeds against a real resource instead of being dropped.
// Registered handles pass through unchanged.
static VkImage klvk_sub_wild(VkImage img) {
    if (!img || klvk_img_reg_has(img)) return img;
    VkImage sc = klvk_wild_scratch();
    return sc ? sc : img;
}

static void VKAPI_CALL klvk_DestroyImage(VkDevice dev, VkImage img,
        const VkAllocationCallbacks *alloc) {
    static PFN_vkDestroyImage real;
    if (!real && real_gdpa) real = (PFN_vkDestroyImage)real_gdpa(dev, "vkDestroyImage");
    klvk_img_reg_del(img);
    klvk_img_info_del(img);
    klvk_shadow_forget(img);
    if (img && msaa_trace_on()) {            // which multisampled images the game lets go (e.g. after a vr_msaa change)
        pthread_mutex_lock(&g_msaa_mu);
        for (int i = 0; i < g_msaa_nimg; i++) if (g_msaa_imgs[i].img == img) {
            fprintf(stderr, "[msaa] destroy image %#llx %ux%u layers %u samples %u fmt %u\n", (unsigned long long)(uintptr_t)img,
                    g_msaa_imgs[i].w, g_msaa_imgs[i].h, g_msaa_imgs[i].layers, g_msaa_imgs[i].samples, g_msaa_imgs[i].fmt);
            g_msaa_imgs[i] = g_msaa_imgs[--g_msaa_nimg];
            break;
        }
        pthread_mutex_unlock(&g_msaa_mu);
    }
    if (real) real(dev, img, alloc);
}

static VkResult VKAPI_CALL klvk_CreateImageView(VkDevice dev,
        const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkImageView *out) {
    static PFN_vkCreateImageView real;
    if (!real && real_gdpa)
        real = (PFN_vkCreateImageView)real_gdpa(dev, "vkCreateImageView");
    if (ci) {
        static int logged;
        if (logged < 4096) {   // high enough to capture the crashing view
            logged++;
            const VkImageSubresourceRange *r = &ci->subresourceRange;
            VKI("vkCreateImageView: image %#llx viewType %d format %d aspect %#x "
                "mips %u+%u layers %u+%u createFlags %#x\n",
                (unsigned long long)(uintptr_t)ci->image, (int)ci->viewType,
                (int)ci->format, r->aspectMask, r->baseMipLevel, r->levelCount,
                r->baseArrayLayer, r->layerCount, ci->flags);
        }
    }
    if (ci && ci->image && !klvk_img_reg_ok(ci->image)) {
        VkImage scratch = klvk_wild_scratch();
        if (scratch && real) {
            VkImageViewCreateInfo sub = *ci;
            sub.image = scratch;
            VkImageSubresourceRange *r = &sub.subresourceRange;
            if (r->baseMipLevel >= KLVK_SCRATCH_MIPS) r->baseMipLevel = 0;
            if (r->levelCount != VK_REMAINING_MIP_LEVELS &&
                r->baseMipLevel + r->levelCount > KLVK_SCRATCH_MIPS)
                r->levelCount = KLVK_SCRATCH_MIPS - r->baseMipLevel;
            if (r->baseArrayLayer >= KLVK_SCRATCH_LAYERS) r->baseArrayLayer = 0;
            if (r->layerCount != VK_REMAINING_ARRAY_LAYERS &&
                r->baseArrayLayer + r->layerCount > KLVK_SCRATCH_LAYERS)
                r->layerCount = KLVK_SCRATCH_LAYERS - r->baseArrayLayer;
            static int subbed;
            if (subbed < 16) {
                subbed++;
                VKI("vkCreateImageView: SUBSTITUTING scratch for wild handle "
                    "%#llx (viewType %d format %d) so the frame can complete\n",
                    (unsigned long long)(uintptr_t)ci->image, (int)ci->viewType,
                    (int)ci->format);
            }
            VkResult rr = real(dev, &sub, alloc, out);
            if (rr != VK_SUCCESS && out) *out = VK_NULL_HANDLE;
            else if (out) klvk_view_map_add(*out, ci->image);
            return rr;
        }
        VKI("vkCreateImageView: REFUSING a view on image %#llx - no create ever\n"
            "        produced that handle, and no scratch to substitute. "
            "viewType %d format %d mips %u+%u\n",
            (unsigned long long)(uintptr_t)ci->image, (int)ci->viewType,
            (int)ci->format, ci->subresourceRange.baseMipLevel,
            ci->subresourceRange.levelCount);
        if (out) *out = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    // KL_VK_ASTC_RGBA8 test: every ASTC image was created RGBA8, so ANY ASTC-format
    // view must be rewritten to match the store or Metal asserts. Keyed on the flag +
    // the requested format (not per-image tracking, which overflowed — olar has 500+
    // ASTC images).
    VkImageViewCreateInfo ivmod;
    if (ci && klvk_is_astc(ci->format) && kl_env_on("KL_VK_ASTC_RGBA8", 0)) {
        ivmod = *ci; ivmod.format = VK_FORMAT_R8G8B8A8_UNORM; ci = &ivmod;
    }
    VkResult klvk_ivr = real(dev, ci, alloc, out);
    if (klvk_ivr==VK_SUCCESS && out && ci) klvk_view_map_add(*out, ci->image);
    return klvk_ivr;
}

// ---------------------------------------------------------------------------
// Pipeline-compile instrumentation. The guest's ShaderWarmupManager compiles its
// whole variant collection here, and on this stack every one goes through
// MoltenVK's SPIR-V->Metal compiler, which is far slower than a native driver.
// From the outside a warmup that is grinding and one that is wedged look
// identical - a frozen loading screen - so this counts and TIMES the compiles
// and logs a running total. A steadily climbing count is "slow, be patient";
// a count that stops while the app is still alive is "wedged, look here".
// Gated on nothing: it is one fprintf per ~50 pipelines, negligible against a
// compile measured in tens of milliseconds.
static unsigned long long g_pipe_gfx, g_pipe_comp, g_pipe_ns;
static double kl_pipe_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
// Which thread the game compiles on, and how many compiles overlap: a compile on
// the render thread is a frame stall, one on a worker is not.
static int g_pipe_inflight, g_pipe_maxinflight;
// This thread's last guest graphics create, per pipeline (for the slow-pipeline log, KL_VK_FOZ_SLOW_MS).
static __thread double t_pipe_last_ms;
static void kl_pipe_report(const char *kind, unsigned n, double dt_ms) {
    g_pipe_ns += (unsigned long long)(dt_ms * 1e6);
    unsigned long long tot = g_pipe_gfx + g_pipe_comp;
    // Slow guest batches: a warm Metal cache answers in a few ms, a cold compile
    // takes 100+ (330-580 ms on the headset), so these count the misses.
    static unsigned over50, over100;
    static double slowest;
    if (dt_ms > 50.0) over50++;
    if (dt_ms > 100.0) over100++;
    if (dt_ms > slowest) slowest = dt_ms;
    static unsigned long long last;
    if (tot - last >= 50 || dt_ms > 100.0) {
        last = tot;
        char tn[64] = "";
        pthread_getname_np(pthread_self(), tn, sizeof tn);
        VKI("pipeline warmup: %llu graphics + %llu compute = %llu compiled, "
            "%.1f s in the compiler so far (last %s batch of %u took %.0f ms, thread '%s', max %d at once; "
            "%u batches over 50 ms, %u over 100 ms, slowest %.0f ms)\n",
            g_pipe_gfx, g_pipe_comp, tot, (double)g_pipe_ns / 1e9, kind, n, dt_ms, tn, g_pipe_maxinflight,
            over50, over100, slowest);
    }
}
static VkResult VKAPI_CALL klvk_CreateGraphicsPipelines_body(VkDevice dev,
        VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkPipeline *out) {
    static PFN_vkCreateGraphicsPipelines real;
    if (!real && real_gdpa)
        real = (PFN_vkCreateGraphicsPipelines)real_gdpa(dev, "vkCreateGraphicsPipelines");

    // KL_VK_FIX_PRIMRESTART (default ON): Metal cannot DISABLE primitive restart
    // for strip/fan topologies, so MoltenVK rejects a strip pipeline that sets
    // primitiveRestartEnable=FALSE with VK_ERROR_FEATURE_NOT_PRESENT and leaves a
    // NULL handle — and every draw that binds it then renders NOTHING (invisible
    // strip geometry: landscape, decals, ribbons, some particle/UI meshes). The
    // olar log shows exactly this ("[mvk-warn] ... Metal does not support
    // disabling primitive restart"). Metal always has restart ON, so forcing it
    // TRUE makes the pipeline valid; the only behavioural change is that a
    // max-value index (0xFFFF/0xFFFFFFFF) in strip data restarts the primitive —
    // which is precisely what that reserved index means. List topologies ignore
    // the flag and MoltenVK accepts them, so they are left untouched.
    // Multisample census (KL_VK_MS_CENSUS=1): how many pipelines render with
    // MSAA, and how many ask for alpha-to-coverage (Source 2 foliage) - the
    // evidence for "is MSAA / A2C actually in effect through MoltenVK".
    {
        static int census = -1;
        static unsigned total, ms, a2c, a2c_ms1, a2o;
        if (census < 0) census = kl_env_on("KL_VK_MS_CENSUS", 0);
        if (census && ci) {
            for (uint32_t i = 0; i < n; i++) {
                const VkPipelineMultisampleStateCreateInfo *m = ci[i].pMultisampleState;
                total++;
                if (!m) continue;
                if (m->rasterizationSamples > 1) ms++;
                if (m->alphaToCoverageEnable) { a2c++; if (m->rasterizationSamples <= 1) a2c_ms1++; }
                if (m->alphaToOneEnable) a2o++;
            }
            if ((total & (total - 1)) == 0 || total % 256 < n)
                VKI("pipeline census: %u graphics pipelines, %u multisampled, %u alpha-to-coverage "
                    "(%u of them single-sampled), %u alpha-to-one\n", total, ms, a2c, a2c_ms1, a2o);
        }
    }
    VkGraphicsPipelineCreateInfo *mod = NULL;
    VkPipelineInputAssemblyStateCreateInfo *ias = NULL;
    int fixed = 0;
    static int fixon = -1;
    if (fixon < 0) fixon = kl_env_on("KL_VK_FIX_PRIMRESTART", 1);
    if (fixon && ci && n) {
        for (uint32_t i = 0; i < n; i++) {
            const VkPipelineInputAssemblyStateCreateInfo *a = ci[i].pInputAssemblyState;
            if (!a || a->primitiveRestartEnable) continue;
            VkPrimitiveTopology t = a->topology;
            if (t != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP &&
                t != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP &&
                t != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN &&
                t != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY &&
                t != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY)
                continue;
            if (!mod) {
                mod = malloc((size_t)n * sizeof *mod);
                ias = malloc((size_t)n * sizeof *ias);
                if (!mod || !ias) { free(mod); free(ias); mod = NULL; ias = NULL; break; }
                memcpy(mod, ci, (size_t)n * sizeof *mod);
            }
            ias[i] = *a;
            ias[i].primitiveRestartEnable = VK_TRUE;
            mod[i].pInputAssemblyState = &ias[i];
            fixed++;
        }
    }

    // KL_VK_PIPE_TRACE (default ON for wrath2): one UE4 warmup pipeline wedges
    // MoltenVK's Metal compiler forever (MVKMetalCompiler::compile never returns),
    // and the existing warmup counter only logs on EXIT, so the hung pipeline is
    // invisible. Log an ENTER (flushed) before real() and a DONE after; the last
    // ENTER with no matching DONE names the wedging pipeline and its shape.
    static int pipe_trace = -1;
    if (pipe_trace < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        pipe_trace = kl_env_on("KL_VK_PIPE_TRACE", t && !strcmp(t, "wrath2"));
    }
    static unsigned long long g_pipe_enter;
    const VkGraphicsPipelineCreateInfo *use = mod ? mod : ci;
    if (pipe_trace && use) {
        for (uint32_t i = 0; i < n; i++) {
            unsigned long long idx = __atomic_add_fetch(&g_pipe_enter, 1, __ATOMIC_RELAXED);
            unsigned smask = 0;
            for (uint32_t s = 0; s < use[i].stageCount && use[i].pStages; s++)
                smask |= use[i].pStages[s].stage;
            VKI("pipe-trace: ENTER gfx #%llu stages=%u stagemask=0x%x tess=%d subpass=%u\n",
                idx, use[i].stageCount, smask, use[i].pTessellationState ? 1 : 0, use[i].subpass);
        }
        fflush(stderr);
    }
    double t0 = kl_pipe_ms();
    int inflight = __atomic_add_fetch(&g_pipe_inflight, 1, __ATOMIC_RELAXED);
    if (inflight > g_pipe_maxinflight) g_pipe_maxinflight = inflight;
    VkResult r = real ? real(dev, cache, n, use, alloc, out)
                      : VK_ERROR_INITIALIZATION_FAILED;
    __atomic_sub_fetch(&g_pipe_inflight, 1, __ATOMIC_RELAXED);
    if (pipe_trace) { VKI("pipe-trace: DONE through #%llu (r=%d)\n", g_pipe_enter, (int)r); fflush(stderr); }
    free(mod); free(ias);
    if (fixed) { static int said; if (said < 8) { said++;
        VKI("CreateGraphicsPipelines: forced primitiveRestartEnable=TRUE on %d "
            "strip pipeline(s) MoltenVK would otherwise reject (invisible "
            "geometry)\n", fixed); } }
    if (!t_pipe_replay) {
        g_pipe_gfx += n;
        t_pipe_last_ms = (kl_pipe_ms() - t0) / (n ? n : 1);
        kl_pipe_report("gfx", n, t_pipe_last_ms * (n ? n : 1));
    }
    return r;
}
static VkResult VKAPI_CALL klvk_CreateComputePipelines_body(VkDevice dev,
        VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *ci,
        const VkAllocationCallbacks *alloc, VkPipeline *out) {
    static PFN_vkCreateComputePipelines real;
    if (!real && real_gdpa)
        real = (PFN_vkCreateComputePipelines)real_gdpa(dev, "vkCreateComputePipelines");
    double t0 = kl_pipe_ms();
    VkResult r = real ? real(dev, cache, n, ci, alloc, out)
                      : VK_ERROR_INITIALIZATION_FAILED;
    if (!t_pipe_replay) {
        g_pipe_comp += n;
        kl_pipe_report("compute", n, kl_pipe_ms() - t0);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
typedef struct { const char *name; PFN_vkVoidFunction fn; } entry;

// --- KL_TRACE: framebuffer + render-pass census (what image is rendered INTO) ---
#define KLVK_FB_MAP_MAX 4096
static struct { VkFramebuffer fb; VkImage img0; unsigned att; } g_fb_map[KLVK_FB_MAP_MAX];
static int g_fb_map_n;
static pthread_mutex_t g_fb_map_lock = PTHREAD_MUTEX_INITIALIZER;
static VkImage klvk_fb_image0(VkFramebuffer fb) {
    VkImage img = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_fb_map_lock);
    for (int i = g_fb_map_n - 1; i >= 0; i--) if (g_fb_map[i].fb == fb) { img = g_fb_map[i].img0; break; }
    pthread_mutex_unlock(&g_fb_map_lock);
    return img;
}
static VkResult VKAPI_CALL klvk_CreateFramebuffer(VkDevice dev,
        const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a,
        VkFramebuffer *out) {
    static PFN_vkCreateFramebuffer real;
    if (!real) real = (PFN_vkCreateFramebuffer)mvk_sym("vkCreateFramebuffer");
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && ci && out) {
        VkImage img0 = VK_NULL_HANDLE;
        if (ci->attachmentCount && ci->pAttachments) img0 = klvk_view_image(ci->pAttachments[0]);
        pthread_mutex_lock(&g_fb_map_lock);
        if (g_fb_map_n < KLVK_FB_MAP_MAX) {
            g_fb_map[g_fb_map_n].fb = *out; g_fb_map[g_fb_map_n].img0 = img0;
            g_fb_map[g_fb_map_n].att = ci->attachmentCount; g_fb_map_n++;
        }
        pthread_mutex_unlock(&g_fb_map_lock);
        // KL_RT_DEBUG: remember the latest substantial UI render target (the guest
        // renders its menu pieces into these; the compositing that would gather
        // them into the submitted panel fails, so this is where the pixels are).
        if (img0 && !klvk_is_eye_image(img0) && ci->width >= 480 && ci->height >= 240) {
            uint32_t area = ci->width * ci->height;
            if (area >= g_content_rt_area) {   // keep the biggest — the main menu panel is the most stable
                g_content_rt = img0; g_content_rt_w = ci->width; g_content_rt_h = ci->height; g_content_rt_area = area;
            }
        }
        if (klvk_trace()) {
            klvk_trace_log("[fb] create %#llx att=%u %ux%u\n",
                (unsigned long long)(uintptr_t)*out, ci->attachmentCount, ci->width, ci->height);
            for (uint32_t i = 0; i < ci->attachmentCount && ci->pAttachments; i++) {
                VkImage im = klvk_view_image(ci->pAttachments[i]);
                klvk_trace_log("[fb]   att%u view %#llx -> img %#llx (%s k=%d)\n",
                    i, (unsigned long long)(uintptr_t)ci->pAttachments[i],
                    (unsigned long long)(uintptr_t)im, klvk_img_tag(im), klvk_layer_image_key(im));
            }
        }
    }
    return r;
}
// --- wanderer eye-resolve discriminator + MSAA-store candidate fix ---
// The eye scene pass is a standard MSAA MULTIVIEW resolve (att0 MSAA colour ->
// att1 single-sample eye array layer, viewMask 0x3) yet the eye reads black; the
// resolve-target storeOp is already STORE, so that is not the lever. Two gates,
// both default-ON for wanderer, both env-overridable:
//  - KL_VK_EYE_RESOLVE_PROBE: bright-magenta CLEAR the eye resolve target so the
//    next run reads out "resolve never landed" (stays magenta) vs "scene empty"
//    (goes black) vs "fixed" (renders).
//  - KL_VK_MSAA_STORE: force the MSAA colour SOURCE (att0) storeOp DONT_CARE->STORE.
//    A memoryless DONT_CARE MSAA source resolves via a bare MultisampleResolve store
//    action; on affected MoltenVK the per-slice resolve into a non-zero multiview
//    array layer does not fire in that config — promoting to StoreAndMultisample
//    Resolve gives it real backing. Costs bandwidth only; safe.
// Wanderer begins its eye pass with vkCmdBeginRenderPass2KHR (NOT the v1 entry),
// so the clear injection MUST hook the v2 begin; the v1 hook is kept for symmetry.
static int klvk_is_wanderer(void) {
    extern const char *kl_driver_target_name(void);
    const char *t = kl_driver_target_name();
    return t && strcmp(t, "wanderer") == 0;
}
static int klvk_eye_probe_on(void) {
    static int v = -1;
    // Default OFF now: the probe already did its job — it came back BLACK (not
    // magenta) on wanderer, proving the multiview MSAA resolve DOES land and the
    // black was empty content (the MoltenVK argument-buffer material-compile bug,
    // fixed via KL_MVK_NO_ARGBUF in vk_init). Leaving it default-on would magenta-
    // clear the eye and mask the now-rendering world. Still armable via the env.
    if (v < 0) v = kl_env_on("KL_VK_EYE_RESOLVE_PROBE", 0);
    return v;
}
static int klvk_msaa_store_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_VK_MSAA_STORE", klvk_is_wanderer());
    return v;
}
#define KLVK_CLEARPASS_MAX 128
static struct { VkRenderPass rp; uint32_t att; } g_clearpass[KLVK_CLEARPASS_MAX];
static int g_clearpass_n;
static pthread_mutex_t g_clearpass_lock = PTHREAD_MUTEX_INITIALIZER;
static void klvk_clearpass_add(VkRenderPass rp, uint32_t att) {
    if (!rp) return;
    pthread_mutex_lock(&g_clearpass_lock);
    if (g_clearpass_n < KLVK_CLEARPASS_MAX) { g_clearpass[g_clearpass_n].rp = rp;
        g_clearpass[g_clearpass_n].att = att; g_clearpass_n++; }
    pthread_mutex_unlock(&g_clearpass_lock);
}
static int klvk_clearpass_lookup(VkRenderPass rp, uint32_t *att) {
    int found = 0; if (!rp) return 0;
    pthread_mutex_lock(&g_clearpass_lock);
    for (int i = g_clearpass_n - 1; i >= 0; i--)
        if (g_clearpass[i].rp == rp) { if (att) *att = g_clearpass[i].att; found = 1; break; }
    pthread_mutex_unlock(&g_clearpass_lock);
    return found;
}
static VkClearValue *klvk_probe_inject_clears(const VkRenderPassBeginInfo *bi,
                                              VkRenderPassBeginInfo *tmp) {
    uint32_t att = 0;
    if (!bi || !klvk_clearpass_lookup(bi->renderPass, &att)) return NULL;
    uint32_t n = bi->clearValueCount > att + 1 ? bi->clearValueCount : att + 1;
    VkClearValue *cv = calloc(n, sizeof *cv);
    if (!cv) return NULL;
    if (bi->clearValueCount && bi->pClearValues)
        memcpy(cv, bi->pClearValues, bi->clearValueCount * sizeof *cv);
    cv[att].color.float32[0] = 1.0f; cv[att].color.float32[1] = 0.0f;   // bright magenta
    cv[att].color.float32[2] = 1.0f; cv[att].color.float32[3] = 1.0f;
    *tmp = *bi; tmp->clearValueCount = n; tmp->pClearValues = cv;
    return cv;
}
static void VKAPI_CALL klvk_CmdBeginRenderPass(VkCommandBuffer cb,
        const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    static PFN_vkCmdBeginRenderPass real;
    if (!real) real = (PFN_vkCmdBeginRenderPass)mvk_sym("vkCmdBeginRenderPass");
    if (klvk_trace() && bi && klvk_trace_once((unsigned long long)(uintptr_t)bi->framebuffer,(unsigned long long)(uintptr_t)klvk_fb_image0(bi->framebuffer))) {
        VkImage im = klvk_fb_image0(bi->framebuffer);
        klvk_trace_log("[rp] begin fb %#llx -> att0 img %#llx (%s k=%d) area %ux%u\n",
            (unsigned long long)(uintptr_t)bi->framebuffer,
            (unsigned long long)(uintptr_t)im, klvk_img_tag(im), klvk_layer_image_key(im),
            bi->renderArea.extent.width, bi->renderArea.extent.height);
    }
    VkRenderPassBeginInfo tmp; VkClearValue *inj = klvk_probe_inject_clears(bi, &tmp);
    if (real) real(cb, inj ? &tmp : bi, c);
    free(inj);
}
static void VKAPI_CALL klvk_CmdBeginRenderPass2(VkCommandBuffer cb,
        const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *sbi) {
    static PFN_vkCmdBeginRenderPass2 real;
    if (!real) { real = (PFN_vkCmdBeginRenderPass2)mvk_sym("vkCmdBeginRenderPass2");
        if (!real) real = (PFN_vkCmdBeginRenderPass2)mvk_sym("vkCmdBeginRenderPass2KHR"); }
    if (klvk_trace() && bi && klvk_trace_once((unsigned long long)(uintptr_t)bi->framebuffer,(unsigned long long)(uintptr_t)klvk_fb_image0(bi->framebuffer))) {
        VkImage im = klvk_fb_image0(bi->framebuffer);
        klvk_trace_log("[rp2] begin fb %#llx -> att0 img %#llx (%s k=%d) area %ux%u\n",
            (unsigned long long)(uintptr_t)bi->framebuffer,
            (unsigned long long)(uintptr_t)im, klvk_img_tag(im), klvk_layer_image_key(im),
            bi->renderArea.extent.width, bi->renderArea.extent.height);
    }
    VkRenderPassBeginInfo tmp; VkClearValue *inj = klvk_probe_inject_clears(bi, &tmp);
    if (real) real(cb, inj ? &tmp : bi, sbi);
    free(inj);
}

// KL_TRACE: render-pass census. Logs each render pass's subpass structure and,
// always, any that MoltenVK REJECTS. AC Nexus's menu compositing target reaches
// vkCreateImage but never a framebuffer/render pass ("temporary render texture
// not found"); this shows whether the passes that DO get built use input
// attachments / multiple subpasses (the MoltenVK-thorny shapes) and whether any
// creation fails outright.
static VkResult VKAPI_CALL klvk_CreateRenderPass_body(VkDevice dev,
        const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a,
        VkRenderPass *out) {
    static PFN_vkCreateRenderPass real;
    if (!real && real_gdpa) real = (PFN_vkCreateRenderPass)real_gdpa(dev, "vkCreateRenderPass");
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (ci && (r != VK_SUCCESS || klvk_trace())) {
        unsigned inputs = 0;
        for (uint32_t i = 0; i < ci->subpassCount; i++)
            inputs += ci->pSubpasses[i].inputAttachmentCount;
        fprintf(stderr, "[TRACE] [rpass] create -> %#llx result %d atts %u subpasses %u inputAtts %u\n",
            (unsigned long long)(uintptr_t)(out ? *out : 0), (int)r,
            ci->attachmentCount, ci->subpassCount, inputs);
        for (uint32_t i = 0; i < ci->attachmentCount; i++)
            fprintf(stderr, "[TRACE] [rpass]   att[%u] format %d samples %d loadOp %d storeOp %d\n",
                i, (int)ci->pAttachments[i].format, (int)ci->pAttachments[i].samples,
                (int)ci->pAttachments[i].loadOp, (int)ci->pAttachments[i].storeOp);
        for (uint32_t sp = 0; sp < ci->subpassCount; sp++) {
            const VkSubpassDescription *d = &ci->pSubpasses[sp];
            fprintf(stderr, "[TRACE] [rpass]   subpass %u: color=%u input=%u depth=%s\n",
                sp, d->colorAttachmentCount, d->inputAttachmentCount,
                d->pDepthStencilAttachment ? "yes" : "no");
            for (uint32_t k = 0; k < d->inputAttachmentCount; k++)
                fprintf(stderr, "[TRACE] [rpass]     input[%u] att=%u layout=%d\n",
                    k, d->pInputAttachments[k].attachment, (int)d->pInputAttachments[k].layout);
        }
    }
    return r;
}
static VkResult VKAPI_CALL klvk_CreateRenderPass2_body(VkDevice dev,
        const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a,
        VkRenderPass *out) {
    static PFN_vkCreateRenderPass2 real;
    if (!real && real_gdpa) {
        real = (PFN_vkCreateRenderPass2)real_gdpa(dev, "vkCreateRenderPass2");
        if (!real) real = (PFN_vkCreateRenderPass2)real_gdpa(dev, "vkCreateRenderPass2KHR");
    }
    // Force STORE on every MSAA resolve target. UE5 (wanderer) resolves its
    // multiview scene colour into the eye image but marks that resolve attachment
    // storeOp=DONT_CARE — legal on a Quest tiler where the resolve write survives,
    // but on Metal/MoltenVK a DONT_CARE resolve attachment maps to a store action
    // that DISCARDS the resolved pixels, leaving the eye image black. Rewriting the
    // resolve target's storeOp to STORE makes MoltenVK write it back. Safe for
    // other titles: a resolve target you keep is exactly one you meant to store, so
    // this only ever costs a writeback that was already intended. KL_VK_RESOLVE_STORE=0
    // opts out. Modifies a COPY; the guest's arrays stay untouched.
    VkAttachmentDescription2 *atts = NULL;
    VkRenderPassCreateInfo2 ci2;
    int probe = klvk_eye_probe_on();
    int msaastore = klvk_msaa_store_on();
    int resolve_store = kl_env_on("KL_VK_RESOLVE_STORE", 1);
    uint32_t probe_att = VK_ATTACHMENT_UNUSED;         // eye resolve target we CLEARed
    if (ci && ci->attachmentCount && (resolve_store || probe || msaastore)) {
        int forced = 0, msaa_forced = 0;
        atts = malloc(ci->attachmentCount * sizeof *atts);
        if (atts) {
            memcpy(atts, ci->pAttachments, ci->attachmentCount * sizeof *atts);
            for (uint32_t sp = 0; sp < ci->subpassCount; sp++) {
                const VkSubpassDescription2 *d = &ci->pSubpasses[sp];
                int mv = d->viewMask != 0;              // multiview = the eye scene pass
                for (uint32_t k = 0; d->pResolveAttachments &&
                                     k < d->colorAttachmentCount; k++) {
                    uint32_t ai  = d->pResolveAttachments[k].attachment;                 // resolve target = eye
                    uint32_t src = d->pColorAttachments ? d->pColorAttachments[k].attachment
                                                        : VK_ATTACHMENT_UNUSED;          // MSAA source
                    if (resolve_store && ai < ci->attachmentCount &&
                        atts[ai].storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) {
                        atts[ai].storeOp = VK_ATTACHMENT_STORE_OP_STORE; forced++;
                    }
                    if (mv && probe && ai < ci->attachmentCount) {
                        atts[ai].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; probe_att = ai;   // discriminator
                    }
                    if (mv && msaastore && src < ci->attachmentCount &&
                        atts[src].samples != VK_SAMPLE_COUNT_1_BIT &&
                        atts[src].storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE) {
                        atts[src].storeOp = VK_ATTACHMENT_STORE_OP_STORE; msaa_forced++; // candidate fix
                    }
                }
            }
            if (forced || msaa_forced || probe_att != VK_ATTACHMENT_UNUSED) {
                ci2 = *ci; ci2.pAttachments = atts; ci = &ci2;
                if (forced)
                    VKI("CreateRenderPass2: forced STORE on %d resolve target(s) "
                        "(was DONT_CARE — Metal would discard the resolve)\n", forced);
                if (probe_att != VK_ATTACHMENT_UNUSED)
                    VKI("CreateRenderPass2: wanderer eye-resolve PROBE armed — att%u "
                        "loadOp->CLEAR (bright magenta); MSAA-source STORE forced on %d att(s)\n",
                        probe_att, msaa_forced);
            } else { free(atts); atts = NULL; }
        }
    }
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && out && probe_att != VK_ATTACHMENT_UNUSED)
        klvk_clearpass_add(*out, probe_att);
    if (ci && (r != VK_SUCCESS || klvk_trace())) {
        unsigned inputs = 0;
        for (uint32_t i = 0; i < ci->subpassCount; i++)
            inputs += ci->pSubpasses[i].inputAttachmentCount;
        fprintf(stderr, "[TRACE] [rpass2] create -> %#llx result %d atts %u subpasses %u inputAtts %u\n",
            (unsigned long long)(uintptr_t)(out ? *out : 0), (int)r,
            ci->attachmentCount, ci->subpassCount, inputs);
        // Per-attachment sample count — an MSAA color (samples>1) paired with a
        // single-sample resolve target is the shape that leaves an eye image
        // black if the multiview resolve into its array layers never lands.
        for (uint32_t i = 0; i < ci->attachmentCount; i++)
            fprintf(stderr, "[TRACE] [rpass2]   att[%u] format %d samples %d loadOp %d storeOp %d\n",
                i, (int)ci->pAttachments[i].format, (int)ci->pAttachments[i].samples,
                (int)ci->pAttachments[i].loadOp, (int)ci->pAttachments[i].storeOp);
        for (uint32_t sp = 0; sp < ci->subpassCount; sp++) {
            const VkSubpassDescription2 *d = &ci->pSubpasses[sp];
            fprintf(stderr, "[TRACE] [rpass2]   subpass %u: color=%u input=%u depth=%s viewMask %#x\n",
                sp, d->colorAttachmentCount, d->inputAttachmentCount,
                d->pDepthStencilAttachment ? "yes" : "no", d->viewMask);
            // Which colour attachment resolves into which — names att1 as the
            // resolve target of the MSAA att0 (the suspected wanderer black).
            for (uint32_t k = 0; d->pResolveAttachments && k < d->colorAttachmentCount; k++)
                if (d->pResolveAttachments[k].attachment != VK_ATTACHMENT_UNUSED)
                    fprintf(stderr, "[TRACE] [rpass2]     color[%u] att=%u RESOLVES into att=%u\n",
                        k, d->pColorAttachments[k].attachment,
                        d->pResolveAttachments[k].attachment);
        }
    }
    free(atts);
    return r;
}
// KL_TRACE: dynamic-rendering census (vkCmdBeginRendering). If the guest renders
// its eye/menu with VK_KHR_dynamic_rendering there is NO framebuffer and NO
// render pass object, so this is the only place those renders are visible.
static void VKAPI_CALL klvk_CmdBeginRendering(VkCommandBuffer cb,
        const VkRenderingInfo *ri) {
    static PFN_vkCmdBeginRendering real;
    if (!real) {
        real = (PFN_vkCmdBeginRendering)mvk_sym("vkCmdBeginRendering");
        if (!real) real = (PFN_vkCmdBeginRendering)mvk_sym("vkCmdBeginRenderingKHR");
    }
    // KL_VK_RP_TRACE=<frame>: every vkCmdBeginRendering of that guest frame (kl_gpu_time's count), with the
    // command buffer it is recorded in, suspend/resume flags, and each attachment's load/store/resolve ops.
    {
        static int at = -2;
        if (at == -2) { const char *e = getenv("KL_VK_RP_TRACE"); at = e && *e ? atoi(e) : -1; }
        if (at >= 0 && ri && (int)kl_gpu_time_frame() == at) {
            char line[512];
            int u = snprintf(line, sizeof line, "  [rp] f%d cb %p area %ux%u+%d+%d layers %u view 0x%x flags 0x%x",
                             at, (void *)cb, ri->renderArea.extent.width, ri->renderArea.extent.height,
                             ri->renderArea.offset.x, ri->renderArea.offset.y, ri->layerCount, ri->viewMask, ri->flags);
            for (uint32_t i = 0; i < ri->colorAttachmentCount && ri->pColorAttachments && u < 440; i++) {
                const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
                u += snprintf(line + u, sizeof line - u, " | c%u ld%d st%d rs%d%s", i, a->loadOp, a->storeOp,
                              a->resolveMode, a->resolveImageView ? "+rv" : "");
            }
            if (ri->pDepthAttachment && ri->pDepthAttachment->imageView && u < 440)
                snprintf(line + u, sizeof line - u, " | z ld%d st%d rs%d%s", ri->pDepthAttachment->loadOp,
                         ri->pDepthAttachment->storeOp, ri->pDepthAttachment->resolveMode,
                         ri->pDepthAttachment->resolveImageView ? "+rv" : "");
            fprintf(stderr, "%s\n", line);
        }
    }
    if (klvk_trace() && ri) {
        klvk_trace_log("[dynr] begin area %ux%u layers %u colorAtts %u depth %s\n",
            ri->renderArea.extent.width, ri->renderArea.extent.height,
            ri->layerCount, ri->colorAttachmentCount,
            ri->pDepthAttachment && ri->pDepthAttachment->imageView ? "yes" : "no");
        for (uint32_t i = 0; i < ri->colorAttachmentCount && ri->pColorAttachments; i++) {
            VkImageView v = ri->pColorAttachments[i].imageView;
            VkImage im = klvk_view_image(v);
            klvk_trace_log("[dynr]   color[%u] view %#llx -> img %#llx (%s k=%d)\n",
                i, (unsigned long long)(uintptr_t)v,
                (unsigned long long)(uintptr_t)im, klvk_img_tag(im), klvk_layer_image_key(im));
        }
    }
    if (real) real(cb, ri);
}
// KL_TRACE: catch a temp-RT allocation that FAILS at bind time — the step Unity
// does through MoltenVK directly, invisible until now. A failure here is what
// makes Unity mark the render target invalid and report "temporary render
// texture not found". Always logged (failures only), so it costs nothing.
// ---------------------------------------------------------------------------
// KL_VK_TEXELBUF_PROBE — decisive evidence for the olar grayscale/fragmented
// scene bug, and the reason a SPIR-V texel-buffer emulation is very likely the
// WRONG fix.
//
// OFFLINE FINDING (proven, no device required): spirv-cross is the exact library
// MoltenVK 1.4.2 uses to convert SPIR-V -> MSL, and it is vendored here
// (vendor/third_party/spirv-cross). Its own reference corpus shows that a
// uniform texel-buffer fetch translates CORRECTLY, in both modes MoltenVK can
// pick:
//   shaders-msl/vert/texture_buffer.vert  (input: samplerBuffer + texelFetch)
//   reference/shaders-msl/vert/texture_buffer.vert (2D fallback):
//       texture2d<float> uSamp; ... uSamp.read(spvTexelBufferCoord(idx))
//       spvTexelBufferCoord(tc) = uint2(tc % 4096, tc / 4096)
//   reference/shaders-msl/vert/texture_buffer.texture-buffer-native.msl21.vert:
//       texture_buffer<float> uSamp; ... uSamp.read(uint(idx))
// The fallback math is byte-identical to kl_glfb's GLES emulation (width 4096
// vs klfb's 2048 — a width choice, not a defect). On Apple Vision Pro MoltenVK
// enables msl_options.texture_buffer_native, so olar takes the native path and
// reads element N linearly and correctly. => The OpImageFetch-on-Dim=Buffer
// translation is NOT where the picture is lost. A SPIR-V rewrite (Dim=Buffer ->
// 2D) would reproduce exactly the code MoltenVK already emits and could not fix
// the bug.
//
// That leaves two live suspects. This probe forks them, and MUST run before any
// large fix is built:
//   (A) the texel buffer's backing bytes are ZERO / absent at draw time — the
//       guest never uploaded them (a compute skinning / transform pass that is
//       not running under Klepton). Emulation is wasted; the fix is elsewhere.
//   (B) the bytes are PRESENT — then the fault is in MoltenVK's *runtime*
//       VkBufferView -> MTLTexture creation for olar's pattern (256+ views into
//       ONE VkBuffer at many offsets/formats). The fix is to replace MoltenVK's
//       per-view texture object, NOT to rewrite the shader.
//
// The probe reads the first few texel-buffer views' backing bytes. When the
// backing memory is host-visible AND the guest has mapped it, the read is direct
// — NO out-of-band GPU submit, so nothing about the guest's queue is perturbed.
// Otherwise it reports provenance (device-local? ever a transfer destination?),
// which still forks A vs B. Everything here is inert unless KL_VK_TEXELBUF_PROBE
// is set; when off, the added CreateBuffer/BindBufferMemory/MapMemory/CmdCopy*
// wrappers are faithful passthrough (one cached-flag test, then the real call).
static int probe_on(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_VK_TEXELBUF_PROBE", 0);
    return v;
}

// KL_VK_EMU_TEXELBUF — the fork-B fix (see the big block below CreateBufferView).
// Per-target default ON for the two UE5-mobile titles whose Manual-Vertex-Fetch
// texel buffers MoltenVK mis-binds (olar, wanderer) and OFF for every other
// Vulkan title, so nothing else changes. KL_VK_EMU_TEXELBUF overrides the
// default either way.
static int emu_on(void) {
    static int v = -1;
    if (v < 0) {
        extern const char *kl_driver_target_name(void);
        const char *t = kl_driver_target_name();
        int def = (t && (strcmp(t, "olar") == 0 || strcmp(t, "wanderer") == 0)) ? 1 : 0;
        v = kl_env_on("KL_VK_EMU_TEXELBUF", def);
    }
    return v;
}
// Buffer/memory-binding tracking is needed by BOTH the probe and the emu fix
// (the fix has to know which VkDeviceMemory + offset a source buffer occupies).
static int ubodump_on(void) { static int v = -1; if (v < 0) v = getenv("KL_VK_UBO_DUMP") != NULL; return v; }
static int track_on(void) { static int v = -1; if (v < 0) v = probe_on() || emu_on() || ubodump_on() || kl_env_on("KL_VK_EYEDRAW", 0); return v; }

// KL_VK_EMU_PROBE (default OFF) — the 2D-mirror source readback diagnostic
// (see klvk_mirror_gpu_copy). Purely observational and OFF unless explicitly set,
// so the normal emu path costs nothing but a cached int test.
static int emu_probe(void) {
    static int v = -1;
    if (v < 0) v = kl_env_on("KL_VK_EMU_PROBE", 0);
    return v;
}

#define KLVK_PROBE_BUFS 8192
#define KLVK_PROBE_MEMS 4096
#define KLVK_PROBE_TBVS 512

static struct probe_buf { VkBuffer buf; VkDeviceSize size; VkBufferUsageFlags usage;
                          VkDeviceMemory mem; VkDeviceSize memoff; int upload_dst; }
    g_probe_bufs[KLVK_PROBE_BUFS];
static int g_probe_nbuf;

static struct probe_mem { VkDeviceMemory mem; uint32_t type_index;
                          void *mapped; VkDeviceSize map_off; VkDeviceSize map_size; }
    g_probe_mems[KLVK_PROBE_MEMS];
static int g_probe_nmem;

static struct probe_tbv { VkBuffer buf; VkDeviceSize off; VkDeviceSize range; VkFormat fmt; }
    g_probe_tbvs[KLVK_PROBE_TBVS];
static int g_probe_ntbv;

static uint32_t g_probe_hostvis_mask;   // bit i set == memory type i is HOST_VISIBLE
static int      g_probe_hostvis_ready;
static int      g_probe_done;

// All of the following run under g_lock.
static struct probe_buf *probe_buf_find(VkBuffer b) {
    for (int i = 0; i < g_probe_nbuf; i++)
        if (g_probe_bufs[i].buf == b) return &g_probe_bufs[i];
    return NULL;
}
static struct probe_mem *probe_mem_find(VkDeviceMemory m) {
    for (int i = 0; i < g_probe_nmem; i++)
        if (g_probe_mems[i].mem == m) return &g_probe_mems[i];
    return NULL;
}
static void probe_compute_hostvis(void) {
    if (g_probe_hostvis_ready || !g_devs[0]) return;
    PFN_vkGetPhysicalDeviceMemoryProperties gmp =
        (PFN_vkGetPhysicalDeviceMemoryProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!gmp) return;
    VkPhysicalDeviceMemoryProperties mp; memset(&mp, 0, sizeof mp);
    gmp(g_devs[0]->phys, &mp);
    uint32_t mask = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            mask |= (1u << i);
    g_probe_hostvis_mask = mask;
    g_probe_hostvis_ready = 1;
}
static void probe_run(void) {
    pthread_mutex_lock(&g_lock);
    if (g_probe_done) { pthread_mutex_unlock(&g_lock); return; }
    g_probe_done = 1;
    probe_compute_hostvis();
    VKI("TEXELBUF_PROBE: %d texel-buffer view(s), %d buffer(s), %d memory alloc(s) tracked\n",
        g_probe_ntbv, g_probe_nbuf, g_probe_nmem);
    int shown = 0;
    for (int i = 0; i < g_probe_ntbv && shown < 16; i++) {
        struct probe_tbv *v = &g_probe_tbvs[i];
        struct probe_buf *b = probe_buf_find(v->buf);
        struct probe_mem *m = (b && b->mem) ? probe_mem_find(b->mem) : NULL;
        int hostvis = m && (g_probe_hostvis_mask & (1u << m->type_index));
        shown++;
        if (m && m->mapped && hostvis) {
            VkDeviceSize mem_abs = b->memoff + v->off;   // offset within the VkDeviceMemory
            VkDeviceSize map_end = (m->map_size == VK_WHOLE_SIZE)
                                 ? (VkDeviceSize)~0ull : m->map_off + m->map_size;
            if (mem_abs >= m->map_off && mem_abs < map_end) {
                const uint8_t *p = (const uint8_t *)m->mapped + (size_t)(mem_abs - m->map_off);
                VkDeviceSize rng = v->range;
                if (rng == VK_WHOLE_SIZE || rng > 64) rng = 64;
                size_t nz = 0;
                for (VkDeviceSize k = 0; k < rng; k++) if (p[k]) nz++;
                uint32_t w[4] = {0,0,0,0};
                memcpy(w, p, (size_t)(rng < 16 ? rng : 16));
                float f[4];
                for (int j = 0; j < 4; j++) memcpy(&f[j], &w[j], 4);
                VKI("  tbv[%d] buf %#llx off %llu fmt %d MAPPED+HOST-VISIBLE: "
                    "%zu/%llu nonzero; u32[%#x %#x %#x %#x] f32[%.4g %.4g %.4g %.4g] %s\n",
                    i, (unsigned long long)(uintptr_t)v->buf,
                    (unsigned long long)v->off, (int)v->fmt,
                    nz, (unsigned long long)rng, w[0], w[1], w[2], w[3],
                    f[0], f[1], f[2], f[3],
                    nz ? "<= DATA PRESENT (points at fork B: MoltenVK bind/read)"
                       : "<= DATA ABSENT (points at fork A: buffer never filled)");
            } else {
                VKI("  tbv[%d] buf %#llx off %llu: host-visible+mapped but view offset "
                    "outside the mapped range\n", i,
                    (unsigned long long)(uintptr_t)v->buf, (unsigned long long)v->off);
            }
        } else {
            const char *cls = !b ? "backing buffer not tracked"
                            : !m ? "backing memory not tracked"
                            : hostvis ? "host-visible but NOT mapped by the guest"
                            : "DEVICE-LOCAL (not host-readable without a GPU copy)";
            VKI("  tbv[%d] buf %#llx off %llu range %llu fmt %d: %s; backing buffer was %sa "
                "transfer destination\n", i, (unsigned long long)(uintptr_t)v->buf,
                (unsigned long long)v->off, (unsigned long long)v->range, (int)v->fmt,
                cls, (b && b->upload_dst) ? "" : "NOT ");
        }
    }
    VKI("TEXELBUF_PROBE: done. DATA PRESENT everywhere => build the buffer-view "
        "mirror (fork B); DATA ABSENT => find the missing upload/compute (fork A); "
        "all DEVICE-LOCAL+never-a-copy-dst => also fork A.\n");
    pthread_mutex_unlock(&g_lock);
}


static VkResult VKAPI_CALL klvk_AllocateMemory(VkDevice dev,
        const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    static PFN_vkAllocateMemory real;
    if (!real && real_gdpa) real = (PFN_vkAllocateMemory)real_gdpa(dev, "vkAllocateMemory");
    VkMemoryAllocateInfo ml_ai;
    if (ai && msaa_ml_on() && g_ml_nimg) {
        const VkMemoryDedicatedAllocateInfo *ded = NULL;
        for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) ded = (const void *)b;
        if (ded && ded->image && ml_has(ded->image)) {
            static PFN_vkGetPhysicalDeviceMemoryProperties gmp;
            if (!gmp) gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
            klvk_device *pd = dev_find(dev);
            int lazy = -1;
            if (pd && gmp) {
                VkPhysicalDeviceMemoryProperties mp; gmp(pd->phys, &mp);
                for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
                    if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) { lazy = (int)i; break; }
            }
            if (lazy >= 0) {
                ml_ai = *ai; ml_ai.memoryTypeIndex = (uint32_t)lazy;
                fprintf(stderr, "[ml] image %#llx: dedicated %.1f MB moved from memory type %u to %d (LAZILY_ALLOCATED, memoryless)\n",
                        (unsigned long long)(uintptr_t)ded->image, ai->allocationSize / 1048576.0, ai->memoryTypeIndex, lazy);
                ai = &ml_ai;
            } else {
                static int said; if (!said++) fprintf(stderr, "[ml] no LAZILY_ALLOCATED memory type; MSAA stays in real memory\n");
            }
        } else if (!ded) {
            // not dedicated: the next allocation of a noted image's exact size (see g_ml_req)
            int k = -1;
            pthread_mutex_lock(&g_msaa_mu);
            for (int i = 0; i < g_ml_nreq; i++)
                if (g_ml_req[i].open && g_ml_req[i].size == ai->allocationSize && ((g_ml_req[i].bits >> ai->memoryTypeIndex) & 1)) { k = i; break; }
            if (k >= 0) g_ml_req[k].open = 0;
            VkImage img = k >= 0 ? g_ml_req[k].img : VK_NULL_HANDLE; uint32_t bits = k >= 0 ? g_ml_req[k].bits : 0;
            pthread_mutex_unlock(&g_msaa_mu);
            int lazy = k >= 0 ? ml_lazy_type(dev, bits, 0) : -1;
            if (lazy >= 0) {
                ml_ai = *ai; ml_ai.memoryTypeIndex = (uint32_t)lazy;
                fprintf(stderr, "[ml] allocation of %.1f MB (image %#llx's size) moved from memory type %u to %d (memoryless)\n",
                        ai->allocationSize / 1048576.0, (unsigned long long)(uintptr_t)img, ai->memoryTypeIndex, lazy);
                ai = &ml_ai;
            }
        }
    }
    VkResult r = real ? real(dev, ai, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && out && ai) live_add(*out, ai->allocationSize);
    if (r == VK_SUCCESS && out && ai == &ml_ai) {
        pthread_mutex_lock(&g_msaa_mu);
        if (g_ml_nmem < kMlImgs) g_ml_mem[g_ml_nmem++] = *out;
        pthread_mutex_unlock(&g_msaa_mu);
    }
    if (r == VK_SUCCESS && ai && out && msaa_trace_on()) {
        pthread_mutex_lock(&g_msaa_mu);
        if (g_msaa_nmem < kMsaaMems) {
            g_msaa_mems[g_msaa_nmem].mem = *out; g_msaa_mems[g_msaa_nmem].type = ai->memoryTypeIndex;
            g_msaa_mems[g_msaa_nmem].size = ai->allocationSize; g_msaa_nmem++;
        }
        g_msaa_live += ai->allocationSize;
        if (ai->allocationSize >= (16u << 20))
            fprintf(stderr, "[msaa] allocate memory %#llx %.1f MB type %u (live %.0f MB)\n", (unsigned long long)(uintptr_t)*out,
                    ai->allocationSize / 1048576.0, ai->memoryTypeIndex, g_msaa_live / 1048576.0);
        pthread_mutex_unlock(&g_msaa_mu);
    }
    if (r != VK_SUCCESS)
        VKI("vkAllocateMemory FAILED result %d size %llu typeIndex %u\n", (int)r,
            ai ? (unsigned long long)ai->allocationSize : 0, ai ? ai->memoryTypeIndex : 0);
    if (probe_on() && r == VK_SUCCESS && ai && out) {
        pthread_mutex_lock(&g_lock);
        struct probe_mem *e = probe_mem_find(*out);
        if (!e && g_probe_nmem < KLVK_PROBE_MEMS) {
            e = &g_probe_mems[g_probe_nmem++];
            e->mem = *out; e->mapped = NULL; e->map_off = 0; e->map_size = 0;
        }
        if (e) e->type_index = ai->memoryTypeIndex;
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}
// With KL_VK_MSAA_TRACE: every vkFreeMemory of 16 MB or more, with the live total of what the game holds, so a
// vr_msaa change shows whether the old attachments' memory goes back or stays in the game's own blocks.
static void VKAPI_CALL klvk_FreeMemory(VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks *a) {
    static PFN_vkFreeMemory real;
    if (!real && real_gdpa) real = (PFN_vkFreeMemory)real_gdpa(dev, "vkFreeMemory");
    if (mem && msaa_trace_on()) {
        pthread_mutex_lock(&g_msaa_mu);
        for (int i = g_msaa_nmem - 1; i >= 0; i--) if (g_msaa_mems[i].mem == mem) {
            VkDeviceSize sz = g_msaa_mems[i].size;
            g_msaa_live -= sz;
            if (sz >= (16u << 20))
                fprintf(stderr, "[msaa] free memory %#llx %.1f MB type %u (live %.0f MB)\n", (unsigned long long)(uintptr_t)mem,
                        sz / 1048576.0, g_msaa_mems[i].type, g_msaa_live / 1048576.0);
            g_msaa_mems[i] = g_msaa_mems[--g_msaa_nmem];
            break;
        }
        pthread_mutex_unlock(&g_msaa_mu);
    }
    live_del(mem);
    if (real) real(dev, mem, a);
}
static VkResult VKAPI_CALL klvk_BindImageMemory(VkDevice dev, VkImage img,
        VkDeviceMemory mem, VkDeviceSize off) {
    static PFN_vkBindImageMemory real;
    if (!real && real_gdpa) real = (PFN_vkBindImageMemory)real_gdpa(dev, "vkBindImageMemory");
    if (g_ml_nmem) {   // a memoryless allocation may only hold the transient image it was made for
        int lazy_mem = 0;
        pthread_mutex_lock(&g_msaa_mu);
        for (int i = 0; i < g_ml_nmem; i++) if (g_ml_mem[i] == mem) lazy_mem = 1;
        pthread_mutex_unlock(&g_msaa_mu);
        if (lazy_mem) fprintf(stderr, "[ml] bind image %#llx to memoryless memory: %s\n", (unsigned long long)(uintptr_t)img,
                              ml_has(img) ? "ok (transient MSAA)" : "NOT a transient MSAA image (would be wrong)");
    }
    VkResult r = real ? real(dev, img, mem, off) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && msaa_trace_on()) {
        int hit = -1, m = -1;
        pthread_mutex_lock(&g_msaa_mu);
        for (int i = 0; i < g_msaa_nimg; i++) if (g_msaa_imgs[i].img == img) { hit = i; break; }
        if (hit >= 0) for (int i = g_msaa_nmem - 1; i >= 0; i--) if (g_msaa_mems[i].mem == mem) { m = i; break; }
        uint32_t type = m >= 0 ? g_msaa_mems[m].type : ~0u;
        VkDeviceSize size = m >= 0 ? g_msaa_mems[m].size : 0;
        pthread_mutex_unlock(&g_msaa_mu);
        if (hit >= 0) {
            uint32_t props = ~0u;
            klvk_device *pd = dev_find(dev);
            static PFN_vkGetPhysicalDeviceMemoryProperties gmp;
            if (!gmp) gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties");
            if (pd && gmp && type != ~0u) {
                VkPhysicalDeviceMemoryProperties mp; gmp(pd->phys, &mp);
                if (type < mp.memoryTypeCount) props = mp.memoryTypes[type].propertyFlags;
            }
            fprintf(stderr, "[msaa] bind image %#llx -> memory type %u props %#x%s, allocation %.1f MB at +%llu\n",
                    (unsigned long long)(uintptr_t)img, type, props, props != ~0u && (props & 0x10u) ? " (LAZILY_ALLOCATED)" : "",
                    size / 1048576.0, (unsigned long long)off);
        }
    }
    if (r != VK_SUCCESS)
        VKI("vkBindImageMemory FAILED result %d image %#llx\n", (int)r,
            (unsigned long long)(uintptr_t)img);
    return r;
}
// KL_TRACE / always-on-failure: the format-capability query Unity runs before
// committing a RenderTexture. If MoltenVK reports a limitation here that
// vkCreateImage does not enforce (a sample count, a usage, a max extent), Unity
// decides the temp RT is invalid, never registers it, and SetRenderTarget then
// fails "temporary render texture not found" — the menu-compositing wall.
static VkResult VKAPI_CALL klvk_GetPhysicalDeviceImageFormatProperties(
        VkPhysicalDevice pd, VkFormat fmt, VkImageType type, VkImageTiling tiling,
        VkImageUsageFlags usage, VkImageCreateFlags flags,
        VkImageFormatProperties *out) {
    static PFN_vkGetPhysicalDeviceImageFormatProperties real;
    if (!real && real_gipa) real = (PFN_vkGetPhysicalDeviceImageFormatProperties)
        real_gipa(g_instance, "vkGetPhysicalDeviceImageFormatProperties");
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceImageFormatProperties");
    VkResult r = real ? real(pd, fmt, type, tiling, usage, flags, out)
                      : VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (r != VK_SUCCESS || (klvk_trace() && (usage & 0x30u))) {
        fprintf(stderr, "[TRACE] [imgfmt] fmt %d type %d tiling %d usage %#x flags %#x -> %d%s\n",
            (int)fmt, (int)type, (int)tiling, usage, flags, (int)r,
            r != VK_SUCCESS ? "  <== UNSUPPORTED" : "");
        if (r == VK_SUCCESS && out)
            fprintf(stderr, "[TRACE] [imgfmt]   maxExtent %ux%ux%u maxMips %u maxLayers %u samples %#x\n",
                out->maxExtent.width, out->maxExtent.height, out->maxExtent.depth,
                out->maxMipLevels, out->maxArrayLayers, out->sampleCounts);
    }
    return r;
}
// ---- GPU-identity spoof --------------------------------------------------
// Some titles gate on the GPU they see rather than on Build.MODEL. batman
// (Batman: Arkham Shadow) is Quest 3 / 3S exclusive: it makes a throwaway
// VkInstance, reads the physical device, and — finding "Apple M5 GPU" / vendor
// 0x106b (Apple) rather than a Qualcomm Adreno — pops "Your device does not
// match the hardware requirements", abandons Vulkan and drops to a GLES2 context
// whose shaders it does not ship. So for such a target we rewrite the device
// identity MoltenVK reports to a Quest 3's Adreno 740 (Qualcomm). Only the
// identity strings/IDs change; every limit and feature stays MoltenVK's real
// answer, so nothing downstream is promised a capability the host lacks.
// Default: on for batman; KL_VK_SPOOF_ADRENO=0/1 forces the A/B either way.
static int klvk_spoof_gpu(void) {
    static int on = -1;
    if (on < 0) {
        const char *t = kl_driver_target_name();
        int dflt = t && strcmp(t, "batman") == 0;
        on = kl_env_on("KL_VK_SPOOF_ADRENO", dflt);
    }
    return on;
}
static void klvk_spoof_props(VkPhysicalDeviceProperties *p) {
    if (!p || !klvk_spoof_gpu()) return;
    p->vendorID = 0x5143;                 // Qualcomm
    p->deviceID = 0x43051401;             // an Adreno 740-class id (plausible)
    snprintf(p->deviceName, sizeof p->deviceName, "Adreno (TM) 740");
    static int said;
    if (!said) { said = 1;
        VKI("GPU identity spoofed to '%s' vendor %#x for the guest's "
            "hardware check (KL_VK_SPOOF_ADRENO)\n", p->deviceName, p->vendorID); }
}
static void VKAPI_CALL klvk_GetPhysicalDeviceProperties(
        VkPhysicalDevice pd, VkPhysicalDeviceProperties *props) {
    static PFN_vkGetPhysicalDeviceProperties real;
    if (!real && real_gipa)
        real = (PFN_vkGetPhysicalDeviceProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceProperties");
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceProperties");
    if (real) real(pd, props);
    klvk_spoof_props(props);
}
static void VKAPI_CALL klvk_GetPhysicalDeviceProperties2(
        VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *props) {
    if (!props) return;
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceProperties2");
    static PFN_vkGetPhysicalDeviceProperties2 real;
    static int tried;
    if (!tried) { tried = 1; if (real_gipa) {
        real = (PFN_vkGetPhysicalDeviceProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceProperties2");
        if (!real) real = (PFN_vkGetPhysicalDeviceProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceProperties2KHR");
    } }
    if (real) { real(pd, props); }
    else {
        PFN_vkGetPhysicalDeviceProperties v1 = real_gipa
            ? (PFN_vkGetPhysicalDeviceProperties)
                  real_gipa(g_instance, "vkGetPhysicalDeviceProperties") : NULL;
        if (v1) v1(pd, &props->properties);
    }
    klvk_spoof_props(&props->properties);
    klvk_minmax_props(props->pNext);

    // Make the deviceLUID Unity reads here EQUAL the one ovrp_GetDisplayAdapterId2
    // handed it, so OculusXR's "which enumerated device is the VR GPU?" match
    // lands instead of leaving the selection NULL (-> getQueueFamilies(NULL) ->
    // MVKPhysicalDevice::getQueueFamilies crash). Returning the right LUID from
    // OVRPlugin was not enough: MoltenVK's deviceLUID/deviceLUIDValid here can
    // differ from what the cache query saw (per-instance, or reported invalid to
    // the guest), so the two sides never agreed. Force agreement at the source.
    // Also logs what MoltenVK actually returned, to confirm the mismatch.
    for (VkBaseOutStructure *p = (VkBaseOutStructure *)props->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES) continue;
        VkPhysicalDeviceIDProperties *idp = (VkPhysicalDeviceIDProperties *)p;
        VKI("guest reads deviceLUID %02x%02x%02x%02x%02x%02x%02x%02x valid=%u"
            "%s\n",
            idp->deviceLUID[0], idp->deviceLUID[1], idp->deviceLUID[2],
            idp->deviceLUID[3], idp->deviceLUID[4], idp->deviceLUID[5],
            idp->deviceLUID[6], idp->deviceLUID[7], idp->deviceLUIDValid,
            g_display_luid_valid ? " -> forcing to the ovrp adapter id" : "");
        if (g_display_luid_valid) {
            memcpy(idp->deviceLUID, g_display_luid, 8 /* VK_LUID_SIZE */);
            idp->deviceLUIDValid = VK_TRUE;
        }
    }
}

// Safety net for a guest that reaches vkGetPhysicalDeviceQueueFamilyProperties
// with a NULL physicalDevice — Unity does exactly this when its GPU match leaves
// the selection null, and MoltenVK then dereferences null in MVKPhysicalDevice::
// getQueueFamilies. A null handle is never legitimate (there is exactly one GPU),
// so substitute the live instance's first physical device. Pure passthrough for a
// real handle, so no working title is affected. The LUID-force above should make
// the match land in the first place; this catches any residual null path.
static void VKAPI_CALL klvk_GetPhysicalDeviceQueueFamilyProperties(
        VkPhysicalDevice pd, uint32_t *count, VkQueueFamilyProperties *props) {
    static PFN_vkGetPhysicalDeviceQueueFamilyProperties real;
    static int tried;
    if (!tried) { tried = 1; if (real_gipa) real =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
            real_gipa(g_instance, "vkGetPhysicalDeviceQueueFamilyProperties"); }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceQueueFamilyProperties");
    if (real) real(pd, count, props);
    else if (count) *count = 0;
}

// The rest of the vkGetPhysicalDevice* family Unity's device init queries on the
// same NULL handle — each substitutes the live device and passes through. Kept
// terse; they share the resolve-once + guard shape.
static void VKAPI_CALL klvk_GetPhysicalDeviceFeatures2(
        VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) {
    static PFN_vkGetPhysicalDeviceFeatures2 real; static int t;
    if (!t) { t = 1; if (real_gipa) { real = (PFN_vkGetPhysicalDeviceFeatures2)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2");
        if (!real) real = (PFN_vkGetPhysicalDeviceFeatures2)
            real_gipa(g_instance, "vkGetPhysicalDeviceFeatures2KHR"); } }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceFeatures2");
    if (real) real(pd, f);
    if (f) klvk_minmax_features(f->pNext);
}
static void VKAPI_CALL klvk_GetPhysicalDeviceFeatures(
        VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) {
    static PFN_vkGetPhysicalDeviceFeatures real; static int t;
    if (!t) { t = 1; if (real_gipa) real = (PFN_vkGetPhysicalDeviceFeatures)
        real_gipa(g_instance, "vkGetPhysicalDeviceFeatures"); }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceFeatures");
    if (real) real(pd, f);
}
// KL_VK_REPORT_USAGE=1: answer VK_EXT_memory_budget's heapUsage for the device-local heap with what the game really
// holds (the sum of its live vkAllocateMemory blocks), not MoltenVK's figure. Source 2 sizes its texture streaming
// to "used of budget" and logs it at start ("Dedicated GPU Memory: 93 MB used of 10,922 MB budget" on MoltenVK,
// with ~4.8 GB actually allocated), so without this its streamer never sees pressure at any budget.
static _Atomic uint64_t g_vk_live;
enum { kLiveSlots = 1 << 16 };
static struct { VkDeviceMemory mem; VkDeviceSize size; } g_live[kLiveSlots];
static pthread_mutex_t g_live_mu = PTHREAD_MUTEX_INITIALIZER;
static int report_usage_on(void) { static int on = -1; if (on < 0) on = kl_env_on("KL_VK_REPORT_USAGE", 0); return on; }
static void live_add(VkDeviceMemory mem, VkDeviceSize size) {
    if (!report_usage_on() || !mem) return;
    uint32_t h = (uint32_t)(((uintptr_t)mem >> 4) * 2654435761u) & (kLiveSlots - 1);
    pthread_mutex_lock(&g_live_mu);
    for (uint32_t k = 0; k < kLiveSlots; k++, h = (h + 1) & (kLiveSlots - 1))
        if (!g_live[h].mem) { g_live[h].mem = mem; g_live[h].size = size; atomic_fetch_add(&g_vk_live, size); break; }
    pthread_mutex_unlock(&g_live_mu);
}
static void live_del(VkDeviceMemory mem) {
    if (!report_usage_on() || !mem) return;
    uint32_t h = (uint32_t)(((uintptr_t)mem >> 4) * 2654435761u) & (kLiveSlots - 1);
    pthread_mutex_lock(&g_live_mu);
    for (uint32_t k = 0; k < kLiveSlots && g_live[h].mem; k++, h = (h + 1) & (kLiveSlots - 1))
        if (g_live[h].mem == mem) {
            atomic_fetch_sub(&g_vk_live, g_live[h].size);
            // backward-shift delete keeps the probe chains intact
            uint32_t i = h;
            for (;;) {
                uint32_t j = (i + 1) & (kLiveSlots - 1);
                if (!g_live[j].mem) break;
                uint32_t want = (uint32_t)(((uintptr_t)g_live[j].mem >> 4) * 2654435761u) & (kLiveSlots - 1);
                if (((j - want) & (kLiveSlots - 1)) >= ((j - i) & (kLiveSlots - 1))) { g_live[i] = g_live[j]; i = j; }
                else break;
            }
            g_live[i].mem = VK_NULL_HANDLE; g_live[i].size = 0;
            break;
        }
    pthread_mutex_unlock(&g_live_mu);
}

// KL_VK_HEAP_MB=<n>: report at most n MB for each device-local heap (and as
// its VK_EXT_memory_budget budget). Engines size their GPU budgets from these
// numbers, and MoltenVK on a big Mac reports most of system memory — Alyx's
// graphics footprint followed it. On a headset the real heap is smaller;
// this lets the Mac rehearse that, and lets the app stay under a jetsam limit.
static void klvk_cap_heaps(VkPhysicalDeviceMemoryProperties *m, void *pnext) {
    static long cap = -2;
    if (cap == -2) { const char *e = getenv("KL_VK_HEAP_MB"); cap = e ? atol(e) : -1; }
    if (!m || (cap <= 0 && !report_usage_on())) return;
    VkDeviceSize c = cap > 0 ? (VkDeviceSize)cap << 20 : ~(VkDeviceSize)0;
    for (uint32_t i = 0; i < m->memoryHeapCount; i++)
        if ((m->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && m->memoryHeaps[i].size > c)
            m->memoryHeaps[i].size = c;
    for (VkBaseOutStructure *b = pnext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT) {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT *bp = (void *)b;
            for (uint32_t i = 0; i < m->memoryHeapCount; i++)
                if (bp->heapBudget[i] > c) bp->heapBudget[i] = c;
            if (report_usage_on()) {
                uint64_t live = atomic_load(&g_vk_live);
                for (uint32_t i = 0; i < m->memoryHeapCount; i++)
                    if ((m->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && bp->heapUsage[i] < live) bp->heapUsage[i] = live;
                static unsigned n;
                if (n++ % 600 == 0) fprintf(stderr, "[vkmem] reporting heap usage %llu MB of a %llu MB budget (query %u)\n",
                                            (unsigned long long)(live >> 20), (unsigned long long)(bp->heapBudget[0] >> 20), n);
            }
        }
}
static void VKAPI_CALL klvk_GetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m) {
    static PFN_vkGetPhysicalDeviceMemoryProperties real; static int t;
    if (!t) { t = 1; if (real_gipa) real = (PFN_vkGetPhysicalDeviceMemoryProperties)
        real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties"); }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceMemoryProperties");
    if (real) real(pd, m);
    klvk_cap_heaps(m, NULL);
}
// LDR ASTC block formats occupy VK_FORMAT_ASTC_4x4_UNORM_BLOCK(157) ..
// VK_FORMAT_ASTC_12x12_SRGB_BLOCK(184) contiguously.
static int klvk_is_astc(VkFormat f) { return (int)f >= 157 && (int)f <= 184; }

// CANDIDATE FIX (olar/wanderer gray hunt): make ASTC formats report full sampling
// support. If MoltenVK on visionOS under-reports optimalTilingFeatures for ASTC
// (missing SAMPLED_IMAGE / SAMPLED_IMAGE_FILTER_LINEAR), UE decides the ASTC texture
// is not usable for sampling and keeps a default placeholder bound -> gray world.
// Apple GPUs DO sample and linearly filter ASTC, so forcing these bits on is correct
// for this hardware. Also logs what MoltenVK reported first (the probe). emu_on()
// gates it to olar/wanderer. Returns nonzero if it patched (for the log).
#define KLVK_FF_SAMPLED        0x00000001u
#define KLVK_FF_FILTER_LINEAR  0x00001000u
#define KLVK_FF_BLIT_SRC       0x00000400u
#define KLVK_FF_TRANSFER_SRC   0x00004000u
#define KLVK_FF_TRANSFER_DST   0x00008000u
static void klvk_astc_patch_features(VkFormat fmt, VkFormatProperties *p) {
    if (!p || !emu_on() || !klvk_is_astc(fmt)) return;
    uint32_t want = KLVK_FF_SAMPLED | KLVK_FF_FILTER_LINEAR |
                    KLVK_FF_BLIT_SRC | KLVK_FF_TRANSFER_SRC | KLVK_FF_TRANSFER_DST;
    uint32_t before = p->optimalTilingFeatures;
    static int said;
    if (said < 24 && (before & want) != want) { said++;
        VKI("[fix] ASTC fmt %d optimalTilingFeatures %#x (missing %#x) — forcing full "
            "sampled+linear support so UE uses the real texture, not a default\n",
            (int)fmt, before, want & ~before); }
    p->optimalTilingFeatures |= want;
}
static void VKAPI_CALL klvk_GetPhysicalDeviceFormatProperties(
        VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties *p) {
    static PFN_vkGetPhysicalDeviceFormatProperties real; static int t;
    if (!t) { t = 1; if (real_gipa) real = (PFN_vkGetPhysicalDeviceFormatProperties)
        real_gipa(g_instance, "vkGetPhysicalDeviceFormatProperties"); }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceFormatProperties");
    if (real) real(pd, fmt, p);
    klvk_astc_patch_features(fmt, p);
    klvk_minmax_format(p);
}
static void VKAPI_CALL klvk_GetPhysicalDeviceFormatProperties2(
        VkPhysicalDevice pd, VkFormat fmt, VkFormatProperties2 *p) {
    static PFN_vkGetPhysicalDeviceFormatProperties2 real; static int t;
    if (!t) { t = 1; if (real_gipa) { real = (PFN_vkGetPhysicalDeviceFormatProperties2)
        real_gipa(g_instance, "vkGetPhysicalDeviceFormatProperties2");
        if (!real) real = (PFN_vkGetPhysicalDeviceFormatProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceFormatProperties2KHR"); } }
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceFormatProperties2");
    if (real) real(pd, fmt, p);
    if (p) klvk_astc_patch_features(fmt, &p->formatProperties);
    if (p) klvk_minmax_format(&p->formatProperties);
}
// The two device-enumeration queries Unity runs while VALIDATING a physical
// device (layers, then extensions) — the phase where AC Nexus was passing NULL.
// After CreateDevice (also guarded) the physical device stops mattering.
static VkResult VKAPI_CALL klvk_EnumerateDeviceLayerProperties(
        VkPhysicalDevice pd, uint32_t *count, VkLayerProperties *props) {
    static PFN_vkEnumerateDeviceLayerProperties real; static int t;
    if (!t) { t = 1; if (real_gipa) real = (PFN_vkEnumerateDeviceLayerProperties)
        real_gipa(g_instance, "vkEnumerateDeviceLayerProperties"); }
    pd = KLVK_PHYS_OR_SUB(pd, "EnumerateDeviceLayerProperties");
    if (real) return real(pd, count, props);
    if (count) *count = 0;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL klvk_EnumerateDeviceExtensionProperties(
        VkPhysicalDevice pd, const char *layer, uint32_t *count,
        VkExtensionProperties *props) {
    static PFN_vkEnumerateDeviceExtensionProperties real; static int t;
    if (!t) { t = 1; if (real_gipa) real = (PFN_vkEnumerateDeviceExtensionProperties)
        real_gipa(g_instance, "vkEnumerateDeviceExtensionProperties"); }
    pd = KLVK_PHYS_OR_SUB(pd, "EnumerateDeviceExtensionProperties");
    if (real) return real(pd, layer, count, props);
    if (count) *count = 0;
    return VK_SUCCESS;
}
static void VKAPI_CALL klvk_GetPhysicalDeviceMemoryProperties2(
        VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *props) {
    if (!props) return;
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceMemoryProperties2");
    // The app made a Vulkan 1.0 instance, so MoltenVK does not export the core
    // 1.1 name — only the KHR alias (VK_KHR_get_physical_device_properties2 is
    // enabled). Prefer the real one so the pNext chain (VK_EXT_memory_budget)
    // is filled; fall back to the v1 struct so the guest at least learns its
    // heaps and memory types. Without this the guest got a zeroed struct — no
    // usable memory — and UE4 called AndroidThunkJava_ForceQuit.
    static PFN_vkGetPhysicalDeviceMemoryProperties2 real;
    static int tried;
    if (!tried) {
        tried = 1;
        if (real_gipa) {
            real = (PFN_vkGetPhysicalDeviceMemoryProperties2)
                real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties2");
            if (!real) real = (PFN_vkGetPhysicalDeviceMemoryProperties2)
                real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties2KHR");
        }
    }
    if (real) { real(pd, props); klvk_cap_heaps(&props->memoryProperties, props->pNext); return; }
    PFN_vkGetPhysicalDeviceMemoryProperties v1 = real_gipa
        ? (PFN_vkGetPhysicalDeviceMemoryProperties)
              real_gipa(g_instance, "vkGetPhysicalDeviceMemoryProperties")
        : NULL;
    if (v1) v1(pd, &props->memoryProperties);
    klvk_cap_heaps(&props->memoryProperties, NULL);
}

static VkResult VKAPI_CALL klvk_GetPhysicalDeviceImageFormatProperties2(
        VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
        VkImageFormatProperties2 *props) {
    pd = KLVK_PHYS_OR_SUB(pd, "GetPhysicalDeviceImageFormatProperties2");
    static PFN_vkGetPhysicalDeviceImageFormatProperties2 real;
    if (!real && real_gipa) {
        real = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceImageFormatProperties2");
        if (!real) real = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            real_gipa(g_instance, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    }
    VkResult r = real ? real(pd, info, props) : VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (info && (r != VK_SUCCESS || (klvk_trace() && (info->usage & 0x30u))))
        fprintf(stderr, "[TRACE] [imgfmt2] fmt %d type %d tiling %d usage %#x flags %#x -> %d%s\n",
            (int)info->format, (int)info->type, (int)info->tiling, info->usage,
            info->flags, (int)r, r != VK_SUCCESS ? "  <== UNSUPPORTED" : "");
    return r;
}
// KL_TRACE: which thread submits guest work. Compared with [thr] createRT this
// shows whether Unity creates its temp render targets on the same thread that
// records/submits the frame, or a different one (the temp-RT race hypothesis).
static _Atomic unsigned long g_fs_submit, g_fs_submit_fenced;
static void fs_report(void);
static void klvk_gpu_watch(VkQueue q) {
    static VkQueue seen[8];
    static _Atomic int nseen;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    int n = nseen;
    for (int i = 0; i < n; i++) if (seen[i] == q) return;
    pthread_mutex_lock(&mu);
    int have = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == q) have = 1;
    if (!have && nseen < 8) {
        klvk_device *d = dev_of_queue(q);
        PFN_vkExportMetalObjectsEXT export_ext = d && real_gdpa
            ? (PFN_vkExportMetalObjectsEXT)real_gdpa(d->dev, "vkExportMetalObjectsEXT") : NULL;
        VkExportMetalCommandQueueInfoEXT cq = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT,
            .queue = q,
        };
        VkExportMetalObjectsInfoEXT info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
            .pNext = &cq,
        };
        if (export_ext) export_ext(d->dev, &info);
        if (cq.mtlCommandQueue) kl_gpu_time_watch((void *)cq.mtlCommandQueue);
        else VKI("no MTLCommandQueue behind guest queue %p; its GPU time is not measured\n", (void *)q);
        seen[nseen] = q;
        nseen = nseen + 1;
    }
    pthread_mutex_unlock(&mu);
}
static const void *ed_ptr(uint64_t buf, uint64_t off) {    // g_lock held; NULL unless [off, off+256) is inside the mapping
    struct probe_buf *e = probe_buf_find((VkBuffer)(uintptr_t)buf);
    struct probe_mem *m = e ? probe_mem_find(e->mem) : NULL;
    if (!m || !m->mapped || e->memoff < m->map_off || off + 256 > e->size) return NULL;
    uint64_t at = e->memoff - m->map_off + off;
    if (m->map_size != VK_WHOLE_SIZE && at + 256 > m->map_size) return NULL;
    return (const char *)m->mapped + at;
}
void klvk_ed_snap_xf(uint64_t buf, uint64_t off, uint32_t rows) {
    pthread_mutex_lock(&g_lock);
    if (rows > 64) rows = 64;
    const void *sp = ed_ptr(buf, off);
    struct probe_buf *e = probe_buf_find((VkBuffer)(uintptr_t)buf);
    if (sp && e && off + (uint64_t)rows * 16384 <= e->size) {
        free(g_eds_xf.data); g_eds_xf.data = malloc((size_t)rows * 16384);
        if (g_eds_xf.data) { memcpy(g_eds_xf.data, sp, (size_t)rows * 16384); g_eds_xf.rows = rows; }
    }
    pthread_mutex_unlock(&g_lock);
}
static void ed_print_draw(const char *tag, uint64_t vb1off) {   // g_lock held
    const uint8_t *src = NULL;
    for (unsigned c = 0; c < 64 && c < g_eds_n && !src; c++) {
        unsigned q = (g_eds_n - 1 - c) % 64;
        if (g_eds_inst[q].data && vb1off >= g_eds_inst[q].dof && vb1off + 4 <= g_eds_inst[q].dof + g_eds_inst[q].sz)
            src = g_eds_inst[q].data + (vb1off - g_eds_inst[q].dof);
    }
    if (!src) { fprintf(stderr, "[eyedraw] %s instance offset %llu: not in a recent instance copy\n", tag, (unsigned long long)vb1off); return; }
    _Float16 hu, hv; memcpy(&hu, src, 2); memcpy(&hv, src + 2, 2);
    float tx = (float)hu * 1024.0f, ty = (float)hv * 1024.0f; int ix = (int)tx, iy = (int)ty;
    char line[2048]; int o = snprintf(line, sizeof line, "[eyedraw] %s instance offset %llu: texel (%.2f, %.2f)", tag, (unsigned long long)vb1off, tx, ty);
    if (g_eds_xf.data && iy >= 0 && (uint32_t)iy < g_eds_xf.rows && ix >= 0 && ix + 4 <= 1024) {
        const float *t = g_eds_xf.data + ((size_t)iy * 1024 + (size_t)ix) * 4;
        uint32_t yb, zb; memcpy(&yb, &t[12 + 1], 4); memcpy(&zb, &t[12 + 2], 4);
        o += snprintf(line + o, sizeof line - o, " | t0 %.3g %.3g %.3g %.3g | t3 x %.3g y %#x z %#x (index %u) w %.3g",
                      t[0], t[1], t[2], t[3], t[12], yb, zb, zb & 0xffff, t[15]);
    } else o += snprintf(line + o, sizeof line - o, " | header row %d not in the latest upload (%u rows)", iy, g_eds_xf.rows);
    fprintf(stderr, "%s\n", line);
}
static void klvk_eyedraw_report(void) {
    if (!eyedraw_on()) return;
    static int said;
    pthread_mutex_lock(&g_lock);
    for (int d = 0; d < g_ned_draw && said < 200; d++) {
        const uint16_t *uv = ed_ptr(g_ed_draw[d].vb1, g_ed_draw[d].vb1off + 4ull * g_ed_draw[d].first);
        if (!uv) {
            uint64_t want = g_ed_draw[d].vb1off + 4ull * g_ed_draw[d].first;
            unsigned lim = g_edc_n < KLEDC ? g_edc_n : KLEDC;
            for (unsigned c = 0; c < lim && !uv; c++) {
                int k = (int)((g_edc_n - 1 - c) % KLEDC);
                if (g_ed_draw[d].vb1 && g_edc[k].dst == g_ed_draw[d].vb1 && want >= g_edc[k].dof && want + 4 <= g_edc[k].dof + g_edc[k].sz) {
                    uv = ed_ptr(g_edc[k].src, g_edc[k].so + (want - g_edc[k].dof));
                    if (said < 200) fprintf(stderr, "[eyedraw]   slot %llu from copy src %#llx +%llu (region dst %llu size %llu)%s\n",
                        (unsigned long long)want, (unsigned long long)g_edc[k].src, (unsigned long long)g_edc[k].so,
                        (unsigned long long)g_edc[k].dof, (unsigned long long)g_edc[k].sz, uv ? "" : " — staging not readable");
                }
            }
        }
        if (!uv) {
            struct probe_buf *e = probe_buf_find((VkBuffer)(uintptr_t)g_ed_draw[d].vb1);
            struct probe_mem *m = e ? probe_mem_find(e->mem) : NULL;
            fprintf(stderr, "[eyedraw] draw first %u count %u: instance stream buf %#llx off %llu not readable (tracked %d, size %llu, usage %#x, mem %d, mapped %d; %d bufs tracked)\n",
                    g_ed_draw[d].first, g_ed_draw[d].count, (unsigned long long)g_ed_draw[d].vb1, (unsigned long long)g_ed_draw[d].vb1off,
                    e != NULL, e ? (unsigned long long)e->size : 0ull, e ? (unsigned)e->usage : 0u, m != NULL, m && m->mapped, g_probe_nbuf);
            said++; continue;
        }
        for (uint32_t i = 0; i < g_ed_draw[d].count && i < 4; i++) {
            _Float16 hu, hv; memcpy(&hu, uv + 2 * i, 2); memcpy(&hv, uv + 2 * i + 1, 2);
            float u = (float)hu, v = (float)hv, tx = u * 1024.0f, ty = v * 1024.0f;
            int ix = (int)tx, iy = (int)ty;
            char line[4096]; int o = snprintf(line, sizeof line, "[eyedraw] draw first %u inst %u: uv (%.6f, %.6f) -> texel (%.2f, %.2f); upload rows %u",
                                             g_ed_draw[d].first, i, u, v, tx, ty, g_ed_draw[d].xrows);
            if (iy >= 0 && (uint32_t)iy < g_ed_draw[d].xrows && ix >= 0 && ix + 8 < 1024 && g_ed_draw[d].xbuf) {
                const float *row = ed_ptr(g_ed_draw[d].xbuf, g_ed_draw[d].xoff + ((uint64_t)iy * 1024 + (uint64_t)ix) * 16);
                if (row) for (int t = 0; t < 8 && o < (int)sizeof line - 200; t++) {
                    uint32_t zb; memcpy(&zb, &row[4 * t + 2], 4);
                    o += snprintf(line + o, sizeof line - o, " | t%d %.3g %.3g z=%#x %.3g", t, row[4 * t], row[4 * t + 1], zb, row[4 * t + 3]);
                }
            } else if (o < (int)sizeof line - 64) o += snprintf(line + o, sizeof line - o, " | (header row not in the latest upload)");
            fprintf(stderr, "%s\n", line); said++;
        }
    }
    g_ned_draw = 0;
    pthread_mutex_unlock(&g_lock);
}
static void klvk_ubodump_frame(void);
static VkResult VKAPI_CALL klvk_QueueSubmit(VkQueue q, uint32_t n,
        const VkSubmitInfo *si, VkFence fence) {
    static PFN_vkQueueSubmit real;
    if (!real && real_gdpa) real = (PFN_vkQueueSubmit)real_gdpa(g_devs[0]?g_devs[0]->dev:NULL, "vkQueueSubmit");
    if (klvk_trace()) {
        static int c;
        if (c < 40) { c++;
            unsigned cb = 0; for (uint32_t i=0;i<n && si;i++) cb += si[i].commandBufferCount;
            fprintf(stderr, "[TRACE] [thr] QueueSubmit tid=%u batches=%u cmdbufs=%u fence=%d\n",
                klvk_tid(), n, cb, fence != VK_NULL_HANDLE);
        }
    }
    if (kl_vulkan_guest_active()) klvk_gpu_watch(q);
    klvk_ubodump_frame();
    if (eyedraw_on() && kl_env_on("KL_VK_EYEDRAW_SUBMIT", 0)) klvk_eyedraw_report();
    VkResult r = real ? real(q, n, si, fence) : VK_ERROR_INITIALIZATION_FAILED;
    // KL_VK_SUBMIT_WAITIDLE=1 (diagnostic only, costs the whole CPU/GPU overlap): idle the queue after every
    // guest submit, so no per-frame buffer can be rewritten while the GPU still reads it — the A/B for a sync race.
    {
        static int wi = -1; static PFN_vkQueueWaitIdle qwi;
        if (wi < 0) { wi = kl_env_on("KL_VK_SUBMIT_WAITIDLE", 0);
                      if (wi && real_gdpa) qwi = (PFN_vkQueueWaitIdle)real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkQueueWaitIdle"); }
        if (wi && qwi && r == VK_SUCCESS) qwi(q);
    }
    g_fs_submit++; if (fence) g_fs_submit_fenced++;
    if (kl_env_on("KL_VK_FENCESTATS", 0)) fs_report();
    // Same thread, same queue, right after the guest's work: copy every wanted
    // overlay texture into its persistent shadow (see klvk_shadow_pump).
    if (r == VK_SUCCESS) klvk_shadow_pump();
    // KL_VK_TEXELBUF_PROBE: after enough submits that the guest is well past its
    // resource uploads, dump the backing bytes of the first few texel buffers
    // once. Deferred to a submit count (not frame 0) so device-local uploads
    // have actually happened by the time we look.
    if (probe_on()) {
        static unsigned subs;
        static int at = -1;
        if (at < 0) at = kl_env_int("KL_VK_TEXELBUF_PROBE_AT", 240);
        if ((int)(++subs) == at) probe_run();
    }
    return r;
}
// UE4's Variable Rate Shading / foveation probe. Two-call enumeration:
// (count, NULL) asks how many rates exist, (count, array) fills that many. The
// generic no-op stub returned VK_SUCCESS WITHOUT writing *count, leaving the
// guest to size an array from an undefined number and then walk it — a field
// read off a bad pointer that lands as a null-ish deref inside UE4's own render
// code, with no Klepton frame on the stack. MoltenVK has no fragment shading
// rate support, so the truthful answer is zero rates: write *count = 0 and
// return VK_SUCCESS, and the guest cleanly takes its no-foveation path. `rates`
// is left untouched because zero rates are being reported into it.
static VkResult VKAPI_CALL klvk_GetPhysicalDeviceFragmentShadingRatesKHR(
        VkPhysicalDevice pd, uint32_t *count, void *rates) {
    (void)pd; (void)rates;
    if (count) *count = 0;
    return VK_SUCCESS;
}

// Diagnostic pass-through. UE4/UE5 pick a GPU here and, if none is acceptable,
// silently fall back and put up "This device does not support Vulkan…" (a fatal
// message box on a Vulkan-only package) — with NOTHING in our log to say why.
// Wrath2 dies exactly this way: one probe VkInstance, no device ever created.
// So log, once, the count MoltenVK returns and the properties the engine reads
// off each device (apiVersion, type, vendor/device ID, graphics-queue support).
// A count of 0 means the portability-enumeration gate ate the device; a count of
// 1+ means the engine rejected it on a property the dump then names. Faithful
// forward otherwise, so every other target is unaffected.

// KL_VK_DUMP_CAPS=1: a vulkaninfo-shaped report of what the guest will see —
// the core features, the 1.1/1.2/1.3 feature structs, key limits, heaps and
// the support of the formats a PC engine leans on — so two hosts can be
// diffed line by line (Source 2 refused the headset's device with no reason).
static void klvk_dump_caps(VkInstance inst, VkPhysicalDevice pd) {
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = (PFN_vkGetPhysicalDeviceFeatures2)real_gipa(inst, "vkGetPhysicalDeviceFeatures2");
    if (!gf2) gf2 = (PFN_vkGetPhysicalDeviceFeatures2)real_gipa(inst, "vkGetPhysicalDeviceFeatures2KHR");
    PFN_vkGetPhysicalDeviceProperties gp = (PFN_vkGetPhysicalDeviceProperties)real_gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceFormatProperties gfp = (PFN_vkGetPhysicalDeviceFormatProperties)real_gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)real_gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkEnumerateDeviceExtensionProperties gde = (PFN_vkEnumerateDeviceExtensionProperties)real_gipa(inst, "vkEnumerateDeviceExtensionProperties");
    if (gf2) {
        VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13 };
        VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
        VkPhysicalDeviceFeatures2 f = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11 };
        gf2(pd, &f);
        const VkBool32 *b = (const VkBool32 *)&f.features;
        fprintf(stderr, "[caps] core features:");
        for (unsigned i = 0; i < sizeof f.features / sizeof(VkBool32); i++) fprintf(stderr, "%s%u=%u", i % 11 ? " " : "\n[caps]   ", i, b[i]);
        fprintf(stderr, "\n");
        #define DUMPSTRUCT(tag, st, first) do { const VkBool32 *q = (const VkBool32 *)&(st).first; \
            unsigned n = (unsigned)((sizeof (st) - offsetof(__typeof__(st), first)) / sizeof(VkBool32)); \
            fprintf(stderr, "[caps] %s:", tag); \
            for (unsigned i = 0; i < n; i++) fprintf(stderr, "%s%u=%u", i % 11 ? " " : "\n[caps]   ", i, q[i]); \
            fprintf(stderr, "\n"); } while (0)
        DUMPSTRUCT("vk11", f11, storageBuffer16BitAccess);
        DUMPSTRUCT("vk12", f12, samplerMirrorClampToEdge);
        DUMPSTRUCT("vk13", f13, robustImageAccess);
    }
    if (gp) {
        VkPhysicalDeviceProperties p; gp(pd, &p);
        const VkPhysicalDeviceLimits *l = &p.limits;
        fprintf(stderr, "[caps] limits: img2D %u img3D %u imgCube %u layers %u texelBuf %u ubo %u ssbo %u push %u "
                "allocs %u samplers %u perStageSamplers %u perStageUBO %u perStageSSBO %u perStageImages %u perStageStorageImg %u perStageRes %u "
                "setSamplers %u setUBO %u setSSBO %u setImages %u setStorageImg %u attachments %u colorAtt %u "
                "vp %u viewport %ux%u fb %ux%u fbLayers %u sampleCounts c%#x d%#x s%#x anisotropy %.0f "
                "timestamps %u tsPeriod %.2f cs %ux%ux%u inv %u shared %u\n",
                l->maxImageDimension2D, l->maxImageDimension3D, l->maxImageDimensionCube, l->maxImageArrayLayers,
                l->maxTexelBufferElements, l->maxUniformBufferRange, l->maxStorageBufferRange, l->maxPushConstantsSize,
                l->maxMemoryAllocationCount, l->maxSamplerAllocationCount, l->maxPerStageDescriptorSamplers,
                l->maxPerStageDescriptorUniformBuffers, l->maxPerStageDescriptorStorageBuffers, l->maxPerStageDescriptorSampledImages,
                l->maxPerStageDescriptorStorageImages, l->maxPerStageResources, l->maxDescriptorSetSamplers,
                l->maxDescriptorSetUniformBuffers, l->maxDescriptorSetStorageBuffers, l->maxDescriptorSetSampledImages,
                l->maxDescriptorSetStorageImages, l->maxFragmentOutputAttachments, l->maxColorAttachments,
                l->maxViewports, l->maxViewportDimensions[0], l->maxViewportDimensions[1],
                l->maxFramebufferWidth, l->maxFramebufferHeight, l->maxFramebufferLayers,
                l->framebufferColorSampleCounts, l->framebufferDepthSampleCounts, l->framebufferStencilSampleCounts,
                l->maxSamplerAnisotropy, l->timestampComputeAndGraphics, l->timestampPeriod,
                l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1], l->maxComputeWorkGroupSize[2],
                l->maxComputeWorkGroupInvocations, l->maxComputeSharedMemorySize);
    }
    if (gmp) {
        VkPhysicalDeviceMemoryProperties m; gmp(pd, &m);
        for (uint32_t i = 0; i < m.memoryHeapCount; i++)
            fprintf(stderr, "[caps] heap %u: %llu MB flags %#x\n", i, (unsigned long long)(m.memoryHeaps[i].size >> 20), m.memoryHeaps[i].flags);
        for (uint32_t i = 0; i < m.memoryTypeCount; i++)
            fprintf(stderr, "[caps] memtype %u: heap %u flags %#x\n", i, m.memoryTypes[i].heapIndex, m.memoryTypes[i].propertyFlags);
    }
    if (gfp) {
        static const int fmts[] = { 37, 43, 44, 50, 64, 91, 97, 100, 109, 122, 123, 124, 125, 126, 127, 128, 129, 130,
                                    131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146,
                                    147, 148, 157, 158, 184, 1000054000 };
        for (unsigned i = 0; i < sizeof fmts / sizeof fmts[0]; i++) {
            VkFormatProperties fp; memset(&fp, 0, sizeof fp);
            gfp(pd, (VkFormat)fmts[i], &fp);
            fprintf(stderr, "[caps] format %d: linear %#x optimal %#x buffer %#x\n", fmts[i],
                    fp.linearTilingFeatures, fp.optimalTilingFeatures, fp.bufferFeatures);
        }
    }
    if (gde) {
        uint32_t n = 0; gde(pd, NULL, &n, NULL);
        VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
        if (e) { gde(pd, NULL, &n, e);
                 for (uint32_t i = 0; i < n; i++) fprintf(stderr, "[caps] devext %s\n", e[i].extensionName);
                 free(e); }
    }
}

static VkResult VKAPI_CALL klvk_EnumeratePhysicalDevices(
        VkInstance inst, uint32_t *count, VkPhysicalDevice *devs) {
    static PFN_vkEnumeratePhysicalDevices real;
    if (!real && real_gipa) real = (PFN_vkEnumeratePhysicalDevices)
        real_gipa(inst ? inst : g_instance, "vkEnumeratePhysicalDevices");
    if (!real) { if (count) *count = 0; return VK_ERROR_INITIALIZATION_FAILED; }

    static int logged;
    if (!logged) {
        logged = 1;
        uint32_t n = 0;
        VkResult rr = real(inst, &n, NULL);
        VKI("vkEnumeratePhysicalDevices -> %u device(s) (result %d)\n", n, (int)rr);
        if (n) {
            VkPhysicalDevice tmp[8];
            uint32_t give = n < 8 ? n : 8;
            real(inst, &give, tmp);
            PFN_vkGetPhysicalDeviceProperties gp =
                (PFN_vkGetPhysicalDeviceProperties)
                    real_gipa(inst, "vkGetPhysicalDeviceProperties");
            PFN_vkGetPhysicalDeviceQueueFamilyProperties gq =
                (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
                    real_gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
            for (uint32_t i = 0; i < give && gp; i++) {
                VkPhysicalDeviceProperties p; memset(&p, 0, sizeof p);
                gp(tmp[i], &p);
                unsigned nq = 0, gfx = 0;
                if (gq) {
                    gq(tmp[i], &nq, NULL);
                    VkQueueFamilyProperties qf[16];
                    unsigned gv = nq < 16 ? nq : 16;
                    gq(tmp[i], &gv, qf);
                    for (unsigned k = 0; k < gv; k++)
                        if (qf[k].queueFlags & VK_QUEUE_GRAPHICS_BIT) gfx = 1;
                }
                VKI("  phys[%u] '%s' api %u.%u.%u type %d vendor %#x device %#x "
                    "queueFamilies %u graphics %d\n", i, p.deviceName,
                    VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                    VK_VERSION_PATCH(p.apiVersion), (int)p.deviceType,
                    p.vendorID, p.deviceID, nq, gfx);
            }
        }
    }
    VkResult rr = real(inst, count, devs);
    { static int dumped;
      if (devs && count && *count && !dumped && kl_env_on("KL_VK_DUMP_CAPS", 0)) { dumped = 1; klvk_dump_caps(inst, devs[0]); } }
    // Every FETCH (devs != NULL), not just the first: AC Nexus enumerates on a
    // throwaway instance, destroys it, makes a second, and reaches device init
    // with a NULL physicalDevice — so the question "what did enumerate hand the
    // guest on THIS instance?" has to be answerable per instance, not once.
    if (devs && count)
        VKI("vkEnumeratePhysicalDevices(inst %p) FETCH -> %u, devs[0] %p (result %d)\n",
            (void *)inst, *count, (void *)devs[0], (int)rr);
    return rr;
}

// Draw-time null-resource tracers (Into the Radius: MVKCmdDrawIndexed::encode
// walks a null object at +0x20 after ~31 clean frames). A null VkBuffer bound as
// the index/vertex source is the usual cause; log it (once each) so the culprit
// is named without a trace flag, and forward unchanged.
// Per-command-buffer record of the index buffer bound since vkBeginCommandBuffer.
// The Into the Radius crash is a nil MTLBuffer handed to Metal's
// drawIndexedPrimitives from MVKCmdDrawIndexed::encode (verified by disassembly:
// the faulting arg x5 is loaded from the encoder's index binding at +0x4c8, and
// x8=0x18 / fault at 0x20 is a base-null object walk). That happens when the
// index binding has no live MTLBuffer — either the guest issued vkCmdDrawIndexed
// with no vkCmdBindIndexBuffer in this command buffer, or it bound a VkBuffer
// whose backing MTLBuffer is nil. Neither is caught by the NULL-*handle* check
// below (the handle is non-null; the *Metal* buffer is missing). So track the
// bound handle per command buffer and re-check it at draw time. 64 slots covers
// UE4's in-flight command buffers with room to spare; a table miss simply loses
// the guard for that one buffer, it never forwards a draw we know to be bad.
#define KLVK_CB_SLOTS 64
static struct { void *cb; uint64_t idxbuf; } g_cb_idx[KLVK_CB_SLOTS];
static void klvk_cb_set_idx(void *cb, uint64_t buf) {
    if (!cb) return;
    int freei = -1;
    for (int i = 0; i < KLVK_CB_SLOTS; i++) {
        if (g_cb_idx[i].cb == cb) { g_cb_idx[i].idxbuf = buf; return; }
        if (freei < 0 && !g_cb_idx[i].cb) freei = i;
    }
    if (freei >= 0) { g_cb_idx[freei].cb = cb; g_cb_idx[freei].idxbuf = buf; }
}
static uint64_t klvk_cb_get_idx(void *cb) {
    for (int i = 0; i < KLVK_CB_SLOTS; i++)
        if (g_cb_idx[i].cb == cb) return g_cb_idx[i].idxbuf;
    return 0;
}
// Ask MoltenVK for the MTLBuffer backing a VkBuffer. Returns NULL when the
// buffer has no memory bound yet (the state that feeds the nil-indexBuffer
// crash) or when MoltenVK does not export the accessor. The ABI is a pointer
// out-param, declared plain-C here just like klvk_overlay_mtl_texture above.
static void *klvk_buf_mtl(uint64_t buf) {
    typedef void (*pfn_get_mtl_buffer_mvk)(uint64_t, void **);
    static pfn_get_mtl_buffer_mvk get;
    static int resolved;
    if (!resolved) { resolved = 1; get = (pfn_get_mtl_buffer_mvk)mvk_sym("vkGetMTLBufferMVK"); }
    if (!get || !buf) return NULL;
    void *b = NULL;
    get(buf, &b);
    return b;
}
static int klvk_have_buf_mtl(void) {
    return mvk_sym("vkGetMTLBufferMVK") != NULL;
}
// vkBeginCommandBuffer restarts recording: any index binding from a previous use
// of this command buffer is gone, so clear our record. Without this reset the
// table would report a stale (now-irrelevant) index buffer as "still bound".
static VkResult VKAPI_CALL klvk_BeginCommandBuffer(void *cb, const void *bi) {
    static VkResult (*real)(void *, const void *);
    if (!real && real_gdpa) real = (VkResult (*)(void *, const void *))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkBeginCommandBuffer");
    klvk_cb_set_idx(cb, 0);
    return real ? real(cb, bi) : VK_ERROR_INITIALIZATION_FAILED;
}
static void VKAPI_CALL klvk_CmdBindIndexBuffer(void *cb, uint64_t buffer,
        uint64_t offset, uint32_t indexType) {
    static void (*real)(void *, uint64_t, uint64_t, uint32_t);
    if (!real && real_gdpa) real = (void (*)(void *, uint64_t, uint64_t, uint32_t))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindIndexBuffer");
    if (!buffer) {
        static int said; if (said < 8) { said++;
            VKI("vkCmdBindIndexBuffer: NULL index buffer bound (offset %llu type %u) "
                "— a following vkCmdDrawIndexed will dereference null in MoltenVK\n",
                (unsigned long long)offset, indexType); }
    }
    klvk_cb_set_idx(cb, buffer);
    if (real) real(cb, buffer, offset, indexType);
}
// The actual crash site. MoltenVK will feed the index binding's MTLBuffer to
// Metal's drawIndexedPrimitives; when that MTLBuffer is nil the AGX driver walks
// a null object and the process dies (SIGSEGV KERN_INVALID_ADDRESS at 0x20,
// inside MVKCmdDrawIndexed::encode+0x768). Check the bound index buffer here and,
// if it has no live MTLBuffer, skip the draw instead — one dropped draw is far
// better than taking the process down. Guarded so we only ever skip a draw we can
// PROVE is bad: if MoltenVK does not export the MTLBuffer accessor we cannot
// verify anything and forward unchanged (preserving old behaviour).
static void VKAPI_CALL klvk_CmdDrawIndexed(void *cb, uint32_t indexCount,
        uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
        uint32_t firstInstance) {
    static void (*real)(void *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdDrawIndexed");
    if (eyedraw_on()) {
        pthread_mutex_lock(&g_lock);
        int k = ed_slot(cb);
        static int printed;
        if (printed < 400) {
            if (ed_is_eye_pipe(g_ed_cb[k].pipe)) {
                for (int j = 0; j < g_ed_nlast; j++) { ed_print_draw("   before", g_ed_lastvb1off[j]); printed++; }
                ed_print_draw("EYE", g_ed_cb[k].vb1off + 4ull * firstInstance); printed++;
                g_ed_near = 3; g_ed_nlast = 0;
            } else {
                if (g_ed_near > 0) { g_ed_near--; ed_print_draw("   after", g_ed_cb[k].vb1off + 4ull * firstInstance); printed++; }
                if (g_ed_nlast < 3) g_ed_lastvb1off[g_ed_nlast++] = g_ed_cb[k].vb1off + 4ull * firstInstance;
                else { g_ed_lastvb1off[0] = g_ed_lastvb1off[1]; g_ed_lastvb1off[1] = g_ed_lastvb1off[2]; g_ed_lastvb1off[2] = g_ed_cb[k].vb1off + 4ull * firstInstance; }
            }
        }
        if (ed_is_eye_pipe(g_ed_cb[k].pipe) && g_ned_draw < KLED_DRAWS) {
            g_ed_draw[g_ned_draw].vb1 = g_ed_cb[k].vb1; g_ed_draw[g_ned_draw].vb1off = g_ed_cb[k].vb1off;
            g_ed_draw[g_ned_draw].xbuf = g_ed_cb[k].xbuf; g_ed_draw[g_ned_draw].xoff = g_ed_cb[k].xoff;
            g_ed_draw[g_ned_draw].xrows = g_ed_cb[k].xrows; g_ed_draw[g_ned_draw].first = firstInstance;
            g_ed_draw[g_ned_draw].count = instanceCount; g_ned_draw++;
        }
        pthread_mutex_unlock(&g_lock);
    }
    // KL_VK_DRAW_GUARD (default OFF): the skip below drops a draw whenever it
    // cannot find a live MTLBuffer for the index binding. That decision rests on
    // g_cb_idx, a 64-slot, UNLOCKED table that is never drained (no
    // vkFreeCommandBuffers/vkResetCommandPool removal) and is written from every
    // command-recording thread. Under a heavily multithreaded recorder (UE5
    // mobile parallel translate) it (a) races and (b) fills with stale command-
    // buffer handles, after which a legitimately-bound draw reads buf==0 and is
    // SKIPPED — i.e. geometry silently goes INVISIBLE. That is exactly the class
    // of symptom olar shows, so the guard must be opt-in: enable it only for the
    // title whose nil-indexBuffer crash it was written for (Into the Radius), and
    // leave every other title on faithful pass-through. Defaulted per-target ON
    // for intotheradius (so its crash-fix survives) and OFF elsewhere — a plain
    // default-OFF would regress intotheradius straight back into the MoltenVK
    // nil-indexBuffer crash the skip exists to prevent.
    extern const char *kl_driver_target_name(void);
    static int guard = -1;
    if (guard < 0) {
        const char *t = kl_driver_target_name();
        int def = (t && strcmp(t, "intotheradius") == 0) ? 1 : 0;
        guard = kl_env_on("KL_VK_DRAW_GUARD", def);
    }
    if (guard && klvk_have_buf_mtl()) {
        uint64_t buf = klvk_cb_get_idx(cb);
        if (!buf || !klvk_buf_mtl(buf)) {
            static int said; if (said < 16) { said++;
                VKI("vkCmdDrawIndexed: bound index buffer %#llx has no MTLBuffer "
                    "(%s) — skipping draw (indexCount %u instanceCount %u "
                    "firstIndex %u) to avoid the nil-indexBuffer crash in "
                    "MoltenVK's MVKCmdDrawIndexed::encode\n",
                    (unsigned long long)buf, buf ? "unbacked" : "none bound",
                    indexCount, instanceCount, firstIndex); }
            return;
        }
    }
    if (real) real(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}
static void VKAPI_CALL klvk_CmdBindPipeline(void *cb, uint32_t bindPoint, uint64_t pipeline) {
    static void (*real)(void *, uint32_t, uint64_t);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint64_t))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindPipeline");
    if (!pipeline) { static int said; if (said < 8) { said++;
        VKI("vkCmdBindPipeline: NULL pipeline bound (bindPoint %u) — a following "
            "draw will dereference null in MoltenVK\n", bindPoint); } }
    if (eyedraw_on() && bindPoint == 0) { pthread_mutex_lock(&g_lock); g_ed_cb[ed_slot(cb)].pipe = pipeline; pthread_mutex_unlock(&g_lock); }
    if (real) real(cb, bindPoint, pipeline);
}
// Defined in the 2D-mirror block below; used here to record the bind-time GPU copy.
#define KLVK_BIND_SEEN 128   /* max distinct mirrors deduped per bind / push call */
static void klvk_dset_gpu_copy(VkCommandBuffer cb, uint64_t set, int *seen, int *nseen);
static void VKAPI_CALL klvk_CmdBindDescriptorSets(void *cb, uint32_t bindPoint,
        uint64_t layout, uint32_t firstSet, uint32_t count, const uint64_t *sets,
        uint32_t dynCount, const uint32_t *dyn) {
    static void (*real)(void *, uint32_t, uint64_t, uint32_t, uint32_t, const uint64_t *, uint32_t, const uint32_t *);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint64_t, uint32_t, uint32_t, const uint64_t *, uint32_t, const uint32_t *))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindDescriptorSets");
    if (sets) for (uint32_t i = 0; i < count; i++) if (!sets[i]) {
        static int said; if (said < 8) { said++;
            VKI("vkCmdBindDescriptorSets: NULL descriptor set at %u — a draw using "
                "it may dereference null in MoltenVK\n", firstSet + i); } break; }
    // 2D-mirror freshness (olar/wanderer only): record a GPU vkCmdCopyBufferToImage
    // (bracketed by barriers) that fills each mirror image used by the sets being
    // bound, ON THIS command buffer, right before the draws that sample it. The
    // copy executes at submit — AFTER the guest's per-frame CPU writes to its pool
    // — so the mirror is always fresh, and OPTIMAL tiling makes it sampleable
    // (both problems the KL_VK_EMU_PROBE run exposed). Deduplicated per bind so one
    // bind copies each mirror once; a later re-bind copies again (freshness).
    // When emu_on() is false this whole block is skipped and the call below is the
    // exact passthrough it was.
    if (emu_on() && sets) {
        int seen[KLVK_BIND_SEEN]; int nseen = 0;
        pthread_mutex_lock(&g_lock);
        for (uint32_t i = 0; i < count; i++)
            if (sets[i]) klvk_dset_gpu_copy((VkCommandBuffer)cb, sets[i], seen, &nseen);
        pthread_mutex_unlock(&g_lock);
    }
    if (real) real(cb, bindPoint, layout, firstSet, count, sets, dynCount, dyn);
}
static void VKAPI_CALL klvk_CmdBindVertexBuffers(void *cb, uint32_t first,
        uint32_t count, const uint64_t *buffers, const uint64_t *offsets) {
    static void (*real)(void *, uint32_t, uint32_t, const uint64_t *, const uint64_t *);
    if (!real && real_gdpa) real = (void (*)(void *, uint32_t, uint32_t, const uint64_t *, const uint64_t *))
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindVertexBuffers");
    if (buffers) for (uint32_t i = 0; i < count; i++) if (!buffers[i]) {
        static int said; if (said < 8) { said++;
            VKI("vkCmdBindVertexBuffers: NULL vertex buffer at binding %u — a draw "
                "using it will dereference null in MoltenVK\n", first + i); }
        break;
    }
    if (eyedraw_on() && buffers && first <= 1 && first + count > 1) {
        pthread_mutex_lock(&g_lock);
        int k = ed_slot(cb); g_ed_cb[k].vb1 = buffers[1 - first]; g_ed_cb[k].vb1off = offsets ? offsets[1 - first] : 0;
        pthread_mutex_unlock(&g_lock);
    }
    if (real) real(cb, first, count, buffers, offsets);
}

// KL_VK_DUMP_TEXELBUF (diagnostic, default OFF) — name every UNIFORM/STORAGE
// texel buffer (samplerBuffer / imageBuffer) the guest creates, with its format
// and byte range. This is the cheapest A/B for the black-vs-invisible split on a
// pass-through title like olar: Klepton neither substitutes, drops, nor rejects
// any of olar's own resources (log-proven), so the corruption is inside
// MoltenVK's translation of specific draws. UE mobile fetches per-instance and
// per-bone transforms through texel buffers, and a texel buffer MoltenVK reads
// as zero collapses that geometry to the origin — i.e. it goes INVISIBLE — while
// a colour/param texel buffer read wrong renders BLACK. This hook does not
// change behaviour; it only reveals whether olar uses texel buffers and which
// formats, so the two symptom sets can be correlated to a resource class.
// (There is no in-repo texel-buffer emulation on the Vulkan path — only the GLES
// path has klfb_rewrite_texel_buffers — so a confirmed miss here points at a
// MoltenVK gap, not a Klepton wrapper.)
// Faithful passthrough wrappers that also feed the probe. When the probe is off
// they are one cached-flag test plus the real call.
static VkResult VKAPI_CALL klvk_CreateBuffer(VkDevice dev, const VkBufferCreateInfo *ci,
        const VkAllocationCallbacks *a, VkBuffer *out) {
    static PFN_vkCreateBuffer real;
    if (!real && real_gdpa) real = (PFN_vkCreateBuffer)real_gdpa(dev, "vkCreateBuffer");
    // 2D-mirror (olar/wanderer only): the per-bind GPU copy uses the guest's texel-
    // buffer POOL as a vkCmdCopyBufferToImage SOURCE, which requires TRANSFER_SRC
    // usage the guest did not ask for. Add it to any buffer created with texel-
    // buffer usage. Adding a usage bit is transparent to the guest (the memory
    // requirements it queries afterwards already reflect it), and it is gated so no
    // other title's buffers change. Everything else is the exact passthrough.
    VkBufferCreateInfo mod;
    if (emu_on() && ci &&
        (ci->usage & (VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                      VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)) &&
        !(ci->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        mod = *ci;
        mod.usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        ci = &mod;
    }
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (track_on() && r == VK_SUCCESS && ci && out) {
        pthread_mutex_lock(&g_lock);
        if (g_probe_nbuf < KLVK_PROBE_BUFS) {
            struct probe_buf *e = &g_probe_bufs[g_probe_nbuf++];
            e->buf = *out; e->size = ci->size; e->usage = ci->usage;
            e->mem = VK_NULL_HANDLE; e->memoff = 0; e->upload_dst = 0;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}
static VkResult VKAPI_CALL klvk_BindBufferMemory(VkDevice dev, VkBuffer buf,
        VkDeviceMemory mem, VkDeviceSize off) {
    static PFN_vkBindBufferMemory real;
    if (!real && real_gdpa) real = (PFN_vkBindBufferMemory)real_gdpa(dev, "vkBindBufferMemory");
    VkResult r = real ? real(dev, buf, mem, off) : VK_ERROR_INITIALIZATION_FAILED;
    if (track_on() && r == VK_SUCCESS) {
        pthread_mutex_lock(&g_lock);
        struct probe_buf *e = probe_buf_find(buf);
        if (e) { e->mem = mem; e->memoff = off; }
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}
static VkResult VKAPI_CALL klvk_MapMemory(VkDevice dev, VkDeviceMemory mem,
        VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags flags, void **ppData) {
    static PFN_vkMapMemory real;
    if (!real && real_gdpa) real = (PFN_vkMapMemory)real_gdpa(dev, "vkMapMemory");
    VkResult r = real ? real(dev, mem, off, size, flags, ppData)
                      : VK_ERROR_INITIALIZATION_FAILED;
    // track_on(), not probe_on(): the 2D-mirror freshness copy needs the source
    // pool's mapped pointer, so emu_on() (which track_on() includes) must record
    // it too. Off for every non-olar/wanderer, non-probe title.
    if (track_on() && r == VK_SUCCESS && ppData) {
        pthread_mutex_lock(&g_lock);
        struct probe_mem *e = probe_mem_find(mem);
        if (!e && g_probe_nmem < KLVK_PROBE_MEMS) {
            e = &g_probe_mems[g_probe_nmem++];
            e->mem = mem; e->type_index = 0;
        }
        if (e) { e->mapped = *ppData; e->map_off = off; e->map_size = size; }
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}
static void klvk_probe_uv_staging(VkBuffer src, VkBuffer dst, uint32_t rc,
                                  const VkBufferCopy *regions);  // defined after emu_mirror
static void VKAPI_CALL klvk_CmdCopyBuffer(VkCommandBuffer cb, VkBuffer src, VkBuffer dst,
        uint32_t rc, const VkBufferCopy *regions) {
    static PFN_vkCmdCopyBuffer real;
    if (!real && real_gdpa) real = (PFN_vkCmdCopyBuffer)
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdCopyBuffer");
    if (probe_on()) {
        pthread_mutex_lock(&g_lock);
        struct probe_buf *e = probe_buf_find(dst);
        if (e) e->upload_dst = 1;
        pthread_mutex_unlock(&g_lock);
    }
    // PROBE (olar gray hunt): when this copy fills a DEVICE-LOCAL UV texel buffer we
    // mirror, read the staging SRC (host-visible) to see the UV data at upload time.
    // "has data" => UVs are uploaded fine (gray is then ASTC decode or the mirror
    // copy, not missing UVs); "ZERO" => UV texcoords are absent => collapsed UVs.
    if (emu_on() && regions && rc) klvk_probe_uv_staging(src, dst, rc, regions);
    if (eyedraw_on() && regions && rc) {
        pthread_mutex_lock(&g_lock);
        struct probe_buf *e = probe_buf_find(dst);
        if (e && e->size == 8388608)
            for (uint32_t i = 0; i < rc; i++) {
                const void *sp = ed_ptr((uint64_t)(uintptr_t)src, regions[i].srcOffset);
                { static int said; if (said < 12) { said++;
                    struct probe_buf *se = probe_buf_find(src); struct probe_mem *sm = se ? probe_mem_find(se->mem) : NULL;
                    fprintf(stderr, "[eyedraw] instance copy src %#llx +%llu -> dst +%llu size %llu: src tracked %d size %llu memoff %llu mem %d mapped %d map_off %llu map_size %llu -> %s\n",
                        (unsigned long long)(uintptr_t)src, (unsigned long long)regions[i].srcOffset, (unsigned long long)regions[i].dstOffset,
                        (unsigned long long)regions[i].size, se != NULL, se ? (unsigned long long)se->size : 0ull, se ? (unsigned long long)se->memoff : 0ull,
                        sm != NULL, sm && sm->mapped, sm ? (unsigned long long)sm->map_off : 0ull, sm ? (unsigned long long)sm->map_size : 0ull, sp ? "ok" : "NULL"); } }
                if (sp && regions[i].size <= (4u << 20)) {
                    unsigned q = g_eds_n++ % 64;
                    free(g_eds_inst[q].data); g_eds_inst[q].data = malloc(regions[i].size);
                    if (g_eds_inst[q].data) { memcpy(g_eds_inst[q].data, sp, regions[i].size); g_eds_inst[q].dof = regions[i].dstOffset; g_eds_inst[q].sz = regions[i].size; }
                }
                int k = g_edc_n++ % KLEDC;
                g_edc[k].dst = (uint64_t)(uintptr_t)dst; g_edc[k].src = (uint64_t)(uintptr_t)src;
                g_edc[k].so = regions[i].srcOffset; g_edc[k].dof = regions[i].dstOffset; g_edc[k].sz = regions[i].size;
            }
        pthread_mutex_unlock(&g_lock);
    }
    if (real) real(cb, src, dst, rc, regions);
}
static void VKAPI_CALL klvk_CmdUpdateBuffer(VkCommandBuffer cb, VkBuffer dst,
        VkDeviceSize off, VkDeviceSize size, const void *data) {
    static PFN_vkCmdUpdateBuffer real;
    if (!real && real_gdpa) real = (PFN_vkCmdUpdateBuffer)
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdUpdateBuffer");
    if (probe_on()) {
        pthread_mutex_lock(&g_lock);
        struct probe_buf *e = probe_buf_find(dst);
        if (e) e->upload_dst = 1;
        pthread_mutex_unlock(&g_lock);
    }
    if (real) real(cb, dst, off, size, data);
}

// ---------------------------------------------------------------------------
// KL_VK_EMU_TEXELBUF — the fork-B fix, "2D-mirror" implementation (default ON
// for olar / wanderer via emu_on(); see the KL_VK_TEXELBUF_PROBE block above
// for the full diagnosis).
//
// PROVEN ROOT CAUSE. olar/wanderer are UE5-mobile Vulkan titles driving Manual
// Vertex Fetch: the vertex factory reads position/tangent and per-material data
// through UNIFORM texel buffers (samplerBuffer). UE packs hundreds of those
// VkBufferViews (256+) as small windows into ONE big shared host-visible
// VkBuffer (olar's pool 0x130a78f00) at many NON-ZERO byte offsets. The probe
// proved the bytes are PRESENT and live (host-visible, mapped, real f32 values
// e.g. a view at offset 132864 holding f32[0.5534 0.1493 0.8195]), and the
// vendored spirv-cross corpus proved the texelFetch->MSL translation is correct.
// What remains — and what this fixes — is MoltenVK's *runtime* VkBufferView ->
// MTLTexture creation for a view at a NON-ZERO offset: MoltenVK reads it WRONG
// (the shader gets zero -> grayscale materials + fragmented/collapsed geometry;
// UI is fine because it uses no texel buffers).
//
// WHY THE OLD ALIAS IS DEAD. The previous fix rebased each view to MTLBuffer
// offset 0 by binding a fresh VkBuffer into the same VkDeviceMemory at the
// window's absolute offset. That is a confirmed no-op even with
// MVK_CONFIG_USE_MTLHEAP=2: MoltenVK will not place mappable memory at MTLBuffer
// offset 0, so the rebased buffer resolves to the same broken absolute offset.
// The alias is removed; this replaces it.
//
// THE 2D-MIRROR. Avoid texel buffers ENTIRELY. For each texel-buffer view we
// create a Klepton-owned OPTIMAL-tiled, DEVICE_LOCAL 2D VkImage of the view's
// format, W=4096 texels wide by ceil(texelCount/4096) tall, usage SAMPLED |
// TRANSFER_DST. At descriptor-BIND time (vkCmdBindDescriptorSets / push /
// push-with-template) we record a GPU vkCmdCopyBufferToImage — bracketed by
// TRANSFER/ SHADER barriers — from the guest's pool VkBuffer into the mirror, on
// the guest's own command buffer, right before the draws that sample it.
//
// WHY GPU-COPY-AT-BIND AND NOT CPU-MEMCPY / LINEAR (what a KL_VK_EMU_PROBE run
// proved). An earlier version used a LINEAR host-visible mirror filled by a CPU
// memcpy at record time. The probe showed BOTH halves of that were broken:
//   * MoltenVK does not sample a LINEAR image reliably even when it holds the
//     exact correct bytes (probe: img==src, real data, still gray) -> OPTIMAL.
//   * a record-time CPU memcpy is STALE: the guest fills its pool per frame AFTER
//     we copy (probe: src-ZERO at copy time for the vertex mirrors) -> the copy
//     must be a GPU command that executes at submit, after the guest's writes.
// The GPU copy at bind fixes both: OPTIMAL is sampleable, and the copy runs in
// queue order after the CPU pool writes, so the mirror is always fresh; the
// barriers order it before the sampling draws. Three companion rewrites keep the
// pipeline consistent, all gated by emu_on():
//   * vkCreateShaderModule    — every OpTypeImage Dim=Buffer,Sampled=1 becomes
//                               Dim=2D, and every OpImageFetch on it gets its
//                               scalar index turned into (idx%4096, idx/4096).
//   * vkCreateDescriptorSetLayout / vkCreateDescriptorPool — UNIFORM_TEXEL_BUFFER
//                               bindings/pool sizes become SAMPLED_IMAGE, so the
//                               layout matches the rewritten shader.
//   * vkUpdateDescriptorSets   — a write to such a binding (pTexelBufferView) is
//                               replaced by a SAMPLED_IMAGE write pointing at the
//                               mirror image's VkImageView.
//
// SCOPE. This handles UNIFORM texel buffers (samplerBuffer, SPIR-V Sampled=1),
// which is exactly what olar/wanderer's Manual Vertex Fetch uses. STORAGE texel
// buffers (imageBuffer, Sampled=2, OpImageRead/Write) are intentionally LEFT ON
// PASSTHROUGH — rewriting them would need a parallel STORAGE_IMAGE path, and the
// two titles this exists for do not use them. Everything below is gated by
// emu_on(); non-olar/wanderer titles never enter any of it and take the exact
// pre-existing passthrough.
// ---------------------------------------------------------------------------

// W: texels per mirror row. Our SPIR-V rewrite computes (idx%KLVK_MIRROR_W,
// idx/KLVK_MIRROR_W); the image is laid out KLVK_MIRROR_W texels per row. The
// two MUST agree — this single constant is the only place the width lives. (4096
// also matches MoltenVK's own spvTexelBufferCoord width, which is where the
// value comes from, but nothing here depends on MoltenVK's constant since the
// Buffer dimensionality is gone by the time MoltenVK sees the module.)
#define KLVK_MIRROR_W 4096u

// Bytes per texel for the uncompressed single-plane formats a texel buffer can
// carry. 0 == unknown: the caller then builds a valid-but-dummy mirror so the
// descriptor stays a well-formed SAMPLED_IMAGE (no layout mismatch) and simply
// skips the freshness copy. olar's set (R32_UINT / R32_SFLOAT /
// R32G32B32A32_SFLOAT / R8G8B8A8_UNORM / R8G8B8A8_SNORM) is all covered here.
static uint32_t klvk_texel_size(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM:
        case VK_FORMAT_R8_UINT:  case VK_FORMAT_R8_SINT:
        case VK_FORMAT_R8_SRGB:
            return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM:
        case VK_FORMAT_R8G8_UINT:  case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R16_UNORM:  case VK_FORMAT_R16_SNORM:
        case VK_FORMAT_R16_UINT:   case VK_FORMAT_R16_SINT:
        case VK_FORMAT_R16_SFLOAT:
            return 2;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM:
        case VK_FORMAT_R8G8B8A8_UINT:  case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM:
        case VK_FORMAT_R16G16_UINT:  case VK_FORMAT_R16G16_SINT:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
        case VK_FORMAT_R32_SFLOAT:
            return 4;
        case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
        case VK_FORMAT_R16G16B16A16_UINT:  case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R32G32_SFLOAT:
            return 8;
        case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT:
        case VK_FORMAT_R32G32B32_SFLOAT:
            return 12;
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;
        default:
            return 0;
    }
}

// One mirror per (srcBuf, srcOff, range, format). The source is addressed by
// VkBuffer handle + view offset (not by absolute VkDeviceMemory) so a view whose
// buffer is bound AFTER the view is created still resolves correctly at copy
// time via probe_buf_find/probe_mem_find.
struct emu_mirror {
    VkBuffer      srcBuf;      // guest pool buffer the view windows into
    VkDeviceSize  srcOff;      // byte offset of the window within srcBuf
    VkDeviceSize  range;       // window size in bytes
    VkFormat      fmt;
    uint32_t      texelSize;   // 0 == dummy mirror (unknown format): skip copies
    uint32_t      texelCount;
    uint32_t      width;       // texels per row (== texelCount when <= W, else W)
    uint32_t      height;
    VkImage       img;         // OPTIMAL-tiled, DEVICE_LOCAL, SAMPLED|TRANSFER_DST
    VkDeviceMemory mem;
    VkImageView   view;        // what the rewritten SAMPLED_IMAGE descriptor uses
    int           everCopied;  // has a GPU copy filled it at least once
};
#define KLVK_EMU_MIRRORS 8192   /* distinct (buf,off,range,fmt) windows; olar's
                                   >4k views collapse to far fewer distinct keys */
static struct emu_mirror g_mirrors[KLVK_EMU_MIRRORS];
static int g_nmirror;

// VkBufferView handle -> mirror index, open-addressed. A bv handle is nonzero,
// so a zero slot is empty; a handle the guest destroys and the driver later
// re-mints is simply overwritten on the next CreateBufferView. Sized well above
// the observed live-view count so probes terminate quickly.
#define KLVK_EMU_BVMAP 32768
static struct { VkBufferView bv; int mirror; } g_bvmap[KLVK_EMU_BVMAP];

static uint32_t klvk_hash64(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return (uint32_t)x;
}
// Runs under g_lock. Overwrite semantics keep the table bounded under handle reuse.
static void klvk_bvmap_put(VkBufferView bv, int mirror) {
    if (!bv) return;
    uint32_t h = klvk_hash64((uint64_t)(uintptr_t)bv) & (KLVK_EMU_BVMAP - 1);
    for (int probe = 0; probe < KLVK_EMU_BVMAP; probe++) {
        uint32_t k = (h + (uint32_t)probe) & (KLVK_EMU_BVMAP - 1);
        if (g_bvmap[k].bv == bv || g_bvmap[k].bv == VK_NULL_HANDLE) {
            g_bvmap[k].bv = bv; g_bvmap[k].mirror = mirror; return;
        }
    }
    // Table full: the view falls back to no substitution. Logged once by caller.
}
static int klvk_bvmap_find(VkBufferView bv) {
    if (!bv) return -1;
    uint32_t h = klvk_hash64((uint64_t)(uintptr_t)bv) & (KLVK_EMU_BVMAP - 1);
    for (int probe = 0; probe < KLVK_EMU_BVMAP; probe++) {
        uint32_t k = (h + (uint32_t)probe) & (KLVK_EMU_BVMAP - 1);
        if (g_bvmap[k].bv == bv) return g_bvmap[k].mirror;
        if (g_bvmap[k].bv == VK_NULL_HANDLE) return -1;
    }
    return -1;
}

// (descriptorSet, binding, arrayElement) -> mirror index, for bind-time freshness.
// Chained by a hash of the set handle so a bind can walk just its own set's
// entries. Overwrite-by-key keeps it bounded under descriptor-set handle reuse
// (UE resets/recycles pools, so handles repeat rather than growing without end).
#define KLVK_EMU_DSET_ENTS    65536
#define KLVK_EMU_DSET_BUCKETS 16384
struct emu_dset_ent { uint64_t set; uint32_t binding; uint32_t elem; int mirror; int next; };
static struct emu_dset_ent g_dset_ents[KLVK_EMU_DSET_ENTS];
static int g_ndset_ent;
static int g_dset_head[KLVK_EMU_DSET_BUCKETS];
static int g_dset_ready;
static void klvk_dset_init(void) {   // runs under g_lock
    if (g_dset_ready) return;
    for (int i = 0; i < KLVK_EMU_DSET_BUCKETS; i++) g_dset_head[i] = -1;
    g_dset_ready = 1;
}
// Runs under g_lock.
static void klvk_dset_put(uint64_t set, uint32_t binding, uint32_t elem, int mirror) {
    klvk_dset_init();
    uint32_t b = klvk_hash64(set) & (KLVK_EMU_DSET_BUCKETS - 1);
    for (int i = g_dset_head[b]; i >= 0; i = g_dset_ents[i].next) {
        struct emu_dset_ent *e = &g_dset_ents[i];
        if (e->set == set && e->binding == binding && e->elem == elem) {
            e->mirror = mirror; return;                 // rewrite in place
        }
    }
    if (g_ndset_ent >= KLVK_EMU_DSET_ENTS) return;      // full: bind-time refresh
                                                        // skipped; the update-time
                                                        // copy already ran, so the
                                                        // window is at worst stale
                                                        // across frames, never a crash
    struct emu_dset_ent *e = &g_dset_ents[g_ndset_ent];
    e->set = set; e->binding = binding; e->elem = elem; e->mirror = mirror;
    e->next = g_dset_head[b];
    g_dset_head[b] = g_ndset_ent++;
}

// Record a GPU copy of the live source window into the mirror image on `cb`,
// bracketed by the layout/visibility barriers. Runs under g_lock (touches only
// the mirror record + records commands into the caller's own command buffer).
//
// WHY GPU + BIND TIME. The probe proved two things: MoltenVK does not sample a
// LINEAR image reliably even when it holds correct bytes, and a record-time CPU
// memcpy is stale because the guest fills its pool per frame AFTER we copy. Both
// are fixed here: the mirror is OPTIMAL (sampleable), and the copy is a real
// vkCmdCopyBufferToImage recorded onto the guest's command buffer at descriptor
// bind — it executes at submit, AFTER the guest's CPU writes, and the barriers
// order it before the draws that sample it. The source is the guest pool VkBuffer
// itself (klvk_CreateBuffer adds TRANSFER_SRC to texel-buffer pools so it can be
// a copy source); bufferOffset is the VIEW offset relative to that buffer (the
// same ci->offset the view was created with), NOT any device-memory offset.
// See the forward declaration at klvk_CmdCopyBuffer: read the staging SRC bytes that
// a staging->device copy is about to write into a UV texel buffer we mirror, so we
// learn whether UV texcoords are real or zero. Runs under its own g_lock section.
static void klvk_probe_uv_staging(VkBuffer src, VkBuffer dst, uint32_t rc,
                                  const VkBufferCopy *regions) {
    static int shownUV, attempts;
    pthread_mutex_lock(&g_lock);
    if (shownUV < 24 && attempts < 600) {
        attempts++;
        for (int mi = 0; mi < g_nmirror && shownUV < 24; mi++) {
            struct emu_mirror *m = &g_mirrors[mi];
            if (m->srcBuf != dst) continue;
            if (!(m->fmt == 103 || m->fmt == 83 || m->fmt == 77)) continue;  // UV formats
            struct probe_buf *sb = probe_buf_find(src);
            struct probe_mem *pm = (sb && sb->mem) ? probe_mem_find(sb->mem) : NULL;
            if (!pm || !pm->mapped) continue;
            for (uint32_t ri = 0; ri < rc; ri++) {
                if (regions[ri].dstOffset > m->srcOff ||
                    regions[ri].dstOffset + regions[ri].size < m->srcOff + m->range) continue;
                VkDeviceSize srcByte = regions[ri].srcOffset + (m->srcOff - regions[ri].dstOffset);
                VkDeviceSize abs = sb->memoff + srcByte;
                VkDeviceSize mend = (pm->map_size == VK_WHOLE_SIZE) ? (VkDeviceSize)~0ull
                                                                   : pm->map_off + pm->map_size;
                if (abs < pm->map_off || abs >= mend) continue;
                const uint8_t *s = (const uint8_t *)pm->mapped + (size_t)(abs - pm->map_off);
                uint32_t su[4] = {0,0,0,0};
                size_t nb = m->range < 16 ? (size_t)m->range : 16;
                memcpy(su, s, nb);
                shownUV++;
                VKI("[emu] UV staging src: mirror #%d fmt %d range %llu: "
                    "[%08x %08x %08x %08x] -> %s\n", mi, (int)m->fmt,
                    (unsigned long long)m->range, su[0], su[1], su[2], su[3],
                    (su[0] | su[1] | su[2] | su[3]) ? "has data" : "ZERO");
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
}
static void klvk_mirror_gpu_copy(VkCommandBuffer cb, struct emu_mirror *m) {
    static PFN_vkCmdPipelineBarrier   rBarrier;
    static PFN_vkCmdCopyBufferToImage rCopy;
    static int resolved;
    if (!resolved) { resolved = 1;
        if (real_gdpa) {
            VkDevice dv = g_devs[0] ? g_devs[0]->dev : NULL;
            rBarrier = (PFN_vkCmdPipelineBarrier)  real_gdpa(dv, "vkCmdPipelineBarrier");
            rCopy    = (PFN_vkCmdCopyBufferToImage)real_gdpa(dv, "vkCmdCopyBufferToImage");
        }
    }
    if (!rBarrier || !rCopy || !cb || !m || m->texelSize == 0 || !m->img || !m->srcBuf)
        return;

    // (1) -> TRANSFER_DST. oldLayout UNDEFINED discards the previous frame's bytes
    // (the copy overwrites the whole sampled region), while srcStage waits for the
    // prior frame's shader reads (WAR) before the transfer overwrites.
    VkImageMemoryBarrier b; memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m->img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    rBarrier(cb, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    // (2) copy source pool bytes -> image, row-major KLVK_MIRROR_W texels/row.
    // Split into whole rows + a partial last row so we never read past the window.
    VkBufferImageCopy rgn[2]; memset(rgn, 0, sizeof rgn);
    uint32_t nr = 0, W = KLVK_MIRROR_W, tc = m->texelCount, ts = m->texelSize;
    if (m->height <= 1) {
        rgn[0].bufferOffset = m->srcOff;
        rgn[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rgn[0].imageSubresource.layerCount = 1;
        rgn[0].imageExtent.width = tc; rgn[0].imageExtent.height = 1; rgn[0].imageExtent.depth = 1;
        nr = 1;
    } else {
        uint32_t fullRows = tc / W, rem = tc % W;
        rgn[0].bufferOffset = m->srcOff;
        rgn[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rgn[0].imageSubresource.layerCount = 1;
        rgn[0].imageExtent.width = W; rgn[0].imageExtent.height = fullRows; rgn[0].imageExtent.depth = 1;
        nr = 1;
        if (rem) {
            rgn[1].bufferOffset = m->srcOff + (VkDeviceSize)fullRows * W * ts;
            rgn[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rgn[1].imageSubresource.layerCount = 1;
            rgn[1].imageOffset.y = (int32_t)fullRows;
            rgn[1].imageExtent.width = rem; rgn[1].imageExtent.height = 1; rgn[1].imageExtent.depth = 1;
            nr = 2;
        }
    }
    rCopy(cb, m->srcBuf, m->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nr, rgn);

    // DECISIVE TEST (KL_VK_UV_CONST=1): overwrite UV-format mirrors with a constant
    // (0.5,0.5) right after the copy (still TRANSFER_DST). Every vertex then samples
    // the CENTER of its albedo texture. If the gray world VISIBLY CHANGES (flat
    // per-surface colors appear), the mirror IS the UV source and the ASTC albedo
    // does hold colour -> collapsed UVs are the bug. If it STAYS gray, ASTC is
    // decoding to gray. If UNCHANGED, UVs come from elsewhere. Diagnostic; off by default.
    if (kl_env_on("KL_VK_UV_CONST", 0) &&
        (m->fmt == 103 || m->fmt == 83 || m->fmt == 77)) {
        static PFN_vkCmdClearColorImage clr;
        if (!clr) clr = (PFN_vkCmdClearColorImage)mvk_sym("vkCmdClearColorImage");
        if (clr) {
            VkClearColorValue uv; uv.float32[0] = 0.5f; uv.float32[1] = 0.5f;
            uv.float32[2] = 0.0f; uv.float32[3] = 0.0f;
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            clr(cb, m->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &uv, 1, &rng);
        }
    }

    // (3) TRANSFER_DST -> SHADER_READ_ONLY, transfer write visible to shader reads.
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    rBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             0, 0, NULL, 0, NULL, 1, &b);
    m->everCopied = 1;

    // KL_VK_EMU_PROBE (default OFF) — record-time SOURCE readback. The mirror is
    // now DEVICE_LOCAL/OPTIMAL and not mappable, so we can only inspect the source
    // pool (host-visible+mapped, tracked). Confirms the pool window holds real data
    // at the moment we RECORD the copy (the copy itself runs later at submit, reading
    // the then-current bytes). src-ZERO here on a meaningful mirror hints the guest
    // fills this window even later than record time.
    if (emu_probe() || emu_on()) {
        // Broadened (olar gray hunt): ALSO dump the 2-component UV texel buffers
        // (103=R32G32_SFLOAT, 83=R16G16_SFLOAT) and other small attribute buffers —
        // the old filter only showed vec4 transforms, so UV data was never inspected.
        // If UV buffers read ZERO here, texcoords collapse -> every surface samples
        // one texel -> flat gray (matches the symptom); if they hold sane [0,1]-ish
        // values, UVs are fine and the gray is an ASTC-decode issue instead.
        int isUV = (m->fmt == 103 /*R32G32_SFLOAT*/) || (m->fmt == 83 /*R16G16_SFLOAT*/) ||
                   (m->fmt == 77 /*R16G16_UNORM*/)   || (m->fmt == 104 /*R32G32B32*/);
        int interesting = (m->fmt == VK_FORMAT_R32G32B32A32_SFLOAT) ||
                          (m->width == KLVK_MIRROR_W) || isUV;
        struct probe_buf *sb = interesting ? probe_buf_find(m->srcBuf) : NULL;
        struct probe_mem *pm = (sb && sb->mem) ? probe_mem_find(sb->mem) : NULL;
        if (pm && pm->mapped) {
            VkDeviceSize abs = sb->memoff + m->srcOff;
            VkDeviceSize mend = (pm->map_size == VK_WHOLE_SIZE)
                              ? (VkDeviceSize)~0ull : pm->map_off + pm->map_size;
            if (abs >= pm->map_off && abs < mend) {
                const uint8_t *s = (const uint8_t *)pm->mapped + (size_t)(abs - pm->map_off);
                uint32_t su[4] = {0,0,0,0};
                size_t nb = (size_t)m->range < 16 ? (size_t)m->range : 16;
                memcpy(su, s, nb);
                float sf[4]; for (int k = 0; k < 4; k++) memcpy(&sf[k], &su[k], 4);
                // Show UV-format buffers on their own budget so the vec4 transforms
                // (which we already know have data) don't crowd them out.
                static int shown, shownUV;
                int budget = isUV ? (shownUV < 24) : (shown < 8);
                if (budget) { if (isUV) shownUV++; else shown++;
                    VKI("[emu] probe src mirror #%d fmt %d %ux%u%s: pool[%08x %08x %08x %08x] "
                        "f[%.4g %.4g %.4g %.4g] at record time -> %s\n",
                        (int)(m - g_mirrors), (int)m->fmt, m->width, m->height,
                        isUV ? " (UV?)" : "",
                        su[0], su[1], su[2], su[3], sf[0], sf[1], sf[2], sf[3],
                        (su[0] | su[1] | su[2] | su[3]) ? "has data" : "ZERO"); }
            }
        }
    }
}

// Record the GPU copy for every mirror bound through descriptor set `set`, into
// command buffer `cb`, deduplicated against `seen` so one bind copies each mirror
// at most once (but a later re-bind copies again — that is the freshness). Runs
// under g_lock.
static void klvk_dset_gpu_copy(VkCommandBuffer cb, uint64_t set, int *seen, int *nseen) {
    if (!g_dset_ready) return;
    uint32_t bkt = klvk_hash64(set) & (KLVK_EMU_DSET_BUCKETS - 1);
    for (int i = g_dset_head[bkt]; i >= 0; i = g_dset_ents[i].next) {
        if (g_dset_ents[i].set != set) continue;
        int mi = g_dset_ents[i].mirror;
        if (mi < 0 || mi >= g_nmirror) continue;
        int dup = 0;
        for (int s = 0; s < *nseen; s++) if (seen[s] == mi) { dup = 1; break; }
        if (dup) continue;
        if (*nseen < KLVK_BIND_SEEN) seen[(*nseen)++] = mi;
        klvk_mirror_gpu_copy(cb, &g_mirrors[mi]);
    }
}


// Create-or-find the mirror for a texel-buffer view. Returns the mirror index,
// or -1 only if even the dummy fallback could not be built (then the caller
// keeps the plain passthrough view — the sole case where a layout/descriptor
// mismatch is possible, and it is logged). Runs under g_lock.
static int klvk_mirror_get(VkDevice dev, VkBuffer srcBuf, VkDeviceSize srcOff,
                           VkDeviceSize ciRange, VkFormat fmt) {
    static PFN_vkCreateImage                 rCreateImg;
    static PFN_vkGetImageMemoryRequirements  rImgReq;
    static PFN_vkAllocateMemory              rAlloc;
    static PFN_vkBindImageMemory            rBind;
    static PFN_vkCreateImageView             rCreateView;
    static PFN_vkDestroyImage                rDestroyImg;
    static PFN_vkFreeMemory                  rFree;
    static int resolved;
    if (!resolved) {
        resolved = 1;
        if (real_gdpa) {
            rCreateImg  = (PFN_vkCreateImage)                real_gdpa(dev, "vkCreateImage");
            rImgReq     = (PFN_vkGetImageMemoryRequirements) real_gdpa(dev, "vkGetImageMemoryRequirements");
            rAlloc      = (PFN_vkAllocateMemory)             real_gdpa(dev, "vkAllocateMemory");
            rBind       = (PFN_vkBindImageMemory)            real_gdpa(dev, "vkBindImageMemory");
            rCreateView = (PFN_vkCreateImageView)            real_gdpa(dev, "vkCreateImageView");
            rDestroyImg = (PFN_vkDestroyImage)               real_gdpa(dev, "vkDestroyImage");
            rFree       = (PFN_vkFreeMemory)                 real_gdpa(dev, "vkFreeMemory");
        }
    }
    if (!rCreateImg || !rImgReq || !rAlloc || !rBind || !rCreateView)
        return -1;

    // Resolve VK_WHOLE_SIZE against the tracked source buffer size.
    VkDeviceSize range = ciRange;
    if (range == VK_WHOLE_SIZE) {
        struct probe_buf *sb = probe_buf_find(srcBuf);
        range = (sb && sb->size > srcOff) ? (sb->size - srcOff) : 0;
    }
    if (range == 0) return -1;

    // Cache hit?
    for (int i = 0; i < g_nmirror; i++) {
        struct emu_mirror *e = &g_mirrors[i];
        if (e->srcBuf == srcBuf && e->srcOff == srcOff &&
            e->range == range && e->fmt == fmt) return i;
    }
    if (g_nmirror >= KLVK_EMU_MIRRORS) return -1;

    uint32_t tsz = klvk_texel_size(fmt);
    // Whole texels only (floor): the GPU copy must not read past the window, and a
    // trailing partial texel is never fetched by the shader anyway.
    uint32_t tcount = tsz ? (uint32_t)(range / tsz) : 1;
    if (tcount == 0) tcount = 1;
    uint32_t width, height;
    if (tcount <= KLVK_MIRROR_W) { width = tcount; height = 1; }
    else { width = KLVK_MIRROR_W; height = (tcount + KLVK_MIRROR_W - 1) / KLVK_MIRROR_W; }

    klvk_device *d = dev_find(dev);
    if (!d) return -1;

    // Build the OPTIMAL-tiled DEVICE_LOCAL sampled 2D image in the view's own
    // format, also usable as a transfer destination (the per-bind GPU copy fills
    // it). OPTIMAL is what MoltenVK samples reliably (linear proved unreliable
    // even holding correct bytes). If the exact format cannot be an optimal
    // sampled+transfer_dst image on this driver, fall back to a 1x1 RGBA8 image so
    // the descriptor stays a valid SAMPLED_IMAGE (no layout mismatch); its GPU
    // copy is then disabled (texelSize left 0).
    VkFormat imgFmt = fmt;
    uint32_t useTsz = tsz, useW = width, useH = height;
    VkImage img = VK_NULL_HANDLE;
    for (int attempt = 0; attempt < 2; attempt++) {
        VkImageCreateInfo ici;
        memset(&ici, 0, sizeof ici);
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = imgFmt;
        ici.extent.width  = useW; ici.extent.height = useH; ici.extent.depth = 1;
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (rCreateImg(dev, &ici, NULL, &img) == VK_SUCCESS && img) break;
        img = VK_NULL_HANDLE;
        imgFmt = VK_FORMAT_R8G8B8A8_UNORM; useTsz = 0; useW = 1; useH = 1;  // dummy
    }
    if (!img) return -1;

    VkMemoryRequirements mr; memset(&mr, 0, sizeof mr);
    rImgReq(dev, img, &mr);
    uint32_t mt = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = mem_type(d, mr.memoryTypeBits, 0);
    if (mt == UINT32_MAX) { if (rDestroyImg) rDestroyImg(dev, img, NULL); return -1; }

    VkMemoryAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (rAlloc(dev, &ai, NULL, &mem) != VK_SUCCESS || !mem) {
        if (rDestroyImg) rDestroyImg(dev, img, NULL); return -1;
    }
    if (rBind(dev, img, mem, 0) != VK_SUCCESS) {
        if (rFree) rFree(dev, mem, NULL);
        if (rDestroyImg) rDestroyImg(dev, img, NULL); return -1;
    }

    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof vi);
    vi.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image    = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format   = imgFmt;
    vi.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel   = 0; vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = 0; vi.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (rCreateView(dev, &vi, NULL, &view) != VK_SUCCESS || !view) {
        if (rFree) rFree(dev, mem, NULL);
        if (rDestroyImg) rDestroyImg(dev, img, NULL); return -1;
    }

    struct emu_mirror *m = &g_mirrors[g_nmirror];
    m->srcBuf = srcBuf; m->srcOff = srcOff; m->range = range; m->fmt = fmt;
    m->texelSize = useTsz; m->texelCount = tcount;
    m->width = useW; m->height = useH;
    m->img = img; m->mem = mem;
    m->view = view;
    m->everCopied = 0;
    {
        static int said; if (said < 8) { said++;
            VKI("[emu] mirror #%d fmt %d %ux%u texelSize %u tcount %u: OPTIMAL "
                "device-local, filled by GPU copy at bind (shader idx width = %u)\n",
                g_nmirror, (int)fmt, useW, useH, useTsz, tcount, (unsigned)KLVK_MIRROR_W); }
    }
    return g_nmirror++;
}

// ---------------------------------------------------------------------------
// SPIR-V rewrite: OpTypeImage Dim=Buffer,Sampled=1 -> Dim=2D, and each
// OpImageFetch on such an image gets its scalar index turned into the 2-vector
// (idx % KLVK_MIRROR_W, idx / KLVK_MIRROR_W). Done directly on the word stream —
// no spirv-cross at runtime. Returns a malloc'd module (caller frees) or NULL to
// mean "leave the original untouched" (no buffer images present, or malformed).
// ---------------------------------------------------------------------------
// SPIR-V opcodes/enums we touch (values verified against the vendored
// spirv-headers unified1/spirv.h; hardcoded so this file needs no new include).
#define KLSPV_MAGIC              0x07230203u
#define KLSPV_OP_TYPE_INT        21
#define KLSPV_OP_TYPE_VECTOR     23
#define KLSPV_OP_TYPE_IMAGE      25
#define KLSPV_OP_CONSTANT        43
#define KLSPV_OP_FUNCTION        54
#define KLSPV_OP_LOAD            61
#define KLSPV_OP_COMPOSITE_CONSTRUCT 80
#define KLSPV_OP_IMAGE_FETCH     95
#define KLSPV_OP_IMAGE           100
#define KLSPV_OP_UDIV            134
#define KLSPV_OP_UMOD            137
#define KLSPV_OP_COPY_OBJECT     83
#define KLSPV_DIM_2D             1u
#define KLSPV_DIM_BUFFER         5u
#define KLSPV_IMGOP_LOD          0x2u   /* Image Operands bit: Lod (integer mip) */

static uint32_t *klvk_spirv_mirror_rewrite(const uint32_t *in, size_t nwords,
                                           size_t *out_nwords) {
    if (!in || nwords < 5 || in[0] != KLSPV_MAGIC) return NULL;
    uint32_t origBound = in[3];
    if (origBound == 0) return NULL;

    uint8_t *isBufImgType = (uint8_t *)calloc(origBound, 1);   // OpTypeImage Buffer,Sampled=1
    uint8_t *isBufImgVal  = (uint8_t *)calloc(origBound, 1);   // values loaded from one
    if (!isBufImgType || !isBufImgVal) { free(isBufImgType); free(isBufImgVal); return NULL; }

    // --- Pass 1: global declarations (everything before the first OpFunction).
    // Find buffer-image types, a reusable uint32 type / const 4096 / v2uint, and
    // the offset of the first function (our global-insertion point).
    uint32_t uintType = 0, c4096 = 0, c0 = 0, v2uintType = 0, firstFuncOff = 0;
    // Track which type ids are 32-bit integer types, to validate the const's type.
    uint8_t *isInt32Type = (uint8_t *)calloc(origBound, 1);
    if (!isInt32Type) { free(isBufImgType); free(isBufImgVal); return NULL; }
    {
        size_t off = 5;
        while (off < nwords) {
            uint32_t w = in[off];
            uint32_t op = w & 0xFFFFu, wc = w >> 16;
            if (wc == 0 || off + wc > nwords) break;   // malformed: bail to passthrough
            if (op == KLSPV_OP_FUNCTION) { firstFuncOff = (uint32_t)off; break; }
            if (op == KLSPV_OP_TYPE_IMAGE && wc >= 9) {
                uint32_t id = in[off + 1];
                uint32_t dim = in[off + 3], sampled = in[off + 7];
                if (dim == KLSPV_DIM_BUFFER && sampled == 1 && id < origBound)
                    isBufImgType[id] = 1;
                // PROBE (olar gray hunt): a Dim=Buffer image with Sampled=2 is a
                // STORAGE texel buffer (imageBuffer / OpImageRead) — the emu does NOT
                // rewrite these, so their reads hit the raw MoltenVK-misread buffer.
                // If olar's world shaders declare these, that is the gray/invisible
                // root (mirror created, never bound-through-a-rewrite). Count + log so
                // one run tells us uniform-vs-storage definitively.
                if (dim == KLSPV_DIM_BUFFER && sampled == 2) {
                    static int said;
                    if (said < 12) { said++;
                        VKI("[emu] PROBE: shader declares a STORAGE texel buffer "
                            "(OpTypeImage Dim=Buffer Sampled=2, fmt word=%u) — NOT "
                            "rewritten by the emu; this is a gray/invisible suspect\n",
                            wc >= 9 ? in[off + 8] : 0); }
                }
            } else if (op == KLSPV_OP_TYPE_INT && wc >= 4) {
                uint32_t id = in[off + 1], wdt = in[off + 2], sgn = in[off + 3];
                if (wdt == 32 && id < origBound) {
                    isInt32Type[id] = 1;
                    if (sgn == 0 && uintType == 0) uintType = id;   // prefer unsigned
                }
            } else if (op == KLSPV_OP_CONSTANT && wc >= 4) {
                uint32_t rt = in[off + 1], id = in[off + 2], val = in[off + 3];
                if (val == 4096 && rt < origBound && isInt32Type[rt] && c4096 == 0) {
                    c4096 = id;
                    if (uintType == 0) uintType = rt;   // usable as our coord scalar type
                }
                if (val == 0 && rt < origBound && isInt32Type[rt] && c0 == 0)
                    c0 = id;                            // any 32-bit int 0 works as the
                                                        // integer Lod (sign irrelevant)
            } else if (op == KLSPV_OP_TYPE_VECTOR && wc >= 4) {
                uint32_t id = in[off + 1], comp = in[off + 2], cnt = in[off + 3];
                if (cnt == 2 && uintType != 0 && comp == uintType && v2uintType == 0)
                    v2uintType = id;
            }
            off += wc;
        }
    }

    // No texel-buffer image in this module -> nothing to do (UI/other shaders).
    int anyBufImg = 0;
    for (uint32_t i = 0; i < origBound; i++) if (isBufImgType[i]) { anyBufImg = 1; break; }
    if (!anyBufImg) { free(isBufImgType); free(isBufImgVal); free(isInt32Type); return NULL; }

    // --- Pass 2: over the function bodies, mark values of buffer-image type and
    // count the fetches we will rewrite (so we can size the new id space).
    uint32_t fetchCount = 0;
    if (firstFuncOff) {
        size_t off = firstFuncOff;
        while (off < nwords) {
            uint32_t w = in[off];
            uint32_t op = w & 0xFFFFu, wc = w >> 16;
            if (wc == 0 || off + wc > nwords) break;
            if ((op == KLSPV_OP_LOAD || op == KLSPV_OP_IMAGE || op == KLSPV_OP_COPY_OBJECT)
                && wc >= 3) {
                uint32_t rt = in[off + 1], rid = in[off + 2];
                if (rt < origBound && isBufImgType[rt] && rid < origBound)
                    isBufImgVal[rid] = 1;
                else if (op == KLSPV_OP_COPY_OBJECT && wc >= 4) {
                    uint32_t srcv = in[off + 3];
                    if (srcv < origBound && isBufImgVal[srcv] && rid < origBound)
                        isBufImgVal[rid] = 1;   // propagate through a copy of the value
                }
            } else if (op == KLSPV_OP_IMAGE_FETCH && wc >= 5) {
                uint32_t imgv = in[off + 3];
                if (imgv < origBound && isBufImgVal[imgv]) fetchCount++;
            }
            off += wc;
        }
    }

    // Plan the new ids. uintType/c4096/v2uintType may be created if absent.
    uint32_t bound = origBound;
    uint32_t newUint = 0, newC4096 = 0, newC0 = 0, newV2 = 0;
    if (uintType == 0)  { newUint  = bound++; uintType   = newUint; }
    if (c4096 == 0)     { newC4096 = bound++; c4096      = newC4096; }
    if (c0 == 0)        { newC0    = bound++; c0         = newC0; }
    if (v2uintType == 0){ newV2    = bound++; v2uintType = newV2; }
    // 3 ids per fetch (x = umod, y = udiv, coord vector = composite).
    // (allocated inline while emitting so they land in order.)

    // Size the output generously: original + injected globals + per-fetch work.
    // Per fetch: OpUMod(5) + OpUDiv(5) + OpCompositeConstruct(5) + up to 2 extra
    // words appended to the fetch for the Lod image operand (mask + value).
    size_t injectedGlobals = (newUint ? 4 : 0) + (newC4096 ? 4 : 0) +
                             (newC0 ? 4 : 0) + (newV2 ? 4 : 0);
    size_t perFetch = (size_t)fetchCount * (5 + 5 + 5 + 2);
    size_t cap = nwords + injectedGlobals + perFetch + 16;
    uint32_t *out = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!out) { free(isBufImgType); free(isBufImgVal); free(isInt32Type); return NULL; }

    size_t o = 0;
    // Header (bound patched at the end).
    for (int i = 0; i < 5; i++) out[o++] = in[i];

    int globalsInjected = 0;
    size_t off = 5;
    while (off < nwords) {
        uint32_t w = in[off];
        uint32_t op = w & 0xFFFFu, wc = w >> 16;
        if (wc == 0 || off + wc > nwords) {
            // Malformed tail: copy the remainder verbatim and stop interpreting.
            while (off < nwords) out[o++] = in[off++];
            break;
        }
        // Inject new global types/constants immediately before the first function.
        if (!globalsInjected && firstFuncOff && off == firstFuncOff) {
            if (newUint) {   // OpTypeInt 32 0
                out[o++] = (4u << 16) | KLSPV_OP_TYPE_INT;
                out[o++] = newUint; out[o++] = 32; out[o++] = 0;
            }
            if (newC4096) {  // OpConstant uintType 4096
                out[o++] = (4u << 16) | KLSPV_OP_CONSTANT;
                out[o++] = uintType; out[o++] = newC4096; out[o++] = KLVK_MIRROR_W;
            }
            if (newC0) {     // OpConstant uintType 0  (the Lod)
                out[o++] = (4u << 16) | KLSPV_OP_CONSTANT;
                out[o++] = uintType; out[o++] = newC0; out[o++] = 0;
            }
            if (newV2) {     // OpTypeVector uintType 2
                out[o++] = (4u << 16) | KLSPV_OP_TYPE_VECTOR;
                out[o++] = newV2; out[o++] = uintType; out[o++] = 2;
            }
            globalsInjected = 1;
        }

        if (op == KLSPV_OP_TYPE_IMAGE && wc >= 9 && in[off + 1] < origBound &&
            isBufImgType[in[off + 1]]) {
            // Copy the type but flip Dim Buffer(5) -> 2D(1).
            for (uint32_t k = 0; k < wc; k++) out[o++] = in[off + k];
            out[o - wc + 3] = KLSPV_DIM_2D;
        } else if (op == KLSPV_OP_IMAGE_FETCH && wc >= 5 &&
                   in[off + 3] < origBound && isBufImgVal[in[off + 3]]) {
            uint32_t coord = in[off + 4];
            uint32_t xid = bound++, yid = bound++, vid = bound++;
            // OpUMod uintType xid coord c4096
            out[o++] = (5u << 16) | KLSPV_OP_UMOD;
            out[o++] = uintType; out[o++] = xid; out[o++] = coord; out[o++] = c4096;
            // OpUDiv uintType yid coord c4096
            out[o++] = (5u << 16) | KLSPV_OP_UDIV;
            out[o++] = uintType; out[o++] = yid; out[o++] = coord; out[o++] = c4096;
            // OpCompositeConstruct v2uintType vid xid yid
            out[o++] = (5u << 16) | KLSPV_OP_COMPOSITE_CONSTRUCT;
            out[o++] = v2uintType; out[o++] = vid; out[o++] = xid; out[o++] = yid;
            // The fetch itself. The original was OpImageFetch on Dim=Buffer, which
            // carries NO Lod operand; on the now-Dim=2D SAMPLED image a fetch is
            // only valid WITH an explicit integer Lod, and drivers/spirv-cross that
            // tolerate its absence often just return 0 (-> gray/collapsed). So:
            //   * replace the scalar coordinate (word 4) with our 2-vector, and
            //   * ensure the Lod image operand (mask 0x2, value = int 0) is present.
            // Word layout: [0]=op/wc [1]=resultType [2]=result [3]=image
            //              [4]=coordinate [5]=ImageOperands mask [6..]=operand ids.
            size_t fbase = o;
            uint32_t hasOperands = (wc >= 6);
            uint32_t oldMask = hasOperands ? in[off + 5] : 0;
            if (oldMask & KLSPV_IMGOP_LOD) {
                // Already has a Lod (unexpected for a buffer fetch): copy verbatim,
                // only swapping the coordinate.
                for (uint32_t k = 0; k < wc; k++) out[o++] = in[off + k];
                out[fbase + 4] = vid;
            } else {
                // Rebuild: header (patched wc), resultType, result, image, coord,
                // (mask|Lod), Lod value (int 0, inserted as the first operand since
                // no lower Image-Operands bit is valid on a fetch), then any
                // pre-existing operand values.
                uint32_t newWc = wc + (hasOperands ? 1u : 2u);
                out[o++] = (newWc << 16) | KLSPV_OP_IMAGE_FETCH;
                out[o++] = in[off + 1];        // result type
                out[o++] = in[off + 2];        // result id
                out[o++] = in[off + 3];        // image
                out[o++] = vid;                // coordinate (our 2-vector)
                out[o++] = oldMask | KLSPV_IMGOP_LOD;   // image operands mask
                out[o++] = c0;                 // Lod = 0
                for (uint32_t k = 6; k < wc; k++) out[o++] = in[off + k];  // existing operands
            }
        } else {
            for (uint32_t k = 0; k < wc; k++) out[o++] = in[off + k];
        }
        off += wc;
    }
    // If the module had no function (so the injection point never hit) append the
    // new globals at the very end — still valid, they precede no use.
    if (!globalsInjected) {
        if (newUint) { out[o++] = (4u << 16) | KLSPV_OP_TYPE_INT;
                       out[o++] = newUint; out[o++] = 32; out[o++] = 0; }
        if (newC4096){ out[o++] = (4u << 16) | KLSPV_OP_CONSTANT;
                       out[o++] = uintType; out[o++] = newC4096; out[o++] = KLVK_MIRROR_W; }
        if (newC0)   { out[o++] = (4u << 16) | KLSPV_OP_CONSTANT;
                       out[o++] = uintType; out[o++] = newC0; out[o++] = 0; }
        if (newV2)   { out[o++] = (4u << 16) | KLSPV_OP_TYPE_VECTOR;
                       out[o++] = newV2; out[o++] = uintType; out[o++] = 2; }
    }

    out[3] = bound;   // patch id bound
    *out_nwords = o;
    free(isBufImgType); free(isBufImgVal); free(isInt32Type);
    return out;
}

// vkCreateShaderModule — gated SPIR-V rewrite. emu_on() false, or a module with
// no texel-buffer image, is EXACT passthrough. A rewrite MoltenVK rejects falls
// back to the original module (buggy but never a hard failure).
// ---------------------------------------------------------------------------
// EYE CUBEMAP FIX (KL_VK_EYE_ENVGUARD, default ON) — Half-Life: Alyx's NPC eyes
// flashed solid red / green / blue / magenta on the headset (Olga at the alley
// gate, Chapter 1: 43 of 400 frames on the Mac).
//
// Every material shader in this Vulkan build unpacks the per-object value its
// vertex shader passes on (flat, location 7) as two fields: cubemap record =
// v >> 8 (tint / box / normalization, a 144-entry array in the scene's light
// constant buffer) and cube-array layer = v & 255. The vr_eyeball pixel shader
// alone is stale: it indexes the record array with the raw v (always >= 256
// for an eye) and samples layer 0. So it reads ~97 KB into a uniform buffer
// bound for 36-64 KB. PC GPUs bounds-check constant-buffer reads and return
// zeros there (D3D guarantees it; NVIDIA, AMD and Adreno do it in hardware),
// so on PC the eyes merely lose their reflection. Metal does not bounds-check
// constant buffers: the read lands in whatever the ring buffer holds next —
// random tints, identity-matrix rows, huge values — hence the flashes.
//
// This rewrites that one shader to decode v the way the rest of the build
// does: record = v >> 8 for its nine table reads, layer = float(v & 255) for
// its two cube-array samples; and, as a safety net, its cubemap-reflection
// term is zeroed (the PC result) if a decoded record were still >= 144.
// The module is recognised by size + SHA-1 (the combo Chapter 1 uses) and
// patched only if every id the rewrite relies on has the expected shape;
// anything else is left untouched and logged.
#define KLVK_EYE_PS_WORDS  (46924u / 4u)
static const char *KLVK_EYE_PS_SHA1 = "47da31f1c09e08ca18b8f8bb6c18a9a8bae7d741";
static uint32_t *klvk_eye_envguard(const uint32_t *w, size_t nw, size_t *out_nw) {
    enum { IDX = 3142, LOAD = 11487, TERM = 11175, COORD_A = 14604, COORD_B = 14605, ZERO_F = 2572,
           T_INT = 12, T_UINT = 11, T_FLOAT = 13, T_VEC3 = 24, T_VEC4 = 29,
           CUBES = 3424, T_CUBES = 216, INT0 = 2571, INT1 = 2574, GLSL450 = 1 };
    if (nw < 5 || w[0] != 0x07230203u) return NULL;
    uint32_t tbool = 0; size_t firstfn = 0;
    int loads = 0, chains = 0, coords = 0, fadds = 0, ok_uint = 0, ok_vec3 = 0, other_uses = 0;
    int ok_int = 0, ok_cubes = 0, ok_consts = 0, ok_glsl = 0, have_v3int = 0, cube_var = 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        if (!wc || i + wc > nw) return NULL;
        if (op == 20) tbool = w[i + 1];                                                   // OpTypeBool
        if (op == 21 && w[i + 1] == T_UINT && w[i + 2] == 32 && w[i + 3] == 0) ok_uint = 1;
        if (op == 23 && w[i + 1] == T_VEC3 && w[i + 2] == T_FLOAT && w[i + 3] == 3) ok_vec3 = 1;
        if (op == 23 && w[i + 2] == T_INT && w[i + 3] == 3) have_v3int = 1;                // (then we'd reuse it; not expected)
        if (op == 21 && w[i + 1] == T_INT && w[i + 2] == 32 && w[i + 3] == 1) ok_int = 1;
        if (op == 25 && w[i + 1] == T_CUBES && w[i + 3] == 3 && w[i + 5] == 1) ok_cubes = 1;   // Cube, arrayed
        if (op == 59 && w[i + 2] == CUBES) cube_var = 1;
        if (op == 43 && w[i + 1] == T_INT && ((w[i + 2] == INT0 && w[i + 3] == 0) || (w[i + 2] == INT1 && w[i + 3] == 1))) ok_consts++;
        if (op == 11 && w[i + 1] == GLSL450) ok_glsl = 1;
        if (op == 54 && !firstfn) firstfn = i;                                            // first OpFunction
        if (op == 61 && wc == 4 && w[i + 3] == IDX) { loads++; if (w[i + 1] != T_UINT || w[i + 2] != LOAD) return NULL; }
        if (op == 65) { for (uint32_t k = 4; k < wc; k++) if (w[i + k] == LOAD) chains++; }   // OpAccessChain index
        else if (op != 61) { for (uint32_t k = 1; k < wc; k++) if (w[i + k] == LOAD) other_uses++; }
        if (op == 80 && wc == 7 && w[i + 1] == T_VEC4 && (w[i + 2] == COORD_A || w[i + 2] == COORD_B) && w[i + 6] == ZERO_F) coords++;
        if (op == 129 && wc == 5 && w[i + 1] == T_VEC3 && (w[i + 3] == TERM || w[i + 4] == TERM)) fadds++;
        i += wc;
    }
    if (!tbool || !firstfn || !ok_uint || !ok_vec3 || loads != 1 || chains != 9 || other_uses || coords != 2 || fadds != 1 ||
        !ok_int || !ok_cubes || !cube_var || ok_consts != 2 || !ok_glsl || have_v3int)
        return NULL;
    uint32_t bound = w[3];
    const uint32_t c8 = bound++, c255 = bound++, c144 = bound++, zero3 = bound++;
    const uint32_t rec = bound++, lay_u = bound++, lay = bound++;
    const uint32_t t_v3int = bound++, img = bound++, sz = bound++, cnt = bound++, cntm1 = bound++, cntm1u = bound++, lay_c = bound++;
    uint32_t *o = malloc((nw + 64) * 4);
    if (!o) return NULL;
    size_t n = firstfn;
    memcpy(o, w, firstfn * 4);
    o[n++] = (4u << 16) | 43; o[n++] = T_UINT; o[n++] = c8;   o[n++] = 8;               // OpConstant %uint 8
    o[n++] = (4u << 16) | 43; o[n++] = T_UINT; o[n++] = c255; o[n++] = 255;             // OpConstant %uint 255
    o[n++] = (4u << 16) | 43; o[n++] = T_UINT; o[n++] = c144; o[n++] = 144;             // OpConstant %uint 144
    o[n++] = (3u << 16) | 46; o[n++] = T_VEC3; o[n++] = zero3;                          // OpConstantNull %v3float
    o[n++] = (4u << 16) | 23; o[n++] = t_v3int; o[n++] = T_INT; o[n++] = 3;             // OpTypeVector %int 3
    for (size_t i = firstfn; i < nw; ) {
        uint32_t op = w[i] & 0xffffu, wc = w[i] >> 16;
        size_t at = n;
        if (op == 129 && wc == 5 && w[i + 1] == T_VEC3 && (w[i + 3] == TERM || w[i + 4] == TERM)) {
            uint32_t cond = bound++, sel = bound++;
            o[n++] = (5u << 16) | 176; o[n++] = tbool;  o[n++] = cond; o[n++] = rec; o[n++] = c144;          // OpULessThan
            o[n++] = (6u << 16) | 169; o[n++] = T_VEC3; o[n++] = sel; o[n++] = cond; o[n++] = TERM; o[n++] = zero3;  // OpSelect
            at = n;
            memcpy(o + n, w + i, wc * 4); n += wc;
            for (size_t k = at + 3; k < n; k++) if (o[k] == TERM) o[k] = sel;
        } else {
            memcpy(o + n, w + i, wc * 4); n += wc;
            if (op == 65) for (size_t k = at + 4; k < n; k++) if (o[k] == LOAD) o[k] = rec;     // table reads: record
            if (op == 80 && wc == 7 && (o[at + 2] == COORD_A || o[at + 2] == COORD_B)) o[at + 6] = lay;  // layer
        }
        if (op == 61 && wc == 4 && w[i + 3] == IDX) {                                   // right after the one load of v
            o[n++] = (5u << 16) | 194; o[n++] = T_UINT;  o[n++] = rec;   o[n++] = LOAD; o[n++] = c8;     // v >> 8
            o[n++] = (5u << 16) | 199; o[n++] = T_UINT;  o[n++] = lay_u; o[n++] = LOAD; o[n++] = c255;   // v & 255
            // clamped to the cube array's real layer count (Metal does not clamp an array index either)
            o[n++] = (4u << 16) | 61;  o[n++] = T_CUBES; o[n++] = img;  o[n++] = CUBES;                 // the cube array
            o[n++] = (5u << 16) | 103; o[n++] = t_v3int; o[n++] = sz;   o[n++] = img; o[n++] = INT0;     // OpImageQuerySizeLod
            o[n++] = (5u << 16) | 81;  o[n++] = T_INT;   o[n++] = cnt;  o[n++] = sz;  o[n++] = 2;        // .z = layers
            o[n++] = (5u << 16) | 130; o[n++] = T_INT;   o[n++] = cntm1; o[n++] = cnt; o[n++] = INT1;    // layers - 1
            o[n++] = (4u << 16) | 124; o[n++] = T_UINT;  o[n++] = cntm1u; o[n++] = cntm1;                // as uint
            o[n++] = (7u << 16) | 12;  o[n++] = T_UINT;  o[n++] = lay_c; o[n++] = GLSL450; o[n++] = 38;  // UMin
            o[n++] = lay_u; o[n++] = cntm1u;
            o[n++] = (4u << 16) | 112; o[n++] = T_FLOAT; o[n++] = lay;   o[n++] = lay_c;                 // float(...)
        }
        i += wc;
    }
    o[3] = bound;
    *out_nw = n;
    return o;
}

static VkResult VKAPI_CALL klvk_CreateShaderModule_body(VkDevice dev,
        const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkShaderModule *out) {
    static PFN_vkCreateShaderModule real;
    if (!real && real_gdpa) real = (PFN_vkCreateShaderModule)real_gdpa(dev, "vkCreateShaderModule");
    // KL_VK_SPV_LOG=<file>: the SHA-1 and size of every SPIR-V module the guest creates, to match them
    // against a pipeline recording (the game's Fossilize database).
    {
        static FILE *spvlog; static int spvinit;
        if (!spvinit) { spvinit = 1; const char *e = getenv("KL_VK_SPV_LOG"); if (e && *e) spvlog = fopen(e, "a"); }
        if (spvlog && ci && ci->pCode) {
            unsigned char md[CC_SHA1_DIGEST_LENGTH];
            CC_SHA1(ci->pCode, (CC_LONG)ci->codeSize, md);
            char hex[2 * CC_SHA1_DIGEST_LENGTH + 1];
            for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
            fprintf(spvlog, "%s %zu\n", hex, ci->codeSize);
            fflush(spvlog);
            const char *dir = getenv("KL_VK_SPV_DUMP");     // ...and the modules themselves, as <sha1>.spv
            if (dir && *dir) {
                char path[1024]; snprintf(path, sizeof path, "%s/%s.spv", dir, hex);
                FILE *o = fopen(path, "wb"); if (o) { fwrite(ci->pCode, 1, ci->codeSize, o); fclose(o); }
            }
        }
    }
    // KL_VK_EYE_ENVGUARD (default ON): the eye cubemap fix, see klvk_eye_envguard above
    static int eyefix = -1; if (eyefix < 0) eyefix = kl_env_on("KL_VK_EYE_ENVGUARD", 1);
    if (eyefix && ci && ci->pCode && real && ci->codeSize == KLVK_EYE_PS_WORDS * 4u) {
        unsigned char md[CC_SHA1_DIGEST_LENGTH]; char hex[41];
        CC_SHA1(ci->pCode, (CC_LONG)ci->codeSize, md);
        for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
        if (strcmp(hex, KLVK_EYE_PS_SHA1) == 0) {
            size_t nw = 0;
            uint32_t *g = klvk_eye_envguard(ci->pCode, ci->codeSize / 4, &nw);
            if (g) {
                VkShaderModuleCreateInfo m = *ci; m.pCode = g; m.codeSize = nw * 4;
                VkResult r = real(dev, &m, alloc, out);
                free(g);
                static int said; if (said < 4) { said++;
                    fprintf(stderr, "  [vk] eye cubemap fix on the vr_eyeball pixel shader: %s\n",
                            r == VK_SUCCESS ? "applied" : "rejected by MoltenVK; original used"); }
                if (r == VK_SUCCESS) return r;
            } else {
                static int said; if (said < 4) { said++;
                    fprintf(stderr, "  [vk] eye cubemap fix: vr_eyeball pixel shader did not have the expected shape; left alone\n"); }
            }
        }
    }
    // KL_VK_SPV_REPLACE=<sha1>=<file.spv> (diagnostic): swap one guest shader module for a patched copy
    {
        static const char *rep = (const char *)1;
        if (rep == (const char *)1) rep = getenv("KL_VK_SPV_REPLACE");
        if (rep && ci && ci->pCode && real && strlen(rep) > 41 && rep[40] == '=') {
            unsigned char md[CC_SHA1_DIGEST_LENGTH]; char hex[41];
            CC_SHA1(ci->pCode, (CC_LONG)ci->codeSize, md);
            for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
            // several pairs, comma-separated: <sha1>=<file>,<sha1>=<file>
            char pick[600] = "";
            for (const char *p = rep; p && *p; ) {
                const char *e = strchr(p, ','); size_t len = e ? (size_t)(e - p) : strlen(p);
                if (len > 41 && len < sizeof pick && strncmp(hex, p, 40) == 0) { memcpy(pick, p, len); pick[len] = 0; break; }
                p = e ? e + 1 : NULL;
            }
            if (pick[0]) {
                FILE *f = fopen(pick + 41, "rb");
                if (f) {
                    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
                    uint32_t *code = malloc((size_t)n);
                    if (code && fread(code, 1, (size_t)n, f) == (size_t)n) {
                        VkShaderModuleCreateInfo m = *ci; m.pCode = code; m.codeSize = (size_t)n;
                        VkResult r = real(dev, &m, alloc, out);
                        fprintf(stderr, "[spvrep] module %s replaced by %s: result %d\n", hex, pick + 41, (int)r);
                        free(code); fclose(f);
                        return r;
                    }
                    free(code); fclose(f);
                }
            }
        }
    }
    if (!emu_on() || !ci || !ci->pCode || (ci->codeSize & 3u) || !real)
        return real ? real(dev, ci, alloc, out) : VK_ERROR_INITIALIZATION_FAILED;
    size_t nwords = ci->codeSize / 4;
    size_t outw = 0;
    uint32_t *nc = klvk_spirv_mirror_rewrite(ci->pCode, nwords, &outw);
    if (!nc) return real(dev, ci, alloc, out);   // nothing to change
    VkShaderModuleCreateInfo m = *ci;
    m.pCode = nc;
    m.codeSize = outw * 4;
    VkResult r = real(dev, &m, alloc, out);
    free(nc);
    if (r != VK_SUCCESS) {
        static int said; if (said < 8) { said++;
            VKI("[emu] SPIR-V texel->2D rewrite rejected (result %d); using the "
                "original module (that shader keeps the MoltenVK texel-buffer bug)\n",
                (int)r); }
        r = real(dev, ci, alloc, out);
    } else {
        static int said; if (said < 8) { said++;
            VKI("[emu] SPIR-V texel->2D rewrite applied (%zu -> %zu words)\n",
                nwords, outw); }
    }
    return r;
}

// vkCreateDescriptorSetLayout — UNIFORM_TEXEL_BUFFER bindings become SAMPLED_IMAGE
// so the layout matches the rewritten shader. Exact passthrough when emu off.
static VkResult VKAPI_CALL klvk_CreateDescriptorSetLayout_body(VkDevice dev,
        const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkDescriptorSetLayout *out) {
    static PFN_vkCreateDescriptorSetLayout real;
    if (!real && real_gdpa) real = (PFN_vkCreateDescriptorSetLayout)real_gdpa(dev, "vkCreateDescriptorSetLayout");
    if (!emu_on() || !ci || ci->bindingCount == 0 || !ci->pBindings || !real)
        return real ? real(dev, ci, alloc, out) : VK_ERROR_INITIALIZATION_FAILED;
    int any = 0;
    for (uint32_t i = 0; i < ci->bindingCount; i++)
        if (ci->pBindings[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) { any = 1; break; }
    // PROBE (olar gray hunt): count STORAGE_TEXEL_BUFFER bindings. The emu rewrites
    // only UNIFORM_TEXEL_BUFFER (layout, shader, descriptor writes); a layout that
    // binds its texel buffers as STORAGE gets a mirror created but never bound
    // through a rewrite, so those reads stay raw -> gray/invisible. If this fires
    // for olar, the fix is to extend the emu to STORAGE texel buffers.
    for (uint32_t i = 0; i < ci->bindingCount; i++)
        if (ci->pBindings[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            static int said;
            if (said < 12) { said++;
                VKI("[emu] PROBE: descriptor-set layout binds a STORAGE_TEXEL_BUFFER "
                    "(binding %u, count %u) — NOT rewritten by the emu; gray/invisible "
                    "suspect\n", ci->pBindings[i].binding, ci->pBindings[i].descriptorCount); }
        }
    if (!any) return real(dev, ci, alloc, out);
    VkDescriptorSetLayoutBinding *nb =
        (VkDescriptorSetLayoutBinding *)malloc(ci->bindingCount * sizeof *nb);
    if (!nb) return real(dev, ci, alloc, out);
    memcpy(nb, ci->pBindings, ci->bindingCount * sizeof *nb);
    for (uint32_t i = 0; i < ci->bindingCount; i++)
        if (nb[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            nb[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            nb[i].pImmutableSamplers = NULL;   // sampled images take no immutable sampler
        }
    VkDescriptorSetLayoutCreateInfo m = *ci;
    m.pBindings = nb;
    VkResult r = real(dev, &m, alloc, out);
    free(nb);
    return r;
}

// vkCreateDescriptorPool — mirror the layout rewrite in pool sizing so a set with
// SAMPLED_IMAGE bindings can be allocated from a pool the guest sized for
// UNIFORM_TEXEL_BUFFER. Exact passthrough when emu off.
static VkResult VKAPI_CALL klvk_CreateDescriptorPool(VkDevice dev,
        const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkDescriptorPool *out) {
    static PFN_vkCreateDescriptorPool real;
    if (!real && real_gdpa) real = (PFN_vkCreateDescriptorPool)real_gdpa(dev, "vkCreateDescriptorPool");
    if (!emu_on() || !ci || ci->poolSizeCount == 0 || !ci->pPoolSizes || !real)
        return real ? real(dev, ci, alloc, out) : VK_ERROR_INITIALIZATION_FAILED;
    int any = 0;
    for (uint32_t i = 0; i < ci->poolSizeCount; i++)
        if (ci->pPoolSizes[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) { any = 1; break; }
    if (!any) return real(dev, ci, alloc, out);
    VkDescriptorPoolSize *ns = (VkDescriptorPoolSize *)malloc(ci->poolSizeCount * sizeof *ns);
    if (!ns) return real(dev, ci, alloc, out);
    memcpy(ns, ci->pPoolSizes, ci->poolSizeCount * sizeof *ns);
    for (uint32_t i = 0; i < ci->poolSizeCount; i++)
        if (ns[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
            ns[i].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;   // duplicate types are legal; MoltenVK sums them
    VkDescriptorPoolCreateInfo m = *ci;
    m.pPoolSizes = ns;
    VkResult r = real(dev, &m, alloc, out);
    free(ns);
    return r;
}

// vkUpdateDescriptorSets — a write to a rewritten (now SAMPLED_IMAGE) binding
// arrives as a UNIFORM_TEXEL_BUFFER write with pTexelBufferView. Substitute a
// SAMPLED_IMAGE write pointing at each view's mirror image, refresh the mirror
// from the live pool bytes, and record (set,binding,elem)->mirror for bind-time
// refresh. Exact passthrough when emu off or when no write targets a texel buffer.
// PROBE (olar gray hunt): classify a COMBINED_IMAGE_SAMPLER / SAMPLED_IMAGE binding
// by the size of the image it points at. The world renders gray with textures that
// DO have data, so the suspect is materials binding UE's 1x1 default (white/gray)
// textures instead of their real albedo. Tally real (>1x1) vs 1x1 across all image
// bindings; a heavy 1x1 skew on the world material sets is the confirmation.
static int klvk_is_astc(VkFormat f);   // defined below
static void klvk_probe_img_binding(VkImageView v) {
    if (!(emu_on() || probe_on())) return;
    static unsigned real_n, tiny_n, null_n, astc_n; static int said;
    VkImage img = v ? klvk_view_image(v) : VK_NULL_HANDLE;
    VkFormat f = 0; uint32_t w = 0, h = 0;
    if (!img || !klvk_img_info_get(img, &f, &w, &h)) { null_n++; }
    else if (klvk_is_astc(f)) { astc_n++; real_n++; }   // an ASTC texture bound to sample
    else if (w <= 1 && h <= 1) tiny_n++;
    else real_n++;
    unsigned tot = real_n + tiny_n + null_n;
    if (tot && (tot & (tot - 1)) == 0 && said < 24) { said++;
        VKI("[probe] image-sampler bindings so far: %u real(>1x1) of which %u ASTC, "
            "%u tiny(1x1 default?), %u unknown-view\n", real_n, astc_n, tiny_n, null_n); }
}

// KL_VK_UBO_DUMP=<dir> (diagnostic, with KL_VK_UBO_TRACE=<binding>): at every guest submit, write the bytes of
// every distinct uniform-buffer range written into that binding this frame to <dir>/f<frame>_<n>.bin (host-visible
// memory the guest has mapped only), with the frame number the GPU-time frame counter uses (KL_GPU_SNAP's).
static struct { VkBuffer b; VkDeviceSize off, range; } g_ubo_seen[64];
static int g_nubo_seen;
static void klvk_ubodump_note(VkBuffer b, VkDeviceSize off, VkDeviceSize range) {
    if (!ubodump_on()) return;
    pthread_mutex_lock(&g_lock);
    int dup = 0;
    for (int i = 0; i < g_nubo_seen; i++) if (g_ubo_seen[i].b == b && g_ubo_seen[i].off == off) dup = 1;
    if (!dup && g_nubo_seen < 64) { g_ubo_seen[g_nubo_seen].b = b; g_ubo_seen[g_nubo_seen].off = off; g_ubo_seen[g_nubo_seen].range = range; g_nubo_seen++; }
    pthread_mutex_unlock(&g_lock);
}
static void klvk_ubodump_frame(void) {
    if (!ubodump_on()) return;
    extern unsigned kl_gpu_time_frame(void);
    static unsigned last = ~0u; static int from = -1, to;
    if (from < 0) { const char *w = getenv("KL_VK_UBO_DUMP_FRAMES"); from = 0; to = 1 << 30;
                    if (w) sscanf(w, "%d:%d", &from, &to); }
    unsigned f = kl_gpu_time_frame();
    if (f == last) return;                       // once per frame: at the first submit of the next frame
    unsigned fr = last; last = f;
    pthread_mutex_lock(&g_lock);
    if ((int)fr >= from && (int)fr <= to && fr != ~0u)
        for (int i = 0; i < g_nubo_seen; i++) {
            struct probe_buf *e = probe_buf_find(g_ubo_seen[i].b);
            struct probe_mem *m = e ? probe_mem_find(e->mem) : NULL;
            if (!m || !m->mapped) continue;
            const char *src = (const char *)m->mapped + (e->memoff - m->map_off) + g_ubo_seen[i].off;
            char path[600]; snprintf(path, sizeof path, "%s/f%u_%d.bin", getenv("KL_VK_UBO_DUMP"), fr, i);
            FILE *fp = fopen(path, "wb");
            if (fp) { fwrite(src, 1, (size_t)(g_ubo_seen[i].range > 65536 ? 65536 : g_ubo_seen[i].range), fp); fclose(fp); }
        }
    g_nubo_seen = 0;
    pthread_mutex_unlock(&g_lock);
}

// KL_VK_SAMPLER_TRACE=<binding> (diagnostic): which samplers the guest writes into that descriptor binding, and
// with what filtering — logged as a histogram (sampler filters, write count). Samplers are remembered at creation,
// descriptor-update templates by their entries for that binding; both update paths are watched.
#define KLST_MAX 4096
static struct { VkSampler s; uint8_t mag, min, mip, aniso, cmp, au; float maxAniso; unsigned writes; } g_st[KLST_MAX];
static int g_nst;
static struct { VkDescriptorUpdateTemplate t; size_t off, stride; uint32_t count, arr; } g_stt[1024];
static VkDescriptorSetLayout g_st_layout;
static struct { VkDescriptorUpdateTemplate t; size_t off; } g_stu[1024]; static int g_nstu;   // KL_VK_UBO_TRACE=<binding>   // KL_VK_SAMPLER_TRACE_NB=<n>: only the layout with n bindings incl. 289
static int g_nstt;
static int klst_binding(void) {
    static int b = -2;
    if (b == -2) { const char *e = getenv("KL_VK_SAMPLER_TRACE"); b = e && *e ? atoi(e) : -1; }
    return b;
}
static void klst_note_sampler(VkSampler s, const VkSamplerCreateInfo *ci) {
    if (klst_binding() < 0 || !ci) return;
    pthread_mutex_lock(&g_lock);
    if (g_nst < KLST_MAX) {
        g_st[g_nst].s = s; g_st[g_nst].mag = (uint8_t)ci->magFilter; g_st[g_nst].min = (uint8_t)ci->minFilter;
        g_st[g_nst].mip = (uint8_t)ci->mipmapMode; g_st[g_nst].aniso = (uint8_t)ci->anisotropyEnable;
        g_st[g_nst].maxAniso = ci->maxAnisotropy; g_st[g_nst].cmp = (uint8_t)ci->compareEnable;
        g_st[g_nst].au = (uint8_t)ci->addressModeU; g_st[g_nst].writes = 0; g_nst++;
    }
    pthread_mutex_unlock(&g_lock);
}
static void klst_note_write(VkSampler s) {
    static unsigned total;
    pthread_mutex_lock(&g_lock);
    int k = -1;
    for (int i = 0; i < g_nst; i++) if (g_st[i].s == s) { k = i; break; }
    if (k >= 0) g_st[k].writes++;
    total++;
    if ((total & (total - 1)) == 0 || total % 20000 == 0) {       // at 1, 2, 4, ... and every 20000 writes
        fprintf(stderr, "[samptrace] binding %d: %u sampler writes so far\n", klst_binding(), total);
        for (int i = 0; i < g_nst; i++) if (g_st[i].writes)
            fprintf(stderr, "[samptrace]   sampler %#llx mag %u min %u mip %u aniso %u (%.0f) compare %u addrU %u: %u writes\n",
                    (unsigned long long)(uintptr_t)g_st[i].s, g_st[i].mag, g_st[i].min, g_st[i].mip, g_st[i].aniso,
                    g_st[i].maxAniso, g_st[i].cmp, g_st[i].au, g_st[i].writes);
        if (k < 0) fprintf(stderr, "[samptrace]   (a sampler created before tracing / unknown: %#llx)\n", (unsigned long long)(uintptr_t)s);
    }
    pthread_mutex_unlock(&g_lock);
}

static void VKAPI_CALL klvk_UpdateDescriptorSets(VkDevice dev,
        uint32_t writeCount, const VkWriteDescriptorSet *writes,
        uint32_t copyCount, const VkCopyDescriptorSet *copies) {
    if (klst_binding() >= 0 && copies && copyCount) {
        static unsigned nc, ncb; nc += copyCount;
        for (uint32_t i = 0; i < copyCount; i++) {
            int k = klst_binding() - (int)copies[i].dstBinding;
            if (k >= 0 && k < (int)copies[i].descriptorCount) ncb++;
        }
        static int said; if (said < 6 || (nc & (nc - 1)) == 0) { said++;
            fprintf(stderr, "[samptrace] descriptor copies: %u (%u touching binding %d); e.g. src binding %u -> dst %u count %u\n",
                    nc, ncb, klst_binding(), copies[0].srcBinding, copies[0].dstBinding, copies[0].descriptorCount); }
    }
    if (klst_binding() >= 0 && writes)
        for (uint32_t i = 0; i < writeCount; i++)
            if (writes[i].pImageInfo && (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                 writes[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
                // a write may run on into the following (single-element) bindings
                int b0 = (int)writes[i].dstBinding, k = klst_binding() - b0;
                static int said; if (said < 16) { said++; fprintf(stderr, "[samptrace] write: binding %d count %u type %d\n",
                    b0, writes[i].descriptorCount, (int)writes[i].descriptorType); }
                if (k >= 0 && k < (int)writes[i].descriptorCount) klst_note_write(writes[i].pImageInfo[k].sampler);
            }
    // PROBE: tally image-sampler bindings (see klvk_probe_img_binding).
    if ((emu_on() || probe_on()) && writes)
        for (uint32_t i = 0; i < writeCount; i++)
            if ((writes[i].descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                 writes[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) &&
                writes[i].pImageInfo)
                for (uint32_t j = 0; j < writes[i].descriptorCount; j++)
                    klvk_probe_img_binding(writes[i].pImageInfo[j].imageView);
    static PFN_vkUpdateDescriptorSets real;
    if (!real && real_gdpa) real = (PFN_vkUpdateDescriptorSets)real_gdpa(dev, "vkUpdateDescriptorSets");
    if (!real) return;
    if (!emu_on() || !writes || writeCount == 0) { real(dev, writeCount, writes, copyCount, copies); return; }
    uint32_t total = 0; int any = 0;
    for (uint32_t i = 0; i < writeCount; i++)
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            any = 1; total += writes[i].descriptorCount;
        }
    if (!any) { real(dev, writeCount, writes, copyCount, copies); return; }

    VkWriteDescriptorSet *nw = (VkWriteDescriptorSet *)malloc(writeCount * sizeof *nw);
    VkDescriptorImageInfo *imgs = total ? (VkDescriptorImageInfo *)malloc(total * sizeof *imgs) : NULL;
    if (!nw || (total && !imgs)) {   // OOM: safest is the unmodified call
        free(nw); free(imgs);
        real(dev, writeCount, writes, copyCount, copies);
        return;
    }
    memcpy(nw, writes, writeCount * sizeof *nw);

    uint32_t ii = 0; unsigned nsubst = 0;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < writeCount; i++) {
        if (writes[i].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
            !writes[i].pTexelBufferView)
            continue;
        VkDescriptorImageInfo *slot = &imgs[ii];
        uint64_t set = (uint64_t)(uintptr_t)writes[i].dstSet;
        for (uint32_t j = 0; j < writes[i].descriptorCount; j++) {
            int mi = klvk_bvmap_find(writes[i].pTexelBufferView[j]);
            slot[j].sampler = VK_NULL_HANDLE;
            slot[j].imageView = (mi >= 0) ? g_mirrors[mi].view : VK_NULL_HANDLE;
            slot[j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (mi >= 0) {
                nsubst++;
                // No copy here (no command buffer): just remember set->mirror so the
                // GPU copy is recorded at the following vkCmdBindDescriptorSets.
                klvk_dset_put(set, writes[i].dstBinding, writes[i].dstArrayElement + j, mi);
            }
        }
        nw[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        nw[i].pImageInfo = slot;
        nw[i].pBufferInfo = NULL;
        nw[i].pTexelBufferView = NULL;
        ii += writes[i].descriptorCount;
    }
    pthread_mutex_unlock(&g_lock);
    {
        static unsigned cum, calls; static int said;
        cum += nsubst; calls++;
        if (nsubst && said < 12) { said++;
            VKI("[emu] update-set: %u texel writes -> mirror image (cum %u over %u "
                "vkUpdateDescriptorSets calls)\n", nsubst, cum, calls); }
    }
    real(dev, writeCount, nw, copyCount, copies);
    free(nw); free(imgs);
}

// ---------------------------------------------------------------------------
// Descriptor UPDATE TEMPLATES and PUSH DESCRIPTORS — the bind paths UE5's Vulkan
// RHI actually uses (olar enables VK_KHR_descriptor_update_template AND
// VK_KHR_push_descriptor). Plain vkUpdateDescriptorSets is NOT how olar binds
// its texel buffers, so the SAMPLED_IMAGE substitution has to happen here too or
// the mirror images are created but never bound (scene stays gray). Same rule as
// everywhere else: exact passthrough when emu_on() is false, and only
// UNIFORM_TEXEL_BUFFER is rewritten (matching the layout/shader/pool rewrites).
// ---------------------------------------------------------------------------

// Byte size of one descriptor element in template pData, by type. 0 == a type we
// do not know how to repack (then a template carrying a texel entry alongside it
// is left un-rewritten rather than corrupted — logged). INLINE_UNIFORM_BLOCK is
// handled specially (its descriptorCount is a byte count, not an element count).
static uint32_t klvk_descr_elem_size(VkDescriptorType t) {
    switch (t) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            return (uint32_t)sizeof(VkDescriptorImageInfo);
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return (uint32_t)sizeof(VkBufferView);
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            return (uint32_t)sizeof(VkDescriptorBufferInfo);
        default:
            return 0;
    }
}

// One recorded template: the guest's ORIGINAL entry layout plus the REBUILT
// (tightly-packed, texel->image-expanded) layout the real rewritten template
// reads. vkUpdateDescriptorSetWithTemplate / vkCmdPushDescriptorSetWithTemplateKHR
// transform the guest's pData from the original into the rebuilt layout.
struct emu_tmpl_entry {
    VkDescriptorType type;            // original descriptor type
    uint32_t dstBinding, dstArrayElement, descriptorCount;
    size_t   origOffset, origStride;  // where the guest wrote it
    size_t   newOffset,  newStride;   // where the rewritten template reads it
    uint32_t elemSize;                // bytes copied per element into the new layout
    int      isTexel;                 // UNIFORM_TEXEL_BUFFER -> SAMPLED_IMAGE
    int      isInline;                // INLINE_UNIFORM_BLOCK: copy descriptorCount raw bytes
};
#define KLVK_EMU_TMPL_MAX     512
#define KLVK_EMU_TMPL_ENTRIES 64
struct emu_tmpl {
    VkDescriptorUpdateTemplate handle;
    int      nentry;
    size_t   dataSize;                // size of the rebuilt pData buffer
    struct emu_tmpl_entry entries[KLVK_EMU_TMPL_ENTRIES];
};
static struct emu_tmpl g_tmpls[KLVK_EMU_TMPL_MAX];
static int g_ntmpl;

// Runs under g_lock.
static struct emu_tmpl *klvk_tmpl_find(VkDescriptorUpdateTemplate h) {
    if (!h) return NULL;
    for (int i = 0; i < g_ntmpl; i++)
        if (g_tmpls[i].handle == h) return &g_tmpls[i];
    return NULL;
}

// Transform the guest's template pData into the rebuilt layout, substituting each
// texel VkBufferView for its mirror image's VkDescriptorImageInfo and refreshing
// the mirror bytes (a template update / push is a bind point). `setKey` is the
// descriptor-set handle for a set update (0 for push, where there is no persistent
// set to record for bind-time refresh — the refresh here already covers it).
// Runs under g_lock. Returns a malloc'd buffer the caller frees, or NULL on OOM.
static void *klvk_tmpl_transform(struct emu_tmpl *t, const void *pData, uint64_t setKey,
                                 unsigned *nsubst, int *mirrors, int *nmir, int maxmir) {
    uint8_t *nd = (uint8_t *)malloc(t->dataSize ? t->dataSize : 1);
    if (!nd) return NULL;
    unsigned subst = 0;
    const uint8_t *base = (const uint8_t *)pData;
    for (int e = 0; e < t->nentry; e++) {
        struct emu_tmpl_entry *en = &t->entries[e];
        const uint8_t *src = base + en->origOffset;
        uint8_t *dst = nd + en->newOffset;
        if (en->isInline) {                       // raw byte block, count == bytes
            memcpy(dst, src, en->descriptorCount);
            continue;
        }
        for (uint32_t j = 0; j < en->descriptorCount; j++) {
            const uint8_t *sj = src + (size_t)j * en->origStride;
            uint8_t *dj = dst + (size_t)j * en->newStride;
            if (en->isTexel) {
                VkBufferView bv;
                memcpy(&bv, sj, sizeof bv);
                int mi = klvk_bvmap_find(bv);
                VkDescriptorImageInfo ii;
                ii.sampler     = VK_NULL_HANDLE;
                ii.imageView   = (mi >= 0) ? g_mirrors[mi].view : VK_NULL_HANDLE;
                ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                memcpy(dj, &ii, sizeof ii);
                if (mi >= 0) {
                    subst++;
                    // Set-path: remember (set,binding,elem)->mirror so the copy is
                    // recorded at the following vkCmdBindDescriptorSets. Push-path:
                    // hand the mirror indices back so the caller records the copy on
                    // its command buffer right here (no persistent set to bind).
                    if (setKey)
                        klvk_dset_put(setKey, en->dstBinding, en->dstArrayElement + j, mi);
                    else if (mirrors && nmir && *nmir < maxmir)
                        mirrors[(*nmir)++] = mi;
                }
            } else {
                memcpy(dj, sj, en->elemSize);
                // PROBE (olar gray hunt): if this entry is an image sampler, tally
                // whether it binds a real texture or a 1x1 default (see
                // klvk_probe_img_binding). Templates are a bind path olar uses.
                if (en->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                    en->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                    VkDescriptorImageInfo di;
                    memcpy(&di, sj, sizeof di);
                    klvk_probe_img_binding(di.imageView);
                }
            }
        }
    }
    if (nsubst) *nsubst = subst;
    return nd;
}

// vkCreateDescriptorUpdateTemplate(+KHR) — rewrite UNIFORM_TEXEL_BUFFER entries
// to SAMPLED_IMAGE, rebuild a tightly-packed data layout (image info is 24 bytes
// vs a bufferview's 8), create the REAL template from the rewritten entries, and
// record the original<->new layout for the update path. Exact passthrough when
// emu off, when no entry is a texel buffer, or when an entry type we cannot
// repack shares the template with a texel entry (logged; that template stays on
// the broken path rather than corrupting pData).
static VkResult VKAPI_CALL klvk_CreateDescriptorUpdateTemplate(VkDevice dev,
        const VkDescriptorUpdateTemplateCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkDescriptorUpdateTemplate *out) {
    static PFN_vkCreateDescriptorUpdateTemplate real;
    if (!real && real_gdpa) {
        real = (PFN_vkCreateDescriptorUpdateTemplate)real_gdpa(dev, "vkCreateDescriptorUpdateTemplate");
        if (!real) real = (PFN_vkCreateDescriptorUpdateTemplate)real_gdpa(dev, "vkCreateDescriptorUpdateTemplateKHR");
    }
    if (klst_binding() >= 0 && ci && ci->pDescriptorUpdateEntries && real) {
        VkResult r0 = real(dev, ci, alloc, out);
        if (r0 == VK_SUCCESS && out && *out && (!kl_env_int("KL_VK_SAMPLER_TRACE_NB", 0) || ci->descriptorSetLayout == g_st_layout))
            for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; i++) {
                const VkDescriptorUpdateTemplateEntry *e = &ci->pDescriptorUpdateEntries[i];
                {   int ub = kl_env_int("KL_VK_UBO_TRACE", -1), ku = ub - (int)e->dstBinding;
                    if (ub >= 0 && ku >= 0 && ku < (int)e->descriptorCount && e->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                        pthread_mutex_lock(&g_lock);
                        if (g_nstu < 1024) { g_stu[g_nstu].t = *out; g_stu[g_nstu].off = e->offset + (size_t)ku * e->stride; g_nstu++; }
                        pthread_mutex_unlock(&g_lock);
                    }
                }
                int k = klst_binding() - (int)e->dstBinding;
                if (e->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || e->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                    static int said; if (said < 16) { said++; fprintf(stderr, "[samptrace] template entry: binding %u count %u off %zu stride %zu\n",
                        e->dstBinding, e->descriptorCount, e->offset, e->stride); }
                }
                if (k >= 0 && k < (int)e->descriptorCount && (e->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                     e->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)) {
                    pthread_mutex_lock(&g_lock);
                    if (g_nstt < 1024) { g_stt[g_nstt].t = *out; g_stt[g_nstt].off = e->offset + (size_t)k * e->stride; g_stt[g_nstt].stride = e->stride;
                                         g_stt[g_nstt].count = e->descriptorCount; g_stt[g_nstt].arr = e->dstArrayElement; g_nstt++; }
                    pthread_mutex_unlock(&g_lock);
                }
            }
        return r0;                                   // diagnostic run: the texel-buffer emu is not used for Alyx
    }
    if (!emu_on() || !ci || !ci->pDescriptorUpdateEntries ||
        ci->descriptorUpdateEntryCount == 0 || !real)
        return real ? real(dev, ci, alloc, out) : VK_ERROR_INITIALIZATION_FAILED;

    uint32_t n = ci->descriptorUpdateEntryCount;
    int anyTexel = 0;
    for (uint32_t i = 0; i < n; i++)
        if (ci->pDescriptorUpdateEntries[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) { anyTexel = 1; break; }
    if (!anyTexel || n > KLVK_EMU_TMPL_ENTRIES)
        return real(dev, ci, alloc, out);

    VkDescriptorUpdateTemplateEntry *ne =
        (VkDescriptorUpdateTemplateEntry *)malloc(n * sizeof *ne);
    if (!ne) return real(dev, ci, alloc, out);

    // Build the rewritten entries + rebuilt layout; bail (passthrough) if any
    // non-texel entry uses a type we cannot size.
    struct emu_tmpl rec;
    memset(&rec, 0, sizeof rec);
    size_t running = 0;
    int cannot = 0;
    for (uint32_t i = 0; i < n; i++) {
        const VkDescriptorUpdateTemplateEntry *oe = &ci->pDescriptorUpdateEntries[i];
        struct emu_tmpl_entry *re = &rec.entries[i];
        running = (running + 7u) & ~(size_t)7u;   // keep each element 8-byte aligned
                                                  // (VkDescriptor*Info hold pointers),
                                                  // even after a raw inline block
        re->type = oe->descriptorType;
        re->dstBinding = oe->dstBinding;
        re->dstArrayElement = oe->dstArrayElement;
        re->descriptorCount = oe->descriptorCount;
        re->origOffset = oe->offset;
        re->origStride = oe->stride;
        re->isTexel = (oe->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
        re->isInline = (oe->descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK);

        ne[i] = *oe;
        if (re->isInline) {
            re->elemSize = 0; re->newStride = 0;
            re->newOffset = running;
            ne[i].offset = running;
            running += oe->descriptorCount;               // count is a byte count
            continue;
        }
        uint32_t esz = re->isTexel ? (uint32_t)sizeof(VkDescriptorImageInfo)
                                   : klvk_descr_elem_size(oe->descriptorType);
        if (esz == 0) { cannot = 1; break; }
        re->elemSize  = esz;
        re->newStride = esz;
        re->newOffset = running;
        if (re->isTexel) ne[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        ne[i].offset = running;
        ne[i].stride = esz;
        running += (size_t)oe->descriptorCount * esz;
    }
    if (cannot) {
        static int said; if (said < 8) { said++;
            VKI("[emu] descriptor update template has a texel entry beside an "
                "unrepackable type — left on passthrough (that template's texel "
                "buffers keep the MoltenVK bug)\n"); }
        free(ne);
        return real(dev, ci, alloc, out);
    }
    rec.nentry = (int)n;
    rec.dataSize = running;

    VkDescriptorUpdateTemplateCreateInfo m = *ci;
    m.pDescriptorUpdateEntries = ne;
    VkResult r = real(dev, &m, alloc, out);
    free(ne);
    if (r != VK_SUCCESS || !out || !*out) return r;

    pthread_mutex_lock(&g_lock);
    rec.handle = *out;
    struct emu_tmpl *slot = klvk_tmpl_find(*out);   // overwrite a reused handle
    if (!slot && g_ntmpl < KLVK_EMU_TMPL_MAX) slot = &g_tmpls[g_ntmpl++];
    if (slot) *slot = rec;
    else {
        static int said; if (said < 8) { said++;
            VKI("[emu] descriptor-update-template table full (%d) — this template's "
                "texel buffers stay unbound (gray)\n", KLVK_EMU_TMPL_MAX); }
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

// vkUpdateDescriptorSetWithTemplate(+KHR) — transform the guest pData into the
// rewritten template's layout (texel bufferviews -> mirror image infos) and call
// through. Exact passthrough when emu off or the template was not rewritten.
static void VKAPI_CALL klvk_UpdateDescriptorSetWithTemplate(VkDevice dev,
        VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl, const void *pData) {
    static PFN_vkUpdateDescriptorSetWithTemplate real;
    if (!real && real_gdpa) {
        real = (PFN_vkUpdateDescriptorSetWithTemplate)real_gdpa(dev, "vkUpdateDescriptorSetWithTemplate");
        if (!real) real = (PFN_vkUpdateDescriptorSetWithTemplate)real_gdpa(dev, "vkUpdateDescriptorSetWithTemplateKHR");
    }
    if (!real) return;
    if (klst_binding() >= 0 && pData) {
        // KL_VK_SAMPLER_TRACE_FORCE_POINT=1 (diagnostic): the traced binding always gets a nearest/clamp sampler
        static int fp = -1; static VkSampler pt;
        if (fp < 0) {
            fp = kl_env_on("KL_VK_SAMPLER_TRACE_FORCE_POINT", 0);
            if (fp) {
                PFN_vkCreateSampler cs = (PFN_vkCreateSampler)real_gdpa(dev, "vkCreateSampler");
                VkSamplerCreateInfo sc = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                    .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 16.0f };
                if (!cs || cs(dev, &sc, NULL, &pt) != VK_SUCCESS) fp = 0;
                fprintf(stderr, "[samptrace] forcing a point sampler into binding %d: %s\n", klst_binding(), fp ? "on" : "failed");
            }
        }
        for (int i = 0; i < g_nstu; i++) if (g_stu[i].t == tmpl) {
            const VkDescriptorBufferInfo *bi = (const VkDescriptorBufferInfo *)((const char *)pData + g_stu[i].off);
            klvk_ubodump_note(bi->buffer, bi->offset, bi->range);
            static unsigned n, hist[8]; static unsigned long long mn = ~0ull, mx;
            unsigned long long r = bi->range == VK_WHOLE_SIZE ? 1ull << 40 : bi->range;
            if (r < mn) mn = r; if (r > mx) mx = r;
            int bk = r < 4096 ? 0 : r < 16384 ? 1 : r < 32768 ? 2 : r < 36064 ? 3 : r < 65536 ? 4 : 5;
            hist[bk]++; n++;
            if ((n & (n - 1)) == 0 || n % 50000 == 0)
                fprintf(stderr, "[ubotrace] binding %d: %u writes; range min %llu max %llu; <4K %u <16K %u <32K %u <36064 %u <64K %u more %u\n",
                        kl_env_int("KL_VK_UBO_TRACE", -1), n, mn, mx, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5]);
        }
        for (int i = 0; i < g_nstt; i++) if (g_stt[i].t == tmpl) {
            const VkDescriptorImageInfo *ii = (const VkDescriptorImageInfo *)((const char *)pData + g_stt[i].off);
            klst_note_write(ii->sampler);
            if (fp) ((VkDescriptorImageInfo *)ii)->sampler = pt;     // the guest's own staging data, rewritten in place
        }
    }
    void *nd = NULL;
    unsigned nsubst = 0;
    if (emu_on() && pData) {
        pthread_mutex_lock(&g_lock);
        struct emu_tmpl *t = klvk_tmpl_find(tmpl);
        // Set path: transform substitutes image infos and records set->mirror; the
        // GPU copy fires at the following vkCmdBindDescriptorSets (no cb here).
        if (t) nd = klvk_tmpl_transform(t, pData, (uint64_t)(uintptr_t)set, &nsubst,
                                        NULL, NULL, 0);
        pthread_mutex_unlock(&g_lock);
    }
    if (nd) {
        static unsigned cum, calls; static int said;
        cum += nsubst; calls++;
        if (nsubst && said < 12) { said++;
            VKI("[emu] template(set): %u texel writes -> mirror image (cum %u over "
                "%u update-with-template calls)\n", nsubst, cum, calls); }
        real(dev, set, tmpl, nd); free(nd);
    } else {
        real(dev, set, tmpl, pData);
    }
}

// vkCmdPushDescriptorSetKHR — same VkBufferView -> mirror-image substitution as
// klvk_UpdateDescriptorSets, refreshing each mirror (a push is a bind point;
// there is no persistent set handle, so no dset table entry — the refresh here
// covers the draws that follow in this command buffer). Passthrough when emu off
// or no write targets a texel buffer.
static void VKAPI_CALL klvk_CmdPushDescriptorSetKHR(VkCommandBuffer cb,
        VkPipelineBindPoint bindPoint, VkPipelineLayout layout, uint32_t set,
        uint32_t writeCount, const VkWriteDescriptorSet *writes) {
    static PFN_vkCmdPushDescriptorSetKHR real;
    if (!real && real_gdpa) real = (PFN_vkCmdPushDescriptorSetKHR)
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdPushDescriptorSetKHR");
    if (!real) return;
    if (!emu_on() || !writes || writeCount == 0) { real(cb, bindPoint, layout, set, writeCount, writes); return; }
    uint32_t total = 0; int any = 0;
    for (uint32_t i = 0; i < writeCount; i++)
        if (writes[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER) {
            any = 1; total += writes[i].descriptorCount;
        }
    if (!any) { real(cb, bindPoint, layout, set, writeCount, writes); return; }

    VkWriteDescriptorSet *nw = (VkWriteDescriptorSet *)malloc(writeCount * sizeof *nw);
    VkDescriptorImageInfo *imgs = total ? (VkDescriptorImageInfo *)malloc(total * sizeof *imgs) : NULL;
    if (!nw || (total && !imgs)) {
        free(nw); free(imgs);
        real(cb, bindPoint, layout, set, writeCount, writes);
        return;
    }
    memcpy(nw, writes, writeCount * sizeof *nw);
    uint32_t ii = 0; unsigned nsubst = 0;
    int seen[KLVK_BIND_SEEN]; int nseen = 0;
    pthread_mutex_lock(&g_lock);
    for (uint32_t i = 0; i < writeCount; i++) {
        if (writes[i].descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
            !writes[i].pTexelBufferView)
            continue;
        VkDescriptorImageInfo *slot = &imgs[ii];
        for (uint32_t j = 0; j < writes[i].descriptorCount; j++) {
            int mi = klvk_bvmap_find(writes[i].pTexelBufferView[j]);
            slot[j].sampler     = VK_NULL_HANDLE;
            slot[j].imageView   = (mi >= 0) ? g_mirrors[mi].view : VK_NULL_HANDLE;
            slot[j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (mi >= 0) {                            // record the fill on this cb now
                nsubst++;
                int dup = 0; for (int s = 0; s < nseen; s++) if (seen[s] == mi) { dup = 1; break; }
                if (!dup) { if (nseen < KLVK_BIND_SEEN) seen[nseen++] = mi;
                            klvk_mirror_gpu_copy(cb, &g_mirrors[mi]); }
            }
        }
        nw[i].descriptorType   = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        nw[i].pImageInfo       = slot;
        nw[i].pBufferInfo      = NULL;
        nw[i].pTexelBufferView = NULL;
        ii += writes[i].descriptorCount;
    }
    pthread_mutex_unlock(&g_lock);
    {
        static unsigned cum, calls; static int said;
        cum += nsubst; calls++;
        if (nsubst && said < 12) { said++;
            VKI("[emu] push: %u texel writes -> mirror image (cum %u over %u "
                "push calls)\n", nsubst, cum, calls); }
    }
    real(cb, bindPoint, layout, set, writeCount, nw);
    free(nw); free(imgs);
}

// vkCmdPushDescriptorSetWithTemplateKHR — the template transform (1)+(2) applied
// at a command-buffer push. Passthrough when emu off or the template was not
// rewritten.
static void VKAPI_CALL klvk_CmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer cb,
        VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout, uint32_t set,
        const void *pData) {
    static PFN_vkCmdPushDescriptorSetWithTemplateKHR real;
    if (!real && real_gdpa) real = (PFN_vkCmdPushDescriptorSetWithTemplateKHR)
        real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdPushDescriptorSetWithTemplateKHR");
    if (!real) return;
    void *nd = NULL;
    unsigned nsubst = 0;
    if (emu_on() && pData) {
        int mirrors[KLVK_BIND_SEEN]; int nmir = 0;
        pthread_mutex_lock(&g_lock);
        struct emu_tmpl *t = klvk_tmpl_find(tmpl);
        // Push path: no persistent set to bind, so record the GPU copy for every
        // substituted mirror onto THIS command buffer right here (deduped).
        if (t) {
            nd = klvk_tmpl_transform(t, pData, 0, &nsubst, mirrors, &nmir, KLVK_BIND_SEEN);
            if (nd) {
                int seen[KLVK_BIND_SEEN]; int nseen = 0;
                for (int k = 0; k < nmir; k++) {
                    int mi = mirrors[k], dup = 0;
                    for (int s = 0; s < nseen; s++) if (seen[s] == mi) { dup = 1; break; }
                    if (dup) continue;
                    if (nseen < KLVK_BIND_SEEN) seen[nseen++] = mi;
                    if (mi >= 0 && mi < g_nmirror) klvk_mirror_gpu_copy(cb, &g_mirrors[mi]);
                }
            }
        }
        pthread_mutex_unlock(&g_lock);
    }
    if (nd) {
        static unsigned cum, calls; static int said;
        cum += nsubst; calls++;
        if (nsubst && said < 12) { said++;
            VKI("[emu] template(push): %u texel writes -> mirror image (cum %u over "
                "%u push-with-template calls)\n", nsubst, cum, calls); }
        real(cb, tmpl, layout, set, nd); free(nd);
    } else {
        real(cb, tmpl, layout, set, pData);
    }
}

// DIAGNOSTIC ONLY — vkCmdBindDescriptorBuffersEXT. VK_EXT_descriptor_buffer is a
// THIRD descriptor bind path (buffers holding descriptor data at GPU addresses,
// consumed via vkCmdBindDescriptorBuffersEXT + vkCmdSetDescriptorBufferOffsetsEXT
// instead of VkDescriptorSet / templates / push). The mirror does NOT cover it.
// If olar binds this way, none of the substitution paths above fire and the scene
// stays gray no matter what. This wrapper logs the first calls (so a run tells us
// whether the extension is merely RESOLVED or actually CALLED) and forwards to the
// real MoltenVK entry when present — so it is a faithful passthrough that only adds
// a log, and never shadows a working implementation. If this fires for olar, the
// remaining work is a descriptor_buffer-specific substitution (out of scope here).
static void VKAPI_CALL klvk_CmdBindDescriptorBuffersEXT(VkCommandBuffer cb,
        uint32_t bufferCount, const VkDescriptorBufferBindingInfoEXT *pBindingInfos) {
    static PFN_vkCmdBindDescriptorBuffersEXT real;
    static int resolved;
    if (!resolved) { resolved = 1;
        if (real_gdpa) real = (PFN_vkCmdBindDescriptorBuffersEXT)
            real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkCmdBindDescriptorBuffersEXT"); }
    if (emu_on()) {
        static int said; if (said < 8) { said++;
            VKI("[emu] vkCmdBindDescriptorBuffersEXT CALLED (bufferCount %u, real %s) "
                "— olar is using VK_EXT_descriptor_buffer, a THIRD bind path the 2D "
                "mirror does NOT cover. This is the prime suspect if still gray.\n",
                bufferCount, real ? "present" : "ABSENT/no-op"); }
    }
    if (real) real(cb, bufferCount, pBindingInfos);
}

static VkResult VKAPI_CALL klvk_CreateSampler_body(VkDevice dev,
        const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *alloc, VkSampler *out) {
    static PFN_vkCreateSampler real;
    if (!real && real_gdpa) real = (PFN_vkCreateSampler)real_gdpa(dev, "vkCreateSampler");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (klvk_fake_minmax() && ci) {
        int had = 0;
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
            had |= b->sType == VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO;
        if (had) {
            static unsigned n;
            if (n++ < 8) VKI("CreateSampler: reduction mode stripped (KL_VK_FAKE_MINMAX) — #%u\n", n);
            klvk_unlink(ci, VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO);
        }
    }
    return real(dev, ci, alloc, out);
}
static VkResult VKAPI_CALL klvk_CreateBufferView(VkDevice dev,
        const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *alloc,
        VkBufferView *out) {
    static PFN_vkCreateBufferView real;
    if (!real && real_gdpa)
        real = (PFN_vkCreateBufferView)real_gdpa(dev, "vkCreateBufferView");
    // The returned handle is ALWAYS a real MoltenVK view (a valid, destroyable
    // handle the guest may store and later vkDestroyBufferView). For olar/wanderer
    // (emu_on) we additionally build the 2D mirror for this window and remember
    // bv -> mirror, so vkUpdateDescriptorSets can substitute a SAMPLED_IMAGE write
    // pointing at the mirror. The real view is never actually sampled — the shader
    // and layout are rewritten to read the mirror — but creating it keeps handle
    // lifetime exactly as the guest expects. When emu_on() is false — every other
    // title — none of the mirror path runs and this is the EXACT faithful
    // passthrough it always was.
    VkResult r = real ? real(dev, ci, alloc, out)
                      : VK_ERROR_INITIALIZATION_FAILED;
    if (emu_on() && r == VK_SUCCESS && ci && out && *out) {
        pthread_mutex_lock(&g_lock);
        int mi = klvk_mirror_get(dev, ci->buffer, ci->offset, ci->range, ci->format);
        if (mi >= 0) klvk_bvmap_put(*out, mi);
        static int okn, fbn;
        if (mi >= 0) okn++; else fbn++;
        static int said;
        if ((okn + fbn) <= 4 || ((okn + fbn) % 128) == 0) {
            if (said < 40) { said++;
                VKI("[emu] texel-buffer views: %d mirrored to 2D image, "
                    "%d unmirrored (left as passthrough texel buffer)\n", okn, fbn); }
        }
        pthread_mutex_unlock(&g_lock);
    }
    static int dump = -1;
    if (dump < 0) dump = kl_env_on("KL_VK_DUMP_TEXELBUF", 0);
    if (dump && ci) {
        static int logged;
        if (logged < 256) { logged++;
            VKI("vkCreateBufferView: -> %#llx result %d buffer %#llx format %d "
                "offset %llu range %llu (texel buffer; a MoltenVK misread here "
                "reads as zero -> collapsed/INVISIBLE geometry or BLACK params)\n",
                (unsigned long long)(uintptr_t)(out ? *out : 0), (int)r,
                (unsigned long long)(uintptr_t)ci->buffer, (int)ci->format,
                (unsigned long long)ci->offset, (unsigned long long)ci->range);
        }
    }
    if (probe_on() && r == VK_SUCCESS && ci && out) {
        pthread_mutex_lock(&g_lock);
        if (g_probe_ntbv < KLVK_PROBE_TBVS) {
            struct probe_tbv *e = &g_probe_tbvs[g_probe_ntbv++];
            e->buf = ci->buffer; e->off = ci->offset;
            e->range = ci->range; e->fmt = ci->format;
        }
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}


// ---------------------------------------------------------------------------
// Recording wrappers: the body above, then the pipeline hooks with the guest's create info
// (kl_vk_pipe_hooks.h). A replayer's own creates come through here too and are skipped.
// ---------------------------------------------------------------------------
static VkResult VKAPI_CALL klvk_CreateSampler(VkDevice dev, const VkSamplerCreateInfo *ci,
        const VkAllocationCallbacks *a, VkSampler *out) {
    VkResult r = klvk_CreateSampler_body(dev, ci, a, out);
    if (r == VK_SUCCESS && ci && out) klst_note_sampler(*out, ci);
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(sampler, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvk_CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo *ci,
        const VkAllocationCallbacks *a, VkShaderModule *out) {
    VkResult r = klvk_CreateShaderModule_body(dev, ci, a, out);
    if (eyedraw_on() && r == VK_SUCCESS && ci && ci->pCode && out) {
        unsigned char md[CC_SHA1_DIGEST_LENGTH]; char hex[41];
        CC_SHA1(ci->pCode, (CC_LONG)ci->codeSize, md);
        for (int i = 0; i < CC_SHA1_DIGEST_LENGTH; i++) snprintf(hex + 2 * i, 3, "%02x", md[i]);
        if (strcmp(hex, "47da31f1c09e08ca18b8f8bb6c18a9a8bae7d741") == 0 && g_ned_mod < 8) {
            g_ed_mod[g_ned_mod++] = (uint64_t)(uintptr_t)*out;
            fprintf(stderr, "[eyedraw] eye pixel shader module %#llx\n", (unsigned long long)(uintptr_t)*out);
        }
    }
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(shader_module, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvk_CreateDescriptorSetLayout(VkDevice dev, const VkDescriptorSetLayoutCreateInfo *ci,
        const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    VkResult r = klvk_CreateDescriptorSetLayout_body(dev, ci, a, out);
    if (r == VK_SUCCESS && ci && out && klst_binding() >= 0) {
        int nb = kl_env_int("KL_VK_SAMPLER_TRACE_NB", 0), has289 = 0;
        for (uint32_t i = 0; i < ci->bindingCount; i++) has289 |= ci->pBindings[i].binding == 289;
        if (nb && (int)ci->bindingCount == nb && has289) {
            g_st_layout = *out;
            fprintf(stderr, "[samptrace] traced set layout %#llx (%u bindings)\n", (unsigned long long)(uintptr_t)*out, ci->bindingCount);
        }
    }
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(set_layout, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvk_CreatePipelineLayout(VkDevice dev, const VkPipelineLayoutCreateInfo *ci,
        const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    static PFN_vkCreatePipelineLayout real;
    if (!real && real_gdpa) real = (PFN_vkCreatePipelineLayout)real_gdpa(dev, "vkCreatePipelineLayout");
    VkResult r = real ? real(dev, ci, a, out) : VK_ERROR_INITIALIZATION_FAILED;
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(pipeline_layout, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvk_CreateRenderPass(VkDevice dev, const VkRenderPassCreateInfo *ci,
        const VkAllocationCallbacks *a, VkRenderPass *out) {
    VkResult r = klvk_CreateRenderPass_body(dev, ci, a, out);
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(render_pass, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvk_CreateRenderPass2(VkDevice dev, const VkRenderPassCreateInfo2 *ci,
        const VkAllocationCallbacks *a, VkRenderPass *out) {
    VkResult r = klvk_CreateRenderPass2_body(dev, ci, a, out);
    if (r == VK_SUCCESS && ci && out) PIPE_HOOK(render_pass2, *out, ci);
    return r;
}
static VkResult VKAPI_CALL klvk_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
        const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    VkResult r = klvk_CreateGraphicsPipelines_body(dev, cache, n, ci, a, out);
    if (eyedraw_on() && ci && out)
        for (uint32_t i = 0; i < n; i++) if (out[i])
            for (uint32_t j = 0; j < ci[i].stageCount; j++)
                for (int m = 0; m < g_ned_mod; m++)
                    if ((uint64_t)(uintptr_t)ci[i].pStages[j].module == g_ed_mod[m] && g_ned_pipes < 64) {
                        g_ed_pipes[g_ned_pipes++] = (uint64_t)(uintptr_t)out[i];
                        fprintf(stderr, "[eyedraw] eye pipeline %#llx\n", (unsigned long long)(uintptr_t)out[i]);
                    }
    if (ci && out) for (uint32_t i = 0; i < n; i++) if (out[i]) PIPE_HOOK(graphics_pipeline, out[i], &ci[i]);
    if (out) for (uint32_t i = 0; i < n; i++) if (out[i]) PIPE_HOOK(pipeline_time, out[i], t_pipe_last_ms);
    return r;
}
static VkResult VKAPI_CALL klvk_CreateComputePipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
        const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    VkResult r = klvk_CreateComputePipelines_body(dev, cache, n, ci, a, out);
    if (ci && out) for (uint32_t i = 0; i < n; i++) if (out[i]) PIPE_HOOK(compute_pipeline, out[i], &ci[i]);
    return r;
}
static const entry g_ours[] = {
#define E(n) { "vk" #n, (PFN_vkVoidFunction)klvk_##n }
    E(CreateInstance), E(EnumerateInstanceExtensionProperties),
    E(EnumeratePhysicalDevices),
    E(CreateDevice),
    E(CreateAndroidSurfaceKHR), E(DestroySurfaceKHR),
    E(GetPhysicalDeviceSurfaceSupportKHR), E(GetPhysicalDeviceSurfaceCapabilitiesKHR),
    E(GetPhysicalDeviceSurfaceFormatsKHR), E(GetPhysicalDeviceSurfacePresentModesKHR),
    E(CreateSwapchainKHR), E(DestroySwapchainKHR), E(GetSwapchainImagesKHR),
    E(AcquireNextImageKHR), E(QueuePresentKHR),
    E(AllocateMemory), E(FreeMemory), E(BindImageMemory),
    E(QueueSubmit),
    E(GetPhysicalDeviceProperties),
    E(GetPhysicalDeviceProperties2),
    E(GetPhysicalDeviceQueueFamilyProperties),
    E(GetPhysicalDeviceFeatures), E(GetPhysicalDeviceFeatures2),
    { "vkGetPhysicalDeviceFeatures2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceFeatures2 },
    E(GetPhysicalDeviceMemoryProperties),
    E(GetPhysicalDeviceFormatProperties), E(GetPhysicalDeviceFormatProperties2),
    { "vkGetPhysicalDeviceFormatProperties2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceFormatProperties2 },
    E(EnumerateDeviceLayerProperties), E(EnumerateDeviceExtensionProperties),
    { "vkGetPhysicalDeviceProperties2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceProperties2 },
    E(GetPhysicalDeviceImageFormatProperties),
    E(GetPhysicalDeviceImageFormatProperties2),
    E(GetPhysicalDeviceMemoryProperties2),
    E(GetPhysicalDeviceFragmentShadingRatesKHR),
    { "vkGetPhysicalDeviceMemoryProperties2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceMemoryProperties2 },
    { "vkGetPhysicalDeviceImageFormatProperties2KHR", (PFN_vkVoidFunction)klvk_GetPhysicalDeviceImageFormatProperties2 },
    E(CreateImage), E(DestroyImage), E(CreateImageView), E(CmdCopyImage),
    E(BeginCommandBuffer),
    E(CmdBindIndexBuffer), E(CmdBindVertexBuffers), E(CmdDrawIndexed),
    E(CmdBindPipeline), E(CmdBindDescriptorSets),
    E(GetImageMemoryRequirements), E(CreateBufferView), E(CreateSampler),
    E(CreateShaderModule), E(CreateDescriptorSetLayout), E(CreatePipelineLayout), E(CreateDescriptorPool),
    E(UpdateDescriptorSets),
    E(CreateDescriptorUpdateTemplate), E(UpdateDescriptorSetWithTemplate),
    { "vkCreateDescriptorUpdateTemplateKHR", (PFN_vkVoidFunction)klvk_CreateDescriptorUpdateTemplate },
    { "vkUpdateDescriptorSetWithTemplateKHR", (PFN_vkVoidFunction)klvk_UpdateDescriptorSetWithTemplate },
    E(CmdPushDescriptorSetKHR), E(CmdPushDescriptorSetWithTemplateKHR),
    E(CmdBindDescriptorBuffersEXT),
    E(CreateBuffer), E(BindBufferMemory), E(MapMemory),
    E(CmdCopyBuffer), E(CmdUpdateBuffer),
    E(CmdBlitImage), E(CmdResolveImage), E(CmdClearColorImage),
    E(CmdCopyBufferToImage), E(CmdCopyImageToBuffer), E(CmdPipelineBarrier),
    E(CmdCopyBufferToImage2),
    { "vkCmdCopyBufferToImage2KHR", (PFN_vkVoidFunction)klvk_CmdCopyBufferToImage2 },
    E(CreateGraphicsPipelines), E(CreateComputePipelines),
    E(CreateFramebuffer), E(CmdBeginRenderPass),
    E(CmdBeginRenderPass2),
    { "vkCmdBeginRenderPass2KHR", (PFN_vkVoidFunction)klvk_CmdBeginRenderPass2 },
    E(CreateRenderPass), E(CreateRenderPass2),
    { "vkCreateRenderPass2KHR", (PFN_vkVoidFunction)klvk_CreateRenderPass2 },
    { "vkCreateRenderPassKHR",  (PFN_vkVoidFunction)klvk_CreateRenderPass },
    E(CmdBeginRendering),
    { "vkCmdBeginRenderingKHR", (PFN_vkVoidFunction)klvk_CmdBeginRendering },
#undef E
};

static VkResult VKAPI_CALL klvk_native_GetSwapchainImagesKHR(VkDevice dev, VkSwapchainKHR sc,
                                                              uint32_t *count, VkImage *images) {
    PFN_vkGetSwapchainImagesKHR real = (PFN_vkGetSwapchainImagesKHR)real_gdpa(dev, "vkGetSwapchainImagesKHR");
    VkResult r = real ? real(dev, sc, count, images) : VK_ERROR_INITIALIZATION_FAILED;
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && images && count)
        for (uint32_t i = 0; i < *count; i++) klvk_img_reg_add(images[i]);
    return r;
}

// KL_VK_FENCESTATS=1: fence traffic, printed every ~5 s from the submit path.
static _Atomic unsigned long g_fs_status_ok, g_fs_status_notready,
                             g_fs_wait_ok, g_fs_wait_timeout, g_fs_reset;
static VkResult VKAPI_CALL klvk_fs_GetFenceStatus(VkDevice d, VkFence f) {
    PFN_vkGetFenceStatus real = (PFN_vkGetFenceStatus)real_gdpa(d, "vkGetFenceStatus");
    VkResult r = real(d, f);
    if (r == VK_SUCCESS) g_fs_status_ok++; else g_fs_status_notready++;
    return r;
}
static VkResult VKAPI_CALL klvk_fs_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t to) {
    PFN_vkWaitForFences real = (PFN_vkWaitForFences)real_gdpa(d, "vkWaitForFences");
    VkResult r = real(d, n, f, all, to);
    if (r == VK_SUCCESS) g_fs_wait_ok++; else g_fs_wait_timeout++;
    return r;
}
static VkResult VKAPI_CALL klvk_fs_ResetFences(VkDevice d, uint32_t n, const VkFence *f) {
    PFN_vkResetFences real = (PFN_vkResetFences)real_gdpa(d, "vkResetFences");
    g_fs_reset += n;
    return real(d, n, f);
}
static void fs_report(void) {
    static uint64_t last;
    uint64_t now = (uint64_t)time(NULL);
    if (now - last < 5) return;
    last = now;
    fprintf(stderr, "  [fence] submits %lu (fenced %lu) status ok %lu notready %lu wait ok %lu timeout %lu resets %lu\n",
            g_fs_submit, g_fs_submit_fenced, g_fs_status_ok, g_fs_status_notready, g_fs_wait_ok, g_fs_wait_timeout, g_fs_reset);
}

// KL_VK_FPS=1: presents per second through the native swapchain.
static VkResult VKAPI_CALL klvk_native_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    static PFN_vkQueuePresentKHR real;
    if (!real) real = (PFN_vkQueuePresentKHR)real_gdpa(g_devs[0] ? g_devs[0]->dev : NULL, "vkQueuePresentKHR");
    static uint64_t t0, n;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    if (!t0) t0 = now;
    n++;
    if (now - t0 >= 5000000000ull) {
        fprintf(stderr, "  [fps] %.1f presents/s\n", (double)n * 1e9 / (double)(now - t0));
        t0 = now; n = 0;
    }
    return real(q, pi);
}

static PFN_vkVoidFunction ours(const char *name) {
    if (kl_env_on("KL_VK_FENCESTATS", 0) && name) {
        if (!strcmp(name, "vkGetFenceStatus")) return (PFN_vkVoidFunction)klvk_fs_GetFenceStatus;
        if (!strcmp(name, "vkWaitForFences")) return (PFN_vkVoidFunction)klvk_fs_WaitForFences;
        if (!strcmp(name, "vkResetFences")) return (PFN_vkVoidFunction)klvk_fs_ResetFences;
    }
    // KL_TRACE: reveal which render-pass / dynamic-rendering entry points the
    // guest actually resolves, and whether we hand back our wrapper or fall
    // through to the real MoltenVK one. This is how we learn why the compositing
    // render pass is invisible to the census.
    if (name && klvk_trace() && (strstr(name, "RenderPass") || strstr(name, "Rendering") || strstr(name, "Framebuffer"))) {
        static int rl;
        if (rl < 80) { rl++;
            PFN_vkVoidFunction hit = NULL;
            for (size_t i = 0; i < sizeof g_ours / sizeof *g_ours; i++)
                if (!strcmp(g_ours[i].name, name)) { hit = g_ours[i].fn; break; }
            fprintf(stderr, "[TRACE] [resolve] guest asked \"%s\" -> %s\n",
                name, hit ? "OUR wrapper" : "real MoltenVK (passthrough)");
        }
    }
    // Native WSI (a desktop guest presenting to a real window through a surface
    // SDL made on MoltenVK): surfaces, swapchains, acquire and present are
    // MoltenVK's own, not the headless swapchain built for XR guests. The
    // swapchain's images still have to join the image registry, or every copy,
    // blit and resolve into them is refused as an unknown image.
    if (kl_vk_native_wsi && name && !strcmp(name, "vkGetSwapchainImagesKHR"))
        return (PFN_vkVoidFunction)klvk_native_GetSwapchainImagesKHR;
    if (kl_vk_native_wsi && name && !strcmp(name, "vkQueuePresentKHR") && kl_env_on("KL_VK_FPS", 0))
        return (PFN_vkVoidFunction)klvk_native_QueuePresentKHR;
    if (kl_vk_native_wsi && name &&
        (strstr(name, "Surface") || strstr(name, "Swapchain") ||
         !strcmp(name, "vkAcquireNextImageKHR") || !strcmp(name, "vkQueuePresentKHR")))
        return NULL;
    for (size_t i = 0; i < sizeof g_ours / sizeof *g_ours; i++)
        if (!strcmp(g_ours[i].name, name)) return g_ours[i].fn;
    return NULL;
}

static PFN_vkVoidFunction VKAPI_CALL klvk_GetInstanceProcAddr(VkInstance inst,
                                                              const char *name);
static PFN_vkVoidFunction VKAPI_CALL klvk_GetDeviceProcAddr(VkDevice dev,
                                                            const char *name);

// A Vulkan entry point the guest resolved that neither we nor MoltenVK provide.
// Returning NULL here is what killed Wrath2: UE4 resolves device functions and
// calls them WITHOUT a null-check, so a NULL becomes a jump to 0x0 — which the
// visionOS crash report flags as CODESIGNING "Invalid Page" (an unmapped, and
// therefore unsigned, executable page). A named no-op stub is strictly safer:
// the guest gets a valid pointer, the call returns VK_SUCCESS (0 — also a safe
// value for a void or pointer-returning entry), and we log the name once so the
// missing function can be implemented properly if the guest actually depends on
// it. Same "name it when it's used" contract as the OVRP and JNI shims.
static VKAPI_ATTR VkResult VKAPI_CALL klvk_missing_trap(void) {
    return VK_SUCCESS;
}
static void klvk_note_missing(const char *name) {
    static char *seen[64];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (!strcmp(seen[i], name)) return;   // already reported
    if (nseen < 64) seen[nseen++] = strdup(name);   // copied — the guest's string need not outlive the call
    VKI("guest resolved '%s' but neither we nor MoltenVK provide it — handing back "
        "a no-op stub instead of NULL (a NULL here is the jump-to-0x0 that AMFI "
        "kills as an invalid page)\n", name);
}

static PFN_vkVoidFunction VKAPI_CALL klvk_GetInstanceProcAddr(VkInstance inst,
                                                              const char *name) {
    vk_init();
    if (!name) return NULL;
    VKT("gipa(%p, %s)\n", (void *)inst, name);
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetDeviceProcAddr;
    PFN_vkVoidFunction m = ours(name);
    if (m) return m;
    if (!g_avail) return NULL;
    PFN_vkVoidFunction fn = real_gipa(inst ? inst : g_instance, name);
    if (!fn) { klvk_note_missing(name); return (PFN_vkVoidFunction)klvk_missing_trap; }
    return fn;
}

static PFN_vkVoidFunction VKAPI_CALL klvk_GetDeviceProcAddr(VkDevice dev,
                                                            const char *name) {
    vk_init();
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return (PFN_vkVoidFunction)klvk_GetDeviceProcAddr;
    PFN_vkVoidFunction m = ours(name);
    if (m) return m;
    if (!g_avail || !real_gdpa) return NULL;
    PFN_vkVoidFunction fn = real_gdpa(dev, name);
    if (!fn) { klvk_note_missing(name); return (PFN_vkVoidFunction)klvk_missing_trap; }
    return fn;
}

// ---------------------------------------------------------------------------
// The synthetic-library face
// ---------------------------------------------------------------------------
static const int g_handle_tag = 0;
#define KLVK_HANDLE ((void *)&g_handle_tag)

int kl_vulkan_claims(const char *soname) {
    if (!name_is_vulkan(soname)) return 0;
    vk_init();
    return g_avail;
}

void *kl_vulkan_dlopen(const char *soname) {
    if (!name_is_vulkan(soname)) return NULL;
    vk_init();
    if (!g_avail) {
        VKI("guest dlopen(\"%s\") refused — MoltenVK is not vendored ('make mvk')\n",
            soname);
        return NULL;
    }
    VKI("guest dlopen(\"%s\") -> synthetic Vulkan loader over MoltenVK\n", soname);
    return KLVK_HANDLE;
}

int kl_vulkan_is_handle(const void *h) { return h == KLVK_HANDLE; }

void *kl_vulkan_sym(const char *name) {
    vk_init();
    if (!name || !g_avail) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (void *)klvk_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (void *)klvk_GetDeviceProcAddr;
    PFN_vkVoidFunction m = ours(name);
    if (m) return (void *)m;
    // MoltenVK exports the whole API statically, so a guest that resolves entry
    // points by dlsym rather than through vkGetInstanceProcAddr — which is legal
    // and which libSLZQuestNative does — gets a working pointer either way.
    return mvk_sym(name);
}

void *kl_vulkan_lookup(const char *name) {
    if (!name || strncmp(name, "vk", 2)) return NULL;
    return kl_vulkan_sym(name);
}

#endif /* __has_include(<vulkan/vulkan.h>) */
