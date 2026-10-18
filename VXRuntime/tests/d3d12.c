// Direct3D 12 on VXRuntime, compute: a device, a root signature serialized and created again, a shader-visible
// heap with an SRV and two UAVs, a compute pipeline from DXIL (tests/shaders12/sum_cs.dxil, run as tools/dxilrun
// runs it on Metal directly), copies from an upload heap, a dispatch, copies to readback heaps, a fence. Checks
// what the shader wrote, as tools/dxilrun does.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS                 // (methods returning structures, as MSVC calls them: a hidden result pointer)
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad;
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED: %s: %#lx\n", #x, (unsigned long)hr_); return 1; } } while (0)

static ID3D12Resource *buffer(ID3D12Device *dev, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
    ID3D12Resource *r = NULL;
    HRESULT hr = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&r);
    if (FAILED(hr)) { printf("CreateCommittedResource(buffer %llu): %#lx\n", (unsigned long long)size, (unsigned long)hr); bad++; }
    return r;
}

static int load(const char *path, unsigned char *buf, size_t cap, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("%s not found (run from VXRuntime)\n", path); return -1; }
    *size = fread(buf, 1, cap, f);
    fclose(f);
    return 0;
}

static void wait_queue(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    ID3D12Fence *fence;
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    ID3D12Fence_SetEventOnCompletion(fence, 1, ev);
    if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0) { printf("FAILED: the fence did not reach 1 in 5 s\n"); bad++; }
    ID3D12Fence_Release(fence);
    CloseHandle(ev);
}

// Drawing: a 64x64 RGBA8 target cleared to blue and a D32 depth buffer cleared to 1, one triangle drawn indexed
// (base vertex 3) through a graphics pipeline from DXIL (tri_vs, tri_ps), the pixel shader's tint from root
// constants; the target read back through a placed footprint.
static int draw_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    static unsigned char vs[65536], ps[65536];
    size_t vs_size, ps_size;
    if (load("tests/shaders12/tri_vs.dxil", vs, sizeof vs, &vs_size) || load("tests/shaders12/tri_ps.dxil", ps, sizeof ps, &ps_size)) return 1;
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS };
    param.Constants.ShaderRegister = 0;
    param.Constants.Num32BitValues = 4;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob *blob, *errs;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errs));
    ID3D12RootSignature *rs;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
    D3D12_INPUT_ELEMENT_DESC il[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs;
    pd.VS = (D3D12_SHADER_BYTECODE){ vs, vs_size };
    pd.PS = (D3D12_SHADER_BYTECODE){ ps, ps_size };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFF;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
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

    // the target, the depth buffer, their views
    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    ID3D12Resource *rt, *depth;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&depth));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtvs, *dsvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsvs));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsvs);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    ID3D12Device_CreateDepthStencilView(dev, depth, NULL, dsv);

    // vertices: three off to the side (skipped by the base vertex), then the triangle; indices 0 1 2
    float verts[6][6] = {
        { 9, 9, 0, 0, 0, 0 }, { 9, 9, 0, 0, 0, 0 }, { 9, 9, 0, 0, 0, 0 },
        { -0.5f, -0.5f, 1, 1, 1, 1 }, { 0, 0.5f, 1, 1, 1, 1 }, { 0.5f, -0.5f, 1, 1, 1, 1 },
    };
    unsigned short idx[3] = { 0, 1, 2 };
    ID3D12Resource *vb = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof verts, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ib = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof idx, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (bad) return 1;
    void *p;
    CHECK(ID3D12Resource_Map(vb, 0, NULL, &p)); memcpy(p, verts, sizeof verts); ID3D12Resource_Unmap(vb, 0, NULL);
    CHECK(ID3D12Resource_Map(ib, 0, NULL, &p)); memcpy(p, idx, sizeof idx); ID3D12Resource_Unmap(ib, 0, NULL);

    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
    float blue[4] = { 0, 0, 1, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, &dsv);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    float tint[4] = { 0.5f, 1, 0.25f, 1 };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, tint, 0);
    D3D12_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    D3D12_RECT sc = { 0, 0, 64, 64 };
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof verts, sizeof verts[0] };
    D3D12_INDEX_BUFFER_VIEW ibv = { ID3D12Resource_GetGPUVirtualAddress(ib), sizeof idx, DXGI_FORMAT_R16_UINT };
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 3, 1, 0, 3, 0);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    unsigned char *px;
    CHECK(ID3D12Resource_Map(rb, 0, NULL, (void **)&px));
    const unsigned char *center = px + 32 * 256 + 32 * 4, *corner = px;
    int hit = abs(center[0] - 128) <= 1 && center[1] == 255 && abs(center[2] - 64) <= 1 && center[3] == 255;
    int bg = corner[0] == 0 && corner[1] == 0 && corner[2] == 255 && corner[3] == 255;
    if (!hit) { printf("draw: the triangle's pixel is (%u, %u, %u, %u), want (128, 255, 64, 255)\n", center[0], center[1], center[2], center[3]); bad++; }
    if (!bg) { printf("draw: the corner is (%u, %u, %u, %u), want the clear colour (0, 0, 255, 255)\n", corner[0], corner[1], corner[2], corner[3]); bad++; }
    int covered = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) covered += px[y * 256 + x * 4 + 1] == 255;
    printf("d3d12 draw: graphics pipeline from DXIL, input layout, indexed draw with a base vertex, root constants, clears, "
           "depth test, %d pixels covered: %s\n", covered, hit && bg ? "ok" : "FAILED");
    return 0;
}

