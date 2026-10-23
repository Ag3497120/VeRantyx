// What a frame of a Direct3D 12 game costs the CPU here: a shader-visible heap of game size (Unreal Engine keeps
// 500 000 to 1 000 000 descriptors), a thousand textures in it, 40 render passes of 25 textured draws and 20 compute
// dispatches a frame, all with fxc's shaders. Prints milliseconds per frame (recording, submission and the GPU's
// work, waited for); not part of `make check` (`make d3d12-bench`).
//   d3d12_bench.exe [descriptors] [frames]
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED: %s: %#lx\n", #x, (unsigned long)hr_); return 1; } } while (0)

static int load(const char *name, D3D12_SHADER_BYTECODE *bc) {
    static unsigned char store[4][65536];
    static int used;
    char path[128];
    snprintf(path, sizeof path, "tests/shaders/%s.dxbc", name);
    FILE *f = fopen(path, "rb");
    if (!f || used == 4) { printf("%s not found (run from VXRuntime)\n", path); return -1; }
    bc->pShaderBytecode = store[used];
    bc->BytecodeLength = fread(store[used++], 1, sizeof store[0], f);
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    UINT descriptors = argc > 1 ? (UINT)atoi(argv[1]) : 500000, frames = argc > 2 ? (UINT)atoi(argv[2]) : 20;
    enum { TEXTURES = 1024, PASSES = 40, DRAWS = 25, DISPATCHES = 20 };
    ID3D12Device *dev;
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *queue;
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    CHECK(ID3D12GraphicsCommandList_Close(list));

    D3D12_SHADER_BYTECODE vs, ps, cs;
    if (load("tex_vs", &vs) || load("tex_ps", &ps) || load("sum_cs", &cs)) return 1;
    D3D12_DESCRIPTOR_RANGE srv_range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_DESCRIPTOR_RANGE uav_range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[6];
    memset(params, 0, sizeof params);
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = (D3D12_ROOT_CONSTANTS){ 0, 0, 4 };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = (D3D12_ROOT_CONSTANTS){ 0, 0, 4 };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = (D3D12_ROOT_DESCRIPTOR_TABLE){ 1, &srv_range };
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = { D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                          D3D12_TEXTURE_ADDRESS_MODE_WRAP, 0, 1, D3D12_COMPARISON_FUNC_NEVER,
                                          D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, 0, 1000, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob *blob, *errors;
    ID3D12RootSignature *rs, *crs;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable = (D3D12_ROOT_DESCRIPTOR_TABLE){ 1, &uav_range };
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[5].Constants = (D3D12_ROOT_CONSTANTS){ 0, 0, 2 };
    D3D12_ROOT_SIGNATURE_DESC crsd = { 3, params + 3, 0, NULL, 0 };
    CHECK(D3D12SerializeRootSignature(&crsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&crs));
    D3D12_INPUT_ELEMENT_DESC il[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs; pd.VS = vs; pd.PS = ps;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFF;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.InputLayout = (D3D12_INPUT_LAYOUT_DESC){ il, 2 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso, *cpso;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { crs, cs };
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&cpso));

    // the heap, the textures and their views; two render targets; buffers
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, descriptors, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap, *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    hd = (D3D12_DESCRIPTOR_HEAP_DESC){ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT rinc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT }, uh = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, 0 };
    UINT stride = descriptors / TEXTURES;                          // (spread over the heap, as a game's are)
    for (UINT i = 0; i < TEXTURES; i++) {
        ID3D12Resource *t;
        CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void **)&t));
        D3D12_CPU_DESCRIPTOR_HANDLE h = { cpu0.ptr + (SIZE_T)i * stride * inc };
        ID3D12Device_CreateShaderResourceView(dev, t, NULL, h);
    }
    ID3D12Resource *rt[2];
    td.Width = td.Height = 256;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (int i = 0; i < 2; i++) {
        CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = { ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs).ptr + i * rinc };
        ID3D12Device_CreateRenderTargetView(dev, rt[i], NULL, h);
    }
    D3D12_RESOURCE_DESC bd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 65536, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, 0 };
    ID3D12Resource *vb, *input, *output;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&vb));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &uh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&input));
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&output));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = { DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER };
    ud.Buffer.NumElements = 64;
    ud.Buffer.StructureByteStride = 4;
    D3D12_CPU_DESCRIPTOR_HANDLE uav = { cpu0.ptr + (SIZE_T)(descriptors - 1) * inc };
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &ud, uav);
    float quad[6][4] = { { 0, 0, 0, 1 }, { 0, 1, 0, 0 }, { 1, 1, 1, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1, 0 }, { 1, 0, 1, 1 } };
    void *p;
    CHECK(ID3D12Resource_Map(vb, 0, NULL, &p)); memcpy(p, quad, sizeof quad); ID3D12Resource_Unmap(vb, 0, NULL);

    ID3D12Fence *fence;
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    LARGE_INTEGER freq, t0, t1, r0, r1;
    QueryPerformanceFrequency(&freq);
    double record_ms = 0, total_ms = 0, best = 1e9;
    for (UINT f = 0; f <= frames; f++) {
        QueryPerformanceCounter(&t0);
        CHECK(ID3D12CommandAllocator_Reset(alloc));
        CHECK(ID3D12GraphicsCommandList_Reset(list, alloc, NULL));
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
        D3D12_VIEWPORT vp = { 0, 0, 256, 256, 0, 1 };
        D3D12_RECT sc = { 0, 0, 256, 256 };
        D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof quad, sizeof quad[0] };
        for (UINT pass = 0; pass < PASSES; pass++) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = { ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs).ptr + (pass & 1) * rinc };
            ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &h, FALSE, NULL);
            ID3D12GraphicsCommandList_SetPipelineState(list, pso);
            ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
            ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
            ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
            for (UINT d = 0; d < DRAWS; d++) {
                float transform[4] = { -1 + (d % 5) * 0.4f, -1 + (d / 5) * 0.4f, 0.4f, 0.4f }, tint[4] = { 1, 1, 1, 1 };
                ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, transform, 0);
                ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 1, 4, tint, 0);
                D3D12_GPU_DESCRIPTOR_HANDLE table = { gpu0.ptr + (UINT64)((pass * DRAWS + d) % TEXTURES) * stride * inc };
                ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, table);
                ID3D12GraphicsCommandList_DrawInstanced(list, 6, 1, 0, 0);
            }
            if (pass % 2 == 1) {
                for (UINT k = 0; k < DISPATCHES / (PASSES / 2); k++) {
                    ID3D12GraphicsCommandList_SetPipelineState(list, cpso);
                    ID3D12GraphicsCommandList_SetComputeRootSignature(list, crs);
                    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(list, 0, ID3D12Resource_GetGPUVirtualAddress(input));
                    D3D12_GPU_DESCRIPTOR_HANDLE table = { gpu0.ptr + (UINT64)(descriptors - 1) * inc };
                    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, table);
                    UINT constants[2] = { 0x3f800000u, 64 };
                    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 2, 2, constants, 0);
                    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
                }
            }
        }
        CHECK(ID3D12GraphicsCommandList_Close(list));
        QueryPerformanceCounter(&r1);
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        ID3D12CommandQueue_Signal(queue, fence, f + 1);
        ID3D12Fence_SetEventOnCompletion(fence, f + 1, NULL);
        QueryPerformanceCounter(&t1);
        double rec = (double)(r1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        double all = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (f) { record_ms += rec; total_ms += all; if (all < best) best = all; }   // (frame 0 compiles pipelines)
        (void)r0;
    }
    printf("d3d12 bench: %u descriptors, %d textures, %d passes x %d draws + %d dispatches: recording %.2f ms, frame %.2f ms (best %.2f) over %u frames\n",
           descriptors, TEXTURES, PASSES, DRAWS, DISPATCHES, record_ms / frames, total_ms / frames, best, frames);
    return 0;
}
