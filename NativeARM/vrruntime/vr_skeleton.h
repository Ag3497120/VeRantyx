#pragma once
// OpenVR's 31-bone hand skeleton, driven from the controller-style values the
// headset sends (pinch -> trigger, fist -> grip, thumb on a button/stick).
// The bone poses come from ALVR (hand_skeleton_alvr.inc, MIT); the blending
// follows ALVR's Controller::GetBoneTransform.
#include "openvr/openvr.h"

namespace vrrt {

constexpr uint32_t kSkeletonBones = 31;

struct FingerInputs { float trigger, grip; bool thumbTouch, indexTouch; };

// Per-hand smoothing of the touch transitions (ALVR animates them too).
struct SkeletonState { float thumbProgress = 0, indexProgress = 0; double last = 0; };

void skeleton_bones(bool left, bool withController, const FingerInputs &in, SkeletonState &st, double now,
                    vr::VRBoneTransform_t out[kSkeletonBones]);
// Reference poses: open hand (bind), fist, and the controller-holding shape.
void skeleton_reference(bool left, vr::EVRSkeletalReferencePose pose, vr::VRBoneTransform_t out[kSkeletonBones]);
// Parent-space -> model-space (root-relative) for the bones' own hierarchy.
void skeleton_to_model(vr::VRBoneTransform_t bones[kSkeletonBones]);
int skeleton_parent(uint32_t bone);
const char *skeleton_bone_name(uint32_t bone);

} // namespace vrrt
