Texture2D<float> depthMap : register(t0);
Texture2DArray<float> depthArray : register(t1);
SamplerComparisonState shadowSampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float passes = depthMap.SampleCmp(shadowSampler, uv, 0.25f);
    float fails = depthMap.SampleCmpLevelZero(shadowSampler, uv, 0.9f);
    float clamped = depthMap.SampleCmp(shadowSampler, uv, 0.25f, int2(0, 0), 0.0f);
    float arrayPass = depthArray.SampleCmp(shadowSampler, float3(uv, 1.0f), 0.25f);
    return float4(passes, fails, clamped, arrayPass);
}
