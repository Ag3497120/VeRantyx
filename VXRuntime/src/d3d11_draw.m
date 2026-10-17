// Direct3D 11 drawing on Metal: shaders (DXBC translated to Metal Shading Language by dxbc.c), input layouts,
// blend, depth-stencil, rasterizer and sampler states, the context's pipeline bindings, and draw calls.
//
// Metal bakes shaders, vertex layout (with the strides of the bound vertex buffers), render target formats and
// blending into one pipeline state object; those are made on first use for each combination and cached.
// Draws go into one render pass for as long as the render targets stay the same.
#include <CommonCrypto/CommonDigest.h>
#include "d3d_common.h"
#include "dxbc.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "d3d_tables.h"
#pragma clang diagnostic pop

static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static void **g_gs_vtbl, **g_vs_vtbl, **g_ps_vtbl, **g_cs_vtbl, **g_uav_vtbl, **g_layout_vtbl, **g_blend_vtbl, **g_depth_vtbl, **g_raster_vtbl, **g_sampler_vtbl;

static d3d_object *self_of(void *this) { return (d3d_object *)this; }
static WINAPI uint32_t child_addref(void *this) { return d3d_addref(self_of(this)); }
static WINAPI uint32_t child_release(void *this) { return d3d_release(self_of(this)); }
static WINAPI void child_get_device(void *this, void **out) { d3d_device *d = self_of(this)->dev; d3d_addref(&d->base); *out = d; }
static WINAPI HRESULT child_get_private(void *this, const GUID *g, uint32_t *size, void *data) { return d3d_get_private(self_of(this), g, size, data); }
static WINAPI HRESULT child_set_private(void *this, const GUID *g, uint32_t size, const void *data) { return d3d_set_private(self_of(this), g, size, data); }
#define CHILD_METHODS \
    { "AddRef", (void *)child_addref }, { "Release", (void *)child_release }, { "GetDevice", (void *)child_get_device }, \
    { "GetPrivateData", (void *)child_get_private }, { "SetPrivateData", (void *)child_set_private }
static HRESULT query(void *this, const GUID *iid, void **out, const GUID *iface) {
    if (!out) return E_POINTER;
    if (d3d_same_guid(iid, &IID_IUnknown_) || d3d_same_guid(iid, &IID_ID3D11DeviceChild) || d3d_same_guid(iid, iface)) {
        d3d_addref(self_of(this)); *out = this; return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

// ---- shaders ------------------------------------------------------------------------------------------------------------
// function: the Metal function, once compiled (ready). Shaders compile in the background: Metal's compiler service
// works on many at once, and a game creating thousands while it loads would otherwise wait for each in turn; the
// first draw (or dispatch) that needs one waits for it.
// so: a geometry shader made with stream output (so_layout), else NULL.
typedef struct { d3d_object base; dxbc_shader info; void *function, *compute; uint32_t id, hash; volatile int ready; void *dual; int dual_tried; void *variant; int variant_tried; void *so; } d3d_shader;   // hash: as in VXRT_DUMP_SHADERS names
// Stream output (stream 0): which output register components go where in the target buffers. An entry copies
// count components from register reg, component comp on, to dword dword of each vertex's record in buffer slot.
typedef struct { uint32_t reg, comp, count, slot_dword; } so_entry;   // slot_dword: slot << 24 | dword
typedef struct { uint32_t n, stride[4]; int rasterized; so_entry e[64]; } so_layout;
static pthread_mutex_t g_compile_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_compiled = PTHREAD_COND_INITIALIZER;
// Variants compile synchronously under their own lock: the compile needs Metal's compiler queue, whose completion
// handlers take g_compile_lock (holding that one here deadlocked when many shaders were compiling).
static pthread_mutex_t g_variant_lock = PTHREAD_MUTEX_INITIALIZER;
// Dual-source blending (SRC1 factors) reads the pixel shader's second output as Metal's color(0) index(1): a
// variant of the shader with o1 declared so, made the first time a blend state asks for it.
static id<MTLFunction> dual_source_function(d3d_device *dev, d3d_shader *s) {
    pthread_mutex_lock(&g_variant_lock);
    if (!s->dual_tried) {
        s->dual_tried = 1;
        const char *msl = s->info.msl, *at = msl ? strstr(msl, " o1 [[color(1)]];") : NULL;
        if (at) {
            NSString *source = [NSString stringWithFormat:@"%.*s o1 [[color(0), index(1)]];%s", (int)(at - msl), msl, at + strlen(" o1 [[color(1)]];")];
            NSError *err = nil;
            id<MTLLibrary> lib = [(__bridge id<MTLDevice>)dev->mtl newLibraryWithSource:source options:nil error:&err];
            id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
            if (fn) s->dual = (void *)CFBridgingRetain(fn);
            else vx_log("Direct3D: dual-source variant of shader %08x did not compile: %s", s->hash, err.localizedDescription.UTF8String);
        }
    }
    pthread_mutex_unlock(&g_variant_lock);
    return (__bridge id<MTLFunction>)s->dual;
}
static id<MTLFunction> shader_function(d3d_shader *s) {
    if (!s) return nil;
    if (!__atomic_load_n(&s->ready, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&g_compile_lock);
        while (!s->ready) pthread_cond_wait(&g_compiled, &g_compile_lock);
        pthread_mutex_unlock(&g_compile_lock);
    }
    return (__bridge id<MTLFunction>)s->function;
}
static uint32_t g_next_id = 1;
static void shader_destroy(d3d_object *o) {
    d3d_shader *s = (d3d_shader *)o;
    if (s->function) CFBridgingRelease(s->function);
    if (s->dual) CFBridgingRelease(s->dual);
    if (s->variant) CFBridgingRelease(s->variant);
    if (s->compute) CFBridgingRelease(s->compute);
    free(s->so);
    dxbc_free(&s->info);
}
static WINAPI HRESULT vs_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11VertexShader); }
static WINAPI HRESULT ps_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11PixelShader); }
static WINAPI HRESULT cs_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11ComputeShader); }
static WINAPI HRESULT gs_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11GeometryShader); }
static WINAPI HRESULT uav_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11UnorderedAccessView); }
static WINAPI void uav_get_resource(void *this, void **out) { d3d_view *v = this; d3d_addref(v->resource); *out = v->resource; }
static WINAPI void uav_get_desc(void *this, uint32_t *out) { d3d_view_get_desc(this, 3, out); }
static void uav_destroy(d3d_object *o) {
    d3d_view *v = (d3d_view *)o;
    d3d_view_release_mtl(v);
    d3d_release(v->resource);
}
// Sets a UAV's hidden counter (D3D11_BUFFER_UAV_FLAG_APPEND or _COUNTER): a fresh 4-byte buffer holding the value,
// so work already encoded keeps the old one (as with a discarded buffer).
static void set_counter(d3d_view *v, uint32_t value) {
    if (!v || v->resource->kind != K_BUFFER || !(v->flags & 6) || value == 0xffffffffu) return;
    id<MTLDevice> dev = (__bridge id<MTLDevice>)v->base.dev->mtl;
    id<MTLBuffer> b = [dev newBufferWithBytes:&value length:4 options:MTLResourceStorageModeShared];
    if (v->counter) CFRelease(v->counter);
    v->counter = (void *)CFBridgingRetain(b);
}
static id<MTLBuffer> counter_of(d3d_view *v) {
    if (!v) return nil;
    if (!v->counter) set_counter(v, 0);
    return (__bridge id<MTLBuffer>)v->counter;
}

// VXRT_DUMP_SHADERS=directory: every translated shader is written there (for looking at the translation).
static void dump(const dxbc_shader *s, const void *code, size_t size) {
    const char *dir = getenv("VXRT_DUMP_SHADERS");
    if (!dir || !*dir) return;
    unsigned char md5[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(code, (CC_LONG)size, md5);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s_%02x%02x%02x%02x.metal", dir, s->type == 1 ? "vs" : s->type == 5 ? "cs" : "ps", md5[0], md5[1], md5[2], md5[3]);
    FILE *f = fopen(path, "w");
    if (f) { fputs(s->msl, f); fclose(f); }
}
// With VXRT_TRACE, every 500 shaders: how long translating and compiling them took (on the threads that made them).
static void shader_stats(uint64_t translate_ns, uint64_t compile_ns) {
    static uint64_t n, t, k;
    uint64_t count = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&t, translate_ns, __ATOMIC_RELAXED);
    __atomic_add_fetch(&k, compile_ns, __ATOMIC_RELAXED);
    if (count % 500 == 0) vx_log("Direct3D: %llu shaders: %llu ms translating, %llu ms compiling", (unsigned long long)count,
                                 (unsigned long long)(t / 1000000), (unsigned long long)(k / 1000000));
}
static HRESULT create_shader(d3d_device *dev, const void *code, size_t size, int want, void **vtbl, void **out) {
    d3d_shader *s = d3d_alloc(sizeof *s, K_SHADER, vtbl, dev);
    s->base.destroy = shader_destroy;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (dxbc_translate(code, size, &s->info) || s->info.type != want) {
        vx_log("Direct3D: shader not translated: %s", s->info.error[0] ? s->info.error : "wrong shader type");
        d3d_release(&s->base);
        return E_INVALIDARG;
    }
    dump(&s->info, code, size);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(code, (CC_LONG)size, digest);
    s->hash = (uint32_t)digest[0] << 24 | (uint32_t)digest[1] << 16 | (uint32_t)digest[2] << 8 | digest[3];
    if (!out) { d3d_release(&s->base); return S_FALSE; }   // only checking: translated is enough
    s->id = __sync_fetch_and_add(&g_next_id, 1);
    uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    NSString *source = @(s->info.msl);
    static int sync = -1;                              // VXRT_SYNC_SHADERS=1: compile before returning, as before
    if (sync < 0) sync = getenv("VXRT_SYNC_SHADERS") != NULL;
    d3d_addref(&s->base);                              // kept until its compile is done
    void (^done)(id<MTLLibrary>, NSError *) = ^(id<MTLLibrary> lib, NSError *err) {
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
        if (vx_trace) shader_stats(t1 - t0, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t1);
        if (!fn) {
            vx_log("Direct3D: translated shader %08x did not compile: %s", s->hash, err.localizedDescription.UTF8String);
            if (vx_trace) vx_log("%s", s->info.msl);
        }
        pthread_mutex_lock(&g_compile_lock);
        s->function = fn ? (void *)CFBridgingRetain(fn) : NULL;
        __atomic_store_n(&s->ready, 1, __ATOMIC_RELEASE);
        pthread_cond_broadcast(&g_compiled);
        pthread_mutex_unlock(&g_compile_lock);
        d3d_release(&s->base);
    };
    if (sync) {
        NSError *err = nil;
        done([(__bridge id<MTLDevice>)dev->mtl newLibraryWithSource:source options:nil error:&err], err);
    } else [(__bridge id<MTLDevice>)dev->mtl newLibraryWithSource:source options:nil completionHandler:done];
    *out = s;
    return S_OK;
}
static WINAPI HRESULT device_create_vs(void *this, const void *code, size_t size, void *linkage, void **out) {
    (void)linkage; return create_shader(this, code, size, DXBC_VERTEX, g_vs_vtbl, out);
}
static WINAPI HRESULT device_create_ps(void *this, const void *code, size_t size, void *linkage, void **out) {
    (void)linkage; return create_shader(this, code, size, DXBC_PIXEL, g_ps_vtbl, out);
}
static WINAPI HRESULT device_create_cs(void *this, const void *code, size_t size, void *linkage, void **out) {
    (void)linkage; return create_shader(this, code, size, DXBC_COMPUTE, g_cs_vtbl, out);
}
// D3D11_UNORDERED_ACCESS_VIEW_DESC: format, dimension (1 buffer, 4 2D, 5 2D array, ...), then the buffer range
// (first element, count, flags: 1 raw) or the mip level and slices.
static WINAPI HRESULT device_create_uav(void *this, d3d_object *res, const uint32_t *desc, void **out) {
    if (!res || (res->kind != K_TEXTURE && res->kind != K_BUFFER)) return E_INVALIDARG;
    d3d_view *v = d3d_alloc(sizeof *v, K_UAV, g_uav_vtbl, this);
    v->resource = res;
    d3d_addref(res);
    v->base.destroy = uav_destroy;
    v->slices = 1;
    if (res->kind == K_TEXTURE) v->format = ((d3d_texture *)res)->desc[TD_FORMAT];
    if (desc) {
        if (desc[0]) v->format = desc[0];
        v->dim = desc[1];
        if (res->kind == K_BUFFER) { v->first = desc[2]; v->count = desc[3]; v->flags = desc[4]; }
        else if (desc[1] == 2 || desc[1] == 4) v->mip = desc[2];                                   // 1D, 2D
        else if (desc[1] == 3 || desc[1] == 5 || desc[1] == 8) { v->mip = desc[2]; v->first_slice = desc[3]; v->slices = desc[4]; }
        if (desc[1] == 8) { v->first_slice = 0; v->slices = 1; }                                  // 3D: depth slices stay whole
    }
    if (!out) { d3d_release(&v->base); return S_FALSE; }
    *out = v;
    return S_OK;
}

// ---- input layouts -------------------------------------------------------------------------------------------------------
typedef struct { uint32_t reg, format, slot, offset, instanced, step; } layout_attr;
typedef struct { d3d_object base; layout_attr attr[32]; uint32_t n, id; } d3d_layout;
static WINAPI HRESULT layout_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11InputLayout); }

static const struct { uint32_t dxgi; MTLVertexFormat mtl; uint32_t bytes; } g_vertex_formats[] = {
    { 2, MTLVertexFormatFloat4, 16 }, { 3, MTLVertexFormatUInt4, 16 }, { 4, MTLVertexFormatInt4, 16 },
    { 6, MTLVertexFormatFloat3, 12 }, { 7, MTLVertexFormatUInt3, 12 }, { 8, MTLVertexFormatInt3, 12 },
    { 10, MTLVertexFormatHalf4, 8 }, { 11, MTLVertexFormatUShort4Normalized, 8 }, { 12, MTLVertexFormatUShort4, 8 },
    { 13, MTLVertexFormatShort4Normalized, 8 }, { 14, MTLVertexFormatShort4, 8 }, { 16, MTLVertexFormatFloat2, 8 },
    { 17, MTLVertexFormatUInt2, 8 }, { 18, MTLVertexFormatInt2, 8 }, { 24, MTLVertexFormatUInt1010102Normalized, 4 },
    { 26, MTLVertexFormatFloatRG11B10, 4 }, { 28, MTLVertexFormatUChar4Normalized, 4 }, { 30, MTLVertexFormatUChar4, 4 },
    { 31, MTLVertexFormatChar4Normalized, 4 }, { 32, MTLVertexFormatChar4, 4 }, { 34, MTLVertexFormatHalf2, 4 },
    { 35, MTLVertexFormatUShort2Normalized, 4 }, { 36, MTLVertexFormatUShort2, 4 }, { 37, MTLVertexFormatShort2Normalized, 4 },
    { 38, MTLVertexFormatShort2, 4 }, { 41, MTLVertexFormatFloat, 4 }, { 42, MTLVertexFormatUInt, 4 }, { 43, MTLVertexFormatInt, 4 },
    { 49, MTLVertexFormatUChar2Normalized, 2 }, { 54, MTLVertexFormatHalf, 2 }, { 56, MTLVertexFormatUShortNormalized, 2 },
    { 61, MTLVertexFormatUCharNormalized, 1 }, { 87, MTLVertexFormatUChar4Normalized_BGRA, 4 },
};
static MTLVertexFormat vertex_format(uint32_t dxgi, uint32_t *bytes) {
    for (size_t i = 0; i < sizeof g_vertex_formats / sizeof g_vertex_formats[0]; i++)
        if (g_vertex_formats[i].dxgi == dxgi) { if (bytes) *bytes = g_vertex_formats[i].bytes; return g_vertex_formats[i].mtl; }
    if (bytes) *bytes = 0;
    return MTLVertexFormatInvalid;
}

// D3D11_INPUT_ELEMENT_DESC: semantic name, semantic index, format, slot, offset, classification, step rate.
typedef struct { const char *semantic; uint32_t index, format, slot, offset, classification, step; } INPUT_ELEMENT;
static WINAPI HRESULT device_create_layout(void *this, const INPUT_ELEMENT *els, uint32_t n, const void *code, size_t size, void **out) {
    dxbc_element sig[32];
    int nsig = dxbc_input_signature(code, size, sig, 32);
    d3d_layout *l = d3d_alloc(sizeof *l, K_LAYOUT, g_layout_vtbl, this);
    uint32_t next_offset[16] = { 0 };
    for (uint32_t i = 0; i < n && l->n < 32; i++) {
        uint32_t bytes;
        if (vertex_format(els[i].format, &bytes) == MTLVertexFormatInvalid) {
            vx_log("Direct3D: vertex format %u is not supported yet", els[i].format);
            d3d_release(&l->base);
            return E_INVALIDARG;
        }
        uint32_t slot = els[i].slot < 16 ? els[i].slot : 0;
        uint32_t offset = els[i].offset == 0xffffffffu ? next_offset[slot] : els[i].offset;   // D3D11_APPEND_ALIGNED_ELEMENT
        next_offset[slot] = offset + bytes;
        for (int k = 0; k < nsig; k++) {
            if (strcasecmp(sig[k].semantic, els[i].semantic) || sig[k].index != els[i].index || sig[k].sysval) continue;
            l->attr[l->n++] = (layout_attr){ sig[k].reg, els[i].format, slot, offset, els[i].classification == 1, els[i].step };
            break;
        }
    }
    l->id = __sync_fetch_and_add(&g_next_id, 1);
    if (!out) { d3d_release(&l->base); return S_FALSE; }
    *out = l;
    return S_OK;
}

// ---- states ------------------------------------------------------------------------------------------------------------------
// D3D11_BLEND_DESC: alpha to coverage, independent blend, then eight render targets: enable, source, destination,
// operation, alpha source, alpha destination, alpha operation, write mask.
typedef struct { d3d_object base; uint32_t desc[66]; uint32_t id; } d3d_blend;
typedef struct { d3d_object base; uint32_t desc[13]; void *mtl; } d3d_depth;      // D3D11_DEPTH_STENCIL_DESC
typedef struct { d3d_object base; uint32_t desc[10]; } d3d_raster;                // D3D11_RASTERIZER_DESC
typedef struct { d3d_object base; uint32_t desc[13]; void *mtl; } d3d_sampler;    // D3D11_SAMPLER_DESC
static WINAPI HRESULT blend_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11BlendState); }
static WINAPI HRESULT depth_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11DepthStencilState); }
static WINAPI HRESULT raster_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11RasterizerState); }
static WINAPI HRESULT sampler_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11SamplerState); }
static WINAPI void blend_get_desc(void *this, uint32_t *d) { memcpy(d, ((d3d_blend *)this)->desc, sizeof ((d3d_blend *)this)->desc); }
static WINAPI void depth_get_desc(void *this, uint32_t *d) { memcpy(d, ((d3d_depth *)this)->desc, sizeof ((d3d_depth *)this)->desc); }
static WINAPI void raster_get_desc(void *this, uint32_t *d) { memcpy(d, ((d3d_raster *)this)->desc, sizeof ((d3d_raster *)this)->desc); }
static WINAPI void sampler_get_desc(void *this, uint32_t *d) { memcpy(d, ((d3d_sampler *)this)->desc, sizeof ((d3d_sampler *)this)->desc); }
static void depth_destroy(d3d_object *o) { if (((d3d_depth *)o)->mtl) CFBridgingRelease(((d3d_depth *)o)->mtl); }
static void sampler_destroy(d3d_object *o) { if (((d3d_sampler *)o)->mtl) CFBridgingRelease(((d3d_sampler *)o)->mtl); }

