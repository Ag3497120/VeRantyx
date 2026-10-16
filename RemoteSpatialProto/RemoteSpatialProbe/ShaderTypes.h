// Types shared between Shaders.metal and Swift (via the bridging header).
#ifndef ShaderTypes_h
#define ShaderTypes_h

#include <simd/simd.h>

// Every piece of geometry in the probe is a flat-coloured triangle in
// world space (immersive-space origin, metres). No vertex descriptor:
// the shader reads this struct by vertex_id.
typedef struct {
    simd_float4 position;   // xyz in metres, w = 1
    simd_float4 color;      // linear RGBA
} ProbeVertex;

// One view-projection matrix per amplified view (left, right).
typedef struct {
    matrix_float4x4 viewProjection[2];
} ProbeUniforms;

#define ProbeBufferVertices 0
#define ProbeBufferUniforms 1

#endif /* ShaderTypes_h */