// Heaps and placed resources, and a texture read in a pixel shader: GetResourceAllocationInfo, three heaps (textures
// only, render targets only, upload buffers), two upload buffers placed at the same offset (they must share memory),
// a 4x4 texture placed and filled from one of them by CopyTextureRegion, an SRV in a shader-visible heap reached by a
// descriptor table, a static point sampler; a full-target triangle (texq_vs, no vertex input) samples it into a placed
// 64x64 target, so each texel must cover a 16x16 block.
static int texture_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    static unsigned char vs[65536], ps[65536];
    size_t vs_size, ps_size;
    if (load("tests/shaders12/texq_vs.dxil", vs, sizeof vs, &vs_size) || load("tests/shaders12/texq_ps.dxil", ps, sizeof ps, &ps_size)) return 1;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss = { D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1, D3D12_COMPARISON_FUNC_NEVER, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK,
                                     0, D3D12_FLOAT32_MAX, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param, 1, &ss, 0 };
    ID3DBlob *blob, *errs;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errs));
    ID3D12RootSignature *rs;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));
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
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));

    // sizes, the heaps
    D3D12_RESOURCE_DESC texd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE };
    D3D12_RESOURCE_DESC rtd = texd;
    rtd.Width = rtd.Height = 64;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_DESC bufd = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 4096, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, 0 };
    D3D12_RESOURCE_ALLOCATION_INFO tex_ai = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &texd);
    D3D12_RESOURCE_ALLOCATION_INFO rt_ai = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &rtd);
    D3D12_RESOURCE_ALLOCATION_INFO buf_ai = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &bufd);
    int sizes = tex_ai.SizeInBytes >= 64 && tex_ai.Alignment == 65536 && rt_ai.SizeInBytes >= 64 * 64 * 4 && rt_ai.SizeInBytes % 65536 == 0 &&
                buf_ai.SizeInBytes == 65536 && buf_ai.Alignment == 65536;
    if (!sizes) { printf("texture: allocation info tex %llu/%llu rt %llu/%llu buffer %llu/%llu\n", (unsigned long long)tex_ai.SizeInBytes,
                         (unsigned long long)tex_ai.Alignment, (unsigned long long)rt_ai.SizeInBytes, (unsigned long long)rt_ai.Alignment,
                         (unsigned long long)buf_ai.SizeInBytes, (unsigned long long)buf_ai.Alignment); bad++; }
    D3D12_HEAP_DESC hd = { 2 * tex_ai.SizeInBytes, { D3D12_HEAP_TYPE_DEFAULT }, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES };
    ID3D12Heap *tex_heap, *rt_heap, *up_heap;
    CHECK(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&tex_heap));
    hd.SizeInBytes = rt_ai.SizeInBytes;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    CHECK(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&rt_heap));
    hd.SizeInBytes = 3 * 65536;
    hd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    CHECK(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&up_heap));
    D3D12_HEAP_DESC got = ID3D12Heap_GetDesc(up_heap);
    if (got.SizeInBytes != 3 * 65536 || got.Properties.Type != D3D12_HEAP_TYPE_UPLOAD || got.Alignment != 65536) { printf("texture: heap GetDesc %llu %d %llu\n", (unsigned long long)got.SizeInBytes, got.Properties.Type, (unsigned long long)got.Alignment); bad++; }

    // placed resources
    ID3D12Resource *tex, *rt, *up, *alias, *outside = NULL;
    CHECK(ID3D12Device_CreatePlacedResource(dev, tex_heap, tex_ai.SizeInBytes, &texd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex));
    CHECK(ID3D12Device_CreatePlacedResource(dev, rt_heap, 0, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    CHECK(ID3D12Device_CreatePlacedResource(dev, up_heap, 65536, &bufd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&up));
    CHECK(ID3D12Device_CreatePlacedResource(dev, up_heap, 65536, &bufd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&alias));
    HRESULT hr_out = ID3D12Device_CreatePlacedResource(dev, up_heap, 3 * 65536, &bufd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&outside);
    if (SUCCEEDED(hr_out)) { printf("texture: a buffer placed past its heap's end was made\n"); bad++; }
    D3D12_HEAP_PROPERTIES hp;
    D3D12_HEAP_FLAGS hf;
    CHECK(ID3D12Resource_GetHeapProperties(up, &hp, &hf));
    if (hp.Type != D3D12_HEAP_TYPE_UPLOAD || hf != D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS) { printf("texture: placed buffer's heap %d %#x\n", hp.Type, hf); bad++; }

    // the texels, written through one buffer and read through the other placed at the same offset
    unsigned char texels[4][4][4];
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { texels[y][x][0] = 40 + 60 * x; texels[y][x][1] = 40 + 60 * y; texels[y][x][2] = (x ^ y) * 80; texels[y][x][3] = 255; }
    unsigned char *w, *r;
    CHECK(ID3D12Resource_Map(alias, 0, NULL, (void **)&w));
    for (int y = 0; y < 4; y++) memcpy(w + 256 * y, texels[y], 16);
    ID3D12Resource_Unmap(alias, 0, NULL);
    CHECK(ID3D12Resource_Map(up, 0, NULL, (void **)&r));
    int aliased = !memcmp(r + 256 * 3, texels[3], 16) && ID3D12Resource_GetGPUVirtualAddress(up) == ID3D12Resource_GetGPUVirtualAddress(alias);
    ID3D12Resource_Unmap(up, 0, NULL);
    if (!aliased) { printf("texture: two buffers placed at the same offset do not share memory\n"); bad++; }

    // views
    D3D12_DESCRIPTOR_HEAP_DESC dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *srvs, *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &dhd, &IID_ID3D12DescriptorHeap, (void **)&srvs));
    dhd = (D3D12_DESCRIPTOR_HEAP_DESC){ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &dhd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE srv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srvs);
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srvs);
    srv.ptr += inc;                                                // (the second descriptor: the table's start is not the heap's)
    srv_gpu.ptr += inc;
    ID3D12Device_CreateShaderResourceView(dev, tex, NULL, srv);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    ID3D12Resource *rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *qrb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_OCCLUSION, 4 };
    ID3D12QueryHeap *occ, *ts;
    CHECK(ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&occ));
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2;
    CHECK(ID3D12Device_CreateQueryHeap(dev, &qd, &IID_ID3D12QueryHeap, (void **)&ts));
    UINT64 freq = 0, gpu0 = 0, cpu0 = 0;
    CHECK(ID3D12CommandQueue_GetTimestampFrequency(queue, &freq));
    CHECK(ID3D12CommandQueue_GetClockCalibration(queue, &gpu0, &cpu0));
    if (bad) return 1;

    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_EndQuery(list, ts, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    D3D12_TEXTURE_COPY_LOCATION from = { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    from.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION to = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
    D3D12_RESOURCE_BARRIER barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    float grey[4] = { 0.5f, 0.5f, 0.5f, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, grey, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &srvs);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, srv_gpu);
    D3D12_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    D3D12_RECT sc = { 0, 0, 64, 64 };
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_BeginQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 0);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 0);
    ID3D12GraphicsCommandList_BeginQuery(list, occ, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(list, occ, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
    ID3D12GraphicsCommandList_BeginQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 2);
    ID3D12GraphicsCommandList_EndQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 2);
    ID3D12GraphicsCommandList_BeginQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 3);   // (two passes: the clear splits them)
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, grey, 0, NULL);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_EndQuery(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 3);
    ID3D12GraphicsCommandList_EndQuery(list, ts, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    ID3D12GraphicsCommandList_ResolveQueryData(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 0, 1, qrb, 0);
    ID3D12GraphicsCommandList_ResolveQueryData(list, occ, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1, 1, qrb, 8);
    ID3D12GraphicsCommandList_ResolveQueryData(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 2, 1, qrb, 16);
    ID3D12GraphicsCommandList_ResolveQueryData(list, ts, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb, 24);
    ID3D12GraphicsCommandList_ResolveQueryData(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 3, 1, qrb, 40);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    unsigned char *px;
    CHECK(ID3D12Resource_Map(rb, 0, NULL, (void **)&px));
    int wrong = 0;
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        const unsigned char *a = px + y * 256 + x * 4, *t = texels[y / 16][x / 16];
        if (memcmp(a, t, 4)) {
            if (!wrong) printf("texture: pixel (%d, %d) is (%u, %u, %u, %u), want texel (%d, %d) (%u, %u, %u, %u)\n", x, y, a[0], a[1], a[2], a[3], x / 16, y / 16, t[0], t[1], t[2], t[3]);
            wrong++;
        }
    }
    if (wrong) bad++;
    UINT64 *q;
    CHECK(ID3D12Resource_Map(qrb, 0, NULL, (void **)&q));
    UINT64 gpu1 = 0, cpu1 = 0;
    ID3D12CommandQueue_GetClockCalibration(queue, &gpu1, &cpu1);
    int queries = q[0] == 4096 && q[1] == 1 && q[2] == 0 && q[5] == 8192 && freq == 1000000000 && q[3] >= gpu0 && q[4] >= q[3] && q[4] <= gpu1;
    printf("d3d12 queries: occlusion %llu samples (want 4096), binary %llu (want 1), empty %llu (want 0), across two passes %llu (want 8192), timestamps %.3f ms apart "
           "within the calibrations' %.3f ms: %s\n", (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
           (unsigned long long)q[5], (q[4] - q[3]) / 1e6, (gpu1 - gpu0) / 1e6, queries ? "ok" : "FAILED");
    if (!queries) { printf("  timestamps %llu %llu, calibration %llu .. %llu\n", (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)gpu0, (unsigned long long)gpu1); bad++; }
    ID3D12Resource_Unmap(qrb, 0, NULL);
    printf("d3d12 texture: heaps, placed buffers and textures (aliased buffers share memory, one past the end refused), "
           "texture upload, SRV through a descriptor table, static sampler, %d of 4096 pixels wrong: %s\n", wrong, !wrong && sizes && aliased ? "ok" : "FAILED");
    return 0;
}

