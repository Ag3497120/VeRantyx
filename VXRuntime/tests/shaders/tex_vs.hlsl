// Position scaled and moved by a constant buffer; texture coordinates passed through.
cbuffer Transform : register(b0) { float2 offset; float2 scale; };
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos * scale + offset, 0.5, 1); o.uv = i.uv; return o; }
