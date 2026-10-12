// CPU cost of Direct3D 11 draws on this runtime: a game-like stream of small indexed draws (a dynamic constant
// buffer mapped with DISCARD per draw, textures and constant buffers switched between draws, some state rebound
// every draw), timed per draw. Not a pass/fail test: "make bench" prints the numbers.
//   drawbench.exe [draws per frame] [frames]
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include "shaders/tex_vs.h"
#include "shaders/tex_ps.h"

static ID3D11Buffer *buffer(ID3D11Device *dev, UINT bind, const void *data, UINT size, int dynamic) {
    D3D11_BUFFER_DESC d = { size, dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT, bind, dynamic ? D3D11_CPU_ACCESS_WRITE : 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
    ID3D11Buffer *b = NULL;
    ID3D11Device_CreateBuffer(dev, &d, data ? &init : NULL, &b);
    return b;
}

int main(int argc, char **argv) {
    int draws = argc > 1 ? atoi(argv[1]) : 4000, frames = argc > 2 ? atoi(argv[2]) : 30;
    ID3D11Device *dev; ID3D11DeviceContext *ctx;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) { printf("drawbench: no device\n"); return 1; }
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11InputLayout *layout;
    ID3D11Device_CreateVertexShader(dev, tex_vs, sizeof tex_vs, NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, tex_ps, sizeof tex_ps, NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11Device_CreateInputLayout(dev, el, 2, tex_vs, sizeof tex_vs, &layout);
    UINT texels[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu };
    D3D11_TEXTURE2D_DESC td = { 2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { texels, 8, 0 };
    ID3D11Texture2D *tex[2]; ID3D11ShaderResourceView *srv[2];
    for (int i = 0; i < 2; i++) {
        ID3D11Device_CreateTexture2D(dev, &td, &init, &tex[i]);
        ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex[i], NULL, &srv[i]);
    }
    D3D11_SAMPLER_DESC sd = { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, { 0 }, 0, D3D11_FLOAT32_MAX };
    ID3D11SamplerState *smp;
    ID3D11Device_CreateSamplerState(dev, &sd, &smp);
    float quad[16] = { -1, 1, 0, 0,   1, 1, 1, 0,   1, -1, 1, 1,   -1, -1, 0, 1 };
    WORD idx[6] = { 0, 1, 2, 0, 2, 3 };
    ID3D11Buffer *vb = buffer(dev, D3D11_BIND_VERTEX_BUFFER, quad, sizeof quad, 0), *ib = buffer(dev, D3D11_BIND_INDEX_BUFFER, idx, sizeof idx, 0);
    float tint[2][4] = { { 1, 1, 1, 1 }, { 0.5f, 0.5f, 0.5f, 1 } };
    ID3D11Buffer *cb_ps[2] = { buffer(dev, D3D11_BIND_CONSTANT_BUFFER, tint[0], 16, 0), buffer(dev, D3D11_BIND_CONSTANT_BUFFER, tint[1], 16, 0) };
    ID3D11Buffer *cb_vs = buffer(dev, D3D11_BIND_CONSTANT_BUFFER, NULL, 16, 1);
    D3D11_TEXTURE2D_DESC rd = { 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Texture2D *rt; ID3D11RenderTargetView *rtv;
    ID3D11Device_CreateTexture2D(dev, &rd, NULL, &rt);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)rt, NULL, &rtv);
    D3D11_VIEWPORT vp = { 0, 0, 256, 256, 0, 1 };
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *done;
    ID3D11Device_CreateQuery(dev, &qd, &done);
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    double best = 1e9, total = 0;
    UINT stride = 16, offset = 0;
    for (int f = 0; f < frames; f++) {
        QueryPerformanceCounter(&t0);
        float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
        for (int d = 0; d < draws; d++) {
            ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
            ID3D11DeviceContext_IASetInputLayout(ctx, layout);
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
            ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
            ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cb_vs, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                float *t = m.pData;
                t[0] = (float)(d % 64) / 64 - 0.5f; t[1] = (float)(d / 64 % 64) / 64 - 0.5f; t[2] = t[3] = 1.0f / 32;
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cb_vs, 0);
            }
            ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb_vs);
            ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb_ps[d / 8 & 1]);
            ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv[d / 4 & 1]);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
            ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
        }
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)done);
        ID3D11DeviceContext_Flush(ctx);
        QueryPerformanceCounter(&t1);
        double us = (double)(t1.QuadPart - t0.QuadPart) * 1e6 / freq.QuadPart / draws;
        if (f >= 2) { total += us; if (us < best) best = us; }        // the first frames build pipelines
        BOOL ok = FALSE;
        while (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)done, &ok, sizeof ok, 0) == S_FALSE) Sleep(0);
    }
    printf("drawbench: %d draws x %d frames: %.2f us per draw (best frame %.2f us)\n", draws, frames, total / (frames > 2 ? frames - 2 : 1), best);
    return 0;
}
