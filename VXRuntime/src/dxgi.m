// DXGI on Metal: the factory, the adapter (the Mac's GPU), its output (the main display), and swap chains.
//
// A swap chain owns one back buffer texture. Present draws it into the window's CAMetalLayer with a small
// shader, which also converts its format (layers take BGRA, Direct3D programs usually render RGBA) and scales
// it to the layer's pixel size.
#import <ImageIO/ImageIO.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "d3d_common.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "d3d_tables.h"
#pragma clang diagnostic pop

static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static void **g_factory_vtbl, **g_adapter_vtbl, **g_output_vtbl, **g_swapchain_vtbl;
static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;
static void make_tables(void);

static d3d_object *self_of(void *this) { return (d3d_object *)this; }
static HRESULT query(void *this, const GUID *iid, void **out, const GUID *const *known, int n) {
    if (!out) return E_POINTER;
    for (int i = 0; i < n; i++)
        if (d3d_same_guid(iid, known[i])) { d3d_addref(self_of(this)); *out = this; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI uint32_t obj_addref(void *this) { return d3d_addref(self_of(this)); }
static WINAPI uint32_t obj_release(void *this) { return d3d_release(self_of(this)); }
static WINAPI HRESULT obj_get_private(void *this, const GUID *g, uint32_t *size, void *data) { return d3d_get_private(self_of(this), g, size, data); }
static WINAPI HRESULT obj_set_private(void *this, const GUID *g, uint32_t size, const void *data) { return d3d_set_private(self_of(this), g, size, data); }
static WINAPI HRESULT obj_set_private_interface(void *this, const GUID *g, void *unk) { (void)unk; return d3d_set_private(self_of(this), g, 0, NULL); }
#define OBJECT_METHODS \
    { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release }, { "GetPrivateData", (void *)obj_get_private }, \
    { "SetPrivateData", (void *)obj_set_private }, { "SetPrivateDataInterface", (void *)obj_set_private_interface }
static HRESULT call_qi(void *obj, const GUID *iid, void **out) {
    return (*(HRESULT (WINAPI **)(void *, const GUID *, void **))*(void ***)obj)(obj, iid, out);
}

// Objects with a parent (adapter -> factory, output -> adapter, swap chain -> factory).
typedef struct { d3d_object base; d3d_object *parent; } child;
static void child_destroy(d3d_object *o) { if (((child *)o)->parent) d3d_release(((child *)o)->parent); }
static WINAPI HRESULT child_get_parent(void *this, const GUID *iid, void **out) {
    child *c = this;
    if (!c->parent) { *out = NULL; return E_NOINTERFACE; }
    return call_qi(c->parent, iid, out);
}

// ---- the factory ----------------------------------------------------------------------------------------------------------
static d3d_object *factory_new(void) {
    pthread_once(&g_tables_once, make_tables);
    return d3d_alloc(sizeof(d3d_object), K_FACTORY, g_factory_vtbl, NULL);
}
static WINAPI HRESULT factory_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_IDXGIObject, &IID_IDXGIFactory, &IID_IDXGIFactory1, &IID_IDXGIFactory2,
                                         &IID_IDXGIFactory3, &IID_IDXGIFactory4 };
    return query(this, iid, out, known, 7);
}
static WINAPI HRESULT factory_get_parent(void *this, const GUID *iid, void **out) { (void)this; (void)iid; *out = NULL; return E_NOINTERFACE; }
static d3d_object *adapter_new(d3d_object *factory);
static WINAPI HRESULT factory_enum_adapters(void *this, uint32_t i, void **out) {
    if (!out) return E_INVALIDARG;
    if (i) { *out = NULL; return DXGI_ERROR_NOT_FOUND; }
    *out = adapter_new(this);
    return S_OK;
}
// IDXGIFactory4: the adapter by LUID (there is one), no WARP adapter; IDXGIFactory3: creation flags.
static WINAPI HRESULT factory_enum_by_luid(void *this, uint64_t luid, const GUID *iid, void **out) {
    (void)luid;
    void *a = NULL;
    HRESULT hr = factory_enum_adapters(this, 0, &a);
    if (hr) return hr;
    hr = ((HRESULT (WINAPI *)(void *, const GUID *, void **))(*(void ***)a)[0])(a, iid, out);
    ((uint32_t (WINAPI *)(void *))(*(void ***)a)[2])(a);
    return hr;
}
static WINAPI HRESULT factory_enum_warp(void *this, const GUID *iid, void **out) { (void)this; (void)iid; *out = NULL; return DXGI_ERROR_NOT_FOUND; }
static WINAPI uint32_t factory_creation_flags(void *this) { (void)this; return 0; }
static WINAPI HRESULT factory_make_window_association(void *this, HANDLE w, uint32_t flags) { (void)this; (void)w; (void)flags; return S_OK; }
static WINAPI HRESULT factory_get_window_association(void *this, HANDLE *w) { (void)this; *w = NULL; return S_OK; }
static WINAPI int factory_is_current(void *this) { (void)this; return 1; }
static WINAPI int factory_stereo(void *this) { (void)this; return 0; }

