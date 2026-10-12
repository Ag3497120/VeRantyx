// A texture sample tinted by a constant buffer.
Texture2D tex : register(t0);
SamplerState smp : register(s0);
cbuffer Tint : register(b0) { float4 tint; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return tex.Sample(smp, uv) * tint; }
