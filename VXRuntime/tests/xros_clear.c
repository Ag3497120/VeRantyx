// A Direct3D 12 swap chain one can see (the visionOS app's first picture, VISIONOS_PORT.md): a 1280x720 window, a
// flip-discard swap chain on the queue, each frame cleared to a colour that moves through the hues, presented with
// sync 1 for `seconds` (argument 1, default 10); the last frame read back. No shaders: only the queue, the swap chain
// and the presentation path.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS                 // (methods returning structures, as MSVC calls them)
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED: %s: %#lx\n", #x, (unsigned long)hr_); return 1; } } while (0)

static void wait_queue(ID3D12Device *dev, ID3D12CommandQueue *q) {
    static ID3D12Fence *f; static UINT64 v; static HANDLE e;
    if (!f) { ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&f); e = CreateEventA(NULL, FALSE, FALSE, NULL); }
    ID3D12CommandQueue_Signal(q, f, ++v);
    if (ID3D12Fence_GetCompletedValue(f) < v) { ID3D12Fence_SetEventOnCompletion(f, v, e); WaitForSingleObject(e, 5000); }
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 10;
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "vxrt_xros_clear";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "vxrt_xros_clear", "VX Runtime clear", WS_POPUP | WS_VISIBLE, 0, 0, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("xros clear: no window -> FAILED\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    ID3D12Device *dev;
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *queue;
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    IDXGIFactory4 *factory;
    CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory));
    DXGI_SWAP_CHAIN_DESC1 sd = { 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                                 DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, 0 };
    IDXGISwapChain1 *sc1;
    CHECK(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc1));
    IDXGISwapChain3 *sc;
    CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
    ID3D12DescriptorHeap *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource *back[2];
    for (UINT i = 0; i < 2; i++) {
        CHECK(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&back[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = { rtv0.ptr + i * inc };
        ID3D12Device_CreateRenderTargetView(dev, back[i], NULL, h);
    }
    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_Close(list);
    LARGE_INTEGER freq, t0, t;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    UINT frames = 0;
    float c[4] = { 0, 0, 0, 1 };
    for (;;) {
        QueryPerformanceCounter(&t);
        double s = (double)(t.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (s >= seconds) break;
        double h = fmod(s * 0.25, 1.0) * 6;                      // (one turn of the hues every 4 s)
        double x = 1 - fabs(fmod(h, 2) - 1);
        int k = (int)h;
        c[0] = (float)(k == 0 || k == 5 ? 1 : k == 1 || k == 4 ? x : 0);
        c[1] = (float)(k == 1 || k == 2 ? 1 : k == 0 || k == 3 ? x : 0);
        c[2] = (float)(k == 3 || k == 4 ? 1 : k == 2 || k == 5 ? x : 0);
        UINT i = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
        D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ back[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rh = { rtv0.ptr + i * inc };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rh, c, 0, NULL);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        CHECK(IDXGISwapChain3_Present(sc, 1, 0));
        wait_queue(dev, queue);
        frames++;
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
    }
    UINT presents = 0;
    IDXGISwapChain3_GetLastPresentCount(sc, &presents);
    printf("xros clear: %u frames in %.1f s (%.1f a second), %u presents, last colour %.2f %.2f %.2f -> %s\n", frames, seconds,
           frames / seconds, presents, c[0], c[1], c[2], frames > seconds * 20 && presents == frames ? "ok" : "FAILED");
    return 0;
}
