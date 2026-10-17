// Milestone 7a: compute shaders on Metal: structured buffers, thread-group shared memory and barriers, atomics
// on a byte-address buffer, a writable texture, UAV clears. Shaders compiled by Microsoft's HLSL compiler
// (tests/shaders); results read back from the GPU.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "shaders/sum_cs.h"
#include "shaders/atomic_cs.h"
#include "shaders/append_cs.h"
#include "shaders/views_cs.h"
#include "shaders/formats_cs.h"
#include "shaders/bounds_cs.h"
#include "shaders/cbounds_cs.h"
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;

static ID3D11Buffer *make_buffer(UINT size, UINT bind, UINT misc, UINT stride, const void *data, UINT usage, UINT cpu) {
    D3D11_BUFFER_DESC d = { size, usage, bind, cpu, misc, stride };
    D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
    ID3D11Buffer *b = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(dev, &d, data ? &init : NULL, &b)));
    return b;
}
static void read_buffer(ID3D11Buffer *src, void *out, UINT size) {
    ID3D11Buffer *staging = make_buffer(size, 0, 0, 0, NULL, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)src);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, size);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    ID3D11Buffer_Release(staging);
}

static void test_sum(void) {
    ID3D11ComputeShader *cs;
    CHECK(SUCCEEDED(ID3D11Device_CreateComputeShader(dev, sum_cs, sizeof sum_cs, NULL, &cs)));
    float input[256];
    for (int i = 0; i < 256; i++) input[i] = (float)(i + 1);
    ID3D11Buffer *in = make_buffer(sizeof input, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, input, D3D11_USAGE_DEFAULT, 0);
    ID3D11Buffer *out = make_buffer(16, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, NULL, D3D11_USAGE_DEFAULT, 0);
    struct { float scale; UINT count; UINT pad[2]; } params = { 2.0f, 256 };
    ID3D11Buffer *cb = make_buffer(sizeof params, D3D11_BIND_CONSTANT_BUFFER, 0, 0, &params, D3D11_USAGE_DEFAULT, 0);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = { DXGI_FORMAT_UNKNOWN, D3D11_SRV_DIMENSION_BUFFER };
    sd.Buffer.FirstElement = 0; sd.Buffer.NumElements = 256;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = { DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER };
    ud.Buffer.FirstElement = 0; ud.Buffer.NumElements = 4;
    ID3D11ShaderResourceView *srv; ID3D11UnorderedAccessView *uav;
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)in, &sd, &srv)));
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)out, &ud, &uav)));
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, NULL);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_Dispatch(ctx, 4, 1, 1);
    float sums[4] = { 0 };
    read_buffer(out, sums, sizeof sums);
    for (int g = 0; g < 4; g++) {
        float expect = 2.0f * (64.0f * 64 * g + 64 * 65 / 2.0f);
        CHECK(fabsf(sums[g] - expect) < 0.01f);
    }
    printf("  group sums: %g %g %g %g\n", sums[0], sums[1], sums[2], sums[3]);
    ID3D11UnorderedAccessView_Release(uav); ID3D11ShaderResourceView_Release(srv);
    ID3D11Buffer_Release(cb); ID3D11Buffer_Release(out); ID3D11Buffer_Release(in); ID3D11ComputeShader_Release(cs);
}

