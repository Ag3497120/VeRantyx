// Direct3D 11 on Metal: the device, its resources and views, and the immediate context.
//
// Resources are Metal textures and buffers. The immediate context records clears and copies into a Metal command
// buffer that is committed on Flush, Map and Present, so the GPU sees them in the order the program issued them.
// Draws need shader translation (DXBC to Metal) and are the next step; until then they are reported once.
#include "d3d_common.h"
#include <os/os_sync_wait_on_address.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "d3d_tables.h"
#pragma clang diagnostic pop

static const GUID IID_IUnknown_ = { 0, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
int d3d_same_guid(const GUID *a, const GUID *b) { return !memcmp(a, b, sizeof *a); }

// ---- shared object machinery ------------------------------------------------------------------------------------------
HRESULT d3d_not_implemented(const char *what) {
    static char seen[512][64]; static int n;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    int known = 0;
    for (int i = 0; i < n && !known; i++) known = !strcmp(seen[i], what);
    if (!known && n < 512) snprintf(seen[n++], 64, "%s", what);
    pthread_mutex_unlock(&lock);
    if (!known) vx_log("Direct3D: %s is not implemented yet", what);
    return E_NOTIMPL;
}
void **d3d_vtable(const char *const *names, void *const *stubs, int count, const d3d_method *impl) {
    void **vt = calloc((size_t)count, sizeof(void *));
    for (int i = 0; i < count; i++) {
        vt[i] = stubs[i];
        for (const d3d_method *m = impl; m->name; m++) if (!strcmp(m->name, names[i])) { vt[i] = m->fn; break; }
    }
    return vt;
}
void *d3d_alloc(size_t size, int kind, void **vtbl, d3d_device *dev) {
    d3d_object *o = calloc(1, size);
    o->vtbl = vtbl; o->refs = 1; o->kind = kind; o->dev = dev;
    return o;
}
uint32_t d3d_addref(d3d_object *o) { return (uint32_t)__sync_add_and_fetch(&o->refs, 1); }
uint32_t d3d_release(d3d_object *o) {
    LONG r = __sync_sub_and_fetch(&o->refs, 1);
    if (r == 0) {
        if (o->destroy) o->destroy(o);
        while (o->pdata) { pdata *p = o->pdata; o->pdata = p->next; free(p->data); free(p); }
        free(o);
    }
    return (uint32_t)(r < 0 ? 0 : r);
}
HRESULT d3d_get_private(d3d_object *o, const GUID *g, uint32_t *size, void *data) {
    for (pdata *p = o->pdata; p; p = p->next) {
        if (!d3d_same_guid(&p->guid, g)) continue;
        if (!data) { *size = p->size; return S_OK; }
        if (*size < p->size) { *size = p->size; return (HRESULT)0x887A0003; }   // DXGI_ERROR_MORE_DATA
        memcpy(data, p->data, p->size);
        *size = p->size;
        return S_OK;
    }
    *size = 0;
    return DXGI_ERROR_NOT_FOUND;
}
HRESULT d3d_set_private(d3d_object *o, const GUID *g, uint32_t size, const void *data) {
    pdata **link = &o->pdata;
    while (*link && !d3d_same_guid(&(*link)->guid, g)) link = &(*link)->next;
    if (*link) { pdata *p = *link; *link = p->next; free(p->data); free(p); }
    if (!data) return S_OK;
    pdata *p = calloc(1, sizeof *p);
    p->guid = *g; p->size = size; p->data = malloc(size ? size : 1);
    memcpy(p->data, data, size);
    p->next = o->pdata; o->pdata = p;
    return S_OK;
}

// ---- formats -----------------------------------------------------------------------------------------------------------------
// DXGI formats Metal has: bytes per texel, or per 4x4 block for the block-compressed formats (block 4).
static const struct { uint32_t dxgi; MTLPixelFormat mtl; uint32_t bytes, block; } g_formats[] = {
    { 1, MTLPixelFormatRGBA32Float, 16, 1 }, { 2, MTLPixelFormatRGBA32Float, 16, 1 }, { 3, MTLPixelFormatRGBA32Uint, 16, 1 },
    { 4, MTLPixelFormatRGBA32Sint, 16, 1 }, { 9, MTLPixelFormatRGBA16Float, 8, 1 }, { 10, MTLPixelFormatRGBA16Float, 8, 1 },
    { 11, MTLPixelFormatRGBA16Unorm, 8, 1 }, { 12, MTLPixelFormatRGBA16Uint, 8, 1 }, { 13, MTLPixelFormatRGBA16Snorm, 8, 1 },
    { 14, MTLPixelFormatRGBA16Sint, 8, 1 }, { 15, MTLPixelFormatRG32Float, 8, 1 }, { 16, MTLPixelFormatRG32Float, 8, 1 },
    { 17, MTLPixelFormatRG32Uint, 8, 1 }, { 18, MTLPixelFormatRG32Sint, 8, 1 },
    { 19, MTLPixelFormatDepth32Float_Stencil8, 8, 1 }, { 20, MTLPixelFormatDepth32Float_Stencil8, 8, 1 },
    { 21, MTLPixelFormatDepth32Float_Stencil8, 8, 1 }, { 22, MTLPixelFormatX32_Stencil8, 8, 1 },
    { 23, MTLPixelFormatRGB10A2Unorm, 4, 1 }, { 24, MTLPixelFormatRGB10A2Unorm, 4, 1 }, { 25, MTLPixelFormatRGB10A2Uint, 4, 1 },
    { 26, MTLPixelFormatRG11B10Float, 4, 1 },
    { 27, MTLPixelFormatRGBA8Unorm, 4, 1 }, { 28, MTLPixelFormatRGBA8Unorm, 4, 1 }, { 29, MTLPixelFormatRGBA8Unorm_sRGB, 4, 1 },
    { 30, MTLPixelFormatRGBA8Uint, 4, 1 }, { 31, MTLPixelFormatRGBA8Snorm, 4, 1 }, { 32, MTLPixelFormatRGBA8Sint, 4, 1 },
    { 33, MTLPixelFormatRG16Float, 4, 1 }, { 34, MTLPixelFormatRG16Float, 4, 1 }, { 35, MTLPixelFormatRG16Unorm, 4, 1 },
    { 36, MTLPixelFormatRG16Uint, 4, 1 }, { 37, MTLPixelFormatRG16Snorm, 4, 1 }, { 38, MTLPixelFormatRG16Sint, 4, 1 },
    { 39, MTLPixelFormatR32Float, 4, 1 }, { 40, MTLPixelFormatDepth32Float, 4, 1 }, { 41, MTLPixelFormatR32Float, 4, 1 },
    { 42, MTLPixelFormatR32Uint, 4, 1 }, { 43, MTLPixelFormatR32Sint, 4, 1 },
    { 44, MTLPixelFormatDepth32Float_Stencil8, 8, 1 }, { 45, MTLPixelFormatDepth32Float_Stencil8, 8, 1 },   // no D24S8 on Apple GPUs
    { 46, MTLPixelFormatDepth32Float_Stencil8, 8, 1 }, { 47, MTLPixelFormatX32_Stencil8, 8, 1 },
    { 48, MTLPixelFormatRG8Unorm, 2, 1 }, { 49, MTLPixelFormatRG8Unorm, 2, 1 }, { 50, MTLPixelFormatRG8Uint, 2, 1 },
    { 51, MTLPixelFormatRG8Snorm, 2, 1 }, { 52, MTLPixelFormatRG8Sint, 2, 1 },
    { 53, MTLPixelFormatR16Float, 2, 1 }, { 54, MTLPixelFormatR16Float, 2, 1 }, { 55, MTLPixelFormatDepth16Unorm, 2, 1 },
    { 56, MTLPixelFormatR16Unorm, 2, 1 }, { 57, MTLPixelFormatR16Uint, 2, 1 }, { 58, MTLPixelFormatR16Snorm, 2, 1 },
    { 59, MTLPixelFormatR16Sint, 2, 1 }, { 60, MTLPixelFormatR8Unorm, 1, 1 }, { 61, MTLPixelFormatR8Unorm, 1, 1 },
    { 62, MTLPixelFormatR8Uint, 1, 1 }, { 63, MTLPixelFormatR8Snorm, 1, 1 }, { 64, MTLPixelFormatR8Sint, 1, 1 },
    { 65, MTLPixelFormatA8Unorm, 1, 1 }, { 67, MTLPixelFormatRGB9E5Float, 4, 1 },
    { 70, MTLPixelFormatBC1_RGBA, 8, 4 }, { 71, MTLPixelFormatBC1_RGBA, 8, 4 }, { 72, MTLPixelFormatBC1_RGBA_sRGB, 8, 4 },
    { 73, MTLPixelFormatBC2_RGBA, 16, 4 }, { 74, MTLPixelFormatBC2_RGBA, 16, 4 }, { 75, MTLPixelFormatBC2_RGBA_sRGB, 16, 4 },
    { 76, MTLPixelFormatBC3_RGBA, 16, 4 }, { 77, MTLPixelFormatBC3_RGBA, 16, 4 }, { 78, MTLPixelFormatBC3_RGBA_sRGB, 16, 4 },
    { 79, MTLPixelFormatBC4_RUnorm, 8, 4 }, { 80, MTLPixelFormatBC4_RUnorm, 8, 4 }, { 81, MTLPixelFormatBC4_RSnorm, 8, 4 },
    { 82, MTLPixelFormatBC5_RGUnorm, 16, 4 }, { 83, MTLPixelFormatBC5_RGUnorm, 16, 4 }, { 84, MTLPixelFormatBC5_RGSnorm, 16, 4 },
    { 85, MTLPixelFormatB5G6R5Unorm, 2, 1 }, { 86, MTLPixelFormatBGR5A1Unorm, 2, 1 },
    { 87, MTLPixelFormatBGRA8Unorm, 4, 1 }, { 88, MTLPixelFormatBGRA8Unorm, 4, 1 },
    { 90, MTLPixelFormatBGRA8Unorm, 4, 1 }, { 91, MTLPixelFormatBGRA8Unorm_sRGB, 4, 1 }, { 92, MTLPixelFormatBGRA8Unorm, 4, 1 },
    { 93, MTLPixelFormatBGRA8Unorm_sRGB, 4, 1 },
    { 94, MTLPixelFormatBC6H_RGBUfloat, 16, 4 }, { 95, MTLPixelFormatBC6H_RGBUfloat, 16, 4 }, { 96, MTLPixelFormatBC6H_RGBFloat, 16, 4 },
    { 97, MTLPixelFormatBC7_RGBAUnorm, 16, 4 }, { 98, MTLPixelFormatBC7_RGBAUnorm, 16, 4 }, { 99, MTLPixelFormatBC7_RGBAUnorm_sRGB, 16, 4 },
    { 115, MTLPixelFormatABGR4Unorm, 2, 1 },
};
MTLPixelFormat d3d_mtl_format(uint32_t dxgi, uint32_t *bytes) {
    for (size_t i = 0; i < sizeof g_formats / sizeof g_formats[0]; i++)
        if (g_formats[i].dxgi == dxgi) { if (bytes) *bytes = g_formats[i].block == 1 ? g_formats[i].bytes : 0; return g_formats[i].mtl; }
    if (bytes) *bytes = 0;
    return MTLPixelFormatInvalid;
}
// The layout of a format's data: bytes per row of `width` texels and the number of rows for `height` (rows of
// 4x4 blocks for the block-compressed formats).
static int format_layout(uint32_t dxgi, uint32_t width, uint32_t height, uint32_t *row_bytes, uint32_t *rows) {
    for (size_t i = 0; i < sizeof g_formats / sizeof g_formats[0]; i++) {
        if (g_formats[i].dxgi != dxgi) continue;
        uint32_t b = g_formats[i].block;
        *row_bytes = (width + b - 1) / b * g_formats[i].bytes;
        *rows = (height + b - 1) / b;
        return 1;
    }
    *row_bytes = *rows = 0;
    return 0;
}
int d3d_is_srgb(uint32_t dxgi) { return dxgi == 29 || dxgi == 91 || dxgi == 93; }
static int is_depth(MTLPixelFormat f) {
    return f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float_Stencil8;
}

// ---- interfaces shared by device children -----------------------------------------------------------------------------------
static d3d_object *self_of(void *this) { return (d3d_object *)this; }
static WINAPI uint32_t child_addref(void *this) { return d3d_addref(self_of(this)); }
static WINAPI uint32_t child_release(void *this) { return d3d_release(self_of(this)); }
static WINAPI void child_get_device(void *this, void **out) { d3d_device *d = self_of(this)->dev; d3d_addref(&d->base); *out = d; }
static WINAPI HRESULT child_get_private(void *this, const GUID *g, uint32_t *size, void *data) { return d3d_get_private(self_of(this), g, size, data); }
static WINAPI HRESULT child_set_private(void *this, const GUID *g, uint32_t size, const void *data) { return d3d_set_private(self_of(this), g, size, data); }
static WINAPI HRESULT child_set_private_interface(void *this, const GUID *g, void *unk) { (void)unk; return d3d_set_private(self_of(this), g, 0, NULL); }
#define CHILD_METHODS \
    { "AddRef", (void *)child_addref }, { "Release", (void *)child_release }, { "GetDevice", (void *)child_get_device }, \
    { "GetPrivateData", (void *)child_get_private }, { "SetPrivateData", (void *)child_set_private }, \
    { "SetPrivateDataInterface", (void *)child_set_private_interface }

// QueryInterface for objects known by one interface and its bases.
static HRESULT query(void *this, const GUID *iid, void **out, const GUID *const *known, int n) {
    if (!out) return E_POINTER;
    for (int i = 0; i < n; i++)
        if (d3d_same_guid(iid, known[i])) { d3d_addref(self_of(this)); *out = this; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}

// ---- textures -------------------------------------------------------------------------------------------------------------------
static void **g_texture_vtbl, **g_buffer_vtbl, **g_rtv_vtbl, **g_dsv_vtbl, **g_srv_vtbl;
// The DXGI side of a resource (IDXGIResource1, through its second table) and a shared texture's keyed mutex.
static int dxgi_resource_qi(d3d_object *o, const GUID *iid, void **out) {
    if (d3d_same_guid(iid, &IID_IDXGIResource) || d3d_same_guid(iid, &IID_IDXGIResource1) || d3d_same_guid(iid, &IID_IDXGIObject) ||
        d3d_same_guid(iid, &IID_IDXGIDeviceSubObject)) { d3d_addref(o); *out = &o->vtbl2; return 1; }
    if (o->kind == K_TEXTURE && ((d3d_texture *)o)->keyed_vtbl && d3d_same_guid(iid, &IID_IDXGIKeyedMutex)) {
        d3d_addref(o); *out = &((d3d_texture *)o)->keyed_vtbl; return 1;
    }
    return 0;
}
static WINAPI HRESULT texture_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture2D };
    if (out && dxgi_resource_qi(self_of(this), iid, out)) return S_OK;
    return query(this, iid, out, known, 4);
}
static WINAPI void texture_get_type(void *this, uint32_t *dim) { *dim = ((d3d_texture *)this)->dim + 1; }   // D3D11_RESOURCE_DIMENSION_TEXTURE1D..3D
static WINAPI void texture_set_eviction(void *this, uint32_t p) { (void)this; (void)p; }
static WINAPI uint32_t texture_get_eviction(void *this) { (void)this; return 0; }
static WINAPI void texture_get_desc(void *this, uint32_t *desc) { memcpy(desc, ((d3d_texture *)this)->desc, sizeof(uint32_t) * TD_COUNT); }
// D3D11_TEXTURE1D_DESC: width, mips, array size, format, usage, bind, CPU access, misc.
static WINAPI void texture1d_get_desc(void *this, uint32_t *desc) {
    const uint32_t *d = ((d3d_texture *)this)->desc;
    uint32_t v[8] = { d[TD_WIDTH], d[TD_MIPS], d[TD_ARRAY], d[TD_FORMAT], d[TD_USAGE], d[TD_BIND], d[TD_CPU], d[TD_MISC] };
    memcpy(desc, v, sizeof v);
}
// D3D11_TEXTURE3D_DESC: width, height, depth, mips, format, usage, bind, CPU access, misc.
static WINAPI void texture3d_get_desc(void *this, uint32_t *desc) {
    const d3d_texture *t = this;
    const uint32_t *d = t->desc;
    uint32_t v[9] = { d[TD_WIDTH], d[TD_HEIGHT], t->depth, d[TD_MIPS], d[TD_FORMAT], d[TD_USAGE], d[TD_BIND], d[TD_CPU], d[TD_MISC] };
    memcpy(desc, v, sizeof v);
}
static WINAPI HRESULT texture1d_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture1D };
    return query(this, iid, out, known, 4);
}
static WINAPI HRESULT texture3d_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Texture3D };
    return query(this, iid, out, known, 4);
}
static void texture_destroy(d3d_object *o) {
    d3d_texture *t = (d3d_texture *)o;
    if (t->mtl) CFBridgingRelease(t->mtl);
    if (t->shared) d3d_shared_release(t->shared - 1);            // only its creator's release frees the slot
    free(t->cpu);
}

