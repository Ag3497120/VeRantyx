// The Direct3D 11 part of tests/recomp/suite.cpp (built for MinGW's headers, linked into the same program): a device
// without a window, a 64x64 render target cleared and read back, then a triangle drawn with tests/shaders' color
// shaders and two pixels read back (inside: the vertex colors' blend; outside: the clear color).
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include "../shaders/color_vs.h"
#include "../shaders/color_ps.h"

static UINT read_pixel(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src, UINT x, UINT y) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(src, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *staging = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &staging))) return 0xdeadbeef;
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)src);
    D3D11_MAPPED_SUBRESOURCE m;
    UINT v = 0xdeadbeef;
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
        v = *(UINT *)((BYTE *)m.pData + y * m.RowPitch + x * 4);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    }
    ID3D11Texture2D_Release(staging);
    return v;
}
static char *put_hex(char *p, UINT v) { *p++ = '0'; *p++ = 'x'; for (int s = 28; s >= 0; s -= 4) *p++ = "0123456789abcdef"[v >> s & 15]; return p; }
static char *put(char *p, const char *t) { while (*t) *p++ = *t++; return p; }

int d3d_test(char *out, int cap) {
    (void)cap;
    char *p = put(out, "d3d11: ");
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, got = 0;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &want, 1, D3D11_SDK_VERSION, &dev, &got, &ctx))) {
        put(p, "no device"); return 1;
    }
    D3D11_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Texture2D *tex = NULL; ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv);
    const float clear[4] = { 0.0f, 0.5f, 1.0f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
    UINT cleared = read_pixel(dev, ctx, tex, 3, 60);

    ID3D11VertexShader *vs = NULL; ID3D11PixelShader *ps = NULL; ID3D11InputLayout *layout = NULL; ID3D11Buffer *vb = NULL;
    ID3D11Device_CreateVertexShader(dev, color_vs, sizeof color_vs, NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, color_ps, sizeof color_ps, NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11Device_CreateInputLayout(dev, el, 2, color_vs, sizeof color_vs, &layout);
    const float verts[] = { -0.8f, -0.8f, 0, 1, 0, 0, 1,   0.0f, 0.8f, 0, 1, 0, 0, 1,   0.8f, -0.8f, 0, 1, 0, 0, 1 };   // (all red)
    D3D11_BUFFER_DESC bd = { sizeof verts, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { verts, 0, 0 };
    ID3D11Device_CreateBuffer(dev, &bd, &init, &vb);
    UINT stride = 28, offset = 0;
    ID3D11DeviceContext_IASetInputLayout(ctx, layout);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    D3D11_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    UINT inside = read_pixel(dev, ctx, tex, 32, 40), outside = read_pixel(dev, ctx, tex, 2, 2);
    p = put(p, "clear="); p = put_hex(p, cleared); p = put(p, " inside="); p = put_hex(p, inside); p = put(p, " outside="); p = put_hex(p, outside);
    *p = 0;
    ID3D11Buffer_Release(vb); ID3D11InputLayout_Release(layout); ID3D11PixelShader_Release(ps); ID3D11VertexShader_Release(vs);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(tex); ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev);
    return !(cleared == 0xffff8000 && inside == 0xff0000ff && outside == 0xffff8000);
}
