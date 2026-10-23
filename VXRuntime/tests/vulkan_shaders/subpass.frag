// Reads the first subpass's color as an input attachment.
#version 450
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput a;
layout(location = 0) out vec4 o;
void main() { o = subpassLoad(a).bgra; }
