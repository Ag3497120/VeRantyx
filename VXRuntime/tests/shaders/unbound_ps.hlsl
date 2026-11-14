// Resources nothing is bound to read 0 in Direct3D: shader resource views, and constant buffers - unbound, or bound but
// smaller than the declaration (b1 declares 300 float4s, 2 are bound: b[0] = 7, the rest is 0). One pixel per case.
cbuffer Unbound : register(b0) { float4 a[2]; };
cbuffer Small : register(b1) { float4 b[300]; };
Texture2D t2d : register(t0);
TextureCube tcube : register(t1);
Texture3D t3d : register(t2);
Texture2DMS<float4> tms : register(t3);
Buffer<float4> tbuf : register(t4);
Texture2DArray<float4> tarr : register(t5);
SamplerState smp : register(s0);
float4 main(float4 pos : SV_Position) : SV_Target {
    uint x = (uint)pos.x;
    if (x == 0) return a[0] + a[1];
    if (x == 1) return b[299];
    if (x == 2) return b[0] + b[1] + b[150];
    if (x == 3) return t2d.SampleLevel(smp, float2(0.5, 0.5), 0) + t2d.Load(int3(0, 0, 0));
    if (x == 4) return tcube.SampleLevel(smp, float3(0, 0, 1), 0);
    if (x == 5) return t3d.Load(int4(0, 0, 0, 0));
    if (x == 6) return tms.Load(int2(0, 0), 0);
    if (x == 7) return tbuf.Load(0);
    if (x == 8) return tarr.SampleLevel(smp, float3(0.5, 0.5, 0), 0);
    return float4(0, 0, 0, 0);
}
