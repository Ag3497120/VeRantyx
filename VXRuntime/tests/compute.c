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

int main(void) {
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) { printf("compute: no device -> FAILED\n"); return 1; }
    test_sum();
    test_atomics_and_texture();
    printf("compute: shared-memory sums, atomics, writable texture, UAV clear: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures;
}
