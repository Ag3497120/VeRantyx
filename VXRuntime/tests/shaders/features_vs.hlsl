// Translator coverage for vertex shaders (compiled, translated and compiled by Metal; not run): indexed
// constant buffers and inputs, an indexable temporary array, clip and cull distances, integer attributes.
cbuffer C : register(b0) { float4x4 bones[64]; float4 table[16]; };
Buffer<float4> extra : register(t0);
struct In { float3 pos : POSITION; uint4 bi : BLENDINDICES; float4 bw : BLENDWEIGHT; uint vid : SV_VertexID; uint iid : SV_InstanceID; };
struct Out { float4 pos : SV_Position; float4 c : COLOR; float clip : SV_ClipDistance0; float cull : SV_CullDistance0; };
Out main(In i) {
    float4 p = 0;
    [unroll] for (int b = 0; b < 4; b++) p += mul(bones[i.bi[b]], float4(i.pos, 1)) * i.bw[b];
    float local[8];
    for (int j = 0; j < 8; j++) local[j] = table[(j + i.vid) & 15].x;
    Out o;
    o.pos = p;
    o.c = float4(local[i.iid & 7], local[(i.iid + 1) & 7], 0, 1) + extra[i.vid];
    o.clip = p.x;
    o.cull = p.y;
    return o;
}
