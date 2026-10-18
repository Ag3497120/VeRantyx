cbuffer Tint : register(b0) { float4 tint; };
float4 main(float4 pos : SV_Position, float4 color : COLOR) : SV_Target { return color * tint; }
