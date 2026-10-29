// Synthetic shared memory, action manifests and a fake Vulkan queue only.
// No Valve binaries, Steam, Metal device, encoder or network connection.
#include "../vrruntime/vr_compositor.cpp"
#include "../vrruntime/vr_misc.cpp"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <limits>
#include <map>
#include <fstream>

vr::IVRInput *vrrt_input();
vr::IVRSystem *vrrt_system();
void vrrt_log_unimplemented(const char *, const char *) {}
extern "C" void *kl_vulkan_xr_physical_device(void *) { return nullptr; }
extern "C" void kl_vulkan_async_begin_frame(void) {}

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
static vrrt::DoubleBufferHeader *header() { return (vrrt::DoubleBufferHeader *)vrrt::shm_base(); }
static vrrt::SharedFrame *frame() {
    return (vrrt::SharedFrame *)(vrrt::shm_base() + 64 + (header()->latestReadyIndex & 1u) * vrrt::kFrameBlockSize);
}
static void identity(float *t, float x, float y, float z) {
    memset(t, 0, 64); t[0] = t[5] = t[10] = t[15] = 1; t[12] = x; t[13] = y; t[14] = z;
}
static void send(const vrrt::SharedHands &h) {
    uint32_t seq = header()->poseSequence;
    __atomic_store_n(&header()->poseSequence, seq | 1u, __ATOMIC_SEQ_CST);
    memcpy(vrrt::shm_base() + vrrt::kHandsOffset, &h, sizeof h);
    __atomic_store_n(&header()->poseSequence, (seq | 1u) + 1u, __ATOMIC_RELEASE);
}

