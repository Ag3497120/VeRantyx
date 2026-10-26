// Minimum precision (min16float, no -enable-16bit-types): the halves are kept as float; constant-buffer rows of four
// min16float are four 32-bit floats.
Texture2D<min16float4> tex : register(t0);
SamplerState smp : register(s0);
cbuffer c : register(b0) { min16float4 tint; float4 big; };
min16float4 main(float4 pos : SV_Position, min16float2 uv : TEXCOORD0) : SV_Target {
    min16float4 c = tex.Sample(smp, uv) * tint;
    return c + (min16float4)big;
}
