// Direct3D 12 the way game engines (Unreal Engine 4/5, Unity) start it and draw a frame: DXGI 1.6 adapter selection,
// the HDR and tearing queries, every feature-option block, pipeline state streams (ID3D12Device2::CreatePipelineState),
// command lists created closed (CreateCommandList1), PIX markers, render passes (BeginRenderPass with a clear and a
// multisample resolve as the ending access), SetEventOnMultipleFenceCompletion, and a flip swap chain with a frame
// latency waitable object. Checks the values engines branch on and the pixels drawn.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS                 // (methods returning structures, as MSVC calls them: a hidden result pointer)
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int bad;
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED: %s: %#lx\n", #x, (unsigned long)hr_); return 1; } } while (0)
#define EXPECT(cond, ...) do { if (!(cond)) { printf("FAILED: " __VA_ARGS__); printf("\n"); bad++; } } while (0)

static int load(const char *path, unsigned char *buf, size_t cap, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("%s not found (run from VXRuntime)\n", path); return -1; }
    *size = fread(buf, 1, cap, f);
    fclose(f);
    return 0;
}

static ID3D12Resource *buffer(ID3D12Device *dev, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = { heap };
    D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, 0 };
    ID3D12Resource *r = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&r))) bad++;
    return r;
}

static void wait_queue(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    ID3D12Fence *fence;
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    ID3D12CommandQueue_Signal(queue, fence, 1);
    ID3D12Fence_SetEventOnCompletion(fence, 1, NULL);              // (no event: waits)
    ID3D12Fence_Release(fence);
}

// ---- DXGI: the adapter by preference, its outputs, tearing --------------------------------------------------------------
static int dxgi_test(IDXGIAdapter4 **adapter_out) {
    IDXGIFactory2 *factory2;
    CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory2, (void **)&factory2));
    IDXGIFactory6 *factory;
    CHECK(IDXGIFactory2_QueryInterface(factory2, &IID_IDXGIFactory6, (void **)&factory));
    IDXGIAdapter4 *adapter = NULL;
    CHECK(IDXGIFactory6_EnumAdapterByGpuPreference(factory, 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter4, (void **)&adapter));
    IDXGIAdapter1 *second = NULL;
    HRESULT hr = IDXGIFactory6_EnumAdapterByGpuPreference(factory, 1, DXGI_GPU_PREFERENCE_MINIMUM_POWER, &IID_IDXGIAdapter1, (void **)&second);
    EXPECT(hr == DXGI_ERROR_NOT_FOUND && !second, "a second adapter: %#lx", (unsigned long)hr);
    DXGI_ADAPTER_DESC3 desc;
    CHECK(IDXGIAdapter4_GetDesc3(adapter, &desc));
    EXPECT(desc.VendorId == 0x106B && desc.DedicatedVideoMemory > 0, "adapter vendor %#x, memory %llu", desc.VendorId,
           (unsigned long long)desc.DedicatedVideoMemory);
    DXGI_QUERY_VIDEO_MEMORY_INFO memory;
    CHECK(IDXGIAdapter4_QueryVideoMemoryInfo(adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory));
    EXPECT(memory.Budget > 0, "no local memory budget");

    IDXGIFactory5 *factory5;
    CHECK(IDXGIFactory6_QueryInterface(factory, &IID_IDXGIFactory5, (void **)&factory5));
    BOOL tearing = FALSE;
    CHECK(IDXGIFactory5_CheckFeatureSupport(factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing));
    EXPECT(IDXGIFactory5_CheckFeatureSupport(factory5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, 2) == E_INVALIDARG, "short feature data accepted");
    IDXGIFactory7 *factory7;
    DWORD cookie = 0;
    CHECK(IDXGIFactory6_QueryInterface(factory, &IID_IDXGIFactory7, (void **)&factory7));
    CHECK(IDXGIFactory7_RegisterAdaptersChangedEvent(factory7, CreateEventA(NULL, FALSE, FALSE, NULL), &cookie));
    CHECK(IDXGIFactory7_UnregisterAdaptersChangedEvent(factory7, cookie));

    IDXGIOutput *output;
    CHECK(IDXGIAdapter4_EnumOutputs(adapter, 0, &output));
    IDXGIOutput6 *output6;
    CHECK(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput6, (void **)&output6));
    DXGI_OUTPUT_DESC1 od;
    CHECK(IDXGIOutput6_GetDesc1(output6, &od));
    EXPECT(od.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 && od.BitsPerColor == 8 && od.AttachedToDesktop &&
           od.DesktopCoordinates.right > od.DesktopCoordinates.left && od.MaxLuminance > 0,
           "output: color space %d, %u bits", od.ColorSpace, od.BitsPerColor);
    UINT modes = 0;
    CHECK(IDXGIOutput6_GetDisplayModeList1(output6, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &modes, NULL));
    DXGI_MODE_DESC1 mode[4];
    CHECK(IDXGIOutput6_GetDisplayModeList1(output6, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &modes, mode));
    EXPECT(modes >= 1 && mode[0].Width > 0 && mode[0].Format == DXGI_FORMAT_R8G8B8A8_UNORM && !mode[0].Stereo, "display modes");
    UINT hw = 99;
    CHECK(IDXGIOutput6_CheckHardwareCompositionSupport(output6, &hw));
    EXPECT(hw == 0, "hardware composition %u", hw);
    printf("d3d12 engine DXGI: adapter by GPU preference (DESC3, budget %llu MB), tearing %s, output 6 SDR %ldx%ld, %u mode(s): %s\n",
           (unsigned long long)(memory.Budget >> 20), tearing ? "allowed" : "not allowed",
           od.DesktopCoordinates.right - od.DesktopCoordinates.left, od.DesktopCoordinates.bottom - od.DesktopCoordinates.top,
           modes, bad ? "FAILED" : "ok");
    IDXGIOutput6_Release(output6);
    IDXGIOutput_Release(output);
    IDXGIFactory7_Release(factory7);
    IDXGIFactory5_Release(factory5);
    IDXGIFactory6_Release(factory);
    IDXGIFactory2_Release(factory2);
    *adapter_out = adapter;
    return 0;
}