static vrrt::PoseSet test_shared_memory() {
    vrrt::SharedHands h = {}, read = {};
    assert(!vrrt::read_hands(&read));
    identity(h.headTransform, 0, 1.7f, 0); identity(h.leftTransform, -.2f, 1.2f, -.4f); identity(h.rightTransform, .2f, 1.2f, -.4f);
    h.headTracked = h.leftTracked = h.rightTracked = h.velocityValid = 1;
    h.leftVelocity[0] = 2; h.rightAngularVelocity[1] = 3;
    float tangents[8] = { -1.1f, .9f, -1, 1, -.9f, 1.1f, -1, 1 };
    memcpy(h.leftTangents, tangents, 32); h.visionTimestamp = 100;
    send(h);
    assert(vrrt::read_hands(&read) && read.visionTimestamp == 100);
    header()->poseSequence |= 1;
    read.visionTimestamp = 777;
    assert(!vrrt::read_hands(&read) && read.visionTimestamp == 777);
    header()->poseSequence++;
    vrrt::PoseSet p;
    vrrt::sample_poses(&p, 0);
    h.visionTimestamp += .01; send(h); vrrt::sample_poses(&p, 0);
    assert(p.fresh && p.inputsFresh && p.dev[1].bPoseIsValid);
    assert(near(p.dev[1].mDeviceToAbsoluteTracking.m[0][3], -.2f));
    assert(near(p.dev[1].vVelocity.v[0], 2) && near(p.dev[2].vAngularVelocity.v[1], 3));
    h.headTracked = 0; h.visionTimestamp += .01; send(h); vrrt::sample_poses(&p, 0);
    assert(!p.fresh && p.inputsFresh && p.dev[1].bPoseIsValid);
    h.headTracked = 1; h.visionTimestamp += .01; send(h); vrrt::sample_poses(&p, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    vrrt::sample_poses(&p, 0);
    assert(!p.fresh && !p.inputsFresh && !p.dev[1].bPoseIsValid);
    h.visionTimestamp = 1; send(h); vrrt::sample_poses(&p, 0);
    assert(!p.fresh); // a restarted ALVR clock needs another advancing sample
    h.visionTimestamp += .01; send(h); vrrt::sample_poses(&p, 0);
    assert(p.fresh && p.inputsFresh);
    auto valid = p;
    h.visionTimestamp = std::numeric_limits<double>::infinity(); send(h); vrrt::sample_poses(&p, 0);
    assert(!p.inputsFresh && p.visionTimestamp == 0);
    h.visionTimestamp = valid.visionTimestamp + .01; send(h); vrrt::sample_poses(&p, 0);
    vrrt::set_last_poses(p);
    auto copy = vrrt::last_poses(); copy.visionTimestamp = 99;
    assert(vrrt::last_poses().visionTimestamp == p.visionTimestamp);

    auto extras = (vrrt::SharedExtras *)(vrrt::shm_base() + vrrt::kExtrasOffset);
    extras->ipdMeters = .065f; extras->ipdMagic = vrrt::kIpdMagic;
    assert(near(vrrt::headset_ipd(), .065f));
    assert(near(vrrt_system()->GetEyeToHeadTransform(vr::Eye_Right).m[0][3], .0325f));
    assert(near(vrrt_system()->GetFloatTrackedDeviceProperty(0, vr::Prop_UserIpdMeters_Float, nullptr), .065f));
    setenv("VERANTYX_IPD", "0.063", 1);
    assert(near(vrrt::headset_ipd(), .063f)); unsetenv("VERANTYX_IPD");
    extras->ipdMeters = std::numeric_limits<float>::quiet_NaN();
    assert(near(vrrt::headset_ipd(), .063f));

    uint32_t index;
    assert(!vrrt::frame_begin(UINT32_MAX, UINT32_MAX, &index));
    assert(!vrrt::frame_begin(4, 0, &index));
    auto pixels = vrrt::frame_begin(4, 2, &index); assert(pixels);
    memset(pixels, 42, 32);
    vrrt::frame_publish(index, 4, 2, 87, p.headTransform, p.visionTimestamp);
    assert(frame()->sequenceNumber && frame()->width == 4 && frame()->visionTimestamp == p.visionTimestamp);
    assert(header()->captureSourceMode == 2 && header()->padding[8] == 0x4E415456);
    auto old = frame();
    vrrt::frame_begin(4, 2, &index); vrrt::frame_publish(index, 4, 2, 87, p.headTransform, 2);
    vrrt::frame_begin(4, 2, &index);
    assert(old->sequenceNumber == 0); // invalidated before its pixels are reused
    return p;
}

static void test_input(const char *manifest, vrrt::PoseSet p) {
    using namespace vr;
    IVRInput *in = vrrt_input();
    assert(in->SetActionManifestPath(manifest) == VRInputError_None);
    assert(!in->IsUsingLegacyInput());
    VREvent_t event = {};
    assert(vrrt_system()->PollNextEvent(&event, sizeof event) && event.eventType == VREvent_Input_BindingLoadSuccessful);
    assert(vrrt_system()->PollNextEvent(&event, sizeof event) && event.eventType == VREvent_Input_BindingsUpdated);
    assert(!vrrt_system()->PollNextEvent(&event, sizeof event));
    VRActionSetHandle_t main, ui;
    assert(in->GetActionSetHandle("/actions/main", &main) == VRInputError_None);
    assert(in->GetActionSetHandle("/actions/ui", &ui) == VRInputError_None);
    auto action = [in](const char *name) { VRActionHandle_t h = 0; assert(in->GetActionHandle(name, &h) == VRInputError_None); return h; };
    auto use = action("/actions/main/in/use"), walk = action("/actions/main/in/walk"), pull = action("/actions/main/in/pull");
    auto pose = action("/actions/main/in/leftpose"), skeleton = action("/actions/main/in/leftskeleton"), pulse = action("/actions/main/out/haptic");
    auto teleport = action("/actions/main/in/teleport"), toggle = action("/actions/main/in/toggle");
    VRActionHandle_t missing = 123;
    assert(in->GetActionHandle("/actions/main/in/missing", &missing) == VRInputError_NameNotFound && !missing);
    VRInputValueHandle_t left, right;
    assert(in->GetInputSourceHandle("/user/hand/left", &left) == VRInputError_None);
    assert(in->GetInputSourceHandle("/user/hand/right", &right) == VRInputError_None);
    VRActiveActionSet_t sets[2] = {}; sets[0].ulActionSet = main;
    auto update = [&](uint32_t count = 1) { vrrt::set_last_poses(p); assert(in->UpdateActionState(sets, sizeof sets[0], count) == VRInputError_None); };
    auto digital = [&](VRActionHandle_t h, VRInputValueHandle_t hand) {
        InputDigitalActionData_t d; assert(in->GetDigitalActionData(h, &d, sizeof d, hand) == VRInputError_None); return d;
    };
    p.hands.leftGrip = p.hands.rightGrip = 255;
    p.hands.rightTrigger = 80; // string threshold 0.2, below the old hard-coded 0.75
    p.hands.leftStickX = .8f; p.hands.rightStickY = .3f;
    p.hands.leftButtons = vrrt::kBtnPrimary << 8;
    update();
    assert(digital(use, left).bState && digital(use, right).bState && digital(use, left).bChanged);
    assert(digital(pull, right).bState && digital(teleport, right).bState && digital(toggle, left).bState);
    p.hands.leftGrip = 0; update();
    auto release = digital(use, left);
    assert(release.bActive && !release.bState && release.bChanged && release.activeOrigin == left);
    assert(digital(use, right).bState && digital(use, k_ulInvalidInputValueHandle).bState);
    update(); assert(!digital(use, left).bChanged && digital(toggle, left).bState);
    p.hands.rightGrip = 128; update(); assert(digital(use, right).bState); // grab hysteresis
    p.hands.rightGrip = 0; update(); assert(!digital(use, right).bState && digital(use, right).bChanged);
    p.hands.rightStickY = .6f; update();
    InputAnalogActionData_t axis;
    assert(in->GetAnalogActionData(walk, &axis, sizeof axis, right) == VRInputError_None);
    assert(axis.bActive && near(axis.y, .6f) && near(axis.deltaY, .3f) && axis.activeOrigin == right);
    assert(in->GetAnalogActionData(walk, &axis, sizeof axis, left) == VRInputError_None && near(axis.x, .8f));
    InputPoseActionData_t pd;
    assert(in->GetPoseActionDataForNextFrame(pose, TrackingUniverseStanding, &pd, sizeof pd, right) == VRInputError_None && !pd.bActive);
    assert(in->GetPoseActionDataForNextFrame(pose, TrackingUniverseStanding, &pd, sizeof pd, left) == VRInputError_None && pd.bActive && pd.pose.bPoseIsValid);
    assert(in->GetDigitalActionData(use, nullptr, 0, left) == VRInputError_InvalidParam);
    InputDigitalActionData_t dd;
    assert(in->GetDigitalActionData(walk, &dd, sizeof dd, left) == VRInputError_WrongType);
    assert(in->UpdateActionState(sets, 1, 1) == VRInputError_InvalidParam);
    sets[1].ulActionSet = ui; sets[1].nPriority = 10; update(2);
    assert(!digital(pull, right).bActive); // UI reserves this physical trigger
    assert(digital(use, right).bActive);  // grip is still available
    sets[0].ulRestrictedToDevice = left; sets[0].ulSecondaryActionSet = ui;
    update(); assert(!digital(use, right).bActive && digital(use, left).bActive);
    sets[0].ulRestrictedToDevice = 0; sets[0].ulSecondaryActionSet = 0;
    p.inputsFresh = false; update();
    assert(!digital(toggle, left).bState); // a lost link cannot leave a toggled button stuck
    p.inputsFresh = true; p.hands.leftTrigger = 128; p.hands.leftGrip = 200; update();
    VRSkeletalSummaryData_t summary;
    assert(in->GetSkeletalSummaryData(skeleton, VRSummaryType_FromDevice, &summary) == VRInputError_None);
    assert(near(summary.flFingerCurl[1], 128/255.f) && near(summary.flFingerCurl[2], 200/255.f));
    std::array<VRBoneTransform_t, 31> bones;
    assert(in->GetSkeletalBoneData(skeleton, VRSkeletalTransformSpace_Model, VRSkeletalMotionRange_WithController, bones.data(), 31) == VRInputError_None);
    for (auto &b : bones) assert(std::isfinite(b.orientation.w));
    auto haptics = (vrrt::SharedHaptic *)(vrrt::shm_base() + vrrt::kHapticsOffset);
    auto rightSeq = haptics[1].sequence;
    assert(in->TriggerHapticVibrationAction(pulse, 0, .2f, 80, .5f, left) == VRInputError_None);
    assert(haptics[0].sequence && near(haptics[0].amplitude, .5f) && haptics[1].sequence == rightSeq);
    assert(in->TriggerHapticVibrationAction(pulse, 0, .2f, 80, 2, right) == VRInputError_InvalidParam);
    assert(vrrt::haptic(1, .01f, .1f, 60, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(15)); vrrt::sample_poses(&p, 0);
    assert(haptics[1].sequence != rightSeq && near(haptics[1].frequency, 60));
    assert(in->SetActionManifestPath((std::string(manifest) + ".missing").c_str()) != VRInputError_None);
    assert(!in->IsUsingLegacyInput()); // failed reloads must not re-enable raw button input
    assert(vrrt_system()->PollNextEvent(&event, sizeof event) && event.eventType == VREvent_Input_BindingLoadFailed);
    assert(!vrrt_system()->PollNextEvent(&event, sizeof event));
    assert(in->SetActionManifestPath(manifest) == VRInputError_None);
}

static void test_pose_contention(vrrt::PoseSet p) {
    using namespace vr;
    auto *in = vrrt_input();
    VRActionSetHandle_t main;
    VRActionHandle_t use, pull;
    VRInputValueHandle_t left, right;
    assert(in->GetActionSetHandle("/actions/main", &main) == VRInputError_None);
    assert(in->GetActionHandle("/actions/main/in/use", &use) == VRInputError_None);
    assert(in->GetActionHandle("/actions/main/in/pull", &pull) == VRInputError_None);
    assert(in->GetInputSourceHandle("/user/hand/left", &left) == VRInputError_None);
    assert(in->GetInputSourceHandle("/user/hand/right", &right) == VRInputError_None);
    VRActiveActionSet_t set = {}; set.ulActionSet = main;
    auto update = [&] {
        vrrt::set_last_poses(p);
        assert(in->UpdateActionState(&set, sizeof set, 1) == VRInputError_None);
    };
    auto digital = [&](VRActionHandle_t action, VRInputValueHandle_t source) {
        InputDigitalActionData_t d;
        assert(in->GetDigitalActionData(action, &d, sizeof d, source) == VRInputError_None);
        return d;
    };
    auto h = p.hands;
    h.visionTimestamp = 200;
    h.headTracked = h.leftTracked = h.rightTracked = 1;
    h.leftGrip = h.rightGrip = h.rightTrigger = 255;
    send(h); vrrt::sample_poses(&p, 0);
    h.visionTimestamp += .01; send(h); vrrt::sample_poses(&p, 0); update();
    assert(p.fresh && p.inputsFresh && digital(use, left).bState && digital(pull, right).bState);
    auto held = p;

    // A writer can be descheduled with an odd sequence. Its partially copied
    // packet must neither release a held object nor give ALVR a zero pose ID.
    __atomic_fetch_or(&header()->poseSequence, 1u, __ATOMIC_SEQ_CST);
    memset(vrrt::shm_base() + vrrt::kHandsOffset, 0, sizeof h);
    for (int i = 0; i < 3; i++) {
        vrrt::sample_poses(&p, 0); update();
        assert(p.fresh && p.inputsFresh && p.visionTimestamp == held.visionTimestamp);
        assert(p.dev[1].bPoseIsValid && p.dev[2].bPoseIsValid);
        assert(!memcmp(p.headTransform, held.headTransform, sizeof p.headTransform));
        assert(digital(use, left).bState && !digital(use, left).bChanged);
        assert(digital(pull, right).bState && !digital(pull, right).bChanged);
        uint32_t index;
        auto *pixels = vrrt::frame_begin(4, 2, &index); assert(pixels); memset(pixels, 42, 32);
        vrrt::frame_publish(index, 4, 2, 87, p.headTransform, p.visionTimestamp);
        assert(frame()->visionTimestamp == held.visionTimestamp && frame()->visionTimestamp > 0);
    }
    h.visionTimestamp += .01; h.leftGrip = h.rightTrigger = 0;
    send(h); vrrt::sample_poses(&p, 0); update();
    assert(!digital(use, left).bState && digital(use, left).bChanged);
    assert(!digital(pull, right).bState && digital(pull, right).bChanged);

    h.visionTimestamp += .01; h.leftGrip = h.rightTrigger = 255;
    send(h); vrrt::sample_poses(&p, 0); update();
    __atomic_fetch_or(&header()->poseSequence, 1u, __ATOMIC_SEQ_CST);
    vrrt::sample_poses(&p, 0); update();
    assert(digital(use, left).bState);
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    vrrt::sample_poses(&p, 0); update();
    assert(!p.fresh && !p.inputsFresh && !p.dev[1].bPoseIsValid);
    assert(!digital(use, left).bState && digital(use, left).bChanged);
    assert(!digital(pull, right).bState);
    // Merely finishing the write without a newer timestamp is still stale.
    send(h); vrrt::sample_poses(&p, 0); assert(!p.inputsFresh);
    h.visionTimestamp += .01; send(h); vrrt::sample_poses(&p, 0); update();
    assert(p.fresh && p.inputsFresh && digital(use, left).bState);

    h.leftTracked = 0; h.visionTimestamp += .01;
    send(h); vrrt::sample_poses(&p, 0);
    assert(p.inputsFresh && !p.dev[1].bPoseIsValid && p.dev[2].bPoseIsValid);
    auto valid = h;
    h.visionTimestamp = std::numeric_limits<double>::quiet_NaN();
    send(h); vrrt::sample_poses(&p, 0); assert(!p.inputsFresh && p.visionTimestamp == 0);
    __atomic_fetch_or(&header()->poseSequence, 1u, __ATOMIC_SEQ_CST);
    vrrt::sample_poses(&p, 0); assert(!p.inputsFresh && p.visionTimestamp == 0);
    h = valid; h.leftTracked = 1; h.visionTimestamp += .01;
    send(h); vrrt::sample_poses(&p, 0); update(); assert(p.inputsFresh);
}

static void test_mixed_input_sources(vrrt::PoseSet p) {
    using namespace vr;
    IVRInput *in = vrrt_input();
    VRActionSetHandle_t main, ui;
    VRActionHandle_t pull, use, walk, pose;
    VRInputValueHandle_t left, right, gamepad, head;
    assert(in->GetActionSetHandle("/actions/main", &main) == VRInputError_None);
    assert(in->GetActionSetHandle("/actions/ui", &ui) == VRInputError_None);
    assert(in->GetActionHandle("/actions/main/in/pull", &pull) == VRInputError_None);
    assert(in->GetActionHandle("/actions/main/in/use", &use) == VRInputError_None);
    assert(in->GetActionHandle("/actions/main/in/walk", &walk) == VRInputError_None);
    assert(in->GetActionHandle("/actions/main/in/leftpose", &pose) == VRInputError_None);
    assert(in->GetInputSourceHandle("/user/hand/left", &left) == VRInputError_None);
    assert(in->GetInputSourceHandle("/user/hand/right", &right) == VRInputError_None);
    assert(in->GetInputSourceHandle("/user/gamepad", &gamepad) == VRInputError_None);
    assert(in->GetInputSourceHandle("/user/head", &head) == VRInputError_None);
    VRActiveActionSet_t sets[3] = {};
    sets[0].ulActionSet = ui; sets[0].ulRestrictedToDevice = gamepad; sets[0].nPriority = 100;
    sets[1].ulActionSet = main; sets[1].ulRestrictedToDevice = left;
    sets[2].ulActionSet = main; sets[2].ulRestrictedToDevice = right;
    auto update = [&](uint32_t count = 3) {
        p.hands.visionTimestamp += .01;
        send(p.hands); vrrt::sample_poses(&p, 0); vrrt::set_last_poses(p);
        assert(in->UpdateActionState(sets, sizeof sets[0], count) == VRInputError_None);
    };
    auto digital = [&](VRActionHandle_t action, VRInputValueHandle_t source) {
        InputDigitalActionData_t d;
        assert(in->GetDigitalActionData(action, &d, sizeof d, source) == VRInputError_None);
        return d;
    };
    // Alyx includes a /user/gamepad restriction in the same update as both
    // hands. An unbound device must not reject the batch or steal hand inputs.
    p.hands.rightTrigger = p.hands.leftGrip = 0;
    update(); assert(digital(pull, right).bActive && !digital(pull, right).bState);
    p.hands.rightTrigger = p.hands.leftGrip = 255;
    update(); assert(digital(pull, right).bState && digital(pull, right).bChanged && digital(use, left).bState);
    for (auto source : {gamepad, head}) {
        auto d = digital(pull, source); assert(!d.bActive && !d.bState && !d.bChanged && !d.activeOrigin);
        InputAnalogActionData_t a;
        assert(in->GetAnalogActionData(walk, &a, sizeof a, source) == VRInputError_None && !a.bActive && a.x == 0 && a.deltaY == 0);
        InputPoseActionData_t poseData;
        assert(in->GetPoseActionDataForNextFrame(pose, TrackingUniverseStanding, &poseData, sizeof poseData, source) == VRInputError_None && !poseData.bActive);
    }
    std::swap(sets[0], sets[2]);
    p.hands.rightTrigger = 0;
    update(); assert(!digital(pull, right).bState && digital(pull, right).bChanged);
    // Secondary sets cover all devices other than the restriction, including
    // both hands when the primary set is for the gamepad.
    sets[0] = {}; sets[0].ulActionSet = ui; sets[0].ulRestrictedToDevice = gamepad; sets[0].ulSecondaryActionSet = main;
    p.hands.rightTrigger = 255;
    update(1); assert(digital(pull, right).bActive && digital(pull, right).bState && digital(use, left).bState);
    sets[0].ulRestrictedToDevice = 0xdeadbeef;
    assert(in->UpdateActionState(sets, sizeof sets[0], 1) == VRInputError_InvalidHandle);
    InputDigitalActionData_t d;
    assert(in->GetDigitalActionData(pull, &d, sizeof d, 0xdeadbeef) == VRInputError_InvalidHandle);
    sets[0].ulRestrictedToDevice = gamepad; sets[0].ulActionSet = 0xdeadbeef;
    assert(in->UpdateActionState(sets, sizeof sets[0], 1) == VRInputError_InvalidHandle);
    sets[0].ulActionSet = ui; sets[0].ulSecondaryActionSet = 0xdeadbeef;
    assert(in->UpdateActionState(sets, sizeof sets[0], 1) == VRInputError_InvalidHandle);
    sets[0].ulRestrictedToDevice = 0; sets[0].ulActionSet = main;
    update(1); assert(digital(pull, right).bState); // unrestricted calls ignore the secondary field
}

static void test_locomotion(const char *manifest, vrrt::PoseSet p) {
    using namespace vr;
    auto *in = vrrt_input();
    const std::string file = std::string(manifest).substr(0, std::string(manifest).find_last_of('/') + 1) + "locomotion_actions.json";
    assert(in->SetActionManifestPath(file.c_str()) == VRInputError_None);
    VRActiveActionSet_t sets[2] = {};
    assert(in->GetActionSetHandle("/actions/move", &sets[0].ulActionSet) == VRInputError_None);
    assert(in->GetActionSetHandle("/actions/buttons", &sets[1].ulActionSet) == VRInputError_None);
    auto action = [&](const char *name) { VRActionHandle_t h; assert(in->GetActionHandle(name, &h) == VRInputError_None); return h; };
    auto walk = action("/actions/move/in/walk"), direction = action("/actions/move/in/direction");
    auto walkDefault = action("/actions/move/in/walkdefault");
    auto turnLeft = action("/actions/move/in/turnleft"), turnRight = action("/actions/move/in/turnright");
    auto continuous = action("/actions/move/in/continuousturn"), clickTurn = action("/actions/move/in/clickturn");
    auto stickClick = action("/actions/move/in/stickclick");
    auto primary = action("/actions/buttons/in/primary"), secondary = action("/actions/buttons/in/secondary");
    auto ejectClip = action("/actions/buttons/in/ejectclip");
    auto update = [&](uint32_t count = 2) {
        vrrt::set_last_poses(p);
        assert(in->UpdateActionState(sets, sizeof sets[0], count) == VRInputError_None);
    };
    auto digital = [&](VRActionHandle_t action) {
        InputDigitalActionData_t d;
        assert(in->GetDigitalActionData(action, &d, sizeof d, 0) == VRInputError_None); return d;
    };
    auto analog = [&](VRActionHandle_t action) {
        InputAnalogActionData_t d;
        assert(in->GetAnalogActionData(action, &d, sizeof d, 0) == VRInputError_None); return d;
    };
    p.inputsFresh = true;
    p.hands.leftButtons = p.hands.rightButtons = 0;
    p.hands.leftStickX = p.hands.leftStickY = p.hands.rightStickX = p.hands.rightStickY = 0;
    update(); assert(!digital(walk).bState && !digital(stickClick).bState);
    p.hands.leftStickY = .8f; update();
    assert(digital(walk).bState && digital(walk).bChanged && !digital(stickClick).bState);
    assert(near(analog(direction).y, (.8f - .05f) / (.9f - .05f)));
    p.hands.leftStickY = .45f; update(); assert(digital(walk).bState && !digital(walk).bChanged);
    p.hands.leftStickY = .39f; update(); assert(!digital(walk).bState && digital(walk).bChanged);
    p.hands.leftStickY = 0; p.hands.leftStickX = -.95f; update();
    assert(digital(walk).bState && near(analog(direction).x, -1));
    p.hands.leftStickX = 0; p.hands.leftStickY = -1; update();
    assert(digital(walk).bState && near(analog(direction).y, -1));
    p.hands.leftStickY = 0; p.hands.leftButtons = vrrt::kBtnStickClick << 8; update();
    assert(!digital(walk).bState && digital(stickClick).bState);
    p.hands.leftButtons = 0;
    p.hands.leftStickY = .3f; update(); assert(digital(walkDefault).bState && !digital(walk).bState);
    p.hands.leftStickY = .23f; update(); assert(digital(walkDefault).bState);
    p.hands.leftStickY = .19f; update(); assert(!digital(walkDefault).bState);
    p.hands.leftStickY = 0;
    for (uint32_t button : {vrrt::kBtnPrimary, vrrt::kBtnSecondary}) {
        p.hands.leftButtons = button << 8; p.hands.rightButtons = button;
        p.hands.rightStickX = .1f; update(); // centre drift while a face button is pressed
        assert(digital(button == vrrt::kBtnPrimary ? primary : secondary).bState);
        assert(digital(ejectClip).bState == (button == vrrt::kBtnPrimary));
        assert(!digital(walk).bState && !digital(turnLeft).bState && !digital(turnRight).bState);
        assert(near(analog(continuous).x, 0) && near(analog(continuous).y, 0));
        // SteamVR Input owns these buttons once an action manifest is set.
        // Publishing the same press through the legacy API also lets the
        // engine's old button mappings perform a second, unrelated action.
        for (uint32_t device : {1u, 2u}) {
            VRControllerState_t legacy; memset(&legacy, 0xff, sizeof legacy);
            assert(!vrrt_system()->GetControllerState(device, &legacy, sizeof legacy));
            assert(!legacy.ulButtonPressed && !legacy.ulButtonTouched);
            for (const auto &axis : legacy.rAxis) assert(axis.x == 0 && axis.y == 0);
            TrackedDevicePose_t pose; memset(&pose, 0xff, sizeof pose);
            assert(!vrrt_system()->GetControllerStateWithPose(TrackingUniverseStanding, device, &legacy, sizeof legacy, &pose));
            assert(!pose.bPoseIsValid && !pose.bDeviceIsConnected);
        }
        assert(digital(ejectClip).bState == (button == vrrt::kBtnPrimary));
    }
    p.hands.leftButtons = p.hands.rightButtons = 0;
    p.hands.rightStickX = .9f; update();
    assert(digital(turnRight).bState && !digital(turnLeft).bState && !digital(clickTurn).bState);
    assert(near(analog(continuous).x, (.9f - .75f) / (1.f - .75f)));
    p.hands.rightButtons = vrrt::kBtnStickClick; update(); assert(digital(clickTurn).bState);
    p.hands.rightButtons = 0; p.hands.rightStickX = -.9f; update();
    assert(digital(turnLeft).bState && !digital(turnRight).bState && analog(continuous).x < 0);
    sets[0] = sets[1]; update(1);
    assert(!digital(turnLeft).bActive && !analog(continuous).bActive); // no movement set in menus
    sets[0].ulActionSet = 0;
    assert(in->GetActionSetHandle("/actions/move", &sets[0].ulActionSet) == VRInputError_None);
    p.inputsFresh = false; update();
    assert(!digital(walk).bState && !digital(turnLeft).bState && near(analog(continuous).x, 0));
}

static void test_loading_prompt(const char *framePath) {
    const std::string file = std::string(framePath) + ".prompt";
    setenv("VERANTYX_GAME_PROMPT_FILE", file.c_str(), 1);
    const std::string loading = std::string(framePath) + ".loading";
    setenv("VERANTYX_NATIVE_LOADING_FILE", loading.c_str(), 1);
    auto loading_text = [&] { std::ifstream f(loading); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); };
    auto text = [&] { std::ifstream f(file); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); };
    Mailbox mailbox;
    uint64_t handle = 0;
    assert(mailbox.RegisterMailbox("hlvr/mailbox", &handle) == 0);
    char buf[128]; uint32_t needed = 0;
    assert(mailbox.ReceiveMessage(handle, buf, 1, &needed) == 1 && needed > 1);
    assert(mailbox.ReceiveMessage(handle, buf, sizeof buf, &needed) == 0 && std::string(buf) == "{\"type\":\"ready\"}");
    auto message = [&](const char *value) { assert(mailbox.SendMessage_(handle, "hlvr/interstitials", value) == 0); };
    message("{\"type\":\"begin_loading\",\"mapName\":\"startup\"}");
    assert(loading_text().find("\"visible\":false") != std::string::npos);
    message("{\"type\":\"show_message\",\"text\":\"Press Trigger To Start\"}");
    vrrt::loading_log("Someone paused the game\n");
    assert(text().empty()); // initial boot stays black, even if it receives a hint
    message("{\"type\":\"end_loading\"}");
    message("{\"type\":\"begin_loading\",\"mapName\":\"synthetic\"}");
    assert(access(file.c_str(), F_OK) != 0);
    assert(loading_text().find("\"visible\":true") != std::string::npos);
    message("{\"type\":\"show_message\",\"text\":\"Press Right Hand To Start\"}");
    assert(text().empty() && loading_text().find("Press Right") == std::string::npos);
    vrrt::loading_log("Someone unpaused the game");
    vrrt::loading_log("Someone paused the game later");
    assert(text().empty());
    vrrt::loading_log("Medved paused the game\n");
    assert(text() == "Press Right Hand To Start");
    assert(loading_text().find("\"messageText\":\"Press Right Hand To Start\"") != std::string::npos);
    message("{\"type\":\"set_paths\",\"webroot\":\"show_message\"}");
    assert(text() == "Press Right Hand To Start");
    vrrt::loading_log("Medved unpaused the game");
    assert(text().empty() && loading_text().find("Press Right") == std::string::npos);
    message("{\"type\":\"end_loading\"}");
    assert(access(loading.c_str(), F_OK) != 0);
    message("{\"type\":\"show_message\",\"text\":\"Press Left Hand To Start\"}");
    vrrt::loading_log("Someone paused the game");
    assert(text().empty()); // late messages and a menu pause cannot create a load
    message("{\"type\":\"begin_loading\",\"mapName\":\"synthetic\"}");
    message("{\"type\":\"show_message\",\"text\":\"Press Trigger To Start\"}");
    assert(text().empty()); // a new load cannot reuse the preceding pause
    message("{\"type\":\"show_message\",\"text\":\"Localized trigger hint\"}");
    assert(text().empty());
    vrrt::loading_log("Different player paused the game");
    assert(text() == "Localized trigger hint");
    message("{\"type\":\"begin_loading\",\"mapName\":\"synthetic\"}");
    vrrt::loading_log("Different player paused the game");
    message("{\"type\":\"show_message\",\"text\":\"Press Left Hand To Start\"}");
    assert(text() == "Press Left Hand To Start"); // either event order is valid
    message("{\"type\":\"show_message\",\"text\":\"Ready\\n\\\"Test\\\"\"}");
    assert(text() == "Ready\n\"Test\"");
    // The room and its transform must reach the renderer; ready is acknowledged
    // only after the matching request has actually loaded, never by a timer.
    const std::string stage = std::string(framePath) + ".stage";
    setenv("VERANTYX_NATIVE_STAGE_FILE", stage.c_str(), 1);
    HmdMatrix34_t transform; vrrt::identity34(&transform); transform.m[0][3] = 2;
    auto settings = DefaultStageRenderSettings(); settings.m_PrimaryColor.r = 0.2f;
    auto compositor = vrrt_compositor();
    assert(compositor->SetStageOverride_Async("/synthetic/room\".obj", &transform, &settings, sizeof settings) == VRCompositorError_None);
    json::Value room; assert(json::parse_file(stage, room));
    assert(room["model"].str == "/synthetic/room\".obj");
    assert(room["transform"].arr[3].num == 2 && near(room["settings"].arr[0].num, 0.2f));
    VREvent_t event;
    while (vrrt::pop_event(&event)) assert(event.eventType != VREvent_Compositor_StageOverrideReady);
    { std::ofstream f(stage + ".ready"); f << room["token"].num + 1; }
    assert(!vrrt::pop_event(&event));
    { std::ofstream f(stage + ".ready"); f << (uint64_t)room["token"].num; }
    assert(vrrt::pop_event(&event) && event.eventType == VREvent_Compositor_StageOverrideReady);
    assert(!vrrt::pop_event(&event));
    transform.m[1][1] = std::numeric_limits<float>::quiet_NaN();
    assert(compositor->SetStageOverride_Async("room.obj", &transform, nullptr, 0) == VRCompositorError_RequestFailed);
    compositor->ClearStageOverride(); assert(access(stage.c_str(), F_OK) != 0);
    unlink((stage + ".ready").c_str()); unsetenv("VERANTYX_NATIVE_STAGE_FILE");
    mailbox.UnregisterMailbox(handle); assert(access(file.c_str(), F_OK) != 0);
    assert(access(loading.c_str(), F_OK) != 0);
    assert(mailbox.ReceiveMessage(handle, buf, sizeof buf, &needed) == 2);
    unsetenv("VERANTYX_GAME_PROMPT_FILE");
    unsetenv("VERANTYX_NATIVE_LOADING_FILE");
}

