RWBuffer<uint> typed_uint : register(u0);
RWBuffer<int> typed_sint : register(u1);
RWTexture2D<uint> texture_uint : register(u2);
RWTexture2D<int> texture_sint : register(u3);
RWByteAddressBuffer raw_uint : register(u4);
RWStructuredBuffer<uint> structured_uint : register(u5);
RWStructuredBuffer<uint> results : register(u6);
RWTexture1D<uint> texture_1d_uint : register(u7);
RWTexture1DArray<uint> texture_1d_array_uint : register(u8);
RWTexture2DArray<uint> texture_2d_array_uint : register(u9);
RWTexture3D<uint> texture_3d_uint : register(u10);
RWTexture3D<uint> texture_3d_full : register(u11);

[numthreads(1, 1, 1)]
void main() {
    uint initialized_typed_uint;
    InterlockedExchange(typed_uint[0], 0u, initialized_typed_uint);
    uint old_typed_uint_max, old_typed_uint_min, old_typed_uint_exchange;
    InterlockedMax(typed_uint[0], 100u, old_typed_uint_max);
    InterlockedMin(typed_uint[0], 20u, old_typed_uint_min);
    InterlockedCompareExchange(typed_uint[0], 20u, 21u, old_typed_uint_exchange);
    results[0] = old_typed_uint_max;
    results[1] = old_typed_uint_min;
    results[2] = old_typed_uint_exchange;
    results[3] = typed_uint[0];

    int initialized_typed_sint;
    InterlockedExchange(typed_sint[0], 0, initialized_typed_sint);
    int old_typed_sint_min, old_typed_sint_max, old_typed_sint_exchange;
    InterlockedMin(typed_sint[0], -50, old_typed_sint_min);
    InterlockedMax(typed_sint[0], -10, old_typed_sint_max);
    InterlockedCompareExchange(typed_sint[0], -10, -1, old_typed_sint_exchange);
    results[4] = asuint(old_typed_sint_min);
    results[5] = asuint(old_typed_sint_max);
    results[6] = asuint(old_typed_sint_exchange);
    results[7] = asuint(typed_sint[0]);

    uint initialized_texture_uint;
    InterlockedExchange(texture_uint[uint2(0, 0)], 0u, initialized_texture_uint);
    uint old_texture_uint_max, old_texture_uint_min, old_texture_uint_exchange;
    InterlockedMax(texture_uint[uint2(0, 0)], 100u, old_texture_uint_max);
    InterlockedMin(texture_uint[uint2(0, 0)], 20u, old_texture_uint_min);
    InterlockedCompareExchange(texture_uint[uint2(0, 0)], 20u, 21u, old_texture_uint_exchange);
    results[8] = old_texture_uint_max;
    results[9] = old_texture_uint_min;
    results[10] = old_texture_uint_exchange;
    results[11] = texture_uint[uint2(0, 0)];

    int initialized_texture_sint;
    InterlockedExchange(texture_sint[uint2(0, 0)], 0, initialized_texture_sint);
    int old_texture_sint_min, old_texture_sint_max, old_texture_sint_exchange;
    InterlockedMin(texture_sint[uint2(0, 0)], -50, old_texture_sint_min);
    InterlockedMax(texture_sint[uint2(0, 0)], -10, old_texture_sint_max);
    InterlockedCompareExchange(texture_sint[uint2(0, 0)], -10, -1, old_texture_sint_exchange);
    results[12] = asuint(old_texture_sint_min);
    results[13] = asuint(old_texture_sint_max);
    results[14] = asuint(old_texture_sint_exchange);
    results[15] = asuint(texture_sint[uint2(0, 0)]);

    uint initialized_raw;
    raw_uint.InterlockedExchange(0, 0u, initialized_raw);
    uint old_raw_max, old_raw_min, old_raw_exchange;
    raw_uint.InterlockedMax(0, 100u, old_raw_max);
    raw_uint.InterlockedMin(0, 20u, old_raw_min);
    raw_uint.InterlockedCompareExchange(0, 20u, 21u, old_raw_exchange);
    results[16] = old_raw_max;
    results[17] = old_raw_min;
    results[18] = old_raw_exchange;
    results[19] = raw_uint.Load(0);

    uint initialized_structured;
    InterlockedExchange(structured_uint[0], 0u, initialized_structured);
    uint old_structured_add, old_structured_exchange;
    InterlockedAdd(structured_uint[0], 5u, old_structured_add);
    InterlockedCompareExchange(structured_uint[0], 5u, 7u, old_structured_exchange);
    results[20] = old_structured_add;
    results[21] = old_structured_exchange;
    results[22] = structured_uint[0];

    uint initialized_1d;
    InterlockedExchange(texture_1d_uint[1], 0u, initialized_1d);
    uint old_1d_add, old_1d_exchange;
    InterlockedAdd(texture_1d_uint[1], 5u, old_1d_add);
    InterlockedCompareExchange(texture_1d_uint[1], 5u, 7u, old_1d_exchange);
    results[23] = old_1d_add;
    results[24] = old_1d_exchange;
    results[25] = texture_1d_uint[1];

    uint initialized_1d_array;
    InterlockedExchange(texture_1d_array_uint[int2(1, 1)], 0u, initialized_1d_array);
    uint old_1d_array_add, old_1d_array_exchange;
    InterlockedAdd(texture_1d_array_uint[int2(1, 1)], 5u, old_1d_array_add);
    InterlockedCompareExchange(texture_1d_array_uint[int2(1, 1)], 5u, 7u, old_1d_array_exchange);
    results[26] = old_1d_array_add;
    results[27] = old_1d_array_exchange;
    results[28] = texture_1d_array_uint[int2(1, 1)];

    uint initialized_2d_array;
    InterlockedExchange(texture_2d_array_uint[uint3(1, 1, 1)], 0u, initialized_2d_array);
    uint old_2d_array_add, old_2d_array_exchange;
    InterlockedAdd(texture_2d_array_uint[uint3(1, 1, 1)], 5u, old_2d_array_add);
    InterlockedCompareExchange(texture_2d_array_uint[uint3(1, 1, 1)], 5u, 7u, old_2d_array_exchange);
    results[29] = old_2d_array_add;
    results[30] = old_2d_array_exchange;
    results[31] = texture_2d_array_uint[uint3(1, 1, 1)];

    uint initialized_3d_full_z1, initialized_3d_full_z2, initialized_3d_full_z3;
    InterlockedExchange(texture_3d_full[uint3(1, 1, 1)], 0u, initialized_3d_full_z1);
    InterlockedExchange(texture_3d_full[uint3(1, 1, 2)], 0u, initialized_3d_full_z2);
    InterlockedExchange(texture_3d_full[uint3(0, 0, 1)], 0u, initialized_3d_full_z1);
    InterlockedExchange(texture_3d_full[uint3(0, 0, 3)], 0u, initialized_3d_full_z3);
    texture_3d_uint[uint3(0, 0, 0)] = 11u;
    texture_3d_uint[uint3(0, 0, 2)] = 13u;
    DeviceMemoryBarrier();
    results[38] = texture_3d_uint[uint3(0, 0, 2)];
    results[39] = texture_3d_full[uint3(0, 0, 1)];
    results[40] = texture_3d_full[uint3(0, 0, 3)];
    uint initialized_3d;
    InterlockedExchange(texture_3d_uint[uint3(1, 1, 1)], 0u, initialized_3d);
    uint old_3d_add, old_3d_exchange;
    InterlockedAdd(texture_3d_uint[uint3(1, 1, 1)], 5u, old_3d_add);
    InterlockedCompareExchange(texture_3d_uint[uint3(1, 1, 1)], 5u, 7u, old_3d_exchange);
    results[32] = old_3d_add;
    results[33] = old_3d_exchange;
    results[34] = texture_3d_uint[uint3(1, 1, 1)];
    uint texture_3d_width, texture_3d_height, texture_3d_depth;
    texture_3d_uint.GetDimensions(texture_3d_width, texture_3d_height, texture_3d_depth);
    results[35] = texture_3d_depth;
    results[36] = texture_3d_full[uint3(1, 1, 2)];
    results[37] = texture_3d_full[uint3(1, 1, 1)];
}