static WINAPI HRESULT device_create_blend(void *this, const uint32_t *desc, void **out) {
    d3d_blend *b = d3d_alloc(sizeof *b, K_STATE, g_blend_vtbl, this);
    memcpy(b->desc, desc, sizeof b->desc);
    b->id = __sync_fetch_and_add(&g_next_id, 1);
    *out = b;
    return S_OK;
}
static MTLCompareFunction compare_function(uint32_t f) {
    static const MTLCompareFunction map[9] = { MTLCompareFunctionAlways, MTLCompareFunctionNever, MTLCompareFunctionLess,
        MTLCompareFunctionEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
        MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways };
    return f < 9 ? map[f] : MTLCompareFunctionAlways;
}
static MTLStencilOperation stencil_op(uint32_t op) {
    static const MTLStencilOperation map[9] = { MTLStencilOperationKeep, MTLStencilOperationKeep, MTLStencilOperationZero,
        MTLStencilOperationReplace, MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp, MTLStencilOperationInvert,
        MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap };
    return op < 9 ? map[op] : MTLStencilOperationKeep;
}
static void *make_depth_state(id<MTLDevice> dev, const uint32_t *d) {
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = d[0] ? compare_function(d[2]) : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = d[0] && d[1];
    if (d[3]) {                                             // stencil: masks in d[4] (two bytes), then front and back faces
        uint8_t read_mask = ((const uint8_t *)&d[4])[0], write_mask = ((const uint8_t *)&d[4])[1];
        for (int face = 0; face < 2; face++) {
            const uint32_t *f = d + 5 + face * 4;           // fail, depth fail, pass, function
            MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
            sd.stencilFailureOperation = stencil_op(f[0]);
            sd.depthFailureOperation = stencil_op(f[1]);
            sd.depthStencilPassOperation = stencil_op(f[2]);
            sd.stencilCompareFunction = compare_function(f[3]);
            sd.readMask = read_mask; sd.writeMask = write_mask;
            if (face) dd.backFaceStencil = sd; else dd.frontFaceStencil = sd;
        }
    }
    return (void *)CFBridgingRetain([dev newDepthStencilStateWithDescriptor:dd]);
}
static WINAPI HRESULT device_create_depth(void *this, const uint32_t *desc, void **out) {
    d3d_depth *s = d3d_alloc(sizeof *s, K_STATE, g_depth_vtbl, this);
    memcpy(s->desc, desc, sizeof s->desc);
    s->mtl = make_depth_state((__bridge id<MTLDevice>)((d3d_device *)this)->mtl, desc);
    s->base.destroy = depth_destroy;
    *out = s;
    return S_OK;
}
static WINAPI HRESULT device_create_raster(void *this, const uint32_t *desc, void **out) {
    d3d_raster *s = d3d_alloc(sizeof *s, K_STATE, g_raster_vtbl, this);
    memcpy(s->desc, desc, sizeof s->desc);
    *out = s;
    return S_OK;
}
static MTLSamplerAddressMode address_mode(uint32_t m) {
    switch (m) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}
// D3D11_SAMPLER_DESC: filter, address U/V/W, mip bias, max anisotropy, comparison, border color, min/max LOD.
static void *make_sampler(id<MTLDevice> dev, const uint32_t *d) {
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    uint32_t filter = d[0];
    sd.mipFilter = filter & 0x1 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    sd.magFilter = filter & 0x4 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sd.minFilter = filter & 0x10 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    static int aniso_max = -1;                         // VXRT_MAX_ANISOTROPY=4: a cap (a quality setting)
    if (aniso_max < 0) aniso_max = getenv("VXRT_MAX_ANISOTROPY") ? MAX(atoi(getenv("VXRT_MAX_ANISOTROPY")), 1) : 16;
    if ((filter & 0x7f) == 0x55) sd.maxAnisotropy = d[5] ? MIN(d[5], (uint32_t)aniso_max) : 1;
    sd.sAddressMode = address_mode(d[1]); sd.tAddressMode = address_mode(d[2]); sd.rAddressMode = address_mode(d[3]);
    if (filter & 0x80) sd.compareFunction = compare_function(d[6]);
    const float *border = (const float *)&d[7];
    sd.borderColor = border[3] == 0 ? MTLSamplerBorderColorTransparentBlack : border[0] == 0 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite;
    float min_lod = *(const float *)&d[11], max_lod = *(const float *)&d[12];
    sd.lodMinClamp = min_lod < 0 ? 0 : min_lod;
    sd.lodMaxClamp = max_lod > 1000 ? FLT_MAX : max_lod;
    sd.supportArgumentBuffers = NO;
    return (void *)CFBridgingRetain([dev newSamplerStateWithDescriptor:sd]);
}
static WINAPI HRESULT device_create_sampler(void *this, const uint32_t *desc, void **out) {
    d3d_sampler *s = d3d_alloc(sizeof *s, K_STATE, g_sampler_vtbl, this);
    memcpy(s->desc, desc, sizeof s->desc);
    s->mtl = make_sampler((__bridge id<MTLDevice>)((d3d_device *)this)->mtl, desc);
    s->base.destroy = sampler_destroy;
    *out = s;
    return S_OK;
}

// ---- the context's bindings -------------------------------------------------------------------------------------------------
enum { STAGE_VS, STAGE_PS, STAGE_CS, STAGE_GS, STAGES };
#define VB_BASE 14                    // vertex buffer slot s is Metal buffer 14 + s (constant buffers take 0..13)
typedef struct d3d_draw_state {
    d3d_layout *layout;
    d3d_buffer *vb[16]; uint32_t stride[16], offset[16];
    d3d_buffer *ib; uint32_t ib_format, ib_offset, topology;
    d3d_shader *shader[STAGES];
    d3d_buffer *cb[STAGES][14]; uint32_t cb_offset[STAGES][14];
    d3d_view *srv[STAGES][32];
    d3d_sampler *smp[STAGES][16];
    d3d_view *uav[STAGES][8];                  // pixel shaders (with render targets) and compute shaders
    d3d_blend *blend; float blend_factor[4];
    d3d_depth *depth; uint32_t stencil_ref;
    d3d_raster *raster;
    RECT scissor[16];
    uint32_t nscissors;
    void *gs_vertices;                         // drawing a geometry kernel's triangles: its output (id<MTLBuffer>), else NULL
    void *gs_counts;                           // ... and how many triangles each primitive made (id<MTLBuffer>)
    void *gs_scratch[3];                       // the geometry path's buffers, kept from draw to draw (id<MTLBuffer>)
    d3d_buffer *so[4];                         // stream output targets
    void *dummy_depth;                         // id<MTLTexture>: for shaders writing depth with no depth target bound
    void *pass_depth;                          // the dummy depth the open render encoder has (or NULL)
    struct pso_key { uint32_t vs, ps, layout, blend, color[8], depth, stride[16]; } last_key;   // the last pipeline state used
    void *last_pso;
    // What is set on the open render encoder (cleared when one is made), so a draw sends Metal only what
    // changed: every Metal call costs under Rosetta, and games rebind much the same state draw after draw.
    struct {
        void *pso, *dss; uint32_t stencil_ref; float blend[4]; uint32_t raster[7];
        uint32_t nvp; MTLViewport vp[16]; MTLScissorRect sc[16];
        void *buf[2][64]; NSUInteger off[2][64]; void *tex[2][80]; void *smp[2][16];
    } bound;
} d3d_draw_state;

static d3d_draw_state *state_of(d3d_context *c) {
    if (!c->draw) c->draw = calloc(1, sizeof *c->draw);
    return c->draw;
}
// Bindings keep a reference, as Direct3D does.
static void bind(void *slot_ptr, void *obj) {
    d3d_object **slot = slot_ptr;
    if (obj) d3d_addref(obj);
    if (*slot) d3d_release(*slot);
    *slot = obj;
}
void d3d_draw_release(d3d_context *c) {
    d3d_draw_state *s = c->draw;
    if (!s) return;
    bind(&s->layout, NULL); bind(&s->ib, NULL); bind(&s->blend, NULL); bind(&s->depth, NULL); bind(&s->raster, NULL);
    for (int i = 0; i < 16; i++) bind(&s->vb[i], NULL);
    for (int st = 0; st < STAGES; st++) {
        bind(&s->shader[st], NULL);
        for (int i = 0; i < 14; i++) bind(&s->cb[st][i], NULL);
        for (int i = 0; i < 32; i++) bind(&s->srv[st][i], NULL);
        for (int i = 0; i < 16; i++) bind(&s->smp[st][i], NULL);
        for (int i = 0; i < 8; i++) bind(&s->uav[st][i], NULL);
    }
    for (int i = 0; i < 4; i++) bind(&s->so[i], NULL);
    for (int i = 0; i < 3; i++) if (s->gs_scratch[i]) CFRelease(s->gs_scratch[i]);
    free(s);
    c->draw = NULL;
}

static WINAPI void ia_set_layout(void *this, void *l) { bind(&state_of(this)->layout, l); }
static WINAPI void ia_set_vertex_buffers(void *this, uint32_t start, uint32_t n, d3d_buffer *const *bufs, const uint32_t *strides, const uint32_t *offsets) {
    d3d_draw_state *s = state_of(this);
    for (uint32_t i = 0; i < n && start + i < 16; i++) {
        bind(&s->vb[start + i], bufs ? bufs[i] : NULL);
        s->stride[start + i] = strides ? strides[i] : 0;
        s->offset[start + i] = offsets ? offsets[i] : 0;
    }
}
static WINAPI void ia_set_index_buffer(void *this, d3d_buffer *b, uint32_t format, uint32_t offset) {
    d3d_draw_state *s = state_of(this);
    bind(&s->ib, b); s->ib_format = format; s->ib_offset = offset;
}
static WINAPI void ia_set_topology(void *this, uint32_t t) { state_of(this)->topology = t; }
static WINAPI void vs_set_shader(void *this, d3d_shader *sh, void *const *ci, uint32_t n) { (void)ci; (void)n; bind(&state_of(this)->shader[STAGE_VS], sh); }
static WINAPI void ps_set_shader(void *this, d3d_shader *sh, void *const *ci, uint32_t n) { (void)ci; (void)n; bind(&state_of(this)->shader[STAGE_PS], sh); }
static void set_cbs(d3d_context *c, int st, uint32_t start, uint32_t n, d3d_buffer *const *bufs) {
    d3d_draw_state *s = state_of(c);
    for (uint32_t i = 0; i < n && start + i < 14; i++) { bind(&s->cb[st][start + i], bufs ? bufs[i] : NULL); s->cb_offset[st][start + i] = 0; }
}
static WINAPI void vs_set_cbs(void *this, uint32_t start, uint32_t n, d3d_buffer *const *b) { set_cbs(this, STAGE_VS, start, n, b); }
static WINAPI void ps_set_cbs(void *this, uint32_t start, uint32_t n, d3d_buffer *const *b) { set_cbs(this, STAGE_PS, start, n, b); }
static void set_srvs(d3d_context *c, int st, uint32_t start, uint32_t n, d3d_view *const *v) {
    d3d_draw_state *s = state_of(c);
    for (uint32_t i = 0; i < n && start + i < 32; i++) bind(&s->srv[st][start + i], v ? v[i] : NULL);
}
static WINAPI void vs_set_srvs(void *this, uint32_t start, uint32_t n, d3d_view *const *v) { set_srvs(this, STAGE_VS, start, n, v); }
static WINAPI void ps_set_srvs(void *this, uint32_t start, uint32_t n, d3d_view *const *v) { set_srvs(this, STAGE_PS, start, n, v); }
static void set_samplers(d3d_context *c, int st, uint32_t start, uint32_t n, d3d_sampler *const *v) {
    d3d_draw_state *s = state_of(c);
    for (uint32_t i = 0; i < n && start + i < 16; i++) bind(&s->smp[st][start + i], v ? v[i] : NULL);
}
static WINAPI void vs_set_samplers(void *this, uint32_t start, uint32_t n, d3d_sampler *const *v) { set_samplers(this, STAGE_VS, start, n, v); }
static WINAPI void ps_set_samplers(void *this, uint32_t start, uint32_t n, d3d_sampler *const *v) { set_samplers(this, STAGE_PS, start, n, v); }
static WINAPI void gs_set_shader(void *this, d3d_shader *sh, void *const *ci, uint32_t n) { (void)ci; (void)n; bind(&state_of(this)->shader[STAGE_GS], sh); }
static WINAPI void gs_set_cbs(void *this, uint32_t start, uint32_t n, d3d_buffer *const *b) { set_cbs(this, STAGE_GS, start, n, b); }
static WINAPI void gs_set_srvs(void *this, uint32_t start, uint32_t n, d3d_view *const *v) { set_srvs(this, STAGE_GS, start, n, v); }
static WINAPI void gs_set_samplers(void *this, uint32_t start, uint32_t n, d3d_sampler *const *v) { set_samplers(this, STAGE_GS, start, n, v); }
static WINAPI void cs_set_shader(void *this, d3d_shader *sh, void *const *ci, uint32_t n) { (void)ci; (void)n; bind(&state_of(this)->shader[STAGE_CS], sh); }
static WINAPI void cs_set_cbs(void *this, uint32_t start, uint32_t n, d3d_buffer *const *b) { set_cbs(this, STAGE_CS, start, n, b); }
static WINAPI void cs_set_srvs(void *this, uint32_t start, uint32_t n, d3d_view *const *v) { set_srvs(this, STAGE_CS, start, n, v); }
static WINAPI void cs_set_samplers(void *this, uint32_t start, uint32_t n, d3d_sampler *const *v) { set_samplers(this, STAGE_CS, start, n, v); }
static WINAPI void cs_set_uavs(void *this, uint32_t start, uint32_t n, d3d_view *const *v, const uint32_t *counts) {
    d3d_draw_state *s = state_of(this);
    for (uint32_t i = 0; i < n && start + i < 8; i++) {
        bind(&s->uav[STAGE_CS][start + i], v ? v[i] : NULL);
        if (v && counts) set_counter(v[i], counts[i]);
    }
}
typedef void (WINAPI *set_targets_fn)(void *, uint32_t, void *const *, void *);
static WINAPI void om_set_targets_and_uavs(void *this, uint32_t nrtv, void *const *rtvs, void *dsv, uint32_t start, uint32_t nuav, d3d_view *const *uavs, const uint32_t *counts) {
    if (nrtv != 0xffffffffu) {                                // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL
        set_targets_fn set = (set_targets_fn)self_of(this)->vtbl[33];   // OMSetRenderTargets
        set(this, nrtv, rtvs, dsv);
    }
    if (nuav != 0xffffffffu) {                                // D3D11_KEEP_UNORDERED_ACCESS_VIEWS
        d3d_draw_state *s = state_of(this);
        for (uint32_t i = 0; i < 8; i++) bind(&s->uav[STAGE_PS][i], i >= start && i < start + nuav && uavs ? uavs[i - start] : NULL);
        for (uint32_t i = 0; uavs && counts && i < nuav; i++) set_counter(uavs[i], counts[i]);
    }
}
static WINAPI void om_set_blend(void *this, d3d_blend *b, const float *factor, uint32_t mask) {
    (void)mask;
    d3d_draw_state *s = state_of(this);
    bind(&s->blend, b);
    for (int i = 0; i < 4; i++) s->blend_factor[i] = factor ? factor[i] : 1.0f;
}
static WINAPI void om_set_depth(void *this, d3d_depth *d, uint32_t ref) { d3d_draw_state *s = state_of(this); bind(&s->depth, d); s->stencil_ref = ref; }
static WINAPI void rs_set_state(void *this, d3d_raster *r) { bind(&state_of(this)->raster, r); }
static WINAPI void rs_set_scissors(void *this, uint32_t n, const RECT *r) {
    if (n > 16) n = 16;
    if (n && r) memcpy(state_of(this)->scissor, r, sizeof *r * n);
    state_of(this)->nscissors = n;
}

