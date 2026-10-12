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
static void **g_vs_vtbl, **g_ps_vtbl, **g_cs_vtbl, **g_uav_vtbl, **g_layout_vtbl, **g_blend_vtbl, **g_depth_vtbl, **g_raster_vtbl, **g_sampler_vtbl;

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
typedef struct { d3d_object base; dxbc_shader info; void *function, *compute; uint32_t id; } d3d_shader;
static uint32_t g_next_id = 1;
static void shader_destroy(d3d_object *o) {
    d3d_shader *s = (d3d_shader *)o;
    if (s->function) CFBridgingRelease(s->function);
    if (s->compute) CFBridgingRelease(s->compute);
    dxbc_free(&s->info);
}
static WINAPI HRESULT vs_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11VertexShader); }
static WINAPI HRESULT ps_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11PixelShader); }
static WINAPI HRESULT cs_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11ComputeShader); }
static WINAPI HRESULT uav_qi(void *this, const GUID *iid, void **out) { return query(this, iid, out, &IID_ID3D11UnorderedAccessView); }
static WINAPI void uav_get_resource(void *this, void **out) { d3d_view *v = this; d3d_addref(v->resource); *out = v->resource; }
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
static HRESULT create_shader(d3d_device *dev, const void *code, size_t size, int want, void **vtbl, void **out) {
    d3d_shader *s = d3d_alloc(sizeof *s, K_SHADER, vtbl, dev);
    s->base.destroy = shader_destroy;
    if (dxbc_translate(code, size, &s->info) || s->info.type != want) {
        vx_log("Direct3D: shader not translated: %s", s->info.error[0] ? s->info.error : "wrong shader type");
        d3d_release(&s->base);
        return E_INVALIDARG;
    }
    dump(&s->info, code, size);
    NSError *err = nil;
    id<MTLLibrary> lib = [(__bridge id<MTLDevice>)dev->mtl newLibraryWithSource:@(s->info.msl) options:nil error:&err];
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"vx_main"] : nil;
    if (!fn) {
        vx_log("Direct3D: translated shader did not compile: %s", err.localizedDescription.UTF8String);
        if (vx_trace) vx_log("%s", s->info.msl);
        d3d_release(&s->base);
        return E_INVALIDARG;
    }
    s->function = (void *)CFBridgingRetain(fn);
    s->id = __sync_fetch_and_add(&g_next_id, 1);
    if (!out) { d3d_release(&s->base); return S_FALSE; }
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
    if ((filter & 0x7f) == 0x55) sd.maxAnisotropy = d[5] ? MIN(d[5], 16u) : 1;
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
enum { STAGE_VS, STAGE_PS, STAGE_CS, STAGES };
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
    RECT scissor;
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
static WINAPI void rs_set_scissors(void *this, uint32_t n, const RECT *r) { if (n && r) state_of(this)->scissor = r[0]; }

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

