// Milestone 5b: Direct3D 11 and DXGI on Metal. A device and swap chain on a window, clears read back through a
// staging texture, uploads and copies, resizing, adapter and output information, and a few presented frames.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

// Copies a texture into a CPU-readable one and returns its pixel at (x, y) (RGBA bytes as a 32-bit value).
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

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(int argc, char **argv) {
    int interactive = argc > 1 && !strcmp(argv[1], "--interactive");
    WNDCLASSA wc = { 0, proc, 0, 0, GetModuleHandleA(NULL), NULL, NULL, NULL, NULL, "VXD3D" };
    RegisterClassA(&wc);
    RECT r = { 0, 0, 320, 240 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowA("VXD3D", "VXRuntime Direct3D 11 on Metal", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL; IDXGISwapChain *sc = NULL;
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 }, got = 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 2, D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx);
    CHECK(SUCCEEDED(hr) && dev && ctx && sc && got == D3D_FEATURE_LEVEL_11_0);
    if (FAILED(hr)) { printf("d3d11: device creation failed %#lx -> FAILED\n", hr); return 1; }

    // The adapter and its output, through the device's DXGI side.
    IDXGIDevice *dxdev = NULL; IDXGIAdapter *adapter = NULL; IDXGIOutput *output = NULL, *none = NULL;
    CHECK(SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dxdev)));
    CHECK(SUCCEEDED(IDXGIDevice_GetAdapter(dxdev, &adapter)));
    DXGI_ADAPTER_DESC ad; IDXGIAdapter_GetDesc(adapter, &ad);
    char gpu[128]; WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, gpu, sizeof gpu, NULL, NULL);
    CHECK(ad.VendorId == 0x106B && ad.DedicatedVideoMemory > 0);
    CHECK(SUCCEEDED(IDXGIAdapter_EnumOutputs(adapter, 0, &output)) && IDXGIAdapter_EnumOutputs(adapter, 1, &none) == DXGI_ERROR_NOT_FOUND);
    UINT modes = 0;
    IDXGIOutput_GetDisplayModeList(output, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &modes, NULL);
    CHECK(modes >= 1);
    IDXGIOutput_Release(output); IDXGIAdapter_Release(adapter);

    // Clear the back buffer and read it back.
    ID3D11Texture2D *back = NULL; ID3D11RenderTargetView *rtv = NULL;
    CHECK(SUCCEEDED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&back)));
    D3D11_TEXTURE2D_DESC bd; ID3D11Texture2D_GetDesc(back, &bd);
    CHECK(bd.Width == 320 && bd.Height == 240 && bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)back, NULL, &rtv)));
    float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, color);
    UINT px = read_pixel(dev, ctx, back, 10, 10);
    CHECK(px == 0xffbf8040 || px == 0xffbf7f40);   // A=255 B=191 G=128 R=64 (little endian)

    // Upload a 4x4 texture, copy part of it into another, read it back.
    UINT pixels[16];
    for (int i = 0; i < 16; i++) pixels[i] = 0xff000000u | (UINT)(i * 16);
    D3D11_TEXTURE2D_DESC td = { 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { pixels, 16, 0 };
    ID3D11Texture2D *a = NULL, *b = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, &init, &a)));
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &b)));
    D3D11_BOX box = { 1, 1, 0, 3, 3, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)b, 0, 0, 0, 0, (ID3D11Resource *)a, 0, &box);
    CHECK(read_pixel(dev, ctx, b, 0, 0) == pixels[5] && read_pixel(dev, ctx, b, 1, 1) == pixels[10]);
    UINT patch = 0xff00ff00u;
    D3D11_BOX one = { 2, 2, 0, 3, 3, 1 };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)a, 0, &one, &patch, 4, 0);
    CHECK(read_pixel(dev, ctx, a, 2, 2) == patch && read_pixel(dev, ctx, a, 1, 2) == pixels[9]);
    ID3D11Texture2D_Release(a); ID3D11Texture2D_Release(b);

    // Frames: a few presents with changing colors (more when watching).
    int frames = interactive ? 600 : 30;
    for (int f = 0; f < frames; f++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        float c[4] = { 0.5f + 0.5f * (f % 60) / 60.0f, 0.2f, 0.6f, 1.0f };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
        CHECK(SUCCEEDED(IDXGISwapChain_Present(sc, 1, 0)));
    }
    UINT presents = 0;
    IDXGISwapChain_GetLastPresentCount(sc, &presents);
    CHECK(presents == (UINT)frames);

    // Resizing: release the views, resize, the buffer has the new size.
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(back);
    CHECK(SUCCEEDED(IDXGISwapChain_ResizeBuffers(sc, 0, 400, 300, DXGI_FORMAT_UNKNOWN, 0)));
    CHECK(SUCCEEDED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&back)));
    ID3D11Texture2D_GetDesc(back, &bd);
    CHECK(bd.Width == 400 && bd.Height == 300);
    ID3D11Texture2D_Release(back);

    // Queries: an event completes once the GPU has done the work before it; timestamps come with a disjoint query.
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *event = NULL, *disjoint = NULL, *stamp = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateQuery(dev, &qd, &event)));
    qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    CHECK(SUCCEEDED(ID3D11Device_CreateQuery(dev, &qd, &disjoint)));
    qd.Query = D3D11_QUERY_TIMESTAMP;
    CHECK(SUCCEEDED(ID3D11Device_CreateQuery(dev, &qd, &stamp)));
    CHECK(ID3D11Query_GetDataSize(event) == sizeof(BOOL) && ID3D11Query_GetDataSize(disjoint) == 16);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *)disjoint);
    float grey[4] = { 0.5f, 0.5f, 0.5f, 1 };
    D3D11_TEXTURE2D_DESC od = { 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Texture2D *offtex = NULL; ID3D11RenderTargetView *off = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &od, NULL, &offtex)));
    CHECK(SUCCEEDED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)offtex, NULL, &off)));
    ID3D11DeviceContext_ClearRenderTargetView(ctx, off, grey);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)stamp);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)event);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *)disjoint);
    BOOL done = FALSE; HRESULT qr; int polls = 0;
    while ((qr = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)event, &done, sizeof done, 0)) == S_FALSE && polls < 100000) polls++;
    CHECK(qr == S_OK && done);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = { 0, TRUE };
    CHECK(ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)disjoint, &dj, sizeof dj, 0) == S_OK && dj.Frequency && !dj.Disjoint);
    UINT64 ts = 0;
    CHECK(ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)stamp, &ts, sizeof ts, 0) == S_OK && ts);
    ID3D11Query_Release(event); ID3D11Query_Release(disjoint); ID3D11Query_Release(stamp);
    ID3D11RenderTargetView_Release(off); ID3D11Texture2D_Release(offtex);

    IDXGISwapChain_Release(sc);
    ID3D11DeviceContext_Release(ctx);
    IDXGIDevice_Release(dxdev);
    ULONG left = ID3D11Device_Release(dev);
    DestroyWindow(hwnd);
    printf("d3d11: %s, clear %#x, copies, %u presents, resize, queries: %d failed checks -> %s\n", gpu, px, presents, failures, failures ? "FAILED" : "ok");
    (void)left;
    return failures;
}