HRESULT dxgi_create_factory(const GUID *iid, void **out) {
    if (!out) return E_POINTER;
    d3d_object *f = factory_new();
    HRESULT hr = factory_qi(f, iid, out);
    d3d_release(f);
    return hr;
}
static WINAPI HRESULT CreateDXGIFactory(const GUID *iid, void **out) { if (vx_trace) vx_log("CreateDXGIFactory"); return dxgi_create_factory(iid, out); }
static WINAPI HRESULT CreateDXGIFactory1(const GUID *iid, void **out) { if (vx_trace) vx_log("CreateDXGIFactory1"); return dxgi_create_factory(iid, out); }
static WINAPI HRESULT CreateDXGIFactory2(uint32_t flags, const GUID *iid, void **out) { (void)flags; return dxgi_create_factory(iid, out); }

// ---- the adapter -----------------------------------------------------------------------------------------------------------
static d3d_object *adapter_new(d3d_object *factory) {
    pthread_once(&g_tables_once, make_tables);
    child *a = d3d_alloc(sizeof *a, K_ADAPTER, g_adapter_vtbl, NULL);
    a->parent = factory ? factory : factory_new();
    if (factory) d3d_addref(factory);
    a->base.destroy = child_destroy;
    return &a->base;
}
d3d_object *dxgi_adapter_create(void) { return adapter_new(NULL); }
static WINAPI HRESULT adapter_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_IDXGIObject, &IID_IDXGIAdapter, &IID_IDXGIAdapter1, &IID_IDXGIAdapter2, &IID_IDXGIAdapter3 };
    return query(this, iid, out, known, 6);
}
// DXGI_QUERY_VIDEO_MEMORY_INFO: budget, current usage, available for reservation, current reservation. The local
// segment is the unified memory Metal lets the GPU use.
static WINAPI HRESULT adapter_query_video_memory(void *this, uint32_t node, uint32_t group, uint64_t *info) {
    (void)this; (void)node;
    memset(info, 0, 32);
    if (group == 0) {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        info[0] = dev.recommendedMaxWorkingSetSize;
        info[1] = dev.currentAllocatedSize;
        info[2] = info[0] / 2;
    }
    return S_OK;
}
static WINAPI HRESULT adapter_set_reservation(void *this, uint32_t node, uint32_t group, uint64_t bytes) { (void)this; (void)node; (void)group; (void)bytes; return S_OK; }
static WINAPI HRESULT adapter_register_budget_event(void *this, HANDLE event, DWORD *cookie) { (void)this; (void)event; *cookie = 1; return S_OK; }
static WINAPI void adapter_unregister_budget_event(void *this, DWORD cookie) { (void)this; (void)cookie; }
// DXGI_ADAPTER_DESC(1): description[128], vendor, device, subsystem, revision, dedicated video memory, dedicated
// system memory, shared system memory, LUID (, flags).
static void adapter_desc(uint8_t *d, int with_flags) {
    memset(d, 0, with_flags ? 312 : 304);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    char name[200];
    snprintf(name, sizeof name, "%s (Metal, VXRuntime)", dev ? dev.name.UTF8String : "Apple GPU");
    vx_utf8_to_utf16(name, strlen(name), (WCHAR *)d, 127);
    *(uint32_t *)(d + 256) = 0x106B;                                   // Apple
    uint64_t memory = 0; size_t len = sizeof memory;
    sysctlbyname("hw.memsize", &memory, &len, NULL, 0);
    *(uint64_t *)(d + 272) = dev ? dev.recommendedMaxWorkingSetSize : memory / 2;   // unified memory the GPU may use
    *(uint64_t *)(d + 288) = memory / 2;
    *(uint32_t *)(d + 296) = 0x5658;                                   // a stable LUID
}
static WINAPI HRESULT adapter_get_desc(void *this, uint8_t *d) { (void)this; adapter_desc(d, 0); return S_OK; }
static WINAPI HRESULT adapter_get_desc1(void *this, uint8_t *d) { (void)this; adapter_desc(d, 1); return S_OK; }
// DXGI_ADAPTER_DESC2: DESC1, then graphics and compute preemption granularities (DMA buffer, dispatch).
static WINAPI HRESULT adapter_get_desc2(void *this, uint8_t *d) { (void)this; adapter_desc(d, 1); *(uint32_t *)(d + 312) = 0; *(uint32_t *)(d + 316) = 1; return S_OK; }
static d3d_object *output_new(d3d_object *adapter);
static WINAPI HRESULT adapter_enum_outputs(void *this, uint32_t i, void **out) {
    if (i) { *out = NULL; return DXGI_ERROR_NOT_FOUND; }
    *out = output_new(this);
    return S_OK;
}
static WINAPI HRESULT adapter_check_interface(void *this, const GUID *iid, int64_t *version) { (void)this; (void)iid; (void)version; return DXGI_ERROR_UNSUPPORTED; }

