// A triangle patch: the control points as they come, edge and inside factors from b0 (integer partitioning,
// clockwise triangles); the patch's color from its control points' average.
cbuffer Factors : register(b0) { float4 factors; };
struct CP { float4 pos : CPPOS; float4 color : COLOR; };
struct PC { float edges[3] : SV_TessFactor; float inside : SV_InsideTessFactor; float4 tint : TINT; };
PC patch_constants(InputPatch<CP, 3> p, uint id : SV_PrimitiveID) {
    PC o;
    o.edges[0] = factors.x; o.edges[1] = factors.y; o.edges[2] = factors.z; o.inside = factors.w;
    o.tint = (p[0].color + p[1].color + p[2].color) / 3.0;
    return o;
}
[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("patch_constants")]
CP main(InputPatch<CP, 3> p, uint i : SV_OutputControlPointID) { return p[i]; }
