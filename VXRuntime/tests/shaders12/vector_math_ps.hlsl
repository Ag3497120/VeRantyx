float4 main(float4 a : TEXCOORD0, float4 b : TEXCOORD1) : SV_Target0 {
    return a.yxwz + b.zyxw;
}