static void test_pixels() {
    Readback::Staging s;
    uint8_t rgba[] = {255, 10, 0, 255, 0, 20, 255, 255, 20, 30, 40, 255, 50, 60, 70, 255};
    s.map = rgba; s.w = s.h = 2; s.fmt = VK_FORMAT_R8G8B8A8_UNORM;
    std::array<uint8_t, 32> out = {}; uint32_t fmt = 0;
    convert_eye(s, out.data(), 16, 0, false, &fmt, 2, 2);
    assert(fmt == 87 && out[0] == 0 && out[2] == 255 && out[16] == 40);
    convert_eye(s, out.data(), 16, 2, true, &fmt, 2, 2, true);
    assert(out[8] == 70 && out[10] == 50 && out[12] == 40 && out[14] == 20);
    uint32_t packed = (1023u << 20) | 1023u; s.map = &packed; s.w = s.h = 1; s.fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    convert_eye(s, out.data(), 16, 0, false, &fmt, 1, 1);
    assert(out[0] == 255 && out[1] == 0 && out[2] == 255 && fmt == 87);
    uint16_t half[] = {0x3c00, 0, 0x3800, 0x3c00}; s.map = half; s.fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    convert_eye(s, out.data(), 16, 0, false, &fmt, 1, 1);
    assert(out[0] == 128 && out[2] == 255 && fmt == 87);
    uint8_t stereo[4 * 2 * 4] = {};
    for (int y = 0; y < 2; y++) for (int x = 0; x < 4; x++)
        for (int c = 0; c < 3; c++) stereo[(y * 4 + x) * 4 + c] = x < 2 ? 48 : (y ? 255 : 0);
    const auto left = pixel_summary(stereo, 4, 2, 0), right = pixel_summary(stereo, 4, 2, 1);
    assert(left.minimum == 48 && left.maximum == 48 && left.mean == 48 && left.deviation == 0);
    assert(right.minimum == 0 && right.maximum == 255 && right.mean == 127.5 && right.deviation == 127.5);
    assert(left.hash != right.hash && pixel_summary(stereo, 4, 2, 0).hash == left.hash);
    std::vector<uint8_t> logo(128*64*4);
    for (size_t i = 0; i < logo.size(); i += 4) { logo[i] = logo[i+2] = logo[i+3] = 255; }
    assert(startup_clear(logo.data(),128,64,0));
    // The sample grid misses this authored pixel; the full verification must not.
    logo[0] = logo[1] = logo[2] = 0;
    assert(!startup_clear(logo.data(),128,64,0) && startup_clear(logo.data(),128,64,1));
}

