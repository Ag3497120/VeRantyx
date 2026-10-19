Texture1DArray<float4> tex : register(t0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    uint width, elements, levels;
    tex.GetDimensions(0, width, elements, levels);
    return float4((float)width * 0.25f, (float)elements * 0.5f, (float)levels, 1.0f);
}