// ---- the output (the main display) ------------------------------------------------------------------------------------------
static d3d_object *output_new(d3d_object *adapter) {
    child *o = d3d_alloc(sizeof *o, K_OUTPUT, g_output_vtbl, NULL);
    o->parent = adapter;
    d3d_addref(adapter);
    o->base.destroy = child_destroy;
    return &o->base;
}
static WINAPI HRESULT output_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_IDXGIObject, &IID_IDXGIOutput };
    return query(this, iid, out, known, 3);
}
static int refresh_rate(void) {
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    double hz = m ? CGDisplayModeGetRefreshRate(m) : 60;
    if (m) CGDisplayModeRelease(m);
    return hz > 1 ? (int)(hz + 0.5) : 60;
}
// DXGI_OUTPUT_DESC: device name[32], desktop rectangle, attached, rotation, monitor.
static WINAPI HRESULT output_get_desc(void *this, uint8_t *d) {
    (void)this;
    memset(d, 0, 96);
    vx_utf8_to_utf16("\\\\.\\DISPLAY1", 12, (WCHAR *)d, 31);
    RECT r; vx_virtual_screen(&r);
    memcpy(d + 64, &r, sizeof r);
    *(uint32_t *)(d + 80) = 1;
    *(uint32_t *)(d + 84) = 1;                                         // DXGI_MODE_ROTATION_IDENTITY
    *(HANDLE *)(d + 88) = (HANDLE)(intptr_t)0x10001;                   // the monitor handle user32 hands out
    return S_OK;
}
// DXGI_MODE_DESC: width, height, refresh numerator/denominator, format, scanline ordering, scaling.
static void current_mode(uint32_t *m, uint32_t format) {
    RECT r; vx_virtual_screen(&r);
    m[0] = (uint32_t)r.right; m[1] = (uint32_t)r.bottom; m[2] = (uint32_t)refresh_rate(); m[3] = 1;
    m[4] = format ? format : 28; m[5] = 1; m[6] = 0;                    // progressive, unspecified scaling
}
static WINAPI HRESULT output_get_modes(void *this, uint32_t format, uint32_t flags, uint32_t *n, uint32_t *modes) {
    (void)this; (void)flags;
    if (!modes) { *n = 1; return S_OK; }
    if (*n < 1) return (HRESULT)0x887A0003;                             // DXGI_ERROR_MORE_DATA
    current_mode(modes, format);
    *n = 1;
    return S_OK;
}
static WINAPI HRESULT output_find_mode(void *this, const uint32_t *want, uint32_t *closest, void *device) {
    (void)this; (void)device;
    current_mode(closest, want ? want[4] : 0);
    return S_OK;
}
static WINAPI HRESULT output_wait_vblank(void *this) { (void)this; usleep(1000000 / refresh_rate()); return S_OK; }
static WINAPI HRESULT output_take_ownership(void *this, void *dev, int exclusive) { (void)this; (void)dev; (void)exclusive; return S_OK; }
static WINAPI void output_release_ownership(void *this) { (void)this; }

