// Direct3D 12 pipelines from fxc's shaders (DXBC, shader model 4 and 5), as Unreal Engine 4 and Unity build them by
// default: the D3D11 test shaders in tests/shaders/*.dxbc bound through root signatures. A textured draw (vertex root
// constants, a pixel root CBV, an SRV descriptor table, a static sampler), a structured-buffer reduction (root SRV, UAV
// table, root constants), and append/consume buffers with their hidden counters plus typed-texture atomics.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS                 // (methods returning structures, as MSVC calls them: a hidden result pointer)
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int bad;
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED: %s: %#lx\n", #x, (unsigned long)hr_); return 1; } } while (0)

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;

static int load(const char *name, D3D12_SHADER_BYTECODE *bc) {
    static unsigned char store[16][65536];
    static int used;
    char path[128];
    // ("12/name": a DXIL shader of tests/shaders12)
    if (!strncmp(name, "12/", 3)) snprintf(path, sizeof path, "tests/shaders12/%s.dxil", name + 3);
    else snprintf(path, sizeof path, "tests/shaders/%s.dxbc", name);
    FILE *f = fopen(path, "rb");
    if (!f || used == 16) { printf("%s not found (run from VXRuntime)\n", path); return -1; }
    bc->pShaderBytecode = store[used];
    bc->BytecodeLength = fread(store[used++], 1, sizeof store[0], f);
    fclose(f);
    return 0;
}

static ID3D12Resource *buffer(D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
    ID3D12Resource *r = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&r))) {
        printf("FAILED: buffer of %llu bytes\n", (unsigned long long)size);
        bad++;
    }
    return r;
}
static void fill(ID3D12Resource *r, const void *data, size_t size) {
    void *p;
    if (FAILED(ID3D12Resource_Map(r, 0, NULL, &p))) { bad++; return; }
    memcpy(p, data, size);
    ID3D12Resource_Unmap(r, 0, NULL);
}
static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    b.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}
// Closes and runs the list, waits, and opens it again.
static int run(void) {
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12Fence *fence;
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    ID3D12CommandQueue_Signal(queue, fence, 1);
    ID3D12Fence_SetEventOnCompletion(fence, 1, NULL);
    ID3D12Fence_Release(fence);
    CHECK(ID3D12CommandAllocator_Reset(alloc));
    CHECK(ID3D12GraphicsCommandList_Reset(list, alloc, NULL));
    return 0;
}
static ID3D12RootSignature *root_signature(const D3D12_ROOT_SIGNATURE_DESC *desc) {
    ID3DBlob *blob = NULL, *errors = NULL;
    ID3D12RootSignature *rs = NULL;
    if (FAILED(D3D12SerializeRootSignature(desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                 &IID_ID3D12RootSignature, (void **)&rs))) {
        printf("FAILED: root signature\n");
        bad++;
    }
    return rs;
}

