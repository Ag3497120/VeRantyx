Texture2D<float> depthTexture : register(t0);
SamplerComparisonState comparisonSampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return depthTexture.GatherCmp(comparisonSampler, uv, 0.25f);
}
