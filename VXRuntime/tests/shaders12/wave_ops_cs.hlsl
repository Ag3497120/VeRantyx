RWStructuredBuffer<uint> results : register(u0);

[numthreads(32, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint lane = WaveGetLaneIndex();
    uint lanes = WaveGetLaneCount();
    uint value = lane + 1u;
    int signed_value = int(lane) - 16;
    float float_value = float(value);
    bool even_lane = (lane & 1u) == 0u;
    uint bits = 0xf0u | lane;
    uint4 ballot = WaveActiveBallot(even_lane);
    uint4 matches = WaveMatch(lane & 3u);
    uint base = tid.x * 33u;

    results[base + 0u] = lanes;
    results[base + 1u] = lane;
    results[base + 2u] = WavePrefixSum(value);
    results[base + 3u] = WavePrefixCountBits(even_lane);
    results[base + 4u] = WaveActiveSum(value);
    results[base + 5u] = WaveActiveProduct(2u);
    results[base + 6u] = WaveActiveMin(value);
    results[base + 7u] = WaveActiveMax(value);
    results[base + 8u] = WaveActiveAllTrue(lane < lanes) ? 1u : 0u;
    results[base + 9u] = WaveActiveAnyTrue(lane == 17u) ? 1u : 0u;
    results[base + 10u] = WaveActiveAllEqual(7u) ? 1u : 0u;
    results[base + 11u] = WaveActiveAllEqual(lane) ? 1u : 0u;
    results[base + 12u] = ballot.x;
    results[base + 13u] = WaveActiveCountBits(even_lane);
    results[base + 14u] = WaveReadLaneAt(value, 3u);
    results[base + 15u] = WaveReadLaneFirst(value);
    results[base + 16u] = WaveIsFirstLane() ? 1u : 0u;
    results[base + 17u] = WaveActiveBitAnd(bits);
    results[base + 18u] = WaveActiveBitOr(bits);
    results[base + 19u] = WaveActiveBitXor(bits);
    results[base + 20u] = matches.x;
    results[base + 21u] = WaveMultiPrefixSum(value, matches);
    results[base + 22u] = WaveMultiPrefixProduct(2u, matches);
    results[base + 23u] = WaveMultiPrefixCountBits(even_lane, matches);
    results[base + 24u] = WaveMultiPrefixBitAnd(bits, matches);
    results[base + 25u] = WaveMultiPrefixBitOr(bits, matches);
    results[base + 26u] = WaveMultiPrefixBitXor(bits, matches);
    results[base + 27u] = WavePrefixProduct(2u);
    results[base + 28u] = WaveActiveBallot(true).x;
    results[base + 29u] = asuint(WaveActiveMin(signed_value));
    results[base + 30u] = asuint(WaveActiveMax(signed_value));
    results[base + 31u] = asuint(WaveMultiPrefixSum(float_value, matches));
    results[base + 32u] = asuint(WaveMultiPrefixProduct(2.0f, matches));
}
