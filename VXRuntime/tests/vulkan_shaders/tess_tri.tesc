// Triangle patches: the control points through gl_out and a user output, levels from the push constants, a patch
// output (a scale the evaluation shader applies to its bump).
#version 450
layout(vertices = 3) out;
layout(push_constant) uniform PC { float level; } pc;
layout(location = 0) in vec2 vuv[];
layout(location = 0) out vec2 cuv[];
layout(location = 1) patch out float bump_scale;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    cuv[gl_InvocationID] = vuv[gl_InvocationID];
    barrier();
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = pc.level; gl_TessLevelOuter[1] = pc.level; gl_TessLevelOuter[2] = pc.level;
        gl_TessLevelInner[0] = pc.level;
        bump_scale = cuv[1].x - cuv[0].x;          // 1 (read after the barrier: another invocation's output)
    }
}
