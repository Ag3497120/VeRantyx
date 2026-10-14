// Milestone 6: drawing with Direct3D 11 on Metal, with shaders compiled by Microsoft's HLSL compiler
// (tests/shaders, DXBC) and translated by this runtime. Results are read back from the GPU and checked.
// "draw.exe --interactive" shows a spinning triangle in a window instead.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "shaders/color_vs.h"
#include "shaders/color_ps.h"
#include "shaders/tex_vs.h"
#include "shaders/tex_ps.h"
#include "shaders/fullscreen_vs.h"
#include "shaders/math_ps.h"
#include "shaders/quad_gs.h"
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;

static ID3D11Texture2D *target(UINT w, UINT h, DXGI_FORMAT f, ID3D11RenderTargetView **rtv) {
    D3D11_TEXTURE2D_DESC d = { w, h, 1, 1, f, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *t = NULL;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &t);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)t, NULL, rtv);
    D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    return t;
}
// Reads a w x h texture back; `bytes` per pixel.
static void readback(ID3D11Texture2D *t, void *out, UINT bytes) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *s = NULL;
    ID3D11Device_CreateTexture2D(dev, &d, NULL, &s);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)s, (ID3D11Resource *)t);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, 0, &m))) {
        for (UINT y = 0; y < d.Height; y++) memcpy((BYTE *)out + y * d.Width * bytes, (BYTE *)m.pData + y * m.RowPitch, d.Width * bytes);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s, 0);
    }
    ID3D11Texture2D_Release(s);
}
static ID3D11Buffer *buffer(UINT bind, const void *data, UINT size) {
    D3D11_BUFFER_DESC d = { size, D3D11_USAGE_DEFAULT, bind, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { data, 0, 0 };
    ID3D11Buffer *b = NULL;
    ID3D11Device_CreateBuffer(dev, &d, data ? &init : NULL, &b);
    return b;
}
#define R(p) ((p) & 0xff)
#define G(p) (((p) >> 8) & 0xff)
#define B(p) (((p) >> 16) & 0xff)

typedef struct { float x, y, z, r, g, b, a; } color_vertex;
static ID3D11VertexShader *color_vs_obj; static ID3D11PixelShader *color_ps_obj; static ID3D11InputLayout *color_layout;
static void color_pipeline(void) {
    CHECK(SUCCEEDED(ID3D11Device_CreateVertexShader(dev, color_vs, sizeof color_vs, NULL, &color_vs_obj)));
    CHECK(SUCCEEDED(ID3D11Device_CreatePixelShader(dev, color_ps, sizeof color_ps, NULL, &color_ps_obj)));
    D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    CHECK(SUCCEEDED(ID3D11Device_CreateInputLayout(dev, el, 2, color_vs, sizeof color_vs, &color_layout)));
}
static void draw_colored(const color_vertex *v, UINT n) {
    ID3D11Buffer *vb = buffer(D3D11_BIND_VERTEX_BUFFER, v, n * sizeof *v);
    UINT stride = sizeof *v, offset = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, color_layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, color_vs_obj, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, color_ps_obj, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, n, 0);
    ID3D11Buffer_Release(vb);
}

static void test_triangle(void) {
    ID3D11RenderTargetView *rtv; ID3D11Texture2D *t = target(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, &rtv);
    float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    color_vertex v[3] = { { -1, 1, 0, 1, 0, 0, 1 }, { 1, -1, 0, 0, 1, 0, 1 }, { -1, -1, 0, 0, 0, 1, 1 } };   // clockwise: front facing
    draw_colored(v, 3);
    static UINT px[64 * 64];
    readback(t, px, 4);
    UINT tl = px[8 * 64 + 1], br = px[62 * 64 + 56], bl = px[61 * 64 + 2], tr = px[4 * 64 + 60];   // just inside each corner
    CHECK(R(tl) > 200 && G(tl) < 60 && B(tl) < 60);         // red corner
    CHECK(G(br) > 200 && R(br) < 60 && B(br) < 60);         // green corner
    CHECK(B(bl) > 200 && R(bl) < 60 && G(bl) < 60);         // blue corner
    CHECK(tr == 0xff000000u);                               // outside: the clear color
    printf("  triangle: corners %06x %06x %06x, outside %08x\n", tl & 0xffffff, br & 0xffffff, bl & 0xffffff, tr);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(t);
}

