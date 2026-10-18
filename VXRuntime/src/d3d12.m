// Direct3D 12 on Metal (d3d12.dll): the device, queues, command lists recorded and replayed into Metal command
// buffers, fences as MTLSharedEvents, descriptor heaps, root signatures (the serialized RTS0 format, both ways),
// pipeline states made from DXIL translated by dxil_msl.c, committed resources and their views.
//
// Descriptors are 32 bytes in the layout the translated shaders read (VxDesc, dxil_msl.h): a GPU address, a Metal
// resource id, a size, a stride, a format and flags. Shader-visible heaps live in Metal buffers, so a GPU descriptor
// handle is a GPU address the shaders can read; at each dispatch the root signature is resolved into one pointer per
// resource range the shader declares (VxRoot at buffer 0).
//
// Not done yet: graphics pipelines and draws, swap chains on D3D12 queues, placed and reserved resources, queries,
// indirect execution, typed buffer views.
#include "d3d_common.h"
#include "dxil_msl.h"
#include <float.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "d3d12_tables.h"
#pragma clang diagnostic pop

enum { K12_DEVICE = 1, K12_QUEUE, K12_ALLOCATOR, K12_LIST, K12_FENCE, K12_HEAP, K12_ROOTSIG, K12_PSO, K12_RESOURCE, K12_BLOB, K12_DEBUG };

static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static void **g_device_vtbl, **g_queue_vtbl, **g_allocator_vtbl, **g_list_vtbl, **g_fence_vtbl, **g_heap_vtbl, **g_rootsig_vtbl,
            **g_pso_vtbl, **g_resource_vtbl, **g_blob_vtbl, **g_debug_vtbl;
static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;
static void make_tables(void);

// ---- the D3D12 structures this runtime reads (their Windows layouts: 4-byte enums, 8-byte handles) -----------------

typedef struct { int32_t Type, Priority, Flags; uint32_t NodeMask; } Q_DESC;
typedef struct { int32_t Type; uint32_t NumDescriptors; int32_t Flags; uint32_t NodeMask; } HEAP_DESC;
typedef struct { int32_t Type, CPUPageProperty, MemoryPoolPreference; uint32_t CreationNodeMask, VisibleNodeMask; } HEAP_PROPS;
typedef struct { int32_t Dimension; uint64_t Alignment, Width; uint32_t Height; uint16_t DepthOrArraySize, MipLevels; uint32_t Format, SampleCount, SampleQuality; int32_t Layout, Flags; } RES_DESC;
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
        uint32_t raw[8];
    };
} UAV_DESC;
typedef struct { int32_t filter, u, v, w; float bias; uint32_t aniso; int32_t cmp; float border[4]; float min_lod, max_lod; } SAMPLER_DESC;
typedef struct { uint64_t offset; uint32_t format, width, height, depth, row_pitch; } FOOTPRINT;
typedef struct { void *res; int32_t type; uint32_t pad; union { FOOTPRINT footprint; uint32_t subresource; }; } COPY_LOC;
typedef struct { uint32_t left, top, front, right, bottom, back; } BOX;
typedef struct { size_t begin, end; } RANGE;

// ---- objects ----------------------------------------------------------------------------------------------------

typedef struct dev12 dev12;
typedef struct { d3d_object base; dev12 *dev; } obj12;

struct dev12 {
    d3d_object base;
    id<MTLDevice> mtl;
    id<MTLCommandQueue> queue;
    NSMutableArray *resources;       // every live buffer and texture (made resident for each command buffer)
    pthread_mutex_t lock;
    NSMutableDictionary *pso_cache;  // MSL source -> id<MTLComputePipelineState>
};

typedef struct { d3d_object base; dev12 *dev; Q_DESC desc; } queue12;
typedef struct { d3d_object base; dev12 *dev; int32_t type; } allocator12;

typedef struct {
    d3d_object base; dev12 *dev;
    id<MTLSharedEvent> event;
    MTLSharedEventListener *listener;
} fence12;

typedef struct {
    d3d_object base; dev12 *dev;
    HEAP_DESC desc;
    id<MTLBuffer> gpu;               // shader-visible heaps
    uint8_t *cpu;                    // the descriptors (gpu.contents, or memory of its own)
    __strong id *keep;               // the Metal object each descriptor stands for (texture view, sampler), held
} heap12;

typedef struct {
    d3d_object base; dev12 *dev;
    RES_DESC desc;
    HEAP_PROPS heap;
    id<MTLBuffer> buffer;
    id<MTLTexture> texture;
    uint32_t mapped;
} res12;

// A root signature, from its serialized form.
typedef struct { int32_t type; uint32_t count, base, space, offset; } range12;
typedef struct { int32_t type, visibility; uint32_t reg, space, n32; uint32_t nranges; range12 *ranges; } param12;
typedef struct { uint32_t reg, space; id<MTLSamplerState> state; } static_sampler12;
typedef struct {
    d3d_object base; dev12 *dev;
    uint32_t nparams, nsamplers, flags;
    param12 *params;
    static_sampler12 *samplers;
} rootsig12;

typedef struct {
    d3d_object base; dev12 *dev;
    id<MTLComputePipelineState> compute;
    dxil_msl msl;                    // the translation's slots and thread counts (msl.msl freed)
} pso12;

typedef struct { d3d_object base; void *data; size_t size; } blob12;

// Recorded commands, replayed when the list is executed.
enum { C_PSO, C_ROOTSIG, C_HEAPS, C_TABLE, C_CONSTANTS, C_ROOT_VA, C_DISPATCH, C_COPY_BUFFER, C_COPY_RESOURCE, C_COPY_TEXTURE, C_UAV_BARRIER };
typedef struct {
    int op;
    uint32_t a, b, c;
    uint64_t va;
    void *obj, *obj2;
    uint64_t off, off2, size;
    COPY_LOC dst, src;
    BOX box;
    int has_box;
    uint32_t values[64];
} cmd12;

typedef struct {
    d3d_object base; dev12 *dev;
    int32_t type;
    cmd12 *cmds;
    uint32_t n, cap;
    int closed;
} list12;

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
    const GUID *known[] = { &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4, &IID_ID3D12Device5 };
    return query(d, iid, out, known, 6);
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
typedef struct { uint32_t nparams; uint32_t pad; const PARAM *params; uint32_t nsamplers; uint32_t pad2; const STATIC_SAMPLER *samplers; int32_t flags; } RS_DESC;
typedef struct { int32_t version; uint32_t pad; RS_DESC desc; } VERSIONED_RS_DESC;

typedef struct { uint8_t *p; size_t n, cap; } bytes;
static void b_u32(bytes *b, uint32_t v) {
    if (b->n + 4 > b->cap) { b->cap = (b->n + 4) * 2; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, &v, 4);
    b->n += 4;
}
static void b_set(bytes *b, size_t at, uint32_t v) { memcpy(b->p + at, &v, 4); }