#define USAGE_DEFAULT 0
#define USAGE_IMMUTABLE 1
#define USAGE_DYNAMIC 2
#define USAGE_STAGING 3
#define BIND_SHADER_RESOURCE 0x8
#define BIND_RENDER_TARGET 0x20
#define BIND_DEPTH_STENCIL 0x40
#define BIND_UNORDERED_ACCESS 0x80
#define MISC_TEXTURECUBE 0x4

#define MISC_SHARED_ANY (0x2 | 0x100 | 0x800)          // D3D11_RESOURCE_MISC_SHARED, _SHARED_KEYEDMUTEX, _SHARED_NTHANDLE
static void *make_mtl_texture(d3d_device *dev, const uint32_t *desc, uint32_t dim, uint32_t depth, HRESULT *hr, int *shared_slot) {
    uint32_t bytes;
    MTLPixelFormat f = d3d_mtl_format(desc[TD_FORMAT], &bytes);
    if (f == MTLPixelFormatInvalid) { vx_log("Direct3D: texture format %u is not supported yet", desc[TD_FORMAT]); *hr = E_INVALIDARG; return NULL; }
    if (desc[TD_BIND] & BIND_DEPTH_STENCIL) {                  // typeless formats that are depth buffers
        if (desc[TD_FORMAT] == 39) f = MTLPixelFormatDepth32Float;
        else if (desc[TD_FORMAT] == 53) f = MTLPixelFormatDepth16Unorm;
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.pixelFormat = f;
    td.width = desc[TD_WIDTH] ? desc[TD_WIDTH] : 1;
    td.height = desc[TD_HEIGHT] ? desc[TD_HEIGHT] : 1;
    uint32_t full = 1;
    for (uint32_t m = MAX(MAX((uint32_t)td.width, (uint32_t)td.height), depth); m > 1; m >>= 1) full++;
    td.mipmapLevelCount = desc[TD_MIPS] ? desc[TD_MIPS] : full;
    uint32_t array = desc[TD_ARRAY] ? desc[TD_ARRAY] : 1, samples = desc[TD_SAMPLES] ? desc[TD_SAMPLES] : 1;
    // A sample count the GPU does not have becomes the highest it has (Apple GPUs: up to 4; 8 is common on PCs).
    id<MTLDevice> mdev = (__bridge id<MTLDevice>)dev->mtl;
    while (samples > 1 && ![mdev supportsTextureSampleCount:samples]) samples >>= 1;
    if (samples != (desc[TD_SAMPLES] ? desc[TD_SAMPLES] : 1)) {
        static int told;
        if (!told++) vx_log("Direct3D: %u samples per pixel are not available on this GPU: %u are used", desc[TD_SAMPLES], samples);
    }
    if (dim == 3) { td.textureType = MTLTextureType3D; td.depth = depth ? depth : 1; }
    else if (dim == 1) {                                       // Metal's 1D textures have one mip level
        td.textureType = array > 1 ? MTLTextureType1DArray : MTLTextureType1D;
        if (array > 1) td.arrayLength = array;
        td.height = 1;
        td.mipmapLevelCount = 1;
    } else if (samples > 1) { td.textureType = array > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample; td.sampleCount = samples; td.mipmapLevelCount = 1; }
    else if ((desc[TD_MISC] & MISC_TEXTURECUBE) && array % 6 == 0) { td.textureType = array > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube; if (array > 6) td.arrayLength = array / 6; }
    else if (array > 1) { td.textureType = MTLTextureType2DArray; td.arrayLength = array; }
    MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (desc[TD_BIND] & (BIND_RENDER_TARGET | BIND_DEPTH_STENCIL)) usage |= MTLTextureUsageRenderTarget;
    if (desc[TD_BIND] & BIND_UNORDERED_ACCESS) usage |= MTLTextureUsageShaderWrite;
    if ((desc[TD_BIND] & BIND_UNORDERED_ACCESS) && (desc[TD_FORMAT] == 39 || desc[TD_FORMAT] == 42 || desc[TD_FORMAT] == 43)) usage |= MTLTextureUsageShaderAtomic;   // R32 (typeless, uint, sint): atomics
    td.usage = usage;
    int cpu = desc[TD_USAGE] == USAGE_STAGING || desc[TD_USAGE] == USAGE_DYNAMIC;
    td.storageMode = cpu && !is_depth(f) && samples == 1 ? MTLStorageModeShared : MTLStorageModePrivate;
    *shared_slot = -1;
    if (desc[TD_MISC] & MISC_SHARED_ANY) {                     // shared with other processes: an IOSurface
        void *shared = d3d_shared_texture(dev, td, desc, bytes, shared_slot);
        if (shared) { *hr = S_OK; return shared; }
    }
    // A staging texture of one plain 2D image lies in a buffer, row after row: Map hands out its memory instead of
    // copying the image out (Metal keeps other textures in the GPU's tiled order, and reading one back took 4 ms
    // for a 1500x1700 eye in VR, every frame).
    uint32_t row_bytes, rows;
    if (desc[TD_USAGE] == USAGE_STAGING && dim == 2 && td.textureType == MTLTextureType2D && td.mipmapLevelCount == 1 && !is_depth(f) &&
        format_layout(desc[TD_FORMAT], (uint32_t)td.width, (uint32_t)td.height, &row_bytes, &rows) && rows == td.height && !getenv("VXRT_TILED_STAGING")) {
        NSUInteger align = [mdev minimumLinearTextureAlignmentForPixelFormat:f], pitch = (row_bytes + align - 1) / align * align;
        id<MTLBuffer> buf = [mdev newBufferWithLength:pitch * td.height options:MTLResourceStorageModeShared];
        id<MTLTexture> tex = buf ? [buf newTextureWithDescriptor:td offset:0 bytesPerRow:pitch] : nil;
        if (tex) { *hr = S_OK; return (void *)CFBridgingRetain(tex); }
    }
    id<MTLTexture> tex = [(__bridge id<MTLDevice>)dev->mtl newTextureWithDescriptor:td];
    if (!tex) { *hr = E_OUTOFMEMORY; return NULL; }
    *hr = S_OK;
    return (void *)CFBridgingRetain(tex);
}

static void **g_texture1d_vtbl, **g_texture3d_vtbl;
static d3d_texture *texture_create(d3d_device *dev, const uint32_t desc[TD_COUNT], uint32_t dim, uint32_t depth, HRESULT *hr) {
    int slot;
    void *mtl = make_mtl_texture(dev, desc, dim, depth, hr, &slot);
    if (!mtl) return NULL;
    d3d_texture *t = d3d_alloc(sizeof *t, K_TEXTURE, dim == 1 ? g_texture1d_vtbl : dim == 3 ? g_texture3d_vtbl : g_texture_vtbl, dev);
    t->dim = dim; t->depth = dim == 3 ? depth : 1;
    memcpy(t->desc, desc, sizeof t->desc);
    id<MTLTexture> tex = (__bridge id<MTLTexture>)mtl;
    t->desc[TD_MIPS] = (uint32_t)tex.mipmapLevelCount;
    if (!t->desc[TD_ARRAY]) t->desc[TD_ARRAY] = 1;
    if (!t->desc[TD_SAMPLES]) t->desc[TD_SAMPLES] = 1;
    t->mtl = mtl;
    t->base.destroy = texture_destroy;
    t->base.vtbl2 = d3d_dxgi_resource_vtbl();
    t->shared = slot + 1;
    if (t->shared && (desc[TD_MISC] & 0x100)) t->keyed_vtbl = d3d_keyed_mutex_vtbl();
    d3d_shared_note(t);
    return t;
}
// A texture object for a Metal texture made elsewhere (a shared texture another process made).
d3d_texture *d3d_texture_wrap(d3d_device *dev, const uint32_t desc[TD_COUNT], void *mtl, int shared) {
    d3d_texture *t = d3d_alloc(sizeof *t, K_TEXTURE, g_texture_vtbl, dev);
    t->dim = 2; t->depth = 1;
    memcpy(t->desc, desc, sizeof t->desc);
    t->mtl = mtl;
    t->base.destroy = texture_destroy;
    t->base.vtbl2 = d3d_dxgi_resource_vtbl();
    t->shared = shared;
    if (desc[TD_MISC] & 0x100) t->keyed_vtbl = d3d_keyed_mutex_vtbl();
    return t;
}
d3d_texture *d3d_texture_create(d3d_device *dev, const uint32_t desc[TD_COUNT], HRESULT *hr) { return texture_create(dev, desc, 2, 1, hr); }
void d3d_texture_replace(d3d_texture *t, void *mtl_texture) {
    if (t->mtl) CFBridgingRelease(t->mtl);
    t->mtl = mtl_texture;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)mtl_texture;
    t->desc[TD_WIDTH] = (uint32_t)tex.width; t->desc[TD_HEIGHT] = (uint32_t)tex.height;
    free(t->cpu); t->cpu = NULL;
}

