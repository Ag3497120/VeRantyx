#include <metal_stdlib>
using namespace metal;

// Synthetic "heavy game" scene: a floor, world-locked pillars at 1-12 m (the
// reprojection reference) and objects orbiting the viewer (their motion shows
// the game rate). The fragment loop is the GPU-load knob.

struct SceneUniforms {
    float4x4 viewProj[2];
    float4 eyePos[2];
    float time;
    uint iterations;
    uint animate;
    uint pad;
};

struct SceneOut {
    float4 position [[position]];
    float3 world;
    float3 normal;
    float3 base;
    float kind;
};

constant uint kPillars = 48;
constant uint kOrbiters = 24;

static float hash31(float3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

static float noise3(float3 x) {
    float3 i = floor(x);
    float3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash31(i + float3(0,0,0)), hash31(i + float3(1,0,0)), f.x),
                   mix(hash31(i + float3(0,1,0)), hash31(i + float3(1,1,0)), f.x), f.y),
               mix(mix(hash31(i + float3(0,0,1)), hash31(i + float3(1,0,1)), f.x),
                   mix(hash31(i + float3(0,1,1)), hash31(i + float3(1,1,1)), f.x), f.y), f.z);
}

static float3 hue(float h) {
    return saturate(abs(fmod(h * 6.0 + float3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0);
}

vertex SceneOut sceneVertex(uint vid [[vertex_id]],
                            uint iid [[instance_id]],
                            ushort eye [[amplification_id]],
                            constant SceneUniforms &u [[buffer(0)]]) {
    // Unit cube, 36 vertices.
    const float3 corners[8] = {
        float3(-1,-1,-1), float3(1,-1,-1), float3(1,1,-1), float3(-1,1,-1),
        float3(-1,-1, 1), float3(1,-1, 1), float3(1,1, 1), float3(-1,1, 1)
    };
    const ushort idx[36] = {
        4,5,6, 4,6,7,   1,0,3, 1,3,2,   5,1,2, 5,2,6,
        0,4,7, 0,7,3,   7,6,2, 7,2,3,   0,1,5, 0,5,4
    };
    const float3 normals[6] = {
        float3(0,0,1), float3(0,0,-1), float3(1,0,0), float3(-1,0,0), float3(0,1,0), float3(0,-1,0)
    };
    float3 p = corners[idx[vid]];
    float3 n = normals[vid / 6];

    float3 scale, center, base;
    float angle = 0.0;
    float kind;
    float t = u.animate ? u.time : 0.0;
    if (iid == 0) {                       // floor
        scale = float3(20, 0.01, 20); center = float3(0, -0.01, 0); base = float3(0.35); kind = 0;
    } else if (iid <= kPillars) {          // world-locked pillars on rings
        uint k = iid - 1;
        uint ring = k / 8;
        const float radii[6] = { 1.0, 1.6, 2.6, 4.0, 7.0, 12.0 };
        float a = (float(k % 8) + 0.5 * float(ring & 1)) * (M_PI_F / 4.0);
        float r = radii[ring];
        scale = float3(0.06 + 0.02 * ring, 1.1 + 0.15 * ring, 0.06 + 0.02 * ring);
        center = float3(sin(a) * r, scale.y, -cos(a) * r);
        base = hue(float(k) / float(kPillars)) * 0.8 + 0.2;
        kind = 1;
    } else if (iid <= kPillars + kOrbiters) { // orbiters: 0.5 rad/s at 2.2 m = 1.1 m/s
        uint k = iid - 1 - kPillars;
        float a = float(k) * (2.0 * M_PI_F / float(kOrbiters)) + t * 0.5;
        scale = float3(0.09);
        center = float3(sin(a) * 2.2, 1.2 + 0.25 * sin(t * 1.3 + float(k)), -cos(a) * 2.2);
        angle = t * 1.5 + float(k);
        base = float3(1.0, 0.85, 0.2);
        kind = 2;
    } else {                               // clock hand 2.5 m ahead, 0.5 rev/s
        scale = float3(0.6, 0.025, 0.025);
        float a = t * M_PI_F;
        float3 local = p * scale + float3(0.6, 0, 0);
        float3 world = float3(local.x * cos(a) - local.y * sin(a), local.x * sin(a) + local.y * cos(a), local.z);
        SceneOut o;
        o.world = world + float3(0, 1.4, -2.5);
        o.normal = float3(0, 0, 1);
        o.base = float3(1.0, 0.2, 0.2);
        o.kind = 3;
        o.position = u.viewProj[eye] * float4(o.world, 1);
        return o;
    }
    float c = cos(angle), s = sin(angle);
    float3 lp = p * scale;
    lp = float3(lp.x * c - lp.z * s, lp.y, lp.x * s + lp.z * c);
    float3 ln = float3(n.x * c - n.z * s, n.y, n.x * s + n.z * c);
    SceneOut o;
    o.world = lp + center;
    o.normal = ln;
    o.base = base;
    o.kind = kind;
    o.position = u.viewProj[eye] * float4(o.world, 1);
    return o;
}

fragment float4 sceneFragment(SceneOut in [[stage_in]], constant SceneUniforms &u [[buffer(0)]]) {
    // Deliberately expensive procedural shading (the "game" GPU cost).
    float3 q = in.world * 3.0;
    float acc = 0.0, amp = 0.5;
    for (uint i = 0; i < u.iterations; ++i) {
        acc += amp * noise3(q);
        q = q * 1.9 + float3(1.7, 9.2, 4.1);
        amp *= 0.62;
        if (amp < 0.002) amp = 0.5;     // keep every iteration contributing work
    }
    float3 albedo = in.base * (0.65 + 0.5 * acc);
    if (in.kind == 0) {
        float2 g = abs(fract(in.world.xz) - 0.5);
        float line = step(0.485, max(g.x, g.y));
        albedo = mix(albedo, float3(0.9), line);
    }
    float3 light = normalize(float3(0.4, 1.0, 0.3));
    float diffuse = 0.35 + 0.65 * saturate(dot(normalize(in.normal), light));
    return float4(albedo * diffuse, 1.0);
}

// Fullscreen triangle helpers -------------------------------------------------

struct FullOut {
    float4 position [[position]];
    float2 uv;
    uint eye [[flat]];
};

vertex FullOut fullscreenVertex(uint vid [[vertex_id]], ushort eye [[amplification_id]],
                                constant uint &eyeBase [[buffer(1)]]) {
    float2 ndc = float2((vid == 1) ? 3.0 : -1.0, (vid == 2) ? 3.0 : -1.0);
    FullOut o;
    o.position = float4(ndc, 0.0, 1.0);
    o.uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    o.eye = eye + eyeBase;   // eyeBase: view index when each eye has its own pass
    return o;
}

struct DepthOnly { float depth [[depth(any)]]; };

// "No depth" comparison: far depth everywhere, so the compositor treats the
// image as infinitely distant and corrects rotation only.
fragment DepthOnly farDepthFragment(FullOut in [[stage_in]]) {
    DepthOnly o; o.depth = 0.0; return o;
}

struct PresentOut {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

// Hold mode: copy the newest finished game frame (already upscaled by MetalFX)
// and its depth into the compositor drawable. The drawable's rasterization rate
// map is bound to the pass, so this copy is foveated by the compositor's map.
fragment PresentOut presentFragment(FullOut in [[stage_in]],
                                    texture2d_array<float> color [[texture(0)]],
                                    depth2d_array<float> depth [[texture(1)]],
                                    constant uint &writeDepth [[buffer(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    PresentOut o;
    o.color = color.sample(s, in.uv, in.eye);
    if (writeDepth) {
        uint2 size = uint2(depth.get_width(), depth.get_height());
        uint2 texel = min(uint2(in.uv * float2(size)), size - 1);
        o.depth = depth.read(texel, in.eye);
    } else {
        o.depth = 0.0;
    }
    return o;
}
