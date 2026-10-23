// Two color outputs (an sRGB target and a B10G11R11 one).
#version 450
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
void main() {
    o0 = vec4(0.5, 0.25, 1.0, 1.0);
    o1 = vec4(2.5, 0.125, 64.0, 1.0);
}