// ---- buffers -----------------------------------------------------------------------------------------------------------------------
static WINAPI HRESULT buffer_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_ID3D11DeviceChild, &IID_ID3D11Resource, &IID_ID3D11Buffer };
    if (out && dxgi_resource_qi(self_of(this), iid, out)) return S_OK;
    return query(this, iid, out, known, 4);
}
static WINAPI void buffer_get_type(void *this, uint32_t *dim) { (void)this; *dim = 1; }   // D3D11_RESOURCE_DIMENSION_BUFFER
static WINAPI void buffer_get_desc(void *this, uint32_t *desc) { memcpy(desc, ((d3d_buffer *)this)->desc, sizeof(uint32_t) * 6); }
static void drop_slice(d3d_device *dev, d3d_buffer *b);
static void buffer_destroy(d3d_object *o) {
    d3d_buffer *b = (d3d_buffer *)o;
    drop_slice(o->dev, b);
    if (b->mtl) CFBridgingRelease(b->mtl);
    if (b->shm) munmap(b->shm, b->shm_size);
    if (b->shared) d3d_shared_release(b->shared - 1);
}
d3d_buffer *d3d_buffer_wrap(d3d_device *dev, const uint32_t desc[6], void *mtl, int shared, void *shm, size_t shm_size) {
    d3d_buffer *b = d3d_alloc(sizeof *b, K_BUFFER, g_buffer_vtbl, dev);
    memcpy(b->desc, desc, sizeof b->desc);
    b->mtl = mtl; b->shared = shared; b->shm = shm; b->shm_size = shm_size;
    b->base.destroy = buffer_destroy;
    b->base.vtbl2 = d3d_dxgi_resource_vtbl();
    return b;
}

// ---- views -------------------------------------------------------------------------------------------------------------------------
static WINAPI HRESULT rtv_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_ID3D11DeviceChild, &IID_ID3D11RenderTargetView };
    return query(this, iid, out, known, 3);
}
static WINAPI HRESULT dsv_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_ID3D11DeviceChild, &IID_ID3D11DepthStencilView };
    return query(this, iid, out, known, 3);
}
static WINAPI HRESULT srv_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_ID3D11DeviceChild, &IID_ID3D11ShaderResourceView };
    return query(this, iid, out, known, 3);
}
static WINAPI void view_get_resource(void *this, void **out) { d3d_view *v = this; d3d_addref(v->resource); *out = v->resource; }
// GetDesc of a view: the D3D11_*_VIEW_DESC it was made with (format, dimension, then the dimension's fields), the
// dimension worked out from the resource for a view made without a description. which: 0 SRV, 1 RTV, 2 DSV, 3 UAV.
void d3d_view_get_desc(const d3d_view *v, int which, uint32_t *out) {
    memset(out, 0, which == 0 ? 24 : which == 2 ? 24 : 20);
    uint32_t dim = v->dim;
    const d3d_object *r = v->resource;
    if (!dim) {
        if (r->kind == K_BUFFER) dim = which == 2 ? 0 : 1;
        else {
            const d3d_texture *t = (const d3d_texture *)r;
            int ms = t->desc[TD_SAMPLES] > 1, array = t->desc[TD_ARRAY] > 1, cube = which == 0 && (t->desc[TD_MISC] & 0x4);
            if (t->dim == 1) dim = which == 2 ? (array ? 2 : 1) : (array ? 3 : 2);
            else if (t->dim == 3) dim = 8;
            else if (cube) dim = t->desc[TD_ARRAY] > 6 ? 10 : 9;
            else if (which == 2) dim = ms ? (array ? 6 : 5) : (array ? 4 : 3);
            else dim = ms ? (array ? 7 : 6) : (array ? 5 : 4);
        }
    }
    out[0] = v->format; out[1] = dim;
    uint32_t *u = out + (which == 2 ? 3 : 2);                  // a DSV has its flags first
    if (r->kind == K_BUFFER) { u[0] = v->first; u[1] = v->count; if (which == 3 || dim == 11) u[2] = v->flags; return; }
    const d3d_texture *t = (const d3d_texture *)r;
    uint32_t mips = v->mips ? v->mips : t->desc[TD_MIPS] - v->mip, slices = v->slices ? v->slices : 1;
    if (which == 0) {                                          // shader resource view
        switch (dim) {
        case 2: case 4: case 8: case 9: u[0] = v->mip; u[1] = mips; break;
        case 3: case 5: u[0] = v->mip; u[1] = mips; u[2] = v->first_slice; u[3] = v->dim ? slices : t->desc[TD_ARRAY]; break;
        case 10: u[0] = v->mip; u[1] = mips; u[2] = v->first_slice; u[3] = (v->dim ? slices : t->desc[TD_ARRAY]) / 6; break;
        case 7: u[0] = v->first_slice; u[1] = v->dim ? slices : t->desc[TD_ARRAY]; break;
        }
    } else {                                                   // render target, depth stencil, unordered access views
        int array = which == 2 ? (dim == 2 || dim == 4) : (dim == 3 || dim == 5);
        int ms_array = which == 2 ? dim == 6 : dim == 7;
        u[0] = ms_array ? v->first_slice : v->mip;
        if (array) { u[1] = v->first_slice; u[2] = v->dim ? slices : t->desc[TD_ARRAY]; }
        else if (ms_array) u[1] = v->dim ? slices : t->desc[TD_ARRAY];
        else if (dim == 8) { u[1] = 0; u[2] = t->depth; }       // 3D: first W slice, W size
    }
}
static WINAPI void srv_get_desc(void *this, uint32_t *out) { d3d_view_get_desc(this, 0, out); }
static WINAPI void rtv_get_desc(void *this, uint32_t *out) { d3d_view_get_desc(this, 1, out); }
static WINAPI void dsv_get_desc(void *this, uint32_t *out) { d3d_view_get_desc(this, 2, out); }
void d3d_view_release_mtl(d3d_view *v) {
    if (v->counter) CFRelease(v->counter);
    if (v->mtl_view) CFRelease(v->mtl_view);
    v->counter = v->mtl_view = v->mtl_src = NULL;
}
static void view_destroy(d3d_object *o) { d3d_view_release_mtl((d3d_view *)o); d3d_release(((d3d_view *)o)->resource); }

// The views' descriptions: format, dimension, then per dimension the mip and slice fields (their layouts differ
// between shader resource, render target and depth views).
static d3d_view *make_view(d3d_device *dev, int kind, void **vtbl, d3d_object *res, const uint32_t *desc, int dsv) {
    d3d_view *v = d3d_alloc(sizeof *v, kind, vtbl, dev);
    v->resource = res;
    d3d_addref(res);
    v->base.destroy = view_destroy;
    if (res->kind == K_TEXTURE) v->format = ((d3d_texture *)res)->desc[TD_FORMAT];
    if (desc) {
        if (desc[0]) v->format = desc[0];
        uint32_t dim = v->dim = desc[1];
        const uint32_t *u = desc + (dsv ? 3 : 2);              // a DSV has a Flags field before its union
        if (res->kind == K_BUFFER) { v->first = u[0]; v->count = u[1]; v->flags = dim == 11 ? u[2] : 0; }   // BUFFER / BUFFEREX
        else if (kind == K_SRV) {
            switch (dim) {
            case 2: case 4: case 8: case 9: v->mip = u[0]; v->mips = u[1]; break;                       // 1D, 2D, 3D, cube
            case 3: case 5: v->mip = u[0]; v->mips = u[1]; v->first_slice = u[2]; v->slices = u[3]; break;   // arrays
            case 10: v->mip = u[0]; v->mips = u[1]; v->first_slice = u[2]; v->slices = u[3] * 6; break;      // cube array
            case 7: v->first_slice = u[0]; v->slices = u[1]; break;                                     // 2D MS array
            }
        } else {
            uint32_t tex1d_array = dsv ? 2 : 3, tex2d = dsv ? 3 : 4, tex2d_array = dsv ? 4 : 5, ms_array = dsv ? 6 : 7;
            if (dim == tex2d || dim == (dsv ? 1u : 2u)) v->mip = u[0];
            else if (dim == tex2d_array || dim == tex1d_array || (!dsv && dim == 8)) { v->mip = u[0]; v->first_slice = u[1]; v->slices = u[2]; }
            else if (dim == ms_array) { v->first_slice = u[0]; v->slices = u[1]; }
        }
    }
    if (!v->slices) v->slices = 1;
    return v;
}

