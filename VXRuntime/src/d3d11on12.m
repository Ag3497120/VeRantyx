// D3D11On12: a Direct3D 11 device that works on a Direct3D 12 device's queue and wraps its resources. SteamVR hands a
// Direct3D 12 game's eye textures to its compositor this way (vrclient's D3D12 submit path calls D3D11On12CreateDevice
// and ID3D11On12Device::CreateWrappedResource).
//
// Both APIs are Metal here, so the Direct3D 11 device is an ordinary one (d3d11.m) whose Metal command queue is the
// Direct3D 12 queue's: its work runs after the Direct3D 12 work submitted before it and before the work submitted after
// it, which is what D3D11On12 promises (it executes on that queue). A wrapped resource is a Direct3D 11 texture or
// buffer object on the Direct3D 12 resource's Metal texture or buffer.
#include "d3d_common.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "d3d_tables.h"
#pragma clang diagnostic pop

typedef struct {
    void **vtbl;                     // ID3D11On12Device2
    d3d_device *dev;                 // the Direct3D 11 device whose interface this is (its reference count is ours)
    void *device12, *queue12;        // held
} on12;
// (on wrapped resources, as private data: the Direct3D 12 resource and the state it is in after Direct3D 11 used it)
static const GUID g_wrapped_guid = { 0x76d3a1c2, 0x0b5e, 0x4f1d, { 0x9a, 0x61, 0x3e, 0x11, 0x0c, 0x52, 0x7b, 0x90 } };
typedef struct { void *resource12; uint32_t out_state; } wrapped;

static void **g_on12_vtbl;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void make_tables(void);
static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

static HRESULT com_qi(void *obj, const GUID *iid, void **out) {
    return (*(HRESULT (WINAPI **)(void *, const GUID *, void **))*(void ***)obj)(obj, iid, out);
}
static uint32_t com_addref(void *obj) { return ((uint32_t (WINAPI *)(void *))(*(void ***)obj)[1])(obj); }
static uint32_t com_release(void *obj) { return ((uint32_t (WINAPI *)(void *))(*(void ***)obj)[2])(obj); }

