Texture2D<float> source_texture : register(t0);
RWStructuredBuffer<uint> results : register(u0);

[numthreads(1, 1, 1)]
void main() {
    results[0] = asuint(source_texture.Load(int3(0, 0, 0)));
    results[1] = asuint(source_texture.Load(int3(1, 0, 0)));
    results[2] = asuint(source_texture.Load(int3(2, 0, 0)));
    results[3] = asuint(source_texture.Load(int3(0, -1, 0)));
    results[4] = asuint(source_texture.Load(int3(0, 0, 1)));
    results[5] = asuint(source_texture.Load(int3(0, 1, 0), int2(1, 0)));
    results[6] = asuint(source_texture.Load(int3(0, 1, 0), int2(-1, 0)));
}