// ---- the immediate context ----------------------------------------------------------------------------------------------------------
static id<MTLCommandBuffer> context_cmd(d3d_context *c) {
    if (!c->cmd) c->cmd = (void *)CFBridgingRetain([(__bridge id<MTLCommandQueue>)c->base.dev->queue commandBuffer]);
    return (__bridge id<MTLCommandBuffer>)c->cmd;
}
void *d3d_context_cmd(d3d_context *c) { return (__bridge void *)context_cmd(c); }
// Clears, copies and uploads are separate passes: the render pass in progress ends first.
void d3d_end_render(d3d_context *c) {
    if (!c->render) return;
    id<MTLRenderCommandEncoder> enc = CFBridgingRelease(c->render);
    c->render = NULL;
    [enc endEncoding];
    for (int i = 0; i < 8; i++) if (c->render_rtv[i]) { d3d_release(&c->render_rtv[i]->base); c->render_rtv[i] = NULL; }
    if (c->render_dsv) { d3d_release(&c->render_dsv->base); c->render_dsv = NULL; }
}
void d3d_vtable_override(void **vt, const char *const *names, int count, const d3d_method *impl) {
    for (int i = 0; i < count; i++)
        for (const d3d_method *m = impl; m->name; m++) if (!strcmp(m->name, names[i])) { vt[i] = m->fn; break; }
}
// VXRT_STATS=1: every 2 seconds, how busy the GPU was with this process's command buffers.
static void gpu_stats(double start, double end) {
    static int on = -1;
    static double busy, since, last_end;
    if (on < 0) on = getenv("VXRT_STATS") != NULL;
    if (!on || end <= start) return;
    double from = start > last_end ? start : last_end;     // command buffers can overlap
    if (end > from) busy += end - from;
    if (end > last_end) last_end = end;
    if (!since) since = start;
    if (end - since >= 2.0) {
        vx_log("GPU: busy %.0f%% of the last %.1f s with this process's work", 100 * busy / (end - since), end - since);
        busy = 0; since = end;
    }
}
void d3d_mark_gpu(d3d_context *c, d3d_object *o) { o->gpu_seq = __atomic_load_n(&c->base.dev->submitted, __ATOMIC_ACQUIRE) + 1; }
// Waits until the GPU is done with what was recorded for a resource (Map of a staging resource): only up to the
// command buffer that last copied to or from it, not everything. DO_NOT_WAIT: DXGI_ERROR_WAS_STILL_DRAWING instead.
#define MAP_FLAG_DO_NOT_WAIT 0x100000
#define DXGI_ERROR_WAS_STILL_DRAWING ((HRESULT)0x887A000A)
#define RECENT 16                                  // the last command buffers committed, by sequence number
static void *g_recent[RECENT];
static uint64_t g_recent_seq[RECENT];
static d3d_device *g_recent_dev[RECENT];
static pthread_mutex_t g_recent_lock = PTHREAD_MUTEX_INITIALIZER;
static HRESULT wait_gpu(d3d_context *c, d3d_object *o, uint32_t flags) {
    d3d_device *dev = c->base.dev;
    uint64_t seq = o->gpu_seq;
    static int trace = -1;
    if (trace < 0) trace = getenv("VXRT_TRACE_MAPS") != NULL;
    if (trace) vx_log("Map of a staging resource: copied by command buffer %llu; %llu submitted, %llu done, recording %d",
                      (unsigned long long)seq, (unsigned long long)dev->submitted, (unsigned long long)dev->completed, ((d3d_context *)dev->context)->cmd != NULL);
    // Work recorded so far goes to the GPU now, as drivers flush on Map: programs that read back frames late
    // (copy to one staging resource, Map another copied earlier) otherwise find their copies never started.
    if (((d3d_context *)dev->context)->cmd) d3d_flush(dev, 0);
    if (!seq || __atomic_load_n(&dev->completed, __ATOMIC_ACQUIRE) >= seq) return S_OK;
    // The command buffer itself says first when the GPU is done (completion handlers run a little later).
    id<MTLCommandBuffer> cmd = nil;
    pthread_mutex_lock(&g_recent_lock);
    if (g_recent_seq[seq % RECENT] == seq && g_recent_dev[seq % RECENT] == dev && g_recent[seq % RECENT]) cmd = (__bridge id<MTLCommandBuffer>)g_recent[seq % RECENT];
    pthread_mutex_unlock(&g_recent_lock);
    if (flags & MAP_FLAG_DO_NOT_WAIT)
        return __atomic_load_n(&dev->completed, __ATOMIC_ACQUIRE) >= seq || (cmd && cmd.status >= MTLCommandBufferStatusCompleted) ? S_OK : DXGI_ERROR_WAS_STILL_DRAWING;
    if (cmd) { [cmd waitUntilCompleted]; return S_OK; }
    for (uint64_t now; (now = __atomic_load_n(&dev->completed, __ATOMIC_ACQUIRE)) < seq; ) {
        if (__builtin_available(macOS 14.4, *))
            os_sync_wait_on_address_with_timeout((void *)&dev->completed, now, 8, OS_SYNC_WAIT_ON_ADDRESS_NONE, OS_CLOCK_MACH_ABSOLUTE_TIME, 2000000);
        else usleep(100);
    }
    return S_OK;
}
void d3d_flush(d3d_device *dev, int wait) {
    d3d_context *c = (d3d_context *)dev->context;
    d3d_end_render(c);
    if (!c->cmd) {
        if (wait) {                                             // still wait for work committed earlier
            id<MTLCommandBuffer> fence = [(__bridge id<MTLCommandQueue>)dev->queue commandBuffer];
            [fence commit];
            [fence waitUntilCompleted];
        }
        return;
    }
    id<MTLCommandBuffer> cmd = CFBridgingRelease(c->cmd);
    c->cmd = NULL;
    uint64_t seq = __atomic_add_fetch(&dev->submitted, 1, __ATOMIC_ACQ_REL);
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> done) {
        gpu_stats(done.GPUStartTime, done.GPUEndTime);
        uint64_t prev = __atomic_load_n(&dev->completed, __ATOMIC_ACQUIRE);
        while (prev < seq && !__atomic_compare_exchange_n(&dev->completed, &prev, seq, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
        if (__builtin_available(macOS 14.4, *)) os_sync_wake_by_address_all((void *)&dev->completed, 8, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    }];
    [cmd commit];
    pthread_mutex_lock(&g_recent_lock);
    void *old = g_recent[seq % RECENT];
    g_recent[seq % RECENT] = (void *)CFBridgingRetain(cmd); g_recent_seq[seq % RECENT] = seq; g_recent_dev[seq % RECENT] = dev;
    pthread_mutex_unlock(&g_recent_lock);
    if (old) CFRelease(old);
    if (wait) [cmd waitUntilCompleted];
}

static WINAPI HRESULT context_qi(void *this, const GUID *iid, void **out) {
    static const GUID *const known[] = { &IID_IUnknown_, &IID_ID3D11DeviceChild, &IID_ID3D11DeviceContext, &IID_ID3D11DeviceContext1 };
    return query(this, iid, out, known, 4);
}

// A render pass that only clears: color attachment 0, or depth/stencil.
static void clear_pass(d3d_context *c, d3d_view *v, const float *color, int depth, float depth_value, int stencil, uint8_t stencil_value) {
    d3d_texture *t = (d3d_texture *)v->resource;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)t->mtl;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (color) {
        rp.colorAttachments[0].texture = tex;
        rp.colorAttachments[0].level = v->mip;
        if (rp.colorAttachments[0].texture.textureType == MTLTextureType3D) rp.colorAttachments[0].depthPlane = v->first_slice;
        else rp.colorAttachments[0].slice = v->first_slice;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(color[0], color[1], color[2], color[3]);
        if (v->slices > 1) rp.renderTargetArrayLength = v->slices;
    } else {
        if (depth) {
            rp.depthAttachment.texture = tex; rp.depthAttachment.level = v->mip; rp.depthAttachment.slice = v->first_slice;
            rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = depth_value;
        }
        if (stencil && tex.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
            rp.stencilAttachment.texture = tex; rp.stencilAttachment.level = v->mip; rp.stencilAttachment.slice = v->first_slice;
            rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.clearStencil = stencil_value;
        }
    }
    d3d_end_render(c);
    [[context_cmd(c) renderCommandEncoderWithDescriptor:rp] endEncoding];
}
static WINAPI void ctx_clear_rtv(void *this, d3d_view *v, const float color[4]) {
    static int traced;
    if (getenv("VXRT_TRACE_DRAWS") && v && v->resource->kind == K_TEXTURE && ((d3d_texture *)v->resource)->desc[TD_WIDTH] != 2048 && traced++ < 40)
        vx_log("ClearRenderTargetView %ux%u to (%g, %g, %g, %g)", ((d3d_texture *)v->resource)->desc[TD_WIDTH], ((d3d_texture *)v->resource)->desc[TD_HEIGHT], color[0], color[1], color[2], color[3]);
    if (v && v->resource->kind == K_TEXTURE) clear_pass(this, v, color, 0, 0, 0, 0);
}
static WINAPI void ctx_clear_dsv(void *this, d3d_view *v, uint32_t flags, float depth, uint8_t stencil) {
    if (v && v->resource->kind == K_TEXTURE) clear_pass(this, v, NULL, flags & 1, depth, flags & 2, stencil);
}
static void hold(d3d_view **slot, d3d_view *v) {
    if (v) d3d_addref(&v->base);
    if (*slot) d3d_release(&(*slot)->base);
    *slot = v;
}
static WINAPI void ctx_om_set_render_targets(void *this, uint32_t n, d3d_view *const *rtvs, d3d_view *dsv) {
    d3d_context *c = this;
    for (uint32_t i = 0; i < 8; i++) hold(&c->rtv[i], i < n && rtvs ? rtvs[i] : NULL);
    hold(&c->dsv, dsv);
}
static WINAPI void ctx_om_get_render_targets(void *this, uint32_t n, d3d_view **rtvs, d3d_view **dsv) {
    d3d_context *c = this;
    for (uint32_t i = 0; i < n && rtvs; i++) { rtvs[i] = i < 8 ? c->rtv[i] : NULL; if (rtvs[i]) d3d_addref(&rtvs[i]->base); }
    if (dsv) { *dsv = c->dsv; if (*dsv) d3d_addref(&(*dsv)->base); }
}
static WINAPI void ctx_rs_set_viewports(void *this, uint32_t n, const float *vp) {
    d3d_context *c = this;
    if (n > 16) n = 16;
    if (n && vp) memcpy(c->viewport, vp, sizeof(float) * 6 * n);
    c->nviewports = n;
}
static WINAPI void ctx_rs_get_viewports(void *this, uint32_t *n, float *vp) {
    d3d_context *c = this;
    if (vp && *n) memcpy(vp, c->viewport, sizeof(float) * 6 * MIN(*n, MAX(c->nviewports, 1)));
    *n = MAX(c->nviewports, 1);
}

// Subresource index -> mip level and array slice.
static void subresource(const d3d_texture *t, uint32_t sub, uint32_t *mip, uint32_t *slice) {
    uint32_t mips = t->desc[TD_MIPS] ? t->desc[TD_MIPS] : 1;
    *mip = sub % mips; *slice = sub / mips;
}
static uint32_t mip_size(uint32_t size, uint32_t mip) { size >>= mip; return size ? size : 1; }
// The region of a subresource a box covers (all of it without one): origin and size in texels.
static void region(const d3d_texture *t, uint32_t mip, const uint32_t *box, MTLOrigin *o, MTLSize *sz) {
    uint32_t w = mip_size(t->desc[TD_WIDTH], mip), h = mip_size(t->desc[TD_HEIGHT], mip), d = mip_size(t->depth, mip);
    if (t->dim == 1) h = 1;
    if (!box) { *o = MTLOriginMake(0, 0, 0); *sz = MTLSizeMake(w, h, d); return; }
    *o = MTLOriginMake(box[0], box[1], t->dim == 3 ? box[2] : 0);
    *sz = MTLSizeMake(MIN(box[3], w) - MIN(box[0], w), MIN(box[4], h) - MIN(box[1], h), t->dim == 3 ? MIN(box[5], d) - MIN(box[2], d) : 1);
}

