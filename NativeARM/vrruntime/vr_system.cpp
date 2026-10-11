// IVRSystem_026: display geometry, device poses and properties, legacy
// controller state, events.
#include "gen_stubs.h"
#include "vr_runtime.h"
#include <cstring>
#include <cmath>
#include <set>
#include <string>
#include <unistd.h>

// From Klepton: the MoltenVK physical device behind a guest VkInstance.
extern "C" void *kl_vulkan_xr_physical_device(void *vk_instance);

namespace {

using namespace vr;

const char *kControllerType = "oculus_touch";

class System : public Stub_IVRSystem {
public:
    // ---- display ----
    void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override {
        *w = vrrt::config().eyeWidth; *h = vrrt::config().eyeHeight;
    }
    HmdMatrix44_t GetProjectionMatrix(EVREye eye, float zNear, float zFar) override {
        { static int n; if (n++ < 12) vrrt::log("GetProjectionMatrix(eye %d, near %.3f, far %.1f)", (int)eye, zNear, zFar); }
        float l, r, t, b; GetProjectionRaw(eye, &l, &r, &t, &b);
        HmdMatrix44_t m; memset(&m, 0, sizeof m);
        // Standard OpenVR off-axis projection from the tangents (right-handed, -Z forward).
        float idx = 1.0f / (r - l), idy = 1.0f / (b - t), idz = 1.0f / (zFar - zNear);
        float sx = r + l, sy = b + t;
        m.m[0][0] = 2 * idx; m.m[0][2] = sx * idx;
        m.m[1][1] = 2 * idy; m.m[1][2] = sy * idy;
        m.m[2][2] = -zFar * idz; m.m[2][3] = -zFar * zNear * idz;
        m.m[3][2] = -1.0f;
        return m;
    }
    void GetProjectionRaw(EVREye eye, float *l, float *r, float *t, float *b) override {
        { static int n; if (n++ < 12) vrrt::log("GetProjectionRaw(eye %d)", (int)eye); }
        const float *tg = vrrt::render_tangents(eye == Eye_Left ? 0 : 1);
        *l = tg[0]; *r = tg[1]; *t = tg[2]; *b = tg[3];
    }
    bool ComputeDistortion(EVREye, float u, float v, DistortionCoordinates_t *d) override {
        d->rfRed[0] = d->rfGreen[0] = d->rfBlue[0] = u;
        d->rfRed[1] = d->rfGreen[1] = d->rfBlue[1] = v;
        return true;
    }
    HmdMatrix34_t GetEyeToHeadTransform(EVREye eye) override {
        HmdMatrix34_t m; vrrt::identity34(&m);
        m.m[0][3] = (eye == Eye_Left ? -0.5f : 0.5f) * vrrt::config().ipd;
        return m;
    }
    bool GetTimeSinceLastVsync(float *seconds, uint64_t *counter) override {
        if (seconds) *seconds = (float)vrrt::seconds_since_vsync();
        if (counter) *counter = vrrt::frame_counter();
        return true;
    }
    int32_t GetD3D9AdapterIndex() override { return -1; }
    void GetDXGIOutputInfo(int32_t *idx) override { if (idx) *idx = -1; }
    void GetOutputDevice(uint64_t *device, ETextureType type, VkInstance_T *instance) override {
        *device = 0;
        if (type == TextureType_Vulkan && instance) {
            void *phys = kl_vulkan_xr_physical_device(instance);
            *device = (uint64_t)(uintptr_t)phys;
            vrrt::log("GetOutputDevice(Vulkan, instance %p) -> physical device %p", (void *)instance, phys);
        }
    }
    bool IsDisplayOnDesktop() override { return false; }
    bool SetDisplayVisibility(bool) override { return false; }

