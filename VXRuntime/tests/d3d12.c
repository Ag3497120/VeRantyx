// Direct3D 12 on VXRuntime: feature queries, compute and graphics pipelines, DXIL, root signatures, shader-visible
// descriptors, heaps and placed resources, occlusion/timestamp queries, and a DXGI swap chain on a D3D12 queue.
// Checks resource contents and draw output against the expected values of these ordinary D3D12 operations.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS                 // (methods returning structures, as MSVC calls them: a hidden result pointer)
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stdint.h>
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

static int cross_queue_fence_test(ID3D12Device *dev) {
    D3D12_COMMAND_QUEUE_DESC producer_desc = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    D3D12_COMMAND_QUEUE_DESC consumer_desc = { D3D12_COMMAND_LIST_TYPE_COMPUTE };
    ID3D12CommandQueue *producer = NULL, *consumer = NULL;
    ID3D12Fence *ready = NULL, *done = NULL;
    HANDLE event = NULL;
    HRESULT hr = ID3D12Device_CreateCommandQueue(dev, &producer_desc, &IID_ID3D12CommandQueue,
                                                  (void **)&producer);
    if (SUCCEEDED(hr))
        hr = ID3D12Device_CreateCommandQueue(dev, &consumer_desc, &IID_ID3D12CommandQueue,
                                             (void **)&consumer);
    if (SUCCEEDED(hr))
        hr = ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                      (void **)&ready);
    if (SUCCEEDED(hr))
        hr = ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
                                      (void **)&done);
    if (SUCCEEDED(hr)) event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (SUCCEEDED(hr) && !event) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = ID3D12CommandQueue_Wait(consumer, ready, 1);
    if (SUCCEEDED(hr)) hr = ID3D12CommandQueue_Signal(producer, ready, 1);
    if (SUCCEEDED(hr)) hr = ID3D12CommandQueue_Signal(consumer, done, 1);
    if (SUCCEEDED(hr)) hr = ID3D12Fence_SetEventOnCompletion(done, 1, event);
    DWORD wait = SUCCEEDED(hr) ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
    int failed = FAILED(hr) || wait != WAIT_OBJECT_0;
    if (failed)
        printf("d3d12 cross-queue fence wait/signal: HRESULT %#lx, wait %lu: FAILED\n",
               (unsigned long)hr, (unsigned long)wait);
    else
        puts("d3d12 cross-queue fence wait/signal: consumer wait unblocked by a producer queue: ok");
    if (event) CloseHandle(event);
    if (done) ID3D12Fence_Release(done);
    if (ready) ID3D12Fence_Release(ready);
    if (consumer) ID3D12CommandQueue_Release(consumer);
    if (producer) ID3D12CommandQueue_Release(producer);
    return failed;
}

static int fence_flags_test(ID3D12Device *dev) {
    ID3D12Fence *fence = (ID3D12Fence *)(uintptr_t)1;
    HRESULT unknown = ID3D12Device_CreateFence(dev, 0, (D3D12_FENCE_FLAGS)0x80000000u,
        &IID_ID3D12Fence, (void **)&fence);
    if (unknown != E_INVALIDARG || fence) {
        printf("d3d12 fence flags: unknown bits returned %#lx, fence %p: FAILED\n",
               (unsigned long)unknown, (void *)fence);
        return 1;
    }

    const D3D12_FENCE_FLAGS unsupported_flags[] = {
        D3D12_FENCE_FLAG_SHARED,
        D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER,
        D3D12_FENCE_FLAG_NON_MONITORED
    };
    for (size_t i = 0; i < sizeof unsupported_flags / sizeof unsupported_flags[0]; i++) {
        fence = (ID3D12Fence *)(uintptr_t)1;
        HRESULT hr = ID3D12Device_CreateFence(dev, 0, unsupported_flags[i],
            &IID_ID3D12Fence, (void **)&fence);
        if (hr != DXGI_ERROR_UNSUPPORTED || fence) {
            printf("d3d12 fence flags: unsupported flag %#x returned %#lx, fence %p: FAILED\n",
                   (unsigned)unsupported_flags[i], (unsigned long)hr, (void *)fence);
            return 1;
        }
    }
    puts("d3d12 fence flags: local monitored mode supported; unknown bits invalid and unimplemented modes rejected: ok");
    return 0;
}

static int queue_description_test(ID3D12Device *dev) {
    D3D12_COMMAND_QUEUE_DESC desc = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *queue = NULL;
    HRESULT invalid_priority, invalid_flags, invalid_node, realtime_priority;
    HRESULT hr = ID3D12Device_CreateCommandQueue(dev, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    if (FAILED(hr) || !queue) {
        printf("d3d12 queue description: default queue creation failed: %#lx\n", (unsigned long)hr);
        return 1;
    }
    ID3D12CommandQueue_Release(queue);

    desc.Priority = 1;
    invalid_priority = ID3D12Device_CreateCommandQueue(dev, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = (D3D12_COMMAND_QUEUE_FLAGS)2;
    invalid_flags = ID3D12Device_CreateCommandQueue(dev, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 1;
    invalid_node = ID3D12Device_CreateCommandQueue(dev, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    desc.NodeMask = 0;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
    realtime_priority = ID3D12Device_CreateCommandQueue(dev, &desc, &IID_ID3D12CommandQueue, (void **)&queue);

    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
    hr = ID3D12Device_CreateCommandQueue(dev, &desc, &IID_ID3D12CommandQueue, (void **)&queue);
    int failed = invalid_priority != E_INVALIDARG || invalid_flags != E_INVALIDARG || invalid_node != E_INVALIDARG ||
                 realtime_priority != DXGI_ERROR_UNSUPPORTED || FAILED(hr) || !queue;
    printf("d3d12 queue description: priority/flags/node validation and unsupported global real-time priority: %s\n",
           failed ? "FAILED" : "ok");
    if (failed)
        printf("  priority=%#lx flags=%#lx node=%#lx realtime=%#lx valid-high=%#lx\n",
               (unsigned long)invalid_priority, (unsigned long)invalid_flags, (unsigned long)invalid_node,
               (unsigned long)realtime_priority, (unsigned long)hr);
    if (queue) ID3D12CommandQueue_Release(queue);
    return failed;
}

static int command_allocator_lifetime_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL, *conflicting_list = NULL;
    ID3D12Fence *gate = NULL;
    HRESULT hr = ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator);
    if (SUCCEEDED(hr))
        hr = ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list);
    if (SUCCEEDED(hr))
        hr = ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&gate);
    if (FAILED(hr)) {
        printf("d3d12 command allocator setup failed: %#lx\n", (unsigned long)hr);
        if (gate) ID3D12Fence_Release(gate);
        if (list) ID3D12GraphicsCommandList_Release(list);
        if (allocator) ID3D12CommandAllocator_Release(allocator);
        return 1;
    }

    HRESULT recording_reset = ID3D12CommandAllocator_Reset(allocator);
    HRESULT duplicate_recording = ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator, NULL, &IID_ID3D12GraphicsCommandList, (void **)&conflicting_list);
    if (conflicting_list) ID3D12GraphicsCommandList_Release(conflicting_list);
    hr = ID3D12GraphicsCommandList_Close(list);
    if (SUCCEEDED(hr)) hr = ID3D12CommandQueue_Wait(queue, gate, 1);
    if (SUCCEEDED(hr)) {
        ID3D12CommandList *submitted[] = { (ID3D12CommandList *)list };
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, submitted);
    }
    HRESULT in_flight_reset = SUCCEEDED(hr) ? ID3D12CommandAllocator_Reset(allocator) : hr;
    HRESULT signal = ID3D12Fence_Signal(gate, 1);
    if (SUCCEEDED(signal)) wait_queue(dev, queue);
    HRESULT completed_reset = SUCCEEDED(signal) ? ID3D12CommandAllocator_Reset(allocator) : signal;
    HRESULT list_reset = SUCCEEDED(completed_reset) ? ID3D12GraphicsCommandList_Reset(list, allocator, NULL) : completed_reset;
    HRESULT recording_again = SUCCEEDED(list_reset) ? ID3D12CommandAllocator_Reset(allocator) : list_reset;
    HRESULT close_again = SUCCEEDED(list_reset) ? ID3D12GraphicsCommandList_Close(list) : list_reset;
    HRESULT reset_after_close = SUCCEEDED(close_again) ? ID3D12CommandAllocator_Reset(allocator) : close_again;
    int failed = recording_reset != E_FAIL || duplicate_recording != E_INVALIDARG || FAILED(hr) ||
        in_flight_reset != E_FAIL || FAILED(signal) || completed_reset != S_OK || FAILED(list_reset) ||
        recording_again != E_FAIL || FAILED(close_again) || reset_after_close != S_OK;
    printf("d3d12 command allocator: one recording list, in-flight protection, reuse after completion: %s\n",
           failed ? "FAILED" : "ok");
    if (failed)
        printf("  recording=%#lx second-list=%#lx close=%#lx in-flight=%#lx signal=%#lx completed=%#lx list-reset=%#lx recording-again=%#lx close-again=%#lx final=%#lx\n",
               (unsigned long)recording_reset, (unsigned long)duplicate_recording, (unsigned long)hr,
               (unsigned long)in_flight_reset, (unsigned long)signal, (unsigned long)completed_reset,
               (unsigned long)list_reset, (unsigned long)recording_again, (unsigned long)close_again,
               (unsigned long)reset_after_close);
    ID3D12Fence_Release(gate);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    return failed;
}

static void put_u32(unsigned char *data, size_t offset, uint32_t value) { memcpy(data + offset, &value, sizeof value); }

static int root_signature_validation_test(ID3D12Device *dev) {
    unsigned char data[64] = { 0 };
    void *root = NULL;
    put_u32(data, 0, 1); put_u32(data, 8, 24);
    HRESULT hr = ID3D12Device_CreateRootSignature(dev, 0, data, 24, &IID_ID3D12RootSignature, &root);
    if (FAILED(hr) || !root) { printf("D3D12 rejected an empty root signature: %#lx\n", (unsigned long)hr); return ++bad; }
    ID3D12RootSignature_Release(root);

    typedef struct { const char *name; const unsigned char *data; size_t size; } invalid_root_case;
    invalid_root_case cases[14];
    unsigned char short_header[23] = { 0 };
    unsigned char bad_version[24] = { 0 };
    unsigned char bad_params_offset[24] = { 0 };
    unsigned char bad_payload[36] = { 0 };
    unsigned char truncated_ranges[44] = { 0 };
    unsigned char oversized_constants[48] = { 0 };
    unsigned char mixed_table[84] = { 0 };
    unsigned char overlapping_table_ranges[84] = { 0 };
    unsigned char duplicate_registers[64] = { 0 };
    unsigned char all_stage_conflict[64] = { 0 };
    unsigned char stage_aliases[64] = { 0 };
    unsigned char unknown_root_flags[24] = { 0 };
    unsigned char local_root_flags_conflict[24] = { 0 };
    unsigned char invalid_range_flags[68] = { 0 };
    unsigned char invalid_root_descriptor_flags[48] = { 0 };
    put_u32(bad_version, 0, 3); put_u32(bad_version, 8, 24);
    put_u32(bad_params_offset, 0, 1); put_u32(bad_params_offset, 4, 1); put_u32(bad_params_offset, 8, UINT32_MAX - 4);
    put_u32(bad_payload, 0, 1); put_u32(bad_payload, 4, 1); put_u32(bad_payload, 8, 24);
    put_u32(bad_payload, 24, 0); put_u32(bad_payload, 28, 0); put_u32(bad_payload, 32, UINT32_MAX - 3);
    put_u32(truncated_ranges, 0, 1); put_u32(truncated_ranges, 4, 1); put_u32(truncated_ranges, 8, 24);
    put_u32(truncated_ranges, 24, 0); put_u32(truncated_ranges, 28, 0); put_u32(truncated_ranges, 32, 36);
    put_u32(truncated_ranges, 36, 1); put_u32(truncated_ranges, 40, 44);
    put_u32(oversized_constants, 0, 1); put_u32(oversized_constants, 4, 1); put_u32(oversized_constants, 8, 24);
    put_u32(oversized_constants, 24, 1); put_u32(oversized_constants, 28, 0); put_u32(oversized_constants, 32, 36);
    put_u32(oversized_constants, 44, 65);
    put_u32(mixed_table, 0, 1); put_u32(mixed_table, 4, 1); put_u32(mixed_table, 8, 24);
    put_u32(mixed_table, 24, 0); put_u32(mixed_table, 28, 0); put_u32(mixed_table, 32, 36);
    put_u32(mixed_table, 36, 2); put_u32(mixed_table, 40, 44);
    put_u32(mixed_table, 44, 0); put_u32(mixed_table, 48, 1); put_u32(mixed_table, 56, 0);
    put_u32(mixed_table, 64, 3); put_u32(mixed_table, 68, 1); put_u32(mixed_table, 80, 1);
    put_u32(overlapping_table_ranges, 0, 1); put_u32(overlapping_table_ranges, 4, 1); put_u32(overlapping_table_ranges, 8, 24);
    put_u32(overlapping_table_ranges, 24, 0); put_u32(overlapping_table_ranges, 28, 0); put_u32(overlapping_table_ranges, 32, 36);
    put_u32(overlapping_table_ranges, 36, 2); put_u32(overlapping_table_ranges, 40, 44);
    put_u32(overlapping_table_ranges, 44, 0); put_u32(overlapping_table_ranges, 48, 2); put_u32(overlapping_table_ranges, 56, 0);
    put_u32(overlapping_table_ranges, 64, 1); put_u32(overlapping_table_ranges, 68, 1); put_u32(overlapping_table_ranges, 80, 1);
    for (int i = 0; i < 3; i++) {
        unsigned char *data = i == 0 ? duplicate_registers : i == 1 ? all_stage_conflict : stage_aliases;
        put_u32(data, 0, 1); put_u32(data, 4, 2); put_u32(data, 8, 24);
        put_u32(data, 24, 3); put_u32(data, 28, i == 2 ? 1 : 0); put_u32(data, 32, 48);
        put_u32(data, 36, 3); put_u32(data, 40, i == 2 ? 5 : i == 1 ? 1 : 0); put_u32(data, 44, 56);
        put_u32(data, 48, 0); put_u32(data, 56, 0);
    }
    put_u32(unknown_root_flags, 0, 2); put_u32(unknown_root_flags, 8, 24); put_u32(unknown_root_flags, 20, 0x1000);
    put_u32(local_root_flags_conflict, 0, 2); put_u32(local_root_flags_conflict, 8, 24); put_u32(local_root_flags_conflict, 20, 0x81);
    put_u32(invalid_range_flags, 0, 2); put_u32(invalid_range_flags, 4, 1); put_u32(invalid_range_flags, 8, 24);
    put_u32(invalid_range_flags, 24, 0); put_u32(invalid_range_flags, 28, 0); put_u32(invalid_range_flags, 32, 36);
    put_u32(invalid_range_flags, 36, 1); put_u32(invalid_range_flags, 40, 44);
    put_u32(invalid_range_flags, 44, 0); put_u32(invalid_range_flags, 48, 1); put_u32(invalid_range_flags, 60, 0x10);
    put_u32(invalid_root_descriptor_flags, 0, 2); put_u32(invalid_root_descriptor_flags, 4, 1);
    put_u32(invalid_root_descriptor_flags, 8, 24); put_u32(invalid_root_descriptor_flags, 24, 2);
    put_u32(invalid_root_descriptor_flags, 32, 36); put_u32(invalid_root_descriptor_flags, 44, 0x1);
    cases[0] = (invalid_root_case){ "short header", short_header, sizeof short_header };
    cases[1] = (invalid_root_case){ "unknown version", bad_version, sizeof bad_version };
    cases[2] = (invalid_root_case){ "parameter array offset", bad_params_offset, sizeof bad_params_offset };
    cases[3] = (invalid_root_case){ "parameter payload offset", bad_payload, sizeof bad_payload };
    cases[4] = (invalid_root_case){ "truncated descriptor ranges", truncated_ranges, sizeof truncated_ranges };
    cases[5] = (invalid_root_case){ "root constants exceeding 64 DWORDs", oversized_constants, sizeof oversized_constants };
    cases[6] = (invalid_root_case){ "sampler and resource ranges in one table", mixed_table, sizeof mixed_table };
    cases[7] = (invalid_root_case){ "overlapping descriptor-table slots of different types", overlapping_table_ranges, sizeof overlapping_table_ranges };
    cases[8] = (invalid_root_case){ "duplicate shader registers", duplicate_registers, sizeof duplicate_registers };
    cases[9] = (invalid_root_case){ "ALL visibility overlapping a stage binding", all_stage_conflict, sizeof all_stage_conflict };
    cases[10] = (invalid_root_case){ "unknown root-signature flags", unknown_root_flags, sizeof unknown_root_flags };
    cases[11] = (invalid_root_case){ "local root signature combined with graphics flags", local_root_flags_conflict, sizeof local_root_flags_conflict };
    cases[12] = (invalid_root_case){ "invalid v1.1 descriptor-range flags", invalid_range_flags, sizeof invalid_range_flags };
    cases[13] = (invalid_root_case){ "invalid v1.1 root-descriptor flags", invalid_root_descriptor_flags, sizeof invalid_root_descriptor_flags };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        root = (void *)(uintptr_t)1;
        hr = ID3D12Device_CreateRootSignature(dev, 0, cases[i].data, cases[i].size,
                                               &IID_ID3D12RootSignature, &root);
        if (hr != E_INVALIDARG || root) {
            printf("D3D12 root signature parser accepted %s (hr=%#lx, root=%p)\n", cases[i].name,
                   (unsigned long)hr, root);
            bad++;
        }
    }
    root = NULL;
    hr = ID3D12Device_CreateRootSignature(dev, 0, stage_aliases, sizeof stage_aliases,
                                           &IID_ID3D12RootSignature, &root);
    if (FAILED(hr) || !root) {
        printf("D3D12 rejected same-register bindings with disjoint stage visibility: %#lx\n", (unsigned long)hr);
        bad++;
    } else ID3D12RootSignature_Release(root);
    unsigned char volatile_range_root[68] = { 0 };
    put_u32(volatile_range_root, 0, 2); put_u32(volatile_range_root, 4, 1); put_u32(volatile_range_root, 8, 24);
    put_u32(volatile_range_root, 24, 0); put_u32(volatile_range_root, 28, 0); put_u32(volatile_range_root, 32, 36);
    put_u32(volatile_range_root, 36, 1); put_u32(volatile_range_root, 40, 44);
    put_u32(volatile_range_root, 44, 0); put_u32(volatile_range_root, 48, 1); put_u32(volatile_range_root, 60, 0x3);
    root = NULL;
    hr = ID3D12Device_CreateRootSignature(dev, 0, volatile_range_root, sizeof volatile_range_root,
                                           &IID_ID3D12RootSignature, &root);
    if (FAILED(hr) || !root) {
        printf("D3D12 rejected valid v1.1 descriptor volatility flags: %#lx\n", (unsigned long)hr);
        bad++;
    } else ID3D12RootSignature_Release(root);
    unsigned char direct_heap_root[24] = { 0 };
    put_u32(direct_heap_root, 0, 2); put_u32(direct_heap_root, 8, 24); put_u32(direct_heap_root, 20, 0xC00);
    root = NULL;
    hr = ID3D12Device_CreateRootSignature(dev, 0, direct_heap_root, sizeof direct_heap_root,
                                           &IID_ID3D12RootSignature, &root);
    if (FAILED(hr) || !root) {
        printf("D3D12 rejected the v1.1 direct descriptor heap flags: %#lx\n", (unsigned long)hr);
        bad++;
    } else ID3D12RootSignature_Release(root);
    printf("d3d12 root signatures: malformed ranges, register conflicts and root-cost overflow rejected; disjoint stage aliases accepted: %s\n",
           bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}

// SM 6.6's ResourceDescriptorHeap intrinsic must be opted into by the root signature.
static int direct_heap_root_flag_test(ID3D12Device *dev) {
    static unsigned char vs[65536], ps[65536];
    size_t vs_size, ps_size;
    if (load("tests/shaders12/texq_vs.dxil", vs, sizeof vs, &vs_size) ||
        load("tests/shaders12/bindless_ps.dxil", ps, sizeof ps, &ps_size)) return 1;

    D3D12_ROOT_SIGNATURE_DESC rsd = { 0 };
    ID3DBlob *blob = NULL, *errors = NULL;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ID3D12RootSignature *root = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob),
                                            ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                            (void **)&root));
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = root;
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
    ID3D12PipelineState *pso = NULL;
    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso);
    if (hr != E_INVALIDARG || pso) {
        printf("D3D12 accepted ResourceDescriptorHeap without the root-signature opt-in (hr=%#lx, pso=%p)\n",
               (unsigned long)hr, pso);
        bad++;
        if (pso) ID3D12PipelineState_Release(pso);
    }
    ID3D12RootSignature_Release(root);
    ID3D10Blob_Release(blob);
    printf("d3d12 bindless validation: SM 6.6 ResourceDescriptorHeap requires the root-signature flag: %s\n",
           hr == E_INVALIDARG && !pso ? "ok" : "FAILED");
    return bad ? 1 : 0;
}

static int graphics_pso_state_test(ID3D12Device *dev) {
    static unsigned char vs[65536], ps[65536];
    size_t vs_size, ps_size;
    if (load("tests/shaders12/tri_vs.dxil", vs, sizeof vs, &vs_size) ||
        load("tests/shaders12/tri_ps.dxil", ps, sizeof ps, &ps_size)) return 1;
    D3D12_ROOT_PARAMETER root_param = { D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS };
    root_param.Constants.ShaderRegister = 0;
    root_param.Constants.Num32BitValues = 4;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = { 1, &root_param, 0, NULL,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob *blob = NULL, *errors = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    ID3D12RootSignature *root = NULL;
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&root);
    if (errors) ID3D10Blob_Release(errors);
    if (blob) ID3D10Blob_Release(blob);
    if (FAILED(hr) || !root) { printf("D3D12 graphics PSO state test could not create its root signature: %#lx\n", (unsigned long)hr); return 1; }

    D3D12_INPUT_ELEMENT_DESC elements[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    memset(&desc, 0, sizeof desc);
    desc.pRootSignature = root;
    desc.VS = (D3D12_SHADER_BYTECODE){ vs, vs_size };
    desc.PS = (D3D12_SHADER_BYTECODE){ ps, ps_size };
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = 0xFFFFFFFF;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.InputLayout = (D3D12_INPUT_LAYOUT_DESC){ elements, 2 };
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    int failed = 0;
    ID3D12PipelineState *pso = NULL;
    desc.BlendState.RenderTarget[0].LogicOpEnable = TRUE;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_AND;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &desc, &IID_ID3D12PipelineState, (void **)&pso);
    failed |= hr != E_NOTIMPL || pso != NULL;
    if (pso) { ID3D12PipelineState_Release(pso); pso = NULL; }

    desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &desc, &IID_ID3D12PipelineState, (void **)&pso);
    failed |= hr != E_INVALIDARG || pso != NULL;
    if (pso) { ID3D12PipelineState_Release(pso); pso = NULL; }
    desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    desc.BlendState.RenderTarget[0].LogicOpEnable = FALSE;

    desc.RasterizerState.ForcedSampleCount = 2;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &desc, &IID_ID3D12PipelineState, (void **)&pso);
    failed |= hr != E_INVALIDARG || pso != NULL;
    desc.RasterizerState.ForcedSampleCount = 4;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &desc, &IID_ID3D12PipelineState, (void **)&pso);
    failed |= hr != E_NOTIMPL || pso != NULL;
    desc.RasterizerState.ForcedSampleCount = 0;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &desc, &IID_ID3D12PipelineState, (void **)&pso);
    failed |= hr != E_NOTIMPL || pso != NULL;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    desc.SampleMask = 0x5A;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &desc, &IID_ID3D12PipelineState, (void **)&pso);
    failed |= FAILED(hr) || !pso;
    if (pso) { ID3D12PipelineState_Release(pso); pso = NULL; }
    desc.PS = (D3D12_SHADER_BYTECODE){ NULL, 0 };
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &desc, &IID_ID3D12PipelineState, (void **)&pso);
    failed |= hr != E_NOTIMPL || pso != NULL;
    if (pso) ID3D12PipelineState_Release(pso);

    ID3D12RootSignature_Release(root);
    printf("d3d12 graphics PSO: static sample mask compiles; unsupported logic/conservative/forced-sample states are rejected: %s\n",
           failed ? "FAILED" : "ok");
    return failed;
}