static void test_textured(void) {
    ID3D11RenderTargetView *rtv; ID3D11Texture2D *t = target(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, &rtv);
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11InputLayout *layout;
    CHECK(SUCCEEDED(ID3D11Device_CreateVertexShader(dev, tex_vs, sizeof tex_vs, NULL, &vs)));
    CHECK(SUCCEEDED(ID3D11Device_CreatePixelShader(dev, tex_ps, sizeof tex_ps, NULL, &ps)));
    D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    CHECK(SUCCEEDED(ID3D11Device_CreateInputLayout(dev, el, 2, tex_vs, sizeof tex_vs, &layout)));
    // A 2x2 texture: red, green / blue, white.
    UINT texels[4] = { 0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu };
    D3D11_TEXTURE2D_DESC td = { 2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { texels, 8, 0 };
    ID3D11Texture2D *tex; ID3D11ShaderResourceView *srv; ID3D11SamplerState *smp;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, &init, &tex)));
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)tex, NULL, &srv)));
    D3D11_SAMPLER_DESC sd = { D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP, 0, 1, D3D11_COMPARISON_NEVER, { 0 }, 0, D3D11_FLOAT32_MAX };
    CHECK(SUCCEEDED(ID3D11Device_CreateSamplerState(dev, &sd, &smp)));
    float quad[16] = { -1, 1, 0, 0,   1, 1, 1, 0,   1, -1, 1, 1,   -1, -1, 0, 1 };
    WORD idx[6] = { 0, 1, 2, 0, 2, 3 };
    ID3D11Buffer *vb = buffer(D3D11_BIND_VERTEX_BUFFER, quad, sizeof quad), *ib = buffer(D3D11_BIND_INDEX_BUFFER, idx, sizeof idx);
    float transform[4] = { 0, 0, 1, 1 }, tint[4] = { 0.5f, 1, 1, 1 };
    ID3D11Buffer *cb_vs = buffer(D3D11_BIND_CONSTANT_BUFFER, transform, 16), *cb_ps = buffer(D3D11_BIND_CONSTANT_BUFFER, tint, 16);
    UINT stride = 16, offset = 0;
    float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb_vs);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb_ps);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp);
    ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
    static UINT px[64 * 64];
    readback(t, px, 4);
    UINT q[4] = { px[16 * 64 + 16], px[16 * 64 + 48], px[48 * 64 + 16], px[48 * 64 + 48] };
    CHECK(R(q[0]) >= 126 && R(q[0]) <= 129 && G(q[0]) == 0 && B(q[0]) == 0);   // red * 0.5
    CHECK(G(q[1]) == 255 && R(q[1]) == 0);                                    // green
    CHECK(B(q[2]) == 255 && R(q[2]) == 0);                                    // blue
    CHECK(R(q[3]) >= 126 && R(q[3]) <= 129 && G(q[3]) == 255 && B(q[3]) == 255);   // white with half red
    printf("  textured quad: %06x %06x %06x %06x\n", q[0] & 0xffffff, q[1] & 0xffffff, q[2] & 0xffffff, q[3] & 0xffffff);

    // Constant buffer updates between draws: each draw keeps the value it was given.
    float white_texels_tint_red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 };
    float left[4] = { -0.5f, 0, 0.5f, 1 }, right[4] = { 0.5f, 0, 0.5f, 1 };   // offset x, y, scale x, y: half-width quads
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb_vs, 0, NULL, left, 0, 0);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb_ps, 0, NULL, white_texels_tint_red, 0, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb_vs, 0, NULL, right, 0, 0);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb_ps, 0, NULL, green, 0, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 6, 0, 0);
    readback(t, px, 4);
    UINT l = px[40 * 64 + 8], r = px[40 * 64 + 56];          // bottom-left texel is blue: tinted red gives 0; use white texel rows
    UINT lw = px[40 * 64 + 24], rw = px[40 * 64 + 56];
    CHECK(R(lw) == 255 && G(lw) == 0);                      // the left draw: white texel * red
    CHECK(G(rw) == 255 && R(rw) == 0);                      // the right draw: white texel * green
    printf("  constant buffer per draw: left %06x right %06x\n", lw & 0xffffff, rw & 0xffffff);
    (void)l; (void)r;
    ID3D11Buffer_Release(vb); ID3D11Buffer_Release(ib); ID3D11Buffer_Release(cb_vs); ID3D11Buffer_Release(cb_ps);
    ID3D11SamplerState_Release(smp); ID3D11ShaderResourceView_Release(srv); ID3D11Texture2D_Release(tex);
    ID3D11InputLayout_Release(layout); ID3D11VertexShader_Release(vs); ID3D11PixelShader_Release(ps);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(t);
}