    // ---- tracking ----
    void GetDeviceToAbsoluteTrackingPose(ETrackingUniverseOrigin, float predict, TrackedDevicePose_t *poses, uint32_t count) override {
        vrrt::PoseSet set; vrrt::sample_poses(&set, predict);
        for (uint32_t i = 0; i < count; i++) {
            if (i < 3) poses[i] = set.dev[i];
            else { memset(&poses[i], 0, sizeof poses[i]); poses[i].eTrackingResult = TrackingResult_Uninitialized; }
        }
    }
    HmdMatrix34_t GetSeatedZeroPoseToStandingAbsoluteTrackingPose() override {
        HmdMatrix34_t m; vrrt::identity34(&m); m.m[1][3] = 1.2f; return m;   // a seated head is ~1.2 m up
    }
    HmdMatrix34_t GetRawZeroPoseToStandingAbsoluteTrackingPose() override { HmdMatrix34_t m; vrrt::identity34(&m); return m; }
    uint32_t GetSortedTrackedDeviceIndicesOfClass(ETrackedDeviceClass cls, TrackedDeviceIndex_t *out, uint32_t cap, TrackedDeviceIndex_t) override {
        uint32_t n = 0;
        for (uint32_t i = 0; i < 3; i++)
            if (GetTrackedDeviceClass(i) == cls) { if (out && n < cap) out[n] = i; n++; }
        return n;
    }
    EDeviceActivityLevel GetTrackedDeviceActivityLevel(TrackedDeviceIndex_t i) override {
        return i < 3 ? k_EDeviceActivityLevel_UserInteraction : k_EDeviceActivityLevel_Unknown;
    }
    void ApplyTransform(TrackedDevicePose_t *out, const TrackedDevicePose_t *in, const HmdMatrix34_t *t) override {
        *out = *in;
        const HmdMatrix34_t &a = in->mDeviceToAbsoluteTracking, &b = *t;
        HmdMatrix34_t r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 4; j++) {
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + (j == 3 ? a.m[i][3] : 0.f);
            }
        out->mDeviceToAbsoluteTracking = r;
    }
    TrackedDeviceIndex_t GetTrackedDeviceIndexForControllerRole(ETrackedControllerRole role) override {
        if (role == TrackedControllerRole_LeftHand) return 1;
        if (role == TrackedControllerRole_RightHand) return 2;
        return k_unTrackedDeviceIndexInvalid;
    }
    ETrackedControllerRole GetControllerRoleForTrackedDeviceIndex(TrackedDeviceIndex_t i) override {
        return i == 1 ? TrackedControllerRole_LeftHand : i == 2 ? TrackedControllerRole_RightHand : TrackedControllerRole_Invalid;
    }
    ETrackedDeviceClass GetTrackedDeviceClass(TrackedDeviceIndex_t i) override {
        return i == 0 ? TrackedDeviceClass_HMD : (i == 1 || i == 2) ? TrackedDeviceClass_Controller : TrackedDeviceClass_Invalid;
    }
    bool IsTrackedDeviceConnected(TrackedDeviceIndex_t i) override { return i < 3; }

