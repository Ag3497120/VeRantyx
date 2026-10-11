#include "vr_skeleton.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include "hand_skeleton_alvr.inc"

namespace vrrt {
using namespace vr;

static HmdVector4_t lerp4(const HmdVector4_t &a, const HmdVector4_t &b, float t) {
    return { a.v[0] + (b.v[0] - a.v[0]) * t, a.v[1] + (b.v[1] - a.v[1]) * t, a.v[2] + (b.v[2] - a.v[2]) * t, 1.f };
}
static HmdQuaternionf_t slerp(HmdQuaternionf_t a, HmdQuaternionf_t b, float t) {
    float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) { d = -d; b = { -b.w, -b.x, -b.y, -b.z }; }
    float wa, wb;
    if (d > 0.9995f) { wa = 1 - t; wb = t; }
    else { float th = acosf(d), s = sinf(th); wa = sinf((1 - t) * th) / s; wb = sinf(t * th) / s; }
    HmdQuaternionf_t r = { wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z };
    float n = sqrtf(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    if (n > 0) { r.w /= n; r.x /= n; r.y /= n; r.z /= n; }
    return r;
}
static void blend(VRBoneTransform_t &o, const VRBoneTransform_t &a, const VRBoneTransform_t &b, float t) {
    o.position = lerp4(a.position, b.position, t);
    o.orientation = slerp(a.orientation, b.orientation, t);
}
static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

void skeleton_bones(bool left, bool withController, const FingerInputs &in, SkeletonState &st, double now,
                    VRBoneTransform_t out[kSkeletonBones]) {
    // Touch transitions ease over ~100 ms rather than snapping.
    const float dt = st.last > 0 ? (float)std::min(0.1, std::max(0.0, now - st.last)) : 0.f;
    st.last = now;
    auto approach = [&](float &v, float target) { float step = dt * 10.f; v += std::max(-step, std::min(step, target - v)); };
    approach(st.thumbProgress, in.thumbTouch ? 1.f : 0.f);
    approach(st.indexProgress, in.indexTouch ? 1.f : 0.f);

    VRBoneTransform_t a[kSkeletonBones], b[kSkeletonBones];
    memset(a, 0, sizeof a); memset(b, 0, sizeof b);
    out[0] = { { 0, 0, 0, 1 }, { 1, 0, 0, 0 } };
    out[1] = left ? VRBoneTransform_t{ { -0.034038f, 0.036503f, 0.164722f, 1 }, { -0.055147f, -0.078608f, -0.920279f, 0.379296f } }
                  : VRBoneTransform_t{ { 0.034038f, 0.036503f, 0.164722f, 1 }, { -0.055147f, -0.078608f, 0.920279f, -0.379296f } };
    // Thumb.
    alvr_skeleton::GetThumbBoneTransform(withController, left, false, a);
    alvr_skeleton::GetThumbBoneTransform(withController, left, true, b);
    for (int i = 2; i < 6; i++) blend(out[i], a[i], b[i], st.thumbProgress);
    // Index to pinky from the trigger (touch -> pull).
    const float trigger = clamp01(in.trigger);
    if (trigger > 0) {
        alvr_skeleton::GetTriggerBoneTransform(withController, left, true, false, a);
        alvr_skeleton::GetTriggerBoneTransform(withController, left, true, true, b);
        for (uint32_t i = 6; i < kSkeletonBones; i++) blend(out[i], a[i], b[i], trigger);
    } else {
        alvr_skeleton::GetTriggerBoneTransform(withController, left, false, false, a);
        alvr_skeleton::GetTriggerBoneTransform(withController, left, true, false, b);
        for (uint32_t i = 6; i < kSkeletonBones; i++) blend(out[i], a[i], b[i], st.indexProgress);
    }
    // Middle to pinky (and their aux bones) closed by the grip.
    const float grip = clamp01(in.grip);
    if (grip > 0) {
        memset(b, 0, sizeof b);
        alvr_skeleton::GetGripClickBoneTransform(withController, left, b);
        for (int i = 11; i < 26; i++) blend(out[i], out[i], b[i], grip);
        for (uint32_t i = 28; i < kSkeletonBones; i++) blend(out[i], out[i], b[i], grip);
    }
}

void skeleton_reference(bool left, EVRSkeletalReferencePose pose, VRBoneTransform_t out[kSkeletonBones]) {
    SkeletonState st;
    FingerInputs in = {};
    bool withController = false;
    switch (pose) {
    case VRSkeletalReferencePose_Fist: in = { 1.f, 1.f, true, true }; break;
    case VRSkeletalReferencePose_GripLimit: in = { 0.f, 0.f, true, true }; withController = true; break;
    default: break;   // bind pose / open hand
    }
    st.thumbProgress = in.thumbTouch ? 1.f : 0.f;
    st.indexProgress = in.indexTouch ? 1.f : 0.f;
    skeleton_bones(left, withController, in, st, 0, out);
}

int skeleton_parent(uint32_t bone) {
    // root(0) wrist(1) thumb 2-5, index 6-10, middle 11-15, ring 16-20,
    // pinky 21-25, then aux bones 26-30 (thumb..pinky) hanging off the root.
    if (bone == 0) return -1;
    if (bone == 1) return 0;
    if (bone >= 26) return 0;
    const uint32_t firsts[] = { 2, 6, 11, 16, 21 };
    for (uint32_t f : firsts) if (bone == f) return 1;
    return (int)bone - 1;
}

static HmdQuaternionf_t qmul(const HmdQuaternionf_t &a, const HmdQuaternionf_t &b) {
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
             a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}
static HmdVector4_t qrot(const HmdQuaternionf_t &q, const HmdVector4_t &v) {
    HmdQuaternionf_t p = { 0, v.v[0], v.v[1], v.v[2] };
    HmdQuaternionf_t c = { q.w, -q.x, -q.y, -q.z };
    HmdQuaternionf_t r = qmul(qmul(q, p), c);
    return { r.x, r.y, r.z, 1 };
}

void skeleton_to_model(VRBoneTransform_t bones[kSkeletonBones]) {
    VRBoneTransform_t model[kSkeletonBones];
    for (uint32_t i = 0; i < kSkeletonBones; i++) {
        int p = skeleton_parent(i);
        if (p < 0 || i >= 26) { model[i] = bones[i]; continue; }   // aux bones are already root-relative
        const VRBoneTransform_t &pm = model[p];
        HmdVector4_t off = qrot(pm.orientation, bones[i].position);
        model[i].position = { pm.position.v[0] + off.v[0], pm.position.v[1] + off.v[1], pm.position.v[2] + off.v[2], 1 };
        model[i].orientation = qmul(pm.orientation, bones[i].orientation);
    }
    memcpy(bones, model, sizeof model);
}

const char *skeleton_bone_name(uint32_t bone) {
    static const char *names[kSkeletonBones] = {
        "Root", "Wrist", "Thumb0", "Thumb1", "Thumb2", "Thumb3",
        "IndexFinger0", "IndexFinger1", "IndexFinger2", "IndexFinger3", "IndexFinger4",
        "MiddleFinger0", "MiddleFinger1", "MiddleFinger2", "MiddleFinger3", "MiddleFinger4",
        "RingFinger0", "RingFinger1", "RingFinger2", "RingFinger3", "RingFinger4",
        "PinkyFinger0", "PinkyFinger1", "PinkyFinger2", "PinkyFinger3", "PinkyFinger4",
        "Aux_Thumb", "Aux_IndexFinger", "Aux_MiddleFinger", "Aux_RingFinger", "Aux_PinkyFinger" };
    return bone < kSkeletonBones ? names[bone] : "";
}

} // namespace vrrt
