// Tessellation test: control points straight through to the hull shader.
struct VSIn { float3 pos : POSITION; float4 color : COLOR; };
struct VSOut { float4 pos : CPPOS; float4 color : COLOR; };
VSOut main(VSIn i) { VSOut o; o.pos = float4(i.pos, 1); o.color = i.color; return o; }
