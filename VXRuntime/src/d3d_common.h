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
} d3d_object;

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
};
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
typedef struct { d3d_object base; uint32_t desc[6]; void *mtl; int shared; void *shm; size_t shm_size; } d3d_buffer;
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
void d3d_view_get_desc(const d3d_view *v, int which, uint32_t *out);   // 0 SRV, 1 RTV, 2 DSV, 3 UAV
struct d3d_draw_state;
typedef struct {
    d3d_object base;                 // ID3D11DeviceContext
    void *cmd;                       // the command buffer being recorded (retained), or NULL
    void *render;                    // the open render command encoder (retained), or NULL
    d3d_view *render_rtv[8], *render_dsv;   // what that encoder renders to
    d3d_view *rtv[8], *dsv;
    float viewport[6];
    struct d3d_draw_state *draw;     // shaders, buffers, states (d3d11_draw.m)
} d3d_context;

void d3d_vtable_override(void **vt, const char *const *names, int count, const d3d_method *impl);
#define D3D_VTABLE_OVERRIDE(vt, iface, impl) d3d_vtable_override(vt, vt_##iface, VT_COUNT_##iface, impl)
void d3d_end_render(d3d_context *c);                                     // closes the open render encoder
void *d3d_context_cmd(d3d_context *c);                                   // id<MTLCommandBuffer>, created on demand
extern const d3d_method d3d_draw_device_methods[], d3d_draw_context_methods[];   // d3d11_draw.m
void d3d_draw_init(void);
void d3d_draw_release(d3d_context *c);

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
d3d_texture *d3d_texture_wrap(d3d_device *dev, const uint32_t desc[TD_COUNT], void *mtl, int shared);   // mtl retained
d3d_buffer  *d3d_buffer_wrap(d3d_device *dev, const uint32_t desc[6], void *mtl, int shared, void *shm, size_t shm_size);
void         d3d_texture_replace(d3d_texture *t, void *mtl_texture);     // a swap chain's buffer after a resize
void         d3d_flush(d3d_device *dev, int wait);                       // commits the immediate context's work
d3d_device  *d3d_device_from(void *unknown);                             // an ID3D11Device / IDXGIDevice pointer
d3d_object  *dxgi_adapter_create(void);                                  // dxgi.m
#endif
