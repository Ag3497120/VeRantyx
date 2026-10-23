// A triangle over the whole target from the vertex index alone, red.
#version 450
layout(location = 0) out vec4 col;
void main() {
    vec2 p = vec2(float((gl_VertexIndex & 1) * 4 - 1), float((gl_VertexIndex >> 1) * 4 - 1));
    gl_Position = vec4(p, 0.0, 1.0);
    col = vec4(1.0, 0.0, 0.0, 1.0);
}
