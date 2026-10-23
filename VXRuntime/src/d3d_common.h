// Direct3D 11 and DXGI on Metal: what d3d11.m and dxgi.m share.
//
// Every Direct3D object is a COM object: a pointer to a method table laid out like its interface (see
// d3d_tables.h), then the object's state. An object may expose a second interface through a second table
// pointer (the device is also an IDXGIDevice1). Methods are called with `this` pointing at the table pointer.
#ifndef VX_D3D_COMMON_H
#define VX_D3D_COMMON_H
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "ui.h"
#include "gpu_ir.h"
#include "d3d_format.h"

typedef int32_t HRESULT;
typedef struct { DWORD d1; uint16_t d2, d3; uint8_t d4[8]; } GUID;
HRESULT d3d_not_implemented(const char *what);    // reports a method once, returns E_NOTIMPL

#define S_OK 0
#define S_FALSE 1
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_NOINTERFACE ((HRESULT)0x80004002)
#define E_POINTER ((HRESULT)0x80004003)
#define E_FAIL ((HRESULT)0x80004005)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define DXGI_ERROR_INVALID_CALL ((HRESULT)0x887A0001)
#define DXGI_ERROR_NOT_FOUND ((HRESULT)0x887A0002)
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004)
#define DXGI_ERROR_FRAME_STATISTICS_DISJOINT ((HRESULT)0x887A000B)

enum { K_DEVICE, K_CONTEXT, K_TEXTURE, K_BUFFER, K_RTV, K_DSV, K_SRV, K_SWAPCHAIN, K_FACTORY, K_ADAPTER, K_OUTPUT,
       K_SHADER, K_LAYOUT, K_STATE, K_UAV };

typedef struct pdata { GUID guid; uint32_t size; void *data; struct pdata *next; } pdata;
typedef struct d3d_device d3d_device;
typedef struct d3d_object {
    void **vtbl;                     // the main interface
    void **vtbl2;                    // a second interface, or NULL
    volatile LONG refs;
    int kind;
    d3d_device *dev;                 // the device a resource or view belongs to
    pdata *pdata;                    // SetPrivateData
    void (*destroy)(struct d3d_object *);
    uint64_t gpu_seq;                // resources: the command buffer (device sequence number) that last copied to or from it
} d3d_object;

// Common GPU IR wrappers used by the D3D11 immediate-context path. Returned resources are retained.
gpu_ir_resource *d3d11_gpu_ir_resource(d3d_object *resource);
void *d3d11_gpu_ir_metal_resource(const gpu_ir_resource *resource);

// `this` of a method called through the second table.
#define FROM_VTBL2(p) ((d3d_object *)((char *)(p) - offsetof(d3d_object, vtbl2)))

// Builds a method table: the implemented methods by name, stubs for the rest.
typedef struct { const char *name; void *fn; } d3d_method;
void **d3d_vtable(const char *const *names, void *const *stubs, int count, const d3d_method *impl);
#define D3D_VTABLE(iface, impl) d3d_vtable(vt_##iface, vtstub_##iface, VT_COUNT_##iface, impl)

void    *d3d_alloc(size_t size, int kind, void **vtbl, d3d_device *dev);   // refs = 1
uint32_t d3d_addref(d3d_object *o);
uint32_t d3d_release(d3d_object *o);
HRESULT  d3d_get_private(d3d_object *o, const GUID *g, uint32_t *size, void *data);
HRESULT  d3d_set_private(d3d_object *o, const GUID *g, uint32_t size, const void *data);
int      d3d_same_guid(const GUID *a, const GUID *b);

// Formats: the Metal pixel format and bytes per pixel of a DXGI_FORMAT (MTLPixelFormatInvalid if unsupported).
MTLPixelFormat d3d_mtl_format(uint32_t dxgi, uint32_t *bytes_per_pixel);
int            d3d_is_srgb(uint32_t dxgi);
int            d3d_copy_format_block(uint32_t dxgi, uint32_t *block_width, uint32_t *block_height,
                                      uint32_t *bytes_per_block);

// D3D11_TEXTURE2D_DESC as eleven 32-bit fields.
enum { TD_WIDTH, TD_HEIGHT, TD_MIPS, TD_ARRAY, TD_FORMAT, TD_SAMPLES, TD_QUALITY, TD_USAGE, TD_BIND, TD_CPU, TD_MISC, TD_COUNT };