static void test_atomics_and_texture(void) {
    ID3D11ComputeShader *cs;
    CHECK(SUCCEEDED(ID3D11Device_CreateComputeShader(dev, atomic_cs, sizeof atomic_cs, NULL, &cs)));
    UINT zero[4] = { 7, 7, 7, 7 };
    ID3D11Buffer *counters = make_buffer(16, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, zero, D3D11_USAGE_DEFAULT, 0);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = { DXGI_FORMAT_R32_TYPELESS, D3D11_UAV_DIMENSION_BUFFER };
    ud.Buffer.FirstElement = 0; ud.Buffer.NumElements = 4; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    ID3D11UnorderedAccessView *uavs[2];
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)counters, &ud, &uavs[0])));
    UINT clear[4] = { 0, 0, 0, 0 };
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(ctx, uavs[0], clear);   // the initial 7s go away
    D3D11_TEXTURE2D_DESC td = { 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
    ID3D11Texture2D *image;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &image)));
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)image, NULL, &uavs[1])));
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 2, uavs, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 2, 2, 1);
    UINT result[4];
    read_buffer(counters, result, sizeof result);
    CHECK(result[0] == 256 && result[1] == 1515 && result[2] == 0xffff && result[3] == 0);
    printf("  atomics: count %u, max %u, or %#x, untouched %u\n", result[0], result[1], result[2], result[3]);
    // The image: x and y as red and green.
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)image);
    D3D11_MAPPED_SUBRESOURCE m;
    UINT p0 = 0, p1 = 0;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        p0 = *(UINT *)((BYTE *)m.pData + 0 * m.RowPitch + 15 * 4);   // (15, 0): red full
        p1 = *(UINT *)((BYTE *)m.pData + 5 * m.RowPitch + 3 * 4);    // (3, 5)
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    CHECK(p0 == 0xff0000ffu);
    CHECK((p1 & 0xff) == 51 && ((p1 >> 8) & 0xff) == 85 && (p1 >> 24) == 255);   // 3/15 and 5/15 of 255
    printf("  image: (15,0) %08x, (3,5) %08x\n", p0, p1);
    ID3D11Texture2D_Release(staging); ID3D11UnorderedAccessView_Release(uavs[0]); ID3D11UnorderedAccessView_Release(uavs[1]);
    ID3D11Texture2D_Release(image); ID3D11Buffer_Release(counters); ID3D11ComputeShader_Release(cs);
}

// Append and consume buffers (hidden counters with initial counts, CopyStructureCount) and atomics on an
// R32_UINT texture.
static void test_append_consume(void) {
    ID3D11ComputeShader *cs;
    CHECK(SUCCEEDED(ID3D11Device_CreateComputeShader(dev, append_cs, sizeof append_cs, NULL, &cs)));
    UINT values[128];
    for (int i = 0; i < 128; i++) values[i] = (UINT)i;
    ID3D11Buffer *in = make_buffer(sizeof values, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, values, D3D11_USAGE_DEFAULT, 0);
    ID3D11Buffer *out = make_buffer(sizeof values, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, NULL, D3D11_USAGE_DEFAULT, 0);
    ID3D11Buffer *count = make_buffer(16, D3D11_BIND_CONSTANT_BUFFER, 0, 0, NULL, D3D11_USAGE_DEFAULT, 0);
    ID3D11Buffer *count_copy = make_buffer(16, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, NULL, D3D11_USAGE_DEFAULT, 0);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = { DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER };
    ud.Buffer.FirstElement = 0; ud.Buffer.NumElements = 128; ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
    ID3D11UnorderedAccessView *uavs[3];
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)in, &ud, &uavs[0])));
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)out, &ud, &uavs[1])));
    D3D11_TEXTURE2D_DESC td = { 4, 1, 1, 1, DXGI_FORMAT_R32_UINT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
    UINT zeros[4] = { 0, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { zeros, 16, 16 };
    ID3D11Texture2D *hist;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, &init, &hist)));
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)hist, NULL, &uavs[2])));
    UINT initial[3] = { 128, 0, 0 };
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 3, uavs, initial);
    ID3D11DeviceContext_Dispatch(ctx, 2, 1, 1);
    ID3D11DeviceContext_CopyStructureCount(ctx, count_copy, 4, uavs[1]);
    ID3D11DeviceContext_CopyStructureCount(ctx, count_copy, 8, uavs[0]);
    UINT got[128], counts[4];
    read_buffer(out, got, sizeof got);
    read_buffer(count_copy, counts, sizeof counts);
    CHECK(counts[1] == 64 && counts[2] == 0);
    UINT seen = 0, sum = 0;
    for (int i = 0; i < 64; i++) { if (got[i] % 20 == 10 && got[i] < 1280) seen++; sum += got[i]; }
    CHECK(seen == 64 && sum == 10 * 64 * 64);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)hist);
    D3D11_MAPPED_SUBRESOURCE m;
    UINT bins[4] = { 0 };
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(bins, m.pData, sizeof bins);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    CHECK(bins[0] == 32 && bins[1] == 32 && bins[2] == 32 && bins[3] == 32);
    printf("  append: %u appended (sum %u), %u left to consume, histogram %u %u %u %u\n", counts[1], sum, counts[2], bins[0], bins[1], bins[2], bins[3]);
    ID3D11UnorderedAccessView *none[3] = { NULL, NULL, NULL };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 3, none, NULL);
    for (int i = 0; i < 3; i++) ID3D11UnorderedAccessView_Release(uavs[i]);
    ID3D11Texture2D_Release(staging); ID3D11Texture2D_Release(hist);
    ID3D11Buffer_Release(count_copy); ID3D11Buffer_Release(count); ID3D11Buffer_Release(out); ID3D11Buffer_Release(in);
    ID3D11ComputeShader_Release(cs);
}