// ---- pipeline states ---------------------------------------------------------------------------------------------------------------
static MTLBlendFactor blend_factor(uint32_t f) {
    switch (f) {
    case 1: return MTLBlendFactorZero;               case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;        case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;        case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;   case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;   case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;        case 15: return MTLBlendFactorOneMinusBlendColor;
    case 16: return MTLBlendFactorSource1Color;      case 17: return MTLBlendFactorOneMinusSource1Color;
    case 18: return MTLBlendFactorSource1Alpha;      case 19: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation blend_op(uint32_t op) {
    switch (op) {
    case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;      case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

// Pipeline states are cached by everything that goes into them, in a hash table keyed by plain numbers (a
// string key cost more per draw than the draw itself); the context also remembers the last one, which most
// draws reuse. Shader, layout and blend ids are never reused, so entries never go stale.
typedef struct pso_key pso_key;
typedef struct { pso_key key; void *pso; } pso_entry;
static pso_entry *g_psos;
static size_t g_pso_cap, g_pso_count;
static pthread_mutex_t g_pso_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t pso_hash(const pso_key *k) {
    const uint32_t *w = (const uint32_t *)k;
    uint64_t h = 0x9e3779b97f4a7c15ull;
    for (size_t i = 0; i < sizeof *k / 4; i++) h = (h ^ w[i]) * 0xff51afd7ed558ccdull;
    return (size_t)(h ^ h >> 32);
}
static void *pso_find(const pso_key *k) {           // with g_pso_lock held
    if (!g_pso_cap) return NULL;
    for (size_t i = pso_hash(k) & (g_pso_cap - 1); g_psos[i].pso; i = (i + 1) & (g_pso_cap - 1))
        if (!memcmp(&g_psos[i].key, k, sizeof *k)) return g_psos[i].pso;
    return NULL;
}
static void pso_add(const pso_key *k, void *pso) {  // with g_pso_lock held; pso retained
    if (2 * (g_pso_count + 1) > g_pso_cap) {
        size_t cap = g_pso_cap ? 2 * g_pso_cap : 256;
        pso_entry *old = g_psos, *grown = calloc(cap, sizeof *grown);
        for (size_t j = 0; j < g_pso_cap; j++) if (old[j].pso) {
            size_t i = pso_hash(&old[j].key) & (cap - 1);
            while (grown[i].pso) i = (i + 1) & (cap - 1);
            grown[i] = old[j];
        }
        g_psos = grown; g_pso_cap = cap;
        free(old);
    }
    size_t i = pso_hash(k) & (g_pso_cap - 1);
    while (g_psos[i].pso) i = (i + 1) & (g_pso_cap - 1);
    g_psos[i] = (pso_entry){ *k, pso };
    g_pso_count++;
}
// The geometry path's other functions, compiled the first time a draw needs them: a geometry shader's
// pass-through vertex function, a vertex shader as a kernel (both kept in `variant`).
static id<MTLFunction> shader_variant(d3d_device *dev, d3d_shader *s, const char *msl, const char *what) {
    pthread_mutex_lock(&g_variant_lock);
    if (!s->variant_tried && msl) {
        s->variant_tried = 1;
        NSError *err = nil;
        id<MTLLibrary> lib = [(__bridge id<MTLDevice>)dev->mtl newLibraryWithSource:@(msl) options:nil error:&err];
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
        if (fn) s->variant = (void *)CFBridgingRetain(fn);
        else vx_log("Direct3D: %s of shader %08x did not compile: %s", what, s->hash, err.localizedDescription.UTF8String);
    }
    pthread_mutex_unlock(&g_variant_lock);
    return (__bridge id<MTLFunction>)s->variant;
}
static id<MTLFunction> gs_passthrough(d3d_device *dev, d3d_shader *gs) { return shader_variant(dev, gs, gs->info.msl_passthrough, "pass-through"); }

static char g_pso_failed;
#define PSO_FAILED ((void *)&g_pso_failed)
// Metal converts vertex data only within a kind (floats, signed or unsigned integers); Direct3D hands an integer
// format's bits to whichever integer type the shader declares. A signed format read as uint (or the reverse) is
// fetched as its counterpart (the same bits, but for negative values' sign extension).
static MTLVertexFormat vertex_format_for(MTLVertexFormat f, const char *msl, uint32_t reg) {
    static const MTLVertexFormat pairs[][2] = {       // signed, unsigned
        { MTLVertexFormatChar, MTLVertexFormatUChar }, { MTLVertexFormatChar2, MTLVertexFormatUChar2 }, { MTLVertexFormatChar4, MTLVertexFormatUChar4 },
        { MTLVertexFormatShort, MTLVertexFormatUShort }, { MTLVertexFormatShort2, MTLVertexFormatUShort2 }, { MTLVertexFormatShort4, MTLVertexFormatUShort4 },
        { MTLVertexFormatInt, MTLVertexFormatUInt }, { MTLVertexFormatInt2, MTLVertexFormatUInt2 }, { MTLVertexFormatInt3, MTLVertexFormatUInt3 },
        { MTLVertexFormatInt4, MTLVertexFormatUInt4 },
    };
    if (!msl) return f;
    char pattern[64];
    snprintf(pattern, sizeof pattern, " v%u [[attribute(%u)]];", reg, reg);
    const char *at = strstr(msl, pattern);
    if (!at || at - msl < 5) return f;
    int wants_uint = !strncmp(at - 5, "uint", 4), wants_int = !wants_uint && !strncmp(at - 4, "int", 3);
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        if (wants_uint && f == pairs[i][0]) return pairs[i][1];
        if (wants_int && f == pairs[i][1]) return pairs[i][0];
    }
    // 32-bit floats read as integers: their bits, as Direct3D hands them over
    static const MTLVertexFormat floats[][3] = {    // float, uint, int
        { MTLVertexFormatFloat, MTLVertexFormatUInt, MTLVertexFormatInt }, { MTLVertexFormatFloat2, MTLVertexFormatUInt2, MTLVertexFormatInt2 },
        { MTLVertexFormatFloat3, MTLVertexFormatUInt3, MTLVertexFormatInt3 }, { MTLVertexFormatFloat4, MTLVertexFormatUInt4, MTLVertexFormatInt4 },
    };
    for (size_t i = 0; i < sizeof floats / sizeof floats[0]; i++)
        if (f == floats[i][0] && (wants_uint || wants_int)) return floats[i][wants_uint ? 1 : 2];
    return f;
}

// unbound: targets of the open render pass that this draw has no render target view for (it keeps the pass, which
// has them: their writes are masked off).
static id<MTLRenderPipelineState> pipeline(d3d_context *c, d3d_draw_state *s, id<MTLTexture> const *colors, int ncolors, id<MTLTexture> depth, uint32_t unbound) {
    d3d_shader *vs = s->shader[STAGE_VS], *ps = s->shader[STAGE_PS], *gs = s->gs_vertices ? s->shader[STAGE_GS] : NULL;
    pso_key key;
    memset(&key, 0, sizeof key);
    key.vs = gs ? gs->id | 0x80000000u : vs->id; key.ps = ps ? ps->id : 0; key.layout = s->layout && !gs ? s->layout->id : 0; key.blend = s->blend ? s->blend->id : 0;
    for (int i = 0; i < ncolors; i++) if (colors[i]) key.color[i] = (uint32_t)colors[i].pixelFormat | (uint32_t)colors[i].sampleCount << 16 | (unbound >> i & 1u) << 31;
    if (depth) key.depth = (uint32_t)depth.pixelFormat | (uint32_t)depth.sampleCount << 16;
    if (s->layout && !gs) for (uint32_t i = 0; i < s->layout->n; i++) key.stride[s->layout->attr[i].slot & 15] = s->stride[s->layout->attr[i].slot & 15] | 0x80000000u;
    if (s->last_pso && !memcmp(&key, &s->last_key, sizeof key)) return (__bridge id<MTLRenderPipelineState>)s->last_pso;
    pthread_mutex_lock(&g_pso_lock);
    void *found = pso_find(&key);
    pthread_mutex_unlock(&g_pso_lock);
    if (found == PSO_FAILED) return nil;               // (Metal refused it before: not tried again every draw)
    if (found) { s->last_key = key; s->last_pso = found; return (__bridge id<MTLRenderPipelineState>)found; }
    id<MTLRenderPipelineState> ps_state;

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = gs ? gs_passthrough(c->base.dev, gs) : shader_function(vs);
    pd.fragmentFunction = shader_function(ps);
    if (!pd.vertexFunction || (ps && !pd.fragmentFunction)) return nil;   // a shader that did not compile
    // SRC1 blend factors on target 0: the shader's second output feeds them (or, with none, Direct3D leaves the
    // result undefined: the first output stands in).
    int dual = 0;
    if (s->blend && ncolors > 0) for (int k = 1; k <= 6 && !dual; k++) if (k != 3) dual = s->blend->desc[2 + k] >= 16 && s->blend->desc[2 + k] <= 19;
    if (dual && ps && shader_function(ps)) {
        id<MTLFunction> f = dual_source_function(c->base.dev, ps);
        if (f) pd.fragmentFunction = f; else dual = 2;
    }
    if (s->layout && s->layout->n && !gs) {
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        for (uint32_t i = 0; i < s->layout->n; i++) {
            const layout_attr *a = &s->layout->attr[i];
            vd.attributes[a->reg].format = vertex_format_for(vertex_format(a->format, NULL), vs->info.msl, a->reg);
            vd.attributes[a->reg].offset = a->offset;
            vd.attributes[a->reg].bufferIndex = VB_BASE + a->slot;
            MTLVertexBufferLayoutDescriptor *bl = vd.layouts[VB_BASE + a->slot];
            uint32_t stride = s->stride[a->slot];
            if (!stride) { bl.stepFunction = MTLVertexStepFunctionConstant; bl.stepRate = 0; bl.stride = 16; }
            else {
                bl.stride = stride;
                bl.stepFunction = a->instanced ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
                bl.stepRate = a->instanced ? (a->step ? a->step : 1) : 1;
            }
        }
        pd.vertexDescriptor = vd;
    }
    for (int i = 0; i < ncolors; i++) {
        if (!colors[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
        ca.pixelFormat = colors[i].pixelFormat;
        pd.rasterSampleCount = colors[i].sampleCount;
        if (s->blend) {
            const uint32_t *rt = s->blend->desc + 2 + 8 * (s->blend->desc[1] ? i : 0);   // independent blend, or target 0's
            ca.blendingEnabled = rt[0] != 0;
            uint32_t f[7];
            memcpy(f, rt, sizeof f);
            if (dual == 2) for (int k = 1; k <= 5; k++) if (k != 3 && f[k] >= 16 && f[k] <= 19) f[k] -= 13;   // SRC1_* -> SRC_*
            ca.sourceRGBBlendFactor = blend_factor(f[1]); ca.destinationRGBBlendFactor = blend_factor(f[2]); ca.rgbBlendOperation = blend_op(f[3]);
            ca.sourceAlphaBlendFactor = blend_factor(f[4]); ca.destinationAlphaBlendFactor = blend_factor(f[5]); ca.alphaBlendOperation = blend_op(f[6]);
            uint8_t m = (uint8_t)rt[7];
            ca.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
            pd.alphaToCoverageEnabled = s->blend->desc[0] != 0;
        }
        if (unbound >> i & 1u) { ca.writeMask = MTLColorWriteMaskNone; ca.blendingEnabled = NO; }
    }
    if (depth) {
        pd.depthAttachmentPixelFormat = depth.pixelFormat;
        if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) pd.stencilAttachmentPixelFormat = depth.pixelFormat;
        if (!ncolors) pd.rasterSampleCount = depth.sampleCount;
    }
    NSError *err = nil;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    ps_state = [(__bridge id<MTLDevice>)c->base.dev->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    if (vx_trace) {                                    // VXRT_TRACE: every 200 pipeline states, the time spent making them
        static uint64_t n, ns;
        __atomic_add_fetch(&ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, __ATOMIC_RELAXED);
        if (__atomic_add_fetch(&n, 1, __ATOMIC_RELAXED) % 200 == 0) vx_log("Direct3D: %llu pipeline states: %llu ms", (unsigned long long)n, (unsigned long long)(ns / 1000000));
    }
    if (!ps_state) {
        vx_log("Direct3D: pipeline state failed: %s", err.localizedDescription.UTF8String);
        pthread_mutex_lock(&g_pso_lock);
        if (!pso_find(&key)) pso_add(&key, PSO_FAILED);
        pthread_mutex_unlock(&g_pso_lock);
        return nil;
    }
    pthread_mutex_lock(&g_pso_lock);
    void *kept = pso_find(&key);                       // another thread may have made the same one meanwhile
    if (!kept) pso_add(&key, kept = (void *)CFBridgingRetain(ps_state));
    pthread_mutex_unlock(&g_pso_lock);
    s->last_key = key; s->last_pso = kept;
    return (__bridge id<MTLRenderPipelineState>)kept;
}

// ---- drawing ----------------------------------------------------------------------------------------------------------------------
static id<MTLTexture> view_texture(d3d_view *v) {
    if (!v || v->resource->kind != K_TEXTURE) return nil;
    return v->base.kind == K_RTV ? (__bridge id<MTLTexture>)d3d_rtv_texture(v) : (__bridge id<MTLTexture>)((d3d_texture *)v->resource)->mtl;
}

// What a shader reads or writes through a view: the texture itself, or a Metal texture view of it when the view
// changes the format (typeless resources, sRGB), the mips or slices, or the type (an array slice as 2D, cube
// faces); a typed view of a buffer is a texture buffer over it. Views are cached while the resource's Metal object
// stays the same (buffers are replaced when discarded).
static MTLTextureType view_type(uint32_t dim, MTLTextureType base) {
    switch (dim) {
    case 2: return MTLTextureType1D;           case 3: return MTLTextureType1DArray;
    case 4: return MTLTextureType2D;           case 5: return MTLTextureType2DArray;
    case 6: return MTLTextureType2DMultisample; case 7: return MTLTextureType2DMultisampleArray;
    case 8: return MTLTextureType3D;           case 9: return MTLTextureTypeCube;
    case 10: return MTLTextureTypeCubeArray;   default: return base;
    }
}
static id<MTLTexture> cache_view(d3d_view *v, id<MTLTexture> t, void *src) {
    if (v->mtl_view) CFRelease(v->mtl_view);
    v->mtl_view = (void *)CFBridgingRetain(t);
    v->mtl_src = src;
    return t;
}
static id<MTLTexture> buffer_texture(d3d_view *v) {
    d3d_buffer *b = (d3d_buffer *)v->resource;
    if (v->mtl_view && v->mtl_src == b->mtl) return (__bridge id<MTLTexture>)v->mtl_view;
    id<MTLBuffer> mb = (__bridge id<MTLBuffer>)b->mtl;
    uint32_t bpp = 0;
    MTLPixelFormat f = d3d_mtl_format(v->format, &bpp);
    if (f == MTLPixelFormatInvalid || !bpp || !mb) return nil;
    NSUInteger off = (NSUInteger)v->first * bpp, n = v->count ? v->count : (mb.length - MIN(off, mb.length)) / bpp;
    if (!n || off + n * bpp > mb.length) return nil;
    NSUInteger align = [mb.device minimumTextureBufferAlignmentForPixelFormat:f];
    if (align && off % align) { vx_log("Direct3D: a buffer view at byte %lu is not aligned for Metal (%lu)", (unsigned long)off, (unsigned long)align); return nil; }
    MTLTextureUsage usage = MTLTextureUsageShaderRead | (v->base.kind == K_UAV ? MTLTextureUsageShaderWrite : 0);
    if (v->base.kind == K_UAV && (v->format == 42 || v->format == 43)) usage |= MTLTextureUsageShaderAtomic;
    MTLTextureDescriptor *td = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:f width:n resourceOptions:mb.resourceOptions usage:usage];
    NSUInteger row = align ? (n * bpp + align - 1) / align * align : n * bpp;
    id<MTLTexture> t = [mb newTextureWithDescriptor:td offset:off bytesPerRow:row];
    return t ? cache_view(v, t, b->mtl) : nil;
}
static id<MTLTexture> shader_texture(d3d_view *v) {
    if (!v) return nil;
    if (v->resource->kind == K_BUFFER) return buffer_texture(v);
    d3d_texture *tex = (d3d_texture *)v->resource;
    id<MTLTexture> base = (__bridge id<MTLTexture>)tex->mtl;
    if (!base) return nil;
    if (v->mtl_view && v->mtl_src == tex->mtl) return (__bridge id<MTLTexture>)v->mtl_view;
    MTLPixelFormat fmt = base.pixelFormat;
    int depth = fmt == MTLPixelFormatDepth32Float || fmt == MTLPixelFormatDepth16Unorm || fmt == MTLPixelFormatDepth32Float_Stencil8;
    MTLPixelFormat f = d3d_mtl_format(v->format, NULL);
    if (depth && f != MTLPixelFormatX32_Stencil8) f = MTLPixelFormatInvalid;   // depth reads as depth; stencil views read stencil
    if (f != MTLPixelFormatInvalid) fmt = f;
    MTLTextureType type = view_type(v->dim, base.textureType);
    NSUInteger levels = base.mipmapLevelCount, first = MIN((NSUInteger)v->mip, levels - 1);
    NSUInteger count = v->base.kind == K_UAV ? 1 : v->mips == 0 || v->mips == 0xffffffffu ? levels - first : MIN((NSUInteger)v->mips, levels - first);
    NSUInteger total = base.textureType == MTLTextureType3D ? 1 : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    NSUInteger sfirst = MIN((NSUInteger)v->first_slice, total - 1), scount = v->dim ? MIN((NSUInteger)v->slices, total - sfirst) : total;
    if (type == MTLTextureType2D || type == MTLTextureType1D || type == MTLTextureType2DMultisample || type == MTLTextureType3D) scount = 1;
    if (type == MTLTextureTypeCube) scount = 6;
    if (v->base.kind == K_UAV && v->dim == 0) { sfirst = 0; scount = total; }
    if (fmt == base.pixelFormat && type == base.textureType && first == 0 && count == levels && sfirst == 0 && scount == total)
        return cache_view(v, base, tex->mtl);                   // the texture itself (remembered: this runs every draw)
    id<MTLTexture> view = [base newTextureViewWithPixelFormat:fmt textureType:type levels:NSMakeRange(first, count) slices:NSMakeRange(sfirst, scount)];
    if (!view) {
        static int told;
        if (told++ < 20) vx_log("Direct3D: no %lu view of a %lu texture (usage %#lx): the texture itself is used", (unsigned long)fmt, (unsigned long)base.pixelFormat, (unsigned long)base.usage);
        return base;
    }
    return cache_view(v, view, tex->mtl);
}

// VXRT_GPU_PROFILE=1: the GPU time of each render pass and compute dispatch (Metal's timestamp counters at the start
// of a pass's vertex stage and the end of its fragment stage), summed by what the pass draws to and the first pixel
// (or compute) shader it runs (the hash VXRT_DUMP_SHADERS names its files by), the costliest every 4 seconds.
#define PROF_SAMPLES 4096                    // (Metal allows 32 KB of timestamps)
typedef struct { char sig[120]; } prof_pass;
static id<MTLCounterSampleBuffer> g_prof_buf;
static prof_pass g_prof_pass[PROF_SAMPLES / 2];
static uint32_t g_prof_next, g_prof_submitted;      // samples taken (mod PROF_SAMPLES), and handed to a command buffer
static int g_prof_on = -1;
static pthread_mutex_t g_prof_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { char sig[120]; double ns; uint32_t n; } prof_sum;
static prof_sum g_prof_sums[512];
static int g_prof_nsums;
static uint64_t g_prof_since;
static int prof_on(d3d_device *dev) {
    if (g_prof_on >= 0) return g_prof_on;
    g_prof_on = 0;
    if (!getenv("VXRT_GPU_PROFILE")) return 0;
    id<MTLDevice> d = (__bridge id<MTLDevice>)dev->mtl;
    if (![d supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) { vx_log("GPU profile: no stage-boundary counters"); return 0; }
    id<MTLCounterSet> ts = nil;
    for (id<MTLCounterSet> set in d.counterSets) if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) ts = set;
    if (!ts) { vx_log("GPU profile: no timestamp counters"); return 0; }
    MTLCounterSampleBufferDescriptor *bd = [MTLCounterSampleBufferDescriptor new];
    bd.counterSet = ts; bd.storageMode = MTLStorageModeShared; bd.sampleCount = PROF_SAMPLES;
    NSError *err = nil;
    g_prof_buf = [d newCounterSampleBufferWithDescriptor:bd error:&err];
    if (!g_prof_buf) { vx_log("GPU profile: %s", err.localizedDescription.UTF8String); return 0; }
    g_prof_on = 1;
    return 1;
}
static uint32_t prof_take(const char *sig) {          // two samples for a pass
    uint32_t i = g_prof_next;
    g_prof_next = (g_prof_next + 2) % PROF_SAMPLES;
    snprintf(g_prof_pass[i / 2].sig, sizeof g_prof_pass[i / 2].sig, "%s", sig);
    return i;
}
static int prof_note_shader(uint32_t sample, uint32_t hash) {    // the pass's first three pixel shaders: 0 when full
    char *s = g_prof_pass[sample / 2].sig, h[16];
    snprintf(h, sizeof h, " %08x", hash);
    if (strstr(s, h)) return 1;
    size_t l = strlen(s);
    snprintf(s + l, sizeof g_prof_pass[0].sig - l, "%s%s", strstr(s, " ps ") ? "" : " ps", h);
    int n = 0;                                         // (the shaders noted: the spaces after " ps")
    for (const char *p = strstr(s, " ps ") + 3; p; p = strchr(p + 1, ' ')) n++;
    return n < 3 && strlen(s) + 10 < sizeof g_prof_pass[0].sig;
}
static void prof_add(const char *sig, double ns) {
    for (int i = 0; i < g_prof_nsums; i++) if (!strcmp(g_prof_sums[i].sig, sig)) { g_prof_sums[i].ns += ns; g_prof_sums[i].n++; return; }
    if (g_prof_nsums < 512) { snprintf(g_prof_sums[g_prof_nsums].sig, sizeof g_prof_sums[0].sig, "%s", sig); g_prof_sums[g_prof_nsums].ns = ns; g_prof_sums[g_prof_nsums].n = 1; g_prof_nsums++; }
}
static int prof_by_ns(const void *a, const void *b) { double x = ((const prof_sum *)a)->ns, y = ((const prof_sum *)b)->ns; return x < y ? 1 : x > y ? -1 : 0; }
void d3d_gpu_profile_submit(d3d_device *dev, void *cmd_ptr) {
    if (g_prof_on <= 0 || !cmd_ptr) return;
    (void)dev;
    uint32_t first = g_prof_submitted, end = g_prof_next;
    g_prof_submitted = end;
    if (first == end) return;
    // (copies of the passes' names: the ring may be reused before the command buffer is done)
    uint32_t count = (end - first + PROF_SAMPLES) % PROF_SAMPLES;
    prof_pass *names = malloc((count / 2 + 1) * sizeof *names);
    for (uint32_t k = 0; k < count; k += 2) names[k / 2] = g_prof_pass[((first + k) % PROF_SAMPLES) / 2];
    [(__bridge id<MTLCommandBuffer>)cmd_ptr addCompletedHandler:^(id<MTLCommandBuffer> done) {
        (void)done;
        pthread_mutex_lock(&g_prof_lock);
        // A pass's time: how far it moved the end of the command buffer's work (the GPU starts a pass's vertices
        // while earlier passes still shade, so its own start-to-end span counts their time too).
        uint64_t last_end = 0;
        for (uint32_t k = 0; k < count; k += 2) {
            uint32_t s = (first + k) % PROF_SAMPLES;
            NSData *data = [g_prof_buf resolveCounterRange:NSMakeRange(s, 2)];
            if (data.length < 2 * sizeof(MTLCounterResultTimestamp)) continue;
            const MTLCounterResultTimestamp *t = data.bytes;
            if (t[0].timestamp == MTLCounterErrorValue || t[1].timestamp == MTLCounterErrorValue || t[1].timestamp < t[0].timestamp) continue;
            uint64_t from = MAX(t[0].timestamp, last_end);
            prof_add(names[k / 2].sig, t[1].timestamp > from ? (double)(t[1].timestamp - from) : 0);
            last_end = MAX(last_end, t[1].timestamp);
        }
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (!g_prof_since) g_prof_since = now;
        if (now - g_prof_since >= 4000000000ull) {
            double secs = (now - g_prof_since) / 1e9, total = 0;
            qsort(g_prof_sums, (size_t)g_prof_nsums, sizeof *g_prof_sums, prof_by_ns);
            for (int i = 0; i < g_prof_nsums; i++) total += g_prof_sums[i].ns;
            vx_log("GPU profile: %.1f ms of passes per second (timestamps assumed ns)", total / 1e6 / secs);
            for (int i = 0; i < g_prof_nsums && i < 16; i++)
                vx_log("   %6.1f ms/s %5.0f/s  %s", g_prof_sums[i].ns / 1e6 / secs, g_prof_sums[i].n / secs, g_prof_sums[i].sig);
            g_prof_nsums = 0; g_prof_since = now;
        }
        pthread_mutex_unlock(&g_prof_lock);
        free(names);
    }];
}

// VXRT_STATS: render passes and the memory traffic of their attachments (loaded at the start, stored at the end), every
// 2 seconds; at high resolutions this is much of a tile-based GPU's time.
static void pass_stats(MTLRenderPassDescriptor *rp) {
    static int on = -1;
    static uint64_t passes, msaa, loaded, stored, cleared, since;
    if (on < 0) on = getenv("VXRT_STATS") != NULL;
    if (!on) return;
    MTLRenderPassAttachmentDescriptor *all[10];
    int n = 0;
    for (int i = 0; i < 8; i++) if (rp.colorAttachments[i].texture) all[n++] = rp.colorAttachments[i];
    if (rp.depthAttachment.texture) all[n++] = rp.depthAttachment;
    if (rp.stencilAttachment.texture && rp.stencilAttachment.texture != rp.depthAttachment.texture) all[n++] = rp.stencilAttachment;
    int multi = 0;
    for (int i = 0; i < n; i++) {
        id<MTLTexture> t = all[i].texture;
        uint64_t bytes = (uint64_t)MAX(t.width >> all[i].level, 1u) * MAX(t.height >> all[i].level, 1u) * t.sampleCount *
                         (t.pixelFormat == MTLPixelFormatRGBA16Float || t.pixelFormat == MTLPixelFormatRG32Float || t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? 8 :
                          t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR8Unorm ? 1 : 4);
        if (all[i].loadAction == MTLLoadActionLoad) loaded += bytes; else if (all[i].loadAction == MTLLoadActionClear) cleared += bytes;
        if (all[i].storeAction == MTLStoreActionStore || all[i].storeAction == MTLStoreActionStoreAndMultisampleResolve) stored += bytes;
        if (t.sampleCount > 1) multi = 1;
    }
    passes++; msaa += multi;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!since) since = now;
    if (now - since < 2000000000ull) return;
    double s = (now - since) / 1e9;
    vx_log("render passes: %.0f/s (%.0f with multisampled targets); attachments loaded %.1f GB/s, stored %.1f GB/s, cleared on load %.1f GB/s",
           passes / s, msaa / s, loaded / s / 1e9, stored / s / 1e9, cleared / s / 1e9);
    d3d_end_reasons_log();
    passes = msaa = loaded = stored = cleared = 0; since = now;
}

// Whether the open render pass can take a draw to the bound targets: the same depth target, and each bound render
// target one of the pass's (a pass may have more: a game that switches between drawing color and depth and drawing
// depth alone keeps one pass, instead of writing its multisampled targets out and reading them back each time).
static int pass_covers(d3d_context *c, id<MTLTexture> extra_depth) {
    if (!c->render || state_of(c)->pass_depth != (__bridge void *)extra_depth || !d3d_same_target(c->render_dsv, c->dsv)) return 0;
    int any = c->dsv != NULL;
    for (int i = 0; i < 8; i++) {
        if (!c->rtv[i]) continue;
        if (!d3d_same_target(c->render_rtv[i], c->rtv[i])) return 0;
        any = 1;
    }
    return any;
}
// The render pass for the bound targets: kept open while it covers them (pass_covers); contents are loaded.
static id<MTLRenderCommandEncoder> render_pass(d3d_context *c, id<MTLTexture> extra_depth) {
    if (pass_covers(c, extra_depth)) return (__bridge id<MTLRenderCommandEncoder>)c->render;
    static int show = -1;                                    // VXRT_TRACE_PASSES: a multisampled pass ended by new targets
    if (show < 0) show = getenv("VXRT_TRACE_PASSES") != NULL;
    if (show && c->render) {
        static int told;
        int multi = c->render_dsv && ((d3d_texture *)c->render_dsv->resource)->desc[TD_SAMPLES] > 1;
        for (int i = 0; i < 8 && !multi; i++) multi = c->render_rtv[i] && ((d3d_texture *)c->render_rtv[i]->resource)->desc[TD_SAMPLES] > 1;
        if (multi && told++ < 60) {
            char was[300] = "", now[300] = ""; size_t a = 0, b = 0;
            for (int i = 0; i < 8; i++) {
                if (c->render_rtv[i]) { d3d_texture *t = (d3d_texture *)c->render_rtv[i]->resource; a += (size_t)snprintf(was + a, sizeof was - a, " c%d:%p/%ux%u/f%u/s%u", i, (void *)t, t->desc[TD_WIDTH], t->desc[TD_HEIGHT], t->desc[TD_FORMAT], t->desc[TD_SAMPLES]); }
                if (c->rtv[i]) { d3d_texture *t = (d3d_texture *)c->rtv[i]->resource; b += (size_t)snprintf(now + b, sizeof now - b, " c%d:%p/%ux%u/f%u/s%u", i, (void *)t, t->desc[TD_WIDTH], t->desc[TD_HEIGHT], t->desc[TD_FORMAT], t->desc[TD_SAMPLES]); }
            }
            if (c->render_dsv) a += (size_t)snprintf(was + a, sizeof was - a, " d:%p", (void *)c->render_dsv->resource);
            if (c->dsv) b += (size_t)snprintf(now + b, sizeof now - b, " d:%p", (void *)c->dsv->resource);
            vx_log("pass ends for new targets:%s ->%s", was, now);
        }
    }
    d3d_end_render(c);
    // Work recorded so far goes to the GPU every so many draws, between render passes, as Windows drivers send it
    // while the program records: the GPU otherwise idled until the frame's end (ReleaseSync, Present) and then ran
    // the whole frame, after the CPU (VXRT_FLUSH_DRAWS: how many; 0: only when the program flushes).
    static int every = -1;
    if (every < 0) every = getenv("VXRT_FLUSH_DRAWS") ? atoi(getenv("VXRT_FLUSH_DRAWS")) : 0;   // (being measured: off by default)
    if (every > 0 && c->unsent >= (uint32_t)every && c == (d3d_context *)c->base.dev->context && c->cmd) d3d_flush(c->base.dev, 0);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    int any = 0;
    for (int i = 0; i < 8; i++) {
        d3d_view *v = c->rtv[i];
        if (!v) continue;
        rp.colorAttachments[i].texture = view_texture(v);
        rp.colorAttachments[i].level = v->mip;
        if (rp.colorAttachments[i].texture.textureType == MTLTextureType3D) rp.colorAttachments[i].depthPlane = v->first_slice;
        else rp.colorAttachments[i].slice = v->first_slice;
        rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[i].storeAction = MTLStoreActionStore;
        any = 1;
    }
    if (c->dsv) {
        id<MTLTexture> t = view_texture(c->dsv);
        rp.depthAttachment.texture = t; rp.depthAttachment.level = c->dsv->mip; rp.depthAttachment.slice = c->dsv->first_slice;
        rp.depthAttachment.loadAction = MTLLoadActionLoad; rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
            rp.stencilAttachment.texture = t; rp.stencilAttachment.level = c->dsv->mip; rp.stencilAttachment.slice = c->dsv->first_slice;
            rp.stencilAttachment.loadAction = MTLLoadActionLoad; rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        any = 1;
    }
    if (!c->dsv && extra_depth) {                  // Direct3D drops depth written with no depth target; Metal needs one
        rp.depthAttachment.texture = extra_depth;
        rp.depthAttachment.loadAction = MTLLoadActionDontCare; rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    }
    state_of(c)->pass_depth = (__bridge void *)extra_depth;
    if (!any) return nil;
    if (c->nclears) d3d_clears_into_pass(c, (__bridge void *)rp, c->rtv, c->dsv);
    pass_stats(rp);
    c->prof_sample = -1;
    if (prof_on(c->base.dev)) {
        char sig[120]; size_t l = 0;
        for (int i = 0; i < 8; i++) if (rp.colorAttachments[i].texture) {
            id<MTLTexture> t = rp.colorAttachments[i].texture;
            l += (size_t)snprintf(sig + l, sizeof sig - l, "c%d %lux%lu f%lu x%lu ", i, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long)t.sampleCount);
            if (l > 80) break;
        }
        if (rp.depthAttachment.texture) l += (size_t)snprintf(sig + l, sizeof sig - l, "d %lux%lu x%lu", (unsigned long)rp.depthAttachment.texture.width, (unsigned long)rp.depthAttachment.texture.height, (unsigned long)rp.depthAttachment.texture.sampleCount);
        if (!l) snprintf(sig, sizeof sig, "(no targets)");
        uint32_t s = prof_take(sig);
        rp.sampleBufferAttachments[0].sampleBuffer = g_prof_buf;
        rp.sampleBufferAttachments[0].startOfVertexSampleIndex = s;
        rp.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
        rp.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
        rp.sampleBufferAttachments[0].endOfFragmentSampleIndex = s + 1;
        c->prof_sample = (int)s;
    }
    id<MTLRenderCommandEncoder> enc = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) renderCommandEncoderWithDescriptor:rp];
    c->render = (void *)CFBridgingRetain(enc);
    memset(&state_of(c)->bound, 0, sizeof state_of(c)->bound);
    for (int i = 0; i < 8; i++) { c->render_rtv[i] = c->rtv[i]; if (c->rtv[i]) d3d_addref(&c->rtv[i]->base); }
    c->render_dsv = c->dsv;
    if (c->dsv) d3d_addref(&c->dsv->base);
    return enc;
}

