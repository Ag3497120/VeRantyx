// Reads through views: one slice and mip of a texture array as sRGB, a typed buffer view; writes a typed buffer
// view and one mip of a texture.
Texture2DArray<float4> slice : register(t0);
Buffer<float4> values : register(t1);
RWBuffer<uint> results : register(u0);
RWTexture2D<float4> level1 : register(u1);
[numthreads(1, 1, 1)]
void main() {
    float4 a = slice.Load(int4(0, 0, 0, 0));
    uint w, h, elements, levels;
    slice.GetDimensions(0, w, h, elements, levels);
    float4 b = values[2];
    results[0] = (uint)(a.r * 255 + 0.5);
    results[1] = (uint)(a.g * 255 + 0.5);
    results[2] = w;
    results[3] = levels * 16 + elements;
    results[4] = (uint)b.x;
    results[5] = (uint)b.w;
    uint lw, lh;
    level1.GetDimensions(lw, lh);
    results[6] = lw;
    level1[uint2(1, 1)] = float4(1, 0.5, 0, 1);
}