static id<MTLRenderPipelineState> pipeline(d3d_context *c, d3d_draw_state *s, id<MTLTexture> const *colors, int ncolors, id<MTLTexture> depth) {
    static NSMutableDictionary *cache;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    d3d_shader *vs = s->shader[STAGE_VS], *ps = s->shader[STAGE_PS];
    NSMutableString *key = [NSMutableString stringWithFormat:@"%u/%u/%u/%u/", vs->id, ps ? ps->id : 0, s->layout ? s->layout->id : 0, s->blend ? s->blend->id : 0];
    for (int i = 0; i < ncolors; i++) [key appendFormat:@"c%lu,%lu/", (unsigned long)(colors[i] ? colors[i].pixelFormat : 0), (unsigned long)(colors[i] ? colors[i].sampleCount : 0)];
    [key appendFormat:@"d%lu/", (unsigned long)(depth ? depth.pixelFormat : 0)];
    if (s->layout) for (uint32_t i = 0; i < s->layout->n; i++) [key appendFormat:@"%u,", s->stride[s->layout->attr[i].slot]];
    pthread_mutex_lock(&lock);
    if (!cache) cache = [NSMutableDictionary new];
    id<MTLRenderPipelineState> ps_state = cache[key];
    pthread_mutex_unlock(&lock);
    if (ps_state) return ps_state;

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = (__bridge id<MTLFunction>)vs->function;
    pd.fragmentFunction = ps ? (__bridge id<MTLFunction>)ps->function : nil;
    if (s->layout && s->layout->n) {
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        for (uint32_t i = 0; i < s->layout->n; i++) {
            const layout_attr *a = &s->layout->attr[i];
            vd.attributes[a->reg].format = vertex_format(a->format, NULL);
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
            ca.sourceRGBBlendFactor = blend_factor(rt[1]); ca.destinationRGBBlendFactor = blend_factor(rt[2]); ca.rgbBlendOperation = blend_op(rt[3]);
            ca.sourceAlphaBlendFactor = blend_factor(rt[4]); ca.destinationAlphaBlendFactor = blend_factor(rt[5]); ca.alphaBlendOperation = blend_op(rt[6]);
            uint8_t m = (uint8_t)rt[7];
            ca.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
            pd.alphaToCoverageEnabled = s->blend->desc[0] != 0;
        }
    }
    if (depth) {
        pd.depthAttachmentPixelFormat = depth.pixelFormat;
        if (depth.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) pd.stencilAttachmentPixelFormat = depth.pixelFormat;
        if (!ncolors) pd.rasterSampleCount = depth.sampleCount;
    }
    NSError *err = nil;
    ps_state = [(__bridge id<MTLDevice>)c->base.dev->mtl newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!ps_state) { vx_log("Direct3D: pipeline state failed: %s", err.localizedDescription.UTF8String); return nil; }
    pthread_mutex_lock(&lock);
    cache[key] = ps_state;
    pthread_mutex_unlock(&lock);
    return ps_state;
}

// ---- drawing ----------------------------------------------------------------------------------------------------------------------
static id<MTLTexture> view_texture(d3d_view *v) { return v && v->resource->kind == K_TEXTURE ? (__bridge id<MTLTexture>)((d3d_texture *)v->resource)->mtl : nil; }

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
    if (fmt == base.pixelFormat && type == base.textureType && first == 0 && count == levels && sfirst == 0 && scount == total) return base;
    id<MTLTexture> view = [base newTextureViewWithPixelFormat:fmt textureType:type levels:NSMakeRange(first, count) slices:NSMakeRange(sfirst, scount)];
    if (!view) return base;
    return cache_view(v, view, tex->mtl);
}

// The render pass for the bound targets: kept open while they stay the same; contents are loaded.
static id<MTLRenderCommandEncoder> render_pass(d3d_context *c) {
    int same = c->render != NULL && c->render_dsv == c->dsv;
    for (int i = 0; i < 8 && same; i++) same = c->render_rtv[i] == c->rtv[i];
    if (same) return (__bridge id<MTLRenderCommandEncoder>)c->render;
    d3d_end_render(c);
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
    if (!any) return nil;
    id<MTLRenderCommandEncoder> enc = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) renderCommandEncoderWithDescriptor:rp];
    c->render = (void *)CFBridgingRetain(enc);
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
static void bind_stage(d3d_context *c, d3d_draw_state *s, int st, id<MTLRenderCommandEncoder> renc, id<MTLComputeCommandEncoder> cenc) {
    d3d_shader *sh = s->shader[st];
    if (!sh) return;
#define SET_BUFFER(b, off, idx) do { if (cenc) [cenc setBuffer:b offset:off atIndex:idx]; \
    else if (st == STAGE_VS) [renc setVertexBuffer:b offset:off atIndex:idx]; else [renc setFragmentBuffer:b offset:off atIndex:idx]; } while (0)
#define SET_TEXTURE(t, idx) do { if (cenc) [cenc setTexture:t atIndex:idx]; \
    else if (st == STAGE_VS) [renc setVertexTexture:t atIndex:idx]; else [renc setFragmentTexture:t atIndex:idx]; } while (0)
