// VXRuntime's Vulkan (the declarations of vulkan_core.h check every command's definition): the objects behind the handles, shared by the vk_*.m files. Vulkan goes straight to Metal:
// memory is placement heaps, descriptor sets are Metal 3 argument buffers written by vkUpdateDescriptorSets, shaders
// are translated by vk_msl.c, and command buffers record into a compact list of their own that vkQueueSubmit encodes
// into a Metal command buffer (Vulkan command buffers are reusable and recorded on any thread; Metal's are neither).
// Why commands go straight to Metal rather than through the common GPU IR: the cost per draw. Recording every draw in
// the GPU IR (descriptor snapshots, resource objects) was what 506c510 took out of Direct3D 11 (~11 us of CPU per
// draw, the render thread the frame-rate limit). Why shaders have a translator of their own is a separate decision,
// not about speed (vk_msl.h).
#ifndef VX_VK_IMPL_H
#define VX_VK_IMPL_H
#include "vulkan/vulkan_core.h"
#include "vk_win32.h"
#include "vk_msl.h"
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

#ifndef WINAPI
#define WINAPI __attribute__((ms_abi))
#endif
#define VKFN VKAPI_ATTR
void vx_log(const char *fmt, ...);

// ---- formats (vk_format.m) ---------------------------------------------------------------------------------------
enum { VKF_COLOR = 1, VKF_DEPTH = 2, VKF_STENCIL = 4 };
enum { VKF_FLOAT = 1, VKF_UINT, VKF_SINT };   // numeric class of a color format's values
typedef struct {
    VkFormat vk;
    MTLPixelFormat mtl;            // MTLPixelFormatInvalid: not an image format here
    MTLVertexFormat vertex;        // MTLVertexFormatInvalid: not a vertex format
    uint8_t bytes;                 // bytes per texel (per block for compressed formats)
    uint8_t block_w, block_h;
    uint8_t aspects, numeric, srgb, compressed;
    uint8_t substitute;            // a format Metal lacks, kept in a wider one (D24 in D32)
} vk_format_info;
const vk_format_info *vk_format(VkFormat format);
VkFormatFeatureFlags vk_format_features(id<MTLDevice> mtl, const vk_format_info *f, int optimal, int buffer);

// ---- instance and devices ----------------------------------------------------------------------------------------
#define VK_LOADER_DATA_WORD void *loader_data   // (dispatchable objects begin with a pointer, as the ICD ABI has it)

typedef struct vk_messenger {
    PFN_vkDebugUtilsMessengerCallbackEXT callback;
    void *user;
    VkDebugUtilsMessageSeverityFlagsEXT severities;
    VkDebugUtilsMessageTypeFlagsEXT types;
    struct vk_messenger *next;
} vk_messenger;

struct VkPhysicalDevice_T {
    VK_LOADER_DATA_WORD;
    VkInstance instance;
    __unsafe_unretained id<MTLDevice> mtl;
};

struct VkInstance_T {
    VK_LOADER_DATA_WORD;
    uint32_t api_version;
    uint32_t extensions;           // VK_INST_EXT_* bits enabled
    __strong id<MTLDevice> mtl;
    struct VkPhysicalDevice_T phys;
    pthread_mutex_t lock;
    vk_messenger *messengers;
};
enum { VK_INST_EXT_SURFACE = 1, VK_INST_EXT_WIN32_SURFACE = 2, VK_INST_EXT_DEBUG_UTILS = 4, VK_INST_EXT_PROPS2 = 8,
       VK_INST_EXT_COLORSPACE = 16, VK_INST_EXT_DEBUG_REPORT = 32, VK_INST_EXT_SURFACE_CAPS2 = 64 };

#define VK_MAX_QUEUES 4
struct VkQueue_T {
    VK_LOADER_DATA_WORD;
    VkDevice device;
    uint32_t family, index;
    __strong id<MTLCommandQueue> mtl;
    pthread_mutex_t lock;          // (submissions to one queue are serialized, as Vulkan requires of callers anyway)
    __strong id<MTLSharedEvent> done;   // signaled with each submission's number when it completes
    uint64_t submitted;            // the last submission's number
    __strong id<MTLFence> last_fence;   // (every encoder waits for the one before it: see vk_command.m)
    __strong id<MTLBuffer> gs_scratch[4];   // geometry draws: vertex outputs, triangles, counts; tessellation: vertex
                                            // outputs, control points, patches, factors (reused draw to draw)
};