// The fake queue checks that no command buffer is reset before its fence was
// waited, including partial frames and the timeout path. No Vulkan is loaded.
static std::mutex vkMu;
static std::map<VkFence, VkCommandBuffer> inFlight;
static std::atomic<bool> timeoutWait{false};
static VkResult VKAPI_PTR reset_command(VkCommandBuffer cmd, VkCommandBufferResetFlags) {
    std::lock_guard<std::mutex> l(vkMu);
    for (auto &f : inFlight) assert(f.second != cmd);
    return VK_SUCCESS;
}
static VkResult VKAPI_PTR begin_command(VkCommandBuffer, const VkCommandBufferBeginInfo *) { return VK_SUCCESS; }
static VkResult VKAPI_PTR end_command(VkCommandBuffer) { return VK_SUCCESS; }
static void VKAPI_PTR barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *, uint32_t, const VkImageMemoryBarrier *) {}
static void VKAPI_PTR copy_image(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t, const VkBufferImageCopy *) {}
static VkResult VKAPI_PTR reset_fences(VkDevice, uint32_t n, const VkFence *f) {
    std::lock_guard<std::mutex> l(vkMu); for (uint32_t i = 0; i < n; i++) assert(!inFlight.count(f[i])); return VK_SUCCESS;
}
static VkResult VKAPI_PTR submit_queue(VkQueue, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    std::lock_guard<std::mutex> l(vkMu); assert(n == 1 && f && !inFlight.count(f)); inFlight[f] = s->pCommandBuffers[0]; return VK_SUCCESS;
}
static VkResult VKAPI_PTR wait_fences(VkDevice, uint32_t n, const VkFence *f, VkBool32, uint64_t) {
    if (timeoutWait) return VK_TIMEOUT;
    std::lock_guard<std::mutex> l(vkMu); for (uint32_t i = 0; i < n; i++) assert(inFlight.erase(f[i]) == 1); return VK_SUCCESS;
}
static void VKAPI_PTR destroy_pool(VkDevice, VkCommandPool, const VkAllocationCallbacks *) {
    std::lock_guard<std::mutex> l(vkMu); assert(inFlight.empty());
}
static void VKAPI_PTR destroy_fence(VkDevice, VkFence f, const VkAllocationCallbacks *) {
    std::lock_guard<std::mutex> l(vkMu); assert(!inFlight.count(f));
}
static void VKAPI_PTR unmap_memory(VkDevice, VkDeviceMemory) {}
static void VKAPI_PTR destroy_buffer(VkDevice, VkBuffer, const VkAllocationCallbacks *) {}
static void VKAPI_PTR free_memory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *) {}
static void fake_queue(Compositor &c, uint8_t pixels[2][2][64]) {
    auto &r = c.m_rb; r.ok = true; r.dev = (VkDevice)1; r.queue = (VkQueue)2;
    r.pool = (VkCommandPool)3; r.cmd = (VkCommandBuffer)100; r.fence = (VkFence)200;
    r.pResetCommandBuffer = reset_command; r.pBeginCommandBuffer = begin_command; r.pEndCommandBuffer = end_command;
    r.pCmdPipelineBarrier = barrier; r.pCmdCopyImageToBuffer = copy_image; r.pResetFences = reset_fences;
    r.pQueueSubmit = submit_queue; r.pWaitForFences = wait_fences;
    r.pDestroyCommandPool = destroy_pool; r.pDestroyFence = destroy_fence;
    r.pUnmapMemory = unmap_memory; r.pDestroyBuffer = destroy_buffer; r.pFreeMemory = free_memory;
    for (int i = 0; i < 2; i++) for (int e = 0; e < 2; e++) {
        auto &sl = r.slot[i]; auto &s = sl.eye[e];
        sl.cmd[e] = (VkCommandBuffer)(uintptr_t)(10 + i*2 + e); sl.fence[e] = (VkFence)(uintptr_t)(20 + i*2 + e);
        s.map = pixels[i][e]; s.cap = 64; s.ready = s.coherent = true;
    }
}
static void test_compositor() {
    using namespace vr;
    vrrt::config().sharedFrustum = false;
    uint8_t pixels[2][2][64] = {};
    {
        Compositor c;
        uint8_t pink[64], black[64] = {}, scene[64];
        for (int i = 0; i < 16; i++) {
            pink[4*i] = pink[4*i+2] = pink[4*i+3] = 255; pink[4*i+1] = 0;
            scene[4*i] = uint8_t(i*13); scene[4*i+1] = 40;
            scene[4*i+2] = 60; scene[4*i+3] = 255;
        }
        Readback::Staging left, right;
        left.w = right.w = left.h = right.h = 4;
        left.fmt = right.fmt = VK_FORMAT_B8G8R8A8_UNORM;
        bool flip[2] = {}; float head[16] = {};
        auto publish = [&](uint8_t *l, uint8_t *r) {
            left.map = l; right.map = r;
            c.publish_from(left, right, flip, flip, head, 123);
            auto *f = frame();
            auto *p = reinterpret_cast<uint8_t *>(f) + sizeof *f;
            return std::vector<uint8_t>(p, p + 128);
        };
        publish(black, black); // a black intro must not disable startup protection
        auto out = publish(pink, pink);
        for (int i = 0; i < 32; i++) assert(out[4*i] == 0 && out[4*i+1] == 0 && out[4*i+2] == 0);
        out = publish(scene, pink); // neither eye may show a clear-only frame
        for (int i = 0; i < 32; i++) assert(out[4*i] == 0 && out[4*i+1] == 0 && out[4*i+2] == 0);
        out = publish(scene, scene); assert(out[1] == 40 && out[2] == 60);
        assert(frame()->visionTimestamp == 123);
        // Authored magenta after the intro, including a full-screen fade, survives.
        out = publish(pink, pink); assert(out[0] == 255 && out[1] == 0 && out[2] == 255);
    }
    VRVulkanTextureData_t image = {}; image.m_nImage = 100; image.m_pDevice = (VkDevice)1; image.m_pQueue = (VkQueue)2;
    image.m_nWidth = image.m_nHeight = 4; image.m_nFormat = VK_FORMAT_R8G8B8A8_UNORM; image.m_nSampleCount = 1;
    Texture_t texture = { &image, TextureType_Vulkan, ColorSpace_Auto };
    {
        Compositor c; fake_queue(c, pixels);
        VRTextureBounds_t edge = {-1e-7f, 0, 1.0000001f, 1};
        for (uint32_t size : {4u, 2u, 4u}) {
            image.m_nWidth = image.m_nHeight = size;
            c.WaitGetPoses(nullptr, 0, nullptr, 0);
            assert(c.Submit(Eye_Left, &texture, &edge, Submit_Default) == VRCompositorError_None);
            assert(c.Submit(Eye_Right, &texture, &edge, Submit_Default) == VRCompositorError_None);
        }
        c.shutdown(); assert(c.m_published == 3 && inFlight.empty());
    }
    {
        Compositor c; fake_queue(c, pixels); c.WaitGetPoses(nullptr, 0, nullptr, 0);
        VRTextureBounds_t outside = {0, 0, 1.01f, 1}, empty = {.5f, 0, .5f, 1};
        assert(c.Submit(Eye_Left, &texture, &outside, Submit_Default) == VRCompositorError_InvalidBounds);
        assert(c.Submit(Eye_Left, &texture, &empty, Submit_Default) == VRCompositorError_InvalidBounds);
        outside.uMax = std::numeric_limits<float>::quiet_NaN();
        assert(c.Submit(Eye_Left, &texture, &outside, Submit_Default) == VRCompositorError_InvalidBounds);
        assert(c.Submit(Eye_Left, &texture, nullptr, Submit_Default) == VRCompositorError_None);
        assert(c.Submit(Eye_Right, &texture, nullptr, Submit_Default) == VRCompositorError_None);
        c.shutdown(); assert(c.m_published == 1 && c.m_boundsErrors == 3 && inFlight.empty());
    }
    {
        Compositor c; fake_queue(c, pixels); c.WaitGetPoses(nullptr, 0, nullptr, 0);
        c.m_framePose.visionTimestamp = 123.125;
        assert(c.Submit(Eye_Right, &texture, nullptr, Submit_Default) == VRCompositorError_None);
        assert(c.Submit(Eye_Right, &texture, nullptr, Submit_Default) == VRCompositorError_AlreadySubmitted);
        assert(c.Submit(Eye_Left, &texture, nullptr, Submit_Default) == VRCompositorError_None);
        c.shutdown(); assert(c.m_published == 1 && frame()->format == 87 && frame()->visionTimestamp == 123.125);
        assert(!c.m_rb.pool && !c.m_rb.slot[0].fence[0]);
        c.resume(); fake_queue(c, pixels); c.WaitGetPoses(nullptr, 0, nullptr, 0);
        c.Submit(Eye_Left, &texture, nullptr, Submit_Default); c.Submit(Eye_Right, &texture, nullptr, Submit_Default);
        c.shutdown(); assert(c.m_published == 2 && inFlight.empty()); // OpenVR Cleanup / Init
    }
    {
        Compositor c; fake_queue(c, pixels); c.WaitGetPoses(nullptr, 0, nullptr, 0);
        c.Submit(Eye_Left, &texture, nullptr, Submit_Default);
        c.WaitGetPoses(nullptr, 0, nullptr, 0);
        c.Submit(Eye_Right, &texture, nullptr, Submit_Default);
        c.shutdown(); assert(c.m_published == 0 && inFlight.empty()); // cannot pair eyes across frames
    }
    {
        Compositor c; fake_queue(c, pixels); c.m_async = false;
        for (int e = 0; e < 2; e++) c.m_rb.eye[e] = c.m_rb.slot[0].eye[e];
        c.WaitGetPoses(nullptr, 0, nullptr, 0);
        c.Submit(Eye_Right, &texture, nullptr, Submit_Default); c.Submit(Eye_Left, &texture, nullptr, Submit_Default);
        c.shutdown(); assert(c.m_published == 1 && inFlight.empty());
    }
    {
        Compositor c; fake_queue(c, pixels); c.m_async = false; timeoutWait = true;
        for (int e = 0; e < 2; e++) c.m_rb.eye[e] = c.m_rb.slot[0].eye[e];
        c.WaitGetPoses(nullptr, 0, nullptr, 0);
        c.Submit(Eye_Left, &texture, nullptr, Submit_Default); c.Submit(Eye_Right, &texture, nullptr, Submit_Default);
        c.shutdown(); assert(c.m_published == 0 && !c.m_rb.syncUsable && inFlight.size() == 1);
        timeoutWait = false; inFlight.clear();
    }
    {
        Compositor c; fake_queue(c, pixels); timeoutWait = true;
        for (int i = 0; i < 2; i++) {
            c.WaitGetPoses(nullptr, 0, nullptr, 0);
            c.Submit(Eye_Left, &texture, nullptr, Submit_Default); c.Submit(Eye_Right, &texture, nullptr, Submit_Default);
        }
        c.shutdown(); assert(c.m_published == 0);
        for (auto &s : c.m_rb.slot) assert(s.state == 3); // outstanding commands stay quarantined
        timeoutWait = false; inFlight.clear();
    }
    {
        Compositor c; fake_queue(c, pixels);
        VRTextureBounds_t bounds = { std::numeric_limits<float>::quiet_NaN(), 0, 1, 1 };
        assert(c.Submit(Eye_Left, &texture, &bounds, Submit_Default) == VRCompositorError_InvalidBounds);
        assert(c.Submit(Eye_Left, nullptr, nullptr, Submit_Default) == VRCompositorError_InvalidTexture);
        alignas(Compositor_FrameTiming) struct { uint32_t size, canary; } small = {4, 0x12345678};
        assert(c.GetFrameTiming((Compositor_FrameTiming *)&small, 0) && small.canary == 0x12345678);
    }
}

