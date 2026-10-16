#include <metal_stdlib>
#include <simd/simd.h>
#import "ShaderTypes.h"

using namespace metal;

struct ColorInOut {
    float4 position [[position]];
    float4 color;
};

// Vertex amplification draws both eyes in one pass (layered layout);
// amp_id selects the eye's view-projection matrix.
vertex ColorInOut probeVertex(uint vid [[vertex_id]],
                              ushort amp_id [[amplification_id]],
                              const device ProbeVertex *vertices [[buffer(ProbeBufferVertices)]],
                              constant ProbeUniforms &uniforms [[buffer(ProbeBufferUniforms)]])
{
    ColorInOut out;
    out.position = uniforms.viewProjection[amp_id] * vertices[vid].position;
    out.color = vertices[vid].color;
    return out;
}

fragment float4 probeFragment(ColorInOut in [[stage_in]])
{
    return in.color;
}
