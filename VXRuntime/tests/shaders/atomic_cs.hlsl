// Atomics on a byte-address buffer and stores to a writable texture.
RWByteAddressBuffer counters : register(u0);
RWTexture2D<float4> image : register(u1);
[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    image[tid.xy] = float4(tid.x / 15.0, tid.y / 15.0, 0, 1);
    counters.InterlockedAdd(0, 1);
    counters.InterlockedMax(4, tid.x * 100 + tid.y);
    uint before;
    counters.InterlockedOr(8, 1u << (tid.x & 31), before);
}
