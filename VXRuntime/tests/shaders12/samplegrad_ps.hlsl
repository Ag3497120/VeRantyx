Texture1D<float4> Tex1D : register(t0);
Texture1DArray<float4> Tex1DArray : register(t1);
Texture2D<float4> Tex2D : register(t2);
Texture2DArray<float4> Tex2DArray : register(t3);
Texture3D<float4> Tex3D : register(t4);
TextureCube<float4> TexCube : register(t5);
TextureCubeArray<float4> TexCubeArray : register(t6);
SamplerState Samp : register(s0);

float4 main(float4 coord : TEXCOORD0) : SV_Target {
    float2 dx2 = ddx(coord.xy);
    float2 dy2 = ddy(coord.xy);
    float3 dx3 = ddx(coord.xyz);
    float3 dy3 = ddy(coord.xyz);
    float result = Tex1D.SampleGrad(Samp, coord.x, ddx(coord.x), ddy(coord.x)).x;
    result += Tex1DArray.SampleGrad(Samp, coord.xy, ddx(coord.x), ddy(coord.x)).x;
    result += Tex2D.SampleGrad(Samp, coord.xy, dx2, dy2).x;
    result += Tex2D.SampleGrad(Samp, coord.xy, dx2, dy2, int2(0, 0), 2.0f).y;
    result += Tex2DArray.SampleGrad(Samp, coord.xyz, dx2, dy2).x;
    result += Tex3D.SampleGrad(Samp, coord.xyz, dx3, dy3).x;
    result += TexCube.SampleGrad(Samp, coord.xyz, dx3, dy3).x;
    result += TexCubeArray.SampleGrad(Samp, coord, dx3, dy3).x;
    return result.xxxx;
}
