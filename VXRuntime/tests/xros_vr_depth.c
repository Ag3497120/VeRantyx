// xros_vr_depth.c: a Direct3D 12 scene with a depth buffer, for the immersive presenter on Vision Pro (VISIONOS_PORT.md,
// "VR presentation"): a floor out to 50 m and cubes at 1.5, 3, 6, 12 and 24 m, rendered through a graphics pipeline
// from DXIL (tests/shaders12/passcolor_vs, passcolor_ps) into a 1280x720 swap chain with a D32_FLOAT depth buffer
// (standard depth, LESS), presented with sync 1 for `seconds` (argument 1, default 60). The cubes turn slowly; the
// camera does not move (in the headset, the head looks around in the frame). Its projection is the presenter's
// default without FSR2's constants: tan(vertical FOV/2) 0.7, the aspect's tan_x, near 0.1, far 100 (VXRT_VR_TAN_Y,
// VXRT_VR_NEAR, VXRT_VR_FAR), so the presenter's warp puts each pixel at its true distance: the cubes stand at their
// distances in both eyes. Vertices are projected here (clip space, w = 1), so the vertex shader only passes them on.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED: %s: %#lx\n", #x, (unsigned long)hr_); return 1; } } while (0)
enum { W = 1280, H = 720, MAXV = 24000 };
static const float TAN_Y = 0.7f, NEAR_ = 0.1f, FAR_ = 100.0f;
typedef struct { float x, y, z, r, g, b, a; } vtx;
static vtx g_v[MAXV];
static int g_n;

