// Arithmetic, integer and control-flow instructions on constant-buffer inputs, so nothing folds away.
cbuffer Inputs : register(b0) { float4 a; float4 b; int4 n; };
float4 main(float4 pos : SV_Position) : SV_Target {
    float r0 = dot(a.xyz, b.xyz);
    float r1 = rsqrt(a.w) + sqrt(b.w);
    float r2 = max(a.x, b.y) - min(a.z, b.x) + frac(b.z) + abs(-a.y);
    int i = n.x * n.y + (n.z << 2) - (n.w >> 1);
    uint u = (uint)n.x ^ 0xffu;
    float acc = 0;
    for (int k = 0; k < n.x; k++) { if (k == 2) continue; acc += k; }
    float r3 = (float)i + (float)u + acc + (a.x > b.x ? 100 : 200) + exp2(b.y) + log2(a.w);
    return float4(r0, r1, r2, r3);
}
