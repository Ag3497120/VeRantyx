// Quad patches: as tess_tri.tesc with four control points.
#version 450
layout(vertices = 4) out;
layout(push_constant) uniform PC { float level; } pc;
layout(location = 0) in vec2 vuv[];
layout(location = 0) out vec2 cuv[];
layout(location = 1) patch out float bump_scale;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    cuv[gl_InvocationID] = vuv[gl_InvocationID];
    barrier();
    if (gl_InvocationID == 0) {
        for (int i = 0; i < 4; i++) gl_TessLevelOuter[i] = pc.level;
        gl_TessLevelInner[0] = pc.level; gl_TessLevelInner[1] = pc.level;
        bump_scale = cuv[1].x - cuv[0].x;
    }
}
