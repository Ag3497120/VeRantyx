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
    DXIL_MSL_DESC_HAS_W_SLICE_RANGE = 8u
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
    int uses_thread_fence;   // MSL atomic_thread_fence requires Metal shading language 3.2.
    uint32_t heap_resource_classes; // bitset of DXIL_SRV / DXIL_UAV / DXIL_CBV descriptors used from ResourceDescriptorHeap
    uint32_t ninputs, noutputs;
    dxil_msl_element inputs[32], outputs[32];
    int structured;          // 1: the control flow came out as if/else and loops; 0: as a state machine
    char *msl;               // the source, entry point "vx_main"
    char error[256];
} dxil_msl;

int dxil_to_msl(const dxil_module *m, dxil_msl *out);
// Apply the D3D12 PSO's static sample mask to fragment coverage. UINT32_MAX is a no-op.
int dxil_to_msl_with_sample_mask(const dxil_module *m, uint32_t sample_mask, dxil_msl *out);
void dxil_msl_free(dxil_msl *out);
#endif