// Views: one slice and mip of a typeless texture array read as sRGB, a typed buffer view (from its third element),
// a typed UAV of a buffer, a UAV of a texture's second mip.
static void test_views(void) {
    ID3D11ComputeShader *cs;
    CHECK(SUCCEEDED(ID3D11Device_CreateComputeShader(dev, views_cs, sizeof views_cs, NULL, &cs)));
    // A 4x4 texture array: 2 slices, 2 mips; slice 1 mip 1 (2x2) has (128, 255, 0, 255) at (0, 0).
    UINT texels[2][2][16];
    memset(texels, 0, sizeof texels);
    texels[1][1][0] = 0xff00ff80u;
    D3D11_SUBRESOURCE_DATA init[4] = { { texels[0][0], 16, 64 }, { texels[0][1], 8, 16 }, { texels[1][0], 16, 64 }, { texels[1][1], 8, 16 } };
    D3D11_TEXTURE2D_DESC td = { 4, 4, 2, 2, DXGI_FORMAT_R8G8B8A8_TYPELESS, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *array;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, init, &array)));
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_SRV_DIMENSION_TEXTURE2DARRAY };
    sd.Texture2DArray.MostDetailedMip = 1; sd.Texture2DArray.MipLevels = 1; sd.Texture2DArray.FirstArraySlice = 1; sd.Texture2DArray.ArraySize = 1;
    ID3D11ShaderResourceView *srvs[2];
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)array, &sd, &srvs[0])));
    float floats[16] = { 0 };
    floats[12] = 42; floats[15] = 7;                          // element 3 of the buffer = element 2 of the view
    ID3D11Buffer *fb = make_buffer(sizeof floats, D3D11_BIND_SHADER_RESOURCE, 0, 0, floats, D3D11_USAGE_DEFAULT, 0);
    D3D11_SHADER_RESOURCE_VIEW_DESC bd = { DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_SRV_DIMENSION_BUFFER };
    bd.Buffer.FirstElement = 1; bd.Buffer.NumElements = 3;
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)fb, &bd, &srvs[1])));
    ID3D11Buffer *results = make_buffer(64, D3D11_BIND_UNORDERED_ACCESS, 0, 0, NULL, D3D11_USAGE_DEFAULT, 0);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = { DXGI_FORMAT_R32_UINT, D3D11_UAV_DIMENSION_BUFFER };
    ud.Buffer.FirstElement = 0; ud.Buffer.NumElements = 16;
    ID3D11UnorderedAccessView *uavs[2];
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)results, &ud, &uavs[0])));
    D3D11_TEXTURE2D_DESC wd = { 4, 4, 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
    ID3D11Texture2D *target;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &wd, NULL, &target)));
    D3D11_UNORDERED_ACCESS_VIEW_DESC tu = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_UAV_DIMENSION_TEXTURE2D };
    tu.Texture2D.MipSlice = 1;
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)target, &tu, &uavs[1])));
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 2, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 2, uavs, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    UINT r[16];
    read_buffer(results, r, sizeof r);
    CHECK(r[0] == 55 && r[1] == 255);                         // sRGB 128 is linear 0.2158
    CHECK(r[2] == 2 && r[3] == 1 * 16 + 1);                   // the view: 2 wide, 1 mip, 1 slice
    CHECK(r[4] == 42 && r[5] == 7);
    CHECK(r[6] == 2);                                         // the UAV is mip 1: 2 wide
    wd.Usage = D3D11_USAGE_STAGING; wd.BindFlags = 0; wd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging;
    ID3D11Device_CreateTexture2D(dev, &wd, NULL, &staging);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)target);
    D3D11_MAPPED_SUBRESOURCE m;
    UINT px = 0;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 1, D3D11_MAP_READ, 0, &m))) {
        px = *(UINT *)((BYTE *)m.pData + m.RowPitch + 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 1);
    }
    CHECK(px == 0xff0080ffu);
    printf("  views: sRGB slice %u %u, %ux? %u mips/slices, buffer view %u %u, mip-1 UAV %u wide, pixel %08x\n", r[0], r[1], r[2], r[3], r[4], r[5], r[6], px);
    ID3D11ShaderResourceView *no_srv[2] = { NULL, NULL };
    ID3D11UnorderedAccessView *no_uav[2] = { NULL, NULL };
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 2, no_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 2, no_uav, NULL);
    ID3D11Texture2D_Release(staging); ID3D11Texture2D_Release(target); ID3D11Texture2D_Release(array);
    ID3D11UnorderedAccessView_Release(uavs[0]); ID3D11UnorderedAccessView_Release(uavs[1]);
    ID3D11ShaderResourceView_Release(srvs[0]); ID3D11ShaderResourceView_Release(srvs[1]);
    ID3D11Buffer_Release(results); ID3D11Buffer_Release(fb); ID3D11ComputeShader_Release(cs);
}

