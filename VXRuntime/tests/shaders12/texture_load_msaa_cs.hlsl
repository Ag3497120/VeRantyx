Texture2DMS<float> input_texture : register(t0);
RWBuffer<uint> output_values : register(u0);

[numthreads(1, 1, 1)]
void main() {
    float value = input_texture.Load(int2(2, 1), 2);
    output_values[0] = asuint(value);
}