static id<MTLDepthStencilState> default_depth_state(d3d_context *c) {
    static void *state;
    if (!state) {
        uint32_t d[13] = { 1, 1, 2, 0 };                     // depth test LESS, writes on: Direct3D's default
        state = make_depth_state((__bridge id<MTLDevice>)c->base.dev->mtl, d);
    }
    return (__bridge id<MTLDepthStencilState>)state;
}
static id<MTLSamplerState> default_sampler(d3d_context *c) {
    static void *state;
    if (!state) {
        uint32_t d[13] = { 0x15, 3, 3, 3, 0, 1, 0 };        // linear filtering, clamped: the default sampler state
        float *f = (float *)d;
        f[11] = -FLT_MAX; f[12] = FLT_MAX;
        state = make_sampler((__bridge id<MTLDevice>)c->base.dev->mtl, d);
    }
    return (__bridge id<MTLSamplerState>)state;
}


// Binds a stage's constant buffers, textures, samplers, buffer views and UAVs on a render or compute encoder.
static id<MTLTexture> view_texture(d3d_view *v);
static id<MTLTexture> shader_texture(d3d_view *v);
static id<MTLSamplerState> default_sampler(d3d_context *c);
typedef __typeof__(((d3d_draw_state *)0)->bound) bound_state;
static void bind_buffer(id<MTLRenderCommandEncoder> enc, bound_state *b, int st, id<MTLBuffer> buf, NSUInteger off, unsigned idx) {
    void *p = (__bridge void *)buf;
    if (idx < 64 && b->buf[st][idx] == p) {
        if (!p || b->off[st][idx] == off) return;
        if (st == STAGE_VS) [enc setVertexBufferOffset:off atIndex:idx]; else [enc setFragmentBufferOffset:off atIndex:idx];
    } else if (st == STAGE_VS) [enc setVertexBuffer:buf offset:off atIndex:idx];
    else [enc setFragmentBuffer:buf offset:off atIndex:idx];
    if (idx < 64) { b->buf[st][idx] = p; b->off[st][idx] = off; }
}
static void bind_texture(id<MTLRenderCommandEncoder> enc, bound_state *b, int st, id<MTLTexture> t, unsigned idx) {
    void *p = (__bridge void *)t;
    if (idx < 80 && b->tex[st][idx] == p) return;
    if (st == STAGE_VS) [enc setVertexTexture:t atIndex:idx]; else [enc setFragmentTexture:t atIndex:idx];
    if (idx < 80) b->tex[st][idx] = p;
}
static void bind_sampler(id<MTLRenderCommandEncoder> enc, bound_state *b, int st, id<MTLSamplerState> sm, unsigned idx) {
    void *p = (__bridge void *)sm;
    if (idx < 16 && b->smp[st][idx] == p) return;
    if (st == STAGE_VS) [enc setVertexSamplerState:sm atIndex:idx]; else [enc setFragmentSamplerState:sm atIndex:idx];
    if (idx < 16) b->smp[st][idx] = p;
}
// Whether a shader resource view reads what a render target or depth view writes: the same texture, overlapping
// mips and layers (a view made without a description covers all of them). Direct3D never lets a shader read a
// texture that is bound for output: binding it as a target unbinds its shader resource views, so the shader reads
// 0. Metal leaves such a read undefined (the tile memory being drawn, half written). A read-only depth view (Flags)
// may be read. (Half-Life: Alyx's chapter scenario never does it.)
static void view_range(const d3d_view *v, int srv, uint32_t *mip0, uint32_t *mip1, uint32_t *s0, uint32_t *s1) {
    const d3d_texture *t = (const d3d_texture *)v->resource;
    uint32_t mips = t->desc[TD_MIPS] ? t->desc[TD_MIPS] : 1, layers = t->dim == 3 ? 0xffffffffu : t->desc[TD_ARRAY] ? t->desc[TD_ARRAY] : 1;
    *mip0 = v->dim ? v->mip : 0;
    *mip1 = !v->dim ? mips : srv ? (v->mips == 0 || v->mips == 0xffffffffu ? mips : v->mip + v->mips) : v->mip + 1;
    int all_layers = !v->dim || t->dim == 3;                    // (3D: a target's W slices, a view's whole volume)
    *s0 = all_layers ? 0 : v->first_slice;
    *s1 = all_layers ? layers : srv && v->dim == 9 ? 6 : v->first_slice + (v->slices ? v->slices : 1);   // (a cube: 6)
}
static int reads_target(const d3d_view *srv, const d3d_view *out, int depth) {
    if (!srv || !out || srv->resource != out->resource || srv->resource->kind != K_TEXTURE) return 0;
    if (depth) {
        int stencil = srv->format == 22 || srv->format == 62;   // (X32_TYPELESS_G8X24_UINT, X24_TYPELESS_G8_UINT)
        if (out->flags & (stencil ? 2 : 1)) return 0;           // that plane is read-only in the depth view
    }
    uint32_t a0, a1, b0, b1, l0, l1, k0, k1;
    view_range(srv, 1, &a0, &a1, &l0, &l1);
    view_range(out, 0, &b0, &b1, &k0, &k1);
    return a0 < b1 && b0 < a1 && l0 < k1 && k0 < l1;
}
static d3d_view *readable_srv(d3d_context *c, d3d_shader *sh, int slot, d3d_view *v) {
    if (!v || v->resource->kind != K_TEXTURE) return v;
    int hit = reads_target(v, c->dsv, 1);
    for (int i = 0; i < 8 && !hit; i++) hit = reads_target(v, c->rtv[i], 0);
    if (!hit) return v;
    static int logged;                                          // (a few, to see that it happens)
    if (logged < 20) { logged++; vx_log("Direct3D: shader %08x reads t%d, which is bound as a render target: it reads 0, as in Direct3D", sh->hash, slot); }
    return NULL;
}
static void bind_stage(d3d_context *c, d3d_draw_state *s, int st, id<MTLRenderCommandEncoder> renc, id<MTLComputeCommandEncoder> cenc) {
    d3d_shader *sh = s->shader[st];
    if (!sh) return;
#define SET_BUFFER(b, off, idx) do { if (cenc) [cenc setBuffer:b offset:off atIndex:idx]; else bind_buffer(renc, &s->bound, st, b, off, idx); } while (0)
#define SET_TEXTURE(t, idx) do { if (cenc) [cenc setTexture:t atIndex:idx]; else bind_texture(renc, &s->bound, st, t, idx); } while (0)
#define SET_SAMPLER(sm, idx) do { if (cenc) [cenc setSamplerState:sm atIndex:idx]; else bind_sampler(renc, &s->bound, st, sm, idx); } while (0)
    dxbc_aux aux = { { 0 }, { 0 } };                             // buffer sizes (for bufinfo), UAV counters
    uint32_t *sizes = aux.sizes;
    for (int i = 0; i < 14; i++) {
        if (!(sh->info.cbuffers & (1u << i))) continue;
        d3d_buffer *cb = s->cb[st][i];
        SET_BUFFER(cb ? (__bridge id<MTLBuffer>)cb->mtl : nil, cb ? cb->off + s->cb_offset[st][i] : 0, i);
    }
    for (int i = 0; i < 32; i++) {
        if (!(sh->info.textures[0] & (1ull << i))) continue;
        SET_TEXTURE(shader_texture(readable_srv(c, sh, i, s->srv[st][i])), i);
    }
    for (int i = 0; i < 16; i++) {
        if (!(sh->info.samplers & (1u << i))) continue;
        SET_SAMPLER(s->smp[st][i] ? (__bridge id<MTLSamplerState>)s->smp[st][i]->mtl : default_sampler(c), i);
    }
    for (int i = 0; i < 22; i++) {                              // raw and structured buffer SRVs
        if (!(sh->info.srv_buffers & (1ull << i))) continue;
        d3d_view *v = s->srv[st][i];
        if (!v || v->resource->kind != K_BUFFER) { SET_BUFFER(nil, 0, DXBC_SRV_BUFFER_BASE + i); continue; }
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)((d3d_buffer *)v->resource)->mtl;
        uint32_t stride = sh->info.srv_stride[i] ? sh->info.srv_stride[i] : 4;
        NSUInteger off = (NSUInteger)v->first * stride;
        SET_BUFFER(b, off, DXBC_SRV_BUFFER_BASE + i);
        sizes[8 + i] = v->count ? v->count * stride : (uint32_t)(b.length - MIN(off, b.length));
    }
    for (int i = 0; i < 8; i++) {                                // UAVs
        if (!(sh->info.uavs & (1u << i))) continue;
        d3d_view *v = s->uav[st][i];
        if (sh->info.uav_kind[i] == DXBC_UAV_TYPED) { SET_TEXTURE(shader_texture(v), DXBC_UAV_TEXTURE_BASE + i); continue; }
        if (!v || v->resource->kind != K_BUFFER) { SET_BUFFER(nil, 0, DXBC_UAV_BUFFER_BASE + i); continue; }
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)((d3d_buffer *)v->resource)->mtl;
        uint32_t stride = sh->info.uav_stride[i] ? sh->info.uav_stride[i] : 4;
        NSUInteger off = (NSUInteger)v->first * stride;
        SET_BUFFER(b, off, DXBC_UAV_BUFFER_BASE + i);
        sizes[i] = v->count ? v->count * stride : (uint32_t)(b.length - MIN(off, b.length));
        if (sh->info.uav_counters & (1u << i)) {                 // the counter, by address
            id<MTLBuffer> cb = counter_of(v);
            aux.counters[i] = cb.gpuAddress;
            if (cenc) [cenc useResource:cb usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            else [renc useResource:cb usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:st == STAGE_VS ? MTLRenderStageVertex : MTLRenderStageFragment];
        }
    }
    if (sh->info.uses_sizes) {
        if (cenc) [cenc setBytes:&aux length:sizeof aux atIndex:DXBC_SIZES_BUFFER];
        else if (st == STAGE_VS) [renc setVertexBytes:&aux length:sizeof aux atIndex:DXBC_SIZES_BUFFER];
        else [renc setFragmentBytes:&aux length:sizeof aux atIndex:DXBC_SIZES_BUFFER];
        if (!cenc) s->bound.buf[st][DXBC_SIZES_BUFFER] = (void *)-1;   // no longer a buffer of ours
    }
