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

int main(void) {
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) { printf("compute: no device -> FAILED\n"); return 1; }
    test_sum();
    test_atomics_and_texture();
    test_append_consume();
    test_views();
    printf("compute: shared-memory sums, atomics, writable texture, UAV clear, append/consume, texture atomics, views: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures;
}
