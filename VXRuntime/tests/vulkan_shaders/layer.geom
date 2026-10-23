// Passes triangles through to layer 1 with the color's red and blue swapped.
#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec4 vcol[];
layout(location = 0) out vec4 col;
void main() {
    for (int i = 0; i < 3; i++) {
        gl_Position = gl_in[i].gl_Position;
        col = vcol[i].bgra;
        gl_Layer = 1;
        EmitVertex();
    }
    EndPrimitive();
}