// Feature queries must describe the backend's actual limits so an application can select a valid shader path.
static int feature_test(ID3D12Device *dev, BOOL *copy_timestamps_supported) {
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels = { 4, levels, 0x12345678 };
    HRESULT feature_levels_hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS,
                                                                  &feature_levels, sizeof feature_levels);
    D3D_FEATURE_LEVEL all_levels_max = feature_levels.MaxSupportedFeatureLevel;
    int feature_levels_ok = feature_levels_hr == S_OK &&
                            all_levels_max == D3D_FEATURE_LEVEL_12_0;

    D3D_FEATURE_LEVEL sparse_levels[] = { D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_11_1 };
    feature_levels = (D3D12_FEATURE_DATA_FEATURE_LEVELS){ 2, sparse_levels, 0x12345678 };
    feature_levels_hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS,
                                                         &feature_levels, sizeof feature_levels);
    D3D_FEATURE_LEVEL sparse_levels_max = feature_levels.MaxSupportedFeatureLevel;
    feature_levels_ok &= feature_levels_hr == S_OK &&
                         sparse_levels_max == D3D_FEATURE_LEVEL_11_1;

    D3D_FEATURE_LEVEL unsupported_levels[] = { D3D_FEATURE_LEVEL_12_1 };
    feature_levels = (D3D12_FEATURE_DATA_FEATURE_LEVELS){ 1, unsupported_levels, 0x12345678 };
    feature_levels_hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS,
                                                         &feature_levels, sizeof feature_levels);
    D3D_FEATURE_LEVEL unsupported_levels_max = feature_levels.MaxSupportedFeatureLevel;
    feature_levels_ok &= feature_levels_hr == S_OK && unsupported_levels_max == 0;

    feature_levels = (D3D12_FEATURE_DATA_FEATURE_LEVELS){ 0, levels, 0x12345678 };
    HRESULT empty_feature_levels_hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS,
                                                                        &feature_levels, sizeof feature_levels);
    feature_levels = (D3D12_FEATURE_DATA_FEATURE_LEVELS){ 1, NULL, 0x12345678 };
    HRESULT null_feature_levels_hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS,
                                                                       &feature_levels, sizeof feature_levels);
    feature_levels_ok &= empty_feature_levels_hr == E_INVALIDARG && null_feature_levels_hr == E_INVALIDARG;
    if (!feature_levels_ok) {
        printf("D3D12 feature-level query: all %#x, sparse %#x, unsupported %#x (hr %#lx, empty %#lx, null %#lx)\n",
               (unsigned)all_levels_max, (unsigned)sparse_levels_max, (unsigned)unsupported_levels_max,
               (unsigned long)feature_levels_hr,
               (unsigned long)empty_feature_levels_hr, (unsigned long)null_feature_levels_hr);
    }

    D3D12_FEATURE_DATA_SHADER_MODEL shader_model = { (D3D_SHADER_MODEL)0x69 };
    HRESULT shader_model_hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL,
                                                               &shader_model, sizeof shader_model);
    int shader_model_ok = shader_model_hr == S_OK && shader_model.HighestShaderModel == D3D_SHADER_MODEL_6_6;
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model_lower = { D3D_SHADER_MODEL_6_5 };
    HRESULT shader_model_lower_hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL,
                                                                     &shader_model_lower, sizeof shader_model_lower);
    shader_model_ok &= shader_model_lower_hr == S_OK &&
                       shader_model_lower.HighestShaderModel == D3D_SHADER_MODEL_6_5;
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model_unknown = { (D3D_SHADER_MODEL)0x6a };
    HRESULT shader_model_unknown_hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL,
                                                                        &shader_model_unknown,
                                                                        sizeof shader_model_unknown);
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model_gap = { (D3D_SHADER_MODEL)0x52 };
    HRESULT shader_model_gap_hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL,
                                                                    &shader_model_gap, sizeof shader_model_gap);
    shader_model_ok &= shader_model_unknown_hr == E_INVALIDARG &&
                       shader_model_unknown.HighestShaderModel == (D3D_SHADER_MODEL)0x6a &&
                       shader_model_gap_hr == E_INVALIDARG &&
                       shader_model_gap.HighestShaderModel == (D3D_SHADER_MODEL)0x52;
    if (!shader_model_ok)
        printf("D3D12 shader-model query: high %#lx/%#x, lower %#lx/%#x, unknown %#lx/%#x, gap %#lx/%#x\n",
               (unsigned long)shader_model_hr, (unsigned)shader_model.HighestShaderModel,
               (unsigned long)shader_model_lower_hr, (unsigned)shader_model_lower.HighestShaderModel,
               (unsigned long)shader_model_unknown_hr, (unsigned)shader_model_unknown.HighestShaderModel,
               (unsigned long)shader_model_gap_hr, (unsigned)shader_model_gap.HighestShaderModel);

    D3D12_FEATURE_DATA_D3D12_OPTIONS options = { 0 };
    HRESULT hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof options);
    if (FAILED(hr)) { printf("D3D12_OPTIONS query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    HRESULT sampler_heap_query_only_hr = ID3D12Device_CreateDescriptorHeap(dev, &sampler_heap_desc,
        &IID_ID3D12DescriptorHeap, NULL);
    ID3D12DescriptorHeap *max_sampler_heap = NULL;
    HRESULT max_sampler_heap_hr = ID3D12Device_CreateDescriptorHeap(dev, &sampler_heap_desc,
        &IID_ID3D12DescriptorHeap, (void **)&max_sampler_heap);
    if (max_sampler_heap) ID3D12DescriptorHeap_Release(max_sampler_heap);
    sampler_heap_desc.NumDescriptors++;
    ID3D12DescriptorHeap *oversized_sampler_heap = NULL;
    HRESULT oversized_sampler_heap_hr = ID3D12Device_CreateDescriptorHeap(dev, &sampler_heap_desc,
        &IID_ID3D12DescriptorHeap, (void **)&oversized_sampler_heap);
    if (oversized_sampler_heap) ID3D12DescriptorHeap_Release(oversized_sampler_heap);
    int sampler_heap_limit_ok = SUCCEEDED(max_sampler_heap_hr) && !oversized_sampler_heap &&
                                FAILED(oversized_sampler_heap_hr) && sampler_heap_query_only_hr == S_FALSE;

    D3D12_FEATURE_DATA_ARCHITECTURE architecture = { 0, FALSE, FALSE, FALSE };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof architecture);
    if (FAILED(hr)) { printf("D3D12 architecture query failed: %#lx\n", (unsigned long)hr); return ++bad; }
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture1 = { 0, FALSE, FALSE, FALSE, FALSE };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_ARCHITECTURE1, &architecture1, sizeof architecture1);
    if (FAILED(hr)) { printf("D3D12 architecture1 query failed: %#lx\n", (unsigned long)hr); return ++bad; }
    D3D12_FEATURE_DATA_ARCHITECTURE1 invalid_architecture = { 1, FALSE, FALSE, FALSE, FALSE };
    HRESULT invalid_architecture_hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_ARCHITECTURE1,
                                                                       &invalid_architecture, sizeof invalid_architecture);

    struct options1 {
        BOOL WaveOps;
        UINT WaveLaneCountMin, WaveLaneCountMax, TotalLaneCount;
        BOOL ExpandedComputeResourceStates, Int64ShaderOps;
    } options1 = { 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, (D3D12_FEATURE)8, &options1, sizeof options1);
    if (FAILED(hr)) { printf("D3D12_OPTIONS1 query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    struct options2 {
        BOOL DepthBoundsTestSupported;
        UINT ProgrammableSamplePositionsTier;
    } options2 = { 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, (D3D12_FEATURE)18, &options2, sizeof options2);
    if (FAILED(hr)) { printf("D3D12_OPTIONS2 query failed: %#lx\n", (unsigned long)hr); return ++bad; }
    struct { struct options2 value; UINT guard; } options2_oversized = { { 0x12345678, 0x12345678 }, 0x87654321 };
    HRESULT options2_large_hr = ID3D12Device_CheckFeatureSupport(dev, (D3D12_FEATURE)18,
                                                                  &options2_oversized.value,
                                                                  sizeof options2_oversized);
    struct options2 options2_undersized = { 0x12345678, 0x12345678 };
    HRESULT options2_small_hr = ID3D12Device_CheckFeatureSupport(dev, (D3D12_FEATURE)18,
                                                                  &options2_undersized,
                                                                  sizeof options2_undersized - sizeof(UINT));

    struct options3 {
        BOOL CopyQueueTimestampQueriesSupported;
        BOOL CastingFullyTypedFormatSupported;
        UINT WriteBufferImmediateSupportFlags;
        UINT ViewInstancingTier;
        BOOL BarycentricsSupported;
    } options3 = { 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, (D3D12_FEATURE)21, &options3, sizeof options3);
    if (FAILED(hr)) { printf("D3D12_OPTIONS3 query failed: %#lx\n", (unsigned long)hr); return ++bad; }
    struct { struct options3 value; UINT guard; } options3_oversized = {
        { 0x12345678, 0x12345678, 0x12345678, 0x12345678, 0x12345678 }, 0x87654321
    };
    HRESULT options3_large_hr = ID3D12Device_CheckFeatureSupport(dev, (D3D12_FEATURE)21,
                                                                  &options3_oversized.value,
                                                                  sizeof options3_oversized);
    struct options3 options3_undersized = { 0x12345678, 0x12345678, 0x12345678, 0x12345678, 0x12345678 };
    HRESULT options3_small_hr = ID3D12Device_CheckFeatureSupport(dev, (D3D12_FEATURE)21,
                                                                  &options3_undersized,
                                                                  sizeof options3_undersized - sizeof(BOOL));

    struct options21 {
        uint32_t work_graphs_tier, execute_indirect_tier;
        BOOL sample_cmp_gradient_and_bias, extended_command_info;
    } options21 = { 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, (D3D12_FEATURE)52, &options21, sizeof options21);
    if (FAILED(hr)) { printf("D3D12_OPTIONS21 query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT gpu_va = { 0, 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &gpu_va, sizeof gpu_va);
    if (FAILED(hr)) { printf("D3D12 GPU virtual-address query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msaa = {
        DXGI_FORMAT_R8G8B8A8_UNORM, 4, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0
    };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &msaa, sizeof msaa);
    if (FAILED(hr)) { printf("D3D12 multisample quality query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT rgba8 = { DXGI_FORMAT_R8G8B8A8_UNORM,
        (D3D12_FORMAT_SUPPORT1)0xffffffffu, (D3D12_FORMAT_SUPPORT2)0xffffffffu };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &rgba8, sizeof rgba8);
    if (FAILED(hr)) { printf("D3D12 RGBA8 format-support query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT r32 = { DXGI_FORMAT_R32_FLOAT,
        (D3D12_FORMAT_SUPPORT1)0xffffffffu, (D3D12_FORMAT_SUPPORT2)0xffffffffu };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &r32, sizeof r32);
    if (FAILED(hr)) { printf("D3D12 R32_FLOAT format-support query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT r32_uint = { DXGI_FORMAT_R32_UINT,
        (D3D12_FORMAT_SUPPORT1)0xffffffffu, (D3D12_FORMAT_SUPPORT2)0xffffffffu };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &r32_uint, sizeof r32_uint);
    if (FAILED(hr)) { printf("D3D12 R32_UINT format-support query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT r32_sint = { DXGI_FORMAT_R32_SINT,
        (D3D12_FORMAT_SUPPORT1)0xffffffffu, (D3D12_FORMAT_SUPPORT2)0xffffffffu };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &r32_sint, sizeof r32_sint);
    if (FAILED(hr)) { printf("D3D12 R32_SINT format-support query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT depth = { DXGI_FORMAT_D32_FLOAT,
        (D3D12_FORMAT_SUPPORT1)0xffffffffu, (D3D12_FORMAT_SUPPORT2)0xffffffffu };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &depth, sizeof depth);
    if (FAILED(hr)) { printf("D3D12 D32_FLOAT format-support query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT depth16 = { DXGI_FORMAT_D16_UNORM,
        (D3D12_FORMAT_SUPPORT1)0xffffffffu, (D3D12_FORMAT_SUPPORT2)0xffffffffu };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &depth16, sizeof depth16);
    if (FAILED(hr)) { printf("D3D12 D16_UNORM format-support query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT vertex3 = { DXGI_FORMAT_R32G32B32_FLOAT,
        (D3D12_FORMAT_SUPPORT1)0xffffffffu, (D3D12_FORMAT_SUPPORT2)0xffffffffu };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &vertex3, sizeof vertex3);
    if (FAILED(hr)) { printf("D3D12 R32G32B32_FLOAT format-support query failed: %#lx\n", (unsigned long)hr); return ++bad; }

    D3D12_FEATURE_DATA_FORMAT_INFO format_info = { DXGI_FORMAT_R8G8B8A8_UNORM, 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_INFO, &format_info, sizeof format_info);
    int format_info_ok = SUCCEEDED(hr) && format_info.PlaneCount == 1;
    D3D12_FEATURE_DATA_FORMAT_INFO planar_info = { DXGI_FORMAT_NV12, 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_INFO, &planar_info, sizeof planar_info);
    format_info_ok &= SUCCEEDED(hr) && planar_info.PlaneCount == 2;
    D3D12_FEATURE_DATA_FORMAT_INFO depth_info = { DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_INFO, &depth_info, sizeof depth_info);
    format_info_ok &= SUCCEEDED(hr) && depth_info.PlaneCount == 2;
    D3D12_FEATURE_DATA_FORMAT_INFO v208_info = { (DXGI_FORMAT)131, 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_INFO, &v208_info, sizeof v208_info);
    format_info_ok &= SUCCEEDED(hr) && v208_info.PlaneCount == 3;
    D3D12_FEATURE_DATA_FORMAT_INFO v408_info = { (DXGI_FORMAT)132, 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_INFO, &v408_info, sizeof v408_info);
    format_info_ok &= SUCCEEDED(hr) && v408_info.PlaneCount == 3;
    D3D12_FEATURE_DATA_FORMAT_INFO invalid_info = { DXGI_FORMAT_UNKNOWN, 0 };
    hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_INFO, &invalid_info, sizeof invalid_info);
    format_info_ok &= hr == E_INVALIDARG;

    int all_format_queries_ok = 1;
    static const struct { uint32_t first, last; } format_ranges[] = { { 1, 115 }, { 130, 132 }, { 189, 191 } };
    for (size_t range = 0; range < sizeof format_ranges / sizeof format_ranges[0]; range++) {
        for (uint32_t format = format_ranges[range].first; format <= format_ranges[range].last; format++) {
            D3D12_FEATURE_DATA_FORMAT_SUPPORT f = { (DXGI_FORMAT)format,
                (D3D12_FORMAT_SUPPORT1)0xffffffffu, (D3D12_FORMAT_SUPPORT2)0xffffffffu };
            hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f);
            all_format_queries_ok &= hr == S_OK;
            if (SUCCEEDED(hr)) {
                all_format_queries_ok &= !(f.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_GATHER) ||
                                         (f.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D);
                all_format_queries_ok &= !(f.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON) ||
                                         (f.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON);
            }
        }
    }

    const UINT rgba8_required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                                D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_RENDER_TARGET |
                                D3D12_FORMAT_SUPPORT1_BLENDABLE | D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE;
    const D3D12_FORMAT_SUPPORT2 atomic_format_mask = D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
        D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
        D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
        D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
    const D3D12_FORMAT_SUPPORT2 atomic_common = D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
        D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS | D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
        D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE;
    D3D12_FORMAT_SUPPORT2 uint_atomics = r32_uint.Support2 & atomic_format_mask;
    D3D12_FORMAT_SUPPORT2 sint_atomics = r32_sint.Support2 & atomic_format_mask;
    int atomic_support_ok = !(r32.Support2 & atomic_format_mask) &&
        !(uint_atomics & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX) &&
        !(sint_atomics & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX) &&
        (!uint_atomics || (uint_atomics & atomic_common) == atomic_common) &&
        (!sint_atomics || (sint_atomics & atomic_common) == atomic_common) &&
        (!uint_atomics || (uint_atomics & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX)) &&
        (!sint_atomics || (sint_atomics & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX));
    int format_support_ok = (rgba8.Support1 & rgba8_required) == rgba8_required &&
                            (rgba8.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_GATHER) &&
                            !(rgba8.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON) &&
                            (r32.Support1 & (D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD)) ==
                                (D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD) &&
                            (r32.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) &&
                            (r32_uint.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) &&
                            (r32_sint.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) &&
                            (depth.Support1 & (D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON |
                                               D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON |
                                               D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET |
                                               D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE)) ==
                                (D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL | D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON |
                                 D3D12_FORMAT_SUPPORT1_SHADER_GATHER_COMPARISON |
                                 D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET |
                                 D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE) &&
                            (depth16.Support1 & (D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL |
                                                 D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET |
                                                 D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE)) ==
                                (D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL |
                                 D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET |
                                 D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE) &&
                            (vertex3.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER) &&
                            ((rgba8.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0) ==
                                (options.TypedUAVLoadAdditionalFormats != FALSE) &&
                            (r32.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0 && atomic_support_ok;
    static const DXGI_FORMAT additional_load_formats[] = {
        DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_SINT,
        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_UINT, DXGI_FORMAT_R16G16B16A16_SINT,
        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UINT, DXGI_FORMAT_R8G8B8A8_SINT,
        DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_UINT, DXGI_FORMAT_R16_SINT,
        DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_SINT
    };
    for (size_t i = 0; i < sizeof additional_load_formats / sizeof additional_load_formats[0]; i++) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT f = { additional_load_formats[i],
            D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
        hr = ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &f, sizeof f);
        int load = SUCCEEDED(hr) && (f.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
        format_support_ok &= load == (options.TypedUAVLoadAdditionalFormats != FALSE);
    }

    int options_queries_ok = !options2.DepthBoundsTestSupported && !options2.ProgrammableSamplePositionsTier &&
                             options2_large_hr == E_INVALIDARG && options2_small_hr == E_INVALIDARG &&
                             options2_oversized.value.DepthBoundsTestSupported == 0x12345678 &&
                             options2_oversized.value.ProgrammableSamplePositionsTier == 0x12345678 &&
                             options2_oversized.guard == 0x87654321 &&
                             options2_undersized.DepthBoundsTestSupported == 0x12345678 &&
                             options2_undersized.ProgrammableSamplePositionsTier == 0x12345678 &&
                             (options3.CopyQueueTimestampQueriesSupported == FALSE ||
                              options3.CopyQueueTimestampQueriesSupported == TRUE) &&
                             !options3.CastingFullyTypedFormatSupported &&
                             options3.WriteBufferImmediateSupportFlags ==
                                 (D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT | D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE |
                                  D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY) && !options3.ViewInstancingTier &&
                             !options3.BarycentricsSupported && options3_large_hr == E_INVALIDARG &&
                             options3_small_hr == E_INVALIDARG &&
                             options3_oversized.value.CopyQueueTimestampQueriesSupported == 0x12345678 &&
                             options3_oversized.value.CastingFullyTypedFormatSupported == 0x12345678 &&
                             options3_oversized.value.WriteBufferImmediateSupportFlags == 0x12345678 &&
                             options3_oversized.value.ViewInstancingTier == 0x12345678 &&
                             options3_oversized.value.BarycentricsSupported == 0x12345678 &&
                             options3_oversized.guard == 0x87654321 &&
                             options3_undersized.CopyQueueTimestampQueriesSupported == 0x12345678 &&
                             options3_undersized.CastingFullyTypedFormatSupported == 0x12345678 &&
                             options3_undersized.WriteBufferImmediateSupportFlags == 0x12345678 &&
                             options3_undersized.ViewInstancingTier == 0x12345678 &&
                             options3_undersized.BarycentricsSupported == 0x12345678;

    int ok = !options.DoublePrecisionFloatShaderOps && !options.OutputMergerLogicOp &&
             options.ResourceBindingTier == D3D12_RESOURCE_BINDING_TIER_3 && sampler_heap_limit_ok &&
             architecture.NodeIndex == 0 && architecture1.NodeIndex == 0 &&
             architecture.TileBasedRenderer == architecture1.TileBasedRenderer &&
             architecture.UMA == architecture1.UMA &&
             architecture.CacheCoherentUMA == architecture1.CacheCoherentUMA &&
             (architecture.TileBasedRenderer == FALSE || architecture.TileBasedRenderer == TRUE) &&
             (architecture.UMA == FALSE || architecture.UMA == TRUE) &&
             (architecture.CacheCoherentUMA == FALSE || architecture.CacheCoherentUMA == TRUE) &&
             (!architecture.CacheCoherentUMA || architecture.UMA) && !architecture1.IsolatedMMU &&
             invalid_architecture_hr == E_INVALIDARG && options_queries_ok &&
             options.TiledResourcesTier == D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&
             !options.StandardSwizzle64KBSupported && options.ResourceHeapTier == D3D12_RESOURCE_HEAP_TIER_2 &&
             !options1.WaveOps && !options1.WaveLaneCountMin && !options1.WaveLaneCountMax && !options1.TotalLaneCount &&
             options1.ExpandedComputeResourceStates && options1.Int64ShaderOps &&
             gpu_va.MaxGPUVirtualAddressBitsPerResource == options.MaxGPUVirtualAddressBitsPerResource &&
             gpu_va.MaxGPUVirtualAddressBitsPerProcess >= gpu_va.MaxGPUVirtualAddressBitsPerResource &&
             options21.work_graphs_tier == 0 && options21.execute_indirect_tier == 11 &&
             !options21.sample_cmp_gradient_and_bias && !options21.extended_command_info && msaa.NumQualityLevels == 1 &&
             format_info_ok && format_support_ok && all_format_queries_ok && feature_levels_ok && shader_model_ok;
    printf("d3d12 features: resource binding tier, sampler-heap limit, shader model, architecture, format support, OPTIONS1/2/3/21, exact feature-query sizes, and MSAA quality: %s\n", ok ? "ok" : "FAILED");
    if (copy_timestamps_supported) *copy_timestamps_supported = options3.CopyQueueTimestampQueriesSupported;
    if (!ok) {
        printf("  options double=%d logic=%d tiled=%d swizzle=%d heap=%d; options1 wave=%d lanes=%u..%u total=%u expanded=%d int64=%d\n",
               options.DoublePrecisionFloatShaderOps, options.OutputMergerLogicOp, options.TiledResourcesTier,
               options.StandardSwizzle64KBSupported, options.ResourceHeapTier, options1.WaveOps,
               options1.WaveLaneCountMin, options1.WaveLaneCountMax, options1.TotalLaneCount,
               options1.ExpandedComputeResourceStates, options1.Int64ShaderOps);
        if (options.ResourceBindingTier != D3D12_RESOURCE_BINDING_TIER_3 || !sampler_heap_limit_ok)
            printf("  resource binding tier=%u, shader-visible sampler heap at 2048=%#lx and 2049=%#lx\n",
                   (unsigned)options.ResourceBindingTier, (unsigned long)max_sampler_heap_hr,
                   (unsigned long)oversized_sampler_heap_hr);
        if (msaa.NumQualityLevels != 1) printf("  RGBA8 4x MSAA quality levels: %u, want 1\n", msaa.NumQualityLevels);
        if (!format_info_ok) printf("  FORMAT_INFO plane counts or invalid-format error are incorrect\n");
        if (!format_support_ok) printf("  format support RGBA8=(%#x,%#x), R32_FLOAT=(%#x,%#x), additional-loads=%d\n",
                                       (unsigned)rgba8.Support1, (unsigned)rgba8.Support2,
                                       (unsigned)r32.Support1, (unsigned)r32.Support2,
                                       options.TypedUAVLoadAdditionalFormats);
        if (!all_format_queries_ok) printf("  at least one known DXGI format support query failed\n");
        bad++;
    }
    return bad ? 1 : 0;
}

static int footprints_test(ID3D12Device *dev) {
    D3D12_RESOURCE_DESC desc = { 0 };
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 32; desc.Height = 3; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = { 0 };
    UINT num_rows = 0;
    UINT64 row_size = 0, total = 0;
    ID3D12Device_GetCopyableFootprints(dev, &desc, 0, 1, 513, &layout, &num_rows, &row_size, &total);
    int ok = layout.Offset == 513 && layout.Footprint.Width == 32 && layout.Footprint.Height == 3 &&
             layout.Footprint.RowPitch == 256 && num_rows == 3 && row_size == 128 && total == 640;

    desc.Width = 6; desc.Height = 5; desc.Format = DXGI_FORMAT_BC1_UNORM;
    ID3D12Device_GetCopyableFootprints(dev, &desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);
    ok &= layout.Footprint.Width == 8 && layout.Footprint.Height == 8 && layout.Footprint.RowPitch == 256 &&
          num_rows == 2 && row_size == 16 && total == 272;

    desc.Width = 5; desc.Height = 2; desc.Format = DXGI_FORMAT_R32G32B32_FLOAT;
    ID3D12Device_GetCopyableFootprints(dev, &desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);
    ok &= layout.Footprint.Width == 5 && layout.Footprint.RowPitch == 256 && num_rows == 2 && row_size == 60 && total == 316;
    desc.Width = 5; desc.Format = DXGI_FORMAT_YUY2;
    ID3D12Device_GetCopyableFootprints(dev, &desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);
    ok &= layout.Footprint.Width == 6 && layout.Footprint.RowPitch == 256 && num_rows == 2 && row_size == 12 && total == 268;

    desc.Width = 8; desc.Height = 4; desc.DepthOrArraySize = 2; desc.MipLevels = 0;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D12Device_GetCopyableFootprints(dev, &desc, 4, 1, 0, &layout, &num_rows, &row_size, &total);
    ok &= layout.Footprint.Width == 8 && layout.Footprint.Height == 4 && num_rows == 4 && row_size == 32;

    desc.Width = 5; desc.Height = 3; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_NV12;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT planes[2] = { 0 };
    UINT plane_rows[2] = { 0 };
    UINT64 plane_row_sizes[2] = { 0 };
    ID3D12Device_GetCopyableFootprints(dev, &desc, 0, 2, 0, planes, plane_rows, plane_row_sizes, &total);
    ok &= planes[0].Footprint.Format == DXGI_FORMAT_R8_TYPELESS && planes[0].Footprint.RowPitch == 512 &&
          planes[0].Footprint.Width == 5 && planes[0].Footprint.Height == 3 && plane_rows[0] == 3 &&
          plane_row_sizes[0] == 5 && planes[1].Offset == 1536 &&
          planes[1].Footprint.Format == DXGI_FORMAT_R8G8_TYPELESS && planes[1].Footprint.RowPitch == 512 &&
          planes[1].Footprint.Width == 3 && planes[1].Footprint.Height == 2 && plane_rows[1] == 2 &&
          plane_row_sizes[1] == 6 && total == 2054;

    desc.Width = 8; desc.Height = 4; desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    ID3D12Device_GetCopyableFootprints(dev, &desc, 0, 2, 0, planes, plane_rows, plane_row_sizes, &total);
    ok &= planes[0].Footprint.Format == DXGI_FORMAT_R32_TYPELESS && plane_row_sizes[0] == 32 &&
          planes[1].Footprint.Format == DXGI_FORMAT_R8_TYPELESS && plane_row_sizes[1] == 8;

    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    layout.Offset = 0; total = 0;
    ID3D12Device_GetCopyableFootprints(dev, &desc, 1, 1, 0, &layout, &num_rows, &row_size, &total);
    ok &= total == UINT64_MAX && layout.Offset == UINT64_MAX && num_rows == UINT_MAX && row_size == UINT64_MAX;

    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 300; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Device_GetCopyableFootprints(dev, &desc, 0, 1, 0, &layout, &num_rows, &row_size, &total);
    ok &= layout.Footprint.Width == 300 && layout.Footprint.Height == 1 && layout.Footprint.RowPitch == 512 &&
          num_rows == 1 && row_size == 300 && total == 300;

    printf("d3d12 footprints: row/placement alignment, packed and BC formats, array/mip indexing, planar/depth planes, invalid range and buffers: %s\n",
           ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

static int allocation_info_alignment_test(ID3D12Device *dev) {
    D3D12_RESOURCE_DESC small = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT,
        16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE };
    D3D12_RESOURCE_ALLOCATION_INFO small_info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &small);
    D3D12_RESOURCE_DESC invalid = small;
    invalid.Alignment = 8192;
    D3D12_RESOURCE_ALLOCATION_INFO invalid_info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &invalid);
    D3D12_RESOURCE_DESC too_large = small;
    too_large.Width = too_large.Height = 256;
    D3D12_RESOURCE_ALLOCATION_INFO large_info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &too_large);
    // For RGBA8 the architecture-independent 4 KiB tile is 32x32 texels: 128x128 is exactly 16 tiles,
    // while adding one column needs 20. Array slices and lower mips do not enlarge the base-mip estimate.
    D3D12_RESOURCE_DESC tile_limit = small;
    tile_limit.Width = tile_limit.Height = 128;
    D3D12_RESOURCE_ALLOCATION_INFO tile_limit_info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &tile_limit);
    D3D12_RESOURCE_DESC over_tile_limit = tile_limit;
    over_tile_limit.Width++;
    D3D12_RESOURCE_ALLOCATION_INFO over_tile_limit_info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &over_tile_limit);
    D3D12_RESOURCE_DESC array_tile_limit = tile_limit;
    array_tile_limit.DepthOrArraySize = 64;
    array_tile_limit.MipLevels = 8;
    D3D12_RESOURCE_ALLOCATION_INFO array_tile_limit_info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &array_tile_limit);
    D3D12_RESOURCE_DESC volume_tile_limit = { D3D12_RESOURCE_DIMENSION_TEXTURE3D, D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT,
        32, 32, 16, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE };
    D3D12_RESOURCE_ALLOCATION_INFO volume_tile_limit_info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &volume_tile_limit);
    D3D12_RESOURCE_DESC volume_over_tile_limit = volume_tile_limit;
    volume_over_tile_limit.DepthOrArraySize++;
    D3D12_RESOURCE_ALLOCATION_INFO volume_over_tile_limit_info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1,
                                                                                                         &volume_over_tile_limit);
    D3D12_RESOURCE_DESC render_target = small;
    render_target.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_ALLOCATION_INFO rt_info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &render_target);
    D3D12_RESOURCE_DESC buffer_desc = { D3D12_RESOURCE_DIMENSION_BUFFER, D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT,
        256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    D3D12_RESOURCE_ALLOCATION_INFO buffer_info = ID3D12Device_GetResourceAllocationInfo(dev, 0, 1, &buffer_desc);

    ID3D12Device4 *dev4 = NULL;
    HRESULT qi = ID3D12Device_QueryInterface(dev, &IID_ID3D12Device4, (void **)&dev4);
    D3D12_RESOURCE_DESC mixed[2] = { small, { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1,
        DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE } };
    D3D12_RESOURCE_ALLOCATION_INFO1 each[2] = { 0 };
    D3D12_RESOURCE_ALLOCATION_INFO mixed_info = { UINT64_MAX, 0 };
    if (SUCCEEDED(qi)) mixed_info = ID3D12Device4_GetResourceAllocationInfo1(dev4, 0, 2, mixed, each);
    if (dev4) ID3D12Device4_Release(dev4);

    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
    void *resource = NULL;
    HRESULT create_invalid = ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &invalid,
        D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, &resource);
    if (resource) ID3D12Resource_Release(resource);
    int ok = small_info.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT &&
             small_info.SizeInBytes != UINT64_MAX && small_info.SizeInBytes % small_info.Alignment == 0 &&
             invalid_info.SizeInBytes == UINT64_MAX && large_info.SizeInBytes == UINT64_MAX &&
             tile_limit_info.SizeInBytes != UINT64_MAX && over_tile_limit_info.SizeInBytes == UINT64_MAX &&
             array_tile_limit_info.SizeInBytes != UINT64_MAX && volume_tile_limit_info.SizeInBytes != UINT64_MAX &&
             volume_over_tile_limit_info.SizeInBytes == UINT64_MAX &&
             rt_info.SizeInBytes == UINT64_MAX && buffer_info.SizeInBytes == UINT64_MAX &&
             FAILED(create_invalid) && SUCCEEDED(qi) &&
             mixed_info.SizeInBytes != UINT64_MAX && mixed_info.Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
             each[0].Offset == 0 && each[0].Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT &&
             each[1].Offset >= small_info.SizeInBytes && each[1].Offset % each[1].Alignment == 0 &&
             each[1].Alignment == D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
             mixed_info.SizeInBytes == each[1].Offset + each[1].SizeInBytes;
    printf("d3d12 allocation info: 4 KiB tile limit, arrays/mips, 3D tiles, invalid alignment/resource cases, mixed GetResourceAllocationInfo1 offsets: %s\n",
           ok ? "ok" : "FAILED");
    if (!ok) {
        printf("  small=%llu/%llu invalid=%llu large=%llu tiles=(%llu,%llu) array=%llu volume=(%llu,%llu) RT=%llu buffer=%llu mixed=%llu/%llu each=(%llu,%llu,%llu),(%llu,%llu,%llu) create=%#lx QI=%#lx\n",
               (unsigned long long)small_info.SizeInBytes, (unsigned long long)small_info.Alignment,
               (unsigned long long)invalid_info.SizeInBytes, (unsigned long long)large_info.SizeInBytes,
               (unsigned long long)tile_limit_info.SizeInBytes, (unsigned long long)over_tile_limit_info.SizeInBytes,
               (unsigned long long)array_tile_limit_info.SizeInBytes,
               (unsigned long long)volume_tile_limit_info.SizeInBytes, (unsigned long long)volume_over_tile_limit_info.SizeInBytes,
               (unsigned long long)rt_info.SizeInBytes, (unsigned long long)buffer_info.SizeInBytes,
               (unsigned long long)mixed_info.SizeInBytes, (unsigned long long)mixed_info.Alignment,
               (unsigned long long)each[0].Offset, (unsigned long long)each[0].Alignment, (unsigned long long)each[0].SizeInBytes,
               (unsigned long long)each[1].Offset, (unsigned long long)each[1].Alignment, (unsigned long long)each[1].SizeInBytes,
               (unsigned long)create_invalid, (unsigned long)qi);
    }
    return ok ? 0 : 1;
}

static int write_buffer_immediate_test(ID3D12Device *dev, ID3D12CommandQueue *direct_queue) {
    ID3D12Resource *destination = buffer(dev, D3D12_HEAP_TYPE_READBACK, 32, 0,
                                         D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandQueue *compute_queue = NULL, *copy_queue = NULL;
    ID3D12CommandAllocator *allocators[3] = { NULL, NULL, NULL };
    ID3D12GraphicsCommandList2 *lists[3] = { NULL, NULL, NULL };
    D3D12_COMMAND_QUEUE_DESC compute_desc = { D3D12_COMMAND_LIST_TYPE_COMPUTE };
    D3D12_COMMAND_QUEUE_DESC copy_desc = { D3D12_COMMAND_LIST_TYPE_COPY };
    D3D12_COMMAND_LIST_TYPE list_types[3] = {
        D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY
    };
    ID3D12CommandQueue *queues[3] = { direct_queue, NULL, NULL };
    if (!destination) return 1;
    HRESULT hr = ID3D12Device_CreateCommandQueue(dev, &compute_desc, &IID_ID3D12CommandQueue,
                                                  (void **)&compute_queue);
    if (SUCCEEDED(hr)) hr = ID3D12Device_CreateCommandQueue(dev, &copy_desc, &IID_ID3D12CommandQueue,
                                                            (void **)&copy_queue);
    if (FAILED(hr)) { printf("WriteBufferImmediate queue setup failed: %#lx\n", (unsigned long)hr); return 1; }
    queues[1] = compute_queue;
    queues[2] = copy_queue;

    void *mapped = NULL;
    CHECK(ID3D12Resource_Map(destination, 0, NULL, &mapped));
    memset(mapped, 0xA5, 32);
    ID3D12Resource_Unmap(destination, 0, NULL);

    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER direct_writes[3] = {
        { ID3D12Resource_GetGPUVirtualAddress(destination), 0x11223344 },
        { ID3D12Resource_GetGPUVirtualAddress(destination) + 4, 0x55667788 },
        { ID3D12Resource_GetGPUVirtualAddress(destination) + 8, 0xAABBCCDD }
    };
    D3D12_WRITEBUFFERIMMEDIATE_MODE direct_modes[3] = {
        D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT,
        D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN,
        D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT
    };
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER compute_write = {
        ID3D12Resource_GetGPUVirtualAddress(destination) + 12, 0xDEADBEEF
    };
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER copy_write = {
        ID3D12Resource_GetGPUVirtualAddress(destination) + 16, 0x12345678
    };
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *writes[3] = { direct_writes, &compute_write, &copy_write };
    UINT counts[3] = { 3, 1, 1 };
    for (UINT i = 0; i < 3; i++) {
        CHECK(ID3D12Device_CreateCommandAllocator(dev, list_types[i], &IID_ID3D12CommandAllocator,
                                                   (void **)&allocators[i]));
        CHECK(ID3D12Device_CreateCommandList(dev, 0, list_types[i], allocators[i], NULL,
            &IID_ID3D12GraphicsCommandList2, (void **)&lists[i]));
        ID3D12GraphicsCommandList2_WriteBufferImmediate(lists[i], counts[i], writes[i],
            i == 0 ? direct_modes : NULL);
        CHECK(ID3D12GraphicsCommandList2_Close(lists[i]));
    }
    for (UINT i = 0; i < 3; i++) {
        ID3D12CommandList *submitted[] = { (ID3D12CommandList *)lists[i] };
        ID3D12CommandQueue_ExecuteCommandLists(queues[i], 1, submitted);
        wait_queue(dev, queues[i]);
    }
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER wrong_queue_write = {
        ID3D12Resource_GetGPUVirtualAddress(destination) + 24, 0xCAFEBABE
    };
    CHECK(ID3D12CommandAllocator_Reset(allocators[0]));
    CHECK(ID3D12GraphicsCommandList2_Reset(lists[0], allocators[0], NULL));
    ID3D12GraphicsCommandList2_WriteBufferImmediate(lists[0], 1, &wrong_queue_write, NULL);
    CHECK(ID3D12GraphicsCommandList2_Close(lists[0]));
    ID3D12CommandList *wrong_queue_list[] = { (ID3D12CommandList *)lists[0] };
    ID3D12CommandQueue_ExecuteCommandLists(copy_queue, 1, wrong_queue_list);
    wait_queue(dev, copy_queue);

    ID3D12Resource *upload = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 4, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!upload) return 1;
    CHECK(ID3D12Resource_Map(upload, 0, NULL, &mapped));
    *(uint32_t *)mapped = 0x5A5A5A5A;
    ID3D12Resource_Unmap(upload, 0, NULL);
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER wrong_state_write = {
        ID3D12Resource_GetGPUVirtualAddress(upload), 0xBAD0C0DE
    };
    CHECK(ID3D12CommandAllocator_Reset(allocators[0]));
    CHECK(ID3D12GraphicsCommandList2_Reset(lists[0], allocators[0], NULL));
    ID3D12GraphicsCommandList2_WriteBufferImmediate(lists[0], 1, &wrong_state_write, NULL);
    CHECK(ID3D12GraphicsCommandList2_Close(lists[0]));
    ID3D12CommandList *wrong_state_list[] = { (ID3D12CommandList *)lists[0] };
    ID3D12CommandQueue_ExecuteCommandLists(direct_queue, 1, wrong_state_list);
    wait_queue(dev, direct_queue);
    CHECK(ID3D12Resource_Map(upload, 0, NULL, &mapped));
    uint32_t upload_after = *(uint32_t *)mapped;
    ID3D12Resource_Unmap(upload, 0, NULL);

    CHECK(ID3D12Resource_Map(destination, 0, NULL, &mapped));
    uint32_t actual[8];
    memcpy(actual, mapped, sizeof actual);
    ID3D12Resource_Unmap(destination, 0, NULL);
    int failed = upload_after != 0x5A5A5A5A || actual[0] != 0x11223344 || actual[1] != 0x55667788 || actual[2] != 0xAABBCCDD ||
                 actual[3] != 0xDEADBEEF || actual[4] != 0x12345678 || actual[5] != 0xA5A5A5A5 ||
                 actual[6] != 0xA5A5A5A5 || actual[7] != 0xA5A5A5A5;
    if (failed) printf("WriteBufferImmediate values, state rejection, or untouched bytes were incorrect\n");
    else puts("d3d12 WriteBufferImmediate: default and marker writes on direct, compute, and copy lists: ok");
    for (UINT i = 0; i < 3; i++) { if (lists[i]) ID3D12GraphicsCommandList2_Release(lists[i]); if (allocators[i]) ID3D12CommandAllocator_Release(allocators[i]); }
    ID3D12CommandQueue_Release(copy_queue);
    ID3D12CommandQueue_Release(compute_queue);
    ID3D12Resource_Release(upload);
    ID3D12Resource_Release(destination);
    return failed;
}

