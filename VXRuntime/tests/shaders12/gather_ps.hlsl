Texture2D<float4> colorTexture : register(t0);
SamplerState pointSampler : register(s0);

struct PixelInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(PixelInput input) : SV_Target {
    return colorTexture.GatherGreen(pointSampler, input.uv, int2(1, 0));
}
