// Constant buffers and indexable temps read outside: 0 in Direct3D. The constant buffer is declared with 32
// float4s but the one bound holds 2 (a dynamic buffer: a slice of a chunk, other constants beyond it).
cbuffer Small : register(b0) { float4 data[32]; };
cbuffer Index : register(b1) { uint4 idx; uint4 far; };   // 1, 20, 6, 2; 7
RWBuffer<uint> results : register(u0);
[numthreads(1, 1, 1)]
void main() {
    results[0] = asuint(data[idx.x].x);                 // inside: 7
    results[1] = asuint(data[idx.y].x) + 1;             // beyond the bound buffer: 1
    float4 a[4];
    for (uint i = 0; i < 4; i++) a[i] = float4(i + 10, 0, 0, 0);
    a[idx.z] = float4(99, 0, 0, 0);                     // dropped
    results[2] = (uint)a[idx.w].x;                      // 12
    results[3] = (uint)a[far.x].x + 1;                  // outside: 1 (an index the compiler cannot know)
    results[4] = asuint(data[20].x) + 1;                // a fixed index beyond the bound buffer: 1
}
