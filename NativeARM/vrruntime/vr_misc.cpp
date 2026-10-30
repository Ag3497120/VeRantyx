// The smaller interfaces: chaperone, applications, settings, overlays, render
// models, headset view and the rest. Mostly honest "not available" answers.
#include "gen_stubs.h"
#include "vr_runtime.h"
#include "json_mini.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <deque>
#include <map>
#include <unistd.h>

namespace {

using namespace vr;

class Chaperone : public Stub_IVRChaperone {
public:
    ChaperoneCalibrationState GetCalibrationState() override { return ChaperoneCalibrationState_OK; }
    bool GetPlayAreaSize(float *x, float *z) override { if (x) *x = 3.0f; if (z) *z = 3.0f; return true; }
    bool GetPlayAreaRect(HmdQuad_t *q) override {
        if (!q) return false;
        const float h = 1.5f;
        q->vCorners[0] = { { h, 0, -h } }; q->vCorners[1] = { { h, 0, h } };
        q->vCorners[2] = { { -h, 0, h } }; q->vCorners[3] = { { -h, 0, -h } };
        return true;
    }
    void ReloadInfo() override {}
    void SetSceneColor(HmdColor_t) override {}
    void GetBoundsColor(HmdColor_t *out, int n, float, HmdColor_t *cam) override {
        for (int i = 0; i < n; i++) out[i] = { 0, 0, 0, 0 };
        if (cam) *cam = { 0, 0, 0, 0 };
    }
    bool AreBoundsVisible() override { return false; }
    void ForceBoundsVisible(bool) override {}
    void ResetZeroPose(ETrackingUniverseOrigin) override {}
};

class ChaperoneSetup : public Stub_IVRChaperoneSetup {
public:
    bool CommitWorkingCopy(EChaperoneConfigFile) override { return true; }
    void RevertWorkingCopy() override {}
    bool GetWorkingPlayAreaSize(float *x, float *z) override { if (x) *x = 3.0f; if (z) *z = 3.0f; return true; }
    bool GetWorkingPlayAreaRect(HmdQuad_t *q) override { return Chaperone().GetPlayAreaRect(q); }
    bool GetWorkingCollisionBoundsInfo(HmdQuad_t *, uint32_t *n) override { if (n) *n = 0; return true; }
    bool GetLiveCollisionBoundsInfo(HmdQuad_t *, uint32_t *n) override { if (n) *n = 0; return true; }
    bool GetWorkingSeatedZeroPoseToRawTrackingPose(HmdMatrix34_t *m) override { vrrt::identity34(m); m->m[1][3] = 1.2f; return true; }
    bool GetWorkingStandingZeroPoseToRawTrackingPose(HmdMatrix34_t *m) override { vrrt::identity34(m); return true; }
    bool GetLiveSeatedZeroPoseToRawTrackingPose(HmdMatrix34_t *m) override { vrrt::identity34(m); m->m[1][3] = 1.2f; return true; }
};

class Applications : public Stub_IVRApplications_007 {
public:
    EVRApplicationError GetApplicationKeyByProcessId(uint32_t, char *buf, uint32_t cap) override {
        if (buf && cap) { strncpy(buf, "steam.app.546560", cap); buf[cap - 1] = 0; }
        return VRApplicationError_None;
    }
    bool IsApplicationInstalled(const char *) override { return true; }
    uint32_t GetApplicationCount() override { return 1; }
    EVRApplicationError IdentifyApplication(uint32_t, const char *key) override { vrrt::log("IdentifyApplication(%s)", key ? key : ""); return VRApplicationError_None; }
    uint32_t GetApplicationProcessId(const char *) override { return (uint32_t)getpid(); }
    const char *GetApplicationsErrorNameFromEnum(EVRApplicationError e) override { static char b[48]; snprintf(b, sizeof b, "VRApplicationError_%d", (int)e); return b; }
    EVRSceneApplicationState GetSceneApplicationState() override { return EVRSceneApplicationState_Running; }
    EVRApplicationError GetStartingApplication(char *buf, uint32_t cap) override { return GetApplicationKeyByProcessId(0, buf, cap); }
    const char *GetSceneApplicationStateNameFromEnum(EVRSceneApplicationState s) override { static char b[48]; snprintf(b, sizeof b, "SceneApplicationState_%d", (int)s); return b; }
    bool GetApplicationPropertyBool(const char *, EVRApplicationProperty, EVRApplicationError *e) override { if (e) *e = VRApplicationError_None; return false; }
    uint64_t GetApplicationPropertyUint64(const char *, EVRApplicationProperty, EVRApplicationError *e) override { if (e) *e = VRApplicationError_None; return 0; }
    uint32_t GetApplicationPropertyString(const char *, EVRApplicationProperty, char *buf, uint32_t cap, EVRApplicationError *e) override { if (buf && cap) buf[0] = 0; if (e) *e = VRApplicationError_None; return 1; }
    EVRApplicationError SetApplicationAutoLaunch(const char *, bool) override { return VRApplicationError_None; }
    bool GetApplicationAutoLaunch(const char *) override { return false; }
    EVRApplicationError PerformApplicationPrelaunchCheck(const char *) override { return VRApplicationError_None; }
    EVRApplicationError LaunchInternalProcess(const char *, const char *, const char *) override { return VRApplicationError_LaunchFailed; }
    uint32_t GetCurrentSceneProcessId() override { return (uint32_t)getpid(); }
};

class Settings : public Stub_IVRSettings {
public:
    const char *GetSettingsErrorNameFromEnum(EVRSettingsError e) override { static char b[48]; snprintf(b, sizeof b, "VRSettingsError_%d", (int)e); return b; }
    void SetBool(const char *, const char *, bool, EVRSettingsError *e) override { if (e) *e = VRSettingsError_None; }
    void SetInt32(const char *, const char *, int32_t, EVRSettingsError *e) override { if (e) *e = VRSettingsError_None; }
    void SetFloat(const char *, const char *, float, EVRSettingsError *e) override { if (e) *e = VRSettingsError_None; }
    void SetString(const char *, const char *, const char *, EVRSettingsError *e) override { if (e) *e = VRSettingsError_None; }
    bool GetBool(const char *sec, const char *key, EVRSettingsError *e) override {
        if (e) *e = VRSettingsError_None;
        if (!strcmp(key, "enableHomeApp") || !strcmp(key, "motionSmoothing") || !strcmp(key, "supersampleManualOverride")) return false;
        if (e) *e = VRSettingsError_UnsetSettingHasNoDefault;
        return false;
    }
    int32_t GetInt32(const char *, const char *, EVRSettingsError *e) override { if (e) *e = VRSettingsError_UnsetSettingHasNoDefault; return 0; }
    float GetFloat(const char *sec, const char *key, EVRSettingsError *e) override {
        if (e) *e = VRSettingsError_None;
        if (!strcmp(key, "supersampleScale") || !strcmp(key, "renderTargetMultiplier")) return 1.0f;
        if (!strcmp(key, "ipd")) return vrrt::config().ipd;
        if (e) *e = VRSettingsError_UnsetSettingHasNoDefault;
        return 0.f;
    }
    void GetString(const char *, const char *, char *buf, uint32_t cap, EVRSettingsError *e) override { if (buf && cap) buf[0] = 0; if (e) *e = VRSettingsError_UnsetSettingHasNoDefault; }
    void RemoveSection(const char *, EVRSettingsError *e) override { if (e) *e = VRSettingsError_None; }
    void RemoveKeyInSection(const char *, const char *, EVRSettingsError *e) override { if (e) *e = VRSettingsError_None; }
};

// No dashboard, no overlays: creation fails, everything else is a no-op error.
class Overlay : public Stub_IVROverlay {
public:
    EVROverlayError FindOverlay(const char *, VROverlayHandle_t *h) override { if (h) *h = k_ulOverlayHandleInvalid; return VROverlayError_UnknownOverlay; }
    EVROverlayError CreateOverlay(const char *key, const char *, VROverlayHandle_t *h) override {
        static int n; if (n++ < 5) vrrt::log("CreateOverlay(%s) refused (no overlay system)", key ? key : "");
        if (h) *h = k_ulOverlayHandleInvalid; return VROverlayError_RequestFailed;
    }
    EVROverlayError DestroyOverlay(VROverlayHandle_t) override { return VROverlayError_UnknownOverlay; }
    uint32_t GetOverlayKey(VROverlayHandle_t, char *buf, uint32_t cap, EVROverlayError *e) override { if (buf && cap) buf[0] = 0; if (e) *e = VROverlayError_UnknownOverlay; return 0; }
    uint32_t GetOverlayName(VROverlayHandle_t, char *buf, uint32_t cap, EVROverlayError *e) override { if (buf && cap) buf[0] = 0; if (e) *e = VROverlayError_UnknownOverlay; return 0; }
    const char *GetOverlayErrorNameFromEnum(EVROverlayError e) override { static char b[48]; snprintf(b, sizeof b, "VROverlayError_%d", (int)e); return b; }
    bool IsDashboardVisible() override { return false; }
    bool IsActiveDashboardOverlay(VROverlayHandle_t) override { return false; }
    EVROverlayError CreateDashboardOverlay(const char *, const char *, VROverlayHandle_t *a, VROverlayHandle_t *b) override {
        if (a) *a = k_ulOverlayHandleInvalid; if (b) *b = k_ulOverlayHandleInvalid; return VROverlayError_RequestFailed;
    }
    bool PollNextOverlayEvent(VROverlayHandle_t, VREvent_t *, uint32_t) override { return false; }
    bool IsOverlayVisible(VROverlayHandle_t) override { return false; }
    VRMessageOverlayResponse ShowMessageOverlay(const char *text, const char *caption, const char *, const char *, const char *, const char *) override {
        vrrt::log("message overlay: [%s] %s", caption ? caption : "", text ? text : "");
        return VRMessageOverlayResponse_ButtonPress_0;
    }
};

class RenderModels : public Stub_IVRRenderModels {
public:
    EVRRenderModelError LoadRenderModel_Async(const char *, RenderModel_t **m) override { if (m) *m = nullptr; return VRRenderModelError_InvalidModel; }
    void FreeRenderModel(RenderModel_t *) override {}
    EVRRenderModelError LoadTexture_Async(TextureID_t, RenderModel_TextureMap_t **t) override { if (t) *t = nullptr; return VRRenderModelError_InvalidTexture; }
    void FreeTexture(RenderModel_TextureMap_t *) override {}
    uint32_t GetRenderModelName(uint32_t, char *buf, uint32_t cap) override { if (buf && cap) buf[0] = 0; return 0; }
    uint32_t GetRenderModelCount() override { return 0; }
    uint32_t GetComponentCount(const char *) override { return 0; }
    uint32_t GetComponentName(const char *, uint32_t, char *buf, uint32_t cap) override { if (buf && cap) buf[0] = 0; return 0; }
    uint64_t GetComponentButtonMask(const char *, const char *) override { return 0; }
    uint32_t GetComponentRenderModelName(const char *, const char *, char *buf, uint32_t cap) override { if (buf && cap) buf[0] = 0; return 0; }
    bool GetComponentStateForDevicePath(const char *, const char *, VRInputValueHandle_t, const RenderModel_ControllerMode_State_t *, RenderModel_ComponentState_t *) override { return false; }
    bool GetComponentState(const char *, const char *, const VRControllerState_t *, const RenderModel_ControllerMode_State_t *, RenderModel_ComponentState_t *) override { return false; }
    bool RenderModelHasComponent(const char *, const char *) override { return false; }
    uint32_t GetRenderModelThumbnailURL(const char *, char *buf, uint32_t cap, EVRRenderModelError *e) override { if (buf && cap) buf[0] = 0; if (e) *e = VRRenderModelError_InvalidModel; return 0; }
    uint32_t GetRenderModelOriginalPath(const char *, char *buf, uint32_t cap, EVRRenderModelError *e) override { if (buf && cap) buf[0] = 0; if (e) *e = VRRenderModelError_InvalidModel; return 0; }
    const char *GetRenderModelErrorNameFromEnum(EVRRenderModelError e) override { static char b[48]; snprintf(b, sizeof b, "VRRenderModelError_%d", (int)e); return b; }
};

class HeadsetView : public Stub_IVRHeadsetView {
public:
    void GetHeadsetViewSize(uint32_t *w, uint32_t *h) override { if (w) *w = vrrt::config().eyeWidth; if (h) *h = vrrt::config().eyeHeight; }
    HeadsetViewMode_t GetHeadsetViewMode() override { return HeadsetViewMode_Both; }
    float GetHeadsetViewAspectRatio() override { return (float)vrrt::config().eyeWidth / (float)vrrt::config().eyeHeight; }
};

class Resources : public Stub_IVRResources {
public:
    uint32_t LoadSharedResource(const char *name, char *buf, uint32_t cap) override { if (buf && cap) buf[0] = 0; vrrt::log("LoadSharedResource(%s): none", name ? name : ""); return 0; }
    uint32_t GetResourceFullPath(const char *name, const char *dir, char *buf, uint32_t cap) override { if (buf && cap) buf[0] = 0; return 0; }
};

class DriverManager : public Stub_IVRDriverManager {
public:
    uint32_t GetDriverCount() const override { return 1; }
    uint32_t GetDriverName(DriverId_t, char *buf, uint32_t cap) override { if (buf && cap) { strncpy(buf, "verantyx", cap); buf[cap - 1] = 0; } return 9; }
    DriverHandle_t GetDriverHandle(const char *) override { return 1; }
    bool IsEnabled(DriverId_t) const override { return true; }
};

// IVRMailbox_001 is Valve-internal; the layout is the one Proton's vrclient
// wraps (vrclient_x64/winIVRMailbox.c): register, unregister, send, receive.
// libvr refuses to start without it. Messages are JSON to SteamVR's processes
// (vrserver, vrcompositor), none of which exist here.
class IVRMailbox_001 {
public:
    virtual uint32_t RegisterMailbox(const char *name, uint64_t *handle) = 0;
    virtual uint32_t UnregisterMailbox(uint64_t handle) = 0;
    virtual uint32_t SendMessage_(uint64_t handle, const char *destination, const char *message) = 0;
    virtual uint32_t ReceiveMessage(uint64_t handle, char *buffer, uint32_t bufferSize, uint32_t *needed) = 0;
};
// IVRMailbox_001, with a stand-in for SteamVR's "hlvr/interstitials" service
// (the client's "Mongoose": the web overlay that draws level-transition
// screens). Half-Life: Alyx registers "hlvr/mailbox", waits for that service's
// {"type":"ready"}, answers with set_paths, and during loads quits with
// "Timed out waiting for response from Mongoose" if no "ready" has come within
// 15 s. The stand-in says ready when the game's mailbox registers, and again
// for each message the game sends to the service other than set_paths (the
// game resets its "ready" state when it starts a transition). Text goes through
// the encoder's existing stereo prompt overlay, also used by the Windows path.
class Mailbox : public IVRMailbox_001 {
public:
    std::mutex m_mu;
    uint64_t m_next = 1;
    std::map<uint64_t, std::string> m_names;
    std::map<std::string, std::deque<std::string>> m_inbox;   // by mailbox name
    int m_logged = 0;

