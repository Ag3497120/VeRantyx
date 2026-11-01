// A geometry shader making a square (two triangles, as one strip) around each point, its half size in cb0.x,
// and a second, cut-off strip giving a triangle in the top-left corner (so cuts are tested too).
cbuffer Size : register(b0) { float4 size; };
struct GSIn { float4 pos : SV_Position; float4 color : COLOR; };
struct GSOut { float4 pos : SV_Position; float4 color : COLOR; };
[maxvertexcount(7)]
void main(point GSIn p[1], inout TriangleStream<GSOut> s) {
    GSOut o;
    o.color = p[0].color;
    float2 c = p[0].pos.xy, h = size.xx;
    o.pos = float4(c + float2(-h.x,  h.y), 0, 1); s.Append(o);
    o.pos = float4(c + float2( h.x,  h.y), 0, 1); s.Append(o);
    o.pos = float4(c + float2(-h.x, -h.y), 0, 1); s.Append(o);
    o.pos = float4(c + float2( h.x, -h.y), 0, 1); s.Append(o);
    s.RestartStrip();
    o.color = float4(0, 0, 1, 1);
    o.pos = float4(-1, 1, 0, 1); s.Append(o);
    o.pos = float4(-0.8, 1, 0, 1); s.Append(o);
    o.pos = float4(-1, 0.8, 0, 1); s.Append(o);
}