static int state_transition_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    ID3D12Resource *source = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                               (void **)&allocator));
    void *mapped = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    memset(mapped, 0x5a, 16);
    ID3D12Resource_Unmap(readback, 0, NULL);

    D3D12_RESOURCE_BARRIER barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList *invalid = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&invalid));
    ID3D12GraphicsCommandList_ResourceBarrier(invalid, 1, &barrier);
    // The second transition's before-state is stale. Submission must reject the whole list transactionally.
    ID3D12GraphicsCommandList_ResourceBarrier(invalid, 1, &barrier);
    ID3D12GraphicsCommandList_CopyBufferRegion(invalid, readback, 0, source, 0, 16);
    CHECK(ID3D12GraphicsCommandList_Close(invalid));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)invalid };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    int rejected = 1;
    for (unsigned i = 0; i < 16; i++) if (((unsigned char *)mapped)[i] != 0x5a) rejected = 0;
    ID3D12Resource_Unmap(readback, 0, NULL);
    ID3D12GraphicsCommandList_Release(invalid);

    ID3D12GraphicsCommandList *bad_copy = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&bad_copy));
    // COPY_DEST alone is not a legal source state for CopyBufferRegion.
    ID3D12GraphicsCommandList_CopyBufferRegion(bad_copy, readback, 0, source, 0, 16);
    CHECK(ID3D12GraphicsCommandList_Close(bad_copy));
    lists[0] = (ID3D12CommandList *)bad_copy;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    for (unsigned i = 0; i < 16; i++) if (((unsigned char *)mapped)[i] != 0x5a) rejected = 0;
    ID3D12Resource_Unmap(readback, 0, NULL);
    ID3D12GraphicsCommandList_Release(bad_copy);

    // A valid split transition still starts from COPY_DEST, proving the rejected list did not commit its first barrier.
    ID3D12GraphicsCommandList *valid = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&valid));
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
    ID3D12GraphicsCommandList_ResourceBarrier(valid, 1, &barrier);
    CHECK(ID3D12GraphicsCommandList_Close(valid));
    lists[0] = (ID3D12CommandList *)valid;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);

    ID3D12GraphicsCommandList *end = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&end));
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
    ID3D12GraphicsCommandList_ResourceBarrier(end, 1, &barrier);
    ID3D12GraphicsCommandList_CopyBufferRegion(end, readback, 0, source, 0, 16);
    CHECK(ID3D12GraphicsCommandList_Close(end));
    lists[0] = (ID3D12CommandList *)end;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    int copied = 1;
    for (unsigned i = 0; i < 16; i++) if (((unsigned char *)mapped)[i] != 0) copied = 0;
    ID3D12Resource_Unmap(readback, 0, NULL);

    // COMMON buffers promote on first GPU access. Decay happens after the whole submission call, not between its
    // command lists: the second list below must still see COPY_DEST and be rejected as a COPY_SOURCE. A later call
    // can transition from COMMON while naming the resource's previously promoted state as StateBefore.
    ID3D12Resource *common = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 16, 0, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *upload = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 16, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *upload_copy = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 16, 0, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *same_batch_readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *later_readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *upload_reuse_readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!common || !upload || !upload_copy || !same_batch_readback || !later_readback || !upload_reuse_readback) return 1;
    CHECK(ID3D12Resource_Map(upload, 0, NULL, &mapped));
    for (unsigned i = 0; i < 16; i++) ((unsigned char *)mapped)[i] = (unsigned char)(0x30 + i);
    ID3D12Resource_Unmap(upload, 0, NULL);
    CHECK(ID3D12Resource_Map(same_batch_readback, 0, NULL, &mapped));
    memset(mapped, 0x5a, 16);
    ID3D12Resource_Unmap(same_batch_readback, 0, NULL);

    ID3D12GraphicsCommandList *promote = NULL, *same_batch_use = NULL, *after_decay = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&promote));
    ID3D12GraphicsCommandList_CopyBufferRegion(promote, common, 0, upload, 0, 16);
    CHECK(ID3D12GraphicsCommandList_Close(promote));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&same_batch_use));
    ID3D12GraphicsCommandList_CopyBufferRegion(same_batch_use, same_batch_readback, 0, common, 0, 16);
    CHECK(ID3D12GraphicsCommandList_Close(same_batch_use));
    ID3D12CommandList *batch_lists[] = { (ID3D12CommandList *)promote, (ID3D12CommandList *)same_batch_use };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 2, batch_lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(same_batch_readback, 0, NULL, &mapped));
    int no_decay_between_lists = 1;
    for (unsigned i = 0; i < 16; i++) if (((unsigned char *)mapped)[i] != 0x5a) no_decay_between_lists = 0;
    ID3D12Resource_Unmap(same_batch_readback, 0, NULL);

    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&after_decay));
    D3D12_RESOURCE_BARRIER from_decayed_common = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    from_decayed_common.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ common, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(after_decay, 1, &from_decayed_common);
    ID3D12GraphicsCommandList_CopyBufferRegion(after_decay, later_readback, 0, common, 0, 16);
    ID3D12GraphicsCommandList_CopyBufferRegion(after_decay, upload_copy, 0, upload, 0, 16);
    CHECK(ID3D12GraphicsCommandList_Close(after_decay));
    lists[0] = (ID3D12CommandList *)after_decay;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(later_readback, 0, NULL, &mapped));
    int decayed_and_copied = 1;
    for (unsigned i = 0; i < 16; i++) if (((unsigned char *)mapped)[i] != (unsigned char)(0x30 + i)) decayed_and_copied = 0;
    ID3D12Resource_Unmap(later_readback, 0, NULL);

    ID3D12GraphicsCommandList *reuse_decayed_upload = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&reuse_decayed_upload));
    ID3D12GraphicsCommandList_CopyBufferRegion(reuse_decayed_upload, upload_reuse_readback, 0, upload_copy, 0, 16);
    CHECK(ID3D12GraphicsCommandList_Close(reuse_decayed_upload));
    lists[0] = (ID3D12CommandList *)reuse_decayed_upload;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(upload_reuse_readback, 0, NULL, &mapped));
    for (unsigned i = 0; i < 16; i++) if (((unsigned char *)mapped)[i] != (unsigned char)(0x30 + i)) decayed_and_copied = 0;
    ID3D12Resource_Unmap(upload_reuse_readback, 0, NULL);

    ID3D12GraphicsCommandList_Release(promote);
    ID3D12GraphicsCommandList_Release(same_batch_use);
    ID3D12GraphicsCommandList_Release(after_decay);
    ID3D12GraphicsCommandList_Release(reuse_decayed_upload);
    ID3D12GraphicsCommandList_Release(valid);
    ID3D12GraphicsCommandList_Release(end);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12Resource_Release(common);
    ID3D12Resource_Release(upload);
    ID3D12Resource_Release(upload_copy);
    ID3D12Resource_Release(same_batch_readback);
    ID3D12Resource_Release(later_readback);
    ID3D12Resource_Release(upload_reuse_readback);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(source);
    printf("d3d12 state tracking: stale before-state and invalid copy use reject lists; split barriers, COMMON promotion/decay and batch ordering: %s\n",
           rejected && copied && no_decay_between_lists && decayed_and_copied ? "ok" : "FAILED");
    return rejected && copied && no_decay_between_lists && decayed_and_copied ? 0 : 1;
}

static int residency_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    ID3D12Device1 *dev1 = NULL;
    ID3D12Device3 *dev3 = NULL;
    ID3D12Resource *source = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 16, 0, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator *allocator = NULL;
    ID3D12GraphicsCommandList *list = NULL;
    ID3D12Pageable *same_resource_twice[] = { (ID3D12Pageable *)source, (ID3D12Pageable *)source };
    HRESULT hr;
    if (!source || !readback) return 1;
    CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device1, (void **)&dev1));
    CHECK(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device3, (void **)&dev3));
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator));
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, source, 0, 16);
    CHECK(ID3D12GraphicsCommandList_Close(list));

    // A created resource starts resident. Two additional references must survive two matching Evict calls.
    CHECK(ID3D12Device_MakeResident(dev, 2, same_resource_twice));
    CHECK(ID3D12Device_Evict(dev, 2, same_resource_twice));
    void *mapped = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    memset(mapped, 0x5a, 16);
    ID3D12Resource_Unmap(readback, 0, NULL);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    int refcounted = 1;
    for (unsigned i = 0; i < 16; i++) if (((uint8_t *)mapped)[i] != 0) refcounted = 0;
    ID3D12Resource_Unmap(readback, 0, NULL);

    // The creation reference is now the last one. Evicted resources cannot be read by a submitted GPU command.
    hr = ID3D12Device_Evict(dev, 1, same_resource_twice);
    if (FAILED(hr)) {
        printf("d3d12 residency: final Evict failed: %#lx\n", (unsigned long)hr);
        refcounted = 0;
    }
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    memset(mapped, 0x5a, 16);
    ID3D12Resource_Unmap(readback, 0, NULL);
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    int evicted_access_rejected = 1;
    for (unsigned i = 0; i < 16; i++) if (((uint8_t *)mapped)[i] != 0x5a) evicted_access_rejected = 0;
    ID3D12Resource_Unmap(readback, 0, NULL);

    CHECK(ID3D12Device_MakeResident(dev, 1, same_resource_twice));
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    int restored = 1;
    for (unsigned i = 0; i < 16; i++) if (((uint8_t *)mapped)[i] != 0) restored = 0;
    ID3D12Resource_Unmap(readback, 0, NULL);

    ID3D12Fence *unsupported_fence = NULL;
    CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,
        (void **)&unsupported_fence));
    ID3D12Pageable *unsupported_pageable = (ID3D12Pageable *)unsupported_fence;
    HRESULT unsupported_residency = ID3D12Device_MakeResident(dev, 1, &unsupported_pageable);
    int unsupported_object_rejected = FAILED(unsupported_residency);
    ID3D12Pageable *source_pageable = (ID3D12Pageable *)source;
    D3D12_RESIDENCY_PRIORITY source_priority = D3D12_RESIDENCY_PRIORITY_MINIMUM;
    HRESULT priority_result = ID3D12Device1_SetResidencyPriority(dev1, 1, &source_pageable, &source_priority);
    int priority_supported = SUCCEEDED(priority_result);
    HRESULT unsupported_priority = ID3D12Device1_SetResidencyPriority(dev1, 1, &unsupported_pageable,
        &source_priority);
    int unsupported_priority_rejected = FAILED(unsupported_priority);

    CHECK(ID3D12Device_Evict(dev, 1, same_resource_twice));
    CHECK(ID3D12Device3_EnqueueMakeResident(dev3, D3D12_RESIDENCY_FLAG_NONE, 1, same_resource_twice,
        unsupported_fence, 2));
    int enqueue_fence_signaled = ID3D12Fence_GetCompletedValue(unsupported_fence) >= 2;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    int enqueued_resident = 1;
    for (unsigned i = 0; i < 16; i++) if (((uint8_t *)mapped)[i] != 0) enqueued_resident = 0;
    ID3D12Resource_Unmap(readback, 0, NULL);
    ID3D12Fence_Release(unsupported_fence);

    D3D12_HEAP_DESC heap_desc = { 65536, { D3D12_HEAP_TYPE_UPLOAD }, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS };
    ID3D12Heap *placed_heap = NULL;
    ID3D12Pageable *placed_pageable = NULL;
    ID3D12Resource *placed_source = NULL, *placed_readback = NULL;
    ID3D12GraphicsCommandList *placed_list = NULL;
    CHECK(ID3D12Device_CreateHeap(dev, &heap_desc, &IID_ID3D12Heap, (void **)&placed_heap));
    placed_pageable = (ID3D12Pageable *)placed_heap;
    D3D12_RESOURCE_DESC placed_desc = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 16, 1, 1, 1, DXGI_FORMAT_UNKNOWN,
        { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    CHECK(ID3D12Device_CreatePlacedResource(dev, placed_heap, 0, &placed_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        NULL, &IID_ID3D12Resource, (void **)&placed_source));
    placed_readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!placed_readback) return 1;
    CHECK(ID3D12Resource_Map(placed_source, 0, NULL, &mapped));
    for (unsigned i = 0; i < 16; i++) ((uint8_t *)mapped)[i] = (uint8_t)(0x30 + i);
    ID3D12Resource_Unmap(placed_source, 0, NULL);
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&placed_list));
    ID3D12GraphicsCommandList_CopyBufferRegion(placed_list, placed_readback, 0, placed_source, 0, 16);
    CHECK(ID3D12GraphicsCommandList_Close(placed_list));
    ID3D12Pageable *placed_pageable_resource = (ID3D12Pageable *)placed_source;
    HRESULT placed_resource_evict = ID3D12Device_Evict(dev, 1, &placed_pageable_resource);
    int placed_resource_rejected = FAILED(placed_resource_evict);
    CHECK(ID3D12Device_Evict(dev, 1, &placed_pageable));
    CHECK(ID3D12Resource_Map(placed_readback, 0, NULL, &mapped));
    memset(mapped, 0x5a, 16);
    ID3D12Resource_Unmap(placed_readback, 0, NULL);
    lists[0] = (ID3D12CommandList *)placed_list;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(placed_readback, 0, NULL, &mapped));
    int placed_heap_rejected = 1;
    for (unsigned i = 0; i < 16; i++) if (((uint8_t *)mapped)[i] != 0x5a) placed_heap_rejected = 0;
    ID3D12Resource_Unmap(placed_readback, 0, NULL);
    CHECK(ID3D12Device_MakeResident(dev, 1, &placed_pageable));
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(placed_readback, 0, NULL, &mapped));
    int placed_heap_restored = 1;
    for (unsigned i = 0; i < 16; i++)
        if (((uint8_t *)mapped)[i] != (uint8_t)(0x30 + i)) placed_heap_restored = 0;
    ID3D12Resource_Unmap(placed_readback, 0, NULL);

    if (!refcounted || !evicted_access_rejected || !restored || !unsupported_object_rejected ||
        !priority_supported || !unsupported_priority_rejected || !enqueue_fence_signaled || !enqueued_resident ||
        !placed_resource_rejected || !placed_heap_rejected || !placed_heap_restored) {
        printf("d3d12 residency: refcount %s, committed eviction %s, restore %s, unsupported object %s, "
               "priority %s/%s, enqueue/fence %s/%s, "
               "placed resource %s, placed heap eviction %s and restore %s: FAILED\n",
               refcounted ? "ok" : "failed", evicted_access_rejected ? "rejected" : "executed",
               restored ? "ok" : "failed", unsupported_object_rejected ? "rejected" : "accepted",
               priority_supported ? "ok" : "failed", unsupported_priority_rejected ? "rejected" : "accepted",
               enqueued_resident ? "resident" : "not resident", enqueue_fence_signaled ? "signaled" : "unsignaled",
               placed_resource_rejected ? "rejected" : "accepted", placed_heap_rejected ? "rejected" : "executed",
               placed_heap_restored ? "ok" : "failed");
        bad++;
    } else {
        puts("d3d12 residency: object types, refcounts, priorities, enqueue fence and committed/placed restoration: ok");
    }
    ID3D12GraphicsCommandList_Release(placed_list);
    ID3D12Resource_Release(placed_readback);
    ID3D12Resource_Release(placed_source);
    ID3D12Heap_Release(placed_heap);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(source);
    ID3D12Device3_Release(dev3);
    ID3D12Device1_Release(dev1);
    return !refcounted || !evicted_access_rejected || !restored || !unsupported_object_rejected ||
        !priority_supported || !unsupported_priority_rejected || !enqueue_fence_signaled || !enqueued_resident ||
        !placed_resource_rejected || !placed_heap_rejected || !placed_heap_restored;
}

static int copy_queue_test(ID3D12Device *dev, BOOL copy_timestamps_supported) {
    D3D12_COMMAND_QUEUE_DESC queue_desc = { D3D12_COMMAND_LIST_TYPE_COPY };
    ID3D12CommandQueue *queue = NULL;
    CHECK(ID3D12Device_CreateCommandQueue(dev, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue));
    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_COPY, &IID_ID3D12CommandAllocator,
                                               (void **)&allocator));
    ID3D12Resource *upload = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 16, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *copy = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 16, 0, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *timestamp_readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC timestamp_desc = { (D3D12_QUERY_HEAP_TYPE)5, 2 };
    ID3D12QueryHeap *timestamps = NULL;
    HRESULT timestamp_heap_hr = ID3D12Device_CreateQueryHeap(dev, &timestamp_desc, &IID_ID3D12QueryHeap,
                                                              (void **)&timestamps);
    int bad = (copy_timestamps_supported ? timestamp_heap_hr != S_OK : SUCCEEDED(timestamp_heap_hr));
    UINT64 timestamp_frequency = 0, gpu_before = 0, cpu_before = 0;
    HRESULT frequency_hr = ID3D12CommandQueue_GetTimestampFrequency(queue, &timestamp_frequency);
    HRESULT calibration_hr = ID3D12CommandQueue_GetClockCalibration(queue, &gpu_before, &cpu_before);
    if (copy_timestamps_supported) {
        if (!timestamps || !timestamp_readback || frequency_hr != S_OK || timestamp_frequency != 1000000000ull ||
            calibration_hr != S_OK) bad++;
    } else if (timestamps || frequency_hr != E_FAIL || calibration_hr != E_FAIL) bad++;
    if (bad || !upload || !copy || !readback || (copy_timestamps_supported && (!timestamps || !timestamp_readback))) {
        printf("d3d12 copy queue: timestamp support=%d heap=%#lx frequency=%#lx (%llu) calibration=%#lx\n",
               copy_timestamps_supported, (unsigned long)timestamp_heap_hr, (unsigned long)frequency_hr,
               (unsigned long long)timestamp_frequency, (unsigned long)calibration_hr);
        return 1;
    }
    void *mapped = NULL;
    CHECK(ID3D12Resource_Map(upload, 0, NULL, &mapped));
    for (unsigned i = 0; i < 16; i++) ((unsigned char *)mapped)[i] = (unsigned char)(0xa0 + i);
    ID3D12Resource_Unmap(upload, 0, NULL);

    // Dispatch is not valid on a COPY queue and must be dropped before replay.
    ID3D12GraphicsCommandList *invalid = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_COPY, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&invalid));
    ID3D12GraphicsCommandList_Dispatch(invalid, 1, 1, 1);
    CHECK(ID3D12GraphicsCommandList_Close(invalid));
    ID3D12CommandList *invalid_lists[] = { (ID3D12CommandList *)invalid };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, invalid_lists);

    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_COPY, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_CopyBufferRegion(list, copy, 0, upload, 0, 16);
    D3D12_RESOURCE_BARRIER barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ copy, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, copy, 0, 16);
    if (copy_timestamps_supported) {
        ID3D12GraphicsCommandList_EndQuery(list, timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        ID3D12GraphicsCommandList_EndQuery(list, timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        ID3D12GraphicsCommandList_ResolveQueryData(list, timestamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                                    timestamp_readback, 0);
    }
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    int copied = 1;
    for (unsigned i = 0; i < 16; i++) if (((unsigned char *)mapped)[i] != (unsigned char)(0xa0 + i)) copied = 0;
    if (!copied) {
        printf("d3d12 copy queue: readback begins %02x %02x %02x %02x, want a0 a1 a2 a3\n",
               ((unsigned char *)mapped)[0], ((unsigned char *)mapped)[1],
               ((unsigned char *)mapped)[2], ((unsigned char *)mapped)[3]);
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    if (copy_timestamps_supported) {
        UINT64 *ticks = NULL, gpu_after = 0, cpu_after = 0;
        CHECK(ID3D12Resource_Map(timestamp_readback, 0, NULL, (void **)&ticks));
        CHECK(ID3D12CommandQueue_GetClockCalibration(queue, &gpu_after, &cpu_after));
        if (ticks[0] < gpu_before || ticks[1] < ticks[0] || ticks[1] > gpu_after) {
            printf("d3d12 copy queue: timestamp ticks %llu, %llu outside calibration %llu .. %llu\n",
                   (unsigned long long)ticks[0], (unsigned long long)ticks[1],
                   (unsigned long long)gpu_before, (unsigned long long)gpu_after);
            bad++;
        }
        ID3D12Resource_Unmap(timestamp_readback, 0, NULL);
    }
    if (!copied) printf("d3d12 copy queue: copied buffer did not match the uploaded bytes\n");
    printf("d3d12 copy queue: dispatch list rejected; COMMON promotion, copy-state transition, readback and %stimestamps: %s\n",
           copy_timestamps_supported ? "copy-queue " : "unsupported ", copied && !bad ? "ok" : "FAILED");
    ID3D12GraphicsCommandList_Release(invalid);
    ID3D12GraphicsCommandList_Release(list);
    if (timestamps) ID3D12QueryHeap_Release(timestamps);
    ID3D12Resource_Release(upload);
    ID3D12Resource_Release(copy);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(timestamp_readback);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12CommandQueue_Release(queue);
    return copied && !bad ? 0 : 1;
}

static int attachment_state_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC color_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1,
        DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_RESOURCE_DESC depth_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1,
        DXGI_FORMAT_D32_FLOAT, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL };
    ID3D12Resource *color = NULL, *depth = NULL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &color_desc,
        D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void **)&color));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&depth));
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 1024, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) return 1;
    void *mapped = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    memset(mapped, 0, 1024);
    ID3D12Resource_Unmap(readback, 0, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtvs = NULL, *dsvs = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsvs));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsvs);
    ID3D12Device_CreateRenderTargetView(dev, color, NULL, rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = { DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D };
    dsv_desc.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
    ID3D12Device_CreateDepthStencilView(dev, depth, &dsv_desc, dsv);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                               (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    float red[4] = { 1, 0, 0, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, red, 0, NULL);
    D3D12_RESOURCE_BARRIER color_barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    color_barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ color, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &color_barrier);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    ID3D12GraphicsCommandList_Release(list);

    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 0.5f, 0, 0, NULL);
    D3D12_RESOURCE_BARRIER depth_barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    depth_barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ depth, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_DEPTH_READ };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &depth_barrier);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    lists[0] = (ID3D12CommandList *)list;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    ID3D12GraphicsCommandList_Release(list);

    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    color_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    color_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &color_barrier);
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &depth_barrier);
    float green[4] = { 0, 1, 0, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, green, 0, NULL);
    color_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    color_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &color_barrier);
    D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { color, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    lists[0] = (ID3D12CommandList *)list;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(readback, 0, NULL, &mapped));
    unsigned char *pixel = mapped;
    int ok = pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255;
    ID3D12Resource_Unmap(readback, 0, NULL);

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(dsvs);
    ID3D12DescriptorHeap_Release(rtvs);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(depth);
    ID3D12Resource_Release(color);
    printf("d3d12 attachment states: invalid RTV clear and read-only DSV clear reject their lists; later valid transitions and clear succeed: %s\n",
           ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

static int rect_clear_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    const UINT width = 8, height = 8, row_pitch = 256;
    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC color_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
        DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_RESOURCE_DESC depth_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
        DXGI_FORMAT_D32_FLOAT, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL };
    ID3D12Resource *color = NULL, *depth = NULL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &color_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&color));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&depth));
    ID3D12Resource *color_readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, row_pitch * height,
        0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *depth_readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, row_pitch * height,
        0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!color_readback || !depth_readback) return 1;

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = NULL, *dsv_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap);
    ID3D12Device_CreateRenderTargetView(dev, color, NULL, rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = { DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D };
    ID3D12Device_CreateDepthStencilView(dev, depth, &dsv_desc, dsv);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));

    const float blue[4] = { 0, 0, 1, 1 }, red[4] = { 1, 0, 0, 1 };
    const D3D12_RECT color_rects[] = { { 1, 1, 4, 4 }, { 3, 3, 6, 6 } };
    const D3D12_RECT depth_rect = { 2, 2, 5, 5 };
    const D3D12_RECT high_depth_rect = { 0, 0, 1, 1 }, subnormal_depth_rect = { 7, 7, 8, 8 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, red, 2, color_rects);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 0.25f, 0, 1, &depth_rect);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.25f, 0, 1, &high_depth_rect);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 0x1p-149f, 0, 1,
                                                     &subnormal_depth_rect);

    D3D12_RESOURCE_BARRIER barriers[2] = { 0 };
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ color,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_SOURCE };
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ depth,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, barriers);

    D3D12_TEXTURE_COPY_LOCATION color_dst = { color_readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    color_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM,
        width, height, 1, row_pitch };
    D3D12_TEXTURE_COPY_LOCATION color_src = { color, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &color_dst, 0, 0, 0, &color_src, NULL);
    D3D12_TEXTURE_COPY_LOCATION depth_dst = { depth_readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    depth_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_D32_FLOAT,
        width, height, 1, row_pitch };
    D3D12_TEXTURE_COPY_LOCATION depth_src = { depth, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &depth_dst, 0, 0, 0, &depth_src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    unsigned char *color_pixels = NULL;
    float *depth_pixels = NULL;
    CHECK(ID3D12Resource_Map(color_readback, 0, NULL, (void **)&color_pixels));
    CHECK(ID3D12Resource_Map(depth_readback, 0, NULL, (void **)&depth_pixels));
    int wrong = 0;
    for (UINT y = 0; y < height; y++) for (UINT x = 0; x < width; x++) {
        int in_red = (x >= 1 && x < 4 && y >= 1 && y < 4) ||
                     (x >= 3 && x < 6 && y >= 3 && y < 6);
        const unsigned char *pixel = color_pixels + (size_t)y * row_pitch + x * 4;
        wrong += pixel[0] != (in_red ? 255 : 0) || pixel[1] != 0 ||
                 pixel[2] != (in_red ? 0 : 255) || pixel[3] != 255;
        float actual_depth;
        memcpy(&actual_depth, (const unsigned char *)depth_pixels + (size_t)y * row_pitch + x * 4,
               sizeof actual_depth);
        float expected_depth = x == 0 && y == 0 ? 1.0f :
            x == 7 && y == 7 ? 0.0f :
            x >= 2 && x < 5 && y >= 2 && y < 5 ? 0.25f : 0.75f;
        wrong += fabsf(actual_depth - expected_depth) > 0.00001f;
    }
    ID3D12Resource_Unmap(color_readback, 0, NULL);
    ID3D12Resource_Unmap(depth_readback, 0, NULL);

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(dsv_heap);
    ID3D12DescriptorHeap_Release(rtv_heap);
    ID3D12Resource_Release(depth_readback);
    ID3D12Resource_Release(color_readback);
    ID3D12Resource_Release(depth);
    ID3D12Resource_Release(color);
    printf("d3d12 rectangular clears: overlapping RTV rectangles and partial DSV depth clear, %d wrong pixels: %s\n",
           wrong, wrong ? "FAILED" : "ok");
    return wrong ? 1 : 0;
}

static int rect_stencil_clear_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    static unsigned char vs[65536], ps[65536];
    size_t vs_size, ps_size;
    if (load("tests/shaders12/tri_vs.dxil", vs, sizeof vs, &vs_size) ||
        load("tests/shaders12/tri_ps.dxil", ps, sizeof ps, &ps_size)) return 1;

    D3D12_ROOT_PARAMETER root_param = { D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS };
    root_param.Constants.ShaderRegister = 0;
    root_param.Constants.Num32BitValues = 4;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = { 1, &root_param, 0, NULL,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob *serialized = NULL, *errors = NULL;
    CHECK(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    ID3D12RootSignature *root = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(serialized),
        ID3D10Blob_GetBufferSize(serialized), &IID_ID3D12RootSignature, (void **)&root));

    D3D12_INPUT_ELEMENT_DESC elements[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    memset(&pso_desc, 0, sizeof pso_desc);
    pso_desc.pRootSignature = root;
    pso_desc.VS = (D3D12_SHADER_BYTECODE){ vs, vs_size };
    pso_desc.PS = (D3D12_SHADER_BYTECODE){ ps, ps_size };
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT32_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.DepthStencilState.StencilEnable = TRUE;
    pso_desc.DepthStencilState.StencilReadMask = 0xff;
    pso_desc.DepthStencilState.StencilWriteMask = 0;
    pso_desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    pso_desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    pso_desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    pso_desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    pso_desc.DepthStencilState.BackFace = pso_desc.DepthStencilState.FrontFace;
    pso_desc.InputLayout = (D3D12_INPUT_LAYOUT_DESC){ elements, 2 };
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    pso_desc.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = NULL;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pso_desc, &IID_ID3D12PipelineState, (void **)&pso));

    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC color_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1,
        DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    D3D12_RESOURCE_DESC depth_desc = color_desc;
    depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    ID3D12Resource *color = NULL, *depth = NULL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &color_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&color));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, NULL, &IID_ID3D12Resource, (void **)&depth));
    ID3D12Resource *vertex_buffer = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 3 * 6 * sizeof(float), 0,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0,
        D3D12_RESOURCE_STATE_COPY_DEST);
    if (!vertex_buffer || !readback) return 1;
    const float vertices[3][6] = {
        { -1, -1, 1, 1, 1, 1 }, { 3, -1, 1, 1, 1, 1 }, { -1, 3, 1, 1, 1, 1 }
    };
    void *mapped = NULL;
    CHECK(ID3D12Resource_Map(vertex_buffer, 0, NULL, &mapped));
    memcpy(mapped, vertices, sizeof vertices);
    ID3D12Resource_Unmap(vertex_buffer, 0, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtv_heap = NULL, *dsv_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap);
    ID3D12Device_CreateRenderTargetView(dev, color, NULL, rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = { DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        D3D12_DSV_DIMENSION_TEXTURE2D };
    ID3D12Device_CreateDepthStencilView(dev, depth, &dsv_desc, dsv);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    const float black[4] = { 0, 0, 0, 1 }, red_tint[4] = { 1, 0, 0, 1 };
    const D3D12_RECT left_half = { 0, 0, 32, 64 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, black, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 1, 1, &left_half);
    D3D12_RESOURCE_BARRIER depth_read = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    depth_read.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ depth,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_DEPTH_READ };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &depth_read);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, &dsv);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, red_tint, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vertex_buffer),
        3 * 6 * sizeof(float), 6 * sizeof(float) };
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    D3D12_VIEWPORT viewport = { 0, 0, 64, 64, 0, 1 };
    D3D12_RECT scissor = { 0, 0, 64, 64 };
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &viewport);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &scissor);
    ID3D12GraphicsCommandList_OMSetStencilRef(list, 1);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ color,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM,
        64, 64, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { color, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    unsigned char *pixels = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
    int wrong = 0;
    for (UINT y = 0; y < 64; y++) for (UINT x = 0; x < 64; x++) {
        const unsigned char *pixel = pixels + (size_t)y * 256 + x * 4;
        int should_be_red = x < 32;
        wrong += pixel[0] != (should_be_red ? 255 : 0) || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255;
    }
    ID3D12Resource_Unmap(readback, 0, NULL);

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(dsv_heap);
    ID3D12DescriptorHeap_Release(rtv_heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(vertex_buffer);
    ID3D12Resource_Release(depth);
    ID3D12Resource_Release(color);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root);
    ID3D10Blob_Release(serialized);
    if (errors) ID3D10Blob_Release(errors);
    printf("d3d12 stencil rectangles: partial stencil clear gates a full-screen draw to the selected half, %d wrong pixels: %s\n",
           wrong, wrong ? "FAILED" : "ok");
    return wrong ? 1 : 0;
}