    // ---- properties ----
    static void unknown(TrackedDeviceIndex_t dev, ETrackedDeviceProperty prop, const char *kind) {
        static std::set<std::string> said;
        char key[64]; snprintf(key, sizeof key, "%u/%d/%s", dev, (int)prop, kind);
        if (said.insert(key).second) vrrt::log("property %d (%s) of device %u not provided", (int)prop, kind, dev);
    }
    bool GetBoolTrackedDeviceProperty(TrackedDeviceIndex_t dev, ETrackedDeviceProperty prop, ETrackedPropertyError *err) override {
        if (err) *err = TrackedProp_Success;
        if (dev >= 3) { if (err) *err = TrackedProp_InvalidDevice; return false; }
        switch (prop) {
        case Prop_WillDriftInYaw_Bool: return false;
        case Prop_DeviceIsWireless_Bool: return true;
        case Prop_DeviceIsCharging_Bool: return false;
        case Prop_DeviceProvidesBatteryStatus_Bool: return false;
        case Prop_DeviceCanPowerOff_Bool: return false;
        case Prop_HasCamera_Bool: return false;
        case Prop_Firmware_UpdateAvailable_Bool: return false;
        case Prop_Firmware_ManualUpdate_Bool: return false;
        case Prop_BlockServerShutdown_Bool: return false;
        case Prop_ContainsProximitySensor_Bool: return false;
        case Prop_HasDriverDirectModeComponent_Bool: return dev == 0;
        case Prop_HasVirtualDisplayComponent_Bool: return dev == 0;
        case Prop_HasDisplayComponent_Bool: return dev == 0;
        case Prop_HasControllerComponent_Bool: return dev != 0;
        case Prop_HasCameraComponent_Bool: return false;
        case Prop_ReportsTimeSinceVSync_Bool: return dev == 0;
        case Prop_IsOnDesktop_Bool: return false;
        case Prop_DisplaySuppressed_Bool: return false;
        case Prop_DisplayAllowNightMode_Bool: return false;
        case Prop_DisplayDebugMode_Bool: return false;
        case Prop_Identifiable_Bool: return dev != 0;
        case Prop_NeverTracked_Bool: return false;
        case Prop_CanUnifyCoordinateSystemWithHmd_Bool: return true;
        case Prop_DriverProvidedChaperoneVisibility_Bool: return false;
        case Prop_DisplaySupportsMultipleFramerates_Bool: return false;
        case Prop_DisplaySupportsRuntimeFramerateChange_Bool: return false;
        case Prop_DisplaySupportsAnalogGain_Bool: return false;
        case Prop_Hmd_SupportsHDR10_Bool: return false;
        case Prop_Hmd_SupportsAppThrottling_Bool: return false;
        case Prop_SupportsRoomViewDepthProjection_Bool: return false;
        default: unknown(dev, prop, "bool"); if (err) *err = TrackedProp_UnknownProperty; return false;
        }
    }
    float GetFloatTrackedDeviceProperty(TrackedDeviceIndex_t dev, ETrackedDeviceProperty prop, ETrackedPropertyError *err) override {
        if (err) *err = TrackedProp_Success;
        if (dev >= 3) { if (err) *err = TrackedProp_InvalidDevice; return 0; }
        switch (prop) {
        case Prop_DeviceBatteryPercentage_Float: return 1.0f;
        case Prop_SecondsFromVsyncToPhotons_Float: return 0.011f;
        case Prop_DisplayFrequency_Float: return (float)vrrt::config().displayHz;
        case Prop_UserIpdMeters_Float: return vrrt::config().ipd;
        case Prop_UserHeadToEyeDepthMeters_Float: return 0.f;
        case Prop_DisplayMCOffset_Float: case Prop_DisplayMCScale_Float: return 0.f;
        case Prop_DisplayGCBlackClamp_Float: return 0.f;
        case Prop_DisplayGCOffset_Float: return 0.f;
        case Prop_DisplayGCScale_Float: return 1.f;
        case Prop_DisplayGCPrescale_Float: return 1.f;
        case Prop_LensCenterLeftU_Float: case Prop_LensCenterLeftV_Float:
        case Prop_LensCenterRightU_Float: case Prop_LensCenterRightV_Float: return 0.5f;
        case Prop_ScreenshotHorizontalFieldOfViewDegrees_Float: case Prop_ScreenshotVerticalFieldOfViewDegrees_Float: return 100.f;
        case Prop_DashboardScale_Float: return 1.0f;
        case Prop_IpdUIRangeMinMeters_Float: return 0.058f;
        case Prop_IpdUIRangeMaxMeters_Float: return 0.072f;
        case Prop_MinimumIpdStepMeters_Float: return 0.0005f;
        case Prop_FieldOfViewLeftDegrees_Float: return (float)(atan(-vrrt::render_tangents(0)[0]) * 180.0 / M_PI);
        case Prop_FieldOfViewRightDegrees_Float: return (float)(atan(vrrt::render_tangents(1)[1]) * 180.0 / M_PI);
        case Prop_FieldOfViewTopDegrees_Float: return (float)(atan(-vrrt::render_tangents(0)[2]) * 180.0 / M_PI);
        case Prop_FieldOfViewBottomDegrees_Float: return (float)(atan(vrrt::render_tangents(0)[3]) * 180.0 / M_PI);
        case Prop_TrackingRangeMinimumMeters_Float: return 0.f;
        case Prop_TrackingRangeMaximumMeters_Float: return 10.f;
        default: unknown(dev, prop, "float"); if (err) *err = TrackedProp_UnknownProperty; return 0;
        }
    }
    int32_t GetInt32TrackedDeviceProperty(TrackedDeviceIndex_t dev, ETrackedDeviceProperty prop, ETrackedPropertyError *err) override {
        if (err) *err = TrackedProp_Success;
        if (dev >= 3) { if (err) *err = TrackedProp_InvalidDevice; return 0; }
        switch (prop) {
        case Prop_DeviceClass_Int32: return (int32_t)GetTrackedDeviceClass(dev);
        case Prop_ControllerRoleHint_Int32: return (int32_t)GetControllerRoleForTrackedDeviceIndex(dev);
        case Prop_Axis0Type_Int32: return dev ? k_eControllerAxis_Joystick : k_eControllerAxis_None;
        case Prop_Axis1Type_Int32: return dev ? k_eControllerAxis_Trigger : k_eControllerAxis_None;
        case Prop_Axis2Type_Int32: return dev ? k_eControllerAxis_Trigger : k_eControllerAxis_None;
        case Prop_Axis3Type_Int32: case Prop_Axis4Type_Int32: return k_eControllerAxis_None;
        case Prop_DisplayMCType_Int32: case Prop_DisplayGCType_Int32: return 0;
        case Prop_EdidVendorID_Int32: case Prop_EdidProductID_Int32: return 0;
        case Prop_DisplayMCImageWidth_Int32: case Prop_DisplayMCImageHeight_Int32: case Prop_DisplayMCImageNumChannels_Int32: return 0;
        case Prop_ExpectedTrackingReferenceCount_Int32: case Prop_ExpectedControllerCount_Int32: return dev == 0 ? 2 : 0;
        case Prop_ControllerHandSelectionPriority_Int32: return 0;
        case Prop_NumCameras_Int32: return 0;
        case Prop_DistortionMeshResolution_Int32: return 0;
        default: unknown(dev, prop, "int32"); if (err) *err = TrackedProp_UnknownProperty; return 0;
        }
    }
    uint64_t GetUint64TrackedDeviceProperty(TrackedDeviceIndex_t dev, ETrackedDeviceProperty prop, ETrackedPropertyError *err) override {
        if (err) *err = TrackedProp_Success;
        if (dev >= 3) { if (err) *err = TrackedProp_InvalidDevice; return 0; }
        switch (prop) {
        case Prop_HardwareRevision_Uint64: return 1;
        case Prop_FirmwareVersion_Uint64: return 1;
        case Prop_FPGAVersion_Uint64: case Prop_VRCVersion_Uint64: case Prop_RadioVersion_Uint64: case Prop_DongleVersion_Uint64: return 0;
        case Prop_CurrentUniverseId_Uint64: return 1;
        case Prop_DisplayFirmwareVersion_Uint64: case Prop_CameraFirmwareVersion_Uint64: case Prop_DisplayFPGAVersion_Uint64:
        case Prop_DisplayBootloaderVersion_Uint64: case Prop_DisplayHardwareVersion_Uint64: case Prop_AudioFirmwareVersion_Uint64: return 0;
        case Prop_ParentDriver_Uint64: return 0;
        case Prop_SupportedButtons_Uint64:
            return dev ? (ButtonMaskFromId(k_EButton_A) | ButtonMaskFromId(k_EButton_ApplicationMenu) | ButtonMaskFromId(k_EButton_Grip) |
                          ButtonMaskFromId(k_EButton_SteamVR_Touchpad) | ButtonMaskFromId(k_EButton_SteamVR_Trigger) |
                          ButtonMaskFromId(k_EButton_System)) : 0;
        default: unknown(dev, prop, "uint64"); if (err) *err = TrackedProp_UnknownProperty; return 0;
        }
    }
    HmdMatrix34_t GetMatrix34TrackedDeviceProperty(TrackedDeviceIndex_t dev, ETrackedDeviceProperty prop, ETrackedPropertyError *err) override {
        HmdMatrix34_t m; vrrt::identity34(&m);
        if (err) *err = TrackedProp_Success;
        if (prop == Prop_StatusDisplayTransform_Matrix34 || prop == Prop_CameraToHeadTransform_Matrix34 || prop == Prop_ImuToHeadTransform_Matrix34) return m;
        unknown(dev, prop, "matrix34"); if (err) *err = TrackedProp_UnknownProperty; return m;
    }
    uint32_t GetArrayTrackedDeviceProperty(TrackedDeviceIndex_t dev, ETrackedDeviceProperty prop, PropertyTypeTag_t, void *, uint32_t, ETrackedPropertyError *err) override {
        unknown(dev, prop, "array"); if (err) *err = TrackedProp_UnknownProperty; return 0;
    }
    static uint32_t copy_out(const char *s, char *buf, uint32_t cap) {
        uint32_t n = (uint32_t)strlen(s) + 1;
        if (buf && cap) { strncpy(buf, s, cap); buf[cap - 1] = 0; }
        return n;
    }
    uint32_t GetStringTrackedDeviceProperty(TrackedDeviceIndex_t dev, ETrackedDeviceProperty prop, char *buf, uint32_t cap, ETrackedPropertyError *err) override {
        if (err) *err = TrackedProp_Success;
        if (dev >= 3) { if (err) *err = TrackedProp_InvalidDevice; return 0; }
        const bool hmd = dev == 0, left = dev == 1;
        const char *s = nullptr;
        char tmp[128];
        switch (prop) {
        case Prop_TrackingSystemName_String: s = hmd ? "verantyx" : "oculus"; break;
        case Prop_ModelNumber_String: s = hmd ? "VerantyxHMD" : (left ? "Oculus Rift S (Left Controller)" : "Oculus Rift S (Right Controller)"); break;
        case Prop_SerialNumber_String: snprintf(tmp, sizeof tmp, "VRX-%s-0001", hmd ? "HMD" : left ? "L" : "R"); s = tmp; break;
        case Prop_RenderModelName_String: s = hmd ? "generic_hmd" : (left ? "oculus_rifts_controller_left" : "oculus_rifts_controller_right"); break;
        case Prop_ManufacturerName_String: s = "Oculus"; break;
        case Prop_TrackingFirmwareVersion_String: case Prop_HardwareRevision_String: case Prop_DriverVersion_String: s = "1.0"; break;
        case Prop_AllWirelessDongleDescriptions_String: case Prop_ConnectedWirelessDongle_String: s = ""; break;
        case Prop_Firmware_ProgrammingTarget_String: s = ""; break;
        case Prop_RegisteredDeviceType_String: s = hmd ? "verantyx/hmd" : (left ? "oculus/WMHD-L" : "oculus/WMHD-R"); break;
        case Prop_InputProfilePath_String: s = hmd ? "{verantyx}/input/hmd_profile.json" : "{oculus}/input/touch_profile.json"; break;
        case Prop_ControllerType_String: s = hmd ? "" : kControllerType; break;
        case Prop_AttachedDeviceId_String: s = ""; break;
        case Prop_ModeLabel_String: s = ""; break;
        case Prop_DriverProvidedChaperonePath_String: s = ""; break;
        case Prop_AdditionalDeviceSettingsPath_String: s = ""; break;
        case Prop_DisplayMCImageLeft_String: case Prop_DisplayMCImageRight_String: s = ""; break;
        case Prop_NamedIconPathDeviceOff_String: case Prop_NamedIconPathDeviceSearching_String: case Prop_NamedIconPathDeviceSearchingAlert_String:
        case Prop_NamedIconPathDeviceReady_String: case Prop_NamedIconPathDeviceReadyAlert_String: case Prop_NamedIconPathDeviceNotReady_String:
        case Prop_NamedIconPathDeviceStandby_String: case Prop_NamedIconPathDeviceAlertLow_String: case Prop_NamedIconPathDeviceStandbyAlert_String:
            s = ""; break;
        case Prop_Audio_DefaultPlaybackDeviceId_String: case Prop_Audio_DefaultRecordingDeviceId_String: s = ""; break;
        default: unknown(dev, prop, "string"); if (err) *err = TrackedProp_UnknownProperty; return 0;
        }
        uint32_t n = copy_out(s, buf, cap);
        if (n > cap && err) *err = TrackedProp_BufferTooSmall;
        return n;
    }
    const char *GetPropErrorNameFromEnum(ETrackedPropertyError e) override {
        static char buf[48]; snprintf(buf, sizeof buf, "TrackedProp_%d", (int)e); return buf;
    }

