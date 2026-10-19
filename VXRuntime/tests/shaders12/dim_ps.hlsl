Texture2D<float4> tex : register(t0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    uint width, height, levels;
    tex.GetDimensions(0, width, height, levels);
    return float4((float)width * 0.25f, (float)height * 0.25f, (float)levels * 0.25f, 1.0f);
}