struct d3d_device {
    d3d_object base;                 // ID3D11Device, and IDXGIDevice1 through vtbl2
    void *mtl;                       // id<MTLDevice> (retained)
    void *queue;                     // id<MTLCommandQueue> (retained)
    d3d_object *context;             // the immediate context
    d3d_object *adapter;             // IDXGIAdapter1, made on demand
    uint32_t flags, feature_level, max_latency;
    volatile uint64_t submitted, completed;   // command buffers committed, and the last one the GPU finished
    int apple_family;                // Metal's Apple GPU family: 7 = M1, 8 = M2, 9 = M3/M4, 10 = M5 and later (0: other)
    void *slices;                    // chunks small renamed buffers are carved from (d3d11.m)
    volatile uint64_t gpu_end_seq[64], gpu_end_ns[64];   // when the GPU finished recent command buffers (by sequence
                                                         // number, host clock ns): timestamp queries (d3d11_draw.m)
};
uint64_t d3d_gpu_end_ns(d3d_device *dev, uint64_t seq);  // that time, or 0 if not known (d3d11.m)
int d3d_apple_family(void *mtl_device);   // d3d11.m
typedef struct {
    d3d_object base;                 // ID3D11Texture2D (or 1D, 3D)
    uint32_t desc[TD_COUNT];         // in the 2D layout, whatever the dimension
    uint32_t dim, depth;             // 1, 2 or 3 dimensions; the depth of a 3D texture (else 1)
    void *mtl;                       // id<MTLTexture> (retained)
    uint8_t *cpu;                    // staging textures: the CPU copy Map hands out
    size_t cpu_pitch, cpu_size;
    int shared;                      // shared between processes: its slot in the shared-resource table + 1
    void **keyed_vtbl;               // IDXGIKeyedMutex (shared textures)
} d3d_texture;

// D3D11_BUFFER_DESC as six fields: byte width, usage, bind flags, CPU access, misc flags, structure stride.
// shared: a buffer in shared memory, between processes (its slot + 1): updated in place, never renamed.
// mtl: the Metal buffer holding the contents (retained), from byte off on: a small buffer renamed by Map(DISCARD)
// or UpdateSubresource is a slice of a larger chunk (then chunk is set; its size is desc[0], not mtl's length).
// so_filled: bytes stream output has written (where an append, offset -1, continues).
typedef struct { d3d_object base; uint32_t desc[6]; void *mtl; int shared; void *shm; size_t shm_size; size_t off; void *chunk; uint32_t so_filled; } d3d_buffer;
// A view: of a texture (mip, slices), or of a buffer (first element and count; flags: raw).
// A view: of a texture (dimension, first mip and mip count, slices), or of a buffer (first element and count;
// flags: raw, append, counter). counter: a UAV's hidden counter (id<MTLBuffer> of 4 bytes, retained); mtl_view:
// the Metal texture view shaders get (retained), made from mtl_src (the resource's Metal object at the time).
typedef struct {
    d3d_object base;
    d3d_object *resource;
    uint32_t format, dim, mip, mips, first_slice, slices, first, count, flags;
    void *counter, *mtl_view, *mtl_src;
} d3d_view;
void d3d_view_release_mtl(d3d_view *v);
void *d3d_rtv_texture(d3d_view *v);           // d3d11.m: what a render target view draws to (id<MTLTexture>)
void d3d_view_get_desc(const d3d_view *v, int which, uint32_t *out);   // 0 SRV, 1 RTV, 2 DSV, 3 UAV
struct d3d_draw_state;
// A clear not done yet (see d3d11.m): the view (held), and what to clear it to; or a discard (DiscardView, or
// DiscardResource: res held, v NULL): the next render pass need not load the contents.
typedef struct { d3d_view *v; d3d_object *res; float color[4]; float depth; uint8_t stencil; uint8_t is_color, clear_depth, clear_stencil, discard; } d3d_pending_clear;
typedef struct {
    d3d_object base;                 // ID3D11DeviceContext
    void *cmd;                       // the command buffer being recorded (retained), or NULL
    void *render;                    // the open render command encoder (retained), or NULL
    d3d_view *render_rtv[8], *render_dsv;   // what that encoder renders to
    d3d_view *rtv[8], *dsv;
    float viewport[16 * 6];          // RSSetViewports: up to 16 (x, y, width, height, min depth, max depth)
    uint32_t nviewports;
    struct d3d_draw_state *draw;     // shaders, buffers, states (d3d11_draw.m)
    uint32_t unsent;                 // draws and dispatches recorded since the command buffer last went to the GPU
    int prof_sample;                 // VXRT_GPU_PROFILE: the open render pass's first sample, until its shader is noted
    d3d_pending_clear clears[9];     // clears waiting for the next render pass (its load action) or other GPU work
    int nclears;
    // Geometry-shader kernels whose inputs the CPU wrote run in a command buffer of their own (pre, with one open
    // compute encoder pre_enc), committed before cmd: the render pass in progress need not end for them.
    void *pre, *pre_enc;
} d3d_context;
void d3d_clears_flush(d3d_context *c);                                   // does the pending clears as passes of their own
int d3d_same_target(const d3d_view *a, const d3d_view *b);              // d3d11.m: the same subresource, as the same format
int d3d_pass_dump_begin(void);                                            // VXRT_PASS_DUMP: whether to list this pass
void d3d_pass_dump_note(const char *targets, uint32_t unsent);           // ... its targets (when d3d_pass_dump_begin said so)
void d3d_end_reasons_log(void);
void d3d_flush_reasons_log(void);                                        // VXRT_STATS: what sent command buffers                                          // VXRT_STATS: what ended render passes with multisampled targets
void d3d_gpu_profile_submit(d3d_device *dev, void *cmd);                 // d3d11_draw.m: VXRT_GPU_PROFILE, the passes of a command buffer
// The pending clears the render pass being made can do as its load actions (d3d11.m): marks those attachments
// MTLLoadActionClear with the values and forgets the clears.
void d3d_clears_into_pass(d3d_context *c, void *render_pass_descriptor, d3d_view *const *rtvs, d3d_view *dsv);

