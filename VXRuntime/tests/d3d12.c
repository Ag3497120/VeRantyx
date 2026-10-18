// Direct3D 12 on VXRuntime, compute: a device, a root signature serialized and created again, a shader-visible
// heap with an SRV and two UAVs, a compute pipeline from DXIL (tests/shaders12/sum_cs.dxil, run as tools/dxilrun
// runs it on Metal directly), copies from an upload heap, a dispatch, copies to readback heaps, a fence. Checks
// what the shader wrote, as tools/dxilrun does.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS                 // (methods returning structures, as MSVC calls them: a hidden result pointer)
#define INITGUID
#include <windows.h>
#include <d3d12.h>
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
    return bad ? 1 : 0;
}
