// D3D11On12 as SteamVR's vrclient uses it for a Direct3D 12 game: the game renders an eye texture with Direct3D 12 and
// submits it; vrclient wraps it in a Direct3D 11 device on the game's queue and copies it with Direct3D 11. Checks the
// ordering both ways without waiting in between: Direct3D 11 sees what Direct3D 12 work submitted before it wrote,
// Direct3D 12 work submitted after the Flush sees what Direct3D 11 wrote, and the wrapped resource's output state is
// what Direct3D 12's state tracking expects afterwards.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <d3d11on12.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED: %s: %#lx\n", #x, (unsigned long)hr_); return 1; } } while (0)

int main(void) {
    int bad = 0;
    ID3D12Device *dev12;
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev12));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *queue;
    CHECK(ID3D12Device_CreateCommandQueue(dev12, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));

    // the eye texture: 64x64 RGBA8, a render target the game clears to red (a large texture so the GPU work takes time)
    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT }, rh = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2048, 2048, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    ID3D12Resource *eye;
    CHECK(ID3D12Device_CreateCommittedResource(dev12, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                               &IID_ID3D12Resource, (void **)&eye));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev12, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    ID3D12Device_CreateRenderTargetView(dev12, eye, NULL, rtv);
    float red[4] = { 1, 0, 0, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, red, 0, NULL);
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ eye, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);  // (not waited for)

    // vrclient: a Direct3D 11 device on the game's queue, the eye texture wrapped
    ID3D11Device *dev11;
    ID3D11DeviceContext *ctx;
    D3D_FEATURE_LEVEL level = 0;
    CHECK(D3D11On12CreateDevice((IUnknown *)dev12, 0, NULL, 0, (IUnknown **)&queue, 1, 0, &dev11, &ctx, &level));
    ID3D11On12Device *on12;
    CHECK(ID3D11Device_QueryInterface(dev11, &IID_ID3D11On12Device, (void **)&on12));
    ID3D11On12Device1 *on12_1;
    CHECK(ID3D11Device_QueryInterface(dev11, &IID_ID3D11On12Device1, (void **)&on12_1));
    ID3D12Device *back = NULL;
    CHECK(ID3D11On12Device1_GetD3D12Device(on12_1, &IID_ID3D12Device, &back));
    if (back != dev12) { printf("GetD3D12Device: another device\n"); bad++; }
    D3D11_RESOURCE_FLAGS flags = { D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0, 0 };
    ID3D11Texture2D *wrapped;
    CHECK(ID3D11On12Device_CreateWrappedResource(on12, (IUnknown *)eye, &flags, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &IID_ID3D11Texture2D, (void **)&wrapped));
    D3D11_TEXTURE2D_DESC wd;
    ID3D11Texture2D_GetDesc(wrapped, &wd);
    if (wd.Width != 2048 || wd.Format != DXGI_FORMAT_R8G8B8A8_UNORM || !(wd.BindFlags & D3D11_BIND_RENDER_TARGET)) { printf("wrapped desc\n"); bad++; }

    // copy what the game drew (red) to a staging texture, then draw green over the corner with Direct3D 11
    ID3D11On12Device_AcquireWrappedResources(on12, (ID3D11Resource **)&wrapped, 1);
    D3D11_TEXTURE2D_DESC sd = wd;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ID3D11Texture2D *staging;
    CHECK(ID3D11Device_CreateTexture2D(dev11, &sd, NULL, &staging));
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)wrapped);
    ID3D11RenderTargetView *rtv11;
    CHECK(ID3D11Device_CreateRenderTargetView(dev11, (ID3D11Resource *)wrapped, NULL, &rtv11));
    float green[4] = { 0, 1, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv11, green);
    ID3D11On12Device_ReleaseWrappedResources(on12, (ID3D11Resource **)&wrapped, 1);
    ID3D11DeviceContext_Flush(ctx);

    // the game's next frame: its barrier starts from the output state, and a copy reads what Direct3D 11 drew
    ID3D12Resource *rb;
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256 * 4, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, 0 };
    CHECK(ID3D12Device_CreateCommittedResource(dev12, &rh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb));
    ID3D12CommandAllocator *alloc2;
    CHECK(ID3D12Device_CreateCommandAllocator(dev12, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc2));
    CHECK(ID3D12GraphicsCommandList_Reset(list, alloc2, NULL));
    b.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ eye, 0, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { eye, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_BOX box = { 0, 0, 0, 4, 4, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, &box);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12Fence *fence;
    CHECK(ID3D12Device_CreateFence(dev12, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    ID3D12CommandQueue_Signal(queue, fence, 1);
    ID3D12Fence_SetEventOnCompletion(fence, 1, NULL);

    D3D11_MAPPED_SUBRESOURCE m;
    CHECK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m));
    uint32_t seen11 = ((uint32_t *)m.pData)[0], seen11_far = ((uint32_t *)((uint8_t *)m.pData + 2047 * m.RowPitch))[2047];
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
    uint32_t *p;
    CHECK(ID3D12Resource_Map(rb, 0, NULL, (void **)&p));
    uint32_t seen12 = p[0];
    ID3D12Resource_Unmap(rb, 0, NULL);
    if (seen11 != 0xff0000ff || seen11_far != 0xff0000ff) { printf("Direct3D 11 copy saw %08x / %08x, want the game's red ff0000ff\n", seen11, seen11_far); bad++; }
    if (seen12 != 0xff00ff00) { printf("Direct3D 12 copy saw %08x, want Direct3D 11's green ff00ff00\n", seen12); bad++; }
    printf("d3d11on12: device on the game's queue (level %#x), wrapped 2048x2048 eye texture, D3D11 read the D3D12 clear (%08x), "
           "D3D12 read the D3D11 clear (%08x) from the output state: %s\n", level, seen11, seen12, bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