static void test_math(void) {
    ID3D11RenderTargetView *rtv; ID3D11Texture2D *t = target(4, 4, DXGI_FORMAT_R32G32B32A32_FLOAT, &rtv);
    ID3D11VertexShader *vs; ID3D11PixelShader *ps;
    CHECK(SUCCEEDED(ID3D11Device_CreateVertexShader(dev, fullscreen_vs, sizeof fullscreen_vs, NULL, &vs)));
    CHECK(SUCCEEDED(ID3D11Device_CreatePixelShader(dev, math_ps, sizeof math_ps, NULL, &ps)));
    struct { float a[4], b[4]; int n[4]; } in = { { 1.5f, -2, 3, 4 }, { 0.5f, 3, 2.25f, 9 }, { 5, 3, 2, 7 } };
    ID3D11Buffer *cb = buffer(D3D11_BIND_CONSTANT_BUFFER, &in, sizeof in);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    float px[16 * 4];
    readback(t, px, 16);
    // The same computation on the CPU.
    float *a = in.a, *b = in.b; int *n = in.n;
    float e0 = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    float e1 = 1.0f / sqrtf(a[3]) + sqrtf(b[3]);
    float e2 = fmaxf(a[0], b[1]) - fminf(a[2], b[0]) + (b[2] - floorf(b[2])) + fabsf(-a[1]);
    int i = n[0] * n[1] + (n[2] << 2) - (n[3] >> 1);
    unsigned u = (unsigned)n[0] ^ 0xffu;
    float acc = 0;
    for (int k = 0; k < n[0]; k++) { if (k == 2) continue; acc += (float)k; }
    float e3 = (float)i + (float)u + acc + (a[0] > b[0] ? 100.0f : 200.0f) + exp2f(b[1]) + log2f(a[3]);
    float *o = &px[(1 * 4 + 1) * 4];
    CHECK(fabsf(o[0] - e0) < 1e-4f && fabsf(o[1] - e1) < 1e-4f && fabsf(o[2] - e2) < 1e-4f && fabsf(o[3] - e3) < 1e-3f);
    printf("  math: (%g, %g, %g, %g), expected (%g, %g, %g, %g)\n", o[0], o[1], o[2], o[3], e0, e1, e2, e3);
    ID3D11Buffer_Release(cb); ID3D11VertexShader_Release(vs); ID3D11PixelShader_Release(ps);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(t);
}

