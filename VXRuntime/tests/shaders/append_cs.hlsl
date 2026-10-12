// Consumes 128 values, appends ten times each odd one, counts each value's low two bits in an R32_UINT texture
// with atomics.
ConsumeStructuredBuffer<uint> input : register(u0);
AppendStructuredBuffer<uint> output : register(u1);
RWTexture2D<uint> histogram : register(u2);
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint v = input.Consume();
    if (v & 1) output.Append(v * 10);
    InterlockedAdd(histogram[uint2(v % 4, 0)], 1);
}