static WINAPI void ctx_copy_resource(void *this, d3d_object *dst, d3d_object *src) {
    d3d_context *c = this;
    HELD_OP(2);
    if (!dst || !src || dst->kind != src->kind) return;
    d3d_end_render(c);
    id<MTLBlitCommandEncoder> blit = [context_cmd(c) blitCommandEncoder];
    if (dst->kind == K_TEXTURE) {
        id<MTLTexture> s = (__bridge id<MTLTexture>)((d3d_texture *)src)->mtl, d = (__bridge id<MTLTexture>)((d3d_texture *)dst)->mtl;
        if (s.width == d.width && s.height == d.height && s.depth == d.depth && s.mipmapLevelCount == d.mipmapLevelCount && s.arrayLength == d.arrayLength && s.sampleCount == d.sampleCount)
            [blit copyFromTexture:s toTexture:d];
        else if (vx_trace) vx_log("CopyResource between textures that differ: %lux%lu x%lu -> %lux%lu x%lu", (unsigned long)s.width, (unsigned long)s.height,
                                  (unsigned long)s.sampleCount, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.sampleCount);
    } else {
        d3d_buffer *sb = (d3d_buffer *)src, *db = (d3d_buffer *)dst;
        [blit copyFromBuffer:(__bridge id<MTLBuffer>)sb->mtl sourceOffset:sb->off toBuffer:(__bridge id<MTLBuffer>)db->mtl destinationOffset:db->off
                        size:MIN(sb->desc[0], db->desc[0])];
    }
    [blit endEncoding];
    d3d_mark_gpu(c, dst); d3d_mark_gpu(c, src);
    if (dst->kind == K_TEXTURE) d3d_shared_written(c->base.dev, (d3d_texture *)dst);
}
// D3D11_BOX: left, top, front, right, bottom, back.
static WINAPI void ctx_copy_subresource_region(void *this, d3d_object *dst, uint32_t dst_sub, uint32_t x, uint32_t y, uint32_t z,
                                               d3d_object *src, uint32_t src_sub, const uint32_t *box) {
    d3d_context *c = this;
    HELD_OP(3);
    if (!dst || !src || dst->kind != src->kind) return;
    d3d_end_render(c);
    id<MTLBlitCommandEncoder> blit = [context_cmd(c) blitCommandEncoder];
    if (dst->kind == K_TEXTURE) {
        d3d_texture *ds = (d3d_texture *)dst, *ss = (d3d_texture *)src;
        uint32_t smip, sslice, dmip, dslice;
        subresource(ss, src_sub, &smip, &sslice);
        subresource(ds, dst_sub, &dmip, &dslice);
        MTLOrigin o; MTLSize sz;
        region(ss, smip, box, &o, &sz);
        // The destination must hold the region (compressed regions at mip edges may be partial blocks).
        sz.width = MIN(sz.width, mip_size(ds->desc[TD_WIDTH], dmip) - MIN(x, mip_size(ds->desc[TD_WIDTH], dmip)));
        sz.height = MIN(sz.height, (ds->dim == 1 ? 1 : mip_size(ds->desc[TD_HEIGHT], dmip)) - MIN(y, mip_size(ds->desc[TD_HEIGHT], dmip)));
        sz.depth = MIN(sz.depth, mip_size(ds->depth, dmip) - MIN(z, mip_size(ds->depth, dmip)));
        if (sz.width && sz.height && sz.depth)
            [blit copyFromTexture:(__bridge id<MTLTexture>)ss->mtl sourceSlice:sslice sourceLevel:smip sourceOrigin:o sourceSize:sz
                        toTexture:(__bridge id<MTLTexture>)ds->mtl destinationSlice:dslice destinationLevel:dmip
                destinationOrigin:MTLOriginMake(x, ds->dim == 1 ? 0 : y, ds->dim == 3 ? z : 0)];
    } else {
        d3d_buffer *sb = (d3d_buffer *)src, *db = (d3d_buffer *)dst;
        uint32_t from = box ? box[0] : 0, size = box ? box[3] - box[0] : sb->desc[0];
        if (from + size <= sb->desc[0] && x + size <= db->desc[0])
            [blit copyFromBuffer:(__bridge id<MTLBuffer>)sb->mtl sourceOffset:sb->off + from toBuffer:(__bridge id<MTLBuffer>)db->mtl destinationOffset:db->off + x size:size];
    }
    [blit endEncoding];
    d3d_mark_gpu(c, dst); d3d_mark_gpu(c, src);
    if (dst->kind == K_TEXTURE) d3d_shared_written(c->base.dev, (d3d_texture *)dst);
}