#undef SET_BUFFER
#undef SET_TEXTURE
#undef SET_SAMPLER
}

// Sets everything a draw needs; returns the encoder, or nil when the draw cannot happen.
// VXRT_TRACE_DRAWS=file: once that file exists, the next 300 draws are logged (shaders, targets, textures bound).
static void trace_draw(d3d_context *c, d3d_draw_state *s) {
    static int left = -1;
    static const char *path;
    if (left < 0) { path = getenv("VXRT_TRACE_DRAWS"); left = path ? 0 : -2; }
    if (left == -2) return;
    if (!left) { static int polls; if (++polls % 64 || access(path, F_OK)) return; left = 300; }
    left--;
    char line[1024]; size_t k = 0;
    d3d_shader *vs = s->shader[STAGE_VS], *ps = s->shader[STAGE_PS];
    k += (size_t)snprintf(line + k, sizeof line - k, "draw vs %08x ps %08x", vs ? vs->hash : 0, ps ? ps->hash : 0);
    for (int i = 0; i < 8; i++) if (c->rtv[i]) {
        d3d_texture *t = (d3d_texture *)c->rtv[i]->resource;
        k += (size_t)snprintf(line + k, sizeof line - k, " rt%d %ux%u f%u%s", i, t->desc[TD_WIDTH], t->desc[TD_HEIGHT], c->rtv[i]->format, t->shared ? "S" : "");
    }
    for (int i = 0; i < 16 && k < sizeof line - 64; i++) if (s->srv[STAGE_PS][i] && s->srv[STAGE_PS][i]->resource->kind == K_TEXTURE) {
        d3d_view *v = s->srv[STAGE_PS][i]; d3d_texture *t = (d3d_texture *)v->resource;
        k += (size_t)snprintf(line + k, sizeof line - k, " t%d %ux%u f%u d%u%s", i, t->desc[TD_WIDTH], t->desc[TD_HEIGHT], v->format, v->dim, t->shared ? "S" : "");
    }
    vx_log("%s", line);
    // VXRT_DRAW_SNAPSHOT=<ps hash>:<file prefix>: the textures that pixel shader reads, as PNGs, once.
    static int done;
    const char *ds = getenv("VXRT_DRAW_SNAPSHOT");
    char want[16];
    if (!done && ds && ps && sscanf(ds, "%8[0-9a-f]", want) == 1 && strtoul(want, NULL, 16) == ps->hash && strchr(ds, ':')) {
        done = 1;
        d3d_flush(c->base.dev, 1);
        for (int b = 0; b < 4; b++) if (s->cb[STAGE_PS][b]) {
            id<MTLBuffer> mb = (__bridge id<MTLBuffer>)s->cb[STAGE_PS][b]->mtl;
            if (mb.storageMode != MTLStorageModeShared) continue;
            const float *f = (const float *)((const uint8_t *)mb.contents + s->cb[STAGE_PS][b]->off + s->cb_offset[STAGE_PS][b]);
            for (NSUInteger r = 0; r < MIN(s->cb[STAGE_PS][b]->desc[0] / 16, 16u); r++)
                vx_log("ps cb%d[%lu] = %g %g %g %g", b, (unsigned long)r, f[4 * r], f[4 * r + 1], f[4 * r + 2], f[4 * r + 3]);
        }
        for (int i = 0; i < 16; i++) if (s->srv[STAGE_PS][i] && s->srv[STAGE_PS][i]->resource->kind == K_TEXTURE) {
            char path[1024];
            snprintf(path, sizeof path, "%s.t%d.png", strchr(ds, ':') + 1, i);
            d3d_texture_png(c->base.dev, ((d3d_texture *)s->srv[STAGE_PS][i]->resource)->mtl, path);
        }
    }
}
static id<MTLRenderCommandEncoder> prepare(d3d_context *c, MTLPrimitiveType *type) {
    HELD_OP(0);
    c->unsent++;
    d3d_draw_state *s = state_of(c);
    if (!s->shader[STAGE_VS]) return nil;
    trace_draw(c, s);
    if (s->gs_vertices) *type = MTLPrimitiveTypeTriangle;   // a geometry kernel's output
    else switch (s->topology) {
    case 1: *type = MTLPrimitiveTypePoint; break;
    case 2: *type = MTLPrimitiveTypeLine; break;
    case 3: *type = MTLPrimitiveTypeLineStrip; break;
    case 5: *type = MTLPrimitiveTypeTriangleStrip; break;
    case 4: *type = MTLPrimitiveTypeTriangle; break;
    default: return nil;                                       // undefined or patch lists
    }
    if (c->nclears && c->render) {                     // a target cleared meanwhile, read by this draw: the clear first
        int reads = 0;
        for (int k = 0; k < c->nclears && !reads; k++) {
            d3d_object *res = c->clears[k].v ? c->clears[k].v->resource : c->clears[k].res;
            for (int st = 0; st < STAGES && !reads; st++) {
                for (int i = 0; i < 32 && !reads; i++) reads = s->srv[st][i] && s->srv[st][i]->resource == res;
                for (int i = 0; i < 8 && !reads; i++) reads = s->uav[st][i] && s->uav[st][i]->resource == res;
            }
        }
        if (reads) { d3d_end_render(c); d3d_clears_flush(c); }
    }
    id<MTLTexture> colors[8] = { nil };
    int ncolors = 0;
    for (int i = 0; i < 8; i++) if (c->rtv[i]) { colors[i] = view_texture(c->rtv[i]); ncolors = i + 1; }
    id<MTLTexture> depth = c->dsv ? view_texture(c->dsv) : nil, extra = nil;
    if (!depth && s->shader[STAGE_PS] && s->shader[STAGE_PS]->info.writes_depth && colors[0]) {
        id<MTLTexture> d = (__bridge id<MTLTexture>)s->dummy_depth;
        if (!d || d.width != colors[0].width || d.height != colors[0].height || d.sampleCount != colors[0].sampleCount) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:colors[0].width height:colors[0].height mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
            if (colors[0].sampleCount > 1) { td.textureType = MTLTextureType2DMultisample; td.sampleCount = colors[0].sampleCount; }
            d = [colors[0].device newTextureWithDescriptor:td];
            if (s->dummy_depth) CFRelease(s->dummy_depth);
            s->dummy_depth = (void *)CFBridgingRetain(d);
        }
        depth = extra = d;
    }
    uint32_t unbound = 0;
    if (pass_covers(c, extra)) {                       // the open pass's targets, those not bound here masked off
        for (int i = 0; i < 8; i++) {
            if (!c->render_rtv[i] || c->rtv[i]) continue;
            colors[i] = view_texture(c->render_rtv[i]); unbound |= 1u << i; ncolors = MAX(ncolors, i + 1);
        }
    }
    id<MTLRenderPipelineState> pso = pipeline(c, s, colors, ncolors, depth, unbound);
    if (!pso) return nil;
    id<MTLRenderCommandEncoder> enc = render_pass(c, extra);
    if (!enc) return nil;
    if (c->prof_sample >= 0 && s->shader[STAGE_PS] && !prof_note_shader((uint32_t)c->prof_sample, s->shader[STAGE_PS]->hash)) c->prof_sample = -1;
    bound_state *bs = &s->bound;
    if (bs->pso != (__bridge void *)pso) { [enc setRenderPipelineState:pso]; bs->pso = (__bridge void *)pso; }
    id<MTLDepthStencilState> dss = s->depth ? (__bridge id<MTLDepthStencilState>)s->depth->mtl : default_depth_state(c);
    if (extra) {                                       // no depth target in Direct3D terms: no depth test
        static void *off;
        if (!off) { uint32_t d[13] = { 0, 0, 8 /* ALWAYS */ }; off = make_depth_state((__bridge id<MTLDevice>)c->base.dev->mtl, d); }
        dss = (__bridge id<MTLDepthStencilState>)off;
    }
    if (bs->dss != (__bridge void *)dss) { [enc setDepthStencilState:dss]; bs->dss = (__bridge void *)dss; }
    if (bs->stencil_ref != s->stencil_ref) { [enc setStencilReferenceValue:s->stencil_ref]; bs->stencil_ref = s->stencil_ref; }
    if (memcmp(bs->blend, s->blend_factor, sizeof bs->blend)) {
        [enc setBlendColorRed:s->blend_factor[0] green:s->blend_factor[1] blue:s->blend_factor[2] alpha:s->blend_factor[3]];
        memcpy(bs->blend, s->blend_factor, sizeof bs->blend);
    }
    // Rasterizer: fill, cull, winding, depth bias and clipping (defaults: solid, back faces culled, clockwise front).
    // A new encoder starts with fill, no culling, clockwise, no bias and clipping: raster[] 0 means "not set yet".
    const uint32_t *rd = s->raster ? s->raster->desc : NULL;
    uint32_t r[7] = { rd && rd[0] == 2 ? 2 : 1, !rd || rd[1] == 3 ? 3 : rd[1] == 2 ? 2 : 1, rd && rd[2] ? 2 : 1,
                      rd ? rd[3] : 0, rd ? rd[4] : 0, rd ? rd[5] : 0, !rd || rd[6] ? 2 : 1 };
    if (memcmp(bs->raster, r, sizeof r)) {
        if (bs->raster[0] != r[0]) [enc setTriangleFillMode:r[0] == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        if (bs->raster[1] != r[1]) [enc setCullMode:r[1] == 3 ? MTLCullModeBack : r[1] == 2 ? MTLCullModeFront : MTLCullModeNone];
        if (bs->raster[2] != r[2]) [enc setFrontFacingWinding:r[2] == 2 ? MTLWindingCounterClockwise : MTLWindingClockwise];
        if (bs->raster[3] != r[3] || bs->raster[4] != r[4] || bs->raster[5] != r[5] || !bs->raster[0]) {
            float clamp, slope;
            memcpy(&clamp, &r[4], 4); memcpy(&slope, &r[5], 4);
            [enc setDepthBias:(float)(int32_t)r[3] / 16777216.0f slopeScale:slope clamp:clamp];
        }
        if (bs->raster[6] != r[6]) [enc setDepthClipMode:r[6] == 2 ? MTLDepthClipModeClip : MTLDepthClipModeClamp];
        memcpy(bs->raster, r, sizeof r);
    }
    id<MTLTexture> target = colors[0] ? colors[0] : depth;
    NSUInteger tw = target ? target.width >> (c->rtv[0] ? c->rtv[0]->mip : 0) : 1, th = target ? target.height >> (c->rtv[0] ? c->rtv[0]->mip : 0) : 1;
    if (!tw) tw = 1;
    if (!th) th = 1;
    // Viewports and scissor rectangles, as many as were set (SV_ViewportArrayIndex picks one per primitive).
    uint32_t nvp = MAX(c->nviewports, 1);
    MTLViewport vps[16];
    MTLScissorRect scs[16];
    for (uint32_t i = 0; i < nvp; i++) {
        const float *v = c->viewport + 6 * i;
        vps[i] = (MTLViewport){ v[0], v[1], v[2], v[3], v[4], v[5] };
        if (vps[i].width <= 0 || vps[i].height <= 0) vps[i] = (MTLViewport){ 0, 0, (double)tw, (double)th, 0, 1 };
        scs[i] = (MTLScissorRect){ 0, 0, tw, th };
        if (rd && rd[7]) {                                      // scissor enabled
            RECT q = s->scissor[i < MAX(s->nscissors, 1) ? i : 0];
            LONG l = MAX(q.left, 0), t = MAX(q.top, 0);
            LONG r = MIN(q.right, (LONG)tw), b = MIN(q.bottom, (LONG)th);
            scs[i] = (MTLScissorRect){ (NSUInteger)MIN(l, (LONG)tw), (NSUInteger)MIN(t, (LONG)th), (NSUInteger)MAX(r - l, 0), (NSUInteger)MAX(b - t, 0) };
        }
    }
    if (bs->nvp != nvp || memcmp(bs->vp, vps, nvp * sizeof *vps) || memcmp(bs->sc, scs, nvp * sizeof *scs)) {
        if (nvp == 1) { [enc setViewport:vps[0]]; [enc setScissorRect:scs[0]]; }
        else { [enc setViewports:vps count:nvp]; [enc setScissorRects:scs count:nvp]; }
        bs->nvp = nvp; memcpy(bs->vp, vps, nvp * sizeof *vps); memcpy(bs->sc, scs, nvp * sizeof *scs);
    }
    if (s->gs_vertices) {
        bind_buffer(enc, bs, STAGE_VS, (__bridge id<MTLBuffer>)s->gs_vertices, 0, 0);
        bind_buffer(enc, bs, STAGE_VS, (__bridge id<MTLBuffer>)s->gs_counts, 0, 1);
    }
    else {
        for (int i = 0; i < 16; i++)
            if (s->vb[i]) bind_buffer(enc, bs, STAGE_VS, (__bridge id<MTLBuffer>)s->vb[i]->mtl, s->vb[i]->off + s->offset[i], VB_BASE + i);
        bind_stage(c, s, STAGE_VS, enc, nil);
    }
    bind_stage(c, s, STAGE_PS, enc, nil);
    return enc;
}

