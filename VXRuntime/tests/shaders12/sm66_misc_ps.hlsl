// Pixel-shader intrinsics a renderer reaches for besides sampling: CalculateLevelOfDetail(Unclamped), SV_SampleIndex,
// SV_Coverage, IsHelperLane (SM 6.6), pack/unpack of 8-bit vectors and dot4add (SM 6.6/6.4).
Texture2D<float4> tex : register(t0);
SamplerState smp : register(s0);
cbuffer c : register(b0) { uint packed_a; uint packed_b; };

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0, uint sample_index : SV_SampleIndex,
            uint coverage : SV_Coverage, out uint mask : SV_Target1) : SV_Target0 {
    float lod = tex.CalculateLevelOfDetail(smp, uv);
    float raw_lod = tex.CalculateLevelOfDetailUnclamped(smp, uv);
    uint helper = IsHelperLane() ? 1u : 0u;
    int4 signed_bytes = unpack_s8s32((int8_t4_packed)packed_a);
    uint4 unsigned_bytes = unpack_u8u32((uint8_t4_packed)packed_b);
    uint repacked = (uint)pack_clamp_u8(int4(signed_bytes.x, 300, -4, 7));
    int d = dot4add_i8packed(packed_a, packed_b, (int)sample_index);
    uint e = dot4add_u8packed(packed_b, packed_a, coverage);
    float h = dot2add(half2(uv), half2(pos.xy), 1.0f);
    mask = coverage ^ (sample_index << 8) ^ helper ^ repacked ^ (uint)d ^ e ^ unsigned_bytes.w;
    return float4(lod, raw_lod, (float)signed_bytes.y + h, (float)unsigned_bytes.z);
}