static int compressed_copy_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    D3D12_RESOURCE_DESC texture_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 8, 1, 1,
        DXGI_FORMAT_BC1_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE };
    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    ID3D12Resource *texture = NULL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&texture));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT rows;
    UINT64 row_size, total;
    ID3D12Device_GetCopyableFootprints(dev, &texture_desc, 0, 1, 0, &footprint, &rows, &row_size, &total);
    if (footprint.Footprint.Format != DXGI_FORMAT_BC1_UNORM || footprint.Footprint.RowPitch != 256 ||
        rows != 2 || row_size != 16 || total != 272) {
        printf("d3d12 compressed copy: BC1 footprint was width %u height %u pitch %u rows %u row bytes %llu total %llu\n",
               footprint.Footprint.Width, footprint.Footprint.Height, footprint.Footprint.RowPitch, rows,
               (unsigned long long)row_size, (unsigned long long)total);
        return 1;
    }
    ID3D12Resource *upload = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 512, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 512, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!upload || !readback) return 1;
    unsigned char payload[32];
    for (unsigned i = 0; i < sizeof payload; i++) payload[i] = (unsigned char)(i * 37u + 11u);
    unsigned char *mapped = NULL;
    CHECK(ID3D12Resource_Map(upload, 0, NULL, (void **)&mapped));
    memcpy(mapped, payload, 16);
    memcpy(mapped + 256, payload + 16, 16);
    ID3D12Resource_Unmap(upload, 0, NULL);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
          (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&list));
    D3D12_TEXTURE_COPY_LOCATION texture_location = { texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION upload_location = { upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    upload_location.PlacedFootprint = footprint;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &texture_location, 0, 0, 0, &upload_location, NULL);
    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    D3D12_TEXTURE_COPY_LOCATION readback_location = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    readback_location.PlacedFootprint = footprint;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &readback_location, 0, 0, 0, &texture_location, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&mapped));
    int ok = !memcmp(mapped, payload, 16) && !memcmp(mapped + 256, payload + 16, 16);
    ID3D12Resource_Unmap(readback, 0, NULL);
    printf("d3d12 compressed copy: BC1 upload and readback preserve two compressed block rows, %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}

// Drawing: a 64x64 RGBA8 target cleared to blue and a D32 depth buffer cleared to 1, one triangle drawn indexed
// (base vertex 3) through a graphics pipeline from DXIL (tri_vs, tri_ps), with root constants and both buffer views
// supplied by a D3D12 command signature; the target is read back through a placed footprint.
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
    D3D12_GRAPHICS_PIPELINE_STATE_DESC masked_pd = pd;
    masked_pd.SampleMask = 0xFFFFFFFEu;
    ID3D12PipelineState *masked_pso = NULL;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &masked_pd, &IID_ID3D12PipelineState,
                                                    (void **)&masked_pso));

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
    float verts[9][6] = {
        { 9, 9, 0, 0, 0, 0 }, { 9, 9, 0, 0, 0, 0 }, { 9, 9, 0, 0, 0, 0 },
        { -0.5f, -0.5f, 1, 1, 1, 1 }, { 0, 0.5f, 1, 1, 1, 1 }, { 0.5f, -0.5f, 1, 1, 1, 1 },
        { -0.9f, 0.7f, 1, 1, 1, 1 }, { -0.5f, 0.7f, 1, 1, 1, 1 }, { -0.7f, 0.95f, 1, 1, 1, 1 },
    };
    unsigned short idx[3] = { 0, 1, 2 };
    ID3D12Resource *vb = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof verts, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *ib = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof idx, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *indirect = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 68, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *gpu_indirect = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 68, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *gpu_indirect_count = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 4, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *native_draw_args = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 48, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *native_indexed_args = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 48, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *native_count_upload = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 4, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *native_count = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 4, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *zero_count_buffer = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 4, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *rb_bundle = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *rb_gpu_indirect = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *rb_reset = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *rb_native_draw = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *rb_native_count = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *rb_native_indexed = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *rb_native_indexed_count = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (bad || !indirect || !gpu_indirect || !gpu_indirect_count || !native_draw_args || !native_indexed_args || !native_count_upload || !native_count ||
        !zero_count_buffer || !rb_bundle || !rb_reset || !rb_native_draw || !rb_native_count || !rb_native_indexed ||
        !rb_gpu_indirect || !rb_native_indexed_count) return 1;
    void *p;
    CHECK(ID3D12Resource_Map(vb, 0, NULL, &p)); memcpy(p, verts, sizeof verts); ID3D12Resource_Unmap(vb, 0, NULL);
    CHECK(ID3D12Resource_Map(ib, 0, NULL, &p)); memcpy(p, idx, sizeof idx); ID3D12Resource_Unmap(ib, 0, NULL);
    CHECK(ID3D12Resource_Map(zero_count_buffer, 0, NULL, &p)); *(uint32_t *)p = 0; ID3D12Resource_Unmap(zero_count_buffer, 0, NULL);
    CHECK(ID3D12Resource_Map(native_count_upload, 0, NULL, &p)); *(uint32_t *)p = 1; ID3D12Resource_Unmap(native_count_upload, 0, NULL);
    uint32_t indirect_words[17] = { 0 };
    float tint[4] = { 0.5f, 1, 0.25f, 1 };
    memcpy(indirect_words, tint, sizeof tint);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof verts, sizeof verts[0] };
    D3D12_INDEX_BUFFER_VIEW ibv = { ID3D12Resource_GetGPUVirtualAddress(ib), sizeof idx, DXGI_FORMAT_R16_UINT };
    memcpy(&indirect_words[4], &vbv, sizeof vbv);
    memcpy(&indirect_words[8], &ibv, sizeof ibv);
    indirect_words[12] = 3; indirect_words[13] = 1; indirect_words[14] = 0;
    indirect_words[15] = 3; indirect_words[16] = 0;
    CHECK(ID3D12Resource_Map(indirect, 0, NULL, &p)); memcpy(p, indirect_words, sizeof indirect_words); ID3D12Resource_Unmap(indirect, 0, NULL);
    uint32_t native_draw_words[12] = { 3, 1, 3, 0, 0xfeed0001, 0xfeed0002,
                                       3, 1, 6, 0, 0xfeed0003, 0xfeed0004 };
    uint32_t native_indexed_words[12] = { 3, 1, 0, 3, 0, 0xfeed0005,
                                          3, 1, 0, 6, 0, 0xfeed0006 };
    CHECK(ID3D12Resource_Map(native_draw_args, 0, NULL, &p));
    memcpy(p, native_draw_words, sizeof native_draw_words); ID3D12Resource_Unmap(native_draw_args, 0, NULL);
    CHECK(ID3D12Resource_Map(native_indexed_args, 0, NULL, &p));
    memcpy(p, native_indexed_words, sizeof native_indexed_words); ID3D12Resource_Unmap(native_indexed_args, 0, NULL);

    D3D12_INDIRECT_ARGUMENT_DESC indirect_desc[4] = { 0 };
    indirect_desc[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    indirect_desc[0].Constant.RootParameterIndex = 0;
    indirect_desc[0].Constant.Num32BitValuesToSet = 4;
    indirect_desc[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    indirect_desc[1].VertexBuffer.Slot = 0;
    indirect_desc[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
    indirect_desc[3].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC indirect_signature_desc = { 68, 4, indirect_desc, 0 };
    ID3D12CommandSignature *indirect_signature;
    CHECK(ID3D12Device_CreateCommandSignature(dev, &indirect_signature_desc, rs,
                                               &IID_ID3D12CommandSignature, (void **)&indirect_signature));
    D3D12_INDIRECT_ARGUMENT_DESC native_draw_desc = { 0 };
    native_draw_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC native_draw_signature_desc = { 24, 1, &native_draw_desc, 0 };
    ID3D12CommandSignature *native_draw_signature;
    CHECK(ID3D12Device_CreateCommandSignature(dev, &native_draw_signature_desc, NULL,
                                               &IID_ID3D12CommandSignature, (void **)&native_draw_signature));
    D3D12_INDIRECT_ARGUMENT_DESC native_indexed_desc = { 0 };
    native_indexed_desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC native_indexed_signature_desc = { 24, 1, &native_indexed_desc, 0 };
    ID3D12CommandSignature *native_indexed_signature;
    CHECK(ID3D12Device_CreateCommandSignature(dev, &native_indexed_signature_desc, NULL,
                                               &IID_ID3D12CommandSignature, (void **)&native_indexed_signature));

    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    float blue[4] = { 0, 0, 1, 1 };
    D3D12_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    D3D12_RECT sc = { 0, 0, 64, 64 };
    // Exercise submission preflight for GPU-only root/IA records produced by a prior list. This list has only
    // state setup before ExecuteIndirect, so its records can be staged without crossing unsubmitted GPU work.
    ID3D12Resource *rb_indirect_preflight = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0,
                                                    D3D12_RESOURCE_STATE_COPY_DEST);
    if (bad || !rb_indirect_preflight) return 1;
    ID3D12GraphicsCommandList *indirect_setup = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&indirect_setup));
    ID3D12GraphicsCommandList_CopyBufferRegion(indirect_setup, gpu_indirect, 0, indirect, 0, sizeof indirect_words);
    D3D12_RESOURCE_BARRIER indirect_setup_barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    indirect_setup_barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ gpu_indirect,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
    ID3D12GraphicsCommandList_ResourceBarrier(indirect_setup, 1, &indirect_setup_barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(indirect_setup, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(indirect_setup, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(indirect_setup));
    ID3D12CommandList *indirect_setup_lists[] = { (ID3D12CommandList *)indirect_setup };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, indirect_setup_lists);
    wait_queue(dev, queue);
    ID3D12GraphicsCommandList_Release(indirect_setup);

    ID3D12GraphicsCommandList *indirect_preflight = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&indirect_preflight));
    ID3D12GraphicsCommandList_SetPipelineState(indirect_preflight, pso);
    ID3D12GraphicsCommandList_OMSetRenderTargets(indirect_preflight, 1, &rtv, FALSE, &dsv);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(indirect_preflight, rs);
    ID3D12GraphicsCommandList_RSSetViewports(indirect_preflight, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(indirect_preflight, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(indirect_preflight, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_RESOURCE_BARRIER preflight_uav_barrier = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
    ID3D12GraphicsCommandList_ResourceBarrier(indirect_preflight, 1, &preflight_uav_barrier);
    ID3D12GraphicsCommandList_ExecuteIndirect(indirect_preflight, indirect_signature, 1, gpu_indirect, 0, NULL, 0);
    D3D12_RESOURCE_BARRIER preflight_rt_barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    preflight_rt_barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(indirect_preflight, 1, &preflight_rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION preflight_src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION preflight_dst = { rb_indirect_preflight, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    preflight_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM,
        64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(indirect_preflight, &preflight_dst, 0, 0, 0, &preflight_src, NULL);
    preflight_rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    preflight_rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(indirect_preflight, 1, &preflight_rt_barrier);
    indirect_setup_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    indirect_setup_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12GraphicsCommandList_ResourceBarrier(indirect_preflight, 1, &indirect_setup_barrier);
    CHECK(ID3D12GraphicsCommandList_Close(indirect_preflight));
    ID3D12CommandList *indirect_preflight_lists[] = { (ID3D12CommandList *)indirect_preflight };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, indirect_preflight_lists);
    wait_queue(dev, queue);
    ID3D12GraphicsCommandList_Release(indirect_preflight);
    unsigned char *preflight_pixels = NULL;
    CHECK(ID3D12Resource_Map(rb_indirect_preflight, 0, NULL, (void **)&preflight_pixels));
    const unsigned char *preflight_center = preflight_pixels + 32 * 256 + 32 * 4;
    if (!(abs(preflight_center[0] - 128) <= 1 && preflight_center[1] == 255 &&
          abs(preflight_center[2] - 64) <= 1 && preflight_center[3] == 255)) {
        printf("draw: staged GPU-only root/IA ExecuteIndirect pixel is (%u, %u, %u, %u), want (128, 255, 64, 255)\n",
               preflight_center[0], preflight_center[1], preflight_center[2], preflight_center[3]);
        bad++;
    } else printf("d3d12 ExecuteIndirect preflight: prior-submission GPU-only root/IA records staged, state-checked and rendered: ok\n");
    ID3D12Resource_Unmap(rb_indirect_preflight, 0, NULL);

    // Records copied earlier in this same list must be read back in order and checked against the
    // resulting resource states. The vertex bytes are valid, but the buffer remains COPY_DEST, so
    // state validation must skip the indirect draw instead of using the otherwise drawable data.
    ID3D12Resource *bad_indirect_vertex = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, sizeof verts, 0,
                                                  D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *bad_indirect_args = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, sizeof indirect_words, 0,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *bad_indirect_upload = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof indirect_words, 0,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *rb_bad_indirect = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0,
                                              D3D12_RESOURCE_STATE_COPY_DEST);
    if (bad || !bad_indirect_vertex || !bad_indirect_args || !bad_indirect_upload || !rb_bad_indirect) return 1;
    uint32_t bad_indirect_words[17];
    memcpy(bad_indirect_words, indirect_words, sizeof bad_indirect_words);
    D3D12_VERTEX_BUFFER_VIEW bad_indirect_vbv = { ID3D12Resource_GetGPUVirtualAddress(bad_indirect_vertex),
                                                  sizeof verts, sizeof verts[0] };
    memcpy(&bad_indirect_words[4], &bad_indirect_vbv, sizeof bad_indirect_vbv);
    CHECK(ID3D12Resource_Map(bad_indirect_upload, 0, NULL, &p));
    memcpy(p, bad_indirect_words, sizeof bad_indirect_words);
    ID3D12Resource_Unmap(bad_indirect_upload, 0, NULL);

    ID3D12GraphicsCommandList *bad_indirect_list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&bad_indirect_list));
    ID3D12GraphicsCommandList_SetPipelineState(bad_indirect_list, pso);
    ID3D12GraphicsCommandList_OMSetRenderTargets(bad_indirect_list, 1, &rtv, FALSE, &dsv);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(bad_indirect_list, rs);
    ID3D12GraphicsCommandList_RSSetViewports(bad_indirect_list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(bad_indirect_list, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(bad_indirect_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_ClearRenderTargetView(bad_indirect_list, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(bad_indirect_list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_CopyBufferRegion(bad_indirect_list, bad_indirect_vertex, 0, vb, 0, sizeof verts);
    ID3D12GraphicsCommandList_CopyBufferRegion(bad_indirect_list, bad_indirect_args, 0,
                                               bad_indirect_upload, 0, sizeof bad_indirect_words);
    D3D12_RESOURCE_BARRIER bad_args_barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    bad_args_barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ bad_indirect_args,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
    ID3D12GraphicsCommandList_ResourceBarrier(bad_indirect_list, 1, &bad_args_barrier);
    ID3D12GraphicsCommandList_ExecuteIndirect(bad_indirect_list, indirect_signature, 1,
                                               bad_indirect_args, 0, NULL, 0);
    D3D12_RESOURCE_BARRIER bad_rt_barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    bad_rt_barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(bad_indirect_list, 1, &bad_rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION bad_rt_src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION bad_rt_dst = { rb_bad_indirect, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    bad_rt_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM,
        64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(bad_indirect_list, &bad_rt_dst, 0, 0, 0, &bad_rt_src, NULL);
    bad_rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bad_rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(bad_indirect_list, 1, &bad_rt_barrier);
    bad_args_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    bad_args_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12GraphicsCommandList_ResourceBarrier(bad_indirect_list, 1, &bad_args_barrier);
    CHECK(ID3D12GraphicsCommandList_Close(bad_indirect_list));
    ID3D12CommandList *bad_indirect_lists[] = { (ID3D12CommandList *)bad_indirect_list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, bad_indirect_lists);
    wait_queue(dev, queue);
    ID3D12GraphicsCommandList_Release(bad_indirect_list);
    unsigned char *bad_indirect_pixels = NULL;
    CHECK(ID3D12Resource_Map(rb_bad_indirect, 0, NULL, (void **)&bad_indirect_pixels));
    const unsigned char *bad_indirect_center = bad_indirect_pixels + 32 * 256 + 32 * 4;
    if (!(bad_indirect_center[0] == 0 && bad_indirect_center[1] == 0 &&
          bad_indirect_center[2] == 255 && bad_indirect_center[3] == 255)) {
        printf("draw: same-list invalid-state GPU ExecuteIndirect pixel is (%u, %u, %u, %u), want blue clear (0, 0, 255, 255)\n",
               bad_indirect_center[0], bad_indirect_center[1], bad_indirect_center[2], bad_indirect_center[3]);
        bad++;
    } else printf("d3d12 ExecuteIndirect replay validation: same-list GPU-produced invalid IA state skipped: ok\n");
    ID3D12Resource_Unmap(rb_bad_indirect, 0, NULL);

    // The matching valid-state case copies both the vertex bytes and ExecuteIndirect records on this
    // list, then transitions each destination before the draw. Preflight must stage the GPU-produced
    // records at the ExecuteIndirect point and validate the VBV against the state reached so far.
    ID3D12Resource *valid_indirect_vertex = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, sizeof verts, 0,
                                                    D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *valid_indirect_args = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, sizeof indirect_words, 0,
                                                  D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *valid_indirect_upload = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof indirect_words, 0,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *rb_valid_indirect = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
    if (bad || !valid_indirect_vertex || !valid_indirect_args || !valid_indirect_upload || !rb_valid_indirect) return 1;
    uint32_t valid_indirect_words[17];
    memcpy(valid_indirect_words, indirect_words, sizeof valid_indirect_words);
    D3D12_VERTEX_BUFFER_VIEW valid_indirect_vbv = { ID3D12Resource_GetGPUVirtualAddress(valid_indirect_vertex),
                                                    sizeof verts, sizeof verts[0] };
    memcpy(&valid_indirect_words[4], &valid_indirect_vbv, sizeof valid_indirect_vbv);
    CHECK(ID3D12Resource_Map(valid_indirect_upload, 0, NULL, &p));
    memcpy(p, valid_indirect_words, sizeof valid_indirect_words);
    ID3D12Resource_Unmap(valid_indirect_upload, 0, NULL);

    ID3D12GraphicsCommandList *valid_indirect_list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&valid_indirect_list));
    ID3D12GraphicsCommandList_SetPipelineState(valid_indirect_list, pso);
    ID3D12GraphicsCommandList_OMSetRenderTargets(valid_indirect_list, 1, &rtv, FALSE, &dsv);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(valid_indirect_list, rs);
    ID3D12GraphicsCommandList_RSSetViewports(valid_indirect_list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(valid_indirect_list, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(valid_indirect_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_ClearRenderTargetView(valid_indirect_list, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(valid_indirect_list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_CopyBufferRegion(valid_indirect_list, valid_indirect_vertex, 0, vb, 0, sizeof verts);
    ID3D12GraphicsCommandList_CopyBufferRegion(valid_indirect_list, valid_indirect_args, 0,
                                               valid_indirect_upload, 0, sizeof valid_indirect_words);
    D3D12_RESOURCE_BARRIER valid_indirect_barriers[2] = { 0 };
    valid_indirect_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    valid_indirect_barriers[0].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ valid_indirect_vertex,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER };
    valid_indirect_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    valid_indirect_barriers[1].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ valid_indirect_args,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
    ID3D12GraphicsCommandList_ResourceBarrier(valid_indirect_list, 2, valid_indirect_barriers);
    ID3D12GraphicsCommandList_ExecuteIndirect(valid_indirect_list, indirect_signature, 1,
                                               valid_indirect_args, 0, NULL, 0);
    D3D12_RESOURCE_BARRIER valid_rt_barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    valid_rt_barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(valid_indirect_list, 1, &valid_rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION valid_rt_src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION valid_rt_dst = { rb_valid_indirect, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    valid_rt_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM,
        64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(valid_indirect_list, &valid_rt_dst, 0, 0, 0, &valid_rt_src, NULL);
    valid_rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    valid_rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(valid_indirect_list, 1, &valid_rt_barrier);
    valid_indirect_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    valid_indirect_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    valid_indirect_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
    valid_indirect_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    ID3D12GraphicsCommandList_ResourceBarrier(valid_indirect_list, 2, valid_indirect_barriers);
    CHECK(ID3D12GraphicsCommandList_Close(valid_indirect_list));
    ID3D12CommandList *valid_indirect_lists[] = { (ID3D12CommandList *)valid_indirect_list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, valid_indirect_lists);
    wait_queue(dev, queue);
    ID3D12GraphicsCommandList_Release(valid_indirect_list);
    unsigned char *valid_indirect_pixels = NULL;
    CHECK(ID3D12Resource_Map(rb_valid_indirect, 0, NULL, (void **)&valid_indirect_pixels));
    const unsigned char *valid_indirect_center = valid_indirect_pixels + 32 * 256 + 32 * 4;
    const unsigned char *valid_indirect_corner = valid_indirect_pixels + 2 * 256 + 2 * 4;
    if (!(abs(valid_indirect_center[0] - 128) <= 1 && valid_indirect_center[1] == 255 &&
          abs(valid_indirect_center[2] - 64) <= 1 && valid_indirect_center[3] == 255 &&
          valid_indirect_corner[0] == 0 && valid_indirect_corner[1] == 0 &&
          valid_indirect_corner[2] == 255 && valid_indirect_corner[3] == 255)) {
        printf("draw: same-list valid-state GPU ExecuteIndirect center is (%u, %u, %u, %u), corner is (%u, %u, %u, %u), expected tinted center and blue corner\n",
               valid_indirect_center[0], valid_indirect_center[1], valid_indirect_center[2], valid_indirect_center[3],
               valid_indirect_corner[0], valid_indirect_corner[1], valid_indirect_corner[2], valid_indirect_corner[3]);
        bad++;
    } else printf("d3d12 ExecuteIndirect replay validation: same-list GPU-produced IA data accepted after valid state transition: ok\n");
    ID3D12Resource_Unmap(rb_valid_indirect, 0, NULL);

    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, &dsv);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12CommandAllocator *bundle_allocator;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_BUNDLE, &IID_ID3D12CommandAllocator,
                                               (void **)&bundle_allocator));
    ID3D12GraphicsCommandList *bundle;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator, NULL,
                                         &IID_ID3D12GraphicsCommandList, (void **)&bundle));
    ID3D12GraphicsCommandList_SetPipelineState(bundle, pso);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(bundle, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_IASetVertexBuffers(bundle, 0, 1, &vbv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(bundle, &ibv);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(bundle, 0, 4, tint, 0);
    ID3D12GraphicsCommandList_ExecuteIndirect(bundle, native_indexed_signature, 1, native_indexed_args, 0, NULL, 0);
    CHECK(ID3D12GraphicsCommandList_Close(bundle));
    ID3D12CommandAllocator *bad_bundle_allocator;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_BUNDLE, &IID_ID3D12CommandAllocator,
                                               (void **)&bad_bundle_allocator));
    ID3D12GraphicsCommandList *bad_bundle;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bad_bundle_allocator, NULL,
                                         &IID_ID3D12GraphicsCommandList, (void **)&bad_bundle));
    ID3D12GraphicsCommandList_ClearRenderTargetView(bad_bundle, rtv, blue, 0, NULL);
    HRESULT bad_bundle_close = ID3D12GraphicsCommandList_Close(bad_bundle);
    int bundle_forbidden_rejected = bad_bundle_close == E_INVALIDARG;
    if (!bundle_forbidden_rejected) printf("draw: bundle Close returned %#lx for a forbidden RTV clear, want E_INVALIDARG\n", (unsigned long)bad_bundle_close);
    ID3D12GraphicsCommandList_Release(bad_bundle);
    ID3D12CommandAllocator_Release(bad_bundle_allocator);
    float before_bundle[4] = { 1, 0, 0, 1 };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, before_bundle, 0);
    ID3D12GraphicsCommandList_ExecuteBundle(list, bundle);
    ID3D12GraphicsCommandList_Release(bundle);
    ID3D12CommandAllocator_Release(bundle_allocator);
    // The direct draw after the bundle must see its root constants and IA bindings; the bundle also inherits
    // the parent's render target, root signature, viewport, and scissor rectangle.
    ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 3, 1, 0, 3, 0);
    D3D12_RESOURCE_BARRIER rt_barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    rt_barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION bundle_dst = { rb_bundle, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    bundle_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    src.SubresourceIndex = 0;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &bundle_dst, 0, 0, 0, &src, NULL);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_ExecuteIndirect(list, indirect_signature, 1, indirect, 0, NULL, 0);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    // Exercise a GPU-only argument stream that updates root constants and both IA views. The count and records
    // are produced by GPU copies, so compatibility replay must stage them before expanding the signature.
    ID3D12GraphicsCommandList_CopyBufferRegion(list, gpu_indirect, 0, indirect, 0, sizeof indirect_words);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, gpu_indirect_count, 0, native_count_upload, 0, sizeof(uint32_t));
    D3D12_RESOURCE_BARRIER gpu_indirect_barriers[2] = { 0 };
    gpu_indirect_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    gpu_indirect_barriers[0].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ gpu_indirect,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
    gpu_indirect_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    gpu_indirect_barriers[1].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ gpu_indirect_count,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, gpu_indirect_barriers);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_ExecuteIndirect(list, indirect_signature, 1, gpu_indirect, 0, gpu_indirect_count, 0);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION gpu_indirect_dst = { rb_gpu_indirect, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    gpu_indirect_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &gpu_indirect_dst, 0, 0, 0, &src, NULL);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    // ExecuteIndirect resets the root constants and IA bindings it changed. Rebind only the IA views and check that
    // the following direct draw sees zero root constants instead of the indirect draw's tint.
    float green[4] = { 0, 1, 0, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, green, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    float zero_count_tint[4] = { 1, 0, 0, 1 };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, zero_count_tint, 0);
    ID3D12GraphicsCommandList_ExecuteIndirect(list, indirect_signature, 1, indirect, 0, zero_count_buffer, 0);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 3, 1, 0, 3, 0);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION reset_dst = { rb_reset, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    reset_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &reset_dst, 0, 0, 0, &src, NULL);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    float native_draw_tint[4] = { 0.25f, 0.5f, 1, 1 };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, native_draw_tint, 0);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_ExecuteIndirect(list, native_draw_signature, 2, native_draw_args, 0, NULL, 0);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION native_draw_dst = { rb_native_draw, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    native_draw_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &native_draw_dst, 0, 0, 0, &src, NULL);

    // The count lives in a GPU-only DEFAULT resource. A preceding blit writes it, then ExecuteIndirect must clamp
    // the two-record stream to one draw without reading the count or arguments back to the CPU.
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, native_count, 0, native_count_upload, 0, 4);
    D3D12_RESOURCE_BARRIER count_barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    count_barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ native_count, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &count_barrier);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, native_draw_tint, 0);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_ExecuteIndirect(list, native_draw_signature, 2, native_draw_args, 0, native_count, 0);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION native_count_dst = { rb_native_count, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    native_count_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &native_count_dst, 0, 0, 0, &src, NULL);

    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, green, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    float native_indexed_tint[4] = { 1, 0.25f, 0.5f, 1 };
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, native_indexed_tint, 0);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv);
    ID3D12GraphicsCommandList_ExecuteIndirect(list, native_indexed_signature, 2, native_indexed_args, 0, NULL, 0);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION native_indexed_dst = { rb_native_indexed, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    native_indexed_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &native_indexed_dst, 0, 0, 0, &src, NULL);

    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, green, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, native_indexed_tint, 0);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv);
    ID3D12GraphicsCommandList_ExecuteIndirect(list, native_indexed_signature, 2, native_indexed_args, 0, native_count, 0);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    D3D12_TEXTURE_COPY_LOCATION native_indexed_count_dst = {
        rb_native_indexed_count, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    native_indexed_count_dst.PlacedFootprint.Footprint =
        (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &native_indexed_count_dst, 0, 0, 0, &src, NULL);
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
    ID3D12Resource_Unmap(rb, 0, NULL);
    CHECK(ID3D12Resource_Map(rb_gpu_indirect, 0, NULL, (void **)&px));
    center = px + 32 * 256 + 32 * 4; corner = px;
    int gpu_root_ia_count_hit = abs(center[0] - 128) <= 1 && center[1] == 255 &&
                                abs(center[2] - 64) <= 1 && center[3] == 255;
    int gpu_root_ia_count_bg = corner[0] == 0 && corner[1] == 0 && corner[2] == 255 && corner[3] == 255;
    if (!gpu_root_ia_count_hit) { printf("draw: GPU-only root/IA ExecuteIndirect pixel is (%u, %u, %u, %u), want (128, 255, 64, 255)\n", center[0], center[1], center[2], center[3]); bad++; }
    if (!gpu_root_ia_count_bg) { printf("draw: GPU-only root/IA ExecuteIndirect corner is (%u, %u, %u, %u), want blue clear\n", corner[0], corner[1], corner[2], corner[3]); bad++; }
    ID3D12Resource_Unmap(rb_gpu_indirect, 0, NULL);
    CHECK(ID3D12Resource_Map(rb_bundle, 0, NULL, (void **)&px));
    center = px + 32 * 256 + 32 * 4; corner = px;
    int bundle_hit = abs(center[0] - 128) <= 1 && center[1] == 255 && abs(center[2] - 64) <= 1 && center[3] == 255;
    int bundle_bg = corner[0] == 0 && corner[1] == 0 && corner[2] == 255 && corner[3] == 255;
    if (!bundle_hit) { printf("draw: bundle draw or leaked root constants pixel is (%u, %u, %u, %u), want (128, 255, 64, 255)\n", center[0], center[1], center[2], center[3]); bad++; }
    if (!bundle_bg) { printf("draw: bundle background is (%u, %u, %u, %u), want blue clear\n", corner[0], corner[1], corner[2], corner[3]); bad++; }
    ID3D12Resource_Unmap(rb_bundle, 0, NULL);
    CHECK(ID3D12Resource_Map(rb_reset, 0, NULL, (void **)&px));
    center = px + 32 * 256 + 32 * 4; corner = px;
    int reset_constants = center[0] == 0 && center[1] == 0 && center[2] == 0 && center[3] == 0;
    int reset_background = corner[0] == 0 && corner[1] == 255 && corner[2] == 0 && corner[3] == 255;
    if (!reset_constants) { printf("draw: after indirect reset, direct draw pixel is (%u, %u, %u, %u), want zero root constants\n", center[0], center[1], center[2], center[3]); bad++; }
    if (!reset_background) { printf("draw: post-reset corner is (%u, %u, %u, %u), want green clear\n", corner[0], corner[1], corner[2], corner[3]); bad++; }
    CHECK(ID3D12Resource_Map(rb_native_draw, 0, NULL, (void **)&px));
    center = px + 32 * 256 + 32 * 4; corner = px;
    int native_draw_hit = abs(center[0] - 64) <= 1 && abs(center[1] - 128) <= 1 && center[2] == 255 && center[3] == 255;
    int native_draw_bg = corner[0] == 0 && corner[1] == 0 && corner[2] == 255 && corner[3] == 255;
    if (!native_draw_hit) { printf("draw: non-indexed native indirect pixel is (%u, %u, %u, %u), want (64, 128, 255, 255)\n", center[0], center[1], center[2], center[3]); bad++; }
    if (!native_draw_bg) { printf("draw: non-indexed native indirect corner is (%u, %u, %u, %u), want blue clear\n", corner[0], corner[1], corner[2], corner[3]); bad++; }
    ID3D12Resource_Unmap(rb_native_draw, 0, NULL);
    CHECK(ID3D12Resource_Map(rb_native_count, 0, NULL, (void **)&px));
    center = px + 32 * 256 + 32 * 4; corner = px;
    const unsigned char *second_triangle = px + 8 * 256 + 8 * 4;
    int native_count_hit = abs(center[0] - 64) <= 1 && abs(center[1] - 128) <= 1 &&
                           center[2] == 255 && center[3] == 255;
    int native_count_clamped = second_triangle[0] == 0 && second_triangle[1] == 0 &&
                               second_triangle[2] == 255 && second_triangle[3] == 255;
    int native_count_bg = corner[0] == 0 && corner[1] == 0 && corner[2] == 255 && corner[3] == 255;
    if (!native_count_hit) { printf("draw: GPU-counted native indirect pixel is (%u, %u, %u, %u), want (64, 128, 255, 255) from the active first record\n", center[0], center[1], center[2], center[3]); bad++; }
    if (!native_count_clamped) { printf("draw: GPU count did not suppress the second record's pixel: (%u, %u, %u, %u)\n", second_triangle[0], second_triangle[1], second_triangle[2], second_triangle[3]); bad++; }
    if (!native_count_bg) { printf("draw: GPU-counted native indirect corner is (%u, %u, %u, %u), want blue clear\n", corner[0], corner[1], corner[2], corner[3]); bad++; }
    ID3D12Resource_Unmap(rb_native_count, 0, NULL);
    CHECK(ID3D12Resource_Map(rb_native_indexed, 0, NULL, (void **)&px));
    center = px + 32 * 256 + 32 * 4; corner = px;
    int native_indexed_hit = abs(center[0] - 255) <= 1 && abs(center[1] - 64) <= 1 && abs(center[2] - 128) <= 1 && center[3] == 255;
    int native_indexed_bg = corner[0] == 0 && corner[1] == 255 && corner[2] == 0 && corner[3] == 255;
    if (!native_indexed_hit) { printf("draw: indexed native indirect pixel is (%u, %u, %u, %u), want (255, 64, 128, 255)\n", center[0], center[1], center[2], center[3]); bad++; }
    if (!native_indexed_bg) { printf("draw: indexed native indirect corner is (%u, %u, %u, %u), want green clear\n", corner[0], corner[1], corner[2], corner[3]); bad++; }
    ID3D12Resource_Unmap(rb_native_indexed, 0, NULL);
    CHECK(ID3D12Resource_Map(rb_native_indexed_count, 0, NULL, (void **)&px));
    center = px + 32 * 256 + 32 * 4; corner = px;
    second_triangle = px + 8 * 256 + 8 * 4;
    int native_indexed_count_hit = abs(center[0] - 255) <= 1 && abs(center[1] - 64) <= 1 &&
                                   abs(center[2] - 128) <= 1 && center[3] == 255;
    int native_indexed_count_clamped = second_triangle[0] == 0 && second_triangle[1] == 255 &&
                                       second_triangle[2] == 0 && second_triangle[3] == 255;
    if (!native_indexed_count_hit) { printf("draw: GPU-counted indexed indirect pixel is (%u, %u, %u, %u), want (255, 64, 128, 255)\n", center[0], center[1], center[2], center[3]); bad++; }
    if (!native_indexed_count_clamped) { printf("draw: GPU count did not suppress the indexed second record's pixel: (%u, %u, %u, %u)\n", second_triangle[0], second_triangle[1], second_triangle[2], second_triangle[3]); bad++; }
    ID3D12Resource_Unmap(rb_native_indexed_count, 0, NULL);

    // A mask with sample zero disabled must clear fragment coverage before the output merger writes color.
    CHECK(ID3D12CommandAllocator_Reset(alloc));
    CHECK(ID3D12GraphicsCommandList_Reset(list, alloc, masked_pso));
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, blue, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, &dsv);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    D3D12_VIEWPORT masked_vp = { 0, 0, 64, 64, 0, 1 };
    D3D12_RECT masked_sc = { 0, 0, 64, 64 };
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &masked_vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &masked_sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(list, &ibv);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, tint, 0);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(list, 3, 1, 0, 3, 0);
    rt_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rt_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &rt_barrier);
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    CHECK(ID3D12Resource_Map(rb, 0, NULL, (void **)&px));
    int sample_mask_empty = 1;
    for (int y = 0; y < 64 && sample_mask_empty; y++) for (int x = 0; x < 64; x++) {
        const unsigned char *pixel = px + y * 256 + x * 4;
        if (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 255 || pixel[3] != 255) {
            sample_mask_empty = 0;
            printf("draw: PSO sample mask wrote pixel (%d, %d) = (%u, %u, %u, %u), expected blue clear\n",
                   x, y, pixel[0], pixel[1], pixel[2], pixel[3]);
            break;
        }
    }
    ID3D12Resource_Unmap(rb, 0, NULL);
    if (!sample_mask_empty) bad++;
    printf("d3d12 draw: graphics pipeline from DXIL, PSO sample-mask coverage, indirect root constants, vertex/index views and indexed draw, clears, "
           "restricted bundle ExecuteIndirect, GPU-only root/IA/count readback fallback, inheritance and state leakage, state reset, native multi-draw, GPU-counted indexed and non-indexed draw clamps, depth test, %d pixels covered: %s\n", covered,
           bundle_forbidden_rejected && bundle_hit && bundle_bg && hit && bg && reset_constants && reset_background && native_draw_hit && native_draw_bg &&
           gpu_root_ia_count_hit && gpu_root_ia_count_bg && native_count_hit && native_count_clamped && native_count_bg && native_indexed_hit && native_indexed_bg && sample_mask_empty &&
           native_indexed_count_hit && native_indexed_count_clamped ? "ok" : "FAILED");
    ID3D12PipelineState_Release(masked_pso);
    return 0;
}