// ---- draws with a geometry shader ------------------------------------------------------------------------------------------
// The vertex shader runs as a kernel over the draw's vertices (fetched by Metal through a stage-input
// descriptor made from the input layout, indices included), the geometry kernel over its primitives, and the
// triangles it made are drawn with its pass-through vertex function and the pixel shader.
// What a vertex format delivers to a shader: 0 floats (also normalized integers), 1 signed integers, 2 unsigned.
static int format_kind(MTLVertexFormat f) {
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
static id<MTLComputePipelineState> vs_kernel_pipeline(d3d_context *c, d3d_draw_state *s, d3d_shader *vs, int index_type) {
    static NSMutableDictionary *cache;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    NSMutableString *key = [NSMutableString stringWithFormat:@"%u/%u/%d/", vs->id, s->layout ? s->layout->id : 0, index_type];
    if (s->layout) for (uint32_t i = 0; i < s->layout->n; i++) [key appendFormat:@"%u,", s->stride[s->layout->attr[i].slot & 15]];
    pthread_mutex_lock(&lock);
    if (!cache) cache = [NSMutableDictionary new];
    id cached = cache[key];
    pthread_mutex_unlock(&lock);
    if (cached) return cached == [NSNull null] ? nil : cached;   // (failures are remembered too: no retry per draw)
    // Compute stage input converts formats only within a kind: the kernel's inputs are declared as the layout's
    // formats deliver them (signed or unsigned integers, or floats), as the IA would hand their bits over.
    NSMutableString *source = [NSMutableString stringWithUTF8String:vs->info.msl_kernel ? vs->info.msl_kernel : ""];
    if (s->layout) for (uint32_t i = 0; i < s->layout->n; i++) {
        const layout_attr *a = &s->layout->attr[i];
        int kind = format_kind(vertex_format(a->format, NULL));
        const char *want = kind == 1 ? "int4" : kind == 2 ? "uint4" : "float4";
        for (int k = 0; k < 3; k++) {
            static const char *const types[3] = { "float4", "int4", "uint4" };
            NSString *from = [NSString stringWithFormat:@"    %s v%u [[attribute(%u)]];", types[k], a->reg, a->reg];
            NSString *to = [NSString stringWithFormat:@"    %s v%u [[attribute(%u)]];", want, a->reg, a->reg];
            [source replaceOccurrencesOfString:from withString:to options:0 range:NSMakeRange(0, source.length)];
        }
    }
    NSError *err = nil;
    id<MTLLibrary> lib = source.length ? [(__bridge id<MTLDevice>)c->base.dev->mtl newLibraryWithSource:source options:nil error:&err] : nil;
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
    id<MTLComputePipelineState> pso = nil;
    if (!fn) {
        vx_log("Direct3D: kernel version of vertex shader %08x did not compile: %s", vs->hash, err.localizedDescription.UTF8String);
        pthread_mutex_lock(&lock); cache[key] = [NSNull null]; pthread_mutex_unlock(&lock);
        return nil;
    }
    MTLStageInputOutputDescriptor *sd = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
    if (s->layout) for (uint32_t i = 0; i < s->layout->n; i++) {
        const layout_attr *a = &s->layout->attr[i];
        sd.attributes[a->reg].format = (MTLAttributeFormat)vertex_format(a->format, NULL);   // the same numbering
        sd.attributes[a->reg].offset = a->offset;
        sd.attributes[a->reg].bufferIndex = VB_BASE + a->slot;
        MTLBufferLayoutDescriptor *bl = sd.layouts[VB_BASE + a->slot];
        uint32_t stride = s->stride[a->slot];
        if (!stride) { bl.stepFunction = MTLStepFunctionConstant; bl.stepRate = 0; bl.stride = 16; }
        else {
            bl.stride = stride;
            bl.stepFunction = a->instanced ? MTLStepFunctionThreadPositionInGridY : index_type ? MTLStepFunctionThreadPositionInGridXIndexed : MTLStepFunctionThreadPositionInGridX;
            bl.stepRate = a->instanced ? (a->step ? a->step : 1) : 1;
        }
    }
    if (index_type) { sd.indexType = index_type == 2 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16; sd.indexBufferIndex = DXBC_GS_INDEX; }
    MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
    cd.computeFunction = fn;
    cd.stageInputDescriptor = sd;
    pso = [(__bridge id<MTLDevice>)c->base.dev->mtl newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&err];
    if (!pso) vx_log("Direct3D: vertex kernel pipeline failed: %s", err.localizedDescription.UTF8String);
    pthread_mutex_lock(&lock);
    cache[key] = pso ? (id)pso : (id)[NSNull null];
    pthread_mutex_unlock(&lock);
    return pso;
}
// Whether a geometry draw's kernels can run ahead of the command buffer being recorded: everything they read
// (vertex, index and constant buffers, the vertex and geometry stages' resources) holds what the CPU put there,
// not what GPU work recorded since the last submission writes (render targets, depth, UAVs, stream output, copies).
static int gpu_may_write(const d3d_object *o, uint64_t submitted) {
    if (!o) return 0;
    if (o->gpu_seq > submitted) return 1;
    uint32_t bind = o->kind == K_BUFFER ? ((const d3d_buffer *)o)->desc[2] : o->kind == K_TEXTURE ? ((const d3d_texture *)o)->desc[TD_BIND] : 0;
    return (bind & (0x10 | 0x20 | 0x40 | 0x80)) != 0;     // stream output, render target, depth stencil, unordered access
}
static int gs_inputs_from_cpu(d3d_context *c, d3d_draw_state *s) {
    uint64_t sub = __atomic_load_n(&c->base.dev->submitted, __ATOMIC_ACQUIRE);
    static int told;
    #define GS_NOT_EARLY(what, i, o) do { if (vx_trace && told++ < 10) vx_log("Direct3D: geometry draw in pass order: %s %d (kind %d, gpu_seq %llu)", what, i, (o)->kind, (unsigned long long)(o)->gpu_seq); return 0; } while (0)
    for (int i = 0; i < 16; i++) if (s->vb[i] && gpu_may_write(&s->vb[i]->base, sub)) GS_NOT_EARLY("vertex buffer", i, &s->vb[i]->base);
    if (s->ib && gpu_may_write(&s->ib->base, sub)) GS_NOT_EARLY("index buffer", 0, &s->ib->base);
    static const int stages[2] = { STAGE_VS, STAGE_GS };
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 14; i++) if (s->cb[stages[k]][i] && gpu_may_write(&s->cb[stages[k]][i]->base, sub)) GS_NOT_EARLY(k ? "GS constants" : "VS constants", i, &s->cb[stages[k]][i]->base);
        for (int i = 0; i < 32; i++) if (s->srv[stages[k]][i] && gpu_may_write(s->srv[stages[k]][i]->resource, sub)) GS_NOT_EARLY(k ? "GS resource" : "VS resource", i, s->srv[stages[k]][i]->resource);
    }
    #undef GS_NOT_EARLY
    return 1;
}
// The vertex kernel over a draw's vertices (thread (x, y): vertex x of instance y), its outputs to vs_out (vsn float4s
// a vertex): vertex buffers moved to the first vertex (or the base vertex of indexed draws).
static void run_vs_kernel(d3d_context *c, d3d_draw_state *s, id<MTLComputeCommandEncoder> enc, id<MTLComputePipelineState> vk, id<MTLBuffer> vs_out, uint32_t vsn,
                          uint32_t count, uint32_t instances, uint32_t start, int32_t base_vertex, uint32_t start_instance, int index_type) {
    int indexed = index_type != 0;
    [enc setComputePipelineState:vk];
    for (int i = 0; i < 16; i++) {
        if (!s->vb[i]) continue;
        int instanced = 0, step_rate = 1;
        if (s->layout) for (uint32_t k = 0; k < s->layout->n; k++) if (s->layout->attr[k].slot == (uint32_t)i && s->layout->attr[k].instanced) { instanced = 1; step_rate = s->layout->attr[k].step ? (int)s->layout->attr[k].step : 1; }
        int64_t off = (int64_t)s->vb[i]->off + s->offset[i] + (int64_t)s->stride[i] * (instanced ? (int64_t)(start_instance / (uint32_t)step_rate) : indexed ? base_vertex : (int64_t)start);
        if (off < 0) off = 0;
        [enc setBuffer:(__bridge id<MTLBuffer>)s->vb[i]->mtl offset:(NSUInteger)off atIndex:VB_BASE + i];
    }
    if (indexed) [enc setBuffer:(__bridge id<MTLBuffer>)s->ib->mtl offset:s->ib->off + s->ib_offset + (NSUInteger)start * (index_type == 2 ? 4 : 2) atIndex:DXBC_GS_INDEX];
    else [enc setBuffer:vs_out offset:0 atIndex:DXBC_GS_INDEX];          // (not read)
    bind_stage(c, s, STAGE_VS, nil, enc);
    uint32_t vp[8] = { count, indexed ? 0 : start, start_instance, vsn, (uint32_t)index_type, 0, (uint32_t)base_vertex, 0 };
    [enc setBytes:vp length:sizeof vp atIndex:DXBC_VS_PARAMS];
    [enc setBuffer:vs_out offset:0 atIndex:DXBC_GS_OUT];
    [enc dispatchThreads:MTLSizeMake(count, instances, 1) threadsPerThreadgroup:MTLSizeMake(MIN(64, vk.maxTotalThreadsPerThreadgroup), 1, 1)];
}

// Stream output with a vertex shader's code: the vertex kernel, then a kernel copying the declared components of each
// vertex's outputs to the targets, from where each one's earlier stream output ended (as Direct3D: in draw order,
// whole primitives of a list; writes past a buffer's end are dropped). Strips are not expanded yet.
static const char g_so_source[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct SoEntry { uint reg, comp, count, slot_dword; };\n"
    "struct SoParams { uint nverts, nentries, vsn, pad; uint base[4], stride[4], cap[4]; SoEntry e[64]; };\n"
    "kernel void vx_so(uint v [[thread_position_in_grid]], const device float4 *src [[buffer(0)]], constant SoParams &p [[buffer(1)]],\n"
    "                  device uint *d0 [[buffer(2)]], device uint *d1 [[buffer(3)]], device uint *d2 [[buffer(4)]], device uint *d3 [[buffer(5)]]) {\n"
    "    if (v >= p.nverts) return;\n"
    "    for (uint i = 0; i < p.nentries; i++) {\n"
    "        SoEntry e = p.e[i];\n"
    "        uint slot = e.slot_dword >> 24, at = p.base[slot] + v * p.stride[slot] + (e.slot_dword & 0xffffff);\n"
    "        if (at + e.count > p.cap[slot]) continue;\n"
    "        device uint *d = slot == 0 ? d0 : slot == 1 ? d1 : slot == 2 ? d2 : d3;\n"
    "        uint4 x = as_type<uint4>(src[v * p.vsn + e.reg]);\n"
    "        for (uint k = 0; k < e.count && e.comp + k < 4; k++) d[at + k] = x[e.comp + k];\n"
    "    }\n"
    "}\n";
static void so_draw(d3d_context *c, uint32_t count, uint32_t instances, uint32_t start, int32_t base_vertex, uint32_t start_instance, int indexed) {
    d3d_draw_state *s = state_of(c);
    d3d_shader *vs = s->shader[STAGE_VS], *gs = s->shader[STAGE_GS];
    const so_layout *so = gs->so;
    if (!vs || !count || !instances || (indexed && !s->ib)) return;
    uint32_t per = s->topology == 1 ? 1 : s->topology == 2 ? 2 : s->topology == 4 ? 3 : 0;   // point, line, triangle lists
    if (!per) { d3d_not_implemented("stream output from strips"); return; }
    count -= count % per;
    int any = 0;
    for (int i = 0; i < 4; i++) any |= s->so[i] != NULL;
    id<MTLDevice> dev = (__bridge id<MTLDevice>)c->base.dev->mtl;
    if (any && count && gs->info.type == DXBC_VERTEX) {
        static id<MTLComputePipelineState> pack;
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&lock);
        if (!pack) {
            NSError *err = nil;
            id<MTLLibrary> lib = [dev newLibraryWithSource:@(g_so_source) options:nil error:&err];
            id<MTLFunction> fn = [lib newFunctionWithName:@"vx_so"];
            pack = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
            if (!pack) vx_log("Direct3D: stream output kernel failed: %s", err.localizedDescription.UTF8String);
        }
        pthread_mutex_unlock(&lock);
        int index_type = indexed ? (s->ib_format == 42 ? 2 : 1) : 0;
        id<MTLComputePipelineState> vk = vs_kernel_pipeline(c, s, vs, index_type);
        uint32_t vsn = (uint32_t)(vs->info.out_regs ? vs->info.out_regs : 1), nverts = count * instances;
        id<MTLBuffer> vs_out = vk && pack ? [dev newBufferWithLength:(NSUInteger)nverts * vsn * 16 options:MTLResourceStorageModePrivate] : nil;
        if (vs_out) {
            d3d_end_render(c);
            id<MTLComputeCommandEncoder> enc = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) computeCommandEncoder];
            run_vs_kernel(c, s, enc, vk, vs_out, vsn, count, instances, start, base_vertex, start_instance, index_type);
            struct { uint32_t nverts, nentries, vsn, pad, base[4], stride[4], cap[4]; so_entry e[64]; } p;
            memset(&p, 0, sizeof p);
            p.nverts = nverts; p.nentries = so->n; p.vsn = vsn;
            memcpy(p.e, so->e, sizeof(so_entry) * so->n);
            [enc setComputePipelineState:pack];
            [enc setBuffer:vs_out offset:0 atIndex:0];
            for (int i = 0; i < 4; i++) {
                d3d_buffer *b = s->so[i];
                if (b) {
                    p.base[i] = b->so_filled / 4; p.stride[i] = so->stride[i]; p.cap[i] = b->desc[0] / 4;
                    [enc setBuffer:(__bridge id<MTLBuffer>)b->mtl offset:b->off atIndex:2 + i];
                    uint32_t end = b->so_filled + nverts * so->stride[i] * 4;
                    b->so_filled = MIN(end, b->desc[0]);
                    d3d_mark_gpu(c, &b->base);
                } else [enc setBuffer:vs_out offset:0 atIndex:2 + i];           // (not written: no entries for it)
            }
            [enc setBytes:&p length:sizeof p atIndex:1];
            [enc dispatchThreads:MTLSizeMake(nverts, 1, 1) threadsPerThreadgroup:MTLSizeMake(MIN(64, pack.maxTotalThreadsPerThreadgroup), 1, 1)];
            [enc endEncoding];
        }
    }
    if (!so->rasterized) return;
    // Stream 0 is also drawn: a vertex shader's code draws as without the geometry stage, a geometry shader as one.
    if (gs->info.type == DXBC_VERTEX) {
        s->shader[STAGE_GS] = NULL;
        MTLPrimitiveType type;
        id<MTLRenderCommandEncoder> renc = prepare(c, &type);
        if (renc && !indexed) [renc drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:start_instance];
        else if (renc) [renc drawIndexedPrimitives:type indexCount:count indexType:s->ib_format == 42 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                                       indexBuffer:(__bridge id<MTLBuffer>)s->ib->mtl indexBufferOffset:s->ib->off + s->ib_offset + start * (s->ib_format == 42 ? 4 : 2)
                                     instanceCount:instances baseVertex:base_vertex baseInstance:start_instance];
        s->shader[STAGE_GS] = gs;
    }
}

static void gs_draw(d3d_context *c, uint32_t count, uint32_t instances, uint32_t start, int32_t base_vertex, uint32_t start_instance, int indexed) {
    d3d_draw_state *s = state_of(c);
    d3d_shader *vs = s->shader[STAGE_VS], *gs = s->shader[STAGE_GS];
    if (gs->so && gs->info.type == DXBC_VERTEX) { so_draw(c, count, instances, start, base_vertex, start_instance, indexed); return; }
    if (gs->so && !((so_layout *)gs->so)->rasterized) return;     // (stream output from a geometry shader: not yet)
    if (!vs || !count || !instances || (indexed && !s->ib)) return;
    uint32_t n = (uint32_t)gs->info.gs_in_verts, step, prims;
    switch (s->topology) {                               // primitives from the input assembler's topology
    case 1: case 2: case 4: case 10: case 12: step = n; prims = count / n; break;    // lists
    case 3: case 5: step = 1; prims = count >= n ? count - (n - 1) : 0; break;        // strips
    default: return;
    }
    if (!prims || !shader_function(gs) || !gs_passthrough(c->base.dev, gs)) return;
    if (vx_trace) {                                      // (VXRT_TRACE: each geometry shader's shape, once, and how it is drawn)
        static uint32_t seen[64]; static int nseen;
        int known = 0;
        for (int i = 0; i < nseen && !known; i++) known = seen[i] == gs->hash;
        if (!known && nseen < 64) {
            seen[nseen++] = gs->hash;
            vx_log("Direct3D: geometry shader %08x: %d vertices in, up to %d triangles of %d registers out; draw of %u vertices (%u primitives) x %u%s",
                   gs->hash, gs->info.gs_in_verts, gs->info.gs_max_tris, gs->info.out_regs, count, prims, instances, indexed ? ", indexed" : "");
        }
    }
    int index_type = indexed ? (s->ib_format == 42 ? 2 : 1) : 0;
    id<MTLComputePipelineState> vk = vs_kernel_pipeline(c, s, vs, index_type);
    if (!vk) return;
    if (!gs->compute) {
        NSError *err = nil;
        id<MTLComputePipelineState> p = [(__bridge id<MTLDevice>)c->base.dev->mtl newComputePipelineStateWithFunction:shader_function(gs) error:&err];
        if (!p) { vx_log("Direct3D: geometry kernel pipeline failed: %s", err.localizedDescription.UTF8String); return; }
        gs->compute = (void *)CFBridgingRetain(p);
    }
    uint32_t vsn = (uint32_t)(vs->info.out_regs ? vs->info.out_regs : 1), gsn = (uint32_t)gs->info.out_regs, tris = (uint32_t)gs->info.gs_max_tris;
    // Inputs only the CPU wrote: the kernels go to the command buffer that runs first, and the render pass in
    // progress stays open (Half-Life: Alyx's menu has 30 geometry draws a frame on the eye targets: each one wrote
    // out and reloaded them, 60 GB/s, 22 fps).
    static int early_ok = -1;
    if (early_ok < 0) early_ok = getenv("VXRT_GS_EARLY") != NULL;    // (being measured: off by default)
    int early = early_ok && gs_inputs_from_cpu(c, s);
    // The buffers are kept from one geometry draw to the next (Metal orders this draw's writes after the last one's
    // reads): a new one each time cost an allocation of up to 66 MB per draw (Alyx's menu: 31 a frame). Kernels run
    // early all go before any of their draws, so those get their own.
    NSUInteger lens[3] = { (NSUInteger)count * instances * vsn * 16, (NSUInteger)prims * instances * tris * 3 * gsn * 16, (NSUInteger)prims * instances * 4 };
    id<MTLBuffer> bufs[3];
    for (int k = 0; k < 3; k++) {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)c->base.dev->mtl;
        id<MTLBuffer> b = early ? nil : (__bridge id<MTLBuffer>)s->gs_scratch[k];
        if (!b || b.length < lens[k]) {
            b = [dev newBufferWithLength:MAX(lens[k], (NSUInteger)4096) options:MTLResourceStorageModePrivate];
            if (!b) return;
            if (!early) { if (s->gs_scratch[k]) CFRelease(s->gs_scratch[k]); s->gs_scratch[k] = (void *)CFBridgingRetain(b); }
        }
        bufs[k] = b;
    }
    id<MTLBuffer> vs_out = bufs[0], gs_out = bufs[1], counts = bufs[2];
    id<MTLComputeCommandEncoder> enc;
    if (early) {
        if (!c->pre) {
            id<MTLCommandBuffer> pre = [(__bridge id<MTLCommandQueue>)c->base.dev->queue commandBuffer];
            c->pre = (void *)CFBridgingRetain(pre);
            c->pre_enc = (void *)CFBridgingRetain([pre computeCommandEncoder]);
        }
        enc = (__bridge id<MTLComputeCommandEncoder>)c->pre_enc;
    } else {
        d3d_end_render(c);
        enc = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) computeCommandEncoder];
    }
    run_vs_kernel(c, s, enc, vk, vs_out, vsn, count, instances, start, base_vertex, start_instance, index_type);
    // The geometry kernel.
    id<MTLComputePipelineState> gk = (__bridge id<MTLComputePipelineState>)gs->compute;
    [enc setComputePipelineState:gk];
    bind_stage(c, s, STAGE_GS, nil, enc);
    uint32_t gp[4] = { prims, step, count, vsn };
    [enc setBytes:gp length:sizeof gp atIndex:DXBC_GS_PARAMS];
    [enc setBuffer:vs_out offset:0 atIndex:DXBC_GS_IN];
    [enc setBuffer:gs_out offset:0 atIndex:DXBC_GS_OUT];
    [enc setBuffer:counts offset:0 atIndex:DXBC_GS_COUNTS];
    [enc dispatchThreads:MTLSizeMake(prims, instances, 1) threadsPerThreadgroup:MTLSizeMake(MIN(64, gk.maxTotalThreadsPerThreadgroup), 1, 1)];
    if (!early) [enc endEncoding];
    // The triangles.
    s->gs_vertices = (__bridge void *)gs_out;
    s->gs_counts = (__bridge void *)counts;
    MTLPrimitiveType type;
    id<MTLRenderCommandEncoder> renc = prepare(c, &type);
    if (renc) [renc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(NSUInteger)prims * instances * tris * 3];
    s->gs_vertices = NULL;
    s->last_pso = NULL;                                  // (the pass-through pipeline is not the next draw's)
}