    void interstitial(const std::string &message) { vrrt::loading_message(message); }

    void post(const std::string &to, const std::string &msg) { m_inbox[to].push_back(msg); }

    uint32_t RegisterMailbox(const char *name, uint64_t *handle) override {
        std::lock_guard<std::mutex> l(m_mu);
        std::string n = name ? name : "";
        vrrt::log("mailbox registered: %s", n.c_str());
        uint64_t h = m_next++;
        m_names[h] = n;
        if (handle) *handle = h;
        if (n == "hlvr/mailbox") { vrrt::loading_reset(); post(n, "{\"type\":\"ready\"}"); }
        return 0;
    }
    uint32_t UnregisterMailbox(uint64_t h) override {
        std::lock_guard<std::mutex> l(m_mu);
        auto it = m_names.find(h);
        if (it != m_names.end() && it->second == "hlvr/mailbox") { vrrt::loading_reset(); m_inbox.erase(it->second); }
        m_names.erase(h);
        return 0;
    }
    uint32_t SendMessage_(uint64_t h, const char *dest, const char *msg) override {
        std::lock_guard<std::mutex> l(m_mu);
        std::string d = dest ? dest : "", m = msg ? msg : "";
        if (m_logged++ < 200) vrrt::log("mailbox %s -> %s: %.400s", m_names.count(h) ? m_names[h].c_str() : "?", d.c_str(), m.c_str());
        if (d == "hlvr/interstitials" && m_names.count(h)) interstitial(m);
        if (d == "hlvr/interstitials" && m.find("\"set_paths\"") == std::string::npos && m_names.count(h))
            post(m_names[h], "{\"type\":\"ready\"}");
        return 0;
    }
    uint32_t ReceiveMessage(uint64_t h, char *buf, uint32_t size, uint32_t *needed) override {
        std::lock_guard<std::mutex> l(m_mu);
        auto it = m_names.find(h);
        if (it == m_names.end()) { if (needed) *needed = 0; return 2; }
        auto &q = m_inbox[it->second];
        if (q.empty()) { if (buf && size) buf[0] = 0; if (needed) *needed = 0; return 2; }   // nothing pending
        const std::string &m = q.front();
        uint32_t len = (uint32_t)m.size() + 1;
        if (needed) *needed = len;
        if (!buf || size < len) return 1;                                                   // buffer too small
        memcpy(buf, m.c_str(), len);
        if (m_logged++ < 200) vrrt::log("mailbox %s <- %s", it->second.c_str(), m.c_str());
        q.pop_front();
        return 0;
    }
};

class OverlayView : public Stub_IVROverlayView {};
class Screenshots : public Stub_IVRScreenshots {};
class Notifications : public Stub_IVRNotifications {};
class Debug : public Stub_IVRDebug {};
class IOBuffer : public Stub_IVRIOBuffer {};
class SpatialAnchors : public Stub_IVRSpatialAnchors {};
class ExtendedDisplay : public Stub_IVRExtendedDisplay {};
class TrackedCamera : public Stub_IVRTrackedCamera {};

Chaperone g_chaperone; ChaperoneSetup g_chaperoneSetup; Applications g_applications; Settings g_settings;
Overlay g_overlay; OverlayView g_overlayView; RenderModels g_renderModels; HeadsetView g_headsetView;
Resources g_resources; DriverManager g_driverManager; Screenshots g_screenshots; Notifications g_notifications;
Debug g_debug; IOBuffer g_ioBuffer; SpatialAnchors g_anchors; ExtendedDisplay g_extendedDisplay; TrackedCamera g_trackedCamera;
Mailbox g_mailbox;

} // namespace

void *vrrt_misc_interface(const char *name) {
    struct { const char *n; void *p; } table[] = {
        { "IVRChaperone_004", &g_chaperone }, { "IVRChaperoneSetup_006", &g_chaperoneSetup },
        { "IVRApplications_007", &g_applications }, { "IVRSettings_003", &g_settings },
        { "IVROverlay_028", &g_overlay }, { "IVROverlayView_003", &g_overlayView },
        { "IVRRenderModels_006", &g_renderModels }, { "IVRHeadsetView_001", &g_headsetView },
        { "IVRResources_001", &g_resources }, { "IVRDriverManager_001", &g_driverManager },
        { "IVRScreenshots_001", &g_screenshots }, { "IVRNotifications_002", &g_notifications },
        { "IVRDebug_001", &g_debug }, { "IVRIOBuffer_002", &g_ioBuffer }, { "IVRSpatialAnchors_001", &g_anchors },
        { "IVRExtendedDisplay_001", &g_extendedDisplay }, { "IVRTrackedCamera_006", &g_trackedCamera },
        { "IVRMailbox_001", &g_mailbox },
    };
    for (auto &t : table) if (!strcmp(t.n, name)) return t.p;
    return nullptr;
}
