/* SPDX-License-Identifier: MIT
 * FOV crop/foveated transport — original implementation, 2026.
 * Separable rectangular foveated transport; see ../PROVENANCE.md.
 * All coordinates below denote PIXEL EDGES; samples use pixel centres.
 */
#include <metal_stdlib>
using namespace metal;

struct ShaderAxis {
    float sourcePixels, packedPixels, sourceBegin, sourceEnd;
    float packedBegin, packedEnd, reserved0, reserved1;
};
struct ShaderEye { ShaderAxis x, y; };
struct ShaderParams { ShaderEye eyes[2]; };
static_assert(sizeof(ShaderAxis) == 32, "axis ABI");
static_assert(sizeof(ShaderEye) == 64, "eye ABI");
static_assert(sizeof(ShaderParams) == 128, "stereo ABI");

// Return source coordinate and local source-pixels / packed-pixel footprint.
static inline float2 unpack_axis(float q, constant ShaderAxis& a) {
    if (q < a.packedBegin) {
        float k = a.sourceBegin / a.packedBegin;
        return float2(q * k, k);
    }
    if (q < a.packedEnd) return float2(a.sourceBegin + q - a.packedBegin, 1.0f);
    float k = (a.sourcePixels - a.sourceEnd) / (a.packedPixels - a.packedEnd);
    return float2(a.sourceEnd + (q - a.packedEnd) * k, k);
}
static inline float4 eye_sample(texture2d<float, access::sample> src,
                               float2 position, uint eye, float2 extent) {
    constexpr sampler s(coord::pixel, filter::linear, address::clamp_to_edge);
    // No interpolation from the other eye at the SBS seam.
    position = clamp(position, float2(0.5f), extent - 0.5f);
    position.x += float(eye) * extent.x;
    return src.sample(s, position, level(0));
}

// A modest edge prefilter: 2x2 maximum for the initial ratio-2 profile.
// Central pixels take exactly ONE bilinear sample at the same pixel centre.
// The shader does not change colour transfer, alpha or any crop/FOV geometry.
kernel void mvr_foveated_pack(texture2d<float, access::sample> src [[texture(0)]],
                              texture2d<half, access::write> dst [[texture(1)]],
                              constant ShaderParams& p [[buffer(0)]],
                              uint2 tid [[thread_position_in_grid]]) {
    if (tid.x >= dst.get_width() || tid.y >= dst.get_height()) return;
    uint eyeWidth = dst.get_width() / 2;
    if (eyeWidth == 0 || (dst.get_width() & 1u)) return;
    uint eye = min(tid.x / eyeWidth, 1u);
    constant ShaderEye& e = p.eyes[eye];
    // Geometry must be validated by the host before dispatch.
    float2 x = unpack_axis(float(tid.x % eyeWidth) + 0.5f, e.x);
    float2 y = unpack_axis(float(tid.y) + 0.5f, e.y);
    float2 pos(x.x, y.x), footprint(x.y, y.y);
    float2 ext(e.x.sourcePixels, e.y.sourcePixels);
    bool filterX = footprint.x > 1.00001f;
    bool filterY = footprint.y > 1.00001f;
    float4 c(0.0f);
    uint nx = filterX ? 2u : 1u, ny = filterY ? 2u : 1u;
    for (uint j = 0; j < ny; ++j) for (uint i = 0; i < nx; ++i) {
        float2 off(0.0f);
        if (filterX) off.x = (float(i) - 0.5f) * footprint.x * 0.5f;
        if (filterY) off.y = (float(j) - 0.5f) * footprint.y * 0.5f;
        c += eye_sample(src, pos + off, eye, ext);
    }
    dst.write(half4(c / float(nx * ny)), tid);
}
