// What an EGL implementation on Direct3D 11 (ANGLE, as Chromium and Steam's web helper ship it) asks of the system
// before it makes a display: the screen's DC as the native display (WindowFromDC gives the desktop window), the
// device's newer IDXGIDevice interfaces, and the adapter's driver version through CheckInterfaceSupport.
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <stdio.h>
static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

static const GUID IID_ID3D10Device_ = { 0x9b7e4c0f, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };

int main(void) {
    // The screen's DC is the desktop window's; a display DC made with CreateDC belongs to no window.
    HDC screen = GetDC(NULL);
    CHECK(screen != NULL && WindowFromDC(screen) == GetDesktopWindow());
    CHECK(GetDeviceCaps(screen, BITSPIXEL) >= 24);
    ReleaseDC(NULL, screen);
    HDC desk = GetDC(GetDesktopWindow());
    CHECK(desk != NULL && WindowFromDC(desk) == GetDesktopWindow());
    ReleaseDC(GetDesktopWindow(), desk);
    HDC display = CreateDCA("DISPLAY", NULL, NULL, NULL);
    CHECK(display != NULL && WindowFromDC(display) == NULL && GetDeviceCaps(display, HORZRES) > 0);
    DeleteDC(display);
    HDC mem = CreateCompatibleDC(NULL);
    CHECK(mem != NULL && WindowFromDC(mem) == NULL);
    DeleteDC(mem);

    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 }, got = 0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2, D3D11_SDK_VERSION, &dev, &got, &ctx);
    CHECK(SUCCEEDED(hr) && dev && ctx);
    if (FAILED(hr)) { printf("egl_host: no device %#lx -> FAILED\n", hr); return 1; }

    // IDXGIDevice1..4 are the same object: the same adapter, and offered resources come back with their contents.
    IDXGIDevice1 *d1 = NULL; IDXGIDevice2 *d2 = NULL; IDXGIDevice3 *d3 = NULL; IDXGIDevice4 *d4 = NULL;
    CHECK(SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice1, (void **)&d1)));
    CHECK(SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice2, (void **)&d2)));
    CHECK(SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice3, (void **)&d3)));
    CHECK(SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice4, (void **)&d4)));
    CHECK((void *)d1 == (void *)d2 && (void *)d2 == (void *)d3 && (void *)d3 == (void *)d4);
    IDXGIAdapter *adapter = NULL;
    CHECK(SUCCEEDED(IDXGIDevice4_GetAdapter(d4, &adapter)) && adapter);

    D3D11_BUFFER_DESC bd = { 256, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
    UINT seed[64]; for (int i = 0; i < 64; i++) seed[i] = 0x5eed0000u + i;
    D3D11_SUBRESOURCE_DATA init = { seed, 0, 0 };
    ID3D11Buffer *buf = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(dev, &bd, &init, &buf)));
    IDXGIResource *res = NULL;
    CHECK(SUCCEEDED(ID3D11Buffer_QueryInterface(buf, &IID_IDXGIResource, (void **)&res)));
    CHECK(SUCCEEDED(IDXGIDevice2_OfferResources(d2, 1, &res, DXGI_OFFER_RESOURCE_PRIORITY_LOW)));
    CHECK(IDXGIDevice2_OfferResources(d2, 1, &res, (DXGI_OFFER_RESOURCE_PRIORITY)7) == E_INVALIDARG);
    BOOL discarded = TRUE;
    CHECK(SUCCEEDED(IDXGIDevice2_ReclaimResources(d2, 1, &res, &discarded)) && !discarded);
    CHECK(SUCCEEDED(IDXGIDevice4_OfferResources1(d4, 1, &res, DXGI_OFFER_RESOURCE_PRIORITY_NORMAL, 0)));
    DXGI_RECLAIM_RESOURCE_RESULTS result = DXGI_RECLAIM_RESOURCE_RESULT_DISCARDED;
    CHECK(SUCCEEDED(IDXGIDevice4_ReclaimResources1(d4, 1, &res, &result)) && result == DXGI_RECLAIM_RESOURCE_RESULT_OK);
    // The contents survived: copy to a staging buffer and read them.
    D3D11_BUFFER_DESC sd = { 256, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
    ID3D11Buffer *staging = NULL;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(dev, &sd, NULL, &staging)));
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)buf);
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m)));
    CHECK(((UINT *)m.pData)[17] == 0x5eed0011u);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);

    // EnqueueSetEvent: set once the GPU work so far has finished.
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(IDXGIDevice3_EnqueueSetEvent(d3, NULL) == E_INVALIDARG);
    CHECK(SUCCEEDED(IDXGIDevice3_EnqueueSetEvent(d3, ev)));
    CHECK(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
    IDXGIDevice3_Trim(d3);
    CloseHandle(ev);

    // The driver version: for the Direct3D 10 device interfaces only.
    LARGE_INTEGER umd = { 0 };
    CHECK(IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_IDXGIDevice, &umd) == S_OK && umd.QuadPart != 0);
    CHECK(HIWORD(umd.HighPart) >= 1);
    LARGE_INTEGER umd10 = { 0 };
    CHECK(IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_ID3D10Device_, &umd10) == S_OK && umd10.QuadPart == umd.QuadPart);
    CHECK(IDXGIAdapter_CheckInterfaceSupport(adapter, &IID_ID3D11Device, &umd) == DXGI_ERROR_UNSUPPORTED);

    printf("egl_host: driver %u.%u.%u.%u: %d failed checks -> %s\n", HIWORD(umd10.HighPart), LOWORD(umd10.HighPart),
           HIWORD(umd10.LowPart), LOWORD(umd10.LowPart), failures, failures ? "FAILED" : "ok");
    IDXGIAdapter_Release(adapter);
    ID3D11Buffer_Release(staging); IDXGIResource_Release(res); ID3D11Buffer_Release(buf);
    IDXGIDevice1_Release(d1); IDXGIDevice2_Release(d2); IDXGIDevice3_Release(d3); IDXGIDevice4_Release(d4);
    ID3D11DeviceContext_Release(ctx); ID3D11Device_Release(dev);
    return failures;
}