    // ---- events ----
    bool PollNextEvent(VREvent_t *e, uint32_t size) override {
        VREvent_t ev;
        if (!vrrt::pop_event(&ev)) return false;
        memcpy(e, &ev, std::min<uint32_t>(size, sizeof ev));
        return true;
    }
    bool PollNextEventWithPose(ETrackingUniverseOrigin, VREvent_t *e, uint32_t size, TrackedDevicePose_t *pose) override {
        if (!PollNextEvent(e, size)) return false;
        if (pose) { const vrrt::PoseSet &p = vrrt::last_poses(); *pose = e->trackedDeviceIndex < 3 ? p.dev[e->trackedDeviceIndex] : p.dev[0]; }
        return true;
    }
    bool PollNextEventWithPoseAndOverlays(ETrackingUniverseOrigin o, VREvent_t *e, uint32_t size, TrackedDevicePose_t *pose, VROverlayHandle_t *h) override {
        if (h) *h = k_ulOverlayHandleInvalid;
        return PollNextEventWithPose(o, e, size, pose);
    }
    const char *GetEventTypeNameFromEnum(EVREventType t) override {
        static char buf[48]; snprintf(buf, sizeof buf, "VREvent_%d", (int)t); return buf;
    }
    HiddenAreaMesh_t GetHiddenAreaMesh(EVREye, EHiddenAreaMeshType) override { HiddenAreaMesh_t m = { nullptr, 0 }; return m; }

