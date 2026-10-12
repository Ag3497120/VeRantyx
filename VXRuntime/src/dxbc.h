// DXBC (Direct3D shader model 4 and 5 bytecode) to Metal Shading Language.
#ifndef VX_DXBC_H
#define VX_DXBC_H
#include <stddef.h>
#include <stdint.h>

enum { DXBC_PIXEL = 0, DXBC_VERTEX = 1, DXBC_COMPUTE = 5 };

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
    int uses_sizes;                 // reads buffer sizes (bufinfo) from buffer(DXBC_SIZES_BUFFER)
    char error[256];
} dxbc_shader;

enum { DXBC_UAV_TYPED = 1, DXBC_UAV_RAW, DXBC_UAV_STRUCTURED };
// Metal binding indices beyond the Direct3D slot numbers: UAV buffers, raw/structured SRV buffers, UAV
// textures, and the buffer-size table.
#define DXBC_UAV_BUFFER_BASE 14
#define DXBC_SRV_BUFFER_BASE 22
#define DXBC_UAV_TEXTURE_BASE 64
#define DXBC_SIZES_BUFFER 30

// Translates a DXBC container. Returns 0 and fills *out, or -1 with out->error set.
int  dxbc_translate(const void *code, size_t size, dxbc_shader *out);
void dxbc_free(dxbc_shader *s);
// The input signature of a container (for input layouts), without translating it.
int  dxbc_input_signature(const void *code, size_t size, dxbc_element *elements, int max);
#endif
