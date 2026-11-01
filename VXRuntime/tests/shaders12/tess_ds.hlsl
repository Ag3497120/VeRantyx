// The point of the patch at the domain location (barycentric), colored by the patch's tint and the location.
struct CP { float4 pos : CPPOS; float4 color : COLOR; };
struct PC { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; float4 tint : TINT; };
struct DSOut { float4 pos : SV_Position; float4 color : COLOR; };
[domain("tri")]
DSOut main(PC pc, float3 uvw : SV_DomainLocation, const OutputPatch<CP, 3> p) {
    DSOut o;
    o.pos = p[0].pos * uvw.x + p[1].pos * uvw.y + p[2].pos * uvw.z;
    o.color = pc.tint;
    return o;
}