typedef struct vk_heap_list {
    __strong id<MTLHeap> *heaps; uint32_t count, cap; pthread_mutex_t lock; uint64_t generation;
    // macOS 15 and later: the heaps also in one residency set every queue has (id<MTLResidencySet>), committed before a
    // submission when it changed; the encoders then need no useHeaps (vk_command.m: 150 render passes a frame each
    // listing every heap was a quarter of No Man's Sky's encoding time, 2026-10-26).
    __strong id residency;
    int residency_dirty;
} vk_heap_list;
void vk_residency_commit(VkDevice dev);   // (before a submission: the set as the heaps are now)
int vk_residency_on(VkDevice dev);         // (the encoders may leave the heaps out)

struct VkDevice_T {
    VK_LOADER_DATA_WORD;
    VkPhysicalDevice phys;
    __unsafe_unretained id<MTLDevice> mtl;
    struct VkQueue_T queues[VK_MAX_QUEUES];
    uint32_t queue_count;
    uint32_t extensions;           // VK_DEV_EXT_* bits enabled
    VkPhysicalDeviceFeatures features;
    vk_heap_list heaps;            // every live memory heap, made resident in each encoder
    __strong id<MTLLibrary> internal;   // blits, clears and fills (vk_internal.m)
    pthread_mutex_t internal_lock;
    __strong NSMutableDictionary *internal_pipelines;
    __strong id<MTLSamplerState> internal_linear, internal_nearest;
    __strong id<MTLBuffer> zero_buffer;   // (bound where a vertex binding has no buffer)
};

// ---- memory, buffers, images -------------------------------------------------------------------------------------
enum { VK_MEMTYPE_DEVICE = 0, VK_MEMTYPE_HOST = 1, VK_MEMTYPE_HOST_CACHED = 2, VK_MEMTYPE_COUNT = 3 };
struct VkDeviceMemory_T {
    uint64_t size;
    uint32_t type;
    __strong id<MTLHeap> heap;
    __strong id<MTLBuffer> backing;   // host-visible memory: a buffer over the whole heap (the mapping)
    void *map;
    struct VkImage_T *dedicated_image;
    uint32_t handle;                  // an exported Win32 handle, 0 when none
    // External memory (VK_KHR_external_memory_win32): a dedicated image's texture on a global IOSurface, imported from
    // a shared handle (Direct3D's, another process's) or made here to be exported; shared_slot is its handle's slot + 1.
    __strong id<MTLTexture> external;
    int shared_slot;
    int keyed_slot;                   // its slot + 1 (imported or exported): the keyed mutex VK_KHR_win32_keyed_mutex takes
};

struct VkBuffer_T {
    uint64_t size;
    VkBufferUsageFlags usage;
    VkBufferCreateFlags flags;
    VkDeviceMemory memory;
    uint64_t memory_offset;
    __strong id<MTLBuffer> mtl;       // the placed buffer, or the memory's backing buffer
    uint64_t offset;                  // this buffer's start within mtl
    uint64_t gpu;                     // GPU address of the start
};

struct VkImage_T {
    VkImageType type;
    VkFormat format;
    const vk_format_info *fmt;
    VkExtent3D extent;
    uint32_t mips, layers, samples;
    VkImageUsageFlags usage;
    VkImageCreateFlags flags;
    VkImageTiling tiling;
    __strong MTLTextureDescriptor *desc;
    __strong id<MTLTexture> mtl;
    VkDeviceMemory memory;
    uint64_t memory_offset;
    uint64_t row_pitch;               // linear images
    struct vk_swapchain *swapchain;   // swapchain images (not bound to memory)
    uint32_t swapchain_index;
    int external;                     // made from shared memory of another process / API
};