// ---- swap chains -------------------------------------------------------------------------------------------------------------
typedef struct {
    child base;                       // parent: the factory
    d3d_device *dev;
    HANDLE hwnd;
    uint32_t width, height, format, count, usage, flags, swap_effect, sample_count;
    d3d_texture *buffer;
    void *layer;                      // CAMetalLayer (owned by the window's view)
    void *pipeline;                   // id<MTLRenderPipelineState> (retained)
    MTLPixelFormat layer_format;
    uint32_t presents;
    int fullscreen;
} swapchain;

// The present shader: a full-screen triangle sampling the back buffer.
static const char *g_present_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V present_vs(uint id [[vertex_id]]) {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    V o; o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
    "}\n"
    "fragment float4 present_fs(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
    "    constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
    "    return float4(t.sample(s, in.uv).rgb, 1);\n"
    "}\n";

static void *present_pipeline(id<MTLDevice> dev, MTLPixelFormat format) {
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(g_present_msl) options:nil error:&err];
    if (!lib) { vx_log("Direct3D: the present shader did not compile: %s", err.localizedDescription.UTF8String); return NULL; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"present_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"present_fs"];
    pd.colorAttachments[0].pixelFormat = format;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!ps) { vx_log("Direct3D: the present pipeline failed: %s", err.localizedDescription.UTF8String); return NULL; }
    return (void *)CFBridgingRetain(ps);
}

static HRESULT make_buffer(swapchain *sc) {
    uint32_t desc[TD_COUNT] = { sc->width, sc->height, 1, 1, sc->format, 1, 0, 0, 0x20 | 0x8, 0, 0 };   // render target, shader resource
    HRESULT hr;
    d3d_texture *t = d3d_texture_create(sc->dev, desc, &hr);
    if (!t) return hr;
    if (!sc->buffer) { sc->buffer = t; return S_OK; }
    // Keep the buffer object (programs may hold it); give it the new Metal texture.
    void *mtl = t->mtl;
    t->mtl = NULL;
    d3d_release(&t->base);
    d3d_texture_replace(sc->buffer, mtl);
    sc->buffer->desc[TD_FORMAT] = sc->format;
    return S_OK;
}
static void client_size(HANDLE hwnd, uint32_t *w, uint32_t *h) {
    vx_window *win = vx_window_get(hwnd);
    *w = win && win->client_w > 0 ? (uint32_t)win->client_w : 640;
    *h = win && win->client_h > 0 ? (uint32_t)win->client_h : 480;
}