// Outside views (Direct3D defines it; Metal does not): loads read 0, stores and atomics are dropped, a sample's
// array index is clamped. Each view is part of a larger resource whose other data is not 0, as a game's often are.
// (Before: buffer loads read the data beyond the view, stores and atomics changed it; the textures here read 0.)
static ID3D11Texture2D *make_texture(UINT w, UINT h, UINT layers, UINT bind, const UINT *data) {
    D3D11_SUBRESOURCE_DATA init[2] = { { data, w * 4, w * h * 4 }, { data + w * h, w * 4, w * h * 4 } };
    D3D11_TEXTURE2D_DESC td = { w, h, 1, layers, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, bind, 0, 0 };
    ID3D11Texture2D *t = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, data ? init : NULL, &t)));
    return t;
}
static void test_bounds(void) {
    ID3D11ComputeShader *cs;
    CHECK(SUCCEEDED(ID3D11Device_CreateComputeShader(dev, bounds_cs, sizeof bounds_cs, NULL, &cs)));
    UINT words[8], ones[8], layer[8];
    for (int i = 0; i < 8; i++) { words[i] = 1000 + i; ones[i] = 0xffffffffu; layer[i] = i < 4 ? 0x40404040u : 0xffffffffu; }
    float floats[16];
    for (int i = 0; i < 16; i++) floats[i] = 50;
    ID3D11Buffer *structured = make_buffer(32, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, words, D3D11_USAGE_DEFAULT, 0);
    ID3D11Buffer *raw = make_buffer(32, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, words, D3D11_USAGE_DEFAULT, 0);
    ID3D11Buffer *fb = make_buffer(sizeof floats, D3D11_BIND_SHADER_RESOURCE, 0, 0, floats, D3D11_USAGE_DEFAULT, 0);
    ID3D11Buffer *rwb = make_buffer(32, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, words, D3D11_USAGE_DEFAULT, 0);
    ID3D11Buffer *results = make_buffer(64, D3D11_BIND_UNORDERED_ACCESS, 0, 0, NULL, D3D11_USAGE_DEFAULT, 0);
    ID3D11Texture2D *image = make_texture(2, 2, 1, D3D11_BIND_SHADER_RESOURCE, ones);
    ID3D11Texture2D *layers = make_texture(2, 2, 2, D3D11_BIND_SHADER_RESOURCE, layer);
    ID3D11Texture2D *target = make_texture(2, 2, 1, D3D11_BIND_UNORDERED_ACCESS, NULL);
    ID3D11ShaderResourceView *srvs[5];
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = { DXGI_FORMAT_UNKNOWN, D3D11_SRV_DIMENSION_BUFFER };
    sd.Buffer.FirstElement = 0; sd.Buffer.NumElements = 4;
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)structured, &sd, &srvs[0])));
    D3D11_SHADER_RESOURCE_VIEW_DESC rd = { DXGI_FORMAT_R32_TYPELESS, D3D11_SRV_DIMENSION_BUFFEREX };
    rd.BufferEx.FirstElement = 0; rd.BufferEx.NumElements = 4; rd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)raw, &rd, &srvs[1])));
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)image, NULL, &srvs[2])));
    D3D11_SHADER_RESOURCE_VIEW_DESC bd = { DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_SRV_DIMENSION_BUFFER };
    bd.Buffer.FirstElement = 0; bd.Buffer.NumElements = 2;
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)fb, &bd, &srvs[3])));
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)layers, NULL, &srvs[4])));
    ID3D11UnorderedAccessView *uavs[3];
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = { DXGI_FORMAT_UNKNOWN, D3D11_UAV_DIMENSION_BUFFER };
    ud.Buffer.FirstElement = 0; ud.Buffer.NumElements = 4;
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)rwb, &ud, &uavs[0])));
    D3D11_UNORDERED_ACCESS_VIEW_DESC td = { DXGI_FORMAT_R32_UINT, D3D11_UAV_DIMENSION_BUFFER };
    td.Buffer.FirstElement = 0; td.Buffer.NumElements = 16;
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)results, &td, &uavs[1])));
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)target, NULL, &uavs[2])));
    D3D11_SAMPLER_DESC smp = { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP };
    smp.MaxLOD = 1000;
    ID3D11SamplerState *sampler;
    CHECK(SUCCEEDED(ID3D11Device_CreateSamplerState(dev, &smp, &sampler)));
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 5, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 3, uavs, NULL);
    ID3D11DeviceContext_CSSetSamplers(ctx, 0, 1, &sampler);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    UINT r[16], w[8];
    read_buffer(results, r, sizeof r);
    read_buffer(rwb, w, sizeof w);
    CHECK(r[0] == 1003 && r[1] == 1);                          // structured: inside, outside
    CHECK(r[2] == 1003 && r[3] == 1);                          // raw
    CHECK(r[4] == 100 && r[5] == 1 && r[6] == 1);              // texture: inside, outside, a mip it lacks
    CHECK(r[7] == 1);                                          // typed buffer
    CHECK(r[8] == 100 && r[9] == 25);                          // array index clamped to the last, the first layer
    CHECK(r[10] == 1);                                         // a layer it lacks
    CHECK(r[11] == 1);                                         // the atomic outside returned 0
    CHECK(w[1] == 11 && w[5] == 1005 && w[6] == 1006);         // stores outside the view dropped
    D3D11_TEXTURE2D_DESC sdesc = { 2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
    ID3D11Texture2D *staging;
    ID3D11Device_CreateTexture2D(dev, &sdesc, NULL, &staging);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)target);
    D3D11_MAPPED_SUBRESOURCE m;
    UINT px[4] = { 1, 1, 1, 1 };
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) px[y * 2 + x] = *(UINT *)((BYTE *)m.pData + y * m.RowPitch + x * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0 && px[3] == 0xff00ff00u);   // the write outside dropped
    printf("  outside views: structured %u %u, raw %u %u, texture %u %u %u, typed %u, layers %u %u %u, atomic %u, buffer %u %u %u, image %08x %08x %08x %08x\n",
           r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], w[1], w[5], w[6], px[0], px[1], px[2], px[3]);
    ID3D11ShaderResourceView *no_srv[5] = { 0 };
    ID3D11UnorderedAccessView *no_uav[3] = { 0 };
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 5, no_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 3, no_uav, NULL);
    for (int i = 0; i < 5; i++) ID3D11ShaderResourceView_Release(srvs[i]);
    for (int i = 0; i < 3; i++) ID3D11UnorderedAccessView_Release(uavs[i]);
    ID3D11SamplerState_Release(sampler); ID3D11Texture2D_Release(staging);
    ID3D11Texture2D_Release(target); ID3D11Texture2D_Release(layers); ID3D11Texture2D_Release(image);
    ID3D11Buffer_Release(results); ID3D11Buffer_Release(rwb); ID3D11Buffer_Release(fb); ID3D11Buffer_Release(raw); ID3D11Buffer_Release(structured);
    ID3D11ComputeShader_Release(cs);
}