// Writes RTS0 (version 1: 1.0 ranges, 2: 1.1) in a DXBC container with one chunk, as D3D12SerializeRootSignature does.
static blob12 *serialize_rootsig(int version, const RS_DESC *d) {
    bytes r = { 0 };
    b_u32(&r, version == 1 ? 1 : 2);
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
        const uint32_t *s = (const uint32_t *)&d->samplers[i];
        for (int k = 0; k < 13; k++) b_u32(&r, s[k]);
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
static const uint8_t *rts0(const uint8_t *p, size_t size, size_t *len) {
    if (size >= 32 && !memcmp(p, "DXBC", 4)) {
        uint32_t count;
        memcpy(&count, p + 28, 4);
        for (uint32_t i = 0; i < count && 32 + 4 * (size_t)i + 4 <= size; i++) {
            uint32_t off;
            memcpy(&off, p + 32 + 4 * i, 4);
            if ((size_t)off + 8 > size || memcmp(p + off, "RTS0", 4)) continue;
            uint32_t n;
            memcpy(&n, p + off + 4, 4);
            if ((size_t)off + 8 + n > size) return NULL;
            *len = n;
            return p + off + 8;
        }
        return NULL;
    }
    *len = size;
    return p;
}

static uint32_t rd32(const uint8_t *p, size_t len, size_t at) { uint32_t v = 0; if (at + 4 <= len) memcpy(&v, p + at, 4); return v; }

static id<MTLSamplerState> make_sampler(dev12 *d, int32_t filter, int32_t u, int32_t v, int32_t w, float bias, uint32_t aniso, int32_t cmp, const float border[4], float min_lod, float max_lod);

static void rootsig_destroy(d3d_object *o) {
    rootsig12 *r = (rootsig12 *)o;
    for (uint32_t i = 0; i < r->nparams; i++) free(r->params[i].ranges);
    free(r->params);
    for (uint32_t i = 0; i < r->nsamplers; i++) r->samplers[i].state = nil;
    free(r->samplers);
    release_dev(o);
}

static HRESULT parse_rootsig(dev12 *d, const void *data, size_t size, rootsig12 *r) {
    size_t len = 0;
    const uint8_t *p = rts0(data, size, &len);
    if (!p || len < 24) return E_INVALIDARG;
    uint32_t version = rd32(p, len, 0);
    r->nparams = rd32(p, len, 4);
    uint32_t params_at = rd32(p, len, 8);
    r->nsamplers = rd32(p, len, 12);
    uint32_t samplers_at = rd32(p, len, 16);
    r->flags = rd32(p, len, 20);
    if (r->nparams > 64 || r->nsamplers > 2048) return E_INVALIDARG;
    r->params = calloc(r->nparams + 1, sizeof *r->params);
    for (uint32_t i = 0; i < r->nparams; i++) {
        param12 *q = &r->params[i];
        size_t at = params_at + 12 * i;
        q->type = (int32_t)rd32(p, len, at);
        q->visibility = (int32_t)rd32(p, len, at + 4);
        uint32_t payload = rd32(p, len, at + 8);
        if (q->type == 0) {
            q->nranges = rd32(p, len, payload);
            uint32_t ranges_at = rd32(p, len, payload + 4);
            q->ranges = calloc(q->nranges + 1, sizeof *q->ranges);
            uint32_t stride = version == 1 ? 20 : 24, running = 0;
            for (uint32_t k = 0; k < q->nranges; k++) {
                size_t g = ranges_at + stride * k;
                range12 *x = &q->ranges[k];
                x->type = (int32_t)rd32(p, len, g);
                x->count = rd32(p, len, g + 4);
                x->base = rd32(p, len, g + 8);
                x->space = rd32(p, len, g + 12);
                x->offset = rd32(p, len, g + (version == 1 ? 16 : 20));
                if (x->offset == 0xFFFFFFFFu) x->offset = running;     // D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                running = x->offset + (x->count == 0xFFFFFFFFu ? 0 : x->count);
            }
        } else if (q->type == 1) {
            q->reg = rd32(p, len, payload); q->space = rd32(p, len, payload + 4); q->n32 = rd32(p, len, payload + 8);
        } else {
            q->reg = rd32(p, len, payload); q->space = rd32(p, len, payload + 4);
        }
    }
    r->samplers = calloc(r->nsamplers + 1, sizeof *r->samplers);
    for (uint32_t i = 0; i < r->nsamplers; i++) {
        uint32_t s[13];
        for (int k = 0; k < 13; k++) s[k] = rd32(p, len, samplers_at + 52 * i + 4 * k);
        float bias, mn, mx;
        memcpy(&bias, &s[4], 4); memcpy(&mn, &s[8], 4); memcpy(&mx, &s[9], 4);
        static const float borders[3][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1, 1 } };
        r->samplers[i].reg = s[10];
        r->samplers[i].space = s[11];
        r->samplers[i].state = make_sampler(d, (int32_t)s[0], (int32_t)s[1], (int32_t)s[2], (int32_t)s[3], bias, s[5], (int32_t)s[6], borders[s[7] < 3 ? s[7] : 0], mn, mx);
    }
    return S_OK;
}

// ---- samplers ---------------------------------------------------------------------------------------------------

static MTLSamplerAddressMode address_mode(int32_t m) {
    switch (m) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

static id<MTLSamplerState> make_sampler(dev12 *d, int32_t filter, int32_t u, int32_t v, int32_t w, float bias, uint32_t aniso, int32_t cmp, const float border[4], float min_lod, float max_lod) {
    MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
    // D3D12_FILTER: bits 0 mip linear, 2 mag linear, 4 min linear, 6 anisotropic, 7 comparison
    s.minFilter = (filter & 0x10) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    s.magFilter = (filter & 0x04) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    s.mipFilter = (filter & 0x01) ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    if (filter & 0x40) { s.minFilter = s.magFilter = MTLSamplerMinMagFilterLinear; s.mipFilter = MTLSamplerMipFilterLinear; s.maxAnisotropy = aniso < 1 ? 1 : aniso > 16 ? 16 : aniso; }
    s.sAddressMode = address_mode(u);
    s.tAddressMode = address_mode(v);
    s.rAddressMode = address_mode(w);
    s.borderColor = border[3] == 0 ? MTLSamplerBorderColorTransparentBlack : border[0] == 0 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite;
    s.lodMinClamp = min_lod < 0 ? 0 : min_lod;
    s.lodMaxClamp = max_lod > 1000 ? FLT_MAX : max_lod;
    if (filter & 0x80) {
        static const MTLCompareFunction f[] = { MTLCompareFunctionNever, MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual,
                                                MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
                                                MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways };
        s.compareFunction = cmp >= 1 && cmp <= 8 ? f[cmp] : MTLCompareFunctionNever;
    }
    s.supportArgumentBuffers = YES;
    (void)bias;                                                    // (Metal samplers take no LOD bias)
    return [d->mtl newSamplerStateWithDescriptor:s];
}

// ---- resources ----------------------------------------------------------------------------------------------------

static void res_destroy(d3d_object *o) {
    res12 *r = (res12 *)o;
    dev12 *d = r->dev;
    pthread_mutex_lock(&d->lock);
    if (r->buffer) [d->resources removeObjectIdenticalTo:r->buffer];
    if (r->texture) [d->resources removeObjectIdenticalTo:r->texture];
    pthread_mutex_unlock(&d->lock);
    r->buffer = nil;
    r->texture = nil;
    release_dev(o);
}

static WINAPI HRESULT res_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12Resource, &IID_ID3D12Resource1, &IID_ID3D12Resource2, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 6);
}
static WINAPI HRESULT res_map(void *this, uint32_t sub, const RANGE *read, void **data) {
    res12 *r = this;
    if (!r->buffer) return d3d_not_implemented("ID3D12Resource::Map of a texture");
    if (data) *data = r->buffer.contents;
    r->mapped++;
    return S_OK;
}
static WINAPI void res_unmap(void *this, uint32_t sub, const RANGE *written) { res12 *r = this; if (r->mapped) r->mapped--; }
static WINAPI RES_DESC *res_get_desc(void *this, RES_DESC *out) { *out = ((res12 *)this)->desc; return out; }
static WINAPI uint64_t res_gpu_va(void *this) { res12 *r = this; return r->buffer ? r->buffer.gpuAddress : 0; }
static WINAPI HRESULT res_get_heap_props(void *this, HEAP_PROPS *props, int32_t *flags) {
    if (props) *props = ((res12 *)this)->heap;
    if (flags) *flags = 0;
    return S_OK;
}

