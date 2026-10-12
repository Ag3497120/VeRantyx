// Translator coverage (compiled, translated and compiled by Metal; not run): gathers, level of detail, sample
// positions and counts, attribute evaluation, msad4, doubles, texel offsets, subroutine calls.
Texture2D<float4> tex : register(t0);
Texture2DMS<float4> ms : register(t1);
TextureCube<float4> cube : register(t2);
Texture2DArray<float4> arr : register(t3);
SamplerState s : register(s0);
SamplerComparisonState sc : register(s1);
cbuffer C : register(b0) { float4 k; int2 off; uint idx; uint mode; };
struct In {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 n : NORMAL;
    centroid float4 c : COLOR;
    nointerpolation uint id : ID;
};
float4 main(In i, uint sid : SV_SampleIndex) : SV_Target {
    float4 r = tex.Gather(s, i.uv);
    r += tex.GatherGreen(s, i.uv, int2(1, 0));
    r += tex.GatherRed(s, i.uv, off);
    r += tex.GatherCmp(sc, i.uv, k.x);
    r += tex.GatherCmpRed(sc, i.uv, k.x, off);
    r.x += tex.CalculateLevelOfDetail(s, i.uv);
    r.y += tex.CalculateLevelOfDetailUnclamped(s, i.uv);
    r.xy += ms.GetSamplePosition(sid);
    uint w, h, n;
    ms.GetDimensions(w, h, n);
    r.z += n + w;
    r.w += GetRenderTargetSampleCount();
    r.xy += GetRenderTargetSamplePosition(sid);
    r += EvaluateAttributeAtSample(i.c, sid);
    r += EvaluateAttributeCentroid(i.c);
    r += EvaluateAttributeSnapped(i.c, off);
    r.x += msad4(idx, uint2(idx * 3, idx * 5), uint4(1, 2, 3, 4)).x;
    r += cube.Gather(s, i.n);
    r += arr.Gather(s, float3(i.uv, 1));
    double d = (double)k.x * (double)k.y + 1.0;
    r.x += (float)(d / (double)k.z);
    r.x += tex.Load(int3(off, 0), int2(1, 1)).x;
    r += tex.SampleLevel(s, i.uv, 0, int2(-1, 1));
    r += ms.Load(int2(off), sid);
    [call] switch (mode) {
    case 0: r *= 2; break;
    case 1: r += k; break;
    default: r = sqrt(abs(r)); break;
    }
    return r;
}
