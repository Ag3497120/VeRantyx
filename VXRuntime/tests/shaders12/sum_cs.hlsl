RWStructuredBuffer<uint> outBuf : register(u0);
ByteAddressBuffer inBuf : register(t0);
RWTexture2D<float4> img : register(u1);
groupshared uint shared_sum[64];
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    shared_sum[gi] = inBuf.Load(tid.x * 4);
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 32; s > 0; s >>= 1) {
        if (gi < s) shared_sum[gi] += shared_sum[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) { uint old; InterlockedAdd(outBuf[0], shared_sum[0], old); }
    img[tid.xy] = float4(WaveActiveSum(gi), WaveGetLaneIndex(), 0, 1);
    switch (gi & 3) { case 0: outBuf[gi + 1] = 1; break; case 1: outBuf[gi + 1] = asuint(sqrt(float(gi))); break; default: outBuf[gi + 1] = countbits(gi); }
}