// ---- the capability queries an engine makes at start-up ----------------------------------------------------------------
static int feature_test(ID3D12Device *dev) {
    int before = bad;
    static const struct { D3D12_FEATURE feature; UINT size; const char *name; } options[] = {
        { D3D12_FEATURE_D3D12_OPTIONS4, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4), "OPTIONS4" },
        { D3D12_FEATURE_D3D12_OPTIONS5, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5), "OPTIONS5" },
        { D3D12_FEATURE_D3D12_OPTIONS6, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS6), "OPTIONS6" },
        { D3D12_FEATURE_D3D12_OPTIONS7, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7), "OPTIONS7" },
        { D3D12_FEATURE_D3D12_OPTIONS8, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS8), "OPTIONS8" },
        { D3D12_FEATURE_D3D12_OPTIONS9, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS9), "OPTIONS9" },
        { D3D12_FEATURE_D3D12_OPTIONS10, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS10), "OPTIONS10" },
        { D3D12_FEATURE_D3D12_OPTIONS11, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS11), "OPTIONS11" },
        { D3D12_FEATURE_D3D12_OPTIONS12, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12), "OPTIONS12" },
        { D3D12_FEATURE_D3D12_OPTIONS13, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS13), "OPTIONS13" },
        { D3D12_FEATURE_D3D12_OPTIONS14, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS14), "OPTIONS14" },
        { D3D12_FEATURE_D3D12_OPTIONS15, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS15), "OPTIONS15" },
        { D3D12_FEATURE_D3D12_OPTIONS16, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS16), "OPTIONS16" },
        { D3D12_FEATURE_D3D12_OPTIONS17, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS17), "OPTIONS17" },
        { D3D12_FEATURE_D3D12_OPTIONS18, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS18), "OPTIONS18" },
        { D3D12_FEATURE_SHADER_CACHE, sizeof(D3D12_FEATURE_DATA_SHADER_CACHE), "SHADER_CACHE" },
        { D3D12_FEATURE_EXISTING_HEAPS, sizeof(D3D12_FEATURE_DATA_EXISTING_HEAPS), "EXISTING_HEAPS" },
        { D3D12_FEATURE_SERIALIZATION, sizeof(D3D12_FEATURE_DATA_SERIALIZATION), "SERIALIZATION" },
        { D3D12_FEATURE_CROSS_NODE, sizeof(D3D12_FEATURE_DATA_CROSS_NODE), "CROSS_NODE" },
        { D3D12_FEATURE_DISPLAYABLE, sizeof(D3D12_FEATURE_DATA_DISPLAYABLE), "DISPLAYABLE" },
        { D3D12_FEATURE_PROTECTED_RESOURCE_SESSION_SUPPORT, sizeof(D3D12_FEATURE_DATA_PROTECTED_RESOURCE_SESSION_SUPPORT), "PROTECTED_SESSION" },
    };
    unsigned char data[64];
    for (size_t i = 0; i < sizeof options / sizeof options[0]; i++) {
        memset(data, 0xcc, sizeof data);
        memset(data, 0, 4);                                         // (NodeIndex 0 where the block starts with one)
        HRESULT hr = ID3D12Device_CheckFeatureSupport(dev, options[i].feature, data, options[i].size);
        EXPECT(hr == S_OK, "%s: %#lx", options[i].name, (unsigned long)hr);
        hr = ID3D12Device_CheckFeatureSupport(dev, options[i].feature, data, options[i].size + 4);
        EXPECT(hr == E_INVALIDARG, "%s with the wrong size: %#lx", options[i].name, (unsigned long)hr);
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5;
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5));
    EXPECT(o5.RenderPassesTier == D3D12_RENDER_PASS_TIER_0 && o5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED, "OPTIONS5 tiers");
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7;
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7));
    EXPECT(o7.MeshShaderTier == D3D12_MESH_SHADER_TIER_NOT_SUPPORTED, "mesh shaders reported");
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12;
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12));
    EXPECT(!o12.EnhancedBarriersSupported && o12.MSPrimitivesPipelineStatisticIncludesCulledPrimitives == D3D12_TRI_STATE_UNKNOWN,
           "OPTIONS12");
    D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY priority = { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH };
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, &priority, sizeof priority));
    EXPECT(priority.PriorityForTypeIsSupported, "high-priority direct queues");
    priority.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_COMMAND_QUEUE_PRIORITY, &priority, sizeof priority));
    EXPECT(!priority.PriorityForTypeIsSupported, "global real-time queues");

    // device interfaces engines ask for, and the optional services they try
    static const GUID *const devices[] = { &IID_ID3D12Device1, &IID_ID3D12Device2, &IID_ID3D12Device4, &IID_ID3D12Device5,
                                           &IID_ID3D12Device8, &IID_ID3D12Device9 };
    for (size_t i = 0; i < sizeof devices / sizeof devices[0]; i++) {
        IUnknown *u = NULL;
        EXPECT(ID3D12Device_QueryInterface(dev, devices[i], (void **)&u) == S_OK, "device interface %zu", i);
        if (u) IUnknown_Release(u);
    }
    ID3D12Device1 *dev1;
    CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device1, (void **)&dev1));
    ID3D12PipelineLibrary *library = (ID3D12PipelineLibrary *)1;
    HRESULT hr = ID3D12Device1_CreatePipelineLibrary(dev1, NULL, 0, &IID_ID3D12PipelineLibrary, (void **)&library);
    EXPECT(hr == DXGI_ERROR_UNSUPPORTED && !library, "CreatePipelineLibrary: %#lx", (unsigned long)hr);
    ID3D12Device1_Release(dev1);
    CHECK(ID3D12Device_SetStablePowerState(dev, TRUE));
    printf("d3d12 engine features: OPTIONS4-18 and the other start-up blocks answered at their exact sizes, render pass tier 0, no mesh/raytracing/enhanced barriers, Device1-9, no pipeline library: %s\n",
           bad == before ? "ok" : "FAILED");
    return 0;
}

