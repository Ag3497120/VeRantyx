// Reads and writes outside views, which Direct3D defines: a load reads 0, a store or an atomic does nothing, a
// sample's array index is clamped to the layers there are. (id.x is 0: offsets the compiler cannot fold away.)
StructuredBuffer<uint> part : register(t0);          // elements 0..3 of a buffer of 8
ByteAddressBuffer bytes : register(t1);               // the first 16 bytes of a buffer of 32
Texture2D<float4> image : register(t2);               // 2x2, one mip, all 1
Buffer<float4> typed : register(t3);                  // 2 elements of a buffer of 4
Texture2DArray<float4> layers : register(t4);         // 2 layers: 0.25, 1
SamplerState nearest : register(s0);
RWStructuredBuffer<uint> rw : register(u0);           // elements 0..3 of a buffer of 8
RWBuffer<uint> results : register(u1);
RWTexture2D<float4> target : register(u2);            // 2x2
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint far = id.x + 5;
    results[0] = part[3];
    results[1] = part[far] + 1;                                          // 1: outside the view, inside the buffer
    results[2] = bytes.Load(12);
    results[3] = bytes.Load(16 + id.x * 4) + 1;
    results[4] = (uint)(image.Load(int3(1, 1, 0)).x * 100);
    results[5] = (uint)(image.Load(int3(2 + id.x, 0, 0)).x * 100) + 1;
    results[6] = (uint)(image.Load(int3(0, 0, 1 + id.x)).x * 100) + 1;   // a mip that does not exist
    results[7] = (uint)typed[2 + id.x].x + 1;
    results[8] = (uint)(layers.SampleLevel(nearest, float3(0.5, 0.5, 7.0 + id.x), 0).x * 100);    // 100: layer 1
    results[9] = (uint)(layers.SampleLevel(nearest, float3(0.5, 0.5, -3.0 - id.x), 0).x * 100);   // 25: layer 0
    results[10] = (uint)(layers.Load(int4(0, 0, 2 + id.x, 0)).x * 100) + 1;
    rw[far] = 99;
    rw[1] = 11;
    uint old;
    InterlockedAdd(rw[far + 1], 5, old);
    results[11] = old + 1;
    target[uint2(2 + id.x, 0)] = float4(1, 1, 1, 1);
    target[uint2(1, 1)] = float4(0, 1, 0, 1);
}
