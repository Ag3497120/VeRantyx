// DXIL (dxil.h) -> Metal Shading Language: Direct3D 12's shaders translated by VXRuntime itself.
//
// Resources reach the shader through one argument buffer at [[buffer(0)]] (VxRoot): a pointer per resource range the
// shader declares ("slot", in dx.resources order: SRVs, UAVs, CBVs, samplers), each pointing at that range's
// descriptors, and the two descriptor heaps (SM 6.6 ResourceDescriptorHeap / SamplerDescriptorHeap). A descriptor is
// 32 bytes (VxDesc): a GPU address, a Metal resource id (texture, texture buffer or sampler), a size in bytes (or
// ResourceMinLODClamp float bits for a flagged texture SRV), a structure stride, a format and flags. The runtime
// resolves the root signature into those pointers at draw time, so one translation serves any root signature.
#ifndef VX_DXIL_MSL_H
#define VX_DXIL_MSL_H
#include "dxil.h"

enum { DXIL_SRV, DXIL_UAV, DXIL_CBV, DXIL_SAMPLER };
// Flags in the 32-byte descriptor ABI shared by D3D12 heaps and translated DXIL.
enum {
    DXIL_MSL_DESC_RAW = 1u,
    DXIL_MSL_DESC_HAS_COUNTER = 2u,
    DXIL_MSL_DESC_HAS_MIN_LOD_CLAMP = 4u,
    DXIL_MSL_DESC_HAS_W_SLICE_RANGE = 8u,
    DXIL_MSL_DESC_COMPONENT_MAPPING_SHIFT = 8u,
    DXIL_MSL_DESC_COMPONENT_MAPPING_MASK = 0x1fffu << DXIL_MSL_DESC_COMPONENT_MAPPING_SHIFT
};
enum {
    DXIL_MSL_ACCESS_READ = 1u,
    DXIL_MSL_ACCESS_WRITE = 2u
};

typedef struct {
    uint8_t cls;             // DXIL_SRV ... DXIL_SAMPLER
    uint8_t kind;            // DXIL resource kind (2 Texture2D, 11 RawBuffer, 12 StructuredBuffer, 13 CBuffer, ...)
    uint8_t comp;            // component type (9 F32, 5 U32, ...) for typed resources
    uint8_t has_counter;     // UAV metadata: the structured resource has a hidden append/consume counter
    uint32_t space, lower, count;   // register space, first register, registers (UINT32_MAX: unbounded)
    uint32_t stride;         // structured buffers
    uint8_t uav_access;      // DXIL_MSL_ACCESS_* union; zero for non-UAV slots
} dxil_msl_slot;

typedef struct {
    char name[32];           // semantic name ("TEXCOORD", "SV_Position", ...)
    uint32_t index;          // semantic index
    uint8_t kind;            // DXIL semantic kind (0 arbitrary, 3 position, 16 target, ...)
    uint8_t comp;            // component type
    uint8_t cols;
    uint32_t attribute;      // vertex shader inputs: the [[attribute(n)]] the input layout must feed
} dxil_msl_element;