// ---- pipeline state streams --------------------------------------------------------------------------------------------
// A subobject: its 4-byte type, then its structure at that structure's alignment, the whole rounded up to 8 bytes.
static void put(unsigned char *stream, size_t *at, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const void *inner, size_t size, size_t align) {
    *(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *)(stream + *at) = type;
    size_t in = align == 8 ? 8 : 4;
    memcpy(stream + *at + in, inner, size);
    *at += (in + size + 7) & ~(size_t)7;
}

// The frame: a 4x multisample target cleared to blue and a triangle drawn into it inside a render pass whose ending
// access resolves it into a single-sample texture; drawn with a pipeline from a stream, on a list made closed by
// CreateCommandList1, with PIX events around it. The resolved centre must be the tinted vertex colour, a corner blue.
static int render_pass_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    int before = bad;
    static unsigned char vs[65536], ps[65536], cs[65536];
    size_t vs_size, ps_size, cs_size;
    if (load("tests/shaders12/tri_vs.dxil", vs, sizeof vs, &vs_size) || load("tests/shaders12/tri_ps.dxil", ps, sizeof ps, &ps_size) ||
        load("tests/shaders12/sum_cs.dxil", cs, sizeof cs, &cs_size)) return 1;
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

    // the stream: only what differs from the defaults (blend, rasterizer, sample mask are left out); depth is off
    static unsigned char stream[1024];
    size_t at = 0;
    put(stream, &at, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &rs, sizeof rs, 8);
    D3D12_SHADER_BYTECODE vbc = { vs, vs_size }, pbc = { ps, ps_size };
    put(stream, &at, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &vbc, sizeof vbc, 8);
    put(stream, &at, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, &pbc, sizeof pbc, 8);
    D3D12_INPUT_LAYOUT_DESC ild = { il, 2 };
    put(stream, &at, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, &ild, sizeof ild, 8);
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    put(stream, &at, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &topology, sizeof topology, 4);
    struct { DXGI_FORMAT RTFormats[8]; UINT NumRenderTargets; } rtf = { { DXGI_FORMAT_R8G8B8A8_UNORM }, 1 };   // D3D12_RT_FORMAT_ARRAY
    put(stream, &at, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &rtf, sizeof rtf, 4);
    D3D12_DEPTH_STENCIL_DESC1 ds = { 0 };
    ds.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    ds.StencilReadMask = ds.StencilWriteMask = 0xff;
    ds.FrontFace = ds.BackFace = (D3D12_DEPTH_STENCILOP_DESC){ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
    put(stream, &at, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, &ds, sizeof ds, 4);
    DXGI_SAMPLE_DESC samples = { 4, 0 };
    put(stream, &at, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, &samples, sizeof samples, 4);
    D3D12_PIPELINE_STATE_FLAGS flags = D3D12_PIPELINE_STATE_FLAG_NONE;
    put(stream, &at, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, &flags, sizeof flags, 4);
    D3D12_PIPELINE_STATE_STREAM_DESC sd = { at, stream };
    ID3D12Device2 *dev2;
    CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device2, (void **)&dev2));
    ID3D12PipelineState *pso;
    CHECK(ID3D12Device2_CreatePipelineState(dev2, &sd, &IID_ID3D12PipelineState, (void **)&pso));

    // a compute stream; a stream with both a vertex and a compute shader, and one naming the root signature twice, fail
    static unsigned char cstream[256];
    size_t cat = 0;
    D3D12_ROOT_PARAMETER cparams[3] = { { D3D12_ROOT_PARAMETER_TYPE_UAV }, { D3D12_ROOT_PARAMETER_TYPE_SRV }, { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE } };
    D3D12_DESCRIPTOR_RANGE crange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, 0 };
    cparams[2].DescriptorTable = (D3D12_ROOT_DESCRIPTOR_TABLE){ 1, &crange };
    D3D12_ROOT_SIGNATURE_DESC crsd = { 3, cparams, 0, NULL, 0 };
    CHECK(D3D12SerializeRootSignature(&crsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errs));
    ID3D12RootSignature *crs;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&crs));
    put(cstream, &cat, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &crs, sizeof crs, 8);
    D3D12_SHADER_BYTECODE cbc = { cs, cs_size };
    put(cstream, &cat, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, &cbc, sizeof cbc, 8);
    D3D12_PIPELINE_STATE_STREAM_DESC csd = { cat, cstream };
    ID3D12PipelineState *cpso;
    CHECK(ID3D12Device2_CreatePipelineState(dev2, &csd, &IID_ID3D12PipelineState, (void **)&cpso));
    ID3D12PipelineState_Release(cpso);
    put(cstream, &cat, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &vbc, sizeof vbc, 8);
    csd.SizeInBytes = cat;
    cpso = NULL;
    EXPECT(ID3D12Device2_CreatePipelineState(dev2, &csd, &IID_ID3D12PipelineState, (void **)&cpso) == E_INVALIDARG && !cpso, "a VS+CS stream accepted");
    cat = 0;
    put(cstream, &cat, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &crs, sizeof crs, 8);
    put(cstream, &cat, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &crs, sizeof crs, 8);
    put(cstream, &cat, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS, &cbc, sizeof cbc, 8);
    csd.SizeInBytes = cat;
    EXPECT(ID3D12Device2_CreatePipelineState(dev2, &csd, &IID_ID3D12PipelineState, (void **)&cpso) == E_INVALIDARG, "a repeated subobject accepted");

    // targets: 4x RGBA8 (render target) and its single-sample resolve destination
    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 32, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 4, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    ID3D12Resource *msaa, *resolved;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&msaa));
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;                            // (a resolve destination needs no flag)
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RESOLVE_DEST, NULL, &IID_ID3D12Resource, (void **)&resolved));
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    ID3D12Device_CreateRenderTargetView(dev, msaa, NULL, rtv);
    float verts[3][6] = { { -0.5f, -0.5f, 1, 1, 1, 1 }, { 0, 0.5f, 1, 1, 1, 1 }, { 0.5f, -0.5f, 1, 1, 1, 1 } };
    ID3D12Resource *vb = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof verts, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 32, D3D12_RESOURCE_STATE_COPY_DEST);
    if (bad > before) return 1;
    void *p;
    CHECK(ID3D12Resource_Map(vb, 0, NULL, &p)); memcpy(p, verts, sizeof verts); ID3D12Resource_Unmap(vb, 0, NULL);

    // a list made closed, without an allocator: Reset gives it one
    ID3D12Device4 *dev4;
    CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device4, (void **)&dev4));
    ID3D12GraphicsCommandList4 *list;
    CHECK(ID3D12Device4_CreateCommandList1(dev4, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                           &IID_ID3D12GraphicsCommandList4, (void **)&list));
    EXPECT(ID3D12GraphicsCommandList4_Close(list) != S_OK, "Close of a list created closed succeeded");
    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    CHECK(ID3D12GraphicsCommandList4_Reset(list, alloc, pso));

    ID3D12GraphicsCommandList4_BeginEvent(list, 1, "frame", 6);       // (PIX_EVENT_ANSI_VERSION)
    ID3D12GraphicsCommandList4_SetMarker(list, 1, "start", 6);
    ID3D12GraphicsCommandList4_SetPredication(list, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
    ID3D12GraphicsCommandList4_DiscardResource(list, msaa, NULL);
    D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS sub = { 0, 0, 0, 0, { 0, 0, 32, 32 } };
    D3D12_RENDER_PASS_RENDER_TARGET_DESC target;
    memset(&target, 0, sizeof target);
    target.cpuDescriptor = rtv;
    target.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
    target.BeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target.BeginningAccess.Clear.ClearValue.Color[2] = target.BeginningAccess.Clear.ClearValue.Color[3] = 1;
    target.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
    target.EndingAccess.Resolve.pSrcResource = msaa;
    target.EndingAccess.Resolve.pDstResource = resolved;
    target.EndingAccess.Resolve.SubresourceCount = 1;
    target.EndingAccess.Resolve.pSubresourceParameters = &sub;
    target.EndingAccess.Resolve.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target.EndingAccess.Resolve.ResolveMode = D3D12_RESOLVE_MODE_AVERAGE;
    ID3D12GraphicsCommandList4_BeginRenderPass(list, 1, &target, NULL, D3D12_RENDER_PASS_FLAG_NONE);
    ID3D12GraphicsCommandList4_SetGraphicsRootSignature(list, rs);
    float tint[4] = { 1, 0.5f, 0, 1 };
    ID3D12GraphicsCommandList4_SetGraphicsRoot32BitConstants(list, 0, 4, tint, 0);
    D3D12_VIEWPORT vp = { 0, 0, 32, 32, 0, 1 };
    D3D12_RECT sc = { 0, 0, 32, 32 };
    ID3D12GraphicsCommandList4_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList4_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList4_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof verts, sizeof verts[0] };
    ID3D12GraphicsCommandList4_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList4_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList4_EndRenderPass(list);
    ID3D12GraphicsCommandList4_EndEvent(list);
    D3D12_RESOURCE_BARRIER barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ resolved, 0, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList4_ResourceBarrier(list, 1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { resolved, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList4_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList4_Close(list));
    ID3D12CommandQueue_BeginEvent(queue, 1, "submit", 7);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    ID3D12CommandQueue_EndEvent(queue);
    wait_queue(dev, queue);
    unsigned char *px;
    CHECK(ID3D12Resource_Map(rb, 0, NULL, (void **)&px));
    const unsigned char *centre = px + 16 * 256 + 16 * 4, *corner = px;
    int centre_ok = centre[0] == 255 && centre[1] >= 127 && centre[1] <= 128 && centre[2] == 0 && centre[3] == 255;
    int corner_ok = corner[0] == 0 && corner[1] == 0 && corner[2] == 255 && corner[3] == 255;
    EXPECT(centre_ok && corner_ok, "render pass: centre (%u, %u, %u, %u), corner (%u, %u, %u, %u)",
           centre[0], centre[1], centre[2], centre[3], corner[0], corner[1], corner[2], corner[3]);
    ID3D12Resource_Unmap(rb, 0, NULL);

    // an EndRenderPass without a pass makes the list invalid
    CHECK(ID3D12CommandAllocator_Reset(alloc));
    CHECK(ID3D12GraphicsCommandList4_Reset(list, alloc, NULL));
    ID3D12GraphicsCommandList4_EndRenderPass(list);
    EXPECT(ID3D12GraphicsCommandList4_Close(list) == E_INVALIDARG, "EndRenderPass without BeginRenderPass accepted");
    printf("d3d12 engine render pass: pipeline state streams (graphics with defaults, compute; mixed and repeated rejected), "
           "CreateCommandList1, PIX events, clear + 4x resolve as begin/end accesses, centre (%u, %u, %u, %u): %s\n",
           centre[0], centre[1], centre[2], centre[3], bad == before ? "ok" : "FAILED");
    ID3D12GraphicsCommandList4_Release(list);
    ID3D12CommandAllocator_Release(alloc);
    ID3D12Device4_Release(dev4);
    ID3D12Device2_Release(dev2);
    return 0;
}