// tex_vs (b0: offset, scale) + tex_ps (t0, s0, b0: tint) over a 2x2 texture: red green / blue white, point sampled.
// A quad over the right half of a 16x16 target, the left half cleared black.
static int draw_test(void) {
    int before = bad;
    D3D12_SHADER_BYTECODE vs, ps;
    if (load("tex_vs", &vs) || load("tex_ps", &ps)) return 1;
    D3D12_DESCRIPTOR_RANGE srv_range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[3];
    memset(params, 0, sizeof params);
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = (D3D12_ROOT_CONSTANTS){ 0, 0, 4 };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = (D3D12_ROOT_DESCRIPTOR){ 0, 0 };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable = (D3D12_ROOT_DESCRIPTOR_TABLE){ 1, &srv_range };
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = { D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                          D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1, D3D12_COMPARISON_FUNC_NEVER,
                                          D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, 0, 0, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3D12RootSignature *rs = root_signature(&rsd);
    if (!rs) return 1;
    D3D12_INPUT_ELEMENT_DESC il[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs;
    pd.VS = vs;
    pd.PS = ps;
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
    ID3D12PipelineState *pso;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));

    // the 2x2 texture, uploaded through a footprint; the target
    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, 0 };
    ID3D12Resource *tex, *rt;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex));
    td.Width = td.Height = 16;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    uint32_t texels[2 * 64] = { 0 };                               // rows of 256 bytes
    texels[0] = 0xff0000ff; texels[1] = 0xff00ff00; texels[64] = 0xffff0000; texels[65] = 0xffffffff;
    ID3D12Resource *upload = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof texels, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *readback = buffer(D3D12_HEAP_TYPE_READBACK, 256 * 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    float tint[64] = { 1, 1, 1, 1 };                               // (a 256-byte constant buffer)
    ID3D12Resource *tint_cb = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof tint, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    // a quad: positions in [0, 1]^2 mapped by scale (1, 2) and offset (0, -1) onto the right half
    float quad[6][4] = { { 0, 0, 0, 1 }, { 0, 1, 0, 0 }, { 1, 1, 1, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1, 0 }, { 1, 0, 1, 1 } };
    ID3D12Resource *vb = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof quad, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (bad > before) return 1;
    fill(upload, texels, sizeof texels);
    fill(tint_cb, tint, sizeof tint);
    fill(vb, quad, sizeof quad);

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap, *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    hd = (D3D12_DESCRIPTOR_HEAP_DESC){ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE srv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    srv.ptr += 2 * inc;                                            // (a table that does not start the heap)
    ID3D12Device_CreateShaderResourceView(dev, tex, NULL, srv);
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    srv_gpu.ptr += 2 * inc;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    D3D12_TEXTURE_COPY_LOCATION dst = { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION src = { upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    src.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    float black[4] = { 0, 0, 0, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, black, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    float transform[4] = { 0, -1, 1, 2 };                          // offset, scale
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, transform, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 1, ID3D12Resource_GetGPUVirtualAddress(tint_cb));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, srv_gpu);
    D3D12_VIEWPORT vp = { 0, 0, 16, 16, 0, 1 };
    D3D12_RECT sc = { 0, 0, 16, 16 };
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof quad, sizeof quad[0] };
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_DrawInstanced(list, 6, 1, 0, 0);
    barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION rb = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    rb.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION from = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &rb, 0, 0, 0, &from, NULL);
    if (run()) return 1;
    // expected: x < 8 black; x >= 8: top-left quarter of the quad red (uv 0,0), top-right green, bottom blue / white
    uint32_t *px;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&px));
    int wrong = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
            uint32_t want = x < 8 ? 0xff000000 : y < 8 ? (x < 12 ? 0xff0000ff : 0xff00ff00) : (x < 12 ? 0xffff0000 : 0xffffffff);
            uint32_t got = px[y * 64 + x];
            if (got != want && wrong++ < 4) printf("draw: pixel (%d, %d) %08x, want %08x\n", x, y, got, want);
        }
    if (wrong && getenv("D3D12_FXC_DUMP"))
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) printf(" %08x", px[y * 64 + x]);
            printf("\n");
        }
    ID3D12Resource_Unmap(readback, 0, NULL);
    if (wrong) bad++;
    printf("d3d12 fxc draw: DXBC tex_vs/tex_ps with vertex root constants, a pixel root CBV, an SRV table and a static sampler, %d of 256 pixels wrong: %s\n",
           wrong, bad == before ? "ok" : "FAILED");
    return 0;
}

// sum_cs: 100 floats in groups of 64, times the root-constant scale 0.5: two partial sums.
static int sum_test(void) {
    int before = bad;
    D3D12_SHADER_BYTECODE cs;
    if (load("sum_cs", &cs)) return 1;
    D3D12_DESCRIPTOR_RANGE uav_range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[3];
    memset(params, 0, sizeof params);
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = (D3D12_ROOT_DESCRIPTOR_TABLE){ 1, &uav_range };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = (D3D12_ROOT_CONSTANTS){ 0, 0, 2 };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 3, params, 0, NULL, 0 };
    ID3D12RootSignature *rs = root_signature(&rsd);
    if (!rs) return 1;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { rs, cs };
    ID3D12PipelineState *pso;
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&pso));
    float in[128];
    for (int i = 0; i < 128; i++) in[i] = (float)(i + 1);
    ID3D12Resource *input = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof in, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *output = buffer(D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *readback = buffer(D3D12_HEAP_TYPE_READBACK, 256, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (bad > before) return 1;
    fill(input, in, sizeof in);
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = { DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER };
    ud.Buffer.NumElements = 64;
    ud.Buffer.StructureByteStride = 4;
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &ud, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap));
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(list, 0, ID3D12Resource_GetGPUVirtualAddress(input));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 1, ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap));
    union { float f; UINT u; } constants[2] = { { .f = 0.5f }, { .u = 100 } };
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(list, 2, 2, constants, 0);
    ID3D12GraphicsCommandList_Dispatch(list, 2, 1, 1);
    barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, 256);
    if (run()) return 1;
    float *r;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&r));
    float want0 = 0.5f * 64 * 65 / 2, want1 = 0.5f * (100 * 101 / 2 - 64 * 65 / 2);
    if (r[0] != want0 || r[1] != want1) { printf("sum: %g %g, want %g %g\n", r[0], r[1], want0, want1); bad++; }
    printf("d3d12 fxc compute: DXBC sum_cs with a root SRV, a UAV table and root constants, sums %g %g: %s\n", r[0], r[1],
           bad == before ? "ok" : "FAILED");
    ID3D12Resource_Unmap(readback, 0, NULL);
    return 0;
}