struct VkImageView_T {
    VkImage image;
    VkImageViewType view_type;
    VkFormat format;
    VkImageSubresourceRange range;
    __strong id<MTLTexture> mtl;      // a texture view (or the image's texture when it covers it all)
    __strong id<MTLTexture> stencil;  // depth/stencil images: the stencil aspect's view
    uint32_t width, height;
};

struct VkBufferView_T {
    VkBuffer buffer;
    VkFormat format;
    uint64_t offset, range;
    __strong id<MTLTexture> mtl;      // a texture over the buffer's bytes
};

struct VkSampler_T {
    __strong id<MTLSamplerState> mtl;
    VkSamplerCreateInfo info;
};

// ---- descriptors -------------------------------------------------------------------------------------------------
typedef struct {
    uint32_t binding;
    VkDescriptorType type;
    uint32_t count;                   // descriptors (for variable-count bindings, the most)
    uint32_t word;                    // first 8-byte word in the set's argument buffer
    uint32_t dynamic;                 // dynamic buffers: first index among the layout's dynamic offsets
    VkShaderStageFlags stages;
    VkDescriptorBindingFlags flags;
    VkSampler *immutable;             // immutable samplers (count of them), or NULL
} vk_set_binding;

struct VkDescriptorSetLayout_T {
    uint32_t refs;
    uint32_t binding_count;
    vk_set_binding *bindings;         // sorted by binding number
    uint32_t words;                   // argument buffer size in words (with the variable binding at its maximum)
    uint32_t dynamic_count;
    VkDescriptorSetLayoutCreateFlags flags;
    int variable_binding;             // index of a VARIABLE_DESCRIPTOR_COUNT binding, -1 if none
};

// What a descriptor refers to, kept beside the argument buffer words (for residency of resources outside heaps,
// copies between sets, and the byte ranges of buffer descriptors).
typedef struct {
    VkDescriptorType type;
    void *object;                     // VkBuffer, VkImageView, VkBufferView or VkSampler
    void *object2;                    // combined image samplers: the sampler
    uint64_t offset, range;
} vk_descriptor;

struct VkDescriptorPool_T {
    __strong id<MTLBuffer> buffer;    // the argument buffers of all its sets
    uint64_t size, used;
    VkDescriptorPoolCreateFlags flags;
    struct vk_free_range { uint64_t offset, size; } *free;
    uint32_t free_count, free_cap;
    struct VkDescriptorSet_T **sets;
    uint32_t set_count, set_cap;
    pthread_mutex_t lock;
};

struct VkDescriptorSet_T {
    VkDescriptorPool pool;
    VkDescriptorSetLayout layout;
    uint64_t offset, size;            // its argument buffer within the pool's buffer
    uint64_t *words;                  // CPU address of that buffer
    uint32_t variable_count;
    vk_descriptor *descriptors;       // one per descriptor, in binding order
    uint32_t descriptor_count;
    uint32_t *first;                  // index into descriptors of each layout binding's first
    int has_unheaped;                 // (refers to swapchain images or other textures outside the memory heaps)
};

struct VkPipelineLayout_T {
    uint32_t set_count;
    VkDescriptorSetLayout sets[VKMSL_MAX_SETS];
    uint32_t push_size;
    VkShaderStageFlags push_stages;
    uint32_t dynamic_base[VKMSL_MAX_SETS];   // each set's first index among all dynamic offsets
    uint32_t dynamic_count;
};

struct VkDescriptorUpdateTemplate_T {
    uint32_t count;
    VkDescriptorUpdateTemplateEntry *entries;
    VkDescriptorSetLayout layout;
};

// ---- shaders and pipelines ---------------------------------------------------------------------------------------
struct VkShaderModule_T {
    uint32_t *code;
    size_t words;
};

struct VkPipelineCache_T { int unused; };