static void test_depth_and_blend(void) {
    ID3D11RenderTargetView *rtv; ID3D11Texture2D *t = target(32, 32, DXGI_FORMAT_R8G8B8A8_UNORM, &rtv);
    D3D11_TEXTURE2D_DESC dd = { 32, 32, 1, 1, DXGI_FORMAT_D32_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL, 0, 0 };
    ID3D11Texture2D *depth; ID3D11DepthStencilView *dsv;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &dd, NULL, &depth)));
    CHECK(SUCCEEDED(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)depth, NULL, &dsv)));
    float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, NULL, 0);                // the default: LESS, writes on
    color_vertex near_red[3] = { { -1, 1, 0.5f, 1, 0, 0, 1 }, { 3, 1, 0.5f, 1, 0, 0, 1 }, { -1, -3, 0.5f, 1, 0, 0, 1 } };
    color_vertex far_green[3] = { { -1, 1, 0.8f, 0, 1, 0, 1 }, { 3, 1, 0.8f, 0, 1, 0, 1 }, { -1, -3, 0.8f, 0, 1, 0, 1 } };
    draw_colored(near_red, 3);
    draw_colored(far_green, 3);                                              // behind: hidden
    static UINT px[32 * 32];
    readback(t, px, 4);
    CHECK(px[16 * 32 + 16] == 0xff0000ffu);
    printf("  depth: %08x (the nearer red stays)\n", px[16 * 32 + 16]);
    // Blending half-transparent red over blue.
    D3D11_BLEND_DESC bd = { 0 };
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState *blend;
    CHECK(SUCCEEDED(ID3D11Device_CreateBlendState(dev, &bd, &blend)));
    float blue[4] = { 0, 0, 1, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, blue);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetBlendState(ctx, blend, NULL, 0xffffffffu);
    color_vertex half_red[3] = { { -1, 1, 0, 1, 0, 0, 0.5f }, { 3, 1, 0, 1, 0, 0, 0.5f }, { -1, -3, 0, 1, 0, 0, 0.5f } };
    draw_colored(half_red, 3);
    readback(t, px, 4);
    UINT p = px[16 * 32 + 16];
    CHECK(R(p) >= 126 && R(p) <= 129 && G(p) == 0 && B(p) >= 126 && B(p) <= 129);
    printf("  blend: %06x (half red over blue)\n", p & 0xffffff);
    ID3D11DeviceContext_OMSetBlendState(ctx, NULL, NULL, 0xffffffffu);
    ID3D11BlendState_Release(blend); ID3D11DepthStencilView_Release(dsv); ID3D11Texture2D_Release(depth);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(t);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_DESTROY) PostQuitMessage(0); return DefWindowProcA(h, m, w, l); }
static void interactive(void) {
    WNDCLASSA wc = { 0, proc, 0, 0, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "VXDraw" };
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("VXDraw", "VXRuntime: Direct3D 11 triangle on Metal", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 640, 520, NULL, NULL, wc.hInstance, NULL);
    IDXGIFactory *factory; IDXGIDevice *dx; IDXGIAdapter *ad; IDXGISwapChain *sc;
    ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dx);
    IDXGIDevice_GetAdapter(dx, &ad);
    IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **)&factory);
    DXGI_SWAP_CHAIN_DESC sd = { { 0, 0, { 60, 1 }, DXGI_FORMAT_R8G8B8A8_UNORM }, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, hwnd, TRUE, DXGI_SWAP_EFFECT_FLIP_DISCARD, 0 };
    IDXGIFactory_CreateSwapChain(factory, (IUnknown *)dev, &sd, &sc);
    ID3D11Texture2D *back; ID3D11RenderTargetView *rtv;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&back);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv);
    D3D11_TEXTURE2D_DESC bd; ID3D11Texture2D_GetDesc(back, &bd);
    D3D11_VIEWPORT vp = { 0, 0, (float)bd.Width, (float)bd.Height, 0, 1 };
    for (int f = 0; f < 1200; f++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { if (msg.message == WM_QUIT) return; DispatchMessageA(&msg); }
        float a = f * 0.02f, bg[4] = { 0.05f, 0.05f, 0.1f, 1 };
        color_vertex v[3];
        for (int k = 0; k < 3; k++) {
            float ang = a + k * 2.0944f;
            v[k] = (color_vertex){ 0.7f * sinf(ang), 0.7f * cosf(ang), 0, k == 0, k == 1, k == 2, 1 };
        }
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, bg);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        draw_colored(v, 3);
        IDXGISwapChain_Present(sc, 1, 0);
    }
}