    // ---- legacy controller state ----
    bool GetControllerState(TrackedDeviceIndex_t dev, VRControllerState_t *st, uint32_t size) override {
        if (dev != 1 && dev != 2) return false;
        VRControllerState_t s; memset(&s, 0, sizeof s);
        const vrrt::PoseSet &p = vrrt::last_poses();
        const vrrt::SharedHands &h = p.hands;
        const bool left = dev == 1;
        const uint32_t shift = left ? 8u : 0u;
        const uint32_t buttons = p.inputsFresh ? (left ? h.leftButtons : h.rightButtons) >> shift : 0;
        const float trig = p.inputsFresh ? (left ? h.leftTrigger : h.rightTrigger) / 255.f : 0.f;
        const float grip = p.inputsFresh ? (left ? h.leftGrip : h.rightGrip) / 255.f : 0.f;
        s.unPacketNum = (uint32_t)vrrt::frame_counter();
        if (buttons & vrrt::kBtnPrimary) s.ulButtonPressed |= ButtonMaskFromId(k_EButton_A);
        if (buttons & vrrt::kBtnSecondary) s.ulButtonPressed |= ButtonMaskFromId(k_EButton_ApplicationMenu);
        if (buttons & vrrt::kBtnStickClick) s.ulButtonPressed |= ButtonMaskFromId(k_EButton_SteamVR_Touchpad);
        if (buttons & vrrt::kBtnStickTouch) s.ulButtonTouched |= ButtonMaskFromId(k_EButton_SteamVR_Touchpad);
        if (buttons & vrrt::kBtnMenu) s.ulButtonPressed |= ButtonMaskFromId(k_EButton_System);
        if (trig >= 0.75f) s.ulButtonPressed |= ButtonMaskFromId(k_EButton_SteamVR_Trigger);
        if (trig > 0.05f) s.ulButtonTouched |= ButtonMaskFromId(k_EButton_SteamVR_Trigger);
        if (grip >= 0.75f) s.ulButtonPressed |= ButtonMaskFromId(k_EButton_Grip);
        s.rAxis[0].x = p.inputsFresh ? (left ? h.leftStickX : h.rightStickX) : 0.f;
        s.rAxis[0].y = p.inputsFresh ? (left ? h.leftStickY : h.rightStickY) : 0.f;
        s.rAxis[1].x = trig;
        s.rAxis[2].x = grip;
        memcpy(st, &s, std::min<uint32_t>(size, sizeof s));
        return true;
    }
    bool GetControllerStateWithPose(ETrackingUniverseOrigin, TrackedDeviceIndex_t dev, VRControllerState_t *st, uint32_t size, TrackedDevicePose_t *pose) override {
        if (!GetControllerState(dev, st, size)) return false;
        if (pose) *pose = vrrt::last_poses().dev[dev];
        return true;
    }
    void TriggerHapticPulse(TrackedDeviceIndex_t, uint32_t, unsigned short) override {}
    const char *GetButtonIdNameFromEnum(EVRButtonId b) override { static char buf[32]; snprintf(buf, sizeof buf, "k_EButton_%d", (int)b); return buf; }
    const char *GetControllerAxisTypeNameFromEnum(EVRControllerAxisType t) override { static char buf[40]; snprintf(buf, sizeof buf, "k_eControllerAxis_%d", (int)t); return buf; }
    bool IsInputAvailable() override { return true; }
    bool IsSteamVRDrawingControllers() override { return false; }
    bool ShouldApplicationPause() override { return false; }
    bool ShouldApplicationReduceRenderingWork() override { return false; }
    EVRFirmwareError PerformFirmwareUpdate(TrackedDeviceIndex_t) override { return VRFirmwareError_None; }
    void AcknowledgeQuit_Exiting() override { vrrt::log("application acknowledged quit"); }
    uint32_t GetAppContainerFilePaths(char *buf, uint32_t cap) override { if (buf && cap) buf[0] = 0; return 1; }
    const char *GetRuntimeVersion() override { return "2.15.6"; }
    EVRInitError SetSDKVersion(uint32_t a, uint32_t b, uint32_t c) override { vrrt::log("application SDK version %u.%u.%u", a, b, c); return VRInitError_None; }
};

System g_system;

} // namespace

vr::IVRSystem *vrrt_system() { return &g_system; }