// Uploads go through a temporary buffer and a blit, so they land in order with the other recorded work. Rows
// are rows of texels, or of 4x4 blocks for the compressed formats.
// c: the context recording the upload; NULL for a texture's initial data, which may come from any thread (device
// methods are free-threaded): a command buffer of its own, committed at once (the queue keeps it before later work).
static void upload_texture(d3d_context *c, d3d_texture *t, uint32_t sub, const uint32_t *box, const void *data, uint32_t row_pitch, uint32_t depth_pitch) {
    uint32_t mip, slice;
    subresource(t, sub, &mip, &slice);
    MTLOrigin o; MTLSize sz;
    region(t, mip, box, &o, &sz);
    uint32_t pitch, rows;
    if (!sz.width || !sz.height || !sz.depth || !format_layout(t->desc[TD_FORMAT], (uint32_t)sz.width, (uint32_t)sz.height, &pitch, &rows)) return;
    id<MTLDevice> dev = (__bridge id<MTLDevice>)t->base.dev->mtl;
    NSUInteger image = (NSUInteger)pitch * rows;
    id<MTLBuffer> staging = [dev newBufferWithLength:image * sz.depth options:MTLResourceStorageModeShared];
    for (NSUInteger z = 0; z < sz.depth; z++)
        for (uint32_t r = 0; r < rows; r++)
            memcpy((uint8_t *)staging.contents + z * image + (size_t)r * pitch, (const uint8_t *)data + z * depth_pitch + (size_t)r * row_pitch, pitch);
    id<MTLCommandBuffer> own = nil;
    if (c) { d3d_end_render(c); d3d_mark_gpu(c, &t->base); }
    else own = [(__bridge id<MTLCommandQueue>)t->base.dev->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [(c ? context_cmd(c) : own) blitCommandEncoder];
    [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:image sourceSize:sz
               toTexture:(__bridge id<MTLTexture>)t->mtl destinationSlice:slice destinationLevel:mip destinationOrigin:o];
    [blit endEncoding];
    if (own) [own commit];
}
// The newest Apple GPU family the device supports (MTLGPUFamilyApple7 = 1007 ... Apple10 = 1010; later ones are
// probed too, so a newer chip reports its own number).
int d3d_apple_family(void *mtl_device) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)mtl_device;
    for (int f = 14; f >= 1; f--) if ([dev supportsFamily:(MTLGPUFamily)(1000 + f)]) return f;
    return 0;
}
// Renamed buffers are recycled: a buffer replaced by UpdateSubresource or Map(DISCARD) goes into a queue for its
// size, tagged with the command buffer that may still read it, and is handed out again once the GPU has finished
// that command buffer (instead of a new Metal buffer for every update of a constant buffer).
typedef struct { void *buf; uint64_t seq; } retired_buf;
typedef struct { d3d_device *dev; size_t len; retired_buf q[256]; int head, count; } buf_bucket;
static buf_bucket g_buckets[64];
static int g_nbuckets;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static buf_bucket *bucket_for(d3d_device *dev, size_t len, int create) {
    for (int i = 0; i < g_nbuckets; i++) if (g_buckets[i].dev == dev && g_buckets[i].len == len) return &g_buckets[i];
    if (!create || g_nbuckets == 64) return NULL;
    buf_bucket *b = &g_buckets[g_nbuckets++];
    b->dev = dev; b->len = len; b->head = b->count = 0;
    return b;
}
static id<MTLBuffer> take_buffer(d3d_device *dev, size_t len) {
    uint64_t done = __atomic_load_n(&dev->completed, __ATOMIC_ACQUIRE);
    pthread_mutex_lock(&g_pool_lock);
    buf_bucket *b = bucket_for(dev, len, 0);
    void *ref = NULL;
    if (b && b->count && b->q[b->head].seq <= done) { ref = b->q[b->head].buf; b->head = (b->head + 1) % 256; b->count--; }
    pthread_mutex_unlock(&g_pool_lock);
    if (ref) return CFBridgingRelease(ref);
    return [(__bridge id<MTLDevice>)dev->mtl newBufferWithLength:len options:MTLResourceStorageModeShared];
}
static void retire_buffer(d3d_device *dev, void *ref) {         // takes over the reference
    size_t len = ((__bridge id<MTLBuffer>)ref).length;
    pthread_mutex_lock(&g_pool_lock);
    buf_bucket *b = bucket_for(dev, len, 1);
    void *drop = NULL;
    if (!b) drop = ref;
    else {
        if (b->count == 256) { drop = b->q[b->head].buf; b->head = (b->head + 1) % 256; b->count--; }
        b->q[(b->head + b->count) % 256] = (retired_buf){ ref, __atomic_load_n(&dev->submitted, __ATOMIC_ACQUIRE) + 1 };
        b->count++;
    }
    pthread_mutex_unlock(&g_pool_lock);
    if (drop) CFRelease(drop);
}
// Small buffers that are renamed (constant buffers, dynamic vertex and index data) get slices of 4 MB chunks
// instead of a Metal buffer each: making a Metal buffer costs microseconds under Rosetta, a slice a few
// instructions. A chunk is filled front to back; a full one is filled again once no buffer holds a slice of it
// and the GPU has finished every command buffer that may read it (seq).
#define SLICE_LIMIT 65536
#define CHUNK_BYTES ((size_t)4 << 20)
typedef struct slice_chunk { void *mtl; uint8_t *base; size_t used; int live; uint64_t seq; struct slice_chunk *next; } slice_chunk;
typedef struct { slice_chunk *cur, *full; } slice_ring;
static slice_chunk *take_slice(d3d_device *dev, size_t len, size_t *off) {
    size_t need = (len + 255) & ~(size_t)255;
    uint64_t done = __atomic_load_n(&dev->completed, __ATOMIC_ACQUIRE);
    pthread_mutex_lock(&g_pool_lock);
    slice_ring *r = dev->slices;
    if (!r) r = dev->slices = calloc(1, sizeof *r);
    slice_chunk *k = r->cur;
    if (!k || k->used + need > CHUNK_BYTES) {
        if (k) { k->next = r->full; r->full = k; }
        k = NULL;
        for (slice_chunk **pp = &r->full; *pp; pp = &(*pp)->next)
            if (!(*pp)->live && (*pp)->seq <= done) { k = *pp; *pp = k->next; break; }
        if (!k) {
            id<MTLBuffer> m = [(__bridge id<MTLDevice>)dev->mtl newBufferWithLength:CHUNK_BYTES options:MTLResourceStorageModeShared];
            k = calloc(1, sizeof *k);
            k->mtl = (void *)CFBridgingRetain(m);
            k->base = m.contents;
        }
        k->used = 0; k->next = NULL;
        r->cur = k;
    }
    *off = k->used;
    k->used += need;
    k->live++;
    pthread_mutex_unlock(&g_pool_lock);
    return k;
}
static void drop_slice(d3d_device *dev, d3d_buffer *b) {    // the buffer stops using its slice
    slice_chunk *k = b->chunk;
    if (!k) return;
    uint64_t last = __atomic_load_n(&dev->submitted, __ATOMIC_ACQUIRE) + 1;   // draws recorded so far may read it
    pthread_mutex_lock(&g_pool_lock);
    k->live--;
    if (k->seq < last) k->seq = last;
    pthread_mutex_unlock(&g_pool_lock);
    b->chunk = NULL; b->off = 0;
}
// New storage for a buffer whose contents are replaced (Map(DISCARD), UpdateSubresource), so draws already
// recorded keep reading the old contents; keep: copy them over (a partial update). Returns the new contents.
// Buffers bound as shader resources get a Metal buffer of their own (views hold the old one).
static uint8_t *rename_buffer(d3d_device *dev, d3d_buffer *b, int keep) {
    size_t len = b->desc[0], off = 0;
    id<MTLBuffer> old = (__bridge id<MTLBuffer>)b->mtl;
    const uint8_t *before = (const uint8_t *)old.contents + b->off;
    int view = (b->desc[2] & 8) != 0;                            // BIND_SHADER_RESOURCE
    slice_chunk *k = NULL;
    id<MTLBuffer> fresh;
    if (!view && len <= SLICE_LIMIT) { k = take_slice(dev, len, &off); fresh = (__bridge id<MTLBuffer>)k->mtl; }
    else if (!view) fresh = take_buffer(dev, old.length);
    else fresh = [(__bridge id<MTLDevice>)dev->mtl newBufferWithLength:old.length options:MTLResourceStorageModeShared];
    uint8_t *p = (uint8_t *)fresh.contents + off;
    if (keep) memcpy(p, before, len);
    if (b->chunk) { drop_slice(dev, b); CFRelease(b->mtl); }    // command buffers hold what they use
    else if (!view && !k) retire_buffer(dev, b->mtl);
    else CFRelease(b->mtl);
    b->mtl = (void *)CFBridgingRetain(fresh);
    b->off = off; b->chunk = k;
    return p;
}
static WINAPI void ctx_update_subresource(void *this, d3d_object *dst, uint32_t sub, const uint32_t *box, const void *data, uint32_t row_pitch, uint32_t depth_pitch) {
    d3d_context *c = this;
    if (!dst || !data) return;
    if (dst->kind == K_TEXTURE) { upload_texture(c, (d3d_texture *)dst, sub, box, data, row_pitch, depth_pitch); return; }
    // Buffers are renamed: the new contents go into a fresh Metal buffer (a copy of the old one), so draws
    // already recorded keep reading what they were given. Buffers the GPU writes (UAVs) are updated by the GPU
    // instead, in order, since their CPU-side contents may be out of date.
    d3d_buffer *b = (d3d_buffer *)dst;
    id<MTLBuffer> old = (__bridge id<MTLBuffer>)b->mtl;
    uint32_t from = box ? box[0] : 0, size = box ? box[3] - box[0] : b->desc[0];
    if (from + size > b->desc[0]) return;
    if (b->shared) { memcpy((uint8_t *)old.contents + b->off + from, data, size); return; }   // shared: in place
    if (b->desc[2] & BIND_UNORDERED_ACCESS) {
        id<MTLBuffer> staging = [(__bridge id<MTLDevice>)c->base.dev->mtl newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
        d3d_end_render(c);
        id<MTLBlitCommandEncoder> blit = [context_cmd(c) blitCommandEncoder];
        [blit copyFromBuffer:staging sourceOffset:0 toBuffer:old destinationOffset:b->off + from size:size];
        [blit endEncoding];
        return;
    }
    uint8_t *p = rename_buffer(c->base.dev, b, from || size < b->desc[0]);   // a partial update keeps the rest
    memcpy(p + from, data, size);
}

// Map waits for the GPU to finish what was recorded so far, then hands out memory the CPU can use. Staging
// textures are copied into a CPU buffer (and back on Unmap when written).
typedef struct { void *data; uint32_t row_pitch, depth_pitch; } MAPPED;
#define MAP_READ 1
// VXRT_STATS=1: every 300 Maps that wait for the GPU, the time they waited, by map type and usage.
static void map_stats(uint32_t type, uint32_t usage, uint64_t ns) {
    static int on = -1;
    static uint64_t n, total, by[6][4];
    if (on < 0) on = getenv("VXRT_STATS") != NULL;
    if (!on) return;
    total += ns; by[type % 6][usage % 4]++;
    if (++n % 300) return;
    vx_log("Map: %llu waits for the GPU, %llu ms; read %llu/%llu/%llu/%llu write %llu/%llu/%llu/%llu read-write %llu/%llu/%llu/%llu (default/immutable/dynamic/staging)",
           (unsigned long long)n, (unsigned long long)(total / 1000000), (unsigned long long)by[1][0], (unsigned long long)by[1][1], (unsigned long long)by[1][2], (unsigned long long)by[1][3],
           (unsigned long long)by[2][0], (unsigned long long)by[2][1], (unsigned long long)by[2][2], (unsigned long long)by[2][3],
           (unsigned long long)by[3][0], (unsigned long long)by[3][1], (unsigned long long)by[3][2], (unsigned long long)by[3][3]);
}
static WINAPI HRESULT ctx_map(void *this, d3d_object *res, uint32_t sub, uint32_t type, uint32_t flags, MAPPED *out) {
    d3d_context *c = this;
    HELD_OP(5);
    if (!res || !out) return E_INVALIDARG;
    if (res->kind == K_BUFFER) {
        d3d_buffer *buf = (d3d_buffer *)res;
        if (type == 4 && buf->shared) {                          // shared: written in place (other processes hold it)
        } else if (type == 4) {                                 // WRITE_DISCARD: fresh storage, nothing to wait for
            rename_buffer(c->base.dev, buf, 0);
        } else if (type != 5) {                                  // READ/WRITE: the GPU must be done with it
            uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            HRESULT hr = buf->desc[1] == 3 ? wait_gpu(c, res, flags) : (d3d_flush(c->base.dev, 1), S_OK);   // staging: copies only
            map_stats(type, buf->desc[1], clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
            if (hr) return hr;
        }
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)buf->mtl;
        out->data = (uint8_t *)b.contents + buf->off; out->row_pitch = out->depth_pitch = buf->desc[0];
        return S_OK;
    }
    d3d_texture *t = (d3d_texture *)res;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    HRESULT hr = t->desc[TD_USAGE] == 3 ? wait_gpu(c, res, flags) : (d3d_flush(c->base.dev, 1), S_OK);   // staging: copies only
    map_stats(type, t->desc[TD_USAGE] & 3, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0);
    if (hr) return hr;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)t->mtl;
    if (tex.buffer) {                                            // a staging image in a buffer: its own memory
        out->data = (uint8_t *)tex.buffer.contents + tex.bufferOffset;
        out->row_pitch = (uint32_t)tex.bufferBytesPerRow; out->depth_pitch = (uint32_t)(tex.bufferBytesPerRow * tex.height);
        return S_OK;
    }
    if (tex.storageMode != MTLStorageModeShared) { vx_log("Direct3D: Map of a texture without CPU access"); return E_INVALIDARG; }
    uint32_t mip, slice;
    subresource(t, sub, &mip, &slice);
    MTLOrigin o; MTLSize sz;
    region(t, mip, NULL, &o, &sz);
    uint32_t pitch, rows;
    format_layout(t->desc[TD_FORMAT], (uint32_t)sz.width, (uint32_t)sz.height, &pitch, &rows);
    size_t image = (size_t)pitch * rows, size = image * sz.depth;
    if (t->cpu_size < size) { free(t->cpu); t->cpu = malloc(size ? size : 1); t->cpu_size = size; }
    t->cpu_pitch = pitch;
    if (type & MAP_READ || type == 3)
        [tex getBytes:t->cpu bytesPerRow:pitch bytesPerImage:image fromRegion:MTLRegionMake3D(0, 0, 0, sz.width, sz.height, sz.depth) mipmapLevel:mip slice:slice];
    out->data = t->cpu; out->row_pitch = pitch; out->depth_pitch = (uint32_t)image;
    return S_OK;
}
static WINAPI void ctx_unmap(void *this, d3d_object *res, uint32_t sub) {
    (void)this;
    if (!res || res->kind != K_TEXTURE) return;
    d3d_texture *t = (d3d_texture *)res;
    if (!(t->desc[TD_CPU] & 0x10000) || !t->cpu || ((__bridge id<MTLTexture>)t->mtl).buffer) return;   // D3D11_CPU_ACCESS_WRITE
    uint32_t mip, slice;
    subresource(t, sub, &mip, &slice);
    MTLOrigin o; MTLSize sz;
    region(t, mip, NULL, &o, &sz);
    uint32_t pitch, rows;
    format_layout(t->desc[TD_FORMAT], (uint32_t)sz.width, (uint32_t)sz.height, &pitch, &rows);
    [(__bridge id<MTLTexture>)t->mtl replaceRegion:MTLRegionMake3D(0, 0, 0, sz.width, sz.height, sz.depth) mipmapLevel:mip slice:slice withBytes:t->cpu
                                       bytesPerRow:pitch bytesPerImage:(NSUInteger)pitch * rows];
}
static WINAPI void ctx_flush(void *this) { HELD_OP(6); d3d_flush(((d3d_object *)this)->dev, 0); }
static WINAPI void ctx_clear_state(void *this) { ctx_om_set_render_targets(this, 0, NULL, NULL); }
static WINAPI uint32_t ctx_get_type(void *this) { (void)this; return 0; }       // D3D11_DEVICE_CONTEXT_IMMEDIATE
static WINAPI uint32_t ctx_get_flags(void *this) { (void)this; return 0; }
static void context_destroy(d3d_object *o) {
    d3d_context *c = (d3d_context *)o;
    d3d_end_render(c);
    d3d_draw_release(c);
    ctx_om_set_render_targets(c, 0, NULL, NULL);
    if (c->cmd) CFBridgingRelease(c->cmd);
}

