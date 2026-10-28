// Direct3D 12 on Metal (d3d12.dll): the device, queues, command lists recorded and replayed into Metal command
// buffers, fences as MTLSharedEvents, descriptor heaps, root signatures (the serialized RTS0 format, both ways),
// pipeline states made from DXIL translated by dxil_msl.c, committed resources and their views.
//
// Descriptors are 32 bytes in the layout the translated shaders read (VxDesc, dxil_msl.h): a GPU address, a Metal
// resource id, a size (or flagged texture-SRV min-LOD float bits), a stride, a format and flags. Shader-visible heaps
// live in Metal buffers, so a GPU descriptor
// handle is a GPU address the shaders can read; at each dispatch the root signature is resolved into one pointer per
// resource range the shader declares (VxRoot at buffer 0).
//
// Implemented: compute and graphics pipelines, draws, UAV clears for typed and raw buffers plus common texture views, swap
// chains on D3D12 queues, placed resources, timestamp and occlusion queries, and ExecuteIndirect tier 1.0 argument
// types plus tier 1.1 incrementing constants, including GPU-side Metal lowering for fixed-signature dispatch/draw
// ranges and their optional count buffers. Direct command lists can execute closed graphics bundles with inherited
// target/root state and bundle state leakage; the current bundle subset supports direct draws and restricted
// single-operation ExecuteIndirect draw signatures without count buffers or root/IA updates. WriteBufferImmediate
// records aligned 32-bit writes on direct, compute, and copy lists through ordered Metal blit operations.
// Opaque-layout textures support Map plus synchronous WriteToSubresource/ReadFromSubresource transfers in COMMON state
// for single-sample, single-plane formats through shared Metal staging buffers and blit commands. Reserved resources
// (tiled resources tier 2, feature level 12_0) are Metal 4 placement-sparse resources whose tiles UpdateTileMappings
// maps onto the D3D12 heaps' Metal placement heaps; pipeline libraries keep pipelines by name and description.
// Not done yet: 3D (tier 3) tiles, protected resource sessions, sampler feedback, ray/mesh dispatch,
// DXBC hull/domain stages, full typed-buffer format coverage, atomics outside R32 integer resources supported by
// the active Metal GPU, structured-buffer UAV clears, and ExecuteIndirect bundle signatures that change root or IA
// state or use a count buffer.
#include "d3d_common.h"
#import <ImageIO/ImageIO.h>
#include "d3d12_view.h"
#include "dxil_msl.h"
#include "dxil_ir.h"
#include "dxbc.h"
#include "gpu_ir.h"
#include "gpu_ir_metal.h"
#include "gpu_ir_msl.h"
#include <float.h>
#include <sys/stat.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "d3d12_tables.h"
#include "d3d_tables.h"                                            // (IDXGISwapChain3, for swap chains on D3D12 queues)
#pragma clang diagnostic pop

enum { K12_DEVICE = 1, K12_QUEUE, K12_ALLOCATOR, K12_LIST, K12_FENCE, K12_HEAP, K12_ROOTSIG, K12_PSO, K12_RESOURCE, K12_BLOB, K12_DEBUG, K12_MEMHEAP, K12_QUERYHEAP, K12_COMMAND_SIGNATURE, K12_PIPELINE_LIBRARY };
#define D3D12_ALL_SUBRESOURCES UINT32_MAX
enum { D3D12_LAYOUT_UNKNOWN12 = 0, D3D12_LAYOUT_ROW_MAJOR12 = 1, D3D12_LAYOUT_64KB_UNDEFINED_SWIZZLE12 = 2 };
enum { D3D12_RESOLVE_MODE_MIN12 = 1, D3D12_RESOLVE_MODE_MAX12 = 2, D3D12_RESOLVE_MODE_AVERAGE12 = 3 };
enum {
    D3D12_STATE_VERTEX_CONSTANT = 0x00000001u,
    D3D12_STATE_INDEX = 0x00000002u,
    D3D12_STATE_RENDER_TARGET = 0x00000004u,
    D3D12_STATE_UAV = 0x00000008u,
    D3D12_STATE_DEPTH_WRITE = 0x00000010u,
    D3D12_STATE_DEPTH_READ = 0x00000020u,
    D3D12_STATE_NON_PIXEL_SHADER = 0x00000040u,
    D3D12_STATE_PIXEL_SHADER = 0x00000080u,
    D3D12_STATE_STREAM_OUT = 0x00000100u,
    D3D12_STATE_INDIRECT = 0x00000200u,
    D3D12_STATE_COPY_DEST = 0x00000400u,
    D3D12_STATE_COPY_SOURCE = 0x00000800u,
    D3D12_STATE_RESOLVE_DEST = 0x00001000u,
    D3D12_STATE_RESOLVE_SOURCE = 0x00002000u,
    D3D12_STATE_VIDEO_DECODE_READ = 0x00010000u,
    D3D12_STATE_VIDEO_DECODE_WRITE = 0x00020000u,
    D3D12_STATE_VIDEO_PROCESS_READ = 0x00040000u,
    D3D12_STATE_VIDEO_PROCESS_WRITE = 0x00080000u,
    D3D12_STATE_RAYTRACING_AS = 0x00400000u,
    D3D12_STATE_VIDEO_ENCODE_READ = 0x00200000u,
    D3D12_STATE_VIDEO_ENCODE_WRITE = 0x00800000u,
    D3D12_STATE_SHADING_RATE = 0x01000000u
};

static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static void **g_swapchain_vtbl;
static void **g_plib_vtbl;
static void **g_device_vtbl, **g_queue_vtbl, **g_allocator_vtbl, **g_list_vtbl, **g_fence_vtbl, **g_heap_vtbl, **g_rootsig_vtbl,
            **g_pso_vtbl, **g_resource_vtbl, **g_blob_vtbl, **g_debug_vtbl, **g_memheap_vtbl, **g_queryheap_vtbl, **g_command_signature_vtbl;
static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;
static void make_tables(void);

// ---- the D3D12 structures this runtime reads (their Windows layouts: 4-byte enums, 8-byte handles) -----------------

typedef struct { int32_t Type, Priority, Flags; uint32_t NodeMask; } Q_DESC;
typedef struct { int32_t Type; uint32_t NumDescriptors; int32_t Flags; uint32_t NodeMask; } HEAP_DESC;
typedef struct { int32_t Type, CPUPageProperty, MemoryPoolPreference; uint32_t CreationNodeMask, VisibleNodeMask; } HEAP_PROPS;
typedef struct { uint64_t SizeInBytes; HEAP_PROPS Properties; uint64_t Alignment; int32_t Flags; } MEMHEAP_DESC;   // D3D12_HEAP_DESC
typedef struct { uint64_t SizeInBytes, Alignment; } ALLOC_INFO;
typedef struct { int32_t Dimension; uint64_t Alignment, Width; uint32_t Height; uint16_t DepthOrArraySize, MipLevels; uint32_t Format, SampleCount, SampleQuality; int32_t Layout, Flags; } RES_DESC;
typedef struct { RES_DESC resource; struct { uint32_t Width, Height, Depth; } sampler_feedback_mip_region; } RES_DESC1;
_Static_assert(__builtin_offsetof(RES_DESC1, sampler_feedback_mip_region) == sizeof(RES_DESC),
               "D3D12_RESOURCE_DESC1 mip-region ABI layout");
static int resource_alignment12(const RES_DESC *desc, uint64_t *alignment);
static int d3d12_resolve_typeless(uint32_t format);
static int no_shared_3d12(void);
typedef struct { const void *code; size_t size; } BYTECODE;
typedef struct { void *root; BYTECODE cs; uint32_t node; const void *cached; size_t cached_size; int32_t flags; } COMPUTE_PSO_DESC;
typedef struct { uint64_t location; uint32_t size; } CBV_DESC;
typedef struct {
    uint32_t format; int32_t dim; uint32_t mapping;
    union {
        struct { uint64_t first; uint32_t count, stride; int32_t flags; } buffer;
        struct { uint32_t most_detailed, mips, plane; float min_lod; } tex2d;
        struct { uint32_t most_detailed, mips, first_slice, slices, plane; float min_lod; } tex2darray;
        uint32_t raw[6];
    };
} SRV_DESC;
typedef struct {
    uint32_t format; int32_t dim;
    union {
        struct { uint64_t first; uint32_t count, stride; uint64_t counter_offset; int32_t flags; } buffer;
        struct { uint32_t mip, plane; } tex2d;
        struct { uint32_t mip, first_slice, slices, plane; } tex2darray;
        struct { uint32_t mip, first_w_slice, w_size; } tex3d;
        uint32_t raw[8];
    };
} UAV_DESC;
typedef struct { int32_t filter, u, v, w; float bias; uint32_t aniso; int32_t cmp; float border[4]; float min_lod, max_lod; } SAMPLER_DESC;
typedef struct { uint64_t offset; uint32_t format, width, height, depth, row_pitch; } FOOTPRINT;
typedef struct { void *res; int32_t type; uint32_t pad; union { FOOTPRINT footprint; uint32_t subresource; }; } COPY_LOC;
typedef struct { uint32_t left, top, front, right, bottom, back; } BOX;
typedef struct { void *resource; uint32_t subresource; int32_t before, after; } TRANSITION_BARRIER;
typedef struct { void *before, *after; } ALIASING_BARRIER;
typedef struct { void *resource; } UAV_BARRIER;
typedef struct { int32_t type, flags; union { TRANSITION_BARRIER transition; ALIASING_BARRIER aliasing; UAV_BARRIER uav; }; } RESOURCE_BARRIER;
typedef struct { size_t begin, end; } RANGE;
typedef struct { const void *decl; uint32_t n, pad; const uint32_t *strides; uint32_t nstrides, rasterized; } SO_DESC;
typedef struct { int32_t enable, logic_enable, src, dst, op, src_a, dst_a, op_a, logic_op; uint8_t mask, pad[3]; } RT_BLEND;
typedef struct { int32_t a2c, independent; RT_BLEND rt[8]; } BLEND_DESC;
typedef struct { int32_t fill, cull, front_ccw, depth_bias; float bias_clamp, slope; int32_t depth_clip, msaa, aa_lines; uint32_t forced_samples; int32_t conservative; } RASTER_DESC;
typedef struct { int32_t fail, depth_fail, pass, func; } STENCIL_OP;
typedef struct { int32_t depth_enable, depth_write, depth_func, stencil_enable; uint8_t read_mask, write_mask, pad[2]; STENCIL_OP front, back; } DS_DESC;
typedef struct { const void *elements; uint32_t n, pad; } IL_DESC;
typedef struct {
    void *root; BYTECODE vs, ps, ds, hs, gs; SO_DESC so; BLEND_DESC blend; uint32_t sample_mask; RASTER_DESC raster; DS_DESC depth;
    IL_DESC input; int32_t strip_cut, topology_type; uint32_t nrt, rtv_formats[8], dsv_format, samples, quality, node;
    const void *cached; size_t cached_size; int32_t flags;
} GFX_PSO_DESC;
typedef struct { const char *name; uint32_t index, format, slot, offset; int32_t cls; uint32_t step; } INPUT_ELEMENT;
typedef struct { uint32_t format; int32_t dim; union { struct { uint32_t mip, plane; } tex2d; struct { uint32_t mip, first, size, plane; } tex2darray; uint32_t raw[4]; }; } RTV_DESC;
typedef struct { uint32_t format; int32_t dim; int32_t flags; union { struct { uint32_t mip; } tex2d; struct { uint32_t mip, first, size; } tex2darray; uint32_t raw[3]; }; } DSV_DESC;
typedef struct { float x, y, w, h, min_depth, max_depth; } VIEWPORT;
typedef struct { int32_t left, top, right, bottom; } RECT12;
typedef struct { uint64_t va; uint32_t size, stride; } VBV;
typedef struct { uint64_t va; uint32_t size, format; } IBV;
typedef struct {
    int32_t type;
    union {
        struct { uint32_t slot; } vb;
        struct { uint32_t root, offset, count; } constants;
        struct { uint32_t root, offset; } incrementing_constant;
        struct { uint32_t root; } descriptor;
        uint32_t raw[3];
    };
} INDIRECT_ARGUMENT_DESC;
typedef struct { uint32_t stride, count; const INDIRECT_ARGUMENT_DESC *args; uint32_t node_mask; } COMMAND_SIGNATURE_DESC;
typedef struct { uint64_t Dest; uint32_t Value, padding; } WRITE_BUFFER_IMMEDIATE_PARAMETER12;

// ---- objects ----------------------------------------------------------------------------------------------------

typedef struct dev12 dev12;
typedef struct res12 res12;
typedef struct memheap12 memheap12;
static uint32_t d3d12_resource_plane_count(uint32_t format);
typedef struct {
    res12 *resource;
    uint64_t offset;
    uint32_t value, mode;
} write_buffer_immediate12;
typedef struct { d3d_object base; dev12 *dev; } obj12;
typedef struct {
    res12 *resource;
    uint32_t mip, mips, slice, slices, plane;
    res12 *counter_resource;
    uint64_t counter_offset;
} shader_view12;

typedef struct res_state_override12 {
    uint32_t subresource, state, promoted;
    struct res_state_override12 *next;
} res_state_override12;
typedef struct res_state_pending12 {
    uint32_t subresource, before, after;
    struct res_state_pending12 *next;
} res_state_pending12;

struct dev12 {
    d3d_object base;
    id<MTLDevice> mtl;
    NSMutableArray *resources;       // every live buffer and texture (made resident for each command buffer)
    pthread_mutex_t lock;
    volatile int nonidentity_srv;    // an SRV with a non-identity component mapping was made (the per-draw check of
                                     // comparison sampling over such views is skipped until then)
    pthread_mutex_t submit_lock;     // preserves queue submission and tracked state order across calling threads
    pthread_mutex_t alloc_lock;      // command lists' and allocators' recording state (Close, Reset, an allocator's
                                     // progress): not the submission lock, which a replay holds for its whole length
    pthread_mutex_t format_lock;
    uint32_t format_support1[256], format_support2[256];
    uint8_t format_support_valid[256];
    int typed_uav_load_additional_formats;
    id<MTLLibrary> format_probe_library;
    id<MTLFunction> format_probe_vertex, format_probe_fragment;
    NSMutableDictionary *pso_cache;  // MSL source -> id<MTLComputePipelineState>
    gpu_ir_metal_device *common_metal; // shared Metal resource and command backend for API-neutral operations
    id<MTLCommandQueue> cpu_transfer_queue; // synchronous opaque-layout resource access
    id<MTLComputePipelineState> indirect_args_filter; // applies ExecuteIndirect count-buffer limits on the GPU
    void **bufs;                     // the buffers (res12), for a GPU address's buffer (vertex and index buffers)
    uint32_t nbufs, capbufs;         // bufs: sorted by GPU address (bufs_find12)
    uint64_t maxbuflen;              // the longest buffer's length (how far back a lookup looks)
    pthread_rwlock_t bufs_lock;      // bufs: written with the device lock and this, read with either
    id<MTLComputePipelineState> occ_sum;   // adds a render pass's occlusion counts into a query heap (made when first needed)
    NSMutableArray *scratch_pool;    // scratch chunks no command buffer uses any more (scratch_lock)
    pthread_mutex_t scratch_lock;
    // macOS 15 and later: every resource's memory (placed resources: their heap's) in one residency set the device's
    // queues carry, instead of each encoder declaring every resource and descriptor-heap texture (2000 useResource
    // entries per render pass in a modest scene). Draws and dispatches still declare what their bindings name, which
    // is what Metal's hazard tracking follows. (lock)
    id residency;                    // id<MTLResidencySet>, or nil
    NSCountedSet *resident_allocations;
    int residency_dirty;
    // Allocations no resource names any more leave the set only once the GPU has finished what was submitted before
    // (their memory too stays until then): the set is the queues', so a removal committed while command buffers that
    // reach the allocation through argument buffers still run takes it from under them - a GPU fault Metal reports
    // as "Insufficient Memory". Each entry: the allocation and each queue's timeline value at the removal. (lock)
    NSMutableArray *residency_pending;
    void *queues[16];                // queue12, for their timelines (lock)
    int tiled_tier;                  // D3D12_TILED_RESOURCES_TIER: 2 with Metal placement sparse (macOS 26), else 0
    id tile_queue;                   // id<MTL4CommandQueue> carrying UpdateTileMappings (made when first needed)
};

struct qjob12;
typedef struct {
    d3d_object base;
    dev12 *dev;
    Q_DESC desc;
    id<MTLCommandQueue> mtl_queue;
    id<MTLSharedEvent> timeline;     // counts the executed command buffers the GPU has finished (submit_lock)
    uint64_t timeline_value;
    // VXRT_D3D12_ASYNC_SUBMIT: ExecuteCommandLists, Signal, Wait and Present go to this queue's own thread in order
    // (the replay into Metal off the program's thread); anything else on the queue waits for it to be idle first.
    int async;
    pthread_t worker;
    pthread_mutex_t jlock;
    pthread_cond_t jcond, jidle;
    struct qjob12 *jhead, *jtail;
    int jbusy, jstop;
    int replays;                     // (jlock) VXRT_D3D12_PARALLEL_REPLAY: lists being replayed on the pool
} queue12;
static void queue_drain12(queue12 *q);
typedef struct {
    d3d_object base;
    dev12 *dev;
    int32_t type;
    uint32_t recording_lists;
    uint32_t in_flight; // accessed atomically so completion callbacks never wait on submit_lock
    uint32_t queued;    // (atomic) lists of it submitted to a queue's thread and not yet replayed
    uint64_t generation;
    id<MTLSharedEvent> last_timeline;    // (submit_lock) where the newest list from this allocator went: its queue's
    uint64_t last_value;                 // timeline, and the value that queue reaches when the GPU has run it
} allocator12;

typedef struct {
    d3d_object base; dev12 *dev;
    id<MTLSharedEvent> event;
    MTLSharedEventListener *listener;
    uint64_t posted;                 // (diagnostics) the last value a queue Signal call asked for, and when it ran
    uint64_t posted_ran;
} fence12;

typedef struct {
    d3d_object base; dev12 *dev;
    HEAP_DESC desc;
    uint32_t residency_refs;
    uint32_t residency_priority;
    id<MTLBuffer> gpu;               // shader-visible heaps
    uint8_t *cpu;                    // the descriptors (gpu.contents, or memory of its own)
    __strong id *keep;               // the Metal object each descriptor stands for (texture view, sampler), held
    uint8_t *descriptor_kind;        // DXIL class + 1 for shader descriptors; zero means unset/non-shader descriptor
    gpu_ir_buffer_view **buffer_view; // API-neutral buffer view, retained alongside the descriptor
    shader_view12 *shader_view;      // API resource and subresource selection for SRV/UAV state validation
    res12 **target_resource;         // RTV/DSV descriptor owner, retained for API-state validation
    uint32_t *target_subresource;
    uint32_t *target_flags;
    // The distinct textures its descriptors name, counted as descriptors are written (texture_lock): what an encoder
    // makes resident for shaders that reach them through the heap. A game's heap holds 500 000 to 1 000 000
    // descriptors, so encoders must not walk it.
    pthread_mutex_t texture_lock;
    NSCountedSet *textures;
    NSArray *texture_list;           // textures as an array, rebuilt when it changed
} heap12;

struct res12 {
    d3d_object base; dev12 *dev;
    RES_DESC desc;
    HEAP_PROPS heap;
    id<MTLBuffer> buffer;
    gpu_ir_resource *gpu_buffer;   // API-neutral reference used by the common command IR
    id<MTLTexture> texture;
    gpu_ir_resource *gpu_texture;  // API-neutral reference used by the common command IR
    uint32_t mapped;
    uint32_t *mapped_subresources;
    uint32_t mapped_subresource_count;
    int32_t heap_flags;
    uint32_t residency_refs;
    uint32_t residency_priority;
    memheap12 *owner_heap;           // placed resources borrow the physical memory owned by this heap
    uint32_t initial_state;
    uint32_t initial_promoted;
    res_state_override12 *state_overrides;
    res_state_pending12 *pending_transitions;
    // Reserved (tiled) resources: Metal placement-sparse buffers and textures with 64 KiB pages, mapped onto the
    // Metal placement heaps of D3D12 heaps. Each tile's current mapping is kept for CopyTileMappings; a mapped heap
    // is held until its tiles are remapped or the resource is destroyed.
    uint8_t reserved;
    uint32_t tile_count;
    memheap12 **tile_heaps;
    uint32_t *tile_offsets;
};

// A heap (ID3D12Heap) for placed resources: a Metal placement heap, so resources placed at the same offset share
// memory as they do in D3D12.
struct memheap12 {
    d3d_object base; dev12 *dev;
    MEMHEAP_DESC desc;
    uint32_t residency_refs;
    uint32_t residency_priority;
    id<MTLHeap> heap;
};

// A query heap: occlusion queries count into a Metal visibility result buffer, timestamps are samples of the GPU's
// timestamp counter (in nanoseconds, as GetTimestampFrequency says); the other kinds read as zeros.
typedef struct {
    d3d_object base; dev12 *dev;
    int32_t type;                    // D3D12_QUERY_HEAP_TYPE: 0 occlusion, 1 timestamp, 2 pipeline statistics, 5 copy queue timestamp, ...
    uint32_t count, stride;          // queries, bytes of each one's result
    uint32_t residency_refs;
    uint32_t residency_priority;
    id<MTLBuffer> results;           // occlusion: Metal's visibility results; others: zeros
    id<MTLCounterSampleBuffer> samples;
} queryheap12;

static int resource_resident12(const res12 *resource) {
    if (!resource) return 0;
    const uint32_t *refs = resource->owner_heap ? &resource->owner_heap->residency_refs : &resource->residency_refs;
    return __atomic_load_n(refs, __ATOMIC_RELAXED) != 0;
}

static int descriptor_heap_resident12(const heap12 *heap) {
    return heap && __atomic_load_n(&heap->residency_refs, __ATOMIC_RELAXED) != 0;
}

enum { VX12_RESIDENCY_PRIORITY_NORMAL = 0x78000000u, VX12_RESIDENCY_PRIORITY_HIGH = 0xa0010000u };

// A root signature, from its serialized form.
typedef struct { int32_t type; uint32_t count, base, space, offset, flags; } range12;
typedef struct { int32_t type, visibility; uint32_t reg, space, n32, flags; uint32_t nranges; range12 *ranges; } param12;
typedef struct { uint32_t reg, space; int32_t visibility; id<MTLSamplerState> state; } static_sampler12;
typedef struct {
    d3d_object base; dev12 *dev;
    uint32_t nparams, nsamplers, flags;
    param12 *params;
    static_sampler12 *samplers;
    gpu_ir_pipeline_layout *gpu_layout;
    uint64_t blob_hash;              // of its serialized form (pipeline libraries match descriptions by content)
} rootsig12;

typedef struct { uint32_t type, payload_offset, size, a, b, c; } command_argument12;
typedef struct {
    d3d_object base; dev12 *dev;
    uint32_t stride, count, payload_size;
    command_argument12 *args;
    rootsig12 *root;
    int gfx;
} command_signature12;

typedef struct {
    d3d_object base; dev12 *dev;
    id<MTLComputePipelineState> compute;
    gpu_ir_pipeline *gpu_pipeline;
    int common_ir_compute;
    dxil_msl msl;                    // the translation's slots and thread counts (msl.msl freed)
    // graphics
    id<MTLRenderPipelineState> render;
    gpu_ir_shader *common_ir_vertex;
    gpu_ir_shader *common_ir_fragment;
    id<MTLDepthStencilState> depth;
    dxil_msl vs, ps;                 // their slots
    MTLCullMode cull;
    MTLWinding winding;
    MTLTriangleFillMode fill;
    MTLDepthClipMode clip;
    float bias, bias_slope, bias_clamp;
    int has_depth, has_stencil;
    int depth_enable, depth_write, stencil_enable, stencil_write;
    // A geometry shader (DXBC; Metal has no such stage): draws run the vertex shader as a kernel over their vertices,
    // the geometry shader as a kernel over its input primitives, then `render` (its pass-through vertex function and
    // the pixel shader) draws the triangles it made. The vertex kernels are compiled per index type and strides.
    id<MTLComputePipelineState> gs_kernel;
    NSString *gs_vs_source;
    NSMutableDictionary *gs_vs_kernels;
    dxil_msl gs;                     // its slots
    uint32_t gs_in_verts, gs_max_tris, gs_out_regs, gs_vs_out_regs, gs_nattr;
    struct { uint32_t reg, offset, slot, instanced, step; MTLVertexFormat format; } gs_attr[32];
    uint64_t desc_hash;              // of the description it was made from (pipeline libraries, cached blobs)
    uint64_t vs_code, ps_code, cs_code;  // hashes of the shaders' bytecode (VXRT_D3D12_PASS_DUMP names them; with
                                     // VXRT_D3D12_SHADER_DUMP the bytecode is written as <hash>.dxil)
    // Tessellation (DXIL): the vertex shader as a kernel (gs_vs_source, gs_attr: as for geometry), the hull shader as a
    // kernel per patch writing control points, patch constants and Metal's tessellation factors, and `render` drawing
    // the patches with the domain shader as its post-tessellation vertex function.
    id<MTLComputePipelineState> hs_kernel;
    dxil_msl hs, ds;                 // their slots
    uint32_t tess_in_cp, tess_out_cp, hs_cp_slots, hs_pc_slots, tess_domain;
} pso12;

typedef struct { d3d_object base; void *data; size_t size; } blob12;

// Recorded commands, replayed when the list is executed.
enum { C_PSO, C_ROOTSIG, C_HEAPS, C_TABLE, C_CONSTANTS, C_ROOT_VA, C_DISPATCH, C_COPY_RESOURCE, C_COPY_TEXTURE,
       C_TOPOLOGY, C_VB, C_IB, C_TARGETS, C_VIEWPORTS, C_SCISSORS, C_CLEAR_RTV, C_CLEAR_DSV, C_CLEAR_UAV, C_DRAW, C_DRAW_INDEXED, C_BLEND_FACTOR, C_STENCIL_REF,
       C_BEGIN_QUERY, C_END_QUERY, C_RESOLVE_QUERY, C_GPU_IR, C_EXECUTE_INDIRECT, C_BUNDLE_BEGIN, C_BUNDLE_END,
       C_WRITE_BUFFER_IMMEDIATE, C_DISCARD };
enum { D3D12_TRACK_COPY_BUFFER = 0x100, D3D12_TRACK_COPY_RESOURCE, D3D12_TRACK_COPY_TEXTURE, D3D12_TRACK_RESOLVE,
       D3D12_TRACK_RENDER_PASS_RESOLVE };   // (EndRenderPass resolves: the source stays a render target)
typedef struct {
    int op;
    uint32_t a, b, c;
    uint64_t va;
    void *obj, *obj2, *obj3;
    uint64_t off, off2, size;
    COPY_LOC dst, src;
    BOX box;
    int has_box;
    RECT12 *rects;
    write_buffer_immediate12 *immediate_writes;
    int gfx;                         // root signature and root arguments: the graphics ones (else compute)
    void *tex[9];                    // C_TARGETS, clears: the views (retained Metal textures), depth last
    res12 *resource[9];              // API resource named by a target descriptor, retained
    uint32_t subresource[9], view_flags[9];
    uint32_t values[64];
    uint32_t gpu_ir_index;
    int gpu_ir_recorded, gpu_ir_transient;
    uint32_t replay_source_index;    // original list index; UINT32_MAX for commands expanded during replay
} cmd12;

typedef struct {
    uint32_t command_index, available;
    const uint8_t *records;
} d3d12_indirect_validation_override12;

typedef struct {
    d3d_object base; dev12 *dev;
    int32_t type;
    allocator12 *allocator;
    uint64_t allocator_generation;
    int allocator_recording;
    cmd12 *cmds;
    uint32_t n, cap;
    pso12 *recording_pso;
    uint32_t recording_topology;
    int recording_topology_set;
    float recording_blend_factor[4];
    VBV recording_vertex_buffers[16];
    IBV recording_index_buffer;
    uint32_t pending;                // submissions not yet replayed by a queue's thread (atomic): Reset waits for them
    // the buffers vertex and index views found (base GPU address, length, the buffer, retained) while recording:
    // the device's registry, behind its lock, only on a miss (a recording list's buffers stay alive)
    struct { uint64_t base, len; gpu_ir_resource *buffer; } vb_cache[8];
    uint32_t vb_cache_next;
    gpu_ir_stream gpu_ir;
    int closed;
    int32_t validation_error;
    // BeginRenderPass: the resolves its ending accesses ask for, carried out by EndRenderPass.
    struct render_pass_resolve12 *render_pass_resolves;
    uint32_t render_pass_nresolves;
    int in_render_pass, render_pass_resolving;
} list12;

static int list_capture_draw_buffers12(list12 *list, gpu_ir_draw_info *draw);
static void list_release_captured_draw_buffers12(gpu_ir_draw_info *draw);

static d3d_object *obj(void *this) { return (d3d_object *)this; }

static HRESULT query(void *this, const GUID *iid, void **out, const GUID *const *known, int n) {
    if (!out) return E_POINTER;
    if (d3d_same_guid(iid, &IID_IUnknown_)) { d3d_addref(obj(this)); *out = this; return S_OK; }
    for (int i = 0; i < n; i++)
        if (d3d_same_guid(iid, known[i])) { d3d_addref(obj(this)); *out = this; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI uint32_t obj_addref(void *this) { return d3d_addref(obj(this)); }
static WINAPI uint32_t obj_release(void *this) { return d3d_release(obj(this)); }
static WINAPI HRESULT obj_set_name(void *this, const uint16_t *name) { return S_OK; }
static WINAPI HRESULT obj_get_private(void *this, const GUID *g, uint32_t *size, void *data) { return d3d_get_private(obj(this), g, size, data); }
static WINAPI HRESULT obj_set_private(void *this, const GUID *g, uint32_t size, const void *data) { return d3d_set_private(obj(this), g, size, data); }
static WINAPI HRESULT obj_set_private_iface(void *this, const GUID *g, void *unk) { return d3d_set_private(obj(this), g, sizeof unk, &unk); }
static WINAPI HRESULT child_get_device(void *this, const GUID *iid, void **out) {
    dev12 *d = ((obj12 *)this)->dev;
    const GUID *known[] = { &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4,
                            &IID_ID3D12Device5, &IID_ID3D12Device6, &IID_ID3D12Device7, &IID_ID3D12Device8, &IID_ID3D12Device9,
                            &IID_ID3D12Device10 };
    return query(d, iid, out, known, 11);
}

#define COMMON_METHODS \
    { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release }, { "SetName", (void *)obj_set_name }, \
    { "GetPrivateData", (void *)obj_get_private }, { "SetPrivateData", (void *)obj_set_private }, \
    { "SetPrivateDataInterface", (void *)obj_set_private_iface }, { "GetDevice", (void *)child_get_device }

static void *alloc12(size_t size, int kind, void **vtbl, dev12 *dev) {
    obj12 *o = d3d_alloc(size, kind, vtbl, NULL);
    o->dev = dev;
    if (dev) d3d_addref(&dev->base);
    return o;
}
static void release_dev(d3d_object *o) { dev12 *d = ((obj12 *)o)->dev; if (d) d3d_release(&d->base); }

static uint64_t fnv1a12(uint64_t h, const void *data, size_t size) {
    const uint8_t *p = data;
    for (size_t i = 0; p && i < size; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

// VXRT_D3D12_SHADER_DUMP=folder: each translated shader is written there before Metal compiles it (named by a hash
// of its source, so one Metal takes long over is on disk while it does); compiles over 2 s are logged in any case.
static uint64_t msl_dump12(const char *msl) {
    uint64_t h = fnv1a12(0xcbf29ce484222325ull, msl, strlen(msl));
    const char *dir = getenv("VXRT_D3D12_SHADER_DUMP");
    if (dir) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%016llx.metal", dir, (unsigned long long)h);
        FILE *f = fopen(path, "w");
        if (f) { fputs(msl, f); fclose(f); }
    }
    return h;
}
// VXRT_D3D12_MSL_OVERRIDE=<folder>: a shader whose <bytecode hash>.metal is there runs that source instead of its
// translation (same slots: an edited copy of the translation), to look into what a shader computes.
static void msl_override12(const BYTECODE *code, dxil_msl *out) {
    const char *dir = getenv("VXRT_D3D12_MSL_OVERRIDE");
    if (!dir || !code || !code->code) return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%016llx.metal", dir,
             (unsigned long long)fnv1a12(0xcbf29ce484222325ull, code->code, code->size));
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (text && fread(text, 1, (size_t)n, f) == (size_t)n) {
        text[n] = 0;
        free(out->msl);
        out->msl = text;
        vx_log("D3D12: shader %s replaced by the source in the override folder", path);
    } else free(text);
    fclose(f);
}
static uint64_t code_dump12(const void *code, size_t size) {
    if (!code || !size) return 0;
    uint64_t h = fnv1a12(0xcbf29ce484222325ull, code, size);
    const char *dir = getenv("VXRT_D3D12_SHADER_DUMP");
    if (dir) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%016llx.dxil", dir, (unsigned long long)h);
        if (access(path, F_OK)) { FILE *f = fopen(path, "wb"); if (f) { fwrite(code, 1, size, f); fclose(f); } }
    }
    return h;
}
static double msl_clock12(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static void msl_timed12(const char *what, uint64_t hash, double start) {
    double t = msl_clock12() - start;
    if (t > 2.0) vx_log("D3D12: %s %016llx took %.1f s to compile", what, (unsigned long long)hash, t);
}

// VXRT_D3D12_FRAME_STATS=<ms> (default 50 when set to 1): each present that came more than <ms> after the last is
// logged with what the runtime did in between (every thread's): pipeline states made and the time in them, shader
// translation and Metal compiles, ExecuteCommandLists (and the time waiting for the submission lock), CPU fence
// waits, the wait for a drawable, and the draws and dispatches skipped. Every 300 presents a summary of the same.
typedef struct {
    uint64_t pso_n, pso_ns, compile_n, compile_ns, exec_n, exec_ns, lock_ns, wait_ns, drawable_ns, skipped, sync_n, sync_ns, gpu_ns;
    uint64_t replay_ns;              // (VXRT_D3D12_ASYNC_SUBMIT: the queue threads' replay)
    uint64_t busy_ns, replay_max_ns; // the GPU's busy time (command buffers' spans merged), the longest list replay
} frame_stats12;
static frame_stats12 g_fs12, g_fs12_sum;
static double frame_stats_ms12(void) {
    static double ms = -1;
    if (ms < 0) { const char *e = getenv("VXRT_D3D12_FRAME_STATS"); ms = !e ? 0 : atof(e) > 1 ? atof(e) : 50; }
    return ms;
}
// What a command buffer ran (pipeline bytecode hashes, in order), for the log when it fails.
static __thread char t_cb_log12[1024];
static __thread int t_cb_len12;
static void cb_log_reset12(void) { t_cb_log12[0] = 0; t_cb_len12 = 0; }
// VXRT_D3D12_SKIP=<file>: draws and dispatches whose shader (pixel shader for draws, compute shader for dispatches)
// is listed in <file> (the 8 hex digits the logs print, one per line; reread at each present when it changes) are
// left out: what a pass costs the frame, measured in one session by editing the file (the picture breaks). Lines *p,
// *c, *i, *I, *g leave out every draw, dispatch, indirect dispatch, indirect draw, copy.
static volatile uint32_t g_skip12[64];
static volatile int g_skip_count12;
static volatile int g_census12;
static volatile int g_no_dispatch_barrier12;   // (a line "!dbarrier": no memory barrier after each dispatch, to compare)               // (a line "?" in the file: presents left to count shaders' uses)
static const char *skip_path12(void) {
    static const char *p = (const char *)-1;
    if (p == (const char *)-1) { p = getenv("VXRT_D3D12_SKIP"); if (p && !*p) p = NULL; }
    return p;
}
static void skip_poll12(void) {
    const char *p = skip_path12();
    if (!p) return;
    static struct timespec seen;
    struct stat st;
    if (stat(p, &st)) { g_skip_count12 = 0; g_no_dispatch_barrier12 = 0; seen.tv_sec = 0; return; }
    if (st.st_mtimespec.tv_sec == seen.tv_sec && st.st_mtimespec.tv_nsec == seen.tv_nsec) return;
    seen = st.st_mtimespec;
    FILE *f = fopen(p, "r");
    int n = 0, nodbar = 0;
    char line[128];
    while (f && n < 64 && fgets(line, sizeof line, f)) {
        char *end;
        if (line[0] == '?') { if (!g_census12) g_census12 = 31; continue; }
        if (!strncmp(line, "!dbarrier", 9)) { nodbar = 1; continue; }
        if (line[0] == '*') { g_skip12[n++] = 0x80000000u | (uint8_t)line[1]; continue; }   // (*p every draw, *c every dispatch, *i every indirect one, *I indirect draws, *g copies)
        unsigned long v = strtoul(line, &end, 16);
        if (end != line) g_skip12[n++] = (uint32_t)v;
    }
    if (f) fclose(f);
    g_skip_count12 = n;
    g_no_dispatch_barrier12 = nodbar;
    vx_log("D3D12: %d shaders skipped (VXRT_D3D12_SKIP)", n);
}
// (a line "?": the next 30 presents' draws and dispatches are counted per shader and logged, most first)
static struct { uint64_t hash; char kind; volatile uint32_t n; } g_census_tab12[1024];
static void census12(uint64_t hash, char kind) {
    uint32_t i = (uint32_t)(hash ^ (hash >> 29) ^ (uint64_t)kind) & 1023;
    for (int probe = 0; probe < 1024; probe++, i = (i + 1) & 1023) {
        if (g_census_tab12[i].hash == hash && g_census_tab12[i].kind == kind) { __atomic_add_fetch(&g_census_tab12[i].n, 1, __ATOMIC_RELAXED); return; }
        if (!g_census_tab12[i].kind && __sync_bool_compare_and_swap(&g_census_tab12[i].kind, 0, kind)) {
            g_census_tab12[i].hash = hash;
            __atomic_add_fetch(&g_census_tab12[i].n, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}
static void census_present12(void) {
    if (!g_census12 || --g_census12) return;
    int order[1024], m = 0;
    for (int i = 0; i < 1024; i++) if (g_census_tab12[i].kind) order[m++] = i;
    for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++) if (g_census_tab12[order[j]].n > g_census_tab12[order[i]].n) { int t = order[i]; order[i] = order[j]; order[j] = t; }
    vx_log("D3D12: census: %d shaders over 30 presents", m);
    for (int i = 0; i < m && i < 200; i++)
        vx_log("D3D12: census: %c%08llx %u", g_census_tab12[order[i]].kind, (unsigned long long)(g_census_tab12[order[i]].hash >> 32), g_census_tab12[order[i]].n);
    memset(g_census_tab12, 0, sizeof g_census_tab12);
}
static int skipped_shader12(uint64_t hash, char kind) {
    if (g_census12) census12(hash, kind);
    int n = g_skip_count12;
    for (int i = 0; i < n; i++) if (g_skip12[i] == (uint32_t)(hash >> 32) || g_skip12[i] == (0x80000000u | (uint8_t)kind)) return 1;
    return 0;
}
static void prof_note12(char kind, uint64_t hash);
static void cb_log12(char kind, uint64_t hash, uint32_t x, uint32_t y, uint32_t z) {
    prof_note12(kind, hash);
    if (t_cb_len12 > (int)sizeof t_cb_log12 - 48) return;
    t_cb_len12 += snprintf(t_cb_log12 + t_cb_len12, sizeof t_cb_log12 - t_cb_len12, "%c%08llx", kind, (unsigned long long)(hash >> 32));
    if (x) t_cb_len12 += snprintf(t_cb_log12 + t_cb_len12, sizeof t_cb_log12 - t_cb_len12, "(%u,%u,%u)", x, y, z);
    t_cb_len12 += snprintf(t_cb_log12 + t_cb_len12, sizeof t_cb_log12 - t_cb_len12, " ");
}
static uint64_t fs_now12(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
#define FS_ADD12(field, v) do { if (frame_stats_ms12() > 0) __atomic_fetch_add(&g_fs12.field, (uint64_t)(v), __ATOMIC_RELAXED); } while (0)
static void frame_stats_present12(uint32_t presents) {
    double limit = frame_stats_ms12();
    if (limit <= 0) return;
    static uint64_t last;
    uint64_t now = fs_now12(), gap = last ? now - last : 0;
    last = now;
    frame_stats12 f;
    #define TAKE12(x) f.x = __atomic_exchange_n(&g_fs12.x, 0, __ATOMIC_ACQ_REL); __atomic_fetch_add(&g_fs12_sum.x, f.x, __ATOMIC_RELAXED)
    TAKE12(pso_n); TAKE12(pso_ns); TAKE12(compile_n); TAKE12(compile_ns); TAKE12(exec_n); TAKE12(exec_ns);
    TAKE12(lock_ns); TAKE12(wait_ns); TAKE12(drawable_ns); TAKE12(skipped); TAKE12(sync_n); TAKE12(sync_ns); TAKE12(gpu_ns); TAKE12(replay_ns); TAKE12(busy_ns); TAKE12(replay_max_ns);
    #undef TAKE12
    if (gap / 1e6 > limit)
        vx_log("D3D12: frame %u took %.1f ms: %llu pipeline states (%.1f ms), %llu compiles (%.1f ms), %llu executes (%.1f ms, "
               "%.1f ms of it for the submission lock), GPU %.1f ms, fence waits %.1f ms, drawable %.1f ms, %llu draws skipped, %llu GPU round trips (%.1f ms), replay on queue threads %.1f ms (longest list %.1f), GPU busy %.1f ms",
               presents, gap / 1e6, (unsigned long long)f.pso_n, f.pso_ns / 1e6, (unsigned long long)f.compile_n, f.compile_ns / 1e6,
               (unsigned long long)f.exec_n, f.exec_ns / 1e6, f.lock_ns / 1e6, f.gpu_ns / 1e6, f.wait_ns / 1e6, f.drawable_ns / 1e6,
               (unsigned long long)f.skipped, (unsigned long long)f.sync_n, f.sync_ns / 1e6, f.replay_ns / 1e6,
               f.replay_max_ns / 1e6, f.busy_ns / 1e6);
    static uint64_t window_start;
    if (!window_start) window_start = now;
    if (presents % 300 == 0) {
        frame_stats12 t;
        #define SUM12(x) t.x = __atomic_exchange_n(&g_fs12_sum.x, 0, __ATOMIC_ACQ_REL)
        SUM12(pso_n); SUM12(pso_ns); SUM12(compile_n); SUM12(compile_ns); SUM12(exec_n); SUM12(exec_ns);
        SUM12(lock_ns); SUM12(wait_ns); SUM12(drawable_ns); SUM12(skipped); SUM12(sync_n); SUM12(sync_ns); SUM12(gpu_ns);
        #undef SUM12
        double span = (now - window_start) / 1e6;
        window_start = now;
        vx_log("D3D12: last 300 presents in %.0f ms (%.1f fps): %llu pipeline states (%.0f ms), %llu compiles (%.0f ms), "
               "%llu executes (%.0f ms, lock %.0f ms), GPU %.0f ms, fence waits %.0f ms, drawable %.0f ms, %llu draws skipped, %llu GPU round trips (%.0f ms)",
               span, span > 0 ? 300000.0 / span : 0, (unsigned long long)t.pso_n, t.pso_ns / 1e6, (unsigned long long)t.compile_n,
               t.compile_ns / 1e6, (unsigned long long)t.exec_n, t.exec_ns / 1e6, t.lock_ns / 1e6, t.gpu_ns / 1e6, t.wait_ns / 1e6,
               t.drawable_ns / 1e6, (unsigned long long)t.skipped, (unsigned long long)t.sync_n, t.sync_ns / 1e6);
    }
}

// Metal's compiler can take without end over a very large shader (a 34 000-line state machine in a game's upscaler
// kept MTLCompilerService busy for over 10 minutes): libraries and compute pipelines are compiled asynchronously and
// given up after VXRT_D3D12_COMPILE_TIMEOUT seconds (default 30), the pipeline creation then failing as on a driver
// that cannot make it. (Metal finishes such a compile in the background.)
static double compile_timeout12(void) {
    static double t = -1;
    if (t < 0) { const char *e = getenv("VXRT_D3D12_COMPILE_TIMEOUT"); t = e && atof(e) > 0 ? atof(e) : 30.0; }
    return t;
}
static id<MTLLibrary> library_with_timeout12_(id<MTLDevice> mtl, NSString *source, MTLCompileOptions *options, NSError **error);
static id<MTLLibrary> library_with_timeout12(id<MTLDevice> mtl, NSString *source, MTLCompileOptions *options, NSError **error) {
    uint64_t t0 = fs_now12();
    id<MTLLibrary> lib = library_with_timeout12_(mtl, source, options, error);
    FS_ADD12(compile_n, 1); FS_ADD12(compile_ns, fs_now12() - t0);
    return lib;
}
static id<MTLLibrary> library_with_timeout12_(id<MTLDevice> mtl, NSString *source, MTLCompileOptions *options, NSError **error) {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block id<MTLLibrary> result = nil;
    __block NSError *failure = nil;
    __block int finished = 0;
    NSObject *lock = [NSObject new];
    [mtl newLibraryWithSource:source options:options completionHandler:^(id<MTLLibrary> library, NSError *e) {
        @synchronized (lock) { result = library; failure = e; finished = 1; }
        dispatch_semaphore_signal(done);
    }];
    long late = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(compile_timeout12() * 1e9)));
    @synchronized (lock) {
        if (late || !finished) { if (error) *error = [NSError errorWithDomain:@"VXRuntime" code:1 userInfo:@{ NSLocalizedDescriptionKey: @"Metal took too long to compile it" }]; return nil; }
        if (error) *error = failure;
        return result;
    }
}
static id<MTLComputePipelineState> compute_pipeline_with_timeout12(id<MTLDevice> mtl, id<MTLFunction> fn, NSError **error) {
    if (!fn) return nil;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block id<MTLComputePipelineState> result = nil;
    __block NSError *failure = nil;
    __block int finished = 0;
    NSObject *lock = [NSObject new];
    [mtl newComputePipelineStateWithFunction:fn completionHandler:^(id<MTLComputePipelineState> state, NSError *e) {
        @synchronized (lock) { result = state; failure = e; finished = 1; }
        dispatch_semaphore_signal(done);
    }];
    long late = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(compile_timeout12() * 1e9)));
    @synchronized (lock) {
        if (late || !finished) { if (error) *error = [NSError errorWithDomain:@"VXRuntime" code:1 userInfo:@{ NSLocalizedDescriptionKey: @"Metal took too long to compile it" }]; return nil; }
        if (error) *error = failure;
        return result;
    }
}

// ---- blobs (ID3DBlob) ---------------------------------------------------------------------------------------------

static WINAPI HRESULT blob_query(void *this, const GUID *iid, void **out) { const GUID *k[] = { &IID_ID3D10Blob }; return query(this, iid, out, k, 1); }
static WINAPI void *blob_pointer(void *this) { return ((blob12 *)this)->data; }
static WINAPI size_t blob_size(void *this) { return ((blob12 *)this)->size; }
static void blob_destroy(d3d_object *o) { free(((blob12 *)o)->data); }
static blob12 *blob_new(const void *data, size_t size) {
    pthread_once(&g_tables_once, make_tables);
    blob12 *b = d3d_alloc(sizeof *b, K12_BLOB, g_blob_vtbl, NULL);
    b->data = malloc(size ? size : 1);
    memcpy(b->data, data, size);
    b->size = size;
    b->base.destroy = blob_destroy;
    return b;
}

// ---- root signatures: the serialized form (RTS0) -----------------------------------------------------------------

// D3D12_ROOT_PARAMETER(1) and friends, as the serializer receives them.
typedef struct { int32_t type; uint32_t count, base, space, offset; } RANGE10;
typedef struct { int32_t type; uint32_t count, base, space; int32_t flags; uint32_t offset; } RANGE11;
typedef struct { int32_t type; uint32_t pad; union { struct { uint32_t n; uint32_t pad2; const void *ranges; } table; struct { uint32_t reg, space, n32; } constants; struct { uint32_t reg, space; int32_t flags; } desc; }; int32_t visibility; uint32_t pad3; } PARAM;
typedef struct { int32_t filter, u, v, w; float bias; uint32_t aniso; int32_t cmp, border; float min_lod, max_lod; uint32_t reg, space; int32_t visibility; } STATIC_SAMPLER;
typedef struct { STATIC_SAMPLER sampler; uint32_t flags; } STATIC_SAMPLER1;
_Static_assert(sizeof(STATIC_SAMPLER) == 52, "RTS0 v1 static sampler layout");
_Static_assert(sizeof(STATIC_SAMPLER1) == 56, "RTS0 v3 static sampler layout");
typedef struct { uint32_t nparams; uint32_t pad; const PARAM *params; uint32_t nsamplers; uint32_t pad2; const STATIC_SAMPLER *samplers; int32_t flags; } RS_DESC;
typedef struct { int32_t version; uint32_t pad; RS_DESC desc; } VERSIONED_RS_DESC;

typedef struct { uint8_t *p; size_t n, cap; } bytes;
static void b_u32(bytes *b, uint32_t v) {
    if (b->n + 4 > b->cap) { b->cap = (b->n + 4) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, &v, 4);
    b->n += 4;
}
static void b_set(bytes *b, size_t at, uint32_t v) { memcpy(b->p + at, &v, 4); }

// Writes RTS0 (version 1: 1.0, 2: 1.1, 3: 1.2) in a DXBC container with one chunk.
static blob12 *serialize_rootsig(int version, const RS_DESC *d) {
    bytes r = { 0 };
    b_u32(&r, (uint32_t)version);
    b_u32(&r, d->nparams);
    b_u32(&r, 24);
    b_u32(&r, d->nsamplers);
    b_u32(&r, 0);                                                  // (samplers offset, filled below)
    b_u32(&r, (uint32_t)d->flags);
    size_t params_at = r.n;
    for (uint32_t i = 0; i < d->nparams; i++) { b_u32(&r, 0); b_u32(&r, 0); b_u32(&r, 0); }
    for (uint32_t i = 0; i < d->nparams; i++) {
        const PARAM *p = &d->params[i];
        b_set(&r, params_at + 12 * i, (uint32_t)p->type);
        b_set(&r, params_at + 12 * i + 4, (uint32_t)p->visibility);
        b_set(&r, params_at + 12 * i + 8, (uint32_t)r.n);
        if (p->type == 0) {
            b_u32(&r, p->table.n);
            b_u32(&r, (uint32_t)r.n + 4);
            for (uint32_t k = 0; k < p->table.n; k++) {
                if (version == 1) {
                    const RANGE10 *g = &((const RANGE10 *)p->table.ranges)[k];
                    b_u32(&r, (uint32_t)g->type); b_u32(&r, g->count); b_u32(&r, g->base); b_u32(&r, g->space); b_u32(&r, g->offset);
                } else {
                    const RANGE11 *g = &((const RANGE11 *)p->table.ranges)[k];
                    b_u32(&r, (uint32_t)g->type); b_u32(&r, g->count); b_u32(&r, g->base); b_u32(&r, g->space); b_u32(&r, (uint32_t)g->flags); b_u32(&r, g->offset);
                }
            }
        } else if (p->type == 1) {
            b_u32(&r, p->constants.reg); b_u32(&r, p->constants.space); b_u32(&r, p->constants.n32);
        } else {
            b_u32(&r, p->desc.reg); b_u32(&r, p->desc.space);
            if (version != 1) b_u32(&r, (uint32_t)p->desc.flags);
        }
    }
    b_set(&r, 16, d->nsamplers ? (uint32_t)r.n : 0);
    for (uint32_t i = 0; i < d->nsamplers; i++) {
        uint32_t fields[14] = { 0 };
        size_t sampler_size = version == 3 ? sizeof(STATIC_SAMPLER1) : sizeof(STATIC_SAMPLER);
        const void *sampler = version == 3 ? (const void *)&((const STATIC_SAMPLER1 *)d->samplers)[i] :
                                                  (const void *)&d->samplers[i];
        memcpy(fields, sampler, sampler_size);
        for (size_t k = 0; k < sampler_size / sizeof fields[0]; k++) b_u32(&r, fields[k]);
    }
    // the container: DXBC, a hash (not checked by this runtime), version 1, size, one chunk "RTS0"
    bytes c = { 0 };
    b_u32(&c, 0x43425844);
    for (int i = 0; i < 4; i++) b_u32(&c, 0);
    b_u32(&c, 1);
    b_u32(&c, 32 + 4 + 8 + (uint32_t)r.n);
    b_u32(&c, 1);
    b_u32(&c, 36);
    b_u32(&c, 0x30535452);                                         // "RTS0"
    b_u32(&c, (uint32_t)r.n);
    for (size_t i = 0; i < r.n; i += 4) { uint32_t v; memcpy(&v, r.p + i, 4); b_u32(&c, v); }
    blob12 *b = blob_new(c.p, c.n);
    free(r.p);
    free(c.p);
    return b;
}

// The RTS0 part of a container (a serialized root signature, or a shader that carries one), or the bytes as they are.
static int span_ok(size_t total, size_t offset, size_t size) {
    return offset <= total && size <= total - offset;
}

static const uint8_t *rts0(const uint8_t *p, size_t size, size_t *len) {
    if (!p || !len) return NULL;
    if (size >= 4 && !memcmp(p, "DXBC", 4)) {
        uint32_t count, container_size;
        if (!span_ok(size, 0, 32)) return NULL;
        memcpy(&container_size, p + 24, 4);
        memcpy(&count, p + 28, 4);
        if (container_size < 32 || container_size > size || count > (container_size - 32) / 4 ||
            !span_ok(container_size, 32, (size_t)count * 4)) return NULL;
        size_t chunks = 32 + (size_t)count * 4;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t off;
            memcpy(&off, p + 32 + 4 * i, 4);
            if (off < chunks || !span_ok(container_size, off, 8)) return NULL;
            if (memcmp(p + off, "RTS0", 4)) {
                uint32_t chunk_size;
                memcpy(&chunk_size, p + off + 4, 4);
                if (!span_ok(container_size, (size_t)off + 8, chunk_size)) return NULL;
                continue;
            }
            uint32_t n;
            memcpy(&n, p + off + 4, 4);
            if (!span_ok(container_size, (size_t)off + 8, n)) return NULL;
            *len = n;
            return p + off + 8;
        }
        return NULL;
    }
    *len = size;
    return p;
}

static uint32_t rd32(const uint8_t *p, size_t len, size_t at) { uint32_t v = 0; if (at + 4 <= len) memcpy(&v, p + at, 4); return v; }

static int valid_descriptor_range_flags12(uint32_t flags) {
    switch (flags) {
    case 0: case 0x1: case 0x2: case 0x4: case 0x8: case 0x3: case 0x5: return 1;
    default: return 0;
    }
}

static int valid_root_descriptor_flags12(uint32_t flags) {
    return flags == 0 || flags == 0x2 || flags == 0x4 || flags == 0x8;
}

static id<MTLSamplerState> make_sampler(dev12 *d, int32_t filter, int32_t u, int32_t v, int32_t w, float bias,
    uint32_t aniso, int32_t cmp, const float border[4], float min_lod, float max_lod, int normalized_coordinates);

static void rootsig_destroy(d3d_object *o) {
    rootsig12 *r = (rootsig12 *)o;
    if (r->params) for (uint32_t i = 0; i < r->nparams; i++) free(r->params[i].ranges);
    free(r->params);
    gpu_ir_pipeline_layout_release(r->gpu_layout);
    if (r->samplers) for (uint32_t i = 0; i < r->nsamplers; i++) r->samplers[i].state = nil;
    free(r->samplers);
    release_dev(o);
}

typedef struct { uint32_t type, space, base, count; int32_t visibility; } root_binding12;

static uint64_t root_binding_end(const root_binding12 *binding) {
    return binding->count == UINT32_MAX ? UINT64_MAX : (uint64_t)binding->base + binding->count;
}

static int root_binding_compare(const void *left, const void *right) {
    const root_binding12 *a = left, *b = right;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    if (a->space != b->space) return a->space < b->space ? -1 : 1;
    if (a->base != b->base) return a->base < b->base ? -1 : 1;
    if (a->visibility != b->visibility) return a->visibility < b->visibility ? -1 : 1;
    return 0;
}

static int range_offset_compare(const void *left, const void *right) {
    const range12 *a = left, *b = right;
    if (a->offset != b->offset) return a->offset < b->offset ? -1 : 1;
    if (a->type != b->type) return a->type < b->type ? -1 : 1;
    return 0;
}

static HRESULT validate_table_ranges(const param12 *param) {
    int has_sampler = 0, has_resource = 0;
    for (uint32_t i = 0; i < param->nranges; i++) {
        if (param->ranges[i].type == 3) has_sampler = 1;
        else has_resource = 1;
    }
    if (has_sampler && has_resource) return E_INVALIDARG;
    if (param->nranges < 2) return S_OK;
    if ((size_t)param->nranges > SIZE_MAX / sizeof *param->ranges) return E_OUTOFMEMORY;
    range12 *sorted = malloc((size_t)param->nranges * sizeof *sorted);
    if (!sorted) return E_OUTOFMEMORY;
    memcpy(sorted, param->ranges, (size_t)param->nranges * sizeof *sorted);
    qsort(sorted, param->nranges, sizeof *sorted, range_offset_compare);
    uint64_t end_by_type[4] = { 0 };
    for (uint32_t i = 0; i < param->nranges; i++) {
        range12 *range = &sorted[i];
        for (uint32_t type = 0; type < 4; type++) {
            if (type != (uint32_t)range->type && end_by_type[type] > range->offset) {
                free(sorted);
                return E_INVALIDARG;
            }
        }
        uint64_t end = range->count == UINT32_MAX ? UINT64_MAX : (uint64_t)range->offset + range->count;
        if (end > end_by_type[range->type]) end_by_type[range->type] = end;
    }
    free(sorted);
    return S_OK;
}

static HRESULT validate_root_bindings(const rootsig12 *root) {
    size_t count = root->nsamplers;
    for (uint32_t i = 0; i < root->nparams; i++) {
        const param12 *param = &root->params[i];
        size_t add = param->type == 0 ? param->nranges : 1;
        if (add > SIZE_MAX - count) return E_OUTOFMEMORY;
        count += add;
    }
    if (!count) return S_OK;
    if (count > SIZE_MAX / sizeof(root_binding12)) return E_OUTOFMEMORY;
    root_binding12 *bindings = malloc(count * sizeof *bindings);
    if (!bindings) return E_OUTOFMEMORY;
    size_t at = 0;
    for (uint32_t i = 0; i < root->nparams; i++) {
        const param12 *param = &root->params[i];
        if (param->type == 0) {
            for (uint32_t j = 0; j < param->nranges; j++) {
                const range12 *range = &param->ranges[j];
                bindings[at++] = (root_binding12){ (uint32_t)range->type, range->space, range->base,
                                                   range->count, param->visibility };
            }
        } else {
            uint32_t type = param->type == 2 ? 2 : param->type == 3 ? 0 : param->type == 4 ? 1 : 2;
            bindings[at++] = (root_binding12){ type, param->space, param->reg, 1, param->visibility };
        }
    }
    for (uint32_t i = 0; i < root->nsamplers; i++) {
        const static_sampler12 *sampler = &root->samplers[i];
        bindings[at++] = (root_binding12){ 3, sampler->space, sampler->reg, 1, sampler->visibility };
    }
    qsort(bindings, count, sizeof *bindings, root_binding_compare);
    uint32_t previous_type = UINT32_MAX, previous_space = UINT32_MAX;
    uint64_t end_by_visibility[8] = { 0 };
    uint8_t seen_visibility[8] = { 0 };
    for (size_t i = 0; i < count; i++) {
        const root_binding12 *binding = &bindings[i];
        if (binding->type != previous_type || binding->space != previous_space) {
            memset(end_by_visibility, 0, sizeof end_by_visibility);
            memset(seen_visibility, 0, sizeof seen_visibility);
            previous_type = binding->type;
            previous_space = binding->space;
        }
        for (uint32_t visibility = 0; visibility < 8; visibility++) {
            if (!seen_visibility[visibility] ||
                (binding->visibility && visibility && binding->visibility != (int32_t)visibility)) continue;
            if (end_by_visibility[visibility] > binding->base) { free(bindings); return E_INVALIDARG; }
        }
        uint32_t visibility = (uint32_t)binding->visibility;
        uint64_t end = root_binding_end(binding);
        if (end > end_by_visibility[visibility]) end_by_visibility[visibility] = end;
        seen_visibility[visibility] = 1;
    }
    free(bindings);
    return S_OK;
}

static uint32_t root_visibility_gpu_stages12(int32_t visibility) {
    switch (visibility) {
    case 0: return GPU_IR_STAGE_ALL;
    case 1: return GPU_IR_STAGE_VERTEX;
    case 2: return GPU_IR_STAGE_TESS_CONTROL;
    case 3: return GPU_IR_STAGE_TESS_EVALUATION;
    case 4: return GPU_IR_STAGE_GEOMETRY;
    case 5: return GPU_IR_STAGE_FRAGMENT;
    case 6: return GPU_IR_STAGE_MESH;
    case 7: return GPU_IR_STAGE_TASK;
    default: return 0;
    }
}

static uint32_t root_gpu_kind12(uint32_t register_class) {
    switch (register_class) {
    case 0: return GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE;
    case 1: return GPU_IR_DESCRIPTOR_STORAGE_RESOURCE;
    case 2: return GPU_IR_DESCRIPTOR_UNIFORM_BUFFER;
    case 3: return GPU_IR_DESCRIPTOR_SAMPLER;
    default: return 0;
    }
}

static uint32_t root_gpu_access12(uint32_t register_class) {
    switch (register_class) {
    case 0: return GPU_IR_ACCESS_SHADER_READ;
    case 1: return GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE;
    case 2: return GPU_IR_ACCESS_UNIFORM_READ;
    case 3: return 0;
    default: return UINT32_MAX;
    }
}

static void root_gpu_binding12(gpu_ir_descriptor_binding *out, uint32_t register_class, uint32_t count,
                               uint32_t reg, uint32_t space, int32_t visibility) {
    *out = (gpu_ir_descriptor_binding){
        .set = space, .binding = reg, .kind = root_gpu_kind12(register_class), .count = count,
        .stage_mask = root_visibility_gpu_stages12(visibility), .access_mask = root_gpu_access12(register_class),
        .binding_namespace = register_class
    };
}

static HRESULT create_root_gpu_layout12(rootsig12 *root) {
    size_t count = root->nsamplers;
    for (uint32_t i = 0; i < root->nparams; i++) {
        const param12 *param = &root->params[i];
        size_t add = param->type == 0 ? param->nranges : (param->type >= 2 && param->type <= 4 ? 1 : 0);
        if (add > SIZE_MAX - count) return E_OUTOFMEMORY;
        count += add;
    }
    if (count > UINT32_MAX || count > SIZE_MAX / sizeof(gpu_ir_descriptor_binding)) return E_OUTOFMEMORY;
    gpu_ir_descriptor_binding *bindings = count ? calloc(count, sizeof *bindings) : NULL;
    if (count && !bindings) return E_OUTOFMEMORY;
    size_t at = 0;
    for (uint32_t i = 0; i < root->nparams; i++) {
        const param12 *param = &root->params[i];
        if (param->type == 0) {
            for (uint32_t j = 0; j < param->nranges; j++) {
                const range12 *range = &param->ranges[j];
                root_gpu_binding12(&bindings[at++], (uint32_t)range->type, range->count,
                                   range->base, range->space, param->visibility);
            }
        } else if (param->type >= 2 && param->type <= 4) {
            uint32_t register_class = param->type == 2 ? 2 : param->type == 3 ? 0 : 1;
            root_gpu_binding12(&bindings[at++], register_class, 1, param->reg, param->space, param->visibility);
        }
    }
    for (uint32_t i = 0; i < root->nsamplers; i++) {
        const static_sampler12 *sampler = &root->samplers[i];
        root_gpu_binding12(&bindings[at++], 3, 1, sampler->reg, sampler->space, sampler->visibility);
    }
    gpu_ir_pipeline_layout_desc desc = { .bindings = bindings, .binding_count = (uint32_t)count };
    root->gpu_layout = gpu_ir_pipeline_layout_create(&desc);
    free(bindings);
    return root->gpu_layout ? S_OK : E_INVALIDARG;
}

static HRESULT parse_rootsig(dev12 *d, const void *data, size_t size, rootsig12 *r) {
    size_t len = 0;
    const uint8_t *p = rts0(data, size, &len);
    if (!p || len < 24) return E_INVALIDARG;
    uint32_t version = rd32(p, len, 0);
    uint32_t params_at = rd32(p, len, 8);
    uint32_t nparams = rd32(p, len, 4), nsamplers = rd32(p, len, 12);
    uint32_t samplers_at = rd32(p, len, 16);
    r->flags = rd32(p, len, 20);
    // D3D12_ROOT_SIGNATURE_FLAGS currently occupies bits 0 through 11. A local root signature is
    // raytracing-only and cannot be combined with graphics root-signature flags.
    if ((version < 1 || version > 3) || (r->flags & ~0xFFFu) ||
        ((r->flags & 0x80u) && r->flags != 0x80u) || nparams > 64 || nsamplers > 2048 || params_at < 24 ||
        !span_ok(len, params_at, (size_t)nparams * 12) ||
        (nsamplers && (samplers_at < 24 ||
         !span_ok(len, samplers_at, (size_t)nsamplers * (version == 3 ? 56 : 52))))) return E_INVALIDARG;
    r->params = calloc((size_t)nparams + 1, sizeof *r->params);
    if (!r->params) return E_OUTOFMEMORY;
    r->nparams = nparams;
    uint32_t root_cost = 0;
    for (uint32_t i = 0; i < nparams; i++) {
        param12 *q = &r->params[i];
        size_t at = (size_t)params_at + 12 * (size_t)i;
        q->type = (int32_t)rd32(p, len, at);
        q->visibility = (int32_t)rd32(p, len, at + 4);
        uint32_t payload = rd32(p, len, at + 8);
        if (q->visibility < 0 || q->visibility > 7) return E_INVALIDARG;
        if (q->type == 0) {
            if (root_cost == 64) return E_INVALIDARG;
            root_cost++;
            if (!span_ok(len, payload, 8)) return E_INVALIDARG;
            q->nranges = rd32(p, len, payload);
            uint32_t ranges_at = rd32(p, len, payload + 4);
            uint32_t stride = version == 1 ? 20 : 24;
            if (q->nranges == UINT32_MAX || ranges_at < 24 ||
                !span_ok(len, ranges_at, (size_t)q->nranges * stride)) return E_INVALIDARG;
            q->ranges = calloc((size_t)q->nranges + 1, sizeof *q->ranges);
            if (!q->ranges) return E_OUTOFMEMORY;
            uint32_t running = 0;
            int append_valid = 1;
            int table_has_sampler = 0, table_has_resource = 0;
            for (uint32_t k = 0; k < q->nranges; k++) {
                size_t g = (size_t)ranges_at + (size_t)stride * k;
                range12 *x = &q->ranges[k];
                x->type = (int32_t)rd32(p, len, g);
                x->count = rd32(p, len, g + 4);
                x->base = rd32(p, len, g + 8);
                x->space = rd32(p, len, g + 12);
                x->flags = version == 1 ? 0 : rd32(p, len, g + 16);
                x->offset = rd32(p, len, g + (version == 1 ? 16 : 20));
                if (x->type < 0 || x->type > 3 || !x->count ||
                    (version >= 2 && !valid_descriptor_range_flags12(x->flags))) return E_INVALIDARG;
                if (x->type == 3) table_has_sampler = 1;
                else table_has_resource = 1;
                if (table_has_sampler && table_has_resource) return E_INVALIDARG;
                if (x->offset == UINT32_MAX) {
                    if (!append_valid || running == UINT32_MAX) return E_INVALIDARG;
                    x->offset = running;                              // D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                }
                if (x->count == UINT32_MAX) append_valid = 0;
                else {
                    if (x->offset > UINT32_MAX - x->count) return E_INVALIDARG;
                    running = x->offset + x->count;
                    append_valid = 1;
                }
            }
            HRESULT hr = validate_table_ranges(q);
            if (hr != S_OK) return hr;
        } else if (q->type == 1) {
            if (!span_ok(len, payload, 12)) return E_INVALIDARG;
            q->reg = rd32(p, len, payload); q->space = rd32(p, len, payload + 4); q->n32 = rd32(p, len, payload + 8);
            if (!q->n32 || q->n32 > 64 - root_cost) return E_INVALIDARG;
            root_cost += q->n32;
        } else if (q->type >= 2 && q->type <= 4) {
            if (!span_ok(len, payload, version == 1 ? 8 : 12)) return E_INVALIDARG;
            if (root_cost > 62) return E_INVALIDARG;
            root_cost += 2;
            q->reg = rd32(p, len, payload); q->space = rd32(p, len, payload + 4);
            q->flags = version == 1 ? 0 : rd32(p, len, payload + 8);
            if (version >= 2 && !valid_root_descriptor_flags12(q->flags)) return E_INVALIDARG;
        } else {
            return E_INVALIDARG;
        }
    }
    r->samplers = calloc((size_t)nsamplers + 1, sizeof *r->samplers);
    if (!r->samplers) return E_OUTOFMEMORY;
    r->nsamplers = nsamplers;
    for (uint32_t i = 0; i < nsamplers; i++) {
        uint32_t s[14] = { 0 };
        uint32_t sampler_stride = version == 3 ? 56 : 52;
        size_t sampler_at = (size_t)samplers_at + sampler_stride * (size_t)i;
        for (int k = 0; k < (version == 3 ? 14 : 13); k++)
            s[k] = rd32(p, len, sampler_at + 4 * (size_t)k);
        uint32_t sampler_flags = s[13];
        // Version 1.2 adds the two integer border enums; Metal exposes only floating-point sampler border colors.
        if (s[7] > (version == 3 ? 4u : 2u) || s[12] > 7 || s[7] >= 3 || (sampler_flags & ~0x2u))
            return E_INVALIDARG;
        float bias, mn, mx;
        memcpy(&bias, &s[4], 4); memcpy(&mn, &s[8], 4); memcpy(&mx, &s[9], 4);
        int normalized_coordinates = !(sampler_flags & 0x2u);
        if (!normalized_coordinates &&
            ((s[0] != 0 && s[0] != 0x15u) ||
             bias != 0.0f || mn != 0.0f || mx != 0.0f ||
             (s[1] != 3 && s[1] != 4) || (s[2] != 3 && s[2] != 4) ||
             ((s[1] == 4 || s[2] == 4) && s[7] != 0))) return E_INVALIDARG;
        static const float borders[3][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1, 1 } };
        r->samplers[i].reg = s[10];
        r->samplers[i].space = s[11];
        r->samplers[i].visibility = (int32_t)s[12];
        r->samplers[i].state = make_sampler(d, (int32_t)s[0], (int32_t)s[1], (int32_t)s[2], (int32_t)s[3],
            bias, s[5], (int32_t)s[6], borders[s[7]], mn, mx, normalized_coordinates);
    }
    HRESULT hr = validate_root_bindings(r);
    return hr == S_OK ? create_root_gpu_layout12(r) : hr;
}

// ---- samplers ---------------------------------------------------------------------------------------------------

static MTLSamplerAddressMode address_mode(int32_t m, int normalized_coordinates, int transparent_border) {
    switch (m) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return !normalized_coordinates && transparent_border ?
        MTLSamplerAddressModeClampToZero : MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

static id<MTLSamplerState> make_sampler_new12(dev12 *d, int32_t filter, int32_t u, int32_t v, int32_t w,
    float bias, uint32_t aniso, int32_t cmp, const float border[4], float min_lod, float max_lod,
    int normalized_coordinates);
// Sampler states by description, shared: a game writes sampler descriptors by the thousand (each made a new Metal
// sampler state, the one a descriptor held freed when the descriptor was overwritten - while command buffers in
// flight could still use it - and Metal allows only so many with argument-buffer support).
static id<MTLSamplerState> make_sampler(dev12 *d, int32_t filter, int32_t u, int32_t v, int32_t w,
    float bias, uint32_t aniso, int32_t cmp, const float border[4], float min_lod, float max_lod,
    int normalized_coordinates) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static NSMutableDictionary<NSData *, id<MTLSamplerState>> *cache;
    struct { void *dev; int32_t filter, u, v, w; float bias; uint32_t aniso; int32_t cmp; float border[4], min_lod, max_lod; int norm; } key;
    memset(&key, 0, sizeof key);
    key.dev = d; key.filter = filter; key.u = u; key.v = v; key.w = w; key.bias = bias; key.aniso = aniso; key.cmp = cmp;
    memcpy(key.border, border, sizeof key.border); key.min_lod = min_lod; key.max_lod = max_lod; key.norm = normalized_coordinates;
    NSData *k = [NSData dataWithBytes:&key length:sizeof key];
    pthread_mutex_lock(&lock);
    if (!cache) cache = [NSMutableDictionary new];
    id<MTLSamplerState> st = cache[k];
    pthread_mutex_unlock(&lock);
    if (st) return st;
    st = make_sampler_new12(d, filter, u, v, w, bias, aniso, cmp, border, min_lod, max_lod, normalized_coordinates);
    if (!st) return nil;
    pthread_mutex_lock(&lock);
    if (cache[k]) st = cache[k]; else cache[k] = st;
    pthread_mutex_unlock(&lock);
    return st;
}
static id<MTLSamplerState> make_sampler_new12(dev12 *d, int32_t filter, int32_t u, int32_t v, int32_t w,
    float bias, uint32_t aniso, int32_t cmp, const float border[4], float min_lod, float max_lod,
    int normalized_coordinates) {
    MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
    // D3D12_FILTER: bits 0 mip linear, 2 mag linear, 4 min linear, 6 anisotropic, 7 comparison
    s.minFilter = (filter & 0x10) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    s.magFilter = (filter & 0x04) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    s.mipFilter = (filter & 0x01) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    if (filter & 0x40) { s.minFilter = s.magFilter = MTLSamplerMinMagFilterLinear; s.mipFilter = MTLSamplerMipFilterLinear; s.maxAnisotropy = aniso < 1 ? 1 : aniso > 16 ? 16 : aniso; }
    if (!normalized_coordinates) s.mipFilter = MTLSamplerMipFilterNotMipmapped;
    int transparent_border = border[0] == 0 && border[1] == 0 && border[2] == 0 && border[3] == 0;
    s.sAddressMode = address_mode(u, normalized_coordinates, transparent_border);
    s.tAddressMode = address_mode(v, normalized_coordinates, transparent_border);
    s.rAddressMode = address_mode(w, normalized_coordinates, transparent_border);
    s.borderColor = border[3] == 0 ? MTLSamplerBorderColorTransparentBlack : border[0] == 0 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite;
    s.normalizedCoordinates = normalized_coordinates;
    s.lodMinClamp = min_lod < 0 ? 0 : min_lod;
    s.lodMaxClamp = max_lod > 1000 ? FLT_MAX : max_lod;
    if (bias != 0.0f) {
        // Metal exposes sampler LOD bias only on Apple GPU family 10. Older Metal devices keep
        // the existing zero-bias behavior because there is no descriptor property to map this to.
        if (@available(macOS 26.0, *))
            if ([d->mtl supportsFamily:MTLGPUFamilyApple10]) s.lodBias = bias;
    }
    // bits 7-8: the reduction, 0 standard, 1 comparison, 2 minimum, 3 maximum (tiled resources tier 2)
    uint32_t reduction = ((uint32_t)filter >> 7) & 3;
    if (reduction >= 2) {
        if (@available(macOS 26.0, *))
            s.reductionMode = reduction == 2 ? MTLSamplerReductionModeMinimum : MTLSamplerReductionModeMaximum;
    }
    if (reduction == 1) {
        static const MTLCompareFunction f[] = { MTLCompareFunctionNever, MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
                                                MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
                                                MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways };
        s.compareFunction = cmp >= 1 && cmp <= 8 ? f[cmp] : MTLCompareFunctionNever;
    }
    s.supportArgumentBuffers = YES;
    return [d->mtl newSamplerStateWithDescriptor:s];
}

// ---- resources ----------------------------------------------------------------------------------------------------

static void gpu12_release_mtl_object(void *object) { (void)CFBridgingRelease(object); }

// The residency set (dev12.residency): resources counted by allocation, the set committed before a submission.
static void residency_add_allocation12(dev12 *d, id allocation) {   // (d->lock)
    if (!d->residency || !allocation) return;
    if (@available(macOS 15.0, *)) {
        if (![d->resident_allocations countForObject:allocation]) {
            [(id<MTLResidencySet>)d->residency addAllocation:(id<MTLAllocation>)allocation];
            d->residency_dirty = 1;
        }
        [d->resident_allocations addObject:allocation];
    }
}
static void residency_remove_allocation12(dev12 *d, id allocation) {   // (d->lock)
    if (!d->residency || !allocation) return;
    if (@available(macOS 15.0, *)) {
        if (![d->resident_allocations countForObject:allocation]) return;
        [d->resident_allocations removeObject:allocation];
        if (![d->resident_allocations countForObject:allocation]) {
            uint64_t at[16];
            for (int i = 0; i < 16; i++) at[i] = d->queues[i] ? __atomic_load_n(&((queue12 *)d->queues[i])->timeline_value, __ATOMIC_ACQUIRE) : 0;
            if (!d->residency_pending) d->residency_pending = [NSMutableArray new];
            [d->residency_pending addObject:@[ allocation, [NSData dataWithBytes:at length:sizeof at] ]];
        }
    }
}
// (before a submission: the pending removals the GPU is done with leave the set)
static void residency_settle12(dev12 *d) {   // (d->lock)
    if (!d->residency_pending.count) return;
    if (@available(macOS 15.0, *)) {
        NSMutableIndexSet *done = [NSMutableIndexSet indexSet];
        [d->residency_pending enumerateObjectsUsingBlock:^(NSArray *entry, NSUInteger k, BOOL *stop) {
            (void)stop;
            const uint64_t *at = ((NSData *)entry[1]).bytes;
            for (int i = 0; i < 16; i++) {
                queue12 *q = d->queues[i];
                if (q && q->timeline && q->timeline.signaledValue < at[i]) return;
            }
            [done addIndex:k];
            if (![d->resident_allocations countForObject:entry[0]]) {
                [(id<MTLResidencySet>)d->residency removeAllocation:(id<MTLAllocation>)entry[0]];
                d->residency_dirty = 1;
            }
        }];
        [d->residency_pending removeObjectsAtIndexes:done];
    }
}
static void residency_add12(dev12 *d, id<MTLResource> resource) {   // (d->lock)
    if (resource) residency_add_allocation12(d, resource.heap ? (id)resource.heap : (id)resource);
}
static void residency_remove12(dev12 *d, id<MTLResource> resource) {   // (d->lock)
    if (resource) residency_remove_allocation12(d, resource.heap ? (id)resource.heap : (id)resource);
}
static void grave_settle12(dev12 *d);
static void residency_commit12(dev12 *d) {   // (d->lock)
    grave_settle12(d);
    if (d->residency) residency_settle12(d);
    if (!d->residency || !d->residency_dirty) return;
    if (@available(macOS 15.0, *)) [(id<MTLResidencySet>)d->residency commit];
    d->residency_dirty = 0;
}

// The buffers by GPU address: the first index whose buffer starts after va (device lock held).
static uint32_t bufs_upper12(dev12 *d, uint64_t va) {
    uint32_t lo = 0, hi = d->nbufs;
    while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (((res12 *)d->bufs[mid])->buffer.gpuAddress <= va) lo = mid + 1; else hi = mid; }
    return lo;
}
// The buffer holding [va, va + size) (placed buffers may overlap: one that holds it), or NULL. Device lock held.
static res12 *bufs_find12(dev12 *d, uint64_t va, uint64_t size) {
    for (uint32_t i = bufs_upper12(d, va); i-- > 0; ) {
        res12 *r = d->bufs[i];
        uint64_t base = r->buffer.gpuAddress, length = r->buffer.length;
        if (va - base > d->maxbuflen) break;                      // (no buffer starting this early reaches va)
        if (va - base <= length && size <= length - (va - base)) return r;
    }
    return NULL;
}
static void bufs_insert12(dev12 *d, res12 *r) {
    pthread_rwlock_wrlock(&d->bufs_lock);
    if (d->nbufs == d->capbufs) { d->capbufs = d->capbufs ? d->capbufs * 2 : 256; d->bufs = realloc(d->bufs, d->capbufs * sizeof *d->bufs); }
    uint32_t at = bufs_upper12(d, r->buffer.gpuAddress);
    memmove(&d->bufs[at + 1], &d->bufs[at], (d->nbufs - at) * sizeof *d->bufs);
    d->bufs[at] = r;
    d->nbufs++;
    if (r->buffer.length > d->maxbuflen) d->maxbuflen = r->buffer.length;
    pthread_rwlock_unlock(&d->bufs_lock);
}
static void bufs_remove_locked12(dev12 *d, res12 *r) {
    for (uint32_t i = bufs_upper12(d, r->buffer.gpuAddress); i-- > 0; ) {
        if (d->bufs[i] != r) { if (((res12 *)d->bufs[i])->buffer.gpuAddress != r->buffer.gpuAddress) break; continue; }
        memmove(&d->bufs[i], &d->bufs[i + 1], (d->nbufs - i - 1) * sizeof *d->bufs);
        d->nbufs--;
        return;
    }
    for (uint32_t i = 0; i < d->nbufs; i++)                        // (not where its address says: anywhere)
        if (d->bufs[i] == r) { memmove(&d->bufs[i], &d->bufs[i + 1], (d->nbufs - i - 1) * sizeof *d->bufs); d->nbufs--; return; }
}
static void bufs_remove12(dev12 *d, res12 *r) {
    pthread_rwlock_wrlock(&d->bufs_lock);
    bufs_remove_locked12(d, r);
    pthread_rwlock_unlock(&d->bufs_lock);
}
static void res_destroy(d3d_object *o) {
    res12 *r = (res12 *)o;
    dev12 *d = r->dev;
    pthread_mutex_lock(&d->lock);
    if (r->buffer) bufs_remove12(d, r);
    if (r->buffer) [d->resources removeObjectIdenticalTo:r->buffer];
    if (r->texture) [d->resources removeObjectIdenticalTo:r->texture];
    residency_remove12(d, r->buffer ? (id<MTLResource>)r->buffer : (id<MTLResource>)r->texture);
    pthread_mutex_unlock(&d->lock);
    gpu_ir_resource_release(r->gpu_buffer);
    r->gpu_buffer = NULL;
    gpu_ir_resource_release(r->gpu_texture);
    r->gpu_texture = NULL;
    while (r->state_overrides) {
        res_state_override12 *next = r->state_overrides->next;
        free(r->state_overrides);
        r->state_overrides = next;
    }
    while (r->pending_transitions) {
        res_state_pending12 *next = r->pending_transitions->next;
        free(r->pending_transitions);
        r->pending_transitions = next;
    }
    free(r->mapped_subresources);
    r->mapped_subresources = NULL;
    if (r->owner_heap) {
        d3d_release(&r->owner_heap->base);
        r->owner_heap = NULL;
    }
    for (uint32_t i = 0; r->tile_heaps && i < r->tile_count; i++)
        if (r->tile_heaps[i]) d3d_release(&r->tile_heaps[i]->base);
    free(r->tile_heaps);
    free(r->tile_offsets);
    r->tile_heaps = NULL;
    r->tile_offsets = NULL;
    r->buffer = nil;
    r->texture = nil;
    release_dev(o);
}

static WINAPI HRESULT res_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12Resource, &IID_ID3D12Resource1, &IID_ID3D12Resource2, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 6);
}

static int res_texture_cpu_access_valid12(res12 *resource, uint32_t subresource) {
    if (!resource || !resource->texture || subresource >= resource->mapped_subresource_count ||
        !__atomic_load_n(&resource->mapped_subresources[subresource], __ATOMIC_RELAXED)) return 0;
    dev12 *device = resource->dev;
    int valid = 1;
    pthread_mutex_lock(&device->lock);
    uint32_t state = resource->initial_state;
    for (res_state_override12 *entry = resource->state_overrides; entry; entry = entry->next)
        if (entry->subresource == subresource) { state = entry->state; break; }
    for (res_state_pending12 *pending = resource->pending_transitions; pending; pending = pending->next)
        if (pending->subresource == subresource || pending->subresource == D3D12_ALL_SUBRESOURCES) { valid = 0; break; }
    if (state != 0) valid = 0;                                   // Textures require COMMON for CPU transfer.
    pthread_mutex_unlock(&device->lock);
    return valid;
}

static uint64_t trace_len12(void);
// (buffers whose contents are watched: the number of non-zero words logged at each 60th present when it changed)
static id<MTLBuffer> g_watch12[8];
static uint32_t g_watch_nz12[8];
static int g_nwatch12;
static void watch_add12(id<MTLBuffer> b) {
    for (int i = 0; i < g_nwatch12; i++) if (g_watch12[i] == b) return;
    if (g_nwatch12 < 8) g_watch12[g_nwatch12++] = b;
}
static void watch_present12(uint32_t presents) {
    if (!g_nwatch12 || presents % 60) return;
    for (int i = 0; i < g_nwatch12; i++) {
        const uint32_t *w = g_watch12[i].contents;
        if (!w) continue;
        uint32_t nz = 0;
        for (NSUInteger k = 0; k < g_watch12[i].length / 4; k++) if (w[k]) nz++;
        if (nz != g_watch_nz12[i]) {
            vx_log("D3D12: watched buffer %#llx: %u non-zero words now (present %u)", (unsigned long long)g_watch12[i].gpuAddress, nz, presents);
            g_watch_nz12[i] = nz;
        }
    }
}
static void trace_writer12(const char *what, uint64_t hash, const gpu_ir_resource_access *a, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) {
        if (!(a[k].access_mask & GPU_IR_ACCESS_SHADER_WRITE)) continue;
        id obj = (__bridge id)gpu_ir_backend_object(a[k].resource);
        if ([obj conformsToProtocol:@protocol(MTLBuffer)] && ((id<MTLBuffer>)obj).length == trace_len12()) {
            static volatile int told;
            if (__sync_add_and_fetch(&told, 1) <= 40)
                vx_log("D3D12: %s %016llx writes the %llu-byte buffer %#llx", what, (unsigned long long)hash,
                       (unsigned long long)trace_len12(), (unsigned long long)((id<MTLBuffer>)obj).gpuAddress);
        }
    }
}
// VXRT_D3D12_TRACE_CS=<hash,...>: those compute shaders' dispatches log what they reach (first 4 dispatches each run)
static int trace_cs12(uint64_t hash) {
    const char *e = getenv("VXRT_D3D12_TRACE_CS");
    if (!e) return 0;
    char key[24]; snprintf(key, sizeof key, "%016llx", (unsigned long long)hash);
    return strstr(e, key) != NULL;
}
static void trace_cs_accesses12(uint64_t hash, uint32_t x, uint32_t y, uint32_t z, const gpu_ir_resource_access *a, uint32_t n) {
    static volatile int told;
    if (__sync_add_and_fetch(&told, 1) > 16) return;
    vx_log("D3D12: dispatch %ux%ux%u of cs %016llx reaches %u resources:", x, y, z, (unsigned long long)hash, n);
    for (uint32_t k = 0; k < n; k++) {
        id obj = (__bridge id)gpu_ir_backend_object(a[k].resource);
        if ([obj conformsToProtocol:@protocol(MTLTexture)]) {
            id<MTLTexture> t = obj;
            vx_log("D3D12:   texture %lux%lux%lu type %lu fmt %lu mips %lu usage %#lx storage %lu heap %p offset %lu alloc %lu, access %#x",
                   (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth, (unsigned long)t.textureType, (unsigned long)t.pixelFormat,
                   (unsigned long)t.mipmapLevelCount, (unsigned long)t.usage, (unsigned long)t.storageMode, (__bridge void *)t.heap, (unsigned long)t.heapOffset,
                   (unsigned long)t.allocatedSize, a[k].access_mask);
        } else if ([obj conformsToProtocol:@protocol(MTLBuffer)]) {
            id<MTLBuffer> b = obj;
            vx_log("D3D12:   buffer %lu bytes at %#llx, access %#x", (unsigned long)b.length, (unsigned long long)b.gpuAddress, a[k].access_mask);
        }
    }
}
// VXRT_D3D12_TRACE_LEN=<bytes>: Map and CopyBufferRegion of buffers that long are logged (where a buffer's data
// comes from).
static uint64_t trace_len12(void) { static long long n = -1; if (n < 0) { const char *e = getenv("VXRT_D3D12_TRACE_LEN"); n = e ? atoll(e) : 0; } return (uint64_t)n; }
static WINAPI HRESULT res_map(void *this, uint32_t sub, const RANGE *read, void **data) {
    res12 *r = this;
    if (trace_len12() && r->buffer && (r->desc.Width == trace_len12() || (getenv("VXRT_D3D12_TRACE_LEN2") && r->desc.Width == (uint64_t)atoll(getenv("VXRT_D3D12_TRACE_LEN2"))))) {
        static volatile int n;
        if (__sync_add_and_fetch(&n, 1) <= 400) vx_log("D3D12: Map of the %llu-byte buffer %#llx (heap type %d, page %d)", (unsigned long long)r->desc.Width,
                                                      (unsigned long long)r->buffer.gpuAddress, r->heap.Type, r->heap.CPUPageProperty);
    }
    if (r->texture) {
        if (sub >= r->mapped_subresource_count) return E_INVALIDARG;
        if (data) *data = NULL;                                  // Opaque Metal texture layouts have no guest pointer.
        __atomic_add_fetch(&r->mapped_subresources[sub], 1, __ATOMIC_RELAXED);
        return S_OK;
    }
    if (!r->buffer) return E_INVALIDARG;
    if (sub != 0) return E_INVALIDARG;
    if (read && (read->begin > r->desc.Width || read->end > r->desc.Width)) return E_INVALIDARG;
    int cpu_visible = r->heap.Type == 2 || r->heap.Type == 3 ||
        (r->heap.Type == 4 && (r->heap.CPUPageProperty == 2 || r->heap.CPUPageProperty == 3));
    if (!cpu_visible) return DXGI_ERROR_INVALID_CALL;
    if (data) {
        if (!r->buffer.contents) return E_FAIL;
        *data = r->buffer.contents;
    }
    __atomic_add_fetch(&r->mapped, 1, __ATOMIC_RELAXED);
    if (getenv("VXRT_D3D12_TRACE_UPLOAD") && r->mapped == 1 && r->heap.Type == 2)
        vx_log("D3D12: first Map of buffer %#llx (%llu bytes): %p", (unsigned long long)r->buffer.gpuAddress, (unsigned long long)r->desc.Width, r->buffer.contents);
    return S_OK;
}
static WINAPI void res_unmap(void *this, uint32_t sub, const RANGE *written) {
    res12 *r = this;
    (void)written;
    if (r->texture) {
        if (sub >= r->mapped_subresource_count) return;
        uint32_t mapped = __atomic_load_n(&r->mapped_subresources[sub], __ATOMIC_RELAXED);
        while (mapped && !__atomic_compare_exchange_n(&r->mapped_subresources[sub], &mapped, mapped - 1, 0,
                                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        return;
    }
    if (sub != 0 || !r->buffer) return;
    uint32_t mapped = __atomic_load_n(&r->mapped, __ATOMIC_RELAXED);
    while (mapped && !__atomic_compare_exchange_n(&r->mapped, &mapped, mapped - 1, 0,
                                                   __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

typedef struct {
    uint32_t mip, block_width, block_height, bytes_per_block;
    NSUInteger slice;
    MTLOrigin origin;
    MTLSize size;
    uint64_t row_bytes, row_count, depth;
    int empty;
} res_texture_copy12;

static uint64_t res_mip_extent12(uint64_t extent, uint32_t mip) {
    if (mip >= 64) return 1;
    extent >>= mip;
    return extent ? extent : 1;
}

static int res_texture_copy_region12(res12 *resource, uint32_t subresource, const BOX *box,
                                     res_texture_copy12 *copy) {
    if (!copy) return 0;
    memset(copy, 0, sizeof *copy);
    if (!resource || !resource->texture) return 0;
    uint32_t mips = (uint32_t)resource->texture.mipmapLevelCount;
    uint32_t array_size = resource->desc.Dimension == 4 ? 1u : resource->desc.DepthOrArraySize;
    uint32_t planes = d3d12_resource_plane_count(resource->desc.Format);
    uint64_t plane_size = (uint64_t)mips * array_size;
    if (!mips || !array_size || !planes || !plane_size || plane_size > UINT64_MAX / planes ||
        subresource >= plane_size * planes) return 0;
    if (box && (box->left >= box->right || box->top >= box->bottom || box->front >= box->back)) {
        copy->empty = 1;
        return 1;
    }
    if (resource->desc.SampleCount != 1 || planes != 1) return -1;
    uint32_t subresource_in_plane = (uint32_t)(subresource % plane_size);
    copy->mip = subresource_in_plane % mips;
    copy->slice = resource->desc.Dimension == 4 ? 0 : subresource_in_plane / mips;

    uint64_t width = res_mip_extent12(resource->texture.width, copy->mip);
    uint64_t height = resource->desc.Dimension == 2 ? 1 : res_mip_extent12(resource->texture.height, copy->mip);
    uint64_t depth = resource->desc.Dimension == 4 ? res_mip_extent12(resource->texture.depth, copy->mip) : 1;
    if (width > UINT32_MAX || height > UINT32_MAX || depth > UINT32_MAX) return 0;
    BOX region = box ? *box : (BOX){ 0, 0, 0, (uint32_t)width, (uint32_t)height, (uint32_t)depth };
    if (region.left >= region.right || region.top >= region.bottom || region.front >= region.back) {
        copy->empty = 1;
        return 1;
    }
    if (region.right > width || region.bottom > height || region.back > depth) return 0;
    if (resource->desc.Dimension != 4 && (region.front != 0 || region.back != 1)) return 0;
    if (resource->desc.Dimension == 2 && (region.top != 0 || region.bottom != 1)) return 0;
    if (!d3d_copy_format_block(resource->desc.Format, &copy->block_width, &copy->block_height,
                               &copy->bytes_per_block) || !copy->block_width || !copy->block_height ||
        !copy->bytes_per_block) return -1;
    if (region.left % copy->block_width || region.top % copy->block_height ||
        (region.right != width && region.right % copy->block_width) ||
        (region.bottom != height && region.bottom % copy->block_height)) return 0;
    uint64_t region_width = region.right - region.left;
    uint64_t region_height = region.bottom - region.top;
    copy->row_bytes = (region_width / copy->block_width + (region_width % copy->block_width != 0)) *
                      copy->bytes_per_block;
    copy->row_count = region_height / copy->block_height + (region_height % copy->block_height != 0);
    copy->depth = region.back - region.front;
    if (!copy->row_bytes || !copy->row_count || !copy->depth || copy->row_bytes > SIZE_MAX) return 0;
    copy->origin = MTLOriginMake(region.left, region.top, region.front);
    copy->size = MTLSizeMake(region_width, region_height, copy->depth);
    return 1;
}

static HRESULT res_texture_transfer12(res12 *resource, uint32_t subresource, const BOX *box,
                                      void *memory, uint32_t row_pitch, uint32_t depth_pitch, int write) {
    res_texture_copy12 copy;
    int region_result = res_texture_copy_region12(resource, subresource, box, &copy);
    if (region_result < 0) return DXGI_ERROR_UNSUPPORTED;
    if (!region_result) return E_INVALIDARG;
    if (copy.empty) return S_OK;
    if (!memory) return E_POINTER;
    uint64_t minimum_depth_pitch;
    if (copy.row_bytes > UINT32_MAX || row_pitch < copy.row_bytes ||
        copy.row_count > UINT64_MAX / row_pitch) return E_INVALIDARG;
    minimum_depth_pitch = (uint64_t)row_pitch * copy.row_count;
    if (copy.depth > 1 && depth_pitch < minimum_depth_pitch) return E_INVALIDARG;
    if (!res_texture_cpu_access_valid12(resource, subresource)) return DXGI_ERROR_INVALID_CALL;

    uint64_t staging_row_pitch = (copy.row_bytes + 255u) & ~UINT64_C(255);
    if (staging_row_pitch < copy.row_bytes || copy.row_count > UINT64_MAX / staging_row_pitch) return E_INVALIDARG;
    uint64_t staging_image_pitch = staging_row_pitch * copy.row_count;
    if (copy.depth > UINT64_MAX / staging_image_pitch) return E_INVALIDARG;
    uint64_t staging_size = staging_image_pitch * copy.depth;
    if (!staging_size || staging_size > SIZE_MAX || staging_row_pitch > UINT32_MAX ||
        staging_image_pitch > UINT32_MAX) return E_INVALIDARG;

    dev12 *device = resource->dev;
    HRESULT result = E_FAIL;
    pthread_mutex_lock(&device->submit_lock);
    if (!res_texture_cpu_access_valid12(resource, subresource)) {
        pthread_mutex_unlock(&device->submit_lock);
        return DXGI_ERROR_INVALID_CALL;
    }
    @autoreleasepool {
        id<MTLBuffer> staging = [device->mtl newBufferWithLength:(NSUInteger)staging_size
                                                         options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> command_buffer = staging ? [device->cpu_transfer_queue commandBuffer] : nil;
        id<MTLBlitCommandEncoder> blit = command_buffer ? [command_buffer blitCommandEncoder] : nil;
        if (!staging || !command_buffer || !blit) {
            result = E_OUTOFMEMORY;
        } else {
            uint8_t *bytes = staging.contents;
            if (write) {
                const uint8_t *source = memory;
                for (uint64_t z = 0; z < copy.depth; z++)
                    for (uint64_t y = 0; y < copy.row_count; y++)
                        memcpy(bytes + z * staging_image_pitch + y * staging_row_pitch,
                               source + z * depth_pitch + y * row_pitch, (size_t)copy.row_bytes);
                [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:(NSUInteger)staging_row_pitch
                    sourceBytesPerImage:(NSUInteger)staging_image_pitch sourceSize:copy.size toTexture:resource->texture
                    destinationSlice:copy.slice destinationLevel:copy.mip destinationOrigin:copy.origin];
            } else {
                [blit copyFromTexture:resource->texture sourceSlice:copy.slice sourceLevel:copy.mip
                    sourceOrigin:copy.origin sourceSize:copy.size toBuffer:staging destinationOffset:0
                    destinationBytesPerRow:(NSUInteger)staging_row_pitch destinationBytesPerImage:(NSUInteger)staging_image_pitch];
            }
            [blit endEncoding];
            [command_buffer commit];
            { uint64_t sync_t0 = fs_now12(); [command_buffer waitUntilCompleted]; FS_ADD12(sync_n, 1); FS_ADD12(sync_ns, fs_now12() - sync_t0); }
            if (command_buffer.status != MTLCommandBufferStatusCompleted) {
                vx_log("D3D12: CPU texture transfer failed: %s",
                       command_buffer.error.localizedDescription.UTF8String ?: "unknown Metal error");
                result = E_FAIL;
            } else {
                if (!write) {
                    uint8_t *destination = memory;
                    const uint8_t *source = staging.contents;
                    for (uint64_t z = 0; z < copy.depth; z++)
                        for (uint64_t y = 0; y < copy.row_count; y++)
                            memcpy(destination + z * depth_pitch + y * row_pitch,
                                   source + z * staging_image_pitch + y * staging_row_pitch,
                                   (size_t)copy.row_bytes);
                }
                result = S_OK;
            }
        }
    }
    pthread_mutex_unlock(&device->submit_lock);
    return result;
}

static WINAPI HRESULT res_write_to_subresource(void *this, uint32_t subresource, const BOX *box,
                                                const void *source, uint32_t row_pitch, uint32_t depth_pitch) {
    res12 *resource = this;
    if (resource->texture)
        return res_texture_transfer12(resource, subresource, box, (void *)source, row_pitch, depth_pitch, 1);
    if (!resource->buffer || subresource) return E_INVALIDARG;
    if (box && (box->left >= box->right || box->top >= box->bottom || box->front >= box->back)) return S_OK;
    if (!source) return E_POINTER;
    if (!__atomic_load_n(&resource->mapped, __ATOMIC_RELAXED)) return DXGI_ERROR_INVALID_CALL;
    if (resource->desc.Width > UINT32_MAX) return DXGI_ERROR_UNSUPPORTED;
    BOX region = box ? *box : (BOX){ 0, 0, 0, (uint32_t)resource->desc.Width, 1, 1 };
    if (region.top || region.bottom != 1 || region.front || region.back != 1 ||
        region.right > resource->desc.Width || row_pitch < region.right - region.left) return E_INVALIDARG;
    memcpy((uint8_t *)resource->buffer.contents + region.left, source, region.right - region.left);
    return S_OK;
}

static WINAPI HRESULT res_read_from_subresource(void *this, void *destination, uint32_t row_pitch,
                                                uint32_t depth_pitch, uint32_t subresource, const BOX *box) {
    res12 *resource = this;
    if (resource->texture)
        return res_texture_transfer12(resource, subresource, box, destination, row_pitch, depth_pitch, 0);
    if (!resource->buffer || subresource) return E_INVALIDARG;
    if (box && (box->left >= box->right || box->top >= box->bottom || box->front >= box->back)) return S_OK;
    if (!destination) return E_POINTER;
    if (!__atomic_load_n(&resource->mapped, __ATOMIC_RELAXED)) return DXGI_ERROR_INVALID_CALL;
    if (resource->desc.Width > UINT32_MAX) return DXGI_ERROR_UNSUPPORTED;
    BOX region = box ? *box : (BOX){ 0, 0, 0, (uint32_t)resource->desc.Width, 1, 1 };
    if (region.top || region.bottom != 1 || region.front || region.back != 1 ||
        region.right > resource->desc.Width || row_pitch < region.right - region.left) return E_INVALIDARG;
    memcpy(destination, (const uint8_t *)resource->buffer.contents + region.left, region.right - region.left);
    return S_OK;
}

static WINAPI RES_DESC *res_get_desc(void *this, RES_DESC *out) { *out = ((res12 *)this)->desc; return out; }
static WINAPI RES_DESC1 *res_get_desc1(void *this, RES_DESC1 *out) {
    memset(out, 0, sizeof *out);
    out->resource = ((res12 *)this)->desc;
    return out;
}
static WINAPI uint64_t res_gpu_va(void *this) { res12 *r = this; return r->buffer ? r->buffer.gpuAddress : 0; }
static WINAPI HRESULT res_get_heap_props(void *this, HEAP_PROPS *props, int32_t *flags) {
    if (props) *props = ((res12 *)this)->heap;
    if (flags) *flags = ((res12 *)this)->heap_flags;
    return S_OK;
}

static uint32_t bytes_per(uint32_t format) { uint32_t b = 0; d3d_mtl_format(format, &b); return b; }

static int d3d12_texture_atomic_format_supported(id<MTLDevice> mtl, uint32_t format);

// The Metal texture a D3D12 texture description stands for (nil: a format it has no Metal counterpart for).
static BOOL d3d12_color_resolve_float_format(MTLPixelFormat format);
static BOOL d3d12_color_resolve_format_supported(MTLPixelFormat format);
static MTLTextureDescriptor *texture_descriptor(dev12 *d, const RES_DESC *desc) {
    uint32_t bpp = 0;
    MTLPixelFormat fmt = d3d_mtl_format(desc->Format, &bpp);
    if (desc->Flags & 0x2) {                                       // ALLOW_DEPTH_STENCIL: a typeless format as its depth format
        uint32_t f = desc->Format;
        if (f == 39 || f == 40 || f == 41) fmt = MTLPixelFormatDepth32Float;
        else if (f == 19 || f == 20 || f == 21 || f == 44 || f == 45 || f == 46 || f == 47) fmt = MTLPixelFormatDepth32Float_Stencil8;
        else if (f == 53 || f == 55 || f == 56) fmt = MTLPixelFormatDepth16Unorm;
    }
    if (fmt == MTLPixelFormatInvalid) { vx_log("D3D12: texture format %u is not supported", desc->Format); return nil; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.pixelFormat = fmt;
    td.width = desc->Width;
    td.height = desc->Height ? desc->Height : 1;
    td.mipmapLevelCount = desc->MipLevels ? desc->MipLevels : 1;
    if (!desc->MipLevels) {                                        // 0: the full chain
        uint64_t m = desc->Width > td.height ? desc->Width : td.height;
        uint32_t n = 1;
        while (m > 1) { m >>= 1; n++; }
        td.mipmapLevelCount = n;
    }
    if (desc->Dimension == 4) { td.textureType = MTLTextureType3D; td.depth = desc->DepthOrArraySize; }
    else if (desc->DepthOrArraySize > 1) { td.textureType = desc->Dimension == 2 ? MTLTextureType1DArray : MTLTextureType2DArray; td.arrayLength = desc->DepthOrArraySize; }
    else td.textureType = desc->Dimension == 2 ? MTLTextureType1D : MTLTextureType2D;
    if (desc->SampleCount > 1) { td.textureType = desc->DepthOrArraySize > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample; td.sampleCount = desc->SampleCount; td.mipmapLevelCount = 1; }
    td.usage = MTLTextureUsagePixelFormatView;
    if (!(desc->Flags & 0x8)) td.usage |= MTLTextureUsageShaderRead;        // DENY_SHADER_RESOURCE
    if (desc->Flags & 0x4) td.usage |= MTLTextureUsageShaderWrite;              // ALLOW_UNORDERED_ACCESS
    if ((desc->Flags & 0x4) && desc->SampleCount <= 1 && d3d12_texture_atomic_format_supported(d->mtl, desc->Format))
        td.usage |= MTLTextureUsageShaderAtomic;
    if (desc->Flags & 0x3) td.usage |= MTLTextureUsageRenderTarget;             // ALLOW_RENDER_TARGET, ALLOW_DEPTH_STENCIL
    // Any single-sample 2D color texture may be a ResolveSubresource destination in D3D12 (no flag asks for it);
    // Metal resolves only into render-target textures.
    if (desc->Dimension == 3 && desc->SampleCount <= 1 &&
        (d3d12_color_resolve_float_format(td.pixelFormat) || d3d12_color_resolve_format_supported(td.pixelFormat)))
        td.usage |= MTLTextureUsageRenderTarget;
    td.storageMode = MTLStorageModePrivate;
    return td;
}

static int texture_sample_supported12(dev12 *device, const RES_DESC *desc) {
    if (!device || !desc || !desc->SampleCount) return 0;
    if (![device->mtl supportsTextureSampleCount:desc->SampleCount]) return 0;
    RES_DESC probe = *desc;
    probe.Width = 1;
    probe.Height = 1;
    probe.DepthOrArraySize = 1;
    probe.MipLevels = 1;
    probe.Alignment = 0;
    probe.SampleQuality = 0;
    MTLTextureDescriptor *td = texture_descriptor(device, &probe);
    if (!td) return 0;
    @autoreleasepool {
        @try {
            id<MTLTexture> texture = [device->mtl newTextureWithDescriptor:td];
            return texture != nil;
        } @catch (NSException *exception) {
            (void)exception;
            return 0;
        }
    }
}

static int resource_clear_value_valid12(const RES_DESC *desc, const void *clear) {
    if (!clear) return 1;
    if (!desc || desc->Dimension == 1) return 0;                  // D3D12 requires NULL for buffers.
    uint32_t clear_format = 0;
    memcpy(&clear_format, clear, sizeof clear_format);
    if (!clear_format || d3d12_resolve_typeless(clear_format)) return 0;
    return d3d_copy_formats_compatible(desc->Format, clear_format);
}

static int d3d12_resource_state_dimension_valid12(const RES_DESC *desc, uint32_t state) {
    const uint32_t buffer_only = D3D12_STATE_VERTEX_CONSTANT | D3D12_STATE_INDEX |
        D3D12_STATE_STREAM_OUT | D3D12_STATE_INDIRECT;
    const uint32_t texture_only = D3D12_STATE_RENDER_TARGET | D3D12_STATE_DEPTH_READ |
        D3D12_STATE_DEPTH_WRITE | D3D12_STATE_RESOLVE_SOURCE | D3D12_STATE_RESOLVE_DEST |
        D3D12_STATE_SHADING_RATE;
    if (desc->Dimension == 1) return !(state & texture_only);
    if (state & buffer_only) return 0;
    return !(state & D3D12_STATE_SHADING_RATE) || desc->Dimension == 3;
}

static int d3d12_resource_state_descriptor_valid12(const RES_DESC *desc, uint32_t state) {
    if ((state & (D3D12_STATE_NON_PIXEL_SHADER | D3D12_STATE_PIXEL_SHADER)) && (desc->Flags & 0x8)) return 0;
    if (!(state & D3D12_STATE_SHADING_RATE)) return 1;
    const uint32_t shading_rate_forbidden_flags = 0x1u | 0x2u | 0x10u | 0x20u | 0x40u | 0x80u;
    return desc->Dimension == 3 && desc->Format == 62 /* DXGI_FORMAT_R8_UINT */ &&
        desc->DepthOrArraySize == 1 && desc->MipLevels == 1 && desc->SampleCount == 1 &&
        !desc->SampleQuality && desc->Layout == D3D12_LAYOUT_UNKNOWN12 &&
        !(desc->Flags & shading_rate_forbidden_flags);
}

static int d3d12_barrier_state_access(uint32_t state, uint32_t *access);

static int d3d12_resource_initial_state_valid12(const RES_DESC *desc, uint32_t state) {
    uint32_t access;
    if (!d3d12_barrier_state_access(state, &access) ||
        !d3d12_resource_state_dimension_valid12(desc, state) || (desc->Flags & 0x8 && !(desc->Flags & 0x2)) ||
        !d3d12_resource_state_descriptor_valid12(desc, state)) return 0;
    if ((state & D3D12_STATE_RENDER_TARGET) && !(desc->Flags & 0x1)) return 0;
    if ((state & (D3D12_STATE_DEPTH_READ | D3D12_STATE_DEPTH_WRITE)) && !(desc->Flags & 0x2)) return 0;
    if ((state & D3D12_STATE_UAV) && !(desc->Flags & 0x4)) return 0;
    if ((state & D3D12_STATE_RESOLVE_SOURCE) && desc->SampleCount <= 1) return 0;
    if ((state & D3D12_STATE_RESOLVE_DEST) && desc->SampleCount != 1) return 0;
    return 1;
}

// ---- tiled (reserved) resources --------------------------------------------------------------------------------------
// D3D12's tile order (D3D12_SUBRESOURCE_TILING): for each array slice, the tiles of its standard mips one after the
// other (X, then Y), then the slice's packed mips. Metal's placement-sparse textures use the same 64 KiB tile shapes
// as D3D12's standard 2D shapes (128x128 for 32 bpp, 512x256 texels for BC1...) and keep a mip tail from
// firstMipmapInTail on, addressed by tile index in X, which is what D3D12 calls packed mips.

#define TILE_BYTES12 65536u
typedef struct {
    uint32_t shape[3];               // standard tile shape, texels (or bytes for buffers)
    uint32_t mips, slices, std_mips, packed_mips, packed_tiles;
    uint32_t per_slice, total;
    uint32_t mip_start[16];          // the first tile of each standard mip within its slice
    uint32_t mip_tiles[16][2];       // width and height of each standard mip, tiles
} tiling12;

static int tiling12_get(const res12 *r, tiling12 *t) {
    memset(t, 0, sizeof *t);
    if (!r || !r->reserved) return 0;
    if (r->buffer) {
        t->shape[0] = TILE_BYTES12; t->shape[1] = t->shape[2] = 1;
        t->mips = t->slices = t->std_mips = 1;
        t->mip_tiles[0][0] = (uint32_t)((r->desc.Width + TILE_BYTES12 - 1) / TILE_BYTES12);
        t->mip_tiles[0][1] = 1;
        t->per_slice = t->total = t->mip_tiles[0][0];
        return 1;
    }
    id<MTLTexture> tex = r->texture;
    if (!tex) return 0;
    if (@available(macOS 26.0, *)) {
        MTLSize s = [r->dev->mtl sparseTileSizeWithTextureType:tex.textureType pixelFormat:tex.pixelFormat
                                                  sampleCount:tex.sampleCount sparsePageSize:MTLSparsePageSize64];
        if (!s.width || !s.height) return 0;
        t->shape[0] = (uint32_t)s.width; t->shape[1] = (uint32_t)s.height; t->shape[2] = s.depth ? (uint32_t)s.depth : 1;
        t->mips = (uint32_t)tex.mipmapLevelCount;
        t->slices = (uint32_t)tex.arrayLength;
        if (t->mips > 16) return 0;
        t->std_mips = tex.firstMipmapInTail < t->mips ? (uint32_t)tex.firstMipmapInTail : t->mips;
        t->packed_mips = t->mips - t->std_mips;
        t->packed_tiles = t->packed_mips ? (uint32_t)((tex.tailSizeInBytes + TILE_BYTES12 - 1) / TILE_BYTES12) : 0;
        if (t->packed_mips && !t->packed_tiles) t->packed_tiles = 1;
        uint32_t at = 0;
        for (uint32_t m = 0; m < t->std_mips; m++) {
            uint64_t w = tex.width >> m, h = tex.height >> m;
            if (!w) w = 1;
            if (!h) h = 1;
            t->mip_start[m] = at;
            t->mip_tiles[m][0] = (uint32_t)((w + t->shape[0] - 1) / t->shape[0]);
            t->mip_tiles[m][1] = (uint32_t)((h + t->shape[1] - 1) / t->shape[1]);
            at += t->mip_tiles[m][0] * t->mip_tiles[m][1];
        }
        t->per_slice = at + t->packed_tiles;
        t->total = t->per_slice * t->slices;
        return 1;
    }
    return 0;
}

// D3D12_TILED_RESOURCE_COORDINATE -> the resource's tile index, or UINT32_MAX when outside the resource.
static uint32_t tiling12_index(const tiling12 *t, uint32_t x, uint32_t y, uint32_t z, uint32_t subresource) {
    uint32_t mip = subresource % t->mips, slice = subresource / t->mips;
    if (slice >= t->slices || z) return UINT32_MAX;
    if (mip >= t->std_mips) {
        if (y || x >= t->packed_tiles) return UINT32_MAX;
        return slice * t->per_slice + (t->per_slice - t->packed_tiles) + x;
    }
    if (x >= t->mip_tiles[mip][0] || y >= t->mip_tiles[mip][1]) return UINT32_MAX;
    return slice * t->per_slice + t->mip_start[mip] + y * t->mip_tiles[mip][0] + x;
}

// The resource's tile index -> Metal's addressing (mip, slice, tile x, y; the tail by tile index in x).
static void tiling12_locate(const tiling12 *t, uint32_t index, uint32_t *mip, uint32_t *slice, uint32_t *x, uint32_t *y) {
    *slice = index / t->per_slice;
    uint32_t i = index % t->per_slice;
    uint32_t standard = t->per_slice - t->packed_tiles;
    if (i >= standard) { *mip = t->std_mips; *x = i - standard; *y = 0; return; }
    uint32_t m = 0;
    while (m + 1 < t->std_mips && t->mip_start[m + 1] <= i) m++;
    i -= t->mip_start[m];
    *mip = m;
    *x = i % t->mip_tiles[m][0];
    *y = i / t->mip_tiles[m][0];
}

static int d3d12_tiled_tier_probe12(id<MTLDevice> mtl) {
    if (getenv("VXRT_D3D12_NO_TILED") || getenv("VXRT_D3D12_NO_RESIDENCY_SET")) return 0;
    if (@available(macOS 26.0, *)) {
        @autoreleasepool {
            id<MTLBuffer> probe = [mtl newBufferWithLength:TILE_BYTES12 options:MTLResourceStorageModePrivate
                                   placementSparsePageSize:MTLSparsePageSize64];
            if (probe && [mtl sparseTileSizeInBytesForSparsePageSize:MTLSparsePageSize64] == TILE_BYTES12) return 2;
        }
    }
    return 0;
}

// A committed resource (heap nil) or one placed in a heap at an offset.
static HRESULT create_resource(dev12 *d, const HEAP_PROPS *hp, int32_t heap_flags, const RES_DESC *desc, int32_t initial_state,
                               const void *clear, memheap12 *in, uint64_t offset, int reserved, const GUID *iid, void **out) {
    const uint32_t generic_read = D3D12_STATE_VERTEX_CONSTANT | D3D12_STATE_INDEX | D3D12_STATE_NON_PIXEL_SHADER |
        D3D12_STATE_PIXEL_SHADER | D3D12_STATE_INDIRECT | D3D12_STATE_COPY_SOURCE;
    uint64_t required_alignment = 0;
    if (!hp || !desc) return E_INVALIDARG;
    // Abstract UPLOAD and READBACK heaps are buffer-only. CPU-visible textures,
    // where supported, use a CUSTOM heap with explicit properties.
    if ((desc->Dimension != 1 && (hp->Type == 2 || hp->Type == 3)) ||
        (hp->Type == 2 && (uint32_t)initial_state != generic_read) ||
        (hp->Type == 3 && (uint32_t)initial_state != D3D12_STATE_COPY_DEST) ||
        !d3d12_resource_initial_state_valid12(desc, (uint32_t)initial_state) ||
        !resource_alignment12(desc, &required_alignment) || !resource_clear_value_valid12(desc, clear)) return E_INVALIDARG;
    if (desc->SampleCount > 1 && !texture_sample_supported12(d, desc)) return E_INVALIDARG;
    res12 *r = alloc12(sizeof *r, K12_RESOURCE, g_resource_vtbl, d);
    r->base.destroy = res_destroy;
    r->desc = *desc;
    r->heap = *hp;
    r->heap_flags = heap_flags;
    r->residency_refs = in ? 0 : 1;
    r->residency_priority = desc->Flags & 0x7u ? VX12_RESIDENCY_PRIORITY_HIGH : VX12_RESIDENCY_PRIORITY_NORMAL;
    r->owner_heap = in;
    if (in) d3d_addref(&in->base);
    r->initial_state = (uint32_t)initial_state;
    r->initial_promoted = 0;
    if (desc->Dimension == 1) {                                    // D3D12_RESOURCE_DIMENSION_BUFFER
        NSUInteger len = desc->Width ? desc->Width : 1;
        if (reserved) {
            if (@available(macOS 26.0, *))
                r->buffer = [d->mtl newBufferWithLength:(len + 0xffff) & ~(NSUInteger)0xffff
                                                options:MTLResourceStorageModePrivate placementSparsePageSize:MTLSparsePageSize64];
        } else if (in) r->buffer = [in->heap newBufferWithLength:len options:MTLResourceStorageModeShared | MTLResourceHazardTrackingModeTracked offset:offset];
        else r->buffer = [d->mtl newBufferWithLength:len options:MTLResourceStorageModeShared];
        if (!r->buffer) { d3d_release(&r->base); return in ? E_INVALIDARG : E_OUTOFMEMORY; }
        if (getenv("VXRT_D3D12_TRACE_UPLOAD") && hp->Type == 2) {
            static volatile int n;
            if (__sync_add_and_fetch(&n, 1) <= 5000)
                vx_log("D3D12: buffer %#llx (%llu bytes, heap type %d) %s %p offset %llu, contents %p", (unsigned long long)r->buffer.gpuAddress,
                       (unsigned long long)desc->Width, hp->Type, in ? "placed in" : "committed", in, (unsigned long long)offset, r->buffer.contents);
        }
        void *backend = (void *)CFBridgingRetain(r->buffer);
        r->gpu_buffer = gpu_ir_buffer_create(r->buffer.length, backend, gpu12_release_mtl_object);
        if (!r->gpu_buffer) {
            gpu12_release_mtl_object(backend);
            d3d_release(&r->base);
            return E_OUTOFMEMORY;
        }
        if (r->buffer.contents) {                                  // (reserved buffers are GPU-only)
            vx_guest_forget(r->buffer.contents, r->buffer.length);
            if (!in && hp->Type != 2) memset(r->buffer.contents, 0, r->buffer.length);
        }   // (default and readback heaps start zeroed)
    } else {
        MTLTextureDescriptor *td = texture_descriptor(d, desc);
        if (!td) { d3d_release(&r->base); return E_INVALIDARG; }
        int texture_exception = 0;
        @try {
            if (reserved) {
                if (@available(macOS 26.0, *)) {
                    td.placementSparsePageSize = MTLSparsePageSize64;
                    r->texture = [d->mtl newTextureWithDescriptor:td];
                }
            } else if (in && !(in->heap.storageMode == MTLStorageModeShared && td.textureType == MTLTextureType3D && no_shared_3d12())) {
                td.storageMode = in->heap.storageMode;
                td.hazardTrackingMode = MTLHazardTrackingModeTracked;
                r->texture = [in->heap newTextureWithDescriptor:td offset:offset];
            } else r->texture = [d->mtl newTextureWithDescriptor:td];   // (a 3D texture in a CPU-visible heap: its own memory)
        } @catch (NSException *exception) {
            texture_exception = 1;
            vx_log("D3D12: Metal rejected texture resource: %s",
                   exception.reason.UTF8String ? exception.reason.UTF8String : "unknown Metal exception");
        }
        if (!r->texture) { d3d_release(&r->base); return texture_exception || in ? E_INVALIDARG : E_OUTOFMEMORY; }
        if (r->desc.MipLevels == 0) r->desc.MipLevels = (uint16_t)td.mipmapLevelCount;
        uint64_t mapped_subresources = (uint64_t)r->desc.MipLevels *
            (r->desc.Dimension == 4 ? 1u : r->desc.DepthOrArraySize) * d3d12_resource_plane_count(r->desc.Format);
        if (!mapped_subresources || mapped_subresources > UINT32_MAX ||
            mapped_subresources > SIZE_MAX / sizeof *r->mapped_subresources) {
            d3d_release(&r->base);
            return E_INVALIDARG;
        }
        r->mapped_subresources = calloc((size_t)mapped_subresources, sizeof *r->mapped_subresources);
        if (!r->mapped_subresources) { d3d_release(&r->base); return E_OUTOFMEMORY; }
        r->mapped_subresource_count = (uint32_t)mapped_subresources;
        uint32_t block_width = 1, block_height = 1, bytes_per_block = bytes_per(desc->Format);
        d3d_copy_format_block(desc->Format, &block_width, &block_height, &bytes_per_block);
        gpu_ir_texture_desc ir_desc = {
            .width = (uint32_t)r->texture.width,
            .height = (uint32_t)r->texture.height,
            .depth = (uint32_t)r->texture.depth,
            .array_layers = (uint32_t)r->texture.arrayLength,
            .mip_levels = (uint32_t)r->texture.mipmapLevelCount,
            .bytes_per_texel = bytes_per(desc->Format),
            .sample_count = (uint32_t)r->texture.sampleCount,
            .is_3d = r->texture.textureType == MTLTextureType3D,
            .block_width = block_width,
            .block_height = block_height,
            .bytes_per_block = bytes_per_block
        };
        void *backend = (void *)CFBridgingRetain(r->texture);
        r->gpu_texture = gpu_ir_texture_create(&ir_desc, backend, gpu12_release_mtl_object);
        if (!r->gpu_texture) {
            gpu12_release_mtl_object(backend);
            d3d_release(&r->base);
            return E_OUTOFMEMORY;
        }
    }
    if (reserved) {
        tiling12 t;
        r->reserved = 1;
        if (!tiling12_get(r, &t) || !t.total) { d3d_release(&r->base); return E_INVALIDARG; }
        r->tile_count = t.total;
        r->tile_heaps = calloc(t.total, sizeof *r->tile_heaps);
        r->tile_offsets = calloc(t.total, sizeof *r->tile_offsets);
        if (!r->tile_heaps || !r->tile_offsets) { d3d_release(&r->base); return E_OUTOFMEMORY; }
    }
    pthread_mutex_lock(&d->lock);
    [d->resources addObject:r->buffer ? (id)r->buffer : (id)r->texture];
    // (A placement-sparse resource in a committed residency set keeps reading pages unmapped since: measured on
    // M1 Max, macOS 26. Its pages are resident through their heaps, which are in the set.)
    if (!reserved) residency_add12(d, r->buffer ? (id<MTLResource>)r->buffer : (id<MTLResource>)r->texture);
    if (r->buffer) {
        bufs_insert12(d, r);
    }
    pthread_mutex_unlock(&d->lock);
    if (!out) { d3d_release(&r->base); return S_FALSE; }
    HRESULT hr = res_query(r, iid, out);
    d3d_release(&r->base);
    return hr;
}

static HRESULT create_committed(dev12 *d, const HEAP_PROPS *hp, int32_t heap_flags, const RES_DESC *desc, int32_t state, const void *clear, const GUID *iid, void **out) {
    return create_resource(d, hp, heap_flags, desc, state, clear, NULL, 0, 0, iid, out);
}

// ---- heaps and placed resources -----------------------------------------------------------------------------------

static void memheap_destroy(d3d_object *o) {
    memheap12 *m = (memheap12 *)o;
    if (m->dev && m->dev->tiled_tier && m->heap) {
        pthread_mutex_lock(&m->dev->lock);
        residency_remove_allocation12(m->dev, m->heap);
        pthread_mutex_unlock(&m->dev->lock);
    }
    m->heap = nil;
    release_dev(o);
}
static WINAPI HRESULT memheap_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12Heap, &IID_ID3D12Heap1, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 5);
}
static WINAPI MEMHEAP_DESC *memheap_get_desc(void *this, MEMHEAP_DESC *out) { *out = ((memheap12 *)this)->desc; return out; }
static WINAPI HRESULT memheap_session(void *this, const GUID *iid, void **out) { if (out) *out = NULL; return E_NOINTERFACE; }

// D3D12's alignments (64 KiB, 4 MiB for multisampled textures) are larger than Metal's, so offsets a program
// computes with them are valid in a Metal heap too. Small-resource eligibility follows the public D3D12 estimate:
// build near-equilateral power-of-two tiles from the smaller alignment restriction, then allow at most 16 tiles for
// ordinary textures or 64 tiles for MSAA textures. Array layers are separate subresources and do not multiply the
// estimate for the most-detailed mip.
static uint32_t resource_floor_log2_12(uint64_t value) {
    uint32_t result = 0;
    while (value >>= 1) result++;
    return result;
}

static int resource_tile_shape12(uint64_t tile_size, uint32_t bytes_per_element, uint32_t dimensions,
                                uint32_t shape[3]) {
    if (!bytes_per_element || tile_size < bytes_per_element || !dimensions || dimensions > 3 || !shape) return 0;
    uint64_t elements = tile_size / bytes_per_element;
    uint32_t total_log2 = resource_floor_log2_12(elements);
    uint32_t exponents[3] = { total_log2, 0, 0 };
    if (dimensions == 2) {
        uint32_t best_spread = UINT32_MAX;
        for (uint32_t x = 0; x <= total_log2; x++) {
            uint32_t y = total_log2 - x;
            uint32_t spread = x > y ? x - y : y - x;
            if (spread < best_spread) {
                exponents[0] = x;
                exponents[1] = y;
                best_spread = spread;
            }
        }
    } else if (dimensions == 3) {
        uint32_t best_spread = UINT32_MAX;
        for (uint32_t x = 0; x <= total_log2; x++) {
            for (uint32_t y = 0; y <= total_log2 - x; y++) {
                uint32_t z = total_log2 - x - y;
                uint32_t min = x < y ? x : y;
                uint32_t max = x > y ? x : y;
                if (z < min) min = z;
                if (z > max) max = z;
                uint32_t spread = max - min;
                if (spread < best_spread) {
                    exponents[0] = x;
                    exponents[1] = y;
                    exponents[2] = z;
                    best_spread = spread;
                }
            }
        }
    }
    // Stable, widest-to-deepest ordering makes the generated tile dimensions deterministic when axes are tied.
    for (uint32_t i = 0; i < dimensions; i++) {
        for (uint32_t j = i + 1; j < dimensions; j++) {
            if (exponents[j] > exponents[i]) {
                uint32_t t = exponents[i]; exponents[i] = exponents[j]; exponents[j] = t;
            }
        }
    }
    shape[0] = 1u << exponents[0];
    shape[1] = dimensions > 1 ? 1u << exponents[1] : 1;
    shape[2] = dimensions > 2 ? 1u << exponents[2] : 1;
    return 1;
}

static uint64_t resource_axis_tile_count12(uint64_t extent, uint32_t block_extent, uint32_t tile_extent) {
    if (!extent || !block_extent || !tile_extent) return 0;
    uint64_t blocks = extent / block_extent + (extent % block_extent != 0);
    return blocks / tile_extent + (blocks % tile_extent != 0);
}

static int small_texture12(const RES_DESC *desc, uint64_t tile_size, int multisampled) {
    if (desc->Dimension < 2 || desc->Dimension > 4) return 0; // 1D, 2D, or 3D texture.
    if (desc->Layout != D3D12_LAYOUT_UNKNOWN12 || !desc->Width || !desc->DepthOrArraySize || !desc->Format) return 0;
    if (desc->Dimension == 2 && desc->Height != 1) return 0;
    if (desc->Dimension >= 3 && !desc->Height) return 0;
    if (multisampled) {
        if (desc->Dimension != 3 || !(desc->Flags & 0x3) ||
            (desc->SampleCount != 2 && desc->SampleCount != 4 && desc->SampleCount != 8 && desc->SampleCount != 16)) return 0;
    } else if (desc->SampleCount != 1 || (desc->Flags & 0x3)) return 0;

    uint32_t block_width = 1, block_height = 1, bytes_per_block = 0;
    if (!d3d_copy_format_block(desc->Format, &block_width, &block_height, &bytes_per_block) ||
        !block_width || !block_height || !bytes_per_block) return 0;
    uint64_t bytes_per_element = (uint64_t)bytes_per_block * (multisampled ? desc->SampleCount : 1u);
    if (!bytes_per_element || bytes_per_element > UINT32_MAX) return 0;
    uint32_t tile[3];
    uint32_t dimensions = desc->Dimension - 1; // Buffer=1, Texture1D=2, Texture2D=3, Texture3D=4.
    if (!resource_tile_shape12(tile_size, (uint32_t)bytes_per_element, dimensions, tile)) return 0;

    uint64_t count[3] = {
        resource_axis_tile_count12(desc->Width, block_width, tile[0]),
        dimensions > 1 ? resource_axis_tile_count12(desc->Height, block_height, tile[1]) : 1,
        dimensions > 2 ? resource_axis_tile_count12(desc->DepthOrArraySize, 1, tile[2]) : 1
    };
    uint64_t max_tiles = tile_size == (4u << 10) ? 16 : tile_size == (64u << 10) ? 64 : 0;
    if (!max_tiles) return 0;
    uint64_t total = 1;
    for (uint32_t i = 0; i < dimensions; i++) {
        if (!count[i] || count[i] > max_tiles / total) return 0;
        total *= count[i];
    }
    return total <= max_tiles;
}

static int resource_flags_valid12(const RES_DESC *desc) {
    const uint32_t flags = (uint32_t)desc->Flags;
    const uint32_t supported_flags = 0x1u | 0x2u | 0x4u | 0x8u | 0x20u;
    // Cross-adapter sharing, video reference resources, and acceleration structures have no backend path.
    if (flags & ~supported_flags) return 0;
    if (desc->Dimension == 1 && (flags & (0x1u | 0x2u))) return 0;
    if ((flags & 0x2u) && (flags & (0x1u | 0x4u | 0x20u))) return 0;
    if ((flags & 0x8u) && !(flags & 0x2u)) return 0;
    if ((flags & 0x20u) && desc->Dimension == 1) return 0;
    return 1;
}

static int resource_alignment12(const RES_DESC *desc, uint64_t *alignment) {
    if (!desc || !alignment || !desc->Width || !desc->SampleCount || desc->SampleQuality ||
        !resource_flags_valid12(desc)) return 0;
    // Buffers expose linear bytes; texture layouts other than UNKNOWN need row-major, tiled, or swizzle
    // support that this Metal backend does not advertise.
    // 64KB_UNDEFINED_SWIZZLE (the layout of reserved textures, also allowed for placed and committed ones) leaves the
    // layout to the driver, as UNKNOWN does.
    if ((desc->Dimension == 1 && desc->Layout != D3D12_LAYOUT_ROW_MAJOR12) ||
        (desc->Dimension >= 2 && desc->Dimension <= 4 && desc->Layout != D3D12_LAYOUT_UNKNOWN12 &&
         desc->Layout != D3D12_LAYOUT_64KB_UNDEFINED_SWIZZLE12)) return 0;
    if (desc->SampleCount > 16) return 0;
    if (desc->SampleCount > 1 &&
        ((desc->SampleCount & (desc->SampleCount - 1)) || desc->Dimension != 3 || desc->MipLevels != 1 ||
         !(desc->Flags & 0x3) || (desc->Flags & (0x4 | 0x20)))) return 0;
    uint64_t requested = desc->Alignment;
    if (!requested) requested = desc->SampleCount > 1 ? (4u << 20) : (64u << 10);
    if (requested != (4u << 10) && requested != (64u << 10) && requested != (4u << 20)) return 0;
    if (desc->Dimension == 1) {
        if (desc->SampleCount != 1 || requested != (64u << 10)) return 0;
    } else if (desc->Dimension >= 2 && desc->Dimension <= 4) {
        if (desc->SampleCount > 1) {
            if (requested == (4u << 10) ||
                (requested == (64u << 10) && !small_texture12(desc, 64u << 10, 1))) return 0;
        } else if (requested == (4u << 10) && !small_texture12(desc, 4u << 10, 0)) return 0;
    } else return 0;
    *alignment = requested;
    return 1;
}

static int align_up_resource12(uint64_t value, uint64_t alignment, uint64_t *result) {
    uint64_t remainder = value % alignment;
    uint64_t padding = remainder ? alignment - remainder : 0;
    if (value > UINT64_MAX - padding) return 0;
    *result = value + padding;
    return 1;
}

// A 3D texture placed in a heap of shared (CPU-visible) memory - D3D12's heaps for buffers and textures alike are made
// so - faults the GPU when a shader writes it ("Insufficient Memory", Cyberpunk 2077's volumetric fog): it gets
// memory of its own instead (VXRT_D3D12_SHARED_3D=1 keeps it in the heap).
static int no_shared_3d12(void) { static int v = -1; if (v < 0) { const char *e = getenv("VXRT_D3D12_SHARED_3D"); v = !(e && *e == '1'); } return v; }
static ALLOC_INFO allocation_info(dev12 *d, const RES_DESC *desc) {
    uint64_t requested_alignment = 0;
    if (!resource_alignment12(desc, &requested_alignment)) return (ALLOC_INFO){ UINT64_MAX, 0 };
    if (desc->SampleCount > 1 && !texture_sample_supported12(d, desc))
        return (ALLOC_INFO){ UINT64_MAX, requested_alignment };
    ALLOC_INFO a = { 0, requested_alignment };
    MTLSizeAndAlign m;
    @try {
        if (desc->Dimension == 1) m = [d->mtl heapBufferSizeAndAlignWithLength:desc->Width ? desc->Width : 1 options:MTLResourceStorageModeShared];
        else {
            MTLTextureDescriptor *td = texture_descriptor(d, desc);
            if (!td) return (ALLOC_INFO){ UINT64_MAX, a.Alignment };
            m = [d->mtl heapTextureSizeAndAlignWithDescriptor:td];
        }
    } @catch (NSException *exception) {
        (void)exception;
        return (ALLOC_INFO){ UINT64_MAX, a.Alignment };
    }
    // Report one of D3D12's public alignment values even if Metal asks for an intermediate alignment.
    if (m.align > a.Alignment) {
        if (m.align <= (64u << 10) && a.Alignment <= (64u << 10)) a.Alignment = 64u << 10;
        else if (m.align <= (4u << 20) && a.Alignment <= (4u << 20)) a.Alignment = 4u << 20;
        else return (ALLOC_INFO){ UINT64_MAX, a.Alignment };
    }
    if (!align_up_resource12(m.size, a.Alignment, &a.SizeInBytes)) return (ALLOC_INFO){ UINT64_MAX, a.Alignment };
    return a;
}

static WINAPI ALLOC_INFO *dev_allocation_info(void *this, ALLOC_INFO *out, uint32_t mask, uint32_t n, const RES_DESC *descs) {
    if (mask) { *out = (ALLOC_INFO){ UINT64_MAX, 0 }; return out; }
    ALLOC_INFO all = { 0, 0 };
    for (uint32_t i = 0; i < n; i++) {
        ALLOC_INFO a = allocation_info(this, &descs[i]);
        if (a.SizeInBytes == UINT64_MAX) { *out = a; return out; }
        if (a.Alignment > all.Alignment) all.Alignment = a.Alignment;
        uint64_t at;
        if (!align_up_resource12(all.SizeInBytes, a.Alignment, &at) || at > UINT64_MAX - a.SizeInBytes) {
            *out = (ALLOC_INFO){ UINT64_MAX, all.Alignment };
            return out;
        }
        all.SizeInBytes = at + a.SizeInBytes;
    }
    *out = all;
    return out;
}
static WINAPI ALLOC_INFO *dev_allocation_info1(void *this, ALLOC_INFO *out, uint32_t mask, uint32_t n, const RES_DESC *descs, ALLOC_INFO *each) {
    if (mask) { *out = (ALLOC_INFO){ UINT64_MAX, 0 }; return out; }
    // (each: D3D12_RESOURCE_ALLOCATION_INFO1 {Offset, Alignment, SizeInBytes})
    ALLOC_INFO all = { 0, 0 };
    for (uint32_t i = 0; i < n; i++) {
        ALLOC_INFO a = allocation_info(this, &descs[i]);
        if (a.SizeInBytes == UINT64_MAX) { *out = a; return out; }
        if (a.Alignment > all.Alignment) all.Alignment = a.Alignment;
        uint64_t at;
        if (!align_up_resource12(all.SizeInBytes, a.Alignment, &at) || at > UINT64_MAX - a.SizeInBytes) {
            *out = (ALLOC_INFO){ UINT64_MAX, all.Alignment };
            return out;
        }
        if (each) { uint64_t *e = (uint64_t *)each + 3 * i; e[0] = at; e[1] = a.Alignment; e[2] = a.SizeInBytes; }
        all.SizeInBytes = at + a.SizeInBytes;
    }
    *out = all;
    return out;
}

static WINAPI HRESULT dev_create_memheap(void *this, const MEMHEAP_DESC *desc, const GUID *iid, void **out) {
    dev12 *d = this;
    if (!desc || !desc->SizeInBytes || (desc->Flags & 0x8) || // ALLOW_DISPLAY is committed-resource-only
        (desc->Alignment && desc->Alignment != (64u << 10) && desc->Alignment != (4u << 20))) return E_INVALIDARG;
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.size = desc->SizeInBytes;
    // Upload and readback heaps, and heaps that may hold buffers, are shared memory (a buffer's contents are read and
    // written by the program); a heap only for textures is private.
    int tex_only = (desc->Flags & 0x4) != 0;                       // D3D12_HEAP_FLAG_DENY_BUFFERS
    hd.storageMode = tex_only && desc->Properties.Type != 2 && desc->Properties.Type != 3 ? MTLStorageModePrivate : MTLStorageModeShared;
    hd.hazardTrackingMode = MTLHazardTrackingModeTracked;
    // (any heap may back reserved resources' tiles: placement-sparse pages of 64 KiB)
    if (d->tiled_tier) if (@available(macOS 26.0, *)) hd.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
    id<MTLHeap> h = [d->mtl newHeapWithDescriptor:hd];
    if (vx_trace || getenv("VXRT_D3D12_TRACE_MEMORY")) {
        static uint64_t total;
        total += desc->SizeInBytes;
        vx_log("D3D12: heap of %llu MB (type %d, flags %#x): %llu MB in heaps", (unsigned long long)(desc->SizeInBytes >> 20),
               desc->Properties.Type, desc->Flags, (unsigned long long)(total >> 20));
    }
    if (!h) { vx_log("D3D12: a heap of %llu bytes could not be made", (unsigned long long)desc->SizeInBytes); return E_OUTOFMEMORY; }
    if (!out) return S_FALSE;
    memheap12 *m = alloc12(sizeof *m, K12_MEMHEAP, g_memheap_vtbl, d);
    m->base.destroy = memheap_destroy;
    m->desc = *desc;
    m->residency_refs = 1;
    m->residency_priority = VX12_RESIDENCY_PRIORITY_NORMAL;
    if (!m->desc.Alignment) m->desc.Alignment = 64u << 10;
    m->heap = h;
    if (d->tiled_tier) {                                           // (tiles mapped from it are reached through no resource of its own)
        pthread_mutex_lock(&d->lock);
        residency_add_allocation12(d, h);
        pthread_mutex_unlock(&d->lock);
    }
    HRESULT hr = memheap_query(m, iid, out);
    d3d_release(&m->base);
    return hr;
}
static WINAPI HRESULT dev_create_memheap1(void *this, const MEMHEAP_DESC *desc, void *session, const GUID *iid, void **out) {
    if (session) { if (out) *out = NULL; return DXGI_ERROR_UNSUPPORTED; }
    return dev_create_memheap(this, desc, iid, out);
}

// D3D12_HEAP_FLAGS constrain the resource classes a placed resource may use. These bits are
// also used by the ALLOW_ONLY_* aliases, so check the individual deny bits rather than comparing
// the full flag value. Committed resources get their implicit heap restrictions from D3D12.
static int heap_flags_allow_placed_resource12(int32_t heap_flags, const RES_DESC *desc) {
    if (!desc) return 0;
    uint32_t flags = (uint32_t)heap_flags;
    if (desc->Dimension == 1) return (flags & 0x4u) == 0;          // DENY_BUFFERS
    if (desc->Dimension < 2 || desc->Dimension > 4) return 0;    // texture1D/2D/3D only
    if (desc->Flags & 0x3) return (flags & 0x40u) == 0;           // DENY_RT_DS_TEXTURES
    return (flags & 0x80u) == 0;                                 // DENY_NON_RT_DS_TEXTURES
}

static WINAPI HRESULT dev_create_placed(void *this, void *heap, uint64_t offset, const RES_DESC *desc, int32_t state, const void *clear, const GUID *iid, void **out) {
    memheap12 *m = heap;
    if (!m || m->base.kind != K12_MEMHEAP || !desc) return E_INVALIDARG;
    if (!heap_flags_allow_placed_resource12(m->desc.Flags, desc)) return E_INVALIDARG;
    uint64_t required_alignment = 0;
    if (!resource_alignment12(desc, &required_alignment) || offset % required_alignment) return E_INVALIDARG;
    dev12 *d = this;
    MTLTextureDescriptor *td = desc->Dimension == 1 ? nil : texture_descriptor(d, desc);
    if (desc->Dimension != 1 && !td) return E_INVALIDARG;
    MTLSizeAndAlign ma = td ? [d->mtl heapTextureSizeAndAlignWithDescriptor:td]
                            : [d->mtl heapBufferSizeAndAlignWithLength:desc->Width ? desc->Width : 1 options:MTLResourceStorageModeShared];
    if (offset % ma.align || offset > m->desc.SizeInBytes || ma.size > m->desc.SizeInBytes - offset) {
        vx_log("D3D12: a resource placed at %llu does not fit in its heap of %llu bytes", (unsigned long long)offset, (unsigned long long)m->desc.SizeInBytes);
        return E_INVALIDARG;
    }
    if (desc->Dimension == 1 && m->heap.storageMode != MTLStorageModeShared) return E_INVALIDARG;
    return create_resource(this, &m->desc.Properties, m->desc.Flags, desc, state, clear, m, offset, 0, iid, out);
}
static WINAPI HRESULT dev_create_placed1(void *this, void *heap, uint64_t offset, const RES_DESC1 *desc1,
                                         int32_t state, const void *clear, const GUID *iid, void **out) {
    if (!desc1) { if (out) *out = NULL; return E_INVALIDARG; }
    if (desc1->sampler_feedback_mip_region.Width || desc1->sampler_feedback_mip_region.Height ||
        desc1->sampler_feedback_mip_region.Depth) {
        if (out) *out = NULL;
        return DXGI_ERROR_UNSUPPORTED;
    }
    return dev_create_placed(this, heap, offset, &desc1->resource, state, clear, iid, out);
}

// ---- reserved resources and tile mappings ---------------------------------------------------------------------------

static HRESULT create_reserved12(dev12 *d, const RES_DESC *desc, int32_t state, const void *clear, const GUID *iid, void **out) {
    if (out) *out = NULL;
    if (!desc) return E_INVALIDARG;
    // Tier 2: buffers and single-sample 2D textures (arrays too) in the 64KB_UNDEFINED_SWIZZLE layout; 3D tiles
    // (tier 3) and multisampled tiles are not offered.
    if (d->tiled_tier < 2) return E_INVALIDARG;
    if (desc->Dimension == 1 ? desc->Layout != D3D12_LAYOUT_ROW_MAJOR12 :
        desc->Dimension != 3 || desc->Layout != D3D12_LAYOUT_64KB_UNDEFINED_SWIZZLE12 || desc->SampleCount != 1)
        return E_INVALIDARG;
    if (desc->Alignment && desc->Alignment != TILE_BYTES12) return E_INVALIDARG;
    HEAP_PROPS hp = { 1, 0, 0, 1, 1 };                            // (reserved resources have no heap of their own)
    if (vx_trace || getenv("VXRT_D3D12_TRACE_MEMORY"))
        vx_log("D3D12: reserved %s %llux%ux%u format %u mips %u", desc->Dimension == 1 ? "buffer" : "texture",
               (unsigned long long)desc->Width, desc->Height, desc->DepthOrArraySize, desc->Format, desc->MipLevels);
    return create_resource(d, &hp, 0, desc, state, clear, NULL, 0, 1, iid, out);
}
static WINAPI HRESULT dev_create_reserved(void *this, const RES_DESC *desc, int32_t state, const void *clear, const GUID *iid, void **out) {
    return create_reserved12(this, desc, state, clear, iid, out);
}
static WINAPI HRESULT dev_create_reserved1(void *this, const RES_DESC *desc, int32_t state, const void *clear,
                                           void *protected_session, const GUID *iid, void **out) {
    if (protected_session) { if (out) *out = NULL; return DXGI_ERROR_UNSUPPORTED; }
    return create_reserved12(this, desc, state, clear, iid, out);
}

// GetResourceTiling(resource, *NumTilesForEntireResource, *PackedMipDesc, *StandardTileShapeForNonPackedMips,
// *NumSubresourceTilings (in: how many wanted, out: how many written), FirstSubresourceTilingToGet, *SubresourceTilings)
typedef struct { uint8_t NumStandardMips, NumPackedMips; uint32_t NumTilesForPackedMips, StartTileIndexInOverallResource; } PACKED_MIP_INFO12;
typedef struct { uint32_t WidthInTexels, HeightInTexels, DepthInTexels; } TILE_SHAPE12;
typedef struct { uint32_t WidthInTiles; uint16_t HeightInTiles, DepthInTiles; uint32_t StartTileIndexInOverallResource; } SUBRESOURCE_TILING12;
static WINAPI void dev_get_resource_tiling(void *this, void *resource, uint32_t *total, PACKED_MIP_INFO12 *packed,
                                           TILE_SHAPE12 *shape, uint32_t *count, uint32_t first, SUBRESOURCE_TILING12 *tilings) {
    res12 *r = resource;
    tiling12 t;
    if (!r || r->base.kind != K12_RESOURCE || !tiling12_get(r, &t)) {
        // (not a reserved resource: no tiles)
        if (total) *total = 0;
        if (packed) memset(packed, 0, sizeof *packed);
        if (shape) memset(shape, 0, sizeof *shape);
        if (count) *count = 0;
        return;
    }
    if (total) *total = t.total;
    if (packed) {
        packed->NumStandardMips = (uint8_t)t.std_mips;
        packed->NumPackedMips = (uint8_t)t.packed_mips;
        packed->NumTilesForPackedMips = t.packed_tiles;
        packed->StartTileIndexInOverallResource = t.per_slice - t.packed_tiles;
    }
    if (shape) { shape->WidthInTexels = t.shape[0]; shape->HeightInTexels = t.shape[1]; shape->DepthInTexels = t.shape[2]; }
    if (count) {
        uint32_t subresources = t.mips * t.slices, n = 0;
        for (uint32_t i = first; i < subresources && n < *count; i++, n++) {
            uint32_t mip = i % t.mips, slice = i / t.mips;
            if (!tilings) continue;
            if (mip >= t.std_mips) tilings[n] = (SUBRESOURCE_TILING12){ 0, 0, 0, 0xffffffffu };   // D3D12_PACKED_TILE
            else tilings[n] = (SUBRESOURCE_TILING12){ t.mip_tiles[mip][0], (uint16_t)t.mip_tiles[mip][1], 1,
                                                      slice * t.per_slice + t.mip_start[mip] };
        }
        *count = n;
    }
}

typedef struct { uint32_t X, Y, Z, Subresource; } TILE_COORD12;               // D3D12_TILED_RESOURCE_COORDINATE
typedef struct { uint32_t NumTiles; int32_t UseBox; uint32_t Width; uint16_t Height, Depth; } TILE_REGION12;   // D3D12_TILE_REGION_SIZE

// The tile indices one region names, in D3D12's order: a box walks X, Y, Z within one subresource; otherwise
// NumTiles tiles from the start coordinate on in the resource's tile order. Returns how many were appended, or -1.
static int tiling12_region(const tiling12 *t, const TILE_COORD12 *start, const TILE_REGION12 *size,
                           uint32_t **list, uint32_t *n, uint32_t *cap) {
    TILE_REGION12 one = { 1, 0, 1, 1, 1 };
    if (!size) size = &one;
    uint32_t first = tiling12_index(t, start->X, start->Y, start->Z, start->Subresource);
    if (first == UINT32_MAX) return -1;
    uint32_t want = size->NumTiles;
    if (size->UseBox) {
        if ((uint64_t)size->Width * size->Height * size->Depth != size->NumTiles || size->Depth != 1) return -1;
    } else if ((uint64_t)first + want > t->total) return -1;
    if (*n + (uint64_t)want > *cap) {
        uint32_t c = *cap ? *cap : 64;
        while (c < *n + (uint64_t)want) c *= 2;
        uint32_t *grown = realloc(*list, (size_t)c * sizeof **list);
        if (!grown) return -1;
        *list = grown;
        *cap = c;
    }
    if (!size->UseBox) {
        for (uint32_t i = 0; i < want; i++) (*list)[(*n)++] = first + i;
        return (int)want;
    }
    for (uint32_t y = 0; y < size->Height; y++)
        for (uint32_t x = 0; x < size->Width; x++) {
            uint32_t index = tiling12_index(t, start->X + x, start->Y + y, start->Z, start->Subresource);
            if (index == UINT32_MAX) return -1;
            (*list)[(*n)++] = index;
        }
    return (int)want;
}

// Applies new mappings (heap NULL: unmapped) to tiles of one resource on the Metal 4 queue, ordered with the D3D12
// queue's work: the mapping waits for what the queue was given before and what it is given afterwards waits for it.
static void tile_mappings_apply12(queue12 *q, res12 *r, const tiling12 *t, const uint32_t *tiles,
                                  memheap12 *const *heaps, const uint32_t *offsets, uint32_t n) {   // (submit_lock)
    dev12 *d = q->dev;
    if (@available(macOS 26.0, *)) {
        if (!d->tile_queue) d->tile_queue = [d->mtl newMTL4CommandQueue];
        id<MTL4CommandQueue> tq = d->tile_queue;
        if (!tq) { vx_log("D3D12: no Metal 4 queue for tile mappings"); return; }
        id<MTLCommandBuffer> before = [q->mtl_queue commandBuffer];
        uint64_t ready = ++q->timeline_value;
        [before encodeSignalEvent:q->timeline value:ready];
        [before commit];
        [tq waitForEvent:q->timeline value:ready];
        // Runs of tiles sharing a heap go in one call (Metal takes one heap per call); an unmap names the heap the
        // tile was mapped from (Metal ignores one given no heap), and a tile mapped nowhere needs none.
        for (uint32_t i = 0; i < n;) {
            memheap12 *heap = heaps[i] ? heaps[i] : r->tile_heaps[tiles[i]];
            int map = heaps[i] != NULL;
            uint32_t j = i;
            while (j < n && (heaps[j] != NULL) == map && (heaps[j] ? heaps[j] : r->tile_heaps[tiles[j]]) == heap) j++;
            if (!heap) { i = j; continue; }
            MTLSparseTextureMappingMode mode = map ? MTLSparseTextureMappingModeMap : MTLSparseTextureMappingModeUnmap;
            if (r->buffer) {
                MTL4UpdateSparseBufferMappingOperation *ops = calloc(j - i, sizeof *ops);
                for (uint32_t k = i; k < j; k++)
                    ops[k - i] = (MTL4UpdateSparseBufferMappingOperation){ mode, NSMakeRange(tiles[k], 1), map ? offsets[k] : 0 };
                [tq updateBufferMappings:r->buffer heap:heap->heap operations:ops count:j - i];
                free(ops);
            } else {
                MTL4UpdateSparseTextureMappingOperation *ops = calloc(j - i, sizeof *ops);
                for (uint32_t k = i; k < j; k++) {
                    uint32_t mip, slice, x, y;
                    tiling12_locate(t, tiles[k], &mip, &slice, &x, &y);
                    ops[k - i] = (MTL4UpdateSparseTextureMappingOperation){ mode, MTLRegionMake2D(x, y, 1, 1), mip, slice,
                                                                           map ? offsets[k] : 0 };
                }
                [tq updateTextureMappings:r->texture heap:heap->heap operations:ops count:j - i];
                free(ops);
            }
            i = j;
        }
        uint64_t mapped = ++q->timeline_value;
        [tq signalEvent:q->timeline value:mapped];
        id<MTLCommandBuffer> after = [q->mtl_queue commandBuffer];
        [after encodeWaitForEvent:q->timeline value:mapped];
        [after commit];
    }
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = tiles[k];
        if (heaps[k]) d3d_addref(&heaps[k]->base);
        if (r->tile_heaps[i]) d3d_release(&r->tile_heaps[i]->base);
        r->tile_heaps[i] = heaps[k];
        r->tile_offsets[i] = heaps[k] ? offsets[k] : 0;
    }
}

enum { TILE_RANGE_NULL12 = 1, TILE_RANGE_SKIP12 = 2, TILE_RANGE_REUSE_SINGLE_TILE12 = 4 };

static WINAPI void queue_update_tile_mappings(void *this, void *resource, uint32_t nregions, const TILE_COORD12 *coords,
                                              const TILE_REGION12 *sizes, void *heap, uint32_t nranges,
                                              const int32_t *range_flags, const uint32_t *heap_starts,
                                              const uint32_t *range_counts, int32_t flags) {
    queue_drain12((queue12 *)this);
    queue12 *q = this;
    res12 *r = resource;
    memheap12 *m = heap;
    tiling12 t;
    if (!r || r->base.kind != K12_RESOURCE || !tiling12_get(r, &t) || (m && m->base.kind != K12_MEMHEAP)) {
        vx_log("D3D12: UpdateTileMappings on something that is not a reserved resource");
        return;
    }
    uint32_t *tiles = NULL, ntiles = 0, cap = 0;
    TILE_COORD12 origin = { 0, 0, 0, 0 };
    if (!nregions) {                                               // (no regions: the whole resource)
        TILE_REGION12 all = { t.total, 0, 0, 0, 0 };
        if (tiling12_region(&t, &origin, &all, &tiles, &ntiles, &cap) < 0) goto bad;
    }
    for (uint32_t i = 0; i < nregions; i++)
        if (tiling12_region(&t, coords ? &coords[i] : &origin, sizes ? &sizes[i] : NULL, &tiles, &ntiles, &cap) < 0) goto bad;
    memheap12 **heaps = calloc(ntiles ? ntiles : 1, sizeof *heaps);
    uint32_t *offsets = calloc(ntiles ? ntiles : 1, sizeof *offsets), *which = calloc(ntiles ? ntiles : 1, sizeof *which);
    uint32_t napply = 0, k = 0;
    uint64_t heap_tiles = m ? m->desc.SizeInBytes / TILE_BYTES12 : 0;
    for (uint32_t range = 0; range < nranges && k < ntiles; range++) {
        int32_t f = range_flags ? range_flags[range] : 0;
        // (no counts: one range covers every tile named; otherwise one tile a range)
        uint32_t count = range_counts ? range_counts[range] : nranges == 1 ? ntiles : 1;
        uint32_t start = heap_starts ? heap_starts[range] : 0;
        for (uint32_t c = 0; c < count && k < ntiles; c++, k++) {
            if (f & TILE_RANGE_SKIP12) continue;
            memheap12 *target = (f & TILE_RANGE_NULL12) ? NULL : m;
            uint32_t offset = (f & TILE_RANGE_REUSE_SINGLE_TILE12) ? start : start + c;
            if (target && (!heap_starts || offset >= heap_tiles)) {
                vx_log("D3D12: UpdateTileMappings range beyond its heap (tile %u of %llu)", offset, (unsigned long long)heap_tiles);
                target = NULL;
            }
            which[napply] = tiles[k];
            heaps[napply] = target;
            offsets[napply++] = offset;
        }
    }
    pthread_mutex_lock(&q->dev->submit_lock);
    tile_mappings_apply12(q, r, &t, which, heaps, offsets, napply);
    pthread_mutex_unlock(&q->dev->submit_lock);
    free(heaps); free(offsets); free(which); free(tiles);
    return;
bad:
    vx_log("D3D12: UpdateTileMappings region outside the resource; ignored");
    free(tiles);
}

static WINAPI void queue_copy_tile_mappings(void *this, void *dst_resource, const TILE_COORD12 *dst_start, void *src_resource,
                                            const TILE_COORD12 *src_start, const TILE_REGION12 *size, int32_t flags) {
    queue_drain12((queue12 *)this);
    queue12 *q = this;
    res12 *dst = dst_resource, *src = src_resource;
    tiling12 dt, st;
    if (!dst || !src || dst->base.kind != K12_RESOURCE || src->base.kind != K12_RESOURCE || !dst_start || !src_start ||
        !tiling12_get(dst, &dt) || !tiling12_get(src, &st)) { vx_log("D3D12: CopyTileMappings between non-reserved resources"); return; }
    uint32_t *dtiles = NULL, *stiles = NULL, dn = 0, sn = 0, dcap = 0, scap = 0;
    if (tiling12_region(&dt, dst_start, size, &dtiles, &dn, &dcap) < 0 ||
        tiling12_region(&st, src_start, size, &stiles, &sn, &scap) < 0 || dn != sn) {
        vx_log("D3D12: CopyTileMappings region outside a resource; ignored");
        free(dtiles); free(stiles);
        return;
    }
    memheap12 **heaps = calloc(dn ? dn : 1, sizeof *heaps);
    uint32_t *offsets = calloc(dn ? dn : 1, sizeof *offsets);
    pthread_mutex_lock(&q->dev->submit_lock);
    for (uint32_t i = 0; i < dn; i++) { heaps[i] = src->tile_heaps[stiles[i]]; offsets[i] = src->tile_offsets[stiles[i]]; }
    tile_mappings_apply12(q, dst, &dt, dtiles, heaps, offsets, dn);
    pthread_mutex_unlock(&q->dev->submit_lock);
    free(heaps); free(offsets); free(dtiles); free(stiles);
}

// ---- query heaps -----------------------------------------------------------------------------------------------------

static void queryheap_destroy(d3d_object *o) { queryheap12 *q = (queryheap12 *)o; q->results = nil; q->samples = nil; release_dev(o); }
static WINAPI HRESULT queryheap_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12QueryHeap, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}

static int d3d12_copy_timestamp_counters_supported12(dev12 *device) {
    // Copy-queue timestamps are sampled between blit operations. Stage-boundary support alone only
    // covers encoder boundaries; Metal exposes the needed in-encoder sampling as AtBlitBoundary.
    if (!device || ![device->mtl supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary]) return 0;
    for (id<MTLCounterSet> set in device->mtl.counterSets)
        if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) return 1;
    return 0;
}

static WINAPI HRESULT dev_create_query_heap(void *this, const uint32_t *desc, const GUID *iid, void **out) {
    // D3D12_QUERY_HEAP_DESC: Type, Count, NodeMask
    dev12 *d = this;
    if (!desc || !desc[1]) return E_INVALIDARG;
    int32_t type = (int32_t)desc[0];
    uint32_t count = desc[1], stride = type == 2 ? 88 : type == 7 ? 112 : type == 3 ? 16 : 8;
    id<MTLCounterSampleBuffer> samples = nil;
    if (type == 1 || type == 5) {
        if (type == 5 && !d3d12_copy_timestamp_counters_supported12(d)) return E_INVALIDARG;
        id<MTLCounterSet> set = nil;
        for (id<MTLCounterSet> cs in d->mtl.counterSets) if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) set = cs;
        MTLCounterSamplingPoint sampling_point = type == 5 ? MTLCounterSamplingPointAtBlitBoundary : MTLCounterSamplingPointAtStageBoundary;
        if (!set || ![d->mtl supportsCounterSampling:sampling_point]) { vx_log("D3D12: this GPU samples no timestamps"); return E_FAIL; }
        MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
        sd.counterSet = set;
        sd.sampleCount = count;
        sd.storageMode = MTLStorageModePrivate;              // (resolved on the GPU by ResolveQueryData)
        NSError *err = nil;
        samples = [d->mtl newCounterSampleBufferWithDescriptor:sd error:&err];
        if (!samples) { vx_log("D3D12: a timestamp heap of %u: %s", count, err.localizedDescription.UTF8String); return E_OUTOFMEMORY; }
    } else if (type != 0) vx_log("D3D12: query heap type %d reads as zeros", type);
    if (!out) return S_FALSE;
    queryheap12 *q = alloc12(sizeof *q, K12_QUERYHEAP, g_queryheap_vtbl, d);
    q->base.destroy = queryheap_destroy;
    q->type = type;
    q->count = count;
    q->stride = stride;
    q->residency_refs = 1;
    q->residency_priority = VX12_RESIDENCY_PRIORITY_NORMAL;
    q->results = [d->mtl newBufferWithLength:(NSUInteger)count * stride options:MTLResourceStorageModeShared];
    memset(q->results.contents, 0, q->results.length);
    q->samples = samples;
    HRESULT hr = queryheap_query(q, iid, out);
    d3d_release(&q->base);
    return hr;
}

// ---- descriptor heaps -----------------------------------------------------------------------------------------------

typedef struct { uint64_t va, res; uint32_t size, stride, fmt, flags; } VxDesc;   // dxil_msl.h's descriptor

static pthread_mutex_t g_heaps_lock = PTHREAD_MUTEX_INITIALIZER;
static heap12 *g_heaps[4096];
static uint32_t g_nheaps;
static uint64_t g_heaps_gen12 = 1;                         // (changes with every heap added or removed)

// Sets the Metal object a descriptor stands for, keeping the heap's set of textures.
// What a shader-visible descriptor held (a texture view, a sampler state) stays alive until the GPU has finished what
// was submitted when the descriptor was overwritten: command buffers in flight may still read it through the heap.
static pthread_mutex_t g_grave_lock12 = PTHREAD_MUTEX_INITIALIZER;
static NSMutableArray *g_grave_open12;                   // what was buried since the last submission
static NSMutableArray *g_grave_bins12;                   // @[ objects, NSData of the queues' timeline values ]
// (each thread gathers 64 before taking the lock: a later bin only keeps them longer)
static __thread void *t_grave12;                   // NSMutableArray, CFBridgingRetain
static void grave_bury12(dev12 *d, id old) {
    if (!d || !old) return;
    if (!t_grave12) t_grave12 = (void *)CFBridgingRetain([NSMutableArray new]);
    NSMutableArray *mine = (__bridge NSMutableArray *)t_grave12;
    [mine addObject:old];
    if (mine.count < 64) return;
    pthread_mutex_lock(&g_grave_lock12);
    if (!g_grave_open12) g_grave_open12 = [NSMutableArray new];
    [g_grave_open12 addObjectsFromArray:mine];
    pthread_mutex_unlock(&g_grave_lock12);
    [mine removeAllObjects];
}
// (at each submission: what was buried since closes with the queues' values now; bins the GPU has passed go)
static void grave_settle12(dev12 *d) {
    pthread_mutex_lock(&g_grave_lock12);
    if (g_grave_open12.count) {
        uint64_t at[16];
        for (int i = 0; i < 16; i++) at[i] = d->queues[i] ? __atomic_load_n(&((queue12 *)d->queues[i])->timeline_value, __ATOMIC_ACQUIRE) : 0;
        if (!g_grave_bins12) g_grave_bins12 = [NSMutableArray new];
        [g_grave_bins12 addObject:@[ g_grave_open12, [NSData dataWithBytes:at length:sizeof at] ]];
        g_grave_open12 = [NSMutableArray new];
    }
    NSMutableArray *gone = nil;                                    // (released after the lock: a bin can hold thousands)
    while (g_grave_bins12.count) {
        const uint64_t *at = ((NSData *)g_grave_bins12[0][1]).bytes;
        int passed = 1;
        for (int i = 0; i < 16 && passed; i++) {
            queue12 *q = d->queues[i];
            if (q && q->timeline && q->timeline.signaledValue < at[i]) passed = 0;
        }
        if (!passed) break;
        if (!gone) gone = [NSMutableArray new];
        [gone addObject:g_grave_bins12[0]];
        [g_grave_bins12 removeObjectAtIndex:0];
    }
    pthread_mutex_unlock(&g_grave_lock12);
    gone = nil;
}
static int g_residency_set_active12;
static int no_heap_use12(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("VXRT_D3D12_HEAP_USE"); v = !(e && *e == '1'); }
    return v && g_residency_set_active12;
}
static void heap_keep12(heap12 *h, uint32_t i, id object) {
    id old = h->keep[i];
    if (old == object) return;
    h->keep[i] = object;
    if (old && h->gpu) grave_bury12(h->dev, old);
    if (!h->textures || no_heap_use12()) return;                   // (the heap's textures are never declared)
    int old_texture = old && [old conformsToProtocol:@protocol(MTLTexture)];
    int new_texture = object && [object conformsToProtocol:@protocol(MTLTexture)];
    if (!old_texture && !new_texture) return;
    pthread_mutex_lock(&h->texture_lock);
    if (old_texture) [h->textures removeObject:old];
    if (new_texture) [h->textures addObject:object];
    h->texture_list = nil;
    pthread_mutex_unlock(&h->texture_lock);
}
// The heap's textures (the array is the caller's: later writes do not change it).
static NSArray *heap_textures12(heap12 *h) {
    if (!h || !h->textures) return nil;
    pthread_mutex_lock(&h->texture_lock);
    if (!h->texture_list) h->texture_list = h->textures.allObjects;
    NSArray *list = h->texture_list;
    pthread_mutex_unlock(&h->texture_lock);
    return list;
}
// Residency for an encoder: every resource of the device (shaders reach buffers through GPU addresses) and the
// textures of the bound resource heap, each list in one call.
// What a replay passes for "resident" when the device's residency set covers every resource and heap texture.
static NSArray *resident_by_set12(void) {
    static NSArray *sentinel;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ sentinel = [NSArray new]; });
    return sentinel;
}
static void use_list_render12(id<MTLRenderCommandEncoder> re, NSArray *list) {
    NSUInteger n = list.count;
    if (!n) return;
    __unsafe_unretained id<MTLResource> *objects = (__unsafe_unretained id<MTLResource> *)malloc(n * sizeof *objects);
    if (!objects) return;
    [list getObjects:objects range:NSMakeRange(0, n)];
    [re useResources:objects count:n usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageVertex | MTLRenderStageFragment];
    free(objects);
}
static void use_list_compute12(id<MTLComputeCommandEncoder> ce, NSArray *list) {
    NSUInteger n = list.count;
    if (!n) return;
    __unsafe_unretained id<MTLResource> *objects = (__unsafe_unretained id<MTLResource> *)malloc(n * sizeof *objects);
    if (!objects) return;
    [list getObjects:objects range:NSMakeRange(0, n)];
    [ce useResources:objects count:n usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    free(objects);
}
// (With the residency set the heap's textures are still declared once an encoder: draws and dispatches do not name
// what an unbounded descriptor range reaches, so Metal's hazard tracking learns of them here.)
// The bound heap's textures are declared to an encoder (useResource) only once a shader that can reach any of them
// runs in it: one with an unbounded or large descriptor array, or SM 6.6 heap indexing. Every other shader's
// bindings are declared draw by draw (their access lists); declaring a heap of thousands of textures to every
// encoder cost more than the draws. (serials: which encoder of this thread was declared to)
static __thread uint64_t t_ce_serial12, t_ce_heap_serial12, t_re_serial12, t_re_heap_serial12;
static int msl_wide12(const dxil_msl *m) {
    if (!m) return 0;
    if (m->uses_heap) return 1;
    for (uint32_t i = 0; i < m->nslots; i++)
        if (m->slots[i].cls != DXIL_SAMPLER && (m->slots[i].count == UINT32_MAX || m->slots[i].count > 64)) return 1;
    return 0;
}
static void use_resident_compute12(id<MTLComputeCommandEncoder> ce, NSArray *resident, heap12 *heap) {
    (void)heap;
    if (resident != resident_by_set12()) use_list_compute12(ce, resident);
    t_ce_serial12++;
}
// With the device's residency set holding every texture's memory, the heap's textures are not declared at all (a
// declaration of thousands per encoder cost Cyberpunk 2077's first scene 100 ms a frame on the CPU, and as much in
// Metal's submission); VXRT_D3D12_HEAP_USE=1 declares them as before.
static void heap_textures_compute12(id<MTLComputeCommandEncoder> ce, heap12 *heap, int wide) {
    if (!ce || !wide || t_ce_heap_serial12 == t_ce_serial12 || no_heap_use12()) return;
    use_list_compute12(ce, heap_textures12(heap));
    t_ce_heap_serial12 = t_ce_serial12;
}

static void heap_destroy(d3d_object *o) {
    heap12 *h = (heap12 *)o;
    if (h->gpu && h->dev) {
        pthread_mutex_lock(&h->dev->lock);
        [h->dev->resources removeObjectIdenticalTo:h->gpu];
        residency_remove12(h->dev, h->gpu);
        pthread_mutex_unlock(&h->dev->lock);
    }
    pthread_mutex_lock(&g_heaps_lock);
    for (uint32_t i = 0; i < g_nheaps; i++) if (g_heaps[i] == h) { g_heaps[i] = g_heaps[--g_nheaps]; break; }
    __atomic_add_fetch(&g_heaps_gen12, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_heaps_lock);
    h->textures = nil;
    h->texture_list = nil;
    pthread_mutex_destroy(&h->texture_lock);
    for (uint32_t i = 0; i < h->desc.NumDescriptors; i++) {
        if (h->keep) h->keep[i] = nil;
        if (h->buffer_view) gpu_ir_buffer_view_release(h->buffer_view[i]);
        if (h->shader_view && h->shader_view[i].resource) d3d_release(&h->shader_view[i].resource->base);
        if (h->shader_view && h->shader_view[i].counter_resource) d3d_release(&h->shader_view[i].counter_resource->base);
        if (h->target_resource && h->target_resource[i]) d3d_release(&h->target_resource[i]->base);
    }
    free((void *)h->keep);
    free(h->descriptor_kind);
    free(h->buffer_view);
    free(h->shader_view);
    free(h->target_resource);
    free(h->target_subresource);
    free(h->target_flags);
    if (!h->gpu) free(h->cpu);
    h->gpu = nil;
    release_dev(o);
}

// The heap and descriptor index of a CPU descriptor handle.
// (each thread keeps the last heaps it found, good while no heap has come or gone since: no lock, no search)
static __thread struct { uint64_t gen; size_t lo, hi; heap12 *h; } t_heap_at12[4];
static __thread uint32_t t_heap_at_next12;
static heap12 *heap_at(size_t cpu, uint32_t *index) {
    uint64_t gen = __atomic_load_n(&g_heaps_gen12, __ATOMIC_ACQUIRE);
    for (int i = 0; i < 4; i++)
        if (t_heap_at12[i].gen == gen && cpu >= t_heap_at12[i].lo && cpu < t_heap_at12[i].hi) {
            *index = (uint32_t)((cpu - t_heap_at12[i].lo) / 32);
            return t_heap_at12[i].h;
        }
    pthread_mutex_lock(&g_heaps_lock);
    gen = g_heaps_gen12;
    heap12 *found = NULL;
    for (uint32_t i = 0; i < g_nheaps; i++) {
        heap12 *h = g_heaps[i];
        if (cpu >= (size_t)h->cpu && cpu < (size_t)h->cpu + 32 * (size_t)h->desc.NumDescriptors) { found = h; *index = (uint32_t)((cpu - (size_t)h->cpu) / 32); break; }
    }
    pthread_mutex_unlock(&g_heaps_lock);
    if (found) {
        uint32_t k = t_heap_at_next12++ & 3;
        t_heap_at12[k].gen = gen;
        t_heap_at12[k].lo = (size_t)found->cpu;
        t_heap_at12[k].hi = (size_t)found->cpu + 32 * (size_t)found->desc.NumDescriptors;
        t_heap_at12[k].h = found;
    }
    return found;
}

static heap12 *heap_cpu_descriptor(size_t cpu, uint32_t *index) {
    uint32_t found_index = 0;
    heap12 *heap = heap_at(cpu, &found_index);
    if (!heap || (cpu - (size_t)heap->cpu) % 32u) return NULL;
    if (index) *index = found_index;
    return heap;
}

static heap12 *heap_gpu_descriptor(uint64_t gpu, uint32_t *index) {
    pthread_mutex_lock(&g_heaps_lock);
    heap12 *found = NULL;
    uint32_t found_index = 0;
    for (uint32_t i = 0; i < g_nheaps; i++) {
        heap12 *heap = g_heaps[i];
        if (!heap->gpu) continue;
        uint64_t base = heap->gpu.gpuAddress;
        uint64_t bytes = (uint64_t)heap->desc.NumDescriptors * 32u;
        if (gpu < base || gpu - base >= bytes || (gpu - base) % 32u) continue;
        found = heap;
        found_index = (uint32_t)((gpu - base) / 32u);
        break;
    }
    pthread_mutex_unlock(&g_heaps_lock);
    if (found && index) *index = found_index;
    return found;
}

static int heap_uav_views_match12(const heap12 *cpu_heap, uint32_t cpu_index,
                                  const heap12 *gpu_heap, uint32_t gpu_index) {
    if (!cpu_heap || !gpu_heap || cpu_index >= cpu_heap->desc.NumDescriptors ||
        gpu_index >= gpu_heap->desc.NumDescriptors ||
        !cpu_heap->descriptor_kind || !gpu_heap->descriptor_kind ||
        cpu_heap->descriptor_kind[cpu_index] != DXIL_UAV + 1 ||
        gpu_heap->descriptor_kind[gpu_index] != DXIL_UAV + 1) return 0;
    shader_view12 *cpu_view = cpu_heap->shader_view ? &cpu_heap->shader_view[cpu_index] : NULL;
    shader_view12 *gpu_view = gpu_heap->shader_view ? &gpu_heap->shader_view[gpu_index] : NULL;
    if (!cpu_view || !gpu_view || !cpu_view->resource || cpu_view->resource != gpu_view->resource ||
        cpu_view->counter_resource != gpu_view->counter_resource || cpu_view->counter_offset != gpu_view->counter_offset ||
        cpu_view->mip != gpu_view->mip || cpu_view->mips != gpu_view->mips ||
        cpu_view->slice != gpu_view->slice || cpu_view->slices != gpu_view->slices ||
        cpu_view->plane != gpu_view->plane) return 0;
    VxDesc cpu_desc, gpu_desc;
    memcpy(&cpu_desc, cpu_heap->cpu + 32ull * cpu_index, sizeof cpu_desc);
    memcpy(&gpu_desc, gpu_heap->cpu + 32ull * gpu_index, sizeof gpu_desc);
    // (res: the Metal texture view each CreateUnorderedAccessView made, different objects for the same view unless the
    // descriptor was copied; it must agree only where it is the counter's address)
    if (cpu_desc.va != gpu_desc.va || ((cpu_desc.flags & DXIL_MSL_DESC_HAS_COUNTER) && cpu_desc.res != gpu_desc.res) ||
        cpu_desc.size != gpu_desc.size || cpu_desc.stride != gpu_desc.stride ||
        cpu_desc.fmt != gpu_desc.fmt || cpu_desc.flags != gpu_desc.flags) return 0;
    gpu_ir_buffer_view *cpu_buffer_view = cpu_heap->buffer_view ? cpu_heap->buffer_view[cpu_index] : NULL;
    gpu_ir_buffer_view *gpu_buffer_view = gpu_heap->buffer_view ? gpu_heap->buffer_view[gpu_index] : NULL;
    if (!!cpu_buffer_view != !!gpu_buffer_view) return 0;
    if (cpu_buffer_view) {
        const gpu_ir_buffer_view_desc *a = gpu_ir_buffer_view_description(cpu_buffer_view);
        const gpu_ir_buffer_view_desc *b = gpu_ir_buffer_view_description(gpu_buffer_view);
        if (!a || !b || a->kind != b->kind || a->byte_offset != b->byte_offset ||
            a->byte_size != b->byte_size || a->element_count != b->element_count ||
            a->element_size != b->element_size || a->structure_stride != b->structure_stride) return 0;
    }
    return 1;
}

static WINAPI HRESULT heap_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12DescriptorHeap, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}
static WINAPI HEAP_DESC *heap_get_desc(void *this, HEAP_DESC *out) { *out = ((heap12 *)this)->desc; return out; }
static WINAPI size_t *heap_cpu_start(void *this, size_t *out) { *out = (size_t)((heap12 *)this)->cpu; return out; }
static WINAPI uint64_t *heap_gpu_start(void *this, uint64_t *out) { heap12 *h = this; *out = h->gpu ? h->gpu.gpuAddress : 0; return out; }

// Writes a descriptor and holds the Metal object it names.
// VXRT_D3D12_TRACE_RESIDENCY=1: a texture written into a descriptor whose memory is not in the residency set is logged
static void trace_residency_desc12(heap12 *h, id keep) {
    static int on = -1;
    if (on < 0) on = getenv("VXRT_D3D12_TRACE_RESIDENCY") != NULL;
    if (!on || !keep || ![keep conformsToProtocol:@protocol(MTLTexture)]) return;
    id<MTLTexture> t = keep;
    while (t.parentTexture) t = t.parentTexture;
    dev12 *d = h->dev;
    if (!d || !d->residency) return;
    id allocation = t.heap ? (id)t.heap : (id)t;
    pthread_mutex_lock(&d->lock);
    NSUInteger n = [d->resident_allocations countForObject:allocation];
    pthread_mutex_unlock(&d->lock);
    if (n) return;
    static volatile int told;
    if (__sync_add_and_fetch(&told, 1) <= 40)
        vx_log("D3D12: descriptor of a texture outside the residency set: %lux%lux%lu fmt %lu type %lu storage %lu heap %p sparse %d",
               (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth, (unsigned long)t.pixelFormat, (unsigned long)t.textureType,
               (unsigned long)t.storageMode, (__bridge void *)t.heap, (int)t.isSparse);
}
static void put_desc(size_t cpu, const VxDesc *v, id keep, int32_t descriptor_class) {
    uint32_t i;
    heap12 *h = heap_at(cpu, &i);
    if (!h) { vx_log("D3D12: a descriptor written outside any heap (%#zx)", cpu); return; }
    trace_residency_desc12(h, keep);
    memcpy(h->cpu + 32 * (size_t)i, v, 32);
    heap_keep12(h, i, keep);
    if (h->descriptor_kind) h->descriptor_kind[i] = descriptor_class >= DXIL_SRV && descriptor_class <= DXIL_SAMPLER
        ? (uint8_t)(descriptor_class + 1) : 0;
    if (h->buffer_view && h->buffer_view[i]) {
        gpu_ir_buffer_view_release(h->buffer_view[i]);
        h->buffer_view[i] = NULL;
    }
    if (h->shader_view && h->shader_view[i].resource) {
        d3d_release(&h->shader_view[i].resource->base);
    }
    if (h->shader_view && h->shader_view[i].counter_resource)
        d3d_release(&h->shader_view[i].counter_resource->base);
    if (h->shader_view) memset(&h->shader_view[i], 0, sizeof h->shader_view[i]);
    if (h->target_resource && h->target_resource[i]) {
        d3d_release(&h->target_resource[i]->base);
        h->target_resource[i] = NULL;
    }
    if (h->target_subresource) h->target_subresource[i] = 0;
    if (h->target_flags) h->target_flags[i] = 0;
}

static void put_buffer_view(size_t cpu, gpu_ir_buffer_view *view) {
    if (view) gpu_ir_buffer_view_retain(view);
    gpu_ir_buffer_view *old = NULL;
    int found = 0;
    pthread_mutex_lock(&g_heaps_lock);
    for (uint32_t k = 0; k < g_nheaps; k++) {
        heap12 *h = g_heaps[k];
        if (cpu < (size_t)h->cpu || cpu >= (size_t)h->cpu + 32 * (size_t)h->desc.NumDescriptors) continue;
        uint32_t i = (uint32_t)((cpu - (size_t)h->cpu) / 32);
        if (h->buffer_view) {
            old = h->buffer_view[i];
            h->buffer_view[i] = view;
            found = 1;
        }
        break;
    }
    pthread_mutex_unlock(&g_heaps_lock);
    if (!found && view) gpu_ir_buffer_view_release(view);
    if (old) gpu_ir_buffer_view_release(old);
}

// Attach API resource identity to an RTV/DSV descriptor. The Metal view alone cannot validate D3D12 resource states.
static void put_target_metadata(size_t cpu, res12 *resource, uint32_t subresource, uint32_t flags) {
    if (resource) d3d_addref(&resource->base);
    res12 *old = NULL;
    int found = 0;
    pthread_mutex_lock(&g_heaps_lock);
    for (uint32_t k = 0; k < g_nheaps; k++) {
        heap12 *h = g_heaps[k];
        if (cpu < (size_t)h->cpu || cpu >= (size_t)h->cpu + 32 * (size_t)h->desc.NumDescriptors) continue;
        uint32_t i = (uint32_t)((cpu - (size_t)h->cpu) / 32);
        if (h->target_resource && h->target_subresource && h->target_flags) {
            old = h->target_resource[i];
            h->target_resource[i] = resource;
            h->target_subresource[i] = subresource;
            h->target_flags[i] = flags;
            found = 1;
        }
        break;
    }
    pthread_mutex_unlock(&g_heaps_lock);
    if (!found && resource) d3d_release(&resource->base);
    if (old) d3d_release(&old->base);
}

static void put_shader_view(size_t cpu, res12 *resource, uint32_t mip, uint32_t mips,
                            uint32_t slice, uint32_t slices, uint32_t plane,
                            res12 *counter_resource, uint64_t counter_offset) {
    if (resource) d3d_addref(&resource->base);
    if (counter_resource) d3d_addref(&counter_resource->base);
    res12 *old = NULL, *old_counter = NULL;
    int found = 0;
    pthread_mutex_lock(&g_heaps_lock);
    for (uint32_t k = 0; k < g_nheaps; k++) {
        heap12 *h = g_heaps[k];
        if (cpu < (size_t)h->cpu || cpu >= (size_t)h->cpu + 32 * (size_t)h->desc.NumDescriptors) continue;
        uint32_t i = (uint32_t)((cpu - (size_t)h->cpu) / 32);
        if (h->shader_view) {
            old = h->shader_view[i].resource;
            old_counter = h->shader_view[i].counter_resource;
            h->shader_view[i] = (shader_view12){
                .resource = resource, .mip = mip, .mips = mips, .slice = slice, .slices = slices, .plane = plane,
                .counter_resource = counter_resource, .counter_offset = counter_offset
            };
            found = 1;
        }
        break;
    }
    pthread_mutex_unlock(&g_heaps_lock);
    if (!found && resource) d3d_release(&resource->base);
    if (!found && counter_resource) d3d_release(&counter_resource->base);
    if (old) d3d_release(&old->base);
    if (old_counter) d3d_release(&old_counter->base);
}

// ---- views ------------------------------------------------------------------------------------------------------------

static id<MTLTexture> texture_view(res12 *r, uint32_t format, MTLTextureType type, uint32_t mip, uint32_t mips, uint32_t slice, uint32_t slices) {
    if (!r || !r->texture) return nil;
    uint32_t bpp;
    MTLPixelFormat fmt = format ? d3d_mtl_format(format, &bpp) : r->texture.pixelFormat;
    // D3D12 exposes a D32_FLOAT depth allocation as an R32_FLOAT SRV. Metal comparison sampling requires a depth
    // texture view, so preserve the allocation's depth pixel format for that compatible view.
    if (format == 41 && r->texture.pixelFormat == MTLPixelFormatDepth32Float) fmt = MTLPixelFormatDepth32Float;
    if (fmt == MTLPixelFormatInvalid) fmt = r->texture.pixelFormat;
    // Metal makes no colour views of depth textures (D16 read as R16_UNORM, D32 as R32_FLOAT, ...) and stops the
    // process over one: such a view is the depth texture's own format, which shaders read in .r as D3D12's do;
    // the stencil of a depth-stencil texture is its X32_Stencil8 / X24_Stencil8 view.
    MTLPixelFormat src = r->texture.pixelFormat;
    int depth_src = src == MTLPixelFormatDepth16Unorm || src == MTLPixelFormatDepth32Float ||
                    src == MTLPixelFormatDepth32Float_Stencil8 || src == MTLPixelFormatDepth24Unorm_Stencil8;
    if (depth_src && fmt != src) {
        // (the stencil views: X32_TYPELESS_G8X24_UINT 22, X24_TYPELESS_G8_UINT 47 - R32_FLOAT_X8X24 21 and
        //  R24_UNORM_X8 46 are the depth's)
        int stencil = format == 22 || format == 47 || format == 118 || format == 117;
        if (stencil && src == MTLPixelFormatDepth32Float_Stencil8) fmt = MTLPixelFormatX32_Stencil8;
        else if (stencil && src == MTLPixelFormatDepth24Unorm_Stencil8) fmt = MTLPixelFormatX24_Stencil8;
        else if (!stencil || (fmt != MTLPixelFormatX32_Stencil8 && fmt != MTLPixelFormatX24_Stencil8)) fmt = src;
    }
    uint32_t total_mips = (uint32_t)r->texture.mipmapLevelCount;
    if (mip >= total_mips) return nil;
    if (mips == 0 || mips == 0xFFFFFFFFu) mips = total_mips - mip;
    else if (mips > total_mips - mip) return nil;
    uint32_t total = r->texture.textureType == MTLTextureType3D ? 1 : (uint32_t)r->texture.arrayLength;
    if (slice > total) return nil;
    if (slices == 0 || slices == 0xFFFFFFFFu) slices = total - slice;
    else if (slices > total - slice) return nil;
    if (!slices) return nil;
    @try {
        return [r->texture newTextureViewWithPixelFormat:fmt textureType:type levels:NSMakeRange(mip, mips) slices:NSMakeRange(slice, slices)];
    } @catch (NSException *exception) {
        (void)exception;
        return nil;
    }
}

static int d3d12_metal_min_lod_clamp_supported(id<MTLDevice> device) {
    if (!device) return 0;
    if (@available(macOS 10.15, *))
        if ([device supportsFamily:MTLGPUFamilyMac2]) return 1;
    if (@available(macOS 14.0, *)) return [device supportsFamily:MTLGPUFamilyApple6];
    return 0;
}

static MTLTextureType view_type(int32_t dim, int uav) {
    // D3D12_SRV_DIMENSION: 1 buffer, 2 tex1d, 3 tex1darray, 4 tex2d, 5 tex2darray, 6 tex2dms, 7 tex2dmsarray, 8 tex3d, 9 cube, 10 cubearray
    // D3D12_UAV_DIMENSION: 1 buffer, 2 tex1d, 3 tex1darray, 4 tex2d, 5 tex2darray, 8 tex3d
    switch (dim) {
    case 2: return MTLTextureType1D;
    case 3: return MTLTextureType1DArray;
    case 5: return MTLTextureType2DArray;
    case 6: return MTLTextureType2DMultisample;
    case 7: return MTLTextureType2DMultisampleArray;
    case 8: return MTLTextureType3D;
    case 9: return uav ? MTLTextureType2DArray : MTLTextureTypeCube;
    case 10: return MTLTextureTypeCubeArray;
    default: return MTLTextureType2D;
    }
}

static uint64_t resource_id(id<MTLResource> r) {
    MTLResourceID rid = [(id<MTLTexture>)r gpuResourceID];        // (textures and sampler states both answer it)
    uint64_t v;
    memcpy(&v, &rid, 8);
    return v;
}

// Metal texture buffers share their source buffer's storage. Use a view from offset zero so D3D12's FirstElement
// remains independent of Metal's per-format offset alignment; the descriptor carries the base element to DXIL MSL.
static id<MTLTexture> typed_buffer_texture(res12 *r, uint32_t format, uint32_t access) {
    if (!r || !r->buffer) return nil;
    uint32_t bpp = 0;
    MTLPixelFormat fmt = d3d_mtl_format(format, &bpp);
    if (fmt == MTLPixelFormatInvalid || !bpp) return nil;
    uint64_t bytes = MIN((uint64_t)r->desc.Width, (uint64_t)r->buffer.length);
    NSUInteger width = (NSUInteger)(bytes / bpp);
    if (!width || width > UINT32_MAX) return nil;
    NSUInteger align = [r->buffer.device minimumTextureBufferAlignmentForPixelFormat:fmt];
    NSUInteger row_bytes = width * bpp;
    if (align) {
        NSUInteger remainder = row_bytes % align;
        if (remainder) {
            if (row_bytes > NSUIntegerMax - (align - remainder)) return nil;
            row_bytes += align - remainder;
        }
    }
    MTLTextureUsage usage = MTLTextureUsageShaderRead;
    if (access & GPU_IR_ACCESS_SHADER_WRITE) usage |= MTLTextureUsageShaderWrite;
    if ((access & GPU_IR_ACCESS_SHADER_WRITE) && d3d12_texture_atomic_format_supported(r->dev->mtl, format))
        usage |= MTLTextureUsageShaderAtomic;
    MTLTextureDescriptor *td = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:fmt width:width
                                                                            resourceOptions:r->buffer.resourceOptions
                                                                                      usage:usage];
    id<MTLTexture> texture = nil;
    @try {
        texture = [r->buffer newTextureWithDescriptor:td offset:0 bytesPerRow:row_bytes];
    } @catch (NSException *exception) {
        (void)exception;
    }
    if (texture || !(usage & MTLTextureUsageShaderAtomic)) return texture;
    td.usage = usage & ~MTLTextureUsageShaderAtomic;
    @try {
        return [r->buffer newTextureWithDescriptor:td offset:0 bytesPerRow:row_bytes];
    } @catch (NSException *exception) {
        (void)exception;
        return nil;
    }
}

static gpu_ir_buffer_view *make_buffer_view(res12 *r, uint64_t first, uint32_t count, uint32_t format,
                                            uint32_t stride, uint32_t flags, uint32_t access,
                                            VxDesc *descriptor, id *keep) {
    int raw = (flags & 1u) != 0;
    if (!r || !r->buffer || !r->gpu_buffer || !count || (flags & ~1u)) return NULL;
    uint32_t element_size = raw ? 4u : stride ? stride : bytes_per(format);
    if (!element_size || first > UINT64_MAX / element_size) return NULL;
    uint64_t byte_offset = first * element_size, byte_size = (uint64_t)count * element_size;
    if (byte_offset > r->desc.Width || byte_size > r->desc.Width - byte_offset ||
        byte_offset > r->buffer.length || byte_size > r->buffer.length - byte_offset ||
        r->buffer.gpuAddress > UINT64_MAX - byte_offset) return NULL;
    uint32_t kind = raw ? GPU_IR_BUFFER_VIEW_RAW : stride ? GPU_IR_BUFFER_VIEW_STRUCTURED : GPU_IR_BUFFER_VIEW_TYPED;
    if (kind != GPU_IR_BUFFER_VIEW_TYPED && byte_size > UINT32_MAX) return NULL;
    gpu_ir_buffer_view_desc desc = {
        .kind = kind, .access_mask = access, .byte_offset = byte_offset, .byte_size = byte_size,
        .element_count = count, .element_size = kind == GPU_IR_BUFFER_VIEW_TYPED ? element_size : 0,
        .structure_stride = kind == GPU_IR_BUFFER_VIEW_STRUCTURED ? stride : 0
    };
    id<MTLTexture> texture = nil;
    void *backend = NULL;
    gpu_ir_backend_object_release release = NULL;
    if (kind == GPU_IR_BUFFER_VIEW_TYPED) {
        texture = typed_buffer_texture(r, format, access);
        if (!texture || first > texture.width || count > texture.width - first) return NULL;
        backend = (void *)CFBridgingRetain(texture);
        release = gpu12_release_mtl_object;
    }
    gpu_ir_buffer_view *view = gpu_ir_buffer_view_create(r->gpu_buffer, &desc, backend, release);
    if (!view) {
        if (backend) gpu12_release_mtl_object(backend);
        return NULL;
    }
    const gpu_ir_buffer_view_desc *normalized = gpu_ir_buffer_view_description(view);
    // Typed-buffer shaders index a texture_buffer, so va holds FirstElement and size holds the view's element count.
    // Raw and structured shaders use the byte GPU address and byte size as before.
    if (kind == GPU_IR_BUFFER_VIEW_TYPED) {
        id<MTLTexture> view_texture = (__bridge id<MTLTexture>)gpu_ir_buffer_view_backend_object(view);
        descriptor->va = normalized->byte_offset / normalized->element_size;
        descriptor->size = normalized->element_count;
        descriptor->stride = 0;
        descriptor->res = resource_id(view_texture);
        *keep = view_texture;
    } else {
        descriptor->va = r->buffer.gpuAddress + normalized->byte_offset;
        descriptor->size = (uint32_t)normalized->byte_size;
        descriptor->stride = normalized->structure_stride;
        descriptor->res = 0;
    }
    descriptor->fmt = format;
    descriptor->flags = flags;
    return view;
}

static WINAPI void dev_create_cbv(void *this, const CBV_DESC *desc, size_t cpu) {
    VxDesc v = { 0 };
    if (desc) { v.va = desc->location; v.size = desc->size; }
    put_desc(cpu, &v, nil, DXIL_CBV);
}

static WINAPI void dev_create_srv(void *this, void *resource, const SRV_DESC *desc, size_t cpu) {
    res12 *r = resource;
    VxDesc v = { 0 };
    id keep = nil;
    gpu_ir_buffer_view *ir_view = NULL;
    uint32_t view_mip = 0, view_mips = 0, view_slice = 0, view_slices = 0, view_plane = 0;
    float resource_min_lod = 0.0f;
    uint32_t raw_component_mapping = desc ? desc->mapping : 0x1688u;
    uint32_t component_mapping = 0;
    int stencil_swizzle = 0;
    // A stencil view: Metal reads the stencil in .r, D3D12 in .g (X32_TYPELESS_G8X24_UINT, X24_TYPELESS_G8_UINT: R
    // and B read 0, A 1). The view's mapping is composed with that, as a swizzle the shader applies.
    if (desc && (desc->format == 22 || desc->format == 47) && r && r->texture &&
        (r->texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 || r->texture.pixelFormat == MTLPixelFormatDepth24Unorm_Stencil8) &&
        (raw_component_mapping & (1u << 12)) && !(raw_component_mapping & ~0x1fffu)) {
        static const uint32_t to_metal[6] = { 4, 0, 4, 5, 4, 5 };     // D3D12 source selector -> where Metal has it
        uint32_t composed = 1u << 12;
        for (uint32_t c = 0; c < 4; c++) {
            uint32_t sel = (raw_component_mapping >> (c * 3)) & 7u;
            composed |= (sel <= 5 ? to_metal[sel] : sel) << (c * 3);
        }
        raw_component_mapping = composed;
        stencil_swizzle = 1;
    }
    int mapping_valid = !desc || d3d12_normalize_srv_component_mapping12(raw_component_mapping, &component_mapping);
    int texture_view_valid = mapping_valid;
    if (!mapping_valid) vx_log("D3D12: invalid SRV Shader4ComponentMapping; descriptor left null");
    if (mapping_valid && component_mapping && !stencil_swizzle && !((dev12 *)this)->nonidentity_srv) {
        ((dev12 *)this)->nonidentity_srv = 1;
        vx_log("D3D12: an SRV with component mapping %#x: comparison sampling is checked at each draw from now on", raw_component_mapping);
    }
    if (r && (r->desc.Flags & 0x8)) {
        vx_log("D3D12: DENY_SHADER_RESOURCE resource cannot create an SRV; descriptor left null");
    } else if (r && r->buffer) {
        if (!desc) { v.va = r->buffer.gpuAddress; v.size = (uint32_t)r->desc.Width; }
        else if (mapping_valid) {
            ir_view = make_buffer_view(r, desc->buffer.first, desc->buffer.count, desc->format,
                                       desc->buffer.stride, (uint32_t)desc->buffer.flags,
                                       GPU_IR_ACCESS_SHADER_READ, &v, &keep);
            if (!ir_view) vx_log("D3D12: buffer SRV range, format, or Metal view is unsupported; descriptor left null");
        }
        if (!mapping_valid) { v = (VxDesc){ 0 }; keep = nil; }
    } else if (r && r->texture) {
        int32_t dim = desc ? desc->dim : (int32_t)d3d12_default_srv_texture_dimension12(
            (uint32_t)r->desc.Dimension, r->desc.DepthOrArraySize, r->desc.SampleCount);
        if (desc) {
            d3d12_srv_texture_range12 range;
            texture_view_valid = mapping_valid && d3d12_normalize_srv_texture_range12((uint32_t)dim, desc->raw,
                (uint32_t)r->texture.mipmapLevelCount, (uint32_t)r->texture.arrayLength, &range);
            if (texture_view_valid) {
                view_mip = range.mip;
                view_mips = range.mips;
                view_slice = range.first_slice;
                view_slices = range.slices;
                view_plane = range.plane;
                resource_min_lod = range.min_lod;
            }
        } else if (!dim) {
            texture_view_valid = 0;
        }
        if (texture_view_valid && resource_min_lod != 0.0f &&
            (!d3d12_metal_min_lod_clamp_supported(r->dev->mtl) || dim == 2 || dim == 3)) {
            vx_log("D3D12: nonzero ResourceMinLODClamp needs Metal Mac2/Apple6 sampling support and a non-1D texture view; descriptor left null");
            texture_view_valid = 0;
        }
        id<MTLTexture> t = texture_view_valid ? texture_view(r, desc ? desc->format : 0, view_type(dim, 0),
                                        view_mip, view_mips, view_slice, view_slices) : nil;
        texture_view_valid = t != nil;
        if (texture_view_valid) {
            v.res = resource_id(t);
            v.fmt = desc ? desc->format : r->desc.Format;
            if (resource_min_lod != 0.0f) {
                memcpy(&v.size, &resource_min_lod, sizeof v.size);
                v.flags |= DXIL_MSL_DESC_HAS_MIN_LOD_CLAMP;
            }
            keep = t;
        } else {
            vx_log("D3D12: invalid or unsupported texture SRV dimension or subresource range; descriptor left null");
        }
    }
    if (mapping_valid && component_mapping &&
        ((r && r->texture && texture_view_valid) || (r && r->buffer && (!desc || ir_view)))) {
        v.flags |= (raw_component_mapping << DXIL_MSL_DESC_COMPONENT_MAPPING_SHIFT) &
                   DXIL_MSL_DESC_COMPONENT_MAPPING_MASK;
    }
    put_desc(cpu, &v, keep, DXIL_SRV);
    put_buffer_view(cpu, ir_view);
    gpu_ir_buffer_view_release(ir_view);
    if (mapping_valid && r && (r->buffer || (r->texture && texture_view_valid)))
        put_shader_view(cpu, r, view_mip, view_mips, view_slice, view_slices, view_plane, NULL, 0);
}

static WINAPI void dev_create_uav(void *this, void *resource, void *counter, const UAV_DESC *desc, size_t cpu) {
    dev12 *d = this;
    res12 *r = resource;
    res12 *counter_resource = counter;
    VxDesc v = { 0 };
    id keep = nil;
    gpu_ir_buffer_view *ir_view = NULL;
    uint32_t view_mip = 0, view_slice = 0, view_slices = 0, view_plane = 0;
    uint64_t counter_offset = 0;
    if (counter) {
        int valid = r && desc && r->base.kind == K12_RESOURCE && counter_resource->base.kind == K12_RESOURCE &&
            r->dev == d && counter_resource->dev == d && r->buffer && counter_resource->buffer &&
            desc->dim == 1 && desc->format == 0 && desc->buffer.stride > 0 &&
            !(desc->buffer.flags & 1) && desc->buffer.counter_offset % 4096u == 0 &&
            desc->buffer.counter_offset <= counter_resource->desc.Width &&
            counter_resource->desc.Width - desc->buffer.counter_offset >= sizeof(uint32_t) &&
            desc->buffer.counter_offset <= counter_resource->buffer.length &&
            counter_resource->buffer.length - desc->buffer.counter_offset >= sizeof(uint32_t) &&
            counter_resource->buffer.gpuAddress <= UINT64_MAX - desc->buffer.counter_offset;
        if (!valid) {
            vx_log("D3D12: invalid UAV counter resource, view format/stride, or 4096-byte counter offset; descriptor left null");
            put_desc(cpu, &v, nil, DXIL_UAV);
            put_buffer_view(cpu, NULL);
            return;
        }
        counter_offset = desc->buffer.counter_offset;
    } else if (r && r->buffer && desc && desc->buffer.counter_offset) {
        vx_log("D3D12: a UAV without a counter must have CounterOffsetInBytes equal to zero; descriptor left null");
        put_desc(cpu, &v, nil, DXIL_UAV);
        put_buffer_view(cpu, NULL);
        return;
    }
    if (r && r->buffer) {
        if (!desc) { v.va = r->buffer.gpuAddress; v.size = (uint32_t)r->desc.Width; }
        else {
            ir_view = make_buffer_view(r, desc->buffer.first, desc->buffer.count, desc->format,
                                       desc->buffer.stride, (uint32_t)desc->buffer.flags,
                                       GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE, &v, &keep);
            if (!ir_view) vx_log("D3D12: buffer UAV range, format, or Metal view is unsupported; descriptor left null");
            if (ir_view && counter_resource) {
                v.res = counter_resource->buffer.gpuAddress + counter_offset;
                v.flags |= DXIL_MSL_DESC_HAS_COUNTER;
                keep = counter_resource->buffer;
            }
        }
    } else if (r && r->texture) {
        uint32_t resource_dimension = (uint32_t)r->desc.Dimension;
        uint32_t array_size = r->desc.DepthOrArraySize;
        int32_t dim = desc ? desc->dim : (int32_t)d3d12_default_uav_texture_dimension12(
            resource_dimension, array_size);
        int texture_view_valid = r->texture.sampleCount == 1 &&
            d3d12_uav_texture_dimension_matches12(resource_dimension, (uint32_t)dim);
        if (texture_view_valid && dim == 8) {
            d3d12_uav_texture3d_range12 range;
            uint32_t mip = desc ? desc->tex3d.mip : 0;
            uint32_t first_w_slice = desc ? desc->tex3d.first_w_slice : 0;
            uint32_t w_size = desc ? desc->tex3d.w_size : UINT32_MAX;
            texture_view_valid = d3d12_normalize_uav_texture3d_range12(mip, first_w_slice, w_size,
                (uint32_t)r->texture.mipmapLevelCount, array_size, &range);
            if (texture_view_valid) {
                view_mip = range.mip;
                v.stride = range.first_w_slice;
                v.size = range.w_size;
                v.flags |= DXIL_MSL_DESC_HAS_W_SLICE_RANGE;
            }
        } else if (texture_view_valid) {
            if (desc && (dim == 2 || dim == 4)) {
                view_mip = desc->tex2d.mip;
                if (dim == 4) view_plane = desc->tex2d.plane;
            }
            if (desc && (dim == 3 || dim == 5)) {
                view_mip = desc->tex2darray.mip;
                view_slice = desc->tex2darray.first_slice;
                view_slices = desc->tex2darray.slices;
                if (dim == 5) view_plane = desc->tex2darray.plane;
            }
        }
        id<MTLTexture> t = texture_view_valid ? texture_view(r, desc ? desc->format : 0,
            view_type(dim, 1), view_mip, 1, view_slice, view_slices) : nil;
        texture_view_valid = t != nil;
        if (texture_view_valid) {
            v.res = resource_id(t);
            v.fmt = desc ? desc->format : r->desc.Format;
            keep = t;
        } else {
            vx_log("D3D12: invalid or unsupported texture UAV dimension or subresource range; descriptor left null");
        }
    }
    put_desc(cpu, &v, keep, DXIL_UAV);
    put_buffer_view(cpu, ir_view);
    gpu_ir_buffer_view_release(ir_view);
    if (r && ((r->texture && v.res) || (r->buffer && (!desc || ir_view))))
        put_shader_view(cpu, r, view_mip, r->texture ? 1 : 0, view_slice, view_slices, view_plane,
                        ir_view && counter_resource ? counter_resource : NULL, ir_view ? counter_offset : 0);
}

static WINAPI void dev_create_sampler(void *this, const SAMPLER_DESC *s, size_t cpu) {
    id<MTLSamplerState> st = make_sampler(this, s->filter, s->u, s->v, s->w, s->bias, s->aniso, s->cmp,
        s->border, s->min_lod, s->max_lod, 1);
    VxDesc v = { 0 };
    v.res = resource_id((id<MTLResource>)st);
    put_desc(cpu, &v, st, DXIL_SAMPLER);
}

static WINAPI void dev_copy_simple(void *this, uint32_t n, size_t dst, size_t src, int32_t type) {
    for (uint32_t k = 0; k < n; k++) {
        uint32_t si, di;
        heap12 *sh = heap_at(src + 32 * (size_t)k, &si), *dh = heap_at(dst + 32 * (size_t)k, &di);
        if (!sh || !dh) continue;
        memcpy(dh->cpu + 32 * (size_t)di, sh->cpu + 32 * (size_t)si, 32);
        heap_keep12(dh, di, sh->keep[si]);
        if (dh->descriptor_kind) dh->descriptor_kind[di] = sh->descriptor_kind ? sh->descriptor_kind[si] : 0;
        gpu_ir_buffer_view *view_ref = sh->buffer_view ? sh->buffer_view[si] : NULL;
        if (view_ref) gpu_ir_buffer_view_retain(view_ref);
        gpu_ir_buffer_view *old_buffer_view = dh->buffer_view ? dh->buffer_view[di] : NULL;
        if (dh->buffer_view) dh->buffer_view[di] = view_ref;
        if (old_buffer_view) gpu_ir_buffer_view_release(old_buffer_view);
        shader_view12 view = sh->shader_view ? sh->shader_view[si] : (shader_view12){ 0 };
        if (view.resource) d3d_addref(&view.resource->base);
        if (view.counter_resource) d3d_addref(&view.counter_resource->base);
        res12 *old_view = dh->shader_view ? dh->shader_view[di].resource : NULL;
        res12 *old_counter_view = dh->shader_view ? dh->shader_view[di].counter_resource : NULL;
        if (dh->shader_view) dh->shader_view[di] = view;
        if (old_view) d3d_release(&old_view->base);
        if (old_counter_view) d3d_release(&old_counter_view->base);
        res12 *target = sh->target_resource ? sh->target_resource[si] : NULL;
        if (target) d3d_addref(&target->base);
        res12 *old = dh->target_resource ? dh->target_resource[di] : NULL;
        if (dh->target_resource) dh->target_resource[di] = target;
        if (dh->target_subresource) dh->target_subresource[di] = sh->target_subresource ? sh->target_subresource[si] : 0;
        if (dh->target_flags) dh->target_flags[di] = sh->target_flags ? sh->target_flags[si] : 0;
        if (old) d3d_release(&old->base);
    }
}

static WINAPI void dev_copy_descriptors(void *this, uint32_t ndst, const size_t *dst_starts, const uint32_t *dst_sizes,
                                        uint32_t nsrc, const size_t *src_starts, const uint32_t *src_sizes, int32_t type) {
    uint32_t di = 0, dk = 0, si = 0, sk = 0;
    while (di < ndst && si < nsrc) {
        uint32_t dn = dst_sizes ? dst_sizes[di] : 1, sn = src_sizes ? src_sizes[si] : 1;
        dev_copy_simple(this, 1, dst_starts[di] + 32 * (size_t)dk, src_starts[si] + 32 * (size_t)sk, type);
        if (++dk >= dn) { dk = 0; di++; }
        if (++sk >= sn) { sk = 0; si++; }
    }
}

// ---- fences -------------------------------------------------------------------------------------------------------

static void fence_destroy(d3d_object *o) { fence12 *f = (fence12 *)o; f->event = nil; f->listener = nil; release_dev(o); }
static WINAPI HRESULT fence_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12Fence, &IID_ID3D12Fence1, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 5);
}
static WINAPI uint64_t fence_completed(void *this) { return ((fence12 *)this)->event.signaledValue; }
static WINAPI HRESULT fence_signal(void *this, uint64_t v) { ((fence12 *)this)->event.signaledValue = v; return S_OK; }
// VXRT_D3D12_TRACE_FENCES=1: a wait (the program's on the CPU, or a queue's on the GPU) still short of its value 5 s
// later is logged, with the value the fence has: what a program stops at when the GPU never gets there.
static int trace_fences12(void) { static int on = -1; if (on < 0) on = getenv("VXRT_D3D12_TRACE_FENCES") != NULL; return on; }
static void fence_watch12(fence12 *f, uint64_t value, const char *who, void *queue) {
    if (!trace_fences12() || f->event.signaledValue >= value) return;
    id<MTLSharedEvent> ev = f->event;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        uint64_t now = ev.signaledValue;
        if (now < value) vx_log("D3D12: fence %p: %s%s%p waits for %llu, 5 s later it is at %llu", (void *)f, who, queue ? " " : "",
                                queue, (unsigned long long)value, (unsigned long long)now);
    });
}
static WINAPI HRESULT fence_set_event(void *this, uint64_t value, HANDLE h) {
    fence12 *f = this;
    fence_watch12(f, value, h ? "the program (event)" : "the program (blocking)", NULL);
    if (f->event.signaledValue >= value) { if (h) vx_set_event(h); return S_OK; }
    if (!h) {                                                      // (no event: wait here)
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [f->event notifyListener:f->listener atValue:value block:^(id<MTLSharedEvent> e, uint64_t v) { dispatch_semaphore_signal(sem); }];
        uint64_t t0 = fs_now12();
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        FS_ADD12(wait_ns, fs_now12() - t0);
        return S_OK;
    }
    [f->event notifyListener:f->listener atValue:value block:^(id<MTLSharedEvent> e, uint64_t v) { vx_set_event(h); }];
    return S_OK;
}
static WINAPI int32_t fence_flags(void *this) { return 0; }

// ID3D12Device1::SetEventOnMultipleFenceCompletion: the event is set when all (flags 0) or any (flags 1) of the
// fences reach their values; without an event the call waits.
typedef struct { int32_t left, done; HANDLE event; dispatch_semaphore_t sem; } multi_wait12;
static void multi_wait_reached12(multi_wait12 *w, int any) {
    if (any ? __sync_lock_test_and_set(&w->done, 1) != 0 : __sync_sub_and_fetch(&w->left, 1) != 0) return;
    if (w->event) vx_set_event(w->event);
    else dispatch_semaphore_signal(w->sem);
}
static WINAPI HRESULT dev_set_event_multiple_fences(void *this, void *const *fences, const uint64_t *values, uint32_t n,
                                                    int32_t flags, HANDLE event) {
    if ((flags & ~1) || (n && (!fences || !values))) return E_INVALIDARG;
    for (uint32_t i = 0; i < n; i++)
        if (!fences[i] || ((d3d_object *)fences[i])->kind != K12_FENCE) return E_INVALIDARG;
    int any = flags & 1;
    uint32_t pending = 0;
    for (uint32_t i = 0; i < n; i++) pending += ((fence12 *)fences[i])->event.signaledValue < values[i];
    if (!n || (any ? pending < n : !pending)) {                   // already complete
        if (event) vx_set_event(event);
        return S_OK;
    }
    // (shared by the listeners; small and kept, as a waiting listener may still fire after the event is set)
    multi_wait12 *w = calloc(1, sizeof *w);
    if (!w) return E_OUTOFMEMORY;
    w->left = (int32_t)pending;
    w->event = event;
    if (!event) w->sem = dispatch_semaphore_create(0);
    dispatch_semaphore_t sem = w->sem;
    for (uint32_t i = 0; i < n; i++) {
        fence12 *f = fences[i];
        if (f->event.signaledValue >= values[i]) continue;
        [f->event notifyListener:f->listener atValue:values[i] block:^(id<MTLSharedEvent> e, uint64_t v) { multi_wait_reached12(w, any); }];
    }
    if (!event) dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    return S_OK;
}

static WINAPI HRESULT dev_create_fence(void *this, uint64_t initial, int32_t flags, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    uint32_t fence_flags = (uint32_t)flags;
    if (fence_flags & ~7u) return E_INVALIDARG;
    // The runtime exposes a local monitored MTLSharedEvent. Shared/cross-adapter handles and
    // non-monitored fences have no matching implementation, so don't accept those flags silently.
    if (fence_flags) return DXGI_ERROR_UNSUPPORTED;
    dev12 *d = this;
    fence12 *f = alloc12(sizeof *f, K12_FENCE, g_fence_vtbl, d);
    f->base.destroy = fence_destroy;
    f->event = [d->mtl newSharedEvent];
    if (!f->event) { d3d_release(&f->base); return E_OUTOFMEMORY; }
    f->event.signaledValue = initial;
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("vxrt.d3d12.fences", DISPATCH_QUEUE_SERIAL); });
    f->listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:q];
    if (!f->listener) { d3d_release(&f->base); return E_OUTOFMEMORY; }
    HRESULT hr = fence_query(f, iid, out);
    d3d_release(&f->base);
    return hr;
}

// ---- pipeline states ---------------------------------------------------------------------------------------------

static void pso_destroy(d3d_object *o) {
    pso12 *p = (pso12 *)o;
    gpu_ir_pipeline_release(p->gpu_pipeline);
    gpu_ir_shader_release(p->common_ir_vertex);
    p->common_ir_vertex = NULL;
    gpu_ir_shader_release(p->common_ir_fragment);
    p->common_ir_fragment = NULL;
    p->gpu_pipeline = NULL;
    p->compute = nil; p->render = nil; p->depth = nil;
    p->gs_kernel = nil; p->gs_vs_source = nil; p->gs_vs_kernels = nil;
    p->hs_kernel = nil;
    release_dev(o);
}
static WINAPI HRESULT pso_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12PipelineState, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}

static int shader_root_heap_flags_valid12(dev12 *device, const dxil_msl *shader, void *root_object) {
    if (!shader->uses_heap && !shader->uses_sampler_heap) return 1;
    rootsig12 *root = root_object;
    if (!root || root->base.kind != K12_ROOTSIG || root->dev != device) return 0;
    if (shader->uses_heap && !(root->flags & 0x400u)) return 0;
    if (shader->uses_sampler_heap && !(root->flags & 0x800u)) return 0;
    return 1;
}

static int root_visibility_allows_shader12(int32_t visibility, int shader_kind) {
    if (visibility == 0) return 1; // D3D12_SHADER_VISIBILITY_ALL
    if (shader_kind == 1) return visibility == 1; // vertex
    if (shader_kind == 0) return visibility == 5; // pixel
    if (shader_kind == 2) return visibility == 4; // geometry
    if (shader_kind == 3) return visibility == 2; // hull
    if (shader_kind == 4) return visibility == 3; // domain
    return 0; // compute shaders use ALL visibility
}

static uint32_t root_deny_flag_for_shader12(int shader_kind) {
    if (shader_kind == 1) return 0x2u;  // DENY_VERTEX_SHADER_ROOT_ACCESS
    if (shader_kind == 0) return 0x20u; // DENY_PIXEL_SHADER_ROOT_ACCESS
    if (shader_kind == 2) return 0x10u; // DENY_GEOMETRY_SHADER_ROOT_ACCESS
    if (shader_kind == 3) return 0x4u;  // DENY_HULL_SHADER_ROOT_ACCESS
    if (shader_kind == 4) return 0x8u;  // DENY_DOMAIN_SHADER_ROOT_ACCESS
    return 0;                           // D3D12 has no compute-root-access deny flag
}

static uint64_t root_register_end12(uint32_t first, uint32_t count) {
    return count == UINT32_MAX ? (1ull << 32) : (uint64_t)first + count;
}

static uint64_t root_binding_end_at12(const rootsig12 *root, uint32_t register_class,
                                      uint32_t space, int shader_kind, uint64_t cursor) {
    uint64_t best = cursor;
    for (uint32_t i = 0; i < root->nparams; i++) {
        const param12 *parameter = &root->params[i];
        if (!root_visibility_allows_shader12(parameter->visibility, shader_kind)) continue;
        if (parameter->type == 0) {
            for (uint32_t j = 0; j < parameter->nranges; j++) {
                const range12 *range = &parameter->ranges[j];
                if ((uint32_t)range->type != register_class || range->space != space || range->base > cursor)
                    continue;
                uint64_t end = root_register_end12(range->base, range->count);
                if (end > cursor && end > best) best = end;
            }
            continue;
        }

        uint32_t parameter_class;
        if (parameter->type == 1 || parameter->type == 2) parameter_class = DXIL_CBV;
        else if (parameter->type == 3) parameter_class = DXIL_SRV;
        else if (parameter->type == 4) parameter_class = DXIL_UAV;
        else continue;
        if (parameter_class == register_class && parameter->space == space && parameter->reg == cursor && best < cursor + 1)
            best = cursor + 1;
    }

    if (register_class == DXIL_SAMPLER) {
        for (uint32_t i = 0; i < root->nsamplers; i++) {
            const static_sampler12 *sampler = &root->samplers[i];
            if (sampler->space == space && sampler->reg == cursor &&
                root_visibility_allows_shader12(sampler->visibility, shader_kind) && best < cursor + 1)
                best = cursor + 1;
        }
    }
    return best;
}

static int root_signature_covers_shader_slot12(const rootsig12 *root, const dxil_msl_slot *slot,
                                               int shader_kind) {
    if (!root || !slot || slot->cls > DXIL_SAMPLER || !slot->count) return 0;
    uint64_t end = root_register_end12(slot->lower, slot->count);
    if (end > (1ull << 32) || end <= slot->lower) return 0;
    uint64_t cursor = slot->lower;
    while (cursor < end) {
        uint64_t next = root_binding_end_at12(root, slot->cls, slot->space, shader_kind, cursor);
        if (next <= cursor) return 0;
        cursor = next < end ? next : end;
    }
    return 1;
}

static int shader_root_signature_valid12(dev12 *device, const dxil_msl *shader, void *root_object) {
    if (!shader) return 1;
    rootsig12 *root = root_object;
    if (!root_object) return shader->nslots == 0 && !shader->uses_heap && !shader->uses_sampler_heap;
    if (root->base.kind != K12_ROOTSIG || root->dev != device ||
        !shader_root_heap_flags_valid12(device, shader, root_object)) return 0;

    int has_root_access = shader->nslots || shader->uses_heap || shader->uses_sampler_heap;
    uint32_t deny_flag = root_deny_flag_for_shader12(shader->stage);
    if (has_root_access && deny_flag && (root->flags & deny_flag)) return 0;
    for (uint32_t i = 0; i < shader->nslots; i++)
        if (!root_signature_covers_shader_slot12(root, &shader->slots[i], shader->stage)) return 0;
    return 1;
}

static MTLCompileOptions *d3d12_shader_compile_options12(dev12 *device, const dxil_msl *shader) {
    int supports_min_lod = d3d12_metal_min_lod_clamp_supported(device->mtl);
    if (shader && shader->uses_min_lod_clamp && !supports_min_lod) return nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    if (shader && shader->uses_thread_fence) {
        if (@available(macOS 15.0, *)) options.languageVersion = MTLLanguageVersion3_2;
        else {
            vx_log("D3D12: shader memory fences require Metal shading language 3.2 (macOS 15 or later)");
            return nil;
        }
    } else options.languageVersion = MTLLanguageVersion3_1;
    if (supports_min_lod) options.preprocessorMacros = @{ @"VXRT_HAS_MIN_LOD_CLAMP": @1 };
    // VXRT_D3D12_SAFE_MATH=1: Metal's safe math (IEEE NaN and infinity) instead of fast math, to compare.
    static int fast = -1;
    if (fast < 0) { const char *e = getenv("VXRT_D3D12_SAFE_MATH"); fast = !(e && *e == '1'); }
    if (!fast) {
        if (@available(macOS 15.0, *)) options.mathMode = MTLMathModeSafe;
        else options.fastMathEnabled = NO;
    }
    return options;
}

static int d3d12_is_dxbc_program(const BYTECODE *code);
static int dxbc_to_msl12(const BYTECODE *code, dxil_msl *out);
static void dxbc_reflect12(const dxbc_shader *src, dxil_msl *out);
// VXRT_D3D12_NO_COMMON_IR=<mask>: shaders translated directly instead of through the common IR - 1 compute, 2 pixel,
// 4 vertex (to tell one translation's mistake from the other's).
static int no_common_ir12(int what) {
    static int mask = -1;
    if (mask < 0) { const char *e = getenv("VXRT_D3D12_NO_COMMON_IR"); mask = e ? atoi(e) : 0; }
    return (mask & what) != 0;
}
static HRESULT create_compute_pso12(void *this, const COMPUTE_PSO_DESC *desc, const GUID *iid, void **out) {
    dev12 *d = this;
    int dxbc = d3d12_is_dxbc_program(&desc->cs);
    dxil_module m;
    if (!dxbc && dxil_read(desc->cs.code, desc->cs.size, &m)) { vx_log("D3D12: compute shader: %s", m.error); return E_INVALIDARG; }
    pso12 *p = alloc12(sizeof *p, K12_PSO, g_pso_vtbl, d);
    p->base.destroy = pso_destroy;
    p->cs_code = code_dump12(desc->cs.code, desc->cs.size);
    gpu_ir_shader *common_shader = NULL;
    int rc;
    if (dxbc) {
        rc = dxbc_to_msl12(&desc->cs, &p->msl);
        if (!rc && p->msl.stage != 5) { snprintf(p->msl.error, sizeof p->msl.error, "not a compute shader"); rc = -1; }
    } else {
        rc = dxil_to_msl(&m, &p->msl);
        if (!rc) msl_override12(&desc->cs, &p->msl);
        if (!rc) {
            int ir_rc = no_common_ir12(1) ? GPU_IR_UNSUPPORTED : dxil_build_gpu_ir_shader(&m, DXIL_IR_ENTRY_FUNCTION, &common_shader);
            if (ir_rc != GPU_IR_OK && ir_rc != GPU_IR_UNSUPPORTED)
                vx_log("D3D12: compute shader common-IR lowering returned %d; retaining direct DXIL translation", ir_rc);
        }
        dxil_free(&m);
    }
    if (rc) { vx_log("D3D12: compute shader not translated: %s", p->msl.error); d3d_release(&p->base); return E_INVALIDARG; }
    if (p->msl.uses_min_lod_clamp && !d3d12_metal_min_lod_clamp_supported(d->mtl)) {
        vx_log("D3D12: shader-level min-LOD sampling needs Metal Mac2/Apple6 support");
        d3d_release(&p->base);
        return E_NOTIMPL;
    }
    if (!shader_root_signature_valid12(d, &p->msl, desc->root)) {
        vx_log("D3D12: compute shader %016llx: bindings are not covered by a compatible root signature", (unsigned long long)p->cs_code);
        d3d_release(&p->base);
        return E_INVALIDARG;
    }
    rootsig12 *root = desc->root;
    NSString *src = @(p->msl.msl);
    pthread_mutex_lock(&d->lock);
    id<MTLComputePipelineState> cached = d->pso_cache[src];
    pthread_mutex_unlock(&d->lock);
    if (cached) p->compute = cached;
    else {
        MTLCompileOptions *opt = d3d12_shader_compile_options12(d, &p->msl);
        if (!opt) {
            d3d_release(&p->base);
            return E_NOTIMPL;
        }
        NSError *err = nil;
        uint64_t hash = msl_dump12(p->msl.msl);
        double started = msl_clock12();
        id<MTLLibrary> lib = library_with_timeout12(d->mtl, src, opt, &err);
        if (lib) p->compute = compute_pipeline_with_timeout12(d->mtl, [lib newFunctionWithName:@"vx_main"], &err);
        msl_timed12("compute shader", hash, started);
        if (!p->compute) {
            vx_log("D3D12: compute shader did not compile: %s\n%s", err.localizedDescription.UTF8String, p->msl.msl);
            d3d_release(&p->base);
            return E_INVALIDARG;
        }
        pthread_mutex_lock(&d->lock);
        d->pso_cache[src] = p->compute;
        pthread_mutex_unlock(&d->lock);
    }
    gpu_ir_pipeline *common_metal_pipeline = NULL;
    char common_error[512] = { 0 };
    gpu_ir_pipeline_layout *common_layout =
        root && root->base.kind == K12_ROOTSIG && root->dev == d ? root->gpu_layout : NULL;
    if (common_shader) {
        common_metal_pipeline = gpu_ir_metal_compute_pipeline_create_for_metal_device(
            (__bridge void *)d->mtl, common_shader, common_layout, common_error, sizeof common_error);
        if (!common_metal_pipeline)
            vx_log("D3D12: common compute MSL/Metal pipeline unavailable; retaining direct DXIL path: %s",
                   common_error[0] ? common_error : "unsupported common shader or root layout");
    }
    void *metal_pipeline_object = NULL;
    if (common_metal_pipeline) {
        id<MTLComputePipelineState> common_state =
            (__bridge id<MTLComputePipelineState>)gpu_ir_pipeline_backend_object(common_metal_pipeline);
        if (common_state) {
            metal_pipeline_object = (__bridge_retained void *)common_state;
            p->common_ir_compute = 1;
        }
    }
    if (!metal_pipeline_object) metal_pipeline_object = (__bridge_retained void *)p->compute;
    gpu_ir_pipeline_desc ir_desc = {
        .info = { .kind = GPU_IR_PIPELINE_COMPUTE, .stage_mask = GPU_IR_STAGE_COMPUTE,
                  .threadgroup_size = { p->msl.threads[0], p->msl.threads[1], p->msl.threads[2] } },
        .layout = common_layout,
        .backend_object = metal_pipeline_object,
        .release = gpu12_release_mtl_object,
        .stage_shaders = { [GPU_IR_STAGE_SLOT_COMPUTE] = common_shader }
    };
    p->gpu_pipeline = gpu_ir_pipeline_create(&ir_desc);
    gpu_ir_pipeline_release(common_metal_pipeline);
    gpu_ir_shader_release(common_shader);
    if (!p->gpu_pipeline) {
        gpu12_release_mtl_object(ir_desc.backend_object);
        vx_log("D3D12: compute pipeline could not be represented by the common GPU IR");
        dxil_msl_free(&p->msl);
        d3d_release(&p->base);
        return E_OUTOFMEMORY;
    }
    dxil_msl_free(&p->msl);
    HRESULT hr = pso_query(p, iid, out);
    d3d_release(&p->base);
    return hr;
}

// ---- root signatures ---------------------------------------------------------------------------------------------------

static WINAPI HRESULT rootsig_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12RootSignature, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 3);
}
static WINAPI HRESULT dev_create_rootsig(void *this, uint32_t node, const void *data, size_t size, const GUID *iid, void **out) {
    if (out) *out = NULL;
    if (!data || !out) return E_INVALIDARG;
    dev12 *d = this;
    rootsig12 *r = alloc12(sizeof *r, K12_ROOTSIG, g_rootsig_vtbl, d);
    r->base.destroy = rootsig_destroy;
    HRESULT hr = parse_rootsig(d, data, size, r);
    r->blob_hash = fnv1a12(0xcbf29ce484222325ull, data, size);
    if (hr == S_OK) hr = rootsig_query(r, iid, out);
    d3d_release(&r->base);
    return hr;
}

// ---- command signatures and ExecuteIndirect ----------------------------------------------------------------------

enum {
    INDIRECT_DRAW, INDIRECT_DRAW_INDEXED, INDIRECT_DISPATCH, INDIRECT_VERTEX_BUFFER, INDIRECT_INDEX_BUFFER,
    INDIRECT_CONSTANT, INDIRECT_CBV, INDIRECT_SRV, INDIRECT_UAV, INDIRECT_DISPATCH_RAYS, INDIRECT_DISPATCH_MESH,
    INDIRECT_INCREMENTING_CONSTANT
};

static void command_signature_destroy(d3d_object *o) {
    command_signature12 *s = (command_signature12 *)o;
    if (s->root) d3d_release(&s->root->base);
    free(s->args);
    release_dev(o);
}
static WINAPI HRESULT command_signature_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12CommandSignature, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}

static int command_root_arg_valid(const rootsig12 *root, const INDIRECT_ARGUMENT_DESC *arg) {
    if (!root || arg->raw[0] >= root->nparams) return 0;
    const param12 *p = &root->params[arg->raw[0]];
    switch (arg->type) {
    case INDIRECT_CONSTANT:
        return p->type == 1 && arg->constants.count && arg->constants.offset <= p->n32 &&
               arg->constants.count <= p->n32 - arg->constants.offset;
    case INDIRECT_INCREMENTING_CONSTANT:
        return p->type == 1 && arg->incrementing_constant.offset < p->n32;
    case INDIRECT_CBV: return p->type == 2;
    case INDIRECT_SRV: return p->type == 3;
    case INDIRECT_UAV: return p->type == 4;
    default: return 0;
    }
}

static WINAPI HRESULT dev_create_command_signature(void *this, const COMMAND_SIGNATURE_DESC *desc, void *root_object,
                                                    const GUID *iid, void **out) {
    if (out) *out = NULL;
    if (!desc || !desc->args || !desc->count || !desc->stride || (desc->stride & 3)) return E_INVALIDARG;
    rootsig12 *root = root_object;
    if (root && (root->base.kind != K12_ROOTSIG || root->dev != this)) return E_INVALIDARG;
    command_argument12 *args = calloc(desc->count, sizeof *args);
    if (!args) return E_OUTOFMEMORY;
    uint32_t payload = 0, operation = UINT32_MAX, operation_count = 0, root_index = 0, previous_constant_end = 0;
    int has_root_args = 0, has_vb = 0, has_ib = 0, has_dispatch = 0, has_draw = 0, has_indexed_draw = 0,
        have_previous_root = 0, has_incrementing_constant = 0;
    uint32_t ib_count = 0;
    uint32_t vb_slots = 0;
    for (uint32_t i = 0; i < desc->count; i++) {
        const INDIRECT_ARGUMENT_DESC *arg = &desc->args[i];
        command_argument12 *dst = &args[i];
        dst->type = (uint32_t)arg->type;
        switch (arg->type) {
        case INDIRECT_DRAW: dst->size = 16; has_draw = 1; operation = i; operation_count++; break;
        case INDIRECT_DRAW_INDEXED: dst->size = 20; has_draw = 1; has_indexed_draw = 1; operation = i; operation_count++; break;
        case INDIRECT_DISPATCH: dst->size = 12; has_dispatch = 1; operation = i; operation_count++; break;
        case INDIRECT_VERTEX_BUFFER:
            if (arg->vb.slot >= 16 || (vb_slots & (1u << arg->vb.slot))) goto invalid;
            vb_slots |= 1u << arg->vb.slot;
            dst->a = arg->vb.slot; dst->size = 16; has_vb = 1; break;
        case INDIRECT_INDEX_BUFFER: dst->size = 16; has_ib = 1; ib_count++; break;
        case INDIRECT_CONSTANT: case INDIRECT_CBV: case INDIRECT_SRV: case INDIRECT_UAV:
            if (!command_root_arg_valid(root, arg)) goto invalid;
            has_root_args = 1;
            dst->a = arg->raw[0];
            if (have_previous_root && dst->a < root_index) goto invalid;
            if (!have_previous_root || dst->a != root_index) previous_constant_end = 0;
            if (arg->type == INDIRECT_CONSTANT) {
                if (arg->constants.offset < previous_constant_end) goto invalid;
                dst->b = arg->constants.offset; dst->c = arg->constants.count;
                previous_constant_end = arg->constants.offset + arg->constants.count;
                dst->size = arg->constants.count * 4;
            } else {
                if (have_previous_root && dst->a == root_index) goto invalid;
                dst->size = 8;
                previous_constant_end = UINT32_MAX;
            }
            root_index = dst->a; have_previous_root = 1;
            break;
        case INDIRECT_INCREMENTING_CONSTANT:
            if (has_incrementing_constant || !command_root_arg_valid(root, arg)) goto invalid;
            has_incrementing_constant = 1;
            has_root_args = 1;
            dst->a = arg->incrementing_constant.root;
            dst->b = arg->incrementing_constant.offset;
            dst->c = 1;
            if (have_previous_root && dst->a < root_index) goto invalid;
            if (!have_previous_root || dst->a != root_index) previous_constant_end = 0;
            if (dst->b < previous_constant_end) goto invalid;
            previous_constant_end = dst->b + 1;
            root_index = dst->a;
            have_previous_root = 1;
            // The counter is supplied by the runtime, so this argument consumes no record bytes.
            dst->size = 0;
            break;
        case INDIRECT_DISPATCH_RAYS:
        case INDIRECT_DISPATCH_MESH:
        default:
            // DXR and mesh dispatch signatures are not supported by this device.
            goto invalid;
        }
        if (payload > UINT32_MAX - dst->size) goto invalid;
        dst->payload_offset = payload;
        payload += dst->size;
    }
    if (has_draw == has_dispatch || operation_count != 1 || operation != desc->count - 1 || (has_dispatch && (has_vb || has_ib)) ||
        (has_ib && !has_indexed_draw) || ib_count > 1 || (has_root_args != (root != NULL)) || payload > desc->stride) goto invalid;
    if (!out) { free(args); return S_FALSE; }
    command_signature12 *s = alloc12(sizeof *s, K12_COMMAND_SIGNATURE, g_command_signature_vtbl, this);
    s->base.destroy = command_signature_destroy;
    s->stride = desc->stride; s->count = desc->count; s->payload_size = payload; s->args = args;
    s->root = root; if (root) d3d_addref(&root->base);
    s->gfx = has_draw;
    HRESULT hr = command_signature_query(s, iid, out);
    d3d_release(&s->base);
    return hr;
invalid:
    free(args);
    return E_INVALIDARG;
}

// ---- command lists: recording ------------------------------------------------------------------------------------------

static cmd12 *rec(list12 *l, int op) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->cmds = realloc(l->cmds, l->cap * sizeof *l->cmds); }
    cmd12 *c = &l->cmds[l->n++];
    memset(c, 0, sizeof *c);
    c->op = op;
    return c;
}

// (objects a recorded command names are held until the list is reset or released)
static void hold(void *o) { if (o) d3d_addref(o); }
static void list_allocator_stop_recording12(list12 *list) {
    if (!list->allocator_recording || !list->allocator) return;
    if (list->allocator->recording_lists) list->allocator->recording_lists--;
    list->allocator_recording = 0;
}
static void list_clear(list12 *l) {
    for (uint32_t i = 0; i < 8; i++) if (l->vb_cache[i].buffer) gpu_ir_resource_release(l->vb_cache[i].buffer);
    memset(l->vb_cache, 0, sizeof l->vb_cache);
    for (uint32_t i = 0; i < l->n; i++) {
        cmd12 *c = &l->cmds[i];
        free(c->rects);
        for (uint32_t j = 0; c->immediate_writes && j < c->a; j++)
            if (c->immediate_writes[j].resource) d3d_release(&c->immediate_writes[j].resource->base);
        free(c->immediate_writes);
        if (c->obj) d3d_release(c->obj);
        if (c->obj2) d3d_release(c->obj2);
        if (c->obj3) d3d_release(c->obj3);
        if (c->op == C_COPY_TEXTURE) { if (c->dst.res) d3d_release(c->dst.res); if (c->src.res) d3d_release(c->src.res); }
        for (int k = 0; k < 9; k++) if (c->tex[k]) CFRelease(c->tex[k]);
        for (int k = 0; k < 9; k++) if (c->resource[k]) d3d_release(&c->resource[k]->base);
    }
    gpu_ir_stream_clear(&l->gpu_ir);
    l->recording_pso = NULL;
    l->recording_topology = 4; // D3D12 triangle list; matches the replay state's initial topology.
    l->recording_topology_set = 0;
    for (uint32_t i = 0; i < 4; i++) l->recording_blend_factor[i] = 1.0f;
    memset(l->recording_vertex_buffers, 0, sizeof l->recording_vertex_buffers);
    memset(&l->recording_index_buffer, 0, sizeof l->recording_index_buffer);
    l->n = 0;
    free(l->render_pass_resolves);
    l->render_pass_resolves = NULL;
    l->render_pass_nresolves = 0;
    l->in_render_pass = 0;
}
static void list_destroy(d3d_object *o) {
    list12 *l = (list12 *)o;
    pthread_mutex_lock(&l->dev->alloc_lock);
    list_allocator_stop_recording12(l);
    allocator12 *allocator = l->allocator;
    l->allocator = NULL;
    pthread_mutex_unlock(&l->dev->alloc_lock);
    if (allocator) d3d_release(&allocator->base);
    list_clear(l);
    gpu_ir_stream_destroy(&l->gpu_ir);
    free(l->cmds);
    release_dev(o);
}

static WINAPI HRESULT list_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12GraphicsCommandList, &IID_ID3D12GraphicsCommandList1, &IID_ID3D12GraphicsCommandList2, &IID_ID3D12GraphicsCommandList3,
                        &IID_ID3D12GraphicsCommandList4, &IID_ID3D12GraphicsCommandList5, &IID_ID3D12GraphicsCommandList6, &IID_ID3D12GraphicsCommandList7,
                        &IID_ID3D12CommandList, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 11);
}
static WINAPI int32_t list_type(void *this) { return ((list12 *)this)->type; }
static int list_bundle_commands_valid12(const list12 *bundle) {
    if (!bundle || bundle->type != 1 || !bundle->closed || bundle->validation_error) return 0;
    pso12 *pipeline = NULL;
    int topology_set = 0;
    for (uint32_t i = 0; i < bundle->n; i++) {
        const cmd12 *command = &bundle->cmds[i];
        switch (command->op) {
        case C_PSO: {
            pso12 *next = command->obj;
            if (!next || next->base.kind != K12_PSO || next->dev != bundle->dev || !next->render) return 0;
            pipeline = next;
            break;
        }
        case C_ROOTSIG: {
            rootsig12 *root = command->obj;
            if (!command->gfx || (root && (root->base.kind != K12_ROOTSIG || root->dev != bundle->dev))) return 0;
            break;
        }
        case C_HEAPS: {
            heap12 *resources = command->obj, *samplers = command->obj2;
            if ((resources && (resources->base.kind != K12_HEAP || resources->dev != bundle->dev || resources->desc.Type != 0)) ||
                (samplers && (samplers->base.kind != K12_HEAP || samplers->dev != bundle->dev || samplers->desc.Type != 1))) return 0;
            break;
        }
        case C_TABLE: case C_CONSTANTS: case C_ROOT_VA:
            if (!command->gfx) return 0;
            break;
        case C_TOPOLOGY:
            if (command->a < 1 || command->a > 5) return 0;
            topology_set = 1;
            break;
        case C_VB: case C_IB: case C_BLEND_FACTOR: case C_STENCIL_REF:
            break;
        case C_DRAW: case C_DRAW_INDEXED:
            if (command->a && command->b && (!pipeline || !topology_set)) return 0;
            break;
        case C_EXECUTE_INDIRECT: {
            command_signature12 *signature = command->obj;
            res12 *arguments = command->obj2;
            if (!pipeline || !topology_set || !signature || signature->dev != bundle->dev || !signature->gfx ||
                signature->root || signature->count != 1 || signature->payload_size != signature->args[0].size ||
                (signature->args[0].type != INDIRECT_DRAW && signature->args[0].type != INDIRECT_DRAW_INDEXED) ||
                command->obj3 || !arguments || arguments->dev != bundle->dev || !arguments->buffer) return 0;
            break;
        }
        // Bundles cannot clear, copy, transition resources, dispatch compute, query, or nest another bundle.
        default:
            return 0;
        }
    }
    return 1;
}
static int list_reserve12(list12 *list, uint32_t additional) {
    if (additional > UINT32_MAX - list->n) return 0;
    uint32_t need = list->n + additional;
    if (need <= list->cap) return 1;
    uint32_t capacity = list->cap ? list->cap : 64;
    while (capacity < need) {
        if (capacity > UINT32_MAX / 2) { capacity = need; break; }
        capacity *= 2;
    }
    if ((size_t)capacity > SIZE_MAX / sizeof *list->cmds) return 0;
    cmd12 *commands = realloc(list->cmds, (size_t)capacity * sizeof *commands);
    if (!commands) return 0;
    list->cmds = commands;
    list->cap = capacity;
    return 1;
}
static WINAPI HRESULT list_close(void *this) {
    list12 *l = this;
    pthread_mutex_lock(&l->dev->alloc_lock);
    if (l->closed) {
        pthread_mutex_unlock(&l->dev->alloc_lock);
        return E_FAIL;
    }
    l->closed = 1;
    list_allocator_stop_recording12(l);
    if (l->type == 1 && !list_bundle_commands_valid12(l)) l->validation_error = E_INVALIDARG;
    HRESULT hr = l->validation_error ? l->validation_error : S_OK;
    pthread_mutex_unlock(&l->dev->alloc_lock);
    return hr;
}
static void list_wait_replayed12(list12 *l) {
    while (__atomic_load_n(&l->pending, __ATOMIC_ACQUIRE)) usleep(50);
}
static WINAPI HRESULT list_reset(void *this, void *allocator, void *pso) {
    list12 *l = this;
    list_wait_replayed12(l);
    allocator12 *next_allocator = allocator;
    pthread_mutex_lock(&l->dev->alloc_lock);
    if (!l->closed) {
        pthread_mutex_unlock(&l->dev->alloc_lock);
        return E_FAIL;
    }
    if (l->validation_error) {
        HRESULT hr = l->validation_error;
        pthread_mutex_unlock(&l->dev->alloc_lock);
        return hr;
    }
    if (!next_allocator || next_allocator->base.kind != K12_ALLOCATOR || next_allocator->dev != l->dev ||
        next_allocator->type != l->type || next_allocator->recording_lists) {
        pthread_mutex_unlock(&l->dev->alloc_lock);
        return E_INVALIDARG;
    }
    list_clear(l);
    if (next_allocator != l->allocator) {
        d3d_addref(&next_allocator->base);
        allocator12 *old_allocator = l->allocator;
        l->allocator = next_allocator;
        if (old_allocator) d3d_release(&old_allocator->base);
    }
    l->allocator_generation = next_allocator->generation;
    next_allocator->recording_lists++;
    l->allocator_recording = 1;
    l->closed = 0;
    l->validation_error = S_OK;
    l->recording_topology = 4;
    l->recording_topology_set = 0;
    for (uint32_t i = 0; i < 4; i++) l->recording_blend_factor[i] = 1.0f;
    if (pso) {
        cmd12 *c = rec(l, C_PSO);
        c->obj = pso;
        hold(pso);
        l->recording_pso = ((d3d_object *)pso)->kind == K12_PSO ? (pso12 *)pso : NULL;
    }
    pthread_mutex_unlock(&l->dev->alloc_lock);
    return S_OK;
}
static WINAPI void list_set_pso(void *this, void *pso) {
    list12 *list = this;
    cmd12 *c = rec(list, C_PSO);
    c->obj = pso;
    hold(pso);
    list->recording_pso = pso && ((d3d_object *)pso)->kind == K12_PSO ? (pso12 *)pso : NULL;
}
static WINAPI void list_execute_bundle(void *this, void *bundle) {
    list12 *parent = this, *child = bundle;
    if (!parent || parent->validation_error) return;
    if (parent->closed || parent->type != 0 || !child || child->base.kind != K12_LIST ||
        child->dev != parent->dev || !list_bundle_commands_valid12(child)) {
        parent->validation_error = E_INVALIDARG;
        return;
    }
    if (parent->n > UINT32_MAX - 2 || child->n > UINT32_MAX - parent->n - 2 ||
        !list_reserve12(parent, child->n + 2)) {
        parent->validation_error = E_OUTOFMEMORY;
        return;
    }
    rec(parent, C_BUNDLE_BEGIN);
    parent->recording_pso = NULL;
    parent->recording_topology_set = 0;
    for (uint32_t i = 0; i < child->n; i++) {
        const cmd12 *source = &child->cmds[i];
        cmd12 *copy = &parent->cmds[parent->n++];
        *copy = *source;
        hold(copy->obj);
        hold(copy->obj2);
        hold(copy->obj3);
        if (copy->op == C_DRAW || copy->op == C_DRAW_INDEXED || copy->op == C_EXECUTE_INDIRECT) {
            copy->gpu_ir_recorded = 0;
            copy->gpu_ir_index = GPU_IR_NO_COMMAND;
            copy->gpu_ir_transient = copy->op != C_EXECUTE_INDIRECT;
        }
        if (copy->op == C_PSO) parent->recording_pso = copy->obj;
        // (input-assembler bindings a bundle sets stay set in its parent, as its pipeline state and topology do:
        //  the parent's later draws capture them)
        if (copy->op == C_VB)
            for (uint32_t b = 0; b < copy->b && copy->a + b < 16; b++)
                parent->recording_vertex_buffers[copy->a + b] = ((const VBV *)copy->values)[b];
        if (copy->op == C_IB) parent->recording_index_buffer = *(const IBV *)copy->values;
        if (copy->op == C_BLEND_FACTOR)
            memcpy(parent->recording_blend_factor, copy->values, sizeof parent->recording_blend_factor);
        if (copy->op == C_TOPOLOGY) {
            parent->recording_topology = copy->a;
            parent->recording_topology_set = 1;
        }
        if (copy->op == C_EXECUTE_INDIRECT && parent->recording_pso &&
            parent->recording_pso->render && parent->recording_pso->gpu_pipeline &&
            parent->recording_topology_set && parent->recording_topology >= 1 && parent->recording_topology <= 5) {
            command_signature12 *signature = copy->obj;
            res12 *arguments = copy->obj2;
            if (signature && arguments && !copy->obj3 && signature->gfx && !signature->root &&
                signature->count == 1 && arguments->gpu_buffer &&
                (signature->args[0].type == INDIRECT_DRAW || signature->args[0].type == INDIRECT_DRAW_INDEXED)) {
                gpu_ir_indirect_draw_info draw = {
                    .topology = parent->recording_topology,
                    .indexed = signature->args[0].type == INDIRECT_DRAW_INDEXED,
                    .draw_count = copy->a, .stride = signature->stride,
                    .argument_offset = copy->off
                };
                uint32_t index = GPU_IR_NO_COMMAND;
                int rc = gpu_ir_record_draw_indirect_counted(&parent->gpu_ir,
                    parent->recording_pso->gpu_pipeline, arguments->gpu_buffer, NULL, &draw, &index);
                if (rc == GPU_IR_OUT_OF_MEMORY) parent->validation_error = E_OUTOFMEMORY;
                else if (rc == GPU_IR_OK && index != GPU_IR_NO_COMMAND) {
                    copy->gpu_ir_recorded = 1;
                    copy->gpu_ir_index = index;
                }
            }
        }
    }
    rec(parent, C_BUNDLE_END);
}
static WINAPI void list_set_root(void *this, void *rs) { cmd12 *c = rec(this, C_ROOTSIG); c->obj = rs; hold(rs); }
static WINAPI void list_set_heaps(void *this, uint32_t n, void *const *heaps) {
    cmd12 *c = rec(this, C_HEAPS);
    for (uint32_t i = 0; i < n && i < 2; i++) {
        heap12 *h = heaps[i];
        if (!h) continue;
        if (h->desc.Type == 1) { c->obj2 = h; hold(h); } else { c->obj = h; hold(h); }
    }
}
static WINAPI void list_set_table(void *this, uint32_t param, uint64_t gpu) { cmd12 *c = rec(this, C_TABLE); c->a = param; c->va = gpu; }
static WINAPI void list_set_constant(void *this, uint32_t param, uint32_t value, uint32_t offset) {
    cmd12 *c = rec(this, C_CONSTANTS);
    c->a = param; c->b = 1; c->c = offset; c->values[0] = value;
}
static WINAPI void list_set_constants(void *this, uint32_t param, uint32_t n, const void *values, uint32_t offset) {
    cmd12 *c = rec(this, C_CONSTANTS);
    c->a = param; c->b = n > 64 ? 64 : n; c->c = offset;
    memcpy(c->values, values, 4 * c->b);
}
static WINAPI void list_set_root_va(void *this, uint32_t param, uint64_t va) { cmd12 *c = rec(this, C_ROOT_VA); c->a = param; c->va = va; }
static WINAPI void list_dispatch(void *this, uint32_t x, uint32_t y, uint32_t z) {
    list12 *list = this;
    if (!x || !y || !z) return;
    cmd12 *command = rec(list, C_DISPATCH);
    command->a = x; command->b = y; command->c = z;
    pso12 *pipeline = list->recording_pso;
    if (pipeline && pipeline->gpu_pipeline) {
        uint32_t index = GPU_IR_NO_COMMAND;
        int rc = gpu_ir_record_dispatch(&list->gpu_ir, pipeline->gpu_pipeline, x, y, z, &index);
        if (rc == GPU_IR_OUT_OF_MEMORY) list->validation_error = E_OUTOFMEMORY;
        else if (rc == GPU_IR_OK && index != GPU_IR_NO_COMMAND) {
            command->gpu_ir_recorded = 1;
            command->gpu_ir_index = index;
        }
    }
}
static WINAPI void list_execute_indirect(void *this, void *signature, uint32_t max_count, void *arguments,
                                         uint64_t argument_offset, void *count_buffer, uint64_t count_offset) {
    list12 *l = this;
    if (l->validation_error) return;
    command_signature12 *s = signature;
    res12 *arg = arguments, *count = count_buffer;
    int bundle_draw = l->type == 1 && s && s->gfx && !s->root && s->count == 1 && !count &&
        (s->args[0].type == INDIRECT_DRAW || s->args[0].type == INDIRECT_DRAW_INDEXED) &&
        s->payload_size == s->args[0].size;
    if (!s || s->base.kind != K12_COMMAND_SIGNATURE || s->dev != l->dev || !arg ||
        arg->base.kind != K12_RESOURCE || arg->dev != l->dev || arg->desc.Dimension != 1 ||
        (count && (count->base.kind != K12_RESOURCE || count->dev != l->dev || count->desc.Dimension != 1)) ||
        (l->type != 0 && l->type != 2 && !bundle_draw) || (l->type == 2 && s->gfx)) {
        l->validation_error = E_INVALIDARG;
        return;
    }
    uint64_t bytes = (uint64_t)max_count * s->stride;
    if ((argument_offset & 3) || argument_offset > arg->buffer.length || bytes > arg->buffer.length - argument_offset ||
        (count && ((count_offset & 3) || count_offset > count->buffer.length || 4 > count->buffer.length - count_offset))) {
        l->validation_error = E_INVALIDARG;
        return;
    }
    cmd12 *c = rec(l, C_EXECUTE_INDIRECT);
    c->obj = s; hold(s);
    c->obj2 = arg; hold(arg);
    c->obj3 = count; hold(count);
    c->a = max_count; c->off = argument_offset; c->off2 = count_offset;
    if ((!count || count->gpu_buffer) && !s->gfx && !s->root && s->count == 1 &&
        s->payload_size == 12 && s->args[0].type == INDIRECT_DISPATCH &&
        s->args[0].payload_offset == 0 && arg->gpu_buffer && l->recording_pso &&
        l->recording_pso->compute && l->recording_pso->gpu_pipeline) {
        gpu_ir_indirect_dispatch_info dispatch = {
            .dispatch_count = max_count, .stride = s->stride,
            .argument_offset = argument_offset, .count_offset = count ? count_offset : 0
        };
        uint32_t index = GPU_IR_NO_COMMAND;
        int rc = gpu_ir_record_dispatch_indirect_range_counted(&l->gpu_ir,
            l->recording_pso->gpu_pipeline, arg->gpu_buffer, count ? count->gpu_buffer : NULL,
            &dispatch, &index);
        if (rc == GPU_IR_OUT_OF_MEMORY) l->validation_error = E_OUTOFMEMORY;
        else if (rc == GPU_IR_OK && index != GPU_IR_NO_COMMAND) {
            c->gpu_ir_recorded = 1;
            c->gpu_ir_index = index;
        }
    }
    if ((!count || count->gpu_buffer) && s->gfx && !s->root && s->count == 1 && arg->gpu_buffer && l->recording_pso &&
        l->recording_pso->render && l->recording_pso->gpu_pipeline &&
        (s->args[0].type == INDIRECT_DRAW || s->args[0].type == INDIRECT_DRAW_INDEXED) &&
        s->payload_size == s->args[0].size) {
        uint32_t topology = l->recording_topology >= 1 && l->recording_topology <= 5 ? l->recording_topology : 4;
        gpu_ir_indirect_draw_info draw = {
            .topology = topology, .indexed = s->args[0].type == INDIRECT_DRAW_INDEXED,
            .draw_count = max_count, .stride = s->stride, .argument_offset = argument_offset,
            .count_offset = count_offset, .has_blend_constants = 1
        };
        memcpy(draw.blend_constants, l->recording_blend_factor, sizeof draw.blend_constants);
        uint32_t index = GPU_IR_NO_COMMAND;
        gpu_ir_draw_info captured = { .indexed = draw.indexed };
        int rc = l->type == 1 || !max_count ? GPU_IR_OK : list_capture_draw_buffers12(l, &captured);
        if (rc == GPU_IR_OK && l->type != 1 && max_count) {
            draw.vertex_buffers_captured = captured.vertex_buffers_captured;
            draw.vertex_buffer_count = captured.vertex_buffer_count;
            memcpy(draw.vertex_buffers, captured.vertex_buffers,
                   captured.vertex_buffer_count * sizeof captured.vertex_buffers[0]);
            draw.index_buffer = captured.index_buffer;
            draw.index_buffer_offset = captured.index_buffer_offset;
            draw.index_buffer_size = captured.index_buffer_size;
            draw.index_type = captured.index_type;
        }
        if (rc == GPU_IR_OK) rc = gpu_ir_record_draw_indirect_counted(&l->gpu_ir,
            l->recording_pso->gpu_pipeline, arg->gpu_buffer, count ? count->gpu_buffer : NULL, &draw, &index);
        list_release_captured_draw_buffers12(&captured);
        if (rc == GPU_IR_OUT_OF_MEMORY) l->validation_error = E_OUTOFMEMORY;
        else if (rc != GPU_IR_OK) l->validation_error = E_INVALIDARG;
        else if (rc == GPU_IR_OK && index != GPU_IR_NO_COMMAND) {
            c->gpu_ir_recorded = 1;
            c->gpu_ir_index = index;
        }
    }
}
static int list_record_gpu_buffer_copy(list12 *l, res12 *dst, uint64_t dst_off, res12 *src, uint64_t src_off, uint64_t size) {
    if (!dst || !src || !dst->gpu_buffer || !src->gpu_buffer) {
        vx_log("D3D12: buffer copy used a resource without a GPU buffer representation");
        return 0;
    }
    uint32_t index = GPU_IR_NO_COMMAND;
    int rc = gpu_ir_record_copy_buffer(&l->gpu_ir, src->gpu_buffer, src_off, dst->gpu_buffer, dst_off, size, &index);
    if (rc != GPU_IR_OK) {
        vx_log("D3D12: buffer copy could not be lowered to the common GPU IR (error %d)", rc);
        return 0;
    }
    if (index != GPU_IR_NO_COMMAND) {
        cmd12 *c = rec(l, C_GPU_IR);
        c->a = index;
        c->b = UINT32_MAX;
        c->gfx = D3D12_TRACK_COPY_BUFFER;
        c->obj = dst; c->obj2 = src;
        hold(dst); hold(src);
    }
    return 1;
}
static WINAPI void list_copy_buffer(void *this, void *dst, uint64_t dst_off, void *src, uint64_t src_off, uint64_t n) {
    if (trace_len12() && dst && ((res12 *)dst)->buffer && (((res12 *)dst)->desc.Width == trace_len12() || n == trace_len12())) {
        static volatile int k;
        res12 *d = dst, *sr = src;
        if (__sync_add_and_fetch(&k, 1) <= 40)
            vx_log("D3D12: CopyBufferRegion to %#llx+%llu (%llu bytes, heap %d) from %#llx+%llu (%llu bytes, heap %d): %llu bytes, list type %d",
                   (unsigned long long)d->buffer.gpuAddress, (unsigned long long)dst_off, (unsigned long long)d->desc.Width, d->heap.Type,
                   sr && sr->buffer ? (unsigned long long)sr->buffer.gpuAddress : 0ull, (unsigned long long)src_off,
                   sr ? (unsigned long long)sr->desc.Width : 0ull, sr ? sr->heap.Type : -1, (unsigned long long)n, ((list12 *)this)->type);
        id<MTLBuffer> db = d->buffer, sb = sr ? sr->buffer : nil;
        uint64_t doff = dst_off, soff = src_off;
        void (^show)(const char *) = ^(const char *when) {
            const uint32_t *dw = db.contents ? (const uint32_t *)((const uint8_t *)db.contents + doff) : NULL;
            const uint32_t *sw = sb.contents ? (const uint32_t *)((const uint8_t *)sb.contents + soff) : NULL;
            uint32_t dn = 0, sn = 0;
            for (uint64_t i = 0; i < n / 4; i++) { if (dw && dw[i]) dn++; if (sw && sw[i]) sn++; }
            vx_log("D3D12: copy to %#llx %s: %u of %llu words non-zero in the destination, %u in the source", (unsigned long long)db.gpuAddress, when,
                   dn, (unsigned long long)(n / 4), sn);
        };
        vx_log("D3D12: (the source has been mapped %d times, contents %p)", sr ? (int)sr->mapped : -1, sb.contents);
        show("when recorded");
        watch_add12(db);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ show("3 s later"); });
    }
    list_record_gpu_buffer_copy(this, dst, dst_off, src, src_off, n);
}
static WINAPI void list_copy_resource(void *this, void *dst, void *src) {
    list12 *l = this;
    res12 *d = dst, *s = src;
    if (trace_len12() && d && d->buffer && d->desc.Width == trace_len12()) {
        static volatile int k;
        if (__sync_add_and_fetch(&k, 1) <= 40) vx_log("D3D12: CopyResource to the %llu-byte buffer %#llx from %#llx (heap %d)", (unsigned long long)trace_len12(),
                                                      (unsigned long long)d->buffer.gpuAddress, s && s->buffer ? (unsigned long long)s->buffer.gpuAddress : 0ull, s ? s->heap.Type : -1);
    }
    if (d && s && d->gpu_buffer && s->gpu_buffer) {
        uint32_t index = GPU_IR_NO_COMMAND;
        int rc = gpu_ir_record_copy_resource(&l->gpu_ir, s->gpu_buffer, d->gpu_buffer, &index);
        if (rc == GPU_IR_OK) {
            cmd12 *command = rec(l, C_GPU_IR);
            command->a = index;
            command->b = UINT32_MAX;
            command->gfx = D3D12_TRACK_COPY_RESOURCE;
            command->obj = dst; command->obj2 = src;
            hold(dst); hold(src);
            return;
        }
        vx_log("D3D12: buffer CopyResource source and destination do not match (error %d)", rc);
        return;
    }
    if (d && s && d->gpu_texture && s->gpu_texture && d->texture.pixelFormat == s->texture.pixelFormat) {
        uint32_t index = GPU_IR_NO_COMMAND;
        int rc = gpu_ir_record_copy_resource(&l->gpu_ir, s->gpu_texture, d->gpu_texture, &index);
        if (rc == GPU_IR_OK) {
            cmd12 *command = rec(l, C_GPU_IR);
            command->a = index;
            command->b = UINT32_MAX;
            command->gfx = D3D12_TRACK_COPY_RESOURCE;
            command->obj = dst; command->obj2 = src;
            hold(dst); hold(src);
            return;
        }
        vx_log("D3D12: texture CopyResource source and destination do not match (error %d)", rc);
        return;
    }
    cmd12 *c = rec(this, C_COPY_RESOURCE);
    c->obj = dst; c->obj2 = src;
    hold(dst); hold(src);
}

static res12 *d3d12_barrier_resource(void *resource);
static uint32_t d3d12_resource_plane_count(uint32_t format);

static uint32_t d3d12_resolve_format_family(uint32_t format) {
    if (format >= 1 && format <= 4) return 1;
    if (format >= 5 && format <= 8) return 2;
    if (format >= 9 && format <= 14) return 3;
    if (format >= 15 && format <= 18) return 4;
    if (format >= 19 && format <= 22) return 5;
    if (format >= 23 && format <= 25) return 6;
    if (format >= 27 && format <= 32) return 7;
    if (format >= 33 && format <= 38) return 8;
    if (format >= 39 && format <= 43) return 9;
    if (format >= 44 && format <= 47) return 10;
    if (format >= 48 && format <= 52) return 11;
    if (format >= 53 && format <= 59) return 12;
    if (format >= 60 && format <= 64) return 13;
    return 0;
}

static int d3d12_resolve_typeless(uint32_t format) {
    switch (format) {
    case 1: case 5: case 9: case 15: case 19: case 23: case 27:
    case 33: case 39: case 44: case 48: case 53: case 60: case 70: case 73:
    case 76: case 79: case 82: case 90: case 92: case 94: case 97:
        return 1;
    default:
        return 0;
    }
}

static int d3d12_resolve_format_valid(uint32_t source, uint32_t destination, uint32_t format) {
    if (source == destination && source == format && !d3d12_resolve_typeless(source)) return 1;
    uint32_t family = d3d12_resolve_format_family(format);
    if (!family || d3d12_resolve_typeless(format) ||
        d3d12_resolve_format_family(source) != family || d3d12_resolve_format_family(destination) != family) return 0;
    if (!d3d12_resolve_typeless(source) && source != format) return 0;
    if (!d3d12_resolve_typeless(destination) && destination != format) return 0;
    return 1;
}

static int d3d12_resolve_location(res12 *resource, uint32_t subresource, gpu_ir_resolve_location *location) {
    uint32_t mips = resource->desc.MipLevels ? resource->desc.MipLevels : 1;
    uint32_t slices = resource->desc.DepthOrArraySize;
    uint32_t planes = d3d12_resource_plane_count(resource->desc.Format);
    uint64_t plane_size = (uint64_t)mips * slices;
    if (subresource >= plane_size * planes || !plane_size || subresource / plane_size) return 0;
    location->mip_level = subresource % mips;
    location->array_slice = subresource / mips;
    return 1;
}

static int d3d12_depth_resolve_format(uint32_t format) {
    return format == 20 || format == 40 || format == 45 || format == 55;
}

static int d3d12_depth_average_resolve_format(uint32_t format) {
    // Metal fragment depth output preserves the native depth representation for these formats.
    return format == 40 || format == 55;
}

static int d3d12_depth_resolve_supported12(dev12 *device) {
    return device && [device->mtl supportsFamily:MTLGPUFamilyApple3];
}

static uint32_t gpu_ir_mip_extent(uint32_t size, uint32_t mip);
static BOOL d3d12_color_resolve_float_format(MTLPixelFormat format);
static BOOL d3d12_color_resolve_format_supported(MTLPixelFormat format);

static void list_resolve_subresource_region_mode12(list12 *list, void *dst, uint32_t dst_subresource,
                                                   uint32_t dst_x, uint32_t dst_y, void *src,
                                                   uint32_t src_subresource, const RECT12 *src_rect,
                                                   uint32_t format, uint32_t mode) {
    if (list->validation_error) return;
    res12 *destination = d3d12_barrier_resource(dst), *source = d3d12_barrier_resource(src);
    gpu_ir_resolve_location src_location, dst_location;
    gpu_ir_resolve_region region = { 0 };
    int depth_format = d3d12_depth_resolve_format(format);
    int depth_mode = mode == GPU_IR_RESOLVE_MODE_MIN || mode == GPU_IR_RESOLVE_MODE_MAX;
    int depth_average_mode = mode == GPU_IR_RESOLVE_MODE_AVERAGE && d3d12_depth_average_resolve_format(format);
    int depth_operation = depth_format && (depth_mode || depth_average_mode);
    int color_average_mode = mode == GPU_IR_RESOLVE_MODE_AVERAGE && !depth_format &&
        d3d12_color_resolve_float_format(source ? source->texture.pixelFormat : MTLPixelFormatInvalid);
    int color_extrema_mode = depth_mode && !depth_format &&
        d3d12_color_resolve_format_supported(source ? source->texture.pixelFormat : MTLPixelFormatInvalid);
    int mode_valid = depth_operation || color_average_mode || color_extrema_mode;
    int flags_valid = depth_format
        ? destination && source && (source->desc.Flags & 0x2) && (destination->desc.Flags & 0x2) &&
          d3d12_depth_resolve_supported12(list->dev)
        : destination && source && (source->desc.Flags & 0x1) && destination->texture &&
          (destination->texture.usage & MTLTextureUsageRenderTarget) &&
          !(source->desc.Flags & 0x2) && !(destination->desc.Flags & 0x2);
    if (!mode_valid || !flags_valid || !destination || !source || source->dev != list->dev ||
        destination->dev != list->dev || !source->gpu_texture || !destination->gpu_texture || source == destination ||
        source->desc.Dimension != 3 || destination->desc.Dimension != 3 ||
        source->desc.SampleCount <= 1 || destination->desc.SampleCount != 1 ||
        source->desc.SampleQuality != 0 || destination->desc.SampleQuality != 0 ||
        !d3d12_resolve_format_valid(source->desc.Format, destination->desc.Format, format) ||
        source->texture.pixelFormat != destination->texture.pixelFormat ||
        !d3d12_resolve_location(source, src_subresource, &src_location) ||
        !d3d12_resolve_location(destination, dst_subresource, &dst_location)) {
        vx_log("D3D12: resolve has unsupported mode, resources, formats, or subresources");
        list->validation_error = E_INVALIDARG;
        return;
    }
    const gpu_ir_texture_desc *source_desc = gpu_ir_texture_description(source->gpu_texture);
    const gpu_ir_texture_desc *destination_desc = gpu_ir_texture_description(destination->gpu_texture);
    if (!source_desc || !destination_desc) {
        list->validation_error = E_INVALIDARG;
        return;
    }
    uint32_t source_width = gpu_ir_mip_extent(source_desc->width, src_location.mip_level);
    uint32_t source_height = gpu_ir_mip_extent(source_desc->height, src_location.mip_level);
    uint32_t destination_width = gpu_ir_mip_extent(destination_desc->width, dst_location.mip_level);
    uint32_t destination_height = gpu_ir_mip_extent(destination_desc->height, dst_location.mip_level);
    if (src_rect) {
        int64_t width = (int64_t)src_rect->right - src_rect->left;
        int64_t height = (int64_t)src_rect->bottom - src_rect->top;
        if (src_rect->left < 0 || src_rect->top < 0 || width <= 0 || height <= 0 ||
            width > UINT32_MAX || height > UINT32_MAX) {
            vx_log("D3D12: ResolveSubresourceRegion has an empty or negative source rectangle");
            list->validation_error = E_INVALIDARG;
            return;
        }
        region.source_x = (uint32_t)src_rect->left;
        region.source_y = (uint32_t)src_rect->top;
        region.width = (uint32_t)width;
        region.height = (uint32_t)height;
    } else {
        region.width = source_width;
        region.height = source_height;
    }
    region.destination_x = dst_x;
    region.destination_y = dst_y;
    if (!region.width || !region.height || region.source_x > source_width ||
        region.width > source_width - region.source_x || region.source_y > source_height ||
        region.height > source_height - region.source_y || region.destination_x > destination_width ||
        region.width > destination_width - region.destination_x || region.destination_y > destination_height ||
        region.height > destination_height - region.destination_y) {
        vx_log("D3D12: ResolveSubresourceRegion rectangle exceeds its source or destination subresource");
        list->validation_error = E_INVALIDARG;
        return;
    }
    BOOL full_region = !region.source_x && !region.source_y && !region.destination_x && !region.destination_y &&
        region.width == source_width && region.height == source_height &&
        source_width == destination_width && source_height == destination_height;
    if (depth_operation && !full_region &&
        (!d3d12_depth_average_resolve_format(format) ||
         !(destination->texture.usage & MTLTextureUsageRenderTarget))) {
        vx_log("D3D12: partial depth resolve format or destination usage is unsupported by the Metal path");
        list->validation_error = E_INVALIDARG;
        return;
    }
    if ((color_extrema_mode || (color_average_mode && !full_region)) &&
        !(destination->texture.usage & MTLTextureUsageRenderTarget)) {
        vx_log("D3D12: color resolve destination lacks Metal render-target usage");
        list->validation_error = E_INVALIDARG;
        return;
    }
    uint32_t index = GPU_IR_NO_COMMAND;
    int rc = gpu_ir_record_resolve_texture_region(&list->gpu_ir, source->gpu_texture, &src_location,
        destination->gpu_texture, &dst_location, &region, mode, &index);
    if (rc != GPU_IR_OK) {
        vx_log("D3D12: resolve could not be lowered to the common GPU IR (error %d)", rc);
        list->validation_error = rc == GPU_IR_OUT_OF_MEMORY ? E_OUTOFMEMORY : E_INVALIDARG;
        return;
    }
    cmd12 *command = rec(list, C_GPU_IR);
    command->a = index;
    command->b = UINT32_MAX;
    command->gfx = list->render_pass_resolving ? D3D12_TRACK_RENDER_PASS_RESOLVE : D3D12_TRACK_RESOLVE;
    command->obj = destination; command->obj2 = source;
    command->c = src_subresource; command->off = dst_subresource;
    hold(destination); hold(source);
}

static void list_resolve_subresource_mode12(list12 *list, void *dst, uint32_t dst_subresource,
                                            void *src, uint32_t src_subresource, uint32_t format, uint32_t mode) {
    res12 *destination = d3d12_barrier_resource(dst), *source = d3d12_barrier_resource(src);
    gpu_ir_resolve_location src_location, dst_location;
    if (!source || !destination || source->desc.Width != destination->desc.Width ||
        source->desc.Height != destination->desc.Height ||
        source->desc.DepthOrArraySize != destination->desc.DepthOrArraySize ||
        !d3d12_resolve_location(source, src_subresource, &src_location) ||
        !d3d12_resolve_location(destination, dst_subresource, &dst_location)) {
        if (list && !list->validation_error) list->validation_error = E_INVALIDARG;
        return;
    }
    const gpu_ir_texture_desc *source_desc = gpu_ir_texture_description(source->gpu_texture);
    const gpu_ir_texture_desc *destination_desc = gpu_ir_texture_description(destination->gpu_texture);
    if (!source_desc || !destination_desc ||
        gpu_ir_mip_extent(source_desc->width, src_location.mip_level) !=
            gpu_ir_mip_extent(destination_desc->width, dst_location.mip_level) ||
        gpu_ir_mip_extent(source_desc->height, src_location.mip_level) !=
            gpu_ir_mip_extent(destination_desc->height, dst_location.mip_level)) {
        if (list && !list->validation_error) list->validation_error = E_INVALIDARG;
        return;
    }
    list_resolve_subresource_region_mode12(list, dst, dst_subresource, 0, 0, src, src_subresource,
                                          NULL, format, mode);
}

static WINAPI void list_resolve_subresource(void *this, void *dst, uint32_t dst_subresource,
                                            void *src, uint32_t src_subresource, uint32_t format) {
    list_resolve_subresource_mode12(this, dst, dst_subresource, src, src_subresource, format,
                                    GPU_IR_RESOLVE_MODE_AVERAGE);
}

// Metal resolves color attachments by averaging, and depth attachments by sample min/max on Apple3+.
// Partial color average and supported D16/D32 depth resolves use render passes; decompression remains unsupported.
static WINAPI void list_resolve_subresource_region(void *this, void *dst, uint32_t dst_subresource,
                                                    uint32_t dst_x, uint32_t dst_y, void *src,
                                                    uint32_t src_subresource, RECT12 *src_rect,
                                                    uint32_t format, int32_t mode) {
    list12 *list = this;
    if (list->validation_error) return;
    uint32_t ir_mode = mode == D3D12_RESOLVE_MODE_AVERAGE12 ? GPU_IR_RESOLVE_MODE_AVERAGE :
                       mode == D3D12_RESOLVE_MODE_MIN12 ? GPU_IR_RESOLVE_MODE_MIN :
                       mode == D3D12_RESOLVE_MODE_MAX12 ? GPU_IR_RESOLVE_MODE_MAX : 0;
    if (!ir_mode) {
        vx_log("D3D12: ResolveSubresourceRegion has an unsupported resolve mode");
        list->validation_error = E_INVALIDARG;
        return;
    }
    list_resolve_subresource_region_mode12(list, dst, dst_subresource, dst_x, dst_y, src,
        src_subresource, src_rect, format, ir_mode);
}

static uint32_t gpu_ir_mip_extent(uint32_t size, uint32_t mip) {
    return mip >= 32 ? 1 : (size >> mip) ? (size >> mip) : 1;
}

// Resolve the D3D12 copy location into a common buffer or texture reference. Plane subresources and
// unsupported pixel formats stay on the established D3D12 replay path below.
static int d3d12_gpu_ir_copy_location(res12 *resource, const COPY_LOC *location, uint32_t x, uint32_t y, uint32_t z,
                                      gpu_ir_resource **ir_resource, gpu_ir_texture_location *ir_location) {
    if (!resource || !location || !ir_resource || !ir_location) return 0;
    memset(ir_location, 0, sizeof *ir_location);
    if (location->type == 1) {
        uint32_t block_width = 1, block_height = 1, bytes_per_block = 0;
        if (!resource->gpu_buffer || !gpu_ir_resource_is_buffer(resource->gpu_buffer) || !location->footprint.row_pitch ||
            !location->footprint.width || !location->footprint.height || !location->footprint.depth ||
            (location->footprint.offset & 511) || (location->footprint.row_pitch & 255)) return 0;
        if (!d3d_copy_format_block(location->footprint.format, &block_width, &block_height, &bytes_per_block)) return 0;
        ir_location->buffer_offset = location->footprint.offset;
        ir_location->row_pitch = location->footprint.row_pitch;
        ir_location->image_pitch = (uint64_t)location->footprint.row_pitch *
                                   (((uint64_t)location->footprint.height + block_height - 1) / block_height);
        ir_location->buffer_width = location->footprint.width;
        ir_location->buffer_height = location->footprint.height;
        ir_location->buffer_depth = location->footprint.depth;
        ir_location->origin_x = x;
        ir_location->origin_y = y;
        ir_location->origin_z = z;
        *ir_resource = resource->gpu_buffer;
        return 1;
    }
    if (location->type != 0 || !resource->gpu_texture) return 0;
    if (resource->texture.sampleCount > 1 || (resource->desc.Flags & 0x2)) return 0;
    const gpu_ir_texture_desc *texture = gpu_ir_texture_description(resource->gpu_texture);
    if (!texture) return 0;
    uint32_t mips = texture->mip_levels;
    uint32_t subresource = location->subresource;
    if (texture->is_3d) {
        if (subresource >= mips) return 0; // D3D12 3D texture subresources have no array-slice component.
        ir_location->mip_level = subresource;
    } else {
        ir_location->mip_level = subresource % mips;
        ir_location->array_slice = subresource / mips;
        if (ir_location->array_slice >= texture->array_layers) return 0; // Also excludes unsupported plane indices.
    }
    ir_location->origin_x = x;
    ir_location->origin_y = y;
    ir_location->origin_z = z;
    *ir_resource = resource->gpu_texture;
    return 1;
}

static int list_record_gpu_texture_copy(list12 *list, res12 *dst, const COPY_LOC *dst_location,
                                       uint32_t dst_x, uint32_t dst_y, uint32_t dst_z, res12 *src,
                                       const COPY_LOC *src_location, const BOX *box) {
    gpu_ir_resource *ir_src = NULL, *ir_dst = NULL;
    gpu_ir_texture_location src_loc, dst_loc;
    uint32_t src_x = box ? box->left : 0, src_y = box ? box->top : 0, src_z = box ? box->front : 0;
    if (!d3d12_gpu_ir_copy_location(src, src_location, src_x, src_y, src_z, &ir_src, &src_loc) ||
        !d3d12_gpu_ir_copy_location(dst, dst_location, dst_x, dst_y, dst_z, &ir_dst, &dst_loc)) return 0;
    const gpu_ir_texture_desc *src_texture = gpu_ir_texture_description(ir_src);
    const gpu_ir_texture_desc *dst_texture = gpu_ir_texture_description(ir_dst);
    if (!src_texture && !dst_texture) return 0;
    if (src_texture && dst_texture && src->texture.pixelFormat != dst->texture.pixelFormat) return 0;
    const gpu_ir_texture_desc *format = src_texture ? src_texture : dst_texture;
    if (src_location->type == 1) {
        uint32_t block_width, block_height, bytes_per_block;
        if (!d3d_copy_format_block(src_location->footprint.format, &block_width, &block_height, &bytes_per_block) ||
            block_width != format->block_width || block_height != format->block_height ||
            bytes_per_block != format->bytes_per_block ||
            d3d_mtl_format(src_location->footprint.format, NULL) != dst->texture.pixelFormat) return 0;
    }
    if (dst_location->type == 1) {
        uint32_t block_width, block_height, bytes_per_block;
        if (!d3d_copy_format_block(dst_location->footprint.format, &block_width, &block_height, &bytes_per_block) ||
            block_width != format->block_width || block_height != format->block_height ||
            bytes_per_block != format->bytes_per_block ||
            d3d_mtl_format(dst_location->footprint.format, NULL) != src->texture.pixelFormat) return 0;
    }
    if (src_texture && dst_texture && ir_src == ir_dst && src_loc.mip_level == dst_loc.mip_level &&
        src_loc.array_slice == dst_loc.array_slice) {
        vx_log("D3D12: CopyTextureRegion source and destination name the same subresource");
        return 1;
    }

    uint32_t width, height, depth;
    if (box) {
        if (box->right < box->left || box->bottom < box->top || box->back < box->front) {
            vx_log("D3D12: CopyTextureRegion has an inverted source box");
            return 1;
        }
        width = box->right - box->left;
        height = box->bottom - box->top;
        depth = box->back - box->front;
    } else if (src_texture) {
        width = gpu_ir_mip_extent(src_texture->width, src_loc.mip_level);
        height = gpu_ir_mip_extent(src_texture->height, src_loc.mip_level);
        depth = src_texture->is_3d ? gpu_ir_mip_extent(src_texture->depth, src_loc.mip_level) : 1;
    } else {
        width = src_location->footprint.width;
        height = src_location->footprint.height;
        depth = src_location->footprint.depth;
    }
    if (!width || !height || !depth) return 1;
    // A block-compressed region at a mip's edge is given in whole blocks (a 2x2 mip copied as one 4x4 block, a
    // 4-texel-wide footprint row): it ends at the mip's edge, as Metal wants it.
    const gpu_ir_texture_desc *edges[2] = { dst_texture ? gpu_ir_texture_description(ir_dst) : NULL,
                                            src_texture ? gpu_ir_texture_description(ir_src) : NULL };
    const gpu_ir_texture_location *at[2] = { &dst_loc, &src_loc };
    for (int k = 0; k < 2; k++) {
        const gpu_ir_texture_desc *t = edges[k];
        if (!t || t->block_width <= 1) continue;
        uint32_t mw = gpu_ir_mip_extent(t->width, at[k]->mip_level), mh = gpu_ir_mip_extent(t->height, at[k]->mip_level);
        if (at[k]->origin_x < mw && at[k]->origin_x + width > mw && at[k]->origin_x + width - mw < t->block_width) width = mw - at[k]->origin_x;
        if (at[k]->origin_y < mh && at[k]->origin_y + height > mh && at[k]->origin_y + height - mh < t->block_height) height = mh - at[k]->origin_y;
    }

    uint32_t index = GPU_IR_NO_COMMAND;
    int rc = gpu_ir_record_copy_texture(&list->gpu_ir, ir_src, &src_loc, ir_dst, &dst_loc,
                                        width, height, depth, &index);
    if (rc == GPU_IR_OK) {
        if (index != GPU_IR_NO_COMMAND) {
            cmd12 *command = rec(list, C_GPU_IR);
            command->a = index;
            command->b = UINT32_MAX;
            command->gfx = D3D12_TRACK_COPY_TEXTURE;
            command->obj = dst; command->obj2 = src;
            command->c = src_location->type == 0 ? src_location->subresource : 0;
            command->off = dst_location->type == 0 ? dst_location->subresource : 0;
            hold(command->obj); hold(command->obj2);
        }
        return 1;
    }
    if (rc == GPU_IR_RANGE) {
        vx_log("D3D12: CopyTextureRegion is outside the source/destination resource range");
        return 1;
    }
    return 0;
}

static WINAPI void list_copy_texture(void *this, const COPY_LOC *dst, uint32_t x, uint32_t y, uint32_t z, const COPY_LOC *src, const BOX *box) {
    list12 *list = this;
    if (dst && src && list_record_gpu_texture_copy(list, dst->res, dst, x, y, z, src->res, src, box)) return;
    cmd12 *c = rec(this, C_COPY_TEXTURE);
    c->dst = *dst; c->src = *src;
    c->a = x; c->b = y; c->c = z;
    if (box) { c->box = *box; c->has_box = 1; }
    hold(dst->res); hold(src->res);
}

// CopyTiles(tiled resource, start, size, buffer, offset, flags): whole tiles between a reserved resource and a buffer
// where each tile is 64 KiB, its texels row after row (flags: 2 buffer to resource, 4 resource to buffer). Standard
// tiles become ordinary copies; the packed mips' tiles have no defined layout on Windows either and are copied as
// each packed mip's texels, row after row, from the start of the packed tiles.
static WINAPI void list_copy_tiles(void *this, void *resource, const TILE_COORD12 *start, const TILE_REGION12 *size,
                                   void *buffer, uint64_t offset, int32_t flags) {
    res12 *r = resource, *b = buffer;
    tiling12 t;
    int to_resource = (flags & 2) != 0;
    if (!r || !b || r->base.kind != K12_RESOURCE || b->base.kind != K12_RESOURCE || !b->buffer || !start ||
        !tiling12_get(r, &t) || ((flags & 6) != 2 && (flags & 6) != 4)) { vx_log("D3D12: CopyTiles with invalid arguments"); return; }
    uint32_t *tiles = NULL, n = 0, cap = 0;
    if (tiling12_region(&t, start, size, &tiles, &n, &cap) < 0) { vx_log("D3D12: CopyTiles region outside the resource"); free(tiles); return; }
    for (uint32_t k = 0; k < n; k++) {
        uint64_t at = offset + (uint64_t)k * TILE_BYTES12;
        if (r->buffer) {
            uint64_t from = (uint64_t)tiles[k] * TILE_BYTES12;
            uint64_t bytes = r->desc.Width > from ? MIN((uint64_t)TILE_BYTES12, r->desc.Width - from) : 0;
            if (!bytes) continue;
            if (to_resource) list_copy_buffer(this, r, from, b, at, bytes);
            else list_copy_buffer(this, b, at, r, from, bytes);
            continue;
        }
        uint32_t mip, slice, x, y;
        tiling12_locate(&t, tiles[k], &mip, &slice, &x, &y);
        uint32_t bw = 1, bh = 1, bpb = 0;
        if (!d3d_copy_format_block(r->desc.Format, &bw, &bh, &bpb) || !bpb) continue;
        uint32_t mips = t.mips;
        if (mip >= t.std_mips) {                                   // a packed tile: the packed mips' texels, in order
            if (x) continue;                                       // (the first packed tile holds them all here)
            uint64_t cursor = at;
            for (uint32_t m = t.std_mips; m < mips; m++) {
                uint32_t w = MAX(1u, (uint32_t)(r->desc.Width >> m)), h = MAX(1u, r->desc.Height >> m);
                uint32_t pitch = (uint32_t)((((w + bw - 1) / bw) * bpb + 255) & ~255u);
                COPY_LOC tex = { .res = r, .type = 0 }, lin = { .res = b, .type = 1 };
                tex.subresource = m + slice * mips;
                lin.footprint = (FOOTPRINT){ cursor, r->desc.Format, (w + bw - 1) / bw * bw, (h + bh - 1) / bh * bh, 1, pitch };
                if (to_resource) list_copy_texture(this, &tex, 0, 0, 0, &lin, NULL);
                else list_copy_texture(this, &lin, 0, 0, 0, &tex, NULL);
                cursor += (uint64_t)pitch * ((h + bh - 1) / bh);
                cursor = (cursor + 511) & ~(uint64_t)511;
            }
            continue;
        }
        uint32_t w = MAX(1u, (uint32_t)(r->desc.Width >> mip)), h = MAX(1u, r->desc.Height >> mip);
        uint32_t x0 = x * t.shape[0], y0 = y * t.shape[1];
        uint32_t x1 = MIN(x0 + t.shape[0], (w + bw - 1) / bw * bw), y1 = MIN(y0 + t.shape[1], (h + bh - 1) / bh * bh);
        uint32_t pitch = t.shape[0] / bw * bpb;                    // (a tile's row: 256 to 1024 bytes)
        COPY_LOC tex = { .res = r, .type = 0 }, lin = { .res = b, .type = 1 };
        tex.subresource = mip + slice * mips;
        lin.footprint = (FOOTPRINT){ at, r->desc.Format, x1 - x0, y1 - y0, 1, pitch };
        if (to_resource) list_copy_texture(this, &tex, x0, y0, 0, &lin, NULL);
        else {
            BOX box = { x0, y0, 0, x1, y1, 1 };
            list_copy_texture(this, &lin, 0, 0, 0, &tex, &box);
        }
    }
    free(tiles);
}
static int d3d12_barrier_state_access(uint32_t state, uint32_t *access) {
    const uint32_t known = D3D12_STATE_VERTEX_CONSTANT | D3D12_STATE_INDEX | D3D12_STATE_RENDER_TARGET |
        D3D12_STATE_UAV | D3D12_STATE_DEPTH_WRITE | D3D12_STATE_DEPTH_READ | D3D12_STATE_NON_PIXEL_SHADER |
        D3D12_STATE_PIXEL_SHADER | D3D12_STATE_STREAM_OUT | D3D12_STATE_INDIRECT | D3D12_STATE_COPY_DEST |
        D3D12_STATE_COPY_SOURCE | D3D12_STATE_RESOLVE_DEST | D3D12_STATE_RESOLVE_SOURCE | D3D12_STATE_SHADING_RATE;
    const uint32_t read_only = D3D12_STATE_VERTEX_CONSTANT | D3D12_STATE_INDEX | D3D12_STATE_DEPTH_READ |
        D3D12_STATE_NON_PIXEL_SHADER | D3D12_STATE_PIXEL_SHADER | D3D12_STATE_INDIRECT |
        D3D12_STATE_COPY_SOURCE | D3D12_STATE_RESOLVE_SOURCE | D3D12_STATE_SHADING_RATE;
    const uint32_t write_only = D3D12_STATE_RENDER_TARGET | D3D12_STATE_DEPTH_WRITE | D3D12_STATE_STREAM_OUT |
        D3D12_STATE_COPY_DEST | D3D12_STATE_RESOLVE_DEST;
    if (state & ~known) return 0;
    if ((state & D3D12_STATE_UAV) && state != D3D12_STATE_UAV) return 0;
    if ((state & D3D12_STATE_DEPTH_WRITE) && state != D3D12_STATE_DEPTH_WRITE) return 0;
    uint32_t writes = state & write_only;
    if ((writes && (writes & (writes - 1))) || (writes && (state & read_only))) return 0;

    uint32_t result = 0;
    if (state & D3D12_STATE_VERTEX_CONSTANT) result |= GPU_IR_ACCESS_VERTEX_READ | GPU_IR_ACCESS_UNIFORM_READ;
    if (state & D3D12_STATE_INDEX) result |= GPU_IR_ACCESS_INDEX_READ;
    if (state & (D3D12_STATE_NON_PIXEL_SHADER | D3D12_STATE_PIXEL_SHADER))
        result |= GPU_IR_ACCESS_SHADER_READ;
    if (state & D3D12_STATE_SHADING_RATE) result |= GPU_IR_ACCESS_MEMORY_READ;
    if (state & D3D12_STATE_UAV) result |= GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE;
    if (state & (D3D12_STATE_RENDER_TARGET)) result |= GPU_IR_ACCESS_COLOR_WRITE;
    if (state & D3D12_STATE_DEPTH_READ) result |= GPU_IR_ACCESS_DEPTH_READ;
    if (state & D3D12_STATE_DEPTH_WRITE) result |= GPU_IR_ACCESS_DEPTH_WRITE;
    if (state & D3D12_STATE_STREAM_OUT) result |= GPU_IR_ACCESS_STREAM_WRITE;
    if (state & D3D12_STATE_INDIRECT) result |= GPU_IR_ACCESS_INDIRECT_READ;
    if (state & D3D12_STATE_COPY_SOURCE) result |= GPU_IR_ACCESS_COPY_READ;
    if (state & D3D12_STATE_COPY_DEST) result |= GPU_IR_ACCESS_COPY_WRITE;
    if (state & D3D12_STATE_RESOLVE_SOURCE) result |= GPU_IR_ACCESS_RESOLVE_READ;
    if (state & D3D12_STATE_RESOLVE_DEST) result |= GPU_IR_ACCESS_RESOLVE_WRITE;
    *access = result;
    return 1;
}

static res12 *d3d12_barrier_resource(void *resource) {
    if (!resource || ((d3d_object *)resource)->kind != K12_RESOURCE) return NULL;
    return resource;
}

static uint32_t d3d12_resource_plane_count(uint32_t format) {
    switch (format) {
    case 19: case 20: case 21: case 22: case 44: case 45: case 46: case 47:
    case 103: case 104: case 105: case 110: case 130:
        return 2;
    case 131: case 132:
        return 3;
    default:
        return 1;
    }
}

static int d3d12_barrier_validate_transition(res12 *resource, const TRANSITION_BARRIER *transition,
                                             uint32_t *before, uint32_t *after) {
    uint32_t before_state = (uint32_t)transition->before, after_state = (uint32_t)transition->after;
    if (!d3d12_barrier_state_access(before_state, before) || !d3d12_barrier_state_access(after_state, after) ||
        before_state == after_state || (before_state & D3D12_STATE_RAYTRACING_AS) ||
        (after_state & D3D12_STATE_RAYTRACING_AS) || resource->heap.Type == 2 || resource->heap.Type == 3 ||
        !d3d12_resource_state_dimension_valid12(&resource->desc, before_state) ||
        !d3d12_resource_state_dimension_valid12(&resource->desc, after_state) ||
        !d3d12_resource_state_descriptor_valid12(&resource->desc, before_state) ||
        !d3d12_resource_state_descriptor_valid12(&resource->desc, after_state)) return 0;
    uint32_t flags = (uint32_t)resource->desc.Flags;
    if ((before_state & D3D12_STATE_RENDER_TARGET || after_state & D3D12_STATE_RENDER_TARGET) && !(flags & 0x1)) return 0;
    if ((before_state & (D3D12_STATE_DEPTH_READ | D3D12_STATE_DEPTH_WRITE) ||
         after_state & (D3D12_STATE_DEPTH_READ | D3D12_STATE_DEPTH_WRITE)) && !(flags & 0x2)) return 0;
    if ((before_state & D3D12_STATE_UAV || after_state & D3D12_STATE_UAV) && !(flags & 0x4)) return 0;
    if ((before_state & D3D12_STATE_RESOLVE_SOURCE) && resource->desc.SampleCount <= 1) return 0;
    if ((after_state & D3D12_STATE_RESOLVE_SOURCE) && resource->desc.SampleCount <= 1) return 0;
    if ((before_state & D3D12_STATE_RESOLVE_DEST) && resource->desc.SampleCount != 1) return 0;
    if ((after_state & D3D12_STATE_RESOLVE_DEST) && resource->desc.SampleCount != 1) return 0;
    if (transition->subresource != D3D12_ALL_SUBRESOURCES) {
        uint64_t mips = resource->desc.MipLevels ? resource->desc.MipLevels : 1;
        uint64_t slices = resource->desc.Dimension == 4 ? 1 : resource->desc.DepthOrArraySize;
        uint64_t count = mips * slices * d3d12_resource_plane_count(resource->desc.Format);
        if (transition->subresource >= count) return 0;
    }
    return 1;
}

typedef struct {
    res12 *resource;
    uint32_t initial_state;
    uint32_t initial_promoted;
    res_state_override12 *overrides;
    res_state_pending12 *pending;
} resource_state_shadow12;

static int queue_validate_list_states12(dev12 *device, int32_t queue_type, id<MTLCommandQueue> metal_queue,
        list12 *list, const d3d12_indirect_validation_override12 *overrides, uint32_t override_count,
        resource_state_shadow12 **out_shadows, uint32_t *out_count);
static void queue_discard_list_states12(resource_state_shadow12 *shadows, uint32_t count);

static void state_overrides_free12(res_state_override12 *p) {
    while (p) { res_state_override12 *next = p->next; free(p); p = next; }
}
static void state_pending_free12(res_state_pending12 *p) {
    while (p) { res_state_pending12 *next = p->next; free(p); p = next; }
}
static int state_overrides_clone12(const res_state_override12 *source, res_state_override12 **out) {
    res_state_override12 **tail = out;
    *out = NULL;
    for (; source; source = source->next) {
        *tail = malloc(sizeof **tail);
        if (!*tail) { state_overrides_free12(*out); *out = NULL; return 0; }
        **tail = *source;
        (*tail)->next = NULL;
        tail = &(*tail)->next;
    }
    return 1;
}
static int state_pending_clone12(const res_state_pending12 *source, res_state_pending12 **out) {
    res_state_pending12 **tail = out;
    *out = NULL;
    for (; source; source = source->next) {
        *tail = malloc(sizeof **tail);
        if (!*tail) { state_pending_free12(*out); *out = NULL; return 0; }
        **tail = *source;
        (*tail)->next = NULL;
        tail = &(*tail)->next;
    }
    return 1;
}
static void state_shadow_clear12(resource_state_shadow12 *shadow) {
    state_overrides_free12(shadow->overrides);
    state_pending_free12(shadow->pending);
    memset(shadow, 0, sizeof *shadow);
}
static resource_state_shadow12 *state_shadow_get12(resource_state_shadow12 **shadows, uint32_t *count,
                                                    uint32_t *capacity, res12 *resource) {
    for (uint32_t i = 0; i < *count; i++) if ((*shadows)[i].resource == resource) return &(*shadows)[i];
    if (*count == *capacity) {
        if (*capacity > UINT32_MAX / 2) return NULL;
        uint32_t next_capacity = *capacity ? *capacity * 2 : 8;
        resource_state_shadow12 *next = realloc(*shadows, next_capacity * sizeof **shadows);
        if (!next) return NULL;
        *shadows = next;
        *capacity = next_capacity;
    }
    resource_state_shadow12 shadow = { .resource = resource, .initial_state = resource->initial_state,
                                       .initial_promoted = resource->initial_promoted };
    if (!state_overrides_clone12(resource->state_overrides, &shadow.overrides) ||
        !state_pending_clone12(resource->pending_transitions, &shadow.pending)) {
        state_shadow_clear12(&shadow);
        return NULL;
    }
    (*shadows)[*count] = shadow;
    return &(*shadows)[(*count)++];
}
static resource_state_shadow12 *state_shadow_find12(resource_state_shadow12 *shadows, uint32_t count, res12 *resource) {
    for (uint32_t i = 0; i < count; i++) if (shadows[i].resource == resource) return &shadows[i];
    return NULL;
}
static uint32_t state_get12(const resource_state_shadow12 *shadow, uint32_t subresource) {
    for (const res_state_override12 *p = shadow->overrides; p; p = p->next)
        if (p->subresource == subresource) return p->state;
    return shadow->initial_state;
}
static uint32_t state_promoted12(const resource_state_shadow12 *shadow, uint32_t subresource) {
    for (const res_state_override12 *p = shadow->overrides; p; p = p->next)
        if (p->subresource == subresource) return p->promoted;
    return shadow->initial_promoted;
}
static int state_is_read_only12(uint32_t state) {
    const uint32_t writes = D3D12_STATE_RENDER_TARGET | D3D12_STATE_UAV | D3D12_STATE_DEPTH_WRITE |
        D3D12_STATE_STREAM_OUT | D3D12_STATE_COPY_DEST | D3D12_STATE_RESOLVE_DEST |
        D3D12_STATE_VIDEO_DECODE_WRITE | D3D12_STATE_VIDEO_PROCESS_WRITE | D3D12_STATE_VIDEO_ENCODE_WRITE;
    uint32_t access;
    return state && !(state & writes) && d3d12_barrier_state_access(state, &access);
}
static int state_can_promote12(const res12 *resource, uint32_t state) {
    const uint32_t buffer_and_simultaneous = D3D12_STATE_VERTEX_CONSTANT | D3D12_STATE_INDEX |
        D3D12_STATE_RENDER_TARGET | D3D12_STATE_UAV | D3D12_STATE_NON_PIXEL_SHADER |
        D3D12_STATE_PIXEL_SHADER | D3D12_STATE_STREAM_OUT | D3D12_STATE_INDIRECT |
        D3D12_STATE_COPY_DEST | D3D12_STATE_COPY_SOURCE | D3D12_STATE_RESOLVE_DEST |
        D3D12_STATE_RESOLVE_SOURCE;
    const uint32_t ordinary_texture = D3D12_STATE_NON_PIXEL_SHADER | D3D12_STATE_PIXEL_SHADER |
        D3D12_STATE_COPY_DEST | D3D12_STATE_COPY_SOURCE;
    int buffer_or_simultaneous = resource->desc.Dimension == 1 || ((uint32_t)resource->desc.Flags & 0x20u);
    uint32_t allowed = buffer_or_simultaneous ? buffer_and_simultaneous : ordinary_texture;
    if (resource->heap.Type == 2) {
        // Upload buffers can serve read accesses covered by GENERIC_READ, but never a write access.
        allowed = D3D12_STATE_VERTEX_CONSTANT | D3D12_STATE_INDEX | D3D12_STATE_NON_PIXEL_SHADER |
            D3D12_STATE_PIXEL_SHADER | D3D12_STATE_INDIRECT | D3D12_STATE_COPY_SOURCE;
    } else if (resource->heap.Type == 3) {
        allowed = D3D12_STATE_COPY_DEST;
    }
    uint32_t access;
    return state && !(state & ~allowed) && d3d12_barrier_state_access(state, &access);
}
static int state_queue_access_allowed12(uint32_t state, int32_t queue_type) {
    if (!state) return 1;
    if (queue_type == 3)
        return state && !(state & ~(D3D12_STATE_COPY_SOURCE | D3D12_STATE_COPY_DEST));
    if (queue_type == 2)
        return !(state & (D3D12_STATE_PIXEL_SHADER | D3D12_STATE_RENDER_TARGET | D3D12_STATE_DEPTH_READ |
                          D3D12_STATE_DEPTH_WRITE | D3D12_STATE_INDEX | D3D12_STATE_STREAM_OUT |
                          D3D12_STATE_RESOLVE_SOURCE | D3D12_STATE_RESOLVE_DEST));
    return 1;
}
static int state_can_promote_on_queue12(const res12 *resource, uint32_t state, int32_t queue_type) {
    return state_can_promote12(resource, state) && state_queue_access_allowed12(state, queue_type);
}
static int state_assign12(resource_state_shadow12 *shadow, uint32_t subresource, uint32_t state, uint32_t promoted) {
    res_state_override12 **link = &shadow->overrides;
    while (*link && (*link)->subresource != subresource) link = &(*link)->next;
    if (state == shadow->initial_state && promoted == shadow->initial_promoted) {
        if (*link) { res_state_override12 *old = *link; *link = old->next; free(old); }
        return 1;
    }
    if (*link) { (*link)->state = state; (*link)->promoted = promoted; return 1; }
    res_state_override12 *entry = malloc(sizeof *entry);
    if (!entry) return 0;
    *entry = (res_state_override12){ .subresource = subresource, .state = state, .promoted = promoted,
                                     .next = shadow->overrides };
    shadow->overrides = entry;
    return 1;
}
static int state_set12(resource_state_shadow12 *shadow, uint32_t subresource, uint32_t state) {
    return state_assign12(shadow, subresource, state, 0);
}
static int state_promote12_set(resource_state_shadow12 *shadow, uint32_t subresource, uint32_t state) {
    return state_assign12(shadow, subresource, state, 1);
}
static uint64_t resource_subresource_count12(const res12 *resource) {
    if (resource->desc.Dimension == 1) return 1;
    uint64_t mips = resource->desc.MipLevels ? resource->desc.MipLevels : 1;
    uint64_t slices = resource->desc.Dimension == 4 ? 1 : resource->desc.DepthOrArraySize;
    return mips * slices * d3d12_resource_plane_count(resource->desc.Format);
}
static int state_all_matches12(const resource_state_shadow12 *shadow, uint32_t expected) {
    uint64_t count = resource_subresource_count12(shadow->resource);
    for (uint64_t i = 0; i < count; i++) {
        uint32_t actual = state_get12(shadow, (uint32_t)i);
        if (actual == expected) continue;
        if (actual != 0 || state_promoted12(shadow, (uint32_t)i) || !state_can_promote12(shadow->resource, expected)) return 0;
    }
    return 1;
}
static res_state_pending12 **state_pending_find12(resource_state_shadow12 *shadow, uint32_t subresource) {
    res_state_pending12 **link = &shadow->pending;
    while (*link && (*link)->subresource != subresource) link = &(*link)->next;
    return link;
}
static int state_has_pending12(const resource_state_shadow12 *shadow) { return shadow->pending != NULL; }
static int state_subresource_pending12(const resource_state_shadow12 *shadow, uint32_t subresource) {
    for (const res_state_pending12 *p = shadow->pending; p; p = p->next)
        if (p->subresource == D3D12_ALL_SUBRESOURCES || p->subresource == subresource) return 1;
    return 0;
}
static int state_matches_before12(const resource_state_shadow12 *shadow, uint32_t subresource, uint32_t before) {
    uint32_t actual = state_get12(shadow, subresource);
    if (actual == before) return 1;
    // Buffers and eligible textures may have decayed to COMMON after an earlier ExecuteCommandLists call.
    return actual == 0 && !state_promoted12(shadow, subresource) && state_can_promote12(shadow->resource, before);
}
// Return 1 for a valid transition, 0 for an invalid state sequence, and -1 on allocation failure.
static int state_transition12(resource_state_shadow12 *shadow, uint32_t subresource, uint32_t before,
                              uint32_t after, uint32_t flags) {
    if (flags == 1) { // BEGIN_ONLY: state changes only when the matching END_ONLY is submitted.
        if ((subresource == D3D12_ALL_SUBRESOURCES ? state_has_pending12(shadow) :
             state_subresource_pending12(shadow, subresource)) ||
            (subresource == D3D12_ALL_SUBRESOURCES ? !state_all_matches12(shadow, before) :
             !state_matches_before12(shadow, subresource, before))) return 0;
        res_state_pending12 *pending = malloc(sizeof *pending);
        if (!pending) return -1;
        *pending = (res_state_pending12){ subresource, before, after, shadow->pending };
        shadow->pending = pending;
        return 1;
    }
    if (flags == 2) { // END_ONLY
        res_state_pending12 **link = state_pending_find12(shadow, subresource);
        if (!*link || (*link)->before != before || (*link)->after != after ||
            (subresource == D3D12_ALL_SUBRESOURCES ? !state_all_matches12(shadow, before) :
             !state_matches_before12(shadow, subresource, before))) return 0;
        if (subresource == D3D12_ALL_SUBRESOURCES) {
            shadow->initial_state = after;
            shadow->initial_promoted = 0;
            state_overrides_free12(shadow->overrides);
            shadow->overrides = NULL;
        } else if (!state_set12(shadow, subresource, after)) return -1;
        res_state_pending12 *done = *link;
        *link = done->next;
        free(done);
        return 1;
    }
    if (subresource == D3D12_ALL_SUBRESOURCES) {
        if (state_has_pending12(shadow)) return 0;
        if (!state_all_matches12(shadow, before)) return 0;
        shadow->initial_state = after;
        shadow->initial_promoted = 0;
        state_overrides_free12(shadow->overrides);
        shadow->overrides = NULL;
        return 1;
    }
    if (state_subresource_pending12(shadow, subresource)) return 0;
    if (!state_matches_before12(shadow, subresource, before)) return 0;
    return state_set12(shadow, subresource, after) ? 1 : -1;
}
static int state_resource_use12(resource_state_shadow12 *shadow, uint32_t subresource,
                                uint32_t required_state, int write, int32_t queue_type) {
    uint64_t count = resource_subresource_count12(shadow->resource);
    if (!state_queue_access_allowed12(required_state, queue_type)) return 0;
    if (subresource == D3D12_ALL_SUBRESOURCES) {
        if (state_has_pending12(shadow)) return 0;
        for (uint64_t i = 0; i < count; i++)
            if (!state_resource_use12(shadow, (uint32_t)i, required_state, write, queue_type)) return 0;
        return 1;
    }
    if (subresource >= count || state_subresource_pending12(shadow, subresource)) return 0;
    uint32_t state = state_get12(shadow, subresource), access;
    if (state == 0 && !state_promoted12(shadow, subresource)) {
        if (!state_can_promote_on_queue12(shadow->resource, required_state, queue_type)) return 0;
        return state_promote12_set(shadow, subresource, required_state) ? 1 : -1;
    }
    if (state_promoted12(shadow, subresource) && state_is_read_only12(state) &&
        state_is_read_only12(required_state) && state_can_promote_on_queue12(shadow->resource, required_state, queue_type)) {
        uint32_t merged = state | required_state;
        if (merged != state && state_can_promote_on_queue12(shadow->resource, merged, queue_type))
            return state_promote12_set(shadow, subresource, merged) ? 1 : -1;
    }
    if (write) return state == required_state;
    return (state & required_state) && d3d12_barrier_state_access(state, &access);
}
static int state_validate_resource_pair12(dev12 *device, int32_t queue_type, resource_state_shadow12 **shadows,
                                          uint32_t *count, uint32_t *capacity, res12 *destination,
                                          uint32_t destination_subresource, uint32_t destination_state,
                                          res12 *source, uint32_t source_subresource, uint32_t source_state) {
    if (!destination || !source || destination == source || destination->dev != device || source->dev != device ||
        !resource_resident12(destination) || !resource_resident12(source)) return 0;
    resource_state_shadow12 *source_shadow = state_shadow_get12(shadows, count, capacity, source);
    resource_state_shadow12 *destination_shadow = state_shadow_get12(shadows, count, capacity, destination);
    if (!source_shadow || !destination_shadow) return -1;
    source_shadow = state_shadow_find12(*shadows, *count, source);
    destination_shadow = state_shadow_find12(*shadows, *count, destination);
    if (!source_shadow || !destination_shadow) return -1;
    int source_result = state_resource_use12(source_shadow, source_subresource, source_state, 0, queue_type);
    if (source_result <= 0) return source_result;
    return state_resource_use12(destination_shadow, destination_subresource, destination_state, 1, queue_type);
}
static int state_validate_resource_use_desc12(dev12 *device, int32_t queue_type, resource_state_shadow12 **shadows,
                                              uint32_t *count, uint32_t *capacity, res12 *resource,
                                              uint32_t subresource, uint32_t required_state, int write) {
    if (!resource || resource->dev != device || !resource_resident12(resource)) return 0;
    resource_state_shadow12 *shadow = state_shadow_get12(shadows, count, capacity, resource);
    if (!shadow) return -1;
    shadow = state_shadow_find12(*shadows, *count, resource);
    if (!shadow) return -1;
    return state_resource_use12(shadow, subresource, required_state, write, queue_type);
}
static res12 *state_resource_for_gpu_range_locked12(dev12 *device, uint64_t va, uint64_t size) {
    if (!va || !size || size > UINT64_MAX - va) return NULL;
    return bufs_find12(device, va, size);
}

static WINAPI void list_barrier(void *this, uint32_t n, const void *barriers) {
    list12 *list = this;
    if (list->validation_error) return;
    if (n && !barriers) { list->validation_error = E_INVALIDARG; return; }
    const RESOURCE_BARRIER *items = barriers;
    for (uint32_t i = 0; i < n; i++) {
        const RESOURCE_BARRIER *item = &items[i];
        gpu_ir_barrier_desc ir = { 0 };
        if (item->flags < 0 || item->flags > 2) { list->validation_error = E_INVALIDARG; return; }
        if (item->type == 0) {
            res12 *resource = d3d12_barrier_resource(item->transition.resource);
            if (!resource || !d3d12_barrier_validate_transition(resource, &item->transition,
                                                                 &ir.before_access, &ir.after_access)) {
                list->validation_error = E_INVALIDARG;
                return;
            }
            ir.type = resource->gpu_buffer || resource->gpu_texture ? GPU_IR_BARRIER_TRANSITION : GPU_IR_BARRIER_GLOBAL;
            ir.flags = item->flags == 1 ? GPU_IR_BARRIER_BEGIN_ONLY : item->flags == 2 ? GPU_IR_BARRIER_END_ONLY : 0;
            ir.subresource = item->transition.subresource;
            ir.resource = resource->gpu_buffer ? resource->gpu_buffer : resource->gpu_texture;
        } else if (item->type == 1) {
            if (item->flags) { list->validation_error = E_INVALIDARG; return; }
            res12 *before = d3d12_barrier_resource(item->aliasing.before);
            res12 *after = d3d12_barrier_resource(item->aliasing.after);
            if ((item->aliasing.before && !before) || (item->aliasing.after && !after)) {
                list->validation_error = E_INVALIDARG;
                return;
            }
            ir.type = GPU_IR_BARRIER_ALIASING;
            ir.resource = before ? (before->gpu_buffer ? before->gpu_buffer : before->gpu_texture) : NULL;
            ir.resource_after = after ? (after->gpu_buffer ? after->gpu_buffer : after->gpu_texture) : NULL;
            if ((before && !ir.resource) || (after && !ir.resource_after)) {
                ir.type = GPU_IR_BARRIER_GLOBAL;
                ir.resource = ir.resource_after = NULL;
            }
        } else if (item->type == 2) {
            if (item->flags) { list->validation_error = E_INVALIDARG; return; }
            res12 *resource = d3d12_barrier_resource(item->uav.resource);
            if (item->uav.resource && (!resource || !(resource->desc.Flags & 0x4))) {
                list->validation_error = E_INVALIDARG;
                return;
            }
            ir.type = resource && (resource->gpu_buffer || resource->gpu_texture) ? GPU_IR_BARRIER_UAV : GPU_IR_BARRIER_GLOBAL;
            ir.resource = resource ? (resource->gpu_buffer ? resource->gpu_buffer : resource->gpu_texture) : NULL;
            ir.before_access = ir.after_access = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE;
        } else {
            list->validation_error = E_INVALIDARG;
            return;
        }
        uint32_t index = GPU_IR_NO_COMMAND;
        int rc = gpu_ir_record_barrier(&list->gpu_ir, &ir, &index);
        if (rc != GPU_IR_OK) {
            list->validation_error = rc == GPU_IR_OUT_OF_MEMORY ? E_OUTOFMEMORY : E_INVALIDARG;
            return;
        }
        cmd12 *command = rec(list, C_GPU_IR);
        command->a = index;
        command->b = (uint32_t)item->type;
        if (item->type == 0) {
            command->obj = item->transition.resource;
            command->c = item->transition.subresource;
            command->off = (uint32_t)item->transition.before;
            command->off2 = (uint32_t)item->transition.after;
            command->gfx = item->flags;
            hold(command->obj);
        }
    }
}

// Render target and depth-stencil views: CPU-only descriptors holding the Metal texture view they draw to (read
// when OMSetRenderTargets or a clear is recorded, as D3D12 reads them then).
static WINAPI void dev_create_rtv(void *this, void *resource, const RTV_DESC *desc, size_t cpu) {
    res12 *r = resource;
    VxDesc v = { 0 };
    id keep = nil;
    uint32_t subresource = 0;
    if (r && r->texture) {
        int32_t dim = desc ? desc->dim : (r->desc.DepthOrArraySize > 1 ? 5 : 4);   // D3D12_RTV_DIMENSION: 4 tex2d, 5 tex2darray
        uint32_t mip = 0, slice = 0, slices = 1;
        uint32_t plane = 0;
        if (desc && dim == 4) { mip = desc->tex2d.mip; plane = desc->tex2d.plane; }
        if (desc && dim == 5) { mip = desc->tex2darray.mip; slice = desc->tex2darray.first; slices = desc->tex2darray.size; plane = desc->tex2darray.plane; }
        if (desc && dim == 7) { slice = desc->raw[0]; slices = desc->raw[1]; }
        // D3D12 RTV dimensions 6 and 7 are TEXTURE2DMS and TEXTURE2DMSARRAY. For a
        // single array slice, a 2DMS texture view binds that slice as an ordinary attachment.
        id<MTLTexture> t = texture_view(r, desc ? desc->format : 0, r->texture.sampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D, mip, 1, slice, 1);
        (void)slices;
        v.fmt = desc ? desc->format : r->desc.Format;
        keep = t;
        uint32_t mips = r->desc.MipLevels ? r->desc.MipLevels : 1;
        uint32_t array_size = r->desc.Dimension == 4 ? 1 : r->desc.DepthOrArraySize;
        subresource = mip + (r->desc.Dimension == 4 ? 0 : slice * mips) + plane * mips * array_size;
    }
    put_desc(cpu, &v, keep, -1);
    if (r && keep) put_target_metadata(cpu, r, subresource, 0);
}

static WINAPI void dev_create_dsv(void *this, void *resource, const DSV_DESC *desc, size_t cpu) {
    res12 *r = resource;
    VxDesc v = { 0 };
    id keep = nil;
    uint32_t subresource = 0;
    if (r && r->texture) {
        uint32_t mip = desc && desc->dim == 3 ? desc->tex2d.mip : 0;
        uint32_t slice = desc && desc->dim == 4 ? desc->tex2darray.first : 0;
        if (desc && desc->dim == 4) mip = desc->tex2darray.mip;
        if (desc && desc->dim == 6) slice = desc->raw[0];
        keep = [r->texture newTextureViewWithPixelFormat:r->texture.pixelFormat textureType:r->texture.sampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D
                                                  levels:NSMakeRange(mip, 1) slices:NSMakeRange(slice, 1)];
        v.fmt = desc ? desc->format : r->desc.Format;
        uint32_t mips = r->desc.MipLevels ? r->desc.MipLevels : 1;
        subresource = mip + (r->desc.Dimension == 4 ? 0 : slice * mips);
    }
    put_desc(cpu, &v, keep, -1);
    if (r && keep) put_target_metadata(cpu, r, subresource, desc ? (uint32_t)desc->flags : 0);
}

// The Metal texture a CPU descriptor (RTV/DSV) names, retained for a recorded command; NULL for none.
static void *view_texture(size_t cpu) {
    uint32_t i;
    heap12 *h = heap_at(cpu, &i);
    if (!h || !h->keep[i]) return NULL;
    return (void *)CFBridgingRetain(h->keep[i]);
}

// Return a retained API resource because the descriptor heap may be released after command recording.
static res12 *view_target_metadata(size_t cpu, uint32_t *subresource, uint32_t *flags) {
    pthread_mutex_lock(&g_heaps_lock);
    res12 *resource = NULL;
    for (uint32_t k = 0; k < g_nheaps; k++) {
        heap12 *h = g_heaps[k];
        if (cpu < (size_t)h->cpu || cpu >= (size_t)h->cpu + 32 * (size_t)h->desc.NumDescriptors) continue;
        uint32_t i = (uint32_t)((cpu - (size_t)h->cpu) / 32);
        if (h->target_resource && h->target_resource[i]) {
            resource = h->target_resource[i];
            d3d_addref(&resource->base);
            if (subresource) *subresource = h->target_subresource[i];
            if (flags) *flags = h->target_flags[i];
        }
        break;
    }
    pthread_mutex_unlock(&g_heaps_lock);
    return resource;
}

// ---- graphics pipeline states -------------------------------------------------------------------------------------------

static MTLBlendFactor blend_factor(int32_t b) {
    switch (b) {
    case 1: return MTLBlendFactorZero;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    case 16: return MTLBlendFactorSource1Color;
    case 17: return MTLBlendFactorOneMinusSource1Color;
    case 18: return MTLBlendFactorSource1Alpha;
    case 19: return MTLBlendFactorOneMinusSource1Alpha;
    case 20: return MTLBlendFactorBlendAlpha;
    case 21: return MTLBlendFactorOneMinusBlendAlpha;
    default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation blend_op(int32_t o) {
    switch (o) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static uint32_t blend_factor_ir12(int32_t factor) {
    switch (factor) {
    case 1: return GPU_IR_BLEND_FACTOR_ZERO;
    case 2: return GPU_IR_BLEND_FACTOR_ONE;
    case 3: return GPU_IR_BLEND_FACTOR_SOURCE_COLOR;
    case 4: return GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE_COLOR;
    case 5: return GPU_IR_BLEND_FACTOR_SOURCE_ALPHA;
    case 6: return GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE_ALPHA;
    case 7: return GPU_IR_BLEND_FACTOR_DESTINATION_ALPHA;
    case 8: return GPU_IR_BLEND_FACTOR_ONE_MINUS_DESTINATION_ALPHA;
    case 9: return GPU_IR_BLEND_FACTOR_DESTINATION_COLOR;
    case 10: return GPU_IR_BLEND_FACTOR_ONE_MINUS_DESTINATION_COLOR;
    case 11: return GPU_IR_BLEND_FACTOR_SOURCE_ALPHA_SATURATE;
    case 14: return GPU_IR_BLEND_FACTOR_CONSTANT_COLOR;
    case 15: return GPU_IR_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 16: return GPU_IR_BLEND_FACTOR_SOURCE1_COLOR;
    case 17: return GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE1_COLOR;
    case 18: return GPU_IR_BLEND_FACTOR_SOURCE1_ALPHA;
    case 19: return GPU_IR_BLEND_FACTOR_ONE_MINUS_SOURCE1_ALPHA;
    case 20: return GPU_IR_BLEND_FACTOR_CONSTANT_ALPHA;
    case 21: return GPU_IR_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return GPU_IR_BLEND_FACTOR_ONE;
    }
}

static uint32_t blend_operation_ir12(int32_t operation) {
    switch (operation) {
    case 2: return GPU_IR_BLEND_OPERATION_SUBTRACT;
    case 3: return GPU_IR_BLEND_OPERATION_REVERSE_SUBTRACT;
    case 4: return GPU_IR_BLEND_OPERATION_MINIMUM;
    case 5: return GPU_IR_BLEND_OPERATION_MAXIMUM;
    default: return GPU_IR_BLEND_OPERATION_ADD;
    }
}

static gpu_ir_color_blend_state color_blend_ir12(const GFX_PSO_DESC *desc) {
    gpu_ir_color_blend_state state = { 0 };
    if (!desc || !desc->nrt) return state;
    state.attachment_count = desc->nrt;
    state.alpha_to_coverage_enabled = desc->blend.a2c != 0;
    for (uint32_t i = 0; i < desc->nrt; i++) {
        const RT_BLEND *blend = &desc->blend.rt[desc->blend.independent ? i : 0];
        state.attachments[i] = (gpu_ir_color_blend_attachment){
            .enabled = blend->enable != 0,
            .source_color_factor = blend_factor_ir12(blend->src),
            .destination_color_factor = blend_factor_ir12(blend->dst),
            .color_operation = blend_operation_ir12(blend->op),
            .source_alpha_factor = blend_factor_ir12(blend->src_a),
            .destination_alpha_factor = blend_factor_ir12(blend->dst_a),
            .alpha_operation = blend_operation_ir12(blend->op_a),
            .write_mask = blend->mask & GPU_IR_COLOR_WRITE_ALL
        };
    }
    return state;
}
static MTLCompareFunction compare(int32_t f) {
    static const MTLCompareFunction m[] = { MTLCompareFunctionAlways, MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
                                            MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
                                            MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways };
    return f >= 1 && f <= 8 ? m[f] : MTLCompareFunctionAlways;
}
static MTLStencilOperation stencil_op(int32_t o) {
    static const MTLStencilOperation m[] = { MTLStencilOperationKeep, MTLStencilOperationKeep, MTLStencilOperationZero, MTLStencilOperationReplace,
                                             MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp, MTLStencilOperationInvert,
                                             MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap };
    return o >= 1 && o <= 8 ? m[o] : MTLStencilOperationKeep;
}
static MTLStencilDescriptor *stencil(const STENCIL_OP *o, uint8_t rm, uint8_t wm) {
    MTLStencilDescriptor *s = [MTLStencilDescriptor new];
    s.stencilFailureOperation = stencil_op(o->fail);
    s.depthFailureOperation = stencil_op(o->depth_fail);
    s.depthStencilPassOperation = stencil_op(o->pass);
    s.stencilCompareFunction = compare(o->func);
    s.readMask = rm;
    s.writeMask = wm;
    return s;
}

static MTLVertexFormat vertex_format(uint32_t f) {
    switch (f) {
    case 2: return MTLVertexFormatFloat4;   case 6: return MTLVertexFormatFloat3;   case 16: return MTLVertexFormatFloat2;  case 41: return MTLVertexFormatFloat;
    case 3: return MTLVertexFormatUInt4;    case 7: return MTLVertexFormatUInt3;    case 17: return MTLVertexFormatUInt2;   case 42: return MTLVertexFormatUInt;
    case 4: return MTLVertexFormatInt4;     case 8: return MTLVertexFormatInt3;     case 18: return MTLVertexFormatInt2;    case 43: return MTLVertexFormatInt;
    case 10: return MTLVertexFormatHalf4;   case 34: return MTLVertexFormatHalf2;   case 54: return MTLVertexFormatHalf;
    case 11: return MTLVertexFormatUShort4Normalized; case 35: return MTLVertexFormatUShort2Normalized; case 56: return MTLVertexFormatUShortNormalized;
    case 12: return MTLVertexFormatUShort4; case 36: return MTLVertexFormatUShort2; case 57: return MTLVertexFormatUShort;
    case 13: return MTLVertexFormatShort4Normalized;  case 37: return MTLVertexFormatShort2Normalized;  case 58: return MTLVertexFormatShortNormalized;
    case 14: return MTLVertexFormatShort4;  case 38: return MTLVertexFormatShort2;  case 59: return MTLVertexFormatShort;
    case 24: return MTLVertexFormatUInt1010102Normalized;
    case 28: return MTLVertexFormatUChar4Normalized;  case 49: return MTLVertexFormatUChar2Normalized; case 61: return MTLVertexFormatUCharNormalized;
    case 30: return MTLVertexFormatUChar4;  case 50: return MTLVertexFormatUChar2;  case 62: return MTLVertexFormatUChar;
    case 31: return MTLVertexFormatChar4Normalized;   case 51: return MTLVertexFormatChar2Normalized;  case 63: return MTLVertexFormatCharNormalized;
    case 32: return MTLVertexFormatChar4;   case 52: return MTLVertexFormatChar2;   case 64: return MTLVertexFormatChar;
    case 87: return MTLVertexFormatUChar4Normalized_BGRA;
    }
    return MTLVertexFormatInvalid;
}

static uint32_t d3d12_vertex_format_size12(uint32_t format) {
    switch (format) {
    case 2: case 3: case 4: return 16;
    case 6: case 7: case 8: return 12;
    case 10: case 11: case 12: case 13: case 14: return 8;
    case 16: case 17: case 18: return 8;
    case 24: case 28: case 30: case 31: case 32: case 87: return 4;
    case 34: case 35: case 36: case 37: case 38: return 4;
    case 41: case 42: case 43: return 4;
    case 49: case 50: case 51: case 52: return 2;
    case 54: case 56: case 57: case 58: case 59: return 2;
    case 61: case 62: case 63: case 64: return 1;
    default: return 0;
    }
}

// DXBC (shader model 4 and 5: what fxc compiles, and what Unreal Engine 4 and Unity give Direct3D 12 by default)
// translated into the descriptor ABI of DXIL shaders, with a reflection of the same shape, so root signatures,
// descriptor tables and validation treat both alike.
static int d3d12_is_dxbc_program(const BYTECODE *code) {
    return code && code->code && dxbc_program_type(code->code, code->size) >= 0;
}
static int dxbc_to_msl12(const BYTECODE *code, dxil_msl *out) {
    static dxbc_shader s;                                          // (large: kept off the stack, under its own lock)
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&lock);
    int rc = dxbc_translate_d3d12(code->code, code->size, &s);
    if (rc) {
        snprintf(out->error, sizeof out->error, "%s", s.error);
        dxbc_free(&s);
        pthread_mutex_unlock(&lock);
        return -1;
    }
    dxbc_reflect12(&s, out);
    out->msl = s.msl;
    s.msl = NULL;
    dxbc_free(&s);
    pthread_mutex_unlock(&lock);
    return 0;
}
// A DXBC translation's reflection in DXIL's shape (out->msl is left to the caller).
static void dxbc_reflect12(const dxbc_shader *src, dxil_msl *out) {
    out->stage = src->type;
    memcpy(out->threads, src->threads, sizeof out->threads);
    out->nslots = (uint32_t)src->nslots;
    for (int i = 0; i < src->nslots; i++)
        out->slots[i] = (dxil_msl_slot){ .cls = src->slots[i].cls, .kind = src->slots[i].kind, .comp = src->slots[i].comp,
            .has_counter = src->slots[i].counter, .space = 0, .lower = src->slots[i].reg, .count = 1, .stride = src->slots[i].stride,
            .uav_access = src->slots[i].cls == DXBC_SLOT_UAV ? DXIL_MSL_ACCESS_READ | DXIL_MSL_ACCESS_WRITE : 0 };
    // signature elements: DXBC component types 1 uint, 2 int, 3 float -> DXIL's 5, 4, 9; the system values engines
    // feed or read as DXIL's semantic kinds (position 3, vertex/instance id 1/2, targets 16, depth 17)
    for (int io = 0; io < 2; io++) {
        const dxbc_element *e = io ? src->outputs : src->inputs;
        int n = io ? src->noutputs : src->ninputs;
        dxil_msl_element *dst = io ? out->outputs : out->inputs;
        uint32_t count = 0;
        for (int i = 0; i < n && count < 32; i++) {
            dxil_msl_element *x = &dst[count++];
            snprintf(x->name, sizeof x->name, "%s", e[i].semantic);
            x->index = e[i].index;
            x->kind = e[i].sysval == 1 ? 3 : e[i].sysval == 6 ? 1 : e[i].sysval == 8 ? 2 :
                      !strncasecmp(e[i].semantic, "SV_Target", 9) ? 16 : !strncasecmp(e[i].semantic, "SV_Depth", 8) ? 17 : 0;
            x->comp = e[i].component_type == 1 ? 5 : e[i].component_type == 2 ? 4 : 9;
            x->cols = (uint8_t)__builtin_popcount(e[i].mask);
            x->attribute = e[i].reg;
        }
        if (io) out->noutputs = count; else out->ninputs = count;
    }
    out->structured = 1;
}

static id<MTLFunction> stage_function_dual(dev12 *d, const BYTECODE *code, dxil_msl *out, const char *what,
                                           uint32_t sample_mask, int dual_source);
static id<MTLFunction> stage_function(dev12 *d, const BYTECODE *code, dxil_msl *out, const char *what,
                                      uint32_t sample_mask) {
    return stage_function_dual(d, code, out, what, sample_mask, 0);
}
static id<MTLFunction> stage_function_dual(dev12 *d, const BYTECODE *code, dxil_msl *out, const char *what,
                                           uint32_t sample_mask, int dual_source) {
    if (d3d12_is_dxbc_program(code)) {
        if (sample_mask != UINT32_MAX) { vx_log("D3D12: %s: a sample mask with a DXBC pixel shader is not supported yet", what); return nil; }
        if (dxbc_to_msl12(code, out)) { vx_log("D3D12: %s (DXBC) not translated: %s", what, out->error); return nil; }
    } else {
        dxil_module m;
        if (dxil_read(code->code, code->size, &m)) { vx_log("D3D12: %s: %s", what, m.error); return nil; }
        int rc = dxil_to_msl_pixel(&m, sample_mask, dual_source, out);
        dxil_free(&m);
        if (rc) { vx_log("D3D12: %s not translated: %s", what, out->error); return nil; }
        msl_override12(code, out);
    }
    if (out->uses_min_lod_clamp && !d3d12_metal_min_lod_clamp_supported(d->mtl)) {
        vx_log("D3D12: %s uses shader-level min-LOD sampling, which needs Metal Mac2/Apple6 support", what);
        dxil_msl_free(out);
        return nil;
    }
    MTLCompileOptions *opt = d3d12_shader_compile_options12(d, out);
    if (!opt) {
        dxil_msl_free(out);
        return nil;
    }
    NSError *err = nil;
    uint64_t hash = msl_dump12(out->msl);
    double started = msl_clock12();
    id<MTLLibrary> lib = library_with_timeout12(d->mtl, @(out->msl), opt, &err);
    msl_timed12(what, hash, started);
    if (!lib) { vx_log("D3D12: %s did not compile: %s\n%s", what, err.localizedDescription.UTF8String, out->msl); return nil; }
    dxil_msl_free(out);
    return [lib newFunctionWithName:@"vx_main"];
}

static id<MTLFunction> common_fragment_stage_function12(dev12 *device, const BYTECODE *code,
                                                        const dxil_msl *reflection,
                                                        gpu_ir_shader **shader_out) {
    if (shader_out) *shader_out = NULL;
    if (!device || !code || !code->code || !code->size || !shader_out || !reflection ||
        reflection->nslots || reflection->uses_heap || reflection->uses_sampler_heap) return nil;
    dxil_module module;
    if (dxil_read(code->code, code->size, &module)) return nil;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        return nil;
    }
    char *source = NULL;
    size_t source_size = 0;
    rc = gpu_ir_msl_emit_fragment(shader, &source, &source_size);
    if (rc != GPU_IR_OK || !source || !source_size) {
        free(source);
        gpu_ir_shader_release(shader);
        return nil;
    }
    NSString *msl = [[NSString alloc] initWithBytes:source length:source_size encoding:NSUTF8StringEncoding];
    free(source);
    MTLCompileOptions *options = d3d12_shader_compile_options12(device, reflection);
    NSError *error = nil;
    id<MTLLibrary> library = msl && options ? [device->mtl newLibraryWithSource:msl options:options error:&error] : nil;
    id<MTLFunction> function = library ? [library newFunctionWithName:@"vx_fragment"] : nil;
    if (!function) {
        if (error) vx_log("D3D12: common fragment MSL compilation failed: %s", error.localizedDescription.UTF8String);
        gpu_ir_shader_release(shader);
        return nil;
    }
    *shader_out = shader;
    return function;
}

static id<MTLFunction> common_vertex_stage_function12(dev12 *device, const BYTECODE *code,
                                                      const dxil_msl *reflection,
                                                      gpu_ir_shader **shader_out) {
    if (shader_out) *shader_out = NULL;
    if (!device || !code || !code->code || !code->size || !shader_out || !reflection ||
        reflection->nslots || reflection->uses_heap || reflection->uses_sampler_heap) return nil;
    dxil_module module;
    if (dxil_read(code->code, code->size, &module)) return nil;
    gpu_ir_shader *shader = NULL;
    int rc = dxil_build_gpu_ir_shader(&module, DXIL_IR_ENTRY_FUNCTION, &shader);
    dxil_free(&module);
    if (rc != GPU_IR_OK || !shader) {
        gpu_ir_shader_release(shader);
        return nil;
    }
    char *source = NULL;
    size_t source_size = 0;
    rc = gpu_ir_msl_emit_vertex(shader, &source, &source_size);
    if (rc != GPU_IR_OK || !source || !source_size) {
        free(source);
        gpu_ir_shader_release(shader);
        return nil;
    }
    NSString *msl = [[NSString alloc] initWithBytes:source length:source_size encoding:NSUTF8StringEncoding];
    free(source);
    MTLCompileOptions *options = d3d12_shader_compile_options12(device, reflection);
    NSError *error = nil;
    id<MTLLibrary> library = msl && options ? [device->mtl newLibraryWithSource:msl options:options error:&error] : nil;
    id<MTLFunction> function = library ? [library newFunctionWithName:@"vx_vertex"] : nil;
    if (!function) {
        if (error) vx_log("D3D12: common vertex MSL compilation failed: %s", error.localizedDescription.UTF8String);
        gpu_ir_shader_release(shader);
        return nil;
    }
    *shader_out = shader;
    return function;
}

static int common_vertex_inputs_bound12(const gpu_ir_shader *shader,
                                        const gpu_ir_vertex_input_desc *input_desc,
                                        const dxil_msl *reflection) {
    if (!shader || !input_desc || !reflection) return 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
        if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE ||
            variable->function_index != UINT32_MAX || !variable->entry_point_interface ||
            variable->storage_class != GPU_IR_SHADER_STORAGE_INPUT || variable->has_builtin ||
            !variable->has_location) continue;
        int found = 0;
        for (uint32_t j = 0; j < input_desc->attribute_count; j++)
            if (input_desc->attributes[j].location == variable->location) found = 1;
        if (!found) return 0;
        const gpu_ir_shader_type *pointer = gpu_ir_shader_find_type(shader, variable->result_type_id);
        const gpu_ir_shader_type *attribute = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
            gpu_ir_shader_find_type(shader, pointer->element_type_id) : NULL;
        uint32_t components = attribute && attribute->kind == GPU_IR_SHADER_TYPE_VECTOR ?
            attribute->element_count : attribute ? 1u : 0u;
        int reflected = 0;
        for (uint32_t j = 0; j < reflection->ninputs; j++) {
            const dxil_msl_element *input = &reflection->inputs[j];
            if (input->attribute != variable->location) continue;
            reflected = input->comp == 9 && input->cols == components;
            break;
        }
        if (!reflected) return 0;
    }
    return 1;
}

// The geometry path of a pipeline (pso12.gs_kernel): the vertex shader's kernel source and its inputs' layout, the
// geometry kernel, and the pass-through vertex function the render pipeline draws its triangles with.
static int gs_pipeline12(dev12 *d, pso12 *p, const GFX_PSO_DESC *desc, MTLRenderPipelineDescriptor *rp) {
    dxbc_shader *vs = calloc(1, sizeof *vs), *gs = calloc(1, sizeof *gs);
    int ok = 0;
    NSError *err = nil;
    id<MTLLibrary> lib = nil;
    id<MTLFunction> fn = nil;
    if (!vs || !gs) goto done;
    if (dxbc_translate_d3d12(desc->vs.code, desc->vs.size, vs) || !vs->msl_kernel) {
        vx_log("D3D12: vertex shader before a geometry shader: %s", vs->error[0] ? vs->error : "no kernel form");
        goto done;
    }
    if (dxbc_translate_d3d12(desc->gs.code, desc->gs.size, gs) || gs->type != DXBC_GEOMETRY || !gs->msl || !gs->msl_passthrough) {
        vx_log("D3D12: geometry shader: %s", gs->error[0] ? gs->error : "not a geometry shader");
        goto done;
    }
    dxbc_reflect12(gs, &p->gs);
    p->gs_in_verts = (uint32_t)gs->gs_in_verts;
    p->gs_max_tris = (uint32_t)gs->gs_max_tris;
    p->gs_out_regs = (uint32_t)gs->out_regs;
    p->gs_vs_out_regs = vs->out_regs ? (uint32_t)vs->out_regs : 1;
    p->gs_vs_source = @(vs->msl_kernel);
    p->gs_vs_kernels = [NSMutableDictionary new];
    lib = [d->mtl newLibraryWithSource:@(gs->msl) options:nil error:&err];
    fn = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
    p->gs_kernel = fn ? [d->mtl newComputePipelineStateWithFunction:fn error:&err] : nil;
    if (!p->gs_kernel) { vx_log("D3D12: geometry kernel: %s\n%s", err.localizedDescription.UTF8String, gs->msl); goto done; }
    lib = [d->mtl newLibraryWithSource:@(gs->msl_passthrough) options:nil error:&err];
    rp.vertexFunction = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
    rp.vertexDescriptor = nil;
    if (!rp.vertexFunction) { vx_log("D3D12: geometry pass-through: %s", err.localizedDescription.UTF8String); goto done; }
    // the input layout onto the vertex kernel's stage-input attributes (its input registers)
    uint32_t running[16] = { 0 };
    const INPUT_ELEMENT *el = desc->input.elements;
    for (uint32_t i = 0; i < desc->input.n; i++) {
        const INPUT_ELEMENT *e = &el[i];
        if (e->slot >= 16) continue;
        uint32_t off = e->offset == 0xFFFFFFFFu ? running[e->slot] : e->offset;
        running[e->slot] = off + d3d12_vertex_format_size12(e->format);
        for (int k = 0; k < vs->ninputs; k++) {
            const dxbc_element *x = &vs->inputs[k];
            if (x->sysval || strcasecmp(x->semantic, e->name) || x->index != e->index || p->gs_nattr >= 32) continue;
            if (e->slot >= 12) { vx_log("D3D12: vertex buffer slot %u before a geometry shader is not supported", e->slot); goto done; }
            p->gs_attr[p->gs_nattr++] = (__typeof__(p->gs_attr[0])){ x->reg, off, e->slot, e->cls == 1, e->step ? e->step : 1,
                                                                       vertex_format(e->format) };
        }
    }
    ok = 1;
done:
    if (vs) dxbc_free(vs);
    if (gs) dxbc_free(gs);
    free(vs); free(gs);
    return ok;
}

_Static_assert(DXIL_GS_COUNTS == DXBC_GS_COUNTS && DXIL_GS_INDEX == DXBC_GS_INDEX && DXIL_GS_PARAMS == DXBC_GS_PARAMS &&
               DXIL_VS_PARAMS == DXBC_VS_PARAMS && DXIL_GS_IN == DXBC_GS_IN && DXIL_GS_OUT == DXBC_GS_OUT,
               "the DXIL and DXBC geometry kernels share one draw path");

// The same for DXIL: the vertex shader as a kernel (dxil_to_msl_vertex_kernel), the geometry kernel reading its
// outputs by semantic and its pass-through vertex function (dxil_to_msl_geometry).
static int gs_pipeline_dxil12(dev12 *d, pso12 *p, const GFX_PSO_DESC *desc, MTLRenderPipelineDescriptor *rp) {
    dxil_module vm, gm;
    dxil_msl vk;
    int ok = 0, have_vm = 0, have_gm = 0, have_vk = 0;
    NSError *err = nil;
    id<MTLLibrary> lib = nil;
    id<MTLFunction> fn = nil;
    uint32_t running[16] = { 0 };
    const INPUT_ELEMENT *el = desc->input.elements;
    memset(&vk, 0, sizeof vk);
    if (dxil_read(desc->vs.code, desc->vs.size, &vm)) { vx_log("D3D12: vertex shader before a geometry shader: %s", vm.error); goto done; }
    have_vm = 1;
    if (dxil_read(desc->gs.code, desc->gs.size, &gm)) { vx_log("D3D12: geometry shader: %s", gm.error); goto done; }
    have_gm = 1;
    if (dxil_to_msl_vertex_kernel(&vm, &vk)) { vx_log("D3D12: vertex kernel before a geometry shader: %s", vk.error); goto done; }
    have_vk = 1;
    dxil_msl_free(&p->gs);
    if (dxil_to_msl_geometry(&gm, &vk, &p->gs)) { vx_log("D3D12: geometry shader: %s", p->gs.error); goto done; }
    p->gs_in_verts = p->gs.gs_in_verts;
    p->gs_max_tris = p->gs.gs_max_tris;
    p->gs_out_regs = p->gs.io_slots;
    p->gs_vs_out_regs = vk.io_slots ? vk.io_slots : 1;
    p->gs_vs_source = @(vk.msl);
    p->gs_vs_kernels = [NSMutableDictionary new];
    lib = [d->mtl newLibraryWithSource:@(p->gs.msl) options:nil error:&err];
    fn = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
    p->gs_kernel = fn ? [d->mtl newComputePipelineStateWithFunction:fn error:&err] : nil;
    if (!p->gs_kernel) { vx_log("D3D12: geometry kernel: %s\n%s", err.localizedDescription.UTF8String, p->gs.msl); goto done; }
    lib = [d->mtl newLibraryWithSource:@(p->gs.msl_passthrough) options:nil error:&err];
    rp.vertexFunction = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
    rp.vertexDescriptor = nil;
    if (!rp.vertexFunction) { vx_log("D3D12: geometry pass-through: %s", err.localizedDescription.UTF8String); goto done; }
    // the input layout onto the vertex kernel's stage-input attributes (by semantic, as the vertex function's)
    for (uint32_t i = 0; i < desc->input.n; i++) {
        const INPUT_ELEMENT *e = &el[i];
        if (e->slot >= 16) continue;
        uint32_t off = e->offset == 0xFFFFFFFFu ? running[e->slot] : e->offset;
        running[e->slot] = off + d3d12_vertex_format_size12(e->format);
        for (uint32_t k = 0; k < vk.ninputs; k++) {
            const dxil_msl_element *x = &vk.inputs[k];
            if (strcasecmp(x->name, e->name) || x->index != e->index || p->gs_nattr >= 32) continue;
            if (e->slot >= 12) { vx_log("D3D12: vertex buffer slot %u before a geometry shader is not supported", e->slot); goto done; }
            p->gs_attr[p->gs_nattr++] = (__typeof__(p->gs_attr[0])){ x->attribute, off, e->slot, e->cls == 1, e->step ? e->step : 1,
                                                                       vertex_format(e->format) };
        }
    }
    ok = 1;
done:
    free(p->gs.msl); p->gs.msl = NULL;                             // (compiled; the slots stay for binding)
    free(p->gs.msl_passthrough); p->gs.msl_passthrough = NULL;
    if (have_vk) dxil_msl_free(&vk);
    if (have_vm) dxil_free(&vm);
    if (have_gm) dxil_free(&gm);
    return ok;
}

// Tessellation (DXIL): the vertex kernel, the hull kernel and the domain shader as the render pipeline's
// post-tessellation vertex function, with the hull shader's partitioning, winding and maximum factor.
static int tess_pipeline_dxil12(dev12 *d, pso12 *p, const GFX_PSO_DESC *desc, MTLRenderPipelineDescriptor *rp) {
    dxil_module vm, hm, dm;
    dxil_msl vk;
    int ok = 0, have_vm = 0, have_hm = 0, have_dm = 0, have_vk = 0;
    NSError *err = nil;
    id<MTLLibrary> lib = nil;
    id<MTLFunction> fn = nil;
    uint32_t running[16] = { 0 };
    const INPUT_ELEMENT *el = desc->input.elements;
    memset(&vk, 0, sizeof vk);
    if (dxil_read(desc->vs.code, desc->vs.size, &vm)) { vx_log("D3D12: vertex shader before a hull shader: %s", vm.error); goto done; }
    have_vm = 1;
    if (dxil_read(desc->hs.code, desc->hs.size, &hm)) { vx_log("D3D12: hull shader: %s", hm.error); goto done; }
    have_hm = 1;
    if (dxil_read(desc->ds.code, desc->ds.size, &dm)) { vx_log("D3D12: domain shader: %s", dm.error); goto done; }
    have_dm = 1;
    if (dxil_to_msl_vertex_kernel(&vm, &vk)) { vx_log("D3D12: vertex kernel before a hull shader: %s", vk.error); goto done; }
    have_vk = 1;
    dxil_msl_free(&p->hs);
    dxil_msl_free(&p->ds);
    if (dxil_to_msl_hull(&hm, &vk, &p->hs)) { vx_log("D3D12: hull shader: %s", p->hs.error); goto done; }
    if (dxil_to_msl_domain(&dm, &p->hs, &p->ds)) { vx_log("D3D12: domain shader: %s", p->ds.error); goto done; }
    p->tess_in_cp = p->hs.tess_in_cp;
    p->tess_out_cp = p->hs.tess_out_cp;
    p->hs_cp_slots = p->hs.io_slots;
    p->hs_pc_slots = p->hs.npatch ? p->hs.npatch : 1;
    p->tess_domain = p->hs.tess_domain;
    p->gs_vs_out_regs = vk.io_slots ? vk.io_slots : 1;
    p->gs_vs_source = @(vk.msl);
    p->gs_vs_kernels = [NSMutableDictionary new];
    lib = [d->mtl newLibraryWithSource:@(p->hs.msl) options:nil error:&err];
    fn = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
    p->hs_kernel = fn ? [d->mtl newComputePipelineStateWithFunction:fn error:&err] : nil;
    if (!p->hs_kernel) { vx_log("D3D12: hull kernel: %s\n%s", err.localizedDescription.UTF8String, p->hs.msl); goto done; }
    lib = [d->mtl newLibraryWithSource:@(p->ds.msl) options:nil error:&err];
    rp.vertexFunction = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
    rp.vertexDescriptor = nil;
    if (!rp.vertexFunction) { vx_log("D3D12: domain shader: %s\n%s", err.localizedDescription.UTF8String, p->ds.msl); goto done; }
    static const MTLTessellationPartitionMode partition[5] = { MTLTessellationPartitionModeInteger, MTLTessellationPartitionModeInteger,
        MTLTessellationPartitionModePow2, MTLTessellationPartitionModeFractionalOdd, MTLTessellationPartitionModeFractionalEven };
    rp.tessellationPartitionMode = partition[p->hs.tess_partitioning <= 4 ? p->hs.tess_partitioning : 0];
    rp.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
    rp.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
    rp.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
    rp.tessellationOutputWindingOrder = p->hs.tess_output == 4 ? MTLWindingCounterClockwise : MTLWindingClockwise;
    // (Metal's maximum is 64 on Apple GPUs, and even for fractional-even partitioning)
    uint32_t max_factor = (uint32_t)ceilf(p->hs.tess_max_factor);
    if (max_factor > 64) max_factor = 64;
    if (max_factor < 2) max_factor = 2;
    if (max_factor & 1) max_factor++;
    rp.maxTessellationFactor = max_factor;
    for (uint32_t i = 0; i < desc->input.n; i++) {
        const INPUT_ELEMENT *e = &el[i];
        if (e->slot >= 16) continue;
        uint32_t off = e->offset == 0xFFFFFFFFu ? running[e->slot] : e->offset;
        running[e->slot] = off + d3d12_vertex_format_size12(e->format);
        for (uint32_t k = 0; k < vk.ninputs; k++) {
            const dxil_msl_element *x = &vk.inputs[k];
            if (strcasecmp(x->name, e->name) || x->index != e->index || p->gs_nattr >= 32) continue;
            if (e->slot >= 12) { vx_log("D3D12: vertex buffer slot %u before a hull shader is not supported", e->slot); goto done; }
            p->gs_attr[p->gs_nattr++] = (__typeof__(p->gs_attr[0])){ x->attribute, off, e->slot, e->cls == 1, e->step ? e->step : 1,
                                                                       vertex_format(e->format) };
        }
    }
    ok = 1;
done:
    free(p->hs.msl); p->hs.msl = NULL;                             // (compiled; the slots stay for binding)
    free(p->ds.msl); p->ds.msl = NULL;
    if (have_vk) dxil_msl_free(&vk);
    if (have_vm) dxil_free(&vm);
    if (have_hm) dxil_free(&hm);
    if (have_dm) dxil_free(&dm);
    return ok;
}

static HRESULT create_graphics_pso12(void *this, const GFX_PSO_DESC *desc, const GUID *iid, void **out) {
    dev12 *d = this;
    if ((desc->hs.size != 0) != (desc->ds.size != 0)) return E_INVALIDARG;
    if (desc->hs.size && (d3d12_is_dxbc_program(&desc->hs) || d3d12_is_dxbc_program(&desc->ds) || d3d12_is_dxbc_program(&desc->vs))) {
        vx_log("D3D12: hull and domain shaders are done for DXIL only so far");
        return E_NOTIMPL;
    }
    if (desc->hs.size && desc->gs.size) { vx_log("D3D12: a geometry shader after tessellation is not done yet"); return E_NOTIMPL; }
    if (desc->gs.size && d3d12_is_dxbc_program(&desc->gs) != d3d12_is_dxbc_program(&desc->vs)) {
        vx_log("D3D12: a geometry shader and a vertex shader of different shader models (DXBC and DXIL)");
        return E_NOTIMPL;
    }
    if (desc->gs.size && desc->so.n) { vx_log("D3D12: stream output is not done yet"); return E_NOTIMPL; }
    if (desc->nrt > 8) return E_INVALIDARG;
    if (desc->raster.conservative < 0 || desc->raster.conservative > 1) return E_INVALIDARG;
    if (desc->raster.forced_samples != 0 && desc->raster.forced_samples != 1 &&
        desc->raster.forced_samples != 4 && desc->raster.forced_samples != 8 &&
        desc->raster.forced_samples != 16) return E_INVALIDARG;
    if (desc->raster.conservative || desc->raster.forced_samples) {
        vx_log("D3D12: conservative rasterization and forced sample count are not supported by the Metal raster path");
        return E_NOTIMPL;
    }
    // Output-merger logic ops (required from feature level 11_1 on, so engines use them): Metal has none. NOOP keeps
    // the target (writes masked off); the others draw as COPY, which is what most uses of them amount to.
    GFX_PSO_DESC adjusted;
    for (uint32_t i = 0; i < desc->nrt; i++) {
        const RT_BLEND *b = &desc->blend.rt[desc->blend.independent ? i : 0];
        if (b->logic_enable && (b->enable || desc->blend.independent)) return E_INVALIDARG;
        if (b->logic_enable) {
            static int told;
            if (!told++) vx_log("D3D12: render-target logic operation %d drawn as %s (Metal has no logic operations)",
                                b->logic_op, b->logic_op == 4 ? "no write" : "a copy");
            if (desc != &adjusted) { adjusted = *desc; desc = &adjusted; }
            uint32_t k = desc->blend.independent ? i : 0;
            adjusted.blend.rt[k].logic_enable = 0;
            if (b->logic_op == 4) adjusted.blend.rt[k].mask = 0;   // D3D12_LOGIC_OP_NOOP
        }
    }
    if (desc->sample_mask != UINT32_MAX && !desc->ps.size) {
        vx_log("D3D12: a non-default sample mask requires a pixel shader for Metal coverage lowering");
        return E_NOTIMPL;
    }
    pso12 *p = alloc12(sizeof *p, K12_PSO, g_pso_vtbl, d);
    p->base.destroy = pso_destroy;
    p->vs_code = code_dump12(desc->vs.code, desc->vs.size);
    p->ps_code = code_dump12(desc->ps.code, desc->ps.size);
    MTLRenderPipelineDescriptor *rp = [MTLRenderPipelineDescriptor new];
    rp.vertexFunction = stage_function(d, &desc->vs, &p->vs, "vertex shader", UINT32_MAX);
    if (desc->ps.size) {
        // (D3D12_BLEND_SRC1_COLOR ... INV_SRC1_ALPHA: the shader's second output blends into target 0; Metal allows
        // that with one render target only)
        const RT_BLEND *b0 = &desc->blend.rt[0];
        int dual = desc->nrt <= 1 && b0->enable && ((b0->src >= 16 && b0->src <= 19) || (b0->dst >= 16 && b0->dst <= 19) ||
                                  (b0->src_a >= 16 && b0->src_a <= 19) || (b0->dst_a >= 16 && b0->dst_a <= 19));
        rp.fragmentFunction = stage_function_dual(d, &desc->ps, &p->ps, "pixel shader", desc->sample_mask, dual);
        if (!rp.fragmentFunction) { d3d_release(&p->base); return E_INVALIDARG; }
        if (desc->sample_mask == UINT32_MAX && desc->nrt == 1 && !no_common_ir12(2)) {
            gpu_ir_shader *common_shader = NULL;
            id<MTLFunction> common_function = common_fragment_stage_function12(d, &desc->ps, &p->ps,
                                                                               &common_shader);
            if (common_function) {
                rp.fragmentFunction = common_function;
                p->common_ir_fragment = common_shader;
            }
        }
    }
    if (!shader_root_signature_valid12(d, &p->vs, desc->root) ||
        !shader_root_signature_valid12(d, &p->ps, desc->root)) {
        vx_log("D3D12: graphics shader bindings are not covered by a compatible root signature");
        d3d_release(&p->base);
        return E_INVALIDARG;
    }
    // the input layout, onto the vertex shader's [[attribute(n)]] by semantic (vertex buffers at Metal buffers 16 + slot)
    MTLVertexDescriptor *vd = [MTLVertexDescriptor new];
    uint32_t running[16] = { 0 };
    gpu_ir_vertex_input_desc common_vertex_input = { 0 };
    const INPUT_ELEMENT *el = desc->input.elements;
    for (uint32_t i = 0; i < desc->input.n; i++) {
        const INPUT_ELEMENT *e = &el[i];
        if (e->slot >= 16 || e->cls < 0 || e->cls > 1 || (e->cls == 0 && e->step != 0)) {
            vx_log("D3D12: invalid input-assembler slot, classification, or per-vertex step rate");
            d3d_release(&p->base);
            return E_INVALIDARG;
        }
        uint32_t slot = e->slot, bpp = d3d12_vertex_format_size12(e->format);
        if (!bpp || vertex_format(e->format) == MTLVertexFormatInvalid) {
            vx_log("D3D12: unsupported vertex input format %u", e->format);
            d3d_release(&p->base);
            return E_NOTIMPL;
        }
        uint32_t off = e->offset == 0xFFFFFFFFu ? running[slot] : e->offset;
        if (off > UINT32_MAX - bpp) {
            d3d_release(&p->base);
            return E_INVALIDARG;
        }
        running[slot] = off + (bpp ? bpp : 4);
        for (uint32_t k = 0; k < p->vs.ninputs; k++) {
            const dxil_msl_element *x = &p->vs.inputs[k];
            if (strcasecmp(x->name, e->name) || x->index != e->index) continue;
            MTLVertexFormat vf = vertex_format(e->format);
            if (vf == MTLVertexFormatInvalid) vx_log("D3D12: vertex format %u is not supported", e->format);
            vd.attributes[x->attribute].format = vf;
            vd.attributes[x->attribute].offset = off;
            vd.attributes[x->attribute].bufferIndex = 16 + slot;
            vd.layouts[16 + slot].stride = MTLBufferLayoutStrideDynamic;
            vd.layouts[16 + slot].stepFunction = e->cls == 1 ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            vd.layouts[16 + slot].stepRate = e->cls == 1 ? (e->step ? e->step : 1) : 1;

            uint32_t binding_index = 0;
            while (binding_index < common_vertex_input.binding_count &&
                   common_vertex_input.bindings[binding_index].binding != slot) binding_index++;
            uint32_t input_rate = e->cls == 1 ? GPU_IR_VERTEX_INPUT_RATE_INSTANCE : GPU_IR_VERTEX_INPUT_RATE_VERTEX;
            uint32_t step_rate = e->cls == 1 ? (e->step ? e->step : 1) : 1;
            if (binding_index == common_vertex_input.binding_count) {
                if (binding_index >= GPU_IR_MAX_VERTEX_BUFFERS) {
                    d3d_release(&p->base);
                    return E_NOTIMPL;
                }
                common_vertex_input.bindings[common_vertex_input.binding_count++] = (gpu_ir_vertex_binding_desc){
                    .binding = slot, .input_rate = input_rate, .step_rate = step_rate
                };
            } else if (common_vertex_input.bindings[binding_index].input_rate != input_rate ||
                       common_vertex_input.bindings[binding_index].step_rate != step_rate) {
                vx_log("D3D12: one input slot uses conflicting per-vertex/per-instance classifications");
                d3d_release(&p->base);
                return E_INVALIDARG;
            }
            if (common_vertex_input.attribute_count >= GPU_IR_MAX_VERTEX_ATTRIBUTES) {
                d3d_release(&p->base);
                return E_NOTIMPL;
            }
            common_vertex_input.attributes[common_vertex_input.attribute_count++] = (gpu_ir_vertex_attribute_desc){
                .location = x->attribute, .binding = slot, .format = GPU_IR_VERTEX_FORMAT_OPAQUE,
                .offset = off, .byte_size = bpp
            };
        }
    }
    gpu_ir_shader *common_vertex_shader = NULL;
    id<MTLFunction> common_vertex_function = no_common_ir12(4) ? nil : common_vertex_stage_function12(d, &desc->vs, &p->vs,
                                                                            &common_vertex_shader);
    if (common_vertex_function && common_vertex_inputs_bound12(common_vertex_shader, &common_vertex_input, &p->vs)) {
        rp.vertexFunction = common_vertex_function;
        p->common_ir_vertex = common_vertex_shader;
    } else {
        gpu_ir_shader_release(common_vertex_shader);
    }
    if (!rp.vertexFunction) {
        vx_log("D3D12: vertex shader is unsupported by both direct DXIL-to-MSL and the common vertex path");
        d3d_release(&p->base);
        return E_INVALIDARG;
    }
    if (desc->gs.size) {
        int gs_ok = d3d12_is_dxbc_program(&desc->gs) ? gs_pipeline12(d, p, desc, rp) : gs_pipeline_dxil12(d, p, desc, rp);
        if (!gs_ok || !shader_root_signature_valid12(d, &p->gs, desc->root)) {
            d3d_release(&p->base);
            return E_INVALIDARG;
        }
    } else if (desc->hs.size) {
        if (!tess_pipeline_dxil12(d, p, desc, rp) || !shader_root_signature_valid12(d, &p->hs, desc->root) ||
            !shader_root_signature_valid12(d, &p->ds, desc->root)) {
            d3d_release(&p->base);
            return E_INVALIDARG;
        }
    } else rp.vertexDescriptor = vd;
    for (uint32_t i = 0; i < desc->nrt && i < 8; i++) {
        uint32_t bpp;
        MTLPixelFormat f = d3d_mtl_format(desc->rtv_formats[i], &bpp);
        rp.colorAttachments[i].pixelFormat = f;
        const RT_BLEND *b = &desc->blend.rt[desc->blend.independent ? i : 0];
        rp.colorAttachments[i].blendingEnabled = b->enable != 0;
        // Second-source factors work for target 0 of a single-target pipeline only (in D3D12 too); elsewhere they
        // are read as the first source's, rather than Metal refusing the pipeline.
        int single = desc->nrt <= 1 && i == 0;
#define SRC1_AS12(f) (!single && (f) >= 16 && (f) <= 19 ? (f) - 13 : (f))   // SRC1_COLOR 16 -> SRC_COLOR 3 ...
        rp.colorAttachments[i].sourceRGBBlendFactor = blend_factor(SRC1_AS12(b->src));
        rp.colorAttachments[i].destinationRGBBlendFactor = blend_factor(SRC1_AS12(b->dst));
        rp.colorAttachments[i].rgbBlendOperation = blend_op(b->op);
        rp.colorAttachments[i].sourceAlphaBlendFactor = blend_factor(SRC1_AS12(b->src_a));
        rp.colorAttachments[i].destinationAlphaBlendFactor = blend_factor(SRC1_AS12(b->dst_a));
#undef SRC1_AS12
        rp.colorAttachments[i].alphaBlendOperation = blend_op(b->op_a);
        MTLColorWriteMask m = 0;
        if (b->mask & 1) m |= MTLColorWriteMaskRed;
        if (b->mask & 2) m |= MTLColorWriteMaskGreen;
        if (b->mask & 4) m |= MTLColorWriteMaskBlue;
        if (b->mask & 8) m |= MTLColorWriteMaskAlpha;
        rp.colorAttachments[i].writeMask = m;
    }
    if (desc->dsv_format) {
        uint32_t f = desc->dsv_format;
        MTLPixelFormat df = f == 55 ? MTLPixelFormatDepth16Unorm : f == 40 ? MTLPixelFormatDepth32Float : MTLPixelFormatDepth32Float_Stencil8;
        rp.depthAttachmentPixelFormat = df;
        p->has_depth = 1;
        if (df == MTLPixelFormatDepth32Float_Stencil8) { rp.stencilAttachmentPixelFormat = df; p->has_stencil = 1; }
    }
    p->depth_enable = p->has_depth && desc->depth.depth_enable;
    p->depth_write = p->depth_enable && desc->depth.depth_write;
    p->stencil_enable = p->has_stencil && desc->depth.stencil_enable;
    p->stencil_write = p->stencil_enable && desc->depth.write_mask &&
        (desc->depth.front.fail != 1 || desc->depth.front.depth_fail != 1 || desc->depth.front.pass != 1 ||
         desc->depth.back.fail != 1 || desc->depth.back.depth_fail != 1 || desc->depth.back.pass != 1);
    rp.rasterSampleCount = desc->samples ? desc->samples : 1;
    rp.alphaToCoverageEnabled = desc->blend.a2c != 0;
    // D3D12_PRIMITIVE_TOPOLOGY_TYPE: 1 point, 2 line, 3 triangle
    rp.inputPrimitiveTopology = desc->topology_type == 1 ? MTLPrimitiveTopologyClassPoint : desc->topology_type == 2 ? MTLPrimitiveTopologyClassLine : MTLPrimitiveTopologyClassTriangle;
    if (desc->gs.size) rp.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;   // (the geometry kernel's triangles)
    NSError *err = nil;
    // (synchronously: the asynchronous form stops the process over a descriptor Metal finds invalid)
    p->render = [d->mtl newRenderPipelineStateWithDescriptor:rp error:&err];
    if (!p->render) { vx_log("D3D12: graphics pipeline: %s", err.localizedDescription.UTF8String); d3d_release(&p->base); return E_INVALIDARG; }
    MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
    if (desc->depth.depth_enable && p->has_depth) {
        ds.depthCompareFunction = compare(desc->depth.depth_func);
        ds.depthWriteEnabled = desc->depth.depth_write != 0;
    }
    if (desc->depth.stencil_enable && p->has_stencil) {
        ds.frontFaceStencil = stencil(&desc->depth.front, desc->depth.read_mask, desc->depth.write_mask);
        ds.backFaceStencil = stencil(&desc->depth.back, desc->depth.read_mask, desc->depth.write_mask);
    }
    p->depth = [d->mtl newDepthStencilStateWithDescriptor:ds];
    // D3D12_CULL_MODE: 1 none, 2 front, 3 back; D3D12_FILL_MODE: 2 wireframe, 3 solid
    p->cull = desc->raster.cull == 2 ? MTLCullModeFront : desc->raster.cull == 3 ? MTLCullModeBack : MTLCullModeNone;
    p->winding = desc->raster.front_ccw ? MTLWindingCounterClockwise : MTLWindingClockwise;
    p->fill = desc->raster.fill == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
    p->clip = desc->raster.depth_clip ? MTLDepthClipModeClip : MTLDepthClipModeClamp;
    p->bias = (float)desc->raster.depth_bias;
    p->bias_slope = desc->raster.slope;
    p->bias_clamp = desc->raster.bias_clamp;
    rootsig12 *root = desc->root;
    gpu_ir_pipeline_desc ir_desc = {
        .info = { .kind = GPU_IR_PIPELINE_GRAPHICS,
                  .stage_mask = GPU_IR_STAGE_VERTEX | (desc->ps.size ? GPU_IR_STAGE_FRAGMENT : 0),
                  .vertex_input = common_vertex_input,
                  .color_blend = color_blend_ir12(desc) },
        .layout = root && root->base.kind == K12_ROOTSIG && root->dev == d ? root->gpu_layout : NULL,
        .backend_object = (__bridge_retained void *)p->render,
        .release = gpu12_release_mtl_object,
        .stage_shaders = {
            [GPU_IR_STAGE_SLOT_VERTEX] = p->common_ir_vertex,
            [GPU_IR_STAGE_SLOT_FRAGMENT] = p->common_ir_fragment
        }
    };
    p->gpu_pipeline = gpu_ir_pipeline_create(&ir_desc);
    if (!p->gpu_pipeline) {
        gpu12_release_mtl_object(ir_desc.backend_object);
        vx_log("D3D12: graphics pipeline could not be represented by the common GPU IR");
        d3d_release(&p->base);
        return E_OUTOFMEMORY;
    }
    HRESULT hr = pso_query(p, iid, out);
    d3d_release(&p->base);
    return hr;
}

// ID3D12Device2::CreatePipelineState: a stream of subobjects, each a 4-byte type and its structure, aligned to
// pointer size (the layout of d3dx12's CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT). The stream becomes the classic
// graphics or compute description, with D3D12's defaults for the subobjects it leaves out.
typedef struct { size_t size; const void *stream; } PSO_STREAM_DESC;
static HRESULT pso_stream_parse(const PSO_STREAM_DESC *desc, GFX_PSO_DESC *g, COMPUTE_PSO_DESC *c, int *compute) {
    // the inner structure's size and alignment by subobject type (0: not accepted here)
    static const struct { uint8_t size, align; } inner[0x1a] = {
        [0x00] = { 8, 8 },  [0x01] = { 16, 8 }, [0x02] = { 16, 8 }, [0x03] = { 16, 8 }, [0x04] = { 16, 8 },
        [0x05] = { 16, 8 }, [0x06] = { 16, 8 }, [0x07] = { 32, 8 }, [0x08] = { 255, 4 }, [0x09] = { 4, 4 },
        [0x0a] = { 44, 4 }, [0x0b] = { 52, 4 }, [0x0c] = { 16, 8 }, [0x0d] = { 4, 4 },  [0x0e] = { 4, 4 },
        [0x0f] = { 36, 4 }, [0x10] = { 4, 4 },  [0x11] = { 8, 4 },  [0x12] = { 4, 4 },  [0x13] = { 16, 8 },
        [0x14] = { 4, 4 },  [0x15] = { 56, 4 }, [0x16] = { 24, 8 } };
    if (!desc || !desc->stream || !desc->size) return E_INVALIDARG;
    memset(g, 0, sizeof *g);
    memset(c, 0, sizeof *c);
    g->blend.rt[0] = (RT_BLEND){ 0, 0, 2, 1, 1, 2, 1, 1, 4, 0xf, { 0 } };   // ONE, ZERO, ADD; NOOP; all channels
    for (int i = 1; i < 8; i++) g->blend.rt[i] = g->blend.rt[0];
    g->sample_mask = UINT32_MAX;
    g->raster = (RASTER_DESC){ 3, 3, 0, 0, 0, 0, 1, 0, 0, 0, 0 };   // solid, cull back, depth clip
    g->depth = (DS_DESC){ 1, 1, 2, 0, 0xff, 0xff, { 0, 0 }, { 1, 1, 1, 8 }, { 1, 1, 1, 8 } };   // LESS, write all
    g->samples = 1;
    const uint8_t *p = desc->stream, *end = p + desc->size;
    uint32_t seen = 0;
    while (p < end) {
        if (end - p < 4) return E_INVALIDARG;
        uint32_t type = *(const uint32_t *)p;
        if (type >= 0x1a || !inner[type].size) {
            vx_log("D3D12: pipeline state stream subobject %u is not supported", type);
            return E_INVALIDARG;
        }
        if (seen & 1u << type) return E_INVALIDARG;                // each subobject at most once
        seen |= 1u << type;
        size_t isize = type == 0x08 ? sizeof(BLEND_DESC) : inner[type].size;
        const uint8_t *in = p + (inner[type].align == 8 ? 8 : 4);
        if ((size_t)(end - in) < isize) return E_INVALIDARG;
        switch (type) {
        case 0x00: g->root = c->root = *(void *const *)in; break;
        case 0x01: g->vs = *(const BYTECODE *)in; break;
        case 0x02: g->ps = *(const BYTECODE *)in; break;
        case 0x03: g->ds = *(const BYTECODE *)in; break;
        case 0x04: g->hs = *(const BYTECODE *)in; break;
        case 0x05: g->gs = *(const BYTECODE *)in; break;
        case 0x06: c->cs = *(const BYTECODE *)in; break;
        case 0x07: g->so = *(const SO_DESC *)in; break;
        case 0x08: g->blend = *(const BLEND_DESC *)in; break;
        case 0x09: g->sample_mask = *(const uint32_t *)in; break;
        case 0x0a: g->raster = *(const RASTER_DESC *)in; break;
        case 0x0b: g->depth = *(const DS_DESC *)in; break;
        case 0x0c: g->input = *(const IL_DESC *)in; break;
        case 0x0d: g->strip_cut = *(const int32_t *)in; break;
        case 0x0e: g->topology_type = *(const int32_t *)in; break;
        case 0x0f: memcpy(g->rtv_formats, in, 32); g->nrt = *(const uint32_t *)(in + 32); break;
        case 0x10: g->dsv_format = *(const uint32_t *)in; break;
        case 0x11: g->samples = ((const uint32_t *)in)[0]; g->quality = ((const uint32_t *)in)[1]; break;
        case 0x12: g->node = c->node = *(const uint32_t *)in; break;
        case 0x13:
            g->cached = c->cached = *(const void *const *)in;
            g->cached_size = c->cached_size = *(const size_t *)(in + 8);
            break;
        case 0x14: g->flags = c->flags = *(const int32_t *)in; break;
        case 0x15:                                                 // DEPTH_STENCIL1: DEPTH_STENCIL and depth bounds
            if (seen & 1u << 0x0b) return E_INVALIDARG;
            memcpy(&g->depth, in, sizeof g->depth);
            if (*(const int32_t *)(in + 52)) {
                vx_log("D3D12: depth bounds tests are not supported");
                return E_INVALIDARG;
            }
            break;
        case 0x16:                                                 // VIEW_INSTANCING (tier 0: one view only)
            if (*(const uint32_t *)in > 1) { vx_log("D3D12: view instancing is not supported"); return E_INVALIDARG; }
            break;
        }
        if (type == 0x0b && seen & 1u << 0x15) return E_INVALIDARG;
        size_t next = (size_t)(in - p) + isize;
        p += (next + 7) & ~(size_t)7;
    }
    int graphics = (seen & 0x3e) != 0, is_compute = (seen & 1u << 0x06) != 0;
    if (graphics == is_compute) return E_INVALIDARG;               // one of VS..GS or CS, not both or neither
    if (is_compute && (seen & ~((1u << 0x00) | (1u << 0x06) | (1u << 0x12) | (1u << 0x13) | (1u << 0x14))))
        return E_INVALIDARG;                                       // graphics state in a compute stream
    *compute = is_compute;
    return S_OK;
}

// ---- pipeline descriptions by content, cached blobs and pipeline libraries ----------------------------------------
// A pipeline library (ID3D12PipelineLibrary1) holds the pipelines a program stored by name; serialized, it keeps each
// name with a hash of the description the pipeline was made from, and a library made from that blob in a later run
// makes a pipeline again when it is loaded by its name and an equal description (Metal keeps its own cache of the
// compiled shaders, so that is quick). A loaded description that differs is E_INVALIDARG, as on Windows.

static uint64_t root_hash12(void *root) {
    rootsig12 *r = root;
    return r && r->base.kind == K12_ROOTSIG ? r->blob_hash : 0;
}
static uint64_t compute_desc_hash12(const COMPUTE_PSO_DESC *d) {
    uint64_t h = fnv1a12(0xcbf29ce484222325ull, "compute", 7), r = root_hash12(d->root);
    h = fnv1a12(h, &r, sizeof r);
    h = fnv1a12(h, d->cs.code, d->cs.size);
    return fnv1a12(h, &d->flags, sizeof d->flags);
}
static uint64_t graphics_desc_hash12(const GFX_PSO_DESC *d) {
    uint64_t h = fnv1a12(0xcbf29ce484222325ull, "graphics", 8), r = root_hash12(d->root);
    h = fnv1a12(h, &r, sizeof r);
    const BYTECODE *code[5] = { &d->vs, &d->ps, &d->ds, &d->hs, &d->gs };
    for (int i = 0; i < 5; i++) { h = fnv1a12(h, &code[i]->size, sizeof code[i]->size); h = fnv1a12(h, code[i]->code, code[i]->size); }
    h = fnv1a12(h, &d->blend, sizeof d->blend);
    h = fnv1a12(h, &d->sample_mask, sizeof d->sample_mask);
    h = fnv1a12(h, &d->raster, sizeof d->raster);
    h = fnv1a12(h, &d->depth, sizeof d->depth);
    const INPUT_ELEMENT *e = d->input.elements;
    for (uint32_t i = 0; e && i < d->input.n; i++) {
        if (e[i].name) h = fnv1a12(h, e[i].name, strlen(e[i].name));
        h = fnv1a12(h, &e[i].index, sizeof e[i] - offsetof(INPUT_ELEMENT, index));
    }
    h = fnv1a12(h, &d->so.n, sizeof d->so.n);
    h = fnv1a12(h, &d->strip_cut, offsetof(GFX_PSO_DESC, cached) - offsetof(GFX_PSO_DESC, strip_cut));
    return fnv1a12(h, &d->flags, sizeof d->flags);
}

// (CachedPSO in a description is accepted and not needed: the pipeline is made from the description.)
static WINAPI HRESULT dev_create_compute_pso(void *this, const COMPUTE_PSO_DESC *desc, const GUID *iid, void **out) {
    if (!desc) return E_INVALIDARG;
    HRESULT hr = create_compute_pso12(this, desc, iid, out);
    if (hr >= 0 && out && *out) ((pso12 *)*out)->desc_hash = compute_desc_hash12(desc);
    return hr;
}
static WINAPI HRESULT dev_create_graphics_pso(void *this, const GFX_PSO_DESC *desc, const GUID *iid, void **out) {
    if (!desc) return E_INVALIDARG;
    HRESULT hr = create_graphics_pso12(this, desc, iid, out);
    if (hr >= 0 && out && *out) ((pso12 *)*out)->desc_hash = graphics_desc_hash12(desc);
    return hr;
}

static const char pso_blob_magic12[8] = { 'V', 'X', 'R', 'T', 'P', 'S', 'O', '1' };
static WINAPI HRESULT pso_cached_blob(void *this, void **out) {
    pso12 *p = this;
    if (!out) return E_INVALIDARG;
    uint8_t data[16];
    memcpy(data, pso_blob_magic12, 8);
    memcpy(data + 8, &p->desc_hash, 8);
    *out = blob_new(data, sizeof data);
    return *out ? S_OK : E_OUTOFMEMORY;
}

typedef struct {
    uint16_t *name;                  // UTF-16, NUL-terminated
    uint32_t name_len;
    uint64_t hash;
    int compute;
    pso12 *pso;                      // NULL for an entry read from a blob and not loaded yet
} plib_entry12;
typedef struct {
    d3d_object base; dev12 *dev;
    pthread_mutex_t lock;
    plib_entry12 *entries;
    uint32_t n, cap;
} plib12;

#define D3D12_ERROR_ADAPTER_NOT_FOUND12 ((HRESULT)0x887E0001)
#define D3D12_ERROR_DRIVER_VERSION_MISMATCH12 ((HRESULT)0x887E0002)
static const char plib_magic12[8] = { 'V', 'X', 'R', 'T', 'P', 'L', 'I', 'B' };
enum { PLIB_VERSION12 = 1 };

static uint64_t plib_adapter12(dev12 *d) {             // (the blob is for this runtime on this GPU)
    uint64_t id = d->mtl.registryID;
    const char *name = d->mtl.name.UTF8String;
    return fnv1a12(fnv1a12(0xcbf29ce484222325ull, &id, sizeof id), name, name ? strlen(name) : 0);
}
static uint32_t wide_len12(const uint16_t *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static plib_entry12 *plib_find12(plib12 *l, const uint16_t *name) {
    uint32_t len = wide_len12(name);
    for (uint32_t i = 0; i < l->n; i++)
        if (l->entries[i].name_len == len && !memcmp(l->entries[i].name, name, len * 2)) return &l->entries[i];
    return NULL;
}
static plib_entry12 *plib_add12(plib12 *l, const uint16_t *name, uint32_t len, uint64_t hash, int compute) {
    if (l->n == l->cap) {
        uint32_t cap = l->cap ? l->cap * 2 : 64;
        plib_entry12 *grown = realloc(l->entries, cap * sizeof *grown);
        if (!grown) return NULL;
        l->entries = grown;
        l->cap = cap;
    }
    plib_entry12 *e = &l->entries[l->n];
    e->name = malloc((len + 1) * 2);
    if (!e->name) return NULL;
    memcpy(e->name, name, len * 2);
    e->name[len] = 0;
    e->name_len = len;
    e->hash = hash;
    e->compute = compute;
    e->pso = NULL;
    l->n++;
    return e;
}
static void plib_destroy(d3d_object *o) {
    plib12 *l = (plib12 *)o;
    for (uint32_t i = 0; i < l->n; i++) {
        free(l->entries[i].name);
        if (l->entries[i].pso) d3d_release(&l->entries[i].pso->base);
    }
    free(l->entries);
    pthread_mutex_destroy(&l->lock);
    release_dev(o);
}
static WINAPI HRESULT plib_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12PipelineLibrary, &IID_ID3D12PipelineLibrary1, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}
static WINAPI HRESULT plib_store(void *this, const uint16_t *name, void *state) {
    plib12 *l = this;
    pso12 *p = state;
    if (!name || !p || p->base.kind != K12_PSO) return E_INVALIDARG;
    pthread_mutex_lock(&l->lock);
    HRESULT hr = E_INVALIDARG;                                     // (a name already stored)
    if (!plib_find12(l, name)) {
        plib_entry12 *e = plib_add12(l, name, wide_len12(name), p->desc_hash, p->compute != nil && !p->render);
        if (e) { e->pso = p; d3d_addref(&p->base); hr = S_OK; }
        else hr = E_OUTOFMEMORY;
    }
    pthread_mutex_unlock(&l->lock);
    return hr;
}
static WINAPI HRESULT dev_create_graphics_pso_timed(void *this, const GFX_PSO_DESC *desc, const GUID *iid, void **out);
static WINAPI HRESULT dev_create_compute_pso_timed(void *this, const COMPUTE_PSO_DESC *desc, const GUID *iid, void **out);
static HRESULT plib_load12(plib12 *l, const uint16_t *name, const GFX_PSO_DESC *graphics, const COMPUTE_PSO_DESC *compute,
                           const GUID *iid, void **out) {
    if (out) *out = NULL;
    if (!name || !out || (!graphics && !compute)) return E_INVALIDARG;
    uint64_t hash = graphics ? graphics_desc_hash12(graphics) : compute_desc_hash12(compute);
    pthread_mutex_lock(&l->lock);
    plib_entry12 *e = plib_find12(l, name);
    HRESULT hr = E_INVALIDARG;
    int wanted = e && e->hash == hash && e->compute == (compute != NULL);
    if (wanted && e->pso) hr = pso_query(e->pso, iid, out);
    pthread_mutex_unlock(&l->lock);
    if (!wanted || *out) return hr;
    // (made outside the library's lock: games load pipeline states from many threads at once, and making one -
    //  translating and compiling its shaders - takes from milliseconds to seconds)
    void *made = NULL;
    hr = graphics ? dev_create_graphics_pso_timed(l->dev, graphics, &IID_ID3D12PipelineState, &made)
                  : dev_create_compute_pso_timed(l->dev, compute, &IID_ID3D12PipelineState, &made);
    if (hr < 0) return hr;
    pthread_mutex_lock(&l->lock);
    e = plib_find12(l, name);                                     // (the entry may have moved, or been made meanwhile)
    if (e && e->hash == hash && !e->pso) { e->pso = made; made = NULL; }   // (the library's reference)
    hr = e && e->pso ? pso_query(e->pso, iid, out) : pso_query(made, iid, out);
    pthread_mutex_unlock(&l->lock);
    if (made) d3d_release(&((pso12 *)made)->base);
    return hr;
}
static WINAPI HRESULT plib_load_graphics(void *this, const uint16_t *name, const GFX_PSO_DESC *desc, const GUID *iid, void **out) {
    return plib_load12(this, name, desc, NULL, iid, out);
}
static WINAPI HRESULT plib_load_compute(void *this, const uint16_t *name, const COMPUTE_PSO_DESC *desc, const GUID *iid, void **out) {
    return plib_load12(this, name, NULL, desc, iid, out);
}
static HRESULT pso_stream_parse(const PSO_STREAM_DESC *desc, GFX_PSO_DESC *g, COMPUTE_PSO_DESC *c, int *compute);
static WINAPI HRESULT plib_load_stream(void *this, const uint16_t *name, const PSO_STREAM_DESC *desc, const GUID *iid, void **out) {
    GFX_PSO_DESC graphics;
    COMPUTE_PSO_DESC compute;
    int is_compute = 0;
    if (out) *out = NULL;
    HRESULT hr = pso_stream_parse(desc, &graphics, &compute, &is_compute);
    if (hr != S_OK) return hr;
    return plib_load12(this, name, is_compute ? NULL : &graphics, is_compute ? &compute : NULL, iid, out);
}
static size_t plib_size12(plib12 *l) {
    size_t n = 8 + 4 + 8 + 4;
    for (uint32_t i = 0; i < l->n; i++) n += 4 + 8 + 4 + (size_t)l->entries[i].name_len * 2;
    return n;
}
static WINAPI size_t plib_serialized_size(void *this) {
    plib12 *l = this;
    pthread_mutex_lock(&l->lock);
    size_t n = plib_size12(l);
    pthread_mutex_unlock(&l->lock);
    return n;
}
static WINAPI HRESULT plib_serialize(void *this, void *data, size_t size) {
    plib12 *l = this;
    pthread_mutex_lock(&l->lock);
    size_t need = plib_size12(l);
    if (!data || size < need) { pthread_mutex_unlock(&l->lock); return E_INVALIDARG; }
    uint8_t *p = data;
    uint32_t version = PLIB_VERSION12;
    uint64_t adapter = plib_adapter12(l->dev);
    memcpy(p, plib_magic12, 8); p += 8;
    memcpy(p, &version, 4); p += 4;
    memcpy(p, &adapter, 8); p += 8;
    memcpy(p, &l->n, 4); p += 4;
    for (uint32_t i = 0; i < l->n; i++) {
        plib_entry12 *e = &l->entries[i];
        uint32_t compute = (uint32_t)e->compute;
        memcpy(p, &e->name_len, 4); p += 4;
        memcpy(p, &e->hash, 8); p += 8;
        memcpy(p, &compute, 4); p += 4;
        memcpy(p, e->name, (size_t)e->name_len * 2); p += (size_t)e->name_len * 2;
    }
    pthread_mutex_unlock(&l->lock);
    return S_OK;
}

static WINAPI HRESULT dev_create_pipeline_library(void *this, const void *blob, size_t size, const GUID *iid, void **out) {
    dev12 *d = this;
    if (out) *out = NULL;
    if (size && !blob) return E_INVALIDARG;
    const uint8_t *p = blob, *end = p + size;
    uint32_t count = 0;
    if (size) {
        uint32_t version;
        uint64_t adapter;
        if (size < 24 || memcmp(p, plib_magic12, 8)) return E_INVALIDARG;
        memcpy(&version, p + 8, 4);
        memcpy(&adapter, p + 12, 8);
        memcpy(&count, p + 20, 4);
        if (version != PLIB_VERSION12) return D3D12_ERROR_DRIVER_VERSION_MISMATCH12;
        if (adapter != plib_adapter12(d)) return D3D12_ERROR_ADAPTER_NOT_FOUND12;
        p += 24;
    }
    if (!out) return S_FALSE;
    plib12 *l = alloc12(sizeof *l, K12_PIPELINE_LIBRARY, g_plib_vtbl, d);
    l->base.destroy = plib_destroy;
    pthread_mutex_init(&l->lock, NULL);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len, compute;
        uint64_t hash;
        if (end - p < 16) goto bad;
        memcpy(&len, p, 4); memcpy(&hash, p + 4, 8); memcpy(&compute, p + 12, 4);
        p += 16;
        if ((size_t)(end - p) < (size_t)len * 2) goto bad;
        uint16_t *name = malloc(((size_t)len + 1) * 2);
        if (!name) goto bad;
        memcpy(name, p, (size_t)len * 2);
        name[len] = 0;
        p += (size_t)len * 2;
        plib_entry12 *e = plib_find12(l, name) ? NULL : plib_add12(l, name, len, hash, compute != 0);
        free(name);
        if (!e) goto bad;
    }
    HRESULT hr = plib_query(l, iid, out);
    d3d_release(&l->base);
    return hr;
bad:
    d3d_release(&l->base);
    return E_INVALIDARG;
}

static WINAPI HRESULT dev_create_pso_stream(void *this, const PSO_STREAM_DESC *desc, const GUID *iid, void **out) {
    GFX_PSO_DESC graphics;
    COMPUTE_PSO_DESC compute;
    int is_compute = 0;
    if (out) *out = NULL;
    HRESULT hr = pso_stream_parse(desc, &graphics, &compute, &is_compute);
    if (hr != S_OK) return hr;
    return is_compute ? dev_create_compute_pso(this, &compute, iid, out) : dev_create_graphics_pso(this, &graphics, iid, out);
}

// ---- command lists: graphics recording -----------------------------------------------------------------------------

static WINAPI void list_set_gfx_root(void *this, void *rs) { cmd12 *c = rec(this, C_ROOTSIG); c->obj = rs; c->gfx = 1; hold(rs); }
static WINAPI void list_set_gfx_table(void *this, uint32_t param, uint64_t gpu) { cmd12 *c = rec(this, C_TABLE); c->a = param; c->va = gpu; c->gfx = 1; }
static WINAPI void list_set_gfx_constant(void *this, uint32_t param, uint32_t value, uint32_t offset) {
    cmd12 *c = rec(this, C_CONSTANTS);
    c->a = param; c->b = 1; c->c = offset; c->values[0] = value; c->gfx = 1;
}
static WINAPI void list_set_gfx_constants(void *this, uint32_t param, uint32_t n, const void *values, uint32_t offset) {
    cmd12 *c = rec(this, C_CONSTANTS);
    c->a = param; c->b = n > 64 ? 64 : n; c->c = offset; c->gfx = 1;
    memcpy(c->values, values, 4 * c->b);
}
static WINAPI void list_set_gfx_root_va(void *this, uint32_t param, uint64_t va) { cmd12 *c = rec(this, C_ROOT_VA); c->a = param; c->va = va; c->gfx = 1; }
static WINAPI void list_topology(void *this, int32_t t) {
    list12 *list = this;
    cmd12 *c = rec(list, C_TOPOLOGY);
    c->a = (uint32_t)t;
    list->recording_topology = (uint32_t)t;
    list->recording_topology_set = 1;
}
static WINAPI void list_vbs(void *this, uint32_t start, uint32_t n, const VBV *views) {
    list12 *list = this;
    cmd12 *c = rec(list, C_VB);
    c->a = start; c->b = start >= 16 ? 0 : MIN(n, 16 - start);
    if (views) memcpy(c->values, views, c->b * sizeof(VBV));
    else memset(c->values, 0, sizeof c->values);
    for (uint32_t i = 0; i < c->b; i++)
        list->recording_vertex_buffers[start + i] = views ? views[i] : (VBV){ 0 };
}
static WINAPI void list_ib(void *this, const IBV *view) {
    list12 *list = this;
    cmd12 *c = rec(list, C_IB);
    if (view) memcpy(c->values, view, sizeof *view);
    list->recording_index_buffer = view ? *view : (IBV){ 0 };
}

static gpu_ir_resource *list_capture_gpu_buffer12(list12 *list, uint64_t va, uint32_t size,
                                                   uint64_t *offset) {
    if (!list || !va || !size || !offset) return NULL;
    for (uint32_t i = 0; i < 8; i++) {
        __typeof__(list->vb_cache[0]) *e = &list->vb_cache[i];
        if (e->buffer && va >= e->base && va - e->base <= e->len && size <= e->len - (va - e->base)) {
            *offset = va - e->base;
            gpu_ir_resource_retain(e->buffer);
            return e->buffer;
        }
    }
    dev12 *device = list->dev;
    pthread_rwlock_rdlock(&device->bufs_lock);                    // (not the device lock: recording threads share this)
    res12 *resource = bufs_find12(device, va, size);
    gpu_ir_resource *buffer = resource ? resource->gpu_buffer : NULL;
    uint64_t base = 0;
    if (buffer) {
        base = resource->buffer.gpuAddress;
        *offset = va - base;
        if (*offset > gpu_ir_buffer_size(buffer) || size > gpu_ir_buffer_size(buffer) - *offset)
            buffer = NULL;
        else gpu_ir_resource_retain(buffer);
    }
    pthread_rwlock_unlock(&device->bufs_lock);
    if (buffer) {
        __typeof__(list->vb_cache[0]) *e = &list->vb_cache[list->vb_cache_next++ & 7];
        if (e->buffer) gpu_ir_resource_release(e->buffer);
        gpu_ir_resource_retain(buffer);
        *e = (__typeof__(*e)){ base, gpu_ir_buffer_size(buffer), buffer };
    }
    return buffer;
}

static void list_release_captured_draw_buffers12(gpu_ir_draw_info *draw) {
    if (!draw) return;
    for (uint32_t i = 0; i < draw->vertex_buffer_count; i++)
        gpu_ir_resource_release(draw->vertex_buffers[i].resource);
    gpu_ir_resource_release(draw->index_buffer);
    draw->vertex_buffer_count = 0;
    draw->index_buffer = NULL;
    draw->index_buffer_size = 0;
}

static int list_capture_draw_buffers12(list12 *list, gpu_ir_draw_info *draw) {
    if (!list || !draw) return GPU_IR_INVALID;
    draw->vertex_buffers_captured = 1;
    for (uint32_t binding = 0; binding < 16; binding++) {
        const VBV *view = &list->recording_vertex_buffers[binding];
        if (!view->va && !view->size) continue;
        uint64_t offset = 0;
        gpu_ir_resource *resource = list_capture_gpu_buffer12(list, view->va, view->size, &offset);
        if (!resource) {
            list_release_captured_draw_buffers12(draw);
            return GPU_IR_INVALID;
        }
        draw->vertex_buffers[draw->vertex_buffer_count++] = (gpu_ir_vertex_buffer_binding){
            .binding = binding, .resource = resource, .offset = offset, .size = view->size,
            .stride = view->stride
        };
    }
    if (draw->indexed) {
        const IBV *view = &list->recording_index_buffer;
        if (!view->va && !view->size) return GPU_IR_OK;
        if ((!view->va || !view->size) || (view->format != 42 && view->format != 57)) {
            list_release_captured_draw_buffers12(draw);
            return GPU_IR_INVALID;
        }
        uint64_t offset = 0;
        gpu_ir_resource *resource = list_capture_gpu_buffer12(list, view->va, view->size, &offset);
        if (!resource) {
            list_release_captured_draw_buffers12(draw);
            return GPU_IR_INVALID;
        }
        draw->index_buffer = resource;
        draw->index_buffer_offset = offset;
        draw->index_buffer_size = view->size;
        draw->index_type = view->format == 42 ? GPU_IR_INDEX_TYPE_UINT32 : GPU_IR_INDEX_TYPE_UINT16;
    }
    return GPU_IR_OK;
}
static int list_capture_replay_draw_buffers12(list12 *list, const VBV vertex_views[16], const IBV *index_view,
                                               gpu_ir_draw_info *draw) {
    if (!list || !vertex_views || !index_view || !draw) return GPU_IR_INVALID;
    draw->vertex_buffers_captured = 1;
    for (uint32_t binding = 0; binding < 16; binding++) {
        const VBV *view = &vertex_views[binding];
        if (!view->va && !view->size) continue;
        uint64_t offset = 0;
        gpu_ir_resource *resource = list_capture_gpu_buffer12(list, view->va, view->size, &offset);
        if (!resource) {
            list_release_captured_draw_buffers12(draw);
            return GPU_IR_INVALID;
        }
        draw->vertex_buffers[draw->vertex_buffer_count++] = (gpu_ir_vertex_buffer_binding){
            .binding = binding, .resource = resource, .offset = offset, .size = view->size,
            .stride = view->stride
        };
    }
    if (draw->indexed) {
        const IBV *view = index_view;
        if (!view->va && !view->size) return GPU_IR_OK;
        if ((!view->va || !view->size) || (view->format != 42 && view->format != 57)) {
            list_release_captured_draw_buffers12(draw);
            return GPU_IR_INVALID;
        }
        uint64_t offset = 0;
        gpu_ir_resource *resource = list_capture_gpu_buffer12(list, view->va, view->size, &offset);
        if (!resource) {
            list_release_captured_draw_buffers12(draw);
            return GPU_IR_INVALID;
        }
        draw->index_buffer = resource;
        draw->index_buffer_offset = offset;
        draw->index_buffer_size = view->size;
        draw->index_type = view->format == 42 ? GPU_IR_INDEX_TYPE_UINT32 : GPU_IR_INDEX_TYPE_UINT16;
    }
    return GPU_IR_OK;
}
static WINAPI void list_targets(void *this, uint32_t n, const size_t *rtvs, int32_t single, const size_t *dsv) {
    cmd12 *c = rec(this, C_TARGETS);
    c->a = n > 8 ? 8 : n;
    for (uint32_t i = 0; rtvs && i < c->a; i++) {
        size_t handle = single ? rtvs[0] + 32 * (size_t)i : rtvs[i];
        c->tex[i] = view_texture(handle);
        c->resource[i] = view_target_metadata(handle, &c->subresource[i], &c->view_flags[i]);
    }
    if (dsv) {
        c->tex[8] = view_texture(*dsv);
        c->resource[8] = view_target_metadata(*dsv, &c->subresource[8], &c->view_flags[8]);
    }
}
static WINAPI void list_viewports(void *this, uint32_t n, const VIEWPORT *v) {
    cmd12 *c = rec(this, C_VIEWPORTS);
    c->a = n > 10 ? 10 : n;
    memcpy(c->values, v, c->a * sizeof *v);
}
static WINAPI void list_scissors(void *this, uint32_t n, const RECT12 *r) {
    cmd12 *c = rec(this, C_SCISSORS);
    c->a = n > 16 ? 16 : n;
    memcpy(c->values, r, c->a * sizeof *r);
}
static float d3d12_clear_float_input12(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    if ((bits & 0x7f800000u) == 0 && (bits & 0x007fffffu) != 0) return 0.0f;
    return value;
}
static int list_record_attachment_clear_ir12(list12 *list, cmd12 *command, int depth_stencil) {
    int texture_slot = depth_stencil ? 8 : 0;
    if (!command->tex[texture_slot]) return 1;
    if (command->b && !command->rects) {
        list->validation_error = E_INVALIDARG;
        return 0;
    }
    gpu_ir_rect *rectangles = NULL;
    if (command->b) {
        if ((size_t)command->b > SIZE_MAX / sizeof *rectangles) {
            list->validation_error = E_OUTOFMEMORY;
            return 0;
        }
        rectangles = malloc((size_t)command->b * sizeof *rectangles);
        if (!rectangles) {
            list->validation_error = E_OUTOFMEMORY;
            return 0;
        }
        for (uint32_t i = 0; i < command->b; i++) {
            rectangles[i] = (gpu_ir_rect){ command->rects[i].left, command->rects[i].top,
                                             command->rects[i].right, command->rects[i].bottom };
        }
    }
    id<MTLTexture> texture = (__bridge id<MTLTexture>)command->tex[texture_slot];
    gpu_ir_texture_desc desc = {
        .width = (uint32_t)texture.width, .height = (uint32_t)texture.height,
        .depth = (uint32_t)texture.depth, .array_layers = 1, .mip_levels = 1,
        .bytes_per_texel = 1, .sample_count = (uint32_t)texture.sampleCount,
        .block_width = 1, .block_height = 1, .bytes_per_block = 1
    };
    gpu_ir_resource *target = gpu_ir_texture_create(&desc, command->tex[texture_slot], gpu12_release_mtl_object);
    if (!target) {
        free(rectangles);
        list->validation_error = E_OUTOFMEMORY;
        return 0;
    }
    command->tex[texture_slot] = NULL; // the common texture resource now owns this retained Metal view
    uint32_t index = GPU_IR_NO_COMMAND;
    int rc;
    if (depth_stencil) {
        float depth;
        memcpy(&depth, &command->values[0], sizeof depth);
        rc = gpu_ir_record_clear_depth_stencil(&list->gpu_ir, target,
            command->a & (GPU_IR_CLEAR_DEPTH | GPU_IR_CLEAR_STENCIL), depth, command->values[1],
            rectangles, command->b, &index);
    } else {
        float color[4];
        memcpy(color, command->values, sizeof color);
        rc = gpu_ir_record_clear_color(&list->gpu_ir, target, color, rectangles, command->b, &index);
    }
    free(rectangles);
    gpu_ir_resource_release(target);
    if (rc != GPU_IR_OK || index == GPU_IR_NO_COMMAND) {
        list->validation_error = rc == GPU_IR_OUT_OF_MEMORY ? E_OUTOFMEMORY : E_INVALIDARG;
        return 0;
    }
    command->gpu_ir_recorded = 1;
    command->gpu_ir_index = index;
    free(command->rects);
    command->rects = NULL;
    return 1;
}

static WINAPI void list_clear_rtv(void *this, size_t rtv, const float color[4], uint32_t nrects, const RECT12 *rects) {
    list12 *list = this;
    if (list->validation_error) return;
    if (!color || (nrects && !rects)) { list->validation_error = E_INVALIDARG; return; }
    if ((size_t)nrects > SIZE_MAX / sizeof *rects) { list->validation_error = E_OUTOFMEMORY; return; }
    cmd12 *c = rec(list, C_CLEAR_RTV);
    c->tex[0] = view_texture(rtv);
    c->resource[0] = view_target_metadata(rtv, &c->subresource[0], &c->view_flags[0]);
    float clear_color[4];
    for (uint32_t i = 0; i < 4; i++) clear_color[i] = d3d12_clear_float_input12(color[i]);
    memcpy(c->values, clear_color, sizeof clear_color);
    if (nrects) {
        c->rects = malloc((size_t)nrects * sizeof *rects);
        if (!c->rects) { list->validation_error = E_OUTOFMEMORY; return; }
        memcpy(c->rects, rects, (size_t)nrects * sizeof *rects);
        c->b = nrects;
    }
    list_record_attachment_clear_ir12(list, c, 0);
}
static WINAPI void list_clear_dsv(void *this, size_t dsv, int32_t flags, float depth, uint8_t stencil_value, uint32_t nrects, const RECT12 *rects) {
    list12 *list = this;
    if (list->validation_error) return;
    if (nrects && !rects) { list->validation_error = E_INVALIDARG; return; }
    if ((size_t)nrects > SIZE_MAX / sizeof *rects) { list->validation_error = E_OUTOFMEMORY; return; }
    cmd12 *c = rec(list, C_CLEAR_DSV);
    c->tex[8] = view_texture(dsv);
    c->resource[8] = view_target_metadata(dsv, &c->subresource[8], &c->view_flags[8]);
    c->a = (uint32_t)flags;
    depth = d3d12_clear_float_input12(depth);
    if (depth < 0.0f) depth = 0.0f;
    else if (depth > 1.0f) depth = 1.0f;
    memcpy(&c->values[0], &depth, 4);
    c->values[1] = stencil_value;
    if (nrects) {
        c->rects = malloc((size_t)nrects * sizeof *rects);
        if (!c->rects) { list->validation_error = E_OUTOFMEMORY; return; }
        memcpy(c->rects, rects, (size_t)nrects * sizeof *rects);
        c->b = nrects;
    }
    list_record_attachment_clear_ir12(list, c, 1);
}
static WINAPI void list_clear_uav(void *this, uint64_t gpu, size_t cpu, void *resource, const void *values,
                                  uint32_t nrects, const RECT12 *rects, uint32_t floating_point) {
    list12 *list = this;
    if (list->validation_error) return;
    res12 *target = resource;
    if (!target || target->base.kind != K12_RESOURCE || target->dev != list->dev || !values ||
        (nrects && !rects)) {
        list->validation_error = E_INVALIDARG;
        return;
    }
    uint32_t gpu_index = 0;
    heap12 *gpu_heap = heap_gpu_descriptor(gpu, &gpu_index);
    gpu_ir_buffer_view *buffer_view = gpu_heap && gpu_heap->buffer_view &&
        gpu_index < gpu_heap->desc.NumDescriptors ? gpu_heap->buffer_view[gpu_index] : NULL;
    const gpu_ir_buffer_view_desc *buffer_desc = buffer_view ? gpu_ir_buffer_view_description(buffer_view) : NULL;
    if (buffer_desc && buffer_desc->kind == GPU_IR_BUFFER_VIEW_STRUCTURED) {
        list->validation_error = E_INVALIDARG;
        vx_log("D3D12: ClearUnorderedAccessView%s is incompatible with structured buffer UAVs",
               floating_point ? "Float" : "Uint");
        return;
    }
    cmd12 *command = rec(list, C_CLEAR_UAV);
    command->a = floating_point;
    command->b = nrects;
    command->off = cpu;
    command->va = gpu;
    command->resource[0] = target;
    d3d_addref(&target->base);
    memcpy(command->values, values, 4 * sizeof(uint32_t));
    if (floating_point) {
        float *clear = (float *)command->values;
        for (uint32_t i = 0; i < 4; i++)
            if (fpclassify(clear[i]) == FP_SUBNORMAL) clear[i] = 0.0f;
    }
    if (nrects) {
        command->rects = malloc((size_t)nrects * sizeof *command->rects);
        if (!command->rects) {
            list->validation_error = E_OUTOFMEMORY;
            return;
        }
        memcpy(command->rects, rects, (size_t)nrects * sizeof *command->rects);
    }
}
static WINAPI void list_clear_uav_uint(void *this, uint64_t gpu, size_t cpu, void *resource,
                                       const uint32_t values[4], uint32_t nrects, const RECT12 *rects) {
    list_clear_uav(this, gpu, cpu, resource, values, nrects, rects, 0);
}
static WINAPI void list_clear_uav_float(void *this, uint64_t gpu, size_t cpu, void *resource,
                                        const float values[4], uint32_t nrects, const RECT12 *rects) {
    list_clear_uav(this, gpu, cpu, resource, values, nrects, rects, 1);
}
static WINAPI void list_draw(void *this, uint32_t vertices, uint32_t instances, uint32_t start, uint32_t start_instance) {
    list12 *list = this;
    cmd12 *c = rec(list, C_DRAW);
    c->a = vertices; c->b = instances; c->c = start; c->off = start_instance;
    pso12 *pipeline = list->recording_pso;
    if (pipeline && pipeline->gpu_pipeline && vertices && instances) {
        uint32_t topology = list->recording_topology >= 1 && list->recording_topology <= 5 ?
            list->recording_topology : 4;
        gpu_ir_draw_info draw = { .topology = topology, .vertex_count = vertices,
            .instance_count = instances, .first_vertex = start, .first_instance = start_instance,
            .has_blend_constants = 1 };
        memcpy(draw.blend_constants, list->recording_blend_factor, sizeof draw.blend_constants);
        uint32_t index = GPU_IR_NO_COMMAND;
        int rc = list_capture_draw_buffers12(list, &draw);
        if (rc == GPU_IR_OK) rc = gpu_ir_record_draw(&list->gpu_ir, pipeline->gpu_pipeline, &draw, &index);
        list_release_captured_draw_buffers12(&draw);
        if (rc == GPU_IR_OUT_OF_MEMORY) list->validation_error = E_OUTOFMEMORY;
        else if (rc != GPU_IR_OK) list->validation_error = E_INVALIDARG;
        else if (rc == GPU_IR_OK && index != GPU_IR_NO_COMMAND) {
            c->gpu_ir_recorded = 1;
            c->gpu_ir_index = index;
        }
    }
}
static WINAPI void list_draw_indexed(void *this, uint32_t indices, uint32_t instances, uint32_t start, int32_t base_vertex, uint32_t start_instance) {
    list12 *list = this;
    cmd12 *c = rec(list, C_DRAW_INDEXED);
    c->a = indices; c->b = instances; c->c = start; c->off = (uint64_t)(int64_t)base_vertex; c->off2 = start_instance;
    pso12 *pipeline = list->recording_pso;
    if (pipeline && pipeline->gpu_pipeline && indices && instances) {
        uint32_t topology = list->recording_topology >= 1 && list->recording_topology <= 5 ?
            list->recording_topology : 4;
        gpu_ir_draw_info draw = { .topology = topology, .indexed = 1, .index_count = indices,
            .instance_count = instances, .first_index = start, .first_instance = start_instance,
            .vertex_offset = base_vertex, .has_blend_constants = 1 };
        memcpy(draw.blend_constants, list->recording_blend_factor, sizeof draw.blend_constants);
        uint32_t index = GPU_IR_NO_COMMAND;
        int rc = list_capture_draw_buffers12(list, &draw);
        if (rc == GPU_IR_OK) rc = gpu_ir_record_draw(&list->gpu_ir, pipeline->gpu_pipeline, &draw, &index);
        list_release_captured_draw_buffers12(&draw);
        if (rc == GPU_IR_OUT_OF_MEMORY) list->validation_error = E_OUTOFMEMORY;
        else if (rc != GPU_IR_OK) list->validation_error = E_INVALIDARG;
        else if (rc == GPU_IR_OK && index != GPU_IR_NO_COMMAND) {
            c->gpu_ir_recorded = 1;
            c->gpu_ir_index = index;
        }
    }
}
static WINAPI void list_blend_factor(void *this, const float f[4]) {
    list12 *list = this;
    cmd12 *command = rec(list, C_BLEND_FACTOR);
    if (f) memcpy(command->values, f, sizeof list->recording_blend_factor);
    else for (uint32_t i = 0; i < 4; i++) ((float *)command->values)[i] = 1.0f;
    memcpy(list->recording_blend_factor, command->values, sizeof list->recording_blend_factor);
}
static WINAPI void list_stencil_ref(void *this, uint32_t v) { cmd12 *c = rec(this, C_STENCIL_REF); c->a = v; }
static WINAPI void list_write_buffer_immediate(void *this, uint32_t count,
    const WRITE_BUFFER_IMMEDIATE_PARAMETER12 *parameters, const int32_t *modes) {
    list12 *list = this;
    if (!list || list->validation_error || !count) return;
    if (list->closed || !parameters || (size_t)count > SIZE_MAX / sizeof(write_buffer_immediate12)) {
        list->validation_error = E_INVALIDARG;
        return;
    }
    write_buffer_immediate12 *writes = calloc(count, sizeof *writes);
    if (!writes) { list->validation_error = E_OUTOFMEMORY; return; }
    uint32_t resolved = 0;
    int valid = 1;
    pthread_mutex_lock(&list->dev->lock);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t mode = modes ? (uint32_t)modes[i] : 0;
        if ((parameters[i].Dest & 3u) || mode > 2u) { valid = 0; break; }
        res12 *resource = state_resource_for_gpu_range_locked12(list->dev, parameters[i].Dest, sizeof(uint32_t));
        if (!resource || resource->dev != list->dev || !resource->buffer) { valid = 0; break; }
        writes[i] = (write_buffer_immediate12){
            .resource = resource,
            .offset = parameters[i].Dest - resource->buffer.gpuAddress,
            .value = parameters[i].Value,
            .mode = mode
        };
        d3d_addref(&resource->base);
        resolved++;
    }
    pthread_mutex_unlock(&list->dev->lock);
    if (!valid) {
        for (uint32_t i = 0; i < resolved; i++) d3d_release(&writes[i].resource->base);
        free(writes);
        list->validation_error = E_INVALIDARG;
        vx_log("D3D12: WriteBufferImmediate requires aligned GPU addresses, a live buffer, and a valid mode");
        return;
    }
    cmd12 *command = rec(list, C_WRITE_BUFFER_IMMEDIATE);
    command->a = count;
    command->immediate_writes = writes;
}
static WINAPI void list_begin_query(void *this, void *heap, int32_t type, uint32_t index) {
    cmd12 *c = rec(this, C_BEGIN_QUERY); c->obj = heap; hold(heap); c->a = (uint32_t)type; c->b = index;
}
static WINAPI void list_end_query(void *this, void *heap, int32_t type, uint32_t index) {
    cmd12 *c = rec(this, C_END_QUERY); c->obj = heap; hold(heap); c->a = (uint32_t)type; c->b = index;
}
static WINAPI void list_resolve_query(void *this, void *heap, int32_t type, uint32_t start, uint32_t n, void *dst, uint64_t offset) {
    cmd12 *c = rec(this, C_RESOLVE_QUERY); c->obj = heap; hold(heap); c->obj2 = dst; hold(dst); c->a = (uint32_t)type; c->b = start; c->c = n; c->off = offset;
}

// ---- command lists: what needs no Metal work ---------------------------------------------------------------------------

// PIX events and markers (WinPixEventRuntime calls these in shipping builds too): nothing to record.
static WINAPI void list_marker(void *this, uint32_t metadata, const void *data, uint32_t size) { }
static WINAPI void list_end_event(void *this) { }
// A discard only says the contents need not be kept; keeping them is correct.
// DiscardResource: what the resource holds becomes undefined. A render target or depth texture is cleared to zero here:
// placed resources share their heap's memory, and Cyberpunk 2077's temporal history, discarded on first use, kept
// whatever bytes another resource had left there - NaN that then never left the history (a black picture).
static WINAPI void list_discard_resource(void *this, void *resource, const void *region) {
    res12 *r = resource;
    (void)region;
    if (!r || !r->texture || !(r->desc.Flags & 0x3)) return;     // ALLOW_RENDER_TARGET, ALLOW_DEPTH_STENCIL
    if (r->texture.textureType == MTLTextureType3D) return;
    cmd12 *c = rec(this, C_DISCARD);
    d3d_addref(&r->base);                                         // (the list's reference: list_clear releases it)
    c->obj = r;
}
// Predication: the draws and dispatches it would skip still run (the result is the same unless a program counts on
// side effects of the skipped work; occlusion-predicated objects are hidden by depth anyway).
static WINAPI void list_set_predication(void *this, void *buffer, uint64_t offset, int32_t op) {
    static int warned;
    if (buffer && !warned) { warned = 1; vx_log("D3D12: SetPredication: predicated work is not skipped"); }
}
// Depth bounds (OPTIONS2 says unsupported), programmable sample positions (tier 0), view instancing (tier 0):
// programs that read the capability bits do not depend on these.
static WINAPI void list_depth_bounds(void *this, float min, float max) { }
static WINAPI void list_sample_positions(void *this, uint32_t samples, uint32_t pixels, const void *positions) { }
static WINAPI void list_view_instance_mask(void *this, uint32_t mask) { }

// ---- render passes (ID3D12GraphicsCommandList4) -------------------------------------------------------------------------
//
// OPTIONS5 reports render pass tier 0, the tier on which Windows itself emulates the API with the ordinary calls:
// BeginRenderPass binds the targets and clears those whose beginning access says so; EndRenderPass resolves those
// whose ending access says so (the clears become Metal load actions, as other clears do).

typedef struct { int32_t type; uint32_t pad; union { struct { void *src, *dst; uint32_t n, pad2; const void *subresources;
                 uint32_t format; int32_t mode; int32_t preserve_source; } resolve; uint8_t raw[48]; }; } RP_END12;
typedef struct { int32_t type; union { struct { uint32_t format; float color[4]; } clear; uint8_t raw[20]; }; } RP_BEGIN12;
typedef struct { size_t view; RP_BEGIN12 begin; RP_END12 end; } RP_RT12;
typedef struct { size_t view; RP_BEGIN12 depth_begin, stencil_begin; RP_END12 depth_end, stencil_end; } RP_DS12;
typedef struct { uint32_t src_subresource, dst_subresource, dst_x, dst_y; RECT12 src_rect; } RP_RESOLVE_SUB12;
_Static_assert(sizeof(RP_RT12) == 88 && sizeof(RP_DS12) == 168 && sizeof(RP_RESOLVE_SUB12) == 32,
               "D3D12_RENDER_PASS_* ABI layout");
struct render_pass_resolve12 { void *src, *dst; RP_RESOLVE_SUB12 sub; uint32_t format; int32_t mode; };
enum { RP_BEGIN_DISCARD, RP_BEGIN_PRESERVE, RP_BEGIN_CLEAR, RP_BEGIN_NO_ACCESS };
enum { RP_END_DISCARD, RP_END_PRESERVE, RP_END_RESOLVE, RP_END_NO_ACCESS };

static int render_pass_add_resolves12(list12 *list, const RP_END12 *end) {
    if (end->type != RP_END_RESOLVE) return end->type >= 0 && end->type <= RP_END_NO_ACCESS;
    if (!end->resolve.src || !end->resolve.dst || (end->resolve.n && !end->resolve.subresources)) return 0;
    uint32_t n = list->render_pass_nresolves + end->resolve.n;
    struct render_pass_resolve12 *grown = realloc(list->render_pass_resolves, (n ? n : 1) * sizeof *grown);
    if (!grown) return 0;
    list->render_pass_resolves = grown;
    for (uint32_t i = 0; i < end->resolve.n; i++) {
        struct render_pass_resolve12 *r = &grown[list->render_pass_nresolves++];
        r->src = end->resolve.src; r->dst = end->resolve.dst;
        r->sub = ((const RP_RESOLVE_SUB12 *)end->resolve.subresources)[i];
        r->format = end->resolve.format; r->mode = end->resolve.mode;
    }
    return 1;
}
static WINAPI void list_begin_render_pass(void *this, uint32_t n, const RP_RT12 *rts, const RP_DS12 *ds, int32_t flags) {
    list12 *list = this;
    if (list->validation_error) return;
    if (list->in_render_pass || n > 8 || (n && !rts) || (flags & ~7)) { list->validation_error = E_INVALIDARG; return; }
    list->in_render_pass = 1;
    list->render_pass_nresolves = 0;
    size_t views[8];
    for (uint32_t i = 0; i < n; i++) {
        views[i] = rts[i].view;
        if (rts[i].begin.type < 0 || rts[i].begin.type > RP_BEGIN_NO_ACCESS || !render_pass_add_resolves12(list, &rts[i].end)) {
            list->validation_error = E_INVALIDARG;
            return;
        }
    }
    int depth = ds && (ds->depth_begin.type != RP_BEGIN_NO_ACCESS || ds->stencil_begin.type != RP_BEGIN_NO_ACCESS);
    if (ds && (!render_pass_add_resolves12(list, &ds->depth_end) || !render_pass_add_resolves12(list, &ds->stencil_end))) {
        list->validation_error = E_INVALIDARG;
        return;
    }
    list_targets(list, n, n ? views : NULL, 0, depth ? &ds->view : NULL);
    for (uint32_t i = 0; i < n; i++)
        if (rts[i].begin.type == RP_BEGIN_CLEAR) list_clear_rtv(list, rts[i].view, rts[i].begin.clear.color, 0, NULL);
    if (depth) {
        int32_t clear = (ds->depth_begin.type == RP_BEGIN_CLEAR ? 1 : 0) | (ds->stencil_begin.type == RP_BEGIN_CLEAR ? 2 : 0);
        // (D3D12_DEPTH_STENCIL_VALUE: the depth float, then the stencil byte)
        float depth_value = ds->depth_begin.clear.color[0];
        uint8_t stencil_value = (uint8_t)(*(const uint32_t *)&ds->stencil_begin.clear.color[1] & 0xff);
        if (clear) list_clear_dsv(list, ds->view, clear, depth_value, stencil_value, 0, NULL);
    }
}
static WINAPI void list_end_render_pass(void *this) {
    list12 *list = this;
    if (list->validation_error) return;
    if (!list->in_render_pass) { list->validation_error = E_INVALIDARG; return; }
    list->in_render_pass = 0;
    list->render_pass_resolving = 1;
    for (uint32_t i = 0; i < list->render_pass_nresolves && !list->validation_error; i++) {
        struct render_pass_resolve12 *r = &list->render_pass_resolves[i];
        list_resolve_subresource_region(list, r->dst, r->sub.dst_subresource, r->sub.dst_x, r->sub.dst_y, r->src,
                                        r->sub.src_subresource, &r->sub.src_rect, r->format, r->mode);
    }
    list->render_pass_resolving = 0;
    list->render_pass_nresolves = 0;
}

// ---- command lists: replay into Metal -----------------------------------------------------------------------------

typedef struct {
    uint64_t root_va[64];
    uint32_t constants[64][64];
    uint64_t table[64];
} root_state;

// Small per-execution memory for root descriptors and constants the shaders read through pointers: chunks of
// 1 MB taken from the device's pool and given back when the command buffer completes (a new Metal buffer per
// execution cost more than the draws: every first write to its pages faulted).
enum { SCRATCH_CHUNK = 1 << 20, SCRATCH_POOL_MAX = 32 };
typedef struct { id<MTLBuffer> buf; size_t used; NSMutableArray *bufs; } scratch;
static id<MTLBuffer> scratch_chunk(dev12 *d, size_t size) {
    id<MTLBuffer> chunk = nil;
    if (size <= SCRATCH_CHUNK) {
        pthread_mutex_lock(&d->scratch_lock);
        chunk = d->scratch_pool.lastObject;
        if (chunk) [d->scratch_pool removeLastObject];
        pthread_mutex_unlock(&d->scratch_lock);
    }
    return chunk ?: [d->mtl newBufferWithLength:MAX((size_t)SCRATCH_CHUNK, size) options:MTLResourceStorageModeShared];
}
// The chunks of an execution the GPU is done with.
static void scratch_recycle(dev12 *d, NSArray *chunks) {
    pthread_mutex_lock(&d->scratch_lock);
    for (id<MTLBuffer> chunk in chunks)
        if (chunk.length == SCRATCH_CHUNK && d->scratch_pool.count < SCRATCH_POOL_MAX) [d->scratch_pool addObject:chunk];
    pthread_mutex_unlock(&d->scratch_lock);
}
static int scratch_alloc(dev12 *d, scratch *s, size_t n, size_t *offset) {
    if (!d || !s || !offset || n > SIZE_MAX - 31) return 0;
    size_t aligned = (n + 31) & ~(size_t)31;
    if (aligned > SIZE_MAX / 2) return 0;
    if (!s->buf || s->used > s->buf.length || aligned > s->buf.length - s->used) {
        id<MTLBuffer> replacement = scratch_chunk(d, aligned);
        if (!replacement) return 0;
        if (!s->bufs) s->bufs = [NSMutableArray new];
        [s->bufs addObject:replacement];
        s->buf = replacement;
        s->used = 0;
    }
    *offset = s->used;
    s->used += aligned;
    return 1;
}
static uint64_t scratch_put(dev12 *d, scratch *s, const void *data, size_t n) {
    if (!data) return 0;
    size_t offset;
    if (!scratch_alloc(d, s, n, &offset)) return 0;
    memcpy((uint8_t *)s->buf.contents + offset, data, n);
    size_t aligned = (n + 31) & ~(size_t)31;
    if (aligned > n) memset((uint8_t *)s->buf.contents + offset + n, 0, aligned - n);
    return s->buf.gpuAddress + offset;
}

static int d3d12_gpu_ir_copy_buffer(void *context, const gpu_ir_resource *src, uint64_t src_off,
                                   const gpu_ir_resource *dst, uint64_t dst_off, uint64_t size) {
    id<MTLBlitCommandEncoder> encoder = (__bridge id<MTLBlitCommandEncoder>)context;
    id<MTLBuffer> source = (__bridge id<MTLBuffer>)gpu_ir_backend_object(src);
    id<MTLBuffer> destination = (__bridge id<MTLBuffer>)gpu_ir_backend_object(dst);
    if (!encoder || !source || !destination || src_off > source.length || size > source.length - src_off ||
        dst_off > destination.length || size > destination.length - dst_off) return GPU_IR_RANGE;
    [encoder copyFromBuffer:source sourceOffset:(NSUInteger)src_off toBuffer:destination
            destinationOffset:(NSUInteger)dst_off size:(NSUInteger)size];
    return GPU_IR_OK;
}

typedef struct {
    dev12 *device;
    scratch *scratch_buffer;
    id<MTLBlitCommandEncoder> encoder;
} d3d12_gpu_ir_fill_context;

static int d3d12_gpu_ir_fill_buffer_pattern(void *context, const gpu_ir_resource *dst, uint64_t dst_offset,
                                            uint64_t size, const void *pattern, uint32_t pattern_size) {
    d3d12_gpu_ir_fill_context *fill = context;
    id<MTLBuffer> destination = (__bridge id<MTLBuffer>)gpu_ir_backend_object(dst);
    if (!fill || !fill->device || !fill->scratch_buffer || !fill->encoder || !destination || !pattern ||
        !pattern_size || pattern_size > GPU_IR_MAX_FILL_PATTERN_SIZE ||
        (pattern_size & (pattern_size - 1u)) ||
        !gpu_ir_resource_is_buffer(dst)) return GPU_IR_INVALID;
    if (dst_offset % pattern_size || size % pattern_size || dst_offset > NSUIntegerMax || size > NSUIntegerMax ||
        dst_offset > destination.length || size > destination.length - dst_offset ||
        dst_offset > gpu_ir_buffer_size(dst) || size > gpu_ir_buffer_size(dst) - dst_offset)
        return GPU_IR_RANGE;
    if (!size) return GPU_IR_OK;

    // Metal's blit fill writes one byte. Stage a bounded element pattern and copy it over the
    // selected UAV range without expanding the guest's full buffer size.
    const size_t max_upload_size = 1024u * 1024u;
    size_t upload_size = size < max_upload_size ? (size_t)size : max_upload_size;
    upload_size -= upload_size % pattern_size;
    size_t source_offset = 0;
    if (!upload_size || !scratch_alloc(fill->device, fill->scratch_buffer, upload_size, &source_offset))
        return GPU_IR_OUT_OF_MEMORY;
    uint8_t *source = (uint8_t *)fill->scratch_buffer->buf.contents + source_offset;
    for (size_t i = 0; i < upload_size; i += pattern_size)
        memcpy(source + i, pattern, pattern_size);

    uint64_t copied = 0;
    while (copied < size) {
        NSUInteger block = (NSUInteger)((size - copied) < upload_size ? (size - copied) : upload_size);
        [fill->encoder copyFromBuffer:fill->scratch_buffer->buf sourceOffset:source_offset toBuffer:destination
                    destinationOffset:(NSUInteger)(dst_offset + copied) size:block];
        copied += block;
    }
    return GPU_IR_OK;
}

static int d3d12_gpu_ir_execute_fill_buffer_pattern(dev12 *device, scratch *scratch_buffer,
                                                    id<MTLBlitCommandEncoder> encoder,
                                                    gpu_ir_resource *destination, uint64_t dst_offset,
                                                    uint64_t size, const void *pattern, uint32_t pattern_size) {
    gpu_ir_stream stream;
    gpu_ir_stream_init(&stream);
    uint32_t index = GPU_IR_NO_COMMAND;
    int rc = gpu_ir_record_fill_buffer_pattern(&stream, destination, dst_offset, size, pattern, pattern_size, &index);
    if (rc == GPU_IR_OK) {
        d3d12_gpu_ir_fill_context context = { device, scratch_buffer, encoder };
        gpu_ir_backend backend = { .context = &context, .fill_buffer_pattern = d3d12_gpu_ir_fill_buffer_pattern };
        rc = gpu_ir_execute(&stream, index, &backend);
    }
    gpu_ir_stream_destroy(&stream);
    return rc;
}

static int d3d12_gpu_ir_execute_fill_buffer(dev12 *device, scratch *scratch_buffer,
                                            id<MTLBlitCommandEncoder> encoder,
                                            gpu_ir_resource *destination, uint64_t dst_offset,
                                            uint64_t size, uint32_t value) {
    return d3d12_gpu_ir_execute_fill_buffer_pattern(device, scratch_buffer, encoder, destination,
                                                    dst_offset, size, &value, sizeof value);
}

static int d3d12_typed_uav_clear_pattern12(const heap12 *heap, uint32_t descriptor_index,
                                          const gpu_ir_buffer_view_desc *view, const uint32_t values[4],
                                          uint8_t pattern[GPU_IR_MAX_FILL_PATTERN_SIZE],
                                          uint32_t *pattern_size) {
    if (!heap || !view || !values || !pattern || !pattern_size ||
        descriptor_index >= heap->desc.NumDescriptors || !heap->cpu) return 0;
    VxDesc descriptor;
    memcpy(&descriptor, heap->cpu + 32ull * descriptor_index, sizeof descriptor);
    if (view->kind != GPU_IR_BUFFER_VIEW_TYPED) return 0;
    if (descriptor.fmt == 25) { // R10G10B10A2_UINT packs four values into one DWORD.
        if (view->element_size != sizeof(uint32_t) || view->byte_size % sizeof(uint32_t)) return 0;
        uint32_t packed = (values[0] & 0x3ffu) | ((values[1] & 0x3ffu) << 10) |
                          ((values[2] & 0x3ffu) << 20) | ((values[3] & 0x3u) << 30);
        memcpy(pattern, &packed, sizeof packed);
        *pattern_size = sizeof packed;
        return 1;
    }
    uint32_t channel_count = 0, channel_size = 0;
    switch (descriptor.fmt) {
    case 62: case 64: channel_count = 1; channel_size = 1; break; // R8_UINT / R8_SINT
    case 50: case 52: channel_count = 2; channel_size = 1; break; // R8G8_UINT / R8G8_SINT
    case 30: case 32: channel_count = 4; channel_size = 1; break; // R8G8B8A8_UINT / R8G8B8A8_SINT
    case 57: case 59: channel_count = 1; channel_size = 2; break; // R16_UINT / R16_SINT
    case 36: case 38: channel_count = 2; channel_size = 2; break; // R16G16_UINT / R16G16_SINT
    case 12: case 14: channel_count = 4; channel_size = 2; break; // R16G16B16A16_UINT / SINT
    case 42: case 43: channel_count = 1; channel_size = 4; break; // R32_UINT / R32_SINT
    case 17: case 18: channel_count = 2; channel_size = 4; break; // R32G32_UINT / R32G32_SINT
    case 3: case 4: channel_count = 4; channel_size = 4; break; // R32G32B32A32_UINT / SINT
    default: return 0;
    }
    uint32_t element_size = channel_count * channel_size;
    if (view->element_size != element_size || view->byte_size % element_size) return 0;
    for (uint32_t channel = 0; channel < channel_count; channel++)
        memcpy(pattern + channel * channel_size, &values[channel], channel_size);
    *pattern_size = element_size;
    return 1;
}

static int d3d12_gpu_ir_copy_resource(void *context, const gpu_ir_resource *src, const gpu_ir_resource *dst) {
    id<MTLBlitCommandEncoder> encoder = (__bridge id<MTLBlitCommandEncoder>)context;
    if (!encoder || !src || !dst) return GPU_IR_INVALID;
    if (gpu_ir_resource_is_buffer(src)) {
        id<MTLBuffer> source = (__bridge id<MTLBuffer>)gpu_ir_backend_object(src);
        id<MTLBuffer> destination = (__bridge id<MTLBuffer>)gpu_ir_backend_object(dst);
        uint64_t size = gpu_ir_buffer_size(src);
        if (!source || !destination || size != gpu_ir_buffer_size(dst) || size > source.length || size > destination.length) return GPU_IR_RANGE;
        [encoder copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0 size:(NSUInteger)size];
        return GPU_IR_OK;
    }
    id<MTLTexture> source = (__bridge id<MTLTexture>)gpu_ir_backend_object(src);
    id<MTLTexture> destination = (__bridge id<MTLTexture>)gpu_ir_backend_object(dst);
    if (!source || !destination) return GPU_IR_INVALID;
    [encoder copyFromTexture:source toTexture:destination];
    return GPU_IR_OK;
}

static int d3d12_gpu_ir_copy_texture(void *context, const gpu_ir_resource *src, const gpu_ir_texture_location *src_loc,
                                    const gpu_ir_resource *dst, const gpu_ir_texture_location *dst_loc,
                                    uint32_t width, uint32_t height, uint32_t depth) {
    id<MTLBlitCommandEncoder> encoder = (__bridge id<MTLBlitCommandEncoder>)context;
    id<MTLTexture> source_texture = gpu_ir_resource_is_buffer(src) ? nil : (__bridge id<MTLTexture>)gpu_ir_backend_object(src);
    id<MTLTexture> destination_texture = gpu_ir_resource_is_buffer(dst) ? nil : (__bridge id<MTLTexture>)gpu_ir_backend_object(dst);
    id<MTLBuffer> source_buffer = gpu_ir_resource_is_buffer(src) ? (__bridge id<MTLBuffer>)gpu_ir_backend_object(src) : nil;
    id<MTLBuffer> destination_buffer = gpu_ir_resource_is_buffer(dst) ? (__bridge id<MTLBuffer>)gpu_ir_backend_object(dst) : nil;
    const gpu_ir_texture_desc *texture = gpu_ir_texture_description(source_texture ? src : dst);
    if (!encoder || !texture || !width || !height || !depth) return GPU_IR_INVALID;
    uint64_t src_offset, dst_offset;
    uint32_t bw = texture->block_width, bh = texture->block_height, bytes_per_block = texture->bytes_per_block;
    if (source_texture && destination_texture) {
        [encoder copyFromTexture:source_texture sourceSlice:src_loc->array_slice sourceLevel:src_loc->mip_level
                    sourceOrigin:MTLOriginMake(src_loc->origin_x, src_loc->origin_y, src_loc->origin_z)
                      sourceSize:MTLSizeMake(width, height, depth)
                        toTexture:destination_texture destinationSlice:dst_loc->array_slice destinationLevel:dst_loc->mip_level
                destinationOrigin:MTLOriginMake(dst_loc->origin_x, dst_loc->origin_y, dst_loc->origin_z)];
    } else if (source_buffer && destination_texture) {
        src_offset = src_loc->buffer_offset + (uint64_t)src_loc->origin_z * src_loc->image_pitch +
                     (uint64_t)(src_loc->origin_y / bh) * src_loc->row_pitch +
                     (uint64_t)(src_loc->origin_x / bw) * bytes_per_block;
        [encoder copyFromBuffer:source_buffer sourceOffset:(NSUInteger)src_offset sourceBytesPerRow:(NSUInteger)src_loc->row_pitch
           sourceBytesPerImage:destination_texture.textureType == MTLTextureType3D ? (NSUInteger)src_loc->image_pitch : 0
                       sourceSize:MTLSizeMake(width, height, depth)
                       toTexture:destination_texture destinationSlice:dst_loc->array_slice destinationLevel:dst_loc->mip_level
               destinationOrigin:MTLOriginMake(dst_loc->origin_x, dst_loc->origin_y, dst_loc->origin_z)];
    } else if (source_texture && destination_buffer) {
        dst_offset = dst_loc->buffer_offset + (uint64_t)dst_loc->origin_z * dst_loc->image_pitch +
                     (uint64_t)(dst_loc->origin_y / bh) * dst_loc->row_pitch +
                     (uint64_t)(dst_loc->origin_x / bw) * bytes_per_block;
        [encoder copyFromTexture:source_texture sourceSlice:src_loc->array_slice sourceLevel:src_loc->mip_level
                    sourceOrigin:MTLOriginMake(src_loc->origin_x, src_loc->origin_y, src_loc->origin_z)
                      sourceSize:MTLSizeMake(width, height, depth)
                          toBuffer:destination_buffer destinationOffset:(NSUInteger)dst_offset
            destinationBytesPerRow:(NSUInteger)dst_loc->row_pitch
          destinationBytesPerImage:source_texture.textureType == MTLTextureType3D ? (NSUInteger)dst_loc->image_pitch : 0];
    } else return GPU_IR_INVALID;
    return GPU_IR_OK;
}

static int d3d12_is_depth_pixel_format(MTLPixelFormat format);

static pthread_mutex_t g_resolve_pipeline_lock = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSString *, NSDictionary *> *g_depth_resolve_pipeline_cache;
static NSMutableDictionary<NSString *, id<MTLRenderPipelineState>> *g_color_resolve_pipeline_cache;

static BOOL d3d12_get_depth_resolve_pipeline(id<MTLDevice> device, MTLPixelFormat format, BOOL array_source,
                                             id<MTLRenderPipelineState> *pipeline_out,
                                             id<MTLDepthStencilState> *depth_state_out) {
    if (!device || !pipeline_out || !depth_state_out) return NO;
    pthread_mutex_lock(&g_resolve_pipeline_lock);
    if (!g_depth_resolve_pipeline_cache) g_depth_resolve_pipeline_cache = [NSMutableDictionary dictionary];
    NSString *key = [NSString stringWithFormat:@"%p:%lu:%u", (__bridge void *)device,
                     (unsigned long)format, array_source ? 1u : 0u];
    NSDictionary *cached = g_depth_resolve_pipeline_cache[key];
    if (cached) {
        *pipeline_out = cached[@"pipeline"];
        *depth_state_out = cached[@"depth_state"];
        pthread_mutex_unlock(&g_resolve_pipeline_lock);
        return YES;
    }

    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct VxDepthResolveRegion { uint source_x, source_y, destination_x, destination_y, source_slice, mode; };\n"
        "struct VxDepthResolveOut { float depth [[depth(any)]]; };\n"
        "vertex float4 vx_depth_resolve_vs(uint id [[vertex_id]]) {\n"
        "  constexpr float2 p[3] = { float2(-1.0f,-1.0f), float2(3.0f,-1.0f), float2(-1.0f,3.0f) };\n"
        "  return float4(p[id], 0.0f, 1.0f);\n}\n"
        "fragment VxDepthResolveOut vx_depth_resolve_2d(float4 pos [[position]],\n"
        "    depth2d_ms<float, access::read> src [[texture(0)]],\n"
        "    constant VxDepthResolveRegion& region [[buffer(0)]]) {\n"
        "  uint2 dst = uint2(pos.xy); uint2 xy = uint2(region.source_x, region.source_y) +\n"
        "      (dst - uint2(region.destination_x, region.destination_y));\n"
        "  uint n = src.get_num_samples(); float value = region.mode == 2u ? 1.0f : 0.0f;\n"
        "  for (uint s = 0; s < n; ++s) { float v = src.read(xy, s);\n"
        "    if (region.mode == 1u) value += v;\n"
        "    else if (region.mode == 2u) value = min(value, v);\n"
        "    else value = max(value, v); }\n"
        "  if (region.mode == 1u) value /= float(n);\n"
        "  return VxDepthResolveOut{ value };\n}\n"
        "fragment VxDepthResolveOut vx_depth_resolve_array(float4 pos [[position]],\n"
        "    depth2d_ms_array<float, access::read> src [[texture(0)]],\n"
        "    constant VxDepthResolveRegion& region [[buffer(0)]]) {\n"
        "  uint2 dst = uint2(pos.xy); uint2 xy = uint2(region.source_x, region.source_y) +\n"
        "      (dst - uint2(region.destination_x, region.destination_y));\n"
        "  uint n = src.get_num_samples(); float value = region.mode == 2u ? 1.0f : 0.0f;\n"
        "  for (uint s = 0; s < n; ++s) { float v = src.read(xy, region.source_slice, s);\n"
        "    if (region.mode == 1u) value += v;\n"
        "    else if (region.mode == 2u) value = min(value, v);\n"
        "    else value = max(value, v); }\n"
        "  if (region.mode == 1u) value /= float(n);\n"
        "  return VxDepthResolveOut{ value };\n}";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    id<MTLFunction> vertex = [library newFunctionWithName:@"vx_depth_resolve_vs"];
    id<MTLFunction> fragment = [library newFunctionWithName:array_source ? @"vx_depth_resolve_array" : @"vx_depth_resolve_2d"];
    if (!library || !vertex || !fragment) {
        vx_log("D3D12: depth resolve shader compilation failed: %s",
               error.localizedDescription.UTF8String ?: "missing Metal function");
        pthread_mutex_unlock(&g_resolve_pipeline_lock);
        return NO;
    }

    MTLRenderPipelineDescriptor *pipeline_desc = [MTLRenderPipelineDescriptor new];
    pipeline_desc.vertexFunction = vertex;
    pipeline_desc.fragmentFunction = fragment;
    pipeline_desc.depthAttachmentPixelFormat = format;
    pipeline_desc.rasterSampleCount = 1;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
    MTLDepthStencilDescriptor *depth_desc = [MTLDepthStencilDescriptor new];
    depth_desc.depthCompareFunction = MTLCompareFunctionAlways;
    depth_desc.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depth_state = [device newDepthStencilStateWithDescriptor:depth_desc];
    if (!pipeline || !depth_state) {
        vx_log("D3D12: depth resolve pipeline creation failed: %s",
               error.localizedDescription.UTF8String ?: "Metal returned no pipeline state");
        pthread_mutex_unlock(&g_resolve_pipeline_lock);
        return NO;
    }
    NSDictionary *entry = @{ @"pipeline": pipeline, @"depth_state": depth_state };
    g_depth_resolve_pipeline_cache[key] = entry;
    *pipeline_out = pipeline;
    *depth_state_out = depth_state;
    pthread_mutex_unlock(&g_resolve_pipeline_lock);
    return YES;
}

typedef enum d3d12_color_resolve_kind {
    D3D12_COLOR_RESOLVE_UNSUPPORTED,
    D3D12_COLOR_RESOLVE_FLOAT,
    D3D12_COLOR_RESOLVE_UINT,
    D3D12_COLOR_RESOLVE_SINT,
} d3d12_color_resolve_kind;

static d3d12_color_resolve_kind d3d12_color_resolve_format_kind(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatA8Unorm: case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB:
    case MTLPixelFormatR8Snorm:
    case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm:
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatRGBA8Snorm: case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm: case MTLPixelFormatR16Float:
    case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm: case MTLPixelFormatRG16Float:
    case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm: case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatR32Float: case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA32Float:
    case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float:
    case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatBGR5A1Unorm: case MTLPixelFormatABGR4Unorm:
        return D3D12_COLOR_RESOLVE_FLOAT;
    case MTLPixelFormatR8Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatR16Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatR32Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatRGB10A2Uint:
        return D3D12_COLOR_RESOLVE_UINT;
    case MTLPixelFormatR8Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatR16Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatR32Sint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA32Sint:
        return D3D12_COLOR_RESOLVE_SINT;
    default:
        return D3D12_COLOR_RESOLVE_UNSUPPORTED;
    }
}

static BOOL d3d12_color_resolve_float_format(MTLPixelFormat format) {
    return d3d12_color_resolve_format_kind(format) == D3D12_COLOR_RESOLVE_FLOAT;
}

static BOOL d3d12_color_resolve_format_supported(MTLPixelFormat format) {
    return d3d12_color_resolve_format_kind(format) != D3D12_COLOR_RESOLVE_UNSUPPORTED;
}

static id<MTLRenderPipelineState> d3d12_get_color_resolve_pipeline(id<MTLDevice> device,
                                                                    MTLPixelFormat format,
                                                                    BOOL array_source) {
    d3d12_color_resolve_kind kind = d3d12_color_resolve_format_kind(format);
    if (!device || kind == D3D12_COLOR_RESOLVE_UNSUPPORTED) return nil;
    pthread_mutex_lock(&g_resolve_pipeline_lock);
    if (!g_color_resolve_pipeline_cache) g_color_resolve_pipeline_cache = [NSMutableDictionary dictionary];
    NSString *key = [NSString stringWithFormat:@"%p:%lu:%u", (__bridge void *)device,
                     (unsigned long)format, array_source ? 1u : 0u];
    id<MTLRenderPipelineState> cached = g_color_resolve_pipeline_cache[key];
    if (cached) {
        pthread_mutex_unlock(&g_resolve_pipeline_lock);
        return cached;
    }

    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vx_color_resolve_vs(uint id [[vertex_id]]) {\n"
        "  constexpr float2 p[3] = { float2(-1.0f,-1.0f), float2(3.0f,-1.0f), float2(-1.0f,3.0f) };\n"
        "  return float4(p[id], 0.0f, 1.0f);\n}\n"
        "struct VxColorResolveRegion { uint source_x, source_y, destination_x, destination_y, source_slice, mode; };\n";
    NSString *value_type = kind == D3D12_COLOR_RESOLVE_UINT ? @"uint4" :
        kind == D3D12_COLOR_RESOLVE_SINT ? @"int4" : @"float4";
    NSString *texture_type = kind == D3D12_COLOR_RESOLVE_UINT ? @"uint" :
        kind == D3D12_COLOR_RESOLVE_SINT ? @"int" : @"float";
    NSString *sample_read = array_source ? @"src.read(xy, region.source_slice, s)" : @"src.read(xy, s)";
    NSString *first_read = array_source ? @"src.read(xy, region.source_slice, 0u)" : @"src.read(xy, 0u)";
    NSString *texture_decl = array_source ? @"texture2d_ms_array<%@, access::read>" :
        @"texture2d_ms<%@, access::read>";
    NSString *reduce_body = kind == D3D12_COLOR_RESOLVE_FLOAT
        ? @"if (region.mode == 1u) value += sample; else if (region.mode == 2u) value = min(value, sample); else value = max(value, sample);"
        : @"if (region.mode == 2u) value = min(value, sample); else value = max(value, sample);";
    NSString *finish = kind == D3D12_COLOR_RESOLVE_FLOAT
        ? @"if (region.mode == 1u) value /= float(n); " : @"";
    NSString *fragment_source = [NSString stringWithFormat:
        @"fragment %@ vx_color_resolve(float4 pos [[position]], %@ src [[texture(0)]], "
         "constant VxColorResolveRegion& region [[buffer(0)]]) { "
         "uint2 dst = uint2(pos.xy); uint2 xy = uint2(region.source_x, region.source_y) + "
         "(dst - uint2(region.destination_x, region.destination_y)); "
         "uint n = src.get_num_samples(); %@ value = %@; "
         "for (uint s = 1u; s < n; ++s) { %@ sample = %@; %@ } %@ return value; }",
         value_type, [NSString stringWithFormat:texture_decl, texture_type], value_type, first_read, value_type,
         sample_read, reduce_body, finish];
    source = [NSString stringWithFormat:@"%@%@", source, fragment_source];
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    id<MTLFunction> vertex = [library newFunctionWithName:@"vx_color_resolve_vs"];
    id<MTLFunction> fragment = [library newFunctionWithName:@"vx_color_resolve"];
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.colorAttachments[0].pixelFormat = format;
    id<MTLRenderPipelineState> pipeline = library && vertex && fragment ?
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error] : nil;
    if (!pipeline) {
        vx_log("D3D12: partial color resolve pipeline creation failed for format %lu: %s",
               (unsigned long)format, error.localizedDescription.UTF8String ?: "Metal returned no pipeline state");
        pthread_mutex_unlock(&g_resolve_pipeline_lock);
        return nil;
    }
    g_color_resolve_pipeline_cache[key] = pipeline;
    pthread_mutex_unlock(&g_resolve_pipeline_lock);
    return pipeline;
}

static int d3d12_gpu_ir_resolve_texture(void *context, const gpu_ir_resource *src,
                                       const gpu_ir_resolve_location *src_location,
                                       const gpu_ir_resource *dst, const gpu_ir_resolve_location *dst_location,
                                       const gpu_ir_resolve_region *region, uint32_t mode) {
    id<MTLCommandBuffer> command_buffer = (__bridge id<MTLCommandBuffer>)context;
    id<MTLTexture> source = (__bridge id<MTLTexture>)gpu_ir_backend_object(src);
    id<MTLTexture> destination = (__bridge id<MTLTexture>)gpu_ir_backend_object(dst);
    if (!command_buffer || !source || !destination || !region || source.sampleCount <= 1 || destination.sampleCount != 1 ||
        source.pixelFormat != destination.pixelFormat) return GPU_IR_INVALID;
    NSUInteger source_width = gpu_ir_mip_extent((uint32_t)source.width, src_location->mip_level);
    NSUInteger source_height = gpu_ir_mip_extent((uint32_t)source.height, src_location->mip_level);
    NSUInteger destination_width = gpu_ir_mip_extent((uint32_t)destination.width, dst_location->mip_level);
    NSUInteger destination_height = gpu_ir_mip_extent((uint32_t)destination.height, dst_location->mip_level);
    BOOL full_region = !region->source_x && !region->source_y && !region->destination_x && !region->destination_y &&
        region->width == source_width && region->height == source_height &&
        source_width == destination_width && source_height == destination_height;
    BOOL depth_format = d3d12_is_depth_pixel_format(source.pixelFormat);
    if (depth_format && !full_region) {
        if ((source.pixelFormat != MTLPixelFormatDepth16Unorm && source.pixelFormat != MTLPixelFormatDepth32Float) ||
            !(destination.usage & MTLTextureUsageRenderTarget) ||
            (mode != GPU_IR_RESOLVE_MODE_AVERAGE && mode != GPU_IR_RESOLVE_MODE_MIN && mode != GPU_IR_RESOLVE_MODE_MAX))
            return GPU_IR_UNSUPPORTED;
        BOOL array_source = source.textureType == MTLTextureType2DMultisampleArray;
        if (!array_source && source.textureType != MTLTextureType2DMultisample) return GPU_IR_UNSUPPORTED;
        id<MTLDevice> device = command_buffer.commandQueue.device;
        id<MTLRenderPipelineState> pipeline = nil;
        id<MTLDepthStencilState> depth_state = nil;
        if (!d3d12_get_depth_resolve_pipeline(device, destination.pixelFormat, array_source, &pipeline, &depth_state))
            return GPU_IR_UNSUPPORTED;
        MTLRenderPassDescriptor *partial_pass = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLRenderPassDepthAttachmentDescriptor *attachment = partial_pass.depthAttachment;
        attachment.texture = destination;
        attachment.level = dst_location->mip_level;
        attachment.slice = dst_location->array_slice;
        attachment.loadAction = MTLLoadActionLoad;
        attachment.storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> partial_encoder = [command_buffer renderCommandEncoderWithDescriptor:partial_pass];
        if (!partial_encoder) return GPU_IR_UNSUPPORTED;
        struct { uint32_t source_x, source_y, destination_x, destination_y, source_slice, mode; } parameters = {
            region->source_x, region->source_y, region->destination_x, region->destination_y,
            src_location->array_slice, mode
        };
        [partial_encoder setRenderPipelineState:pipeline];
        [partial_encoder setDepthStencilState:depth_state];
        [partial_encoder setFragmentTexture:source atIndex:0];
        [partial_encoder setFragmentBytes:&parameters length:sizeof parameters atIndex:0];
        [partial_encoder setViewport:(MTLViewport){ 0, 0, (double)destination_width, (double)destination_height, 0, 1 }];
        [partial_encoder setScissorRect:(MTLScissorRect){ region->destination_x, region->destination_y,
            region->width, region->height }];
        [partial_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [partial_encoder endEncoding];
        return GPU_IR_OK;
    }
    BOOL custom_color_resolve = !depth_format &&
        (mode == GPU_IR_RESOLVE_MODE_MIN || mode == GPU_IR_RESOLVE_MODE_MAX ||
         (mode == GPU_IR_RESOLVE_MODE_AVERAGE && !full_region));
    if (custom_color_resolve) {
        if (!(destination.usage & MTLTextureUsageRenderTarget)) return GPU_IR_UNSUPPORTED;
        id<MTLRenderPipelineState> pipeline = d3d12_get_color_resolve_pipeline(
            command_buffer.commandQueue.device, destination.pixelFormat,
            source.textureType == MTLTextureType2DMultisampleArray);
        if (!pipeline) return GPU_IR_UNSUPPORTED;
        MTLRenderPassDescriptor *color_pass = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLRenderPassColorAttachmentDescriptor *attachment = color_pass.colorAttachments[0];
        attachment.texture = destination;
        attachment.level = dst_location->mip_level;
        attachment.slice = dst_location->array_slice;
        attachment.loadAction = full_region ? MTLLoadActionDontCare : MTLLoadActionLoad;
        attachment.storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> color_encoder = [command_buffer renderCommandEncoderWithDescriptor:color_pass];
        if (!color_encoder) return GPU_IR_UNSUPPORTED;
        struct { uint32_t source_x, source_y, destination_x, destination_y, source_slice, mode; } parameters = {
            region->source_x, region->source_y, region->destination_x, region->destination_y,
            src_location->array_slice, mode
        };
        [color_encoder setRenderPipelineState:pipeline];
        [color_encoder setFragmentTexture:source atIndex:0];
        [color_encoder setFragmentBytes:&parameters length:sizeof parameters atIndex:0];
        [color_encoder setViewport:(MTLViewport){ 0, 0, (double)destination_width, (double)destination_height, 0, 1 }];
        if (!full_region) [color_encoder setScissorRect:(MTLScissorRect){ region->destination_x, region->destination_y,
            region->width, region->height }];
        [color_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [color_encoder endEncoding];
        return GPU_IR_OK;
    }
    if (!full_region) return GPU_IR_UNSUPPORTED;
    if (depth_format && mode == GPU_IR_RESOLVE_MODE_AVERAGE) {
        if (source.pixelFormat != MTLPixelFormatDepth16Unorm && source.pixelFormat != MTLPixelFormatDepth32Float)
            return GPU_IR_UNSUPPORTED;
        BOOL array_source = source.textureType == MTLTextureType2DMultisampleArray;
        if (!array_source && source.textureType != MTLTextureType2DMultisample) return GPU_IR_UNSUPPORTED;
        id<MTLDevice> device = command_buffer.commandQueue.device;
        id<MTLRenderPipelineState> pipeline = nil;
        id<MTLDepthStencilState> depth_state = nil;
        if (!d3d12_get_depth_resolve_pipeline(device, destination.pixelFormat, array_source, &pipeline, &depth_state))
            return GPU_IR_UNSUPPORTED;
        MTLRenderPassDescriptor *average_pass = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLRenderPassDepthAttachmentDescriptor *depth_attachment = average_pass.depthAttachment;
        depth_attachment.texture = destination;
        depth_attachment.level = dst_location->mip_level;
        depth_attachment.slice = dst_location->array_slice;
        depth_attachment.loadAction = MTLLoadActionDontCare;
        depth_attachment.storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> average_encoder = [command_buffer renderCommandEncoderWithDescriptor:average_pass];
        if (!average_encoder) return GPU_IR_UNSUPPORTED;
        NSUInteger width = gpu_ir_mip_extent((uint32_t)destination.width, dst_location->mip_level);
        NSUInteger height = gpu_ir_mip_extent((uint32_t)destination.height, dst_location->mip_level);
        [average_encoder setRenderPipelineState:pipeline];
        [average_encoder setDepthStencilState:depth_state];
        [average_encoder setFragmentTexture:source atIndex:0];
        struct { uint32_t source_x, source_y, destination_x, destination_y, source_slice, mode; } parameters = {
            region->source_x, region->source_y, region->destination_x, region->destination_y,
            src_location->array_slice, mode
        };
        [average_encoder setFragmentBytes:&parameters length:sizeof parameters atIndex:0];
        [average_encoder setViewport:(MTLViewport){ 0, 0, (double)width, (double)height, 0, 1 }];
        [average_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [average_encoder endEncoding];
        return GPU_IR_OK;
    }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    if (d3d12_is_depth_pixel_format(source.pixelFormat)) {
        if (mode != GPU_IR_RESOLVE_MODE_MIN && mode != GPU_IR_RESOLVE_MODE_MAX) return GPU_IR_UNSUPPORTED;
        MTLRenderPassDepthAttachmentDescriptor *attachment = pass.depthAttachment;
        attachment.texture = source;
        attachment.level = src_location->mip_level;
        attachment.slice = src_location->array_slice;
        attachment.loadAction = MTLLoadActionLoad;
        attachment.storeAction = MTLStoreActionMultisampleResolve;
        attachment.resolveTexture = destination;
        attachment.resolveLevel = dst_location->mip_level;
        attachment.resolveSlice = dst_location->array_slice;
        attachment.depthResolveFilter = mode == GPU_IR_RESOLVE_MODE_MIN
            ? MTLMultisampleDepthResolveFilterMin : MTLMultisampleDepthResolveFilterMax;
    } else {
        if (mode != GPU_IR_RESOLVE_MODE_AVERAGE) return GPU_IR_UNSUPPORTED;
        MTLRenderPassColorAttachmentDescriptor *attachment = pass.colorAttachments[0];
        attachment.texture = source;
        attachment.level = src_location->mip_level;
        attachment.slice = src_location->array_slice;
        attachment.loadAction = MTLLoadActionLoad;
        attachment.storeAction = MTLStoreActionMultisampleResolve;
        attachment.resolveTexture = destination;
        attachment.resolveLevel = dst_location->mip_level;
        attachment.resolveSlice = dst_location->array_slice;
    }
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) return GPU_IR_UNSUPPORTED;
    [encoder endEncoding];
    return GPU_IR_OK;
}

static int d3d12_gpu_ir_barrier(void *context, const gpu_ir_barrier_desc *barrier) {
    (void)context;
    (void)barrier;
    // Replay ends the active Metal encoder at every recorded barrier. Metal's tracked-resource hazard handling
    // orders those encoders; keeping the barrier in the common stream preserves API order for other backends.
    return GPU_IR_OK;
}

// The pointer the shader's slot reads its descriptors through, from the root signature and the root arguments.
// Whether a root parameter or static sampler of this D3D12_SHADER_VISIBILITY reaches a shader of this DXIL stage kind
// (0 pixel, 1 vertex, 2 geometry, 3 hull, 4 domain, 5 compute). Compute shaders see every parameter; graphics stages
// only ALL and their own: the vertex and pixel shaders of one pipeline often use the same register for different
// parameters (b0 of each stage).
static int root_visible12(int32_t visibility, int stage) {
    static const int32_t own[] = { 5, 1, 4, 2, 3 };
    return stage == 5 || visibility == 0 || (stage >= 0 && stage < 5 && visibility == own[stage]);
}

static uint64_t resolve_slot(dev12 *d, const rootsig12 *rs, const root_state *st, const dxil_msl_slot *s, int stage, scratch *sc, NSMutableArray *used_bufs) {
    if (!rs) return 0;
    for (uint32_t i = 0; i < rs->nparams; i++) {
        const param12 *p = &rs->params[i];
        if (!root_visible12(p->visibility, stage)) continue;
        if (p->type == 0) {
            for (uint32_t k = 0; k < p->nranges; k++) {
                const range12 *g = &p->ranges[k];
                if (g->type != s->cls || g->space != s->space) continue;
                if (s->lower < g->base || (g->count != 0xFFFFFFFFu && s->lower - g->base >= g->count)) continue;
                return st->table[i] + 32ull * (g->offset + (s->lower - g->base));
            }
        } else if (p->type == 1) {
            if (s->cls != DXIL_CBV || p->reg != s->lower || p->space != s->space) continue;
            uint64_t va = scratch_put(d, sc, st->constants[i], 4 * p->n32);
            VxDesc v = { va, 0, 4 * p->n32, 0, 0, 0 };
            return scratch_put(d, sc, &v, sizeof v);
        } else {
            // root CBV (2), SRV (3), UAV (4): a descriptor of its own, unbounded as D3D12's root descriptors are
            int cls = p->type == 2 ? DXIL_CBV : p->type == 3 ? DXIL_SRV : DXIL_UAV;
            if (cls != s->cls || p->reg != s->lower || p->space != s->space) continue;
            VxDesc v = { st->root_va[i], 0, 0xFFFFFFF0u, s->stride, 0, 0 };
            return scratch_put(d, sc, &v, sizeof v);
        }
    }
    if (s->cls == DXIL_SAMPLER) {
        for (uint32_t i = 0; i < rs->nsamplers; i++) {
            if (rs->samplers[i].reg != s->lower || rs->samplers[i].space != s->space ||
                !root_visible12(rs->samplers[i].visibility, stage)) continue;
            VxDesc v = { 0, resource_id((id<MTLResource>)rs->samplers[i].state), 0, 0, 0, 0 };
            return scratch_put(d, sc, &v, sizeof v);
        }
    }
    vx_log("D3D12: the root signature has no place for %s register %u, space %u", s->cls == DXIL_SRV ? "SRV" : s->cls == DXIL_UAV ? "UAV" : s->cls == DXIL_CBV ? "CBV" : "sampler", s->lower, s->space);
    return 0;
}

// The buffer a GPU address is in, and the offset in it.
static id<MTLBuffer> buffer_at(dev12 *d, uint64_t va, uint64_t *off) {
    pthread_mutex_lock(&d->lock);
    id<MTLBuffer> found = nil;
    res12 *r = va ? bufs_find12(d, va, 1) : NULL;
    if (r) { found = r->buffer; *off = va - r->buffer.gpuAddress; }
    pthread_mutex_unlock(&d->lock);
    return found;
}

typedef struct {
    rootsig12 *rs;
    root_state st;
} root_binding;

// The root argument buffer of one stage (VxRoot): a pointer per resource range, then the two heaps.
static void stage_root(dev12 *d, const dxil_msl *msl, root_binding *rb, heap12 *heap, heap12 *sheap, scratch *sc, uint64_t *root, uint32_t *words) {
    uint32_t n = msl->nslots ? msl->nslots : 1;
    memset(root, 0, 8 * (n + 2));
    for (uint32_t k = 0; k < msl->nslots; k++) root[k] = resolve_slot(d, rb->rs, &rb->st, &msl->slots[k], msl->stage, sc, nil);
    root[n] = heap && heap->gpu ? heap->gpu.gpuAddress : 0;
    root[n + 1] = sheap && sheap->gpu ? sheap->gpu.gpuAddress : 0;
    *words = n + 2;
}

// Resolve one reflected shader register to its root parameter. Return 1 for a descriptor location, 2 for
// root constants/static samplers (no resource state to check), 0 when the application has not bound it, and -1
// for an invalid root-table range.
static int state_root_slot12(const rootsig12 *root, const root_state *state, const dxil_msl_slot *slot, int stage,
                             uint64_t *descriptor_va, uint64_t *root_va, const range12 **matched_range) {
    *descriptor_va = 0;
    *root_va = 0;
    *matched_range = NULL;
    if (!root) return 0;
    for (uint32_t i = 0; i < root->nparams; i++) {
        const param12 *parameter = &root->params[i];
        if (!root_visible12(parameter->visibility, stage)) continue;
        if (parameter->type == 0) {
            for (uint32_t k = 0; k < parameter->nranges; k++) {
                const range12 *range = &parameter->ranges[k];
                if (range->type != slot->cls || range->space != slot->space || slot->lower < range->base ||
                    (range->count != UINT32_MAX && slot->lower - range->base >= range->count)) continue;
                uint64_t descriptor_offset = (uint64_t)range->offset + slot->lower - range->base;
                if (descriptor_offset > (UINT64_MAX - state->table[i]) / 32) return -1;
                if (!state->table[i]) return 0;
                *descriptor_va = state->table[i] + 32 * descriptor_offset;
                *matched_range = range;
                return 1;
            }
        } else if (parameter->type == 1 && slot->cls == DXIL_CBV &&
                   parameter->reg == slot->lower && parameter->space == slot->space) {
            return 2;
        } else if (parameter->type >= 2) {
            int cls = parameter->type == 2 ? DXIL_CBV : parameter->type == 3 ? DXIL_SRV : DXIL_UAV;
            if (cls != slot->cls || parameter->reg != slot->lower || parameter->space != slot->space) continue;
            *root_va = state->root_va[i];
            return *root_va ? 1 : 0;
        }
    }
    if (slot->cls == DXIL_SAMPLER) {
        for (uint32_t i = 0; i < root->nsamplers; i++)
            if (root->samplers[i].reg == slot->lower && root->samplers[i].space == slot->space &&
                root_visible12(root->samplers[i].visibility, stage)) return 2;
    }
    return 0;
}

typedef struct {
    gpu_ir_resource_access *values;
    uint32_t count, capacity;
} gpu_ir_access_list12;

static int gpu_ir_access_add_resource12(gpu_ir_access_list12 *list, gpu_ir_resource *resource,
                                        uint32_t stage, uint32_t access) {
    if (!list || !resource || !stage || !access) return GPU_IR_INVALID;
    for (uint32_t i = 0; i < list->count; i++) {
        gpu_ir_resource_access *existing = &list->values[i];
        if (existing->resource == resource && existing->stage_mask == stage) {
            existing->access_mask |= access;
            return GPU_IR_OK;
        }
    }
    if (list->count == list->capacity) {
        uint32_t capacity = list->capacity ? list->capacity * 2 : 16;
        if (capacity < list->capacity || (size_t)capacity > SIZE_MAX / sizeof *list->values)
            return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_resource_access *values = realloc(list->values, (size_t)capacity * sizeof *values);
        if (!values) return GPU_IR_OUT_OF_MEMORY;
        list->values = values;
        list->capacity = capacity;
    }
    gpu_ir_resource_retain(resource);
    list->values[list->count++] = (gpu_ir_resource_access){ resource, stage, access };
    return GPU_IR_OK;
}

static void gpu_ir_access_list_release12(gpu_ir_access_list12 *list) {
    if (!list) return;
    for (uint32_t i = 0; i < list->count; i++) gpu_ir_resource_release(list->values[i].resource);
    free(list->values);
    memset(list, 0, sizeof *list);
}

static int gpu_ir_access_add12(gpu_ir_access_list12 *list, res12 *resource, uint32_t stage, uint32_t access) {
    if (!resource) return GPU_IR_INVALID;
    gpu_ir_resource *ir_resource = resource->gpu_buffer ? resource->gpu_buffer : resource->gpu_texture;
    return gpu_ir_access_add_resource12(list, ir_resource, stage, access);
}

typedef struct {
    gpu_ir_descriptor_value *values;
    uint32_t count, capacity;
} gpu_ir_descriptor_values12;

static void gpu_ir_descriptor_value_discard12(gpu_ir_descriptor_value *value) {
    if (!value) return;
    if (value->backend_release) value->backend_release(value->backend_object);
    if (value->backend_release_aux) value->backend_release_aux(value->backend_object_aux);
    value->backend_object = value->backend_object_aux = NULL;
    value->backend_release = value->backend_release_aux = NULL;
}

static int gpu_ir_descriptor_value_append12(gpu_ir_descriptor_values12 *list,
                                             const gpu_ir_descriptor_value *value) {
    if (!list || !value) return GPU_IR_INVALID;
    if (list->count == list->capacity) {
        uint32_t capacity = list->capacity ? list->capacity * 2 : 16;
        if (capacity < list->capacity || (size_t)capacity > SIZE_MAX / sizeof *list->values)
            return GPU_IR_OUT_OF_MEMORY;
        gpu_ir_descriptor_value *values = realloc(list->values, (size_t)capacity * sizeof *values);
        if (!values) return GPU_IR_OUT_OF_MEMORY;
        list->values = values;
        list->capacity = capacity;
    }
    list->values[list->count++] = *value;
    return GPU_IR_OK;
}

// (set: the next snapshot on this thread hands over its values, the caller discards each and frees the array)
static __thread gpu_ir_descriptor_values12 *t_raw_values12;
// (only the resources matter then: samplers are skipped and no Metal object is retained for the value)
static int gpu_ir_descriptor_value_for_slot12(dev12 *device, const rootsig12 *root,
    const root_state *state, const dxil_msl_slot *slot, uint32_t stage, heap12 *resource_heap,
    heap12 *sampler_heap, const range12 *range, uint32_t array_element,
    uint64_t descriptor_va, uint64_t root_va, gpu_ir_descriptor_value *out) {
    if (slot->cls == DXIL_SAMPLER && !range && !descriptor_va && !root_va) {
        for (uint32_t i = 0; i < root->nsamplers; i++) {
            const static_sampler12 *sampler = &root->samplers[i];
            if (sampler->reg != slot->lower || sampler->space != slot->space) continue;
            gpu_ir_descriptor_binding binding;
            if (!root->gpu_layout || gpu_ir_pipeline_layout_find_binding_for_stage(root->gpu_layout,
                    slot->space, slot->cls, sampler->reg, stage, &binding) != GPU_IR_OK)
                return GPU_IR_INVALID;
            *out = (gpu_ir_descriptor_value){
                .set = slot->space, .binding_namespace = slot->cls, .binding = sampler->reg,
                .kind = binding.kind, .stage_mask = stage, .access_mask = binding.access_mask
            };
            if (!sampler->state) return GPU_IR_INVALID;
            out->backend_object = (void *)CFBridgingRetain(sampler->state);
            out->backend_release = gpu12_release_mtl_object;
            return GPU_IR_OK;
        }
        return GPU_IR_INVALID;
    }

    uint32_t binding_register = 0;
    if (range) {
        binding_register = range->base;
    } else {
        int found = 0;
        for (uint32_t i = 0; i < root->nparams; i++) {
            const param12 *parameter = &root->params[i];
            uint32_t register_class = parameter->type == 2 ? DXIL_CBV :
                parameter->type == 3 ? DXIL_SRV : parameter->type == 4 ? DXIL_UAV : UINT32_MAX;
            if (register_class == slot->cls && parameter->reg == slot->lower &&
                parameter->space == slot->space) {
                binding_register = parameter->reg;
                found = 1;
                break;
            }
        }
        if (!found || !root_va) return GPU_IR_INVALID;
    }

    gpu_ir_descriptor_binding binding;
    if (!root->gpu_layout || gpu_ir_pipeline_layout_find_binding_for_stage(root->gpu_layout,
        slot->space, slot->cls, binding_register, stage, &binding) != GPU_IR_OK) return GPU_IR_INVALID;
    *out = (gpu_ir_descriptor_value){
        .set = slot->space, .binding_namespace = slot->cls, .binding = binding_register,
        .array_element = array_element, .kind = binding.kind, .stage_mask = stage,
        .access_mask = binding.access_mask
    };

    if (root_va) {
        if (slot->has_counter) return GPU_IR_INVALID; // D3D12 root UAV descriptors do not carry a hidden counter.
        res12 *resource = state_resource_for_gpu_range_locked12(device, root_va, 1);
        if (!resource || !resource->buffer || !resource->gpu_buffer ||
            root_va < resource->buffer.gpuAddress) return GPU_IR_INVALID;
        out->resource = resource->gpu_buffer;
        out->offset = root_va - resource->buffer.gpuAddress;
        out->range = resource->buffer.length - out->offset;
        return GPU_IR_OK;
    }

    heap12 *heap = slot->cls == DXIL_SAMPLER ? sampler_heap : resource_heap;
    if (!descriptor_heap_resident12(heap) || !heap->gpu || descriptor_va < heap->gpu.gpuAddress ||
        (descriptor_va - heap->gpu.gpuAddress) % 32) return GPU_IR_INVALID;
    uint64_t index64 = (descriptor_va - heap->gpu.gpuAddress) / 32;
    if (index64 >= heap->desc.NumDescriptors) return GPU_IR_INVALID;
    uint32_t index = (uint32_t)index64;
    VxDesc descriptor;
    memcpy(&descriptor, heap->cpu + 32ull * index, sizeof descriptor);
    uint8_t encoded_class = heap->descriptor_kind ? heap->descriptor_kind[index] : 0;
    if (encoded_class && encoded_class - 1 != slot->cls) return GPU_IR_INVALID;
    if (slot->cls == DXIL_SRV) {
        uint32_t raw_mapping = (descriptor.flags & DXIL_MSL_DESC_COMPONENT_MAPPING_MASK) >>
                               DXIL_MSL_DESC_COMPONENT_MAPPING_SHIFT;
        if (raw_mapping && !d3d12_normalize_srv_component_mapping12(raw_mapping, &out->component_mapping))
            return GPU_IR_INVALID;
    }

    if (binding.kind == GPU_IR_DESCRIPTOR_SAMPLER) {
        id<MTLSamplerState> sampler = heap->keep ? heap->keep[index] : nil;
        if (!sampler) {
            if (descriptor.va || descriptor.res || descriptor.size || descriptor.stride || descriptor.fmt || descriptor.flags)
                return GPU_IR_INVALID;
            out->flags = GPU_IR_DESCRIPTOR_VALUE_NULL;
            return GPU_IR_OK;
        }
        out->backend_object = (void *)CFBridgingRetain(sampler);
        out->backend_release = gpu12_release_mtl_object;
        return GPU_IR_OK;
    }

    gpu_ir_buffer_view *buffer_view = heap->buffer_view ? heap->buffer_view[index] : NULL;
    shader_view12 *shader_view = heap->shader_view ? &heap->shader_view[index] : NULL;
    if (slot->has_counter && (!(descriptor.flags & DXIL_MSL_DESC_HAS_COUNTER) ||
        !shader_view || !shader_view->counter_resource)) return GPU_IR_INVALID;
    if (buffer_view) {
        const gpu_ir_resource *resource = gpu_ir_buffer_view_resource(buffer_view);
        if (!resource) return GPU_IR_INVALID;
        out->resource = (gpu_ir_resource *)resource;
        out->buffer_view = buffer_view;
    } else if (shader_view && shader_view->resource) {
        res12 *resource = shader_view->resource;
        out->resource = resource->gpu_buffer ? resource->gpu_buffer : resource->gpu_texture;
        if (!out->resource) return GPU_IR_INVALID;
        if (heap->keep && heap->keep[index] && !t_raw_values12) {
            out->backend_object = (void *)CFBridgingRetain(heap->keep[index]);
            out->backend_release = gpu12_release_mtl_object;
        }
    } else if (descriptor.va) {
        uint64_t size = descriptor.size ? descriptor.size : 1;
        res12 *resource = state_resource_for_gpu_range_locked12(device, descriptor.va, size);
        if (!resource || !resource->buffer || !resource->gpu_buffer ||
            descriptor.va < resource->buffer.gpuAddress) return GPU_IR_INVALID;
        out->resource = resource->gpu_buffer;
        out->offset = descriptor.va - resource->buffer.gpuAddress;
        out->range = descriptor.size;
    } else if (descriptor.res) {
        return GPU_IR_INVALID;
    } else {
        out->flags = GPU_IR_DESCRIPTOR_VALUE_NULL;
    }
    if (shader_view && shader_view->counter_resource) {
        res12 *counter = shader_view->counter_resource;
        if (!counter->gpu_buffer || shader_view->counter_offset > counter->buffer.length ||
            sizeof(uint32_t) > counter->buffer.length - shader_view->counter_offset) return GPU_IR_INVALID;
        out->auxiliary_resource = counter->gpu_buffer;
        out->auxiliary_access_mask = GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE;
        out->auxiliary_offset = shader_view->counter_offset;
        out->auxiliary_range = sizeof(uint32_t);
    }
    return GPU_IR_OK;
}

static int gpu_ir_snapshot_root_bindings12_ex(dev12 *device, const dxil_msl *shader, uint32_t stage,
    root_binding *root_binding, heap12 *resource_heap, heap12 *sampler_heap,
    gpu_ir_descriptor_state **out_state, int bounded_only);

static int gpu_ir_snapshot_root_bindings12(dev12 *device, const dxil_msl *shader, uint32_t stage,
    root_binding *root_binding, heap12 *resource_heap, heap12 *sampler_heap,
    gpu_ir_descriptor_state **out_state) {
    return gpu_ir_snapshot_root_bindings12_ex(device, shader, stage, root_binding, resource_heap, sampler_heap, out_state, 0);
}
// bounded_only: unbounded ranges (bindless arrays reaching hundreds of thousands of descriptors) and bounded arrays of
// more than 64 are left out; what they reach is declared once an encoder (the heap's textures) instead of at every draw.
static int gpu_ir_snapshot_root_bindings12_ex(dev12 *device, const dxil_msl *shader, uint32_t stage,
    root_binding *root_binding, heap12 *resource_heap, heap12 *sampler_heap,
    gpu_ir_descriptor_state **out_state, int bounded_only) {
    if (!out_state) return GPU_IR_INVALID;
    *out_state = NULL;
    if (!shader) return GPU_IR_OK;
    if (!root_binding || !root_binding->rs || !root_binding->rs->gpu_layout) return GPU_IR_INVALID;
    gpu_ir_descriptor_values12 values = { 0 };
    int result = GPU_IR_OK;
    for (uint32_t i = 0; i < shader->nslots; i++) {
        const dxil_msl_slot *slot = &shader->slots[i];
        if (t_raw_values12 && slot->cls == DXIL_SAMPLER) continue;  // (accesses only: samplers name no resource)
        uint64_t descriptor_va, root_va;
        const range12 *range;
        int slot_result = state_root_slot12(root_binding->rs, &root_binding->st, slot, shader->stage,
                                            &descriptor_va, &root_va, &range);
        if (slot_result < 0) { result = GPU_IR_INVALID; break; }
        if (!slot_result || (slot_result == 2 && slot->cls != DXIL_SAMPLER)) continue;
        if (slot_result == 2) {
            gpu_ir_descriptor_value value;
            result = gpu_ir_descriptor_value_for_slot12(device, root_binding->rs, &root_binding->st,
                slot, stage, resource_heap, sampler_heap, NULL, 0, 0, 0, &value);
            if (result != GPU_IR_OK) break;
            result = gpu_ir_descriptor_value_append12(&values, &value);
            if (result != GPU_IR_OK) { gpu_ir_descriptor_value_discard12(&value); break; }
            continue;
        }
        if (root_va) {
            gpu_ir_descriptor_value value;
            result = gpu_ir_descriptor_value_for_slot12(device, root_binding->rs, &root_binding->st,
                slot, stage, resource_heap, sampler_heap, NULL, 0, 0, root_va, &value);
            if (result != GPU_IR_OK) break;
            result = gpu_ir_descriptor_value_append12(&values, &value);
            if (result != GPU_IR_OK) { gpu_ir_descriptor_value_discard12(&value); break; }
            continue;
        }
        if (!range || slot->lower < range->base) { result = GPU_IR_INVALID; break; }
        heap12 *heap = slot->cls == DXIL_SAMPLER ? sampler_heap : resource_heap;
        if (!descriptor_heap_resident12(heap) || !heap->gpu || descriptor_va < heap->gpu.gpuAddress ||
            (descriptor_va - heap->gpu.gpuAddress) % 32) { result = GPU_IR_INVALID; break; }
        uint64_t first = (descriptor_va - heap->gpu.gpuAddress) / 32;
        if (first >= heap->desc.NumDescriptors) { result = GPU_IR_INVALID; break; }
        uint64_t heap_available = heap->desc.NumDescriptors - first;
        uint64_t range_available = range->count == UINT32_MAX ? UINT64_MAX :
            (uint64_t)range->count - (slot->lower - range->base);
        // (so are large bounded arrays: a Texture2D t[2048] walked at every draw cost Cyberpunk 2077's character
        //  creator 2.6 s a frame)
        if (bounded_only && slot->cls != DXIL_SAMPLER && (slot->count == UINT32_MAX || slot->count > 64)) continue;
        uint64_t elements = slot->count == UINT32_MAX ? MIN(heap_available, range_available) :
            (slot->count ? slot->count : 1);
        if (!elements || elements > heap_available || elements > range_available) {
            result = GPU_IR_INVALID;
            break;
        }
        uint64_t element_va = 0, element_root_va = 0;
        const range12 *element_range = NULL;
        for (uint64_t element = 0; element < elements; element++) {
            if (element > UINT32_MAX - slot->lower) { result = GPU_IR_RANGE; break; }
            dxil_msl_slot element_slot = *slot;
            element_slot.lower += (uint32_t)element;
            // (the next element of the same range is the next descriptor: the root signature is walked once a range)
            if (element && element_range && (element_range->count == UINT32_MAX ||
                                             element_slot.lower - element_range->base < element_range->count)) {
                element_va += 32;
            } else {
                int element_result = state_root_slot12(root_binding->rs, &root_binding->st, &element_slot, shader->stage,
                    &element_va, &element_root_va, &element_range);
                if (element_result != 1 || element_root_va || !element_range) {
                    result = GPU_IR_INVALID;
                    break;
                }
            }
            gpu_ir_descriptor_value value;
            result = gpu_ir_descriptor_value_for_slot12(device, root_binding->rs, &root_binding->st,
                &element_slot, stage, resource_heap, sampler_heap, element_range,
                element_slot.lower - element_range->base, element_va, 0, &value);
            if (result != GPU_IR_OK) break;
            result = gpu_ir_descriptor_value_append12(&values, &value);
            if (result != GPU_IR_OK) { gpu_ir_descriptor_value_discard12(&value); break; }
        }
        if (result != GPU_IR_OK) break;
    }
    int state_created = 0;
    if (result == GPU_IR_OK && t_raw_values12) {                   // (the values themselves: no state made)
        *t_raw_values12 = values;
        return result;
    }
    if (result == GPU_IR_OK) {
        gpu_ir_descriptor_state_desc desc = { .values = values.values, .value_count = values.count };
        *out_state = gpu_ir_descriptor_state_create(root_binding->rs->gpu_layout, &desc);
        if (!*out_state) result = GPU_IR_INVALID;
        else state_created = 1; // Successful state creation takes ownership of backend-object references.
    }
    if (!state_created)
        for (uint32_t i = 0; i < values.count; i++) gpu_ir_descriptor_value_discard12(&values.values[i]);
    free(values.values);
    return result;
}

static int d3d12_shader_component_mapping_read12(dev12 *device, const dxil_msl *shader, uint32_t stage,
    root_binding *root, heap12 *resource_heap, heap12 *sampler_heap) {
    if (!device || !shader || !shader->uses_component_mapping_unsupported_read || !device->nonidentity_srv) return GPU_IR_OK;
    int mapped = 0;
    if (shader->nslots) {
        gpu_ir_descriptor_state *bindings = NULL;
        pthread_mutex_lock(&device->lock);
        int result = gpu_ir_snapshot_root_bindings12(device, shader, stage, root, resource_heap,
                                                     sampler_heap, &bindings);
        pthread_mutex_unlock(&device->lock);
        if (result != GPU_IR_OK) return result;
        for (uint32_t i = 0; i < gpu_ir_descriptor_state_value_count(bindings); i++) {
            gpu_ir_descriptor_value value;
            result = gpu_ir_descriptor_state_value_at(bindings, i, &value);
            if (result != GPU_IR_OK) break;
            if (value.component_mapping) { mapped = 1; break; }
        }
        gpu_ir_descriptor_state_release(bindings);
        if (result != GPU_IR_OK) return result;
    }
    // SM 6.6 handles can index any SRV in the visible heap. Scan conservatively because the shader
    // does not expose which runtime index will be read at dispatch/draw time.
    if (!mapped && shader->uses_heap && resource_heap && resource_heap->cpu) {
        pthread_mutex_lock(&device->lock);
        for (uint32_t i = 0; i < resource_heap->desc.NumDescriptors; i++) {
            uint8_t encoded_class = resource_heap->descriptor_kind ? resource_heap->descriptor_kind[i] : 0;
            if (encoded_class && encoded_class != DXIL_SRV + 1) continue;
            VxDesc descriptor;
            memcpy(&descriptor, resource_heap->cpu + 32ull * i, sizeof descriptor);
            uint32_t raw_mapping = (descriptor.flags & DXIL_MSL_DESC_COMPONENT_MAPPING_MASK) >>
                                   DXIL_MSL_DESC_COMPONENT_MAPPING_SHIFT;
            if (!raw_mapping) continue;
            uint32_t normalized;
            if (!d3d12_normalize_srv_component_mapping12(raw_mapping, &normalized)) {
                pthread_mutex_unlock(&device->lock);
                return GPU_IR_INVALID;
            }
            if (normalized) { mapped = 1; break; }
        }
        pthread_mutex_unlock(&device->lock);
    }
    if (mapped) {
        vx_log("D3D12: non-identity SRV component mapping is unsupported for comparison sampling and comparison gather");
        return GPU_IR_UNSUPPORTED;
    }
    return GPU_IR_OK;
}

static int gpu_ir_access_add_descriptor12(dev12 *device, gpu_ir_access_list12 *list, heap12 *heap,
                                          uint32_t index, uint32_t stage, uint32_t access) {
    if (!device || !list || !heap || index >= heap->desc.NumDescriptors) return GPU_IR_INVALID;
    VxDesc descriptor;
    memcpy(&descriptor, heap->cpu + 32ull * index, sizeof descriptor);
    shader_view12 *view = heap->shader_view ? &heap->shader_view[index] : NULL;
    int primary_result = GPU_IR_OK;
    if (heap->buffer_view && heap->buffer_view[index]) {
        const gpu_ir_resource *resource = gpu_ir_buffer_view_resource(heap->buffer_view[index]);
        if (!resource) return GPU_IR_INVALID;
        primary_result = gpu_ir_access_add_resource12(list, (gpu_ir_resource *)resource, stage, access);
    } else if (view && view->resource) {
        primary_result = gpu_ir_access_add12(list, view->resource, stage, access);
    } else if (descriptor.va) {
        res12 *resource = state_resource_for_gpu_range_locked12(device, descriptor.va,
            descriptor.size ? descriptor.size : 1);
        primary_result = resource ? gpu_ir_access_add12(list, resource, stage, access) : GPU_IR_INVALID;
    } else if (descriptor.res) return GPU_IR_INVALID;
    if (primary_result != GPU_IR_OK) return primary_result;
    if (view && view->counter_resource)
        return gpu_ir_access_add12(list, view->counter_resource, stage,
                                   GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE);
    return GPU_IR_OK; // A null descriptor has no resource access.
}

static int gpu_ir_collect_shader_accesses12(dev12 *device, const dxil_msl *shader, uint32_t stage,
                                            root_binding *root, heap12 *resource_heap, heap12 *sampler_heap,
                                            gpu_ir_access_list12 *list) {
    if (!shader) return GPU_IR_OK;
    if (!root) return GPU_IR_INVALID;
    if ((shader->uses_heap && (!root->rs || !(root->rs->flags & 0x400u) || !resource_heap || !resource_heap->gpu ||
                               resource_heap->desc.Type != 0 || !descriptor_heap_resident12(resource_heap))) ||
        (shader->uses_sampler_heap && (!root->rs || !(root->rs->flags & 0x800u) || !sampler_heap || !sampler_heap->gpu ||
                                       sampler_heap->desc.Type != 1 || !descriptor_heap_resident12(sampler_heap))))
        return GPU_IR_INVALID;

    gpu_ir_descriptor_state *bindings = NULL;
    gpu_ir_descriptor_values12 raw = { 0 };
    t_raw_values12 = &raw;
    int binding_result = gpu_ir_snapshot_root_bindings12_ex(device, shader, stage, root, resource_heap,
                                                            sampler_heap, &bindings, 1);
    t_raw_values12 = NULL;
    if (binding_result != GPU_IR_OK) return binding_result;
    int rc = GPU_IR_OK;
    for (uint32_t i = 0; i < raw.count; i++) {
        const gpu_ir_descriptor_value *value = &raw.values[i];
        const gpu_ir_resource *resource = value->resource ? value->resource :
            gpu_ir_buffer_view_resource(value->buffer_view);
        if (!resource || rc != GPU_IR_OK) continue; // samplers and explicit null descriptors do not name resource accesses.
        rc = gpu_ir_access_add_resource12(list, (gpu_ir_resource *)resource, value->stage_mask, value->access_mask);
        if (rc == GPU_IR_OK && value->auxiliary_resource)
            rc = gpu_ir_access_add_resource12(list, value->auxiliary_resource, value->stage_mask,
                                              value->auxiliary_access_mask);
    }
    for (uint32_t i = 0; i < raw.count; i++) gpu_ir_descriptor_value_discard12(&raw.values[i]);
    free(raw.values);
    if (rc != GPU_IR_OK) return rc;

    // Direct ResourceDescriptorHeap indexing is data-dependent. Match residency/useResource access to the DXIL class.
    // (A game's heap of a million descriptors walked at every draw cost more than the draws: past 4096 descriptors
    // what the heap holds is declared once an encoder instead, make_resident_render / use_resident_compute12.)
    if (shader->uses_heap && resource_heap->desc.NumDescriptors <= 4096) {
        uint32_t classes = shader->heap_resource_classes;
        if (!classes) classes = 1u << DXIL_SRV; // CreateHandleFromHeap defaults to an SRV absent AnnotateHandle.
        for (uint32_t i = 0; i < resource_heap->desc.NumDescriptors; i++) {
            uint8_t encoded = resource_heap->descriptor_kind ? resource_heap->descriptor_kind[i] : 0;
            if (!encoded) continue;
            uint32_t descriptor_class = encoded - 1;
            if (descriptor_class > DXIL_CBV || !(classes & (1u << descriptor_class))) continue;
            uint32_t access = descriptor_class == DXIL_CBV ? GPU_IR_ACCESS_UNIFORM_READ :
                descriptor_class == DXIL_UAV ? GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_SHADER_WRITE :
                                                GPU_IR_ACCESS_SHADER_READ;
            int rc = gpu_ir_access_add_descriptor12(device, list, resource_heap, i, stage, access);
            if (rc != GPU_IR_OK) return rc;
        }
    }
    return GPU_IR_OK;
}

static int state_texture_view12(dev12 *device, int32_t queue_type, resource_state_shadow12 **shadows, uint32_t *count,
                                uint32_t *capacity, const shader_view12 *view, uint32_t required_state, int write) {
    res12 *resource = view->resource;
    if (!resource || resource->dev != device) return 0;
    if (resource->buffer) return state_validate_resource_use_desc12(device, queue_type, shadows, count, capacity,
        resource, 0, required_state, write);
    if (!resource->texture) return 0;
    uint32_t mips_total = resource->desc.MipLevels ? resource->desc.MipLevels : 1;
    uint32_t planes_total = d3d12_resource_plane_count(resource->desc.Format);
    uint32_t slices_total = resource->desc.Dimension == 4 ? 1 : resource->desc.DepthOrArraySize;
    uint32_t mips = view->mips == 0 || view->mips == UINT32_MAX ? mips_total - view->mip : view->mips;
    uint32_t slices = view->slices == 0 || view->slices == UINT32_MAX ? slices_total - view->slice : view->slices;
    if (view->mip >= mips_total || mips > mips_total - view->mip || view->slice >= slices_total ||
        slices > slices_total - view->slice || view->plane >= planes_total) return 0;
    for (uint32_t slice = 0; slice < slices; slice++) {
        uint32_t array_slice = resource->desc.Dimension == 4 ? 0 : view->slice + slice;
        for (uint32_t mip = 0; mip < mips; mip++) {
            uint32_t subresource = view->mip + mip + array_slice * mips_total + view->plane * mips_total * slices_total;
            int use_result = state_validate_resource_use_desc12(device, queue_type, shadows, count, capacity,
                resource, subresource, required_state, write);
            if (use_result <= 0) return use_result;
        }
    }
    return 1;
}

static int state_validate_shader_descriptor12(dev12 *device, int32_t queue_type, resource_state_shadow12 **shadows,
                                              uint32_t *count, uint32_t *capacity, heap12 *heap, uint32_t index,
                                              uint32_t required_state, int write, int require_counter) {
    if (!descriptor_heap_resident12(heap) || index >= heap->desc.NumDescriptors) return 0;
    VxDesc descriptor;
    memcpy(&descriptor, heap->cpu + 32ull * index, sizeof descriptor);
    shader_view12 *view = heap->shader_view ? &heap->shader_view[index] : NULL;
    if (require_counter && (!(descriptor.flags & DXIL_MSL_DESC_HAS_COUNTER) || !view || !view->counter_resource)) return 0;
    if (view && view->resource) {
        int main_result = state_texture_view12(device, queue_type, shadows, count, capacity, view, required_state, write);
        if (main_result <= 0) return main_result;
        if (view->counter_resource) {
            if (!write || required_state != D3D12_STATE_UAV || view->counter_resource->dev != device ||
                !view->counter_resource->buffer || view->counter_offset > view->counter_resource->buffer.length ||
                sizeof(uint32_t) > view->counter_resource->buffer.length - view->counter_offset) return 0;
            return state_validate_resource_use_desc12(device, queue_type, shadows, count, capacity,
                view->counter_resource, 0, D3D12_STATE_UAV, 1);
        }
        return main_result;
    }
    if (descriptor.va) {
        res12 *resource = state_resource_for_gpu_range_locked12(device, descriptor.va,
            descriptor.size ? descriptor.size : 1);
        if (!resource) return 0;
        return state_validate_resource_use_desc12(device, queue_type, shadows, count, capacity,
            resource, 0, required_state, write);
    }
    if (descriptor.res) return 0; // resource descriptors created by the API must retain their D3D12 owner
    return 1; // null descriptor
}

static int descriptor_has_min_lod12(const heap12 *heap, uint32_t index) {
    if (!heap || index >= heap->desc.NumDescriptors || !heap->cpu) return 0;
    VxDesc descriptor;
    memcpy(&descriptor, heap->cpu + 32ull * index, sizeof descriptor);
    return (descriptor.flags & DXIL_MSL_DESC_HAS_MIN_LOD_CLAMP) != 0;
}

static const char *shader_min_lod_incompatible_access12(const dxil_msl *shader) {
    if (shader->uses_gather && shader->uses_texture_load) return "TextureGather/TextureLoad";
    if (shader->uses_gather) return "TextureGather";
    if (shader->uses_texture_load) return "TextureLoad";
    return NULL;
}

static int state_validate_shader_stage12(dev12 *device, int32_t queue_type, resource_state_shadow12 **shadows, uint32_t *count,
                                         uint32_t *capacity, const dxil_msl *shader, const rootsig12 *root,
                                         const root_state *state, heap12 *resource_heap, heap12 *sampler_heap) {
    if (!shader) return 1;
    const char *min_lod_incompatible_access = shader_min_lod_incompatible_access12(shader);
    // ResourceDescriptorHeap and SamplerDescriptorHeap are only valid when their root-signature flags
    // opt in, and require the matching shader-visible heap to be bound at submission time.
    if ((shader->uses_heap && (!root || !(root->flags & 0x400u) || !resource_heap || !resource_heap->gpu ||
                               resource_heap->desc.Type != 0 || !descriptor_heap_resident12(resource_heap))) ||
        (shader->uses_sampler_heap && (!root || !(root->flags & 0x800u) || !sampler_heap || !sampler_heap->gpu ||
                                       sampler_heap->desc.Type != 1 || !descriptor_heap_resident12(sampler_heap)))) return 0;
    uint32_t required_state_by_class[] = {
        D3D12_STATE_PIXEL_SHADER, D3D12_STATE_UAV, D3D12_STATE_VERTEX_CONSTANT, 0
    };
    for (uint32_t i = 0; i < shader->nslots; i++) {
        const dxil_msl_slot *slot = &shader->slots[i];
        uint64_t descriptor_va, root_va;
        const range12 *range;
        int slot_result = state_root_slot12(root, state, slot, shader->stage, &descriptor_va, &root_va, &range);
        if (slot_result < 0) return 0;
        if (slot_result == 0 || slot_result == 2) continue;
        if (slot->cls == DXIL_SAMPLER) {
            heap12 *heap = sampler_heap;
            if (root_va || !range || !heap || !heap->gpu || !descriptor_heap_resident12(heap)) return 0;
            uint64_t base = heap->gpu.gpuAddress;
            if (descriptor_va < base || (descriptor_va - base) % 32) return 0;
            uint64_t first = (descriptor_va - base) / 32;
            if (first >= heap->desc.NumDescriptors) return 0;
            uint64_t available = heap->desc.NumDescriptors - first;
            uint64_t elements = slot->count == UINT32_MAX ? available : (slot->count ? slot->count : 1);
            if (range->count != UINT32_MAX) {
                uint64_t range_available = (uint64_t)range->count - (slot->lower - range->base);
                if (elements > range_available) return 0;
            }
            if (elements > available) return 0;
            continue;
        }
        int is_root_descriptor = root_va != 0;
        int is_write = slot->cls == DXIL_UAV;
        if (slot->has_counter && is_root_descriptor) return 0;
        uint32_t required_state = slot->cls == DXIL_SRV ?
            (shader->stage == 0 ? D3D12_STATE_PIXEL_SHADER : D3D12_STATE_NON_PIXEL_SHADER) :
            required_state_by_class[slot->cls];
        if (is_root_descriptor) {
            res12 *resource = state_resource_for_gpu_range_locked12(device, root_va, 1);
            if (!resource) return 0;
            int use_result = state_validate_resource_use_desc12(device, queue_type, shadows, count, capacity,
                resource, 0, required_state, is_write);
            if (use_result <= 0) return use_result;
            continue;
        }
        heap12 *heap = slot->cls == DXIL_SAMPLER ? sampler_heap : resource_heap;
        if (!heap || !heap->gpu || !descriptor_heap_resident12(heap)) return 0;
        uint64_t base = heap->gpu.gpuAddress;
        if (descriptor_va < base || (descriptor_va - base) % 32) return 0;
        uint64_t first = (descriptor_va - base) / 32;
        if (first >= heap->desc.NumDescriptors) return 0;
        uint64_t available = heap->desc.NumDescriptors - first;
        uint64_t elements = slot->count == UINT32_MAX ? available : (slot->count ? slot->count : 1);
        if (range && range->count != UINT32_MAX) {
            uint64_t range_available = (uint64_t)range->count - (slot->lower - range->base);
            if (elements > range_available) return 0;
        }
        if (elements > available) return 0;
        // (an unbounded or large array is not walked descriptor by descriptor at every draw - a bindless range of a
        //  million descriptors made a frame of Cyberpunk 2077's loading screen take 8.6 s; what it reaches is not
        //  checked against its resources' states)
        if (slot->count == UINT32_MAX || elements > 64) continue;
        for (uint64_t element = 0; element < elements; element++) {
            uint32_t descriptor_index = (uint32_t)(first + element);
            if (min_lod_incompatible_access && slot->cls == DXIL_SRV && descriptor_has_min_lod12(heap, descriptor_index)) {
                vx_log("D3D12: %s cannot preserve a nonzero SRV ResourceMinLODClamp", min_lod_incompatible_access);
                return 0;
            }
            int use_result = state_validate_shader_descriptor12(device, queue_type, shadows, count, capacity,
                heap, descriptor_index, required_state, is_write, slot->has_counter);
            if (use_result <= 0) return use_result;
        }
    }
    // ResourceDescriptorHeap indexes are data-dependent, so check every live descriptor of each class used by DXIL.
    // This can include entries the dynamic index never selects, but avoids accepting a reachable bad D3D12 state.
    if (shader->uses_heap && resource_heap->desc.NumDescriptors <= 4096) {   // (as for the resource accesses)
        uint32_t classes = shader->heap_resource_classes;
        if (!classes) classes = 1u << DXIL_SRV;
        for (uint32_t index = 0; index < resource_heap->desc.NumDescriptors; index++) {
            uint8_t encoded = resource_heap->descriptor_kind ? resource_heap->descriptor_kind[index] : 0;
            if (!encoded) continue;
            uint32_t descriptor_class = encoded - 1;
            if (descriptor_class > DXIL_CBV || !(classes & (1u << descriptor_class))) continue;
            if (min_lod_incompatible_access && descriptor_class == DXIL_SRV &&
                descriptor_has_min_lod12(resource_heap, index)) {
                vx_log("D3D12: %s cannot preserve a nonzero SRV ResourceMinLODClamp", min_lod_incompatible_access);
                return 0;
            }
            uint32_t required_state = descriptor_class == DXIL_CBV ? D3D12_STATE_VERTEX_CONSTANT :
                descriptor_class == DXIL_UAV ? D3D12_STATE_UAV :
                shader->stage == 0 ? D3D12_STATE_PIXEL_SHADER : D3D12_STATE_NON_PIXEL_SHADER;
            int use_result = state_validate_shader_descriptor12(device, queue_type, shadows, count, capacity,
                resource_heap, index, required_state, descriptor_class == DXIL_UAV, 0);
            if (use_result <= 0) return use_result;
        }
    }
    return 1;
}

typedef struct {
    void *tex[9];                    // the render targets (retained), depth last
    res12 *resource[9];              // their API resources, retained by the recorded target commands
    int changed;
    MTLPrimitiveType prim;
    uint32_t topology;               // the D3D_PRIMITIVE_TOPOLOGY (geometry draws read adjacency and strips from it)
    VBV vb[16];
    IBV ib;
    VIEWPORT vp[10];
    uint32_t nvp;
    RECT12 sc[16];
    uint32_t nsc;
    float blend[4];
    uint32_t stencil_ref;
    queryheap12 *occ;                // the occlusion query heap of the last query begun
    int occ_on, occ_binary, occ_fresh;   // a query begun and not ended (counting into occ_index); not in a pass yet
    uint32_t occ_index;
    // Metal writes a pass's visibility results over its buffer when the pass ends, while a D3D12 query counts across
    // passes: each pass counts into a buffer of its own, added into the heap's results when it ends.
    queryheap12 *pass_heap;          // the open pass's heap (NULL: it counts nothing)
    id<MTLBuffer> pass_vis;
    uint32_t npass, pass_q[64];      // the queries the open pass counted: index | 1 << 30 begun in it | 1 << 31 binary
    // The last draw's resource accesses and what they were collected from: a draw with the same pipeline, root
    // tables and descriptors, heaps, vertex/index buffers and targets reuses them (collecting them was most of the
    // replay's time in a game: thousands of draws a frame with the same bindings).
    // (several: draws alternate between a few binding sets)
    uint64_t access_key[8];
    gpu_ir_access_list12 access_cache[8];
    uint8_t access_cached[8];
    uint32_t access_next;
} gfx_state;
static void gfx_access_cache_release12(gfx_state *g) {
    for (int i = 0; i < 8; i++) if (g->access_cached[i]) { gpu_ir_access_list_release12(&g->access_cache[i]); g->access_cached[i] = 0; }
}

static int gpu_ir_collect_draw_accesses12(dev12 *device, pso12 *pipeline, gfx_state *graphics,
                                          root_binding *root, heap12 *resource_heap, heap12 *sampler_heap,
                                          gpu_ir_access_list12 *list, int indexed) {
    int rc = gpu_ir_collect_shader_accesses12(device, &pipeline->vs, GPU_IR_STAGE_VERTEX,
                                               root, resource_heap, sampler_heap, list);
    if (rc != GPU_IR_OK) return rc;
    rc = gpu_ir_collect_shader_accesses12(device, &pipeline->ps, GPU_IR_STAGE_FRAGMENT,
                                           root, resource_heap, sampler_heap, list);
    if (rc != GPU_IR_OK) return rc;

    for (uint32_t i = 0; i < 16; i++) {
        const VBV *view = &graphics->vb[i];
        if (!view->va && !view->size) continue;
        res12 *resource = state_resource_for_gpu_range_locked12(device, view->va, view->size);
        rc = resource ? gpu_ir_access_add12(list, resource, GPU_IR_STAGE_VERTEX, GPU_IR_ACCESS_VERTEX_READ) : GPU_IR_INVALID;
        if (rc != GPU_IR_OK) return rc;
    }
    if (indexed) {
        res12 *resource = state_resource_for_gpu_range_locked12(device, graphics->ib.va, graphics->ib.size);
        rc = resource ? gpu_ir_access_add12(list, resource, GPU_IR_STAGE_VERTEX, GPU_IR_ACCESS_INDEX_READ) : GPU_IR_INVALID;
        if (rc != GPU_IR_OK) return rc;
    }

    const gpu_ir_pipeline_info *pipeline_info = gpu_ir_pipeline_description(pipeline->gpu_pipeline);
    if (!pipeline_info) return GPU_IR_INVALID;
    if (pipeline_info->stage_mask & GPU_IR_STAGE_FRAGMENT) {
        for (uint32_t i = 0; i < 8; i++) {
            if (!graphics->resource[i]) continue;
            rc = gpu_ir_access_add12(list, graphics->resource[i], GPU_IR_STAGE_FRAGMENT, GPU_IR_ACCESS_COLOR_WRITE);
            if (rc != GPU_IR_OK) return rc;
        }
    }
    if (graphics->resource[8] && (pipeline->depth_enable || pipeline->stencil_enable)) {
        uint32_t access = 0;
        if (pipeline->depth_enable)
            access |= pipeline->depth_write ? GPU_IR_ACCESS_DEPTH_WRITE : GPU_IR_ACCESS_DEPTH_READ;
        if (pipeline->stencil_enable)
            access |= pipeline->stencil_write ? GPU_IR_ACCESS_DEPTH_WRITE : GPU_IR_ACCESS_DEPTH_READ;
        if (!access) access = GPU_IR_ACCESS_DEPTH_READ;
        uint32_t stage = pipeline_info->stage_mask & GPU_IR_STAGE_FRAGMENT ? GPU_IR_STAGE_FRAGMENT : GPU_IR_STAGE_VERTEX;
        rc = gpu_ir_access_add12(list, graphics->resource[8], stage, access);
        if (rc != GPU_IR_OK) return rc;
    }
    return GPU_IR_OK;
}

static int gpu_ir_access_list_copy12(const gpu_ir_access_list12 *from, gpu_ir_access_list12 *to) {
    memset(to, 0, sizeof *to);
    if (!from->count) return GPU_IR_OK;
    to->values = malloc(from->count * sizeof *to->values);
    if (!to->values) return GPU_IR_INVALID;
    memcpy(to->values, from->values, from->count * sizeof *to->values);
    for (uint32_t i = 0; i < from->count; i++) gpu_ir_resource_retain(to->values[i].resource);
    to->count = to->capacity = from->count;
    return GPU_IR_OK;
}
static int gpu_ir_collect_draw_accesses_cached12(dev12 *device, pso12 *pipeline, gfx_state *graphics,
                                                 root_binding *root, heap12 *resource_heap, heap12 *sampler_heap,
                                                 gpu_ir_access_list12 *list, int indexed) {
    // (Cyberpunk 2077's draws never repeat a key - root constant buffers and tables move every draw - so the cache
    //  only cost its copies there: VXRT_D3D12_ACCESS_CACHE=1 keeps it)
    static int on = -1;
    if (on < 0) on = getenv("VXRT_D3D12_ACCESS_CACHE") && *getenv("VXRT_D3D12_ACCESS_CACHE") == '1';
    if (!on) return gpu_ir_collect_draw_accesses12(device, pipeline, graphics, root, resource_heap, sampler_heap, list, indexed);
    uint64_t h = 0xcbf29ce484222325ull;
    const void *ptrs[5] = { pipeline, root ? root->rs : NULL, resource_heap, sampler_heap, (void *)(intptr_t)indexed };
    h = fnv1a12(h, ptrs, sizeof ptrs);
    if (root) { h = fnv1a12(h, root->st.table, sizeof root->st.table); h = fnv1a12(h, root->st.root_va, sizeof root->st.root_va); }
    h = fnv1a12(h, graphics->vb, sizeof graphics->vb);
    if (indexed) h = fnv1a12(h, &graphics->ib, sizeof graphics->ib);
    h = fnv1a12(h, graphics->resource, sizeof graphics->resource);
    for (int i = 0; i < 8; i++)
        if (graphics->access_cached[i] && graphics->access_key[i] == h) return gpu_ir_access_list_copy12(&graphics->access_cache[i], list);
    int rc = gpu_ir_collect_draw_accesses12(device, pipeline, graphics, root, resource_heap, sampler_heap, list, indexed);
    uint32_t k = graphics->access_next++ & 7;
    if (graphics->access_cached[k]) { gpu_ir_access_list_release12(&graphics->access_cache[k]); graphics->access_cached[k] = 0; }
    if (rc == GPU_IR_OK && gpu_ir_access_list_copy12(list, &graphics->access_cache[k]) == GPU_IR_OK) {
        graphics->access_key[k] = h;
        graphics->access_cached[k] = 1;
    }
    return rc;
}

static void pass_query(gfx_state *g) {
    if (!g->occ_on || g->npass >= 64) return;
    for (uint32_t k = 0; k < g->npass; k++) if ((g->pass_q[k] & 0x3fffffff) == g->occ_index) return;
    g->pass_q[g->npass++] = g->occ_index | (g->occ_fresh ? 1u << 30 : 0) | (g->occ_binary ? 1u << 31 : 0);
    g->occ_fresh = 0;
}

static id<MTLComputePipelineState> occ_sum_pipeline(dev12 *d) {
    pthread_mutex_lock(&d->lock);
    if (!d->occ_sum) {
        static const char src[] =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void vx_occ(device ulong *dst [[buffer(0)]], device const ulong *src [[buffer(1)]], constant uint *q [[buffer(2)]], uint i [[thread_position_in_grid]]) {\n"
            "    uint x = q[i], k = x & 0x3fffffff;\n"
            "    ulong v = ((x >> 30) & 1 ? 0 : dst[k]) + src[k];\n"
            "    dst[k] = (x >> 31) ? (ulong)(v != 0) : v;\n"
            "}\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [d->mtl newLibraryWithSource:@(src) options:nil error:&err];
        id<MTLFunction> f = [lib newFunctionWithName:@"vx_occ"];
        d->occ_sum = f ? [d->mtl newComputePipelineStateWithFunction:f error:&err] : nil;
        if (!d->occ_sum) vx_log("D3D12: the occlusion query kernel: %s", err.localizedDescription.UTF8String);
    }
    id<MTLComputePipelineState> p = d->occ_sum;
    pthread_mutex_unlock(&d->lock);
    return p;
}

// Ends the open render pass, adding what its occlusion queries counted into their heap.

// ---- VXRT_D3D12_PASS_DUMP=<folder>: one frame's passes as pictures ------------------------------------------------
// When <folder>/now appears (it is removed), the frame after the next present is taken apart: each render pass's
// color and depth attachments as it leaves them, and each texture a dispatch writes, are written as PNGs
// (<frame>-<nnnn>-<what>.png: float formats tone-mapped x/(1+x), NaN and infinity magenta, depth as grey) and a line
// per pass and dispatch names its shaders (their bytecode hashes; VXRT_D3D12_SHADER_DUMP writes the bytecode).
static const char *pass_dump_dir12(void) {
    static const char *dir = (const char *)-1;
    if (dir == (const char *)-1) { dir = getenv("VXRT_D3D12_PASS_DUMP"); if (dir && !*dir) dir = NULL; }
    return dir;
}
static volatile int g_dump_armed12;                 // 1 while the frame being dumped is recorded
static volatile int g_dump_frame12, g_dump_seq12;
static __thread void *t_pass_tex12[9];               // (retained: CFBridgingRetain)
static __thread int t_pass_id12, t_pass_draws12, t_pass_npso12;
static __thread uint64_t t_pass_pso12[24][2];
static __thread void *t_pass_srv12[32];                // (VXRT_D3D12_PASS_DUMP_PS=<hash>: that pixel shader's textures)
static __thread int t_pass_nsrv12, t_pass_nsrv_seen12;
static __thread void *t_pass_buf12[16];                // (and the small buffers it reads: their first floats logged)
static __thread int t_pass_nbuf12;
static int pass_dump_ps12(uint64_t ps) {            // (a comma-separated list of hashes, or "all")
    const char *e = getenv("VXRT_D3D12_PASS_DUMP_PS");
    if (!e || !ps) return 0;
    if (!strcmp(e, "all")) return 1;
    for (const char *c = e; *c; ) {
        char *end;
        uint64_t h = strtoull(c, &end, 16);
        if (h == ps) return 1;
        if (end == c) break;
        c = *end == ',' ? end + 1 : end;
    }
    return 0;
}
static void pass_dump_present12(void) {
    const char *dir = pass_dump_dir12();
    if (!dir) return;
    if (g_dump_armed12) { g_dump_armed12 = 0; vx_log("D3D12: pass dump %d done (%d pictures)", g_dump_frame12, g_dump_seq12); return; }
    char path[1024];
    snprintf(path, sizeof path, "%s/now", dir);
    if (access(path, F_OK)) return;
    unlink(path);
    g_dump_frame12++;
    g_dump_seq12 = 0;
    g_dump_armed12 = 1;
    vx_log("D3D12: pass dump %d: the next frame's passes go to %s", g_dump_frame12, dir);
}
static id<MTLComputePipelineState> pass_dump_pipeline12(id<MTLDevice> mtl, int depth) {
    static id<MTLComputePipelineState> p[4];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!p[depth]) {
        NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void vx_dump(texture2d<float, access::read> t [[texture(0)]], device uchar4 *out [[buffer(0)]],\n"
            "                    constant uint4 &k [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
            "  if (g.x >= k.x || g.y >= k.y) return;\n"
            "  float4 v = t.read(g); bool bad = any(isnan(v)) || any(isinf(v));\n"
            "  if (k.z) v = v / (1.0f + abs(v));\n"
            "  float4 c = clamp(v, 0.0f, 1.0f);\n"
            "  if (k.w == 1) c = float4(c.r, c.r, c.r, 1); else if (k.w == 2) c = float4(c.r, c.g, 0, 1);\n"
            "  out[g.y * k.x + g.x] = bad ? uchar4(255, 0, 255, 255) : uchar4(uchar(c.b * 255), uchar(c.g * 255), uchar(c.r * 255), 255);\n"
            "}\n"
            "kernel void vx_dump_depth(depth2d<float, access::read> t [[texture(0)]], device uchar4 *out [[buffer(0)]],\n"
            "                    constant uint4 &k [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
            "  if (g.x >= k.x || g.y >= k.y) return;\n"
            "  float d = t.read(g); float v = isnan(d) ? 0 : pow(clamp(d, 0.0f, 1.0f), 0.25f);\n"
            "  uchar u = uchar(v * 255); out[g.y * k.x + g.x] = uchar4(u, u, u, 255);\n"
            "}\n"
            "kernel void vx_dump_stencil(texture2d<uint, access::read> t [[texture(0)]], device uchar4 *out [[buffer(0)]],\n"
            "                    constant uint4 &k [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
            "  if (g.x >= k.x || g.y >= k.y) return;\n"
            "  uint v = t.read(g).r & 255u;\n"
            "  out[g.y * k.x + g.x] = v ? uchar4(uchar((v * 151u) & 255u), uchar((v * 91u) & 255u), uchar(64u + (v & 3u) * 60u), 255) : uchar4(0, 0, 0, 255);\n"
            "}\n"
            "kernel void vx_dump_uint(texture2d<uint, access::read> t [[texture(0)]], device uchar4 *out [[buffer(0)]],\n"
            "                    constant uint4 &k [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
            "  if (g.x >= k.x || g.y >= k.y) return;\n"
            "  uint v = t.read(g).r;\n"
            "  out[g.y * k.x + g.x] = v ? uchar4(uchar(((v & 0xffu) * 37u + 40u) & 255u), uchar((((v >> 8) & 0xffu) * 73u + ((v & 0xffu) ? 80u : 0u)) & 255u), uchar(((v >> 16) * 151u + 60u) & 255u), 255) : uchar4(0, 0, 0, 255);\n"
            "}\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [mtl newLibraryWithSource:src options:nil error:&err];
        id<MTLFunction> fn = [lib newFunctionWithName:depth == 3 ? @"vx_dump_uint" : depth == 2 ? @"vx_dump_stencil" : depth ? @"vx_dump_depth" : @"vx_dump"];
        p[depth] = fn ? [mtl newComputePipelineStateWithFunction:fn error:&err] : nil;
        if (!p[depth]) vx_log("D3D12: pass dump kernel: %s", err.localizedDescription.UTF8String);
    }
    pthread_mutex_unlock(&lock);
    return p[depth];
}
static int pass_dump_format12(MTLPixelFormat f, int *hdr, int *chan, int *depth) {
    *hdr = 0; *chan = 0; *depth = 0;
    switch (f) {
    case MTLPixelFormatDepth32Float: case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float_Stencil8: *depth = 1; return 1;
    case MTLPixelFormatX32_Stencil8: *depth = 2; return 1;
    case MTLPixelFormatR8Uint: case MTLPixelFormatR16Uint: case MTLPixelFormatR32Uint: case MTLPixelFormatRG32Uint:
    case MTLPixelFormatRG16Uint: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA32Uint: *depth = 3; return 1;
    case MTLPixelFormatR16Float: case MTLPixelFormatR32Float: *hdr = 1; *chan = 1; return 1;
    case MTLPixelFormatRG16Float: case MTLPixelFormatRG32Float: *hdr = 1; *chan = 2; return 1;
    case MTLPixelFormatRGBA16Float: case MTLPixelFormatRGBA32Float: case MTLPixelFormatRG11B10Float: case MTLPixelFormatRGB9E5Float: *hdr = 1; return 1;
    case MTLPixelFormatR8Unorm: case MTLPixelFormatR16Unorm: case MTLPixelFormatR8Snorm: case MTLPixelFormatR16Snorm: *chan = 1; return 1;
    case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG8Snorm: case MTLPixelFormatRG16Snorm: *chan = 2; return 1;
    case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatBGR10A2Unorm: case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatRGBA16Snorm: return 1;
    default: return 0;
    }
}
// Encodes the copy of `t` (mip 0, slice 0) into a new compute encoder of `cb` (or `into`), the PNG written when the
// command buffer completes.
static void pass_dump_texture12(id<MTLCommandBuffer> cb, id<MTLComputeCommandEncoder> into, id<MTLTexture> t, const char *what) {
    if (!t || t.sampleCount > 1) return;
    int hdr, chan, depth;
    if (!pass_dump_format12(t.pixelFormat, &hdr, &chan, &depth)) return;
    id<MTLTexture> view = t;
    if (t.textureType != MTLTextureType2D || t.mipmapLevelCount > 1) {
        if (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray && t.textureType != MTLTextureTypeCube) return;
        if (!(t.usage & MTLTextureUsagePixelFormatView) && t.textureType != MTLTextureType2D) { /* (views of the same format are allowed) */ }
        view = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)];
        if (!view) return;
    }
    id<MTLComputePipelineState> pipe = pass_dump_pipeline12(cb.device, depth);
    if (!pipe) return;
    uint32_t w = (uint32_t)view.width, h = (uint32_t)view.height;
    id<MTLBuffer> out = [cb.device newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
    if (!out) return;
    id<MTLComputeCommandEncoder> e = into ? into : [cb computeCommandEncoder];
    if (into) [e memoryBarrierWithScope:MTLBarrierScopeTextures | MTLBarrierScopeBuffers];
    [e setComputePipelineState:pipe];
    [e setTexture:view atIndex:0];
    [e setBuffer:out offset:0 atIndex:0];
    uint32_t k[4] = { w, h, (uint32_t)hdr, (uint32_t)chan };
    [e setBytes:k length:sizeof k atIndex:1];
    [e useResource:view usage:MTLResourceUsageRead];
    [e dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    if (!into) [e endEncoding];
    else [e memoryBarrierWithScope:MTLBarrierScopeTextures | MTLBarrierScopeBuffers];
    char path[1024];
    snprintf(path, sizeof path, "%s/%d-%04d-%s-fmt%lu.png", pass_dump_dir12(), g_dump_frame12, __sync_add_and_fetch(&g_dump_seq12, 1), what,
             (unsigned long)t.pixelFormat);
    NSString *file = @(path);
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(out.contents, w, h, 8, w * 4, cs, kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
        CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
        CGImageDestinationRef dst = img ? CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:file], CFSTR("public.png"), 1, NULL) : NULL;
        if (dst) { CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
        if (img) CGImageRelease(img);
        if (ctx) CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
    }];
}
static void pass_dump_opened12(MTLRenderPassDescriptor *descriptor) {
    if (!g_dump_armed12) return;
    for (int k = 0; k < 9; k++) if (t_pass_tex12[k]) { CFBridgingRelease(t_pass_tex12[k]); t_pass_tex12[k] = NULL; }
    for (int k = 0; k < 8; k++) if (descriptor.colorAttachments[k].texture) t_pass_tex12[k] = (void *)CFBridgingRetain(descriptor.colorAttachments[k].texture);
    if (descriptor.depthAttachment.texture) t_pass_tex12[8] = (void *)CFBridgingRetain(descriptor.depthAttachment.texture);
    t_pass_id12 = __sync_add_and_fetch(&g_dump_seq12, 1);
    t_pass_draws12 = 0;
    t_pass_npso12 = 0;
}
static void pass_dump_draw12(const pso12 *p) {
    if (!g_dump_armed12 || !t_pass_id12 || !p) return;
    t_pass_draws12++;
    for (int i = 0; i < t_pass_npso12; i++) if (t_pass_pso12[i][0] == p->vs_code && t_pass_pso12[i][1] == p->ps_code) return;
    if (t_pass_npso12 < 24) { t_pass_pso12[t_pass_npso12][0] = p->vs_code; t_pass_pso12[t_pass_npso12][1] = p->ps_code; t_pass_npso12++; }
}
static void pass_dump_srvs12(const pso12 *p, const gpu_ir_resource_access *a, uint32_t n) {
    if (!g_dump_armed12 || !t_pass_id12 || !p || !pass_dump_ps12(p->ps_code)) return;
    t_pass_nsrv_seen12 = 1;
    for (uint32_t i = 0; i < n && t_pass_nsrv12 < 32; i++) {
        if (!(a[i].access_mask & GPU_IR_ACCESS_SHADER_READ)) continue;
        id obj = (__bridge id)gpu_ir_backend_object(a[i].resource);
        if ([obj conformsToProtocol:@protocol(MTLBuffer)] && ((id<MTLBuffer>)obj).length > 4096) {
            vx_log("D3D12: pass dump %d: pass %d reads a buffer of %lu bytes (gpu address %#llx)", g_dump_frame12, t_pass_id12,
                   (unsigned long)((id<MTLBuffer>)obj).length, (unsigned long long)((id<MTLBuffer>)obj).gpuAddress);
            continue;
        }
        if ([obj conformsToProtocol:@protocol(MTLBuffer)] && ((id<MTLBuffer>)obj).length <= 4096) {
            int dup = 0;
            for (int k = 0; k < t_pass_nbuf12; k++) if (t_pass_buf12[k] == (__bridge void *)obj) dup = 1;
            if (!dup && t_pass_nbuf12 < 16) {
                t_pass_buf12[t_pass_nbuf12++] = (void *)CFBridgingRetain(obj);
                vx_log("D3D12: pass dump %d: pass %d reads buffer %d at %#llx (%lu bytes, storage %lu)", g_dump_frame12, t_pass_id12, t_pass_nbuf12 - 1,
                       (unsigned long long)((id<MTLBuffer>)obj).gpuAddress, (unsigned long)((id<MTLBuffer>)obj).length, (unsigned long)((id<MTLBuffer>)obj).storageMode);
            }
            continue;
        }
        if (![obj conformsToProtocol:@protocol(MTLTexture)]) continue;
        int dup = 0;
        for (int k = 0; k < t_pass_nsrv12; k++) if (t_pass_srv12[k] == (__bridge void *)obj) dup = 1;
        if (!dup) t_pass_srv12[t_pass_nsrv12++] = (void *)CFBridgingRetain(obj);
    }
}
// (VXRT_D3D12_PASS_DUMP_PS passes: a few pixels of each RGBA16F/RGBA32F target logged as numbers)
static void pass_dump_pixels12(id<MTLCommandBuffer> cb, id<MTLTexture> t, int pass, int k) {
    if (!t || (t.pixelFormat != MTLPixelFormatRGBA16Float && t.pixelFormat != MTLPixelFormatRGBA32Float) || t.textureType != MTLTextureType2D) return;
    NSUInteger bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : 16;
    id<MTLBuffer> out = [cb.device newBufferWithLength:bpp * 5 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    struct { NSUInteger x[5], y[5]; } at = { { t.width / 2, t.width / 2, t.width / 2, t.width / 4, t.width / 2 },
                                              { t.height / 4, t.height / 2, t.height * 3 / 4, t.height / 2, t.height / 8 } };
    for (int i = 0; i < 5; i++)
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(at.x[i], at.y[i], 0) sourceSize:MTLSizeMake(1, 1, 1)
                  toBuffer:out destinationOffset:bpp * i destinationBytesPerRow:bpp destinationBytesPerImage:bpp];
    [b endEncoding];
    int frame = g_dump_frame12;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        char line[1024]; int n = snprintf(line, sizeof line, "D3D12: pass dump %d: pass %d c%d pixels:", frame, pass, k);
        for (int i = 0; i < 5; i++) {
            float v[4];
            if (bpp == 8) { const __fp16 *h = (const __fp16 *)((const uint8_t *)out.contents + 8 * i); for (int c = 0; c < 4; c++) v[c] = h[c]; }
            else memcpy(v, (const uint8_t *)out.contents + 16 * i, 16);
            n += snprintf(line + n, sizeof line - n, " (%lu,%lu)=[%g %g %g %g]", (unsigned long)at.x[i], (unsigned long)at.y[i], v[0], v[1], v[2], v[3]);
        }
        vx_log("%s", line);
    }];
}
static void pass_dump_closed12(id<MTLCommandBuffer> cb) {
    if (!t_pass_id12) return;
    int pass = t_pass_id12;
    t_pass_id12 = 0;
    for (int k = 0; k < t_pass_nsrv12; k++) {
        id<MTLTexture> t = (id<MTLTexture>)CFBridgingRelease(t_pass_srv12[k]);
        t_pass_srv12[k] = NULL;
        if (!g_dump_armed12) continue;
        vx_log("D3D12: pass dump %d: pass %d reads texture %d: %lux%lu fmt %lu type %lu mips %lu", g_dump_frame12, pass, k,
               (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long)t.textureType,
               (unsigned long)t.mipmapLevelCount);
        char what[64];
        snprintf(what, sizeof what, "pass%d-srv%d", pass, k);
        pass_dump_texture12(cb, nil, t, what);
    }
    t_pass_nsrv12 = 0;
    int seen_ps = t_pass_nsrv_seen12;
    t_pass_nsrv_seen12 = 0;
    for (int k = 0; k < t_pass_nbuf12; k++) {
        id<MTLBuffer> b = (id<MTLBuffer>)CFBridgingRelease(t_pass_buf12[k]);
        t_pass_buf12[k] = NULL;
        if (!g_dump_armed12 || !b) continue;
        NSUInteger len = b.length;
        id<MTLBuffer> copy = [cb.device newBufferWithLength:len options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:b sourceOffset:0 toBuffer:copy destinationOffset:0 size:len];
        [blit endEncoding];
        int frame = g_dump_frame12, kk = k;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            (void)done;
            const float *f = copy.contents;
            char line[1024]; int n = snprintf(line, sizeof line, "D3D12: pass dump %d: pass %d buffer %d (%lu bytes):", frame, pass, kk, (unsigned long)len);
            for (NSUInteger j = 0; j < len / 4 && j < 24 && n < (int)sizeof line; j++) n += snprintf(line + n, sizeof line - n, " %g", f[j]);
            vx_log("%s", line);
        }];
    }
    t_pass_nbuf12 = 0;
    id<MTLTexture> tex[9];
    for (int k = 0; k < 9; k++) { tex[k] = t_pass_tex12[k] ? (id<MTLTexture>)CFBridgingRelease(t_pass_tex12[k]) : nil; t_pass_tex12[k] = NULL; }
    if (!g_dump_armed12) return;
    char line[2048];
    int n = snprintf(line, sizeof line, "D3D12: pass dump %d: pass %d, %d draws:", g_dump_frame12, pass, t_pass_draws12);
    for (int k = 0; k < 9; k++) {
        id<MTLTexture> t = tex[k];
        if (!t) continue;
        if (n < (int)sizeof line) n += snprintf(line + n, sizeof line - n, " %s%d %lux%lu fmt %lu", k == 8 ? "depth" : "c", k == 8 ? 0 : k,
                                                (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat);
    }
    for (int i = 0; i < t_pass_npso12 && n < (int)sizeof line; i++)
        n += snprintf(line + n, sizeof line - n, " [vs %016llx ps %016llx]", (unsigned long long)t_pass_pso12[i][0], (unsigned long long)t_pass_pso12[i][1]);
    vx_log("%s", line);
    for (int k = 0; k < 9; k++) {
        if (!tex[k]) continue;
        char what[64];
        snprintf(what, sizeof what, "pass%d-%s%d", pass, k == 8 ? "depth" : "c", k == 8 ? 0 : k);
        pass_dump_texture12(cb, nil, tex[k], what);
        if (k < 8 && seen_ps) pass_dump_pixels12(cb, tex[k], pass, k);
        if (k == 8 && tex[k].pixelFormat == MTLPixelFormatDepth32Float_Stencil8 && tex[k].mipmapLevelCount == 1 && tex[k].textureType == MTLTextureType2D) {
            id<MTLTexture> sv = [tex[k] newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
            snprintf(what, sizeof what, "pass%d-stencil", pass);
            if (sv) pass_dump_texture12(cb, nil, sv, what);
        }
    }
}
static void pass_dump_dispatch12(id<MTLCommandBuffer> cb, id<MTLComputeCommandEncoder> ce, const pso12 *p,
                                 const gpu_ir_resource_access *a, uint32_t n, uint32_t x, uint32_t y, uint32_t z) {
    int step = __sync_add_and_fetch(&g_dump_seq12, 1);
    vx_log("D3D12: pass dump %d: dispatch %d %ux%ux%u of cs %016llx", g_dump_frame12, step, x, y, z, (unsigned long long)(p ? p->cs_code : 0));
    if (!ce) return;
    int shown = 0;
    // (VXRT_D3D12_PASS_DUMP_PS also names compute shaders whose inputs are wanted: every texture they read)
    if (p && pass_dump_ps12(p->cs_code))
        for (uint32_t i = 0; i < n; i++) {
            if (!(a[i].access_mask & GPU_IR_ACCESS_SHADER_READ) || (a[i].access_mask & GPU_IR_ACCESS_SHADER_WRITE)) continue;
            id obj = (__bridge id)gpu_ir_backend_object(a[i].resource);
            if (![obj conformsToProtocol:@protocol(MTLTexture)]) continue;
            id<MTLTexture> t = obj;
            char what[96];
            snprintf(what, sizeof what, "dispatch%d-cs%016llx-in%u-%lux%lu", step, (unsigned long long)p->cs_code, i,
                     (unsigned long)t.width, (unsigned long)t.height);
            pass_dump_texture12(cb, ce, t, what);
        }
    for (uint32_t i = 0; i < n && shown < 4; i++) {
        if (!(a[i].access_mask & GPU_IR_ACCESS_SHADER_WRITE)) continue;
        id obj = (__bridge id)gpu_ir_backend_object(a[i].resource);
        if (![obj conformsToProtocol:@protocol(MTLTexture)]) continue;
        char what[64];
        snprintf(what, sizeof what, "dispatch%d-cs%016llx-u%u", step, (unsigned long long)(p ? p->cs_code : 0), i);
        pass_dump_texture12(cb, ce, (id<MTLTexture>)obj, what);
        shown++;
    }
}

// ---- VXRT_D3D12_GPU_PROFILE=<folder>: one frame's encoders timed on the GPU ---------------------------------------
// When <folder>/prof appears (it is removed), every render pass and compute encoder of the next frame gets GPU
// timestamps at its start and end (Metal stage-boundary counters); a second later the encoders are logged longest
// first, with the shaders they ran (bytecode hashes) - which pass the GPU spends a frame on.
static id<MTLCounterSampleBuffer> g_prof_buf12;
static volatile int g_prof_armed12, g_prof_next12, g_prof_frame12;
static char *g_prof_label12[2048];
static __thread char *t_prof_label12;
static __thread size_t t_prof_len12;
static const char *gpu_profile_dir12(void) {
    static const char *dir = (const char *)-1;
    if (dir == (const char *)-1) { dir = getenv("VXRT_D3D12_GPU_PROFILE"); if (dir && !*dir) dir = NULL; }
    return dir;
}
// The pair of sample indices for a new encoder (or -1), its label started.
static int prof_begin12(id<MTLDevice> mtl, char kind) {
    if (!g_prof_armed12 || !g_prof_buf12) return -1;
    int k = __sync_fetch_and_add(&g_prof_next12, 2);
    if (k + 1 >= 4096) return -1;
    char *label = calloc(1, 512);
    if (label) { label[0] = kind; label[1] = ':'; }
    g_prof_label12[k / 2] = label;
    t_prof_label12 = label;
    t_prof_len12 = label ? 2 : 0;
    (void)mtl;
    return k;
}
static void prof_note12(char kind, uint64_t hash) {
    if (!g_prof_armed12 || !t_prof_label12 || t_prof_len12 > 480) return;
    t_prof_len12 += snprintf(t_prof_label12 + t_prof_len12, 512 - t_prof_len12, " %c%08llx", kind, (unsigned long long)(hash >> 32));
}
static id<MTLComputeCommandEncoder> new_compute12(id<MTLCommandBuffer> cb) {
    int k = prof_begin12(cb.device, 'C');
    if (k < 0) return [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
    MTLComputePassDescriptor *pd = [MTLComputePassDescriptor computePassDescriptor];
    pd.dispatchType = MTLDispatchTypeSerial;
    pd.sampleBufferAttachments[0].sampleBuffer = g_prof_buf12;
    pd.sampleBufferAttachments[0].startOfEncoderSampleIndex = (NSUInteger)k;
    pd.sampleBufferAttachments[0].endOfEncoderSampleIndex = (NSUInteger)k + 1;
    return [cb computeCommandEncoderWithDescriptor:pd];
}
static void prof_render_pass12(MTLRenderPassDescriptor *descriptor, id<MTLDevice> mtl) {
    int k = prof_begin12(mtl, 'R');
    if (k < 0) return;
    descriptor.sampleBufferAttachments[0].sampleBuffer = g_prof_buf12;
    descriptor.sampleBufferAttachments[0].startOfVertexSampleIndex = (NSUInteger)k;
    descriptor.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
    descriptor.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
    descriptor.sampleBufferAttachments[0].endOfFragmentSampleIndex = (NSUInteger)k + 1;
}
static void prof_present12(id<MTLDevice> mtl) {
    const char *dir = gpu_profile_dir12();
    if (!dir) return;
    if (g_prof_armed12) {
        g_prof_armed12 = 0;
        int n = g_prof_next12, frame = g_prof_frame12;
        id<MTLCounterSampleBuffer> buf = g_prof_buf12;
        MTLTimestamp c0, g0, c1, g1;
        [mtl sampleTimestamps:&c0 gpuTimestamp:&g0];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            MTLTimestamp cc1, gg1;
            [mtl sampleTimestamps:&cc1 gpuTimestamp:&gg1];
            double ns_per_tick = gg1 > g0 ? (double)(cc1 - c0) / (double)(gg1 - g0) : 1.0;
            NSData *data = [buf resolveCounterRange:NSMakeRange(0, (NSUInteger)n)];
            const MTLCounterResultTimestamp *t = data.bytes;
            struct { double ms, start; int k; } rows[2048];
            int m = 0;
            double total = 0;
            uint64_t first = UINT64_MAX;
            for (int k = 0; k + 1 < n && data; k += 2)
                if (t[k].timestamp && t[k].timestamp != MTLCounterErrorValue && t[k].timestamp < first) first = t[k].timestamp;
            for (int k = 0; k + 1 < n && data && m < 2048; k += 2) {
                uint64_t a = t[k].timestamp, b = t[k + 1].timestamp;
                if (!a || !b || a == MTLCounterErrorValue || b == MTLCounterErrorValue || b < a) continue;
                rows[m].ms = (double)(b - a) * ns_per_tick / 1e6; rows[m].start = (double)(a - first) * ns_per_tick / 1e6;
                rows[m].k = k / 2; total += rows[m].ms; m++;
            }
            // (the timeline, in start order: a file next to the trigger)
            char tl[1024];
            snprintf(tl, sizeof tl, "%s/timeline-%d.txt", gpu_profile_dir12(), frame);
            FILE *tf = fopen(tl, "w");
            if (tf) {
                for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++) if (rows[j].start < rows[i].start) { __typeof__(rows[0]) x = rows[i]; rows[i] = rows[j]; rows[j] = x; }
                for (int i = 0; i < m; i++) fprintf(tf, "%9.3f %8.3f %s\n", rows[i].start, rows[i].ms, g_prof_label12[rows[i].k] ? g_prof_label12[rows[i].k] : "?");
                fclose(tf);
            }
            for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++) if (rows[j].ms > rows[i].ms) { __typeof__(rows[0]) x = rows[i]; rows[i] = rows[j]; rows[j] = x; }
            vx_log("D3D12: GPU profile %d: %d encoders timed, %.1f ms in all (encoders may overlap)", frame, m, total);
            for (int i = 0; i < m && i < 40; i++)
                vx_log("D3D12: GPU profile %d: %6.2f ms %s", frame, rows[i].ms, g_prof_label12[rows[i].k] ? g_prof_label12[rows[i].k] : "?");
            for (int i = 0; i < 2048; i++) { free(g_prof_label12[i]); g_prof_label12[i] = NULL; }
        });
        (void)c1; (void)g1;
        return;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/prof", dir);
    if (access(path, F_OK)) return;
    unlink(path);
    if (!g_prof_buf12) {
        id<MTLCounterSet> ts = nil;
        for (id<MTLCounterSet> cs in mtl.counterSets) if ([cs.name isEqualToString:MTLCommonCounterSetTimestamp]) ts = cs;
        if (!ts || ![mtl supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) { vx_log("D3D12: no GPU timestamps at stage boundaries"); return; }
        MTLCounterSampleBufferDescriptor *sd = [MTLCounterSampleBufferDescriptor new];
        sd.counterSet = ts; sd.storageMode = MTLStorageModeShared; sd.sampleCount = 4096;
        NSError *err = nil;
        g_prof_buf12 = [mtl newCounterSampleBufferWithDescriptor:sd error:&err];
        if (!g_prof_buf12) { vx_log("D3D12: GPU profile: %s", err.localizedDescription.UTF8String); return; }
    }
    g_prof_next12 = 0;
    g_prof_frame12++;
    g_prof_armed12 = 1;
    vx_log("D3D12: GPU profile %d: timing the next frame's encoders", g_prof_frame12);
}
static void close_pass(dev12 *d, gfx_state *g, id<MTLCommandBuffer> cb, id<MTLRenderCommandEncoder> __strong *re) {
    if (!*re) return;
    [*re endEncoding];
    *re = nil;
    pass_dump_closed12(cb);
    g->changed = 1;
    if (g->pass_heap && g->npass) {
        id<MTLComputePipelineState> p = occ_sum_pipeline(d);
        if (p) {
            id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:p];
            [e setBuffer:g->pass_heap->results offset:0 atIndex:0];
            [e setBuffer:g->pass_vis offset:0 atIndex:1];
            [e setBytes:g->pass_q length:4 * g->npass atIndex:2];
            [e dispatchThreads:MTLSizeMake(g->npass, 1, 1) threadsPerThreadgroup:MTLSizeMake(g->npass, 1, 1)];
            [e endEncoding];
        }
    }
    g->pass_heap = NULL;
    g->pass_vis = nil;
    g->npass = 0;
}

static void end_encoders(id<MTLComputeCommandEncoder> __strong *ce, id<MTLBlitCommandEncoder> __strong *be, id<MTLRenderCommandEncoder> __strong *re) {
    if (*ce) { [*ce endEncoding]; *ce = nil; }
    if (*be) { [*be endEncoding]; *be = nil; }
    if (*re) { [*re endEncoding]; *re = nil; }
}

static int gpu_errors12(void) {
    static int on = -1;
    if (on < 0) on = getenv("VXRT_D3D12_GPU_ERRORS") != NULL;
    return on;
}
static void make_resident_render(id<MTLRenderCommandEncoder> re, NSArray *resident, heap12 *heap) {
    (void)heap;
    if (resident != resident_by_set12()) use_list_render12(re, resident);   // (else the residency set has them)
    t_re_serial12++;
}
static void heap_textures_render12(id<MTLRenderCommandEncoder> re, heap12 *heap, int wide) {
    if (!re || !wide || t_re_heap_serial12 == t_re_serial12 || no_heap_use12()) return;
    use_list_render12(re, heap_textures12(heap));                  // (as for compute)
    t_re_heap_serial12 = t_re_serial12;
}

static int open_render_pass12(dev12 *device, gfx_state *g, id<MTLCommandBuffer> command_buffer,
                              id<MTLComputeCommandEncoder> __strong *compute_encoder,
                              id<MTLBlitCommandEncoder> __strong *blit_encoder,
                              id<MTLRenderCommandEncoder> __strong *render_encoder,
                              NSArray *resident, heap12 *resource_heap) {
    if (*render_encoder && !g->changed) return 1;
    close_pass(device, g, command_buffer, render_encoder);
    end_encoders(compute_encoder, blit_encoder, render_encoder);
    MTLRenderPassDescriptor *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    uint32_t width = 0, height = 0;
    for (int k = 0; k < 8; k++) {
        if (!g->tex[k]) continue;
        id<MTLTexture> texture = (__bridge id<MTLTexture>)g->tex[k];
        descriptor.colorAttachments[k].texture = texture;
        descriptor.colorAttachments[k].loadAction = MTLLoadActionLoad;
        descriptor.colorAttachments[k].storeAction = MTLStoreActionStore;
        width = (uint32_t)texture.width;
        height = (uint32_t)texture.height;
    }
    if (g->tex[8]) {
        id<MTLTexture> texture = (__bridge id<MTLTexture>)g->tex[8];
        descriptor.depthAttachment.texture = texture;
        descriptor.depthAttachment.loadAction = MTLLoadActionLoad;
        descriptor.depthAttachment.storeAction = MTLStoreActionStore;
        if (texture.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
            descriptor.stencilAttachment.texture = texture;
            descriptor.stencilAttachment.loadAction = MTLLoadActionLoad;
            descriptor.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (!width) { width = (uint32_t)texture.width; height = (uint32_t)texture.height; }
    }
    if (!width && g->nvp) {
        // (from the viewport: kept within Metal's 16384 and at least 1)
        float rw = g->vp[0].x + g->vp[0].w, rh = g->vp[0].y + g->vp[0].h;
        descriptor.renderTargetWidth = (NSUInteger)(rw >= 1 ? (rw <= 16384 ? rw : 16384) : 1);
        descriptor.renderTargetHeight = (NSUInteger)(rh >= 1 ? (rh <= 16384 ? rh : 16384) : 1);
        descriptor.defaultRasterSampleCount = 1;
    }
    if (g->occ) {
        g->pass_heap = g->occ;
        g->pass_vis = [device->mtl newBufferWithLength:g->occ->results.length options:MTLResourceStorageModeShared];
        if (!g->pass_vis) { g->pass_heap = NULL; return 0; }
        memset(g->pass_vis.contents, 0, g->pass_vis.length);
        descriptor.visibilityResultBuffer = g->pass_vis;
    }
    prof_render_pass12(descriptor, device->mtl);
    *render_encoder = [command_buffer renderCommandEncoderWithDescriptor:descriptor];
    if (!*render_encoder) { g->pass_heap = NULL; g->pass_vis = nil; return 0; }
    pass_dump_opened12(descriptor);
    if (gpu_errors12()) {                                          // (named for VXRT_D3D12_GPU_ERRORS reports)
        NSMutableString *name = [NSMutableString stringWithFormat:@"pass %lux%lu samples %lu:", (unsigned long)descriptor.renderTargetWidth,
                                 (unsigned long)descriptor.renderTargetHeight, (unsigned long)descriptor.defaultRasterSampleCount];
        for (int k = 0; k < 8; k++) {
            id<MTLTexture> t = descriptor.colorAttachments[k].texture;
            if (t) [name appendFormat:@" c%d %lux%lu fmt %lu x%lu", k, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long)t.sampleCount];
        }
        id<MTLTexture> dt = descriptor.depthAttachment.texture;
        if (dt) [name appendFormat:@" depth %lux%lu fmt %lu", (unsigned long)dt.width, (unsigned long)dt.height, (unsigned long)dt.pixelFormat];
        (*render_encoder).label = name;
    }
    make_resident_render(*render_encoder, resident, resource_heap);
    if (g->occ_on) { [*render_encoder setVisibilityResultMode:MTLVisibilityResultModeCounting offset:8 * g->occ_index]; pass_query(g); }
    g->changed = 0;
    g->sc[15].right = (int32_t)(width ? width : descriptor.renderTargetWidth);
    g->sc[15].bottom = (int32_t)(height ? height : descriptor.renderTargetHeight);
    return 1;
}

typedef struct {
    dev12 *device;
    void *command_buffer, *compute_encoder, *blit_encoder, *render_encoder, *resident_resources;
    gfx_state *graphics_state;
    pso12 *api_pipeline;
    heap12 *resource_heap, *sampler_heap;
    root_binding *compute_root;
    scratch *scratch_buffer;
} d3d12_dispatch_context;

static MTLResourceUsage d3d12_metal_usage12(uint32_t access) {
    const uint32_t reads = GPU_IR_ACCESS_VERTEX_READ | GPU_IR_ACCESS_INDEX_READ | GPU_IR_ACCESS_UNIFORM_READ |
        GPU_IR_ACCESS_SHADER_READ | GPU_IR_ACCESS_DEPTH_READ | GPU_IR_ACCESS_INDIRECT_READ |
        GPU_IR_ACCESS_COPY_READ | GPU_IR_ACCESS_RESOLVE_READ | GPU_IR_ACCESS_ACCELERATION_STRUCTURE;
    const uint32_t writes = GPU_IR_ACCESS_SHADER_WRITE | GPU_IR_ACCESS_COLOR_WRITE | GPU_IR_ACCESS_DEPTH_WRITE |
        GPU_IR_ACCESS_STREAM_WRITE | GPU_IR_ACCESS_COPY_WRITE | GPU_IR_ACCESS_RESOLVE_WRITE |
        GPU_IR_ACCESS_ACCELERATION_STRUCTURE;
    MTLResourceUsage usage = 0;
    if (access & reads) usage |= MTLResourceUsageRead;
    if (access & writes) usage |= MTLResourceUsageWrite;
    return usage ? usage : MTLResourceUsageRead;
}

static MTLRenderStages d3d12_metal_stages12(uint32_t stage_mask) {
    MTLRenderStages stages = 0;
    if (stage_mask & GPU_IR_STAGE_VERTEX) stages |= MTLRenderStageVertex;
    if (stage_mask & GPU_IR_STAGE_FRAGMENT) stages |= MTLRenderStageFragment;
    return stages ? stages : MTLRenderStageVertex | MTLRenderStageFragment;
}

// What an encoder has been told it uses (useResource holds for the rest of the encoder): each thread remembers it for
// its current encoder of each kind, so the draws after the first that bind the same resources declare nothing new.
typedef struct { void *resource; uint64_t epoch; uint32_t usage, stages; } use_seen12;
static __thread use_seen12 t_use_seen12[2][2048];
static __thread uint64_t t_use_epoch12[2];
static __thread void *t_use_encoder12[2];
static __thread uint64_t t_use_serial12[2];
static int use_needed12(int kind, void *encoder, uint64_t serial, void *resource, uint32_t *usage, uint32_t *stages) {
    static int off = -1;
    if (off < 0) off = getenv("VXRT_D3D12_USE_DEDUP") && *getenv("VXRT_D3D12_USE_DEDUP") == '0';
    if (off) return 1;
    if (t_use_encoder12[kind] != encoder || t_use_serial12[kind] != serial) {
        t_use_encoder12[kind] = encoder;
        t_use_serial12[kind] = serial;
        t_use_epoch12[kind]++;
    }
    uint64_t epoch = t_use_epoch12[kind];
    uint32_t h = (uint32_t)(((uintptr_t)resource >> 4) * 2654435761u) & 2047;
    for (int probe = 0; probe < 8; probe++) {
        use_seen12 *e = &t_use_seen12[kind][(h + probe) & 2047];
        if (e->epoch != epoch) { *e = (use_seen12){ resource, epoch, *usage, *stages }; return 1; }
        if (e->resource != resource) continue;
        if ((e->usage | *usage) == e->usage && (e->stages | *stages) == e->stages) return 0;
        e->usage |= *usage; e->stages |= *stages;
        *usage = e->usage; *stages = e->stages;
        return 1;
    }
    return 1;                                                      // (table crowded: declare it again)
}
static int d3d12_use_compute_accesses12(id<MTLComputeCommandEncoder> encoder,
                                       const gpu_ir_resource_access *accesses, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        id<MTLResource> resource = (__bridge id<MTLResource>)gpu_ir_backend_object(accesses[i].resource);
        if (!resource) return GPU_IR_INVALID;
        uint32_t usage = (uint32_t)d3d12_metal_usage12(accesses[i].access_mask), stages = 0;
        if (!use_needed12(0, (__bridge void *)encoder, t_ce_serial12, (__bridge void *)resource, &usage, &stages)) continue;
        [encoder useResource:resource usage:(MTLResourceUsage)usage];
    }
    return GPU_IR_OK;
}

static int d3d12_common_binding_is_texture12(const gpu_ir_shader *shader,
                                             const gpu_ir_descriptor_binding *binding) {
    if (!shader || !binding) return 0;
    for (uint32_t i = 0; i < gpu_ir_shader_instruction_count(shader); i++) {
        const gpu_ir_shader_instruction *variable = gpu_ir_shader_instruction_at(shader, i);
        if (!variable || variable->opcode != GPU_IR_SHADER_OP_VARIABLE ||
            variable->function_index != UINT32_MAX ||
            variable->storage_class != GPU_IR_SHADER_STORAGE_UNIFORM_CONSTANT ||
            variable->resource_set != binding->set || variable->binding_namespace != binding->binding_namespace ||
            variable->binding != binding->binding) continue;
        const gpu_ir_shader_type *pointer = gpu_ir_shader_find_type(shader, variable->result_type_id);
        const gpu_ir_shader_type *image = pointer && pointer->kind == GPU_IR_SHADER_TYPE_POINTER ?
            gpu_ir_shader_find_type(shader, pointer->element_type_id) : NULL;
        if (image && image->kind == GPU_IR_SHADER_TYPE_IMAGE) return 1;
    }
    return 0;
}

static int d3d12_bind_common_compute_descriptors12(id<MTLComputeCommandEncoder> encoder,
                                                   const gpu_ir_pipeline *pipeline,
                                                   const gpu_ir_descriptor_state *state) {
    const gpu_ir_pipeline_layout *layout = gpu_ir_pipeline_layout_of(pipeline);
    if (!layout) return GPU_IR_OK;
    const gpu_ir_shader *shader = gpu_ir_pipeline_shader(pipeline, GPU_IR_STAGE_COMPUTE);
    uint64_t buffer_ranges[GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS] = { 0 };
    uint32_t metal_slot = 0, texture_slot = 0;
    for (uint32_t i = 0; i < gpu_ir_pipeline_layout_binding_count(layout); i++) {
        gpu_ir_descriptor_binding binding;
        if (gpu_ir_pipeline_layout_binding_at(layout, i, &binding) != GPU_IR_OK) return GPU_IR_INVALID;
        if (!(binding.stage_mask & GPU_IR_STAGE_COMPUTE)) continue;
        if (d3d12_common_binding_is_texture12(shader, &binding)) {
            if (binding.binding_namespace != 0 || binding.kind != GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE ||
                !binding.count || binding.count == GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED ||
                binding.count > GPU_IR_METAL_MAX_COMPUTE_TEXTURES - texture_slot)
                return GPU_IR_UNSUPPORTED;
            for (uint32_t array_element = 0; array_element < binding.count; array_element++) {
                gpu_ir_descriptor_value value = { 0 };
                int result = state ? gpu_ir_descriptor_state_find_for_stage(state, binding.set,
                    binding.binding_namespace, binding.binding, array_element, GPU_IR_STAGE_COMPUTE, &value) :
                    GPU_IR_NOT_FOUND;
                if (result == GPU_IR_NOT_FOUND || (result == GPU_IR_OK &&
                    (value.flags & GPU_IR_DESCRIPTOR_VALUE_NULL))) {
                    [encoder setTexture:nil atIndex:texture_slot++];
                    continue;
                }
                if (result != GPU_IR_OK || !value.resource || gpu_ir_resource_is_buffer(value.resource))
                    return result == GPU_IR_OK ? GPU_IR_INVALID : result;
                id<MTLTexture> texture = value.backend_object ?
                    (__bridge id<MTLTexture>)value.backend_object :
                    (__bridge id<MTLTexture>)gpu_ir_backend_object(value.resource);
                if (!texture) return GPU_IR_INVALID;
                if (value.component_mapping) {
                    void *swizzled_view = gpu_ir_metal_texture_view_with_component_mapping(
                        (__bridge void *)texture, value.component_mapping);
                    if (!swizzled_view) return GPU_IR_UNSUPPORTED;
                    texture = (__bridge_transfer id<MTLTexture>)swizzled_view;
                }
                [encoder setTexture:texture atIndex:texture_slot];
                [encoder useResource:texture usage:d3d12_metal_usage12(value.access_mask)];
                texture_slot++;
            }
            continue;
        }
        if (!binding.count || binding.count == GPU_IR_DESCRIPTOR_COUNT_UNBOUNDED ||
            binding.count > GPU_IR_METAL_MAX_COMPUTE_STORAGE_BUFFERS - metal_slot ||
            !((binding.binding_namespace == 0 &&
               (binding.kind == GPU_IR_DESCRIPTOR_STORAGE_BUFFER ||
                binding.kind == GPU_IR_DESCRIPTOR_READ_ONLY_RESOURCE)) ||
              (binding.binding_namespace == 1 && binding.kind == GPU_IR_DESCRIPTOR_STORAGE_RESOURCE)))
            return GPU_IR_UNSUPPORTED;
        for (uint32_t array_element = 0; array_element < binding.count; array_element++) {
            gpu_ir_descriptor_value value = { 0 };
            int result = state ? gpu_ir_descriptor_state_find_for_stage(state, binding.set,
                binding.binding_namespace, binding.binding, array_element, GPU_IR_STAGE_COMPUTE, &value) :
                GPU_IR_NOT_FOUND;
            if (result == GPU_IR_NOT_FOUND || (result == GPU_IR_OK &&
                (value.flags & GPU_IR_DESCRIPTOR_VALUE_NULL))) {
                [encoder setBuffer:nil offset:0 atIndex:metal_slot++];
                continue;
            }
            if (result != GPU_IR_OK || !value.resource || !gpu_ir_resource_is_buffer(value.resource))
                return result == GPU_IR_OK ? GPU_IR_INVALID : result;

            id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(value.resource);
            if (!buffer) return GPU_IR_INVALID;
            uint64_t offset = value.offset;
            uint64_t range = value.range;
            if (value.buffer_view) {
                const gpu_ir_buffer_view_desc *view = gpu_ir_buffer_view_description(value.buffer_view);
                if (!view || offset || range) return GPU_IR_INVALID;
                offset = view->byte_offset;
                range = view->byte_size;
            }
            uint64_t buffer_size = gpu_ir_buffer_size(value.resource);
            if (offset > NSUIntegerMax || offset > buffer.length || offset > buffer_size) return GPU_IR_RANGE;
            if (!range) range = MIN((uint64_t)buffer.length - offset, buffer_size - offset);
            if (range > (uint64_t)buffer.length - offset || range > buffer_size - offset) return GPU_IR_RANGE;

            buffer_ranges[metal_slot] = range;
            [encoder setBuffer:buffer offset:(NSUInteger)offset atIndex:metal_slot];
            [encoder useResource:buffer usage:d3d12_metal_usage12(value.access_mask)];
            metal_slot++;
        }
    }
    if (metal_slot)
        [encoder setBytes:buffer_ranges length:sizeof buffer_ranges atIndex:GPU_IR_METAL_BUFFER_RANGE_SLOT];
    return GPU_IR_OK;
}

static int d3d12_clear_uav_format_kind12(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatR16Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatR32Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRGBA32Uint:
        return 2;
    case MTLPixelFormatR8Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatR16Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatR32Sint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA32Sint:
        return 3;
    case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Snorm: case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatRG8Snorm: case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Snorm:
    case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatR16Unorm: case MTLPixelFormatR16Snorm:
    case MTLPixelFormatR16Float: case MTLPixelFormatRG16Unorm: case MTLPixelFormatRG16Snorm:
    case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA16Unorm: case MTLPixelFormatRGBA16Snorm:
    case MTLPixelFormatRGBA16Float: case MTLPixelFormatR32Float: case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA32Float: case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatRG11B10Float:
        return 1;
    default:
        return 0;
    }
}

static uint32_t d3d12_clear_uav_component_bits12(MTLPixelFormat format) {
    switch (format) {
    case MTLPixelFormatR8Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatR8Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRGBA8Sint:
        return 8;
    case MTLPixelFormatR16Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatR16Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA16Sint:
        return 16;
    case MTLPixelFormatR32Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatR32Sint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA32Sint:
        return 32;
    default:
        return 0;
    }
}

static id<MTLComputePipelineState> d3d12_clear_uav_pipeline12(dev12 *device, MTLTextureType texture_type,
                                                               int format_kind, uint32_t component_bits) {
    NSString *metal_texture = nil, *bounds = nil, *coordinate = nil;
    switch (texture_type) {
    case MTLTextureTypeTextureBuffer:
        metal_texture = @"texture_buffer"; bounds = @"p.x < dst.get_width()"; coordinate = @"p.x"; break;
    case MTLTextureType1D:
        metal_texture = @"texture1d"; bounds = @"p.x < dst.get_width()"; coordinate = @"p.x"; break;
    case MTLTextureType1DArray:
        metal_texture = @"texture1d_array";
        bounds = @"p.x < dst.get_width() && p.y < dst.get_array_size()";
        coordinate = @"p.x, p.y";
        break;
    case MTLTextureType2D:
        metal_texture = @"texture2d";
        bounds = @"p.x < dst.get_width() && p.y < dst.get_height()";
        coordinate = @"p.xy";
        break;
    case MTLTextureType2DArray:
        metal_texture = @"texture2d_array";
        bounds = @"p.x < dst.get_width() && p.y < dst.get_height() && p.z < dst.get_array_size()";
        coordinate = @"p.xy, p.z";
        break;
    case MTLTextureType3D:
        metal_texture = @"texture3d";
        bounds = @"p.x < dst.get_width() && p.y < dst.get_height() && p.z < dst.get_depth()";
        coordinate = @"p";
        break;
    default:
        return nil;
    }
    NSString *component = format_kind == 1 ? @"float" : format_kind == 3 ? @"int" : @"uint";
    NSString *vector = format_kind == 1 ? @"float4" : format_kind == 3 ? @"int4" : @"uint4";
    NSString *value_conversion = @"";
    NSString *write_value = @"value";
    if (format_kind != 1 && component_bits < 32) {
        NSString *mask = component_bits == 8 ? @"0xffu" : @"0xffffu";
        if (format_kind == 2) {
            value_conversion = [NSString stringWithFormat:
                @"uint4 stored_value = value & uint4(%@); ", mask];
        } else {
            NSString *sign = component_bits == 8 ? @"0x80u" : @"0x8000u";
            value_conversion = [NSString stringWithFormat:
                @"uint4 value_bits = as_type<uint4>(value) & uint4(%@); "
                @"int4 stored_value = as_type<int4>((value_bits ^ uint4(%@)) - uint4(%@)); ",
                mask, sign, sign];
        }
        write_value = @"stored_value";
    }
    NSString *source = [NSString stringWithFormat:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        @"kernel void vx_clear(%@<%@, access::write> dst [[texture(0)]], "
        @"constant %@& value [[buffer(0)]], constant uint3& origin [[buffer(1)]], "
        @"uint3 tid [[thread_position_in_grid]]) { uint3 p = tid + origin; "
        @"if (%@) { %@dst.write(%@, %@); } }\n",
         metal_texture, component, vector, bounds, value_conversion, write_value, coordinate];
    pthread_mutex_lock(&device->lock);
    id<MTLComputePipelineState> cached = device->pso_cache[source];
    pthread_mutex_unlock(&device->lock);
    if (cached) return cached;

    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_1;
    NSError *error = nil;
    id<MTLLibrary> library = [device->mtl newLibraryWithSource:source options:options error:&error];
    id<MTLComputePipelineState> pipeline = library ?
        [device->mtl newComputePipelineStateWithFunction:[library newFunctionWithName:@"vx_clear"] error:&error] : nil;
    if (!pipeline) {
        vx_log("D3D12: UAV clear shader did not compile: %s\n%s", error.localizedDescription.UTF8String,
               source.UTF8String);
        return nil;
    }
    pthread_mutex_lock(&device->lock);
    cached = device->pso_cache[source];
    if (!cached) device->pso_cache[source] = pipeline;
    else pipeline = cached;
    pthread_mutex_unlock(&device->lock);
    return pipeline;
}

static int d3d12_clear_uav_encode12(dev12 *device, id<MTLCommandBuffer> command_buffer,
                                   id<MTLComputeCommandEncoder> __strong *encoder,
                                   const cmd12 *command, heap12 *resource_heap) {
    uint32_t descriptor_index = 0;
    heap12 *descriptor_heap = heap_gpu_descriptor(command->va, &descriptor_index);
    if (!descriptor_heap || descriptor_heap != resource_heap || descriptor_index >= descriptor_heap->desc.NumDescriptors ||
        !descriptor_heap->keep || ![descriptor_heap->keep[descriptor_index] conformsToProtocol:@protocol(MTLTexture)])
        return 0;
    id<MTLTexture> texture = descriptor_heap->keep[descriptor_index];
    int format_kind = d3d12_clear_uav_format_kind12(texture.pixelFormat);
    if (command->a ? format_kind != 1 : (format_kind != 2 && format_kind != 3)) return 0;
    uint32_t component_bits = format_kind == 1 ? 32 : d3d12_clear_uav_component_bits12(texture.pixelFormat);
    if (!component_bits) return 0;
    id<MTLComputePipelineState> pipeline = d3d12_clear_uav_pipeline12(device, texture.textureType,
                                                                      format_kind, component_bits);
    if (!pipeline) return 0;

    MTLSize full_size;
    NSUInteger texture_buffer_first = 0;
    switch (texture.textureType) {
    case MTLTextureTypeTextureBuffer: {
        VxDesc descriptor;
        memcpy(&descriptor, descriptor_heap->cpu + 32ull * descriptor_index, sizeof descriptor);
        if (!descriptor.size || descriptor.va > texture.width || descriptor.size > texture.width - descriptor.va)
            return 0;
        texture_buffer_first = (NSUInteger)descriptor.va;
        full_size = MTLSizeMake(descriptor.size, 1, 1);
        break;
    }
    case MTLTextureType1D:
        full_size = MTLSizeMake(texture.width, 1, 1); break;
    case MTLTextureType1DArray:
        full_size = MTLSizeMake(texture.width, texture.arrayLength, 1); break;
    case MTLTextureType2D:
        full_size = MTLSizeMake(texture.width, texture.height, 1); break;
    case MTLTextureType2DArray:
        full_size = MTLSizeMake(texture.width, texture.height, texture.arrayLength); break;
    case MTLTextureType3D:
        full_size = MTLSizeMake(texture.width, texture.height, texture.depth); break;
    default:
        return 0;
    }
    int rect_supported = texture.textureType != MTLTextureTypeTextureBuffer;
    if (command->b && !rect_supported) return 0;
    [*encoder setComputePipelineState:pipeline];
    [*encoder setTexture:texture atIndex:0];
    [*encoder setBytes:command->values length:4 * sizeof(uint32_t) atIndex:0];
    uint32_t threadgroup_origin[4] = { 0, 0, 0, 0 };
    NSUInteger width = MAX((NSUInteger)1, MIN((NSUInteger)pipeline.threadExecutionWidth, full_size.width));
    MTLSize threads_per_group = MTLSizeMake(width, 1, 1);
    uint32_t rectangle_count = command->b ? command->b : 1;
    for (uint32_t i = 0; i < rectangle_count; i++) {
        MTLSize region = full_size;
        memset(threadgroup_origin, 0, sizeof threadgroup_origin);
        if (command->b) {
            const RECT12 *rect = &command->rects[i];
            int64_t left = MAX((int64_t)0, (int64_t)rect->left);
            int64_t top = MAX((int64_t)0, (int64_t)rect->top);
            int64_t right = MIN((int64_t)full_size.width, (int64_t)rect->right);
            int64_t bottom = MIN((int64_t)full_size.height, (int64_t)rect->bottom);
            if (right <= left || bottom <= top) continue;
            threadgroup_origin[0] = (uint32_t)left;
            threadgroup_origin[1] = (uint32_t)top;
            region.width = (NSUInteger)(right - left);
            region.height = (NSUInteger)(bottom - top);
        } else if (texture.textureType == MTLTextureTypeTextureBuffer)
            threadgroup_origin[0] = (uint32_t)texture_buffer_first;
        [*encoder setBytes:&threadgroup_origin length:sizeof threadgroup_origin atIndex:1];
        [*encoder dispatchThreads:region threadsPerThreadgroup:threads_per_group];
    }
    [*encoder useResource:texture usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    if (command->resource[0] && command->resource[0]->texture &&
        command->resource[0]->texture != texture)
        [*encoder useResource:command->resource[0]->texture usage:MTLResourceUsageRead | MTLResourceUsageWrite];
    (void)command_buffer;
    return 1;
}

static int d3d12_use_render_accesses12(id<MTLRenderCommandEncoder> encoder,
                                      const gpu_ir_resource_access *accesses, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        id<MTLResource> resource = (__bridge id<MTLResource>)gpu_ir_backend_object(accesses[i].resource);
        if (!resource) return GPU_IR_INVALID;
        uint32_t usage = (uint32_t)d3d12_metal_usage12(accesses[i].access_mask);
        uint32_t stages = (uint32_t)d3d12_metal_stages12(accesses[i].stage_mask);
        if (!use_needed12(1, (__bridge void *)encoder, t_re_serial12, (__bridge void *)resource, &usage, &stages)) continue;
        [encoder useResource:resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages];
    }
    return GPU_IR_OK;
}

static int d3d12_gpu_ir_dispatch(void *opaque, const gpu_ir_pipeline *pipeline,
                                 uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z,
                                 const gpu_ir_resource_access *accesses, uint32_t access_count) {
    d3d12_dispatch_context *context = opaque;
    pso12 *api_pipeline = context->api_pipeline;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!api_pipeline || !api_pipeline->compute || api_pipeline->gpu_pipeline != pipeline || !info ||
        info->kind != GPU_IR_PIPELINE_COMPUTE) return GPU_IR_INVALID;

    dev12 *device = context->device;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)context->command_buffer;
    id<MTLComputeCommandEncoder> __strong *ce = (id<MTLComputeCommandEncoder> __strong *)context->compute_encoder;
    id<MTLBlitCommandEncoder> __strong *be = (id<MTLBlitCommandEncoder> __strong *)context->blit_encoder;
    id<MTLRenderCommandEncoder> __strong *re = (id<MTLRenderCommandEncoder> __strong *)context->render_encoder;
    NSArray *resident = (__bridge NSArray *)context->resident_resources;
    gfx_state *g = context->graphics_state;

    if (*be) { [*be endEncoding]; *be = nil; }
    close_pass(device, g, cb, re);
    if (!*ce) {
        *ce = new_compute12(cb);
        // Descriptor-table resources are accessed indirectly, so keep the complete visible set resident.
        use_resident_compute12(*ce, resident, context->resource_heap);
    }
    heap_textures_compute12(*ce, context->resource_heap, msl_wide12(&context->api_pipeline->msl));

    int access_result = d3d12_use_compute_accesses12(*ce, accesses, access_count);
    if (access_result != GPU_IR_OK) return access_result;
    int use_common_pipeline = api_pipeline->common_ir_compute;
    gpu_ir_descriptor_state *bindings = NULL;
    int bind_result = GPU_IR_OK;
    if (use_common_pipeline && api_pipeline->msl.nslots) {
        pthread_mutex_lock(&device->lock);
        bind_result = gpu_ir_snapshot_root_bindings12(device, &api_pipeline->msl,
            GPU_IR_STAGE_COMPUTE, context->compute_root, context->resource_heap,
            context->sampler_heap, &bindings);
        pthread_mutex_unlock(&device->lock);
    }
    if (bind_result == GPU_IR_OK && use_common_pipeline)
        bind_result = d3d12_bind_common_compute_descriptors12(*ce, pipeline, bindings);
    gpu_ir_descriptor_state_release(bindings);
    if (bind_result != GPU_IR_OK) return bind_result;
    if (use_common_pipeline) {
        id<MTLComputePipelineState> metal_pipeline =
            (__bridge id<MTLComputePipelineState>)gpu_ir_pipeline_backend_object(pipeline);
        if (!metal_pipeline) return GPU_IR_INVALID;
        [*ce setComputePipelineState:metal_pipeline];
    } else {
        [*ce setComputePipelineState:api_pipeline->compute];
        uint64_t root[64 + 2];
        uint32_t words;
        stage_root(device, &api_pipeline->msl, context->compute_root, context->resource_heap,
                   context->sampler_heap, context->scratch_buffer, root, &words);
        [*ce setBytes:root length:8 * words atIndex:0];
        if (context->scratch_buffer->buf) [*ce useResource:context->scratch_buffer->buf usage:MTLResourceUsageRead];
    }
    [*ce dispatchThreadgroups:MTLSizeMake(group_count_x, group_count_y, group_count_z)
        threadsPerThreadgroup:MTLSizeMake(info->threadgroup_size[0], info->threadgroup_size[1], info->threadgroup_size[2])];
    // D3D12 UAV ordering is retained here until common resource-access records can express shader hazards.
    if (!g_no_dispatch_barrier12) [*ce memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
    return GPU_IR_OK;
}

static int d3d12_gpu_ir_dispatch_indirect(void *opaque, const gpu_ir_pipeline *pipeline,
                                         const gpu_ir_resource *arguments, uint64_t argument_offset,
                                         const gpu_ir_resource_access *accesses, uint32_t access_count) {
    d3d12_dispatch_context *context = opaque;
    pso12 *api_pipeline = context->api_pipeline;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!api_pipeline || !api_pipeline->compute || api_pipeline->gpu_pipeline != pipeline || !info ||
        info->kind != GPU_IR_PIPELINE_COMPUTE || !gpu_ir_resource_is_buffer(arguments) ||
        (argument_offset & 3) || argument_offset > gpu_ir_buffer_size(arguments) ||
        12 > gpu_ir_buffer_size(arguments) - argument_offset) return GPU_IR_INVALID;
    int mapping_result = d3d12_shader_component_mapping_read12(context->device, &api_pipeline->msl,
        GPU_IR_STAGE_COMPUTE, context->compute_root, context->resource_heap, context->sampler_heap);
    if (mapping_result != GPU_IR_OK) return mapping_result;

    dev12 *device = context->device;
    id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)context->command_buffer;
    id<MTLComputeCommandEncoder> __strong *ce = (id<MTLComputeCommandEncoder> __strong *)context->compute_encoder;
    id<MTLBlitCommandEncoder> __strong *be = (id<MTLBlitCommandEncoder> __strong *)context->blit_encoder;
    id<MTLRenderCommandEncoder> __strong *re = (id<MTLRenderCommandEncoder> __strong *)context->render_encoder;
    NSArray *resident = (__bridge NSArray *)context->resident_resources;
    gfx_state *g = context->graphics_state;
    id<MTLBuffer> argument_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(arguments);
    id<MTLComputePipelineState> metal_pipeline = api_pipeline->compute;
    if (!argument_buffer || !metal_pipeline || argument_offset > argument_buffer.length ||
        12 > argument_buffer.length - argument_offset) return GPU_IR_INVALID;

    id<MTLBuffer> traced_args = nil;
    if (getenv("VXRT_D3D12_TRACE_INDIRECT")) {                    // (the arguments as the dispatch reads them)
        if (*ce) { [*ce endEncoding]; *ce = nil; }
        close_pass(device, g, cb, re);
        if (!*be) *be = [cb blitCommandEncoder];
        traced_args = [device->mtl newBufferWithLength:16 options:MTLResourceStorageModeShared];
        [*be copyFromBuffer:argument_buffer sourceOffset:(NSUInteger)argument_offset toBuffer:traced_args destinationOffset:0 size:12];
    }
    if (*be) { [*be endEncoding]; *be = nil; }
    close_pass(device, g, cb, re);
    if (!*ce) {
        *ce = new_compute12(cb);
        // Descriptor-table resources are accessed indirectly, so keep the complete visible set resident.
        use_resident_compute12(*ce, resident, context->resource_heap);
    }
    heap_textures_compute12(*ce, context->resource_heap, msl_wide12(&context->api_pipeline->msl));

    int access_result = d3d12_use_compute_accesses12(*ce, accesses, access_count);
    if (access_result != GPU_IR_OK) return access_result;
    [*ce useResource:argument_buffer usage:MTLResourceUsageRead];
    [*ce setComputePipelineState:metal_pipeline];
    uint64_t root[64 + 2];
    uint32_t words;
    stage_root(device, &api_pipeline->msl, context->compute_root, context->resource_heap,
               context->sampler_heap, context->scratch_buffer, root, &words);
    [*ce setBytes:root length:8 * words atIndex:0];
    if (context->scratch_buffer->buf) [*ce useResource:context->scratch_buffer->buf usage:MTLResourceUsageRead];
    if (getenv("VXRT_D3D12_TRACE_INDIRECT")) {                    // (the sizes the GPU chose, read once it is done)
        id<MTLBuffer> ab = traced_args ? traced_args : argument_buffer;
        uint64_t off = traced_args ? 0 : argument_offset, hash = api_pipeline->cs_code;
        uint32_t tg0 = info->threadgroup_size[0], tg1 = info->threadgroup_size[1], tg2 = info->threadgroup_size[2];
        [(__bridge id<MTLCommandBuffer>)context->command_buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
            (void)done;
            static uint64_t hs[256]; static int cnt[256], nh;
            static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
            const uint32_t *w = ab.contents ? (const uint32_t *)((const uint8_t *)ab.contents + off) : NULL;
            int show = 0;
            pthread_mutex_lock(&m);
            int k = 0;
            while (k < nh && hs[k] != hash) k++;
            if (k == nh && nh < 256) { hs[nh] = hash; cnt[nh] = 0; nh++; }
            if (k < 256 && cnt[k]++ % 50 == 0) show = 1;
            pthread_mutex_unlock(&m);
            if (w && show)
                vx_log("D3D12: indirect dispatch of cs %016llx: %u x %u x %u groups of %u x %u x %u", (unsigned long long)hash, w[0], w[1], w[2], tg0, tg1, tg2);
        }];
    }
    [*ce dispatchThreadgroupsWithIndirectBuffer:argument_buffer indirectBufferOffset:(NSUInteger)argument_offset
                         threadsPerThreadgroup:MTLSizeMake(info->threadgroup_size[0], info->threadgroup_size[1],
                                                            info->threadgroup_size[2])];
    // Keep the same ordering point as a direct dispatch until common resource accesses model UAV hazards.
    if (!g_no_dispatch_barrier12) [*ce memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
    return GPU_IR_OK;
}

typedef struct {
    dev12 *device;
    void *command_buffer;
    void *compute_encoder, *blit_encoder, *resident_resources;
    void *render_encoder;
    gfx_state *graphics_state;
    pso12 *api_pipeline;
    heap12 *resource_heap, *sampler_heap;
    root_binding *graphics_root;
    scratch *scratch_buffer;
} d3d12_draw_context;

static uint32_t gpu_ir_topology_from_metal12(MTLPrimitiveType primitive) {
    switch (primitive) {
    case MTLPrimitiveTypePoint: return GPU_IR_TOPOLOGY_POINT_LIST;
    case MTLPrimitiveTypeLine: return GPU_IR_TOPOLOGY_LINE_LIST;
    case MTLPrimitiveTypeLineStrip: return GPU_IR_TOPOLOGY_LINE_STRIP;
    case MTLPrimitiveTypeTriangle: return GPU_IR_TOPOLOGY_TRIANGLE_LIST;
    case MTLPrimitiveTypeTriangleStrip: return GPU_IR_TOPOLOGY_TRIANGLE_STRIP;
    default: return GPU_IR_TOPOLOGY_TRIANGLE_LIST;
    }
}

// The fixed-function state of a draw: pipeline, depth/stencil, rasterizer, blend color, viewports and scissors.
static void encode_render_state12(id<MTLRenderCommandEncoder> re, pso12 *api_pipeline, gfx_state *g, const float *blend_constants) {
    [re setRenderPipelineState:api_pipeline->render];
    [re setDepthStencilState:api_pipeline->depth];
    [re setCullMode:api_pipeline->cull];
    [re setFrontFacingWinding:api_pipeline->winding];
    [re setTriangleFillMode:api_pipeline->fill];
    [re setDepthClipMode:api_pipeline->clip];
    [re setDepthBias:api_pipeline->bias slopeScale:api_pipeline->bias_slope clamp:api_pipeline->bias_clamp];
    [re setStencilReferenceValue:g->stencil_ref];
    [re setBlendColorRed:blend_constants[0] green:blend_constants[1]
                    blue:blend_constants[2] alpha:blend_constants[3]];
    if (g->nvp) {
        MTLViewport viewports[10];
        for (uint32_t k = 0; k < g->nvp; k++)
            viewports[k] = (MTLViewport){ g->vp[k].x, g->vp[k].y, g->vp[k].w, g->vp[k].h,
                                          g->vp[k].min_depth, g->vp[k].max_depth };
        [re setViewports:viewports count:g->nvp];
    }
    {
        int32_t width = g->sc[15].right, height = g->sc[15].bottom;
        uint32_t count = g->nsc ? (g->nsc > 15 ? 15 : g->nsc) : 0;
        MTLScissorRect scissors[16];
        for (uint32_t k = 0; k < count; k++) {
            int32_t x0 = MAX(0, g->sc[k].left), y0 = MAX(0, g->sc[k].top);
            int32_t x1 = MIN(width, g->sc[k].right), y1 = MIN(height, g->sc[k].bottom);
            scissors[k] = (MTLScissorRect){ (NSUInteger)x0, (NSUInteger)y0,
                (NSUInteger)MAX(0, x1 - x0), (NSUInteger)MAX(0, y1 - y0) };
        }
        if (!count) { scissors[0] = (MTLScissorRect){ 0, 0, 0, 0 }; count = 1; }
        [re setScissorRects:scissors count:count];
    }
}

static int d3d12_gpu_ir_prepare_draw12(d3d12_draw_context *context, const gpu_ir_pipeline *pipeline,
                                      const gpu_ir_draw_info *draw, uint32_t topology,
                                      const gpu_ir_resource_access *accesses,
                                      uint32_t access_count, MTLPrimitiveType *primitive_out,
                                      id<MTLRenderCommandEncoder> __strong *encoder_out) {
    pso12 *api_pipeline = context->api_pipeline;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!api_pipeline || !api_pipeline->render || api_pipeline->gpu_pipeline != pipeline || !info ||
        info->kind != GPU_IR_PIPELINE_GRAPHICS) return GPU_IR_INVALID;
    MTLPrimitiveType primitive;
    switch (topology) {
    case GPU_IR_TOPOLOGY_POINT_LIST: primitive = MTLPrimitiveTypePoint; break;
    case GPU_IR_TOPOLOGY_LINE_LIST: primitive = MTLPrimitiveTypeLine; break;
    case GPU_IR_TOPOLOGY_LINE_STRIP: primitive = MTLPrimitiveTypeLineStrip; break;
    case GPU_IR_TOPOLOGY_TRIANGLE_LIST: primitive = MTLPrimitiveTypeTriangle; break;
    case GPU_IR_TOPOLOGY_TRIANGLE_STRIP: primitive = MTLPrimitiveTypeTriangleStrip; break;
    default: return GPU_IR_INVALID;
    }
    id<MTLRenderCommandEncoder> __strong *re = (id<MTLRenderCommandEncoder> __strong *)context->render_encoder;
    if (!re || !*re) return GPU_IR_INVALID;
    heap_textures_render12(*re, context->resource_heap, msl_wide12(&api_pipeline->vs) || msl_wide12(&api_pipeline->ps));
    int mapping_result = d3d12_shader_component_mapping_read12(context->device, &api_pipeline->vs,
        GPU_IR_STAGE_VERTEX, context->graphics_root, context->resource_heap, context->sampler_heap);
    if (mapping_result != GPU_IR_OK) return mapping_result;
    mapping_result = d3d12_shader_component_mapping_read12(context->device, &api_pipeline->ps,
        GPU_IR_STAGE_FRAGMENT, context->graphics_root, context->resource_heap, context->sampler_heap);
    if (mapping_result != GPU_IR_OK) return mapping_result;
    int access_result = d3d12_use_render_accesses12(*re, accesses, access_count);
    if (access_result != GPU_IR_OK) return access_result;
    dev12 *device = context->device;
    gfx_state *g = context->graphics_state;
    heap12 *heap = context->resource_heap, *sheap = context->sampler_heap;
    scratch *sc = context->scratch_buffer;

    const float *blend_constants = draw && draw->has_blend_constants ? draw->blend_constants : g->blend;
    encode_render_state12(*re, api_pipeline, g, blend_constants);
    for (uint32_t k = 0; k < 16; k++) {
        if (draw && draw->vertex_buffers_captured) {
            const gpu_ir_vertex_buffer_binding *vertex = NULL;
            for (uint32_t i = 0; i < draw->vertex_buffer_count; i++)
                if (draw->vertex_buffers[i].binding == k) { vertex = &draw->vertex_buffers[i]; break; }
            if (!vertex) {
                [*re setVertexBuffer:nil offset:0 atIndex:16 + k];
                continue;
            }
            if (!gpu_ir_resource_is_buffer(vertex->resource) || vertex->offset > NSUIntegerMax)
                return GPU_IR_INVALID;
            id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(vertex->resource);
            if (!buffer || vertex->offset > buffer.length) return GPU_IR_RANGE;
            [*re setVertexBuffer:buffer offset:(NSUInteger)vertex->offset attributeStride:vertex->stride
                         atIndex:16 + k];
            continue;
        }
        if (!g->vb[k].va) { [*re setVertexBuffer:nil offset:0 atIndex:16 + k]; continue; }
        uint64_t offset = 0;
        id<MTLBuffer> buffer = buffer_at(device, g->vb[k].va, &offset);
        if (buffer) [*re setVertexBuffer:buffer offset:offset attributeStride:g->vb[k].stride atIndex:16 + k];
    }
    uint64_t root[64 + 2];
    uint32_t words;
    stage_root(device, &api_pipeline->vs, context->graphics_root, heap, sheap, sc, root, &words);
    [*re setVertexBytes:root length:8 * words atIndex:0];
    if (api_pipeline->render) {
        stage_root(device, &api_pipeline->ps, context->graphics_root, heap, sheap, sc, root, &words);
        [*re setFragmentBytes:root length:8 * words atIndex:0];
    }
    if (sc->buf) [*re useResource:sc->buf usage:MTLResourceUsageRead
        stages:MTLRenderStageVertex | MTLRenderStageFragment];
    *primitive_out = primitive;
    *encoder_out = *re;
    return GPU_IR_OK;
}

// ---- draws with a geometry shader ------------------------------------------------------------------------------------
//
// The vertex shader runs as a kernel over the draw's vertices (Metal fetches them through a stage-input descriptor
// made from the input layout, indexed draws included), the geometry shader as a kernel over its input primitives, and
// the triangles it made are drawn with its pass-through vertex function and the pixel shader (as in d3d11_draw.m).
enum { GS12_VB_BASE = 14 };                    // vertex buffer slot s at Metal buffer 14 + s (DXBC_GS_* above them)

// What a vertex format delivers to a shader: 0 floats (also normalized integers), 1 signed integers, 2 unsigned.
static int gs12_format_kind(MTLVertexFormat f) {
    switch (f) {
    case MTLVertexFormatChar: case MTLVertexFormatChar2: case MTLVertexFormatChar3: case MTLVertexFormatChar4:
    case MTLVertexFormatShort: case MTLVertexFormatShort2: case MTLVertexFormatShort3: case MTLVertexFormatShort4:
    case MTLVertexFormatInt: case MTLVertexFormatInt2: case MTLVertexFormatInt3: case MTLVertexFormatInt4: return 1;
    case MTLVertexFormatUChar: case MTLVertexFormatUChar2: case MTLVertexFormatUChar3: case MTLVertexFormatUChar4:
    case MTLVertexFormatUShort: case MTLVertexFormatUShort2: case MTLVertexFormatUShort3: case MTLVertexFormatUShort4:
    case MTLVertexFormatUInt: case MTLVertexFormatUInt2: case MTLVertexFormatUInt3: case MTLVertexFormatUInt4: return 2;
    default: return 0;
    }
}
// The vertex kernel for an index type (0 none, 1 16-bit, 2 32-bit) and the bound vertex strides.
static id<MTLComputePipelineState> gs12_vertex_kernel(dev12 *d, pso12 *p, gfx_state *g, int index_type) {
    NSMutableString *key = [NSMutableString stringWithFormat:@"%d/", index_type];
    for (uint32_t i = 0; i < p->gs_nattr; i++) [key appendFormat:@"%u,", g->vb[p->gs_attr[i].slot].stride];
    pthread_mutex_lock(&d->lock);
    id cached = p->gs_vs_kernels[key];
    pthread_mutex_unlock(&d->lock);
    if (cached) return cached == [NSNull null] ? nil : cached;
    // Compute stage input converts formats only within a kind: the kernel's inputs are declared as the layout's
    // formats deliver them, as the input assembler would hand their bits over.
    NSMutableString *source = [p->gs_vs_source mutableCopy];
    MTLStageInputOutputDescriptor *sd = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
    for (uint32_t i = 0; i < p->gs_nattr; i++) {
        uint32_t reg = p->gs_attr[i].reg, slot = p->gs_attr[i].slot;
        int kind = gs12_format_kind(p->gs_attr[i].format);
        const char *want = kind == 1 ? "int4" : kind == 2 ? "uint4" : "float4";
        for (int k = 0; k < 3; k++) {
            static const char *const types[3] = { "float4", "int4", "uint4" };
            NSString *from = [NSString stringWithFormat:@"    %s v%u [[attribute(%u)]];", types[k], reg, reg];
            NSString *to = [NSString stringWithFormat:@"    %s v%u [[attribute(%u)]];", want, reg, reg];
            [source replaceOccurrencesOfString:from withString:to options:0 range:NSMakeRange(0, source.length)];
        }
        sd.attributes[reg].format = (MTLAttributeFormat)p->gs_attr[i].format;   // (the same numbering)
        sd.attributes[reg].offset = p->gs_attr[i].offset;
        sd.attributes[reg].bufferIndex = GS12_VB_BASE + slot;
        MTLBufferLayoutDescriptor *layout = sd.layouts[GS12_VB_BASE + slot];
        uint32_t stride = g->vb[slot].stride;
        if (!stride) { layout.stepFunction = MTLStepFunctionConstant; layout.stepRate = 0; layout.stride = 16; }
        else {
            layout.stride = stride;
            layout.stepFunction = p->gs_attr[i].instanced ? MTLStepFunctionThreadPositionInGridY :
                                  index_type ? MTLStepFunctionThreadPositionInGridXIndexed : MTLStepFunctionThreadPositionInGridX;
            layout.stepRate = p->gs_attr[i].instanced ? p->gs_attr[i].step : 1;
        }
    }
    if (index_type) { sd.indexType = index_type == 2 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16; sd.indexBufferIndex = DXBC_GS_INDEX; }
    NSError *err = nil;
    id<MTLLibrary> lib = [d->mtl newLibraryWithSource:source options:nil error:&err];
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
    id<MTLComputePipelineState> kernel = nil;
    if (fn) {
        MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
        cd.computeFunction = fn;
        cd.stageInputDescriptor = sd;
        kernel = [d->mtl newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&err];
    }
    if (!kernel) vx_log("D3D12: vertex kernel before a geometry shader: %s", err.localizedDescription.UTF8String);
    pthread_mutex_lock(&d->lock);
    p->gs_vs_kernels[key] = kernel ? (id)kernel : (id)[NSNull null];   // (failures too: no retry per draw)
    pthread_mutex_unlock(&d->lock);
    return kernel;
}
static void gs12_draw(dev12 *d, gfx_state *g, pso12 *p, const cmd12 *c, id<MTLCommandBuffer> cb,
                      id<MTLComputeCommandEncoder> __strong *ce, id<MTLBlitCommandEncoder> __strong *be,
                      id<MTLRenderCommandEncoder> __strong *re, NSArray *resident, heap12 *heap, heap12 *sheap,
                      root_binding *graphics, scratch *sc) {
    int indexed = c->op == C_DRAW_INDEXED;
    uint32_t count = c->a, instances = c->b, start = c->c;
    uint32_t start_instance = indexed ? (uint32_t)c->off2 : (uint32_t)c->off;
    int32_t base_vertex = indexed ? (int32_t)(int64_t)c->off : 0;
    uint32_t n = p->gs_in_verts, step, prims;
    switch (g->topology) {                     // primitives from the input assembler's topology
    case 1: case 2: case 4: case 10: case 12: step = n; prims = count / n; break;     // lists (with adjacency)
    case 3: case 5: step = 1; prims = count >= n ? count - (n - 1) : 0; break;         // strips
    default: vx_log("D3D12: topology %u before a geometry shader is not supported", g->topology); return;
    }
    if (!prims || !instances || (indexed && !g->ib.va)) return;
    int index_type = indexed ? (g->ib.format == 42 ? 2 : 1) : 0;
    id<MTLComputePipelineState> vk = gs12_vertex_kernel(d, p, g, index_type);
    if (!vk) return;
    uint32_t vsn = p->gs_vs_out_regs, gsn = p->gs_out_regs, tris = p->gs_max_tris;
    NSUInteger lens[3] = { (NSUInteger)count * instances * vsn * 16, (NSUInteger)prims * instances * tris * 3 * gsn * 16,
                           (NSUInteger)prims * instances * 4 };
    id<MTLBuffer> bufs[3];
    for (int k = 0; k < 3; k++)
        if (!(bufs[k] = [d->mtl newBufferWithLength:MAX(lens[k], (NSUInteger)16) options:MTLResourceStorageModePrivate])) return;
    id<MTLBuffer> vs_out = bufs[0], gs_out = bufs[1], counts = bufs[2];

    if (*be) { [*be endEncoding]; *be = nil; }
    close_pass(d, g, cb, re);
    if (!*ce) {
        *ce = new_compute12(cb);
        use_resident_compute12(*ce, resident, heap);
        heap_textures_compute12(*ce, heap, 1);
    }
    // the vertex kernel: thread (x, y) is vertex x of instance y
    [*ce setComputePipelineState:vk];
    for (uint32_t i = 0; i < p->gs_nattr; i++) {
        uint32_t slot = p->gs_attr[i].slot;
        uint64_t offset = 0;
        id<MTLBuffer> buffer = g->vb[slot].va ? buffer_at(d, g->vb[slot].va, &offset) : nil;
        if (!buffer) continue;
        int64_t first = p->gs_attr[i].instanced ? (int64_t)(start_instance / p->gs_attr[i].step) : indexed ? base_vertex : (int64_t)start;
        int64_t at = (int64_t)offset + (int64_t)g->vb[slot].stride * first;
        [*ce setBuffer:buffer offset:(NSUInteger)(at < 0 ? 0 : at) atIndex:GS12_VB_BASE + slot];
    }
    if (indexed) {
        uint64_t offset = 0;
        id<MTLBuffer> ib = buffer_at(d, g->ib.va, &offset);
        if (!ib) return;
        [*ce setBuffer:ib offset:(NSUInteger)(offset + (uint64_t)start * (index_type == 2 ? 4 : 2)) atIndex:DXBC_GS_INDEX];
    } else [*ce setBuffer:vs_out offset:0 atIndex:DXBC_GS_INDEX];   // (not read)
    uint64_t root[64 + 2];
    uint32_t words;
    stage_root(d, &p->vs, graphics, heap, sheap, sc, root, &words);
    [*ce setBytes:root length:8 * words atIndex:0];
    uint32_t vp[8] = { count, indexed ? 0 : start, start_instance, vsn, (uint32_t)index_type, 0, (uint32_t)base_vertex, 0 };
    [*ce setBytes:vp length:sizeof vp atIndex:DXBC_VS_PARAMS];
    [*ce setBuffer:vs_out offset:0 atIndex:DXBC_GS_OUT];
    [*ce dispatchThreads:MTLSizeMake(count, instances, 1) threadsPerThreadgroup:MTLSizeMake(MIN(64, vk.maxTotalThreadsPerThreadgroup), 1, 1)];
    // the geometry kernel: thread (x, y) is input primitive x of instance y
    [*ce setComputePipelineState:p->gs_kernel];
    stage_root(d, &p->gs, graphics, heap, sheap, sc, root, &words);
    [*ce setBytes:root length:8 * words atIndex:0];
    uint32_t gp[4] = { prims, step, count, vsn };
    [*ce setBytes:gp length:sizeof gp atIndex:DXBC_GS_PARAMS];
    [*ce setBuffer:vs_out offset:0 atIndex:DXBC_GS_IN];
    [*ce setBuffer:gs_out offset:0 atIndex:DXBC_GS_OUT];
    [*ce setBuffer:counts offset:0 atIndex:DXBC_GS_COUNTS];
    if (sc->buf) [*ce useResource:sc->buf usage:MTLResourceUsageRead];
    [*ce dispatchThreads:MTLSizeMake(prims, instances, 1) threadsPerThreadgroup:MTLSizeMake(MIN(64, p->gs_kernel.maxTotalThreadsPerThreadgroup), 1, 1)];
    // the triangles
    if (!open_render_pass12(d, g, cb, ce, be, re, resident, heap)) { vx_log("D3D12: could not open a render pass for a geometry draw"); return; }
    heap_textures_render12(*re, heap, 1);
    encode_render_state12(*re, p, g, g->blend);
    [*re setVertexBuffer:gs_out offset:0 atIndex:0];
    [*re setVertexBuffer:counts offset:0 atIndex:1];
    stage_root(d, &p->ps, graphics, heap, sheap, sc, root, &words);
    [*re setFragmentBytes:root length:8 * words atIndex:0];
    if (sc->buf) [*re useResource:sc->buf usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
    gpu_ir_access_list12 accesses = { 0 };
    pthread_mutex_lock(&d->lock);
    int access_rc = gpu_ir_collect_draw_accesses12(d, p, g, graphics, heap, sheap, &accesses, indexed);
    pthread_mutex_unlock(&d->lock);
    if (access_rc == GPU_IR_OK) d3d12_use_render_accesses12(*re, accesses.values, accesses.count);
    gpu_ir_access_list_release12(&accesses);
    [*re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)prims * instances * tris * 3];
}

// Draws with tessellation: the vertex kernel over the control points (as for geometry), the hull kernel over the
// patches (control points, patch constants, tessellation factors), then the patches with the domain shader.
static void tess12_draw(dev12 *d, gfx_state *g, pso12 *p, const cmd12 *c, id<MTLCommandBuffer> cb,
                        id<MTLComputeCommandEncoder> __strong *ce, id<MTLBlitCommandEncoder> __strong *be,
                        id<MTLRenderCommandEncoder> __strong *re, NSArray *resident, heap12 *heap, heap12 *sheap,
                        root_binding *graphics, scratch *sc) {
    int indexed = c->op == C_DRAW_INDEXED;
    uint32_t count = c->a, instances = c->b, start = c->c;
    uint32_t start_instance = indexed ? (uint32_t)c->off2 : (uint32_t)c->off;
    int32_t base_vertex = indexed ? (int32_t)(int64_t)c->off : 0;
    uint32_t n = p->tess_in_cp;
    if (g->topology < 33 || g->topology > 64 || g->topology - 32 != n) {
        vx_log("D3D12: topology %u for a hull shader of %u control points", g->topology, n);
        return;
    }
    uint32_t patches = count / n;
    if (!patches || !instances || (indexed && !g->ib.va)) return;
    int index_type = indexed ? (g->ib.format == 42 ? 2 : 1) : 0;
    id<MTLComputePipelineState> vk = gs12_vertex_kernel(d, p, g, index_type);
    if (!vk) return;
    uint32_t vsn = p->gs_vs_out_regs, total = patches * instances;
    uint32_t fstride = p->tess_domain == 3 ? 12 : 8;               // (bytes of a patch's half factors)
    NSUInteger lens[4] = { (NSUInteger)count * instances * vsn * 16, (NSUInteger)total * p->tess_out_cp * p->hs_cp_slots * 16,
                           (NSUInteger)total * p->hs_pc_slots * 16, (NSUInteger)total * fstride };
    id<MTLBuffer> bufs[4];
    for (int k = 0; k < 4; k++)
        if (!(bufs[k] = [d->mtl newBufferWithLength:MAX(lens[k], (NSUInteger)16) options:MTLResourceStorageModePrivate])) return;
    id<MTLBuffer> vs_out = bufs[0], cp_out = bufs[1], pc_out = bufs[2], factors = bufs[3];

    if (*be) { [*be endEncoding]; *be = nil; }
    close_pass(d, g, cb, re);
    if (!*ce) {
        *ce = new_compute12(cb);
        use_resident_compute12(*ce, resident, heap);
        heap_textures_compute12(*ce, heap, 1);
    }
    // the vertex kernel: thread (x, y) is vertex x of instance y
    [*ce setComputePipelineState:vk];
    for (uint32_t i = 0; i < p->gs_nattr; i++) {
        uint32_t slot = p->gs_attr[i].slot;
        uint64_t offset = 0;
        id<MTLBuffer> buffer = g->vb[slot].va ? buffer_at(d, g->vb[slot].va, &offset) : nil;
        if (!buffer) continue;
        int64_t first = p->gs_attr[i].instanced ? (int64_t)(start_instance / p->gs_attr[i].step) : indexed ? base_vertex : (int64_t)start;
        int64_t at = (int64_t)offset + (int64_t)g->vb[slot].stride * first;
        [*ce setBuffer:buffer offset:(NSUInteger)(at < 0 ? 0 : at) atIndex:GS12_VB_BASE + slot];
    }
    if (indexed) {
        uint64_t offset = 0;
        id<MTLBuffer> ib = buffer_at(d, g->ib.va, &offset);
        if (!ib) return;
        [*ce setBuffer:ib offset:(NSUInteger)(offset + (uint64_t)start * (index_type == 2 ? 4 : 2)) atIndex:DXBC_GS_INDEX];
    } else [*ce setBuffer:vs_out offset:0 atIndex:DXBC_GS_INDEX];   // (not read)
    uint64_t root[64 + 2];
    uint32_t words;
    stage_root(d, &p->vs, graphics, heap, sheap, sc, root, &words);
    [*ce setBytes:root length:8 * words atIndex:0];
    uint32_t vp[8] = { count, indexed ? 0 : start, start_instance, vsn, (uint32_t)index_type, 0, (uint32_t)base_vertex, 0 };
    [*ce setBytes:vp length:sizeof vp atIndex:DXBC_VS_PARAMS];
    [*ce setBuffer:vs_out offset:0 atIndex:DXBC_GS_OUT];
    [*ce dispatchThreads:MTLSizeMake(count, instances, 1) threadsPerThreadgroup:MTLSizeMake(MIN(64, vk.maxTotalThreadsPerThreadgroup), 1, 1)];
    // the hull kernel: thread (x, y) is patch x of instance y
    [*ce setComputePipelineState:p->hs_kernel];
    stage_root(d, &p->hs, graphics, heap, sheap, sc, root, &words);
    [*ce setBytes:root length:8 * words atIndex:0];
    uint32_t hp[4] = { patches, count, vsn, 0 };
    [*ce setBytes:hp length:sizeof hp atIndex:DXIL_HS_PARAMS];
    [*ce setBuffer:vs_out offset:0 atIndex:DXIL_HS_IN];
    [*ce setBuffer:cp_out offset:0 atIndex:DXIL_HS_CP];
    [*ce setBuffer:pc_out offset:0 atIndex:DXIL_HS_PC];
    [*ce setBuffer:factors offset:0 atIndex:DXIL_HS_FACTORS];
    if (sc->buf) [*ce useResource:sc->buf usage:MTLResourceUsageRead];
    [*ce dispatchThreads:MTLSizeMake(patches, instances, 1) threadsPerThreadgroup:MTLSizeMake(MIN(64, p->hs_kernel.maxTotalThreadsPerThreadgroup), 1, 1)];
    // the patches
    if (!open_render_pass12(d, g, cb, ce, be, re, resident, heap)) { vx_log("D3D12: could not open a render pass for a tessellated draw"); return; }
    heap_textures_render12(*re, heap, 1);
    encode_render_state12(*re, p, g, g->blend);
    [*re setTessellationFactorBuffer:factors offset:0 instanceStride:0];
    [*re setVertexBuffer:cp_out offset:0 atIndex:DXIL_DS_CP];
    [*re setVertexBuffer:pc_out offset:0 atIndex:DXIL_DS_PC];
    uint32_t dp[4] = { p->tess_out_cp, p->hs_cp_slots, patches, p->hs_pc_slots };
    [*re setVertexBytes:dp length:sizeof dp atIndex:DXIL_DS_PARAMS];
    stage_root(d, &p->ds, graphics, heap, sheap, sc, root, &words);
    [*re setVertexBytes:root length:8 * words atIndex:0];
    stage_root(d, &p->ps, graphics, heap, sheap, sc, root, &words);
    [*re setFragmentBytes:root length:8 * words atIndex:0];
    if (sc->buf) [*re useResource:sc->buf usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
    gpu_ir_access_list12 accesses = { 0 };
    pthread_mutex_lock(&d->lock);
    int access_rc = gpu_ir_collect_draw_accesses12(d, p, g, graphics, heap, sheap, &accesses, indexed);
    pthread_mutex_unlock(&d->lock);
    if (access_rc == GPU_IR_OK) d3d12_use_render_accesses12(*re, accesses.values, accesses.count);
    gpu_ir_access_list_release12(&accesses);
    [*re drawPatches:0 patchStart:0 patchCount:total patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
}

static int d3d12_gpu_ir_draw(void *opaque, const gpu_ir_pipeline *pipeline, const gpu_ir_draw_info *draw,
                             const gpu_ir_resource_access *accesses, uint32_t access_count) {
    d3d12_draw_context *context = opaque;
    if (!draw || draw->indexed > 1 || !context || !context->api_pipeline ||
        !context->api_pipeline->render || context->api_pipeline->gpu_pipeline != pipeline)
        return GPU_IR_INVALID;
    if (!draw->instance_count || (draw->indexed ? !draw->index_count : !draw->vertex_count)) return GPU_IR_OK;
    MTLPrimitiveType primitive;
    id<MTLRenderCommandEncoder> encoder = nil;
    int rc = d3d12_gpu_ir_prepare_draw12(context, pipeline, draw, draw->topology, accesses, access_count,
                                          &primitive, &encoder);
    if (rc != GPU_IR_OK) return rc;
    if (!draw->indexed) {
        [encoder drawPrimitives:primitive vertexStart:draw->first_vertex vertexCount:draw->vertex_count
            instanceCount:draw->instance_count baseInstance:draw->first_instance];
    } else {
        uint64_t offset = 0;
        id<MTLBuffer> index_buffer = nil;
        uint32_t format = 0;
        if (draw->vertex_buffers_captured && draw->index_buffer) {
            if (!gpu_ir_resource_is_buffer(draw->index_buffer) ||
                (draw->index_type != GPU_IR_INDEX_TYPE_UINT32 && draw->index_type != GPU_IR_INDEX_TYPE_UINT16))
                return GPU_IR_INVALID;
            index_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(draw->index_buffer);
            offset = draw->index_buffer_offset;
            format = draw->index_type == GPU_IR_INDEX_TYPE_UINT32 ? 42u : 57u;
        } else if (!draw->vertex_buffers_captured) {
            index_buffer = buffer_at(context->device, context->graphics_state->ib.va, &offset);
            format = context->graphics_state->ib.format;
        }
        if (!index_buffer || (format != 42 && format != 57)) {
            vx_log("D3D12: an indexed draw without a supported index buffer");
            return GPU_IR_INVALID;
        }
        int index32 = format == 42;
        [encoder drawIndexedPrimitives:primitive indexCount:draw->index_count
            indexType:index32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:index_buffer
            indexBufferOffset:offset + (uint64_t)draw->first_index * (index32 ? 4 : 2)
            instanceCount:draw->instance_count baseVertex:draw->vertex_offset baseInstance:draw->first_instance];
    }
    return GPU_IR_OK;
}

static id<MTLComputePipelineState> d3d12_indirect_args_filter_pipeline12(dev12 *device) {
    static NSString *source =
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        @"struct FilterArgs { ulong source_stride_words; uint payload_words; uint max_count; };\n"
        @"kernel void vx_filter(device const uint *source [[buffer(0)]], device uint *destination [[buffer(1)]],\n"
        @"    device const uint *count [[buffer(2)]], constant FilterArgs& args [[buffer(3)]],\n"
        @"    uint i [[thread_position_in_grid]]) {\n"
        @"    if (i >= args.max_count) return;\n"
        @"    bool active = i < min(*count, args.max_count);\n"
        @"    ulong src = (ulong)i * args.source_stride_words;\n"
        @"    ulong dst = (ulong)i * args.payload_words;\n"
        @"    for (uint word = 0; word < args.payload_words; ++word) {\n"
        @"        uint value = source[src + word];\n"
        @"        destination[dst + word] = (!active && word == 0) ? 0 : value;\n"
        @"    }\n"
        @"}\n";
    pthread_mutex_lock(&device->lock);
    id<MTLComputePipelineState> pipeline = device->indirect_args_filter;
    pthread_mutex_unlock(&device->lock);
    if (pipeline) return pipeline;

    NSError *error = nil;
    id<MTLLibrary> library = [device->mtl newLibraryWithSource:source options:nil error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"vx_filter"];
    pipeline = function ? [device->mtl newComputePipelineStateWithFunction:function error:&error] : nil;
    if (!pipeline) {
        vx_log("D3D12: ExecuteIndirect argument filter did not compile: %s", error.localizedDescription.UTF8String);
        return nil;
    }
    pthread_mutex_lock(&device->lock);
    if (!device->indirect_args_filter) device->indirect_args_filter = pipeline;
    else pipeline = device->indirect_args_filter;
    pthread_mutex_unlock(&device->lock);
    return pipeline;
}

static int d3d12_gpu_ir_dispatch_indirect_range(void *opaque, const gpu_ir_pipeline *pipeline,
                                               const gpu_ir_resource *arguments,
                                               const gpu_ir_resource *count_arguments,
                                               const gpu_ir_indirect_dispatch_info *dispatch,
                                               const gpu_ir_resource_access *accesses, uint32_t access_count) {
    d3d12_dispatch_context *context = opaque;
    if (!context || !dispatch || !dispatch->dispatch_count || dispatch->stride < 12 || (dispatch->stride & 3) ||
        (dispatch->argument_offset & 3) || (count_arguments && (dispatch->count_offset & 3)) ||
        !gpu_ir_resource_is_buffer(arguments) ||
        (count_arguments && !gpu_ir_resource_is_buffer(count_arguments))) return GPU_IR_INVALID;
    pso12 *api_pipeline = context->api_pipeline;
    const gpu_ir_pipeline_info *info = gpu_ir_pipeline_description(pipeline);
    if (!api_pipeline || !api_pipeline->compute || api_pipeline->gpu_pipeline != pipeline || !info ||
        info->kind != GPU_IR_PIPELINE_COMPUTE) return GPU_IR_INVALID;
    int mapping_result = d3d12_shader_component_mapping_read12(context->device, &api_pipeline->msl,
        GPU_IR_STAGE_COMPUTE, context->compute_root, context->resource_heap, context->sampler_heap);
    if (mapping_result != GPU_IR_OK) return mapping_result;

    uint64_t records = (uint64_t)dispatch->dispatch_count - 1;
    uint64_t last_offset, required_end;
    uint64_t argument_size = gpu_ir_buffer_size(arguments);
    if ((records && dispatch->stride > UINT64_MAX / records) ||
        dispatch->argument_offset > UINT64_MAX - records * dispatch->stride) return GPU_IR_RANGE;
    last_offset = records * dispatch->stride;
    required_end = dispatch->argument_offset + last_offset;
    if (required_end > UINT64_MAX - 12 || (required_end += 12) > argument_size) return GPU_IR_RANGE;
    if (!count_arguments) {
        for (uint32_t i = 0; i < dispatch->dispatch_count; i++) {
            uint64_t offset = dispatch->argument_offset + (uint64_t)i * dispatch->stride;
            int rc = d3d12_gpu_ir_dispatch_indirect(opaque, pipeline, arguments, offset, accesses, access_count);
            if (rc != GPU_IR_OK) return rc;
        }
        return GPU_IR_OK;
    }
    if (dispatch->argument_offset > SIZE_MAX ||
        dispatch->count_offset > SIZE_MAX || dispatch->count_offset > gpu_ir_buffer_size(count_arguments) ||
        4 > gpu_ir_buffer_size(count_arguments) - dispatch->count_offset) return GPU_IR_RANGE;

    dev12 *device = context->device;
    id<MTLCommandBuffer> command_buffer = (__bridge id<MTLCommandBuffer>)context->command_buffer;
    id<MTLComputeCommandEncoder> __strong *compute_encoder =
        (id<MTLComputeCommandEncoder> __strong *)context->compute_encoder;
    id<MTLBlitCommandEncoder> __strong *blit_encoder =
        (id<MTLBlitCommandEncoder> __strong *)context->blit_encoder;
    id<MTLRenderCommandEncoder> __strong *render_encoder =
        (id<MTLRenderCommandEncoder> __strong *)context->render_encoder;
    NSArray *resident = (__bridge NSArray *)context->resident_resources;
    scratch *scratch_buffer = context->scratch_buffer;
    id<MTLBuffer> argument_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(arguments);
    id<MTLBuffer> count_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(count_arguments);
    id<MTLComputePipelineState> metal_pipeline = api_pipeline->compute;
    if (!device || !command_buffer || !compute_encoder || !blit_encoder || !render_encoder || !scratch_buffer ||
        !argument_buffer || !count_buffer || !metal_pipeline ||
        required_end > argument_buffer.length || dispatch->count_offset > count_buffer.length ||
        sizeof(uint32_t) > count_buffer.length - dispatch->count_offset) return GPU_IR_INVALID;
    id<MTLComputePipelineState> filter = d3d12_indirect_args_filter_pipeline12(device);
    if (!filter) return GPU_IR_UNSUPPORTED;

    size_t output_size = (size_t)dispatch->dispatch_count * 12;
    size_t output_offset = 0;
    if (!scratch_alloc(device, scratch_buffer, output_size, &output_offset)) return GPU_IR_OUT_OF_MEMORY;

    close_pass(device, context->graphics_state, command_buffer, render_encoder);
    if (*blit_encoder) { [*blit_encoder endEncoding]; *blit_encoder = nil; }
    if (!*compute_encoder) {
        *compute_encoder = new_compute12(command_buffer);
        // Descriptor-table resources are accessed indirectly, so keep the complete visible set resident.
        use_resident_compute12(*compute_encoder, resident, context->resource_heap);
        heap_textures_compute12(*compute_encoder, context->resource_heap, 1);   // (indirect: as before)
    }
    if (!*compute_encoder) return GPU_IR_UNSUPPORTED;
    int access_result = d3d12_use_compute_accesses12(*compute_encoder, accesses, access_count);
    if (access_result != GPU_IR_OK) return access_result;
    [*compute_encoder useResource:argument_buffer usage:MTLResourceUsageRead];
    [*compute_encoder useResource:count_buffer usage:MTLResourceUsageRead];
    [*compute_encoder useResource:scratch_buffer->buf usage:MTLResourceUsageWrite];
    [*compute_encoder setComputePipelineState:filter];
    [*compute_encoder setBuffer:argument_buffer offset:(NSUInteger)dispatch->argument_offset atIndex:0];
    [*compute_encoder setBuffer:scratch_buffer->buf offset:output_offset atIndex:1];
    [*compute_encoder setBuffer:count_buffer offset:(NSUInteger)dispatch->count_offset atIndex:2];
    struct { uint64_t source_stride_words; uint32_t payload_words, max_count; } parameters = {
        .source_stride_words = dispatch->stride / sizeof(uint32_t),
        .payload_words = 3,
        .max_count = dispatch->dispatch_count
    };
    [*compute_encoder setBytes:&parameters length:sizeof parameters atIndex:3];
    NSUInteger width = MIN((NSUInteger)dispatch->dispatch_count,
                           MAX((NSUInteger)1, filter.threadExecutionWidth));
    [*compute_encoder dispatchThreads:MTLSizeMake(dispatch->dispatch_count, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
    [*compute_encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];

    uint64_t root[64 + 2];
    uint32_t words;
    stage_root(device, &api_pipeline->msl, context->compute_root, context->resource_heap,
               context->sampler_heap, scratch_buffer, root, &words);
    [*compute_encoder setComputePipelineState:metal_pipeline];
    [*compute_encoder setBytes:root length:8 * words atIndex:0];
    [*compute_encoder useResource:scratch_buffer->buf usage:MTLResourceUsageRead];
    if (getenv("VXRT_D3D12_TRACE_INDIRECT")) {
        static volatile int told;
        if (__sync_add_and_fetch(&told, 1) <= 60)
            vx_log("D3D12: indirect dispatch range of cs %016llx: up to %u records, stride %u", (unsigned long long)api_pipeline->cs_code,
                   dispatch->dispatch_count, dispatch->stride);
    }
    for (uint32_t i = 0; i < dispatch->dispatch_count; i++) {
        NSUInteger offset = output_offset + (NSUInteger)i * 12;
        [*compute_encoder dispatchThreadgroupsWithIndirectBuffer:scratch_buffer->buf indirectBufferOffset:offset
            threadsPerThreadgroup:MTLSizeMake(info->threadgroup_size[0], info->threadgroup_size[1],
                                               info->threadgroup_size[2])];
        [*compute_encoder memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
    }
    return GPU_IR_OK;
}

static int d3d12_gpu_ir_draw_indirect(void *opaque, const gpu_ir_pipeline *pipeline,
                                     const gpu_ir_resource *arguments, const gpu_ir_resource *count_arguments,
                                     const gpu_ir_indirect_draw_info *draw,
                                     const gpu_ir_resource_access *accesses, uint32_t access_count) {
    d3d12_draw_context *context = opaque;
    if (!context || !draw || draw->indexed > 1 || !draw->draw_count ||
        !gpu_ir_resource_is_buffer(arguments) || (draw->argument_offset & 3) ||
        draw->argument_offset > SIZE_MAX) return GPU_IR_INVALID;
    uint32_t argument_size = draw->indexed ? 20 : 16;
    uint64_t last_offset, end_offset;
    if (draw->stride < argument_size || (draw->stride & 3) ||
        (draw->draw_count - 1 && draw->stride > UINT64_MAX / (draw->draw_count - 1))) return GPU_IR_INVALID;
    last_offset = (uint64_t)(draw->draw_count - 1) * draw->stride;
    if (draw->argument_offset > UINT64_MAX - last_offset ||
        draw->argument_offset + last_offset > UINT64_MAX - argument_size) return GPU_IR_INVALID;
    end_offset = draw->argument_offset + last_offset + argument_size;
    id<MTLBuffer> argument_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(arguments);
    if (!argument_buffer || end_offset > argument_buffer.length)
        return GPU_IR_RANGE;
    id<MTLBuffer> indirect_buffer = argument_buffer;
    NSUInteger indirect_offset = (NSUInteger)draw->argument_offset;
    if (count_arguments) {
        if (!gpu_ir_resource_is_buffer(count_arguments) || (draw->count_offset & 3) || draw->count_offset > SIZE_MAX)
            return GPU_IR_INVALID;
        id<MTLBuffer> count_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(count_arguments);
        if (!count_buffer || draw->count_offset > count_buffer.length ||
            sizeof(uint32_t) > count_buffer.length - draw->count_offset) return GPU_IR_RANGE;
        if (!context->command_buffer || !context->compute_encoder || !context->blit_encoder ||
            !context->resident_resources || !context->scratch_buffer) return GPU_IR_INVALID;

        dev12 *device = context->device;
        id<MTLCommandBuffer> command_buffer = (__bridge id<MTLCommandBuffer>)context->command_buffer;
        id<MTLComputeCommandEncoder> __strong *compute_encoder =
            (id<MTLComputeCommandEncoder> __strong *)context->compute_encoder;
        id<MTLBlitCommandEncoder> __strong *blit_encoder =
            (id<MTLBlitCommandEncoder> __strong *)context->blit_encoder;
        id<MTLRenderCommandEncoder> __strong *render_encoder =
            (id<MTLRenderCommandEncoder> __strong *)context->render_encoder;
        NSArray *resident = (__bridge NSArray *)context->resident_resources;
        scratch *scratch_buffer = context->scratch_buffer;
        id<MTLComputePipelineState> filter = d3d12_indirect_args_filter_pipeline12(device);
        if (!filter || (size_t)draw->draw_count > SIZE_MAX / argument_size) return GPU_IR_UNSUPPORTED;
        size_t output_size = (size_t)draw->draw_count * argument_size;
        size_t output_offset = 0;
        if (!scratch_alloc(device, scratch_buffer, output_size, &output_offset)) return GPU_IR_OUT_OF_MEMORY;

        close_pass(device, context->graphics_state, command_buffer, render_encoder);
        if (*blit_encoder) { [*blit_encoder endEncoding]; *blit_encoder = nil; }
        if (!*compute_encoder)
            *compute_encoder = new_compute12(command_buffer);
        if (!*compute_encoder) return GPU_IR_UNSUPPORTED;
        [*compute_encoder useResource:argument_buffer usage:MTLResourceUsageRead];
        [*compute_encoder useResource:count_buffer usage:MTLResourceUsageRead];
        [*compute_encoder useResource:scratch_buffer->buf usage:MTLResourceUsageWrite];
        [*compute_encoder setComputePipelineState:filter];
        [*compute_encoder setBuffer:argument_buffer offset:(NSUInteger)draw->argument_offset atIndex:0];
        [*compute_encoder setBuffer:scratch_buffer->buf offset:output_offset atIndex:1];
        [*compute_encoder setBuffer:count_buffer offset:(NSUInteger)draw->count_offset atIndex:2];
        struct { uint64_t source_stride_words; uint32_t payload_words, max_count; } parameters = {
            .source_stride_words = draw->stride / sizeof(uint32_t),
            .payload_words = argument_size / sizeof(uint32_t),
            .max_count = draw->draw_count
        };
        [*compute_encoder setBytes:&parameters length:sizeof parameters atIndex:3];
        NSUInteger width = MIN((NSUInteger)draw->draw_count, MAX((NSUInteger)1, filter.threadExecutionWidth));
        [*compute_encoder dispatchThreads:MTLSizeMake(draw->draw_count, 1, 1)
                    threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [*compute_encoder memoryBarrierWithScope:MTLBarrierScopeBuffers];
        [*compute_encoder endEncoding];
        *compute_encoder = nil;

        // A new render pass loads the existing attachments; tracked Metal resources order the filter writes
        // before these indirect argument reads in the same command buffer.
        if (!open_render_pass12(device, context->graphics_state, command_buffer, compute_encoder,
                                blit_encoder, render_encoder, resident, context->resource_heap))
            return GPU_IR_UNSUPPORTED;
        indirect_buffer = scratch_buffer->buf;
        indirect_offset = output_offset;
    }
    gfx_state *g = context->graphics_state;
    MTLPrimitiveType primitive;
    id<MTLRenderCommandEncoder> encoder = nil;
    gpu_ir_draw_info captured_state = {
        .vertex_buffers_captured = draw->vertex_buffers_captured,
        .vertex_buffer_count = draw->vertex_buffer_count,
        .has_blend_constants = draw->has_blend_constants
    };
    memcpy(captured_state.blend_constants, draw->blend_constants, sizeof captured_state.blend_constants);
    if (draw->vertex_buffer_count)
        memcpy(captured_state.vertex_buffers, draw->vertex_buffers,
               draw->vertex_buffer_count * sizeof draw->vertex_buffers[0]);
    int rc = d3d12_gpu_ir_prepare_draw12(context, pipeline, &captured_state, draw->topology, accesses, access_count,
                                          &primitive, &encoder);
    if (rc != GPU_IR_OK) return rc;
    id<MTLBuffer> index_buffer = nil;
    uint64_t index_offset = 0;
    MTLIndexType index_type = MTLIndexTypeUInt16;
    if (draw->indexed) {
        if (draw->vertex_buffers_captured) {
            if (!draw->index_buffer ||
                (draw->index_type != GPU_IR_INDEX_TYPE_UINT16 && draw->index_type != GPU_IR_INDEX_TYPE_UINT32))
                return GPU_IR_INVALID;
            index_buffer = (__bridge id<MTLBuffer>)gpu_ir_backend_object(draw->index_buffer);
            index_offset = draw->index_buffer_offset;
            if (!index_buffer || index_offset > index_buffer.length ||
                draw->index_buffer_size > index_buffer.length - index_offset) return GPU_IR_RANGE;
            index_type = draw->index_type == GPU_IR_INDEX_TYPE_UINT32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
        } else {
            if (!g->ib.va || (g->ib.format != 42 && g->ib.format != 57)) return GPU_IR_INVALID;
            index_buffer = buffer_at(context->device, g->ib.va, &index_offset);
            if (!index_buffer) return GPU_IR_INVALID;
            index_type = g->ib.format == 42 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
        }
    }
    [encoder useResource:indirect_buffer usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
    for (uint32_t i = 0; i < draw->draw_count; i++) {
        NSUInteger offset = indirect_offset + (NSUInteger)i * (count_arguments ? argument_size : draw->stride);
        if (!draw->indexed)
            [encoder drawPrimitives:primitive indirectBuffer:indirect_buffer indirectBufferOffset:offset];
        else
            [encoder drawIndexedPrimitives:primitive indexType:index_type indexBuffer:index_buffer
                indexBufferOffset:(NSUInteger)index_offset indirectBuffer:indirect_buffer indirectBufferOffset:offset];
    }
    return GPU_IR_OK;
}

static int gpu_ir_execute_transient_dispatch(gpu_ir_pipeline *pipeline, uint32_t x, uint32_t y, uint32_t z,
                                             const gpu_ir_resource_access *accesses, uint32_t access_count,
                                             const gpu_ir_backend *backend) {
    gpu_ir_stream stream;
    gpu_ir_stream_init(&stream);
    uint32_t index = GPU_IR_NO_COMMAND;
    int rc = gpu_ir_record_dispatch_with_accesses(&stream, pipeline, x, y, z, accesses, access_count, &index);
    if (rc == GPU_IR_OK && index != GPU_IR_NO_COMMAND) rc = gpu_ir_execute(&stream, index, backend);
    gpu_ir_stream_destroy(&stream);
    return rc;
}

static int gpu_ir_execute_transient_draw(gpu_ir_pipeline *pipeline, const gpu_ir_draw_info *draw,
                                         const gpu_ir_resource_access *accesses, uint32_t access_count,
                                         const gpu_ir_backend *backend) {
    gpu_ir_stream stream;
    gpu_ir_stream_init(&stream);
    uint32_t index = GPU_IR_NO_COMMAND;
    int rc = gpu_ir_record_draw_with_accesses(&stream, pipeline, draw, accesses, access_count, &index);
    if (rc == GPU_IR_OK && index != GPU_IR_NO_COMMAND) rc = gpu_ir_execute(&stream, index, backend);
    gpu_ir_stream_destroy(&stream);
    return rc;
}

static int replay_insert(cmd12 **commands, uint32_t *count, uint32_t at, const cmd12 *items, uint32_t nitems) {
    if (!nitems) return 1;
    if (at > *count || nitems > UINT32_MAX - *count || (size_t)(*count + nitems) > SIZE_MAX / sizeof **commands) return 0;
    uint32_t next_count = *count + nitems;
    cmd12 *next = realloc(*commands, (size_t)next_count * sizeof **commands);
    if (!next) return 0;
    memmove(next + at + nitems, next + at, (size_t)(*count - at) * sizeof *next);
    memcpy(next + at, items, (size_t)nitems * sizeof *next);
    *commands = next;
    *count = next_count;
    return 1;
}

enum {
    D3D12_INDIRECT_CPU_OK,
    D3D12_INDIRECT_CPU_COUNT_UNREADABLE,
    D3D12_INDIRECT_CPU_ARGUMENT_RANGE,
    D3D12_INDIRECT_CPU_ARGUMENT_UNREADABLE
};

// The CPU compatibility path can only decode UPLOAD buffers. Share its exact count clamp and range checks between
// replay and submission-time state validation so both interpret the same records and skip conditions.
static int d3d12_indirect_cpu_records12(const cmd12 *command, const command_signature12 *signature,
                                       const res12 *arguments, const res12 *count_buffer,
                                       uint32_t *available_out, const uint8_t **records_out) {
    if (!command || !signature || !arguments || !available_out || !records_out) return D3D12_INDIRECT_CPU_ARGUMENT_RANGE;
    uint32_t available = command->a;
    const uint8_t *records = NULL;
    if (count_buffer && available) {
        if (!count_buffer->buffer || count_buffer->heap.Type != 2 || !count_buffer->buffer.contents ||
            command->off2 > count_buffer->buffer.length || 4 > count_buffer->buffer.length - command->off2) {
            *available_out = 0;
            *records_out = NULL;
            return D3D12_INDIRECT_CPU_COUNT_UNREADABLE;
        }
        uint32_t gpu_count = 0;
        memcpy(&gpu_count, (const uint8_t *)count_buffer->buffer.contents + command->off2, sizeof gpu_count);
        if (gpu_count < available) available = gpu_count;
    }
    if (available && (!arguments->buffer || command->off > arguments->buffer.length ||
        (uint64_t)available * signature->stride > arguments->buffer.length - command->off)) {
        *available_out = 0;
        *records_out = NULL;
        return D3D12_INDIRECT_CPU_ARGUMENT_RANGE;
    }
    if (available && (arguments->heap.Type != 2 || !arguments->buffer.contents)) {
        *available_out = 0;
        *records_out = NULL;
        return D3D12_INDIRECT_CPU_ARGUMENT_UNREADABLE;
    }
    if (available) records = (const uint8_t *)arguments->buffer.contents + command->off;
    *available_out = available;
    *records_out = records;
    return D3D12_INDIRECT_CPU_OK;
}

// A preflight readback is safe only if this command list has not already issued GPU work that could have
// produced the argument or count data. Earlier command lists on this Metal queue are ordered before the
// readback command buffer; work earlier in this list is not, because replay has not submitted it yet.
static int d3d12_indirect_has_prior_gpu_work12(const list12 *list, uint32_t command_index) {
    if (!list || command_index > list->n) return 1;
    for (uint32_t i = 0; i < command_index; i++) {
        switch (list->cmds[i].op) {
        case C_PSO: case C_ROOTSIG: case C_HEAPS: case C_TABLE: case C_CONSTANTS: case C_ROOT_VA:
        case C_TOPOLOGY: case C_VB: case C_IB: case C_TARGETS: case C_VIEWPORTS: case C_SCISSORS:
        case C_BLEND_FACTOR: case C_STENCIL_REF: case C_BUNDLE_BEGIN: case C_BUNDLE_END:
            break;
        case C_GPU_IR:
            if (gpu_ir_command_kind(&list->gpu_ir, list->cmds[i].gpu_ir_index) == GPU_IR_COMMAND_BARRIER) break;
            return 1;
        default:
            // This includes draws, dispatches, clears, copies, queries, and immediate writes.
            return 1;
        }
    }
    return 0;
}

// Stage GPU-only ExecuteIndirect data during submission preflight when no earlier command in this list can
// write it. This lets the state validator inspect root/IA-updating records without executing or committing the
// list. If the list already contains GPU work, its producer must run first and replay retains its ordered fallback.
static int d3d12_indirect_preflight_gpu_records12(id<MTLCommandQueue> queue, const cmd12 *command,
        const command_signature12 *signature, const res12 *arguments, const res12 *count_buffer,
        uint32_t *available_out, const uint8_t **records_out, id<MTLBuffer> __strong *staging_out) {
    if (!queue || !command || !signature || !arguments || !arguments->buffer ||
        !available_out || !records_out || !staging_out) return 0;
    *available_out = 0;
    *records_out = NULL;
    *staging_out = nil;
    if (!command->a) return 1;

    uint64_t argument_bytes64 = (uint64_t)command->a * signature->stride;
    if (argument_bytes64 > SIZE_MAX || argument_bytes64 > NSUIntegerMax ||
        command->off > arguments->buffer.length || argument_bytes64 > arguments->buffer.length - command->off)
        return 0;
    size_t argument_bytes = (size_t)argument_bytes64;
    size_t count_offset = argument_bytes;
    size_t readback_bytes = argument_bytes;
    if (count_buffer) {
        if (!count_buffer->buffer || command->off2 > count_buffer->buffer.length ||
            sizeof(uint32_t) > count_buffer->buffer.length - command->off2 ||
            readback_bytes > SIZE_MAX - sizeof(uint32_t)) return 0;
        readback_bytes += sizeof(uint32_t);
    }
    if (!readback_bytes || readback_bytes > NSUIntegerMax) return 0;

    id<MTLBuffer> staging = [queue.device newBufferWithLength:(NSUInteger)readback_bytes
        options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    if (!staging || !command_buffer) return 0;
    id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
    if (!blit) return 0;
    [blit copyFromBuffer:arguments->buffer sourceOffset:(NSUInteger)command->off
        toBuffer:staging destinationOffset:0 size:(NSUInteger)argument_bytes];
    if (count_buffer) {
        [blit copyFromBuffer:count_buffer->buffer sourceOffset:(NSUInteger)command->off2
            toBuffer:staging destinationOffset:count_offset size:sizeof(uint32_t)];
    }
    [blit endEncoding];
    [command_buffer commit];
    { uint64_t sync_t0 = fs_now12(); [command_buffer waitUntilCompleted]; FS_ADD12(sync_n, 1); FS_ADD12(sync_ns, fs_now12() - sync_t0); }
    if (command_buffer.status != MTLCommandBufferStatusCompleted || !staging.contents) return 0;

    uint32_t available = command->a;
    if (count_buffer) {
        uint32_t gpu_count = 0;
        memcpy(&gpu_count, (const uint8_t *)staging.contents + count_offset, sizeof gpu_count);
        if (gpu_count < available) available = gpu_count;
    }
    *available_out = available;
    *records_out = (const uint8_t *)staging.contents;
    *staging_out = staging;
    return 1;
}

// Root/IA-changing ExecuteIndirect signatures need their records on the CPU for the current Metal lowering.
// When D3D12 puts those records in DEFAULT resources, stage them through an ordered Metal blit and wait for that
// blit before expanding the signature. This is a compatibility fallback; fixed draw/dispatch signatures stay GPU-side.
static int d3d12_indirect_gpu_records12(dev12 *device, id<MTLCommandQueue> queue,
                                       id<MTLCommandBuffer> __strong *command_buffer,
                                       id<MTLCommandBuffer> *outer_command_buffer,
                                       id<MTLComputeCommandEncoder> __strong *compute_encoder,
                                       id<MTLBlitCommandEncoder> __strong *blit_encoder,
                                       id<MTLRenderCommandEncoder> __strong *render_encoder,
                                       gfx_state *graphics_state, const cmd12 *command,
                                       const command_signature12 *signature, const res12 *arguments,
                                       const res12 *count_buffer, uint32_t *available_out,
                                       const uint8_t **records_out, id<MTLBuffer> __strong *readback_out) {
    if (!device || !queue || !command_buffer || !*command_buffer || !outer_command_buffer || !command ||
        !signature || !arguments || !arguments->buffer || !available_out || !records_out || !readback_out)
        return 0;
    *available_out = 0;
    *records_out = NULL;
    *readback_out = nil;
    if (!command->a) return 1;

    uint64_t argument_bytes64 = (uint64_t)command->a * signature->stride;
    if (argument_bytes64 > SIZE_MAX || argument_bytes64 > NSUIntegerMax) return 0;
    size_t argument_bytes = (size_t)argument_bytes64;
    size_t count_offset = argument_bytes;
    size_t readback_bytes = argument_bytes;
    if (count_buffer) {
        if (readback_bytes > SIZE_MAX - sizeof(uint32_t)) return 0;
        readback_bytes += sizeof(uint32_t);
    }
    if (!readback_bytes || readback_bytes > NSUIntegerMax) return 0;

    id<MTLBuffer> staging = [device->mtl newBufferWithLength:(NSUInteger)readback_bytes
        options:MTLResourceStorageModeShared];
    if (!staging) return 0;

    id<MTLCommandBuffer> next_command_buffer = [queue commandBuffer];
    if (!next_command_buffer) return 0;

    id<MTLCommandBuffer> current = *command_buffer;
    close_pass(device, graphics_state, current, render_encoder);
    end_encoders(compute_encoder, blit_encoder, render_encoder);
    *blit_encoder = [current blitCommandEncoder];
    if (!*blit_encoder) return 0;
    [*blit_encoder copyFromBuffer:arguments->buffer sourceOffset:(NSUInteger)command->off
        toBuffer:staging destinationOffset:0 size:(NSUInteger)argument_bytes];
    if (count_buffer) {
        [*blit_encoder copyFromBuffer:count_buffer->buffer sourceOffset:(NSUInteger)command->off2
            toBuffer:staging destinationOffset:count_offset size:sizeof(uint32_t)];
    }
    [*blit_encoder endEncoding];
    *blit_encoder = nil;
    [current commit];
    { uint64_t sync_t0 = fs_now12(); [current waitUntilCompleted]; FS_ADD12(sync_n, 1); FS_ADD12(sync_ns, fs_now12() - sync_t0); }
    if (current.status != MTLCommandBufferStatusCompleted) {
        vx_log("D3D12: Metal command buffer failed while staging ExecuteIndirect records: %s",
               current.error.localizedDescription.UTF8String ?: "unknown Metal error");
        return -1;
    }

    uint32_t available = command->a;
    if (count_buffer) {
        uint32_t gpu_count = 0;
        memcpy(&gpu_count, (const uint8_t *)staging.contents + count_offset, sizeof gpu_count);
        if (gpu_count < available) available = gpu_count;
    }
    *available_out = available;
    *records_out = (const uint8_t *)staging.contents;
    *readback_out = staging;
    *command_buffer = next_command_buffer;
    *outer_command_buffer = next_command_buffer;
    return 1;
}

typedef struct { dev12 *device; void *command_buffer; } d3d12_clear_context12;

static int d3d12_gpu_ir_clear_color12(void *opaque, const gpu_ir_resource *resource,
                                     const gpu_ir_clear_color_info *clear) {
    d3d12_clear_context12 *context = opaque;
    if (!context || !context->device || !context->device->common_metal || !context->command_buffer)
        return GPU_IR_INVALID;
    return gpu_ir_metal_encode_clear_color(context->device->common_metal, context->command_buffer,
                                           resource, clear);
}

static int d3d12_gpu_ir_clear_depth_stencil12(void *opaque, const gpu_ir_resource *resource,
                                              const gpu_ir_clear_depth_stencil_info *clear) {
    d3d12_clear_context12 *context = opaque;
    if (!context || !context->device || !context->device->common_metal || !context->command_buffer)
        return GPU_IR_INVALID;
    return gpu_ir_metal_encode_clear_depth_stencil(context->device->common_metal, context->command_buffer,
                                                    resource, clear);
}

static int replay_indirect_override_reserve12(d3d12_indirect_validation_override12 **overrides,
        uint32_t *capacity, uint32_t need) {
    if (need <= *capacity) return 1;
    uint32_t next = *capacity ? *capacity : 4;
    while (next < need) {
        if (next > UINT32_MAX / 2) { next = need; break; }
        next *= 2;
    }
    d3d12_indirect_validation_override12 *grown = realloc(*overrides, (size_t)next * sizeof **overrides);
    if (!grown) return 0;
    *overrides = grown;
    *capacity = next;
    return 1;
}

static int replay(dev12 *d, id<MTLCommandQueue> queue, int32_t queue_type, list12 *l,
                  resource_state_shadow12 **state_shadows, uint32_t *state_count,
                  id<MTLCommandBuffer> *command_buffer, scratch *sc) {
    id<MTLCommandBuffer> cb = *command_buffer;
    uint32_t command_count = l->n;
    cmd12 *commands = command_count ? malloc((size_t)command_count * sizeof *commands) : NULL;
    if (command_count && !commands) { vx_log("D3D12: could not copy a command list for replay"); return 0; }
    if (command_count) {
        memcpy(commands, l->cmds, (size_t)command_count * sizeof *commands);
        for (uint32_t i = 0; i < command_count; i++) commands[i].replay_source_index = i;
    }
    pso12 *pso = NULL;
    heap12 *heap = NULL, *sheap = NULL;
    root_binding *compute = calloc(1, sizeof *compute), *graphics = calloc(1, sizeof *graphics);
    gfx_state *g = calloc(1, sizeof *g);
    if (!compute || !graphics || !g) {
        vx_log("D3D12: could not allocate command-list replay state");
        gfx_access_cache_release12(g); free(commands); free(compute); free(graphics); free(g);
        return 0;
    }
    g->prim = MTLPrimitiveTypeTriangle;
    for (int k = 0; k < 4; k++) g->blend[k] = 1;
    id<MTLComputeCommandEncoder> ce = nil;
    id<MTLBlitCommandEncoder> be = nil;
    id<MTLRenderCommandEncoder> re = nil;
    d3d12_indirect_validation_override12 *indirect_overrides = NULL;
    uint32_t indirect_override_count = 0, indirect_override_capacity = 0;
    NSMutableArray<id<MTLBuffer>> *indirect_staging_buffers = [NSMutableArray array];
    int replay_failed = 0;
    pthread_mutex_lock(&d->lock);
    // (with a residency set, encoders declare nothing beyond what draws and dispatches bind)
    residency_commit12(d);
    NSArray *resident = d->residency ? resident_by_set12() : [d->resources copy];
    pthread_mutex_unlock(&d->lock);
    for (uint32_t i = 0; i < command_count; i++) {
        cmd12 *c = &commands[i];
        root_binding *rb = c->gfx ? graphics : compute;
        switch (c->op) {
        case C_BUNDLE_BEGIN:
            // Bundles inherit graphics bindings and targets, but start with undefined PSO and topology.
            pso = NULL;
            g->prim = (MTLPrimitiveType)-1;
            break;
        case C_BUNDLE_END:
            // State set by the bundle remains active on the parent command list.
            break;
        case C_PSO: pso = c->obj; break;
        case C_ROOTSIG: rb->rs = c->obj; memset(&rb->st, 0, sizeof rb->st); break;
        case C_HEAPS: heap = c->obj; sheap = c->obj2; break;
        case C_TABLE: if (c->a < 64) rb->st.table[c->a] = c->va; break;
        case C_CONSTANTS: if (c->a < 64) for (uint32_t k = 0; k < c->b && c->c + k < 64; k++) rb->st.constants[c->a][c->c + k] = c->values[k]; break;
        case C_ROOT_VA: if (c->a < 64) rb->st.root_va[c->a] = c->va; break;
        case C_TOPOLOGY: {
            // D3D_PRIMITIVE_TOPOLOGY: 1 points, 2 lines, 3 line strip, 4 triangles, 5 triangle strip
            uint32_t t = c->a;
            g->topology = t;
            g->prim = t == 1 ? MTLPrimitiveTypePoint : t == 2 ? MTLPrimitiveTypeLine : t == 3 ? MTLPrimitiveTypeLineStrip : t == 5 ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle;
            if (t > 5 && !(pso && pso->gs_kernel) && !(t >= 33 && t <= 64)) vx_log("D3D12: primitive topology %u is not supported", t);
            break;
        }
        case C_VB: for (uint32_t k = 0; k < c->b && c->a + k < 16; k++) memcpy(&g->vb[c->a + k], (VBV *)c->values + k, sizeof(VBV)); break;
        case C_IB: memcpy(&g->ib, c->values, sizeof g->ib); break;
        case C_VIEWPORTS: g->nvp = c->a; memcpy(g->vp, c->values, c->a * sizeof(VIEWPORT)); break;
        case C_SCISSORS: g->nsc = c->a; memcpy(g->sc, c->values, c->a * sizeof(RECT12)); break;
        case C_BLEND_FACTOR: memcpy(g->blend, c->values, 16); break;
        case C_STENCIL_REF: g->stencil_ref = c->a; break;
        case C_BEGIN_QUERY: {
            queryheap12 *q = c->obj;
            if (q->type != 0 || c->b >= q->count) break;           // (only occlusion queries are begun; others read as zeros)
            g->occ = q;
            g->occ_on = 1;
            g->occ_fresh = 1;
            g->occ_index = c->b;
            g->occ_binary = c->a == 1;
            if (re && g->pass_heap != q) g->changed = 1;           // (the next draw opens a pass counting into this heap)
            else if (re) { [re setVisibilityResultMode:MTLVisibilityResultModeCounting offset:8 * c->b]; pass_query(g); }
            break;
        }
        case C_END_QUERY: {
            queryheap12 *q = c->obj;
            if (q->type == 0) {
                if (re && g->occ_on && g->pass_heap == q) [re setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
                if (g->occ_on && g->occ_fresh && c->b < q->count) {   // (no pass counted it: zero)
                    if (re && g->pass_heap == q) pass_query(g);
                    else {
                        if (ce) { [ce endEncoding]; ce = nil; }
                        close_pass(d, g, cb, &re);
                        if (!be) be = [cb blitCommandEncoder];
                        [be fillBuffer:q->results range:NSMakeRange(8 * (NSUInteger)c->b, 8) value:0];
                    }
                }
                g->occ_on = 0;
            } else if (q->type == 5 && q->samples && c->b < q->count) {
                // Copy-queue timestamps mark the point between blit operations. Metal only exposes
                // those points on devices advertising AtBlitBoundary sampling.
                close_pass(d, g, cb, &re);
                if (ce) { [ce endEncoding]; ce = nil; }
                if (!be) be = [cb blitCommandEncoder];
                [be sampleCountersInBuffer:q->samples atSampleIndex:c->b withBarrier:YES];
            } else if (q->samples && c->b < q->count) {
                // A timestamp: when the work before it has finished; Apple GPUs sample only where an encoder starts
                // or ends, so an encoder of its own ends here and samples at its end.
                close_pass(d, g, cb, &re); end_encoders(&ce, &be, &re);
                g->changed = 1;
                MTLBlitPassDescriptor *bp = [MTLBlitPassDescriptor blitPassDescriptor];
                bp.sampleBufferAttachments[0].sampleBuffer = q->samples;
                bp.sampleBufferAttachments[0].startOfEncoderSampleIndex = MTLCounterDontSample;
                bp.sampleBufferAttachments[0].endOfEncoderSampleIndex = c->b;
                id<MTLBlitCommandEncoder> tb = [cb blitCommandEncoderWithDescriptor:bp];
                [tb fillBuffer:q->results range:NSMakeRange(0, 4) value:0];   // (an encoder with no work may be dropped)
                [tb endEncoding];
            }
            break;
        }
        case C_RESOLVE_QUERY: {
            queryheap12 *q = c->obj;
            res12 *dst = c->obj2;
            if (!dst || !dst->buffer || c->b + c->c > q->count || c->off + (uint64_t)c->c * q->stride > dst->buffer.length) {
                vx_log("D3D12: ResolveQueryData outside the heap or the buffer");
                break;
            }
            if (ce) { [ce endEncoding]; ce = nil; }
            close_pass(d, g, cb, &re);
            if (q->type == 5 && be) { [be endEncoding]; be = nil; }
            if (!be) be = [cb blitCommandEncoder];
            if (q->samples) [be resolveCounters:q->samples inRange:NSMakeRange(c->b, c->c) destinationBuffer:dst->buffer destinationOffset:c->off];
            else {
                [be copyFromBuffer:q->results sourceOffset:(uint64_t)c->b * q->stride toBuffer:dst->buffer destinationOffset:c->off size:(uint64_t)c->c * q->stride];
            }
            break;
        }
        case C_TARGETS:
            for (int k = 0; k < 9; k++) {
                g->tex[k] = c->tex[k];       // (the command holds them)
                g->resource[k] = c->resource[k];
            }
            g->changed = 1;
            break;
        case C_CLEAR_UAV: {
            cb_log12('u', 0, c->a ? 1 : 2, 0, 0);
            uint32_t descriptor_index = 0;
            heap12 *descriptor_heap = heap_gpu_descriptor(c->va, &descriptor_index);
            gpu_ir_buffer_view *buffer_view = descriptor_heap && descriptor_heap->buffer_view &&
                descriptor_index < descriptor_heap->desc.NumDescriptors ? descriptor_heap->buffer_view[descriptor_index] : NULL;
            const gpu_ir_buffer_view_desc *buffer_desc = buffer_view ? gpu_ir_buffer_view_description(buffer_view) : NULL;
            int raw_buffer = buffer_desc && buffer_desc->kind == GPU_IR_BUFFER_VIEW_RAW;
            uint8_t typed_pattern[GPU_IR_MAX_FILL_PATTERN_SIZE];
            uint32_t typed_pattern_size = 0;
            int typed_clear = !c->a && !c->b &&
                d3d12_typed_uav_clear_pattern12(descriptor_heap, descriptor_index, buffer_desc,
                                                 c->values, typed_pattern, &typed_pattern_size);
            close_pass(d, g, cb, &re);
            if (raw_buffer || typed_clear) {
                end_encoders(&ce, &be, &re);
                if (raw_buffer && (c->a || c->b)) {
                    vx_log("D3D12: raw-buffer ClearUnorderedAccessView requires Uint values and no rectangles");
                    replay_failed = 1;
                    break;
                }
                be = [cb blitCommandEncoder];
                if (!be) {
                    vx_log("D3D12: could not create a Metal blit encoder for a buffer UAV clear");
                    replay_failed = 1;
                    break;
                }
                int rc = raw_buffer ? d3d12_gpu_ir_execute_fill_buffer(d, sc, be,
                    (gpu_ir_resource *)gpu_ir_buffer_view_resource(buffer_view), buffer_desc->byte_offset,
                    buffer_desc->byte_size, c->values[0]) : d3d12_gpu_ir_execute_fill_buffer_pattern(d, sc, be,
                    (gpu_ir_resource *)gpu_ir_buffer_view_resource(buffer_view), buffer_desc->byte_offset,
                    buffer_desc->byte_size, typed_pattern, typed_pattern_size);
                if (rc != GPU_IR_OK) {
                    const char *clear_kind = raw_buffer ? "raw-buffer" : "typed-buffer";
                    vx_log("D3D12: %s UAV clear failed through common GPU IR (error %d)", clear_kind, rc);
                    replay_failed = 1;
                }
                break;
            }
            if (be) { [be endEncoding]; be = nil; }
            if (!ce) ce = new_compute12(cb);
            if (!ce || !d3d12_clear_uav_encode12(d, cb, &ce, c, heap)) {
                vx_log("D3D12: could not execute ClearUnorderedAccessView%s", c->a ? "Float" : "Uint");
                replay_failed = 1;
            }
            break;
        }
        case C_CLEAR_RTV: case C_CLEAR_DSV: {
            cb_log12(c->op == C_CLEAR_RTV ? 'r' : 'd', 0, 0, 0, 0);
            // Clears are common IR operations so later API front ends can share attachment semantics and backend hooks.
            close_pass(d, g, cb, &re); end_encoders(&ce, &be, &re);
            d3d12_clear_context12 context = { d, (__bridge void *)cb };
            gpu_ir_backend backend = { .context = &context, .clear_color = d3d12_gpu_ir_clear_color12,
                                       .clear_depth_stencil = d3d12_gpu_ir_clear_depth_stencil12 };
            if (!c->gpu_ir_recorded) break;
            int rc = gpu_ir_execute(&l->gpu_ir, c->gpu_ir_index, &backend);
            if (rc != GPU_IR_OK) {
                vx_log("D3D12: common GPU IR could not execute ClearRenderTargetView or ClearDepthStencilView (error %d)", rc);
                replay_failed = 1;
            }
            g->changed = 1;
            break;
        }
        case C_DRAW: case C_DRAW_INDEXED: {
            if (!pso || !pso->render) { vx_log("D3D12: a draw without a graphics pipeline state"); break; }
            if (pso->gs_kernel) {
                cb_log12('G', pso->ps_code, 0, 0, 0);
                gs12_draw(d, g, pso, c, cb, &ce, &be, &re, resident, heap, sheap, graphics, sc);
                break;
            }
            if (pso->hs_kernel) {
                cb_log12('T', pso->ps_code, 0, 0, 0);
                tess12_draw(d, g, pso, c, cb, &ce, &be, &re, resident, heap, sheap, graphics, sc);
                break;
            }
            if (!open_render_pass12(d, g, cb, &ce, &be, &re, resident, heap)) {
                vx_log("D3D12: could not open a render pass for a draw");
                break;
            }
            if ((g_skip_count12 || g_census12) && skipped_shader12(pso->ps_code, 'p')) break;
            pass_dump_draw12(pso);
            if (!t_cb_len12 || t_cb_log12[t_cb_len12 - 10] != 'p') cb_log12('p', pso->ps_code, 0, 0, 0);
            d3d12_draw_context context = {
                .device = d, .render_encoder = &re, .graphics_state = g, .api_pipeline = pso,
                .resource_heap = heap, .sampler_heap = sheap, .graphics_root = graphics, .scratch_buffer = sc
            };
            gpu_ir_backend backend = { .context = &context, .draw = d3d12_gpu_ir_draw };
            gpu_ir_draw_info draw = {
                .topology = gpu_ir_topology_from_metal12(g->prim), .indexed = c->op == C_DRAW_INDEXED,
                .instance_count = c->b, .first_instance = c->op == C_DRAW ? (uint32_t)c->off : (uint32_t)c->off2,
                .has_blend_constants = 1
            };
            memcpy(draw.blend_constants, g->blend, sizeof draw.blend_constants);
            if (draw.indexed) {
                draw.index_count = c->a;
                draw.first_index = c->c;
                draw.vertex_offset = (int32_t)(int64_t)c->off;
            } else {
                draw.vertex_count = c->a;
                draw.first_vertex = c->c;
            }
            gpu_ir_access_list12 accesses = { 0 };
            pthread_rwlock_rdlock(&d->bufs_lock);   // (collection looks buffers up by address: their registry's lock)
            int access_rc = gpu_ir_collect_draw_accesses_cached12(d, pso, g, graphics, heap, sheap, &accesses, draw.indexed);
            pthread_rwlock_unlock(&d->bufs_lock);
            int rc;
            if (access_rc == GPU_IR_OK) {
                if (c->gpu_ir_recorded)
                    rc = gpu_ir_execute_with_accesses(&l->gpu_ir, c->gpu_ir_index, &backend, accesses.values, accesses.count);
                else if (c->gpu_ir_transient) {
                    rc = list_capture_replay_draw_buffers12(l, g->vb, &g->ib, &draw);
                    if (rc == GPU_IR_OK)
                        rc = gpu_ir_execute_transient_draw(pso->gpu_pipeline, &draw, accesses.values,
                                                           accesses.count, &backend);
                    list_release_captured_draw_buffers12(&draw);
                }
                else
                    rc = d3d12_gpu_ir_draw(&context, pso->gpu_pipeline, &draw, accesses.values, accesses.count);
            } else {
                vx_log("D3D12: common IR could not collect draw resource accesses (error %d); keeping conservative Metal residency",
                       access_rc);
                if (c->gpu_ir_recorded) rc = gpu_ir_execute(&l->gpu_ir, c->gpu_ir_index, &backend);
                else rc = d3d12_gpu_ir_draw(&context, pso->gpu_pipeline, &draw, NULL, 0);
            }
            if (g_dump_armed12) pass_dump_srvs12(pso, accesses.values, accesses.count);
            if (trace_len12()) trace_writer12("draw with pixel shader", pso->ps_code, accesses.values, accesses.count);
            gpu_ir_access_list_release12(&accesses);
            if (rc != GPU_IR_OK) { FS_ADD12(skipped, 1); vx_log("D3D12: common GPU IR draw failed (error %d)", rc); }
            break;
        }
        case C_DISPATCH: {
            if (g_prof_armed12 && ce) { [ce endEncoding]; ce = nil; }   // (GPU profile: a compute encoder per dispatch)
            if (!pso || !pso->compute) { vx_log("D3D12: Dispatch without a compute pipeline state"); break; }
            if ((g_skip_count12 || g_census12) && skipped_shader12(pso->cs_code, 'c')) break;
            cb_log12('c', pso->cs_code, c->a, c->b, c->c);
            int mapping_result = d3d12_shader_component_mapping_read12(d, &pso->msl, GPU_IR_STAGE_COMPUTE,
                                                                        compute, heap, sheap);
            if (mapping_result != GPU_IR_OK) {
                FS_ADD12(skipped, 1);
                static volatile int told;
                if (__sync_add_and_fetch(&told, 1) <= 20)
                    vx_log("D3D12: dispatch of pipeline %016llx skipped: its component mapping could not be read (error %d)",
                           (unsigned long long)pso->desc_hash, mapping_result);
                break;
            }
            if (c->gpu_ir_recorded || c->gpu_ir_transient) {
                d3d12_dispatch_context context = {
                    .device = d, .command_buffer = (__bridge void *)cb,
                    .compute_encoder = &ce, .blit_encoder = &be, .render_encoder = &re,
                    .resident_resources = (__bridge void *)resident, .graphics_state = g,
                    .api_pipeline = pso, .resource_heap = heap, .sampler_heap = sheap,
                    .compute_root = compute, .scratch_buffer = sc
                };
                gpu_ir_backend backend = { .context = &context, .dispatch = d3d12_gpu_ir_dispatch };
                gpu_ir_access_list12 accesses = { 0 };
                pthread_rwlock_rdlock(&d->bufs_lock);   // (collection looks buffers up by address: their registry's lock)
                int access_rc = gpu_ir_collect_shader_accesses12(d, &pso->msl, GPU_IR_STAGE_COMPUTE,
                                                                  compute, heap, sheap, &accesses);
                pthread_rwlock_unlock(&d->bufs_lock);
                int rc;
                if (access_rc == GPU_IR_OK) {
                    if (c->gpu_ir_recorded)
                        rc = gpu_ir_execute_with_accesses(&l->gpu_ir, c->gpu_ir_index, &backend,
                                                          accesses.values, accesses.count);
                    else
                        rc = gpu_ir_execute_transient_dispatch(pso->gpu_pipeline, c->a, c->b, c->c,
                                                               accesses.values, accesses.count, &backend);
                } else {
                    vx_log("D3D12: common IR could not collect dispatch resource accesses (error %d); keeping conservative Metal residency",
                           access_rc);
                    if (c->gpu_ir_recorded) rc = gpu_ir_execute(&l->gpu_ir, c->gpu_ir_index, &backend);
                    else rc = d3d12_gpu_ir_dispatch(&context, pso->gpu_pipeline, c->a, c->b, c->c, NULL, 0);
                }
                if (g_dump_armed12) pass_dump_dispatch12(cb, ce, pso, accesses.values, accesses.count, c->a, c->b, c->c);
                if (trace_cs12(pso->cs_code)) trace_cs_accesses12(pso->cs_code, c->a, c->b, c->c, accesses.values, accesses.count);
                if (trace_len12())                                 // (VXRT_D3D12_TRACE_LEN: who writes buffers that long)
                    for (uint32_t k = 0; k < accesses.count; k++) {
                        if (!(accesses.values[k].access_mask & GPU_IR_ACCESS_SHADER_WRITE)) continue;
                        id obj = (__bridge id)gpu_ir_backend_object(accesses.values[k].resource);
                        if ([obj conformsToProtocol:@protocol(MTLBuffer)] && ((id<MTLBuffer>)obj).length == trace_len12()) {
                            static volatile int told;
                            if (__sync_add_and_fetch(&told, 1) <= 40)
                                vx_log("D3D12: dispatch %ux%ux%u of cs %016llx writes the %llu-byte buffer %#llx", c->a, c->b, c->c,
                                       (unsigned long long)pso->cs_code, (unsigned long long)trace_len12(), (unsigned long long)((id<MTLBuffer>)obj).gpuAddress);
                        }
                    }
                gpu_ir_access_list_release12(&accesses);
                if (rc != GPU_IR_OK) { FS_ADD12(skipped, 1); } if (rc != GPU_IR_OK) vx_log("D3D12: common GPU IR dispatch failed (error %d)", rc);
                break;
            }
            if (be) { [be endEncoding]; be = nil; }
            close_pass(d, g, cb, &re);
            if (!ce) {
                ce = new_compute12(cb);
                // (what the shaders reach through pointers and argument buffers must be resident: every resource
                //  of the device, and the texture views the bound heaps' descriptors name)
                use_resident_compute12(ce, resident, heap);
            }
            heap_textures_compute12(ce, heap, msl_wide12(&pso->msl));
            [ce setComputePipelineState:pso->compute];
            uint64_t root[64 + 2];
            uint32_t words;
            stage_root(d, &pso->msl, compute, heap, sheap, sc, root, &words);
            [ce setBytes:root length:8 * words atIndex:0];
            if (sc->buf) [ce useResource:sc->buf usage:MTLResourceUsageRead];
            [ce dispatchThreadgroups:MTLSizeMake(c->a, c->b, c->c) threadsPerThreadgroup:MTLSizeMake(pso->msl.threads[0], pso->msl.threads[1], pso->msl.threads[2])];
            if (g_dump_armed12) vx_log("D3D12: pass dump %d: dispatch %ux%ux%u of cs %016llx (direct path, its writes not shown)", g_dump_frame12,
                                       c->a, c->b, c->c, (unsigned long long)pso->cs_code);
            if (trace_len12() || trace_cs12(pso->cs_code)) {
                gpu_ir_access_list12 acc = { 0 };
                pthread_rwlock_rdlock(&d->bufs_lock);   // (collection looks buffers up by address: their registry's lock)
                if (gpu_ir_collect_shader_accesses12(d, &pso->msl, GPU_IR_STAGE_COMPUTE, compute, heap, sheap, &acc) == GPU_IR_OK)
                {
                    if (trace_len12()) trace_writer12("direct dispatch of cs", pso->cs_code, acc.values, acc.count);
                    if (trace_cs12(pso->cs_code)) trace_cs_accesses12(pso->cs_code, c->a, c->b, c->c, acc.values, acc.count);
                }
                pthread_rwlock_unlock(&d->bufs_lock);
                gpu_ir_access_list_release12(&acc);
            }
            // (the next dispatch sees this one's writes, as D3D12's UAV barriers promise; resources reached through
            //  argument buffers are not tracked by Metal)
            if (!g_no_dispatch_barrier12) [ce memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
            break;
        }
        case C_EXECUTE_INDIRECT: {
            if (g_prof_armed12 && ce) { [ce endEncoding]; ce = nil; }   // (GPU profile: a compute encoder per dispatch)
            command_signature12 *sig = c->obj;
            if (g_skip_count12 && pso && skipped_shader12(sig && sig->gfx ? pso->ps_code : pso->cs_code, sig && sig->gfx ? 'I' : 'i')) break;
            cb_log12(sig && sig->gfx ? 'I' : 'i', pso ? (sig && sig->gfx ? pso->ps_code : pso->cs_code) : 0, c->a, 0, 0);
            res12 *arg_buffer = c->obj2, *count_buffer = c->obj3;
            if (!sig) { vx_log("D3D12: ExecuteIndirect has no command signature"); break; }
            if (sig->gfx && pso && pso->gs_kernel) {
                static int told;
                if (!told++) vx_log("D3D12: ExecuteIndirect draws with a geometry shader are not done yet; skipped");
                break;
            }
            root_binding *indirect_root = sig->gfx ? graphics : compute;
            uint32_t indirect_count = c->a;
            if (getenv("VXRT_D3D12_TRACE_INDIRECT")) {
                static uint64_t seen[256]; static int nseen;
                uint64_t key = (pso ? pso->cs_code ^ pso->ps_code : 0) ^ ((uint64_t)c->a << 1) ^ (count_buffer ? 1 : 0);
                int dup = 0;
                for (int k = 0; k < nseen; k++) if (seen[k] == key) dup = 1;
                if (!dup && nseen < 256) seen[nseen++] = key;
                if (!dup)
                    vx_log("D3D12: ExecuteIndirect %s of %016llx: max %u, count buffer %d, signature %u args (first type %d), payload %u, root %d, recorded %d",
                           sig->gfx ? "draw" : "dispatch", pso ? (unsigned long long)(sig->gfx ? pso->ps_code : pso->cs_code) : 0ull, c->a,
                           count_buffer != NULL, sig->count, sig->count ? sig->args[0].type : -1, sig->payload_size, sig->root != NULL, c->gpu_ir_recorded);
            }
            if (!sig || !arg_buffer || !arg_buffer->buffer ||
                (sig->root && indirect_root->rs != sig->root)) {
                vx_log("D3D12: ExecuteIndirect has an invalid command signature, argument buffer, or root signature");
                break;
            }
            // A fixed-signature dispatch range stays on the GPU, including count-buffer clamping. Keep the broader
            // root-update path below for signatures that cannot be represented by the API-neutral range operation.
            if (indirect_count && (!count_buffer || count_buffer->gpu_buffer) && !sig->gfx && !sig->root && sig->count == 1 &&
                sig->payload_size == 12 && sig->args[0].type == INDIRECT_DISPATCH &&
                sig->args[0].payload_offset == 0 && c->gpu_ir_recorded && pso && pso->compute &&
                pso->gpu_pipeline && arg_buffer->gpu_buffer) {
                d3d12_dispatch_context context = {
                    .device = d, .command_buffer = (__bridge void *)cb,
                    .compute_encoder = &ce, .blit_encoder = &be, .render_encoder = &re,
                    .resident_resources = (__bridge void *)resident, .graphics_state = g,
                    .api_pipeline = pso, .resource_heap = heap, .sampler_heap = sheap,
                    .compute_root = compute, .scratch_buffer = sc
                };
                gpu_ir_backend backend = { .context = &context,
                    .dispatch_indirect_range = d3d12_gpu_ir_dispatch_indirect_range };
                gpu_ir_access_list12 accesses = { 0 };
                pthread_rwlock_rdlock(&d->bufs_lock);   // (collection looks buffers up by address: their registry's lock)
                int access_rc = gpu_ir_collect_shader_accesses12(d, &pso->msl, GPU_IR_STAGE_COMPUTE,
                                                                  indirect_root, heap, sheap, &accesses);
                if (trace_len12()) trace_writer12("indirect dispatch of cs", pso->cs_code, accesses.values, accesses.count);
                pthread_rwlock_unlock(&d->bufs_lock);
                int rc = access_rc == GPU_IR_OK ?
                    gpu_ir_execute_with_accesses(&l->gpu_ir, c->gpu_ir_index, &backend,
                                                 accesses.values, accesses.count) : access_rc;
                gpu_ir_access_list_release12(&accesses);
                if (rc == GPU_IR_OK) {
                    if (vx_trace) vx_log("D3D12: ExecuteIndirect %u dispatch records%s used GPU-side Metal indirect arguments",
                                          indirect_count, count_buffer ? " with count-buffer clamping" : "");
                    break;
                }
                if (access_rc != GPU_IR_OK) {
                    vx_log("D3D12: native indirect-dispatch access collection failed (error %d); using conservative residency",
                           access_rc);
                    rc = gpu_ir_execute(&l->gpu_ir, c->gpu_ir_index, &backend);
                    if (rc == GPU_IR_OK) break;
                }
                vx_log("D3D12: native indirect-dispatch lowering failed (error %d); using the compatibility path", rc);
            }
            if (indirect_count && (!count_buffer || count_buffer->gpu_buffer) && sig->gfx && !sig->root && sig->count == 1 &&
                (sig->args[0].type == INDIRECT_DRAW || sig->args[0].type == INDIRECT_DRAW_INDEXED) &&
                sig->payload_size == sig->args[0].size && c->gpu_ir_recorded && arg_buffer->gpu_buffer &&
                pso && pso->render && pso->gpu_pipeline) {
                int indexed = sig->args[0].type == INDIRECT_DRAW_INDEXED;
                if (!open_render_pass12(d, g, cb, &ce, &be, &re, resident, heap)) {
                    vx_log("D3D12: could not open a render pass for ExecuteIndirect");
                    break;
                }
                d3d12_draw_context context = {
                    .device = d, .command_buffer = (__bridge void *)cb,
                    .compute_encoder = &ce, .blit_encoder = &be,
                    .resident_resources = (__bridge void *)resident,
                    .render_encoder = &re, .graphics_state = g, .api_pipeline = pso,
                    .resource_heap = heap, .sampler_heap = sheap, .graphics_root = graphics, .scratch_buffer = sc
                };
                gpu_ir_backend backend = { .context = &context, .draw_indirect = d3d12_gpu_ir_draw_indirect };
                gpu_ir_access_list12 accesses = { 0 };
                pthread_rwlock_rdlock(&d->bufs_lock);   // (collection looks buffers up by address: their registry's lock)
                int access_rc = gpu_ir_collect_draw_accesses12(d, pso, g, graphics, heap, sheap, &accesses, indexed);
                pthread_rwlock_unlock(&d->bufs_lock);
                int rc = access_rc == GPU_IR_OK ?
                    gpu_ir_execute_with_accesses(&l->gpu_ir, c->gpu_ir_index, &backend,
                                                 accesses.values, accesses.count) : access_rc;
                gpu_ir_access_list_release12(&accesses);
                if (rc == GPU_IR_OK) {
                    if (vx_trace) vx_log("D3D12: ExecuteIndirect %u draw records used Metal indirect arguments without CPU readback",
                                          indirect_count);
                    break;
                }
                if (access_rc != GPU_IR_OK) {
                    vx_log("D3D12: native indirect-draw access collection failed (error %d); using conservative residency",
                           access_rc);
                    rc = gpu_ir_execute(&l->gpu_ir, c->gpu_ir_index, &backend);
                    if (rc == GPU_IR_OK) break;
                }
                vx_log("D3D12: native indirect draw lowering failed (error %d); using the compatibility path", rc);
            }
            uint32_t available = 0;
            const uint8_t *data = NULL;
            id<MTLBuffer> __strong indirect_readback = nil;
            int gpu_records_readback = 0;
            int cpu_status = d3d12_indirect_cpu_records12(c, sig, arg_buffer, count_buffer, &available, &data);
            if (cpu_status == D3D12_INDIRECT_CPU_COUNT_UNREADABLE ||
                cpu_status == D3D12_INDIRECT_CPU_ARGUMENT_UNREADABLE) {
                int readback_status = d3d12_indirect_gpu_records12(d, queue, &cb, command_buffer,
                    &ce, &be, &re, g, c, sig, arg_buffer, count_buffer,
                    &available, &data, &indirect_readback);
                if (readback_status > 0) {
                    cpu_status = D3D12_INDIRECT_CPU_OK;
                    gpu_records_readback = 1;
                    if (vx_trace) vx_log("D3D12: staged %u ExecuteIndirect records from GPU-only buffers for compatibility replay",
                                          available);
                } else if (readback_status < 0) {
                    replay_failed = 1;
                    break;
                } else {
                    available = 0;
                    data = NULL;
                    vx_log("D3D12: ExecuteIndirect GPU readback allocation failed; skipping records and applying signature resets");
                }
            } else if (cpu_status == D3D12_INDIRECT_CPU_ARGUMENT_RANGE) {
                vx_log("D3D12: ExecuteIndirect argument buffer is out of range");
            }
            int changes_indirect_state = sig->root != NULL;
            for (uint32_t argument_index = 0; argument_index < sig->count; argument_index++)
                if (sig->args[argument_index].type == INDIRECT_VERTEX_BUFFER ||
                    sig->args[argument_index].type == INDIRECT_INDEX_BUFFER) changes_indirect_state = 1;
            if (gpu_records_readback && changes_indirect_state && available) {
                int validation_ready = c->replay_source_index != UINT32_MAX && state_shadows && state_count &&
                    replay_indirect_override_reserve12(&indirect_overrides, &indirect_override_capacity,
                                                       indirect_override_count + 1);
                if (validation_ready) {
                    indirect_overrides[indirect_override_count++] = (d3d12_indirect_validation_override12){
                        .command_index = c->replay_source_index, .available = available, .records = data };
                    resource_state_shadow12 *validated_shadows = NULL;
                    uint32_t validated_count = 0;
                    int validation_result = queue_validate_list_states12(d, queue_type, queue, l,
                        indirect_overrides, indirect_override_count, &validated_shadows, &validated_count);
                    if (validation_result == 1) {
                        queue_discard_list_states12(*state_shadows, *state_count);
                        *state_shadows = validated_shadows;
                        *state_count = validated_count;
                        [indirect_staging_buffers addObject:indirect_readback];
                    } else {
                        indirect_override_count--;
                        available = 0;
                        data = NULL;
                        vx_log("D3D12: GPU-produced ExecuteIndirect records failed per-record resource-state validation; skipping records and applying signature resets");
                        if (validated_shadows) queue_discard_list_states12(validated_shadows, validated_count);
                    }
                } else {
                    available = 0;
                    data = NULL;
                    vx_log("D3D12: could not retain GPU-produced ExecuteIndirect validation data; skipping records and applying signature resets");
                }
            }
            uint64_t generated64 = (uint64_t)available * sig->count + sig->count;
            if (generated64 > UINT32_MAX || generated64 > SIZE_MAX / sizeof(cmd12)) {
                vx_log("D3D12: ExecuteIndirect would expand to too many Metal commands");
                break;
            }
            uint32_t generated_count = 0, capacity = (uint32_t)generated64;
            cmd12 *generated = calloc(capacity, sizeof *generated);
            if (!generated) {
                vx_log("D3D12: could not expand ExecuteIndirect arguments");
                break;
            }
            for (uint32_t j = 0; j < available; j++) {
                const uint8_t *record = data + (uint64_t)j * sig->stride;
                for (uint32_t k = 0; k < sig->count; k++) {
                    const command_argument12 *a = &sig->args[k];
                    const uint8_t *p = record + a->payload_offset;
                    cmd12 *out = &generated[generated_count++];
                    switch (a->type) {
                    case INDIRECT_DRAW: {
                        uint32_t v[4]; memcpy(v, p, sizeof v);
                        out->op = C_DRAW; out->a = v[0]; out->b = v[1]; out->c = v[2]; out->off = v[3];
                        out->gpu_ir_transient = 1;
                        break;
                    }
                    case INDIRECT_DRAW_INDEXED: {
                        uint32_t v[5]; memcpy(v, p, sizeof v);
                        out->op = C_DRAW_INDEXED; out->a = v[0]; out->b = v[1]; out->c = v[2];
                        out->off = (uint64_t)(int64_t)(int32_t)v[3]; out->off2 = v[4];
                        out->gpu_ir_transient = 1;
                        break;
                    }
                    case INDIRECT_DISPATCH: {
                        uint32_t v[3]; memcpy(v, p, sizeof v);
                        out->op = C_DISPATCH; out->a = v[0]; out->b = v[1]; out->c = v[2];
                        out->gpu_ir_transient = 1;
                        break;
                    }
                    case INDIRECT_VERTEX_BUFFER: {
                        VBV view; memcpy(&view, p, sizeof view);
                        out->op = C_VB; out->a = a->a; out->b = 1; memcpy(out->values, &view, sizeof view);
                        break;
                    }
                    case INDIRECT_INDEX_BUFFER: {
                        IBV view; memcpy(&view, p, sizeof view);
                        out->op = C_IB; memcpy(out->values, &view, sizeof view);
                        break;
                    }
                    case INDIRECT_CONSTANT:
                        out->op = C_CONSTANTS; out->a = a->a; out->b = a->c; out->c = a->b; out->gfx = sig->gfx;
                        memcpy(out->values, p, a->size);
                        break;
                    case INDIRECT_INCREMENTING_CONSTANT:
                        out->op = C_CONSTANTS; out->a = a->a; out->b = 1; out->c = a->b; out->gfx = sig->gfx;
                        out->values[0] = j;
                        break;
                    case INDIRECT_CBV: case INDIRECT_SRV: case INDIRECT_UAV:
                        out->op = C_ROOT_VA; out->a = a->a; out->gfx = sig->gfx; memcpy(&out->va, p, sizeof out->va);
                        break;
                    default: break;
                    }
                }
            }
            for (uint32_t j = 0; j < generated_count; j++) generated[j].replay_source_index = UINT32_MAX;
            // D3D12 defines the bindings touched by a signature as null/zero after the indirect commands.
            for (uint32_t k = 0; k < sig->count; k++) {
                const command_argument12 *a = &sig->args[k];
                if (a->type != INDIRECT_VERTEX_BUFFER && a->type != INDIRECT_INDEX_BUFFER &&
                    a->type != INDIRECT_CONSTANT && a->type != INDIRECT_INCREMENTING_CONSTANT &&
                    a->type != INDIRECT_CBV && a->type != INDIRECT_SRV && a->type != INDIRECT_UAV) continue;
                cmd12 *out = &generated[generated_count++];
                switch (a->type) {
                case INDIRECT_VERTEX_BUFFER: out->op = C_VB; out->a = a->a; out->b = 1; break;
                case INDIRECT_INDEX_BUFFER: out->op = C_IB; break;
                case INDIRECT_CONSTANT:
                    out->op = C_CONSTANTS; out->a = a->a; out->b = a->c; out->c = a->b; out->gfx = sig->gfx; break;
                case INDIRECT_INCREMENTING_CONSTANT:
                    out->op = C_CONSTANTS; out->a = a->a; out->b = 1; out->c = a->b; out->gfx = sig->gfx; break;
                case INDIRECT_CBV: case INDIRECT_SRV: case INDIRECT_UAV:
                    out->op = C_ROOT_VA; out->a = a->a; out->gfx = sig->gfx; break;
                }
            }
            if (!replay_insert(&commands, &command_count, i + 1, generated, generated_count))
                vx_log("D3D12: could not insert expanded ExecuteIndirect commands");
            free(generated);
            break;
        }
        case C_DISCARD: {
            res12 *r = c->obj;
            id<MTLTexture> t = r ? r->texture : nil;
            if (!t) break;
            close_pass(d, g, cb, &re);
            end_encoders(&ce, &be, &re);
            MTLPixelFormat f = t.pixelFormat;
            int depth = f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float_Stencil8;
            int stencil = f == MTLPixelFormatDepth32Float_Stencil8;
            NSUInteger slices = t.textureType == MTLTextureTypeCube ? 6 : t.textureType == MTLTextureTypeCubeArray ? 6 * t.arrayLength : t.arrayLength;
            for (NSUInteger mip = 0; mip < t.mipmapLevelCount && mip < 16; mip++)
                for (NSUInteger slice = 0; slice < slices && slice < 64; slice++) {
                    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                    if (depth) {
                        rp.depthAttachment.texture = t; rp.depthAttachment.level = mip; rp.depthAttachment.slice = slice;
                        rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = 0;
                        rp.depthAttachment.storeAction = MTLStoreActionStore;
                        if (stencil) {
                            rp.stencilAttachment.texture = t; rp.stencilAttachment.level = mip; rp.stencilAttachment.slice = slice;
                            rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.clearStencil = 0;
                            rp.stencilAttachment.storeAction = MTLStoreActionStore;
                        }
                    } else {
                        rp.colorAttachments[0].texture = t; rp.colorAttachments[0].level = mip; rp.colorAttachments[0].slice = slice;
                        rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                    }
                    id<MTLRenderCommandEncoder> clear = [cb renderCommandEncoderWithDescriptor:rp];
                    [clear endEncoding];
                }
            break;
        }
        case C_WRITE_BUFFER_IMMEDIATE: {
            for (uint32_t write_index = 0; write_index < c->a; write_index++) {
                const write_buffer_immediate12 *write = &c->immediate_writes[write_index];
                close_pass(d, g, cb, &re);
                if (write->mode) end_encoders(&ce, &be, &re);
                else if (ce) { [ce endEncoding]; ce = nil; }
                if (!be) be = [cb blitCommandEncoder];
                size_t source_offset = 0;
                if (!be || !scratch_alloc(d, sc, sizeof write->value, &source_offset)) {
                    vx_log("D3D12: could not stage a WriteBufferImmediate value");
                    replay_failed = 1;
                    break;
                }
                memcpy((uint8_t *)sc->buf.contents + source_offset, &write->value, sizeof write->value);
                [be copyFromBuffer:sc->buf sourceOffset:source_offset toBuffer:write->resource->buffer
                  destinationOffset:(NSUInteger)write->offset size:sizeof write->value];
            }
            break;
        }
        case C_GPU_IR: {
            close_pass(d, g, cb, &re);
            uint32_t kind = gpu_ir_command_kind(&l->gpu_ir, c->a);
            if (g_skip_count12 && kind != GPU_IR_COMMAND_BARRIER && skipped_shader12(0, 'g')) break;
            if (kind != GPU_IR_COMMAND_BARRIER) cb_log12('g', 0, kind + 1, 0, 0);
            if (kind == GPU_IR_COMMAND_BARRIER || kind == GPU_IR_COMMAND_RESOLVE_TEXTURE) end_encoders(&ce, &be, &re);
            else {
                if (ce) { [ce endEncoding]; ce = nil; }
                if (!be) be = [cb blitCommandEncoder];
            }
            gpu_ir_backend backend = {
                .context = kind == GPU_IR_COMMAND_BARRIER ? NULL :
                           kind == GPU_IR_COMMAND_RESOLVE_TEXTURE ? (__bridge void *)cb : (__bridge void *)be,
                .copy_resource = d3d12_gpu_ir_copy_resource,
                .copy_buffer = d3d12_gpu_ir_copy_buffer,
                .copy_texture = d3d12_gpu_ir_copy_texture,
                .resolve_texture = d3d12_gpu_ir_resolve_texture,
                .barrier = d3d12_gpu_ir_barrier
            };
            int rc = gpu_ir_execute(&l->gpu_ir, c->a, &backend);
            if (rc != GPU_IR_OK) vx_log("D3D12: common GPU IR command failed (error %d)", rc);
            break;
        }
        case C_COPY_RESOURCE: case C_COPY_TEXTURE: {
            if (ce) { [ce endEncoding]; ce = nil; }
            close_pass(d, g, cb, &re);
            if (!be) be = [cb blitCommandEncoder];
            if (c->op == C_COPY_RESOURCE) {
                res12 *dst = c->obj, *src = c->obj2;
                if (dst->texture && src->texture) [be copyFromTexture:src->texture toTexture:dst->texture];
            } else {
                res12 *dst = c->dst.res, *src = c->src.res;
                // D3D12_TEXTURE_COPY_TYPE: 0 subresource index, 1 placed footprint
                if (c->dst.type == 1 && src->texture) {                   // texture -> buffer
                    uint32_t sub = c->src.subresource, mips = (uint32_t)src->texture.mipmapLevelCount;
                    uint32_t w = c->has_box ? c->box.right - c->box.left : c->dst.footprint.width, h = c->has_box ? c->box.bottom - c->box.top : c->dst.footprint.height;
                    uint32_t ox = c->has_box ? c->box.left : 0, oy = c->has_box ? c->box.top : 0;
                    [be copyFromTexture:src->texture sourceSlice:sub / mips sourceLevel:sub % mips sourceOrigin:MTLOriginMake(ox, oy, 0) sourceSize:MTLSizeMake(w, h, 1)
                               toBuffer:dst->buffer destinationOffset:c->dst.footprint.offset + (uint64_t)c->b * c->dst.footprint.row_pitch + c->a * bytes_per(c->dst.footprint.format)
                      destinationBytesPerRow:c->dst.footprint.row_pitch destinationBytesPerImage:(uint64_t)c->dst.footprint.row_pitch * c->dst.footprint.height];
                } else if (c->src.type == 1 && dst->texture) {            // buffer -> texture
                    uint32_t sub = c->dst.subresource, mips = (uint32_t)dst->texture.mipmapLevelCount;
                    uint32_t w = c->has_box ? c->box.right - c->box.left : c->src.footprint.width, h = c->has_box ? c->box.bottom - c->box.top : c->src.footprint.height;
                    uint64_t off = c->src.footprint.offset + (c->has_box ? (uint64_t)c->box.top * c->src.footprint.row_pitch + c->box.left * bytes_per(c->src.footprint.format) : 0);
                    [be copyFromBuffer:src->buffer sourceOffset:off sourceBytesPerRow:c->src.footprint.row_pitch sourceBytesPerImage:(uint64_t)c->src.footprint.row_pitch * c->src.footprint.height
                            sourceSize:MTLSizeMake(w, h, 1) toTexture:dst->texture destinationSlice:sub / mips destinationLevel:sub % mips destinationOrigin:MTLOriginMake(c->a, c->b, 0)];
                } else if (dst->texture && src->texture) {
                    uint32_t ds = c->dst.subresource, ss = c->src.subresource;
                    uint32_t dm = (uint32_t)dst->texture.mipmapLevelCount, sm = (uint32_t)src->texture.mipmapLevelCount;
                    MTLSize size = c->has_box ? MTLSizeMake(c->box.right - c->box.left, c->box.bottom - c->box.top, 1)
                                              : MTLSizeMake(MAX(1u, (uint32_t)src->texture.width >> (ss % sm)), MAX(1u, (uint32_t)src->texture.height >> (ss % sm)), 1);
                    [be copyFromTexture:src->texture sourceSlice:ss / sm sourceLevel:ss % sm sourceOrigin:MTLOriginMake(c->has_box ? c->box.left : 0, c->has_box ? c->box.top : 0, 0)
                             sourceSize:size toTexture:dst->texture destinationSlice:ds / dm destinationLevel:ds % dm destinationOrigin:MTLOriginMake(c->a, c->b, 0)];
                }
            }
            break;
        }
        }
        if (replay_failed) break;
    }
    if (replay_failed) {
        close_pass(d, g, cb, &re); end_encoders(&ce, &be, &re);
        free(indirect_overrides);
        gfx_access_cache_release12(g); free(commands); free(compute); free(graphics); free(g);
        *command_buffer = cb;
        return 0;
    }
    close_pass(d, g, cb, &re); end_encoders(&ce, &be, &re);
    free(indirect_overrides);
    free(commands);
    free(compute);
    free(graphics);
    gfx_access_cache_release12(g);
    free(g);
    *command_buffer = cb;
    return 1;
}

// ---- queues -----------------------------------------------------------------------------------------------------------

static void queue_async_stop12(queue12 *q);
static void queue_destroy(d3d_object *o) {
    queue_async_stop12((queue12 *)o);
    dev12 *qd = ((queue12 *)o)->dev;
    if (qd) { pthread_mutex_lock(&qd->lock); for (int i = 0; i < 16; i++) if (qd->queues[i] == o) qd->queues[i] = NULL; pthread_mutex_unlock(&qd->lock); }
    ((queue12 *)o)->mtl_queue = nil;
    ((queue12 *)o)->timeline = nil;
    release_dev(o);
}
static WINAPI HRESULT queue_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12CommandQueue, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}

static int queue_supports_list_type12(int32_t queue_type, int32_t list_type) {
    if (list_type == 1) return 0; // Bundles execute only through ExecuteBundle on a direct list.
    if (queue_type == 0) return list_type == 0 || list_type == 2 || list_type == 3;
    if (queue_type == 2) return list_type == 2 || list_type == 3;
    if (queue_type == 3) return list_type == 3;
    return 0;
}

static int queue_query_command_allowed12(dev12 *device, int32_t queue_type, const cmd12 *command) {
    queryheap12 *query = command ? command->obj : NULL;
    if (!device || !query || query->dev != device || !__atomic_load_n(&query->residency_refs, __ATOMIC_RELAXED)) return 0;
    if (command->op == C_BEGIN_QUERY || command->op == C_END_QUERY) {
        if (command->b >= query->count) return 0;
        if (query->type == 0)
            return queue_type == 0 && command->a <= 1;
        if (command->op == C_BEGIN_QUERY) return 0; // D3D12 timestamps are written by EndQuery only.
        if (query->type == 1)
            return queue_type != 3 && command->a == 2;
        if (query->type == 5)
            return queue_type == 3 && command->a == 2 && d3d12_copy_timestamp_counters_supported12(device);
        if (query->type == 2) return queue_type != 3 && command->a == 3;
        if (query->type == 3) return queue_type != 3 && command->a >= 4 && command->a <= 7;
        return 0;
    }
    if (command->op == C_RESOLVE_QUERY) {
        if (command->b > query->count || command->c > query->count - command->b) return 0;
        if (query->type == 0)
            return queue_type == 0 && command->a <= 1;
        if (query->type == 1)
            return queue_type != 3 && command->a == 2;
        if (query->type == 5)
            return queue_type == 3 && command->a == 2 && d3d12_copy_timestamp_counters_supported12(device);
        if (query->type == 2) return queue_type != 3 && command->a == 3;
        if (query->type == 3) return queue_type != 3 && command->a >= 4 && command->a <= 7;
        return queue_type != 3;
    }
    return 0;
}

static int queue_command_allowed12(dev12 *device, int32_t queue_type, int32_t list_type, const cmd12 *command) {
    int graphics_command = 0;
    switch (command->op) {
    case C_CLEAR_RTV: case C_CLEAR_DSV: case C_DRAW: case C_DRAW_INDEXED:
    case C_TARGETS: case C_VIEWPORTS: case C_SCISSORS: case C_TOPOLOGY:
    case C_VB: case C_IB: case C_BLEND_FACTOR: case C_STENCIL_REF:
        graphics_command = 1;
        break;
    default:
        break;
    }
    if (graphics_command && (queue_type != 0 || list_type != 0)) return 0;
    if ((command->op == C_BUNDLE_BEGIN || command->op == C_BUNDLE_END) && (queue_type != 0 || list_type != 0)) return 0;
    if (list_type == 3) {
        if (command->op == C_WRITE_BUFFER_IMMEDIATE) return 1;
        if (command->op == C_END_QUERY || command->op == C_RESOLVE_QUERY) {
            int allowed = queue_type == 3 && queue_query_command_allowed12(device, queue_type, command);
            if (!allowed) vx_log("D3D12: copy-list query command rejected (op %u, heap type %d, query type %u, queue %d)",
                                 command->op, command->obj ? ((queryheap12 *)command->obj)->type : -1,
                                 command->a, queue_type);
            return allowed;
        }
        if (command->op != C_GPU_IR) return 0;
        if (command->b == UINT32_MAX)
            return command->gfx >= D3D12_TRACK_COPY_BUFFER && command->gfx <= D3D12_TRACK_COPY_TEXTURE;
        if (command->b == 0)
            return state_queue_access_allowed12((uint32_t)command->off, 3) &&
                   state_queue_access_allowed12((uint32_t)command->off2, 3);
        return command->b == 1; // aliasing barriers
    }
    if (queue_type == 3) {
        switch (command->op) {
        case C_DISPATCH: case C_CLEAR_UAV: case C_EXECUTE_INDIRECT: case C_BEGIN_QUERY:
            return 0;
        default:
            break;
        }
    }
    if (list_type == 2 && command->op == C_EXECUTE_INDIRECT &&
        (!command->obj || ((command_signature12 *)command->obj)->gfx)) return 0;
    if (queue_type == 2 && command->op == C_EXECUTE_INDIRECT &&
        (!command->obj || ((command_signature12 *)command->obj)->gfx)) return 0;
    if (command->op == C_GPU_IR && command->b == UINT32_MAX &&
        (command->gfx == D3D12_TRACK_RESOLVE || command->gfx == D3D12_TRACK_RENDER_PASS_RESOLVE) &&
        (queue_type != 0 || list_type != 0))
        return 0;
    if (command->op == C_GPU_IR && command->b == 2 && queue_type == 3) return 0; // UAV barriers are not copy-queue work.
    if (command->op == C_BEGIN_QUERY || command->op == C_END_QUERY || command->op == C_RESOLVE_QUERY) {
        int allowed = queue_query_command_allowed12(device, queue_type, command);
        if (!allowed) vx_log("D3D12: query command rejected (op %u, heap type %d, query type %u, queue %d)",
                             command->op, command->obj ? ((queryheap12 *)command->obj)->type : -1,
                             command->a, queue_type);
        return allowed;
    }
    return 1;
}

static int queue_validate_graphics_draw12(dev12 *device, int32_t queue_type,
                                          resource_state_shadow12 **shadows, uint32_t *count, uint32_t *capacity,
                                          const VBV vertex_buffers[16], const IBV *index_buffer,
                                          pso12 *pipeline, root_binding *graphics,
                                          heap12 *resource_heap, heap12 *sampler_heap,
                                          res12 *const targets[9], uint32_t target_count,
                                          const uint32_t target_subresources[9],
                                          const uint32_t target_flags[9], int indexed) {
    for (uint32_t k = 0; k < 16; k++) {
        if (!vertex_buffers[k].va && !vertex_buffers[k].size) continue;
        res12 *resource = state_resource_for_gpu_range_locked12(device, vertex_buffers[k].va, vertex_buffers[k].size);
        if (!resource) return 0;
        int use_result = state_validate_resource_use_desc12(device, queue_type, shadows, count, capacity,
            resource, 0, D3D12_STATE_VERTEX_CONSTANT, 0);
        if (use_result <= 0) return use_result < 0 ? -1 : 0;
    }
    if (indexed) {
        res12 *resource = state_resource_for_gpu_range_locked12(device, index_buffer->va, index_buffer->size);
        if (!resource) return 0;
        int use_result = state_validate_resource_use_desc12(device, queue_type, shadows, count, capacity,
            resource, 0, D3D12_STATE_INDEX, 0);
        if (use_result <= 0) return use_result < 0 ? -1 : 0;
    }
    if (pipeline) {
        if (!shader_root_signature_valid12(device, &pipeline->vs, graphics->rs) ||
            !shader_root_signature_valid12(device, &pipeline->ps, graphics->rs)) return 0;
        int use_result = state_validate_shader_stage12(device, queue_type, shadows, count, capacity,
            &pipeline->vs, graphics->rs, &graphics->st, resource_heap, sampler_heap);
        if (use_result <= 0) return use_result < 0 ? -1 : 0;
        use_result = state_validate_shader_stage12(device, queue_type, shadows, count, capacity,
            &pipeline->ps, graphics->rs, &graphics->st, resource_heap, sampler_heap);
        if (use_result <= 0) return use_result < 0 ? -1 : 0;
    }
    for (uint32_t k = 0; k < target_count; k++) {
        if (!targets[k]) continue;
        int use_result = state_validate_resource_use_desc12(device, queue_type, shadows, count, capacity,
            targets[k], target_subresources[k], D3D12_STATE_RENDER_TARGET, 1);
        if (use_result <= 0) return use_result < 0 ? -1 : 0;
    }
    if (pipeline && targets[8] && (pipeline->depth_enable || pipeline->stencil_enable)) {
        int depth_write = pipeline->depth_enable && pipeline->depth_write;
        int stencil_write = pipeline->stencil_enable && pipeline->stencil_write;
        if ((depth_write && (target_flags[8] & 1u)) || (stencil_write && (target_flags[8] & 2u))) return 0;
        uint32_t required = depth_write || stencil_write ? D3D12_STATE_DEPTH_WRITE : D3D12_STATE_DEPTH_READ;
        int use_result = state_validate_resource_use_desc12(device, queue_type, shadows, count, capacity,
            targets[8], target_subresources[8], required, required == D3D12_STATE_DEPTH_WRITE);
        if (use_result <= 0) return use_result < 0 ? -1 : 0;
    }
    return 1;
}

static int queue_validate_indirect_state12(dev12 *device, int32_t queue_type,
                                           resource_state_shadow12 **shadows, uint32_t *count, uint32_t *capacity,
                                           id<MTLCommandQueue> metal_queue, const list12 *list, uint32_t command_index,
                                           const d3d12_indirect_validation_override12 *overrides,
                                           uint32_t override_count,
                                           const cmd12 *command, const command_signature12 *signature,
                                           res12 *arguments, res12 *count_buffer, root_binding *compute,
                                           root_binding *graphics, VBV vertex_buffers[16], IBV *index_buffer,
                                           pso12 *pipeline, heap12 *resource_heap, heap12 *sampler_heap,
                                           res12 *const targets[9], uint32_t target_count,
                                           const uint32_t target_subresources[9], const uint32_t target_flags[9]) {
    uint32_t available = 0;
    const uint8_t *records = NULL;
    int read_status = D3D12_INDIRECT_CPU_ARGUMENT_UNREADABLE;
    for (uint32_t i = 0; i < override_count; i++) {
        if (overrides[i].command_index != command_index) continue;
        available = overrides[i].available;
        records = overrides[i].records;
        read_status = D3D12_INDIRECT_CPU_OK;
        break;
    }
    if (read_status != D3D12_INDIRECT_CPU_OK)
        read_status = d3d12_indirect_cpu_records12(command, signature, arguments, count_buffer,
                                                   &available, &records);
    id<MTLBuffer> __strong staged_records = nil;
    if ((read_status == D3D12_INDIRECT_CPU_COUNT_UNREADABLE ||
         read_status == D3D12_INDIRECT_CPU_ARGUMENT_UNREADABLE) &&
        !d3d12_indirect_has_prior_gpu_work12(list, command_index)) {
        int staged = d3d12_indirect_preflight_gpu_records12(metal_queue, command, signature, arguments,
            count_buffer, &available, &records, &staged_records);
        if (staged) read_status = D3D12_INDIRECT_CPU_OK;
    }
    // If this list has already issued GPU work, replay reads producer data only after that prefix completes,
    // then re-runs this validator with the staged records before expanding any indirect commands.
    if (read_status != D3D12_INDIRECT_CPU_OK) available = 0;
    root_binding *binding = signature->gfx ? graphics : compute;
    for (uint32_t record_index = 0; record_index < available; record_index++) {
        const uint8_t *record = records + (uint64_t)record_index * signature->stride;
        for (uint32_t argument_index = 0; argument_index < signature->count; argument_index++) {
            const command_argument12 *argument = &signature->args[argument_index];
            const uint8_t *payload = record + argument->payload_offset;
            switch (argument->type) {
            case INDIRECT_VERTEX_BUFFER:
                if (argument->a >= 16) return 0;
                memcpy(&vertex_buffers[argument->a], payload, sizeof vertex_buffers[argument->a]);
                break;
            case INDIRECT_INDEX_BUFFER:
                memcpy(index_buffer, payload, sizeof *index_buffer);
                break;
            case INDIRECT_CONSTANT:
                if (argument->a >= 64 || argument->b > 64 || argument->c > 64 - argument->b) return 0;
                memcpy(&binding->st.constants[argument->a][argument->b], payload, (size_t)argument->c * sizeof(uint32_t));
                break;
            case INDIRECT_INCREMENTING_CONSTANT:
                if (argument->a >= 64 || argument->b >= 64) return 0;
                binding->st.constants[argument->a][argument->b] = record_index;
                break;
            case INDIRECT_CBV: case INDIRECT_SRV: case INDIRECT_UAV:
                if (argument->a >= 64) return 0;
                memcpy(&binding->st.root_va[argument->a], payload, sizeof(uint64_t));
                break;
            default:
                break;
            }
        }

        const command_argument12 *operation = &signature->args[signature->count - 1];
        const uint8_t *payload = record + operation->payload_offset;
        int use_result = 1;
        if (operation->type == INDIRECT_DRAW) {
            uint32_t draw[4];
            memcpy(draw, payload, sizeof draw);
            if (draw[0] && draw[1])
                use_result = queue_validate_graphics_draw12(device, queue_type, shadows, count, capacity,
                    vertex_buffers, index_buffer, pipeline, graphics, resource_heap, sampler_heap,
                    targets, target_count, target_subresources, target_flags, 0);
        } else if (operation->type == INDIRECT_DRAW_INDEXED) {
            uint32_t draw[5];
            memcpy(draw, payload, sizeof draw);
            if (draw[0] && draw[1])
                use_result = queue_validate_graphics_draw12(device, queue_type, shadows, count, capacity,
                    vertex_buffers, index_buffer, pipeline, graphics, resource_heap, sampler_heap,
                    targets, target_count, target_subresources, target_flags, 1);
        } else if (operation->type == INDIRECT_DISPATCH && pipeline && pipeline->compute) {
            uint32_t dispatch[3];
            memcpy(dispatch, payload, sizeof dispatch);
            if (dispatch[0] && dispatch[1] && dispatch[2]) {
                if (!shader_root_signature_valid12(device, &pipeline->msl, compute->rs)) use_result = 0;
                else use_result = state_validate_shader_stage12(device, queue_type, shadows, count, capacity,
                    &pipeline->msl, compute->rs, &compute->st, resource_heap, sampler_heap);
            }
        }
        if (use_result <= 0) return use_result;
    }

    // ExecuteIndirect clears every root argument and IA binding named by its signature, even for zero commands.
    for (uint32_t argument_index = 0; argument_index < signature->count; argument_index++) {
        const command_argument12 *argument = &signature->args[argument_index];
        switch (argument->type) {
        case INDIRECT_VERTEX_BUFFER:
            if (argument->a >= 16) return 0;
            memset(&vertex_buffers[argument->a], 0, sizeof vertex_buffers[argument->a]);
            break;
        case INDIRECT_INDEX_BUFFER:
            memset(index_buffer, 0, sizeof *index_buffer);
            break;
        case INDIRECT_CONSTANT:
            if (argument->a >= 64 || argument->b > 64 || argument->c > 64 - argument->b) return 0;
            memset(&binding->st.constants[argument->a][argument->b], 0, (size_t)argument->c * sizeof(uint32_t));
            break;
        case INDIRECT_INCREMENTING_CONSTANT:
            if (argument->a >= 64 || argument->b >= 64) return 0;
            binding->st.constants[argument->a][argument->b] = 0;
            break;
        case INDIRECT_CBV: case INDIRECT_SRV: case INDIRECT_UAV:
            if (argument->a >= 64) return 0;
            binding->st.root_va[argument->a] = 0;
            break;
        default:
            break;
        }
    }
    return 1;
}

// State advances in command-list submission order, because different threads may record lists concurrently and a
// list can be submitted more than once. Work on a per-list shadow so a bad transition cannot partially corrupt the
// resource state used to validate later submissions.
static int queue_validate_list_states12(dev12 *device, int32_t queue_type, id<MTLCommandQueue> metal_queue,
                                        list12 *list,
                                        const d3d12_indirect_validation_override12 *overrides,
                                        uint32_t override_count, resource_state_shadow12 **out_shadows,
                                        uint32_t *out_count) {
    if (!out_shadows || !out_count || !list->closed || list->validation_error) return 0;
    *out_shadows = NULL;
    *out_count = 0;
    resource_state_shadow12 *shadows = NULL;
    uint32_t count = 0, capacity = 0;
    res12 *targets[9] = { 0 };
    uint32_t target_subresources[9] = { 0 }, target_flags[9] = { 0 }, target_count = 0;
    VBV vertex_buffers[16] = { 0 };
    IBV index_buffer = { 0 };
    root_binding compute = { 0 }, graphics = { 0 };
    heap12 *resource_heap = NULL, *sampler_heap = NULL;
    pso12 *pipeline = NULL;
    heap12 *bundle_resource_heap = NULL, *bundle_sampler_heap = NULL;
    uint32_t topology = 4;
    int in_bundle = 0;
    int result = 1;
    uint32_t at = 0;
    pthread_mutex_lock(&device->lock);
    for (uint32_t i = 0; i < list->n; i++) {
        const cmd12 *command = &list->cmds[i];
        at = i;
        if (!queue_command_allowed12(device, queue_type, list->type, command)) {
            vx_log("D3D12: queue %d rejected command %u at index %u from list type %d",
                   queue_type, command->op, i, list->type);
            result = 0;
            break;
        }
        if (command->op == C_BUNDLE_BEGIN) {
            if (in_bundle) { result = 0; break; }
            in_bundle = 1;
            bundle_resource_heap = resource_heap;
            bundle_sampler_heap = sampler_heap;
            pipeline = NULL;
            topology = 0;
        } else if (command->op == C_BUNDLE_END) {
            if (!in_bundle) { result = 0; break; }
            in_bundle = 0;
        } else if (in_bundle && command->op == C_HEAPS &&
                   (command->obj != bundle_resource_heap || command->obj2 != bundle_sampler_heap)) {
            result = 0;
            break;
        } else if (command->op == C_PSO) {
            pipeline = command->obj;
        } else if (command->op == C_ROOTSIG) {
            root_binding *binding = command->gfx ? &graphics : &compute;
            binding->rs = command->obj;
            memset(&binding->st, 0, sizeof binding->st);
        } else if (command->op == C_HEAPS) {
            resource_heap = command->obj;
            sampler_heap = command->obj2;
        } else if (command->op == C_TOPOLOGY) {
            topology = command->a;
        } else if (command->op == C_TABLE) {
            root_binding *binding = command->gfx ? &graphics : &compute;
            if (command->a < 64) binding->st.table[command->a] = command->va;
        } else if (command->op == C_CONSTANTS) {
            root_binding *binding = command->gfx ? &graphics : &compute;
            if (command->a < 64) for (uint32_t k = 0; k < command->b && command->c + k < 64; k++)
                binding->st.constants[command->a][command->c + k] = command->values[k];
        } else if (command->op == C_ROOT_VA) {
            root_binding *binding = command->gfx ? &graphics : &compute;
            if (command->a < 64) binding->st.root_va[command->a] = command->va;
        } else if (command->op == C_TARGETS) {
            target_count = command->a > 8 ? 8 : command->a;
            for (uint32_t k = 0; k < 9; k++) {
                targets[k] = command->resource[k];
                target_subresources[k] = command->subresource[k];
                target_flags[k] = command->view_flags[k];
            }
        } else if (command->op == C_VB) {
            for (uint32_t k = 0; k < command->b && command->a + k < 16; k++)
                memcpy(&vertex_buffers[command->a + k], (const VBV *)command->values + k, sizeof(VBV));
        } else if (command->op == C_IB) {
            memcpy(&index_buffer, command->values, sizeof index_buffer);
        } else if (command->op == C_CLEAR_RTV && command->resource[0]) {
            int use_result = state_validate_resource_use_desc12(device, queue_type, &shadows, &count, &capacity,
                command->resource[0], command->subresource[0], D3D12_STATE_RENDER_TARGET, 1);
            if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
        } else if (command->op == C_CLEAR_DSV) {
            uint32_t clear_flags = command->a;
            if (!command->resource[8] || !clear_flags || (clear_flags & ~3u) ||
                ((clear_flags & 1u) && (command->view_flags[8] & 1u)) ||
                ((clear_flags & 2u) && (command->view_flags[8] & 2u))) {
                result = 0;
                break;
            }
            int use_result = state_validate_resource_use_desc12(device, queue_type, &shadows, &count, &capacity,
                command->resource[8], command->subresource[8], D3D12_STATE_DEPTH_WRITE, 1);
            if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
        } else if (command->op == C_CLEAR_UAV) {
            uint32_t cpu_index = 0, gpu_index = 0;
            heap12 *cpu_heap = heap_cpu_descriptor((size_t)command->off, &cpu_index);
            heap12 *gpu_heap = heap_gpu_descriptor(command->va, &gpu_index);
            res12 *resource = command->resource[0];
            if (!resource || resource->dev != device || !(resource->desc.Flags & 0x4u) ||
                !cpu_heap || cpu_heap->desc.Type != 0 || cpu_heap->gpu || (cpu_heap->desc.Flags & 1u) ||
                !gpu_heap || gpu_heap != resource_heap || gpu_heap->desc.Type != 0 || !gpu_heap->gpu ||
                !(gpu_heap->desc.Flags & 1u) || !heap_uav_views_match12(cpu_heap, cpu_index, gpu_heap, gpu_index)) {
                result = 0;
                break;
            }
            shader_view12 *view = &gpu_heap->shader_view[gpu_index];
            if (view->resource != resource) { result = 0; break; }
            gpu_ir_buffer_view *buffer_view = gpu_heap->buffer_view ? gpu_heap->buffer_view[gpu_index] : NULL;
            const gpu_ir_buffer_view_desc *buffer_desc = buffer_view ? gpu_ir_buffer_view_description(buffer_view) : NULL;
            if (buffer_view && (!buffer_desc ||
                buffer_desc->kind == GPU_IR_BUFFER_VIEW_STRUCTURED || command->b ||
                (buffer_desc->kind == GPU_IR_BUFFER_VIEW_RAW && command->a))) {
                // Structured buffers are incompatible with D3D12 ClearUnorderedAccessView*; raw
                // buffers support Uint clears only, and buffer rectangles are invalid.
                result = 0;
                break;
            }
            int use_result = state_texture_view12(device, queue_type, &shadows, &count, &capacity,
                view, D3D12_STATE_UAV, 1);
            if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
        } else if ((command->op == C_DRAW && command->a && command->b) ||
                   (command->op == C_DRAW_INDEXED && command->a && command->b)) {
            // (lists and strips; with adjacency for geometry shaders; patch lists of 1-32 control points)
            if (!topology || (topology > 5 && topology < 10) || (topology > 13 && topology < 33) || topology > 64) { result = 0; break; }
            int use_result = queue_validate_graphics_draw12(device, queue_type, &shadows, &count, &capacity,
                vertex_buffers, &index_buffer, pipeline, &graphics, resource_heap, sampler_heap, targets,
                target_count, target_subresources, target_flags, command->op == C_DRAW_INDEXED);
            if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
        } else if (command->op == C_DISPATCH && pipeline) {
            int use_result = shader_root_signature_valid12(device, &pipeline->msl, compute.rs) ?
                state_validate_shader_stage12(device, queue_type, &shadows, &count, &capacity,
                    &pipeline->msl, compute.rs, &compute.st, resource_heap, sampler_heap) : 0;
            if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
        } else if (command->op == C_WRITE_BUFFER_IMMEDIATE) {
            if (!command->a || !command->immediate_writes) { result = 0; break; }
            for (uint32_t write_index = 0; write_index < command->a; write_index++) {
                const write_buffer_immediate12 *write = &command->immediate_writes[write_index];
                if (!write->resource || write->resource->dev != device || !write->resource->buffer ||
                    (write->offset & 3u) || write->offset > write->resource->buffer.length ||
                    sizeof write->value > write->resource->buffer.length - write->offset || write->mode > 2u) {
                    result = 0;
                    break;
                }
                int use_result = state_validate_resource_use_desc12(device, queue_type, &shadows, &count, &capacity,
                    write->resource, 0, D3D12_STATE_COPY_DEST, 1);
                if (use_result <= 0) {
                    result = use_result < 0 ? -1 : 0;
                    break;
                }
            }
            if (result != 1) break;
        } else if (command->op == C_GPU_IR && command->b == 0) {
            res12 *resource = command->obj;
            if (!resource || resource->dev != device || !resource_resident12(resource)) { result = 0; break; }
            if (!state_queue_access_allowed12((uint32_t)command->off, queue_type) ||
                !state_queue_access_allowed12((uint32_t)command->off2, queue_type)) { result = 0; break; }
            resource_state_shadow12 *shadow = state_shadow_get12(&shadows, &count, &capacity, resource);
            if (!shadow) { result = -1; break; }
            int transition_result = state_transition12(shadow, command->c, (uint32_t)command->off,
                                                       (uint32_t)command->off2, (uint32_t)command->gfx);
            if (transition_result <= 0) { result = transition_result < 0 ? -1 : 0; break; }
        } else if (command->op == C_GPU_IR && command->b == UINT32_MAX &&
                   command->gfx >= D3D12_TRACK_COPY_BUFFER && command->gfx <= D3D12_TRACK_RENDER_PASS_RESOLVE) {
            int resolve = command->gfx == D3D12_TRACK_RESOLVE || command->gfx == D3D12_TRACK_RENDER_PASS_RESOLVE;
            uint32_t src_state = command->gfx == D3D12_TRACK_RENDER_PASS_RESOLVE ? D3D12_STATE_RENDER_TARGET :
                                 resolve ? D3D12_STATE_RESOLVE_SOURCE : D3D12_STATE_COPY_SOURCE;
            uint32_t dst_state = resolve ? D3D12_STATE_RESOLVE_DEST : D3D12_STATE_COPY_DEST;
            uint32_t src_subresource = command->gfx == D3D12_TRACK_COPY_RESOURCE ? D3D12_ALL_SUBRESOURCES : command->c;
            uint32_t dst_subresource = command->gfx == D3D12_TRACK_COPY_RESOURCE ? D3D12_ALL_SUBRESOURCES :
                                       resolve ? (uint32_t)command->off :
                                       command->gfx == D3D12_TRACK_COPY_TEXTURE ? (uint32_t)command->off : 0;
            int use_result = state_validate_resource_pair12(device, queue_type, &shadows, &count, &capacity,
                command->obj, dst_subresource, dst_state, command->obj2, src_subresource, src_state);
            if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
        } else if (command->op == C_COPY_RESOURCE) {
            int use_result = state_validate_resource_pair12(device, queue_type, &shadows, &count, &capacity,
                command->obj, D3D12_ALL_SUBRESOURCES, D3D12_STATE_COPY_DEST,
                command->obj2, D3D12_ALL_SUBRESOURCES, D3D12_STATE_COPY_SOURCE);
            if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
        } else if (command->op == C_COPY_TEXTURE) {
            res12 *destination = command->dst.res, *source = command->src.res;
            uint32_t dst_subresource = command->dst.type == 0 ? command->dst.subresource : 0;
            uint32_t src_subresource = command->src.type == 0 ? command->src.subresource : 0;
            int use_result = state_validate_resource_pair12(device, queue_type, &shadows, &count, &capacity,
                destination, dst_subresource, D3D12_STATE_COPY_DEST,
                source, src_subresource, D3D12_STATE_COPY_SOURCE);
            if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
        } else if (command->op == C_RESOLVE_QUERY) {
            queryheap12 *query = command->obj;
            res12 *destination = command->obj2;
            if (!query || query->dev != device || !destination || destination->dev != device ||
                destination->desc.Dimension != 1 || !destination->buffer || (command->off & 7u)) {
                vx_log("D3D12: ResolveQueryData parameters are invalid");
                result = 0;
                break;
            }
            uint64_t bytes = (uint64_t)command->c * query->stride;
            if (command->off > destination->buffer.length || bytes > destination->buffer.length - command->off) {
                vx_log("D3D12: ResolveQueryData output range is invalid");
                result = 0;
                break;
            }
            int use_result = state_validate_resource_use_desc12(device, queue_type, &shadows, &count, &capacity,
                destination, 0, D3D12_STATE_COPY_DEST, 1);
            if (use_result <= 0) {
                vx_log("D3D12: ResolveQueryData destination is not in COPY_DEST (queue %d)", queue_type);
                result = use_result < 0 ? -1 : 0;
                break;
            }
        } else if (command->op == C_EXECUTE_INDIRECT) {
            command_signature12 *signature = command->obj;
            res12 *arguments = command->obj2, *count_buffer = command->obj3;
            root_binding *indirect_root = signature && signature->gfx ? &graphics : &compute;
            if (!signature || signature->dev != device ||
                (signature->root && indirect_root->rs != signature->root) ||
                !arguments || arguments->dev != device || arguments->desc.Dimension != 1 ||
                !resource_resident12(arguments) ||
                (count_buffer && (count_buffer->dev != device || count_buffer->desc.Dimension != 1 ||
                                  !resource_resident12(count_buffer)))) {
                result = 0;
                break;
            }
            resource_state_shadow12 *arg_state = state_shadow_get12(&shadows, &count, &capacity, arguments);
            resource_state_shadow12 *count_state = count_buffer ?
                state_shadow_get12(&shadows, &count, &capacity, count_buffer) : NULL;
            if (!arg_state || (count_buffer && !count_state)) { result = -1; break; }
            arg_state = state_shadow_find12(shadows, count, arguments);
            count_state = count_buffer ? state_shadow_find12(shadows, count, count_buffer) : NULL;
            int arg_use = arg_state ? state_resource_use12(arg_state, 0, D3D12_STATE_INDIRECT, 0, queue_type) : 0;
            int count_use = count_state ? state_resource_use12(count_state, 0, D3D12_STATE_INDIRECT, 0, queue_type) : 1;
            if (!arg_state || (count_buffer && !count_state) || arg_use <= 0 || count_use <= 0) {
                result = arg_use < 0 || count_use < 0 ? -1 : 0;
                break;
            }
            if (command->a && (!count_buffer || count_buffer->gpu_buffer) && signature && signature->gfx && !signature->root &&
                signature->count == 1 && signature->payload_size == signature->args[0].size &&
                (signature->args[0].type == INDIRECT_DRAW || signature->args[0].type == INDIRECT_DRAW_INDEXED)) {
                int use_result = queue_validate_graphics_draw12(device, queue_type, &shadows, &count, &capacity,
                    vertex_buffers, &index_buffer, pipeline, &graphics, resource_heap, sampler_heap, targets,
                    target_count, target_subresources, target_flags,
                    signature->args[0].type == INDIRECT_DRAW_INDEXED);
                if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
            }
            if (command->a && signature && !signature->gfx && !signature->root && signature->count == 1 &&
                signature->payload_size == signature->args[0].size &&
                signature->args[0].type == INDIRECT_DISPATCH && pipeline && pipeline->compute) {
                int use_result = shader_root_signature_valid12(device, &pipeline->msl, compute.rs) ?
                    state_validate_shader_stage12(device, queue_type, &shadows, &count, &capacity,
                        &pipeline->msl, compute.rs, &compute.st, resource_heap, sampler_heap) : 0;
                if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
            }
            int changes_indirect_state = signature->root != NULL;
            for (uint32_t argument_index = 0; argument_index < signature->count; argument_index++)
                if (signature->args[argument_index].type == INDIRECT_VERTEX_BUFFER ||
                    signature->args[argument_index].type == INDIRECT_INDEX_BUFFER) changes_indirect_state = 1;
            if (changes_indirect_state) {
                int use_result = queue_validate_indirect_state12(device, queue_type, &shadows, &count, &capacity,
                    metal_queue, list, i, overrides, override_count, command, signature, arguments, count_buffer,
                    &compute, &graphics, vertex_buffers, &index_buffer,
                    pipeline, resource_heap, sampler_heap, targets, target_count, target_subresources, target_flags);
                if (use_result <= 0) { result = use_result < 0 ? -1 : 0; break; }
            }
        }
    }
    if (result == 1 && in_bundle) result = 0;
    pthread_mutex_unlock(&device->lock);
    // VXRT_TRACE_D3D12_STATES=1: which recorded command a rejected list stopped at
    static int trace = -1;
    if (trace < 0) trace = getenv("VXRT_TRACE_D3D12_STATES") != NULL;
    if (trace && result == 0 && at < list->n)
        vx_log("D3D12: state validation stopped at command %u of %u (op %d, gpu-ir kind %u)", at, list->n, list->cmds[at].op, list->cmds[at].gfx);
    if (result == 1) { *out_shadows = shadows; *out_count = count; }
    else {
        for (uint32_t i = 0; i < count; i++) state_shadow_clear12(&shadows[i]);
        free(shadows);
    }
    return result;
}
static void queue_commit_list_states12(dev12 *device, resource_state_shadow12 *shadows, uint32_t count) {
    pthread_mutex_lock(&device->lock);
    for (uint32_t i = 0; i < count; i++) {
        resource_state_shadow12 *shadow = &shadows[i];
        res12 *resource = shadow->resource;
        state_overrides_free12(resource->state_overrides);
        state_pending_free12(resource->pending_transitions);
        resource->initial_state = shadow->initial_state;
        resource->initial_promoted = shadow->initial_promoted;
        resource->state_overrides = shadow->overrides;
        resource->pending_transitions = shadow->pending;
        shadow->overrides = NULL;
        shadow->pending = NULL;
    }
    pthread_mutex_unlock(&device->lock);
    for (uint32_t i = 0; i < count; i++) state_shadow_clear12(&shadows[i]);
    free(shadows);
}
static void queue_discard_list_states12(resource_state_shadow12 *shadows, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) state_shadow_clear12(&shadows[i]);
    free(shadows);
}

static int queue_batch_touched_reserve12(res12 ***resources, uint32_t *capacity, uint32_t need) {
    if (need <= *capacity) return 1;
    if (need && sizeof **resources > SIZE_MAX / (size_t)need) return 0;
    uint32_t next = *capacity ? *capacity : 16;
    while (next < need) {
        if (next > UINT32_MAX / 2) { next = need; break; }
        next *= 2;
    }
    res12 **grown = realloc(*resources, (size_t)next * sizeof **resources);
    if (!grown) return 0;
    *resources = grown;
    *capacity = next;
    return 1;
}

static void resource_decay_promoted_reads12(res12 *resource) {
    if (resource->initial_promoted && state_is_read_only12(resource->initial_state)) {
        resource->initial_state = 0;
        resource->initial_promoted = 0;
    }
    res_state_override12 **link = &resource->state_overrides;
    while (*link) {
        res_state_override12 *entry = *link;
        if (entry->promoted && state_is_read_only12(entry->state)) {
            entry->state = 0;
            entry->promoted = 0;
            if (entry->state == resource->initial_state && entry->promoted == resource->initial_promoted) {
                *link = entry->next;
                free(entry);
                continue;
            }
        }
        link = &entry->next;
    }
}

static void queue_decay_batch_states12(dev12 *device, res12 *const *resources, uint32_t count,
                                      int32_t queue_type) {
    pthread_mutex_lock(&device->lock);
    for (uint32_t i = 0; i < count; i++) {
        res12 *resource = resources[i];
        if (!resource || resource->pending_transitions) continue;
        int whole_resource_decays = queue_type == 3 || resource->desc.Dimension == 1 ||
                                    ((uint32_t)resource->desc.Flags & 0x20u);
        if (whole_resource_decays) {
            resource->initial_state = 0;
            resource->initial_promoted = 0;
            state_overrides_free12(resource->state_overrides);
            resource->state_overrides = NULL;
        } else {
            resource_decay_promoted_reads12(resource);
        }
    }
    pthread_mutex_unlock(&device->lock);
}

static void queue_execute_(void *this, uint32_t n, void *const *lists);
// ---- a queue's own thread (VXRT_D3D12_ASYNC_SUBMIT) ----------------------------------------------------------------
typedef struct qjob12 {
    struct qjob12 *next;
    void (*run)(queue12 *q, struct qjob12 *j);
    uint32_t n;
    void **objs;                       // ExecuteCommandLists: the lists (each referenced, pending counted)
    void *obj;                         // Signal/Wait: the fence; Present: the swap chain (referenced)
    uint64_t value;
    void *drawable, *texture;          // Present (CFBridgingRetain)
    uint32_t index;
    HANDLE waitable;
} qjob12;
static int async_submit12(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("VXRT_D3D12_ASYNC_SUBMIT"); v = e && *e == '1'; }
    return v;
}
static void *queue_worker12(void *arg) {
    queue12 *q = arg;
    pthread_setname_np("d3d12 queue");
    pthread_mutex_lock(&q->jlock);
    for (;;) {
        while (!q->jhead && !q->jstop) pthread_cond_wait(&q->jcond, &q->jlock);
        if (!q->jhead) break;
        qjob12 *j = q->jhead;
        q->jhead = j->next;
        if (!q->jhead) q->jtail = NULL;
        q->jbusy = 1;
        pthread_mutex_unlock(&q->jlock);
        @autoreleasepool { j->run(q, j); }
        free(j->objs);
        free(j);
        pthread_mutex_lock(&q->jlock);
        q->jbusy = 0;
        if (!q->jhead && !q->replays) pthread_cond_broadcast(&q->jidle);
    }
    pthread_mutex_unlock(&q->jlock);
    return NULL;
}
static void queue_post12(queue12 *q, qjob12 *j) {
    pthread_mutex_lock(&q->jlock);
    if (q->jtail) q->jtail->next = j; else q->jhead = j;
    q->jtail = j;
    pthread_cond_signal(&q->jcond);
    pthread_mutex_unlock(&q->jlock);
}
// (what bypasses the queue's thread waits until it has done everything posted)
static void queue_drain12(queue12 *q) {
    if (!q || !q->async) return;
    pthread_mutex_lock(&q->jlock);
    while (q->jhead || q->jbusy || q->replays) pthread_cond_wait(&q->jidle, &q->jlock);
    pthread_mutex_unlock(&q->jlock);
}
static void queue_async_start12(queue12 *q) {
    if (!async_submit12()) return;
    pthread_mutex_init(&q->jlock, NULL);
    pthread_cond_init(&q->jcond, NULL);
    pthread_cond_init(&q->jidle, NULL);
    if (pthread_create(&q->worker, NULL, queue_worker12, q) == 0) q->async = 1;
}
static void queue_async_stop12(queue12 *q) {
    if (!q->async) return;
    queue_drain12(q);
    pthread_mutex_lock(&q->jlock);
    q->jstop = 1;
    pthread_cond_signal(&q->jcond);
    pthread_mutex_unlock(&q->jlock);
    pthread_join(q->worker, NULL);
    q->async = 0;
}
// (a job's last references go from another thread: a swap chain or queue destroyed on the queue's own thread would
//  wait for that thread)
static void release_elsewhere12(d3d_object *o) {
    if (!o) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ d3d_release(o); });
}
// VXRT_D3D12_PARALLEL_REPLAY=1 (with VXRT_D3D12_ASYNC_SUBMIT): each list gets its command buffer enqueued in
// submission order on the queue's thread, which goes on at once; the replay into it and its commit run on a pool of
// threads (Metal runs enqueued command buffers in their enqueued order whenever they are committed). Lists are not
// state-validated there (as VXRT_D3D12_LENIENT_STATES after its first 1000 failures).
static int parallel_replay12(void) {
    static int v = -1;
    // (only with VXRT_D3D12_LENIENT_STATES: lists that must be rejected for their states are found serially)
    if (v < 0) { const char *e = getenv("VXRT_D3D12_PARALLEL_REPLAY"); v = e && *e == '1' && getenv("VXRT_D3D12_LENIENT_STATES"); }
    return v;
}
// VXRT_D3D12_CB_TRACE=<file>: when <file>.now appears (it is removed), the next 600 command buffers of every queue are
// written to <file> as they complete: queue type, what (L list with its timeline value and the longest-running
// commands' log, S signal / W wait with fence and value, P present), when it was enqueued and committed on the CPU and
// when the GPU ran it (seconds, one clock): to find where the GPU stands idle and what it waited for.
static const char *cb_trace_path12(void) {
    static const char *p = (const char *)-1;
    if (p == (const char *)-1) { p = getenv("VXRT_D3D12_CB_TRACE"); if (p && !*p) p = NULL; }
    return p;
}
static volatile int g_cb_trace_left12;
static __thread double t_cb_enq12;            // (when the list's command buffer was enqueued, if it was)
static void cb_trace_poll12(void) {                    // (at each present)
    const char *p = cb_trace_path12();
    if (!p || g_cb_trace_left12) return;
    char now[1024];
    snprintf(now, sizeof now, "%s.now", p);
    if (access(now, F_OK) == 0) { unlink(now); unlink(p); g_cb_trace_left12 = 600; }
}
static void cb_trace12(id<MTLCommandBuffer> cb, int type, char kind, const void *what, uint64_t value, double enq, const char *log) {
    if (!cb_trace_path12() || __atomic_load_n(&g_cb_trace_left12, __ATOMIC_RELAXED) <= 0) return;
    if (__atomic_sub_fetch(&g_cb_trace_left12, 1, __ATOMIC_RELAXED) < 0) return;
    double commit = CACurrentMediaTime();
    char *l = log ? strndup(log, 160) : NULL;
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&lk);
        FILE *f = fopen(cb_trace_path12(), "a");
        if (f) {
            fprintf(f, "%d %c %p %llu enq %.6f commit %.6f gpu %.6f %.6f %s\n", type, kind, what, (unsigned long long)value,
                    enq ? enq : commit, commit, done.GPUStartTime, done.GPUEndTime, l ? l : "");
            fclose(f);
        }
        pthread_mutex_unlock(&lk);
        free(l);
    }];
}

static void list_submit_replayed12(dev12 *d, queue12 *q, allocator12 *command_allocator, id<MTLCommandBuffer> cb,
                                   scratch sc, uint64_t value);
// (our own threads: GCD started fewer than were asked for under Rosetta, the rest of the slots waiting)
typedef struct replay_task12 { struct replay_task12 *next; void *run; } replay_task12;   // (run: CFBridgingRetain of the block)
static pthread_mutex_t g_rpool_lock12 = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rpool_cond12 = PTHREAD_COND_INITIALIZER;
static replay_task12 *g_rpool_head12, *g_rpool_tail12;
static void *replay_thread12(void *arg) {
    (void)arg;
    pthread_setname_np("d3d12 replay");
    for (;;) {
        pthread_mutex_lock(&g_rpool_lock12);
        while (!g_rpool_head12) pthread_cond_wait(&g_rpool_cond12, &g_rpool_lock12);
        replay_task12 *t = g_rpool_head12;
        g_rpool_head12 = t->next;
        if (!g_rpool_head12) g_rpool_tail12 = NULL;
        pthread_mutex_unlock(&g_rpool_lock12);
        @autoreleasepool { void (^run)(void) = CFBridgingRelease(t->run); run(); }
        free(t);
    }
    return NULL;
}
static void replay_pool_post12(void (^run)(void)) {
    replay_task12 *t = calloc(1, sizeof *t);
    t->run = (void *)CFBridgingRetain([run copy]);
    pthread_mutex_lock(&g_rpool_lock12);
    if (g_rpool_tail12) g_rpool_tail12->next = t; else g_rpool_head12 = t;
    g_rpool_tail12 = t;
    pthread_cond_signal(&g_rpool_cond12);
    pthread_mutex_unlock(&g_rpool_lock12);
}
static void job_execute_parallel12(queue12 *q, qjob12 *j) {
    static dispatch_semaphore_t slots;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("VXRT_D3D12_REPLAY_THREADS");
        int n = e && atoi(e) > 0 ? atoi(e) : 4;
        slots = dispatch_semaphore_create(n);
        for (int i = 0; i < n; i++) {
            pthread_t t;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 8 << 20);
            if (pthread_create(&t, &attr, replay_thread12, NULL) == 0) pthread_detach(t);
            pthread_attr_destroy(&attr);
        }
    });
    dev12 *d = q->dev;
    static int status_only = -1;
    if (status_only < 0) { const char *e = getenv("VXRT_D3D12_ENCODER_STATUS"); status_only = !(e && *e == '0'); }
    for (uint32_t i = 0; i < j->n; i++) {
        list12 *l = j->objs[i];
        allocator12 *queued_allocator = j->objs[j->n + i];
        if (!l) continue;
        pthread_mutex_lock(&d->alloc_lock);
        int list_stale = l->dev != d || !l->closed || !l->allocator || l->allocator->generation != l->allocator_generation;
        allocator12 *a = l->allocator;
        pthread_mutex_unlock(&d->alloc_lock);
        int unsupported = !list_stale && !queue_supports_list_type12(q->desc.Type, l->type);
        if (list_stale || unsupported) {
            vx_log(list_stale ? "D3D12: open command list or command list invalidated by allocator reset; skipping it" :
                                "D3D12: bundle or unsupported command-list type cannot be submitted directly; skipping it");
            if (queued_allocator) { __atomic_sub_fetch(&queued_allocator->queued, 1, __ATOMIC_RELEASE); release_elsewhere12(&queued_allocator->base); }
            __atomic_sub_fetch(&l->pending, 1, __ATOMIC_RELEASE);
            release_elsewhere12(&l->base);
            continue;
        }
        id<MTLCommandBuffer> cb;
        if (gpu_errors12() || status_only) {
            MTLCommandBufferDescriptor *cd = [MTLCommandBufferDescriptor new];
            cd.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
            cb = [q->mtl_queue commandBufferWithDescriptor:cd];
        } else cb = [q->mtl_queue commandBuffer];
        [cb enqueue];
        double enq_time = cb_trace_path12() ? CACurrentMediaTime() : 0;
        __atomic_add_fetch(&a->in_flight, 1, __ATOMIC_RELEASE);
        d3d_addref(&a->base);
        uint64_t value = ++q->timeline_value;
        pthread_mutex_lock(&d->alloc_lock);
        a->last_timeline = q->timeline;
        a->last_value = value;
        pthread_mutex_unlock(&d->alloc_lock);
        pthread_mutex_lock(&q->jlock);
        q->replays++;
        pthread_mutex_unlock(&q->jlock);
        dispatch_semaphore_wait(slots, DISPATCH_TIME_FOREVER);
        replay_pool_post12(^{
            @autoreleasepool {
                uint64_t t0 = fs_now12();
                __block id<MTLCommandBuffer> c = cb;
                id<MTLCommandBuffer> c2 = c;
                scratch sc = { nil, 0, nil };
                resource_state_shadow12 *shadows = NULL;
                uint32_t state_count = 0;
                cb_log_reset12();
                t_cb_enq12 = enq_time;
                replay(d, q->mtl_queue, q->desc.Type, l, &shadows, &state_count, &c2, &sc);
                queue_discard_list_states12(shadows, state_count);
                list_submit_replayed12(d, q, a, c2, sc, value);
                uint64_t took = fs_now12() - t0;
                FS_ADD12(replay_ns, took);
                for (uint64_t m = __atomic_load_n(&g_fs12.replay_max_ns, __ATOMIC_RELAXED);
                     took > m && !__atomic_compare_exchange_n(&g_fs12.replay_max_ns, &m, took, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED); ) {}
            }
            if (queued_allocator) { __atomic_sub_fetch(&queued_allocator->queued, 1, __ATOMIC_RELEASE); d3d_release(&queued_allocator->base); }
            __atomic_sub_fetch(&l->pending, 1, __ATOMIC_RELEASE);
            d3d_release(&l->base);
            dispatch_semaphore_signal(slots);
            pthread_mutex_lock(&q->jlock);
            if (!--q->replays && !q->jhead && !q->jbusy) pthread_cond_broadcast(&q->jidle);
            pthread_mutex_unlock(&q->jlock);
        });
    }
}
static void job_execute12(queue12 *q, qjob12 *j) {
    if (parallel_replay12()) { job_execute_parallel12(q, j); return; }
    uint64_t t0 = fs_now12();
    queue_execute_(q, j->n, j->objs);
    FS_ADD12(replay_ns, fs_now12() - t0);
    for (uint32_t i = 0; i < j->n; i++) {
        list12 *l = j->objs[i];
        if (!l) continue;
        allocator12 *a = j->objs[j->n + i];
        if (a) { __atomic_sub_fetch(&a->queued, 1, __ATOMIC_RELEASE); release_elsewhere12(&a->base); }
        __atomic_sub_fetch(&l->pending, 1, __ATOMIC_RELEASE);
        release_elsewhere12(&l->base);
    }
}

static WINAPI void queue_execute(void *this, uint32_t n, void *const *lists) {
    uint64_t t0 = fs_now12();
    queue12 *q = this;
    if (q->async && n && lists) {
        qjob12 *j = calloc(1, sizeof *j);
        j->run = job_execute12;
        j->n = n;
        j->objs = calloc(2 * (size_t)n, sizeof *j->objs);         // (the lists, then their allocators)
        for (uint32_t i = 0; i < n; i++) {
            list12 *l = lists[i];
            j->objs[i] = l;
            if (!l) continue;
            d3d_addref(&l->base);
            __atomic_add_fetch(&l->pending, 1, __ATOMIC_ACQ_REL);
            pthread_mutex_lock(&q->dev->alloc_lock);
            allocator12 *a = l->allocator;
            if (a) { d3d_addref(&a->base); __atomic_add_fetch(&a->queued, 1, __ATOMIC_ACQ_REL); }
            pthread_mutex_unlock(&q->dev->alloc_lock);
            j->objs[n + i] = a;
        }
        queue_post12(q, j);
    } else queue_execute_(this, n, lists);
    FS_ADD12(exec_n, 1); FS_ADD12(exec_ns, fs_now12() - t0);
}
// After a list's replay: the queue's timeline signal, the handler that frees what the GPU used, and the commit.
// value 0: the next timeline value, counted in flight on the list's allocator here; else counted already.
static void list_submit_replayed12(dev12 *d, queue12 *q, allocator12 *command_allocator, id<MTLCommandBuffer> cb,
                                   scratch sc, uint64_t value) {
    // (the scratch memory lives until the GPU is done with it: held by the handler; Metal holds the rest)
    NSArray *held = sc.bufs;
    char *what_ran = strdup(t_cb_log12);                   // (named when the command buffer fails)
    if (!value) {                                              // (in order here; else given when it was enqueued)
        __atomic_add_fetch(&command_allocator->in_flight, 1, __ATOMIC_RELEASE);
        value = ++q->timeline_value;
        pthread_mutex_lock(&d->alloc_lock);
        command_allocator->last_timeline = q->timeline;
        command_allocator->last_value = value;
        pthread_mutex_unlock(&d->alloc_lock);
        d3d_addref(&command_allocator->base);
    }
    [cb encodeSignalEvent:q->timeline value:value];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (held) scratch_recycle(d, held);
        if (done.GPUEndTime > done.GPUStartTime) {
            FS_ADD12(gpu_ns, (uint64_t)((done.GPUEndTime - done.GPUStartTime) * 1e9));
            // (busy: each span past the end of those before it - command buffers complete about in order)
            static double busy_end;
            static pthread_mutex_t busy_lock = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutex_lock(&busy_lock);
            double from = done.GPUStartTime > busy_end ? done.GPUStartTime : busy_end;
            if (done.GPUEndTime > from) FS_ADD12(busy_ns, (uint64_t)((done.GPUEndTime - from) * 1e9));
            if (done.GPUEndTime > busy_end) busy_end = done.GPUEndTime;
            pthread_mutex_unlock(&busy_lock);
        }
        char *ran = what_ran;
        if (done.status != MTLCommandBufferStatusError) { free(ran); ran = NULL; }
        if (done.status == MTLCommandBufferStatusError) {
            vx_log("D3D12: Metal command buffer failed: %s (it ran: %s)",
                   done.error.localizedDescription.UTF8String ?: "unknown Metal error", ran ? ran : "?");
            free(ran);
            NSArray *infos = done.error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
            for (id<MTLCommandBufferEncoderInfo> info in infos)
                if (info.errorState == MTLCommandEncoderErrorStateFaulted)
                    vx_log("D3D12:   faulted encoder: %s", info.label.UTF8String ?: "(compute or blit)");
            static int told;
            if (!told++) {
                uint64_t resident = 0, count = 0;
                if (@available(macOS 15.0, *)) if (d->residency) {
                    resident = [(id<MTLResidencySet>)d->residency allocatedSize];
                    count = [(id<MTLResidencySet>)d->residency allocationCount];
                }
                vx_log("D3D12: GPU memory: %llu MB allocated, %llu MB recommended working set, residency set %llu allocations %llu MB",
                       (unsigned long long)(d->mtl.currentAllocatedSize >> 20), (unsigned long long)(d->mtl.recommendedMaxWorkingSetSize >> 20),
                       (unsigned long long)count, (unsigned long long)(resident >> 20));
            }
        }
        __atomic_fetch_sub(&command_allocator->in_flight, 1, __ATOMIC_ACQ_REL);
        d3d_release(&command_allocator->base);
    }];
    cb_trace12(cb, (int)q->desc.Type, 'L', NULL, value, t_cb_enq12, what_ran);
    [cb commit];
}

static void queue_execute_(void *this, uint32_t n, void *const *lists) {
    queue12 *q = this;
    dev12 *d = q->dev;
    if (n && !lists) return;
    res12 **batch_touched = NULL;
    uint32_t batch_touched_count = 0, batch_touched_capacity = 0;
    uint64_t lock_t0 = fs_now12();
    pthread_mutex_lock(&d->submit_lock);
    FS_ADD12(lock_ns, fs_now12() - lock_t0);
    for (uint32_t i = 0; i < n; i++) {
        list12 *l = lists[i];
        if (!l || l->dev != d) continue;
        pthread_mutex_lock(&d->alloc_lock);
        int list_stale = !l->closed || !l->allocator || l->allocator->generation != l->allocator_generation;
        pthread_mutex_unlock(&d->alloc_lock);
        if (list_stale) {
            vx_log("D3D12: open command list or command list invalidated by allocator reset; skipping it");
            continue;
        }
        if (!queue_supports_list_type12(q->desc.Type, l->type)) {
            vx_log("D3D12: bundle or unsupported command-list type cannot be submitted directly; skipping it");
            continue;
        }
        resource_state_shadow12 *shadows = NULL;
        uint32_t state_count = 0;
        // VXRT_D3D12_LENIENT_STATES=1: run such a list anyway, as Windows does without its debug layer (Metal tracks
        // the hazards itself); its transitions are then not recorded. Once 1000 lists have failed, the states are
        // not tracked well enough to be worth validating: lists run without (it was a tenth of the replay's time in
        // Cyberpunk 2077, whose lists nearly all fail); VXRT_D3D12_VALIDATE_STATES=1 keeps validating.
        static int lenient = -1, told, keep_validating = -1;
        if (lenient < 0) lenient = getenv("VXRT_D3D12_LENIENT_STATES") != NULL;
        if (keep_validating < 0) keep_validating = getenv("VXRT_D3D12_VALIDATE_STATES") != NULL;
        int state_result = lenient && told >= 1000 && !keep_validating ? 1 :
            queue_validate_list_states12(d, q->desc.Type, q->mtl_queue, l, NULL, 0, &shadows, &state_count);
        if (state_result != 1) {
            if (state_result == 0 && lenient) {
                if (told++ % 1000 == 0) vx_log("D3D12: command list has an invalid transition or resource-use state; run anyway (%d so far)", told);
                shadows = NULL;
                state_count = 0;
            } else {
                if (state_result < 0) vx_log("D3D12: could not allocate resource-state validation data; skipping command list");
                else vx_log("D3D12: command list has an invalid transition or resource-use state; skipping it");
                continue;
            }
        }
        if (state_count > UINT32_MAX - batch_touched_count ||
            !queue_batch_touched_reserve12(&batch_touched, &batch_touched_capacity,
                                           batch_touched_count + state_count)) {
            vx_log("D3D12: could not allocate submission-batch resource-state data; skipping command list");
            queue_discard_list_states12(shadows, state_count);
            continue;
        }
        id<MTLCommandBuffer> cb;
        // Command buffers report encoder status (MTLCommandBufferErrorOptionEncoderExecutionStatus): without it,
        // Cyberpunk 2077's intro faulted the GPU ("Insufficient Memory", every later submission ignored) in 4 of 5
        // runs, with it in none of 11 - the driver keeps the encoders of such a command buffer apart. The cause behind
        // is not found yet. VXRT_D3D12_ENCODER_STATUS=0 turns it off.
        static int status_only = -1;
        if (status_only < 0) { const char *e = getenv("VXRT_D3D12_ENCODER_STATUS"); status_only = !(e && *e == '0'); }
        if (q->async) pthread_mutex_unlock(&d->submit_lock);         // (this may wait for the GPU: not with the lock held)
        if (gpu_errors12() || status_only) {
            MTLCommandBufferDescriptor *cd = [MTLCommandBufferDescriptor new];
            cd.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
            cb = [q->mtl_queue commandBufferWithDescriptor:cd];
        } else cb = [q->mtl_queue commandBuffer];
        if (q->async) pthread_mutex_lock(&d->submit_lock);
        scratch sc = { nil, 0, nil };
        cb_log_reset12();
        if (!replay(d, q->mtl_queue, q->desc.Type, l, &shadows, &state_count, &cb, &sc)) {
            queue_discard_list_states12(shadows, state_count);
            continue;
        }
        list_submit_replayed12(d, q, l->allocator, cb, sc, 0);
        for (uint32_t j = 0; j < state_count; j++) batch_touched[batch_touched_count++] = shadows[j].resource;
        queue_commit_list_states12(d, shadows, state_count);
    }
    // D3D12 keeps state across lists passed to one ExecuteCommandLists call, then decays eligible resources once
    // the whole batch completes. submit_lock preserves this logical order for later calls on the queue.
    queue_decay_batch_states12(d, batch_touched, batch_touched_count, q->desc.Type);
    free(batch_touched);
    pthread_mutex_unlock(&d->submit_lock);
}

static HRESULT queue_signal_now12(queue12 *q, fence12 *fence_object, uint64_t v);
static HRESULT queue_wait_now12(queue12 *q, fence12 *fence_object, uint64_t v);
static void job_signal12(queue12 *q, qjob12 *j) { queue_signal_now12(q, j->obj, j->value); release_elsewhere12(&((fence12 *)j->obj)->base); }
static void job_wait12(queue12 *q, qjob12 *j) { queue_wait_now12(q, j->obj, j->value); release_elsewhere12(&((fence12 *)j->obj)->base); }
static WINAPI HRESULT queue_signal(void *this, void *fence, uint64_t v) {
    queue12 *q = this;
    fence12 *fence_object = fence;
    if (!fence_object || fence_object->base.kind != K12_FENCE || fence_object->dev != q->dev ||
        !fence_object->event) return E_INVALIDARG;
    __atomic_store_n(&fence_object->posted, v, __ATOMIC_RELAXED);
    if (q->async) {
        qjob12 *j = calloc(1, sizeof *j);
        j->run = job_signal12;
        j->obj = fence_object;
        j->value = v;
        d3d_addref(&fence_object->base);
        queue_post12(q, j);
        return S_OK;
    }
    return queue_signal_now12(q, fence_object, v);
}
static HRESULT queue_signal_now12(queue12 *q, fence12 *fence_object, uint64_t v) {
    void *fence = fence_object;
    id<MTLCommandBuffer> cb = [q->mtl_queue commandBuffer];       // (before the lock: this may wait for the GPU)
    if (!cb) return E_FAIL;
    pthread_mutex_lock(&q->dev->submit_lock);
    [cb encodeSignalEvent:fence_object->event value:v];
    __atomic_store_n(&fence_object->posted_ran, v, __ATOMIC_RELAXED);
    {
        int type = (int)q->desc.Type;
        void *fp = fence_object;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            if (done.status == MTLCommandBufferStatusError)
                vx_log("D3D12: a queue (type %d) signal of fence %p = %llu failed (%s)", type, fp, (unsigned long long)v,
                       done.error.localizedDescription.UTF8String ?: "?");
        }];
    }
    if (trace_fences12()) {
        static volatile int n;
        if (__sync_add_and_fetch(&n, 1) <= 40) vx_log("D3D12: queue %p (type %d) signals fence %p = %llu (now %llu)", (void *)q, (int)q->desc.Type,
                                                      fence, (unsigned long long)v, (unsigned long long)fence_object->event.signaledValue);
    }
    cb_trace12(cb, (int)q->desc.Type, 'S', fence, v, 0, NULL);
    [cb commit];
    pthread_mutex_unlock(&q->dev->submit_lock);
    return S_OK;
}
static WINAPI HRESULT queue_wait(void *this, void *fence, uint64_t v) {
    queue12 *q = this;
    fence12 *fence_object = fence;
    if (!fence_object || fence_object->base.kind != K12_FENCE || fence_object->dev != q->dev ||
        !fence_object->event) return E_INVALIDARG;
    if (q->async) {
        qjob12 *j = calloc(1, sizeof *j);
        j->run = job_wait12;
        j->obj = fence_object;
        j->value = v;
        d3d_addref(&fence_object->base);
        queue_post12(q, j);
        return S_OK;
    }
    return queue_wait_now12(q, fence_object, v);
}
static HRESULT queue_wait_now12(queue12 *q, fence12 *fence_object, uint64_t v) {
    id<MTLCommandBuffer> cb = [q->mtl_queue commandBuffer];       // (before the lock: this may wait for the GPU)
    if (!cb) return E_FAIL;
    pthread_mutex_lock(&q->dev->submit_lock);
    fence_watch12(fence_object, v, "queue", q);
    [cb encodeWaitForEvent:fence_object->event value:v];
    {
        id<MTLSharedEvent> ev = fence_object->event;
        int type = (int)q->desc.Type;
        fence12 *fp = fence_object;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            if (done.status == MTLCommandBufferStatusError)
                vx_log("D3D12: a queue (type %d) wait for fence %p >= %llu failed (%s); the fence is at %llu, the last Signal call asked for %llu, the last committed %llu",
                       type, (void *)fp, (unsigned long long)v, done.error.localizedDescription.UTF8String ?: "?", (unsigned long long)ev.signaledValue,
                       (unsigned long long)fp->posted, (unsigned long long)fp->posted_ran);
        }];
    }
    cb_trace12(cb, (int)q->desc.Type, 'W', fence_object, v, 0, NULL);
    [cb commit];
    pthread_mutex_unlock(&q->dev->submit_lock);
    return S_OK;
}
static WINAPI HRESULT queue_timestamp_frequency(void *this, uint64_t *f) {
    queue12 *queue = this;
    if (queue->desc.Type == 3 && !d3d12_copy_timestamp_counters_supported12(queue->dev)) return E_FAIL;
    if (f) *f = 1000000000ull;
    return S_OK;
}
// (the GPU's timestamps and QueryPerformanceCounter are both nanoseconds of the same clock, mach_absolute_time's)
static WINAPI HRESULT queue_clock_calibration(void *this, uint64_t *gpu, uint64_t *cpu) {
    queue12 *queue = this;
    if (queue->desc.Type == 3 && !d3d12_copy_timestamp_counters_supported12(queue->dev)) return E_FAIL;
    MTLTimestamp c = 0, g = 0;
    [queue->dev->mtl sampleTimestamps:&c gpuTimestamp:&g];
    if (gpu) *gpu = g;
    if (cpu) *cpu = c;
    return S_OK;
}
static WINAPI Q_DESC *queue_get_desc(void *this, Q_DESC *out) { *out = ((queue12 *)this)->desc; return out; }

static WINAPI HRESULT dev_create_queue(void *this, const Q_DESC *desc, const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    *out = NULL;
    if (!desc || (desc->Type != 0 && desc->Type != 2 && desc->Type != 3) ||
        (desc->Priority != 0 && desc->Priority != 100 && desc->Priority != 10000) ||
        (desc->Flags & ~1) || desc->NodeMask != 0) return E_INVALIDARG;
    // Metal does not expose a queue scheduling guarantee equivalent to D3D12's global real-time priority.
    if (desc->Priority == 10000) return DXGI_ERROR_UNSUPPORTED;
    dev12 *d = this;
    queue12 *q = alloc12(sizeof *q, K12_QUEUE, g_queue_vtbl, d);
    q->base.destroy = queue_destroy;
    q->desc = *desc;
    // (Metal's default of 64 command buffers in flight blocks the next commandBuffer call until one completes: a
    //  queue's thread holding the submission lock there, while the GPU waits for another queue to signal, would wait
    //  for ever)
    q->mtl_queue = async_submit12() ? [d->mtl newCommandQueueWithMaxCommandBufferCount:1024] : [d->mtl newCommandQueue];
    q->timeline = [d->mtl newSharedEvent];
    if (d->residency) if (@available(macOS 15.0, *)) [q->mtl_queue addResidencySet:(id<MTLResidencySet>)d->residency];
    if (!q->mtl_queue || !q->timeline) { d3d_release(&q->base); return E_FAIL; }
    queue_async_start12(q);
    pthread_mutex_lock(&d->lock);
    for (int i = 0; i < 16; i++) if (!d->queues[i]) { d->queues[i] = q; break; }
    pthread_mutex_unlock(&d->lock);
    HRESULT hr = queue_query(q, iid, out);
    d3d_release(&q->base);
    return hr;
}

// ---- allocators and lists -----------------------------------------------------------------------------------------------

static WINAPI HRESULT allocator_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12CommandAllocator, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}
static WINAPI HRESULT allocator_reset(void *this) {
    allocator12 *allocator = this;
    pthread_mutex_lock(&allocator->dev->alloc_lock);
    HRESULT hr = S_OK;
    // A program resets an allocator once its fence says the GPU is done. Metal runs the completion handlers that count
    // the allocator's command buffers down later (they lag the shared events fences are), so the GPU's progress is
    // read from the queue's timeline, which advances as fences do.
    int gpu_done = allocator->last_timeline && allocator->last_timeline.signaledValue >= allocator->last_value;
    if (allocator->recording_lists || (!gpu_done && __atomic_load_n(&allocator->in_flight, __ATOMIC_ACQUIRE)) ||
        __atomic_load_n(&allocator->queued, __ATOMIC_ACQUIRE) ||
        allocator->generation == UINT64_MAX) {
        hr = E_FAIL;
    } else {
        allocator->generation++;
    }
    pthread_mutex_unlock(&allocator->dev->alloc_lock);
    return hr;
}
static void allocator_destroy(d3d_object *o) {
    ((allocator12 *)o)->last_timeline = nil;
    release_dev(o);
}
static WINAPI HRESULT dev_create_allocator(void *this, int32_t type, const GUID *iid, void **out) {
    if (type < 0 || type > 3) return E_INVALIDARG;
    allocator12 *a = alloc12(sizeof *a, K12_ALLOCATOR, g_allocator_vtbl, this);
    a->base.destroy = allocator_destroy;
    a->type = type;
    a->generation = 1;
    HRESULT hr = allocator_query(a, iid, out);
    d3d_release(&a->base);
    return hr;
}
static WINAPI HRESULT dev_create_list(void *this, uint32_t node, int32_t type, void *allocator, void *pso, const GUID *iid, void **out) {
    dev12 *device = this;
    allocator12 *command_allocator = allocator;
    if (type < 0 || type > 3 || !command_allocator || command_allocator->base.kind != K12_ALLOCATOR ||
        command_allocator->dev != device || command_allocator->type != type) return E_INVALIDARG;
    pthread_mutex_lock(&device->alloc_lock);
    if (command_allocator->recording_lists) {
        pthread_mutex_unlock(&device->alloc_lock);
        return E_INVALIDARG;
    }
    list12 *l = alloc12(sizeof *l, K12_LIST, g_list_vtbl, this);
    l->base.destroy = list_destroy;
    l->type = type;
    l->allocator = command_allocator;
    l->allocator_generation = command_allocator->generation;
    l->allocator_recording = 1;
    command_allocator->recording_lists++;
    d3d_addref(&command_allocator->base);
    l->recording_topology = 4;
    for (uint32_t i = 0; i < 4; i++) l->recording_blend_factor[i] = 1.0f;
    gpu_ir_stream_init(&l->gpu_ir);
    if (pso) list_set_pso(l, pso);
    pthread_mutex_unlock(&device->alloc_lock);
    HRESULT hr = list_query(l, iid, out);
    d3d_release(&l->base);
    return hr;
}

// ID3D12Device4::CreateCommandList1: a list created closed, without an allocator; Reset gives it one.
static WINAPI HRESULT dev_create_list1(void *this, uint32_t node, int32_t type, int32_t flags, const GUID *iid, void **out) {
    dev12 *device = this;
    if (out) *out = NULL;
    if (type < 0 || type > 3 || flags || node > 1) return E_INVALIDARG;
    if (!out) return S_FALSE;
    pthread_mutex_lock(&device->submit_lock);
    list12 *l = alloc12(sizeof *l, K12_LIST, g_list_vtbl, this);
    l->base.destroy = list_destroy;
    l->type = type;
    l->closed = 1;
    l->recording_topology = 4;
    for (uint32_t i = 0; i < 4; i++) l->recording_blend_factor[i] = 1.0f;
    gpu_ir_stream_init(&l->gpu_ir);
    pthread_mutex_unlock(&device->submit_lock);
    HRESULT hr = list_query(l, iid, out);
    d3d_release(&l->base);
    return hr;
}

static WINAPI HRESULT dev_create_heap(void *this, const HEAP_DESC *desc, const GUID *iid, void **out) {
    dev12 *d = this;
    if (!desc) return E_INVALIDARG;
    if (out) *out = NULL;
    if (desc->Type < 0 || desc->Type > 3 || !desc->NumDescriptors ||
        (desc->Flags & ~1) || (desc->Flags & 1 && desc->Type > 1) || desc->NodeMask ||
        (desc->Type == 1 && (desc->Flags & 1) && desc->NumDescriptors > 2048))
        return E_INVALIDARG;
    if (!out) return S_FALSE;                                      // capability test: validate but do not create the heap
    heap12 *h = alloc12(sizeof *h, K12_HEAP, g_heap_vtbl, d);
    h->base.destroy = heap_destroy;
    h->desc = *desc;
    h->residency_refs = 1;
    h->residency_priority = VX12_RESIDENCY_PRIORITY_NORMAL;
    size_t count = desc->NumDescriptors ? desc->NumDescriptors : 1;
    size_t size = 32 * count;
    if (desc->Flags & 1) {                                         // SHADER_VISIBLE
        h->gpu = [d->mtl newBufferWithLength:size options:MTLResourceStorageModeShared];
        if (!h->gpu) { d3d_release(&h->base); return E_OUTOFMEMORY; }
        h->cpu = h->gpu.contents;
        memset(h->cpu, 0, size);
        pthread_mutex_lock(&d->lock);
        [d->resources addObject:h->gpu];
        residency_add12(d, h->gpu);
        pthread_mutex_unlock(&d->lock);
    } else {
        h->cpu = calloc(1, size);
    }
    h->keep = (__strong id *)calloc(count, sizeof(id));
    pthread_mutex_init(&h->texture_lock, NULL);
    if (desc->Type == 0) h->textures = [NSCountedSet new];
    h->descriptor_kind = calloc(count, sizeof(*h->descriptor_kind));
    h->buffer_view = calloc(count, sizeof(*h->buffer_view));
    h->shader_view = calloc(count, sizeof(*h->shader_view));
    h->target_resource = calloc(count, sizeof(*h->target_resource));
    h->target_subresource = calloc(count, sizeof(*h->target_subresource));
    h->target_flags = calloc(count, sizeof(*h->target_flags));
    if (!h->cpu || !h->keep || !h->descriptor_kind || !h->buffer_view || !h->shader_view || !h->target_resource || !h->target_subresource || !h->target_flags) {
        d3d_release(&h->base);
        return E_OUTOFMEMORY;
    }
    pthread_mutex_lock(&g_heaps_lock);
    int registered = g_nheaps < 4096;
    if (registered) { g_heaps[g_nheaps++] = h; __atomic_add_fetch(&g_heaps_gen12, 1, __ATOMIC_RELEASE); }
    pthread_mutex_unlock(&g_heaps_lock);
    if (!registered) { d3d_release(&h->base); return E_OUTOFMEMORY; }
    HRESULT hr = heap_query(h, iid, out);
    d3d_release(&h->base);
    return hr;
}

static WINAPI uint32_t dev_increment(void *this, int32_t type) { return 32; }


// ---- swap chains on D3D12 queues (DXGI's factory hands them here) ---------------------------------------------------
//
// BufferCount textures the program gets one by one (GetBuffer) and renders into in turn (GetCurrentBackBufferIndex);
// Present draws the current one into the window's CAMetalLayer on the queue's Metal queue, after the work executed
// before it, and moves on to the next.

typedef struct {
    d3d_object base;
    dev12 *dev;
    queue12 *queue;
    void *factory;
    HANDLE hwnd;
    uint32_t width, height, format, count, flags, index, presents;
    res12 *buffers[16];
    CAMetalLayer *layer;
    id<MTLRenderPipelineState> pipeline;
    // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT: a semaphore with one count per frame the program may have
    // queued (SetMaximumFrameLatency, 1 by default); each present gives its count back when the GPU has drawn it.
    // Without the flag the object is an event that stays signalled.
    HANDLE waitable;
    uint32_t max_latency, src_width, src_height;
} swap12;
enum { SWAP_FLAG_FRAME_LATENCY_WAITABLE = 0x40 };

static const char *g_present12_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V present_vs(uint id [[vertex_id]]) {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    V o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "fragment float4 present_fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], constant float2 &scale [[buffer(0)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    return float4(t.sample(s, in.uv * scale).rgb, 1);\n"
    "}\n";

static HRESULT swap_make_buffers(swap12 *sc) {
    for (uint32_t i = 0; i < sc->count; i++) {
        if (sc->buffers[i]) { d3d_release(&sc->buffers[i]->base); sc->buffers[i] = NULL; }
        HEAP_PROPS hp = { 1, 0, 0, 0, 0 };
        RES_DESC rd = { 3, 0, sc->width, sc->height, 1, 1, sc->format, 1, 0, 0, 0x1 };   // a 2D render target
        void *out = NULL;
        HRESULT hr = create_committed(sc->dev, &hp, 0, &rd, 0, NULL, &IID_ID3D12Resource, &out);
        if (hr != S_OK) return hr;
        sc->buffers[i] = out;
    }
    sc->index = 0;
    return S_OK;
}

static void swap_destroy(d3d_object *o) {
    swap12 *sc = (swap12 *)o;
    queue_drain12(sc->queue);
    for (uint32_t i = 0; i < 16; i++) if (sc->buffers[i]) d3d_release(&sc->buffers[i]->base);
    sc->layer = nil;
    sc->pipeline = nil;
    if (sc->queue) d3d_release(&sc->queue->base);
    if (sc->factory) d3d_release(sc->factory);
    if (sc->dev) d3d_release(&sc->dev->base);
}

int d3d12_is_queue(void *p) { return p && ((d3d_object *)p)->vtbl == g_queue_vtbl; }
// (a queue another API submits to directly, as D3D11On12 does, keeps its order only without its own thread)
void *d3d12_queue_metal(void *queue) {
    if (!d3d12_is_queue(queue)) return NULL;
    queue_async_stop12((queue12 *)queue);
    return (__bridge void *)((queue12 *)queue)->mtl_queue;
}
void *d3d12_device_of(void *p) {
    if (!p) return NULL;
    if (((d3d_object *)p)->vtbl == g_device_vtbl) return p;
    return d3d12_is_queue(p) ? ((queue12 *)p)->dev : NULL;
}
int d3d12_resource_info_get(void *resource, d3d12_resource_info *out) {
    res12 *r = resource;
    if (!r || r->base.vtbl != g_resource_vtbl || !out) return 0;
    *out = (d3d12_resource_info){ r->desc.Dimension, r->desc.Width, r->desc.Height, r->desc.DepthOrArraySize, r->desc.MipLevels,
                                  r->desc.Format, r->desc.SampleCount, r->desc.SampleQuality, r->desc.Flags,
                                  (__bridge void *)r->texture, (__bridge void *)r->buffer };
    return 1;
}
void d3d12_resource_force_state(void *resource, uint32_t state) {
    res12 *r = resource;
    if (!r || r->base.vtbl != g_resource_vtbl) return;
    pthread_mutex_lock(&r->dev->lock);
    state_overrides_free12(r->state_overrides);
    r->state_overrides = NULL;
    state_pending_free12(r->pending_transitions);
    r->pending_transitions = NULL;
    r->initial_state = state;
    r->initial_promoted = 0;
    pthread_mutex_unlock(&r->dev->lock);
}

HRESULT d3d12_swapchain_create(void *factory, void *queue, HANDLE hwnd, uint32_t w, uint32_t h, uint32_t format, uint32_t count, uint32_t flags, void **out) {
    queue12 *q = queue;
    vx_window *win = vx_window_get(hwnd);
    if (!win || !out) return E_INVALIDARG;
    if (count < 2 || count > 16) count = count < 2 ? 2 : 16;
    swap12 *sc = d3d_alloc(sizeof *sc, K_SWAPCHAIN, g_swapchain_vtbl, NULL);
    sc->base.destroy = swap_destroy;
    sc->dev = q->dev; d3d_addref(&sc->dev->base);
    sc->queue = q; d3d_addref(&q->base);
    sc->factory = factory; d3d_addref(factory);
    sc->hwnd = hwnd;
    if (!w || !h) { w = win->client_w > 0 ? (uint32_t)win->client_w : 640; h = win->client_h > 0 ? (uint32_t)win->client_h : 480; }
    sc->width = sc->src_width = w; sc->height = sc->src_height = h; sc->format = format; sc->count = count; sc->flags = flags;
    sc->max_latency = 1;
    if (flags & SWAP_FLAG_FRAME_LATENCY_WAITABLE) {
        HANDLE (WINAPI *create)(void *, LONG, LONG, const uint16_t *) =
            (HANDLE (WINAPI *)(void *, LONG, LONG, const uint16_t *))vx_find_export("kernel32.dll", "CreateSemaphoreW");
        sc->waitable = create(NULL, 1, 16, NULL);
    }
    MTLPixelFormat lf = d3d_is_srgb(format) ? MTLPixelFormatBGRA8Unorm_sRGB : format == 10 ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
    sc->layer = (__bridge CAMetalLayer *)vx_cocoa_metal_layer(win, (__bridge void *)sc->dev->mtl, (int)lf);
    NSError *err = nil;
    id<MTLLibrary> lib = [sc->dev->mtl newLibraryWithSource:@(g_present12_msl) options:nil error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"present_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"present_fs"];
    pd.colorAttachments[0].pixelFormat = lf;
    sc->pipeline = lib ? [sc->dev->mtl newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
    if (!sc->layer || !sc->pipeline || swap_make_buffers(sc) != S_OK) { d3d_release(&sc->base); return E_FAIL; }
    *out = sc;
    return S_OK;
}

static WINAPI HRESULT swap_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_IDXGIObject, &IID_IDXGIDeviceSubObject, &IID_IDXGISwapChain, &IID_IDXGISwapChain1, &IID_IDXGISwapChain2,
                        &IID_IDXGISwapChain3, &IID_IDXGISwapChain4 };
    return query(this, iid, out, k, 7);
}
static WINAPI HRESULT swap_get_buffer(void *this, uint32_t i, const GUID *iid, void **out) {
    swap12 *sc = this;
    if (i >= sc->count || !sc->buffers[i]) return E_INVALIDARG;
    return res_query(sc->buffers[i], iid, out);
}
static WINAPI uint32_t swap_index(void *this) { return ((swap12 *)this)->index; }
// VXRT_PRESENT_SNAPSHOT=file.png (VXRT_PRESENT_SNAPSHOT_EVERY=n, default 300): every n-th present's picture, as the
// window shows it, written to that file (to see what a program draws without a screen capture); the log notes
// the presents every 300.
static void swap_snapshot12(swap12 *sc, uint32_t index) {
    static const char *path;
    static int every = -1;
    if (every < 0) {
        path = getenv("VXRT_PRESENT_SNAPSHOT");
        every = getenv("VXRT_PRESENT_SNAPSHOT_EVERY") ? atoi(getenv("VXRT_PRESENT_SNAPSHOT_EVERY")) : 300;
        if (every < 1) every = 300;
    }
    if (sc->presents % 300 == 0) vx_log("D3D12: %u presents (%ux%u)", sc->presents, sc->width, sc->height);
    if (!path || sc->presents % (uint32_t)every) return;
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:sc->width height:sc->height mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [sc->dev->mtl newTextureWithDescriptor:td];
        id<MTLBuffer> out = [sc->dev->mtl newBufferWithLength:(NSUInteger)sc->width * sc->height * 4 options:MTLResourceStorageModeShared];
        if (!target || !out || sc->layer.pixelFormat != MTLPixelFormatBGRA8Unorm) return;
        id<MTLCommandBuffer> cb = [sc->queue->mtl_queue commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:sc->pipeline];
        [enc setFragmentTexture:sc->buffers[index]->texture atIndex:0];
        float scale[2] = { (float)sc->src_width / (float)sc->width, (float)sc->src_height / (float)sc->height };
        [enc setFragmentBytes:scale length:sizeof scale atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(sc->width, sc->height, 1)
                     toBuffer:out destinationOffset:0 destinationBytesPerRow:sc->width * 4 destinationBytesPerImage:(NSUInteger)sc->width * sc->height * 4];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(out.contents, sc->width, sc->height, 8, sc->width * 4, cs,
                                                 kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst);
        CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
        NSURL *url = [NSURL fileURLWithPath:@(path)];
        CGImageDestinationRef dst = img ? CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL) : NULL;
        if (dst) { CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
        if (img) CGImageRelease(img);
        if (ctx) CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
    }
}
// (the copy of the back buffer into the drawable, and the present: on the queue's thread with VXRT_D3D12_ASYNC_SUBMIT)
static void swap_present_now12(swap12 *sc, uint32_t index, id<CAMetalDrawable> drawable, HANDLE waitable) {
    int queued = 0;
    id<MTLCommandBuffer> cb = drawable ? [sc->queue->mtl_queue commandBuffer] : nil;   // (before the lock: may wait)
    // (the snapshot waits for the GPU: not with the submission lock held, which another queue's Signal the GPU may be
    //  waiting for needs)
    swap_snapshot12(sc, index);
    pthread_mutex_lock(&sc->dev->submit_lock);
    @autoreleasepool {
        if (drawable && cb) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = drawable.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:sc->pipeline];
            [enc setFragmentTexture:sc->buffers[index]->texture atIndex:0];
            float scale[2] = { (float)sc->src_width / (float)sc->width, (float)sc->src_height / (float)sc->height };
            [enc setFragmentBytes:scale length:sizeof scale atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
            [cb presentDrawable:drawable];
            if (waitable) [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { (void)done; vx_release_semaphore(waitable, 1); }];
            cb_trace12(cb, (int)sc->queue->desc.Type, 'P', sc, sc->presents, 0, NULL);
            [cb commit];
            queued = 1;
        }
    }
    pthread_mutex_unlock(&sc->dev->submit_lock);
    if (waitable && !queued) vx_release_semaphore(waitable, 1);
}
static void job_present12(queue12 *q, qjob12 *j) {
    swap12 *sc = j->obj;
    id<CAMetalDrawable> drawable = j->drawable ? CFBridgingRelease(j->drawable) : nil;
    swap_present_now12(sc, j->index, drawable, j->waitable);
    release_elsewhere12(&sc->base);
}
static WINAPI HRESULT swap_present(void *this, uint32_t sync, uint32_t flags) {
    swap12 *sc = this;
    if (flags & 1) return S_OK;                                    // DXGI_PRESENT_TEST
    if (sync > 4) return DXGI_ERROR_INVALID_CALL;
    sc->layer.displaySyncEnabled = sync != 0;
    HANDLE waitable = sc->flags & SWAP_FLAG_FRAME_LATENCY_WAITABLE ? sc->waitable : NULL;
    @autoreleasepool {
        // (the drawable is waited for before the submission lock: a wait for the display holds up no other thread's
        //  ExecuteCommandLists)
        uint64_t t0 = fs_now12();
        id<CAMetalDrawable> drawable = [sc->layer nextDrawable];
        FS_ADD12(drawable_ns, fs_now12() - t0);
        if (sc->queue->async) {
            qjob12 *j = calloc(1, sizeof *j);
            j->run = job_present12;
            j->obj = sc;
            j->index = sc->index;
            j->drawable = drawable ? (void *)CFBridgingRetain(drawable) : NULL;
            j->waitable = waitable;
            d3d_addref(&sc->base);
            queue_post12(sc->queue, j);
        } else swap_present_now12(sc, sc->index, drawable, waitable);
    }
    sc->index = (sc->index + 1) % sc->count;
    sc->presents++;
    frame_stats_present12(sc->presents);
    cb_trace_poll12();
    skip_poll12();
    census_present12();
    pass_dump_present12();
    watch_present12(sc->presents);
    prof_present12(sc->dev->mtl);
    return S_OK;
}
static WINAPI HRESULT swap_present1(void *this, uint32_t sync, uint32_t flags, const void *params) { return swap_present(this, sync, flags); }
static WINAPI HRESULT swap_resize(void *this, uint32_t count, uint32_t w, uint32_t h, uint32_t format, uint32_t flags) {
    swap12 *sc = this;
    queue_drain12(sc->queue);
    vx_window *win = vx_window_get(sc->hwnd);
    if (count) sc->count = count > 16 ? 16 : count;
    if (format) sc->format = format;
    if (!w || !h) { w = win && win->client_w > 0 ? (uint32_t)win->client_w : sc->width; h = win && win->client_h > 0 ? (uint32_t)win->client_h : sc->height; }
    sc->width = sc->src_width = w; sc->height = sc->src_height = h;
    sc->flags = (flags & ~SWAP_FLAG_FRAME_LATENCY_WAITABLE) | (sc->flags & SWAP_FLAG_FRAME_LATENCY_WAITABLE);
    return swap_make_buffers(sc);
}
static WINAPI HRESULT swap_resize3(void *this, uint32_t count, uint32_t w, uint32_t h, uint32_t format, uint32_t flags, const uint32_t *masks, void *const *queues) {
    return swap_resize(this, count, w, h, format, flags);
}
// DXGI_SWAP_CHAIN_DESC1: width, height, format, stereo, sample count/quality, usage, buffer count, scaling, effect, alpha, flags
static WINAPI HRESULT swap_get_desc1(void *this, uint32_t *d) {
    swap12 *sc = this;
    memset(d, 0, 11 * 4);
    d[0] = sc->width; d[1] = sc->height; d[2] = sc->format; d[4] = 1; d[6] = 0x20; d[7] = sc->count; d[9] = 4; d[10] = sc->flags;
    return S_OK;
}
// DXGI_SWAP_CHAIN_DESC: mode (width, height, refresh, format, ordering, scaling), samples, usage, count, window, windowed, effect, flags
static WINAPI HRESULT swap_get_desc(void *this, uint8_t *d) {
    swap12 *sc = this;
    memset(d, 0, 68);
    uint32_t *m = (uint32_t *)d;
    m[0] = sc->width; m[1] = sc->height; m[2] = 60; m[3] = 1; m[4] = sc->format;
    *(uint32_t *)(d + 28) = 1; *(uint32_t *)(d + 36) = 0x20; *(uint32_t *)(d + 40) = sc->count;
    *(HANDLE *)(d + 48) = sc->hwnd; *(int32_t *)(d + 56) = 1; *(uint32_t *)(d + 60) = 4; *(uint32_t *)(d + 64) = sc->flags;
    return S_OK;
}
static WINAPI HRESULT swap_get_hwnd(void *this, HANDLE *h) { *h = ((swap12 *)this)->hwnd; return S_OK; }
static WINAPI HRESULT swap_set_fullscreen(void *this, int32_t full, void *output) { return S_OK; }
static WINAPI HRESULT swap_get_fullscreen(void *this, int32_t *full, void **output) { if (full) *full = 0; if (output) *output = NULL; return S_OK; }
static WINAPI HRESULT swap_get_device(void *this, const GUID *iid, void **out) { return queue_query(((swap12 *)this)->queue, iid, out); }
static WINAPI HRESULT swap_get_parent(void *this, const GUID *iid, void **out) {
    void *f = ((swap12 *)this)->factory;
    return ((HRESULT (WINAPI *)(void *, const GUID *, void **))(((d3d_object *)f)->vtbl[0]))(f, iid, out);
}
static WINAPI HRESULT swap_set_latency(void *this, uint32_t n) {
    swap12 *sc = this;
    if (n < 1 || n > 16) return DXGI_ERROR_INVALID_CALL;
    if (sc->flags & SWAP_FLAG_FRAME_LATENCY_WAITABLE && sc->waitable && n > sc->max_latency)
        vx_release_semaphore(sc->waitable, (LONG)(n - sc->max_latency));
    // (a lower limit takes effect as the program's waits use up the counts already given out)
    sc->max_latency = n;
    return S_OK;
}
static WINAPI HRESULT swap_get_latency(void *this, uint32_t *n) {
    if (!n) return DXGI_ERROR_INVALID_CALL;
    *n = ((swap12 *)this)->max_latency;
    return S_OK;
}
static WINAPI HANDLE swap_waitable(void *this) {
    swap12 *sc = this;
    if (!sc->waitable) {
        HANDLE (WINAPI *create)(void *, int, int, const uint16_t *) = (HANDLE (WINAPI *)(void *, int, int, const uint16_t *))vx_find_export("kernel32.dll", "CreateEventW");
        sc->waitable = create(NULL, 1, 1, NULL);                  // manual reset, signalled
    }
    return sc->waitable;
}
static WINAPI HRESULT swap_check_color_space(void *this, uint32_t cs, uint32_t *support) { *support = cs == 0 ? 1 : 0; return S_OK; }
static WINAPI HRESULT swap_set_color_space(void *this, uint32_t cs) { return cs == 0 ? S_OK : E_INVALIDARG; }
static WINAPI HRESULT swap_last_present(void *this, uint32_t *n) { *n = ((swap12 *)this)->presents; return S_OK; }
static WINAPI HRESULT swap_frame_stats(void *this, void *stats) { return DXGI_ERROR_FRAME_STATISTICS_DISJOINT; }
static WINAPI HRESULT swap_get_output(void *this, void **out) {
    if (!out) return E_INVALIDARG;
    *out = dxgi_output_create();
    return S_OK;
}
// IDXGISwapChain2 source size: the region of the back buffers presented (scaled to the window).
static WINAPI HRESULT swap_set_source_size(void *this, uint32_t w, uint32_t h) {
    swap12 *sc = this;
    if (!w || !h || w > sc->width || h > sc->height) return E_INVALIDARG;
    sc->src_width = w; sc->src_height = h;
    return S_OK;
}
static WINAPI HRESULT swap_get_source_size(void *this, uint32_t *w, uint32_t *h) {
    swap12 *sc = this;
    if (!w || !h) return E_INVALIDARG;
    *w = sc->src_width; *h = sc->src_height;
    return S_OK;
}
static WINAPI HRESULT swap_resize_target(void *this, const void *mode) { return S_OK; }
static WINAPI int32_t swap_mono(void *this) { return 0; }
static WINAPI HRESULT swap_set_hdr_metadata(void *this, uint32_t type, uint32_t size, const void *data) {
    return dxgi_check_hdr_metadata(type, size, data);
}

// ---- the device ---------------------------------------------------------------------------------------------------------

static WINAPI HRESULT dev_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4, &IID_ID3D12Device5,
                        &IID_ID3D12Device6, &IID_ID3D12Device7, &IID_ID3D12Device8, &IID_ID3D12Device9, &IID_ID3D12Device10, &IID_ID3D12Object };
    return query(this, iid, out, k, 12);
}
static WINAPI uint32_t dev_node_count(void *this) { return 1; }
// GPU clocks are Metal's to choose; stable-power mode only matters to profiling tools.
static WINAPI HRESULT dev_set_stable_power_state(void *this, int32_t enable) { return S_OK; }
static WINAPI HRESULT dev_set_background_processing(void *this, int32_t mode, int32_t measurements, HANDLE event, int32_t *further) {
    if (further) *further = 0;
    return S_OK;
}
// Serialized driver data (pipeline caches, acceleration structures) is never recognized.
static WINAPI int32_t dev_check_driver_identifier(void *this, int32_t type, const void *identifier) { return 3; }
static WINAPI HRESULT dev_removed_reason(void *this) { return S_OK; }
static uint32_t *residency_counter12(dev12 *device, void *object) {
    if (!device || !object) return NULL;
    d3d_object *base = object;
    if (((obj12 *)base)->dev != device) return NULL;
    switch (base->kind) {
    case K12_HEAP: return &((heap12 *)base)->residency_refs;
    case K12_MEMHEAP: return &((memheap12 *)base)->residency_refs;
    case K12_QUERYHEAP: return &((queryheap12 *)base)->residency_refs;
    case K12_RESOURCE: {
        res12 *resource = (res12 *)base;
        // Placed resources borrow physical memory from their heap. Residency is managed on that owner.
        return resource->owner_heap ? NULL : &resource->residency_refs;
    }
    default: return NULL;
    }
}

static uint32_t *residency_priority12(dev12 *device, void *object) {
    if (!device || !object) return NULL;
    d3d_object *base = object;
    if (((obj12 *)base)->dev != device) return NULL;
    switch (base->kind) {
    case K12_HEAP: return &((heap12 *)base)->residency_priority;
    case K12_MEMHEAP: return &((memheap12 *)base)->residency_priority;
    case K12_QUERYHEAP: return &((queryheap12 *)base)->residency_priority;
    case K12_RESOURCE: {
        res12 *resource = (res12 *)base;
        return resource->owner_heap ? NULL : &resource->residency_priority;
    }
    default: return NULL;
    }
}

static WINAPI HRESULT dev_update_residency(void *this, uint32_t count, void *const *objects, int make_resident) {
    dev12 *device = this;
    if (count && !objects) return E_INVALIDARG;
    pthread_mutex_lock(&device->lock);
    HRESULT result = S_OK;
    // Validate the whole batch first so a bad object cannot leave a partially changed residency set.
    for (uint32_t i = 0; i < count && result >= 0; i++) {
        uint32_t *refs = residency_counter12(device, objects[i]);
        if (!refs) { result = E_INVALIDARG; break; }
        int first = 1;
        uint64_t occurrences = 1;
        for (uint32_t j = 0; j < i; j++) if (objects[j] == objects[i]) { first = 0; break; }
        if (!first) continue;
        for (uint32_t j = i + 1; j < count; j++) if (objects[j] == objects[i]) occurrences++;
        if (make_resident) {
            uint32_t current = __atomic_load_n(refs, __ATOMIC_RELAXED);
            if (occurrences > UINT32_MAX - current) result = E_OUTOFMEMORY;
        } else if (occurrences > __atomic_load_n(refs, __ATOMIC_RELAXED)) result = E_INVALIDARG;
    }
    if (result >= 0) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t *refs = residency_counter12(device, objects[i]);
            if (make_resident) __atomic_add_fetch(refs, 1, __ATOMIC_RELAXED);
            else __atomic_sub_fetch(refs, 1, __ATOMIC_RELAXED);
        }
    }
    pthread_mutex_unlock(&device->lock);
    return result;
}
static WINAPI HRESULT dev_make_resident(void *this, uint32_t n, void *const *objs) {
    return dev_update_residency(this, n, objs, 1);
}
static WINAPI HRESULT dev_evict(void *this, uint32_t n, void *const *objs) {
    return dev_update_residency(this, n, objs, 0);
}
static WINAPI HRESULT dev_set_residency_priority(void *this, uint32_t count, void *const *objects,
                                                 const uint32_t *priorities) {
    dev12 *device = this;
    if (count && (!objects || !priorities)) return E_INVALIDARG;
    pthread_mutex_lock(&device->lock);
    HRESULT result = S_OK;
    for (uint32_t i = 0; i < count; i++)
        if (!residency_priority12(device, objects[i])) { result = E_INVALIDARG; break; }
    if (result >= 0)
        for (uint32_t i = 0; i < count; i++)
            __atomic_store_n(residency_priority12(device, objects[i]), priorities[i], __ATOMIC_RELAXED);
    pthread_mutex_unlock(&device->lock);
    return result;
}
static WINAPI HRESULT dev_enqueue_make_resident(void *this, uint32_t flags, uint32_t count,
                                               void *const *objects, void *fence_object,
                                               uint64_t fence_value) {
    dev12 *device = this;
    fence12 *fence = fence_object;
    if ((flags & ~1u) || !fence || fence->dev != device) return E_INVALIDARG;
    HRESULT result = dev_update_residency(device, count, objects, 1);
    if (result < 0) return result;
    // The Metal backend has no explicit residency queue; residency bookkeeping completes synchronously here.
    return fence_signal(fence, fence_value);
}
static WINAPI uint64_t *dev_adapter_luid(void *this, uint64_t *out) { *out = 0x5658525400000001ull; return out; }

enum {
    VX12_FS1_BUFFER = 0x00000001, VX12_FS1_IA_VERTEX_BUFFER = 0x00000002,
    VX12_FS1_IA_INDEX_BUFFER = 0x00000004, VX12_FS1_TEXTURE1D = 0x00000010,
    VX12_FS1_TEXTURE2D = 0x00000020, VX12_FS1_TEXTURE3D = 0x00000040,
    VX12_FS1_TEXTURECUBE = 0x00000080, VX12_FS1_SHADER_LOAD = 0x00000100,
    VX12_FS1_SHADER_SAMPLE = 0x00000200, VX12_FS1_SHADER_SAMPLE_COMPARISON = 0x00000400,
    VX12_FS1_MIP = 0x00001000, VX12_FS1_RENDER_TARGET = 0x00004000,
    VX12_FS1_BLENDABLE = 0x00008000, VX12_FS1_DEPTH_STENCIL = 0x00010000,
    VX12_FS1_MULTISAMPLE_RESOLVE = 0x00040000, VX12_FS1_DISPLAY = 0x00080000,
    VX12_FS1_MULTISAMPLE_RENDERTARGET = 0x00200000, VX12_FS1_MULTISAMPLE_LOAD = 0x00400000,
    VX12_FS1_SHADER_GATHER = 0x00800000, VX12_FS1_TYPED_UNORDERED_ACCESS_VIEW = 0x02000000,
    VX12_FS1_SHADER_GATHER_COMPARISON = 0x04000000,
    VX12_FS2_UAV_ATOMIC_ADD = 0x00000001, VX12_FS2_UAV_ATOMIC_BITWISE_OPS = 0x00000002,
    VX12_FS2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE = 0x00000004, VX12_FS2_UAV_ATOMIC_EXCHANGE = 0x00000008,
    VX12_FS2_UAV_ATOMIC_SIGNED_MIN_OR_MAX = 0x00000010, VX12_FS2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX = 0x00000020,
    VX12_FS2_UAV_TYPED_LOAD = 0x00000040, VX12_FS2_UAV_TYPED_STORE = 0x00000080
};

static int d3d12_dxgi_format_valid(uint32_t format) {
    return (format >= 1 && format <= 26) || (format >= 27 && format <= 115) ||
           (format >= 130 && format <= 132) || (format >= 189 && format <= 191);
}

static int d3d12_typed_uav_load_tier_format(uint32_t format, MTLReadWriteTextureTier tier) {
    if (tier >= MTLReadWriteTextureTier1 && (format == 41 || format == 42 || format == 43)) return 1;
    if (tier < MTLReadWriteTextureTier2) return 0;
    switch (format) {
    case 2: case 3: case 4:                                      // R32G32B32A32_FLOAT/UINT/SINT
    case 10: case 11: case 12: case 13: case 14:                 // R16G16B16A16_* typed loads
    case 16: case 17: case 18:                                   // R32G32_FLOAT/UINT/SINT
    case 24: case 25: case 26:                                   // R10G10B10A2_UNORM/UINT, R11G11B10_FLOAT
    case 28: case 30: case 32:                                   // R8G8B8A8_UNORM/UINT/SINT
    case 31:                                                      // R8G8B8A8_SNORM
    case 34: case 35: case 36: case 37: case 38:                 // R16G16_* typed loads
    case 49: case 50: case 51: case 52:                           // R8G8_* typed loads
    case 54: case 57: case 59:                                   // R16_FLOAT/UINT/SINT
    case 56: case 58: case 61: case 62: case 63: case 64: case 65: // R16/R8/A8 single-channel typed loads
    case 85: case 86: case 115:                                  // B5G6R5, B5G5R5A1, B4G4R4A4
        return 1;
    default:
        return 0;
    }
}

static id<MTLTexture> d3d12_probe_texture(dev12 *d, uint32_t format, MTLTextureType type,
                                          MTLTextureUsage usage, uint32_t samples, uint32_t mip_levels) {
    MTLPixelFormat pixel_format = d3d_mtl_format(format, NULL);
    if (pixel_format == MTLPixelFormatInvalid || (samples > 1 && ![d->mtl supportsTextureSampleCount:samples])) return nil;
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.pixelFormat = pixel_format;
    td.textureType = type;
    td.width = 4;
    td.height = (type == MTLTextureType1D || type == MTLTextureType1DArray) ? 1 : 4;
    td.depth = type == MTLTextureType3D ? 4 : 1;
    td.arrayLength = 1;
    td.mipmapLevelCount = samples > 1 ? 1 : mip_levels;
    td.sampleCount = samples;
    td.usage = usage;
    td.storageMode = MTLStorageModePrivate;
    @try {
        return [d->mtl newTextureWithDescriptor:td];
    } @catch (NSException *exception) {
        (void)exception;
        return nil;
    }
}

static int d3d12_is_depth_pixel_format(MTLPixelFormat format) {
    return format == MTLPixelFormatDepth16Unorm || format == MTLPixelFormatDepth32Float ||
           format == MTLPixelFormatDepth32Float_Stencil8 || format == MTLPixelFormatX32_Stencil8;
}

static int d3d12_is_block_compressed_format(uint32_t format) {
    return (format >= 70 && format <= 84) || (format >= 94 && format <= 99);
}

// Texture buffers accept a smaller pixel-format set than ordinary textures. Probe each requested usage
// separately after this conservative format filter; depth, sRGB, and block-compressed formats are excluded.
static int d3d12_typed_buffer_format(uint32_t format) {
    switch (format) {
    case 2: case 3: case 4: case 10: case 11: case 12: case 13: case 14:
    case 16: case 17: case 18: case 24: case 25: case 26:
    case 28: case 30: case 31: case 32: case 34: case 35: case 36: case 37: case 38:
    case 41: case 42: case 43: case 48: case 49: case 50: case 51: case 52:
    case 54: case 56: case 57: case 58: case 59: case 61: case 62: case 63: case 64: case 65:
    case 87: case 88:
        return 1;
    default:
        return 0;
    }
}

static int d3d12_texture_atomic_format_supported(id<MTLDevice> mtl, uint32_t format) {
    if (!mtl || (format != 42 && format != 43)) return 0;          // R32_UINT / R32_SINT
    if (@available(macOS 14.0, *)) return [mtl supportsFamily:MTLGPUFamilyApple6];
    return 0;
}

// Metal permits blend-enabled pipelines only for pixel formats with the Blend capability. Keep this
// candidate list to those color formats; a Metal validation assertion on an unsupported format is fatal.
static int d3d12_blend_probe_format(uint32_t format) {
    switch (format) {
    case 2: case 10: case 11: case 13: case 16: case 24: case 26: case 28: case 29: case 31:
    case 34: case 35: case 37: case 41: case 48: case 49: case 51: case 54: case 56: case 58:
    case 61: case 63: case 85: case 86: case 87: case 88: case 90: case 92: case 115:
        return 1;
    default:
        return 0;
    }
}

static int d3d12_metal_resolve_format(uint32_t format) {
    switch (format) {
    case 10: case 11: case 13: case 24: case 26: case 28: case 29: case 31:
    case 34: case 35: case 37: case 48: case 49: case 51: case 54: case 56: case 58:
    case 61: case 63: case 67: case 85: case 86: case 87: case 88: case 90: case 91:
    case 92: case 93: case 115:
        return 1;
    default:
        return 0;
    }
}

static int d3d12_probe_typed_buffer(dev12 *d, uint32_t format, MTLTextureUsage usage) {
    if (!d3d12_typed_buffer_format(format)) return 0;
    uint32_t bytes = 0;
    MTLPixelFormat pixel_format = d3d_mtl_format(format, &bytes);
    if (pixel_format == MTLPixelFormatInvalid || !bytes || d3d12_is_depth_pixel_format(pixel_format)) return 0;
    const NSUInteger width = 8;
    NSUInteger row_bytes = width * bytes;
    NSUInteger alignment = [d->mtl minimumTextureBufferAlignmentForPixelFormat:pixel_format];
    if (alignment) {
        NSUInteger remainder = row_bytes % alignment;
        if (remainder) row_bytes += alignment - remainder;
    }
    id<MTLBuffer> buffer = [d->mtl newBufferWithLength:row_bytes options:MTLResourceStorageModeShared];
    if (!buffer) return 0;
    MTLTextureDescriptor *td = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pixel_format
                                                                                       width:width
                                                                              resourceOptions:buffer.resourceOptions
                                                                                       usage:usage];
    @try {
        return [buffer newTextureWithDescriptor:td offset:0 bytesPerRow:row_bytes] != nil;
    } @catch (NSException *exception) {
        (void)exception;
        return 0;
    }
}

static int d3d12_probe_typed_uav_store(dev12 *d, uint32_t format) {
    if (d3d12_is_depth_pixel_format(d3d_mtl_format(format, NULL))) return 0;
    MTLTextureUsage usage = MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    if (d3d12_probe_texture(d, format, MTLTextureType2D, usage, 1, 1)) return 1;
    return d3d12_probe_typed_buffer(d, format, MTLTextureUsageShaderWrite);
}

static int d3d12_probe_typed_uav_load(dev12 *d, uint32_t format) {
    if (!d3d12_typed_uav_load_tier_format(format, d->mtl.readWriteTextureSupport)) return 0;
    // D3D12 reports the 15-format additional group as all-or-none. Optional formats still receive
    // individual checks after the required Metal resource and shader access modes are available.
    static const uint32_t additional_set[] = {
        2, 3, 4, 10, 12, 14, 28, 30, 32, 54, 57, 59, 61, 62, 64
    };
    for (size_t i = 0; i < sizeof additional_set / sizeof additional_set[0]; i++)
        if (format == additional_set[i] && !d->typed_uav_load_additional_formats) return 0;
    MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    if (d3d12_probe_texture(d, format, MTLTextureType2D, usage, 1, 1)) return 1;
    return d3d12_probe_typed_buffer(d, format, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
}

static int d3d12_probe_typed_uav_load_additional_set(dev12 *d) {
    static const uint32_t formats[] = {
        2, 3, 4, 10, 12, 14, 28, 30, 32, 54, 57, 59, 61, 62, 64
    };
    if (d->mtl.readWriteTextureSupport < MTLReadWriteTextureTier2) return 0;
    MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    for (size_t i = 0; i < sizeof formats / sizeof formats[0]; i++) {
        uint32_t format = formats[i];
        if (!d3d12_probe_texture(d, format, MTLTextureType2D, usage, 1, 1) &&
            !d3d12_probe_typed_buffer(d, format, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)) return 0;
    }
    return 1;
}

static int d3d12_probe_typed_buffer_atomic(dev12 *d, uint32_t format) {
    if (!d3d12_texture_atomic_format_supported(d->mtl, format) ||
        !d3d12_probe_typed_buffer(d, format, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite)) return 0;
    uint32_t bytes = 0;
    MTLPixelFormat pixel_format = d3d_mtl_format(format, &bytes);
    if (pixel_format == MTLPixelFormatInvalid || !bytes) return 0;
    const NSUInteger width = 8;
    NSUInteger row_bytes = width * bytes;
    NSUInteger alignment = [d->mtl minimumTextureBufferAlignmentForPixelFormat:pixel_format];
    if (alignment) {
        NSUInteger remainder = row_bytes % alignment;
        if (remainder) row_bytes += alignment - remainder;
    }
    id<MTLBuffer> buffer = [d->mtl newBufferWithLength:row_bytes options:MTLResourceStorageModeShared];
    if (!buffer) return 0;
    MTLTextureDescriptor *td = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pixel_format
                                                                                       width:width
                                                                              resourceOptions:buffer.resourceOptions
                                                                                       usage:MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageShaderAtomic];
    @try {
        return [buffer newTextureWithDescriptor:td offset:0 bytesPerRow:row_bytes] != nil;
    } @catch (NSException *exception) {
        (void)exception;
        return 0;
    }
}

static int d3d12_probe_texture_atomics(dev12 *d, uint32_t format) {
    if (!d3d12_texture_atomic_format_supported(d->mtl, format)) return 0;
    MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageShaderAtomic;
    const MTLTextureType types[] = { MTLTextureType1D, MTLTextureType1DArray, MTLTextureType2D,
                                     MTLTextureType2DArray, MTLTextureType3D };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if (!d3d12_probe_texture(d, format, types[i], usage, 1, 1)) return 0;
    return d3d12_probe_typed_buffer_atomic(d, format);
}

static int d3d12_probe_blending(dev12 *d, MTLPixelFormat format) {
    if (!d->format_probe_library) {
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 vx_probe_vertex(uint id [[vertex_id]]) { return float4(0.0f, 0.0f, 0.0f, 1.0f); }\n"
            "fragment float4 vx_probe_fragment() { return float4(1.0f); }\n";
        NSError *error = nil;
        d->format_probe_library = [d->mtl newLibraryWithSource:source options:nil error:&error];
        if (!d->format_probe_library) return 0;
        d->format_probe_vertex = [d->format_probe_library newFunctionWithName:@"vx_probe_vertex"];
        d->format_probe_fragment = [d->format_probe_library newFunctionWithName:@"vx_probe_fragment"];
        if (!d->format_probe_vertex || !d->format_probe_fragment) return 0;
    }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = d->format_probe_vertex;
    pd.fragmentFunction = d->format_probe_fragment;
    MTLRenderPipelineColorAttachmentDescriptor *color = pd.colorAttachments[0];
    color.pixelFormat = format;
    color.blendingEnabled = YES;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationAlphaBlendFactor = MTLBlendFactorZero;
    @try {
        NSError *error = nil;
        return [d->mtl newRenderPipelineStateWithDescriptor:pd error:&error] != nil;
    } @catch (NSException *exception) {
        (void)exception;
        return 0;
    }
}

static void d3d12_query_format_support(dev12 *d, uint32_t format, uint32_t *support1, uint32_t *support2) {
    const MTLTextureUsage read_usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    uint32_t s1 = 0, s2 = 0;
    if (vertex_format(format) != MTLVertexFormatInvalid) s1 |= VX12_FS1_IA_VERTEX_BUFFER;
    if (format == 42 || format == 57) s1 |= VX12_FS1_IA_INDEX_BUFFER;
    if (d3d12_probe_typed_buffer(d, format, MTLTextureUsageShaderRead)) s1 |= VX12_FS1_BUFFER;

    id<MTLTexture> tex2d = d3d12_probe_texture(d, format, MTLTextureType2D, read_usage, 1, 1);
    if (tex2d) {
        s1 |= VX12_FS1_TEXTURE2D | VX12_FS1_SHADER_LOAD;
        // TextureGather lowers to MSL gather on sample-access color textures. The DXIL
        // lowering currently emits depth texture types only for TextureGatherCmp.
        if (!d3d12_is_depth_pixel_format(tex2d.pixelFormat)) s1 |= VX12_FS1_SHADER_GATHER;
        if ((format != 2 && format != 16 && format != 41) || d->mtl.supports32BitFloatFiltering)
            s1 |= VX12_FS1_SHADER_SAMPLE;
        if (d3d12_probe_texture(d, format, MTLTextureType2D, read_usage, 1, 2)) s1 |= VX12_FS1_MIP;
        if (!d3d12_is_depth_pixel_format(tex2d.pixelFormat)) {
            if (!d3d12_is_block_compressed_format(format)) {
                if (d3d12_probe_texture(d, format, MTLTextureType1D, read_usage, 1, 1)) s1 |= VX12_FS1_TEXTURE1D;
                if (d3d12_probe_texture(d, format, MTLTextureType3D, read_usage, 1, 1)) s1 |= VX12_FS1_TEXTURE3D;
            }
        }
        if (d3d12_probe_texture(d, format, MTLTextureTypeCube, read_usage, 1, 1)) s1 |= VX12_FS1_TEXTURECUBE;

        int depth_format = format == 20 || format == 40 || format == 45 || format == 55;
        MTLTextureUsage target_usage = read_usage | MTLTextureUsageRenderTarget;
        if (!d3d12_is_block_compressed_format(format) &&
            d3d12_probe_texture(d, format, MTLTextureType2D, target_usage, 1, 1)) {
            if (depth_format) {
                s1 |= VX12_FS1_DEPTH_STENCIL | VX12_FS1_SHADER_SAMPLE_COMPARISON |
                      VX12_FS1_SHADER_GATHER_COMPARISON;
                // D3D12's multisample-render-target bit also covers depth-stencil targets. Probe the
                // requested sample count and actual Metal depth texture format before advertising it.
                if ([d->mtl supportsTextureSampleCount:2] &&
                    d3d12_probe_texture(d, format, MTLTextureType2DMultisample, target_usage, 2, 1))
                    s1 |= VX12_FS1_MULTISAMPLE_RENDERTARGET;
                // The custom depth-average resolve path supports these two depth formats on the same
                // Apple GPU families accepted by ResolveSubresourceRegion validation.
                if ((s1 & VX12_FS1_MULTISAMPLE_RENDERTARGET) &&
                    d3d12_depth_average_resolve_format(format) && d3d12_depth_resolve_supported12(d))
                    s1 |= VX12_FS1_MULTISAMPLE_RESOLVE;
            } else {
                s1 |= VX12_FS1_RENDER_TARGET;
                if (d3d12_blend_probe_format(format) && d3d12_probe_blending(d, tex2d.pixelFormat))
                    s1 |= VX12_FS1_BLENDABLE;
                if (format == 28 || format == 87) s1 |= VX12_FS1_DISPLAY;
                if ([d->mtl supportsTextureSampleCount:2]) {
                    if (d3d12_probe_texture(d, format, MTLTextureType2DMultisample, target_usage, 2, 1)) {
                        s1 |= VX12_FS1_MULTISAMPLE_RENDERTARGET;
                        if (d3d12_metal_resolve_format(format)) s1 |= VX12_FS1_MULTISAMPLE_RESOLVE;
                        if (d3d12_probe_texture(d, format, MTLTextureType2DMultisample,
                                                target_usage | MTLTextureUsageShaderRead, 2, 1))
                            s1 |= VX12_FS1_MULTISAMPLE_LOAD;
                    }
                }
            }
        }
    }

    int depth_format = format == 20 || format == 40 || format == 45 || format == 55;
    if (!depth_format) {
        int typed_store = d3d12_probe_typed_uav_store(d, format);
        int typed_load = d3d12_probe_typed_uav_load(d, format);
        if (typed_store || typed_load) s1 |= VX12_FS1_TYPED_UNORDERED_ACCESS_VIEW;
        if (typed_store) s2 |= VX12_FS2_UAV_TYPED_STORE;
        if (typed_load) s2 |= VX12_FS2_UAV_TYPED_LOAD;
    }
    if ((format == 42 || format == 43) && d3d12_probe_texture_atomics(d, format)) {
        s2 |= VX12_FS2_UAV_ATOMIC_ADD | VX12_FS2_UAV_ATOMIC_BITWISE_OPS |
              VX12_FS2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE | VX12_FS2_UAV_ATOMIC_EXCHANGE;
        s2 |= format == 43 ? VX12_FS2_UAV_ATOMIC_SIGNED_MIN_OR_MAX : VX12_FS2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
    }
    *support1 = s1;
    *support2 = s2;
}

static int d3d12_is_apple_gpu(id<MTLDevice> mtl) {
    // Apple-family GPU 1 is the common baseline for Apple-designed GPUs. This distinguishes Apple Silicon's
    // tile-based renderer from Intel/AMD Mac GPUs, which can also be exposed through Metal.
    return mtl && [mtl supportsFamily:(MTLGPUFamily)1001];
}

static WINAPI HEAP_PROPS *dev_custom_heap_properties(void *this, HEAP_PROPS *out,
                                                    uint32_t node_mask, int32_t heap_type) {
    dev12 *device = this;
    memset(out, 0, sizeof *out);
    if ((node_mask && node_mask != 1) || heap_type < 1 || heap_type > 3) return out;

    int uma = device->mtl.hasUnifiedMemory;
    int cache_coherent_uma = uma && d3d12_is_apple_gpu(device->mtl);
    out->Type = 4;                                                 // D3D12_HEAP_TYPE_CUSTOM
    out->CreationNodeMask = out->VisibleNodeMask = node_mask ? node_mask : 1;
    out->MemoryPoolPreference = heap_type == 1 && !uma ? 1 : 0;   // DEFAULT uses L1 only on non-UMA adapters.
    if (heap_type == 1) out->CPUPageProperty = 1;                 // NOT_AVAILABLE
    else if (heap_type == 2 && !cache_coherent_uma) out->CPUPageProperty = 2; // WRITE_COMBINE upload
    else out->CPUPageProperty = 3;                               // WRITE_BACK readback/coherent UMA upload
    return out;
}

static int d3d12_int64_shader_ops_supported12(dev12 *device) {
    // Metal exposes 64-bit integer arithmetic starting with Apple GPU family 3.
    return device && device->mtl && [device->mtl supportsFamily:MTLGPUFamilyApple3];
}

static int d3d12_feature_level_known12(int32_t level) {
    return level == 0xb000 || level == 0xb100 || level == 0xc000 || level == 0xc100 || level == 0xc200;
}

// D3D12_FEATURE: only what tells a program what it may use.
// 12_0 needs resource binding tier 2, tiled resources tier 2 (with min/max filtering) and typed UAV loads of the
// additional formats; 12_1 adds conservative rasterization and rasterizer-ordered views, which Metal does not give.
// Output-merger logic ops (asked by 11_1) are not offered either, as since 11_1 was first reported.
static int32_t d3d12_max_feature_level12(dev12 *d) {
    return d->tiled_tier >= 2 && d->typed_uav_load_additional_formats ? 0xc000 : 0xb100;
}

static WINAPI HRESULT dev_check_feature(void *this, int32_t feature, void *data, uint32_t size) {
    dev12 *device = this;
    switch (feature) {
    case 0: {                                                       // D3D12_OPTIONS (15 32-bit fields)
        struct options {
            int32_t double_precision_float_shader_ops, output_merger_logic_op;
            uint32_t min_precision_support, tiled_resources_tier, resource_binding_tier;
            int32_t ps_stencil_ref, typed_uav_load_additional_formats, rovs_supported;
            uint32_t conservative_rasterization_tier, max_gpu_va_bits_per_resource;
            int32_t standard_swizzle_64kb, cross_node_sharing_tier, cross_adapter_row_major;
            int32_t vp_and_rt_array_index_without_gs_emulation;
            uint32_t resource_heap_tier;
        };
        if (!data || size != sizeof(struct options)) return E_INVALIDARG;
        struct options *o = data;
        memset(o, 0, sizeof *o);
        // DXIL double types are lowered to float; Metal does not implement D3D12 logic ops or the swizzle layout.
        o->resource_binding_tier = 3;                              // descriptor heaps and dynamically indexed ranges
        o->tiled_resources_tier = (uint32_t)device->tiled_tier;   // Metal placement-sparse resources
        o->ps_stencil_ref = 1;                                     // pixel output [[stencil]] and OM stencil reference
        o->typed_uav_load_additional_formats = device->typed_uav_load_additional_formats;
        o->max_gpu_va_bits_per_resource = 40;
        o->vp_and_rt_array_index_without_gs_emulation = 1;
        o->resource_heap_tier = 2;
        return S_OK;
    }
    case 1: {                                                      // ARCHITECTURE {NodeIndex, TileBasedRenderer, UMA, CacheCoherentUMA}
        if (!data || size != 16) return E_INVALIDARG;
        uint32_t *o = data;
        if (o[0] != 0) return E_INVALIDARG;
        int apple_gpu = d3d12_is_apple_gpu(device->mtl);
        int uma = device->mtl.hasUnifiedMemory;
        o[1] = apple_gpu;
        o[2] = uma;
        o[3] = apple_gpu && uma;
        return S_OK;
    }
    case 2: {                                                      // FEATURE_LEVELS {NumFeatureLevels, pFeatureLevelsRequested, MaxSupportedFeatureLevel}
        struct { uint32_t n; uint32_t pad; const int32_t *levels; int32_t max; } *f = data;
        if (!data || size != sizeof *f || !f->n || !f->levels) return E_INVALIDARG;
        int32_t best = 0, max = d3d12_max_feature_level12(device);
        for (uint32_t i = 0; i < f->n; i++)
            if (d3d12_feature_level_known12(f->levels[i]) && f->levels[i] <= max && f->levels[i] > best)
                best = f->levels[i];
        f->max = best;
        return S_OK;
    }
    case 3: {                                                      // FORMAT_SUPPORT
        struct format_support { uint32_t format, support1, support2; } *f = data;
        if (!data || size != sizeof *f || !d3d12_dxgi_format_valid(f->format)) return E_INVALIDARG;
        dev12 *d = this;
        uint32_t format = f->format;
        pthread_mutex_lock(&d->format_lock);
        if (!d->format_support_valid[format]) {
            d3d12_query_format_support(d, format, &d->format_support1[format], &d->format_support2[format]);
            d->format_support_valid[format] = 1;
        }
        f->support1 = d->format_support1[format];
        f->support2 = d->format_support2[format];
        pthread_mutex_unlock(&d->format_lock);
        return S_OK;
    }
    case 4: {                                                      // MULTISAMPLE_QUALITY_LEVELS
        struct multisample_quality_levels { uint32_t format, sample_count, flags, quality_levels; } *q = data;
        if (!data || size != sizeof *q || (q->flags & ~1u)) return E_INVALIDARG;
        uint32_t sample_count = q->sample_count;
        q->quality_levels = 0;
        if (!sample_count || sample_count > 16 || (sample_count & (sample_count - 1)) || (q->flags & 1)) return S_OK;
        dev12 *d = this;
        RES_DESC probe = { 3, 0, 1, 1, 1, 1, q->format, sample_count, 0, D3D12_LAYOUT_UNKNOWN12, 1 };
        if (texture_sample_supported12(d, &probe))
            q->quality_levels = 1;                               // Metal's standard sample positions map to quality index 0.
        return S_OK;
    }
    case 5: {                                                      // FORMAT_INFO
        struct format_info { uint32_t format; uint8_t plane_count, padding[3]; } *f = data;
        if (!data || size != sizeof *f || !d3d12_dxgi_format_valid(f->format)) return E_INVALIDARG;
        f->plane_count = (uint8_t)d3d12_resource_plane_count(f->format);
        return S_OK;
    }
    case 6: {                                                      // GPU_VIRTUAL_ADDRESS_SUPPORT
        struct gpu_va_support { uint32_t per_resource, per_process; } *v = data;
        if (!data || size != sizeof *v) return E_INVALIDARG;
        v->per_resource = 40;
        v->per_process = 40;
        return S_OK;
    }
    case 7: {                                                      // SHADER_MODEL {HighestShaderModel}
        if (!data || size != sizeof(int32_t)) return E_INVALIDARG;
        int32_t *sm = data;
        int32_t requested = *sm;
        if (requested != 0x51 && (requested < 0x60 || requested > 0x69)) return E_INVALIDARG;
        if (requested > 0x66) *sm = 0x66;                          // (6.6: SM 6.7+ features are not translated yet)
        return S_OK;
    }
    case 8: {                                                      // D3D12_OPTIONS1
        struct options1 {
            int32_t wave_ops;
            uint32_t wave_lane_count_min, wave_lane_count_max, total_lane_count;
            int32_t expanded_compute_resource_states, int64_shader_ops;
        };
        if (!data || size != sizeof(struct options1)) return E_INVALIDARG;
        struct options1 *o = data;
        memset(o, 0, sizeof *o);
        // Every Shader Model 6.0 wave and quad intrinsic is translated to Metal SIMD-group functions; Apple GPUs run
        // SIMD groups of 32 threads. (TotalLaneCount is informational: an estimate for these GPUs.) Metal tracks
        // resource transitions; only advertise shader int64 arithmetic on GPU families that support it.
        o->wave_ops = 1;
        o->wave_lane_count_min = o->wave_lane_count_max = 32;
        o->total_lane_count = 32 * 128;
        o->expanded_compute_resource_states = 1;
        o->int64_shader_ops = d3d12_int64_shader_ops_supported12(device);
        return S_OK;
    }
    case 52: {                                                     // D3D12_OPTIONS21
        // Work graphs, expanded comparison sampling and extended command info remain unsupported. ExecuteIndirect
        // tier 1.1 is implemented, including the zero-payload incrementing-constant argument.
        if (!data || size != 4 * sizeof(uint32_t)) return E_INVALIDARG;
        uint32_t *o = data;
        memset(o, 0, 4 * sizeof *o);
        o[1] = 11;                                                  // D3D12_EXECUTE_INDIRECT_TIER_1_1
        return S_OK;
    }
    // Later option blocks, answered as the Windows runtime answers on hardware without those features: engines read
    // them unconditionally at start-up and treat a failed query as "not supported" at best.
    case 23:                                                       // D3D12_OPTIONS4: MSAA 64 KB alignment, shared-resource tier, native 16-bit ops (yes)
    case 27:                                                       // D3D12_OPTIONS5: tiled tier 3, render pass tier 0 (BeginRenderPass emulated), no raytracing
    case 30:                                                       // D3D12_OPTIONS6: variable-rate shading
    case 32:                                                       // D3D12_OPTIONS7: mesh shaders, sampler feedback
    case 36:                                                       // D3D12_OPTIONS8: unaligned block textures
    case 37:                                                       // D3D12_OPTIONS9: mesh statistics, 64-bit atomics, wave MMA
    case 39:                                                       // D3D12_OPTIONS10: VRS combiners
    case 40:                                                       // D3D12_OPTIONS11: 64-bit atomics on heap resources
    case 41:                                                       // D3D12_OPTIONS12: enhanced barriers, relaxed format casting
    case 42:                                                       // D3D12_OPTIONS13: copy pitch, vertex alignment, inverted viewports
    case 43:                                                       // D3D12_OPTIONS14: advanced texture ops, writable MSAA, stencil masks
    case 44:                                                       // D3D12_OPTIONS15: triangle fans, strip cut
    case 45:                                                       // D3D12_OPTIONS16: dynamic depth bias, GPU upload heaps
    case 46:                                                       // D3D12_OPTIONS17: non-normalized samplers (only 1.2 root signatures), write tracking
    case 47: {                                                     // D3D12_OPTIONS18: render passes valid
        static const struct { int32_t feature; uint32_t size; } sizes[] = {
            { 23, 12 }, { 27, 12 }, { 30, 20 }, { 32, 8 }, { 36, 4 }, { 37, 24 }, { 39, 8 }, { 40, 4 }, { 41, 12 },
            { 42, 24 }, { 43, 12 }, { 44, 8 }, { 45, 8 }, { 46, 8 }, { 47, 4 } };
        uint32_t want = 0;
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) if (sizes[i].feature == feature) want = sizes[i].size;
        if (!data || size != want) return E_INVALIDARG;
        memset(data, 0, size);
        if (feature == 41) *(int32_t *)data = -1;                  // D3D12_TRI_STATE_UNKNOWN (no mesh shaders)
        // OPTIONS4.Native16BitShaderOpsSupported: half and 16-bit integers are Metal's half/short/ushort, 16-bit
        // buffer elements and constant-buffer rows are read and written as such (dxil_msl.c).
        if (feature == 23) ((int32_t *)data)[2] = 1;
        return S_OK;
    }
    case 10:                                                       // PROTECTED_RESOURCE_SESSION_SUPPORT {NodeIndex, Support}
    case 24:                                                       // SERIALIZATION {NodeIndex, HeapSerializationTier}
    case 33: {                                                     // PROTECTED_RESOURCE_SESSION_TYPE_COUNT {NodeIndex, Count}
        uint32_t *o = data;
        if (!data || size != 8 || o[0] != 0) return E_INVALIDARG;
        o[1] = 0;
        return S_OK;
    }
    case 19: case 22: {                                            // SHADER_CACHE {SupportFlags}, EXISTING_HEAPS {Supported}
        // SHADER_CACHE: SINGLE_PSO (GetCachedBlob) and LIBRARY (ID3D12PipelineLibrary); no existing heaps.
        if (!data || size != 4) return E_INVALIDARG;
        *(uint32_t *)data = feature == 19 ? 0x1u | 0x2u : 0;
        return S_OK;
    }
    case 25: case 28: {                                            // CROSS_NODE {SharingTier, AtomicShaderInstructions}, DISPLAYABLE
        if (!data || size != 8) return E_INVALIDARG;
        memset(data, 0, 8);
        return S_OK;
    }
    case 20: {                                                     // COMMAND_QUEUE_PRIORITY {CommandListType, Priority, Supported}
        uint32_t *o = data;
        if (!data || size != 12) return E_INVALIDARG;
        if (o[0] > 7 || o[0] == 1) return E_INVALIDARG;            // (no bundle queues)
        o[2] = o[1] == 0 || o[1] == 100;                           // NORMAL and HIGH; not GLOBAL_REALTIME
        return S_OK;
    }
    case 18: {                                                     // D3D12_OPTIONS2
        struct options2 { int32_t depth_bounds_test_supported; uint32_t programmable_sample_positions_tier; };
        if (!data || size != sizeof(struct options2)) return E_INVALIDARG;
        memset(data, 0, sizeof(struct options2));
        return S_OK;
    }
    case 21: {                                                     // D3D12_OPTIONS3
        struct options3 {
            int32_t copy_queue_timestamp_queries_supported;
            int32_t casting_fully_typed_format_supported;
            uint32_t write_buffer_immediate_support_flags;
            uint32_t view_instancing_tier;
            int32_t barycentrics_supported;
        };
        if (!data || size != sizeof(struct options3)) return E_INVALIDARG;
        memset(data, 0, sizeof(struct options3));
        struct options3 *options = data;
        options->copy_queue_timestamp_queries_supported = d3d12_copy_timestamp_counters_supported12(device);
        // WriteBufferImmediate is lowered to ordered Metal blit writes on direct, compute, and copy lists.
        // D3D12 command-list support bits are DIRECT=1, COMPUTE=4, and COPY=8; bundles remain unsupported.
        options->write_buffer_immediate_support_flags = 1u | 4u | 8u;
        return S_OK;
    }
    case 12: {                                                     // ROOT_SIGNATURE {HighestVersion}
        if (!data || size != sizeof(int32_t)) return E_INVALIDARG;
        int32_t *v = data;
        if (*v > 2) *v = 2;
        return S_OK;
    }
    case 16: {                                                     // ARCHITECTURE1 (adds IsolatedMMU)
        struct architecture1 { uint32_t node_index; int32_t tile_based, uma, cache_coherent_uma, isolated_mmu; } *a = data;
        if (!data || size != sizeof *a || a->node_index != 0) return E_INVALIDARG;
        int apple_gpu = d3d12_is_apple_gpu(device->mtl);
        int uma = device->mtl.hasUnifiedMemory;
        a->tile_based = apple_gpu;
        a->uma = uma;
        a->cache_coherent_uma = apple_gpu && uma;
        // The guest address-space bridge does not expose the CPU page-table guarantees this field promises.
        a->isolated_mmu = 0;
        return S_OK;
    }
    }
    return E_INVALIDARG;
}

static int d3d12_footprint_align(uint64_t value, uint32_t alignment, uint64_t *out) {
    uint64_t mask = alignment - 1;
    if (!alignment || (alignment & mask) || value > UINT64_MAX - mask) return 0;
    *out = (value + mask) & ~mask;
    return 1;
}

static uint64_t d3d12_footprint_mip(uint64_t extent, uint32_t mip) {
    if (mip >= 64) return 1;
    extent >>= mip;
    return extent ? extent : 1;
}

static uint32_t d3d12_footprint_full_mips(uint64_t extent) {
    uint32_t count = 0;
    do { count++; extent >>= 1; } while (extent);
    return count;
}

static uint64_t d3d12_footprint_ceil_div(uint64_t value, uint32_t divisor) {
    return value / divisor + (value % divisor != 0);
}

static int d3d12_footprint_plane(uint32_t format, uint32_t plane, uint32_t width, uint32_t height,
                                 uint32_t *plane_format, uint32_t *pitch_width,
                                 uint32_t *plane_width, uint32_t *plane_height, int *is_planar) {
    *plane_format = format;
    *pitch_width = *plane_width = width;
    *plane_height = height;
    *is_planar = 0;
    switch (format) {
    case 103: case 104: case 105: case 110: case 130: case 131: case 132:
        *is_planar = 1;
        break;
    case 19: case 20: case 21: case 22: case 44: case 45: case 46: case 47:
        if (plane > 1) return 0;
        *plane_format = plane ? 60 : 39;                   // stencil is one byte; depth is a 32-bit plane
        return 1;
    default:
        if (plane) return 0;
        return 1;
    }

    switch (format) {
    case 103: case 104: case 105:                      // 4:2:0: Y and interleaved UV
        if (plane > 1) return 0;
        if (!plane) {
            *plane_format = format == 103 ? 60 : 53;
        } else {
            *plane_format = format == 103 ? 48 : 33;
            *plane_width = *pitch_width = width / 2 + (width & 1);
            *plane_height = height / 2 + (height & 1);
        }
        return 1;
    case 110:                                           // NV11: 4:1:1, with row-major chroma padding
        if (plane > 1) return 0;
        if (!plane) *plane_format = 60;
        else {
            *plane_format = 48;
            *plane_width = width / 4 + ((width & 3) != 0);
            *pitch_width = width / 2 + (width & 1);
        }
        return 1;
    case 130:                                           // P208: 4:2:2
        if (plane > 1) return 0;
        if (!plane) *plane_format = 60;
        else { *plane_format = 48; *plane_width = *pitch_width = width / 2 + (width & 1); }
        return 1;
    case 131:                                           // V208: 4:4:0
        if (plane > 2) return 0;
        *plane_format = 60;
        if (plane) *plane_height = height / 2 + (height & 1);
        return 1;
    case 132:                                           // V408: 4:4:4
        if (plane > 2) return 0;
        *plane_format = 60;
        return 1;
    default:
        return 0;
    }
}

static void d3d12_footprints_invalid(uint32_t n, FOOTPRINT *layouts, uint32_t *rows,
                                    uint64_t *row_bytes, uint64_t *total) {
    if (layouts) memset(layouts, 0xff, (size_t)n * sizeof *layouts);
    if (rows) memset(rows, 0xff, (size_t)n * sizeof *rows);
    if (row_bytes) memset(row_bytes, 0xff, (size_t)n * sizeof *row_bytes);
    if (total) *total = UINT64_MAX;
}

// Footprints are adapter-independent: rows use 256-byte alignment and each subresource starts at 512 bytes.
static WINAPI void dev_footprints(void *this, const RES_DESC *desc, uint32_t first, uint32_t n, uint64_t base,
                                  FOOTPRINT *layouts, uint32_t *rows, uint64_t *row_bytes, uint64_t *total) {
    (void)this;
    if (total) *total = UINT64_MAX;
    if (!desc || (uint64_t)first + n > UINT32_MAX) {
        d3d12_footprints_invalid(n, layouts, rows, row_bytes, total);
        return;
    }

    uint32_t mips = desc->MipLevels;
    uint32_t array_size = 1;
    uint32_t planes = 1;
    uint64_t max_dimension = desc->Width;
    if (desc->Dimension == 1) {                            // buffer
        if (!desc->Width || desc->Width > UINT32_MAX || desc->Height != 1 || desc->DepthOrArraySize != 1 ||
            desc->MipLevels != 1 || desc->Format != 0 || desc->SampleCount != 1 || desc->SampleQuality != 0) goto invalid;
    } else if (desc->Dimension >= 2 && desc->Dimension <= 4) {
        if (!desc->Width || !desc->DepthOrArraySize || !desc->Format || desc->SampleCount != 1 || desc->SampleQuality != 0) goto invalid;
        if (desc->Dimension == 2 && desc->Height != 1) goto invalid;
        if (desc->Dimension == 3 && !desc->Height) goto invalid;
        if (desc->Dimension == 4 && (!desc->Height || desc->DepthOrArraySize == 0)) goto invalid;
        array_size = desc->Dimension == 4 ? 1 : desc->DepthOrArraySize;
        if (desc->Dimension >= 3 && desc->Height > max_dimension) max_dimension = desc->Height;
        if (desc->Dimension == 4 && desc->DepthOrArraySize > max_dimension) max_dimension = desc->DepthOrArraySize;
        if (!mips) mips = d3d12_footprint_full_mips(max_dimension);
        if (!mips || mips > d3d12_footprint_full_mips(max_dimension)) goto invalid;
        planes = d3d12_resource_plane_count(desc->Format);
        if (!planes) goto invalid;
    } else goto invalid;

    uint64_t subresource_count = (uint64_t)mips * array_size * planes;
    if ((uint64_t)first > subresource_count || n > subresource_count - first) goto invalid;

    uint64_t cursor = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t subresource = first + i;
        uint32_t mip = subresource % mips;
        uint32_t plane = (subresource / mips) / array_size;
        uint64_t width64, height64, depth64;
        uint32_t footprint_format = desc->Format;
        uint32_t min_pitch_width = 0, plane_width = 0, plane_height = 0;
        uint32_t block_width = 1, block_height = 1, bytes_per_block = 1;
        int planar = 0;

        if (desc->Dimension == 1) {
            width64 = desc->Width; height64 = depth64 = 1;
            min_pitch_width = plane_width = (uint32_t)width64;
            plane_height = 1;
            footprint_format = 0;
        } else {
            width64 = d3d12_footprint_mip(desc->Width, mip);
            height64 = desc->Dimension == 2 ? 1 : d3d12_footprint_mip(desc->Height, mip);
            depth64 = desc->Dimension == 4 ? d3d12_footprint_mip(desc->DepthOrArraySize, mip) : 1;
            if (width64 > UINT32_MAX || height64 > UINT32_MAX || depth64 > UINT32_MAX) goto invalid;
            if (!d3d12_footprint_plane(desc->Format, plane, (uint32_t)width64, (uint32_t)height64,
                                       &footprint_format, &min_pitch_width, &plane_width, &plane_height, &planar)) goto invalid;
            if (!d3d_copy_format_block(footprint_format, &block_width, &block_height, &bytes_per_block)) goto invalid;
            if (!planar) {
                uint64_t aligned_width, aligned_height;
                if (!d3d12_footprint_align(width64, block_width, &aligned_width) ||
                    !d3d12_footprint_align(height64, block_height, &aligned_height) ||
                    aligned_width > UINT32_MAX || aligned_height > UINT32_MAX) goto invalid;
                plane_width = min_pitch_width = (uint32_t)aligned_width;
                plane_height = (uint32_t)aligned_height;
            }
        }

        uint64_t minimum_row_bytes = d3d12_footprint_ceil_div(min_pitch_width, block_width) * bytes_per_block;
        uint64_t actual_row_bytes = d3d12_footprint_ceil_div(plane_width, block_width) * bytes_per_block;
        uint64_t pitch;
        uint32_t pitch_alignment = planar ? 512 : 256;
        if (!minimum_row_bytes || !actual_row_bytes || !plane_height || !depth64 ||
            !d3d12_footprint_align(minimum_row_bytes, pitch_alignment, &pitch) || pitch > UINT32_MAX) goto invalid;
        uint64_t num_rows = planar ? plane_height : d3d12_footprint_ceil_div(plane_height, block_height);
        uint64_t row_slices, padded_rows, size;
        if (!num_rows || num_rows > UINT64_MAX / depth64) goto invalid;
        row_slices = num_rows * depth64;
        if (row_slices - 1 > UINT64_MAX / pitch) goto invalid;
        padded_rows = (row_slices - 1) * pitch;
        if (minimum_row_bytes > UINT64_MAX - padded_rows) goto invalid;
        size = padded_rows + minimum_row_bytes;

        uint64_t aligned_cursor, placed_offset;
        if (!d3d12_footprint_align(cursor, 512, &aligned_cursor) || base > UINT64_MAX - aligned_cursor) goto invalid;
        placed_offset = base + aligned_cursor;
        if (layouts) layouts[i] = (FOOTPRINT){ placed_offset, footprint_format, plane_width, plane_height,
                                                (uint32_t)depth64, (uint32_t)pitch };
        if (rows) rows[i] = (uint32_t)num_rows;
        if (row_bytes) row_bytes[i] = actual_row_bytes;
        if (size > UINT64_MAX - aligned_cursor) goto invalid;
        cursor = aligned_cursor + size;
    }
    if (total) *total = cursor;
    return;

invalid:
    d3d12_footprints_invalid(n, layouts, rows, row_bytes, total);
}

// Device8/9 forms of earlier methods: D3D12_RESOURCE_DESC1 (its sampler-feedback mip region unused here) and a queue
// creator id (only for tools telling queues apart).
static WINAPI void dev_footprints1(void *this, const RES_DESC1 *desc, uint32_t first, uint32_t n, uint64_t base,
                                   FOOTPRINT *layouts, uint32_t *rows, uint64_t *row_bytes, uint64_t *total) {
    dev_footprints(this, desc ? &desc->resource : NULL, first, n, base, layouts, rows, row_bytes, total);
}
static WINAPI ALLOC_INFO *dev_allocation_info2(void *this, ALLOC_INFO *out, uint32_t mask, uint32_t n, const RES_DESC1 *descs, ALLOC_INFO *each) {
    RES_DESC *plain = calloc(n ? n : 1, sizeof *plain);
    if (!plain) { *out = (ALLOC_INFO){ UINT64_MAX, 0 }; return out; }
    for (uint32_t i = 0; i < n; i++) plain[i] = descs[i].resource;
    dev_allocation_info1(this, out, mask, n, plain, each);
    free(plain);
    return out;
}
static WINAPI HRESULT dev_create_queue1(void *this, const Q_DESC *desc, const GUID *creator, const GUID *iid, void **out) {
    return dev_create_queue(this, desc, iid, out);
}

static WINAPI HRESULT dev_create_committed(void *this, const HEAP_PROPS *hp, int32_t flags, const RES_DESC *desc, int32_t state, const void *clear, const GUID *iid, void **out) {
    return create_committed(this, hp, flags, desc, state, clear, iid, out);
}
static WINAPI HRESULT dev_create_committed1(void *this, const HEAP_PROPS *hp, int32_t flags, const RES_DESC *desc,
                                             int32_t state, const void *clear, void *protected_session,
                                             const GUID *iid, void **out) {
    if (protected_session) {
        if (out) *out = NULL;
        return DXGI_ERROR_UNSUPPORTED;
    }
    return create_committed(this, hp, flags, desc, state, clear, iid, out);
}
static WINAPI HRESULT dev_create_committed2(void *this, const HEAP_PROPS *hp, int32_t flags, const RES_DESC1 *desc,
                                             int32_t state, const void *clear, void *protected_session,
                                             const GUID *iid, void **out) {
    if (!desc) { if (out) *out = NULL; return E_INVALIDARG; }
    if (protected_session || desc->sampler_feedback_mip_region.Width || desc->sampler_feedback_mip_region.Height ||
        desc->sampler_feedback_mip_region.Depth) {
        if (out) *out = NULL;
        return DXGI_ERROR_UNSUPPORTED;
    }
    return create_committed(this, hp, flags, &desc->resource, state, clear, iid, out);
}

static void device_destroy(d3d_object *o) {
    dev12 *d = (dev12 *)o;
    gpu_ir_metal_device_release(d->common_metal);
    d->common_metal = NULL;
    d->cpu_transfer_queue = nil;
    pthread_mutex_destroy(&d->submit_lock);
    pthread_mutex_destroy(&d->format_lock);
    pthread_mutex_destroy(&d->scratch_lock);
    d->scratch_pool = nil;
    pthread_mutex_destroy(&d->lock);
    d->format_probe_library = nil;
    d->format_probe_vertex = nil;
    d->format_probe_fragment = nil;
    d->resources = nil;
    d->residency = nil;
    d->resident_allocations = nil;
    d->residency_pending = nil;
    d->pso_cache = nil;
    d->mtl = nil;
}

// ---- the debug interface -----------------------------------------------------------------------------------------------

static WINAPI HRESULT debug_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12Debug, &IID_ID3D12Debug1, &IID_ID3D12Debug2, &IID_ID3D12Debug3, &IID_ID3D12Debug4, &IID_ID3D12Debug5 };
    return query(this, iid, out, k, 6);
}
static WINAPI void debug_noop(void *this) { }

// ---- exports ---------------------------------------------------------------------------------------------------------------

static WINAPI HRESULT D3D12CreateDevice(void *adapter, int32_t min_level, const GUID *iid, void **out) {
    pthread_once(&g_tables_once, make_tables);
    if (min_level && !d3d12_feature_level_known12(min_level)) return E_INVALIDARG;
    if (min_level > 0xc000) return DXGI_ERROR_UNSUPPORTED;          // highest feature level offered is 12_0
    id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
    if (!mtl) return E_FAIL;
    dev12 *d = d3d_alloc(sizeof *d, K12_DEVICE, g_device_vtbl, NULL);
    d->base.destroy = device_destroy;
    d->mtl = mtl;
    pthread_mutex_init(&d->lock, NULL);
    pthread_rwlock_init(&d->bufs_lock, NULL);
    pthread_mutex_init(&d->submit_lock, NULL);
    pthread_mutex_init(&d->alloc_lock, NULL);
    pthread_mutex_init(&d->format_lock, NULL);
    pthread_mutex_init(&d->scratch_lock, NULL);
    d->scratch_pool = [NSMutableArray new];
    d->common_metal = gpu_ir_metal_device_create_for_metal_device((__bridge void *)mtl);
    d->cpu_transfer_queue = [mtl newCommandQueue];
    if (!d->common_metal || !d->cpu_transfer_queue) {
        d3d_release(&d->base);
        return E_OUTOFMEMORY;
    }
    d->typed_uav_load_additional_formats = d3d12_probe_typed_uav_load_additional_set(d);
    d->resources = [NSMutableArray new];
    if (@available(macOS 15.0, *)) {
        if (!getenv("VXRT_D3D12_NO_RESIDENCY_SET")) {
            MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
            rd.label = @"VXRuntime D3D12 resources";
            d->residency = [mtl newResidencySetWithDescriptor:rd error:nil];
            d->resident_allocations = [NSCountedSet new];
            g_residency_set_active12 = 1;
        }
    }
    d->pso_cache = [NSMutableDictionary new];
    if (d->residency) d->tiled_tier = d3d12_tiled_tier_probe12(mtl);
    if (min_level > d3d12_max_feature_level12(d)) { d3d_release(&d->base); return DXGI_ERROR_UNSUPPORTED; }
    if (!out) { d3d_release(&d->base); return S_FALSE; }          // (a check that a device could be made)
    HRESULT hr = dev_query(d, iid, out);
    d3d_release(&d->base);
    return hr;
}

static WINAPI HRESULT D3D12GetDebugInterface(const GUID *iid, void **out) {
    pthread_once(&g_tables_once, make_tables);
    d3d_object *o = d3d_alloc(sizeof(obj12), K12_DEBUG, g_debug_vtbl, NULL);
    HRESULT hr = debug_query(o, iid, out);
    d3d_release(o);
    return hr;
}

static WINAPI HRESULT D3D12SerializeRootSignature(const RS_DESC *desc, int32_t version, void **blob, void **errors) {
    if (errors) *errors = NULL;
    if (!blob) return E_INVALIDARG;
    *blob = NULL;
    if (!desc || version != 1) return E_INVALIDARG;
    *blob = serialize_rootsig(version, desc);
    return *blob ? S_OK : E_OUTOFMEMORY;
}
static WINAPI HRESULT D3D12SerializeVersionedRootSignature(const VERSIONED_RS_DESC *desc, void **blob, void **errors) {
    if (errors) *errors = NULL;
    if (!blob) return E_INVALIDARG;
    *blob = NULL;
    if (!desc || desc->version < 1 || desc->version > 3) return E_INVALIDARG;
    *blob = serialize_rootsig(desc->version, &desc->desc);
    return *blob ? S_OK : E_OUTOFMEMORY;
}
static WINAPI HRESULT D3D12EnableExperimentalFeatures(uint32_t n, const GUID *iids, void *configs, uint32_t *sizes) { return E_NOINTERFACE; }
static WINAPI HRESULT D3D12GetInterface(const GUID *clsid, const GUID *iid, void **out) { if (out) *out = NULL; return E_NOINTERFACE; }

static WINAPI HRESULT dev_create_compute_pso_timed(void *this, const COMPUTE_PSO_DESC *desc, const GUID *iid, void **out) {
    uint64_t t0 = fs_now12();
    HRESULT hr = dev_create_compute_pso(this, desc, iid, out);
    FS_ADD12(pso_n, 1); FS_ADD12(pso_ns, fs_now12() - t0);
    return hr;
}
static WINAPI HRESULT dev_create_graphics_pso_timed(void *this, const GFX_PSO_DESC *desc, const GUID *iid, void **out) {
    uint64_t t0 = fs_now12();
    HRESULT hr = dev_create_graphics_pso(this, desc, iid, out);
    FS_ADD12(pso_n, 1); FS_ADD12(pso_ns, fs_now12() - t0);
    return hr;
}
static WINAPI HRESULT dev_create_pso_stream_timed(void *this, const PSO_STREAM_DESC *desc, const GUID *iid, void **out) {
    uint64_t t0 = fs_now12();
    HRESULT hr = dev_create_pso_stream(this, desc, iid, out);
    FS_ADD12(pso_n, 1); FS_ADD12(pso_ns, fs_now12() - t0);
    return hr;
}
static void make_tables(void) {
    static const d3d_method device[] = {
        { "QueryInterface", (void *)dev_query }, COMMON_METHODS,
        { "GetNodeCount", (void *)dev_node_count }, { "CreateCommandQueue", (void *)dev_create_queue },
        { "CreateCommandAllocator", (void *)dev_create_allocator }, { "CreateComputePipelineState", (void *)dev_create_compute_pso_timed },
        { "CreateCommandList", (void *)dev_create_list }, { "CheckFeatureSupport", (void *)dev_check_feature },
        { "CreatePipelineState", (void *)dev_create_pso_stream_timed }, { "CreateCommandList1", (void *)dev_create_list1 },
        { "SetEventOnMultipleFenceCompletion", (void *)dev_set_event_multiple_fences },
        { "CreatePipelineLibrary", (void *)dev_create_pipeline_library }, { "SetStablePowerState", (void *)dev_set_stable_power_state },
        { "SetBackgroundProcessingMode", (void *)dev_set_background_processing },
        { "CheckDriverMatchingIdentifier", (void *)dev_check_driver_identifier },
        { "CreateDescriptorHeap", (void *)dev_create_heap }, { "GetDescriptorHandleIncrementSize", (void *)dev_increment },
        { "CreateRootSignature", (void *)dev_create_rootsig }, { "CreateConstantBufferView", (void *)dev_create_cbv },
        { "CreateShaderResourceView", (void *)dev_create_srv }, { "CreateUnorderedAccessView", (void *)dev_create_uav },
        { "CreateSampler", (void *)dev_create_sampler }, { "CopyDescriptors", (void *)dev_copy_descriptors },
        { "CopyDescriptorsSimple", (void *)dev_copy_simple }, { "CreateCommittedResource", (void *)dev_create_committed },
        { "CreateFence", (void *)dev_create_fence }, { "GetDeviceRemovedReason", (void *)dev_removed_reason },
        { "GetCopyableFootprints", (void *)dev_footprints }, { "MakeResident", (void *)dev_make_resident },
        { "Evict", (void *)dev_evict }, { "SetResidencyPriority", (void *)dev_set_residency_priority },
        { "EnqueueMakeResident", (void *)dev_enqueue_make_resident }, { "GetAdapterLuid", (void *)dev_adapter_luid },
        { "CreateGraphicsPipelineState", (void *)dev_create_graphics_pso_timed }, { "CreateRenderTargetView", (void *)dev_create_rtv },
        { "CreateDepthStencilView", (void *)dev_create_dsv }, { "CreateHeap", (void *)dev_create_memheap },
        { "CreateHeap1", (void *)dev_create_memheap1 }, { "CreatePlacedResource", (void *)dev_create_placed },
        { "CreatePlacedResource1", (void *)dev_create_placed1 }, { "CreateCommandSignature", (void *)dev_create_command_signature },
        { "GetResourceAllocationInfo", (void *)dev_allocation_info },
        { "GetCustomHeapProperties", (void *)dev_custom_heap_properties },
        { "GetResourceAllocationInfo1", (void *)dev_allocation_info1 }, { "CreateQueryHeap", (void *)dev_create_query_heap },
        { "CreateCommittedResource1", (void *)dev_create_committed1 }, { "CreateCommittedResource2", (void *)dev_create_committed2 },
        { "CreateReservedResource", (void *)dev_create_reserved }, { "CreateReservedResource1", (void *)dev_create_reserved1 },
        { "GetResourceTiling", (void *)dev_get_resource_tiling },
        { "GetCopyableFootprints1", (void *)dev_footprints1 }, { "GetResourceAllocationInfo2", (void *)dev_allocation_info2 },
        { "CreateCommandQueue1", (void *)dev_create_queue1 },
        { NULL, NULL } };
    g_device_vtbl = D3D_VTABLE(ID3D12Device10, device);
    static const d3d_method queue[] = {
        { "QueryInterface", (void *)queue_query }, COMMON_METHODS, { "ExecuteCommandLists", (void *)queue_execute },
        { "UpdateTileMappings", (void *)queue_update_tile_mappings }, { "CopyTileMappings", (void *)queue_copy_tile_mappings },
        { "Signal", (void *)queue_signal }, { "Wait", (void *)queue_wait }, { "GetTimestampFrequency", (void *)queue_timestamp_frequency },
        { "GetClockCalibration", (void *)queue_clock_calibration },
        { "GetDesc", (void *)queue_get_desc }, { "SetMarker", (void *)list_marker }, { "BeginEvent", (void *)list_marker },
        { "EndEvent", (void *)list_end_event }, { NULL, NULL } };
    g_queue_vtbl = D3D_VTABLE(ID3D12CommandQueue, queue);
    static const d3d_method allocator[] = { { "QueryInterface", (void *)allocator_query }, COMMON_METHODS, { "Reset", (void *)allocator_reset }, { NULL, NULL } };
    g_allocator_vtbl = D3D_VTABLE(ID3D12CommandAllocator, allocator);
    static const d3d_method list[] = {
        { "QueryInterface", (void *)list_query }, COMMON_METHODS, { "GetType", (void *)list_type }, { "Close", (void *)list_close },
        { "Reset", (void *)list_reset }, { "SetPipelineState", (void *)list_set_pso }, { "SetComputeRootSignature", (void *)list_set_root },
        { "SetDescriptorHeaps", (void *)list_set_heaps }, { "SetComputeRootDescriptorTable", (void *)list_set_table },
        { "SetComputeRoot32BitConstant", (void *)list_set_constant }, { "SetComputeRoot32BitConstants", (void *)list_set_constants },
        { "SetComputeRootConstantBufferView", (void *)list_set_root_va }, { "SetComputeRootShaderResourceView", (void *)list_set_root_va },
        { "SetComputeRootUnorderedAccessView", (void *)list_set_root_va }, { "Dispatch", (void *)list_dispatch },
        { "CopyBufferRegion", (void *)list_copy_buffer }, { "CopyResource", (void *)list_copy_resource },
        { "CopyTextureRegion", (void *)list_copy_texture }, { "ResolveSubresource", (void *)list_resolve_subresource },
        { "ResolveSubresourceRegion", (void *)list_resolve_subresource_region },
        { "ResourceBarrier", (void *)list_barrier }, { "ExecuteBundle", (void *)list_execute_bundle },
        { "SetGraphicsRootSignature", (void *)list_set_gfx_root }, { "SetGraphicsRootDescriptorTable", (void *)list_set_gfx_table },
        { "SetGraphicsRoot32BitConstant", (void *)list_set_gfx_constant }, { "SetGraphicsRoot32BitConstants", (void *)list_set_gfx_constants },
        { "SetGraphicsRootConstantBufferView", (void *)list_set_gfx_root_va }, { "SetGraphicsRootShaderResourceView", (void *)list_set_gfx_root_va },
        { "SetGraphicsRootUnorderedAccessView", (void *)list_set_gfx_root_va }, { "IASetPrimitiveTopology", (void *)list_topology },
        { "IASetVertexBuffers", (void *)list_vbs }, { "IASetIndexBuffer", (void *)list_ib }, { "OMSetRenderTargets", (void *)list_targets },
        { "RSSetViewports", (void *)list_viewports }, { "RSSetScissorRects", (void *)list_scissors },
        { "ClearRenderTargetView", (void *)list_clear_rtv }, { "ClearDepthStencilView", (void *)list_clear_dsv },
        { "ClearUnorderedAccessViewUint", (void *)list_clear_uav_uint },
        { "ClearUnorderedAccessViewFloat", (void *)list_clear_uav_float },
        { "DrawInstanced", (void *)list_draw }, { "DrawIndexedInstanced", (void *)list_draw_indexed },
        { "OMSetBlendFactor", (void *)list_blend_factor }, { "OMSetStencilRef", (void *)list_stencil_ref },
        { "BeginQuery", (void *)list_begin_query }, { "EndQuery", (void *)list_end_query }, { "ResolveQueryData", (void *)list_resolve_query },
        { "ExecuteIndirect", (void *)list_execute_indirect }, { "CopyTiles", (void *)list_copy_tiles },
        { "WriteBufferImmediate", (void *)list_write_buffer_immediate },
        { "SetMarker", (void *)list_marker }, { "BeginEvent", (void *)list_marker }, { "EndEvent", (void *)list_end_event },
        { "DiscardResource", (void *)list_discard_resource }, { "SetPredication", (void *)list_set_predication },
        { "OMSetDepthBounds", (void *)list_depth_bounds }, { "SetSamplePositions", (void *)list_sample_positions },
        { "SetViewInstanceMask", (void *)list_view_instance_mask },
        { "BeginRenderPass", (void *)list_begin_render_pass }, { "EndRenderPass", (void *)list_end_render_pass }, { NULL, NULL } };
    g_list_vtbl = D3D_VTABLE(ID3D12GraphicsCommandList7, list);
    static const d3d_method fence[] = {
        { "QueryInterface", (void *)fence_query }, COMMON_METHODS, { "GetCompletedValue", (void *)fence_completed },
        { "SetEventOnCompletion", (void *)fence_set_event }, { "Signal", (void *)fence_signal }, { "GetCreationFlags", (void *)fence_flags }, { NULL, NULL } };
    g_fence_vtbl = D3D_VTABLE(ID3D12Fence1, fence);
    static const d3d_method heap[] = {
        { "QueryInterface", (void *)heap_query }, COMMON_METHODS, { "GetDesc", (void *)heap_get_desc },
        { "GetCPUDescriptorHandleForHeapStart", (void *)heap_cpu_start }, { "GetGPUDescriptorHandleForHeapStart", (void *)heap_gpu_start }, { NULL, NULL } };
    g_heap_vtbl = D3D_VTABLE(ID3D12DescriptorHeap, heap);
    static const d3d_method rootsig[] = { { "QueryInterface", (void *)rootsig_query }, COMMON_METHODS, { NULL, NULL } };
    g_rootsig_vtbl = D3D_VTABLE(ID3D12RootSignature, rootsig);
    static const d3d_method pso[] = { { "QueryInterface", (void *)pso_query }, COMMON_METHODS,
                                      { "GetCachedBlob", (void *)pso_cached_blob }, { NULL, NULL } };
    g_pso_vtbl = D3D_VTABLE(ID3D12PipelineState, pso);
    static const d3d_method plib[] = {
        { "QueryInterface", (void *)plib_query }, COMMON_METHODS, { "StorePipeline", (void *)plib_store },
        { "LoadGraphicsPipeline", (void *)plib_load_graphics }, { "LoadComputePipeline", (void *)plib_load_compute },
        { "GetSerializedSize", (void *)plib_serialized_size }, { "Serialize", (void *)plib_serialize },
        { "LoadPipeline", (void *)plib_load_stream }, { NULL, NULL } };
    g_plib_vtbl = D3D_VTABLE(ID3D12PipelineLibrary1, plib);
    static const d3d_method resource[] = {
        { "QueryInterface", (void *)res_query }, COMMON_METHODS, { "Map", (void *)res_map }, { "Unmap", (void *)res_unmap },
        { "WriteToSubresource", (void *)res_write_to_subresource },
        { "ReadFromSubresource", (void *)res_read_from_subresource },
        { "GetDesc", (void *)res_get_desc }, { "GetDesc1", (void *)res_get_desc1 },
        { "GetGPUVirtualAddress", (void *)res_gpu_va }, { "GetHeapProperties", (void *)res_get_heap_props }, { NULL, NULL } };
    g_resource_vtbl = D3D_VTABLE(ID3D12Resource2, resource);
    static const d3d_method memheap[] = {
        { "QueryInterface", (void *)memheap_query }, COMMON_METHODS, { "GetDesc", (void *)memheap_get_desc },
        { "GetProtectedResourceSession", (void *)memheap_session }, { NULL, NULL } };
    g_memheap_vtbl = D3D_VTABLE(ID3D12Heap1, memheap);
    static const d3d_method queryheap[] = { { "QueryInterface", (void *)queryheap_query }, COMMON_METHODS, { NULL, NULL } };
    g_queryheap_vtbl = D3D_VTABLE(ID3D12QueryHeap, queryheap);
    static const d3d_method command_signature[] = { { "QueryInterface", (void *)command_signature_query }, COMMON_METHODS, { NULL, NULL } };
    g_command_signature_vtbl = D3D_VTABLE(ID3D12CommandSignature, command_signature);
    static const d3d_method blob[] = { { "QueryInterface", (void *)blob_query }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
                                       { "GetBufferPointer", (void *)blob_pointer }, { "GetBufferSize", (void *)blob_size }, { NULL, NULL } };
    g_blob_vtbl = D3D_VTABLE(ID3D10Blob, blob);
    static const d3d_method debug[] = { { "QueryInterface", (void *)debug_query }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
                                        { "EnableDebugLayer", (void *)debug_noop }, { "SetEnableGPUBasedValidation", (void *)debug_noop },
                                        { "SetEnableSynchronizedCommandQueueValidation", (void *)debug_noop }, { NULL, NULL } };
    g_debug_vtbl = D3D_VTABLE(ID3D12Debug5, debug);
    static const d3d_method swap[] = {
        { "QueryInterface", (void *)swap_query }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
        { "SetPrivateData", (void *)obj_set_private }, { "GetPrivateData", (void *)obj_get_private }, { "SetPrivateDataInterface", (void *)obj_set_private_iface },
        { "GetParent", (void *)swap_get_parent }, { "GetDevice", (void *)swap_get_device }, { "Present", (void *)swap_present },
        { "Present1", (void *)swap_present1 }, { "GetBuffer", (void *)swap_get_buffer }, { "SetFullscreenState", (void *)swap_set_fullscreen },
        { "GetFullscreenState", (void *)swap_get_fullscreen }, { "GetDesc", (void *)swap_get_desc }, { "GetDesc1", (void *)swap_get_desc1 },
        { "ResizeBuffers", (void *)swap_resize }, { "ResizeBuffers1", (void *)swap_resize3 }, { "GetHwnd", (void *)swap_get_hwnd },
        { "GetCurrentBackBufferIndex", (void *)swap_index }, { "SetMaximumFrameLatency", (void *)swap_set_latency },
        { "GetMaximumFrameLatency", (void *)swap_get_latency }, { "GetFrameLatencyWaitableObject", (void *)swap_waitable },
        { "CheckColorSpaceSupport", (void *)swap_check_color_space }, { "SetColorSpace1", (void *)swap_set_color_space },
        { "GetLastPresentCount", (void *)swap_last_present }, { "GetFrameStatistics", (void *)swap_frame_stats },
        { "GetContainingOutput", (void *)swap_get_output }, { "SetSourceSize", (void *)swap_set_source_size },
        { "GetSourceSize", (void *)swap_get_source_size }, { "ResizeTarget", (void *)swap_resize_target },
        { "IsTemporaryMonoSupported", (void *)swap_mono }, { "SetHDRMetaData", (void *)swap_set_hdr_metadata }, { NULL, NULL } };
    g_swapchain_vtbl = D3D_VTABLE(IDXGISwapChain4, swap);
}

static const vx_export d3d12[] = {
    VX_EXPORT(D3D12CreateDevice), VX_EXPORT(D3D12GetDebugInterface), VX_EXPORT(D3D12SerializeRootSignature),
    VX_EXPORT(D3D12SerializeVersionedRootSignature), VX_EXPORT(D3D12EnableExperimentalFeatures), VX_EXPORT(D3D12GetInterface),
};
void vx_d3d12_init(void) {
    pthread_once(&g_tables_once, make_tables);
    vx_register_module("d3d12.dll", d3d12, sizeof d3d12 / sizeof d3d12[0]);
}

#ifdef __aarch64__
#include "thunks/d3d12.m.inc"
#endif
