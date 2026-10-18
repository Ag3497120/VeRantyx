struct Material { uint tex; float4 tint; };
StructuredBuffer<Material> materials : register(t0, space1);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0, nointerpolation uint mat : MAT) : SV_Target {
    Material m = materials[mat];
    Texture2D<float4> t = ResourceDescriptorHeap[NonUniformResourceIndex(m.tex)];
    float4 c = t.SampleLevel(samp, uv, 0) * m.tint;
    min16float h = (min16float)c.r;
    return float4(c.rgb * ddx(uv.x), (float)h);
}