static uint32_t bytes_per(uint32_t format) { uint32_t b = 0; d3d_mtl_format(format, &b); return b; }

static HRESULT create_committed(dev12 *d, const HEAP_PROPS *hp, int32_t heap_flags, const RES_DESC *desc, int32_t state, const void *clear, const GUID *iid, void **out) {
    res12 *r = alloc12(sizeof *r, K12_RESOURCE, g_resource_vtbl, d);
    r->base.destroy = res_destroy;
    r->desc = *desc;
    r->heap = *hp;
    if (desc->Dimension == 1) {                                    // D3D12_RESOURCE_DIMENSION_BUFFER
        r->buffer = [d->mtl newBufferWithLength:desc->Width ? desc->Width : 1 options:MTLResourceStorageModeShared];
        if (!r->buffer) { d3d_release(&r->base); return E_OUTOFMEMORY; }
        vx_guest_forget(r->buffer.contents, r->buffer.length);
        if (hp->Type != 2) memset(r->buffer.contents, 0, r->buffer.length);   // (default and readback heaps start zeroed)
    } else {
        uint32_t bpp = 0;
        MTLPixelFormat fmt = d3d_mtl_format(desc->Format, &bpp);
        if (fmt == MTLPixelFormatInvalid) { vx_log("D3D12: texture format %u is not supported", desc->Format); d3d_release(&r->base); return E_INVALIDARG; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.pixelFormat = fmt;
        td.width = desc->Width;
        td.height = desc->Height ? desc->Height : 1;
        td.mipmapLevelCount = desc->MipLevels ? desc->MipLevels : 1;
        if (desc->Dimension == 4) { td.textureType = MTLTextureType3D; td.depth = desc->DepthOrArraySize; }
        else if (desc->DepthOrArraySize > 1) { td.textureType = desc->Dimension == 2 ? MTLTextureType1DArray : MTLTextureType2DArray; td.arrayLength = desc->DepthOrArraySize; }
        else td.textureType = desc->Dimension == 2 ? MTLTextureType1D : MTLTextureType2D;
        if (desc->SampleCount > 1) { td.textureType = desc->DepthOrArraySize > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample; td.sampleCount = desc->SampleCount; td.mipmapLevelCount = 1; }
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        if (desc->Flags & 0x4) td.usage |= MTLTextureUsageShaderWrite;          // ALLOW_UNORDERED_ACCESS
        if (desc->Flags & 0x3) td.usage |= MTLTextureUsageRenderTarget;         // ALLOW_RENDER_TARGET, ALLOW_DEPTH_STENCIL
        td.storageMode = MTLStorageModePrivate;
        r->texture = [d->mtl newTextureWithDescriptor:td];
        if (!r->texture) { d3d_release(&r->base); return E_OUTOFMEMORY; }
        if (r->desc.MipLevels == 0) r->desc.MipLevels = (uint16_t)td.mipmapLevelCount;
    }
    pthread_mutex_lock(&d->lock);
    [d->resources addObject:r->buffer ? (id)r->buffer : (id)r->texture];
    pthread_mutex_unlock(&d->lock);
    if (!out) { d3d_release(&r->base); return S_FALSE; }
    HRESULT hr = res_query(r, iid, out);
    d3d_release(&r->base);
    return hr;
}

// ---- descriptor heaps -----------------------------------------------------------------------------------------------

typedef struct { uint64_t va, res; uint32_t size, stride, fmt, flags; } VxDesc;   // dxil_msl.h's descriptor

static pthread_mutex_t g_heaps_lock = PTHREAD_MUTEX_INITIALIZER;
static heap12 *g_heaps[4096];
static uint32_t g_nheaps;

static void heap_destroy(d3d_object *o) {
    heap12 *h = (heap12 *)o;
    pthread_mutex_lock(&g_heaps_lock);
    for (uint32_t i = 0; i < g_nheaps; i++) if (g_heaps[i] == h) { g_heaps[i] = g_heaps[--g_nheaps]; break; }
    pthread_mutex_unlock(&g_heaps_lock);
    for (uint32_t i = 0; i < h->desc.NumDescriptors; i++) h->keep[i] = nil;
    free((void *)h->keep);
    if (!h->gpu) free(h->cpu);
    h->gpu = nil;
    release_dev(o);
}

// The heap and descriptor index of a CPU descriptor handle.
static heap12 *heap_at(size_t cpu, uint32_t *index) {
    pthread_mutex_lock(&g_heaps_lock);
    heap12 *found = NULL;
    for (uint32_t i = 0; i < g_nheaps; i++) {
        heap12 *h = g_heaps[i];
        if (cpu >= (size_t)h->cpu && cpu < (size_t)h->cpu + 32 * (size_t)h->desc.NumDescriptors) { found = h; *index = (uint32_t)((cpu - (size_t)h->cpu) / 32); break; }
    }
    pthread_mutex_unlock(&g_heaps_lock);
    return found;
}

static WINAPI HRESULT heap_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12DescriptorHeap, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}
static WINAPI HEAP_DESC *heap_get_desc(void *this, HEAP_DESC *out) { *out = ((heap12 *)this)->desc; return out; }
static WINAPI size_t *heap_cpu_start(void *this, size_t *out) { *out = (size_t)((heap12 *)this)->cpu; return out; }
static WINAPI uint64_t *heap_gpu_start(void *this, uint64_t *out) { heap12 *h = this; *out = h->gpu ? h->gpu.gpuAddress : 0; return out; }

