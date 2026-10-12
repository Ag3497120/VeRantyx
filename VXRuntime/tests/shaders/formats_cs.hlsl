// Reads block-compressed, 3D and 1D textures.
Texture2D<float4> bc1 : register(t0);
Texture2D<float4> bc3 : register(t1);
Texture3D<float> volume : register(t2);
Texture1D<float> row : register(t3);
RWStructuredBuffer<float4> results : register(u0);
[numthreads(1, 1, 1)]
void main() {
    results[0] = bc1.Load(int3(2, 1, 0));
    results[1] = bc3.Load(int3(5, 6, 0));
    results[2] = float4(volume.Load(int4(1, 2, 3, 0)), row.Load(int2(5, 0)), 0, 0);
    uint w, h, d;
    volume.GetDimensions(w, h, d);
    results[3] = float4(w, h, d, 0);
}