// Presenting: a window, a swap chain on the D3D12 queue (2 buffers, flip discard), three frames each clearing the
// current back buffer to its own colour and presenting; the index must go 0 1 0 and buffer 0 hold frame 2's colour.
static int present_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "vxrt_d3d12_test";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "vxrt_d3d12_test", "D3D12 present", WS_OVERLAPPEDWINDOW, 0, 0, 160, 120, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("present: no window\n"); return ++bad; }
    IDXGIFactory4 *factory;
    CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory));
    DXGI_SWAP_CHAIN_DESC1 sd = { 128, 96, DXGI_FORMAT_R8G8B8A8_UNORM, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
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
    UINT seen[3];
    float colors[3][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 1, 1, 0, 1 } };
    for (UINT f = 0; f < 3; f++) {
        UINT i = seen[f] = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
        D3D12_CPU_DESCRIPTOR_HANDLE h = { rtv0.ptr + i * inc };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, colors[f], 0, NULL);
        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        CHECK(IDXGISwapChain3_Present(sc, 1, 0));
        wait_queue(dev, queue);
    }
    if (seen[0] != 0 || seen[1] != 1 || seen[2] != 0) { printf("present: back buffer indices %u %u %u, want 0 1 0\n", seen[0], seen[1], seen[2]); bad++; }
    // buffer 0 last held frame 2's colour
    ID3D12Resource *rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 512 * 96, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 128, 96, 1, 512 };
    D3D12_TEXTURE_COPY_LOCATION src = { back[0], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    ID3D12GraphicsCommandList_Close(list);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    unsigned char *px;
    CHECK(ID3D12Resource_Map(rb, 0, NULL, (void **)&px));
    int yellow = px[0] == 255 && px[1] == 255 && px[2] == 0 && px[3] == 255;
    if (!yellow) { printf("present: buffer 0 holds (%u, %u, %u, %u), want frame 2's yellow\n", px[0], px[1], px[2], px[3]); bad++; }
    UINT presents = 0;
    IDXGISwapChain3_GetLastPresentCount(sc, &presents);
    printf("d3d12 present: swap chain on the queue, %u presents, back buffer indices %u %u %u: %s\n", presents, seen[0], seen[1], seen[2],
           seen[0] == 0 && seen[1] == 1 && seen[2] == 0 && yellow ? "ok" : "FAILED");
    DestroyWindow(hwnd);
    return 0;
}

