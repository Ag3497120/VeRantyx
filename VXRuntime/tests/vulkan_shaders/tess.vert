// Tessellation's vertex shader: a position from location 0, and a user output the control shader carries on.
#version 450
layout(location = 0) in vec2 pos;
layout(location = 0) out vec2 vuv;
void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    vuv = pos * 0.5 + 0.5;
}