static int resolve_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC source_desc = {
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        { 4, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    D3D12_RESOURCE_DESC destination_desc = source_desc;
    destination_desc.SampleDesc.Count = 1;
    ID3D12Resource *source, *destination;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &source_desc,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL,
                                                &IID_ID3D12Resource, (void **)&source));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &destination_desc,
                                                D3D12_RESOURCE_STATE_RESOLVE_DEST, NULL,
                                                &IID_ID3D12Resource, (void **)&destination));
    D3D12_DESCRIPTOR_HEAP_DESC view_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *view_heap;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &view_heap_desc, &IID_ID3D12DescriptorHeap, (void **)&view_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(view_heap);
    ID3D12Device_CreateRenderTargetView(dev, source, NULL, rtv);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12CommandAllocator *allocator;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                                          &IID_ID3D12GraphicsCommandList, (void **)&list));
    float color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, color, 0, NULL);
    D3D12_RESOURCE_BARRIER barriers[2] = { 0 };
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){
        source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE
    };
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){
        destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE
    };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barriers[0]);
    ID3D12GraphicsCommandList1 *list1;
    CHECK(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList1, (void **)&list1));
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, destination, 0, 0, 0, source, 0, NULL,
                                                         DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOLVE_MODE_AVERAGE);
    D3D12_RECT full_source_rect = { 0, 0, 16, 16 };
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, destination, 0, 0, 0, source, 0,
                                                         &full_source_rect, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                         D3D12_RESOLVE_MODE_AVERAGE);
    ID3D12GraphicsCommandList1_Release(list1);
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barriers[1]);
    D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12GraphicsCommandList *invalid_list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                                          &IID_ID3D12GraphicsCommandList, (void **)&invalid_list));
    ID3D12GraphicsCommandList1 *invalid_list1;
    CHECK(ID3D12GraphicsCommandList_QueryInterface(invalid_list, &IID_ID3D12GraphicsCommandList1,
                                                    (void **)&invalid_list1));
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(invalid_list1, destination, 0, 1, 0, source, 0, NULL,
                                                         DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOLVE_MODE_AVERAGE);
    ID3D12GraphicsCommandList1_Release(invalid_list1);
    HRESULT invalid_close = ID3D12GraphicsCommandList_Close(invalid_list);
    printf("d3d12 ResolveSubresourceRegion: out-of-bounds destination region rejected at Close: %s\n",
           invalid_close == E_INVALIDARG ? "ok" : "FAILED");
    bad += invalid_close != E_INVALIDARG;
    ID3D12GraphicsCommandList_Release(invalid_list);
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    unsigned char *pixels;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
    int wrong = 0;
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
        unsigned char *pixel = pixels + y * 256 + x * 4;
        wrong += abs((int)pixel[0] - 64) > 1 || abs((int)pixel[1] - 128) > 1 ||
                 abs((int)pixel[2] - 191) > 1 || pixel[3] != 255;
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    printf("d3d12 resolve: 4x multisample color clear resolved into a single-sample texture, %d wrong pixels: %s\n",
           wrong, wrong ? "FAILED" : "ok");
    bad += wrong != 0;
    return 0;
}

static int resolve_region_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC source_desc = {
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
        { 4, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    D3D12_RESOURCE_DESC destination_desc = source_desc;
    destination_desc.SampleDesc.Count = 1;
    ID3D12Resource *source = NULL, *destination = NULL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &source_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&source));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &destination_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&destination));
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
    ID3D12DescriptorHeap *views = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&views));
    D3D12_CPU_DESCRIPTOR_HANDLE source_rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(views);
    D3D12_CPU_DESCRIPTOR_HANDLE destination_rtv = source_rtv;
    destination_rtv.ptr += ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Device_CreateRenderTargetView(dev, source, NULL, source_rtv);
    ID3D12Device_CreateRenderTargetView(dev, destination, NULL, destination_rtv);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 8, 0,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) return 1;

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    const float green[4] = { 0, 1, 0, 1 }, red[4] = { 1, 0, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, source_rtv, green, 0, NULL);
    D3D12_RECT source_clear = { 1, 1, 4, 4 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, source_rtv, red, 1, &source_clear);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, destination_rtv, blue, 0, NULL);
    D3D12_RESOURCE_BARRIER barriers[2] = { 0 };
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){
        source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE
    };
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){
        destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_DEST
    };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, barriers);
    ID3D12GraphicsCommandList1 *list1 = NULL;
    CHECK(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList1, (void **)&list1));
    D3D12_RECT source_region = { 1, 1, 4, 4 };
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, destination, 0, 3, 2, source, 0,
        &source_region, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOLVE_MODE_AVERAGE);
    ID3D12GraphicsCommandList1_Release(list1);
    barriers[0].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){
        destination, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE
    };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, barriers);
    D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    unsigned char *pixels = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
    int wrong = 0;
    for (UINT y = 0; y < 8; y++) for (UINT x = 0; x < 8; x++) {
        const unsigned char *pixel = pixels + y * 256 + x * 4;
        int resolved = x >= 3 && x < 6 && y >= 2 && y < 5;
        wrong += resolved ? pixel[0] != 255 || pixel[1] != 0 || pixel[2] != 0 || pixel[3] != 255 :
                             pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 255 || pixel[3] != 255;
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    printf("d3d12 ResolveSubresourceRegion: 3x3 source rect at (1,1) resolved to (3,2), outside preserved, %d wrong pixels: %s\n",
           wrong, wrong ? "FAILED" : "ok");
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(views);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(destination);
    ID3D12Resource_Release(source);
    return wrong != 0;
}

static int resolve_integer_region_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    const UINT width = 6, height = 5, row_pitch = 256, image_bytes = row_pitch * height;
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC source_desc = {
        D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UINT,
        { 4, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
    };
    D3D12_RESOURCE_DESC target_desc = source_desc;
    target_desc.SampleDesc.Count = 1;
    ID3D12Resource *source = NULL, *minimum = NULL, *maximum = NULL, *full_maximum = NULL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &source_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&source));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &target_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&minimum));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &target_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&maximum));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &target_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&full_maximum));
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, image_bytes * 3,
        0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) return 1;

    D3D12_DESCRIPTOR_HEAP_DESC source_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    D3D12_DESCRIPTOR_HEAP_DESC target_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
    ID3D12DescriptorHeap *source_heap = NULL, *target_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &source_heap_desc, &IID_ID3D12DescriptorHeap,
        (void **)&source_heap));
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &target_heap_desc, &IID_ID3D12DescriptorHeap,
        (void **)&target_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE source_rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(source_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE target_rtvs[3];
    target_rtvs[0] = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(target_heap);
    const UINT rtv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 1; i < 3; i++) {
        target_rtvs[i] = target_rtvs[0];
        target_rtvs[i].ptr += (SIZE_T)i * rtv_increment;
    }
    ID3D12Device_CreateRenderTargetView(dev, source, NULL, source_rtv);
    ID3D12Device_CreateRenderTargetView(dev, minimum, NULL, target_rtvs[0]);
    ID3D12Device_CreateRenderTargetView(dev, maximum, NULL, target_rtvs[1]);
    ID3D12Device_CreateRenderTargetView(dev, full_maximum, NULL, target_rtvs[2]);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *invalid_list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&invalid_list));
    ID3D12GraphicsCommandList1 *invalid_list1 = NULL;
    CHECK(ID3D12GraphicsCommandList_QueryInterface(invalid_list, &IID_ID3D12GraphicsCommandList1,
        (void **)&invalid_list1));
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(invalid_list1, minimum, 0, 0, 0, source, 0,
        NULL, DXGI_FORMAT_R8G8B8A8_UINT, D3D12_RESOLVE_MODE_AVERAGE);
    ID3D12GraphicsCommandList1_Release(invalid_list1);
    HRESULT integer_average_close = ID3D12GraphicsCommandList_Close(invalid_list);
    int integer_average_rejected = integer_average_close == E_INVALIDARG;
    ID3D12GraphicsCommandList_Release(invalid_list);
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    const D3D12_RECT seed_rect = { 0, 0, width, height };
    const float source_value[4] = { 17, 53, 101, 201 };
    const float untouched_value[4] = { 7, 11, 13, 19 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, source_rtv, source_value, 1, &seed_rect);
    for (UINT i = 0; i < 3; i++)
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, target_rtvs[i], untouched_value, 1, &seed_rect);

    D3D12_RESOURCE_BARRIER barriers[4] = { 0 };
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ source,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RESOLVE_SOURCE };
    ID3D12Resource *targets[3] = { minimum, maximum, full_maximum };
    for (UINT i = 0; i < 3; i++) {
        barriers[i + 1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i + 1].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ targets[i],
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RESOLVE_DEST };
    }
    ID3D12GraphicsCommandList_ResourceBarrier(list, 4, barriers);
    ID3D12GraphicsCommandList1 *list1 = NULL;
    CHECK(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList1, (void **)&list1));
    D3D12_RECT source_region = { 1, 1, 4, 4 };
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, minimum, 0, 2, 1, source, 0,
        &source_region, DXGI_FORMAT_R8G8B8A8_UINT, D3D12_RESOLVE_MODE_MIN);
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, maximum, 0, 2, 1, source, 0,
        &source_region, DXGI_FORMAT_R8G8B8A8_UINT, D3D12_RESOLVE_MODE_MAX);
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, full_maximum, 0, 0, 0, source, 0,
        NULL, DXGI_FORMAT_R8G8B8A8_UINT, D3D12_RESOLVE_MODE_MAX);
    ID3D12GraphicsCommandList1_Release(list1);
    for (UINT i = 0; i < 3; i++) {
        barriers[i].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ targets[i],
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RESOLVE_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE };
    }
    ID3D12GraphicsCommandList_ResourceBarrier(list, 3, barriers);
    D3D12_TEXTURE_COPY_LOCATION copy_dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    copy_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){
        DXGI_FORMAT_R8G8B8A8_UINT, width, height, 1, row_pitch };
    D3D12_TEXTURE_COPY_LOCATION copy_src = { NULL, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    for (UINT i = 0; i < 3; i++) {
        copy_dst.PlacedFootprint.Offset = (uint64_t)i * image_bytes;
        copy_src.pResource = targets[i];
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &copy_dst, 0, 0, 0, &copy_src, NULL);
    }
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    unsigned char *pixels = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
    int wrong = integer_average_rejected ? 0 : 1;
    for (UINT result = 0; result < 3; result++) for (UINT y = 0; y < height; y++) for (UINT x = 0; x < width; x++) {
        const unsigned char *pixel = pixels + (size_t)result * image_bytes + (size_t)y * row_pitch + x * 4;
        int resolved = result == 2 || (x >= 2 && x < 5 && y >= 1 && y < 4);
        for (UINT channel = 0; channel < 4; channel++) {
            unsigned char expected = (unsigned char)(resolved ? source_value[channel] : untouched_value[channel]);
            wrong += pixel[channel] != expected;
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    printf("d3d12 integer color resolve: integer AVERAGE rejected; partial MIN/MAX and full MAX typed paths, outside preserved, %d wrong values: %s\n",
        wrong, wrong ? "FAILED" : "ok");
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(source_heap);
    ID3D12DescriptorHeap_Release(target_heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(full_maximum);
    ID3D12Resource_Release(maximum);
    ID3D12Resource_Release(minimum);
    ID3D12Resource_Release(source);
    return wrong != 0;
}

static int depth_resolve_format_test(ID3D12Device *dev, ID3D12CommandQueue *queue, uint32_t format, int unorm16) {
    const UINT width = 4, height = 4, row_pitch = 256, image_bytes = row_pitch * height;
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC source_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
        format, { 4, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL };
    D3D12_RESOURCE_DESC destination_desc = source_desc;
    destination_desc.SampleDesc.Count = 1;
    D3D12_CLEAR_VALUE clear_value = { format };
    clear_value.DepthStencil.Depth = 0.375f;
    ID3D12Resource *source = NULL, *destinations[4] = { 0 };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &source_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear_value, &IID_ID3D12Resource, (void **)&source));
    for (UINT i = 0; i < 4; i++)
        CHECK(ID3D12Device_CreateCommittedResource(dev, &heap, D3D12_HEAP_FLAG_NONE, &destination_desc,
            D3D12_RESOURCE_STATE_RESOLVE_DEST, &clear_value, &IID_ID3D12Resource, (void **)&destinations[i]));
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, image_bytes * 4,
        0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) return 1;
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 };
    ID3D12DescriptorHeap *dsv_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &dsv_heap_desc, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = { format, D3D12_DSV_DIMENSION_TEXTURE2DMS };
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap);
    ID3D12Device_CreateDepthStencilView(dev, source, &dsv_desc, dsv);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList1 *list1 = NULL;
    CHECK(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList1, (void **)&list1));
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 0.375f, 0, 0, NULL);
    D3D12_RESOURCE_BARRIER barriers[5] = { 0 };
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_RESOLVE_SOURCE };
    for (UINT i = 0; i < 4; i++) {
        barriers[i + 1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i + 1].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ destinations[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
    }
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barriers[0]);
    // Keep each operation's output separate; otherwise only the final resolve is observed below.
    ID3D12GraphicsCommandList_ResolveSubresource(list, destinations[0], 0, source, 0, format);
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, destinations[1], 0, 0, 0, source, 0, NULL,
        format, D3D12_RESOLVE_MODE_AVERAGE);
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, destinations[2], 0, 0, 0, source, 0, NULL,
        format, D3D12_RESOLVE_MODE_MIN);
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, destinations[3], 0, 0, 0, source, 0, NULL,
        format, D3D12_RESOLVE_MODE_MAX);
    ID3D12GraphicsCommandList1_Release(list1);
    ID3D12GraphicsCommandList_ResourceBarrier(list, 4, &barriers[1]);
    D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ format, width, height, 1, row_pitch };
    D3D12_TEXTURE_COPY_LOCATION src = { NULL, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    for (UINT i = 0; i < 4; i++) {
        dst.PlacedFootprint.Offset = (uint64_t)i * image_bytes;
        src.pResource = destinations[i];
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    }
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);
    unsigned char *pixels = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
    int wrong = 0;
    for (UINT result = 0; result < 4; result++) for (UINT y = 0; y < height; y++) for (UINT x = 0; x < width; x++) {
        const unsigned char *pixel = pixels + (size_t)result * image_bytes + (size_t)y * row_pitch + x * (unorm16 ? 2 : 4);
        if (unorm16) {
            uint16_t value;
            memcpy(&value, pixel, sizeof value);
            wrong += abs((int)value - (int)(0.375f * 65535.0f + 0.5f)) > 1;
        } else {
            float value;
            memcpy(&value, pixel, sizeof value);
            wrong += fabsf(value - 0.375f) > 0.00001f;
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    printf("d3d12 depth resolve: legacy average and ResolveSubresourceRegion AVERAGE/MIN/MAX from 4x %s, %d wrong pixels across four outputs: %s\n",
        unorm16 ? "D16" : "D32",
        wrong, wrong ? "FAILED" : "ok");
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(dsv_heap);
    ID3D12Resource_Release(readback);
    for (UINT i = 0; i < 4; i++) ID3D12Resource_Release(destinations[i]);
    ID3D12Resource_Release(source);
    return wrong ? 1 : 0;
}

static int depth_resolve_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    int bad = 0;
    bad += depth_resolve_format_test(dev, queue, DXGI_FORMAT_D32_FLOAT, 0);
    bad += depth_resolve_format_test(dev, queue, DXGI_FORMAT_D16_UNORM, 1);
    return bad != 0;
}

