struct VSIn { float2 pos : POSITION; float4 color : COLOR; };
struct VSOut { float4 pos : SV_Position; float4 color : COLOR; };
VSOut main(VSIn v) { VSOut o; o.pos = float4(v.pos, 0.5, 1); o.color = v.color; return o; }
