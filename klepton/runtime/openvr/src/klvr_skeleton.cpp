// The SteamVR hand skeleton, driven from PSVR2 Sense finger curls.
//
// Alyx draws its hands from IVRInput's skeletal actions: 31 bones per hand,
// in SteamVR's bone frames. Getting those frames wrong by an axis is not a
// subtle error, so the reference poses are Valve's own data (see
// tools/gen_hand_poses.py) rather than something derived here, and a live pose
// is a per-finger blend between Valve's open hand and Valve's fist.
//
// The Sense controllers report thumb, index and middle-finger contact and the
// trigger's travel, so the blend weights are an estimate — the same kind
// SteamVR itself makes for Oculus Touch ("partial" skeletal tracking).
#include "klvr_internal.h"
#include "klvr_skeleton.h"
#include <cmath>

namespace klvr {
namespace {
#include "generated/klvr_hand_ref.inc"

const char *const kBoneNames[kBoneCount] = {
    "Root", "Wrist",
    "Thumb0", "Thumb1", "Thumb2", "Thumb3",
    "IndexFinger0", "IndexFinger1", "IndexFinger2", "IndexFinger3", "IndexFinger4",
    "MiddleFinger0", "MiddleFinger1", "MiddleFinger2", "MiddleFinger3", "MiddleFinger4",
    "RingFinger0", "RingFinger1", "RingFinger2", "RingFinger3", "RingFinger4",
    "PinkyFinger0", "PinkyFinger1", "PinkyFinger2", "PinkyFinger3", "PinkyFinger4",
    "Aux_Thumb", "Aux_IndexFinger", "Aux_MiddleFinger", "Aux_RingFinger", "Aux_PinkyFinger",
};

// Aux bones hang off the root: checked against the reference data, where each
// sits beside its finger's last joint only when read relative to the root.
const int8_t kParent[kBoneCount] = {
    -1, 0,
    1, 2, 3, 4,
    1, 6, 7, 8, 9,
    1, 11, 12, 13, 14,
    1, 16, 17, 18, 19,
    1, 21, 22, 23, 24,
    0, 0, 0, 0, 0,
};

// Which finger (0 thumb .. 4 pinky) moves each bone; -1 for root and wrist.
int finger_of(int bone) {
    if (bone >= 2 && bone <= 5) return 0;
    if (bone >= 6 && bone <= 25) return 1 + (bone - 6) / 5;
    if (bone >= 26) return bone - 26;
    return -1;
}

vr::VRBoneTransform_t bone(const float r[7]) {
    vr::VRBoneTransform_t b;
    b.position = { { r[0], r[1], r[2], 1.0f } };
    b.orientation = { r[6], r[3], r[4], r[5] };   // HmdQuaternionf_t is w, x, y, z
    return b;
}

Quat q_of(const vr::VRBoneTransform_t &b) { return { b.orientation.x, b.orientation.y, b.orientation.z, b.orientation.w }; }
Vec3 p_of(const vr::VRBoneTransform_t &b) { return { b.position.v[0], b.position.v[1], b.position.v[2] }; }
vr::VRBoneTransform_t make(Vec3 p, Quat q) {
    vr::VRBoneTransform_t b;
    b.position = { { p.x, p.y, p.z, 1.0f } };
    b.orientation = { q.w, q.x, q.y, q.z };
    return b;
}
} // namespace

const char *skel_bone_name(int i) { return i >= 0 && i < kBoneCount ? kBoneNames[i] : nullptr; }
int skel_parent(int i) { return i >= 0 && i < kBoneCount ? kParent[i] : -1; }

// How far toward Valve's full fist a curl of 1 goes while the hand is holding
// a controller. SteamVR ships a separate "grip limit" reference pose for this;
// the Unity plugin does not include it, so it is approximated as a fraction of
// the fist. KLVR_GRIP_LIMIT tunes it on device.
static float grip_limit() {
    static float v = env_float("KLVR_GRIP_LIMIT", 0.8f);
    return v;
}

void skel_reference(int hand, vr::EVRSkeletalReferencePose which, vr::VRBoneTransform_t out[kBoneCount]) {
    hand = hand ? 1 : 0;
    if (which == vr::VRSkeletalReferencePose_GripLimit) {
        float c[5] = { 1, 1, 1, 1, 1 };
        skel_pose(hand, c, vr::VRSkeletalMotionRange_WithController, out);
        return;
    }
    const float (*t)[7] = which == vr::VRSkeletalReferencePose_Fist ? kRefFist[hand]
                        : which == vr::VRSkeletalReferencePose_OpenHand ? kRefOpen[hand]
                        : kRefBind[hand];
    for (int i = 0; i < kBoneCount; i++) out[i] = bone(t[i]);
}

void skel_pose(int hand, const float curl[5], vr::EVRSkeletalMotionRange range, vr::VRBoneTransform_t out[kBoneCount]) {
    hand = hand ? 1 : 0;
    float limit = range == vr::VRSkeletalMotionRange_WithController ? grip_limit() : 1.0f;
    for (int i = 0; i < kBoneCount; i++) {
        vr::VRBoneTransform_t a = bone(kRefOpen[hand][i]), b = bone(kRefFist[hand][i]);
        int f = finger_of(i);
        float t = f < 0 ? 0.0f : std::fmin(std::fmax(curl[f], 0.0f), 1.0f) * limit;
        out[i] = make(vlerp(p_of(a), p_of(b), t), qslerp(q_of(a), q_of(b), t));
    }
}

void skel_to_model(const vr::VRBoneTransform_t parent[kBoneCount], vr::VRBoneTransform_t model[kBoneCount]) {
    Pose m[kBoneCount];
    for (int i = 0; i < kBoneCount; i++) {
        Pose local = { p_of(parent[i]), q_of(parent[i]) };
        m[i] = kParent[i] < 0 ? local : pmul(m[kParent[i]], local);
        model[i] = make(m[i].p, m[i].q);
    }
}

} // namespace klvr
