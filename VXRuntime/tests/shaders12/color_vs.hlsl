cbuffer Camera : register(b0) { float4x4 mvp; float3 light; };
struct VSIn { float3 pos : POSITION; float3 nrm : NORMAL; uint vid : SV_VertexID; };
struct VSOut { float4 pos : SV_Position; float4 color : COLOR; nointerpolation uint id : ID; };
VSOut main(VSIn v) {
    VSOut o;
    o.pos = mul(mvp, float4(v.pos, 1));
    float d = saturate(dot(normalize(v.nrm), normalize(light)));
    o.color = float4(d.xxx, 1) * (v.vid % 2 ? 0.5 : 1.0);
    o.id = v.vid;
    return o;
}