HRESULT d3d11on12_query(d3d_device *dev, const GUID *iid, void **out) {
    on12 *o = (on12 *)dev->on12;
    if (!o || !out) return E_NOINTERFACE;
    if (d3d_same_guid(iid, &IID_ID3D11On12Device) || d3d_same_guid(iid, &IID_ID3D11On12Device1) ||
        d3d_same_guid(iid, &IID_ID3D11On12Device2)) {
        d3d_addref(&dev->base);
        *out = o;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
void d3d11on12_destroy(d3d_device *dev) {
    on12 *o = (on12 *)dev->on12;
    dev->on12 = NULL;
    if (!o) return;
    if (o->queue12) com_release(o->queue12);
    if (o->device12) com_release(o->device12);
    free(o);
}

static WINAPI HRESULT on12_qi(void *this, const GUID *iid, void **out) {
    on12 *o = this;
    if (!out) return E_POINTER;
    if (d3d_same_guid(iid, &IID_IUnknown_)) { d3d_addref(&o->dev->base); *out = o; return S_OK; }
    if (d3d11on12_query(o->dev, iid, out) == S_OK) return S_OK;
    return com_qi(o->dev, iid, out);                               // (the device's other interfaces)
}
static WINAPI uint32_t on12_addref(void *this) { return d3d_addref(&((on12 *)this)->dev->base); }
static WINAPI uint32_t on12_release(void *this) { return d3d_release(&((on12 *)this)->dev->base); }

// D3D11_RESOURCE_FLAGS: bind, misc, CPU access, structure stride. Without them, the bind flags follow the Direct3D 12
// resource's flags (render target, depth stencil, unordered access, shader resource unless denied).
typedef struct { uint32_t bind, misc, cpu, stride; } RESOURCE_FLAGS11;
static WINAPI HRESULT on12_wrap(void *this, void *resource12, const RESOURCE_FLAGS11 *flags, uint32_t in_state, uint32_t out_state,
                                const GUID *iid, void **out) {
    on12 *o = this;
    d3d12_resource_info info;
    if (!out) return E_POINTER;
    *out = NULL;
    if (!d3d12_resource_info_get(resource12, &info)) return E_INVALIDARG;
    uint32_t bind = flags ? flags->bind : (info.flags & 1 ? 0x20u : 0) | (info.flags & 2 ? 0x40u : 0) | (info.flags & 4 ? 0x80u : 0) |
                                          (info.flags & 8 ? 0 : 0x8u);
    d3d_object *wrapper = NULL;
    if (info.dimension == 3 && info.texture) {                     // 2D textures (eye textures, swap images)
        uint32_t desc[TD_COUNT] = { (uint32_t)info.width, info.height, info.mips ? info.mips : 1, info.depth_or_array, info.format,
                                    info.samples ? info.samples : 1, info.quality, 0, bind, flags ? flags->cpu : 0,
                                    flags ? flags->misc : 0 };
        wrapper = &d3d_texture_wrap(o->dev, desc, (void *)CFBridgingRetain((__bridge id)info.texture), 0)->base;
    } else if (info.dimension == 1 && info.buffer) {
        uint32_t desc[6] = { (uint32_t)info.width, 0, bind, flags ? flags->cpu : 0, flags ? flags->misc : 0,
                             flags ? flags->stride : 0 };
        wrapper = &d3d_buffer_wrap(o->dev, desc, (void *)CFBridgingRetain((__bridge id)info.buffer), 0, NULL, 0)->base;
    } else {
        vx_log("D3D11On12: wrapping a Direct3D 12 resource of dimension %d is not supported yet", info.dimension);
        return E_NOTIMPL;
    }
    wrapped w = { resource12, out_state };
    d3d_set_private(wrapper, &g_wrapped_guid, sizeof w, &w);
    HRESULT hr = com_qi(wrapper, iid, out);
    d3d_release(wrapper);
    if (vx_trace) vx_log("D3D11On12: wrapped a %llux%u resource (format %u), states %#x -> %#x", (unsigned long long)info.width,
                         info.height, info.format, in_state, out_state);
    return hr;
}
// Acquiring needs nothing (the work is ordered on one queue); releasing hands the resources back to Direct3D 12 in
// their output state, which its state tracking then expects.
static WINAPI void on12_acquire(void *this, void *const *resources, uint32_t n) { }
static WINAPI void on12_release_wrapped(void *this, void *const *resources, uint32_t n) {
    for (uint32_t i = 0; resources && i < n; i++) {
        void *r = resources[i];
        if (!r) continue;
        void *unknown = NULL;
        if (com_qi(r, &IID_IUnknown_, &unknown) != S_OK) continue;
        wrapped w;
        uint32_t size = sizeof w;
        if (d3d_get_private((d3d_object *)unknown, &g_wrapped_guid, &size, &w) == S_OK && size == sizeof w)
            d3d12_resource_force_state(w.resource12, w.out_state);
        com_release(unknown);
    }
}
static WINAPI HRESULT on12_get_device12(void *this, const GUID *iid, void **out) {
    on12 *o = this;
    if (!out) return E_POINTER;
    return com_qi(o->device12, iid, out);
}
static WINAPI HRESULT on12_unwrap(void *this, void *resource11, void *queue, const GUID *iid, void **out) {
    if (out) *out = NULL;
    return E_NOTIMPL;
}
static WINAPI HRESULT on12_return(void *this, void *resource11, uint32_t n, const uint64_t *values, void *const *fences) { return E_NOTIMPL; }

static WINAPI HRESULT D3D11On12CreateDevice(void *device12, uint32_t flags, const uint32_t *levels, uint32_t nlevels,
                                            void *const *queues, uint32_t nqueues, uint32_t node_mask,
                                            void **out_device, void **out_context, uint32_t *out_level) {
    pthread_once(&g_once, make_tables);
    if (out_device) *out_device = NULL;
    if (out_context) *out_context = NULL;
    void *dev12 = d3d12_device_of(device12);
    if (!dev12 || dev12 != device12 || !nqueues || !queues || !queues[0] || d3d12_device_of(queues[0]) != dev12 || node_mask > 1)
        return E_INVALIDARG;
    void *metal_queue = d3d12_queue_metal(queues[0]);
    if (nqueues > 1) vx_log("D3D11On12: %u queues given; work goes to the first", nqueues);
    typedef HRESULT (WINAPI *create_fn)(void *, uint32_t, HANDLE, uint32_t, const uint32_t *, uint32_t, uint32_t, void **, uint32_t *, void **);
    create_fn create = (create_fn)vx_find_export("d3d11.dll", "D3D11CreateDevice");
    void *device11 = NULL, *context11 = NULL;
    HRESULT hr = create(NULL, 1 /* D3D_DRIVER_TYPE_HARDWARE */, NULL, flags, levels, nlevels, 7, &device11, out_level, &context11);
    if (hr != S_OK) return hr;
    d3d_device *d = d3d_device_from(device11);
    CFBridgingRelease(d->queue);
    d->queue = (void *)CFBridgingRetain((__bridge id)metal_queue);
    on12 *o = calloc(1, sizeof *o);
    o->vtbl = g_on12_vtbl;
    o->dev = d;
    o->device12 = device12; com_addref(device12);
    o->queue12 = queues[0]; com_addref(queues[0]);
    d->on12 = (d3d_object *)o;
    if (vx_trace) vx_log("D3D11On12CreateDevice: a Direct3D 11 device on the Direct3D 12 queue");
    if (out_device) *out_device = device11; else com_release(device11);
    if (out_context) *out_context = context11; else com_release(context11);
    return S_OK;
}

static void make_tables(void) {
    static const d3d_method m[] = {
        { "QueryInterface", (void *)on12_qi }, { "AddRef", (void *)on12_addref }, { "Release", (void *)on12_release },
        { "CreateWrappedResource", (void *)on12_wrap }, { "ReleaseWrappedResources", (void *)on12_release_wrapped },
        { "AcquireWrappedResources", (void *)on12_acquire }, { "GetD3D12Device", (void *)on12_get_device12 },
        { "UnwrapUnderlyingResource", (void *)on12_unwrap }, { "ReturnUnderlyingResource", (void *)on12_return }, { NULL, NULL } };
    g_on12_vtbl = D3D_VTABLE(ID3D11On12Device2, m);
}

static const vx_export d3d11on12[] = { VX_EXPORT(D3D11On12CreateDevice) };
void vx_d3d11on12_init(void) {
    pthread_once(&g_once, make_tables);
    vx_register_module("d3d11.dll", d3d11on12, sizeof d3d11on12 / sizeof d3d11on12[0]);
}

#ifdef __aarch64__
#include "thunks/d3d11on12.m.inc"
#endif