static void swapchain_destroy(d3d_object *o);
static HRESULT swapchain_create(void *factory, void *device, HANDLE hwnd, uint32_t w, uint32_t h, uint32_t format,
                                uint32_t count, uint32_t usage, uint32_t flags, uint32_t effect, uint32_t samples, void **out) {
    d3d_device *dev = d3d_device_from(device);
    vx_window *win = vx_window_get(hwnd);
    if (!dev || !win || !out) return E_INVALIDARG;
    if (d3d_mtl_format(format, NULL) == MTLPixelFormatInvalid) { vx_log("Direct3D: swap chain format %u is not supported", format); return E_INVALIDARG; }
    swapchain *sc = d3d_alloc(sizeof *sc, K_SWAPCHAIN, g_swapchain_vtbl, dev);
    sc->base.parent = factory;
    d3d_addref(factory);
    d3d_addref(&dev->base);
    sc->dev = dev; sc->hwnd = hwnd; sc->format = format; sc->count = count; sc->usage = usage; sc->flags = flags;
    sc->base.base.destroy = swapchain_destroy;
    sc->swap_effect = effect; sc->sample_count = samples ? samples : 1;
    if (!w || !h) client_size(hwnd, &w, &h);
    sc->width = w; sc->height = h;
    HRESULT hr = make_buffer(sc);
    if (hr != S_OK) { d3d_release(&sc->base.base); return hr; }
    sc->layer_format = d3d_is_srgb(format) ? MTLPixelFormatBGRA8Unorm_sRGB : format == 10 ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
    sc->layer = vx_cocoa_metal_layer(win, dev->mtl, (int)sc->layer_format);
    sc->pipeline = present_pipeline((__bridge id<MTLDevice>)dev->mtl, sc->layer_format);
    if (!sc->layer || !sc->pipeline) { d3d_release(&sc->base.base); return E_FAIL; }
    *out = sc;
    return S_OK;
}
static void swapchain_destroy(d3d_object *o) {
    swapchain *sc = (swapchain *)o;
    if (sc->buffer) d3d_release(&sc->buffer->base);
    if (sc->pipeline) CFBridgingRelease(sc->pipeline);
    d3d_release(&sc->dev->base);
    child_destroy(o);
}
// DXGI_SWAP_CHAIN_DESC: buffer mode (width, height, refresh, format, ordering, scaling), sample count/quality,
// usage, buffer count, window, windowed, swap effect, flags.
static WINAPI HRESULT factory_create_swapchain(void *this, void *device, const uint8_t *desc, void **out) {
    const uint32_t *mode = (const uint32_t *)desc;
    return swapchain_create(this, device, *(HANDLE *)(desc + 48), mode[0], mode[1], mode[4], *(const uint32_t *)(desc + 40),
                            *(const uint32_t *)(desc + 36), *(const uint32_t *)(desc + 64), *(const uint32_t *)(desc + 60),
                            *(const uint32_t *)(desc + 28), out);
}
// DXGI_SWAP_CHAIN_DESC1: width, height, format, stereo, sample count/quality, usage, buffer count, scaling,
// swap effect, alpha mode, flags.
static WINAPI HRESULT factory_create_swapchain_hwnd(void *this, void *device, HANDLE hwnd, const uint32_t *d, const void *fs, void *output, void **out) {
    (void)fs; (void)output;
    return swapchain_create(this, device, hwnd, d[0], d[1], d[2], d[7], d[6], d[11], d[9], d[4], out);
}

