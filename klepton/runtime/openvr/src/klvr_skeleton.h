// The SteamVR hand skeleton. See klvr_skeleton.cpp.
#pragma once
#include "openvr.h"

namespace klvr {
enum { kBoneCount = 31 };

const char *skel_bone_name(int bone);
int skel_parent(int bone);

// Parent-space transforms.
void skel_reference(int hand, vr::EVRSkeletalReferencePose which, vr::VRBoneTransform_t out[kBoneCount]);
// curl[finger] 0 = open .. 1 = closed; fingers are thumb, index, middle, ring, pinky.
void skel_pose(int hand, const float curl[5], vr::EVRSkeletalMotionRange range, vr::VRBoneTransform_t out[kBoneCount]);
void skel_to_model(const vr::VRBoneTransform_t parent[kBoneCount], vr::VRBoneTransform_t model[kBoneCount]);
} // namespace klvr