// GenerateMips: a 4x4 texture whose top mip is half black, half white (red channel) averages to grey at 1x1.
static void test_generate_mips(void) {
    D3D11_TEXTURE2D_DESC d = { 4, 4, 3, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS };
    ID3D11Texture2D *t = NULL; ID3D11ShaderResourceView *srv = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &t)));
    CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)t, NULL, &srv)));
    UINT top[16];
    for (int i = 0; i < 16; i++) top[i] = i % 4 < 2 ? 0xff000000u : 0xff0000ffu;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)t, 0, NULL, top, 16, 0);
    ID3D11DeviceContext_GenerateMips(ctx, srv);
    D3D11_TEXTURE2D_DESC sd = { 1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
    ID3D11Texture2D *s = NULL;
    ID3D11Device_CreateTexture2D(dev, &sd, NULL, &s);
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)s, 0, 0, 0, 0, (ID3D11Resource *)t, 2, NULL);
    D3D11_MAPPED_SUBRESOURCE m;
    UINT px = 0, polls = 0;
    HRESULT hr;                                             // polled as games read back without stalling
    while ((hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)s, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m)) == DXGI_ERROR_WAS_STILL_DRAWING && polls < 100000) { polls++; Sleep(0); }
    CHECK(hr == S_OK);
    if (SUCCEEDED(hr)) { px = *(UINT *)m.pData; ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)s, 0); }
    CHECK(R(px) >= 120 && R(px) <= 135);
    printf("  generated mips: 1x1 level %08x (read back without waiting after %u polls)\n", px, polls);
    ID3D11Texture2D_Release(s); ID3D11ShaderResourceView_Release(srv); ID3D11Texture2D_Release(t);
}

// A geometry shader: one point becomes a square (a strip of four), and after a cut a small triangle in the corner.
static void test_geometry_shader(void) {
    ID3D11RenderTargetView *rtv; ID3D11Texture2D *t = target(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, &rtv);
    ID3D11GeometryShader *gs = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateGeometryShader(dev, quad_gs, sizeof quad_gs, NULL, &gs)));
    float size[4] = { 0.25f, 0, 0, 0 };
    ID3D11Buffer *cb = buffer(D3D11_BIND_CONSTANT_BUFFER, size, sizeof size);
    float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    color_vertex v[1] = { { 0, 0, 0, 1, 0, 0, 1 } };
    ID3D11Buffer *vb = buffer(D3D11_BIND_VERTEX_BUFFER, v, sizeof v);
    UINT stride = sizeof v[0], offset = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, color_layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(ctx, color_vs_obj, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_GSSetConstantBuffers(ctx, 0, 1, &cb);
    ID3D11DeviceContext_PSSetShader(ctx, color_ps_obj, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 1, 0);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    static UINT px[64 * 64];
    readback(t, px, 4);
    UINT centre = px[32 * 64 + 32], inside = px[26 * 64 + 26], outside = px[16 * 64 + 48], corner = px[1 * 64 + 1];
    CHECK(R(centre) > 200 && G(centre) < 60 && B(centre) < 60);     // the square (red)
    CHECK(R(inside) > 200);
    CHECK(outside == 0xff000000u);                                   // beyond it: the clear colour
    CHECK(B(corner) > 200 && R(corner) < 60);                        // the second strip (blue)
    printf("  geometry shader: centre %06x, inside %06x, outside %06x, corner %06x\n", centre & 0xffffff, inside & 0xffffff, outside & 0xffffff, corner & 0xffffff);
    // Indexed, with a base vertex: of four points, indices { 1, 0 } with base vertex 1 draw points 2 (green, right)
    // and 1 (white, top); points 0 (left) and 3 (bottom) stay undrawn.
    color_vertex pts[4] = { { -0.5f, 0, 0, 1, 0, 0, 1 }, { 0, 0.6f, 0, 1, 1, 1, 1 }, { 0.5f, 0, 0, 0, 1, 0, 1 }, { 0, -0.6f, 0, 1, 1, 0, 1 } };
    WORD idx[2] = { 1, 0 };
    ID3D11Buffer *vb2 = buffer(D3D11_BIND_VERTEX_BUFFER, pts, sizeof pts), *ib = buffer(D3D11_BIND_INDEX_BUFFER, idx, sizeof idx);
    float small[4] = { 0.1f, 0, 0, 0 };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, small, 0, 0);
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb2, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 2, 0, 1);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    readback(t, px, 4);
    UINT left = px[32 * 64 + 16], mid = px[(32 - 19) * 64 + 32], right = px[32 * 64 + 48], low = px[(32 + 19) * 64 + 32];
    CHECK(left == 0xff000000u);                                      // vertex 0: not drawn
    CHECK(R(mid) > 200 && G(mid) > 200 && B(mid) > 200);             // index 0 + base 1: the white point
    CHECK(G(right) > 200 && R(right) < 60);                          // index 1 + base 1: the green point
    CHECK(low == 0xff000000u);                                       // vertex 3: not drawn
    printf("  geometry shader, indexed: left %06x, top %06x, right %06x, bottom %06x\n", left & 0xffffff, mid & 0xffffff, right & 0xffffff, low & 0xffffff);
    ID3D11Buffer_Release(vb2); ID3D11Buffer_Release(ib);
    ID3D11Buffer_Release(vb); ID3D11Buffer_Release(cb);
    if (gs) ID3D11GeometryShader_Release(gs);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(t);
}