#define SET_SAMPLER(sm, idx) do { if (cenc) [cenc setSamplerState:sm atIndex:idx]; \
    else if (st == STAGE_VS) [renc setVertexSamplerState:sm atIndex:idx]; else [renc setFragmentSamplerState:sm atIndex:idx]; } while (0)
    dxbc_aux aux = { { 0 }, { 0 } };                             // buffer sizes (for bufinfo), UAV counters
    uint32_t *sizes = aux.sizes;
    for (int i = 0; i < 14; i++) {
        if (!(sh->info.cbuffers & (1u << i))) continue;
        id<MTLBuffer> b = s->cb[st][i] ? (__bridge id<MTLBuffer>)s->cb[st][i]->mtl : nil;
        SET_BUFFER(b, 0, i);
    }
    for (int i = 0; i < 32; i++) {
        if (!(sh->info.textures[0] & (1ull << i))) continue;
        SET_TEXTURE(shader_texture(s->srv[st][i]), i);
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
    }
#undef SET_BUFFER
#undef SET_TEXTURE
#undef SET_SAMPLER
}

// Sets everything a draw needs; returns the encoder, or nil when the draw cannot happen.
static id<MTLRenderCommandEncoder> prepare(d3d_context *c, MTLPrimitiveType *type) {
    d3d_draw_state *s = state_of(c);
    if (!s->shader[STAGE_VS]) return nil;
    switch (s->topology) {
    case 1: *type = MTLPrimitiveTypePoint; break;
    case 2: *type = MTLPrimitiveTypeLine; break;
    case 3: *type = MTLPrimitiveTypeLineStrip; break;
    case 5: *type = MTLPrimitiveTypeTriangleStrip; break;
    case 4: *type = MTLPrimitiveTypeTriangle; break;
    default: return nil;                                       // undefined or patch lists
    }
    id<MTLTexture> colors[8] = { nil };
    int ncolors = 0;
    for (int i = 0; i < 8; i++) if (c->rtv[i]) { colors[i] = view_texture(c->rtv[i]); ncolors = i + 1; }
    id<MTLTexture> depth = c->dsv ? view_texture(c->dsv) : nil;
    id<MTLRenderPipelineState> pso = pipeline(c, s, colors, ncolors, depth);
    if (!pso) return nil;
    id<MTLRenderCommandEncoder> enc = render_pass(c);
    if (!enc) return nil;
    [enc setRenderPipelineState:pso];
    [enc setDepthStencilState:s->depth ? (__bridge id<MTLDepthStencilState>)s->depth->mtl : default_depth_state(c)];
    [enc setStencilReferenceValue:s->stencil_ref];
    [enc setBlendColorRed:s->blend_factor[0] green:s->blend_factor[1] blue:s->blend_factor[2] alpha:s->blend_factor[3]];
    // Rasterizer: fill, cull, winding, depth bias and clipping (defaults: solid, back faces culled, clockwise front).
    const uint32_t *rd = s->raster ? s->raster->desc : NULL;
    [enc setTriangleFillMode:rd && rd[0] == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
    [enc setCullMode:!rd || rd[1] == 3 ? MTLCullModeBack : rd[1] == 2 ? MTLCullModeFront : MTLCullModeNone];
    [enc setFrontFacingWinding:rd && rd[2] ? MTLWindingCounterClockwise : MTLWindingClockwise];
    if (rd) [enc setDepthBias:(float)(int32_t)rd[3] / 16777216.0f slopeScale:*(const float *)&rd[5] clamp:*(const float *)&rd[4]];
    [enc setDepthClipMode:!rd || rd[6] ? MTLDepthClipModeClip : MTLDepthClipModeClamp];
    id<MTLTexture> target = colors[0] ? colors[0] : depth;
    NSUInteger tw = target ? target.width >> (c->rtv[0] ? c->rtv[0]->mip : 0) : 1, th = target ? target.height >> (c->rtv[0] ? c->rtv[0]->mip : 0) : 1;
    if (!tw) tw = 1;
    if (!th) th = 1;
    MTLViewport vp = { c->viewport[0], c->viewport[1], c->viewport[2], c->viewport[3], c->viewport[4], c->viewport[5] };
    if (vp.width <= 0 || vp.height <= 0) vp = (MTLViewport){ 0, 0, (double)tw, (double)th, 0, 1 };
    [enc setViewport:vp];
    MTLScissorRect sc = { 0, 0, tw, th };
    if (rd && rd[7]) {                                          // scissor enabled
        LONG l = MAX(s->scissor.left, 0), t = MAX(s->scissor.top, 0);
        LONG r = MIN(s->scissor.right, (LONG)tw), b = MIN(s->scissor.bottom, (LONG)th);
        sc = (MTLScissorRect){ (NSUInteger)l, (NSUInteger)t, (NSUInteger)MAX(r - l, 0), (NSUInteger)MAX(b - t, 0) };
    }
    [enc setScissorRect:sc];
    for (int i = 0; i < 16; i++)
        if (s->vb[i]) [enc setVertexBuffer:(__bridge id<MTLBuffer>)s->vb[i]->mtl offset:s->offset[i] atIndex:VB_BASE + i];
    bind_stage(c, s, STAGE_VS, enc, nil);
    bind_stage(c, s, STAGE_PS, enc, nil);
    return enc;
}

static WINAPI void ctx_draw(void *this, uint32_t count, uint32_t start) {
    MTLPrimitiveType type;
    id<MTLRenderCommandEncoder> enc = prepare(this, &type);
    if (enc && count) [enc drawPrimitives:type vertexStart:start vertexCount:count];
}
static WINAPI void ctx_draw_instanced(void *this, uint32_t count, uint32_t instances, uint32_t start, uint32_t start_instance) {
    MTLPrimitiveType type;
    id<MTLRenderCommandEncoder> enc = prepare(this, &type);
    if (enc && count && instances) [enc drawPrimitives:type vertexStart:start vertexCount:count instanceCount:instances baseInstance:start_instance];
}
static void draw_indexed(d3d_context *c, uint32_t count, uint32_t instances, uint32_t start, int32_t base_vertex, uint32_t start_instance) {
    d3d_draw_state *s = state_of(c);
    MTLPrimitiveType type;
    if (!s->ib || !count || !instances) return;
    id<MTLRenderCommandEncoder> enc = prepare(c, &type);
    if (!enc) return;
    int wide = s->ib_format == 42;                             // R32_UINT, else R16_UINT
    [enc drawIndexedPrimitives:type indexCount:count indexType:wide ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16
                   indexBuffer:(__bridge id<MTLBuffer>)s->ib->mtl indexBufferOffset:s->ib_offset + start * (wide ? 4 : 2)
                 instanceCount:instances baseVertex:base_vertex baseInstance:start_instance];
}
static WINAPI void ctx_draw_indexed(void *this, uint32_t count, uint32_t start, int32_t base_vertex) { draw_indexed(this, count, 1, start, base_vertex, 0); }
static WINAPI void ctx_draw_indexed_instanced(void *this, uint32_t count, uint32_t instances, uint32_t start, int32_t base_vertex, uint32_t start_instance) {
    draw_indexed(this, count, instances, start, base_vertex, start_instance);
}


// ---- compute ---------------------------------------------------------------------------------------------------------------------
static WINAPI void ctx_dispatch(void *this, uint32_t x, uint32_t y, uint32_t z) {
    d3d_context *c = this;
    d3d_draw_state *s = state_of(c);
    d3d_shader *cs = s->shader[STAGE_CS];
    if (!cs || !x || !y || !z) return;
    if (!cs->compute) {
        NSError *err = nil;
        id<MTLComputePipelineState> pso = [(__bridge id<MTLDevice>)c->base.dev->mtl newComputePipelineStateWithFunction:(__bridge id<MTLFunction>)cs->function error:&err];
        if (!pso) { vx_log("Direct3D: compute pipeline failed: %s", err.localizedDescription.UTF8String); return; }
        cs->compute = (void *)CFBridgingRetain(pso);
    }
    d3d_end_render(c);
    id<MTLComputeCommandEncoder> enc = [(__bridge id<MTLCommandBuffer>)d3d_context_cmd(c) computeCommandEncoder];
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
    [blit copyFromBuffer:counter_of(v) sourceOffset:0 toBuffer:(__bridge id<MTLBuffer>)dst->mtl destinationOffset:offset size:4];
    [blit endEncoding];
}
static WINAPI void ctx_clear_uav_uint(void *this, d3d_view *v, const uint32_t *values) { clear_uav(this, v, values[0]); }
static WINAPI void ctx_clear_uav_float(void *this, d3d_view *v, const float *values) { uint32_t bits; memcpy(&bits, &values[0], 4); clear_uav(this, v, bits); }

// ---- tables -------------------------------------------------------------------------------------------------------------------------
const d3d_method d3d_draw_device_methods[] = {
    { "CreateVertexShader", (void *)device_create_vs }, { "CreatePixelShader", (void *)device_create_ps },
    { "CreateInputLayout", (void *)device_create_layout }, { "CreateBlendState", (void *)device_create_blend },
    { "CreateDepthStencilState", (void *)device_create_depth }, { "CreateRasterizerState", (void *)device_create_raster },
    { "CreateSamplerState", (void *)device_create_sampler }, { "CreateComputeShader", (void *)device_create_cs },
    { "CreateUnorderedAccessView", (void *)device_create_uav }, { NULL, NULL },
};
const d3d_method d3d_draw_context_methods[] = {
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
    { "CopyStructureCount", (void *)ctx_copy_structure_count }, { "ClearUnorderedAccessViewUint", (void *)ctx_clear_uav_uint }, { "ClearUnorderedAccessViewFloat", (void *)ctx_clear_uav_float },
    { NULL, NULL },
};
void d3d_draw_init(void) {
    static const d3d_method vs[] = { { "QueryInterface", (void *)vs_qi }, CHILD_METHODS, { NULL, NULL } };
    static const d3d_method ps[] = { { "QueryInterface", (void *)ps_qi }, CHILD_METHODS, { NULL, NULL } };
    static const d3d_method cs[] = { { "QueryInterface", (void *)cs_qi }, CHILD_METHODS, { NULL, NULL } };
    static const d3d_method uav[] = { { "QueryInterface", (void *)uav_qi }, CHILD_METHODS, { "GetResource", (void *)uav_get_resource }, { NULL, NULL } };
    static const d3d_method layout[] = { { "QueryInterface", (void *)layout_qi }, CHILD_METHODS, { NULL, NULL } };
    static const d3d_method blend[] = { { "QueryInterface", (void *)blend_qi }, CHILD_METHODS, { "GetDesc", (void *)blend_get_desc }, { NULL, NULL } };
    static const d3d_method depth[] = { { "QueryInterface", (void *)depth_qi }, CHILD_METHODS, { "GetDesc", (void *)depth_get_desc }, { NULL, NULL } };
    static const d3d_method raster[] = { { "QueryInterface", (void *)raster_qi }, CHILD_METHODS, { "GetDesc", (void *)raster_get_desc }, { NULL, NULL } };
    static const d3d_method sampler[] = { { "QueryInterface", (void *)sampler_qi }, CHILD_METHODS, { "GetDesc", (void *)sampler_get_desc }, { NULL, NULL } };
    g_vs_vtbl = D3D_VTABLE(ID3D11VertexShader, vs);
    g_ps_vtbl = D3D_VTABLE(ID3D11PixelShader, ps);
    g_cs_vtbl = D3D_VTABLE(ID3D11ComputeShader, cs);
    g_uav_vtbl = D3D_VTABLE(ID3D11UnorderedAccessView, uav);
    g_layout_vtbl = D3D_VTABLE(ID3D11InputLayout, layout);
    g_blend_vtbl = D3D_VTABLE(ID3D11BlendState, blend);
    g_depth_vtbl = D3D_VTABLE(ID3D11DepthStencilState, depth);
    g_raster_vtbl = D3D_VTABLE(ID3D11RasterizerState, raster);
    g_sampler_vtbl = D3D_VTABLE(ID3D11SamplerState, sampler);
}