static WINAPI void ctx_draw(void *this, uint32_t count, uint32_t start) {
    if (state_of(this)->shader[STAGE_GS]) { gs_draw(this, count, 1, start, 0, 0, 0); return; }
    MTLPrimitiveType type;
    id<MTLRenderCommandEncoder> enc = prepare(this, &type);
    if (enc && count) [enc drawPrimitives:type vertexStart:start vertexCount:count];
}
static WINAPI void ctx_draw_instanced(void *this, uint32_t count, uint32_t instances, uint32_t start, uint32_t start_instance) {
    if (state_of(this)->shader[STAGE_GS]) { gs_draw(this, count, instances, start, 0, start_instance, 0); return; }
    MTLPrimitiveType type;
    id<MTLRenderCommandEncoder> enc = prepare(this, &type);
    if (enc && count && instances) [enc drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:start_instance];
}
static void draw_indexed(d3d_context *c, uint32_t count, uint32_t instances, uint32_t start, int32_t base_vertex, uint32_t start_instance) {
    d3d_draw_state *s = state_of(c);
    if (s->shader[STAGE_GS]) { gs_draw(c, count, instances, start, base_vertex, start_instance, 1); return; }
    MTLPrimitiveType type;
    if (!s->ib || !count || !instances) return;
    id<MTLRenderCommandEncoder> enc = prepare(c, &type);
    if (!enc) return;
    int wide = s->ib_format == 42;                             // R32_UINT, else R16_UINT
    [enc drawIndexedPrimitives:type indexCount:count indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                   indexBuffer:(__bridge id<MTLBuffer>)s->ib->mtl indexBufferOffset:s->ib->off + s->ib_offset + start * (wide ? 4 : 2)
                 instanceCount:instances baseVertex:base_vertex baseInstance:start_instance];
}
static WINAPI void ctx_draw_indexed(void *this, uint32_t count, uint32_t start, int32_t base_vertex) { draw_indexed(this, count, 1, start, base_vertex, 0); }
static WINAPI void ctx_draw_indexed_instanced(void *this, uint32_t count, uint32_t instances, uint32_t start, int32_t base_vertex, uint32_t start_instance) {
    draw_indexed(this, count, instances, start, base_vertex, start_instance);
}


// ---- compute ---------------------------------------------------------------------------------------------------------------------
static WINAPI void ctx_dispatch(void *this, uint32_t x, uint32_t y, uint32_t z) {
    d3d_context *c = this;
    HELD_OP(1);
    c->unsent++;
    d3d_draw_state *s = state_of(c);
    d3d_shader *cs = s->shader[STAGE_CS];
    if (!cs || !x || !y || !z) return;
    if (!cs->compute) {
        if (!shader_function(cs)) return;              // did not compile
        NSError *err = nil;
        id<MTLComputePipelineState> pso = [(__bridge id<MTLDevice>)c->base.dev->mtl newComputePipelineStateWithFunction:shader_function(cs) error:&err];
        if (!pso) { vx_log("Direct3D: compute pipeline failed: %s", err.localizedDescription.UTF8String); return; }
        cs->compute = (void *)CFBridgingRetain(pso);
    }
    d3d_end_render(c);
    id<MTLComputeCommandEncoder> enc;
    if (prof_on(c->base.dev)) {                         // VXRT_GPU_PROFILE: the dispatch timed
        char sig[120]; snprintf(sig, sizeof sig, "compute cs %08x", cs->hash);
        uint32_t smp = prof_take(sig);
        MTLComputePassDescriptor *cd = [MTLComputePassDescriptor computePassDescriptor];
        cd.sampleBufferAttachments[0].sampleBuffer = g_prof_buf;
        cd.sampleBufferAttachments[0].startOfEncoderSampleIndex = smp;
        cd.sampleBufferAttachments[0].endOfEncoderSampleIndex = smp + 1;
        enc = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) computeCommandEncoderWithDescriptor:cd];
    } else enc = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) computeCommandEncoder];
    [enc setComputePipelineState:(__bridge id<MTLComputePipelineState>)cs->compute];
    bind_stage(c, s, STAGE_CS, nil, enc);
    const uint32_t *t = cs->info.threads;
    [enc dispatchThreadgroups:MTLSizeMake(x, y, z) threadsPerThreadgroup:MTLSizeMake(t[0] ? t[0] : 1, t[1] ? t[1] : 1, t[2] ? t[2] : 1)];
    [enc endEncoding];
}
// ClearUnorderedAccessView: buffers are filled on the GPU (in order with the other work); a value whose four bytes
// differ is written as words after the GPU work so far has finished.
static void clear_uav(d3d_context *c, d3d_view *v, uint32_t value) {
    if (!v || v->resource->kind != K_BUFFER) { d3d_not_implemented("ID3D11DeviceContext::ClearUnorderedAccessView (textures)"); return; }
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)((d3d_buffer *)v->resource)->mtl;
    NSUInteger off = (NSUInteger)v->first * 4, len = v->count ? (NSUInteger)v->count * 4 : b.length - MIN(off, b.length);
    if (off + len > b.length) len = b.length - MIN(off, b.length);
    uint8_t byte = (uint8_t)value;
    if (value == byte * 0x01010101u) {
        d3d_end_render(c);
        id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) blitCommandEncoder];
        [blit fillBuffer:b range:NSMakeRange(off, len) value:byte];
        [blit endEncoding];
        return;
    }
    d3d_flush(c->base.dev, 1);
    uint32_t *w = (uint32_t *)((uint8_t *)b.contents + off);
    for (NSUInteger i = 0; i < len / 4; i++) w[i] = value;
}
// CopyStructureCount: a UAV's counter into a buffer, on the GPU in order with the other work.
static WINAPI void ctx_copy_structure_count(void *this, d3d_buffer *dst, uint32_t offset, d3d_view *v) {
    d3d_context *c = this;
    if (!dst || !v) return;
    d3d_end_render(c);
    id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) blitCommandEncoder];
    [blit copyFromBuffer:counter_of(v) sourceOffset:0 toBuffer:(__bridge id<MTLBuffer>)dst->mtl destinationOffset:dst->off + offset size:4];
    [blit endEncoding];
    d3d_mark_gpu(c, &dst->base);
}
static WINAPI void ctx_clear_uav_uint(void *this, d3d_view *v, const uint32_t *values) { clear_uav(this, v, values[0]); }
static WINAPI void ctx_clear_uav_float(void *this, d3d_view *v, const float *values) { uint32_t bits; memcpy(&bits, &values[0], 4); clear_uav(this, v, bits); }

// ---- tables -------------------------------------------------------------------------------------------------------------------------
// ---- getting the bindings back (each with a reference, as Direct3D gives them) ---------------------------------------------
static void *held(void *o) { if (o) d3d_addref(o); return o; }
static void get_list(void *const *from, uint32_t start, uint32_t n, uint32_t limit, void **out) {
    for (uint32_t i = 0; i < n; i++) out[i] = start + i < limit ? held(from[start + i]) : NULL;
}
#define STAGE_GETTERS(P, st) \
    static WINAPI void P##_get_shader(void *this, void **sh, void **ci, uint32_t *n) { (void)ci; *sh = held(state_of(this)->shader[st]); if (n) *n = 0; } \
    static WINAPI void P##_get_cbs(void *this, uint32_t start, uint32_t n, void **out) { get_list((void *const *)state_of(this)->cb[st], start, n, 14, out); } \
    static WINAPI void P##_get_srvs(void *this, uint32_t start, uint32_t n, void **out) { get_list((void *const *)state_of(this)->srv[st], start, n, 32, out); } \
    static WINAPI void P##_get_samplers(void *this, uint32_t start, uint32_t n, void **out) { get_list((void *const *)state_of(this)->smp[st], start, n, 16, out); }
STAGE_GETTERS(vs, STAGE_VS)
STAGE_GETTERS(ps, STAGE_PS)
STAGE_GETTERS(cs, STAGE_CS)
STAGE_GETTERS(gs, STAGE_GS)
// The stages not translated yet (geometry, hull, domain) have nothing bound.
static WINAPI void none_get_shader(void *this, void **sh, void **ci, uint32_t *n) { (void)this; (void)ci; *sh = NULL; if (n) *n = 0; }
static WINAPI void none_get_list(void *this, uint32_t start, uint32_t n, void **out) { (void)this; (void)start; for (uint32_t i = 0; i < n; i++) out[i] = NULL; }
static WINAPI void none_set_list(void *this, uint32_t start, uint32_t n, void *const *in) { (void)this; (void)start; (void)n; (void)in; }
static WINAPI void cs_get_uavs(void *this, uint32_t start, uint32_t n, void **out) { get_list((void *const *)state_of(this)->uav[STAGE_CS], start, n, 8, out); }
static WINAPI void om_get_blend(void *this, void **b, float *factor, uint32_t *mask) {
    d3d_draw_state *s = state_of(this);
    if (b) *b = held(s->blend);
    if (factor) memcpy(factor, s->blend_factor, sizeof s->blend_factor);
    if (mask) *mask = 0xffffffffu;
}
static WINAPI void om_get_depth(void *this, void **d, uint32_t *ref) { d3d_draw_state *s = state_of(this); if (d) *d = held(s->depth); if (ref) *ref = s->stencil_ref; }
typedef void (WINAPI *get_targets_fn)(void *, uint32_t, void **, void **);
static WINAPI void om_get_targets_and_uavs(void *this, uint32_t nrtv, void **rtvs, void **dsv, uint32_t start, uint32_t nuav, void **uavs) {
    get_targets_fn get = (get_targets_fn)self_of(this)->vtbl[89];   // OMGetRenderTargets
    if (rtvs || dsv) get(this, nrtv, rtvs, dsv);
    if (uavs) get_list((void *const *)state_of(this)->uav[STAGE_PS], start, nuav, 8, uavs);
}
static WINAPI void ia_get_index_buffer(void *this, void **b, uint32_t *format, uint32_t *offset) {
    d3d_draw_state *s = state_of(this);
    if (b) *b = held(s->ib); if (format) *format = s->ib_format; if (offset) *offset = s->ib_offset;
}
static WINAPI void ia_get_layout(void *this, void **l) { *l = held(state_of(this)->layout); }
static WINAPI void ia_get_topology(void *this, uint32_t *t) { *t = state_of(this)->topology; }
static WINAPI void ia_get_vertex_buffers(void *this, uint32_t start, uint32_t n, void **b, uint32_t *strides, uint32_t *offsets) {
    d3d_draw_state *s = state_of(this);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = start + i;
        if (b) b[i] = k < 16 ? held(s->vb[k]) : NULL;
        if (strides) strides[i] = k < 16 ? s->stride[k] : 0;
        if (offsets) offsets[i] = k < 16 ? s->offset[k] : 0;
    }
}
static WINAPI void rs_get_scissors(void *this, uint32_t *n, RECT *r) {
    if (r && *n) memcpy(r, state_of(this)->scissor, sizeof *r * MIN(*n, MAX(state_of(this)->nscissors, 1)));
    *n = MAX(state_of(this)->nscissors, 1);
}
static WINAPI void rs_get_state(void *this, void **r) { *r = held(state_of(this)->raster); }
static WINAPI void so_get_targets(void *this, uint32_t n, void **out) { for (uint32_t i = 0; i < n; i++) out[i] = i < 4 ? held(state_of(this)->so[i]) : NULL; }
// An offset of -1 appends to what earlier stream output wrote to the buffer.
static WINAPI void so_set_targets(void *this, uint32_t n, d3d_buffer *const *targets, const uint32_t *offsets) {
    d3d_draw_state *s = state_of(this);
    for (uint32_t i = 0; i < 4; i++) {
        d3d_buffer *b = targets && i < n ? targets[i] : NULL;
        bind(&s->so[i], b);
        if (b && offsets && offsets[i] != ~0u) b->so_filled = offsets[i];
    }
}
// ResolveSubresource: a render pass that resolves the multisampled texture into the other.
static WINAPI void ctx_resolve(void *this, d3d_object *dst, uint32_t dst_sub, d3d_object *src, uint32_t src_sub, uint32_t format) {
    (void)format;
    d3d_context *c = this;
    HELD_OP(4);
    if (!dst || !src || dst->kind != K_TEXTURE || src->kind != K_TEXTURE) return;
    d3d_texture *ds = (d3d_texture *)dst, *ss = (d3d_texture *)src;
    id<MTLTexture> st = (__bridge id<MTLTexture>)ss->mtl, dt = (__bridge id<MTLTexture>)ds->mtl;
    if (st.sampleCount <= 1) {                                   // nothing to resolve: a copy
        d3d_end_render(c);
        id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) blitCommandEncoder];
        [blit copyFromTexture:st toTexture:dt];
        [blit endEncoding];
        d3d_mark_gpu(c, dst);
        d3d_shared_written(c->base.dev, ds);
        return;
    }
    uint32_t dmips = ds->desc[TD_MIPS] ? ds->desc[TD_MIPS] : 1, smips = ss->desc[TD_MIPS] ? ss->desc[TD_MIPS] : 1;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    int depth = st.pixelFormat == MTLPixelFormatDepth32Float || st.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 || st.pixelFormat == MTLPixelFormatDepth16Unorm;
    MTLRenderPassAttachmentDescriptor *a = depth ? (MTLRenderPassAttachmentDescriptor *)rp.depthAttachment : rp.colorAttachments[0];
    a.texture = st; a.slice = src_sub / smips;
    a.resolveTexture = dt; a.resolveLevel = dst_sub % dmips; a.resolveSlice = dst_sub / dmips;
    a.loadAction = MTLLoadActionLoad; a.storeAction = MTLStoreActionMultisampleResolve;
    d3d_end_render(c);
    if (prof_on(c->base.dev)) {                         // VXRT_GPU_PROFILE: the resolve timed
        char sig[120]; snprintf(sig, sizeof sig, "resolve %lux%lu f%lu x%lu", (unsigned long)st.width, (unsigned long)st.height, (unsigned long)st.pixelFormat, (unsigned long)st.sampleCount);
        uint32_t smp = prof_take(sig);
        rp.sampleBufferAttachments[0].sampleBuffer = g_prof_buf;
        rp.sampleBufferAttachments[0].startOfVertexSampleIndex = smp;
        rp.sampleBufferAttachments[0].endOfVertexSampleIndex = MTLCounterDontSample;
        rp.sampleBufferAttachments[0].startOfFragmentSampleIndex = MTLCounterDontSample;
        rp.sampleBufferAttachments[0].endOfFragmentSampleIndex = smp + 1;
    }
    id<MTLRenderCommandEncoder> enc = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) renderCommandEncoderWithDescriptor:rp];
    [enc endEncoding];
    d3d_mark_gpu(c, dst);
    d3d_shared_written(c->base.dev, ds);
}

// ---- queries and predicates -------------------------------------------------------------------------------------------------
// D3D11_QUERY_DESC: type, flags. Results are ready once the GPU has finished the command buffer that was being
// recorded when the query ended. Occlusion counts report everything visible (nothing is culled wrongly);
// timestamps are when the GPU finished the work recorded before them (the command buffer is sent at the query, and
// its end time taken; the host clock in ns, as QueryPerformanceCounter, at a 1 GHz frequency): SteamVR's compositor
// and Alyx time their GPU work with them (CPU times made the compositor misjudge when to start composing).
typedef struct { d3d_object base; uint32_t desc[2]; uint64_t target, stamp, issued; int ended; } d3d_query;
static void **g_query_vtbl, **g_predicate_vtbl;
static WINAPI HRESULT query_qi(void *this, const GUID *iid, void **out) {
    d3d_query *q = this;
    if (out && (d3d_same_guid(iid, &IID_ID3D11Asynchronous) || d3d_same_guid(iid, &IID_ID3D11Query) ||
                (q->desc[0] == 5 && d3d_same_guid(iid, &IID_ID3D11Predicate)))) { d3d_addref(&q->base); *out = this; return S_OK; }
    return query(this, iid, out, &IID_ID3D11Query);
}
static uint32_t query_size(uint32_t type) {
    switch (type) {
    case 0: case 5: case 7: return 4;                     // event, occlusion predicate, SO overflow predicate: BOOL
    case 1: case 2: return 8;                             // occlusion samples, timestamp
    case 3: case 6: return 16;                            // timestamp disjoint, SO statistics
    case 4: return 88;                                    // pipeline statistics
    default: return 4;
    }
}
static WINAPI uint32_t query_data_size(void *this) { return query_size(((d3d_query *)this)->desc[0]); }
static WINAPI void query_get_desc(void *this, uint32_t *desc) { memcpy(desc, ((d3d_query *)this)->desc, 8); }
static HRESULT make_query(void *this, const uint32_t *desc, void **out, int predicate) {
    if (!desc) return E_INVALIDARG;
    if (!out) return S_FALSE;
    d3d_query *q = d3d_alloc(sizeof *q, K_STATE, predicate ? g_predicate_vtbl : g_query_vtbl, (d3d_device *)this);
    q->desc[0] = desc[0]; q->desc[1] = desc[1];
    *out = q;
    return S_OK;
}
static WINAPI HRESULT device_create_query(void *this, const uint32_t *desc, void **out) { return make_query(this, desc, out, 0); }
static WINAPI HRESULT device_create_predicate(void *this, const uint32_t *desc, void **out) { return make_query(this, desc, out, 1); }
static WINAPI HRESULT device_create_counter(void *this, const void *desc, void **out) { (void)this; (void)desc; if (out) *out = NULL; return E_INVALIDARG; }
static WINAPI void device_check_counter_info(void *this, uint32_t *info) { (void)this; memset(info, 0, 12); }
static WINAPI void ctx_begin(void *this, d3d_query *q) { (void)this; if (q) q->ended = 0; }
static WINAPI void ctx_end(void *this, d3d_query *q) {
    d3d_context *c = this;
    if (!q) return;
    d3d_context *imm = (d3d_context *)c->base.dev->context;
    // The command buffer being recorded, or (none open) the work already committed: an event query after a
    // flush must not wait for work that may never come.
    q->target = __atomic_load_n(&c->base.dev->submitted, __ATOMIC_ACQUIRE) + (imm->cmd ? 1 : 0);
    static int stats = -1;                               // VXRT_STATS: queries ended, by type, every 2000
    static uint64_t ends[8], all;
    if (stats < 0) stats = getenv("VXRT_STATS") != NULL;
    if (stats) {
        ends[q->desc[0] & 7]++;
        if (++all % 2000 == 0) vx_log("queries ended: %llu event, %llu occlusion, %llu timestamp, %llu disjoint, %llu other",
                                      (unsigned long long)ends[0], (unsigned long long)ends[1], (unsigned long long)ends[2], (unsigned long long)ends[3],
                                      (unsigned long long)(all - ends[0] - ends[1] - ends[2] - ends[3]));
    }
    q->stamp = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);     // (the host clock, as QueryPerformanceCounter)
    if (q->desc[0] == 2 && c == imm && getenv("VXRT_GPU_TIMESTAMPS")) {   // (being measured: CPU times by default)
        if (imm->cmd) d3d_flush(c->base.dev, 0);             // the work so far, in a command buffer of its own
        q->target = __atomic_load_n(&c->base.dev->submitted, __ATOMIC_ACQUIRE);
        q->issued = q->stamp;                                 // (the GPU cannot reach it earlier)
        q->stamp = 0;                                         // (taken from that buffer's end when it is done)
    }
    q->ended = 1;
}
#define D3D11_ASYNC_GETDATA_DONOTFLUSH 1
static WINAPI HRESULT ctx_get_data(void *this, d3d_query *q, void *data, uint32_t size, uint32_t flags) {
    d3d_context *c = this;
    if (!q) return E_INVALIDARG;
    if (q->desc[0] == 3) {                                    // disjoint: always ready
        if (data && size >= 16) { uint64_t f = 1000000000ull; memcpy(data, &f, 8); memset((uint8_t *)data + 8, 0, 8); }
        return S_OK;
    }
    if (!q->ended) return S_FALSE;
    d3d_device *dev = c->base.dev;
    if (__atomic_load_n(&dev->completed, __ATOMIC_ACQUIRE) < q->target) {
        if (!(flags & D3D11_ASYNC_GETDATA_DONOTFLUSH) && dev->submitted < q->target) d3d_flush(dev, 0);
        if (__atomic_load_n(&dev->completed, __ATOMIC_ACQUIRE) < q->target) return S_FALSE;
    }
    if (!data || !size) return S_OK;
    uint8_t out[88] = { 0 };
    switch (q->desc[0]) {
    case 0: case 5: *(uint32_t *)out = 1; break;
    case 1: *(uint64_t *)out = 1000000; break;
    case 2:
        if (!q->stamp) {
            q->stamp = q->target ? d3d_gpu_end_ns(dev, q->target) : 0;
            if (!q->stamp) q->stamp = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);   // (long gone from the ring, or nothing ran yet)
            if (q->stamp < q->issued) q->stamp = q->issued;   // the GPU was idle: it got there when it was issued
        }
        *(uint64_t *)out = q->stamp; break;
    }
    memcpy(data, out, MIN(size, query_size(q->desc[0])));
    return S_OK;
}
static WINAPI void ctx_set_predication(void *this, void *p, int value) { (void)this; (void)p; (void)value; }   // draws are never skipped
static WINAPI void ctx_get_predication(void *this, void **p, int *value) { (void)this; if (p) *p = NULL; if (value) *value = 0; }
// Geometry, hull and domain shaders are not translated yet: unbinding them is fine; binding one is reported.
static WINAPI void ctx_set_unsupported_stage(void *this, void *shader, void *const *ci, uint32_t n) {
    (void)this; (void)ci; (void)n;
    if (shader) d3d_not_implemented("geometry/tessellation shader stages (bound)");
}