enum { VK_DYN_VIEWPORT = 1u << 0, VK_DYN_SCISSOR = 1u << 1, VK_DYN_LINE_WIDTH = 1u << 2, VK_DYN_DEPTH_BIAS = 1u << 3,
       VK_DYN_BLEND_CONSTANTS = 1u << 4, VK_DYN_DEPTH_BOUNDS = 1u << 5, VK_DYN_STENCIL_COMPARE = 1u << 6,
       VK_DYN_STENCIL_WRITE = 1u << 7, VK_DYN_STENCIL_REFERENCE = 1u << 8, VK_DYN_CULL_MODE = 1u << 9,
       VK_DYN_FRONT_FACE = 1u << 10, VK_DYN_TOPOLOGY = 1u << 11, VK_DYN_VIEWPORT_COUNT = 1u << 12,
       VK_DYN_SCISSOR_COUNT = 1u << 13, VK_DYN_STRIDE = 1u << 14, VK_DYN_DEPTH_TEST = 1u << 15,
       VK_DYN_DEPTH_WRITE = 1u << 16, VK_DYN_DEPTH_COMPARE = 1u << 17, VK_DYN_DEPTH_BOUNDS_TEST = 1u << 18,
       VK_DYN_STENCIL_TEST = 1u << 19, VK_DYN_STENCIL_OP = 1u << 20, VK_DYN_DEPTH_BIAS_ENABLE = 1u << 21,
       VK_DYN_PRIMITIVE_RESTART = 1u << 22, VK_DYN_RASTERIZER_DISCARD = 1u << 23 };

struct VkPipeline_T {
    VkPipelineBindPoint bind_point;
    VkPipelineLayout layout_copy_unused;
    struct VkPipelineLayout_T layout;     // (a copy: the layout may be destroyed while the pipeline lives)
    __strong id<MTLRenderPipelineState> render;
    __strong id<MTLComputePipelineState> compute;
    __strong id<MTLDepthStencilState> depth_stencil;
    MTLSize threads;                      // compute: the workgroup size
    uint32_t dynamic;                     // VK_DYN_* states taken from commands
    // Fixed state, applied when the pipeline is bound (unless dynamic).
    MTLPrimitiveType primitive;
    MTLCullMode cull;
    MTLWinding winding;
    MTLTriangleFillMode fill;
    MTLDepthClipMode depth_clip;
    int depth_bias_enable;
    float depth_bias[3];                  // constant, slope, clamp
    float blend_constants[4];
    uint32_t stencil_ref[2];
    uint32_t viewport_count, scissor_count;
    VkViewport viewports[16];
    VkRect2D scissors[16];
    VkPipelineDepthStencilStateCreateInfo ds;   // (for dynamic depth/stencil states)
    uint32_t vertex_strides[VKMSL_MAX_VERTEX_BUFFERS];
    uint32_t vertex_bindings;            // mask of vertex buffer bindings used
    int rasterizer_discard;
    uint32_t sets_vertex, sets_fragment, sets_compute;   // descriptor sets each stage reads
    uint8_t push_vertex, push_fragment, push_compute, aux_vertex, aux_fragment, aux_compute;
    uint32_t sample_mask;
    // A geometry shader: the vertex shader as kernels (not indexed, 16- and 32-bit indices), the geometry kernel, and
    // the render pipeline drawing its triangles ("vkpass" and the fragment shader).
    __strong id<MTLComputePipelineState> gs_vs[3];
    __strong id<MTLComputePipelineState> gs_kernel;
    uint32_t gs_in_vertices, gs_max_tris, gs_out_slots, gs_invocations, gs_in_slots;
    uint32_t sets_geometry;
    uint8_t push_geometry, aux_geometry;
    uint8_t vertex_rate[VKMSL_MAX_VERTEX_BUFFERS];   // 1: per instance
    uint32_t vertex_divisor[VKMSL_MAX_VERTEX_BUFFERS];
    VkPrimitiveTopology topology;
    uint8_t primitive_restart;            // (input assembly's primitiveRestartEnable)
    // Tessellation: the vertex shader as gs_vs kernels, the control shader as a kernel, the evaluation shader as the
    // render pipeline's (post-tessellation) vertex function.
    __strong id<MTLComputePipelineState> tcs_kernel;
    __strong id<MTLComputePipelineState> tcs_kernel_one;   // (VXRT_VK_TCS_AB=1: the same with a patch a threadgroup, to compare)
    uint8_t tess, tess_quad, push_tcs, aux_tcs, push_tes, aux_tes;
    uint32_t sets_tcs, sets_tes;
    uint32_t patch_cp, tess_out_vertices, tess_cp_slots, tess_patch_slots;
};

