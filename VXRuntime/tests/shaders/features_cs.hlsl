// Translator coverage for compute (compiled, translated and compiled by Metal; not run): append and consume
// buffers, structured buffer counters, atomics on typed UAVs, bit operations.
AppendStructuredBuffer<uint> app : register(u0);
ConsumeStructuredBuffer<uint> con : register(u1);
RWTexture2D<uint> img : register(u2);
RWBuffer<uint> buf : register(u3);
RWByteAddressBuffer raw : register(u4);
RWStructuredBuffer<uint2> counted : register(u5);
RWStructuredBuffer<uint> drained : register(u6);
groupshared uint g[64];
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint li : SV_GroupIndex) {
    uint v = con.Consume();
    app.Append(v * 2);
    uint old;
    InterlockedAdd(img[id.xy], 1, old);
    InterlockedMax(buf[id.x], v);
    uint prev;
    InterlockedCompareExchange(buf[0], 0, v, prev);
    uint slot = counted.IncrementCounter();
    counted[slot] = uint2(v, old);
    g[li] = v;
    GroupMemoryBarrierWithGroupSync();
    raw.Store(li * 4, g[63 - li] + old + prev + countbits(v) + reversebits(v) + firstbithigh(v));
    if (li == 0) drained[drained.DecrementCounter()] = v;
}
