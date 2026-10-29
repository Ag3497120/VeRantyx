// Exercise the real capture proxy with shared poses, without SteamVR or a headset.
#include <assert.h>
#include "../src/openvr_capture_proxy.cpp"

static vr::TrackedDeviceIndex_t leftRole = vr::k_unTrackedDeviceIndexInvalid, rightRole = vr::k_unTrackedDeviceIndexInvalid;
static bool roleQueryAvailable = true;
static bool TestRoles(vr::TrackedDeviceIndex_t* left, vr::TrackedDeviceIndex_t* right) {
    *left = leftRole; *right = rightRole;
    return roleQueryAvailable;
}

static void Roles(vr::TrackedDeviceIndex_t left, vr::TrackedDeviceIndex_t right) {
    leftRole = left; rightRole = right;
    g_controllerRoleProbeAfter = 0;
}

static void Identity(float* matrix, float x, float y, float z) {
    memset(matrix, 0, sizeof(float) * 16);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
    matrix[12] = x; matrix[13] = y; matrix[14] = z;
}

static void DriverHeight(float offset, bool supported = true) {
    // The existing pose-history header reserves two words for driver coordinate metadata.
    uint8_t* history = g_sharedBytes + kPoseHistoryOffset;
    const uint32_t magic = kPoseHistoryMagic, heightMagic = supported ? 0x464f4856 : 0;
    memcpy(history + 4, &magic, sizeof(magic));
    memcpy(history + 8, &heightMagic, sizeof(heightMagic));
    memcpy(history + 12, &offset, sizeof(offset));
}

static vr::ETrackingUniverseOrigin VR_CALLTYPE StandingSpace(void*) { return vr::TrackingUniverseStanding; }