// Render passes: attachments and subpasses as given; framebuffers name the views.
typedef struct {
    uint32_t color_count;
    uint32_t colors[8];                  // attachment indices (VK_ATTACHMENT_UNUSED allowed)
    uint32_t resolves[8];
    uint32_t depth;                      // VK_ATTACHMENT_UNUSED if none
    uint32_t depth_resolve;
    uint32_t inputs[8], input_count;
    uint32_t view_mask;
    VkResolveModeFlagBits depth_resolve_mode, stencil_resolve_mode;
} vk_subpass;

struct VkRenderPass_T {
    uint32_t attachment_count;
    VkAttachmentDescription2 *attachments;
    uint32_t subpass_count;
    vk_subpass *subpasses;
    uint32_t *first_use, *last_use;     // per attachment: the subpasses that load and store it
    uint64_t *dep_dst;                  // per subpass: the stages its dependencies make wait (VkSubpassDependency2 dstStageMask)
};
// The stages of a draw before rasterization (pipeline stage bits, Vulkan 1.0 and synchronization2): a dependency that
// makes them wait needs the previous work done before a render pass's vertex stage; others before its fragment stage.
#define VK_PRE_RASTER_STAGES (0x2ull | 0x4ull | 0x8ull | 0x10ull | 0x20ull | 0x40ull | 0x8000ull | 0x10000ull | \
                              0x1000000000ull | 0x2000000000ull | 0x4000000000ull)

struct VkFramebuffer_T {
    uint32_t width, height, layers;
    uint32_t attachment_count;
    VkImageView *views;                  // NULL entries for an imageless framebuffer
};

// ---- command buffers -------------------------------------------------------------------------------------------
struct VkCommandPool_T {
    uint32_t family;
    VkCommandPoolCreateFlags flags;
    struct VkCommandBuffer_T *buffers;   // (a list through next)
    pthread_mutex_t lock;
};

typedef struct vk_chunk { struct vk_chunk *next; size_t used, size; uint8_t data[]; } vk_chunk;

struct VkCommandBuffer_T {
    VK_LOADER_DATA_WORD;
    VkDevice device;
    VkCommandPool pool;
    VkCommandBufferLevel level;
    struct VkCommandBuffer_T *next, *prev;
    VkCommandBufferUsageFlags usage;
    int recording;
    vk_chunk *first, *last;              // the recorded commands
    uint32_t count;
};

// ---- synchronization ------------------------------------------------------------------------------------------------
struct VkFence_T {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int signaled;
    uint64_t pending;                    // submissions not yet complete that signal it
};

struct VkSemaphore_T {
    int timeline;
    __strong id<MTLSharedEvent> event;
    _Atomic uint64_t value;              // binary: the value the next wait waits for; timeline: the last signal submitted
};

struct VkEvent_T { _Atomic int set; };

struct VkQueryPool_T {
    VkQueryType type;
    uint32_t count;
    __strong id<MTLBuffer> results;      // occlusion: Metal visibility results; timestamps: values
    uint8_t *available;
};

// ---- surfaces and swapchains -----------------------------------------------------------------------------------
typedef struct vk_surface {
    VkInstance instance;
    void *hwnd;
    __strong CAMetalLayer *layer;
} vk_surface;

typedef struct vk_swapchain {
    VkDevice device;
    vk_surface *surface;
    VkSwapchainCreateInfoKHR info;
    uint32_t count;
    struct VkImage_T *images;
    VkImage *handles;
    uint64_t *presented;                 // queue submission number of each image's last present
    VkQueue *present_queue;
    uint32_t next;
    int retired;
    __strong id<MTLHeap> heap;
} vk_swapchain;

