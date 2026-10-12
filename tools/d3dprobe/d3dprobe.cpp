// d3dprobe: which Direct3D runtime is answering, and does it work end to end?
// D3D11: create a device, clear a 64x64 texture to a known color, read it back.
// D3D12: create a device, a command queue and a fence, signal it, wait for it.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>

static void adapters() {
    IDXGIFactory1 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) { printf("DXGI: no factory\n"); return; }
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
        printf("DXGI adapter %u: %ls vendor=%04x device=%04x vram=%llu MB\n", i, d.Description, d.VendorId, d.DeviceId,
               (unsigned long long)d.DedicatedVideoMemory >> 20);
        a->Release();
    }
    f->Release();
}

static int d3d11() {
    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr; D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("D3D11: CreateDevice failed 0x%08lx\n", hr); return 1; }
    printf("D3D11: device ok, feature level %x\n", fl);
    D3D11_TEXTURE2D_DESC td = {}; td.Width = td.Height = 64; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *tex = nullptr, *staging = nullptr; ID3D11RenderTargetView *rtv = nullptr;
    dev->CreateTexture2D(&td, nullptr, &tex);
    dev->CreateRenderTargetView(tex, nullptr, &rtv);
    const float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    ctx->ClearRenderTargetView(rtv, color);
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    dev->CreateTexture2D(&td, nullptr, &staging);
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m = {};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) { printf("D3D11: Map failed 0x%08lx\n", hr); return 1; }
    const unsigned char *p = (const unsigned char *)m.pData;
    printf("D3D11: readback pixel = %u %u %u %u (expect 64 128 191 255)\n", p[0], p[1], p[2], p[3]);
    ctx->Unmap(staging, 0);
    return 0;
}

static int d3d12() {
    typedef HRESULT (WINAPI *CreateFn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    HMODULE h = LoadLibraryA("d3d12.dll");
    CreateFn create = h ? (CreateFn)GetProcAddress(h, "D3D12CreateDevice") : nullptr;
    if (!create) { printf("D3D12: no d3d12.dll / D3D12CreateDevice\n"); return 1; }
    ID3D12Device *dev = nullptr;
    HRESULT hr = create(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&dev);
    if (FAILED(hr)) { printf("D3D12: CreateDevice failed 0x%08lx\n", hr); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd = {}; ID3D12CommandQueue *q = nullptr;
    hr = dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&q);
    ID3D12Fence *fence = nullptr;
    if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&fence);
    if (FAILED(hr)) { printf("D3D12: queue/fence failed 0x%08lx\n", hr); return 1; }
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    q->Signal(fence, 1); fence->SetEventOnCompletion(1, ev);
    DWORD w = WaitForSingleObject(ev, 5000);
    printf("D3D12: device, queue and fence ok; fence %s\n", w == WAIT_OBJECT_0 ? "signalled" : "TIMED OUT");
    return w == WAIT_OBJECT_0 ? 0 : 1;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    adapters();
    int r = d3d11();
    r |= d3d12();
    printf("d3dprobe: %s\n", r ? "FAILED" : "ok");
    return r;
}