static int depth_resolve_average_samples_test(ID3D12Device *dev, ID3D12CommandQueue *queue,
                                              uint32_t format, int unorm16) {
    static unsigned char vs[65536], ps[65536];
    size_t vs_size, ps_size;
    if (load("tests/shaders12/tri_vs.dxil", vs, sizeof vs, &vs_size) ||
        load("tests/shaders12/tri_ps.dxil", ps, sizeof ps, &ps_size)) return 1;
    const UINT width = 4, height = 4, row_pitch = 256, image_bytes = row_pitch * height;
    const float sample_depths[4] = { 0.125f, 0.375f, 0.625f, 0.875f };
    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC depth_ms_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 2, 1,
        format, { 4, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL };
    D3D12_RESOURCE_DESC color_ms_desc = depth_ms_desc;
    color_ms_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    color_ms_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_DESC depth_single_desc = depth_ms_desc;
    depth_single_desc.SampleDesc.Count = 1;
    D3D12_RESOURCE_DESC color_single_desc = color_ms_desc;
    color_single_desc.SampleDesc.Count = 1;
    D3D12_CLEAR_VALUE depth_clear = { format };
    depth_clear.DepthStencil.Depth = 1.0f;
    ID3D12Resource *source_depth = NULL, *color = NULL, *average = NULL, *minimum = NULL, *maximum = NULL;
    ID3D12Resource *color_minimum = NULL, *color_maximum = NULL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &depth_ms_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, &IID_ID3D12Resource, (void **)&source_depth));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &color_ms_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&color));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &depth_single_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, &IID_ID3D12Resource, (void **)&average));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &depth_single_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, &IID_ID3D12Resource, (void **)&minimum));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &depth_single_desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, &IID_ID3D12Resource, (void **)&maximum));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &color_single_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&color_minimum));
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &color_single_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&color_maximum));
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, image_bytes * 5,
        0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) return 1;

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    D3D12_DESCRIPTOR_HEAP_DESC color_target_rtv_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2 };
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 4 };
    ID3D12DescriptorHeap *rtv_heap = NULL, *color_target_rtv_heap = NULL, *dsv_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &color_target_rtv_heap_desc, &IID_ID3D12DescriptorHeap,
        (void **)&color_target_rtv_heap));
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &dsv_heap_desc, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap));
    D3D12_RENDER_TARGET_VIEW_DESC rtv_desc = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY };
    rtv_desc.Texture2DMSArray.FirstArraySlice = 1;
    rtv_desc.Texture2DMSArray.ArraySize = 1;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = { format, D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY };
    dsv_desc.Texture2DMSArray.FirstArraySlice = 1;
    dsv_desc.Texture2DMSArray.ArraySize = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE color_target_rtvs[2];
    color_target_rtvs[0] = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(color_target_rtv_heap);
    color_target_rtvs[1] = color_target_rtvs[0];
    color_target_rtvs[1].ptr += ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap);
    ID3D12Device_CreateRenderTargetView(dev, color, &rtv_desc, rtv);
    D3D12_RENDER_TARGET_VIEW_DESC color_target_rtv_desc = { DXGI_FORMAT_R8G8B8A8_UNORM,
        D3D12_RTV_DIMENSION_TEXTURE2DARRAY };
    color_target_rtv_desc.Texture2DArray.FirstArraySlice = 1;
    color_target_rtv_desc.Texture2DArray.ArraySize = 1;
    ID3D12Device_CreateRenderTargetView(dev, color_minimum, &color_target_rtv_desc, color_target_rtvs[0]);
    ID3D12Device_CreateRenderTargetView(dev, color_maximum, &color_target_rtv_desc, color_target_rtvs[1]);
    ID3D12Device_CreateDepthStencilView(dev, source_depth, &dsv_desc, dsv);
    const UINT dsv_increment = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    D3D12_DEPTH_STENCIL_VIEW_DESC destination_dsv_desc = { format, D3D12_DSV_DIMENSION_TEXTURE2DARRAY };
    destination_dsv_desc.Texture2DArray.FirstArraySlice = 1;
    destination_dsv_desc.Texture2DArray.ArraySize = 1;
    ID3D12Resource *depth_targets[3] = { average, minimum, maximum };
    D3D12_CPU_DESCRIPTOR_HANDLE destination_dsvs[3];
    for (UINT i = 0; i < 3; i++) {
        destination_dsvs[i] = dsv;
        destination_dsvs[i].ptr += (SIZE_T)(i + 1) * dsv_increment;
        ID3D12Device_CreateDepthStencilView(dev, depth_targets[i], &destination_dsv_desc, destination_dsvs[i]);
    }

    D3D12_ROOT_PARAMETER root_param = { D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS };
    root_param.Constants.ShaderRegister = 0;
    root_param.Constants.Num32BitValues = 4;
    root_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = { 1, &root_param, 0, NULL,
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob *serialized = NULL, *errors = NULL;
    CHECK(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
    ID3D12RootSignature *root = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(serialized),
        ID3D10Blob_GetBufferSize(serialized), &IID_ID3D12RootSignature, (void **)&root));
    D3D12_INPUT_ELEMENT_DESC elements[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    memset(&pso_desc, 0, sizeof pso_desc);
    pso_desc.pRootSignature = root;
    pso_desc.VS = (D3D12_SHADER_BYTECODE){ vs, vs_size };
    pso_desc.PS = (D3D12_SHADER_BYTECODE){ ps, ps_size };
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.DepthStencilState.DepthEnable = TRUE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso_desc.InputLayout = (D3D12_INPUT_LAYOUT_DESC){ elements, 2 };
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.DSVFormat = format;
    pso_desc.SampleDesc.Count = 4;
    ID3D12PipelineState *pipelines[4] = { 0 };
    for (UINT sample = 0; sample < 4; sample++) {
        pso_desc.SampleMask = 1u << sample;
        CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pso_desc, &IID_ID3D12PipelineState,
            (void **)&pipelines[sample]));
    }

    const float vertices[3][6] = {
        { -1, -1, 1, 1, 1, 1 }, { 3, -1, 1, 1, 1, 1 }, { -1, 3, 1, 1, 1, 1 }
    };
    ID3D12Resource *vertex_buffer = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof vertices, 0,
        D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!vertex_buffer) return 1;
    void *mapped_vertices = NULL;
    CHECK(ID3D12Resource_Map(vertex_buffer, 0, NULL, &mapped_vertices));
    memcpy(mapped_vertices, vertices, sizeof vertices);
    ID3D12Resource_Unmap(vertex_buffer, 0, NULL);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vertex_buffer), sizeof vertices, sizeof vertices[0] };
    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pipelines[0],
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    float clear_color[4] = { 0, 0, 0, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear_color, 0, NULL);
    const float untouched_color[4] = { 0.9f, 0.1f, 0.9f, 0.1f };
    for (UINT i = 0; i < 2; i++)
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, color_target_rtvs[i], untouched_color, 0, NULL);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
    const float untouched_depth = 0.25f;
    for (UINT i = 0; i < 3; i++)
        ID3D12GraphicsCommandList_ClearDepthStencilView(list, destination_dsvs[i], D3D12_CLEAR_FLAG_DEPTH,
            untouched_depth, 0, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, &dsv);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
    const float sample_colors[4][4] = {
        { 0.1f, 0.8f, 0.4f, 0.6f }, { 0.4f, 0.3f, 0.8f, 0.5f },
        { 0.7f, 0.6f, 0.2f, 0.7f }, { 0.2f, 0.4f, 0.6f, 0.8f },
    };
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
    D3D12_RECT scissor = { 0, 0, width, height };
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &scissor);
    for (UINT sample = 0; sample < 4; sample++) {
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 0, 4, sample_colors[sample], 0);
        ID3D12GraphicsCommandList_SetPipelineState(list, pipelines[sample]);
        D3D12_VIEWPORT viewport = { 0, 0, width, height, sample_depths[sample], sample_depths[sample] };
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &viewport);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    }

    D3D12_RESOURCE_BARRIER source_to_resolve = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    source_to_resolve.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ source_depth,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_RESOLVE_SOURCE };
    D3D12_RESOURCE_BARRIER targets_to_resolve[3] = { 0 };
    for (UINT i = 0; i < 3; i++) {
        targets_to_resolve[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        targets_to_resolve[i].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ depth_targets[i],
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_RESOLVE_DEST };
    }
    D3D12_RESOURCE_BARRIER color_source_to_resolve = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    color_source_to_resolve.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ color,
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_RESOLVE_SOURCE };
    ID3D12Resource *color_targets[2] = { color_minimum, color_maximum };
    D3D12_RESOURCE_BARRIER color_targets_to_resolve[2] = { 0 };
    for (UINT i = 0; i < 2; i++) {
        color_targets_to_resolve[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        color_targets_to_resolve[i].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ color_targets[i],
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RESOLVE_DEST };
    }
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, color_targets_to_resolve);
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &color_source_to_resolve);
    ID3D12GraphicsCommandList_ResourceBarrier(list, 3, targets_to_resolve);
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &source_to_resolve);
    ID3D12GraphicsCommandList1 *list1 = NULL;
    CHECK(ID3D12GraphicsCommandList_QueryInterface(list, &IID_ID3D12GraphicsCommandList1, (void **)&list1));
    D3D12_RECT resolve_region = { 1, 1, 3, 3 };
    const float expected[3] = { 0.5f, 0.125f, 0.875f };
    ID3D12Resource *targets[2] = { minimum, maximum };
    const D3D12_RESOLVE_MODE modes[2] = { D3D12_RESOLVE_MODE_MIN, D3D12_RESOLVE_MODE_MAX };
    ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, average, 1, 1, 2, source_depth, 1,
        &resolve_region, format, D3D12_RESOLVE_MODE_AVERAGE);
    for (UINT i = 0; i < 2; i++) {
        ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, targets[i], 1, 1, 2, source_depth, 1,
            &resolve_region, format, modes[i]);
        ID3D12GraphicsCommandList1_ResolveSubresourceRegion(list1, color_targets[i], 1, 1, 2, color, 1,
            &resolve_region, DXGI_FORMAT_R8G8B8A8_UNORM, modes[i]);
    }
    ID3D12GraphicsCommandList1_Release(list1);
    D3D12_RESOURCE_BARRIER targets_to_copy[3] = { 0 };
    for (UINT i = 0; i < 3; i++) {
        targets_to_copy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        targets_to_copy[i].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ depth_targets[i],
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RESOLVE_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE };
    }
    D3D12_RESOURCE_BARRIER color_targets_to_copy[2] = { 0 };
    for (UINT i = 0; i < 2; i++) {
        color_targets_to_copy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        color_targets_to_copy[i].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ color_targets[i],
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RESOLVE_DEST,
            D3D12_RESOURCE_STATE_COPY_SOURCE };
    }
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, color_targets_to_copy);
    ID3D12GraphicsCommandList_ResourceBarrier(list, 3, targets_to_copy);
    D3D12_TEXTURE_COPY_LOCATION readback_dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    readback_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){
        format, width, height, 1, row_pitch };
    D3D12_TEXTURE_COPY_LOCATION resolved_src = { NULL, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    resolved_src.SubresourceIndex = 1;
    for (UINT i = 0; i < 3; i++) {
        readback_dst.PlacedFootprint.Offset = (uint64_t)i * image_bytes;
        resolved_src.pResource = depth_targets[i];
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &readback_dst, 0, 0, 0, &resolved_src, NULL);
    }
    readback_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){
        DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, row_pitch };
    for (UINT i = 0; i < 2; i++) {
        readback_dst.PlacedFootprint.Offset = (uint64_t)(i + 3) * image_bytes;
        resolved_src.pResource = color_targets[i];
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &readback_dst, 0, 0, 0, &resolved_src, NULL);
    }
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    unsigned char *pixels = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
    int wrong = 0;
    float observed[3] = { 0 };
    for (UINT result = 0; result < 3; result++) for (UINT y = 0; y < height; y++) for (UINT x = 0; x < width; x++) {
        const unsigned char *pixel = pixels + (size_t)result * image_bytes + (size_t)y * row_pitch + x * (unorm16 ? 2 : 4);
        int resolved = x >= 1 && x < 3 && y >= 2 && y < 4;
        float wanted = resolved ? expected[result] : untouched_depth;
        float value;
        if (unorm16) {
            uint16_t encoded;
            memcpy(&encoded, pixel, sizeof encoded);
            value = (float)encoded / 65535.0f;
            wrong += abs((int)encoded - (int)lroundf(wanted * 65535.0f)) > 2;
        } else {
            memcpy(&value, pixel, sizeof value);
            wrong += fabsf(value - wanted) > 0.00001f;
        }
        if (x == 1 && y == 2) observed[result] = value;
    }
    const float expected_colors[2][4] = {
        { 0.1f, 0.3f, 0.2f, 0.5f }, { 0.7f, 0.8f, 0.8f, 0.8f },
    };
    unsigned char observed_colors[2][4] = { 0 };
    for (UINT result = 0; result < 2; result++) for (UINT y = 0; y < height; y++) for (UINT x = 0; x < width; x++) {
        const unsigned char *pixel = pixels + (size_t)(result + 3) * image_bytes + (size_t)y * row_pitch + x * 4;
        int resolved = x >= 1 && x < 3 && y >= 2 && y < 4;
        for (UINT channel = 0; channel < 4; channel++) {
            unsigned char wanted = (unsigned char)lroundf((resolved ? expected_colors[result][channel] : untouched_color[channel]) * 255.0f);
            wrong += abs((int)pixel[channel] - wanted) > 2;
            if (x == 1 && y == 2) observed_colors[result][channel] = pixel[channel];
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    printf("d3d12 depth resolve: 4x %s MSAA array slice 1, partial 2x2 region average/min/max (%.6f, %.6f, %.6f), outside preserved at %.2f, %d wrong pixels: %s\n",
        unorm16 ? "D16" : "D32", observed[0], observed[1], observed[2], untouched_depth, wrong, wrong ? "FAILED" : "ok");
    printf("d3d12 color resolve: partial 4x MSAA RGBA8 array MIN/MAX (%u,%u,%u,%u) / (%u,%u,%u,%u), outside preserved, %s\n",
        observed_colors[0][0], observed_colors[0][1], observed_colors[0][2], observed_colors[0][3],
        observed_colors[1][0], observed_colors[1][1], observed_colors[1][2], observed_colors[1][3],
        wrong ? "FAILED" : "ok");
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12Resource_Release(vertex_buffer);
    ID3D12PipelineState_Release(pipelines[0]);
    ID3D12PipelineState_Release(pipelines[1]);
    ID3D12PipelineState_Release(pipelines[2]);
    ID3D12PipelineState_Release(pipelines[3]);
    ID3D12RootSignature_Release(root);
    ID3D10Blob_Release(serialized);
    ID3D12DescriptorHeap_Release(rtv_heap);
    ID3D12DescriptorHeap_Release(color_target_rtv_heap);
    ID3D12DescriptorHeap_Release(dsv_heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(color_maximum);
    ID3D12Resource_Release(color_minimum);
    ID3D12Resource_Release(maximum);
    ID3D12Resource_Release(minimum);
    ID3D12Resource_Release(average);
    ID3D12Resource_Release(color);
    ID3D12Resource_Release(source_depth);
    return wrong != 0;
}

// Heaps and placed resources, and a texture read in a pixel shader: GetResourceAllocationInfo, three heaps (textures
// only, render targets only, upload buffers), two upload buffers placed at the same offset (they must share memory),
// a 4x4 texture placed and filled from one of them by CopyTextureRegion, an SRV in a shader-visible heap reached by a
// descriptor table, a static point sampler; full-target triangles sample it and check GetDimensions on 2D and 1D-array SRVs.
static int texture_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    static unsigned char vs[65536], ps[65536], gather_ps[65536], dim_ps[65536], dim1da_ps[65536], inc_ps[65536];
    size_t vs_size, ps_size, gather_ps_size, dim_ps_size, dim1da_ps_size, inc_ps_size;
    if (load("tests/shaders12/texq_vs.dxil", vs, sizeof vs, &vs_size) || load("tests/shaders12/texq_ps.dxil", ps, sizeof ps, &ps_size) ||
        load("tests/shaders12/gather_ps.dxil", gather_ps, sizeof gather_ps, &gather_ps_size) ||
        load("tests/shaders12/dim_ps.dxil", dim_ps, sizeof dim_ps, &dim_ps_size) || load("tests/shaders12/tex_ps.dxil", inc_ps, sizeof inc_ps, &inc_ps_size) ||
        load("tests/shaders12/dim1da_ps.dxil", dim1da_ps, sizeof dim1da_ps, &dim1da_ps_size)) return 1;
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
    pd.PS = (D3D12_SHADER_BYTECODE){ gather_ps, gather_ps_size };
    ID3D12PipelineState *gather_pso;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&gather_pso));

    D3D12_DESCRIPTOR_RANGE wrong_range = range;
    wrong_range.BaseShaderRegister = 1;
    D3D12_ROOT_PARAMETER wrong_param = param;
    wrong_param.DescriptorTable.pDescriptorRanges = &wrong_range;
    D3D12_ROOT_SIGNATURE_DESC wrong_rsd = { 1, &wrong_param, 1, &ss, 0 };
    ID3DBlob *wrong_blob = NULL, *wrong_errors = NULL;
    ID3D12RootSignature *wrong_rs = NULL;
    HRESULT wrong_hr = D3D12SerializeRootSignature(&wrong_rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                                    &wrong_blob, &wrong_errors);
    if (SUCCEEDED(wrong_hr))
        wrong_hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(wrong_blob),
            ID3D10Blob_GetBufferSize(wrong_blob), &IID_ID3D12RootSignature, (void **)&wrong_rs);
    ID3D12PipelineState *wrong_pso = NULL;
    if (SUCCEEDED(wrong_hr)) {
        pd.pRootSignature = wrong_rs;
        pd.PS = (D3D12_SHADER_BYTECODE){ gather_ps, gather_ps_size };
        wrong_hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState,
                                                             (void **)&wrong_pso);
    }
    int register_mismatch_rejected = wrong_hr == E_INVALIDARG && wrong_pso == NULL;
    if (wrong_pso) ID3D12PipelineState_Release(wrong_pso);
    if (wrong_blob) ID3D10Blob_Release(wrong_blob);
    if (wrong_errors) ID3D10Blob_Release(wrong_errors);

    D3D12_ROOT_PARAMETER wrong_visibility_param = param;
    wrong_visibility_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_STATIC_SAMPLER_DESC wrong_visibility_sampler = ss;
    wrong_visibility_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC wrong_visibility_rsd = { 1, &wrong_visibility_param, 1,
        &wrong_visibility_sampler, 0 };
    ID3DBlob *wrong_visibility_blob = NULL, *wrong_visibility_errors = NULL;
    ID3D12RootSignature *wrong_visibility_rs = NULL;
    HRESULT wrong_visibility_hr = D3D12SerializeRootSignature(&wrong_visibility_rsd,
        D3D_ROOT_SIGNATURE_VERSION_1, &wrong_visibility_blob, &wrong_visibility_errors);
    if (SUCCEEDED(wrong_visibility_hr))
        wrong_visibility_hr = ID3D12Device_CreateRootSignature(dev, 0,
            ID3D10Blob_GetBufferPointer(wrong_visibility_blob), ID3D10Blob_GetBufferSize(wrong_visibility_blob),
            &IID_ID3D12RootSignature, (void **)&wrong_visibility_rs);

    D3D12_ROOT_PARAMETER deny_param = param;
    deny_param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC deny_sampler = ss;
    deny_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC deny_rsd = { 1, &deny_param, 1, &deny_sampler,
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS };
    ID3DBlob *deny_blob = NULL, *deny_errors = NULL;
    ID3D12RootSignature *deny_rs = NULL;
    HRESULT deny_hr = D3D12SerializeRootSignature(&deny_rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                                   &deny_blob, &deny_errors);
    if (SUCCEEDED(deny_hr))
        deny_hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(deny_blob),
            ID3D10Blob_GetBufferSize(deny_blob), &IID_ID3D12RootSignature, (void **)&deny_rs);
    ID3D12PipelineState *deny_pso = NULL;
    if (SUCCEEDED(deny_hr)) {
        pd.pRootSignature = deny_rs;
        deny_hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState,
                                                            (void **)&deny_pso);
    }
    int denied_stage_rejected = deny_hr == E_INVALIDARG && deny_pso == NULL;
    if (deny_pso) ID3D12PipelineState_Release(deny_pso);
    if (deny_rs) ID3D12RootSignature_Release(deny_rs);
    if (deny_blob) ID3D10Blob_Release(deny_blob);
    if (deny_errors) ID3D10Blob_Release(deny_errors);
    pd.pRootSignature = rs;
    pd.PS = (D3D12_SHADER_BYTECODE){ gather_ps, gather_ps_size };
    if (!register_mismatch_rejected || !denied_stage_rejected || !wrong_visibility_rs) {
        printf("d3d12 graphics root validation: register mismatch hr=%#lx, denied pixel stage hr=%#lx, wrong visibility hr=%#lx: FAILED\n",
               (unsigned long)wrong_hr, (unsigned long)deny_hr, (unsigned long)wrong_visibility_hr);
        bad++;
    } else {
        puts("d3d12 graphics root validation: descriptor registers, stage visibility and denied-stage access checked: ok");
    }
    pd.PS = (D3D12_SHADER_BYTECODE){ dim_ps, dim_ps_size };
    ID3D12PipelineState *dim_pso;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&dim_pso));
    pd.PS = (D3D12_SHADER_BYTECODE){ dim1da_ps, dim1da_ps_size };
    ID3D12PipelineState *dim1da_pso;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&dim1da_pso));

    D3D12_ROOT_PARAMETER inc_params[2] = { 0 };
    inc_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    inc_params[0].Constants.ShaderRegister = 0;
    inc_params[0].Constants.Num32BitValues = 5;
    inc_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    inc_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    inc_params[1].DescriptorTable.NumDescriptorRanges = 1;
    inc_params[1].DescriptorTable.pDescriptorRanges = &range;
    inc_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC inc_rsd = { 2, inc_params, 1, &ss, 0 };
    ID3DBlob *inc_blob, *inc_errs;
    CHECK(D3D12SerializeRootSignature(&inc_rsd, D3D_ROOT_SIGNATURE_VERSION_1, &inc_blob, &inc_errs));
    ID3D12RootSignature *inc_rs;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(inc_blob), ID3D10Blob_GetBufferSize(inc_blob),
                                           &IID_ID3D12RootSignature, (void **)&inc_rs));
    pd.pRootSignature = inc_rs;
    pd.PS = (D3D12_SHADER_BYTECODE){ inc_ps, inc_ps_size };
    ID3D12PipelineState *inc_pso;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&inc_pso));

    D3D12_DESCRIPTOR_RANGE sampler_range = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0,
                                               D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER sampler_params[2] = { 0 };
    sampler_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    sampler_params[0].DescriptorTable.NumDescriptorRanges = 1;
    sampler_params[0].DescriptorTable.pDescriptorRanges = &range;
    sampler_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    sampler_params[1].DescriptorTable.NumDescriptorRanges = 1;
    sampler_params[1].DescriptorTable.pDescriptorRanges = &sampler_range;
    sampler_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC sampler_rsd = { 2, sampler_params, 0, NULL, 0 };
    ID3DBlob *sampler_blob = NULL, *sampler_errors = NULL;
    CHECK(D3D12SerializeRootSignature(&sampler_rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                     &sampler_blob, &sampler_errors));
    ID3D12RootSignature *sampler_rs = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(sampler_blob),
        ID3D10Blob_GetBufferSize(sampler_blob), &IID_ID3D12RootSignature, (void **)&sampler_rs));
    ID3D10Blob_Release(sampler_blob);
    if (sampler_errors) ID3D10Blob_Release(sampler_errors);
    pd.pRootSignature = sampler_rs;
    pd.PS = (D3D12_SHADER_BYTECODE){ ps, ps_size };
    ID3D12PipelineState *sampler_pso = NULL;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&sampler_pso));

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
    uint64_t texture_heap_test_size = (tex_ai.SizeInBytes > rt_ai.SizeInBytes ? tex_ai.SizeInBytes : rt_ai.SizeInBytes) * 2;
    uint64_t rt_heap_test_size = tex_ai.SizeInBytes > rt_ai.SizeInBytes ? tex_ai.SizeInBytes : rt_ai.SizeInBytes;
    D3D12_HEAP_DESC hd = { texture_heap_test_size, { D3D12_HEAP_TYPE_DEFAULT }, 0, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES };
    ID3D12Heap *tex_heap, *rt_heap, *up_heap;
    CHECK(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&tex_heap));
    hd.SizeInBytes = rt_heap_test_size;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES;
    CHECK(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&rt_heap));
    hd.SizeInBytes = 3 * 65536;
    hd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    CHECK(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **)&up_heap));
    D3D12_HEAP_DESC got = ID3D12Heap_GetDesc(up_heap);
    if (got.SizeInBytes != 3 * 65536 || got.Properties.Type != D3D12_HEAP_TYPE_UPLOAD || got.Alignment != 65536) { printf("texture: heap GetDesc %llu %d %llu\n", (unsigned long long)got.SizeInBytes, got.Properties.Type, (unsigned long long)got.Alignment); bad++; }
    D3D12_HEAP_DESC bad_alignment_desc = hd;
    bad_alignment_desc.Alignment = 4096;
    HRESULT bad_heap_alignment = ID3D12Device_CreateHeap(dev, &bad_alignment_desc, &IID_ID3D12Heap, NULL);
    D3D12_HEAP_DESC accepted_alignment_desc = hd;
    accepted_alignment_desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    HRESULT accepted_default_alignment = ID3D12Device_CreateHeap(dev, &accepted_alignment_desc, &IID_ID3D12Heap, NULL);
    accepted_alignment_desc.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
    HRESULT accepted_msaa_alignment = ID3D12Device_CreateHeap(dev, &accepted_alignment_desc, &IID_ID3D12Heap, NULL);
    D3D12_HEAP_DESC display_heap_desc = hd;
    display_heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_DISPLAY;
    HRESULT display_heap_result = ID3D12Device_CreateHeap(dev, &display_heap_desc, &IID_ID3D12Heap, NULL);
    if (bad_heap_alignment != E_INVALIDARG || accepted_default_alignment != S_FALSE || accepted_msaa_alignment != S_FALSE ||
        SUCCEEDED(display_heap_result)) {
        printf("d3d12 heaps: validation returned alignment=%#lx, 64 KiB=%#lx, 4 MiB=%#lx, explicit ALLOW_DISPLAY=%#lx\n",
               (unsigned long)bad_heap_alignment, (unsigned long)accepted_default_alignment,
               (unsigned long)accepted_msaa_alignment, (unsigned long)display_heap_result);
        bad++;
    } else {
        puts("d3d12 heaps: invalid alignment and explicit ALLOW_DISPLAY rejected; default, 64 KiB and 4 MiB accepted: ok");
    }

    // The three DENY_* bits also define the ALLOW_ONLY_* aliases. Use heaps large enough for both
    // texture classes so rejection here checks the descriptor's class, not a placement-size failure.
    D3D12_RESOURCE_DESC small_rtd = texd;
    small_rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource *denied_resource = NULL;
    HRESULT deny_buffer_hr = ID3D12Device_CreatePlacedResource(dev, tex_heap, 0, &bufd,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&denied_resource);
    if (SUCCEEDED(deny_buffer_hr)) { ID3D12Resource_Release(denied_resource); denied_resource = NULL; }
    HRESULT deny_rt_hr = ID3D12Device_CreatePlacedResource(dev, tex_heap, 0, &small_rtd,
        D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&denied_resource);
    if (SUCCEEDED(deny_rt_hr)) { ID3D12Resource_Release(denied_resource); denied_resource = NULL; }
    HRESULT deny_non_rt_hr = ID3D12Device_CreatePlacedResource(dev, rt_heap, 0, &texd,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&denied_resource);
    if (SUCCEEDED(deny_non_rt_hr)) { ID3D12Resource_Release(denied_resource); denied_resource = NULL; }
    if (SUCCEEDED(deny_buffer_hr) || SUCCEEDED(deny_rt_hr) || SUCCEEDED(deny_non_rt_hr)) {
        printf("texture: heap resource-class restrictions were ignored (buffer %#lx, RT %#lx, non-RT %#lx)\n",
               (unsigned long)deny_buffer_hr, (unsigned long)deny_rt_hr, (unsigned long)deny_non_rt_hr);
        bad++;
    } else {
        puts("d3d12 heaps: DENY_BUFFERS, DENY_RT_DS_TEXTURES and DENY_NON_RT_DS_TEXTURES reject incompatible placed resources: ok");
    }

    // placed resources
    ID3D12Resource *tex, *tex_copy, *rt, *up, *alias, *outside = NULL;
    ID3D12Resource *misaligned = NULL;
    HRESULT hr_misaligned = ID3D12Device_CreatePlacedResource(dev, up_heap, 4096, &bufd,
        D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&misaligned);
    if (SUCCEEDED(hr_misaligned)) {
        ID3D12Resource_Release(misaligned);
        printf("texture: a default-alignment buffer was placed at a 4 KiB-only offset\n");
        bad++;
    }
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
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { texels[y][x][0] = 40 + 60 * x; texels[y][x][1] = 128; texels[y][x][2] = (x ^ y) * 80; texels[y][x][3] = 255; }
    unsigned char *w, *r;
    CHECK(ID3D12Resource_Map(alias, 0, NULL, (void **)&w));
    for (int y = 0; y < 4; y++) memcpy(w + 256 * y, texels[y], 16);
    ID3D12Resource_Unmap(alias, 0, NULL);
    CHECK(ID3D12Resource_Map(up, 0, NULL, (void **)&r));
    int aliased = !memcmp(r + 256 * 3, texels[3], 16) && ID3D12Resource_GetGPUVirtualAddress(up) == ID3D12Resource_GetGPUVirtualAddress(alias);
    ID3D12Resource_Unmap(up, 0, NULL);
    if (!aliased) { printf("texture: two buffers placed at the same offset do not share memory\n"); bad++; }

    // views. Slot 0 holds a 1D-array texture for the second dimensions pass; slot 1 is the original 2D test texture.
    D3D12_HEAP_PROPERTIES dhp = { D3D12_HEAP_TYPE_DEFAULT };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dhp, D3D12_HEAP_FLAG_NONE, &texd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&tex_copy));
    D3D12_RESOURCE_DESC array1d_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE1D, 0, 4, 1, 2, 1,
                                         DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource *array1d;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dhp, D3D12_HEAP_FLAG_NONE, &array1d_desc,
                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL, &IID_ID3D12Resource, (void **)&array1d));
    D3D12_DESCRIPTOR_HEAP_DESC dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *srvs, *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &dhd, &IID_ID3D12DescriptorHeap, (void **)&srvs));
    D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *sampler_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &sampler_heap_desc, &IID_ID3D12DescriptorHeap,
                                             (void **)&sampler_heap));
    D3D12_SAMPLER_DESC table_sampler_desc = {
        D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        0, 1, D3D12_COMPARISON_FUNC_NEVER, { 0, 0, 0, 0 }, 0, D3D12_FLOAT32_MAX
    };
    ID3D12Device_CreateSampler(dev, &table_sampler_desc,
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(sampler_heap));
    D3D12_GPU_DESCRIPTOR_HANDLE table_sampler_gpu =
        ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(sampler_heap);
    dhd = (D3D12_DESCRIPTOR_HEAP_DESC){ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &dhd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE srv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srvs);
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srvs);
    D3D12_SHADER_RESOURCE_VIEW_DESC array1d_srv = { 0 };
    array1d_srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    array1d_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
    array1d_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    array1d_srv.Texture1DArray.MipLevels = 1;
    array1d_srv.Texture1DArray.ArraySize = 2;
    ID3D12Device_CreateShaderResourceView(dev, array1d, &array1d_srv, srv);
    srv.ptr += inc;                                                // (the second descriptor: the table's start is not the heap's)
    srv_gpu.ptr += inc;
    ID3D12Device_CreateShaderResourceView(dev, tex_copy, NULL, srv);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    ID3D12Resource *rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *dim_rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *dim1da_rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *gather_rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
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
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_CopyResource(list, tex_copy, tex);
    D3D12_RESOURCE_BARRIER texture_transitions[2] = { 0 };
    texture_transitions[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    texture_transitions[0].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                                               D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON };
    texture_transitions[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    texture_transitions[1].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ tex_copy, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                                               D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, texture_transitions);
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
    ID3D12DescriptorHeap *shader_heaps[] = { srvs, sampler_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 2, shader_heaps);
    ID3D12GraphicsCommandList_SetPipelineState(list, sampler_pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, sampler_rs);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, srv_gpu);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 1, table_sampler_gpu);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, grey, 0, NULL);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    ID3D12GraphicsCommandList_ResolveQueryData(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 0, 1, qrb, 0);
    ID3D12GraphicsCommandList_ResolveQueryData(list, occ, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1, 1, qrb, 8);
    ID3D12GraphicsCommandList_ResolveQueryData(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 2, 1, qrb, 16);
    ID3D12GraphicsCommandList_ResolveQueryData(list, ts, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, qrb, 24);
    ID3D12GraphicsCommandList_ResolveQueryData(list, occ, D3D12_QUERY_TYPE_OCCLUSION, 3, 1, qrb, 40);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_SetPipelineState(list, dim_pso);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, grey, 0, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, srv_gpu);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dim_dst = { dim_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dim_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dim_dst, 0, 0, 0, &src, NULL);
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_SetPipelineState(list, dim1da_pso);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, grey, 0, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srvs));
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dim1da_dst = { dim1da_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dim1da_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dim1da_dst, 0, 0, 0, &src, NULL);
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &srvs);
    ID3D12GraphicsCommandList_SetPipelineState(list, gather_pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, srv_gpu);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, grey, 0, NULL);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION gather_dst = { gather_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    gather_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &gather_dst, 0, 0, 0, &src, NULL);
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
    unsigned char *dim_px;
    CHECK(ID3D12Resource_Map(dim_rb, 0, NULL, (void **)&dim_px));
    int dim_wrong = 0;
    const unsigned char want_dim[4] = { 255, 255, 64, 255 };        // (4x4 at mip 0, one mip level)
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        const unsigned char *a = dim_px + y * 256 + x * 4;
        if (memcmp(a, want_dim, 4)) {
            if (!dim_wrong) printf("dimensions: pixel (%d, %d) is (%u, %u, %u, %u), want (255, 255, 64, 255)\n", x, y, a[0], a[1], a[2], a[3]);
            dim_wrong++;
        }
    }
    if (dim_wrong) bad++;
    ID3D12Resource_Unmap(dim_rb, 0, NULL);
    unsigned char *dim1da_px;
    CHECK(ID3D12Resource_Map(dim1da_rb, 0, NULL, (void **)&dim1da_px));
    int dim1da_wrong = 0;
    const unsigned char want_dim1da[4] = { 255, 255, 255, 255 };    // (width 4, two array elements, one mip level)
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        const unsigned char *a = dim1da_px + y * 256 + x * 4;
        if (memcmp(a, want_dim1da, 4)) {
            if (!dim1da_wrong) printf("dimensions 1D-array: pixel (%d, %d) is (%u, %u, %u, %u), want (255, 255, 255, 255)\n", x, y, a[0], a[1], a[2], a[3]);
            dim1da_wrong++;
        }
    }
    if (dim1da_wrong) bad++;
    ID3D12Resource_Unmap(dim1da_rb, 0, NULL);
    unsigned char *gather_px;
    CHECK(ID3D12Resource_Map(gather_rb, 0, NULL, (void **)&gather_px));
    int gather_wrong = 0;
    const unsigned char gather_expected[4] = { 128, 128, 128, 128 };
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        const unsigned char *a = gather_px + y * 256 + x * 4;
        if (memcmp(a, gather_expected, 4)) {
            if (!gather_wrong) printf("texture gather: pixel (%d, %d) is (%u, %u, %u, %u), want (128, 128, 128, 128)\n",
                                      x, y, a[0], a[1], a[2], a[3]);
            gather_wrong++;
        }
    }
    if (gather_wrong) bad++;
    ID3D12Resource_Unmap(gather_rb, 0, NULL);

    // A compatible PSO does not make a later, incompatible root signature valid. The root slots here have the same
    // registers as the PSO's signature, but only the vertex stage can see them; a pixel draw must leave the target
    // unchanged and the list's preceding clear must be rejected transactionally too.
    int bound_visibility_rejected = 0;
    if (wrong_visibility_rs) {
        CHECK(ID3D12GraphicsCommandList_Reset(list, alloc, NULL));
        barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        CHECK(ID3D12GraphicsCommandList_Close(list));
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        wait_queue(dev, queue);

        CHECK(ID3D12GraphicsCommandList_Reset(list, alloc, gather_pso));
        float rejected_clear[4] = { 1, 0, 0, 1 };
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, rejected_clear, 0, NULL);
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &srvs);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, wrong_visibility_rs);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, srv_gpu);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        CHECK(ID3D12GraphicsCommandList_Close(list));
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        wait_queue(dev, queue);

        CHECK(ID3D12GraphicsCommandList_Reset(list, alloc, NULL));
        barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &gather_dst, 0, 0, 0, &src, NULL);
        CHECK(ID3D12GraphicsCommandList_Close(list));
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        wait_queue(dev, queue);

        CHECK(ID3D12Resource_Map(gather_rb, 0, NULL, (void **)&gather_px));
        int rejected_draw_changed_target = 0;
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++)
            rejected_draw_changed_target += memcmp(gather_px + y * 256 + x * 4, gather_expected, 4) != 0;
        ID3D12Resource_Unmap(gather_rb, 0, NULL);
        bound_visibility_rejected = rejected_draw_changed_target == 0;
        if (!bound_visibility_rejected)
            printf("d3d12 bound root visibility: incompatible pixel draw changed %d target pixels: FAILED\n",
                   rejected_draw_changed_target);
        else
            puts("d3d12 bound root visibility: incompatible pixel draw and its clear were rejected: ok");
    }
    if (wrong_visibility_blob) ID3D10Blob_Release(wrong_visibility_blob);
    if (wrong_visibility_errors) ID3D10Blob_Release(wrong_visibility_errors);
    if (wrong_visibility_rs) ID3D12RootSignature_Release(wrong_visibility_rs);
    if (wrong_rs) ID3D12RootSignature_Release(wrong_rs);
    if (!bound_visibility_rejected) bad++;

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
           "texture upload and CopyResource, SRV descriptor table, static and descriptor-table samplers, %d of 4096 pixels wrong: %s\n",
           wrong, !wrong && sizes && aliased ? "ok" : "FAILED");
    printf("d3d12 dimensions: 2D SRV GetDimensions at mip 0 (4x4, one mip), %d of 4096 pixels wrong: %s\n", dim_wrong, dim_wrong ? "FAILED" : "ok");
    printf("d3d12 dimensions: 1D-array SRV GetDimensions at mip 0 (width 4, two elements, one mip), %d of 4096 pixels wrong: %s\n", dim1da_wrong, dim1da_wrong ? "FAILED" : "ok");
    printf("d3d12 texture gather: GatherGreen with static texel offset, %d of 4096 pixels wrong: %s\n",
           gather_wrong, gather_wrong ? "FAILED" : "ok");

    // Tier 1.1's incrementing constant consumes no record bytes, starts at zero for each ExecuteIndirect call, and
    // resets its target root constant after the call. The shader samples 0, 1, then 2 texels (scaled by 0.5) so the
    // final image from three indirect draws is one full texel; a second two-draw call ends at half a texel, and a
    // direct draw after it sees the specified reset value of zero.
    D3D12_INDIRECT_ARGUMENT_DESC inc_args[2] = { 0 };
    inc_args[0].Type = (D3D12_INDIRECT_ARGUMENT_TYPE)11;       // D3D12_INDIRECT_ARGUMENT_TYPE_INCREMENTING_CONSTANT
    uint32_t incrementing_fields[2] = { 0, 4 };                // root parameter 0, DWORD offset 4
    memcpy((uint8_t *)&inc_args[0] + sizeof inc_args[0].Type, incrementing_fields, sizeof incrementing_fields);
    inc_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC inc_sig_desc = { 16, 2, inc_args, 0 };
    ID3D12CommandSignature *inc_sig;
    CHECK(ID3D12Device_CreateCommandSignature(dev, &inc_sig_desc, inc_rs, &IID_ID3D12CommandSignature, (void **)&inc_sig));

    D3D12_INDIRECT_ARGUMENT_DESC duplicate_inc[3] = { 0 };
    duplicate_inc[0] = inc_args[0];
    duplicate_inc[1] = inc_args[0];
    duplicate_inc[2].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC duplicate_sig_desc = { 16, 3, duplicate_inc, 0 };
    ID3D12CommandSignature *invalid_sig = NULL;
    HRESULT invalid_sig_hr = ID3D12Device_CreateCommandSignature(dev, &duplicate_sig_desc, inc_rs,
                                                                  &IID_ID3D12CommandSignature, (void **)&invalid_sig);
    if (invalid_sig_hr != E_INVALIDARG || invalid_sig) {
        printf("d3d12 incrementing constant: duplicate arguments were not rejected (hr %#lx)\n", (unsigned long)invalid_sig_hr);
        if (invalid_sig) ID3D12CommandSignature_Release(invalid_sig);
        bad++;
    }
    D3D12_INDIRECT_ARGUMENT_DESC bad_offset_args[2] = { 0 };
    bad_offset_args[0] = inc_args[0];
    ((uint32_t *)((uint8_t *)&bad_offset_args[0] + sizeof bad_offset_args[0].Type))[1] = 5;
    bad_offset_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
    D3D12_COMMAND_SIGNATURE_DESC bad_offset_sig_desc = { 16, 2, bad_offset_args, 0 };
    invalid_sig = NULL;
    invalid_sig_hr = ID3D12Device_CreateCommandSignature(dev, &bad_offset_sig_desc, inc_rs,
                                                          &IID_ID3D12CommandSignature, (void **)&invalid_sig);
    if (invalid_sig_hr != E_INVALIDARG || invalid_sig) {
        printf("d3d12 incrementing constant: out-of-range DWORD offset was not rejected (hr %#lx)\n", (unsigned long)invalid_sig_hr);
        if (invalid_sig) ID3D12CommandSignature_Release(invalid_sig);
        bad++;
    }

    uint32_t draw_records[3][4] = { { 3, 1, 0, 0 }, { 3, 1, 0, 0 }, { 3, 1, 0, 0 } };
    ID3D12Resource *inc_indirect = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof draw_records, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *inc_rt = NULL;
    ID3D12Resource *inc_readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *inc_second_readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *inc_reset_readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_DESC inc_rt_desc = rtd;
    D3D12_HEAP_PROPERTIES inc_heap_props = { D3D12_HEAP_TYPE_DEFAULT };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &inc_heap_props, D3D12_HEAP_FLAG_NONE, &inc_rt_desc,
                                                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void **)&inc_rt));
    D3D12_DESCRIPTOR_HEAP_DESC inc_rtv_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *inc_rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &inc_rtv_desc, &IID_ID3D12DescriptorHeap, (void **)&inc_rtvs));
    D3D12_CPU_DESCRIPTOR_HANDLE inc_rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(inc_rtvs);
    ID3D12Device_CreateRenderTargetView(dev, inc_rt, NULL, inc_rtv);
    if (bad || !inc_indirect || !inc_readback || !inc_second_readback || !inc_reset_readback) return 1;
    void *inc_map = NULL;
    CHECK(ID3D12Resource_Map(inc_indirect, 0, NULL, &inc_map));
    memcpy(inc_map, draw_records, sizeof draw_records);
    ID3D12Resource_Unmap(inc_indirect, 0, NULL);

    ID3D12CommandAllocator *inc_alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&inc_alloc));
    ID3D12GraphicsCommandList *inc_list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, inc_alloc, inc_pso,
                                          &IID_ID3D12GraphicsCommandList, (void **)&inc_list));
    float clear[4] = { 0.25f, 0.25f, 0.25f, 1.0f };
    uint32_t root_values[5];
    float half_scale[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
    memcpy(root_values, half_scale, sizeof half_scale);
    root_values[4] = 99;
    D3D12_VIEWPORT inc_vp = { 0, 0, 64, 64, 0, 1 };
    D3D12_RECT inc_sc = { 0, 0, 64, 64 };
    ID3D12GraphicsCommandList_OMSetRenderTargets(inc_list, 1, &inc_rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(inc_list, 1, &srvs);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(inc_list, inc_rs);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(inc_list, 0, 5, root_values, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(inc_list, 1, srv_gpu);
    ID3D12GraphicsCommandList_RSSetViewports(inc_list, 1, &inc_vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(inc_list, 1, &inc_sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(inc_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_ExecuteIndirect(inc_list, inc_sig, 3, inc_indirect, 0, NULL, 0);
    D3D12_RESOURCE_BARRIER inc_barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    inc_barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ inc_rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(inc_list, 1, &inc_barrier);
    D3D12_TEXTURE_COPY_LOCATION inc_dst = { inc_readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    inc_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION inc_src = { inc_rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(inc_list, &inc_dst, 0, 0, 0, &inc_src, NULL);
    inc_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    inc_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(inc_list, 1, &inc_barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(inc_list, inc_rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_ExecuteIndirect(inc_list, inc_sig, 2, inc_indirect, 0, NULL, 0);
    inc_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    inc_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(inc_list, 1, &inc_barrier);
    inc_dst.pResource = inc_second_readback;
    ID3D12GraphicsCommandList_CopyTextureRegion(inc_list, &inc_dst, 0, 0, 0, &inc_src, NULL);
    inc_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    inc_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(inc_list, 1, &inc_barrier);
    ID3D12GraphicsCommandList_ClearRenderTargetView(inc_list, inc_rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_DrawInstanced(inc_list, 3, 1, 0, 0);
    inc_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    inc_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(inc_list, 1, &inc_barrier);
    inc_dst.pResource = inc_reset_readback;
    ID3D12GraphicsCommandList_CopyTextureRegion(inc_list, &inc_dst, 0, 0, 0, &inc_src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(inc_list));
    ID3D12CommandList *inc_lists[] = { (ID3D12CommandList *)inc_list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, inc_lists);
    wait_queue(dev, queue);

    ID3D12Resource *inc_readbacks[3] = { inc_readback, inc_second_readback, inc_reset_readback };
    int inc_wrong[3] = { 0 };
    for (int pass = 0; pass < 3; pass++) {
        unsigned char *image = NULL;
        CHECK(ID3D12Resource_Map(inc_readbacks[pass], 0, NULL, (void **)&image));
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            const unsigned char *actual = image + y * 256 + x * 4;
            const unsigned char *texel = texels[y / 16][x / 16];
            int shifted_x = (int)((((float)x + 0.5f) / 64.0f + 0.01f) * 4.0f);
            if (shifted_x > 3) shifted_x = 3;
            const unsigned char *shifted_texel = texels[y / 16][shifted_x];
            int mismatch = 0;
            for (int channel = 0; channel < 4; channel++) {
                int expected = pass == 0 ? (texel[channel] + shifted_texel[channel] + 1) / 2 :
                               pass == 1 ? (texel[channel] + 1) / 2 : 0;
                if (abs((int)actual[channel] - expected) > (pass == 2 ? 0 : 1)) mismatch = 1;
            }
            if (mismatch) {
                if (!inc_wrong[pass]) printf("d3d12 incrementing constant pass %d: pixel (%d, %d) is (%u, %u, %u, %u), expected %s\n",
                                              pass, x, y, actual[0], actual[1], actual[2], actual[3],
                                              pass == 0 ? "one texture sample" : pass == 1 ? "half a texture sample" : "zero after the reset");
                inc_wrong[pass]++;
            }
        }
        ID3D12Resource_Unmap(inc_readbacks[pass], 0, NULL);
        if (inc_wrong[pass]) bad++;
    }
    printf("d3d12 ExecuteIndirect tier 1.1: three draws produce counter values 0, 1, 2; a second call restarts at 0; targeted DWORD resets to zero; duplicate and out-of-range signatures rejected: %s\n",
           !inc_wrong[0] && !inc_wrong[1] && !inc_wrong[2] && !bad ? "ok" : "FAILED");
    return 0;
}

// Comparison sampling from R32_TYPELESS/D32_FLOAT depth textures, using 2D and 2D-array SRVs, implicit LOD,
// an explicit clamp, and SampleCmpLevelZero. Less-than passes for 0.25 against depth 0.75 and fails for 0.9.
static int comparison_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    static unsigned char vs[65536], ps[65536], gather_ps[65536];
    size_t vs_size, ps_size, gather_ps_size;
    if (load("tests/shaders12/texq_vs.dxil", vs, sizeof vs, &vs_size) ||
        load("tests/shaders12/cmp_ps.dxil", ps, sizeof ps, &ps_size) ||
        load("tests/shaders12/gather_cmp_ps.dxil", gather_ps, sizeof gather_ps, &gather_ps_size)) return 1;

    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC ss = { D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 0, 1,
                                     D3D12_COMPARISON_FUNC_LESS, D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK, 0,
                                     D3D12_FLOAT32_MAX, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL };
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
    pd.PS = (D3D12_SHADER_BYTECODE){ gather_ps, gather_ps_size };
    ID3D12PipelineState *gather_pso;
    CHECK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&gather_pso));

    D3D12_HEAP_PROPERTIES dh = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM,
                               { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    ID3D12Resource *rt;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                               NULL, &IID_ID3D12Resource, (void **)&rt));
    D3D12_RESOURCE_DESC dd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 4, 4, 1, 1, DXGI_FORMAT_R32_TYPELESS,
                               { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL };
    D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_D32_FLOAT };
    cv.DepthStencil.Depth = 0.75f;
    ID3D12Resource *depth;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                               &cv, &IID_ID3D12Resource, (void **)&depth));
    dd.DepthOrArraySize = 2;
    ID3D12Resource *depth_array;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &dh, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                               &cv, &IID_ID3D12Resource, (void **)&depth_array));

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *srvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srvs));
    hd = (D3D12_DESCRIPTOR_HEAP_DESC){ D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap *rtvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtvs));
    hd = (D3D12_DESCRIPTOR_HEAP_DESC){ D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2 };
    ID3D12DescriptorHeap *dsvs;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsvs));
    D3D12_CPU_DESCRIPTOR_HANDLE srv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srvs);
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srvs);
    UINT srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd;
    memset(&sd, 0, sizeof sd);
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(dev, depth, &sd, srv);
    D3D12_CPU_DESCRIPTOR_HANDLE srv_array = srv;
    srv_array.ptr += srv_inc;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = 2;
    ID3D12Device_CreateShaderResourceView(dev, depth_array, &sd, srv_array);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtvs);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsvs);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_array = dsv;
    dsv_array.ptr += ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvd = { DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D };
    ID3D12Device_CreateDepthStencilView(dev, depth, &dsvd, dsv);
    dsvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
    dsvd.Texture2DArray.ArraySize = 1;
    dsvd.Texture2DArray.FirstArraySlice = 1;
    ID3D12Device_CreateDepthStencilView(dev, depth_array, &dsvd, dsv_array);
    ID3D12Resource *rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *gather_rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 256 * 64, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (bad) return 1;

    ID3D12CommandAllocator *alloc;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    ID3D12GraphicsCommandList *list;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso, &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 0, NULL);
    D3D12_RESOURCE_BARRIER barrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ depth, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_ClearDepthStencilView(list, dsv_array, D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 0, NULL);
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ depth_array, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    float clear[4] = { 0, 0, 0, 1 };
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &srvs);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, rs);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 0, srv_gpu);
    D3D12_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    D3D12_RECT sc = { 0, 0, 64, 64 };
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION dst = { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    D3D12_TEXTURE_COPY_LOCATION src = { rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    ID3D12GraphicsCommandList_SetPipelineState(list, gather_pso);
    ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
    barrier.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ rt, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                                               D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION gather_dst = { gather_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    gather_dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ DXGI_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 256 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &gather_dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    unsigned char *px;
    CHECK(ID3D12Resource_Map(rb, 0, NULL, (void **)&px));
    const unsigned char *center = px + 32 * 256 + 32 * 4;
    int ok = center[0] == 255 && center[1] == 0 && center[2] == 255 && center[3] == 255;
    printf("d3d12 comparison sampling: 2D/array SampleCmp, explicit clamp and SampleCmpLevelZero, pixel (%u, %u, %u, %u): %s\n",
           center[0], center[1], center[2], center[3], ok ? "ok" : "FAILED");
    if (!ok) bad++;
    ID3D12Resource_Unmap(rb, 0, NULL);
    unsigned char *gather_pixels = NULL;
    CHECK(ID3D12Resource_Map(gather_rb, 0, NULL, (void **)&gather_pixels));
    const unsigned char *gather_center = gather_pixels + 32 * 256 + 32 * 4;
    int gather_ok = gather_center[0] == 255 && gather_center[1] == 255 &&
                    gather_center[2] == 255 && gather_center[3] == 255;
    printf("d3d12 comparison gather: GatherCmp over four passing depth texels, pixel (%u, %u, %u, %u): %s\n",
           gather_center[0], gather_center[1], gather_center[2], gather_center[3], gather_ok ? "ok" : "FAILED");
    if (!gather_ok) bad++;
    ID3D12Resource_Unmap(gather_rb, 0, NULL);
    return 0;
}

static int typed_buffer_test_impl(ID3D12Device *dev, ID3D12CommandQueue *queue,
                                 const char *shader_path, DXGI_FORMAT format, int signed_values) {
    static unsigned char code[4096];
    size_t code_size = 0;
    if (load(shader_path, code, sizeof code, &code_size)) return 1;

    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
    };
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rs_desc = { 1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = NULL, *errors = NULL;
    CHECK(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ID3D12RootSignature *root = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                           &IID_ID3D12RootSignature, (void **)&root));
    ID3D10Blob_Release(blob);
    if (errors) ID3D10Blob_Release(errors);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = { root, { code, code_size } };
    ID3D12PipelineState *pso = NULL;
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &pso_desc, &IID_ID3D12PipelineState, (void **)&pso));

    ID3D12Resource *input = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 1024, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *output = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 1024,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 1024, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!input || !output || !readback) return 1;
    UINT input_words[256] = { 0 }, output_words[256] = { 0 };
    for (UINT i = 0; i < 256; i++) input_words[i] = i * 10 + 10;
    if (signed_values) input_words[2] = (UINT)-40;
    void *mapped = NULL;
    CHECK(ID3D12Resource_Map(input, 0, NULL, &mapped));
    memcpy(mapped, input_words, sizeof input_words);
    ID3D12Resource_Unmap(input, 0, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap));
    UINT increment = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {
        format, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
    };
    srv.Buffer.FirstElement = 2;
    srv.Buffer.NumElements = 2;
    ID3D12Device_CreateShaderResourceView(dev, input, &srv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { format, D3D12_UAV_DIMENSION_BUFFER };
    uav.Buffer.FirstElement = 1;
    uav.Buffer.NumElements = 2;
    cpu.ptr += increment;
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &uav, cpu);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                                               (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
                                         &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){
        output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE
    };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, sizeof output_words);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    UINT *actual = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&actual));
    UINT expected = signed_values ? (UINT)-20 : 42;
    int failed = actual[2] != expected;
    if (failed)
        printf("typed buffer SRV/UAV with FirstElement and GetDimensions: output[2] = %#x, want %#x\n",
               actual[2], expected);
    else if (signed_values)
        puts("d3d12 typed sint buffer: R32_SINT SRV/UAV, signed divide, FirstElement, GetDimensions and readback: ok");
    else
        puts("d3d12 typed buffer: R32_UINT SRV/UAV views honor FirstElement, NumElements, GetDimensions and readback: ok");

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(output);
    ID3D12Resource_Release(input);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root);
    return failed;
}