// Stream output with a vertex shader's code (as Unity skins meshes): each vertex's color and the x and y of its
// position go to a buffer, a second draw appending after the first.
static void test_stream_output(void) {
    D3D11_SO_DECLARATION_ENTRY decl[] = { { 0, "COLOR", 0, 0, 4, 0 }, { 0, "SV_Position", 0, 0, 2, 0 } };
    UINT stride = 24;
    ID3D11GeometryShader *gs = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateGeometryShaderWithStreamOutput(dev, color_vs, sizeof color_vs, decl, 2, &stride, 1, D3D11_SO_NO_RASTERIZED_STREAM, NULL, &gs)));
    color_vertex v[3] = { { 1, 2, 0, 0.5f, 0.25f, 0, 1 }, { 3, 4, 0, 1, 1, 1, 1 }, { 5, 6, 0, 0, 0, 0, 0 } };
    ID3D11Buffer *vb = buffer(D3D11_BIND_VERTEX_BUFFER, v, sizeof v), *so = buffer(D3D11_BIND_STREAM_OUTPUT, NULL, 6 * 24);
    UINT vstride = sizeof v[0], offset = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, color_layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &vstride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(ctx, color_vs_obj, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_SOSetTargets(ctx, 1, &so, &offset);
    ID3D11DeviceContext_Draw(ctx, 2, 0);                  // vertices 0 and 1
    UINT append = (UINT)-1;
    ID3D11DeviceContext_SOSetTargets(ctx, 1, &so, &append);
    ID3D11DeviceContext_Draw(ctx, 1, 2);                  // vertex 2, after them
    ID3D11Buffer *bound = NULL;
    ID3D11DeviceContext_SOGetTargets(ctx, 1, &bound);
    CHECK(bound == so);
    if (bound) ID3D11Buffer_Release(bound);
    ID3D11DeviceContext_SOSetTargets(ctx, 0, NULL, NULL);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    D3D11_BUFFER_DESC sd = { 6 * 24, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
    ID3D11Buffer *staging = NULL;
    ID3D11Device_CreateBuffer(dev, &sd, NULL, &staging);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)so);
    float out[6 * 6] = { 0 };
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        memcpy(out, m.pData, sizeof out);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    for (int i = 0; i < 3; i++) {
        const float *o = &out[i * 6], *want = &v[i].r;
        CHECK(o[0] == want[0] && o[1] == want[1] && o[2] == want[2] && o[3] == want[3] && o[4] == v[i].x && o[5] == v[i].y);
    }
    CHECK(out[18] == 0 && out[35] == 0);                  // nothing past the three vertices
    printf("  stream output: (%g %g %g %g, %g %g) (%g ... %g) (%g ... %g)\n", out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[11], out[12], out[17]);
    ID3D11Buffer_Release(staging); ID3D11Buffer_Release(so); ID3D11Buffer_Release(vb);
    if (gs) ID3D11GeometryShader_Release(gs);
}

int main(int argc, char **argv) {
    D3D_FEATURE_LEVEL level;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &level, &ctx))) { printf("draw: no device -> FAILED\n"); return 1; }
    color_pipeline();
    if (argc > 1 && !strcmp(argv[1], "--interactive")) { interactive(); return 0; }
    test_triangle();
    test_textured();
    test_math();
    test_depth_and_blend();
    test_generate_mips();
    test_geometry_shader();
    test_stream_output();
    printf("draw: triangle, textured quad, shader math, depth, blending, per-draw constants, mips, geometry shader, stream output: %d failed checks -> %s\n", failures, failures ? "FAILED" : "ok");
    return failures;
}