int main() {
    SetEnvironmentVariableA("VERANTYX_DECOUPLED_POSES", "1");
    g_queryControllerRoles = TestRoles;
    g_sharedBytes = (uint8_t*)VirtualAlloc(nullptr, kMapSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(g_sharedBytes);
    g_sharedHeader = (SharedHeader*)g_sharedBytes;
    SharedHands* hands = (SharedHands*)(g_sharedBytes + kHandsOffset);
    Identity(hands->headTransform, 0, 1.6f, 0);
    Identity(hands->leftTransform, -0.2f, 1.3f, -0.5f);
    Identity(hands->rightTransform, 0.3f, 1.4f, -0.4f);
    g_lastRenderPoseCount = vr::k_unMaxTrackedDeviceCount;
    g_realPoseSets = 90;
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount] = {};
    assert(DecoupledWait(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0));
    // The headset is put on after the game has already switched to cached poses.
    hands->headTracked = hands->leftTracked = hands->rightTracked = 1;
    assert(g_leftHandDeviceIndex == -1 && g_rightHandDeviceIndex == -1);
    assert(!DecoupledWait(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0));

    // Roles can become available while poses are still invalid, or without another real pose set.
    Roles(3, 7);
    hands->leftTracked = hands->rightTracked = 0;
    SynthesizePoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(g_leftHandDeviceIndex == 3 && g_rightHandDeviceIndex == 7);
    assert(poses[3].bDeviceIsConnected && !poses[3].bPoseIsValid && !poses[7].bPoseIsValid);
    hands->leftTracked = hands->rightTracked = 1;
    hands->velocityValid = 1;
    hands->leftVelocity[0] = 0.25f;
    hands->rightAngularVelocity[1] = 0.5f;
    assert(DecoupledWait(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0));
    assert(poses[3].bPoseIsValid && poses[7].bPoseIsValid);
    assert(poses[3].mDeviceToAbsoluteTracking.m[0][3] == -0.2f);
    assert(poses[7].mDeviceToAbsoluteTracking.m[0][3] == 0.3f);
    assert(poses[3].vVelocity.v[0] == 0.25f && poses[7].vAngularVelocity.v[1] == 0.5f);
    assert(g_realPoseSets == 90);

    Roles(5, 9);
    SynthesizePoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(poses[5].bPoseIsValid && poses[9].bPoseIsValid);
    assert(!poses[3].bPoseIsValid && !poses[7].bPoseIsValid);
    Roles(vr::k_unTrackedDeviceIndexInvalid, 9);
    hands->leftTracked = 0;
    SynthesizePoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(!poses[5].bPoseIsValid && poses[9].bPoseIsValid);
    Roles(5, 9);
    hands->leftTracked = 1;
    memcpy(hands->rightTransform, hands->leftTransform, sizeof(hands->leftTransform));
    SynthesizePoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(g_leftHandDeviceIndex == 5 && g_rightHandDeviceIndex == 9);
    assert(poses[5].bPoseIsValid && poses[9].bPoseIsValid); // Coincident hands still have distinct roles.

    Roles(0, vr::k_unMaxTrackedDeviceCount);
    SynthesizePoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(g_leftHandDeviceIndex == -1 && g_rightHandDeviceIndex == -1);
    assert(poses[0].mDeviceToAbsoluteTracking.m[1][3] == 1.6f);
    Roles(5, 5);
    SynthesizePoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(g_leftHandDeviceIndex == -1 && g_rightHandDeviceIndex == -1);
    Roles(5, 9);
    struct { vr::TrackedDevicePose_t head; uint32_t guard; } shortOutput = { {}, 0x12345678 };
    SynthesizePoses(&shortOutput.head, 1);
    assert(shortOutput.guard == 0x12345678);

    // Preserve pose-based discovery for clients without the public role interface.
    roleQueryAvailable = false;
    g_controllerRoleProbeAfter = 0;
    g_leftHandDeviceIndex = g_rightHandDeviceIndex = -1;
    memset(poses, 0, sizeof(poses));
    poses[1].bDeviceIsConnected = poses[1].bPoseIsValid = true;
    poses[2].bDeviceIsConnected = poses[2].bPoseIsValid = true;
    ColumnMajorToHmdMatrix(hands->leftTransform, &poses[1].mDeviceToAbsoluteTracking);
    ColumnMajorToHmdMatrix(hands->rightTransform, &poses[2].mDeviceToAbsoluteTracking);
    RememberRenderPoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(g_leftHandDeviceIndex == 1 && g_rightHandDeviceIndex == 2);
    SynthesizePoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(poses[1].bPoseIsValid && poses[2].bPoseIsValid);

    // After a tracking break, action poses from SteamVR include the driver's height correction.
    // Decoupled render/game poses must use that same origin or hands appear below the head.
    DriverHeight(-0.74f);
    vr::TrackedDevicePose_t gamePoses[vr::k_unMaxTrackedDeviceCount] = {};
    assert(DecoupledWait(poses, vr::k_unMaxTrackedDeviceCount, gamePoses, vr::k_unMaxTrackedDeviceCount));
    for (vr::TrackedDevicePose_t* output : { poses, gamePoses }) {
        assert(fabsf(output[0].mDeviceToAbsoluteTracking.m[1][3] - 0.86f) < 0.0001f);
        assert(fabsf(output[1].mDeviceToAbsoluteTracking.m[1][3] - 0.56f) < 0.0001f);
        assert(fabsf(output[2].mDeviceToAbsoluteTracking.m[1][3] - 0.56f) < 0.0001f);
        assert(output[1].vVelocity.v[0] == 0.25f && output[2].vAngularVelocity.v[1] == 0.5f);
    }
    assert(hands->headTransform[13] == 1.6f && hands->leftTransform[13] == 1.3f);
    DriverHeight(0.35f);
    SynthesizePoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(fabsf(poses[0].mDeviceToAbsoluteTracking.m[1][3] - 1.95f) < 0.0001f);
    DriverHeight(-0.74f, false); // An older driver has no height metadata.
    SynthesizePoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(poses[0].mDeviceToAbsoluteTracking.m[1][3] == 1.6f);
    DriverHeight(NAN);
    SynthesizePoses(poses, vr::k_unMaxTrackedDeviceCount);
    assert(poses[0].mDeviceToAbsoluteTracking.m[1][3] == 1.6f);
    DriverHeight(0);
    g_sharedHeader->poseSequence = 1;
    SharedHands snapshot = {};
    assert(!ReadSharedHandsSnapshot(&snapshot));
    g_sharedHeader->poseSequence = 2;

    // Standing games must not reset SteamVR's seated origin while the headset is on a table.
    void* compositorTable[2] = { nullptr, (void*)&StandingSpace };
    struct { void** vtable; } compositor = { compositorTable };
    g_firstTrackedTicks = PerformanceTicks() - (uint64_t)(PerformanceFrequency() * 3);
    g_lastHead = poses[0].mDeviceToAbsoluteTracking;
    g_lastHeadChangeTicks = PerformanceTicks();
    g_seatedRecentered = 0;
    MaybeRecenterSeated(&compositor, poses, vr::k_unMaxTrackedDeviceCount);
    assert(g_seatedRecentered == 0 && g_seatedOffsetSet == 0);
    puts("controller hotplug: late roles, reconnect, bounds, velocities, shared height origin and standing recenter guard -> ok");
    return 0;
}