// append_cs: consumes 1..128 from u0 (counter 128), appends v*10 for odd v to u1 (counter 0), counts v%4 in an
// R32_UINT texture u2 with InterlockedAdd. Then: 64 values appended, the consume counter at 0, 32 in each bin.
static int append_test(void) {
    int before = bad;
    D3D12_SHADER_BYTECODE cs;
    if (load("append_cs", &cs)) return 1;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 0 };
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    param.DescriptorTable = (D3D12_ROOT_DESCRIPTOR_TABLE){ 1, &range };
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param, 0, NULL, 0 };
    ID3D12RootSignature *rs = root_signature(&rsd);
    if (!rs) return 1;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { rs, cs };
    ID3D12PipelineState *pso;
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **)&pso));
    // each buffer: 128 uints of data, then its counter at byte 512 (CounterOffsetInBytes, 4 KiB-aligned rules aside)
    uint32_t data[128 + 1024] = { 0 };
    for (int i = 0; i < 128; i++) data[i] = (uint32_t)i + 1;
    data[1024] = 128;                                              // the consume counter at byte 4096
    ID3D12Resource *upload = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof data, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *in = buffer(D3D12_HEAP_TYPE_DEFAULT, sizeof data, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *out = buffer(D3D12_HEAP_TYPE_DEFAULT, sizeof data, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *readback = buffer(D3D12_HEAP_TYPE_READBACK, 2 * sizeof data + 256, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 1, 1, 1, DXGI_FORMAT_R32_UINT, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    ID3D12Resource *hist;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&hist));
    if (bad > before) return 1;
    fill(upload, data, sizeof data);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, in, 0, upload, 0, sizeof data);
    barrier(in, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *heap;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = { DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER };
    ud.Buffer.NumElements = 128;
    ud.Buffer.StructureByteStride = 4;
    ud.Buffer.CounterOffsetInBytes = 4096;
    ID3D12Device_CreateUnorderedAccessView(dev, in, in, &ud, h);
    h.ptr += inc;
    ID3D12Device_CreateUnorderedAccessView(dev, out, out, &ud, h);
    h.ptr += inc;
    ID3D12Device_CreateUnorderedAccessView(dev, hist, NULL, NULL, h);
    // (the clear's CPU handle is in a heap the shaders do not see, as D3D12 requires)
    D3D12_DESCRIPTOR_HEAP_DESC cpu_hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1 };
    ID3D12DescriptorHeap *cpu_heap;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &cpu_hd, &IID_ID3D12DescriptorHeap, (void **)&cpu_heap));
    h = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_heap);
    ID3D12Device_CreateUnorderedAccessView(dev, hist, NULL, NULL, h);
    UINT zero[4] = { 0 };
    D3D12_GPU_DESCRIPTOR_HANDLE hist_gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    hist_gpu.ptr += 2 * inc;
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, hist_gpu, h, hist, zero, 0, NULL);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap));
    ID3D12GraphicsCommandList_Dispatch(list, 2, 1, 1);
    barrier(in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(hist, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, in, 0, sizeof data);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, sizeof data, out, 0, sizeof data);
    D3D12_TEXTURE_COPY_LOCATION rb = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    rb.PlacedFootprint.Offset = 2 * sizeof data;
    rb.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R32_UINT, 4, 1, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION from = { hist, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &rb, 0, 0, 0, &from, NULL);
    if (run()) return 1;
    uint32_t *r;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&r));
    const uint32_t *in_after = r, *out_after = r + sizeof data / 4, *bins = r + 2 * sizeof data / 4;
    uint64_t sum = 0;
    int odd = 1;
    for (uint32_t i = 0; i < out_after[1024] && i < 128; i++) { sum += out_after[i]; odd &= (out_after[i] / 10) & 1; }
    int ok = in_after[1024] == 0 && out_after[1024] == 64 && sum == 10ull * 64 * 64 && odd &&
             bins[0] == 32 && bins[1] == 32 && bins[2] == 32 && bins[3] == 32;
    if (!ok) { printf("append: consume counter %u, append counter %u, sum %llu, bins %u %u %u %u\n", in_after[1024], out_after[1024],
                      (unsigned long long)sum, bins[0], bins[1], bins[2], bins[3]); bad++; }
    printf("d3d12 fxc counters: DXBC append_cs consumed 128, appended %u (sum %llu), texture-atomic bins %u %u %u %u: %s\n",
           out_after[1024], (unsigned long long)sum, bins[0], bins[1], bins[2], bins[3], bad == before ? "ok" : "FAILED");
    ID3D12Resource_Unmap(readback, 0, NULL);
    return 0;
}