static WINAPI HRESULT swapchain_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_IDXGIObject, &IID_IDXGIDeviceSubObject, &IID_IDXGISwapChain, &IID_IDXGISwapChain1,
                                         &IID_IDXGISwapChain2, &IID_IDXGISwapChain3 };
    return query(this, iid, out, known, 7);
}
// IDXGISwapChain2/3: frame latency (presents do not queue up here, so the waitable object is always signalled),
// source size, color spaces (sRGB only), the current back buffer (there is one).
static WINAPI HRESULT swapchain_set_latency(void *this, uint32_t n) { (void)this; (void)n; return S_OK; }
static WINAPI HRESULT swapchain_get_latency(void *this, uint32_t *n) { (void)this; *n = 1; return S_OK; }
static WINAPI HANDLE swapchain_latency_object(void *this) {
    (void)this;
    HANDLE (WINAPI *create)(void *, BOOL, BOOL, const WCHAR *) = (HANDLE (WINAPI *)(void *, BOOL, BOOL, const WCHAR *))vx_find_export("kernel32.dll", "CreateEventW");
    return create(NULL, 1, 1, NULL);                            // manual reset, signalled
}
static WINAPI HRESULT swapchain_set_source_size(void *this, uint32_t w, uint32_t h) { (void)this; (void)w; (void)h; return S_OK; }
static WINAPI HRESULT swapchain_get_source_size(void *this, uint32_t *w, uint32_t *h) { swapchain *sc = this; *w = sc->width; *h = sc->height; return S_OK; }
static WINAPI uint32_t swapchain_back_buffer_index(void *this) { (void)this; return 0; }
static WINAPI HRESULT swapchain_check_color_space(void *this, uint32_t cs, uint32_t *support) { (void)this; *support = cs == 0 ? 1 : 0; return S_OK; }
static WINAPI HRESULT swapchain_set_color_space(void *this, uint32_t cs) { (void)this; return cs == 0 ? S_OK : E_INVALIDARG; }
static WINAPI HRESULT swapchain_get_device(void *this, const GUID *iid, void **out) { return call_qi(((swapchain *)this)->dev, iid, out); }
static WINAPI HRESULT swapchain_get_buffer(void *this, uint32_t index, const GUID *iid, void **out) {
    swapchain *sc = this;
    (void)index;                                    // one buffer stands for all of them
    return call_qi(sc->buffer, iid, out);
}
// VXRT_PRESENT_SNAPSHOT=file.png (with VXRT_PRESENT_SNAPSHOT_AT=n, default 300): the back buffer of the n-th
// present is written to that file (8-bit RGBA/BGRA buffers), to check what a program draws.
static void snapshot(swapchain *sc) {
    const char *path = getenv("VXRT_PRESENT_SNAPSHOT");
    const char *at = getenv("VXRT_PRESENT_SNAPSHOT_AT");
    if (!path || sc->presents + 1 != (uint64_t)(at ? atoll(at) : 300)) return;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)sc->buffer->mtl;
    MTLPixelFormat f = tex.pixelFormat;
    int bgra = f == MTLPixelFormatBGRA8Unorm || f == MTLPixelFormatBGRA8Unorm_sRGB;
    if (!bgra && f != MTLPixelFormatRGBA8Unorm && f != MTLPixelFormatRGBA8Unorm_sRGB) { vx_log("snapshot: back buffer format %d is not 8-bit", (int)f); return; }
    NSUInteger w = tex.width, h = tex.height, row = w * 4;
    id<MTLBuffer> buf = [tex.device newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cmd = [(__bridge id<MTLCommandQueue>)sc->dev->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [blit endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGBitmapInfo info = bgra ? (kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst) : (kCGBitmapByteOrder32Big | kCGImageAlphaNoneSkipLast);
    CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, info);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    CGImageDestinationRef dst = img ? CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, NULL) : NULL;
    if (dst) { CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
    if (img) CGImageRelease(img);
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    vx_log("snapshot of present %llu (%lux%lu) written to %s", (unsigned long long)sc->presents + 1, (unsigned long)w, (unsigned long)h, path);
}
#define DXGI_PRESENT_TEST 1
static WINAPI HRESULT swapchain_present(void *this, uint32_t sync, uint32_t flags) {
    swapchain *sc = this;
    if (flags & DXGI_PRESENT_TEST) return S_OK;
    d3d_flush(sc->dev, 0);                          // the frame's work goes first, on the same queue
    snapshot(sc);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)sc->layer;
    layer.displaySyncEnabled = sync != 0;
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        if (!drawable) return S_OK;                 // hidden or minimized: nothing to show
        id<MTLCommandBuffer> cmd = [(__bridge id<MTLCommandQueue>)sc->dev->queue commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)sc->pipeline];
        [enc setFragmentTexture:(__bridge id<MTLTexture>)sc->buffer->mtl atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        [cmd presentDrawable:drawable];
        [cmd commit];
    }
    sc->presents++;
    return S_OK;
}
static WINAPI HRESULT swapchain_present1(void *this, uint32_t sync, uint32_t flags, const void *params) { (void)params; return swapchain_present(this, sync, flags); }
static WINAPI HRESULT swapchain_resize_buffers(void *this, uint32_t count, uint32_t w, uint32_t h, uint32_t format, uint32_t flags) {
    swapchain *sc = this;
    d3d_flush(sc->dev, 1);
    if (count) sc->count = count;
    if (format) sc->format = format;
    sc->flags = flags;
    if (!w || !h) client_size(sc->hwnd, &w, &h);
    sc->width = w; sc->height = h;
    return make_buffer(sc);
}
static WINAPI HRESULT swapchain_resize_target(void *this, const uint32_t *mode) {
    (void)this; (void)mode;
    return S_OK;                                    // the window keeps its size; ResizeBuffers follows it
}
static WINAPI HRESULT swapchain_get_desc(void *this, uint8_t *d) {
    swapchain *sc = this;
    memset(d, 0, 72);
    uint32_t *mode = (uint32_t *)d;
    mode[0] = sc->width; mode[1] = sc->height; mode[2] = (uint32_t)refresh_rate(); mode[3] = 1; mode[4] = sc->format;
    *(uint32_t *)(d + 28) = sc->sample_count;
    *(uint32_t *)(d + 36) = sc->usage;
    *(uint32_t *)(d + 40) = sc->count;
    *(HANDLE *)(d + 48) = sc->hwnd;
    *(uint32_t *)(d + 56) = !sc->fullscreen;
    *(uint32_t *)(d + 60) = sc->swap_effect;
    *(uint32_t *)(d + 64) = sc->flags;
    return S_OK;
}
static WINAPI HRESULT swapchain_get_desc1(void *this, uint32_t *d) {
    swapchain *sc = this;
    memset(d, 0, 48);
    d[0] = sc->width; d[1] = sc->height; d[2] = sc->format; d[4] = sc->sample_count; d[6] = sc->usage; d[7] = sc->count;
    d[9] = sc->swap_effect; d[11] = sc->flags;
    return S_OK;
}
static WINAPI HRESULT swapchain_set_fullscreen(void *this, int full, void *output) { (void)output; ((swapchain *)this)->fullscreen = full; return S_OK; }
static WINAPI HRESULT swapchain_get_fullscreen(void *this, int *full, void **output) {
    if (full) *full = ((swapchain *)this)->fullscreen;
    if (output) *output = NULL;
    return S_OK;
}
static WINAPI HRESULT swapchain_get_output(void *this, void **out) {
    (void)this;
    d3d_object *adapter = dxgi_adapter_create();
    *out = output_new(adapter);
    d3d_release(adapter);
    return S_OK;
}
static WINAPI HRESULT swapchain_frame_statistics(void *this, void *stats) { (void)this; (void)stats; return DXGI_ERROR_FRAME_STATISTICS_DISJOINT; }
static WINAPI HRESULT swapchain_last_present_count(void *this, uint32_t *n) { *n = ((swapchain *)this)->presents; return S_OK; }
static WINAPI HRESULT swapchain_get_hwnd(void *this, HANDLE *w) { *w = ((swapchain *)this)->hwnd; return S_OK; }
static WINAPI int swapchain_mono(void *this) { (void)this; return 0; }