static int load(const char *path, void *buf, size_t cap, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("xros vr depth: cannot open %s\n", path); return 1; }
    *size = fread(buf, 1, cap, f);
    fclose(f);
    return *size == 0;
}
// a view-space point (x right, y up, z forward, metres) projected: standard depth f/(f-n) - f n / ((f-n) z)
static int project(const float p[3], vtx *o) {
    if (p[2] < NEAR_) return 0;
    float tx = TAN_Y * W / H;
    o->x = p[0] / (p[2] * tx);
    o->y = p[1] / (p[2] * TAN_Y);
    o->z = FAR_ / (FAR_ - NEAR_) - FAR_ * NEAR_ / ((FAR_ - NEAR_) * p[2]);
    return 1;
}
static void tri(const float a[3], const float b[3], const float c[3], float r, float g, float bl) {
    if (g_n + 3 > MAXV) return;
    vtx v[3];
    if (!project(a, &v[0]) || !project(b, &v[1]) || !project(c, &v[2])) return;
    for (int k = 0; k < 3; k++) { v[k].r = r; v[k].g = g; v[k].b = bl; v[k].a = 1; g_v[g_n++] = v[k]; }
}
static void quad(const float a[3], const float b[3], const float c[3], const float d[3], float r, float g, float bl) {
    tri(a, b, c, r, g, bl); tri(a, c, d, r, g, bl);
}
static void cube(float cx, float cy, float cz, float s, float ang, float r, float g, float b) {
    float p[8][3];
    float ca = cosf(ang), sa = sinf(ang);
    for (int k = 0; k < 8; k++) {
        float x = (k & 1 ? s : -s), y = (k & 2 ? s : -s), z = (k & 4 ? s : -s);
        p[k][0] = cx + x * ca + z * sa; p[k][1] = cy + y; p[k][2] = cz - x * sa + z * ca;
    }
    static const int f[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
    static const float shade[6] = { 1.0f, 0.55f, 0.7f, 0.9f, 0.8f, 0.65f };
    for (int i = 0; i < 6; i++) quad(p[f[i][0]], p[f[i][1]], p[f[i][2]], p[f[i][3]], r * shade[i], g * shade[i], b * shade[i]);
}
static void scene(double t) {
    g_n = 0;
    // the floor: 1 m tiles in a checkerboard, 1.6 m below the eyes, 20 m to each side and out to 50 m
    for (int z = 0; z < 50; z++) for (int x = -20; x < 20; x++) {
        float a[3] = { (float)x, -1.6f, (float)z + 0.5f }, b[3] = { (float)x + 1, -1.6f, (float)z + 0.5f },
              c[3] = { (float)x + 1, -1.6f, (float)z + 1.5f }, d[3] = { (float)x, -1.6f, (float)z + 1.5f };
        float k = ((x + z) & 1) ? 0.35f : 0.22f;
        quad(a, b, c, d, k, k, k * 1.1f);
    }
    static const float dist[5] = { 1.5f, 3, 6, 12, 24 };
    static const float col[5][3] = { { 1, 0.3f, 0.2f }, { 1, 0.8f, 0.2f }, { 0.3f, 1, 0.3f }, { 0.3f, 0.6f, 1 }, { 0.8f, 0.4f, 1 } };
    for (int i = 0; i < 5; i++) {
        float z = dist[i], s = 0.15f * z / 1.5f > 1.2f ? 1.2f : 0.15f * z / 1.5f;
        cube((i - 2) * 0.35f * z, -0.2f, z, s, (float)(t * 0.5 + i), col[i][0], col[i][1], col[i][2]);
    }
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 60;
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "vxrt_xros_vr_depth";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "vxrt_xros_vr_depth", "VX Runtime depth", WS_POPUP | WS_VISIBLE, 0, 0, W, H, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("xros vr depth: no window -> FAILED\n"); return 1; }
    static unsigned char vs[65536], ps[65536];
    size_t vs_size, ps_size;
    if (load("tests/shaders12/passcolor_vs.dxil", vs, sizeof vs, &vs_size) || load("tests/shaders12/passcolor_ps.dxil", ps, sizeof ps, &ps_size)) return 1;
    ID3D12Device *dev;
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *queue;
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    IDXGIFactory4 *factory;
    CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory));
    DXGI_SWAP_CHAIN_DESC1 sd = { W, H, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                                 DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED, 0 };
    IDXGISwapChain1 *sc1;
    CHECK(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc1));
    IDXGISwapChain3 *sc;
    CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));

    D3D12_ROOT_SIGNATURE_DESC rsd = { 0, NULL, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob *blob, *errs;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errs));
    ID3D12RootSignature *rs;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    D3D12_INPUT_ELEMENT_DESC il[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs;
    pd.VS = (D3D12_SHADER_BYTECODE){ vs, vs_size };
    pd.PS = (D3D12_SHADER_BYTECODE){ ps, ps_size };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFF;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.InputLayout = (D3D12_INPUT_LAYOUT_DESC){ il, 2 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
    ID3D12DescriptorHeap *rtvs, *dsvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; hd.NumDescriptors = 1;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsvs));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsvs);
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource *back[2];
    for (UINT i = 0; i < 2; i++) {
        CHECK(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&back[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = { rtv0.ptr + i * inc };
        ID3D12Device_CreateRenderTargetView(dev, back[i], NULL, h);
    }
    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT }, uh = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H, 1, 1, DXGI_FORMAT_D32_FLOAT, { 1, 0 },
                               D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL };
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_D32_FLOAT }; cv.DepthStencil.Depth = 1;
    ID3D12Resource *depth;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource, (void **)&depth));
    ID3D12Device_CreateDepthStencilView(dev, depth, NULL, dsv);
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, sizeof g_v, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, 0 };
    ID3D12Resource *vb;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&vb));
    void *vmap;
    CHECK(ID3D12Resource_Map(vb, 0, NULL, &vmap));
    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_Close(list);
    ID3D12Fence *fence; HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL); UINT64 fv = 0;
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));

    LARGE_INTEGER freq, t0, t;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    UINT frames = 0; int verts = 0;
    for (;;) {
        QueryPerformanceCounter(&t);
        double s = (double)(t.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        if (s >= seconds) break;
        scene(s);
        memcpy(vmap, g_v, (size_t)g_n * sizeof *g_v);
        verts = g_n;
        UINT i = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, pso);
        D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ back[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
        D3D12_CPU_DESCRIPTOR_HANDLE rh = { rtv0.ptr + i * inc };
        float sky[4] = { 0.25f, 0.35f, 0.55f, 1 };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rh, sky, 0, NULL);
        ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rh, FALSE, &dsv);
        D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
        D3D12_RECT sr = { 0, 0, W, H };
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sr);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), (UINT)(g_n * sizeof *g_v), sizeof *g_v };
        ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
        ID3D12GraphicsCommandList_DrawInstanced(list, (UINT)g_n, 1, 0, 0);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        CHECK(IDXGISwapChain3_Present(sc, 1, 0));
        ID3D12CommandQueue_Signal(queue, fence, ++fv);
        if (ID3D12Fence_GetCompletedValue(fence) < fv) { ID3D12Fence_SetEventOnCompletion(fence, fv, ev); WaitForSingleObject(ev, 5000); }
        frames++;
        if (frames == 1 || frames % 600 == 0) printf("xros vr depth: frame %u, %d vertices, %.1f s\n", frames, verts, s);
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
    }
    printf("xros vr depth: %u frames in %.1f s (%.1f a second), %d vertices a frame -> %s\n", frames, seconds, frames / seconds, verts,
           frames > seconds * 10 && verts > 1000 ? "ok" : "FAILED");
    return 0;
}
