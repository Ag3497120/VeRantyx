#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(binding=0) uniform sampler2D source;
layout(push_constant) uniform Params{mat4 rotation;vec4 srcFov;vec4 dstFov;vec4 eye;} p;
void main(){vec3 ray=vec3(mix(p.dstFov.x,p.dstFov.y,uv.x),mix(p.dstFov.w,p.dstFov.z,uv.y),-1);vec3 old=(p.rotation*vec4(ray,0)).xyz;vec2 tangent=old.xy/(-old.z);vec2 st=vec2((tangent.x-p.srcFov.x)/(p.srcFov.y-p.srcFov.x),(p.srcFov.w-tangent.y)/(p.srcFov.w-p.srcFov.z));if(old.z>=0||any(lessThan(st,vec2(0)))||any(greaterThan(st,vec2(1))))color=vec4(0,0,0,1);else color=texture(source,vec2((st.x+p.eye.x)*0.5,st.y));}