// Writes a descriptor and holds the Metal object it names.
static void put_desc(size_t cpu, const VxDesc *v, id keep) {
    uint32_t i;
    heap12 *h = heap_at(cpu, &i);
    if (!h) { vx_log("D3D12: a descriptor written outside any heap (%#zx)", cpu); return; }
    memcpy(h->cpu + 32 * (size_t)i, v, 32);
    h->keep[i] = keep;
}

// ---- views ------------------------------------------------------------------------------------------------------------

static id<MTLTexture> texture_view(res12 *r, uint32_t format, MTLTextureType type, uint32_t mip, uint32_t mips, uint32_t slice, uint32_t slices) {
    uint32_t bpp;
    MTLPixelFormat fmt = format ? d3d_mtl_format(format, &bpp) : r->texture.pixelFormat;
    if (fmt == MTLPixelFormatInvalid) fmt = r->texture.pixelFormat;
    if (mips == 0 || mips == 0xFFFFFFFFu || mip + mips > r->texture.mipmapLevelCount) mips = (uint32_t)r->texture.mipmapLevelCount - mip;
    uint32_t total = r->texture.textureType == MTLTextureType3D ? 1 : (uint32_t)r->texture.arrayLength;
    if (slices == 0 || slices == 0xFFFFFFFFu || slice + slices > total) slices = total - slice;
    return [r->texture newTextureViewWithPixelFormat:fmt textureType:type levels:NSMakeRange(mip, mips) slices:NSMakeRange(slice, slices)];
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

static WINAPI void dev_create_cbv(void *this, const CBV_DESC *desc, size_t cpu) {
    VxDesc v = { 0 };
    if (desc) { v.va = desc->location; v.size = desc->size; }
    put_desc(cpu, &v, nil);
}

static WINAPI void dev_create_srv(void *this, void *resource, const SRV_DESC *desc, size_t cpu) {
    res12 *r = resource;
    VxDesc v = { 0 };
    id keep = nil;
    if (r && r->buffer) {
        if (!desc) { v.va = r->buffer.gpuAddress; v.size = (uint32_t)r->desc.Width; }
        else {
            uint32_t stride = desc->buffer.stride ? desc->buffer.stride : (desc->buffer.flags & 1) ? 4 : bytes_per(desc->format);
            if (!stride) stride = 4;
            v.va = r->buffer.gpuAddress + desc->buffer.first * stride;
            v.size = desc->buffer.count * stride;
            v.stride = desc->buffer.stride;
            v.fmt = desc->format;
            if (!desc->buffer.stride && !(desc->buffer.flags & 1) && desc->format) {
                d3d_not_implemented("typed buffer shader resource views");
            }
        }
    } else if (r && r->texture) {
        int32_t dim = desc ? desc->dim : (r->desc.DepthOrArraySize > 1 ? 5 : 4);
        uint32_t mip = 0, mips = 0, slice = 0, slices = 0;
        if (desc && (dim == 4 || dim == 2)) { mip = desc->tex2d.most_detailed; mips = desc->tex2d.mips; }
        if (desc && (dim == 5 || dim == 3 || dim == 7)) { mip = desc->tex2darray.most_detailed; mips = desc->tex2darray.mips; slice = desc->tex2darray.first_slice; slices = desc->tex2darray.slices; }
        id<MTLTexture> t = texture_view(r, desc ? desc->format : 0, view_type(dim, 0), mip, mips, slice, slices);
        v.res = resource_id(t);
        v.fmt = desc ? desc->format : r->desc.Format;
        keep = t;
    }
    put_desc(cpu, &v, keep);
}

static WINAPI void dev_create_uav(void *this, void *resource, void *counter, const UAV_DESC *desc, size_t cpu) {
    res12 *r = resource;
    VxDesc v = { 0 };
    id keep = nil;
    if (r && r->buffer) {
        if (!desc) { v.va = r->buffer.gpuAddress; v.size = (uint32_t)r->desc.Width; }
        else {
            uint32_t stride = desc->buffer.stride ? desc->buffer.stride : (desc->buffer.flags & 1) ? 4 : bytes_per(desc->format);
            if (!stride) stride = 4;
            v.va = r->buffer.gpuAddress + desc->buffer.first * stride;
            v.size = desc->buffer.count * stride;
            v.stride = desc->buffer.stride;
            v.fmt = desc->format;
            if (counter) d3d_not_implemented("UAV counters");
        }
    } else if (r && r->texture) {
        int32_t dim = desc ? desc->dim : 4;
        uint32_t mip = 0, slice = 0, slices = 0;
        if (desc && (dim == 4 || dim == 2)) mip = desc->tex2d.mip;
        if (desc && (dim == 5 || dim == 3)) { mip = desc->tex2darray.mip; slice = desc->tex2darray.first_slice; slices = desc->tex2darray.slices; }
        id<MTLTexture> t = texture_view(r, desc ? desc->format : 0, view_type(dim, 1), mip, 1, slice, slices);
        v.res = resource_id(t);
        v.fmt = desc ? desc->format : r->desc.Format;
        keep = t;
    }
    put_desc(cpu, &v, keep);
}

static WINAPI void dev_create_sampler(void *this, const SAMPLER_DESC *s, size_t cpu) {
    id<MTLSamplerState> st = make_sampler(this, s->filter, s->u, s->v, s->w, s->bias, s->aniso, s->cmp, s->border, s->min_lod, s->max_lod);
    VxDesc v = { 0 };
    v.res = resource_id((id<MTLResource>)st);
    put_desc(cpu, &v, st);
}

static WINAPI void dev_copy_simple(void *this, uint32_t n, size_t dst, size_t src, int32_t type) {
    for (uint32_t k = 0; k < n; k++) {
        uint32_t si, di;
        heap12 *sh = heap_at(src + 32 * (size_t)k, &si), *dh = heap_at(dst + 32 * (size_t)k, &di);
        if (!sh || !dh) continue;
        memcpy(dh->cpu + 32 * (size_t)di, sh->cpu + 32 * (size_t)si, 32);
        dh->keep[di] = sh->keep[si];
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
static WINAPI HRESULT fence_set_event(void *this, uint64_t value, HANDLE h) {
    fence12 *f = this;
    if (f->event.signaledValue >= value) { if (h) vx_set_event(h); return S_OK; }
    if (!h) {                                                      // (no event: wait here)
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [f->event notifyListener:f->listener atValue:value block:^(id<MTLSharedEvent> e, uint64_t v) { dispatch_semaphore_signal(sem); }];
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
        return S_OK;
    }
    [f->event notifyListener:f->listener atValue:value block:^(id<MTLSharedEvent> e, uint64_t v) { vx_set_event(h); }];
    return S_OK;
}
static WINAPI int32_t fence_flags(void *this) { return 0; }

static WINAPI HRESULT dev_create_fence(void *this, uint64_t initial, int32_t flags, const GUID *iid, void **out) {
    dev12 *d = this;
    fence12 *f = alloc12(sizeof *f, K12_FENCE, g_fence_vtbl, d);
    f->base.destroy = fence_destroy;
    f->event = [d->mtl newSharedEvent];
    f->event.signaledValue = initial;
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("vxrt.d3d12.fences", DISPATCH_QUEUE_SERIAL); });
    f->listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:q];
    HRESULT hr = fence_query(f, iid, out);
    d3d_release(&f->base);
    return hr;
}

