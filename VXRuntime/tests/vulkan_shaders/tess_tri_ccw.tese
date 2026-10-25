// Triangles (ccw): the position from the control points by gl_TessCoord, the edge between the first two pushed
// towards the third by a bump (4uv, scaled by the patch output): straight with level 1, bowed when tessellated.
#version 450
layout(triangles, equal_spacing, ccw) in;
layout(location = 0) in vec2 cuv[];
layout(location = 1) patch in float bump_scale;
layout(location = 0) out vec4 col;
void main() {
    vec3 c = gl_TessCoord;
    vec4 p = c.x * gl_in[0].gl_Position + c.y * gl_in[1].gl_Position + c.z * gl_in[2].gl_Position;
    p.y += 4.0 * c.x * c.y * bump_scale;
    gl_Position = p;
    col = vec4(0.0, 1.0, (cuv[0].x + cuv[1].x + cuv[2].x) * 0.0, 1.0);
}
