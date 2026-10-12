// Milestone 9b: Direct3D 11 resources shared between processes. "shared.exe make" creates a shared texture with a
// keyed mutex (cleared to a colour on the GPU) and a shared buffer, and publishes their handles in a named
// mapping; "shared.exe open" (another process) opens both with OpenSharedResource, takes the mutex, and reads
// them back. make check runs both.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>
typedef struct { volatile LONG ready, done; HANDLE texture, buffer; } board;

int main(int argc, char **argv) {
    int make = argc > 1 && !strcmp(argv[1], "make");
    if (argc < 2) { printf("usage: shared make|open\n"); return 2; }
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "vxrt_shared_test_board");
    board *b = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof *b);
    ID3D11Device *dev; ID3D11DeviceContext *ctx;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) { printf("shared: no device -> FAILED\n"); return 1; }
    int ok = 0;
    if (make) {
        D3D11_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX };
        ID3D11Texture2D *tex; ID3D11RenderTargetView *rtv;
        ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex);
        ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv);
        IDXGIKeyedMutex *km; IDXGIResource *res;
        ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIKeyedMutex, (void **)&km);
        ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource, (void **)&res);
        IDXGIKeyedMutex_AcquireSync(km, 0, INFINITE);
        float c[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
        IDXGIKeyedMutex_ReleaseSync(km, 1);                       // for the other process
        IDXGIResource_GetSharedHandle(res, &b->texture);
        UINT words[64] = { 0 };
        for (int i = 0; i < 64; i++) words[i] = 1000 + i;
        D3D11_BUFFER_DESC bd = { sizeof words, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, D3D11_RESOURCE_MISC_SHARED, 0 };
        D3D11_SUBRESOURCE_DATA init = { words, 0, 0 };
        ID3D11Buffer *buf; IDXGIResource *bres;
        ID3D11Device_CreateBuffer(dev, &bd, &init, &buf);
        ID3D11Buffer_QueryInterface(buf, &IID_IDXGIResource, (void **)&bres);
        IDXGIResource_GetSharedHandle(bres, &b->buffer);
        InterlockedExchange(&b->ready, 1);
        for (int i = 0; i < 10000 && !b->done; i++) Sleep(1);
        ok = b->done == 1;
        IDXGIResource_Release(bres); ID3D11Buffer_Release(buf);
        IDXGIResource_Release(res); IDXGIKeyedMutex_Release(km); ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(tex);
    } else {
        for (int i = 0; i < 10000 && !b->ready; i++) Sleep(1);
        ID3D11Texture2D *tex = NULL; ID3D11Buffer *buf = NULL;
        HRESULT hr1 = b->ready ? ID3D11Device_OpenSharedResource(dev, b->texture, &IID_ID3D11Texture2D, (void **)&tex) : E_FAIL;
        HRESULT hr2 = b->ready ? ID3D11Device_OpenSharedResource(dev, b->buffer, &IID_ID3D11Buffer, (void **)&buf) : E_FAIL;
        UINT pixel = 0, word = 0;
        if (SUCCEEDED(hr1)) {
            IDXGIKeyedMutex *km;
            ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIKeyedMutex, (void **)&km);
            HRESULT acquired = IDXGIKeyedMutex_AcquireSync(km, 1, 5000);
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D_GetDesc(tex, &td);
            td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
            ID3D11Texture2D *staging;
            ID3D11Device_CreateTexture2D(dev, &td, NULL, &staging);
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
                pixel = *(UINT *)((BYTE *)m.pData + 10 * m.RowPitch + 10 * 4);
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
            }
            if (acquired == S_OK) IDXGIKeyedMutex_ReleaseSync(km, 0);
            ID3D11Texture2D_Release(staging); IDXGIKeyedMutex_Release(km); ID3D11Texture2D_Release(tex);
        }
        if (SUCCEEDED(hr2)) {
            D3D11_BUFFER_DESC bd;
            ID3D11Buffer_GetDesc(buf, &bd);
            bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0; bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; bd.MiscFlags = 0;
            ID3D11Buffer *staging;
            ID3D11Device_CreateBuffer(dev, &bd, NULL, &staging);
            ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)buf);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m))) {
                word = ((UINT *)m.pData)[7];
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
            }
            ID3D11Buffer_Release(staging); ID3D11Buffer_Release(buf);
        }
        ok = pixel == 0xffbf8040u && word == 1007;
        printf("shared resources between processes: texture %#lx (handle %p), buffer word %u (handle %p) -> %s\n",
               (unsigned long)pixel, b->texture, word, b->buffer, ok ? "ok" : "FAILED");
        InterlockedExchange(&b->done, ok ? 1 : 2);
    }
    ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev);
    UnmapViewOfFile(b); CloseHandle(map);
    return !ok;
}