// ---- pipeline states ---------------------------------------------------------------------------------------------

static void pso_destroy(d3d_object *o) { pso12 *p = (pso12 *)o; p->compute = nil; release_dev(o); }
static WINAPI HRESULT pso_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12PipelineState, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}

static WINAPI HRESULT dev_create_compute_pso(void *this, const COMPUTE_PSO_DESC *desc, const GUID *iid, void **out) {
    dev12 *d = this;
    dxil_module m;
    if (dxil_read(desc->cs.code, desc->cs.size, &m)) { vx_log("D3D12: compute shader: %s", m.error); return E_INVALIDARG; }
    pso12 *p = alloc12(sizeof *p, K12_PSO, g_pso_vtbl, d);
    p->base.destroy = pso_destroy;
    int rc = dxil_to_msl(&m, &p->msl);
    dxil_free(&m);
    if (rc) { vx_log("D3D12: compute shader not translated: %s", p->msl.error); d3d_release(&p->base); return E_INVALIDARG; }
    NSString *src = @(p->msl.msl);
    pthread_mutex_lock(&d->lock);
    id<MTLComputePipelineState> cached = d->pso_cache[src];
    pthread_mutex_unlock(&d->lock);
    if (cached) p->compute = cached;
    else {
        MTLCompileOptions *opt = [MTLCompileOptions new];
        opt.languageVersion = MTLLanguageVersion3_1;
        NSError *err = nil;
        id<MTLLibrary> lib = [d->mtl newLibraryWithSource:src options:opt error:&err];
        if (lib) p->compute = [d->mtl newComputePipelineStateWithFunction:[lib newFunctionWithName:@"vx_main"] error:&err];
        if (!p->compute) {
            vx_log("D3D12: compute shader did not compile: %s\n%s", err.localizedDescription.UTF8String, p->msl.msl);
            d3d_release(&p->base);
            return E_INVALIDARG;
        }
        pthread_mutex_lock(&d->lock);
        d->pso_cache[src] = p->compute;
        pthread_mutex_unlock(&d->lock);
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
    dev12 *d = this;
    rootsig12 *r = alloc12(sizeof *r, K12_ROOTSIG, g_rootsig_vtbl, d);
    r->base.destroy = rootsig_destroy;
    HRESULT hr = parse_rootsig(d, data, size, r);
    if (hr == S_OK) hr = rootsig_query(r, iid, out);
    d3d_release(&r->base);
    return hr;
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
static void list_clear(list12 *l) {
    for (uint32_t i = 0; i < l->n; i++) {
        cmd12 *c = &l->cmds[i];
        if (c->obj) d3d_release(c->obj);
        if (c->obj2) d3d_release(c->obj2);
        if (c->op == C_COPY_TEXTURE) { if (c->dst.res) d3d_release(c->dst.res); if (c->src.res) d3d_release(c->src.res); }
    }
    l->n = 0;
}
static void list_destroy(d3d_object *o) { list12 *l = (list12 *)o; list_clear(l); free(l->cmds); release_dev(o); }

static WINAPI HRESULT list_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12GraphicsCommandList, &IID_ID3D12GraphicsCommandList1, &IID_ID3D12GraphicsCommandList2, &IID_ID3D12GraphicsCommandList3,
                        &IID_ID3D12GraphicsCommandList4, &IID_ID3D12GraphicsCommandList5, &IID_ID3D12GraphicsCommandList6, &IID_ID3D12GraphicsCommandList7,
                        &IID_ID3D12CommandList, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 11);
}
static WINAPI int32_t list_type(void *this) { return ((list12 *)this)->type; }
static WINAPI HRESULT list_close(void *this) { ((list12 *)this)->closed = 1; return S_OK; }
static WINAPI HRESULT list_reset(void *this, void *allocator, void *pso) {
    list12 *l = this;
    list_clear(l);
    l->closed = 0;
    if (pso) { cmd12 *c = rec(l, C_PSO); c->obj = pso; hold(pso); }
    return S_OK;
}
static WINAPI void list_set_pso(void *this, void *pso) { cmd12 *c = rec(this, C_PSO); c->obj = pso; hold(pso); }
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
static WINAPI void list_dispatch(void *this, uint32_t x, uint32_t y, uint32_t z) { cmd12 *c = rec(this, C_DISPATCH); c->a = x; c->b = y; c->c = z; }
static WINAPI void list_copy_buffer(void *this, void *dst, uint64_t dst_off, void *src, uint64_t src_off, uint64_t n) {
    cmd12 *c = rec(this, C_COPY_BUFFER);
    c->obj = dst; c->obj2 = src; c->off = dst_off; c->off2 = src_off; c->size = n;
    hold(dst); hold(src);
}
static WINAPI void list_copy_resource(void *this, void *dst, void *src) {
    cmd12 *c = rec(this, C_COPY_RESOURCE);
    c->obj = dst; c->obj2 = src;
    hold(dst); hold(src);
}
static WINAPI void list_copy_texture(void *this, const COPY_LOC *dst, uint32_t x, uint32_t y, uint32_t z, const COPY_LOC *src, const BOX *box) {
    cmd12 *c = rec(this, C_COPY_TEXTURE);
    c->dst = *dst; c->src = *src;
    c->a = x; c->b = y; c->c = z;
    if (box) { c->box = *box; c->has_box = 1; }
    hold(dst->res); hold(src->res);
}
static WINAPI void list_barrier(void *this, uint32_t n, const void *barriers) { rec(this, C_UAV_BARRIER); }   // (Metal tracks the hazards it sees; see replay)

// ---- command lists: replay into Metal -----------------------------------------------------------------------------

typedef struct {
    uint64_t root_va[64];
    uint32_t constants[64][64];
    uint64_t table[64];
} root_state;

// Small per-execution memory for root descriptors and constants the shaders read through pointers.
typedef struct { id<MTLBuffer> buf; size_t used; } scratch;
static uint64_t scratch_put(dev12 *d, scratch *s, const void *data, size_t n) {
    n = (n + 31) & ~(size_t)31;
    if (!s->buf || s->used + n > s->buf.length) {
        s->buf = [d->mtl newBufferWithLength:MAX(65536, n * 2) options:MTLResourceStorageModeShared];
        s->used = 0;
    }
    memcpy((uint8_t *)s->buf.contents + s->used, data, n);
    uint64_t va = s->buf.gpuAddress + s->used;
    s->used += n;
    return va;
}

