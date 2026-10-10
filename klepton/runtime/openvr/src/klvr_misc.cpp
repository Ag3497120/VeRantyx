// The smaller interfaces: chaperone, settings, render models, applications.
//
// Only the calls whose default (zero) answer would mislead a game are here;
// everything else falls through to the generated stubs, which log by name.
#include "klvr_internal.h"

using namespace klvr;
namespace klvr { namespace impl {

// ---- IVRChaperone ---------------------------------------------------------
// A 2 m x 2 m play area centred on the floor origin. visionOS draws its own
// boundary, so the game's is never shown.
static float play_half() { return env_float("KLVR_PLAY_AREA_M", 2.0f) / 2; }

vr::ChaperoneCalibrationState IVRChaperone__GetCalibrationState(void *) { return vr::ChaperoneCalibrationState_OK; }
bool IVRChaperone__GetPlayAreaSize(void *, float *x, float *z) {
    if (x) *x = play_half() * 2;
    if (z) *z = play_half() * 2;
    return true;
}
bool IVRChaperone__GetPlayAreaRect(void *, vr::HmdQuad_t *q) {
    if (!q) return false;
    float h = play_half();
    const float c[4][2] = { { -h, -h }, { h, -h }, { h, h }, { -h, h } };
    for (int i = 0; i < 4; i++) q->vCorners[i] = { { c[i][0], 0, c[i][1] } };
    return true;
}
void IVRChaperone__ReloadInfo(void *) {}
void IVRChaperone__SetSceneColor(void *, vr::HmdColor_t) {}
void IVRChaperone__GetBoundsColor(void *, vr::HmdColor_t *colors, int n, float, vr::HmdColor_t *camera) {
    for (int i = 0; colors && i < n; i++) colors[i] = { 0, 0, 0, 0 };
    if (camera) *camera = { 0, 0, 0, 0 };
}
bool IVRChaperone__AreBoundsVisible(void *) { return false; }
void IVRChaperone__ForceBoundsVisible(void *, bool) {}
void IVRChaperone__ResetZeroPose(void *, vr::ETrackingUniverseOrigin u) {
    if (u == vr::TrackingUniverseSeated) reset_seated_zero();
}

// ---- IVRSettings ----------------------------------------------------------
// No SteamVR settings store. Every read says "unset" so the game uses its own
// default, which is what it does on a fresh SteamVR install.
bool IVRSettings__GetBool(void *, const char *, const char *, vr::EVRSettingsError *e) {
    if (e) *e = vr::VRSettingsError_UnsetSettingHasNoDefault;
    return false;
}
int32_t IVRSettings__GetInt32(void *, const char *, const char *, vr::EVRSettingsError *e) {
    if (e) *e = vr::VRSettingsError_UnsetSettingHasNoDefault;
    return 0;
}
float IVRSettings__GetFloat(void *, const char *, const char *, vr::EVRSettingsError *e) {
    if (e) *e = vr::VRSettingsError_UnsetSettingHasNoDefault;
    return 0;
}
void IVRSettings__GetString(void *, const char *, const char *, char *buf, uint32_t size, vr::EVRSettingsError *e) {
    put_string("", buf, size);
    if (e) *e = vr::VRSettingsError_UnsetSettingHasNoDefault;
}
void IVRSettings__SetBool(void *, const char *, const char *, bool, vr::EVRSettingsError *e) { if (e) *e = vr::VRSettingsError_None; }
void IVRSettings__SetInt32(void *, const char *, const char *, int32_t, vr::EVRSettingsError *e) { if (e) *e = vr::VRSettingsError_None; }
void IVRSettings__SetFloat(void *, const char *, const char *, float, vr::EVRSettingsError *e) { if (e) *e = vr::VRSettingsError_None; }
void IVRSettings__SetString(void *, const char *, const char *, const char *, vr::EVRSettingsError *e) { if (e) *e = vr::VRSettingsError_None; }
void IVRSettings__RemoveSection(void *, const char *, vr::EVRSettingsError *e) { if (e) *e = vr::VRSettingsError_None; }
void IVRSettings__RemoveKeyInSection(void *, const char *, const char *, vr::EVRSettingsError *e) { if (e) *e = vr::VRSettingsError_None; }
bool IVRSettings__Sync(void *, bool, vr::EVRSettingsError *e) { if (e) *e = vr::VRSettingsError_None; return true; }
const char *IVRSettings__GetSettingsErrorNameFromEnum(void *, vr::EVRSettingsError) { return "VRSettingsError"; }

// ---- IVRRenderModels ------------------------------------------------------
// No SteamVR render models on this device. Alyx draws its own hands; a
// controller model request answers "not found" rather than "loading" so no
// one waits on it.
vr::EVRRenderModelError IVRRenderModels__LoadRenderModel_Async(void *, const char *name, vr::RenderModel_t **out) {
    static bool said = false;
    if (!said) { said = true; logf("IVRRenderModels: no render models served (first asked for %s)", name ? name : "?"); }
    if (out) *out = nullptr;
    return vr::VRRenderModelError_InvalidModel;
}
vr::EVRRenderModelError IVRRenderModels__LoadTexture_Async(void *, vr::TextureID_t, vr::RenderModel_TextureMap_t **out) {
    if (out) *out = nullptr;
    return vr::VRRenderModelError_InvalidTexture;
}
void IVRRenderModels__FreeRenderModel(void *, vr::RenderModel_t *) {}
void IVRRenderModels__FreeTexture(void *, vr::RenderModel_TextureMap_t *) {}
uint32_t IVRRenderModels__GetRenderModelCount(void *) { return 0; }
uint32_t IVRRenderModels__GetRenderModelName(void *, uint32_t, char *buf, uint32_t size) { put_string("", buf, size); return 0; }
uint32_t IVRRenderModels__GetComponentCount(void *, const char *) { return 0; }
uint32_t IVRRenderModels__GetComponentName(void *, const char *, uint32_t, char *buf, uint32_t size) { put_string("", buf, size); return 0; }
uint64_t IVRRenderModels__GetComponentButtonMask(void *, const char *, const char *) { return 0; }
uint32_t IVRRenderModels__GetComponentRenderModelName(void *, const char *, const char *, char *buf, uint32_t size) {
    put_string("", buf, size);
    return 0;
}
bool IVRRenderModels__GetComponentStateForDevicePath(void *, const char *, const char *, vr::VRInputValueHandle_t,
                                                     const vr::RenderModel_ControllerMode_State_t *, vr::RenderModel_ComponentState_t *) {
    return false;
}
bool IVRRenderModels__GetComponentState(void *, const char *, const char *, const vr::VRControllerState_t *,
                                        const vr::RenderModel_ControllerMode_State_t *, vr::RenderModel_ComponentState_t *) {
    return false;
}
bool IVRRenderModels__RenderModelHasComponent(void *, const char *, const char *) { return false; }
uint32_t IVRRenderModels__GetRenderModelThumbnailURL(void *, const char *, char *buf, uint32_t size, vr::EVRRenderModelError *e) {
    if (e) *e = vr::VRRenderModelError_InvalidModel;
    return put_string("", buf, size);
}
uint32_t IVRRenderModels__GetRenderModelOriginalPath(void *, const char *, char *buf, uint32_t size, vr::EVRRenderModelError *e) {
    if (e) *e = vr::VRRenderModelError_InvalidModel;
    return put_string("", buf, size);
}
const char *IVRRenderModels__GetRenderModelErrorNameFromEnum(void *, vr::EVRRenderModelError) { return "VRRenderModelError"; }

// ---- IVRApplications ------------------------------------------------------
vr::EVRApplicationError IVRApplications__IdentifyApplication(void *, uint32_t, const char *) { return vr::VRApplicationError_None; }
vr::EVRApplicationError IVRApplications__GetApplicationKeyByProcessId(void *, uint32_t, char *buf, uint32_t size) {
    put_string("steam.app.546560", buf, size);   // Half-Life: Alyx
    return vr::VRApplicationError_None;
}
bool IVRApplications__IsApplicationInstalled(void *, const char *) { return true; }
vr::EVRApplicationError IVRApplications__AddApplicationManifest(void *, const char *, bool) { return vr::VRApplicationError_None; }
vr::EVRSceneApplicationState IVRApplications__GetSceneApplicationState(void *) { return vr::EVRSceneApplicationState_Running; }

}} // namespace klvr::impl