// ---- SetEventOnMultipleFenceCompletion ---------------------------------------------------------------------------------
static int multiple_fence_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    int before = bad;
    ID3D12Device1 *dev1;
    CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device1, (void **)&dev1));
    ID3D12Fence *fences[2];
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fences[0]));
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fences[1]));
    UINT64 values[2] = { 1, 2 };
    HANDLE all = CreateEventA(NULL, FALSE, FALSE, NULL), any = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(ID3D12Device1_SetEventOnMultipleFenceCompletion(dev1, fences, values, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, all));
    CHECK(ID3D12Device1_SetEventOnMultipleFenceCompletion(dev1, fences, values, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY, any));
    EXPECT(WaitForSingleObject(all, 0) == WAIT_TIMEOUT && WaitForSingleObject(any, 0) == WAIT_TIMEOUT, "set before any fence moved");
    CHECK(ID3D12CommandQueue_Signal(queue, fences[0], 1));
    EXPECT(WaitForSingleObject(any, 5000) == WAIT_OBJECT_0, "ANY not set after one fence");
    EXPECT(WaitForSingleObject(all, 100) == WAIT_TIMEOUT, "ALL set after one fence");
    CHECK(ID3D12Fence_Signal(fences[1], 2));                       // (from the CPU)
    EXPECT(WaitForSingleObject(all, 5000) == WAIT_OBJECT_0, "ALL not set after both fences");
    // both reached already: the event is set at once; no event: the call returns when they are
    CHECK(ID3D12Device1_SetEventOnMultipleFenceCompletion(dev1, fences, values, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, all));
    EXPECT(WaitForSingleObject(all, 0) == WAIT_OBJECT_0, "not set for fences already complete");
    values[0] = 3;
    CHECK(ID3D12CommandQueue_Signal(queue, fences[0], 3));
    CHECK(ID3D12Device1_SetEventOnMultipleFenceCompletion(dev1, fences, values, 2, D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL, NULL));
    EXPECT(ID3D12Fence_GetCompletedValue(fences[0]) >= 3, "returned before the fence reached 3");
    printf("d3d12 engine fences: SetEventOnMultipleFenceCompletion ALL/ANY across a queue and a CPU signal, waiting without an event: %s\n",
           bad == before ? "ok" : "FAILED");
    CloseHandle(all); CloseHandle(any);
    ID3D12Fence_Release(fences[0]); ID3D12Fence_Release(fences[1]);
    ID3D12Device1_Release(dev1);
    return 0;
}

