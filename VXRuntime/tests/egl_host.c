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

struct call { IDXGIDevice3 *d; HANDLE ev; HRESULT hr; };
static DWORD WINAPI enqueue_thread(void *p) { struct call *c = p; c->hr = IDXGIDevice3_EnqueueSetEvent(c->d, c->ev); return 0; }

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
    // It returns at once, not when the GPU is done: with copies of 2 x 64 MB (tens of milliseconds of GPU work)
    // still pending, the event is not yet set when the call returns, and is set later.
    D3D11_BUFFER_DESC bigd = { 64u << 20, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0 };
    ID3D11Buffer *big[2] = { NULL, NULL };
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(dev, &bigd, NULL, &big[0])) && SUCCEEDED(ID3D11Device_CreateBuffer(dev, &bigd, NULL, &big[1])));
    if (big[0] && big[1]) {
        for (int i = 0; i < 160; i++) ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)big[i & 1], (ID3D11Resource *)big[!(i & 1)]);
        ResetEvent(ev);
        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        CHECK(SUCCEEDED(IDXGIDevice3_EnqueueSetEvent(d3, ev)));
        QueryPerformanceCounter(&t1);
        CHECK(WaitForSingleObject(ev, 0) == WAIT_TIMEOUT);
        CHECK(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0);
        CHECK((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart < 1000);
        // Called on another thread while this one has recorded copies it has not sent yet: the event is set only once
        // those copies are done on the GPU, never before. They cannot be done before they are sent, so the event is
        // still unset half a second later; this thread's Flush sends them, and then it is set.
        for (int i = 0; i < 4; i++) ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)big[i & 1], (ID3D11Resource *)big[!(i & 1)]);
        ResetEvent(ev);
        struct call call = { d3, ev, E_FAIL };
        HANDLE th = CreateThread(NULL, 0, enqueue_thread, &call, 0, NULL);
        CHECK(th && WaitForSingleObject(th, 5000) == WAIT_OBJECT_0 && SUCCEEDED(call.hr));
        if (th) CloseHandle(th);
        CHECK(WaitForSingleObject(ev, 500) == WAIT_TIMEOUT);
        ID3D11DeviceContext_Flush(ctx);
        CHECK(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0);

        // The caller closes its handle right after the call and the next event it makes gets the same handle value
        // (Windows hands out freed values again): the work's end sets the event the call was given, never the new
        // one. And no handle is left behind once it has been set.
        DWORD handles_before = 0, handles_after = 0;
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles_before));
        for (int i = 0; i < 40; i++) ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)big[i & 1], (ID3D11Resource *)big[!(i & 1)]);
        HANDLE once = CreateEventA(NULL, TRUE, FALSE, NULL);
        CHECK(SUCCEEDED(IDXGIDevice3_EnqueueSetEvent(d3, once)));
        CloseHandle(once);
        HANDLE reused = CreateEventA(NULL, TRUE, FALSE, NULL);
        CHECK(reused == once);                             // (what this check needs: the value was handed out again)
        ResetEvent(ev);
        CHECK(SUCCEEDED(IDXGIDevice3_EnqueueSetEvent(d3, ev)));   // after the first: everything before it is done
        CHECK(WaitForSingleObject(ev, 10000) == WAIT_OBJECT_0);
        Sleep(200);
        CHECK(WaitForSingleObject(reused, 0) == WAIT_TIMEOUT);
        CloseHandle(reused);
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &handles_after) && handles_after == handles_before);
    }
    if (big[0]) ID3D11Buffer_Release(big[0]);
    if (big[1]) ID3D11Buffer_Release(big[1]);
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