// color_vs -> quad_gs (b0: half size, geometry-visible root constants) -> color_ps: each point becomes a square, and
// every primitive adds a small blue triangle in the top-left corner (after a strip cut). Drawn twice into a 32x32
// target: plain, and indexed with a base vertex past a vertex that must not be drawn.
// The same with the shaders compiled to DXIL (tests/shaders12/passcolor_vs, quad_gs, passcolor_ps: the same sources).
static int geometry_test(int dxil) {
    int before = bad;
    D3D12_SHADER_BYTECODE vs, gs, ps;
    if (load(dxil ? "12/passcolor_vs" : "color_vs", &vs) || load(dxil ? "12/quad_gs" : "quad_gs", &gs) ||
        load(dxil ? "12/passcolor_ps" : "color_ps", &ps)) return 1;
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS };
    param.Constants = (D3D12_ROOT_CONSTANTS){ 0, 0, 4 };
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_GEOMETRY;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3D12RootSignature *rs = root_signature(&rsd);
    if (!rs) return 1;
    D3D12_INPUT_ELEMENT_DESC il[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rs; pd.VS = vs; pd.GS = gs; pd.PS = ps;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFF;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.InputLayout = (D3D12_INPUT_LAYOUT_DESC){ il, 2 };
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso));

    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 32, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                               D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    ID3D12Resource *rt;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&rt));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    // vertices: one never drawn (yellow), then the red and green points; indices 1, 0 drawn with base vertex 1
    struct { float pos[3]; uint32_t color; } verts[3] = {
        { { 0, 0.8f, 0 }, 0xff00ffff }, { { -0.5f, 0, 0 }, 0xff0000ff }, { { 0.5f, 0, 0 }, 0xff00ff00 } };
    uint16_t idx[2] = { 1, 0 };
    ID3D12Resource *vb = buffer(D3D12_HEAP_TYPE_UPLOAD, sizeof verts, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ib = buffer(D3D12_HEAP_TYPE_UPLOAD, 256, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *readback = buffer(D3D12_HEAP_TYPE_READBACK, 2 * 256 * 32, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (bad > before) return 1;
    fill(vb, verts, sizeof verts);
    fill(ib, idx, sizeof idx);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof verts, sizeof verts[0] };
    D3D12_INDEX_BUFFER_VIEW ibv = { ID3D12Resource_GetGPUVirtualAddress(ib), sizeof idx, DXGI_FORMAT_R16_UINT };
    D3D12_VIEWPORT vp = { 0, 0, 32, 32, 0, 1 };
    D3D12_RECT sc = { 0, 0, 32, 32 };
    float black[4] = { 0, 0, 0, 1 }, size[4] = { 0.25f, 0, 0, 0 };
    for (int pass = 0; pass < 2; pass++) {
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, black, 0, NULL);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, size, 0);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
        if (pass == 0) ID3D12GraphicsCommandList_DrawInstanced(list, 2, 1, 1, 0);
        else {
            ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv);
            ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 2, 1, 0, 1, 0);
        }
        barrier(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION rb = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        rb.PlacedFootprint.Offset = (UINT64)pass * 256 * 32;
        rb.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 256 };
        D3D12_TEXTURE_COPY_LOCATION from = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &rb, 0, 0, 0, &from, NULL);
        barrier(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    if (run()) return 1;
    uint32_t *px;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&px));
    int wrong = 0;
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t *img = px + pass * 64 * 32;
        // (x, y) pixel centres in NDC: ((x + 0.5) / 16 - 1, 1 - (y + 0.5) / 16)
        static const struct { int x, y; uint32_t want; } probe[] = {
            { 8, 16, 0xff0000ff }, { 5, 13, 0xff0000ff }, { 24, 16, 0xff00ff00 }, { 26, 18, 0xff00ff00 },
            { 0, 0, 0xffff0000 }, { 16, 16, 0xff000000 }, { 16, 3, 0xff000000 }, { 2, 30, 0xff000000 } };
        for (size_t k = 0; k < sizeof probe / sizeof probe[0]; k++) {
            uint32_t got = img[probe[k].y * 64 + probe[k].x];
            if (got != probe[k].want && wrong++ < 6)
                printf("geometry (%s): pixel (%d, %d) %08x, want %08x\n", pass ? "indexed" : "plain", probe[k].x, probe[k].y, got, probe[k].want);
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    if (wrong) bad++;
    printf("d3d12 %s geometry: %s vertex -> geometry (squares from points, a cut strip) -> pixel, plain and indexed with a base vertex, %d wrong probes: %s\n",
           dxil ? "dxc" : "fxc", dxil ? "DXIL" : "DXBC", wrong, bad == before ? "ok" : "FAILED");
    return 0;
}

int main(void) {
    CHECK(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    if (draw_test()) return 1;
    if (sum_test()) return 1;
    if (append_test()) return 1;
    if (geometry_test(0)) return 1;
    if (geometry_test(1)) return 1;
    return bad ? 1 : 0;
}