// ---- shared helpers ---------------------------------------------------------------------------------------------
const void *vk_find_struct(const void *chain, VkStructureType type);
void *vk_find_struct_mut(void *chain, VkStructureType type);
void vk_device_add_heap(VkDevice device, id<MTLHeap> heap);
void vk_device_remove_heap(VkDevice device, id<MTLHeap> heap);
uint32_t vk_api_version(void);
VkResult vk_queue_submit_internal(VkQueue queue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence);
id<MTLComputePipelineState> vk_internal_compute(VkDevice device, NSString *name);
id<MTLRenderPipelineState> vk_internal_render(VkDevice device, NSString *vs, NSString *fs, MTLPixelFormat color,
                                              MTLPixelFormat depth, MTLPixelFormat stencil, NSUInteger samples,
                                              int write_depth);
void vk_internal_init(VkDevice device);
int vk_trace(void);
// 1 the first time a kind of message is asked for with this value (what is not supported is logged once, not per call).
int vk_log_once(const char *kind, uint32_t value);
// vx_log, but a message already logged (the same text) is not logged again.
void vk_log_unique(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- VXRT_STATS: what a Vulkan program's frames cost (vk_queue.m writes them to the log every 5 seconds) ----------
enum {
    VKS_SUBMITS, VKS_CMDBUFS, VKS_MTL_CBS, VKS_SUBMIT_NS, VKS_ENCODE_NS, VKS_KEYED_NS,
    VKS_PRESENTS, VKS_PRESENT_NS, VKS_DRAWABLE_NS, VKS_ACQUIRES, VKS_ACQUIRE_NS,
    VKS_FENCE_WAITS, VKS_FENCE_NS, VKS_SEM_WAITS, VKS_SEM_NS, VKS_IDLE_WAITS, VKS_IDLE_NS, VKS_QUERY_IDLE,
    VKS_GFX_PIPES, VKS_GFX_PIPE_NS, VKS_COMP_PIPES, VKS_COMP_PIPE_NS, VKS_TRANSLATE_NS, VKS_MSL_COMPILES, VKS_MSL_COMPILE_NS,
    VKS_MSL_CACHED, VKS_DESC_UPDATES, VKS_DESC_WRITES, VKS_DESC_TEMPLATES, VKS_DESC_ALLOCS,
    VKS_BUFFERS, VKS_IMAGES, VKS_VIEWS, VKS_ALLOCS, VKS_ALLOC_BYTES, VKS_MAPS,
    VKS_DRAWS, VKS_INDIRECT, VKS_DISPATCHES, VKS_PASSES, VKS_ENCODERS, VKS_COPY_BYTES, VKS_UPDATE_BYTES, VKS_GS_TESS,
    VKS_TESS_VERTS, VKS_TESS_PATCHES, VKS_TESS_CP, VKS_BARRIERS, VKS_BARRIERS_IN_PASS, VKS_VERTEX_WAITS, VKS_GPU_NS, VKS_COUNT
};
extern _Atomic uint64_t vk_stats[VKS_COUNT];
int vk_stats_on(void);
static inline uint64_t vk_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
#define VK_STAT(i, n) do { if (vk_stats_on()) atomic_fetch_add_explicit(&vk_stats[i], (uint64_t)(n), memory_order_relaxed); } while (0)
// (a stretch of time: VK_STAT_T0(x); ... VK_STAT_NS(VKS_..., x);)
#define VK_STAT_T0(t) uint64_t t = vk_stats_on() ? vk_now_ns() : 0
#define VK_STAT_NS(i, t) do { if (t) atomic_fetch_add_explicit(&vk_stats[i], vk_now_ns() - (t), memory_order_relaxed); } while (0)
void vk_stats_frame(void);
void vk_stats_gpu(id<MTLCommandBuffer> mcb);   // (its GPU time counted when it completes)
// A tuning switch: VXRT_VK_<name> (0 or 1), else the default. VXRT_VK_LIVE=<file> (lines <name>=0|1) overrides it while
// the program runs (read again every second): one run can measure both settings alternately (vk_queue.m).
int vk_option(const char *name, int dflt);   // (each present: the 5-second report, frame times)

// Exported commands (vk_dispatch.c lists them).
PFN_vkVoidFunction vk_lookup(const char *name, int level);   // level 0 global, 1 instance, 2 device

#endif
