// IVRSystem: what the headset and controllers are, where they are, and the
// display's optics.
#include <atomic>
#include <cmath>
#include "klvr_internal.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace klvr;
namespace klvr { float klvr_game_hz(float panel); }
namespace klvr { namespace impl {

static klvr_display display() {
    klvr_display d{};
    klvr_plat_display(&d);
    return d;
}

// ---- display --------------------------------------------------------------
void IVRSystem__GetRecommendedRenderTargetSize(void *, uint32_t *w, uint32_t *h) {
    klvr_display d = display();
    // KLVR_RENDER_SCALE multiplies the platform's recommendation. It is the
    // lever the performance work pulls, so it lives here, in the one answer
    // every renderer sizes itself from.
    float s = env_float("KLVR_RENDER_SCALE", 1.0f);
    if (w) *w = (uint32_t)std::lround(d.eye_w * s);
    if (h) *h = (uint32_t)std::lround(d.eye_h * s);
}

// OpenVR's raw projection: tangents with left negative, and top/bottom in a
// y-down sense, so "top" is the DOWN tangent negated and "bottom" is the UP
// tangent. (That is what makes the matrix below map the up edge to +1.)
void IVRSystem__GetProjectionRaw(void *, vr::EVREye eye, float *l, float *r, float *t, float *b) {
    klvr_display d = display();
    int e = eye == vr::Eye_Right ? 1 : 0;
    if (l) *l = -d.tan[e][0];
    if (r) *r = d.tan[e][1];
    // SteamVR's convention, confusing as it reads: "top" is minus the DOWN
    // tangent and "bottom" is the UP one. Games feed these straight into
    // ComposeProjection (below), where ndc y = +1 lands on tan = bottom, so
    // this is what makes the frustum they render with span U up and D down.
    // OpenComposite measured it against real SteamVR (Rift S: SteamVR top
    // -1.150 = OpenXR angleDown); the opposite mirrors each eye's frustum
    // vertically, which the game then renders and the compositor places
    // with the real one: the world sits a few degrees off and swims on pitch.
    if (t) *t = -d.tan[e][3];
    if (b) *b = d.tan[e][2];
}

// The matrix SteamVR returns: built from the raw tangents exactly as
// hellovr_vulkan's ComposeProjection does, with Direct3D depth (0..1).
vr::HmdMatrix44_t IVRSystem__GetProjectionMatrix(void *self, vr::EVREye eye, float zn, float zf) {
    float l, r, t, b;
    IVRSystem__GetProjectionRaw(self, eye, &l, &r, &t, &b);
    float idx = 1.0f / (r - l), idy = 1.0f / (b - t);
    float sx = r + l, sy = b + t;
    vr::HmdMatrix44_t m;
    memset(&m, 0, sizeof m);
    m.m[0][0] = 2 * idx; m.m[0][2] = sx * idx;
    m.m[1][1] = 2 * idy; m.m[1][2] = sy * idy;
    m.m[2][2] = -zf / (zf - zn); m.m[2][3] = -(zf * zn) / (zf - zn);
    m.m[3][2] = -1.0f;
    return m;
}

vr::HmdMatrix34_t IVRSystem__GetEyeToHeadTransform(void *, vr::EVREye eye) {
    klvr_display d = display();
    return to_mat34(to_pose(d.eye_in_head[eye == vr::Eye_Right ? 1 : 0]));
}

bool IVRSystem__ComputeDistortion(void *, vr::EVREye, float u, float v, vr::DistortionCoordinates_t *out) {
    // The platform compositor owns lens correction; to the game the display
    // is undistorted.
    if (out) {
        for (int i = 0; i < 2; i++) {
            out->rfRed[i] = out->rfGreen[i] = out->rfBlue[i] = i == 0 ? u : v;
        }
    }
    return true;
}

vr::HiddenAreaMesh_t IVRSystem__GetHiddenAreaMesh(void *, vr::EVREye, vr::EHiddenAreaMeshType) {
    // No mask: every pixel of the eye texture can be visible after the
    // compositor's reprojection. (A mask is a performance win the graphics
    // work can add later from CompositorServices' view geometry.)
    return { nullptr, 0 };
}

bool IVRSystem__GetTimeSinceLastVsync(void *, float *seconds, uint64_t *frame) {
    // The counter counts display VSYNCS, not frames the game rendered: a game
    // that times itself by vsyncs elapsed (period x counter delta) runs in slow
    // motion when it renders below the display rate if this counts its frames.
    // Vsyncs are laid on a grid phased to the frame clock's display times.
    Frame f = current_frame();
    klvr_display d = display();
    double period = d.refresh_hz > 0 ? 1.0 / d.refresh_hz : 1.0 / 90;
    static std::atomic<double> anchor{0};
    double a = anchor.load();
    if (a == 0 && f.display_time > 0) { a = f.display_time; anchor.store(a); }
    double now = klvr_plat_now();
    if (a == 0) a = now;
    double n = std::floor((now - a) / period);
    double since = (now - a) - n * period;
    if (seconds) *seconds = (float)since;
    if (frame) *frame = (uint64_t)(int64_t)(n + 1e6);   // offset keeps it positive before the anchor
    return true;
}

bool IVRSystem__IsDisplayOnDesktop(void *) { return false; }
bool IVRSystem__SetDisplayVisibility(void *, bool) { return false; }
void IVRSystem__GetOutputDevice(void *, uint64_t *dev, vr::ETextureType, void *) {
    // For Vulkan this is the VkPhysicalDevice the compositor wants. There is
    // exactly one GPU; zero tells the game to pick it itself.
    if (dev) *dev = 0;
}
void IVRSystem__GetDXGIOutputInfo(void *, int32_t *idx) { if (idx) *idx = 0; }

// ---- tracking -------------------------------------------------------------
void IVRSystem__GetDeviceToAbsoluteTrackingPose(void *, vr::ETrackingUniverseOrigin u,
                                                float seconds_from_now,
                                                vr::TrackedDevicePose_t *poses, uint32_t count) {
    device_poses(klvr_plat_now() + seconds_from_now, u, poses, count);
}

void IVRSystem__ResetSeatedZeroPose(void *) {
    reset_seated_zero();
    push_event(vr::VREvent_SeatedZeroPoseReset, kHmd);
}

vr::HmdMatrix34_t IVRSystem__GetSeatedZeroPoseToStandingAbsoluteTrackingPose(void *) {
    return to_mat34(pinv(universe_from_floor(vr::TrackingUniverseSeated)));
}

vr::HmdMatrix34_t IVRSystem__GetRawZeroPoseToStandingAbsoluteTrackingPose(void *) {
    return identity34();
}

uint32_t IVRSystem__GetSortedTrackedDeviceIndicesOfClass(void *self, vr::ETrackedDeviceClass cls,
                                                         vr::TrackedDeviceIndex_t *out, uint32_t cap,
                                                         vr::TrackedDeviceIndex_t relative_to) {
    (void)relative_to;
    uint32_t n = 0;
    for (vr::TrackedDeviceIndex_t d = 0; d < kDeviceCount; d++) {
        if (IVRSystem__GetTrackedDeviceClass(self, d) != cls) continue;
        if (out && n < cap) out[n] = d;
        n++;
    }
    return n;
}

vr::EDeviceActivityLevel IVRSystem__GetTrackedDeviceActivityLevel(void *, vr::TrackedDeviceIndex_t) {
    return vr::k_EDeviceActivityLevel_UserInteraction;
}

void IVRSystem__ApplyTransform(void *, vr::TrackedDevicePose_t *out, const vr::TrackedDevicePose_t *in,
                               const vr::HmdMatrix34_t *xf) {
    if (!out || !in || !xf) return;
    *out = *in;
    Pose p = pmul(from_mat34(in->mDeviceToAbsoluteTracking), from_mat34(*xf));
    out->mDeviceToAbsoluteTracking = to_mat34(p);
}

vr::TrackedDeviceIndex_t IVRSystem__GetTrackedDeviceIndexForControllerRole(void *, vr::ETrackedControllerRole role) {
    if (role == vr::TrackedControllerRole_LeftHand) return kLeftHand;
    if (role == vr::TrackedControllerRole_RightHand) return kRightHand;
    return vr::k_unTrackedDeviceIndexInvalid;
}

vr::ETrackedControllerRole IVRSystem__GetControllerRoleForTrackedDeviceIndex(void *, vr::TrackedDeviceIndex_t d) {
    if (d == kLeftHand) return vr::TrackedControllerRole_LeftHand;
    if (d == kRightHand) return vr::TrackedControllerRole_RightHand;
    return vr::TrackedControllerRole_Invalid;
}

vr::ETrackedDeviceClass IVRSystem__GetTrackedDeviceClass(void *, vr::TrackedDeviceIndex_t d) {
    if (d == kHmd) return vr::TrackedDeviceClass_HMD;
    if (d == kLeftHand || d == kRightHand) return vr::TrackedDeviceClass_Controller;
    return vr::TrackedDeviceClass_Invalid;
}

bool IVRSystem__IsTrackedDeviceConnected(void *, vr::TrackedDeviceIndex_t d) {
    if (d == kHmd) return true;
    int hand = hand_of_device(d);
    if (hand < 0) return false;
    klvr_controller c{};
    klvr_plat_controller(hand, &c);
    return c.connected != 0;
}

// ---- properties -----------------------------------------------------------
//
// The controllers present themselves as Oculus Touch. Alyx ships default
// bindings for Touch, and the PSVR2 Sense layout (stick, two face buttons,
// trigger, grip, capacitive touch) is the same shape. KLVR_CONTROLLER_TYPE
// swaps the type (e.g. "knuckles") without a rebuild.
static std::string controller_type() {
    const char *v = getenv("KLVR_CONTROLLER_TYPE");
    return v && *v ? v : "oculus_touch";
}

static const char *string_prop(vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p, std::string &tmp) {
    bool left = d == kLeftHand;
    if (d == kHmd) {
        switch (p) {
        case vr::Prop_TrackingSystemName_String: return "klepton";
        case vr::Prop_ModelNumber_String: return "Apple Vision Pro";
        case vr::Prop_ManufacturerName_String: return "Apple";
        case vr::Prop_SerialNumber_String: return "AVP-KLEPTON-0";
        case vr::Prop_RenderModelName_String: return "generic_hmd";
        case vr::Prop_RegisteredDeviceType_String: return "klepton/AVP-KLEPTON-0";
        case vr::Prop_ExpectedControllerType_String: tmp = controller_type(); return tmp.c_str();
        case vr::Prop_InputProfilePath_String: return "{klepton}/input/hmd_profile.json";
        default: return nullptr;
        }
    }
    if (hand_of_device(d) < 0) return nullptr;
    bool touch = controller_type() == "oculus_touch";
    switch (p) {
    case vr::Prop_TrackingSystemName_String: return touch ? "oculus" : "klepton";
    case vr::Prop_ModelNumber_String:
        return touch ? (left ? "Oculus Quest2 (Left Controller)" : "Oculus Quest2 (Right Controller)")
                     : (left ? "PSVR2 Sense (Left)" : "PSVR2 Sense (Right)");
    case vr::Prop_ManufacturerName_String: return touch ? "Oculus" : "Sony";
    case vr::Prop_SerialNumber_String: return left ? "SENSE-LEFT" : "SENSE-RIGHT";
    case vr::Prop_RenderModelName_String:
        return touch ? (left ? "oculus_quest2_controller_left" : "oculus_quest2_controller_right")
                     : (left ? "sense_left" : "sense_right");
    case vr::Prop_RegisteredDeviceType_String:
        return left ? "oculus/SENSE-LEFT_Controller_Left" : "oculus/SENSE-RIGHT_Controller_Right";
    case vr::Prop_ControllerType_String: tmp = controller_type(); return tmp.c_str();
    case vr::Prop_InputProfilePath_String:
        tmp = touch ? "{oculus}/input/touch_profile.json" : "{klepton}/input/" + controller_type() + "_profile.json";
        return tmp.c_str();
    default: return nullptr;
    }
}

uint32_t IVRSystem__GetStringTrackedDeviceProperty(void *, vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p,
                                                   char *buf, uint32_t size, vr::ETrackedPropertyError *err) {
    std::string tmp;
    const char *s = string_prop(d, p, tmp);
    if (!s) {
        if (err) *err = d < kDeviceCount ? vr::TrackedProp_UnknownProperty : vr::TrackedProp_InvalidDevice;
        if (buf && size) buf[0] = 0;
        return 0;
    }
    uint32_t need = put_string(s, buf, size);
    if (err) *err = need > size ? vr::TrackedProp_BufferTooSmall : vr::TrackedProp_Success;
    return need;
}

float IVRSystem__GetFloatTrackedDeviceProperty(void *, vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p,
                                               vr::ETrackedPropertyError *err) {
    if (err) *err = vr::TrackedProp_Success;
    if (d == kHmd) {
        klvr_display disp = display();
        switch (p) {
        case vr::Prop_DisplayFrequency_Float: return klvr_game_hz(disp.refresh_hz > 0 ? disp.refresh_hz : 90.0f);
        case vr::Prop_UserIpdMeters_Float: {
            Vec3 a = to_pose(disp.eye_in_head[0]).p, b = to_pose(disp.eye_in_head[1]).p;
            Vec3 dd = vsub(b, a);
            return std::sqrt(dd.x * dd.x + dd.y * dd.y + dd.z * dd.z);
        }
        case vr::Prop_SecondsFromVsyncToPhotons_Float: return 0.0f;
        case vr::Prop_UserHeadToEyeDepthMeters_Float: return 0.0f;
        default: break;
        }
    }
    if (hand_of_device(d) >= 0 && p == vr::Prop_DeviceBatteryPercentage_Float) return 1.0f;
    if (err) *err = d < kDeviceCount ? vr::TrackedProp_UnknownProperty : vr::TrackedProp_InvalidDevice;
    return 0.0f;
}

int32_t IVRSystem__GetInt32TrackedDeviceProperty(void *self, vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p,
                                                 vr::ETrackedPropertyError *err) {
    if (err) *err = vr::TrackedProp_Success;
    if (p == vr::Prop_DeviceClass_Int32 && d < kDeviceCount) return IVRSystem__GetTrackedDeviceClass(self, d);
    int hand = hand_of_device(d);
    if (hand >= 0) {
        switch (p) {
        case vr::Prop_ControllerRoleHint_Int32:
            return hand == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand;
        case vr::Prop_Axis0Type_Int32: return vr::k_eControllerAxis_Joystick;
        case vr::Prop_Axis1Type_Int32: return vr::k_eControllerAxis_Trigger;
        case vr::Prop_Axis2Type_Int32: return vr::k_eControllerAxis_Trigger;
        case vr::Prop_Axis3Type_Int32:
        case vr::Prop_Axis4Type_Int32: return vr::k_eControllerAxis_None;
        default: break;
        }
    }
    if (err) *err = d < kDeviceCount ? vr::TrackedProp_UnknownProperty : vr::TrackedProp_InvalidDevice;
    return 0;
}

uint64_t IVRSystem__GetUint64TrackedDeviceProperty(void *, vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p,
                                                   vr::ETrackedPropertyError *err) {
    if (err) *err = vr::TrackedProp_Success;
    if (p == vr::Prop_CurrentUniverseId_Uint64 && d == kHmd) return 1;
    if (hand_of_device(d) >= 0 && p == vr::Prop_SupportedButtons_Uint64) {
        return vr::ButtonMaskFromId(vr::k_EButton_System) | vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu) |
               vr::ButtonMaskFromId(vr::k_EButton_Grip) | vr::ButtonMaskFromId(vr::k_EButton_A) |
               vr::ButtonMaskFromId(vr::k_EButton_Axis0) | vr::ButtonMaskFromId(vr::k_EButton_Axis1) |
               vr::ButtonMaskFromId(vr::k_EButton_Axis2);
    }
    if (err) *err = d < kDeviceCount ? vr::TrackedProp_UnknownProperty : vr::TrackedProp_InvalidDevice;
    return 0;
}

bool IVRSystem__GetBoolTrackedDeviceProperty(void *, vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty p,
                                             vr::ETrackedPropertyError *err) {
    if (err) *err = vr::TrackedProp_Success;
    switch (p) {
    case vr::Prop_DeviceIsWireless_Bool: return true;
    case vr::Prop_DeviceIsCharging_Bool: return false;
    case vr::Prop_DeviceProvidesBatteryStatus_Bool: return false;
    case vr::Prop_HasDisplayComponent_Bool: return d == kHmd;
    case vr::Prop_HasControllerComponent_Bool: return hand_of_device(d) >= 0;
    case vr::Prop_HasCameraComponent_Bool: return false;
    case vr::Prop_HasDriverDirectModeComponent_Bool: return true;   // as a SteamVR direct-mode HMD; Alyx reads it
    case vr::Prop_HasVirtualDisplayComponent_Bool: return false;
    default: break;
    }
    if (err) *err = d < kDeviceCount ? vr::TrackedProp_UnknownProperty : vr::TrackedProp_InvalidDevice;
    return false;
}

vr::HmdMatrix34_t IVRSystem__GetMatrix34TrackedDeviceProperty(void *, vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty,
                                                              vr::ETrackedPropertyError *err) {
    if (err) *err = d < kDeviceCount ? vr::TrackedProp_UnknownProperty : vr::TrackedProp_InvalidDevice;
    return identity34();
}

uint32_t IVRSystem__GetArrayTrackedDeviceProperty(void *, vr::TrackedDeviceIndex_t d, vr::ETrackedDeviceProperty,
                                                  vr::PropertyTypeTag_t, void *, uint32_t, vr::ETrackedPropertyError *err) {
    if (err) *err = d < kDeviceCount ? vr::TrackedProp_UnknownProperty : vr::TrackedProp_InvalidDevice;
    return 0;
}

const char *IVRSystem__GetPropErrorNameFromEnum(void *, vr::ETrackedPropertyError e) {
    switch (e) {
    case vr::TrackedProp_Success: return "TrackedProp_Success";
    case vr::TrackedProp_UnknownProperty: return "TrackedProp_UnknownProperty";
    case vr::TrackedProp_InvalidDevice: return "TrackedProp_InvalidDevice";
    case vr::TrackedProp_BufferTooSmall: return "TrackedProp_BufferTooSmall";
    default: return "TrackedProp_Error";
    }
}

// ---- events ---------------------------------------------------------------
bool IVRSystem__PollNextEvent(void *, vr::VREvent_t *e, uint32_t size) {
    return pop_event(e, size);
}

bool IVRSystem__PollNextEventWithPose(void *self, vr::ETrackingUniverseOrigin u, vr::VREvent_t *e,
                                      uint32_t size, vr::TrackedDevicePose_t *pose) {
    if (!pop_event(e, size)) return false;
    if (pose) {
        vr::TrackedDevicePose_t all[kDeviceCount];
        device_poses(klvr_plat_now(), u, all, kDeviceCount);
        if (e && e->trackedDeviceIndex < kDeviceCount) *pose = all[e->trackedDeviceIndex];
        else memset(pose, 0, sizeof *pose);
    }
    (void)self;
    return true;
}

const char *IVRSystem__GetEventTypeNameFromEnum(void *, vr::EVREventType e) {
    static char buf[32];
    snprintf(buf, sizeof buf, "VREvent_%d", (int)e);
    return buf;
}

const char *IVRSystem__GetButtonIdNameFromEnum(void *, vr::EVRButtonId b) {
    switch (b) {
    case vr::k_EButton_System: return "k_EButton_System";
    case vr::k_EButton_ApplicationMenu: return "k_EButton_ApplicationMenu";
    case vr::k_EButton_Grip: return "k_EButton_Grip";
    case vr::k_EButton_A: return "k_EButton_A";
    case vr::k_EButton_Axis0: return "k_EButton_Axis0";
    case vr::k_EButton_Axis1: return "k_EButton_Axis1";
    case vr::k_EButton_Axis2: return "k_EButton_Axis2";
    default: return "k_EButton_Unknown";
    }
}

const char *IVRSystem__GetControllerAxisTypeNameFromEnum(void *, vr::EVRControllerAxisType t) {
    switch (t) {
    case vr::k_eControllerAxis_Joystick: return "k_eControllerAxis_Joystick";
    case vr::k_eControllerAxis_Trigger: return "k_eControllerAxis_Trigger";
    case vr::k_eControllerAxis_TrackPad: return "k_eControllerAxis_TrackPad";
    default: return "k_eControllerAxis_None";
    }
}

// ---- legacy input ---------------------------------------------------------
bool IVRSystem__GetControllerState(void *, vr::TrackedDeviceIndex_t d, vr::VRControllerState_t *out, uint32_t size) {
    int hand = hand_of_device(d);
    if (!out || hand < 0) return false;
    vr::VRControllerState_t s;
    legacy_controller_state(hand, &s);
    memcpy(out, &s, size < sizeof s ? size : sizeof s);
    return true;
}

bool IVRSystem__GetControllerStateWithPose(void *self, vr::ETrackingUniverseOrigin u, vr::TrackedDeviceIndex_t d,
                                           vr::VRControllerState_t *out, uint32_t size, vr::TrackedDevicePose_t *pose) {
    if (!IVRSystem__GetControllerState(self, d, out, size)) return false;
    if (pose) {
        vr::TrackedDevicePose_t all[kDeviceCount];
        device_poses(klvr_plat_now(), u, all, kDeviceCount);
        *pose = all[d];
    }
    return true;
}

void IVRSystem__TriggerHapticPulse(void *, vr::TrackedDeviceIndex_t d, uint32_t axis, unsigned short us) {
    int hand = hand_of_device(d);
    if (hand >= 0) legacy_haptic_pulse(hand, axis, us);
}

// ---- focus, pause, misc ---------------------------------------------------
bool IVRSystem__IsInputAvailable(void *) { return true; }
bool IVRSystem__IsSteamVRDrawingControllers(void *) { return false; }
bool IVRSystem__ShouldApplicationPause(void *) { return false; }
bool IVRSystem__ShouldApplicationReduceRenderingWork(void *) { return false; }
bool IVRSystem__IsInputFocusCapturedByAnotherProcess(void *) { return false; }
bool IVRSystem__CaptureInputFocus(void *) { return true; }
void IVRSystem__ReleaseInputFocus(void *) {}
const char *IVRSystem__GetRuntimeVersion(void *) { return "2.15.6-klepton"; }
uint32_t IVRSystem__GetAppContainerFilePaths(void *, char *buf, uint32_t size) { return put_string("", buf, size); }
uint32_t IVRSystem__DriverDebugRequest(void *, vr::TrackedDeviceIndex_t, const char *, char *buf, uint32_t size) {
    return put_string("", buf, size);
}
void IVRSystem__AcknowledgeQuit_Exiting(void *) {}
void IVRSystem__PerformanceTestEnableCapture(void *, bool) {}
void IVRSystem__PerformanceTestReportFidelityLevelChange(void *, int) {}
vr::EVRInitError IVRSystem__SetSDKVersion(void *, uint32_t, uint32_t, uint32_t) { return vr::VRInitError_None; }

// Foveation from gaze. visionOS does not give apps the eye position, so there
// is nothing to report; returning false tells the game gaze-foveation is
// unavailable, and it falls back to its fixed path.
bool IVRSystem__GetEyeTrackedFoveationCenter(void *, vr::HmdVector2_t *, vr::HmdVector2_t *) { return false; }
bool IVRSystem__GetEyeTrackedFoveationCenterForProjection(void *, const vr::HmdMatrix44_t *, vr::HmdVector2_t *) {
    return false;
}

}} // namespace klvr::impl