static void test_legacy_input(vrrt::PoseSet p) {
    using namespace vr;
    assert(vrrt_input()->IsUsingLegacyInput());
    p.inputsFresh = true;
    p.hands.leftButtons = vrrt::kBtnPrimary << 8;
    p.hands.rightButtons = vrrt::kBtnPrimary;
    p.hands.leftStickX = -.5f; p.hands.rightStickY = .8f;
    vrrt::set_last_poses(p);
    for (uint32_t device : {1u, 2u}) {
        VRControllerState_t state = {};
        TrackedDevicePose_t pose = {};
        assert(vrrt_system()->GetControllerStateWithPose(TrackingUniverseStanding, device, &state, sizeof state, &pose));
        assert(state.ulButtonPressed & ButtonMaskFromId(k_EButton_A));
        assert(pose.bPoseIsValid);
        assert(device == 1 ? near(state.rAxis[0].x, -.5f) : near(state.rAxis[0].y, .8f));
    }
    assert(!vrrt_system()->GetControllerState(1, nullptr, sizeof(VRControllerState_t)));
}

static void test_quit(const char *framePath) {
    using namespace vr;
    std::string request = std::string(framePath) + ".quit";
    setenv("VERANTYX_NATIVE_QUIT_FILE", request.c_str(), 1);
    VREvent_t event = {};
    while (vrrt::pop_event(&event)) {}
    vrrt_system()->AcknowledgeQuit_Exiting();
    assert(access((request + ".ack").c_str(), F_OK) != 0);
    { std::ofstream f(request); f << "partial"; }
    assert(!vrrt::pop_event(&event));
    for (int i = 0; i < 256; i++) {
        VREvent_t input = {}; input.eventType = VREvent_ButtonPress;
        vrrt::push_event(input);
    }
    { std::ofstream f(request); f << "quit\n"; }
    assert(vrrt_system()->PollNextEvent(&event, sizeof event));
    assert(event.eventType == VREvent_Quit && event.trackedDeviceIndex == k_unTrackedDeviceIndexInvalid);
    assert(event.data.process.pid == (uint32_t)getpid() && !event.data.process.bForced);
    assert(access((request + ".delivered").c_str(), F_OK) == 0);
    assert(access((request + ".ack").c_str(), F_OK) != 0);
    vrrt_system()->AcknowledgeQuit_Exiting();
    assert(access((request + ".ack").c_str(), F_OK) == 0);
    { std::ofstream f(request); f << "quit\n"; }
    int remaining = 0;
    while (vrrt::pop_event(&event)) { assert(event.eventType == VREvent_ButtonPress); remaining++; }
    assert(remaining == 256); // quit is neither dropped nor repeated, and preserves queued input
    for (const char *suffix : {"", ".ack", ".delivered"}) unlink((request + suffix).c_str());
    unsetenv("VERANTYX_NATIVE_QUIT_FILE");
}

