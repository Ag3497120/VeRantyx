// Sums groups of 64 values in thread-group shared memory: structured buffers, a constant buffer, barriers.
StructuredBuffer<float> input : register(t0);
RWStructuredBuffer<float> output : register(u0);
cbuffer Params : register(b0) { float scale; uint count; };
groupshared float partial[64];
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint li : SV_GroupIndex, uint3 gid : SV_GroupID) {
    partial[li] = tid.x < count ? input[tid.x] * scale : 0;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 32; s > 0; s >>= 1) {
        if (li < s) partial[li] += partial[li + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (li == 0) output[gid.x] = partial[0];
}