// Geometry shaders run as compute kernels (see dxbc.h). With VXRT_DUMP_SHADERS their DXBC is also written out
// (gs_<hash>.dxbc).
static WINAPI HRESULT device_create_gs(void *this, const void *code, size_t size, void *linkage, void **out) {
    (void)linkage;
    const char *dir = getenv("VXRT_DUMP_SHADERS");
    if (dir && *dir && code) {
        unsigned char md[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(code, (CC_LONG)size, md);
        char path[1024];
        snprintf(path, sizeof path, "%s/gs_%02x%02x%02x%02x.dxbc", dir, md[0], md[1], md[2], md[3]);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(code, 1, size, f); fclose(f); }
    }
    if (getenv("VXRT_NO_GEOMETRY_SHADERS")) { if (out) *out = NULL; return E_NOTIMPL; }   // (for comparisons)
    return create_shader(this, code, size, DXBC_GEOMETRY, g_gs_vtbl, out);
}
// A geometry shader with stream output. Games use it mostly with a vertex shader's code (no geometry stage: the
// vertex shader's outputs go to the buffers, as Unity skins meshes); stream output from a real geometry shader is
// not implemented (its triangles are still drawn when stream 0 is rasterized).
typedef struct { uint32_t stream; const char *semantic; uint32_t index; uint8_t start, count, slot; } so_decl;   // D3D11_SO_DECLARATION_ENTRY
static WINAPI HRESULT device_create_gs_so(void *this, const void *code, size_t size, const so_decl *decl, uint32_t ndecl,
                                          const uint32_t *strides, uint32_t nstrides, uint32_t rasterized, void *linkage, void **out) {
    (void)linkage;
    int type = code ? dxbc_program_type(code, size) : -1;
    if (type != DXBC_VERTEX && type != DXBC_GEOMETRY) return E_INVALIDARG;
    if (type == DXBC_GEOMETRY) d3d_not_implemented("stream output from a geometry shader");
    HRESULT hr = create_shader(this, code, size, type, g_gs_vtbl, out);
    if (hr != S_OK || !out) return hr;
    d3d_shader *sh = *out;
    so_layout *so = calloc(1, sizeof *so);
    so->rasterized = rasterized == 0;
    uint32_t dwords[4] = { 0 };
    for (uint32_t i = 0; i < ndecl; i++) {
        const so_decl *d = &decl[i];
        if (d->stream || d->slot >= 4) continue;
        if (d->semantic && so->n < 64) {                             // (no name: a gap of count components)
            const dxbc_element *el = NULL;
            for (int k = 0; k < sh->info.noutputs && !el; k++)
                if (!strcasecmp(sh->info.outputs[k].semantic, d->semantic) && sh->info.outputs[k].index == d->index) el = &sh->info.outputs[k];
            if (el) {
                int first = 0;
                while (first < 4 && !(el->mask & (1 << first))) first++;
                so->e[so->n++] = (so_entry){ el->reg, (uint32_t)(first + d->start), d->count, (uint32_t)d->slot << 24 | dwords[d->slot] };
            } else if (vx_trace) vx_log("Direct3D: stream output of %s%u, which the shader does not write", d->semantic, d->index);
        }
        dwords[d->slot] += d->count;
    }
    for (int k = 0; k < 4; k++) so->stride[k] = (k < (int)nstrides && strides[k] ? strides[k] : dwords[k] * 4) / 4;   // (in dwords)
    if (vx_trace) vx_log("Direct3D: stream output shader %08x (%s): %u entries, strides %u %u, rasterized stream %d", sh->hash,
                         type == DXBC_VERTEX ? "vertex" : "geometry", so->n, so->stride[0] * 4, so->stride[1] * 4, (int)rasterized);
    sh->so = so;
    return S_OK;
}
const d3d_method d3d_draw_device_methods[] = {
    { "CreateGeometryShader", (void *)device_create_gs }, { "CreateGeometryShaderWithStreamOutput", (void *)device_create_gs_so },
    { "CreateVertexShader", (void *)device_create_vs }, { "CreatePixelShader", (void *)device_create_ps },
    { "CreateInputLayout", (void *)device_create_layout }, { "CreateBlendState", (void *)device_create_blend },
    { "CreateDepthStencilState", (void *)device_create_depth }, { "CreateRasterizerState", (void *)device_create_raster },
    { "CreateSamplerState", (void *)device_create_sampler }, { "CreateComputeShader", (void *)device_create_cs },
    { "CreateUnorderedAccessView", (void *)device_create_uav }, { "CreateQuery", (void *)device_create_query },
    { "CreatePredicate", (void *)device_create_predicate }, { "CreateCounter", (void *)device_create_counter },
    { "CheckCounterInfo", (void *)device_check_counter_info }, { NULL, NULL },
};
// GenerateMips: the view's first mip filtered down into the rest of its mips (a blit; Metal picks the filter).
static WINAPI void ctx_generate_mips(void *this, d3d_view *v) {
    d3d_context *c = this;
    if (!v || v->resource->kind != K_TEXTURE) return;
    id<MTLTexture> t = shader_texture(v);
    if (!t || t.mipmapLevelCount < 2) return;
    d3d_end_render(c);
    id<MTLBlitCommandEncoder> blit = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) blitCommandEncoder];
    [blit generateMipmapsForTexture:t];
    [blit endEncoding];
}
const d3d_method d3d_draw_context_methods[] = {
    { "GenerateMips", (void *)ctx_generate_mips },
    { "IASetInputLayout", (void *)ia_set_layout }, { "IASetVertexBuffers", (void *)ia_set_vertex_buffers },
    { "IASetIndexBuffer", (void *)ia_set_index_buffer }, { "IASetPrimitiveTopology", (void *)ia_set_topology },
    { "VSSetShader", (void *)vs_set_shader }, { "PSSetShader", (void *)ps_set_shader },
    { "VSSetConstantBuffers", (void *)vs_set_cbs }, { "PSSetConstantBuffers", (void *)ps_set_cbs },
    { "VSSetShaderResources", (void *)vs_set_srvs }, { "PSSetShaderResources", (void *)ps_set_srvs },
    { "VSSetSamplers", (void *)vs_set_samplers }, { "PSSetSamplers", (void *)ps_set_samplers },
    { "OMSetBlendState", (void *)om_set_blend }, { "OMSetDepthStencilState", (void *)om_set_depth },
    { "RSSetState", (void *)rs_set_state }, { "RSSetScissorRects", (void *)rs_set_scissors },
    { "Draw", (void *)ctx_draw }, { "DrawInstanced", (void *)ctx_draw_instanced },
    { "DrawIndexed", (void *)ctx_draw_indexed }, { "DrawIndexedInstanced", (void *)ctx_draw_indexed_instanced },
    { "CSSetShader", (void *)cs_set_shader }, { "CSSetConstantBuffers", (void *)cs_set_cbs }, { "CSSetShaderResources", (void *)cs_set_srvs },
    { "CSSetSamplers", (void *)cs_set_samplers }, { "CSSetUnorderedAccessViews", (void *)cs_set_uavs }, { "Dispatch", (void *)ctx_dispatch },
    { "OMSetRenderTargetsAndUnorderedAccessViews", (void *)om_set_targets_and_uavs },
    { "Begin", (void *)ctx_begin }, { "End", (void *)ctx_end }, { "GetData", (void *)ctx_get_data },
    { "VSGetShader", (void *)vs_get_shader }, { "VSGetConstantBuffers", (void *)vs_get_cbs }, { "VSGetShaderResources", (void *)vs_get_srvs },
    { "VSGetSamplers", (void *)vs_get_samplers }, { "PSGetShader", (void *)ps_get_shader }, { "PSGetConstantBuffers", (void *)ps_get_cbs },
    { "PSGetShaderResources", (void *)ps_get_srvs }, { "PSGetSamplers", (void *)ps_get_samplers }, { "CSGetShader", (void *)cs_get_shader },
    { "CSGetConstantBuffers", (void *)cs_get_cbs }, { "CSGetShaderResources", (void *)cs_get_srvs }, { "CSGetSamplers", (void *)cs_get_samplers },
    { "CSGetUnorderedAccessViews", (void *)cs_get_uavs },
    { "GSGetShader", (void *)gs_get_shader }, { "HSGetShader", (void *)none_get_shader }, { "DSGetShader", (void *)none_get_shader },
    { "GSGetConstantBuffers", (void *)gs_get_cbs }, { "GSGetShaderResources", (void *)gs_get_srvs }, { "GSGetSamplers", (void *)gs_get_samplers },
    { "HSGetConstantBuffers", (void *)none_get_list }, { "HSGetShaderResources", (void *)none_get_list }, { "HSGetSamplers", (void *)none_get_list },
    { "DSGetConstantBuffers", (void *)none_get_list }, { "DSGetShaderResources", (void *)none_get_list }, { "DSGetSamplers", (void *)none_get_list },
    { "GSSetConstantBuffers", (void *)gs_set_cbs }, { "GSSetShaderResources", (void *)gs_set_srvs }, { "GSSetSamplers", (void *)gs_set_samplers },
    { "HSSetConstantBuffers", (void *)none_set_list }, { "HSSetShaderResources", (void *)none_set_list }, { "HSSetSamplers", (void *)none_set_list },
    { "DSSetConstantBuffers", (void *)none_set_list }, { "DSSetShaderResources", (void *)none_set_list }, { "DSSetSamplers", (void *)none_set_list },
    { "OMGetBlendState", (void *)om_get_blend }, { "OMGetDepthStencilState", (void *)om_get_depth },
    { "OMGetRenderTargetsAndUnorderedAccessViews", (void *)om_get_targets_and_uavs }, { "IAGetIndexBuffer", (void *)ia_get_index_buffer },
    { "IAGetInputLayout", (void *)ia_get_layout }, { "IAGetPrimitiveTopology", (void *)ia_get_topology },
    { "IAGetVertexBuffers", (void *)ia_get_vertex_buffers }, { "RSGetScissorRects", (void *)rs_get_scissors }, { "RSGetState", (void *)rs_get_state },
    { "SOGetTargets", (void *)so_get_targets }, { "SOSetTargets", (void *)so_set_targets }, { "ResolveSubresource", (void *)ctx_resolve },
    { "SetPredication", (void *)ctx_set_predication }, { "GetPredication", (void *)ctx_get_predication },
    { "GSSetShader", (void *)gs_set_shader }, { "HSSetShader", (void *)ctx_set_unsupported_stage },
    { "DSSetShader", (void *)ctx_set_unsupported_stage },
    { "CopyStructureCount", (void *)ctx_copy_structure_count }, { "ClearUnorderedAccessViewUint", (void *)ctx_clear_uav_uint }, { "ClearUnorderedAccessViewFloat", (void *)ctx_clear_uav_float },
    { NULL, NULL },
};
void d3d_draw_init(void) {
    static const d3d_method vs[] = { { "QueryInterface", (void *)vs_qi }, CHILD_METHODS, { NULL, NULL } };
    static const d3d_method ps[] = { { "QueryInterface", (void *)ps_qi }, CHILD_METHODS, { NULL, NULL } };
    static const d3d_method cs[] = { { "QueryInterface", (void *)cs_qi }, CHILD_METHODS, { NULL, NULL } };
    static const d3d_method gs[] = { { "QueryInterface", (void *)gs_qi }, CHILD_METHODS, { NULL, NULL } };
    static const d3d_method uav[] = { { "QueryInterface", (void *)uav_qi }, CHILD_METHODS, { "GetResource", (void *)uav_get_resource }, { "GetDesc", (void *)uav_get_desc }, { NULL, NULL } };
    static const d3d_method layout[] = { { "QueryInterface", (void *)layout_qi }, CHILD_METHODS, { NULL, NULL } };
    static const d3d_method blend[] = { { "QueryInterface", (void *)blend_qi }, CHILD_METHODS, { "GetDesc", (void *)blend_get_desc }, { NULL, NULL } };
    static const d3d_method depth[] = { { "QueryInterface", (void *)depth_qi }, CHILD_METHODS, { "GetDesc", (void *)depth_get_desc }, { NULL, NULL } };
    static const d3d_method raster[] = { { "QueryInterface", (void *)raster_qi }, CHILD_METHODS, { "GetDesc", (void *)raster_get_desc }, { NULL, NULL } };
    static const d3d_method sampler[] = { { "QueryInterface", (void *)sampler_qi }, CHILD_METHODS, { "GetDesc", (void *)sampler_get_desc }, { NULL, NULL } };
    g_vs_vtbl = D3D_VTABLE(ID3D11VertexShader, vs);
    g_ps_vtbl = D3D_VTABLE(ID3D11PixelShader, ps);
    g_cs_vtbl = D3D_VTABLE(ID3D11ComputeShader, cs);
    g_gs_vtbl = D3D_VTABLE(ID3D11GeometryShader, gs);
    g_uav_vtbl = D3D_VTABLE(ID3D11UnorderedAccessView, uav);
    g_layout_vtbl = D3D_VTABLE(ID3D11InputLayout, layout);
    g_blend_vtbl = D3D_VTABLE(ID3D11BlendState, blend);
    g_depth_vtbl = D3D_VTABLE(ID3D11DepthStencilState, depth);
    g_raster_vtbl = D3D_VTABLE(ID3D11RasterizerState, raster);
    g_sampler_vtbl = D3D_VTABLE(ID3D11SamplerState, sampler);
    static const d3d_method q[] = { { "QueryInterface", (void *)query_qi }, CHILD_METHODS, { "GetDataSize", (void *)query_data_size },
                                    { "GetDesc", (void *)query_get_desc }, { NULL, NULL } };
    g_query_vtbl = D3D_VTABLE(ID3D11Query, q);
    g_predicate_vtbl = D3D_VTABLE(ID3D11Predicate, q);
}

#ifdef __aarch64__
// vCPU: immediate-context calls the guest may record and the host runs at the guest's next exit (src/arm64.c,
// translator/guest.c): no result, arrays copied when called. (Get* and Map answer: they are called, after the batch.)
#include "arm64.h"
static const vx_defer g_deferred_draw[] = {
    { ia_set_layout, 2, 0, {{0}} }, { ia_set_index_buffer, 4, 0, {{0}} }, { ia_set_topology, 2, 0, {{0}} },
    { ia_set_vertex_buffers, 6, 3, { VX_COPY(3, 2, 8, 32), VX_COPY(4, 2, 4, 32), VX_COPY(5, 2, 4, 32) } },
    { vs_set_shader, 4, 1, { VX_COPY(2, 3, 8, 256) } }, { ps_set_shader, 4, 1, { VX_COPY(2, 3, 8, 256) } },
    { gs_set_shader, 4, 1, { VX_COPY(2, 3, 8, 256) } }, { cs_set_shader, 4, 1, { VX_COPY(2, 3, 8, 256) } },
    { vs_set_cbs, 4, 1, { VX_COPY(3, 2, 8, 128) } }, { ps_set_cbs, 4, 1, { VX_COPY(3, 2, 8, 128) } },
    { gs_set_cbs, 4, 1, { VX_COPY(3, 2, 8, 128) } }, { cs_set_cbs, 4, 1, { VX_COPY(3, 2, 8, 128) } },
    { vs_set_srvs, 4, 1, { VX_COPY(3, 2, 8, 128) } }, { ps_set_srvs, 4, 1, { VX_COPY(3, 2, 8, 128) } },
    { gs_set_srvs, 4, 1, { VX_COPY(3, 2, 8, 128) } }, { cs_set_srvs, 4, 1, { VX_COPY(3, 2, 8, 128) } },
    { vs_set_samplers, 4, 1, { VX_COPY(3, 2, 8, 128) } }, { ps_set_samplers, 4, 1, { VX_COPY(3, 2, 8, 128) } },
    { gs_set_samplers, 4, 1, { VX_COPY(3, 2, 8, 128) } }, { cs_set_samplers, 4, 1, { VX_COPY(3, 2, 8, 128) } },
    { cs_set_uavs, 5, 2, { VX_COPY(3, 2, 8, 64), VX_COPY(4, 2, 4, 64) } },
    { om_set_targets_and_uavs, 8, 3, { VX_COPY(2, 1, 8, 8), VX_COPY(6, 5, 8, 64), VX_COPY(7, 5, 4, 64) } },
    { om_set_blend, 4, 1, { VX_COPY_N(2, 4, 4) } }, { om_set_depth, 3, 0, {{0}} }, { rs_set_state, 2, 0, {{0}} },
    { rs_set_scissors, 3, 1, { VX_COPY(2, 1, 16, 16) } },
    { ctx_draw, 3, 0, {{0}} }, { ctx_draw_instanced, 5, 0, {{0}} }, { ctx_draw_indexed, 4, 0, {{0}} },
    { ctx_draw_indexed_instanced, 6, 0, {{0}} }, { ctx_dispatch, 4, 0, {{0}} },
    { so_set_targets, 4, 2, { VX_COPY(2, 1, 8, 4), VX_COPY(3, 1, 4, 4) } },
    { ctx_begin, 2, 0, {{0}} }, { ctx_end, 2, 0, {{0}} }, { ctx_generate_mips, 2, 0, {{0}} },
};
__attribute__((constructor)) static void deferred_draw_add(void) { vx_defer_add(g_deferred_draw, sizeof g_deferred_draw / sizeof *g_deferred_draw); }
#endif

#ifdef __aarch64__
#include "thunks/d3d11_draw.m.inc"
#endif
