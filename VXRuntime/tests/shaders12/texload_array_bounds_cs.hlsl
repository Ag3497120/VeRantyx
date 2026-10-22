Texture2DArray<uint> source_texture : register(t0);
RWStructuredBuffer<uint> results : register(u0);

[numthreads(1, 1, 1)]
void main() {
    results[0] = source_texture.Load(int4(1, 1, 0, 0));
    results[1] = source_texture.Load(int4(2, 0, 0, 0));
    results[2] = source_texture.Load(int4(0, 2, 0, 0));
    results[3] = source_texture.Load(int4(-1, 0, 0, 0));
    results[4] = source_texture.Load(int4(0, -1, 0, 0));
    results[5] = source_texture.Load(int4(0, 0, 2, 0));
    results[6] = source_texture.Load(int4(0, 0, -1, 0));
    results[7] = source_texture.Load(int4(0, 0, 1, 0), int2(1, 0));
    results[8] = source_texture.Load(int4(0, 0, 0, 1), int2(-1, 0));
}
