// SampleCmpLevelZero outside a pixel shader (compute shadow filtering).
Texture2D<float> depth : register(t0);
SamplerComparisonState cmp : register(s0);
RWStructuredBuffer<float> results : register(u0);

[numthreads(4, 4, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    float2 uv = (float2(tid.xy) + 0.5) / 4.0;
    results[tid.y * 4 + tid.x] = depth.SampleCmpLevelZero(cmp, uv, 0.5);
}
