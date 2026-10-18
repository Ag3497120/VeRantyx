struct In { float4 pos : SV_Position; float4 color : COLOR; };
cbuffer Tint : register(b0) { float4 tint; };
float4 main(In i) : SV_Target { return i.color * tint + float4(0.25, 0, 0, 0); }
