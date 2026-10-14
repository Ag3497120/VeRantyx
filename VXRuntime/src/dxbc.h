// DXBC (Direct3D shader model 4 and 5 bytecode) to Metal Shading Language.
#ifndef VX_DXBC_H
#define VX_DXBC_H
#include <stddef.h>
#include <stdint.h>

enum { DXBC_PIXEL = 0, DXBC_VERTEX = 1, DXBC_GEOMETRY = 2, DXBC_COMPUTE = 5 };

// One element of an input or output signature.
typedef struct {
    char semantic[64];
    uint32_t index, sysval, component_type, reg;   // component type: 1 uint, 2 int, 3 float
    uint8_t mask;
} dxbc_element;

typedef struct {
    int type;                       // DXBC_PIXEL, DXBC_VERTEX, ...
    int major, minor;
    char *msl;                      // the generated source (malloc'd)
    const char *entry;              // its function name
    dxbc_element inputs[32], outputs[32];
    int ninputs, noutputs;
    uint32_t cbuffers;              // slots used (bit per slot)
    uint64_t textures[2];           // t0..t127
    uint32_t samplers;
    // Compute (and UAV) resources: unordered access views u0..u7, raw/structured buffer SRVs, the group size.
    uint32_t uavs;
    uint8_t uav_kind[8];            // DXBC_UAV_*
    uint8_t uav_read[8];            // a typed UAV that is also read (needs read_write access)
    uint32_t uav_stride[8];         // structured element size
    uint64_t srv_buffers;           // t# that are raw or structured buffers (bound as Metal buffers)
    uint32_t srv_stride[64];
    uint32_t threads[3];            // [numthreads(x, y, z)]
    int uses_sizes;                 // reads buffer sizes (bufinfo) or UAV counters from buffer(DXBC_SIZES_BUFFER)
    uint8_t uav_counters;           // u# with a hidden counter (append, consume, IncrementCounter)
    int writes_depth;               // a pixel shader writing oDepth
    // Geometry shaders run as compute kernels (Metal has no such stage): the vertex shader before one runs as a
    // kernel too (msl_kernel: its outputs to a buffer, out_regs float4s per vertex), then the geometry kernel turns
    // each input primitive (gs_in_verts vertices) into up to gs_max_tris triangles of out_regs float4s per vertex,
    // which msl_passthrough (a vertex function) hands to the pixel shader.
    int out_regs;                   // output registers o0..o(n-1) (vertex and geometry shaders)
    int gs_in_verts, gs_max_tris, gs_instances;
    char *msl_kernel, *msl_passthrough;
    char error[256];
} dxbc_shader;

enum { DXBC_UAV_TYPED = 1, DXBC_UAV_RAW, DXBC_UAV_STRUCTURED };
// Metal binding indices beyond the Direct3D slot numbers: UAV buffers, raw/structured SRV buffers, UAV
// textures, and the auxiliary table (dxbc_aux: buffer sizes and UAV counter addresses).
#define DXBC_UAV_BUFFER_BASE 14
#define DXBC_SRV_BUFFER_BASE 22
#define DXBC_UAV_TEXTURE_BASE 64
#define DXBC_SIZES_BUFFER 30
// The geometry path's buffers (vertex kernel: its parameters, outputs, index buffer; geometry kernel: its
// parameters, inputs, outputs).
#define DXBC_GS_INDEX 27
#define DXBC_GS_PARAMS 27
#define DXBC_VS_PARAMS 28
#define DXBC_GS_IN 28
#define DXBC_GS_OUT 29
#define DXBC_GS_COUNTS 26           // (the geometry kernel: triangles made per primitive)
typedef struct { uint32_t sizes[32]; uint64_t counters[8]; } dxbc_aux;   // bytes of u0..u7 then t0..t21; GPU addresses

// Translates a DXBC container. Returns 0 and fills *out, or -1 with out->error set.
int  dxbc_translate(const void *code, size_t size, dxbc_shader *out);
void dxbc_free(dxbc_shader *s);
// The input signature of a container (for input layouts), without translating it.
int  dxbc_input_signature(const void *code, size_t size, dxbc_element *elements, int max);
// The program type of a container (DXBC_VERTEX, ...), or -1.
int  dxbc_program_type(const void *code, size_t size);
#endif
