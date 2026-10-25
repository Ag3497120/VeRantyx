// Quads (ccw): the patch covers the target, its top edge (v = 0) bowed down by a sine bump when tessellated.
#version 450
layout(quads, fractional_odd_spacing, ccw) in;
layout(location = 0) in vec2 cuv[];
layout(location = 1) patch in float bump_scale;
layout(location = 0) out vec4 col;
void main() {
    vec2 c = gl_TessCoord.xy;
    vec4 a = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, c.x), b = mix(gl_in[3].gl_Position, gl_in[2].gl_Position, c.x);
    vec4 p = mix(a, b, c.y);
    p.y += (1.0 - c.y) * sin(3.14159265 * c.x) * bump_scale;
    gl_Position = p;
    col = vec4(0.0, 1.0, cuv[0].x * 0.0, 1.0);
}