// The pointer the shader's slot reads its descriptors through, from the root signature and the root arguments.
static uint64_t resolve_slot(dev12 *d, const rootsig12 *rs, const root_state *st, const dxil_msl_slot *s, scratch *sc, NSMutableArray *used_bufs) {
    if (!rs) return 0;
    for (uint32_t i = 0; i < rs->nparams; i++) {
        const param12 *p = &rs->params[i];
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
            if (rs->samplers[i].reg != s->lower || rs->samplers[i].space != s->space) continue;
            VxDesc v = { 0, resource_id((id<MTLResource>)rs->samplers[i].state), 0, 0, 0, 0 };
            return scratch_put(d, sc, &v, sizeof v);
        }
    }
    vx_log("D3D12: the root signature has no place for %s register %u, space %u", s->cls == DXIL_SRV ? "SRV" : s->cls == DXIL_UAV ? "UAV" : s->cls == DXIL_CBV ? "CBV" : "sampler", s->lower, s->space);
    return 0;
}

static void replay(dev12 *d, list12 *l, id<MTLCommandBuffer> cb, scratch *sc) {
    pso12 *pso = NULL;
    rootsig12 *rs = NULL;
    heap12 *heap = NULL, *sheap = NULL;
    root_state *st = calloc(1, sizeof *st);
    id<MTLComputeCommandEncoder> ce = nil;
    id<MTLBlitCommandEncoder> be = nil;
    pthread_mutex_lock(&d->lock);
    NSArray *resident = [d->resources copy];
    pthread_mutex_unlock(&d->lock);
    for (uint32_t i = 0; i < l->n; i++) {
        cmd12 *c = &l->cmds[i];
        switch (c->op) {
        case C_PSO: pso = c->obj; break;
        case C_ROOTSIG: rs = c->obj; memset(st, 0, sizeof *st); break;
        case C_HEAPS: heap = c->obj; sheap = c->obj2; break;
        case C_TABLE: if (c->a < 64) st->table[c->a] = c->va; break;
        case C_CONSTANTS: if (c->a < 64) for (uint32_t k = 0; k < c->b && c->c + k < 64; k++) st->constants[c->a][c->c + k] = c->values[k]; break;
        case C_ROOT_VA: if (c->a < 64) st->root_va[c->a] = c->va; break;
        case C_DISPATCH: {
            if (!pso || !pso->compute) { vx_log("D3D12: Dispatch without a compute pipeline state"); break; }
            if (be) { [be endEncoding]; be = nil; }
            if (!ce) {
                ce = [cb computeCommandEncoderWithDispatchType:MTLDispatchTypeSerial];
                // (what the shaders reach through pointers and argument buffers must be resident: every resource
                //  of the device, and the texture views the bound heaps' descriptors name)
                for (id<MTLResource> res in resident) [ce useResource:res usage:MTLResourceUsageRead | MTLResourceUsageWrite];
                for (heap12 *h2 = heap; h2; h2 = NULL)
                    for (uint32_t k = 0; k < h2->desc.NumDescriptors; k++)
                        if ([h2->keep[k] conformsToProtocol:@protocol(MTLTexture)]) [ce useResource:h2->keep[k] usage:MTLResourceUsageRead | MTLResourceUsageWrite];
            }
            [ce setComputePipelineState:pso->compute];
            uint64_t root[64 + 2] = { 0 };
            for (uint32_t k = 0; k < pso->msl.nslots; k++) root[k] = resolve_slot(d, rs, st, &pso->msl.slots[k], sc, nil);
            root[pso->msl.nslots ? pso->msl.nslots : 1] = heap && heap->gpu ? heap->gpu.gpuAddress : 0;
            root[(pso->msl.nslots ? pso->msl.nslots : 1) + 1] = sheap && sheap->gpu ? sheap->gpu.gpuAddress : 0;
            [ce setBytes:root length:8 * ((pso->msl.nslots ? pso->msl.nslots : 1) + 2) atIndex:0];
            if (sc->buf) [ce useResource:sc->buf usage:MTLResourceUsageRead];
            [ce dispatchThreadgroups:MTLSizeMake(c->a, c->b, c->c) threadsPerThreadgroup:MTLSizeMake(pso->msl.threads[0], pso->msl.threads[1], pso->msl.threads[2])];
            // (the next dispatch sees this one's writes, as D3D12's UAV barriers promise; resources reached through
            //  argument buffers are not tracked by Metal)
            [ce memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
            break;
        }
        case C_UAV_BARRIER:
            if (ce) [ce memoryBarrierWithScope:MTLBarrierScopeBuffers | MTLBarrierScopeTextures];
            break;
        case C_COPY_BUFFER: case C_COPY_RESOURCE: case C_COPY_TEXTURE: {
            if (ce) { [ce endEncoding]; ce = nil; }
            if (!be) be = [cb blitCommandEncoder];
            if (c->op == C_COPY_BUFFER) {
                res12 *dst = c->obj, *src = c->obj2;
                if (dst->buffer && src->buffer) [be copyFromBuffer:src->buffer sourceOffset:c->off2 toBuffer:dst->buffer destinationOffset:c->off size:c->size];
            } else if (c->op == C_COPY_RESOURCE) {
                res12 *dst = c->obj, *src = c->obj2;
                if (dst->buffer && src->buffer) [be copyFromBuffer:src->buffer sourceOffset:0 toBuffer:dst->buffer destinationOffset:0 size:MIN(dst->buffer.length, src->buffer.length)];
                else if (dst->texture && src->texture) [be copyFromTexture:src->texture toTexture:dst->texture];
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
    }
    if (ce) [ce endEncoding];
    if (be) [be endEncoding];
    free(st);
}

// ---- queues -----------------------------------------------------------------------------------------------------------

static void queue_destroy(d3d_object *o) { release_dev(o); }
static WINAPI HRESULT queue_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12CommandQueue, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}

static WINAPI void queue_execute(void *this, uint32_t n, void *const *lists) {
    queue12 *q = this;
    dev12 *d = q->dev;
    for (uint32_t i = 0; i < n; i++) {
        list12 *l = lists[i];
        if (!l) continue;
        id<MTLCommandBuffer> cb = [d->queue commandBuffer];
        scratch sc = { nil, 0 };
        replay(d, l, cb, &sc);
        // (the scratch memory lives until the GPU is done with it: held by the handler; Metal holds the rest)
        id<MTLBuffer> held = sc.buf;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) { (void)held; }];
        [cb commit];
    }
}

static WINAPI HRESULT queue_signal(void *this, void *fence, uint64_t v) {
    queue12 *q = this;
    id<MTLCommandBuffer> cb = [q->dev->queue commandBuffer];
    [cb encodeSignalEvent:((fence12 *)fence)->event value:v];
    [cb commit];
    return S_OK;
}
static WINAPI HRESULT queue_wait(void *this, void *fence, uint64_t v) {
    queue12 *q = this;
    id<MTLCommandBuffer> cb = [q->dev->queue commandBuffer];
    [cb encodeWaitForEvent:((fence12 *)fence)->event value:v];
    [cb commit];
    return S_OK;
}
static WINAPI HRESULT queue_timestamp_frequency(void *this, uint64_t *f) { if (f) *f = 1000000000ull; return S_OK; }
static WINAPI Q_DESC *queue_get_desc(void *this, Q_DESC *out) { *out = ((queue12 *)this)->desc; return out; }

static WINAPI HRESULT dev_create_queue(void *this, const Q_DESC *desc, const GUID *iid, void **out) {
    dev12 *d = this;
    queue12 *q = alloc12(sizeof *q, K12_QUEUE, g_queue_vtbl, d);
    q->base.destroy = queue_destroy;
    q->desc = *desc;
    HRESULT hr = queue_query(q, iid, out);
    d3d_release(&q->base);
    return hr;
}

// ---- allocators and lists -----------------------------------------------------------------------------------------------

static WINAPI HRESULT allocator_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12CommandAllocator, &IID_ID3D12Pageable, &IID_ID3D12DeviceChild, &IID_ID3D12Object };
    return query(this, iid, out, k, 4);
}
static WINAPI HRESULT allocator_reset(void *this) { return S_OK; }
static WINAPI HRESULT dev_create_allocator(void *this, int32_t type, const GUID *iid, void **out) {
    allocator12 *a = alloc12(sizeof *a, K12_ALLOCATOR, g_allocator_vtbl, this);
    a->base.destroy = release_dev;
    a->type = type;
    HRESULT hr = allocator_query(a, iid, out);
    d3d_release(&a->base);
    return hr;
}
static WINAPI HRESULT dev_create_list(void *this, uint32_t node, int32_t type, void *allocator, void *pso, const GUID *iid, void **out) {
    list12 *l = alloc12(sizeof *l, K12_LIST, g_list_vtbl, this);
    l->base.destroy = list_destroy;
    l->type = type;
    if (pso) list_set_pso(l, pso);
    HRESULT hr = list_query(l, iid, out);
    d3d_release(&l->base);
    return hr;
}