void d3d_vtable_override(void **vt, const char *const *names, int count, const d3d_method *impl);
#define D3D_VTABLE_OVERRIDE(vt, iface, impl) d3d_vtable_override(vt, vt_##iface, VT_COUNT_##iface, impl)
void d3d_end_render(d3d_context *c);                                     // closes the open render encoder
void *d3d_context_cmd(d3d_context *c);                                   // id<MTLCommandBuffer>, created on demand
extern const d3d_method d3d_draw_device_methods[], d3d_draw_context_methods[];   // d3d11_draw.m
void d3d_draw_init(void);
void d3d_draw_release(d3d_context *c);
int d3d11_record_fill_buffer_pattern(d3d_context *context, d3d_buffer *buffer, uint64_t offset,
                                    uint64_t size, const void *pattern, uint32_t pattern_size);

d3d_texture *d3d_texture_create(d3d_device *dev, const uint32_t desc[TD_COUNT], HRESULT *hr);
// Resources shared between processes (d3d11_shared.m): IOSurface textures and shared-memory buffers in a table
// every vxrt process sees; a shared handle is 0x40000000 + 4 * slot.
#define D3D_SHARED_HANDLE_BASE 0x40000000u
void   *d3d_shared_texture(d3d_device *dev, MTLTextureDescriptor *td, const uint32_t desc[TD_COUNT], uint32_t bytes_per_pixel, int *slot);
void   *d3d_shared_buffer(d3d_device *dev, const uint32_t desc[6], void **shm, size_t *shm_size, int *slot);
void    d3d_shared_release(int slot);
HRESULT d3d_open_shared(d3d_device *dev, HANDLE h, const GUID *iid, void **out);
void  **d3d_dxgi_resource_vtbl(void);        // IDXGIResource1 on resources' second table
void  **d3d_keyed_mutex_vtbl(void);
void d3d_shared_note(d3d_texture *t);   // d3d11_shared.m (debugging)
void d3d_shared_written(d3d_device *dev, d3d_texture *t);   // d3d11_shared.m (debugging)
void d3d_flash_frame(d3d_device *dev, d3d_texture *src, d3d_texture *dst, const uint32_t *box);   // d3d11_draw.m (VXRT_FLASH_CAPTURE)
// VXRT_TRACE_KEYED: what a thread does while it holds a keyed mutex (draws, dispatches, copies, region copies,
// resolves, maps, flushes), logged at the release.
extern __thread int vx_keyed_held;
extern __thread uint32_t vx_held_ops[7];
#define HELD_OP(i) do { if (vx_keyed_held) vx_held_ops[i]++; } while (0)
void d3d_texture_png(d3d_device *dev, void *texture, const char *path);   // dxgi.m (debugging)
d3d_texture *d3d_texture_wrap(d3d_device *dev, const uint32_t desc[TD_COUNT], void *mtl, int shared);   // mtl retained
d3d_buffer  *d3d_buffer_wrap(d3d_device *dev, const uint32_t desc[6], void *mtl, int shared, void *shm, size_t shm_size);
void         d3d_texture_replace(d3d_texture *t, void *mtl_texture);     // a swap chain's buffer after a resize
void         d3d_flush(d3d_device *dev, int wait);                       // commits the immediate context's work
void         d3d_mark_gpu(d3d_context *c, d3d_object *o);                // a copy recorded now reads or writes o
d3d_device  *d3d_device_from(void *unknown);                             // an ID3D11Device / IDXGIDevice pointer
d3d_object  *dxgi_adapter_create(void);                                  // dxgi.m
d3d_object  *dxgi_output_create(void);                                   // the main display (IDXGIOutput6)
HRESULT      dxgi_check_hdr_metadata(uint32_t type, uint32_t size, const void *data);
// Swap chains on Direct3D 12 command queues (d3d12.m): DXGI's factory hands a queue there.
int     d3d12_is_queue(void *p);
HRESULT d3d12_swapchain_create(void *factory, void *queue, HANDLE hwnd, uint32_t w, uint32_t h, uint32_t format, uint32_t count, uint32_t flags, void **out);
#endif