static int typed_buffer_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    return typed_buffer_test_impl(dev, queue, "tests/shaders12/typed_buffer_cs.dxil", DXGI_FORMAT_R32_UINT, 0);
}

static int typed_sint_buffer_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    return typed_buffer_test_impl(dev, queue, "tests/shaders12/typed_sint_buffer_cs.dxil", DXGI_FORMAT_R32_SINT, 1);
}

static int signed_conversions_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    unsigned char code[4096];
    size_t code_size = 0;
    if (load("tests/shaders12/signed_conversion_cs.dxil", code, sizeof code, &code_size)) return 1;

    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
    };
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rs_desc = { 1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = NULL, *errors = NULL;
    CHECK(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ID3D12RootSignature *root = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&root));
    ID3D10Blob_Release(blob);
    if (errors) ID3D10Blob_Release(errors);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = { root, { code, code_size } };
    ID3D12PipelineState *pso = NULL;
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &pso_desc, &IID_ID3D12PipelineState, (void **)&pso));

    ID3D12Resource *input = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 16, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *output = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 16,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!input || !output || !readback) return 1;
    const UINT input_words[4] = { 0xfffffff8u, 0, 0, 0 }; // signed -8, read through an R32_UINT view
    void *mapped = NULL;
    CHECK(ID3D12Resource_Map(input, 0, NULL, &mapped));
    memcpy(mapped, input_words, sizeof input_words);
    ID3D12Resource_Unmap(input, 0, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap));
    UINT increment = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {
        DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
    };
    srv.Buffer.NumElements = 2;
    ID3D12Device_CreateShaderResourceView(dev, input, &srv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_BUFFER };
    uav.Buffer.NumElements = 2;
    cpu.ptr += increment;
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &uav, cpu);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){
        output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE
    };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, 2 * sizeof(UINT));
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    UINT *actual = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&actual));
    int failed = actual[0] != 0xc1000000u || actual[1] != 0xfffffff5u;
    if (failed)
        printf("d3d12 signed conversions: sitofp bits %#x (want 0xc1000000), fptosi bits %#x (want 0xfffffff5)\n",
               actual[0], actual[1]);
    else
        puts("d3d12 signed conversions: DXIL sitofp/fptosi with -8, common-MSL dispatch and readback: ok");
    ID3D12Resource_Unmap(readback, 0, NULL);

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(output);
    ID3D12Resource_Release(input);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root);
    return failed;
}

static int typed_float_buffer_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    unsigned char code[4096];
    size_t code_size = 0;
    if (load("tests/shaders12/typed_float_buffer_cs.dxil", code, sizeof code, &code_size)) return 1;

    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
    };
    D3D12_ROOT_PARAMETER param = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC rs_desc = { 1, &param, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = NULL, *errors = NULL;
    CHECK(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ID3D12RootSignature *root = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&root));
    ID3D10Blob_Release(blob);
    if (errors) ID3D10Blob_Release(errors);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = { root, { code, code_size } };
    ID3D12PipelineState *pso = NULL;
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &pso_desc, &IID_ID3D12PipelineState, (void **)&pso));

    ID3D12Resource *input = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 16, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *output = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 16,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, 16, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!input || !output || !readback) return 1;
    const float input_values[4] = { -1.0f, 3.5f, 0.0f, 0.0f };
    void *mapped = NULL;
    CHECK(ID3D12Resource_Map(input, 0, NULL, &mapped));
    memcpy(mapped, input_values, sizeof input_values);
    ID3D12Resource_Unmap(input, 0, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap));
    UINT increment = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {
        DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_BUFFER, D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
    };
    srv.Buffer.FirstElement = 1;
    srv.Buffer.NumElements = 2;
    ID3D12Device_CreateShaderResourceView(dev, input, &srv, cpu);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { DXGI_FORMAT_R32_FLOAT, D3D12_UAV_DIMENSION_BUFFER };
    uav.Buffer.FirstElement = 1;
    uav.Buffer.NumElements = 2;
    cpu.ptr += increment;
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &uav, cpu);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){
        output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE
    };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, 4 * sizeof(float));
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    float *actual = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&actual));
    int failed = fabsf(actual[1] - 5.75f) > 0.00001f;
    if (failed) printf("d3d12 typed float buffer: common DXIL path returned output[1] = %g, want 5.75\n", actual[1]);
    else puts("d3d12 typed float buffer: R32_FLOAT SRV/UAV views, GetDimensions, arithmetic, common-MSL dispatch and readback: ok");
    ID3D12Resource_Unmap(readback, 0, NULL);

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(output);
    ID3D12Resource_Release(input);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root);
    return failed;
}

static int wave_operations_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    enum { thread_count = 33, values_per_lane = 33, result_count = thread_count * values_per_lane };
    unsigned char code[8192];
    size_t code_size = 0;
    if (load("tests/shaders12/wave_ops_partial_cs.dxil", code, sizeof code, &code_size)) return 1;

    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER parameter = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC root_desc = { 1, &parameter, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = NULL, *errors = NULL;
    CHECK(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ID3D12RootSignature *root = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                           &IID_ID3D12RootSignature, (void **)&root));
    ID3D10Blob_Release(blob);
    if (errors) ID3D10Blob_Release(errors);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc = { root, { code, code_size } };
    ID3D12PipelineState *pipeline = NULL;
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &pipeline_desc, &IID_ID3D12PipelineState,
                                                   (void **)&pipeline));

    ID3D12Resource *output = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, result_count * sizeof(UINT),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, result_count * sizeof(UINT), 0,
        D3D12_RESOURCE_STATE_COPY_DEST);
    if (!output || !readback) return 1;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap));
    D3D12_UNORDERED_ACCESS_VIEW_DESC view = { DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER };
    view.Buffer.NumElements = result_count;
    view.Buffer.StructureByteStride = sizeof(UINT);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &view, cpu);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
          &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12DescriptorHeap *heaps[] = { heap };
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    ID3D12GraphicsCommandList_SetPipelineState(list, pipeline);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, result_count * sizeof(UINT));
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    UINT *actual = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&actual));
    UINT lanes = actual[0];
    int wrong = lanes < 4 || lanes > 32 || (lanes & (lanes - 1));
    if (wrong) printf("d3d12 wave ops: unexpected native wave width %u\n", lanes);
    for (UINT tid = 0; !wrong && tid < thread_count; tid++) {
        UINT base = tid * values_per_lane;
        UINT lane = actual[base + 1];
        UINT wave_start = tid - lane;
        UINT active_lanes = thread_count - wave_start < lanes ? thread_count - wave_start : lanes;
        if (actual[base] != lanes || lane != tid % lanes || !active_lanes ||
            (tid == thread_count - 1 && active_lanes == lanes)) {
            if (wrong < 4) printf("wave[%u]: width/index = %u/%u, want %u/%u\n", tid,
                actual[base], lane, lanes, tid % lanes);
            wrong++;
            continue;
        }
        UINT residue = lane & 3u, group_index = lane >> 2;
        UINT ballot_even = 0, match_mask = 0, active_and = UINT32_MAX, active_or = 0, active_xor = 0;
        UINT multi_sum = 0, multi_product = 1, multi_count = 0;
        UINT multi_and = UINT32_MAX, multi_or = 0, multi_xor = 0;
        UINT active_product = 1, prefix_product = 1;
        for (UINT i = 0; i < active_lanes; i++) {
            UINT value_bits = 0xf0u | i;
            active_and &= value_bits;
            active_or |= value_bits;
            active_xor ^= value_bits;
            active_product *= 2u;
            if ((i & 1u) == 0) ballot_even |= 1u << i;
            if ((i & 3u) == residue) match_mask |= 1u << i;
        }
        for (UINT i = 0; i < lane; i++) prefix_product *= 2u;
        for (UINT i = 0; i < group_index; i++) {
            UINT prior_lane = residue + 4u * i;
            UINT value_bits = 0xf0u | prior_lane;
            multi_sum += prior_lane + 1u;
            multi_product *= 2u;
            multi_count += (prior_lane & 1u) == 0;
            multi_and &= value_bits;
            multi_or |= value_bits;
            multi_xor ^= value_bits;
        }
        float multi_float_sum = (float)multi_sum, multi_float_product = (float)multi_product;
        UINT multi_float_sum_bits, multi_float_product_bits;
        memcpy(&multi_float_sum_bits, &multi_float_sum, sizeof multi_float_sum_bits);
        memcpy(&multi_float_product_bits, &multi_float_product, sizeof multi_float_product_bits);
        UINT expected[values_per_lane] = {
            lanes, lane,
            lane * (lane + 1u) / 2u,
            (lane + 1u) / 2u,
            active_lanes * (active_lanes + 1u) / 2u,
            active_product, 1u, active_lanes,
            1u, active_lanes > 17u, 1u, active_lanes == 1u,
            ballot_even, (active_lanes + 1u) / 2u,
            tid < lanes ? 4u : lane + 1u, 1u, lane == 0u,
            active_and, active_or, active_xor,
            match_mask, multi_sum, multi_product, multi_count,
            group_index ? multi_and : UINT32_MAX,
            multi_or, multi_xor, prefix_product,
            active_lanes == 32u ? UINT32_MAX : (1u << active_lanes) - 1u,
            (UINT)-16, (UINT)((int)active_lanes - 17),
            multi_float_sum_bits, multi_float_product_bits
        };
        for (UINT i = 0; i < values_per_lane; i++) if (actual[base + i] != expected[i]) {
            if (wrong < 4) printf("wave[%u].result[%u] = %#x, want %#x\n", tid, i, actual[base + i], expected[i]);
            wrong++;
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    printf("d3d12 wave ops: ballot, active reductions, exclusive prefixes, lane reads, WaveMatch and all SM 6.5 multi-prefix forms, %d wrong values: %s\n",
           wrong, wrong ? "FAILED" : "ok");
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(output);
    ID3D12PipelineState_Release(pipeline);
    ID3D12RootSignature_Release(root);
    return wrong ? 1 : 0;
}

static int texture_load_bounds_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    const UINT result_count = 8;
    unsigned char code[8192];
    size_t code_size = 0;
    if (load("tests/shaders12/texload_bounds_cs.dxil", code, sizeof code, &code_size)) return 1;

    D3D12_DESCRIPTOR_RANGE ranges[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND }
    };
    D3D12_ROOT_PARAMETER parameter = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC root_desc = { 1, &parameter, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = NULL, *errors = NULL;
    CHECK(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ID3D12RootSignature *root = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
        &IID_ID3D12RootSignature, (void **)&root));
    ID3D10Blob_Release(blob);
    if (errors) ID3D10Blob_Release(errors);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc = { root, { code, code_size } };
    ID3D12PipelineState *pipeline = NULL;
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &pipeline_desc, &IID_ID3D12PipelineState,
        (void **)&pipeline));

    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC source_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 2, 2, 1, 1,
        DXGI_FORMAT_R32_UINT, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource *source = NULL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &source_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&source));
    ID3D12Resource *output = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, result_count * sizeof(UINT),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, result_count * sizeof(UINT), 0,
        D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *upload = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 512, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!source || !output || !readback || !upload) return 1;
    void *upload_bytes = NULL;
    D3D12_RANGE no_read = { 0, 0 };
    CHECK(ID3D12Resource_Map(upload, 0, &no_read, &upload_bytes));
    memset(upload_bytes, 0, 512);
    const UINT texels[4] = { 0xc0ffee12u, 0x10203040u, 0x50607080u, 0x90abcdefu };
    memcpy(upload_bytes, texels, 2 * sizeof(UINT));
    memcpy((unsigned char *)upload_bytes + 256, texels + 2, 2 * sizeof(UINT));
    D3D12_RANGE written = { 0, 512 };
    ID3D12Resource_Unmap(upload, 0, &written);

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap));
    UINT increment = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    ID3D12Device_CreateShaderResourceView(dev, source, NULL, cpu);
    cpu.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC output_view = { DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER };
    output_view.Buffer.NumElements = result_count;
    output_view.Buffer.StructureByteStride = sizeof(UINT);
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &output_view, cpu);
    cpu.ptr += increment;

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&list));
    D3D12_TEXTURE_COPY_LOCATION source_location = { source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION upload_location = { upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    upload_location.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
    upload_location.PlacedFootprint.Footprint.Width = 2;
    upload_location.PlacedFootprint.Footprint.Height = 2;
    upload_location.PlacedFootprint.Footprint.Depth = 1;
    upload_location.PlacedFootprint.Footprint.RowPitch = 256;
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &source_location, 0, 0, 0, &upload_location, NULL);
    D3D12_RESOURCE_BARRIER source_to_srv = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    source_to_srv.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &source_to_srv);
    ID3D12DescriptorHeap *heaps[] = { heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_SetPipelineState(list, pipeline);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    D3D12_RESOURCE_BARRIER output_to_copy = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    output_to_copy.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &output_to_copy);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, result_count * sizeof(UINT));
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    static const UINT expected[8] = { 0x90abcdefu, 0, 0, 0, 0, 0, 0, 0 };
    D3D12_RANGE read_range = { 0, sizeof expected };
    UINT *actual = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, &read_range, (void **)&actual));
    int wrong = 0;
    for (UINT i = 0; i < result_count; i++) if (actual[i] != expected[i]) {
        printf("d3d12 TextureLoad result[%u] = %#x, want %#x\n", i, actual[i], expected[i]);
        wrong++;
    }
    D3D12_RANGE no_write = { 0, 0 };
    ID3D12Resource_Unmap(readback, 0, &no_write);
    printf("d3d12 TextureLoad: 2D coordinates, mip, signed bounds and static offsets return zero out of range, %d wrong values: %s\n",
        wrong, wrong ? "FAILED" : "ok");

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(output);
    ID3D12Resource_Release(upload);
    ID3D12Resource_Release(source);
    ID3D12PipelineState_Release(pipeline);
    ID3D12RootSignature_Release(root);
    return wrong;
}

static int atomic_resource_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    const UINT result_count = 41;
    const D3D12_FORMAT_SUPPORT2 compare = D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT uint_support = { DXGI_FORMAT_R32_UINT, 0, 0 };
    D3D12_FEATURE_DATA_FORMAT_SUPPORT sint_support = { DXGI_FORMAT_R32_SINT, 0, 0 };
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &uint_support, sizeof uint_support));
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &sint_support, sizeof sint_support));
    if (!(uint_support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ||
        !(sint_support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) ||
        !(uint_support.Support2 & compare) ||
        !(uint_support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX) ||
        !(sint_support.Support2 & compare) ||
        !(sint_support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX)) {
        puts("d3d12 atomics: skipped because this device does not report typed R32 integer UAV atomics");
        return 0;
    }

    unsigned char code[8192];
    size_t code_size = 0;
    if (load("tests/shaders12/atomic_resources_cs.dxil", code, sizeof code, &code_size)) return 1;
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 12, 0, 0, 0 };
    D3D12_ROOT_PARAMETER parameter = { D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE };
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC root_desc = { 1, &parameter, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob *blob = NULL, *errors = NULL;
    CHECK(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    ID3D12RootSignature *root = NULL;
    CHECK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                           &IID_ID3D12RootSignature, (void **)&root));
    ID3D10Blob_Release(blob);
    if (errors) ID3D10Blob_Release(errors);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc = { root, { code, code_size } };
    ID3D12PipelineState *pipeline = NULL;
    CHECK(ID3D12Device_CreateComputePipelineState(dev, &pipeline_desc, &IID_ID3D12PipelineState, (void **)&pipeline));

    ID3D12Resource *typed_uint = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *typed_sint = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *raw_uint = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *structured_uint = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, 256,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *texture_uint = NULL, *texture_sint = NULL;
    ID3D12Resource *texture_1d = NULL, *texture_1d_array = NULL;
    ID3D12Resource *texture_2d_array = NULL, *texture_3d = NULL;
    ID3D12Resource *results = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, result_count * sizeof(UINT),
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, result_count * sizeof(UINT), 0,
        D3D12_RESOURCE_STATE_COPY_DEST);
    if (!typed_uint || !typed_sint || !raw_uint || !structured_uint || !results || !readback) return 1;
    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC texture_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1, 1, 1, 1,
        DXGI_FORMAT_R32_UINT, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&texture_uint));
    texture_desc.Format = DXGI_FORMAT_R32_SINT;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&texture_sint));

    D3D12_RESOURCE_DESC extra_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE1D, 0, 4, 1, 1, 1,
        DXGI_FORMAT_R32_UINT, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &extra_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&texture_1d));
    extra_desc.DepthOrArraySize = 2;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &extra_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&texture_1d_array));
    extra_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    extra_desc.Width = 2;
    extra_desc.Height = 2;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &extra_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&texture_2d_array));
    extra_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    extra_desc.DepthOrArraySize = 4;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &extra_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&texture_3d));

    D3D12_DESCRIPTOR_HEAP_DESC gpu_heap_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 12, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &gpu_heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap));
    UINT increment = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    D3D12_UNORDERED_ACCESS_VIEW_DESC view = { DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_BUFFER };
    view.Buffer.NumElements = 1;
    ID3D12Device_CreateUnorderedAccessView(dev, typed_uint, NULL, &view, cpu);
    cpu.ptr += increment;
    view.Format = DXGI_FORMAT_R32_SINT;
    ID3D12Device_CreateUnorderedAccessView(dev, typed_sint, NULL, &view, cpu);
    cpu.ptr += increment;
    view = (D3D12_UNORDERED_ACCESS_VIEW_DESC){ DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE2D };
    ID3D12Device_CreateUnorderedAccessView(dev, texture_uint, NULL, &view, cpu);
    cpu.ptr += increment;
    view.Format = DXGI_FORMAT_R32_SINT;
    ID3D12Device_CreateUnorderedAccessView(dev, texture_sint, NULL, &view, cpu);
    cpu.ptr += increment;
    view = (D3D12_UNORDERED_ACCESS_VIEW_DESC){ DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER };
    view.Buffer.NumElements = 64;
    view.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(dev, raw_uint, NULL, &view, cpu);
    cpu.ptr += increment;
    view = (D3D12_UNORDERED_ACCESS_VIEW_DESC){ DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER };
    view.Buffer.NumElements = 1;
    view.Buffer.StructureByteStride = sizeof(UINT);
    ID3D12Device_CreateUnorderedAccessView(dev, structured_uint, NULL, &view, cpu);
    cpu.ptr += increment;
    view = (D3D12_UNORDERED_ACCESS_VIEW_DESC){ DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER };
    view.Buffer.NumElements = result_count;
    view.Buffer.StructureByteStride = sizeof(UINT);
    ID3D12Device_CreateUnorderedAccessView(dev, results, NULL, &view, cpu);
    cpu.ptr += increment;
    view = (D3D12_UNORDERED_ACCESS_VIEW_DESC){ DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE1D };
    ID3D12Device_CreateUnorderedAccessView(dev, texture_1d, NULL, &view, cpu);
    cpu.ptr += increment;
    view = (D3D12_UNORDERED_ACCESS_VIEW_DESC){ DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE1DARRAY };
    view.Texture1DArray.ArraySize = 2;
    ID3D12Device_CreateUnorderedAccessView(dev, texture_1d_array, NULL, &view, cpu);
    cpu.ptr += increment;
    view = (D3D12_UNORDERED_ACCESS_VIEW_DESC){ DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE2DARRAY };
    view.Texture2DArray.ArraySize = 2;
    ID3D12Device_CreateUnorderedAccessView(dev, texture_2d_array, NULL, &view, cpu);
    cpu.ptr += increment;
    view = (D3D12_UNORDERED_ACCESS_VIEW_DESC){ DXGI_FORMAT_R32_UINT, D3D12_UAV_DIMENSION_TEXTURE3D };
    view.Texture3D.FirstWSlice = 1;
    view.Texture3D.WSize = 2;
    ID3D12Device_CreateUnorderedAccessView(dev, texture_3d, NULL, &view, cpu);
    cpu.ptr += increment;
    ID3D12Device_CreateUnorderedAccessView(dev, texture_3d, NULL, NULL, cpu);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
          &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12DescriptorHeap *heaps[] = { heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    ID3D12GraphicsCommandList_SetPipelineState(list, pipeline);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    D3D12_RESOURCE_BARRIER result_to_copy = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    result_to_copy.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ results, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &result_to_copy);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, results, 0, result_count * sizeof(UINT));
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    static const UINT expected[] = {
        0, 100, 20, 21,
        0, (UINT)-50, (UINT)-10, (UINT)-1,
        0, 100, 20, 21,
        0, (UINT)-50, (UINT)-10, (UINT)-1,
        0, 100, 20, 21,
        0, 5, 7,
        0, 5, 7,
        0, 5, 7,
        0, 5, 7,
        0, 5, 7,
        2, 7, 0, 0, 11, 0
    };
    UINT *actual = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&actual));
    int wrong = 0;
    for (UINT i = 0; i < result_count; i++) if (actual[i] != expected[i]) {
        if (wrong < 4) printf("atomic result[%u] = %#x, want %#x\n", i, actual[i], expected[i]);
        wrong++;
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    printf("d3d12 atomics: raw/structured and typed R32_UINT/SINT buffers, 1D/1D-array/2D/2D-array/3D textures, min/max, exchange and compare-exchange, %d wrong values: %s\n",
           wrong, wrong ? "FAILED" : "ok");

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(results);
    ID3D12Resource_Release(structured_uint);
    ID3D12Resource_Release(raw_uint);
    ID3D12Resource_Release(texture_3d);
    ID3D12Resource_Release(texture_2d_array);
    ID3D12Resource_Release(texture_1d_array);
    ID3D12Resource_Release(texture_1d);
    ID3D12Resource_Release(texture_sint);
    ID3D12Resource_Release(texture_uint);
    ID3D12Resource_Release(typed_sint);
    ID3D12Resource_Release(typed_uint);
    ID3D12PipelineState_Release(pipeline);
    ID3D12RootSignature_Release(root);
    return wrong ? 1 : 0;
}