// A constant buffer read beyond the one bound (declared with 32 float4s, 2 bound: a dynamic buffer, so a slice with
// other constants after it) and an indexable temp outside itself: both read 0, and the write outside is dropped.
static void test_cb_bounds(void) {
    ID3D11ComputeShader *cs;
    CHECK(SUCCEEDED(ID3D11Device_CreateComputeShader(dev, cbounds_cs, sizeof cbounds_cs, NULL, &cs)));
    ID3D11Buffer *small = make_buffer(32, D3D11_BIND_CONSTANT_BUFFER, 0, 0, NULL, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
    ID3D11Buffer *others[8];
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)small, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float v[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        memcpy(&v[4], &(UINT){ 7 }, 4);
        memcpy(m.pData, v, 32);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)small, 0);
    }
    for (int i = 0; i < 8; i++) {                               // (constants after it: 0x55 everywhere)
        others[i] = make_buffer(256, D3D11_BIND_CONSTANT_BUFFER, 0, 0, NULL, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)others[i], 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memset(m.pData, 0x55, 256);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)others[i], 0);
        }
    }
    UINT idx[8] = { 1, 20, 6, 2, 7, 0, 0, 0 };
    ID3D11Buffer *ib = make_buffer(32, D3D11_BIND_CONSTANT_BUFFER, 0, 0, idx, D3D11_USAGE_DEFAULT, 0);
    ID3D11Buffer *results = make_buffer(32, D3D11_BIND_UNORDERED_ACCESS, 0, 0, NULL, D3D11_USAGE_DEFAULT, 0);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = { DXGI_FORMAT_R32_UINT, D3D11_UAV_DIMENSION_BUFFER };
    ud.Buffer.FirstElement = 0; ud.Buffer.NumElements = 8;
    ID3D11UnorderedAccessView *uav;
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)results, &ud, &uav)));
    ID3D11Buffer *cbs[2] = { small, ib };
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 2, cbs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    UINT r[8];
    read_buffer(results, r, sizeof r);
    CHECK(r[0] == 7 && r[1] == 1 && r[4] == 1);                 // inside the bound constants; beyond them (computed, fixed)
    CHECK(r[2] == 12 && r[3] == 1);                             // the temp array: inside; outside (and not written)
    printf("  outside constants and temp arrays: %u %u %u, %u %u\n", r[0], r[1], r[4], r[2], r[3]);
    ID3D11UnorderedAccessView *no_uav = NULL;
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &no_uav, NULL);
    ID3D11UnorderedAccessView_Release(uav);
    ID3D11Buffer_Release(results); ID3D11Buffer_Release(ib); ID3D11Buffer_Release(small);
    for (int i = 0; i < 8; i++) ID3D11Buffer_Release(others[i]);
    ID3D11ComputeShader_Release(cs);
}