// ---- tables and exports --------------------------------------------------------------------------------------------------------
static void make_tables(void) {
    static const d3d_method factory[] = { { "QueryInterface", (void *)factory_qi }, OBJECT_METHODS, { "GetParent", (void *)factory_get_parent },
        { "EnumAdapters", (void *)factory_enum_adapters }, { "EnumAdapters1", (void *)factory_enum_adapters },
        { "MakeWindowAssociation", (void *)factory_make_window_association }, { "GetWindowAssociation", (void *)factory_get_window_association },
        { "CreateSwapChain", (void *)factory_create_swapchain }, { "CreateSwapChainForHwnd", (void *)factory_create_swapchain_hwnd },
        { "IsCurrent", (void *)factory_is_current }, { "EnumAdapterByLuid", (void *)factory_enum_by_luid },
        { "EnumWarpAdapter", (void *)factory_enum_warp }, { "GetCreationFlags", (void *)factory_creation_flags }, { "IsWindowedStereoEnabled", (void *)factory_stereo }, { NULL, NULL } };
    static const d3d_method adapter[] = { { "QueryInterface", (void *)adapter_qi }, OBJECT_METHODS, { "GetParent", (void *)child_get_parent },
        { "EnumOutputs", (void *)adapter_enum_outputs }, { "GetDesc", (void *)adapter_get_desc }, { "GetDesc1", (void *)adapter_get_desc1 },
        { "CheckInterfaceSupport", (void *)adapter_check_interface }, { "GetDesc2", (void *)adapter_get_desc2 },
        { "QueryVideoMemoryInfo", (void *)adapter_query_video_memory }, { "SetVideoMemoryReservation", (void *)adapter_set_reservation },
        { "RegisterVideoMemoryBudgetChangeNotificationEvent", (void *)adapter_register_budget_event },
        { "UnregisterVideoMemoryBudgetChangeNotification", (void *)adapter_unregister_budget_event }, { NULL, NULL } };
    static const d3d_method output[] = { { "QueryInterface", (void *)output_qi }, OBJECT_METHODS, { "GetParent", (void *)child_get_parent },
        { "GetDesc", (void *)output_get_desc }, { "GetDisplayModeList", (void *)output_get_modes },
        { "FindClosestMatchingMode", (void *)output_find_mode }, { "WaitForVBlank", (void *)output_wait_vblank },
        { "TakeOwnership", (void *)output_take_ownership }, { "ReleaseOwnership", (void *)output_release_ownership }, { NULL, NULL } };
    static const d3d_method sc[] = { { "QueryInterface", (void *)swapchain_qi }, OBJECT_METHODS, { "GetParent", (void *)child_get_parent },
        { "GetDevice", (void *)swapchain_get_device }, { "Present", (void *)swapchain_present }, { "Present1", (void *)swapchain_present1 },
        { "GetBuffer", (void *)swapchain_get_buffer }, { "SetFullscreenState", (void *)swapchain_set_fullscreen },
        { "GetFullscreenState", (void *)swapchain_get_fullscreen }, { "GetDesc", (void *)swapchain_get_desc },
        { "GetDesc1", (void *)swapchain_get_desc1 }, { "ResizeBuffers", (void *)swapchain_resize_buffers },
        { "ResizeTarget", (void *)swapchain_resize_target }, { "GetContainingOutput", (void *)swapchain_get_output },
        { "GetFrameStatistics", (void *)swapchain_frame_statistics }, { "GetLastPresentCount", (void *)swapchain_last_present_count },
        { "GetHwnd", (void *)swapchain_get_hwnd }, { "IsTemporaryMonoSupported", (void *)swapchain_mono },
        { "SetMaximumFrameLatency", (void *)swapchain_set_latency }, { "GetMaximumFrameLatency", (void *)swapchain_get_latency },
        { "GetFrameLatencyWaitableObject", (void *)swapchain_latency_object }, { "SetSourceSize", (void *)swapchain_set_source_size },
        { "GetSourceSize", (void *)swapchain_get_source_size }, { "GetCurrentBackBufferIndex", (void *)swapchain_back_buffer_index },
        { "CheckColorSpaceSupport", (void *)swapchain_check_color_space }, { "SetColorSpace1", (void *)swapchain_set_color_space }, { NULL, NULL } };
    g_factory_vtbl = D3D_VTABLE(IDXGIFactory4, factory);
    g_adapter_vtbl = D3D_VTABLE(IDXGIAdapter3, adapter);
    g_output_vtbl = D3D_VTABLE(IDXGIOutput, output);
    g_swapchain_vtbl = D3D_VTABLE(IDXGISwapChain3, sc);
}

static const vx_export dxgi[] = { VX_EXPORT(CreateDXGIFactory), VX_EXPORT(CreateDXGIFactory1), VX_EXPORT(CreateDXGIFactory2) };
void vx_dxgi_init(void) {
    pthread_once(&g_tables_once, make_tables);
    vx_register_module("dxgi.dll", dxgi, sizeof dxgi / sizeof dxgi[0]);
}
