// A position from vertex input location 0 (vec2), green.
#version 450
layout(location = 0) in vec2 pos;
layout(location = 0) out vec4 col;
void main() {
    gl_Position = vec4(pos, 0.0, 1.0);
    col = vec4(0.0, 1.0, 0.0, 1.0);
}