typedef struct {
    int stage;               // the program header's shader kind: 0 pixel, 1 vertex, 5 compute, ...
    uint32_t threads[3];     // compute: [numthreads]
    uint32_t nslots;
    dxil_msl_slot slots[64];
    int uses_heap, uses_sampler_heap;
    int uses_counters;
    int uses_min_lod_clamp;  // A DXIL sample instruction carries its own non-undef min-LOD operand.
    int uses_gather;         // Metal gather has no per-operation min-LOD clamp overload.
    int uses_texture_load;   // ResourceMinLODClamp still rejects integer-coordinate reads without a matching clamp path.
    int uses_component_mapping_unsupported_read; // SampleCmp and GatherCmp need API-specific component handling.
    int uses_thread_fence;   // MSL atomic_thread_fence requires Metal shading language 3.2.
    uint32_t heap_resource_classes; // bitset of DXIL_SRV / DXIL_UAV / DXIL_CBV descriptors used from ResourceDescriptorHeap
    uint32_t ninputs, noutputs;
    dxil_msl_element inputs[32], outputs[32];
    int structured;          // 1: the control flow came out as if/else and loops; 0: as a state machine
    char *msl;               // the source, entry point "vx_main"
    char error[256];
    // Geometry pipelines (Metal has no geometry stage; d3d12.m draws them as in the DXBC path): the vertex shader as
    // a kernel writing each vertex's outputs (one float4 of bits per output row, in outputs[] order) and the geometry
    // shader as a kernel turning its triangle strips into triangles, drawn by msl_passthrough.
    char *msl_passthrough;   // geometry: the vertex function drawing the kernel's triangles
    uint32_t io_slots;       // vertex kernel / geometry kernel: float4s written per vertex
    uint32_t gs_in_verts, gs_max_tris;
    // Tessellation: a hull shader as a kernel (msl: per patch, the control-point phase for each output control point,
    // then the patch-constant phase writing the patch constants and Metal's half tessellation factors) and a domain
    // shader as a post-tessellation vertex function (msl). outputs[] of a hull shader are its control points' rows,
    // patch[] its patch constants' (io_slots, npatch float4s a control point, a patch).
    uint32_t npatch;
    dxil_msl_element patch[32];
    uint32_t tess_domain;        // 2 triangle, 3 quad (D3D12_TESSELLATOR_DOMAIN)
    uint32_t tess_partitioning;  // 1 integer, 2 pow2, 3 fractional odd, 4 fractional even
    uint32_t tess_output;        // 3 triangle cw, 4 triangle ccw
    uint32_t tess_in_cp, tess_out_cp;
    float tess_max_factor;
} dxil_msl;

// Buffer indices of the geometry kernels, the same as the DXBC path's (dxbc.h DXBC_GS_*).
enum { DXIL_GS_COUNTS = 26, DXIL_GS_INDEX = 27, DXIL_GS_PARAMS = 27, DXIL_VS_PARAMS = 28, DXIL_GS_IN = 28, DXIL_GS_OUT = 29 };
// The vertex shader as a kernel (msl): thread (x, y) is vertex x of instance y; stage-input attributes as the vertex
// function's, outputs to buffer DXIL_GS_OUT. vp[0]: vertices, first vertex, base instance; vp[1]: indices (0 none,
// 1 16-bit, 2 32-bit), first index, base vertex (the index buffer at DXIL_GS_INDEX).
int dxil_to_msl_vertex_kernel(const dxil_module *m, dxil_msl *out);
// A geometry shader (triangle-strip output) as a kernel (msl) whose inputs are the vertex kernel `vs` wrote, matched
// by semantic, and its pass-through vertex function (msl_passthrough).
int dxil_to_msl_geometry(const dxil_module *m, const dxil_msl *vs, dxil_msl *out);
// Buffer indices of the tessellation kernels and the domain function.
enum { DXIL_HS_IN = 28, DXIL_HS_CP = 29, DXIL_HS_PC = 30, DXIL_HS_FACTORS = 26, DXIL_HS_PARAMS = 27,
       DXIL_DS_CP = 1, DXIL_DS_PC = 2, DXIL_DS_PARAMS = 3 };
// A hull shader as a kernel: thread (x, y) is patch x of instance y; its input control points are the vertex kernel
// `vs` wrote (matched by semantic). hp[0]: patches per instance, vertices per instance, the vertex kernel's slots.
int dxil_to_msl_hull(const dxil_module *m, const dxil_msl *vs, dxil_msl *out);
// A domain shader as a post-tessellation vertex function reading the hull kernel's control points and patch constants
// (matched by semantic to `hs`). dp: control points per patch.
int dxil_to_msl_domain(const dxil_module *m, const dxil_msl *hs, dxil_msl *out);

int dxil_to_msl(const dxil_module *m, dxil_msl *out);
// A pixel shader for a pipeline that blends with the second source (SRC1 factors): SV_Target1 as color(0) index(1).
int dxil_to_msl_pixel(const dxil_module *m, uint32_t sample_mask, int dual_source, dxil_msl *out);
// Apply the D3D12 PSO's static sample mask to fragment coverage. UINT32_MAX is a no-op.
int dxil_to_msl_with_sample_mask(const dxil_module *m, uint32_t sample_mask, dxil_msl *out);
void dxil_msl_free(dxil_msl *out);
#endif
