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
    char error[256];
} dxbc_shader;

// Translates a DXBC container. Returns 0 and fills *out, or -1 with out->error set.
int  dxbc_translate(const void *code, size_t size, dxbc_shader *out);
void dxbc_free(dxbc_shader *s);
// The input signature of a container (for input layouts), without translating it.
int  dxbc_input_signature(const void *code, size_t size, dxbc_element *elements, int max);
#endif