static WINAPI HRESULT dev_create_heap(void *this, const HEAP_DESC *desc, const GUID *iid, void **out) {
    dev12 *d = this;
    heap12 *h = alloc12(sizeof *h, K12_HEAP, g_heap_vtbl, d);
    h->base.destroy = heap_destroy;
    h->desc = *desc;
    size_t size = 32 * (size_t)(desc->NumDescriptors ? desc->NumDescriptors : 1);
    if (desc->Flags & 1) {                                         // SHADER_VISIBLE
        h->gpu = [d->mtl newBufferWithLength:size options:MTLResourceStorageModeShared];
        h->cpu = h->gpu.contents;
        memset(h->cpu, 0, size);
        pthread_mutex_lock(&d->lock);
        [d->resources addObject:h->gpu];
        pthread_mutex_unlock(&d->lock);
    } else {
        h->cpu = calloc(1, size);
    }
    h->keep = (__strong id *)calloc(desc->NumDescriptors + 1, sizeof(id));
    pthread_mutex_lock(&g_heaps_lock);
    if (g_nheaps < 4096) g_heaps[g_nheaps++] = h;
    pthread_mutex_unlock(&g_heaps_lock);
    HRESULT hr = heap_query(h, iid, out);
    d3d_release(&h->base);
    return hr;
}

static WINAPI uint32_t dev_increment(void *this, int32_t type) { return 32; }

// ---- the device ---------------------------------------------------------------------------------------------------------

static WINAPI HRESULT dev_query(void *this, const GUID *iid, void **out) {
    const GUID *k[] = { &IID_ID3D12Device, &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device3, &IID_ID3D12Device4, &IID_ID3D12Device5,
                        &IID_ID3D12Device6, &IID_ID3D12Device7, &IID_ID3D12Device8, &IID_ID3D12Device9, &IID_ID3D12Device10, &IID_ID3D12Object };
    return query(this, iid, out, k, 12);
}
static WINAPI uint32_t dev_node_count(void *this) { return 1; }
static WINAPI HRESULT dev_removed_reason(void *this) { return S_OK; }
static WINAPI HRESULT dev_make_resident(void *this, uint32_t n, void *const *objs) { return S_OK; }
static WINAPI uint64_t *dev_adapter_luid(void *this, uint64_t *out) { *out = 0x5658525400000001ull; return out; }

// D3D12_FEATURE: only what tells a program what it may use.
static WINAPI HRESULT dev_check_feature(void *this, int32_t feature, void *data, uint32_t size) {
    switch (feature) {
    case 0:                                                        // D3D12_OPTIONS
        if (size >= 60) {
            memset(data, 0, size);
            uint32_t *o = data;
            o[0] = 1;                                              // DoublePrecisionFloatShaderOps (as float)
            o[1] = 1;                                              // OutputMergerLogicOp
            o[2] = 0;                                              // MinPrecisionSupport: none
            o[3] = 3;                                              // TiledResourcesTier: 0? (none)
            o[3] = 0;
            o[4] = 3;                                              // ResourceBindingTier 3
            o[5] = 1;                                              // PSSpecifiedStencilRefSupported
            o[6] = 1;                                              // TypedUAVLoadAdditionalFormats
            o[7] = 0;                                              // ROVsSupported
            o[8] = 0;                                              // ConservativeRasterizationTier
            o[9] = 40;                                             // MaxGPUVirtualAddressBitsPerResource
            o[10] = 1;                                             // StandardSwizzle64KBSupported
            o[11] = 0;                                             // CrossNodeSharingTier
            o[12] = 0;                                             // CrossAdapterRowMajorTextureSupported
            o[13] = 1;                                             // VPAndRTArrayIndexFromAnyShaderFeedingRasterizer...
            o[14] = 2;                                             // ResourceHeapTier 2
        }
        return S_OK;
    case 1: {                                                      // ARCHITECTURE {NodeIndex, TileBasedRenderer, UMA, CacheCoherentUMA}
        uint32_t *o = data;
        if (size >= 16) { o[1] = 1; o[2] = 1; o[3] = 1; }
        return S_OK;
    }
    case 2: {                                                      // FEATURE_LEVELS {NumFeatureLevels, pFeatureLevelsRequested, MaxSupportedFeatureLevel}
        struct { uint32_t n; uint32_t pad; const int32_t *levels; int32_t max; } *f = data;
        int32_t best = 0;
        for (uint32_t i = 0; i < f->n; i++) if (f->levels[i] <= 0xc000 && f->levels[i] > best) best = f->levels[i];
        f->max = best ? best : 0xb000;
        return S_OK;
    }
    case 7: {                                                      // SHADER_MODEL {HighestShaderModel}
        int32_t *sm = data;
        if (*sm > 0x66) *sm = 0x66;                                // (6.6: SM 6.7+ features are not translated yet)
        return S_OK;
    }
    case 13: {                                                     // ROOT_SIGNATURE {HighestVersion}
        int32_t *v = data;
        if (*v > 2) *v = 2;
        return S_OK;
    }
    }
    return E_INVALIDARG;
}

