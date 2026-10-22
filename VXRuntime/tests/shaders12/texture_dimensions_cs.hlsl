Texture2D<float4> input_texture : register(t0);
RWBuffer<uint> output_values : register(u0);

[numthreads(1, 1, 1)]
void main() {
    uint width, height, mip_levels;
    input_texture.GetDimensions(0, width, height, mip_levels);
    output_values[0] = width;
    output_values[1] = height;
    output_values[2] = mip_levels;
}
