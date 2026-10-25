// Two components to an RGBA8 target, and a depth value with no depth attachment (Vulkan drops it).
#version 450
layout(location = 0) out vec2 o;
void main() { o = vec2(0.25, 0.75); gl_FragDepth = 0.125; }