int main(int argc, char **argv) {
    assert(argc == 3);
    if (!strcmp(argv[1], "--without-shared")) {
        unsetenv("VERANTYX_SHARED_FRAME"); setenv("WINEPREFIX", argv[2], 1);
        vrrt::init(""); assert(!vrrt::shm_ready());
        puts("NativeARM: no implicit writes to a Wine prefix");
        return 0;
    }
    setenv("VERANTYX_SHARED_FRAME", argv[1], 1); unsetenv("VERANTYX_IPD");
    vrrt::init(""); assert(vrrt::shm_ready());
    // The cached frustum is beside this native session's shared map, with no
    // dependency on a Wine prefix or a previously connected Vision Pro.
    assert(near(vrrt::config().tangentsLeft[0], -1.2f) && near(vrrt::config().tangentsRight[1], 1.2f));
    float u0, u1, v0, v1;
    vrrt::eye_crop(0, &u0, &u1, &v0, &v1); assert(near(u0, 0) && near(u1, 5/6.f));
    vrrt::eye_crop(1, &u0, &u1, &v0, &v1); assert(near(u0, 1/6.f) && near(u1, 1));
    auto p = test_shared_memory();
    test_legacy_input(p);
    test_input(argv[2], p); test_mixed_input_sources(p); test_pose_contention(p);
    test_locomotion(argv[2], p); test_loading_prompt(argv[1]); test_pixels(); test_compositor();
    test_quit(argv[1]);
    puts("NativeARM: shared memory, pose contention, mixed hand/gamepad input, locomotion, binding events, loading prompts, skeleton, haptics, compositor and cooperative quit tests passed");
}
