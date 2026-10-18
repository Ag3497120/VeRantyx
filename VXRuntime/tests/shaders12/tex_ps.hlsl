Texture2D<float4> tex : register(t0);
SamplerState samp : register(s0);
cbuffer Params : register(b0) { float4 scale; int count; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 sum = 0;
    [loop] for (int i = 0; i < count; i++) {
        float4 c = tex.Sample(samp, uv + float2(i * 0.01, 0));
        if (c.a < 0.5) continue;
        sum += c * scale;
    }
    if (sum.r > 1) discard;
    return sum;
}