// ---- the device ------------------------------------------------------------------------------------------------------------------
static d3d_device *dev_of(void *this) { return (d3d_device *)this; }
static WINAPI HRESULT device_qi(void *this, const GUID *iid, void **out) {
    d3d_device *d = dev_of(this);
    if (!out) return E_POINTER;
    if (d3d_same_guid(iid, &IID_IUnknown_) || d3d_same_guid(iid, &IID_ID3D11Device) || d3d_same_guid(iid, &IID_ID3D11Device1)) { d3d_addref(&d->base); *out = &d->base.vtbl; return S_OK; }
    if (d3d_same_guid(iid, &IID_IDXGIObject) || d3d_same_guid(iid, &IID_IDXGIDevice) || d3d_same_guid(iid, &IID_IDXGIDevice1)) {
        d3d_addref(&d->base); *out = &d->base.vtbl2; return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static WINAPI uint32_t device_addref(void *this) { return d3d_addref(this); }
static WINAPI uint32_t device_release(void *this) { return d3d_release(this); }

// D3D11_SUBRESOURCE_DATA: pointer, row pitch, slice pitch.
typedef struct { const void *data; uint32_t pitch, slice_pitch; } SUBRESOURCE_DATA;
static HRESULT create_texture(void *this, const uint32_t *desc, uint32_t dim, uint32_t depth, const SUBRESOURCE_DATA *init, void **out) {
    d3d_device *d = dev_of(this);
    HRESULT hr;
    d3d_texture *t = texture_create(d, desc, dim, depth, &hr);
    static int trace = -1;                             // VXRT_TRACE_TEXTURES: every texture made, and failures
    if (trace < 0) trace = getenv("VXRT_TRACE_TEXTURES") != NULL;
    if (trace || (!t && vx_trace))
        vx_log("Direct3D: texture %ux%u (%ud) x%u mips %u format %u samples %u usage %u bind %#x cpu %#x misc %#x -> %#x", desc[TD_WIDTH], desc[TD_HEIGHT],
               dim, desc[TD_ARRAY], desc[TD_MIPS], desc[TD_FORMAT], desc[TD_SAMPLES], desc[TD_USAGE], desc[TD_BIND], desc[TD_CPU], desc[TD_MISC], (unsigned)hr);
    if (!t) { if (out) *out = NULL; return hr; }
    if (init) {
        uint32_t subs = t->desc[TD_MIPS] * t->desc[TD_ARRAY];
        for (uint32_t s = 0; s < subs; s++)
            if (init[s].data) upload_texture(NULL, t, s, NULL, init[s].data, init[s].pitch, init[s].slice_pitch);
    }
    if (!out) { d3d_release(&t->base); return S_FALSE; }   // a validation-only call
    *out = t;
    return S_OK;
}
static WINAPI HRESULT device_create_texture2d(void *this, const uint32_t *desc, const SUBRESOURCE_DATA *init, void **out) {
    return create_texture(this, desc, 2, 1, init, out);
}
// D3D11_TEXTURE1D_DESC and D3D11_TEXTURE3D_DESC, kept in the 2D layout.
static WINAPI HRESULT device_create_texture1d(void *this, const uint32_t *d, const SUBRESOURCE_DATA *init, void **out) {
    uint32_t desc[TD_COUNT] = { d[0], 1, d[1], d[2], d[3], 1, 0, d[4], d[5], d[6], d[7] };
    return create_texture(this, desc, 1, 1, init, out);
}
static WINAPI HRESULT device_create_texture3d(void *this, const uint32_t *d, const SUBRESOURCE_DATA *init, void **out) {
    uint32_t desc[TD_COUNT] = { d[0], d[1], d[3], 1, d[4], 1, 0, d[5], d[6], d[7], d[8] };
    return create_texture(this, desc, 3, d[2] ? d[2] : 1, init, out);
}
static WINAPI HRESULT device_create_buffer(void *this, const uint32_t *desc, const SUBRESOURCE_DATA *init, void **out) {
    d3d_device *d = dev_of(this);
    if (!desc || !desc[0]) return E_INVALIDARG;
    if (!out) return S_FALSE;
    if (desc[4] & MISC_SHARED_ANY) {                             // shared with other processes: shared memory
        void *shm = NULL; size_t size = 0; int slot;
        void *mtl = d3d_shared_buffer(d, desc, &shm, &size, &slot);
        if (mtl) {
            if (init && init->data) memcpy(shm, init->data, desc[0]);
            *out = d3d_buffer_wrap(d, desc, mtl, slot + 1, shm, size);
            return S_OK;
        }
    }
    id<MTLBuffer> b = [(__bridge id<MTLDevice>)d->mtl newBufferWithLength:desc[0] options:MTLResourceStorageModeShared];
    if (!b) return E_OUTOFMEMORY;
    if (init && init->data) memcpy(b.contents, init->data, desc[0]);
    *out = d3d_buffer_wrap(d, desc, (void *)CFBridgingRetain(b), 0, NULL, 0);
    return S_OK;
}
static WINAPI HRESULT device_create_rtv(void *this, d3d_object *res, const uint32_t *desc, void **out) {
    if (!res || res->kind != K_TEXTURE) return E_INVALIDARG;
    if (!out) return S_FALSE;
    *out = make_view(dev_of(this), K_RTV, g_rtv_vtbl, res, desc, 0);
    return S_OK;
}
static WINAPI HRESULT device_create_dsv(void *this, d3d_object *res, const uint32_t *desc, void **out) {
    if (!res || res->kind != K_TEXTURE) return E_INVALIDARG;
    if (!out) return S_FALSE;
    *out = make_view(dev_of(this), K_DSV, g_dsv_vtbl, res, desc, 1);
    return S_OK;
}
static WINAPI HRESULT device_create_srv(void *this, d3d_object *res, const uint32_t *desc, void **out) {
    if (!res) return E_INVALIDARG;
    if (!out) return S_FALSE;
    *out = make_view(dev_of(this), K_SRV, g_srv_vtbl, res, desc, 0);
    return S_OK;
}
// D3D11_FORMAT_SUPPORT bits for the formats Metal has: textures of every dimension, buffers, mips (also made by
// GenerateMips), loads, sampling and gathers; render targets, blending and multisampling (rendering, loads and
// resolves) for the uncompressed colour formats; depth formats for depth/stencil and comparison sampling.
static uint32_t format_support(uint32_t format) {
    MTLPixelFormat f = d3d_mtl_format(format, NULL);
    if (f == MTLPixelFormatInvalid) return 0;
    uint32_t s = 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x1000 | 0x100000 | 0x800000;   // 1D 2D 3D cube, load, sample, mip, cast, gather
    int compressed = (format >= 70 && format <= 84) || (format >= 94 && format <= 99);
    if (is_depth(f)) return s | 0x400 | 0x10000 | 0x200000 | 0x400000 | 0x4000000;          // comparison, depth, MSAA target and load
    if (compressed) return s | 0x20000;
    return s | 0x1 | 0x2000 | 0x4000 | 0x8000 | 0x20000 | 0x40000 | 0x200000 | 0x400000 | 0x2000000 | (format == 28 || format == 87 || format == 29 || format == 91 || format == 24 || format == 10 ? 0x80000 : 0);
}
static WINAPI HRESULT device_check_format_support(void *this, uint32_t format, uint32_t *support) {
    (void)this;
    *support = format_support(format);
    return *support ? S_OK : E_FAIL;
}
static WINAPI HRESULT device_check_msaa(void *this, uint32_t format, uint32_t count, uint32_t *levels) {
    (void)format;
    *levels = count == 1 || [(__bridge id<MTLDevice>)dev_of(this)->mtl supportsTextureSampleCount:count] ? 1 : 0;
    return S_OK;
}
static WINAPI HRESULT device_check_feature_support(void *this, uint32_t feature, void *data, uint32_t size) {
    (void)this;
    if ((feature == 2 || feature == 3) && size >= 8) {                                          // FORMAT_SUPPORT(2): the format, then the bits
        uint32_t format = ((uint32_t *)data)[0];
        ((uint32_t *)data)[1] = feature == 2 ? format_support(format) : format_support(format) ? 0x1 | 0x2 | 0x4 | 0x8 | 0x40 | 0x80 : 0;   // atomics, typed UAV load/store
        return S_OK;
    }
    memset(data, 0, size);
    if (feature == 0 && size >= 8) { ((uint32_t *)data)[0] = 1; ((uint32_t *)data)[1] = 0; }   // THREADING: concurrent creates, no command lists
    else if (feature == 4 && size >= 4) ((uint32_t *)data)[0] = 1;                              // D3D10_X_HARDWARE_OPTIONS
    return S_OK;
}
static WINAPI uint32_t device_get_feature_level(void *this) { return dev_of(this)->feature_level; }
static WINAPI uint32_t device_get_creation_flags(void *this) { return dev_of(this)->flags; }
static WINAPI HRESULT device_open_shared(void *this, HANDLE h, const GUID *iid, void **out) { return d3d_open_shared(dev_of(this), h, iid, out); }
static WINAPI HRESULT device_open_shared1(void *this, HANDLE h, const GUID *iid, void **out) { return d3d_open_shared(dev_of(this), h, iid, out); }
static WINAPI HRESULT device_get_removed_reason(void *this) { (void)this; return S_OK; }
static WINAPI void device_get_immediate_context(void *this, void **out) { d3d_device *d = dev_of(this); d3d_addref(d->context); *out = d->context; }
static WINAPI HRESULT device_set_exception_mode(void *this, uint32_t m) { (void)this; (void)m; return S_OK; }
static WINAPI uint32_t device_get_exception_mode(void *this) { (void)this; return 0; }
static WINAPI HRESULT device_get_private(void *this, const GUID *g, uint32_t *size, void *data) { return d3d_get_private(this, g, size, data); }
static WINAPI HRESULT device_set_private(void *this, const GUID *g, uint32_t size, const void *data) { return d3d_set_private(this, g, size, data); }
static WINAPI HRESULT device_set_private_interface(void *this, const GUID *g, void *unk) { (void)unk; return d3d_set_private(this, g, 0, NULL); }

// The device seen as an IDXGIDevice1.
static WINAPI HRESULT dxgidev_qi(void *this, const GUID *iid, void **out) { return device_qi(FROM_VTBL2(this), iid, out); }
static WINAPI uint32_t dxgidev_addref(void *this) { return d3d_addref(FROM_VTBL2(this)); }
static WINAPI uint32_t dxgidev_release(void *this) { return d3d_release(FROM_VTBL2(this)); }
static WINAPI HRESULT dxgidev_get_adapter(void *this, void **out) {
    d3d_device *d = (d3d_device *)FROM_VTBL2(this);
    if (!d->adapter) d->adapter = dxgi_adapter_create();
    d3d_addref(d->adapter);
    *out = d->adapter;
    return S_OK;
}
static WINAPI HRESULT dxgidev_get_parent(void *this, const GUID *iid, void **out) {
    void *adapter;
    dxgidev_get_adapter(this, &adapter);
    HRESULT hr = (*(HRESULT (WINAPI **)(void *, const GUID *, void **))*(void ***)adapter)(adapter, iid, out);
    d3d_release(adapter);
    return hr;
}
static WINAPI HRESULT dxgidev_set_latency(void *this, uint32_t n) { ((d3d_device *)FROM_VTBL2(this))->max_latency = n; return S_OK; }
static WINAPI HRESULT dxgidev_get_latency(void *this, uint32_t *n) { *n = ((d3d_device *)FROM_VTBL2(this))->max_latency; return S_OK; }
static WINAPI HRESULT dxgidev_set_priority(void *this, int p) { (void)this; (void)p; return S_OK; }
static WINAPI HRESULT dxgidev_get_priority(void *this, int *p) { (void)this; *p = 0; return S_OK; }
static WINAPI HRESULT dxgidev_residency(void *this, void *const *res, uint32_t *status, uint32_t n) { (void)this; (void)res; for (uint32_t i = 0; i < n; i++) status[i] = 1; return S_OK; }
static WINAPI HRESULT dxgidev_get_private(void *this, const GUID *g, uint32_t *size, void *data) { return d3d_get_private(FROM_VTBL2(this), g, size, data); }
static WINAPI HRESULT dxgidev_set_private(void *this, const GUID *g, uint32_t size, const void *data) { return d3d_set_private(FROM_VTBL2(this), g, size, data); }

d3d_device *d3d_device_from(void *unknown) {
    if (!unknown) return NULL;
    void *dev = NULL;
    HRESULT hr = (*(HRESULT (WINAPI **)(void *, const GUID *, void **))*(void ***)unknown)(unknown, &IID_ID3D11Device, &dev);
    if (hr != S_OK || !dev) return NULL;
    d3d_release(dev);                                  // the caller still holds its own reference
    return dev;
}

static void device_destroy(d3d_object *o) {
    d3d_device *d = (d3d_device *)o;
    d3d_flush(d, 1);
    d->context->dev = NULL;
    d3d_release(d->context);
    if (d->adapter) d3d_release(d->adapter);
    CFBridgingRelease(d->queue);
    CFBridgingRelease(d->mtl);
}

// ---- method tables --------------------------------------------------------------------------------------------------------------
static void **g_device_vtbl, **g_dxgidev_vtbl, **g_context_vtbl;
static pthread_once_t g_tables_once = PTHREAD_ONCE_INIT;
static void make_tables(void) {
    static const d3d_method texture[] = { { "QueryInterface", (void *)texture_qi }, CHILD_METHODS, { "GetType", (void *)texture_get_type },
        { "SetEvictionPriority", (void *)texture_set_eviction }, { "GetEvictionPriority", (void *)texture_get_eviction },
        { "GetDesc", (void *)texture_get_desc }, { NULL, NULL } };
    static const d3d_method texture1d[] = { { "QueryInterface", (void *)texture1d_qi }, CHILD_METHODS, { "GetType", (void *)texture_get_type },
        { "SetEvictionPriority", (void *)texture_set_eviction }, { "GetEvictionPriority", (void *)texture_get_eviction },
        { "GetDesc", (void *)texture1d_get_desc }, { NULL, NULL } };
    static const d3d_method texture3d[] = { { "QueryInterface", (void *)texture3d_qi }, CHILD_METHODS, { "GetType", (void *)texture_get_type },
        { "SetEvictionPriority", (void *)texture_set_eviction }, { "GetEvictionPriority", (void *)texture_get_eviction },
        { "GetDesc", (void *)texture3d_get_desc }, { NULL, NULL } };
    static const d3d_method buffer[] = { { "QueryInterface", (void *)buffer_qi }, CHILD_METHODS, { "GetType", (void *)buffer_get_type },
        { "SetEvictionPriority", (void *)texture_set_eviction }, { "GetEvictionPriority", (void *)texture_get_eviction },
        { "GetDesc", (void *)buffer_get_desc }, { NULL, NULL } };
    static const d3d_method rtv[] = { { "QueryInterface", (void *)rtv_qi }, CHILD_METHODS, { "GetResource", (void *)view_get_resource }, { "GetDesc", (void *)rtv_get_desc }, { NULL, NULL } };
    static const d3d_method dsv[] = { { "QueryInterface", (void *)dsv_qi }, CHILD_METHODS, { "GetResource", (void *)view_get_resource }, { "GetDesc", (void *)dsv_get_desc }, { NULL, NULL } };
    static const d3d_method srv[] = { { "QueryInterface", (void *)srv_qi }, CHILD_METHODS, { "GetResource", (void *)view_get_resource }, { "GetDesc", (void *)srv_get_desc }, { NULL, NULL } };
    static const d3d_method context[] = { { "QueryInterface", (void *)context_qi }, CHILD_METHODS,
        { "ClearRenderTargetView", (void *)ctx_clear_rtv }, { "ClearDepthStencilView", (void *)ctx_clear_dsv },
        { "OMSetRenderTargets", (void *)ctx_om_set_render_targets }, { "OMGetRenderTargets", (void *)ctx_om_get_render_targets },
        { "RSSetViewports", (void *)ctx_rs_set_viewports }, { "RSGetViewports", (void *)ctx_rs_get_viewports },
        { "CopyResource", (void *)ctx_copy_resource }, { "CopySubresourceRegion", (void *)ctx_copy_subresource_region },
        { "UpdateSubresource", (void *)ctx_update_subresource }, { "Map", (void *)ctx_map }, { "Unmap", (void *)ctx_unmap },
        { "Flush", (void *)ctx_flush }, { "ClearState", (void *)ctx_clear_state }, { "GetType", (void *)ctx_get_type },
        { "GetContextFlags", (void *)ctx_get_flags },
        { NULL, NULL } };
    static const d3d_method device[] = { { "QueryInterface", (void *)device_qi }, { "AddRef", (void *)device_addref },
        { "Release", (void *)device_release }, { "CreateTexture2D", (void *)device_create_texture2d },
        { "CreateTexture1D", (void *)device_create_texture1d }, { "CreateTexture3D", (void *)device_create_texture3d },
        { "CreateBuffer", (void *)device_create_buffer }, { "CreateRenderTargetView", (void *)device_create_rtv },
        { "CreateDepthStencilView", (void *)device_create_dsv }, { "CreateShaderResourceView", (void *)device_create_srv },
        { "CheckFormatSupport", (void *)device_check_format_support }, { "CheckMultisampleQualityLevels", (void *)device_check_msaa },
        { "CheckFeatureSupport", (void *)device_check_feature_support }, { "GetFeatureLevel", (void *)device_get_feature_level },
        { "GetCreationFlags", (void *)device_get_creation_flags }, { "GetDeviceRemovedReason", (void *)device_get_removed_reason },
        { "GetImmediateContext", (void *)device_get_immediate_context }, { "SetExceptionMode", (void *)device_set_exception_mode },
        { "GetExceptionMode", (void *)device_get_exception_mode }, { "GetPrivateData", (void *)device_get_private },
        { "SetPrivateData", (void *)device_set_private }, { "SetPrivateDataInterface", (void *)device_set_private_interface },
        { "OpenSharedResource", (void *)device_open_shared }, { "OpenSharedResource1", (void *)device_open_shared1 },
        { "GetImmediateContext1", (void *)device_get_immediate_context },
        { NULL, NULL } };
    static const d3d_method dxgidev[] = { { "QueryInterface", (void *)dxgidev_qi }, { "AddRef", (void *)dxgidev_addref },
        { "Release", (void *)dxgidev_release }, { "GetParent", (void *)dxgidev_get_parent }, { "GetAdapter", (void *)dxgidev_get_adapter },
        { "SetMaximumFrameLatency", (void *)dxgidev_set_latency }, { "GetMaximumFrameLatency", (void *)dxgidev_get_latency },
        { "SetGPUThreadPriority", (void *)dxgidev_set_priority }, { "GetGPUThreadPriority", (void *)dxgidev_get_priority },
        { "QueryResourceResidency", (void *)dxgidev_residency }, { "GetPrivateData", (void *)dxgidev_get_private },
        { "SetPrivateData", (void *)dxgidev_set_private }, { NULL, NULL } };
    g_texture_vtbl = D3D_VTABLE(ID3D11Texture2D, texture);
    g_texture1d_vtbl = D3D_VTABLE(ID3D11Texture1D, texture1d);
    g_texture3d_vtbl = D3D_VTABLE(ID3D11Texture3D, texture3d);
    g_buffer_vtbl = D3D_VTABLE(ID3D11Buffer, buffer);
    g_rtv_vtbl = D3D_VTABLE(ID3D11RenderTargetView, rtv);
    g_dsv_vtbl = D3D_VTABLE(ID3D11DepthStencilView, dsv);
    g_srv_vtbl = D3D_VTABLE(ID3D11ShaderResourceView, srv);
    g_context_vtbl = D3D_VTABLE(ID3D11DeviceContext1, context);   // the 11.1 interfaces: their new methods report themselves
    g_device_vtbl = D3D_VTABLE(ID3D11Device1, device);
    g_dxgidev_vtbl = D3D_VTABLE(IDXGIDevice1, dxgidev);
    d3d_draw_init();
    D3D_VTABLE_OVERRIDE(g_device_vtbl, ID3D11Device, d3d_draw_device_methods);
    D3D_VTABLE_OVERRIDE(g_context_vtbl, ID3D11DeviceContext, d3d_draw_context_methods);
}

// ---- entry points ------------------------------------------------------------------------------------------------------------------
#define FEATURE_LEVEL_11_0 0xb000
static WINAPI HRESULT D3D11CreateDevice(void *adapter, uint32_t driver, HANDLE software, uint32_t flags, const uint32_t *levels,
                                        uint32_t nlevels, uint32_t sdk, void **out_device, uint32_t *out_level, void **out_context) {
    (void)adapter; (void)driver; (void)software; (void)sdk;
    if (vx_trace) vx_log("D3D11CreateDevice(driver %u, flags %#x, %u levels)", driver, flags, nlevels);
    pthread_once(&g_tables_once, make_tables);
    // Metal offers what feature level 11_0 needs; a program asking only for higher levels is refused.
    uint32_t level = 0;
    if (!levels || !nlevels) level = FEATURE_LEVEL_11_0;
    for (uint32_t i = 0; i < nlevels && !level; i++) if (levels[i] <= FEATURE_LEVEL_11_0) level = levels[i];
    if (!level) return E_INVALIDARG;
    if (out_level) *out_level = level;
    if (!out_device && !out_context) return S_FALSE;
    id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
    if (!mtl) { vx_log("Direct3D: no Metal device"); return DXGI_ERROR_UNSUPPORTED; }
    d3d_device *d = d3d_alloc(sizeof *d, K_DEVICE, g_device_vtbl, NULL);
    d->base.vtbl2 = g_dxgidev_vtbl;
    d->base.dev = d;
    d->base.destroy = device_destroy;
    d->mtl = (void *)CFBridgingRetain(mtl);
    d->queue = (void *)CFBridgingRetain([mtl newCommandQueue]);
    d->flags = flags; d->feature_level = level; d->max_latency = 3;
    d->apple_family = d3d_apple_family((__bridge void *)mtl);
    static int told;
    if (vx_trace && !told++) vx_log("Direct3D: %s, Apple GPU family %d%s", mtl.name.UTF8String, d->apple_family,
                        d->apple_family >= 10 ? " (M5 generation or later)" : d->apple_family == 9 ? " (M3/M4 generation)" :
                        d->apple_family == 8 ? " (M2 generation)" : d->apple_family == 7 ? " (M1 generation)" : "");
    d3d_context *c = d3d_alloc(sizeof *c, K_CONTEXT, g_context_vtbl, d);
    c->base.destroy = context_destroy;
    d->context = &c->base;
    if (vx_trace) vx_log("Direct3D 11 device on %s, feature level %#x", mtl.name.UTF8String, level);
    if (out_context) { d3d_addref(d->context); *out_context = d->context; }
    if (out_device) *out_device = d; else d3d_release(&d->base);
    return S_OK;
}
typedef HRESULT (WINAPI *create_swapchain_fn)(void *factory, void *device, const void *desc, void **out);
HRESULT dxgi_create_factory(const GUID *iid, void **out);         // dxgi.m
static WINAPI HRESULT D3D11CreateDeviceAndSwapChain(void *adapter, uint32_t driver, HANDLE software, uint32_t flags, const uint32_t *levels,
                                                    uint32_t nlevels, uint32_t sdk, const void *desc, void **out_swapchain,
                                                    void **out_device, uint32_t *out_level, void **out_context) {
    void *dev = NULL, *ctx = NULL;
    HRESULT hr = D3D11CreateDevice(adapter, driver, software, flags, levels, nlevels, sdk, &dev, out_level, &ctx);
    if (hr != S_OK) return hr;
    if (out_swapchain) {
        void *factory = NULL;
        dxgi_create_factory(&IID_IDXGIFactory, &factory);
        // IDXGIFactory::CreateSwapChain is method 10 (after IUnknown, IDXGIObject and the factory's first three).
        create_swapchain_fn create = (create_swapchain_fn)(*(void ***)factory)[10];
        hr = create(factory, dev, desc, out_swapchain);
        d3d_release(factory);
        if (hr != S_OK) { d3d_release(ctx); d3d_release(dev); return hr; }
    }
    if (out_device) *out_device = dev; else d3d_release(dev);
    if (out_context) *out_context = ctx; else d3d_release(ctx);
    return S_OK;
}

static const vx_export d3d11[] = { VX_EXPORT(D3D11CreateDevice), VX_EXPORT(D3D11CreateDeviceAndSwapChain) };
void vx_d3d11_init(void) {
    pthread_once(&g_tables_once, make_tables);
    vx_register_module("d3d11.dll", d3d11, sizeof d3d11 / sizeof d3d11[0]);
}