// Block-compressed textures (BC1, and BC3 updated in part), a 3D and a 1D texture.
static void test_formats(void) {
    ID3D11ComputeShader *cs;
    CHECK(SUCCEEDED(ID3D11Device_CreateComputeShader(dev, formats_cs, sizeof formats_cs, NULL, &cs)));
    // BC1: one 4x4 block, colour 0 red (565), colour 1 blue, every index 0: all red.
    unsigned char bc1[8] = { 0x00, 0xf8, 0x1f, 0x00, 0, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { bc1, 8, 8 };
    D3D11_TEXTURE2D_DESC td = { 4, 4, 1, 1, DXGI_FORMAT_BC1_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *t1, *t3;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, &init, &t1)));
    // BC3: 8x8 (2x2 blocks), the bottom-right block set afterwards: alpha 255, colour green.
    td.Width = td.Height = 8; td.Format = DXGI_FORMAT_BC3_UNORM;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &t3)));
    unsigned char bc3[16] = { 0xff, 0x00, 0, 0, 0, 0, 0, 0, 0xe0, 0x07, 0xe0, 0x07, 0, 0, 0, 0 };
    D3D11_BOX box = { 4, 4, 0, 8, 8, 1 };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)t3, 0, &box, bc3, 16, 16);
    // A 4x4x4 volume of x + 10y + 100z, a row of 8 texels of 30i.
    float vol[64];
    for (int i = 0; i < 64; i++) vol[i] = (float)(i % 4 + 10 * (i / 4 % 4) + 100 * (i / 16));
    D3D11_SUBRESOURCE_DATA vinit = { vol, 16, 64 };
    D3D11_TEXTURE3D_DESC vd = { 4, 4, 4, 1, DXGI_FORMAT_R32_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture3D *t3d;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture3D(dev, &vd, &vinit, &t3d)));
    unsigned char line[8];
    for (int i = 0; i < 8; i++) line[i] = (unsigned char)(30 * i);
    D3D11_SUBRESOURCE_DATA linit = { line, 8, 8 };
    D3D11_TEXTURE1D_DESC ld = { 8, 1, 1, DXGI_FORMAT_R8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture1D *t1d;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture1D(dev, &ld, &linit, &t1d)));
    D3D11_TEXTURE3D_DESC vback;
    ID3D11Texture3D_GetDesc(t3d, &vback);
    CHECK(vback.Depth == 4 && vback.Format == DXGI_FORMAT_R32_FLOAT);
    D3D11_RESOURCE_DIMENSION rdim;
    ID3D11Texture1D_GetType(t1d, &rdim);
    CHECK(rdim == D3D11_RESOURCE_DIMENSION_TEXTURE1D);
    ID3D11ShaderResourceView *srvs[4];
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t1, NULL, &srvs[0])));
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t3, NULL, &srvs[1])));
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t3d, NULL, &srvs[2])));
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t1d, NULL, &srvs[3])));
    ID3D11Buffer *results = make_buffer(64, D3D11_BIND_UNORDERED_ACCESS, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, NULL, D3D11_USAGE_DEFAULT, 0);
    ID3D11UnorderedAccessView *uav;
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *)results, NULL, &uav)));
    ID3D11DeviceContext_CSSetShader(ctx, cs, NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 4, srvs);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &uav, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    float r[16];
    read_buffer(results, r, sizeof r);
    CHECK(r[0] == 1 && r[1] == 0 && r[2] == 0 && r[3] == 1);
    CHECK(r[4] == 0 && r[5] == 1 && r[6] == 0 && r[7] == 1);
    CHECK(r[8] == 321 && fabsf(r[9] - 150.0f / 255) < 0.002f);
    CHECK(r[12] == 4 && r[13] == 4 && r[14] == 4);
    // A staging copy of the BC1 texture maps with a row pitch of one block row.
    td.Width = td.Height = 4; td.Format = DXGI_FORMAT_BC1_UNORM; td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *staging;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging)));
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)t1);
    D3D11_MAPPED_SUBRESOURCE m = { 0 };
    int same = 0;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        same = !memcmp(m.pData, bc1, 8);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    CHECK(m.RowPitch == 8 && same);
    printf("  formats: BC1 (%g %g %g %g), BC3 (%g %g %g %g), 3D %g, 1D %g, volume %gx%gx%g, BC1 row pitch %u\n",
           r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[12], r[13], r[14], m.RowPitch);
    ID3D11ShaderResourceView *no_srv[4] = { NULL, NULL, NULL, NULL };
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 4, no_srv);
    for (int i = 0; i < 4; i++) ID3D11ShaderResourceView_Release(srvs[i]);
    ID3D11UnorderedAccessView_Release(uav); ID3D11Buffer_Release(results);
    ID3D11Texture2D_Release(staging); ID3D11Texture2D_Release(t1); ID3D11Texture2D_Release(t3);
    ID3D11Texture3D_Release(t3d); ID3D11Texture1D_Release(t1d); ID3D11ComputeShader_Release(cs);
}

int main(void) {
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) { printf("compute: no device -> FAILED\n"); return 1; }
    test_sum();
    test_atomics_and_texture();
    test_append_consume();
    test_views();
    test_formats();
    test_bounds();
    test_cb_bounds();
    printf("compute: shared-memory sums, atomics, writable texture, UAV clear, append/consume, texture atomics, views, formats, outside views and constants: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures;
}