int main(void) {
    FILE *f = fopen("tests/shaders12/sum_cs.dxil", "rb");
    if (!f) { printf("tests/shaders12/sum_cs.dxil not found (run from VXRuntime)\n"); return 1; }
    static unsigned char cs[65536];
    size_t cs_size = fread(cs, 1, sizeof cs, f);
    fclose(f);

    ID3D12Device *dev = NULL;
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *queue;
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));

    // root signature: one table, t0 then u0..u1 (sum_cs: ByteAddressBuffer t0, RWStructuredBuffer u0, RWTexture2D u1)
    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
    };
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = NULL, *errs = NULL;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errs));
    ID3D12RootSignature *rs;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&rs));

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = { rs, { cs, cs_size } };
    ID3D12PipelineState *pso;
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));

    // resources
    UINT in[64], expect_sum = 0, init[66] = { 1000 };
    for (int i = 0; i < 64; i++) { in[i] = (UINT)(i * 7 + 3); expect_sum += in[i]; }
    ID3D12Resource *up_in = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof in, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *up_init = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof init, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *uav = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, sizeof init, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, sizeof init, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *img_rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 1024, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (bad) return 1;
    void *p;
    CHECK(ID3D12Resource_Map(up_in, 0, NULL, &p)); memcpy(p, in, sizeof in); ID3D12Resource_Unmap(up_in, 0, NULL);
    CHECK(ID3D12Resource_Map(up_init, 0, NULL, &p)); memcpy(p, init, sizeof init); ID3D12Resource_Unmap(up_init, 0, NULL);
    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    ID3D12Resource *img;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&img));

    // descriptors: t0 = the input (raw), u0 = the structured buffer, u1 = the texture
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = { DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING };
    sv.Buffer.NumElements = 64;
    sv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    ID3D12Device_CreateShaderResourceView(dev, up_in, &sv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = { DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER };
    uv.Buffer.NumElements = 66;
    uv.Buffer.StructureByteStride = 4;
    cpu.ptr += inc;
    ID3D12Device_CreateUnorderedAccessView(dev, uav, NULL, &uv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC tv = { DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_UAV_DIMENSION_TEXTURE2D };
    cpu.ptr += inc;
    ID3D12Device_CreateUnorderedAccessView(dev, img, NULL, &tv, cpu);

    // record: initialize the UAV buffer, dispatch, read back
    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_CopyBufferRegion(list, uav, 0, up_init, 0, sizeof init);
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition.pResource = uav;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, uav, 0, sizeof init);
    D3D12_TEXTURE_COPY_LOCATION dst = { img_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = 64;
    dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 1024;
    D3D12_TEXTURE_COPY_LOCATION src = { img, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);

    ID3D12Fence *fence;
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(ID3D12CommandQueue_Signal(queue, fence, 1));
    CHECK(ID3D12Fence_SetEventOnCompletion(fence, 1, ev));
    if (WaitForSingleObject(ev, 5000) != WAIT_OBJECT_0) { printf("FAILED: the fence did not reach 1 in 5 s\n"); return 1; }

    UINT *o;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&o));
    if (o[0] != 1000 + expect_sum) { printf("InterlockedAdd gave %u, want %u\n", o[0], 1000 + expect_sum); bad++; }
    for (UINT gi = 0; gi < 64; gi++) {
        float s = sqrtf((float)gi);
        UINT su;
        memcpy(&su, &s, 4);
        UINT want = (gi & 3) == 0 ? 1 : (gi & 3) == 1 ? su : (UINT)__builtin_popcount(gi);
        int ulp1 = (gi & 3) == 1 && (o[gi + 1] == want + 1 || o[gi + 1] + 1 == want);
        if (o[gi + 1] != want && !ulp1 && bad++ < 5) printf("outBuf[%u] = %#x, want %#x\n", gi + 1, o[gi + 1], want);
    }
    float *px;
    CHECK(ID3D12Resource_Map(img_rb, 0, NULL, (void **)&px));
    UINT simd = 0;
    for (UINT x = 0; x < 64; x++) if (px[4 * x + 1] == 0 && x) { simd = x; break; }   // (the lane index wraps at the SIMD width)
    if (!simd) simd = 64;
    for (UINT x = 0; x < 64; x++) {
        UINT base = x / simd * simd, sum = 0;
        for (UINT k = base; k < base + simd; k++) sum += k;
        if ((px[4 * x] != (float)sum || px[4 * x + 1] != (float)(x % simd) || px[4 * x + 3] != 1) && bad++ < 8)
            printf("img[%u] = (%g, %g, %g, %g)\n", x, px[4 * x], px[4 * x + 1], px[4 * x + 2], px[4 * x + 3]);
    }
    printf("d3d12 compute: root signature serialized and created, descriptor table, upload -> default copy, dispatch of DXIL, "
           "readback of a buffer and a texture, fence (SIMD width %u): %s\n", simd, bad ? "FAILED" : "ok");
    draw_test(dev, queue);
    texture_test(dev, queue);
    present_test(dev, queue);
    return bad ? 1 : 0;
}