// GetCopyableFootprints: the layout of a texture's subresources in a buffer (rows 256-byte aligned, subresources 512).
static WINAPI void dev_footprints(void *this, const RES_DESC *desc, uint32_t first, uint32_t n, uint64_t base, FOOTPRINT *layouts,
                                  uint32_t *rows, uint64_t *row_bytes, uint64_t *total) {
    uint64_t off = base, all = 0;
    uint32_t bpp = bytes_per(desc->Format);
    uint32_t mips = desc->MipLevels ? desc->MipLevels : 1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sub = first + i, mip = sub % mips;
        uint32_t w = (uint32_t)MAX(1ull, desc->Width >> mip), h = desc->Dimension == 1 ? 1 : MAX(1u, desc->Height >> mip);
        uint32_t d = desc->Dimension == 4 ? MAX(1u, (uint32_t)desc->DepthOrArraySize >> mip) : 1;
        if (desc->Dimension == 1) { w = (uint32_t)desc->Width; bpp = 1; }
        uint64_t rb = (uint64_t)w * (bpp ? bpp : 4);
        uint32_t pitch = (uint32_t)((rb + 255) & ~255ull);
        off = (off + 511) & ~511ull;
        if (layouts) { layouts[i].offset = off; layouts[i].format = desc->Format; layouts[i].width = w; layouts[i].height = h; layouts[i].depth = d; layouts[i].row_pitch = pitch; }
        if (rows) rows[i] = h;
        if (row_bytes) row_bytes[i] = rb;
        uint64_t size = (uint64_t)pitch * h * d;
        all = off + size - base;
        off += size;
    }
    if (total) *total = all;
}

static WINAPI HRESULT dev_create_committed(void *this, const HEAP_PROPS *hp, int32_t flags, const RES_DESC *desc, int32_t state, const void *clear, const GUID *iid, void **out) {
    return create_committed(this, hp, flags, desc, state, clear, iid, out);
}

static void device_destroy(d3d_object *o) {
    dev12 *d = (dev12 *)o;
    d->resources = nil;
    d->pso_cache = nil;
    d->queue = nil;
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
    if (min_level > 0xc200) return E_INVALIDARG;                   // (feature level 12_2 and above: not offered)
    id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
    if (!mtl) return E_FAIL;
    if (!out) return S_FALSE;                                      // (a check that a device could be made)
    dev12 *d = d3d_alloc(sizeof *d, K12_DEVICE, g_device_vtbl, NULL);
    d->base.destroy = device_destroy;
    d->mtl = mtl;
    d->queue = [mtl newCommandQueue];
    d->resources = [NSMutableArray new];
    d->pso_cache = [NSMutableDictionary new];
    pthread_mutex_init(&d->lock, NULL);
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
    if (!desc || !blob) return E_INVALIDARG;
    *blob = serialize_rootsig(version == 2 ? 2 : 1, desc);
    return S_OK;
}
static WINAPI HRESULT D3D12SerializeVersionedRootSignature(const VERSIONED_RS_DESC *desc, void **blob, void **errors) {
    if (errors) *errors = NULL;
    if (!desc || !blob) return E_INVALIDARG;
    *blob = serialize_rootsig(desc->version == 2 || desc->version == 3 ? 2 : 1, &desc->desc);
    return S_OK;
}
static WINAPI HRESULT D3D12EnableExperimentalFeatures(uint32_t n, const GUID *iids, void *configs, uint32_t *sizes) { return E_NOINTERFACE; }
static WINAPI HRESULT D3D12GetInterface(const GUID *clsid, const GUID *iid, void **out) { if (out) *out = NULL; return E_NOINTERFACE; }

static void make_tables(void) {
    static const d3d_method device[] = {
        { "QueryInterface", (void *)dev_query }, COMMON_METHODS,
        { "GetNodeCount", (void *)dev_node_count }, { "CreateCommandQueue", (void *)dev_create_queue },
        { "CreateCommandAllocator", (void *)dev_create_allocator }, { "CreateComputePipelineState", (void *)dev_create_compute_pso },
        { "CreateCommandList", (void *)dev_create_list }, { "CheckFeatureSupport", (void *)dev_check_feature },
        { "CreateDescriptorHeap", (void *)dev_create_heap }, { "GetDescriptorHandleIncrementSize", (void *)dev_increment },
        { "CreateRootSignature", (void *)dev_create_rootsig }, { "CreateConstantBufferView", (void *)dev_create_cbv },
        { "CreateShaderResourceView", (void *)dev_create_srv }, { "CreateUnorderedAccessView", (void *)dev_create_uav },
        { "CreateSampler", (void *)dev_create_sampler }, { "CopyDescriptors", (void *)dev_copy_descriptors },
        { "CopyDescriptorsSimple", (void *)dev_copy_simple }, { "CreateCommittedResource", (void *)dev_create_committed },
        { "CreateFence", (void *)dev_create_fence }, { "GetDeviceRemovedReason", (void *)dev_removed_reason },
        { "GetCopyableFootprints", (void *)dev_footprints }, { "MakeResident", (void *)dev_make_resident },
        { "Evict", (void *)dev_make_resident }, { "GetAdapterLuid", (void *)dev_adapter_luid }, { NULL, NULL } };
    g_device_vtbl = D3D_VTABLE(ID3D12Device10, device);
    static const d3d_method queue[] = {
        { "QueryInterface", (void *)queue_query }, COMMON_METHODS, { "ExecuteCommandLists", (void *)queue_execute },
        { "Signal", (void *)queue_signal }, { "Wait", (void *)queue_wait }, { "GetTimestampFrequency", (void *)queue_timestamp_frequency },
        { "GetDesc", (void *)queue_get_desc }, { NULL, NULL } };
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
        { "CopyTextureRegion", (void *)list_copy_texture }, { "ResourceBarrier", (void *)list_barrier }, { NULL, NULL } };
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
    static const d3d_method pso[] = { { "QueryInterface", (void *)pso_query }, COMMON_METHODS, { NULL, NULL } };
    g_pso_vtbl = D3D_VTABLE(ID3D12PipelineState, pso);
    static const d3d_method resource[] = {
        { "QueryInterface", (void *)res_query }, COMMON_METHODS, { "Map", (void *)res_map }, { "Unmap", (void *)res_unmap },
        { "GetDesc", (void *)res_get_desc }, { "GetGPUVirtualAddress", (void *)res_gpu_va }, { "GetHeapProperties", (void *)res_get_heap_props }, { NULL, NULL } };
    g_resource_vtbl = D3D_VTABLE(ID3D12Resource2, resource);
    static const d3d_method blob[] = { { "QueryInterface", (void *)blob_query }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
                                       { "GetBufferPointer", (void *)blob_pointer }, { "GetBufferSize", (void *)blob_size }, { NULL, NULL } };
    g_blob_vtbl = D3D_VTABLE(ID3D10Blob, blob);
    static const d3d_method debug[] = { { "QueryInterface", (void *)debug_query }, { "AddRef", (void *)obj_addref }, { "Release", (void *)obj_release },
                                        { "EnableDebugLayer", (void *)debug_noop }, { "SetEnableGPUBasedValidation", (void *)debug_noop },
                                        { "SetEnableSynchronizedCommandQueueValidation", (void *)debug_noop }, { NULL, NULL } };
    g_debug_vtbl = D3D_VTABLE(ID3D12Debug5, debug);
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