// ---- a flip swap chain with a frame latency waitable object -------------------------------------------------------------
// The engine loop: wait on the object, render, Present. With a maximum latency of 2 the object lets two frames through
// before a present, and lets the next one through when the GPU has drawn a present.
static int swapchain_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    int before = bad;
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "vxrt_d3d12_engine";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "vxrt_d3d12_engine", "D3D12 engine", WS_OVERLAPPEDWINDOW, 0, 0, 160, 120, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("swap chain: no window\n"); return ++bad; }
    IDXGIFactory6 *factory;
    CHECK(CreateDXGIFactory2(0, &IID_IDXGIFactory6, (void **)&factory));
    DXGI_SWAP_CHAIN_DESC1 sd = { 128, 96, DXGI_FORMAT_R10G10B10A2_UNORM, FALSE, { 1, 0 }, DXGI_USAGE_RENDER_TARGET_OUTPUT, 3,
                                 DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED,
                                 DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING };
    IDXGISwapChain1 *sc1;
    CHECK(IDXGIFactory6_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc1));
    IDXGISwapChain4 *sc;
    CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain4, (void **)&sc));
    CHECK(IDXGISwapChain4_SetHDRMetaData(sc, DXGI_HDR_METADATA_TYPE_NONE, 0, NULL));
    UINT cs_support = 0;
    CHECK(IDXGISwapChain4_CheckColorSpaceSupport(sc, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &cs_support));
    EXPECT(!cs_support, "HDR10 color space reported as presentable");
    IDXGIOutput *output;
    CHECK(IDXGISwapChain4_GetContainingOutput(sc, &output));
    IDXGIOutput6 *output6;
    CHECK(IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput6, (void **)&output6));
    IDXGIOutput6_Release(output6);
    IDXGIOutput_Release(output);

    CHECK(IDXGISwapChain4_SetMaximumFrameLatency(sc, 2));
    UINT latency = 0;
    CHECK(IDXGISwapChain4_GetMaximumFrameLatency(sc, &latency));
    HANDLE waitable = IDXGISwapChain4_GetFrameLatencyWaitableObject(sc);
    EXPECT(latency == 2 && waitable, "latency %u, object %p", latency, waitable);
    DWORD w1 = WaitForSingleObject(waitable, 1000), w2 = WaitForSingleObject(waitable, 1000), w3 = WaitForSingleObject(waitable, 0);
    EXPECT(w1 == WAIT_OBJECT_0 && w2 == WAIT_OBJECT_0 && w3 == WAIT_TIMEOUT, "before presents: waits %lu %lu %lu, want 0 0 timeout",
           w1, w2, w3);

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
    ID3D12DescriptorHeap *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Resource *back[3];
    for (UINT i = 0; i < 3; i++) {
        CHECK(IDXGISwapChain4_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&back[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = { rtv0.ptr + i * inc };
        ID3D12Device_CreateRenderTargetView(dev, back[i], NULL, h);
    }
    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    ID3D12GraphicsCommandList4 *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList4, (void **)&list));
    ID3D12GraphicsCommandList4_Close(list);
    ID3D12Fence *frame_fence;
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&frame_fence));
    int frames = 0, timeouts = 0;
    for (UINT f = 0; f < 6; f++) {
        if (f >= 2 && WaitForSingleObject(waitable, 2000) != WAIT_OBJECT_0) timeouts++;   // (frames 0 and 1 took theirs above)
        if (f) ID3D12Fence_SetEventOnCompletion(frame_fence, f, NULL);                       // the allocator is free again
        UINT i = IDXGISwapChain4_GetCurrentBackBufferIndex(sc);
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList4_Reset(list, alloc, NULL);
        D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        b.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ back[i], 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        ID3D12GraphicsCommandList4_ResourceBarrier(list, 1, &b);
        D3D12_RENDER_PASS_RENDER_TARGET_DESC target;
        memset(&target, 0, sizeof target);
        target.cpuDescriptor.ptr = rtv0.ptr + i * inc;
        target.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
        target.BeginningAccess.Clear.ClearValue.Color[0] = (float)f / 6;
        target.BeginningAccess.Clear.ClearValue.Color[3] = 1;
        target.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
        ID3D12GraphicsCommandList4_BeginRenderPass(list, 1, &target, NULL, D3D12_RENDER_PASS_FLAG_NONE);
        ID3D12GraphicsCommandList4_EndRenderPass(list);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList4_ResourceBarrier(list, 1, &b);
        CHECK(ID3D12GraphicsCommandList4_Close(list));
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
        CHECK(IDXGISwapChain4_Present(sc, 0, DXGI_PRESENT_ALLOW_TEARING));
        CHECK(ID3D12CommandQueue_Signal(queue, frame_fence, f + 1));
        frames++;
    }
    EXPECT(!timeouts, "%d of 4 waits on the latency object timed out", timeouts);
    wait_queue(dev, queue);
    UINT presents = 0;
    IDXGISwapChain4_GetLastPresentCount(sc, &presents);
    EXPECT(presents == 6, "%u presents", presents);
    printf("d3d12 engine swap chain: 10-bit flip-discard, tearing flag, IDXGISwapChain4, containing output 6, latency 2 "
           "lets 2 frames through then one per drawn present, %d frames: %s\n", frames, bad == before ? "ok" : "FAILED");
    for (UINT i = 0; i < 3; i++) ID3D12Resource_Release(back[i]);
    ID3D12GraphicsCommandList4_Release(list);
    ID3D12CommandAllocator_Release(alloc);
    IDXGISwapChain4_Release(sc);
    IDXGISwapChain1_Release(sc1);
    IDXGIFactory6_Release(factory);
    DestroyWindow(hwnd);
    return 0;
}

int main(void) {
    IDXGIAdapter4 *adapter = NULL;
    if (dxgi_test(&adapter)) return 1;
    ID3D12Device *dev = NULL;
    CHECK(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    if (feature_test(dev)) return 1;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *queue;
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    if (render_pass_test(dev, queue)) return 1;
    if (multiple_fence_test(dev, queue)) return 1;
    if (swapchain_test(dev, queue)) return 1;
    return bad ? 1 : 0;
}