static int clear_uav_texture_test(ID3D12Device *dev, ID3D12CommandQueue *queue, DXGI_FORMAT format,
                                  int floating_point) {
    const UINT width = 8, height = 4, row_pitch = 256;
    UINT element_size = floating_point || format == DXGI_FORMAT_R32_UINT || format == DXGI_FORMAT_R32_SINT ? 4 :
        format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_R16_SINT ? 2 :
        format == DXGI_FORMAT_R8_UINT || format == DXGI_FORMAT_R8_SINT ? 1 : 0;
    UINT integer_clear_value = format == DXGI_FORMAT_R8_SINT ? 0x876543feu :
        format == DXGI_FORMAT_R16_SINT ? 0x8765c321u :
        format == DXGI_FORMAT_R8_UINT || format == DXGI_FORMAT_R16_UINT ? 0x9abcdef0u : 0x12345678u;
    if (!element_size) {
        printf("d3d12 texture UAV clear: unexpected test format %#x\n", format);
        return 1;
    }
    if (!floating_point && element_size < 4) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { format, 0, 0 };
        CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support));
        if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
            !(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) {
            printf("d3d12 texture UAV clear (%#x): skipped, typed texture UAV is unsupported\n", format);
            return 0;
        }
    }
    D3D12_HEAP_PROPERTIES default_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC texture_desc = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, 1, 1,
        format, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    ID3D12Resource *texture = NULL;
    CHECK(ID3D12Device_CreateCommittedResource(dev, &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL, &IID_ID3D12Resource, (void **)&texture));
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, row_pitch * height, 0,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) return 1;

    D3D12_DESCRIPTOR_HEAP_DESC cpu_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0
    };
    D3D12_DESCRIPTOR_HEAP_DESC gpu_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *cpu_heap = NULL, *gpu_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &cpu_desc, &IID_ID3D12DescriptorHeap, (void **)&cpu_heap));
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &gpu_desc, &IID_ID3D12DescriptorHeap, (void **)&gpu_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE gpu_cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(gpu_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(gpu_heap);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { format, D3D12_UAV_DIMENSION_TEXTURE2D };
    ID3D12Device_CreateUnorderedAccessView(dev, texture, NULL, &uav, cpu);
    ID3D12Device_CopyDescriptorsSimple(dev, 1, gpu_cpu, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
          &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12DescriptorHeap *heaps[] = { gpu_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    if (floating_point) {
        const float initial[4] = { 0, 0, 0, 1 };
        const float region_value[4] = { 1, 0.5f, 0.25f, 1 };
        const D3D12_RECT rect = { 2, 1, 6, 3 };
        ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(list, gpu, cpu, texture, initial, 0, NULL);
        D3D12_RESOURCE_BARRIER uav_barrier = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
        uav_barrier.UAV.pResource = texture;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &uav_barrier);
        ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(list, gpu, cpu, texture, region_value, 1, &rect);
    } else {
        const UINT initial[4] = { 1, 2, 3, 4 };
        const UINT region_value[4] = { integer_clear_value, 5, 6, 7 };
        const D3D12_RECT rect = { 2, 1, 6, 3 };
        ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, gpu, cpu, texture, initial, 0, NULL);
        D3D12_RESOURCE_BARRIER uav_barrier = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
        uav_barrier.UAV.pResource = texture;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &uav_barrier);
        ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, gpu, cpu, texture, region_value, 1, &rect);
    }
    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    D3D12_TEXTURE_COPY_LOCATION dst = { readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){ format, width, height, 1, row_pitch };
    D3D12_TEXTURE_COPY_LOCATION src = { texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst, 0, 0, 0, &src, NULL);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    unsigned char *pixels = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&pixels));
    int wrong = 0;
    for (UINT y = 0; y < height; y++) for (UINT x = 0; x < width; x++) {
        int in_rect = x >= 2 && x < 6 && y >= 1 && y < 3;
        unsigned char *pixel = pixels + (size_t)y * row_pitch + element_size * x;
        if (floating_point) {
            const unsigned char *expected = in_rect ? (const unsigned char[]){ 255, 128, 64, 255 }
                                                    : (const unsigned char[]){ 0, 0, 0, 255 };
            for (UINT c = 0; c < 4; c++) wrong += abs((int)pixel[c] - expected[c]) > (c == 1 || c == 2 ? 1 : 0);
        } else {
            UINT actual = 0;
            memcpy(&actual, pixel, element_size);
            UINT mask = element_size == sizeof(UINT) ? UINT32_MAX : (1u << (8u * element_size)) - 1u;
            UINT expected = in_rect ? integer_clear_value : 1u;
            wrong += actual != (expected & mask);
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    const char *clear_kind = floating_point ? "float" :
        format == DXGI_FORMAT_R8_SINT || format == DXGI_FORMAT_R16_SINT || format == DXGI_FORMAT_R32_SINT ?
        "sint" : "uint";
    printf("d3d12 %s UAV clear: full clear then a 4x2 rectangle through CPU and shader-visible descriptors, %d wrong pixels: %s\n",
           clear_kind, wrong, wrong ? "FAILED" : "ok");

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(gpu_heap);
    ID3D12DescriptorHeap_Release(cpu_heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(texture);
    return wrong ? 1 : 0;
}

static uint16_t d3d12_test_float_to_half(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    uint32_t sign = (bits >> 16) & 0x8000u;
    int exponent = (int)((bits >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    uint32_t rounded;
    if (exponent <= 0) return (uint16_t)sign;
    if (exponent >= 31) return (uint16_t)(sign | 0x7c00u);
    rounded = mantissa + 0xfffu + ((mantissa >> 13) & 1u);
    if (rounded & 0x800000u) {
        rounded = 0;
        if (++exponent >= 31) return (uint16_t)(sign | 0x7c00u);
    }
    return (uint16_t)(sign | ((uint32_t)exponent << 10) | (rounded >> 13));
}

static int clear_uav_typed_float_buffer_test(ID3D12Device *dev, ID3D12CommandQueue *queue,
                                            DXGI_FORMAT format) {
    const UINT count = 8, clear_first = 2, clear_count = 3;
    UINT channel_count = 0, channel_size = 0;
    switch (format) {
    case DXGI_FORMAT_R16_FLOAT: channel_count = 1; channel_size = 2; break;
    case DXGI_FORMAT_R16G16_FLOAT: channel_count = 2; channel_size = 2; break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: channel_count = 4; channel_size = 2; break;
    case DXGI_FORMAT_R32_FLOAT: channel_count = 1; channel_size = 4; break;
    case DXGI_FORMAT_R32G32_FLOAT: channel_count = 2; channel_size = 4; break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: channel_count = 4; channel_size = 4; break;
    default: break;
    }
    if (!channel_count) {
        printf("d3d12 typed-float-buffer UAV clear: unexpected test format %#x\n", format);
        return 1;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { format, 0, 0 };
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support));
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) {
        printf("d3d12 typed-float-buffer UAV clear (%#x): skipped, typed UAV is unsupported\n", format);
        return 0;
    }

    const UINT element_size = channel_count * channel_size;
    const UINT64 byte_count = (UINT64)count * element_size;
    ID3D12Resource *initial = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, byte_count, 0,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *output = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, byte_count,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, byte_count, 0,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    if (!initial || !output || !readback) return 1;

    uint8_t initial_values[8 * 16] = { 0 };
    for (UINT i = 0; i < count; i++) for (UINT channel = 0; channel < channel_count; channel++) {
        float initial_value = (float)((int)(i * 4u + channel) - 15);
        size_t offset = (size_t)i * element_size + (size_t)channel * channel_size;
        if (channel_size == 2) {
            uint16_t half = d3d12_test_float_to_half(initial_value);
            memcpy(initial_values + offset, &half, sizeof half);
        } else memcpy(initial_values + offset, &initial_value, sizeof initial_value);
    }
    void *init_data = NULL;
    CHECK(ID3D12Resource_Map(initial, 0, NULL, &init_data));
    memcpy(init_data, initial_values, byte_count);
    ID3D12Resource_Unmap(initial, 0, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC cpu_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0
    };
    D3D12_DESCRIPTOR_HEAP_DESC gpu_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *cpu_heap = NULL, *gpu_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &cpu_desc, &IID_ID3D12DescriptorHeap, (void **)&cpu_heap));
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &gpu_desc, &IID_ID3D12DescriptorHeap, (void **)&gpu_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE gpu_cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(gpu_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(gpu_heap);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { format, D3D12_UAV_DIMENSION_BUFFER };
    uav.Buffer.FirstElement = clear_first;
    uav.Buffer.NumElements = clear_count;
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &uav, cpu);
    ID3D12Device_CopyDescriptorsSimple(dev, 1, gpu_cpu, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
          &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_CopyBufferRegion(list, output, 0, initial, 0, byte_count);
    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    ID3D12DescriptorHeap *heaps[] = { gpu_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    const float clear_values[4] = { 1.25f, -2.5f, 3.75f, 0.5f };
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(list, gpu, cpu, output, clear_values, 0, NULL);
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, byte_count);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    uint8_t *actual = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&actual));
    int wrong = 0;
    for (UINT i = 0; i < count; i++) for (UINT channel = 0; channel < channel_count; channel++) {
        float expected = i >= clear_first && i < clear_first + clear_count ? clear_values[channel] :
            (float)((int)(i * 4u + channel) - 15);
        size_t offset = (size_t)i * element_size + (size_t)channel * channel_size;
        int mismatch;
        if (channel_size == 2) {
            uint16_t actual_value = 0, expected_value = d3d12_test_float_to_half(expected);
            memcpy(&actual_value, actual + offset, sizeof actual_value);
            mismatch = actual_value != expected_value;
            if (mismatch && wrong < 4)
                printf("  element %u channel %u = %#x, want %#x\n", i, channel,
                       actual_value, expected_value);
        } else {
            float actual_value = 0.0f;
            memcpy(&actual_value, actual + offset, sizeof actual_value);
            mismatch = actual_value != expected;
            if (mismatch && wrong < 4)
                printf("  element %u channel %u = %.9g, want %.9g\n", i, channel, actual_value, expected);
        }
        if (mismatch) {
            wrong++;
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    printf("d3d12 typed-float-buffer UAV clear (%#x, %u channels, %u-bit components): FirstElement %u, NumElements %u, %d wrong values: %s\n",
           format, channel_count, channel_size * 8, clear_first, clear_count, wrong, wrong ? "FAILED" : "ok");

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(gpu_heap);
    ID3D12DescriptorHeap_Release(cpu_heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(output);
    ID3D12Resource_Release(initial);
    return wrong ? 1 : 0;
}

static int clear_uav_typed_buffer_test(ID3D12Device *dev, ID3D12CommandQueue *queue,
                                      DXGI_FORMAT format, UINT clear_value) {
    const UINT count = 16, clear_first = 3, clear_count = 5;
    int packed_r10 = format == DXGI_FORMAT_R10G10B10A2_UINT;
    UINT channel_count = 0, channel_size = 0;
    switch (format) {
    case DXGI_FORMAT_R10G10B10A2_UINT:
        channel_count = 4; break;
    case DXGI_FORMAT_R8_UINT: case DXGI_FORMAT_R8_SINT:
        channel_count = 1; channel_size = 1; break;
    case DXGI_FORMAT_R8G8_UINT: case DXGI_FORMAT_R8G8_SINT:
        channel_count = 2; channel_size = 1; break;
    case DXGI_FORMAT_R8G8B8A8_UINT: case DXGI_FORMAT_R8G8B8A8_SINT:
        channel_count = 4; channel_size = 1; break;
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SINT:
        channel_count = 1; channel_size = 2; break;
    case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SINT:
        channel_count = 2; channel_size = 2; break;
    case DXGI_FORMAT_R16G16B16A16_UINT: case DXGI_FORMAT_R16G16B16A16_SINT:
        channel_count = 4; channel_size = 2; break;
    case DXGI_FORMAT_R32_UINT: case DXGI_FORMAT_R32_SINT:
        channel_count = 1; channel_size = 4; break;
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
        channel_count = 2; channel_size = 4; break;
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
        channel_count = 4; channel_size = 4; break;
    default: break;
    }
    UINT element_size = packed_r10 ? sizeof(UINT) : channel_count * channel_size;
    if (!element_size) {
        printf("d3d12 typed-buffer UAV clear: unexpected test format %#x\n", format);
        return 1;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { format, 0, 0 };
    CHECK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support));
    if (!(support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)) {
        printf("d3d12 typed-buffer UAV clear (%#x): skipped, typed UAV is unsupported\n", format);
        return 0;
    }
    const UINT64 byte_count = count * element_size;
    ID3D12Resource *initial = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, byte_count, 0,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *output = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, byte_count,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, byte_count, 0,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    if (!initial || !output || !readback) return 1;
    uint8_t init_values[16 * 16] = { 0 };
    for (UINT i = 0; i < count; i++) {
        if (packed_r10) {
            UINT seed[4];
            for (UINT channel = 0; channel < 4; channel++) seed[channel] = 0x1000u + i * 16u + channel;
            UINT packed = (seed[0] & 0x3ffu) | ((seed[1] & 0x3ffu) << 10) |
                          ((seed[2] & 0x3ffu) << 20) | ((seed[3] & 0x3u) << 30);
            memcpy(init_values + (size_t)i * element_size, &packed, sizeof packed);
        } else {
            for (UINT channel = 0; channel < channel_count; channel++) {
                UINT seed = 0x1000u + i * 16u + channel;
                memcpy(init_values + (size_t)i * element_size + (size_t)channel * channel_size,
                       &seed, channel_size);
            }
        }
    }
    void *init_data = NULL;
    CHECK(ID3D12Resource_Map(initial, 0, NULL, &init_data));
    memcpy(init_data, init_values, byte_count);
    ID3D12Resource_Unmap(initial, 0, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC cpu_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0
    };
    D3D12_DESCRIPTOR_HEAP_DESC gpu_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *cpu_heap = NULL, *gpu_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &cpu_desc, &IID_ID3D12DescriptorHeap, (void **)&cpu_heap));
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &gpu_desc, &IID_ID3D12DescriptorHeap, (void **)&gpu_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE gpu_cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(gpu_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(gpu_heap);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { format, D3D12_UAV_DIMENSION_BUFFER };
    uav.Buffer.FirstElement = clear_first;
    uav.Buffer.NumElements = clear_count;
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &uav, cpu);
    ID3D12Device_CopyDescriptorsSimple(dev, 1, gpu_cpu, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
          &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_CopyBufferRegion(list, output, 0, initial, 0, byte_count);
    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    ID3D12DescriptorHeap *heaps[] = { gpu_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    const UINT value[4] = { clear_value, 0x12345678u, 0x89abcdefu, 0x76543210u };
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, gpu, cpu, output, value, 0, NULL);
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, byte_count);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    uint8_t *actual = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&actual));
    int wrong = 0;
    for (UINT i = 0; i < count; i++) {
        if (packed_r10) {
            UINT actual_value = 0, expected_values[4];
            memcpy(&actual_value, actual + (size_t)i * element_size, sizeof actual_value);
            if (i >= clear_first && i < clear_first + clear_count) memcpy(expected_values, value, sizeof expected_values);
            else for (UINT channel = 0; channel < 4; channel++) expected_values[channel] = 0x1000u + i * 16u + channel;
            UINT expected = (expected_values[0] & 0x3ffu) | ((expected_values[1] & 0x3ffu) << 10) |
                            ((expected_values[2] & 0x3ffu) << 20) | ((expected_values[3] & 0x3u) << 30);
            if (actual_value != expected) {
                if (wrong < 4) printf("  element %u packed = %#x, want %#x\n", i, actual_value, expected);
                wrong++;
            }
        } else {
            UINT mask = channel_size == sizeof(UINT) ? UINT32_MAX : (1u << (8u * channel_size)) - 1u;
            for (UINT channel = 0; channel < channel_count; channel++) {
                UINT actual_value = 0;
                memcpy(&actual_value, actual + (size_t)i * element_size + (size_t)channel * channel_size,
                       channel_size);
                UINT expected = i >= clear_first && i < clear_first + clear_count ? value[channel] :
                    0x1000u + i * 16u + channel;
                if (actual_value != (expected & mask)) {
                    if (wrong < 4) printf("  element %u channel %u = %#x, want %#x\n",
                                          i, channel, actual_value, expected & mask);
                    wrong++;
                }
            }
        }
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    if (packed_r10)
        printf("d3d12 typed-buffer UAV clear (%#x, packed 10:10:10:2-bit elements): FirstElement %u, NumElements %u, %d wrong values: %s\n",
               format, clear_first, clear_count, wrong, wrong ? "FAILED" : "ok");
    else
        printf("d3d12 typed-buffer UAV clear (%#x, %u-channel %u-bit elements): FirstElement %u, NumElements %u, %d wrong values: %s\n",
               format, channel_count, channel_size * 8, clear_first, clear_count, wrong, wrong ? "FAILED" : "ok");

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(gpu_heap);
    ID3D12DescriptorHeap_Release(cpu_heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(output);
    ID3D12Resource_Release(initial);
    return wrong ? 1 : 0;
}

static int clear_uav_raw_buffer_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    const UINT count = 16, clear_first = 3, clear_count = 5;
    const UINT64 byte_count = count * sizeof(UINT);
    ID3D12Resource *initial = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, byte_count, 0,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *output = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, byte_count,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, byte_count, 0,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    if (!initial || !output || !readback) return 1;
    UINT *init_data = NULL;
    CHECK(ID3D12Resource_Map(initial, 0, NULL, (void **)&init_data));
    for (UINT i = 0; i < count; i++) init_data[i] = 0x2000u + i;
    ID3D12Resource_Unmap(initial, 0, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC cpu_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0
    };
    D3D12_DESCRIPTOR_HEAP_DESC gpu_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *cpu_heap = NULL, *gpu_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &cpu_desc, &IID_ID3D12DescriptorHeap, (void **)&cpu_heap));
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &gpu_desc, &IID_ID3D12DescriptorHeap, (void **)&gpu_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE gpu_cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(gpu_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(gpu_heap);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { DXGI_FORMAT_R32_TYPELESS, D3D12_UAV_DIMENSION_BUFFER };
    uav.Buffer.FirstElement = clear_first;
    uav.Buffer.NumElements = clear_count;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &uav, cpu);
    ID3D12Device_CopyDescriptorsSimple(dev, 1, gpu_cpu, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
          &IID_ID3D12CommandAllocator, (void **)&allocator));
    ID3D12GraphicsCommandList *list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&list));
    ID3D12GraphicsCommandList_CopyBufferRegion(list, output, 0, initial, 0, byte_count);
    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    ID3D12DescriptorHeap *heaps[] = { gpu_heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, heaps);
    const UINT value[4] = { 0x89abcdefu, 0x12345678u, 0x55555555u, 0xaaaaaaaa };
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(list, gpu, cpu, output, value, 0, NULL);
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &transition);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, byte_count);
    CHECK(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    UINT *actual = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&actual));
    int wrong = 0;
    for (UINT i = 0; i < count; i++) {
        UINT expected = i >= clear_first && i < clear_first + clear_count ? value[0] : 0x2000u + i;
        wrong += actual[i] != expected;
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    printf("d3d12 raw-buffer UAV clear: FirstElement %u, NumElements %u, first value repeated across the view, %d wrong values: %s\n",
           clear_first, clear_count, wrong, wrong ? "FAILED" : "ok");

    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(gpu_heap);
    ID3D12DescriptorHeap_Release(cpu_heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(output);
    ID3D12Resource_Release(initial);
    return wrong ? 1 : 0;
}

static int clear_uav_structured_buffer_test(ID3D12Device *dev, ID3D12CommandQueue *queue) {
    const UINT count = 16, stride = 12;
    const UINT words_per_element = stride / sizeof(UINT);
    const UINT word_count = count * words_per_element;
    const UINT64 byte_count = (UINT64)count * stride;
    ID3D12Resource *initial = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, byte_count, 0,
                                     D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *output = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, byte_count,
                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                    D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, byte_count, 0,
                                      D3D12_RESOURCE_STATE_COPY_DEST);
    if (!initial || !output || !readback) return 1;
    UINT init_values[16 * 3];
    for (UINT i = 0; i < word_count; i++) init_values[i] = 0x2000u + i;
    UINT *init_data = NULL;
    CHECK(ID3D12Resource_Map(initial, 0, NULL, (void **)&init_data));
    memcpy(init_data, init_values, sizeof init_values);
    ID3D12Resource_Unmap(initial, 0, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC cpu_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0
    };
    D3D12_DESCRIPTOR_HEAP_DESC gpu_desc = {
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0
    };
    ID3D12DescriptorHeap *cpu_heap = NULL, *gpu_heap = NULL;
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &cpu_desc, &IID_ID3D12DescriptorHeap, (void **)&cpu_heap));
    CHECK(ID3D12Device_CreateDescriptorHeap(dev, &gpu_desc, &IID_ID3D12DescriptorHeap, (void **)&gpu_heap));
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(cpu_heap);
    D3D12_CPU_DESCRIPTOR_HANDLE gpu_cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(gpu_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(gpu_heap);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = { DXGI_FORMAT_UNKNOWN, D3D12_UAV_DIMENSION_BUFFER };
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = count;
    uav.Buffer.StructureByteStride = stride;
    ID3D12Device_CreateUnorderedAccessView(dev, output, NULL, &uav, cpu);
    ID3D12Device_CopyDescriptorsSimple(dev, 1, gpu_cpu, cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ID3D12CommandAllocator *allocator = NULL;
    CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
          &IID_ID3D12CommandAllocator, (void **)&allocator));
    D3D12_RESOURCE_BARRIER transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
    ID3D12DescriptorHeap *heaps[] = { gpu_heap };
    const UINT value[4] = { 0x89abcdefu, 0x12345678u, 0x55555555u, 0xaaaaaaaau };

    // Initialize the resource in a valid list so each invalid clear can be rejected without
    // changing either the contents or the tracked UAV state.
    ID3D12GraphicsCommandList *initialize = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&initialize));
    ID3D12GraphicsCommandList_CopyBufferRegion(initialize, output, 0, initial, 0, byte_count);
    ID3D12GraphicsCommandList_ResourceBarrier(initialize, 1, &transition);
    CHECK(ID3D12GraphicsCommandList_Close(initialize));
    ID3D12CommandList *lists[] = { (ID3D12CommandList *)initialize };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    ID3D12GraphicsCommandList *clear_uint = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&clear_uint));
    ID3D12GraphicsCommandList_SetDescriptorHeaps(clear_uint, 1, heaps);
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewUint(clear_uint, gpu, cpu, output, value, 0, NULL);
    HRESULT uint_close = ID3D12GraphicsCommandList_Close(clear_uint);

    ID3D12GraphicsCommandList *clear_float = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&clear_float));
    ID3D12GraphicsCommandList_SetDescriptorHeaps(clear_float, 1, heaps);
    const float float_value[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    ID3D12GraphicsCommandList_ClearUnorderedAccessViewFloat(clear_float, gpu, cpu, output, float_value, 0, NULL);
    HRESULT float_close = ID3D12GraphicsCommandList_Close(clear_float);

    ID3D12GraphicsCommandList *read = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
          &IID_ID3D12GraphicsCommandList, (void **)&read));
    transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    transition.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    ID3D12GraphicsCommandList_ResourceBarrier(read, 1, &transition);
    ID3D12GraphicsCommandList_CopyBufferRegion(read, readback, 0, output, 0, byte_count);
    CHECK(ID3D12GraphicsCommandList_Close(read));
    lists[0] = (ID3D12CommandList *)read;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    wait_queue(dev, queue);

    UINT *actual = NULL;
    CHECK(ID3D12Resource_Map(readback, 0, NULL, (void **)&actual));
    int wrong = 0;
    for (UINT i = 0; i < word_count; i++) {
        if (actual[i] != init_values[i]) wrong++;
    }
    ID3D12Resource_Unmap(readback, 0, NULL);
    int rejected = uint_close == E_INVALIDARG && float_close == E_INVALIDARG;
    if (!rejected)
        printf("  structured clear Close results: Uint=%#lx, Float=%#lx, expected E_INVALIDARG\n",
               (unsigned long)uint_close, (unsigned long)float_close);
    printf("d3d12 structured-buffer UAV clear: Uint and Float clears rejected at Close; %d seeded DWORDs changed: %s\n",
           wrong, wrong || !rejected ? "FAILED" : "ok");

    ID3D12GraphicsCommandList_Release(initialize);
    ID3D12GraphicsCommandList_Release(clear_uint);
    ID3D12GraphicsCommandList_Release(clear_float);
    ID3D12GraphicsCommandList_Release(read);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12DescriptorHeap_Release(gpu_heap);
    ID3D12DescriptorHeap_Release(cpu_heap);
    ID3D12Resource_Release(readback);
    ID3D12Resource_Release(output);
    ID3D12Resource_Release(initial);
    return wrong || !rejected ? 1 : 0;
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
        D3D12_RESOURCE_BARRIER backbuffer_state = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
        backbuffer_state.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ back[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET };
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &backbuffer_state);
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, colors[f], 0, NULL);
        backbuffer_state.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        backbuffer_state.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &backbuffer_state);
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
    D3D12_RESOURCE_BARRIER backbuffer_read = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    backbuffer_read.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ back[0], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &backbuffer_read);
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
    BOOL copy_timestamps_supported = FALSE;
    if (root_signature_validation_test(dev)) return 1;
    if (direct_heap_root_flag_test(dev)) return 1;
    if (graphics_pso_state_test(dev)) return 1;
    if (feature_test(dev, &copy_timestamps_supported)) return 1;
    if (footprints_test(dev)) return 1;
    if (allocation_info_alignment_test(dev)) return 1;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue *queue;
    CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    if (queue_description_test(dev)) return 1;
    if (fence_flags_test(dev)) return 1;
    if (cross_queue_fence_test(dev)) return 1;
    if (command_allocator_lifetime_test(dev, queue)) return 1;
    if (write_buffer_immediate_test(dev, queue)) return 1;
    if (state_transition_test(dev, queue)) return 1;
    if (residency_test(dev, queue)) return 1;
    if (copy_queue_test(dev, copy_timestamps_supported)) return 1;
    if (attachment_state_test(dev, queue)) return 1;
    if (rect_clear_test(dev, queue)) return 1;
    if (rect_stencil_clear_test(dev, queue)) return 1;
    if (compressed_copy_test(dev, queue)) return 1;
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
    D3D12_INDIRECT_ARGUMENT_DESC dispatch_arg = { 0 };
    dispatch_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC dispatch_signature_desc = { 16, 1, &dispatch_arg, 0 };
    ID3D12CommandSignature *dispatch_signature;
    CHECK(ID3D12Device_CreateCommandSignature(dev, &dispatch_signature_desc, NULL,
                                               &IID_ID3D12CommandSignature, (void **)&dispatch_signature));
    D3D12_INDIRECT_ARGUMENT_DESC invalid_args[2] = { 0 };
    invalid_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
    invalid_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC invalid_signature_desc = { 28, 2, invalid_args, 0 };
    ID3D12CommandSignature *invalid_signature = NULL;
    HRESULT invalid_hr = ID3D12Device_CreateCommandSignature(dev, &invalid_signature_desc, NULL,
                                                              &IID_ID3D12CommandSignature, (void **)&invalid_signature);
    if (invalid_hr != E_INVALIDARG || invalid_signature) { printf("D3D12 accepted a vertex binding in a compute signature\n"); bad++; }

    // resources
    UINT in[64], expect_sum = 0, init[66] = { 1000 };
    for (int i = 0; i < 64; i++) { in[i] = (UINT)(i * 7 + 3); expect_sum += in[i]; }
    ID3D12Resource *up_in = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof in, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *up_init = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, sizeof init, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource *uav = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, sizeof init, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *indirect_count = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, sizeof(UINT), 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *uav_copy = buffer(dev, D3D12_HEAP_TYPE_DEFAULT, sizeof init, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *readback = buffer(dev, D3D12_HEAP_TYPE_READBACK, sizeof init, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *img_rb = buffer(dev, D3D12_HEAP_TYPE_READBACK, 1024, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *dispatch_args = buffer(dev, D3D12_HEAP_TYPE_UPLOAD, 32, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (bad || !dispatch_args || !indirect_count) return 1;
    void *p;
    CHECK(ID3D12Resource_Map(up_in, 0, NULL, &p)); memcpy(p, in, sizeof in); ID3D12Resource_Unmap(up_in, 0, NULL);
    CHECK(ID3D12Resource_Map(up_init, 0, NULL, &p)); memcpy(p, init, sizeof init); ID3D12Resource_Unmap(up_init, 0, NULL);
    UINT dispatch_records[8] = { 1, 1, 1, 0xdeadbeef, 1, 1, 1, 0xcafebabe };
    CHECK(ID3D12Resource_Map(dispatch_args, 0, NULL, &p)); memcpy(p, dispatch_records, sizeof dispatch_records); ID3D12Resource_Unmap(dispatch_args, 0, NULL);
    ID3D12GraphicsCommandList *invalid_list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                          &IID_ID3D12GraphicsCommandList, (void **)&invalid_list));
    ID3D12GraphicsCommandList_ExecuteIndirect(invalid_list, dispatch_signature, 1, dispatch_args, 1, NULL, 0);
    invalid_hr = ID3D12GraphicsCommandList_Close(invalid_list);
    if (invalid_hr != E_INVALIDARG) { printf("D3D12 accepted an unaligned ExecuteIndirect argument offset: %#lx\n", (unsigned long)invalid_hr); bad++; }
    ID3D12GraphicsCommandList_Release(invalid_list);
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

    // A UAV descriptor cannot be dispatched while its buffer is still COPY_DEST. The following transition is
    // otherwise valid, so a missing shader-use check would commit COPY_SOURCE and break the real dispatch below.
    ID3D12GraphicsCommandList *bad_state_list = NULL;
    CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
        &IID_ID3D12GraphicsCommandList, (void **)&bad_state_list));
    ID3D12GraphicsCommandList_SetPipelineState(bad_state_list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(bad_state_list, rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(bad_state_list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(bad_state_list, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(bad_state_list, 1, 1, 1);
    D3D12_RESOURCE_BARRIER bad_uav_transition = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    bad_uav_transition.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ uav, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(bad_state_list, 1, &bad_uav_transition);
    CHECK(ID3D12GraphicsCommandList_Close(bad_state_list));
    ID3D12CommandList *bad_state_lists[] = { (ID3D12CommandList *)bad_state_list };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, bad_state_lists);
    ID3D12GraphicsCommandList_Release(bad_state_list);

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
    D3D12_RESOURCE_BARRIER simple_indirect_order = { D3D12_RESOURCE_BARRIER_TYPE_UAV };
    simple_indirect_order.UAV.pResource = uav;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &simple_indirect_order);
    // Two fixed-stride DISPATCH records, no count buffer or root updates: Metal consumes both on the GPU.
    ID3D12GraphicsCommandList_ExecuteIndirect(list, dispatch_signature, 2, dispatch_args, 0, NULL, 0);
    D3D12_RESOURCE_BARRIER count_barriers[3] = { 0 };
    count_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    count_barriers[0].UAV.pResource = uav;
    count_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    count_barriers[1].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ uav, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
    count_barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    count_barriers[2].Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ indirect_count, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &count_barriers[0]);
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &count_barriers[1]);
    // Snapshot the shader-produced uav[1] into a separate INDIRECT_ARGUMENT resource. D3D12 requires the
    // count resource and the shader's UAV output to be in their respective states at the indirect dispatch.
    ID3D12GraphicsCommandList_CopyBufferRegion(list, indirect_count, 0, uav, sizeof(UINT), sizeof(UINT));
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &count_barriers[2]);
    count_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    count_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &count_barriers[1]);
    ID3D12GraphicsCommandList_ExecuteIndirect(list, dispatch_signature, 2, dispatch_args, 0, indirect_count, 0);
    D3D12_RESOURCE_BARRIER uav_to_copy = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    uav_to_copy.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ uav, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &uav_to_copy);
    ID3D12GraphicsCommandList_CopyResource(list, uav_copy, uav);
    D3D12_RESOURCE_BARRIER copy_state = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    copy_state.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ uav_copy, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &copy_state);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, uav_copy, 0, sizeof init);
    D3D12_TEXTURE_COPY_LOCATION dst = { img_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = 64;
    dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 1024;
    D3D12_TEXTURE_COPY_LOCATION src = { img, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    src.SubresourceIndex = 0;
    D3D12_RESOURCE_BARRIER image_state = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
    image_state.Transition = (D3D12_RESOURCE_TRANSITION_BARRIER){ img, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &image_state);
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
    if (o[0] != 1000 + 4 * expect_sum) { printf("InterlockedAdd gave %u, want %u\n", o[0], 1000 + 4 * expect_sum); bad++; }
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
    printf("d3d12 compute: descriptor table, DXIL dispatch, two no-count native ExecuteIndirect dispatches, GPU-side counted ExecuteIndirect dispatch clamped to MaxCommandCount, buffer CopyResource, "
           "readback of a buffer and a texture, fence (SIMD width %u): %s\n", simd, bad ? "FAILED" : "ok");
    if (draw_test(dev, queue)) bad++;
    if (resolve_test(dev, queue)) bad++;
    if (resolve_region_test(dev, queue)) bad++;
    if (resolve_integer_region_test(dev, queue)) bad++;
    if (depth_resolve_test(dev, queue)) bad++;
    if (depth_resolve_average_samples_test(dev, queue, DXGI_FORMAT_D32_FLOAT, 0)) bad++;
    if (depth_resolve_average_samples_test(dev, queue, DXGI_FORMAT_D16_UNORM, 1)) bad++;
    if (texture_test(dev, queue)) bad++;
    if (comparison_test(dev, queue)) bad++;
    if (typed_buffer_test(dev, queue)) bad++;
    if (typed_sint_buffer_test(dev, queue)) bad++;
    if (signed_conversions_test(dev, queue)) bad++;
    if (typed_float_buffer_test(dev, queue)) bad++;
    if (texture_load_bounds_test(dev, queue)) bad++;
    if (wave_operations_test(dev, queue)) bad++;
    if (atomic_resource_test(dev, queue)) bad++;
    if (clear_uav_texture_test(dev, queue, DXGI_FORMAT_R8G8B8A8_UNORM, 1)) bad++;
    if (clear_uav_texture_test(dev, queue, DXGI_FORMAT_R8_UINT, 0)) bad++;
    if (clear_uav_texture_test(dev, queue, DXGI_FORMAT_R8_SINT, 0)) bad++;
    if (clear_uav_texture_test(dev, queue, DXGI_FORMAT_R16_UINT, 0)) bad++;
    if (clear_uav_texture_test(dev, queue, DXGI_FORMAT_R16_SINT, 0)) bad++;
    if (clear_uav_texture_test(dev, queue, DXGI_FORMAT_R32_UINT, 0)) bad++;
    if (clear_uav_texture_test(dev, queue, DXGI_FORMAT_R32_SINT, 0)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R8_UINT, 0x9abcdef0u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R8_SINT, 0x876543feu)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R8G8_UINT, 0x9abcdef0u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R8G8_SINT, 0x876543feu)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R8G8B8A8_UINT, 0x9abcdef0u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R8G8B8A8_SINT, 0x876543feu)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R16_UINT, 0x9abcdef0u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R16_SINT, 0x8765c321u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R16G16_UINT, 0x9abcdef0u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R16G16_SINT, 0x8765c321u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R16G16B16A16_UINT, 0x9abcdef0u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R16G16B16A16_SINT, 0x8765c321u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R32_UINT, 0x9abcdef0u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R32_SINT, 0x87654321u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R32G32_UINT, 0x9abcdef0u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R32G32_SINT, 0x87654321u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R32G32B32A32_UINT, 0x9abcdef0u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R32G32B32A32_SINT, 0x87654321u)) bad++;
    if (clear_uav_typed_buffer_test(dev, queue, DXGI_FORMAT_R10G10B10A2_UINT, 0x9abcdef0u)) bad++;
    if (clear_uav_typed_float_buffer_test(dev, queue, DXGI_FORMAT_R32_FLOAT)) bad++;
    if (clear_uav_typed_float_buffer_test(dev, queue, DXGI_FORMAT_R32G32_FLOAT)) bad++;
    if (clear_uav_typed_float_buffer_test(dev, queue, DXGI_FORMAT_R32G32B32A32_FLOAT)) bad++;
    if (clear_uav_typed_float_buffer_test(dev, queue, DXGI_FORMAT_R16_FLOAT)) bad++;
    if (clear_uav_typed_float_buffer_test(dev, queue, DXGI_FORMAT_R16G16_FLOAT)) bad++;
    if (clear_uav_typed_float_buffer_test(dev, queue, DXGI_FORMAT_R16G16B16A16_FLOAT)) bad++;
    if (clear_uav_raw_buffer_test(dev, queue)) bad++;
    if (clear_uav_structured_buffer_test(dev, queue)) bad++;
    present_test(dev, queue);
    return bad ? 1 : 0;
}
