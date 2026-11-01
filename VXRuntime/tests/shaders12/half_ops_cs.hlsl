// Native 16-bit types (SM 6.2, -enable-16bit-types): half and 16-bit integer arithmetic, conversions, 16-bit loads and
// stores of raw and structured buffers, packing through bit casts.
ByteAddressBuffer input : register(t0);
RWByteAddressBuffer raw_out : register(u0);
RWStructuredBuffer<uint> results : register(u1);
RWStructuredBuffer<float16_t> halves : register(u2);

[numthreads(8, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
    uint i = tid.x;
    float16_t a = input.Load<float16_t>(i * 2);            // 0.5 * (i + 1)
    float16_t b = (float16_t)1.25;
    float16_t2 v = float16_t2(a, b);
    float16_t s = dot(v, v) + mad(a, b, (float16_t)-0.5);
    int16_t si = (int16_t)i - 4;
    uint16_t ui = (uint16_t)(i * 5000u);                    // wraps at 65536
    uint16_t shifted = ui >> 3;
    int16_t prod = si * (int16_t)-3;
    uint base = i * 8;
    results[base + 0] = asuint((float)s);
    results[base + 1] = (uint)(int)prod;
    results[base + 2] = (uint)shifted;
    results[base + 3] = (uint)ui;
    results[base + 4] = (uint)asuint16(a);
    results[base + 5] = (uint)(int)(si < 0 ? max(si, (int16_t)-2) : min(si, (int16_t)2));
    results[base + 6] = asuint((float)min(a, b));
    results[base + 7] = (uint)f32tof16(float(a) * 3.0);
    raw_out.Store<uint16_t>(i * 2, ui);
    halves[i] = a * (float16_t)2.0;
}
