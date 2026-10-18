// DXIL (dxil.h) -> Metal Shading Language: Direct3D 12's shaders translated by VXRuntime itself.
//
// Resources reach the shader through one argument buffer at [[buffer(0)]] (VxRoot): a pointer per resource range the
// shader declares ("slot", in dx.resources order: SRVs, UAVs, CBVs, samplers), each pointing at that range's
// descriptors, and the two descriptor heaps (SM 6.6 ResourceDescriptorHeap / SamplerDescriptorHeap). A descriptor is
// 32 bytes (VxDesc): a GPU address, a Metal resource id (texture, texture buffer or sampler), a size in bytes, a
// structure stride, a format and flags. The runtime resolves the root signature into those pointers at draw time, so
// one translation serves any root signature.
#ifndef VX_DXIL_MSL_H
#define VX_DXIL_MSL_H
#include "dxil.h"

enum { DXIL_SRV, DXIL_UAV, DXIL_CBV, DXIL_SAMPLER };

typedef struct {
    uint8_t cls;             // DXIL_SRV ... DXIL_SAMPLER
    uint8_t kind;            // DXIL resource kind (2 Texture2D, 11 RawBuffer, 12 StructuredBuffer, 13 CBuffer, ...)
    uint8_t comp;            // component type (9 F32, 5 U32, ...) for typed resources
    uint32_t space, lower, count;   // register space, first register, registers (UINT32_MAX: unbounded)
    uint32_t stride;         // structured buffers
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
    uint32_t ninputs, noutputs;
    dxil_msl_element inputs[32], outputs[32];
    int structured;          // 1: the control flow came out as if/else and loops; 0: as a state machine
    char *msl;               // the source, entry point "vx_main"
    char error[256];
} dxil_msl;

int dxil_to_msl(const dxil_module *m, dxil_msl *out);
void dxil_msl_free(dxil_msl *out);
#endif
