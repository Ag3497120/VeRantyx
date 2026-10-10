// Shared state and helpers for the OpenVR shim.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <mutex>
#include "openvr.h"
#include "klvr_platform.h"
#include "generated/klvr_decls.h"

#if defined(__GNUC__) || defined(__clang__)
#define KLVR_WEAK __attribute__((weak))
#define KLVR_EXPORT extern "C" __attribute__((visibility("default")))
#define KLVR_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define KLVR_WEAK
#define KLVR_EXPORT extern "C"
#define KLVR_PRINTF(a, b)
#endif

extern "C" void klvr_unimplemented(const char *iface, const char *method);
extern "C" void klvr_unrepresentable_slot(void);

namespace klvr {

void logf(const char *fmt, ...) KLVR_PRINTF(1, 2);

// Copy `s` into an OpenVR-style (buffer, size) out-parameter and return the
// size including the terminator, which is what every OpenVR string getter
// returns whether or not the buffer was big enough.
uint32_t put_string(const char *s, char *buf, uint32_t size);

// ---- interface registry ---------------------------------------------------
struct InterfaceVersion {
    const char        *version;   // "IVRSystem_022"
    const char        *family;    // "IVRSystem"
    const void *const *vtable;
    uint32_t           slots;
    const char        *sdk_tag;   // first... newest SDK release shipping it
};
extern const InterfaceVersion kInterfaceVersions[];
extern const size_t kInterfaceVersionCount;
const InterfaceVersion *find_interface(const char *version);

// ---- private SteamVR interfaces (klvr_private.cpp) ------------------------
// Objects laid out as {vtable, name}. kProbeSlots[n] logs "<name> slot n
// called" once and returns 0; a private interface starts from that table and
// overrides the slots whose meaning is known.
struct PrivateObject { const void *const *vtable; const char *name; };
constexpr int kProbeSlotCount = 96;
extern const void *const *const kProbeSlots;
void *private_interface(const char *version);   // nullptr if not one of ours
// KLVR_TRACE_CALLS=1 (klvr_trace.cpp): `obj` behind a counting vtable.
void *trace_wrap(void *obj, const char *version, uint32_t slots);
} // namespace klvr
KLVR_EXPORT void *VRControlPanel(void);          // libvr.so imports it by name
namespace klvr {

// ---- devices --------------------------------------------------------------
// Fixed layout: the headset and two Sense controllers. OpenVR allows 64.
enum : vr::TrackedDeviceIndex_t { kHmd = 0, kLeftHand = 1, kRightHand = 2, kDeviceCount = 3 };
inline int hand_of_device(vr::TrackedDeviceIndex_t d) {
    return d == kLeftHand ? 0 : d == kRightHand ? 1 : -1;
}
inline vr::TrackedDeviceIndex_t device_of_hand(int hand) {
    return hand == 0 ? kLeftHand : kRightHand;
}

// ---- math -----------------------------------------------------------------
struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };
Quat qmul(Quat a, Quat b);
Quat qconj(Quat a);
Quat qnormalize(Quat a);
Quat qslerp(Quat a, Quat b, float t);
Vec3 qrot(Quat q, Vec3 v);
Quat qfrom_axis_angle(Vec3 axis, float radians);
inline Vec3 vadd(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 vsub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 vscale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 vlerp(Vec3 a, Vec3 b, float t) { return vadd(a, vscale(vsub(b, a), t)); }

struct Pose { Vec3 p; Quat q; };
Pose to_pose(const klvr_pose &k);
Pose pmul(const Pose &a, const Pose &b);   // a then b, b expressed in a
bool walk_enabled();
void walk_update(double dt);   // KLVR_WALK: once per frame, from WaitGetPoses
Pose pinv(const Pose &a);
vr::HmdMatrix34_t to_mat34(const Pose &p);
Pose from_mat34(const vr::HmdMatrix34_t &m);
vr::HmdMatrix34_t identity34();

// ---- tracking universe ----------------------------------------------------
// Floor space is the platform's; standing == floor. Seated is floor re-origined
// at the seated zero pose (set by ResetSeatedZeroPose, default: the head's
// first pose, yaw only).
Pose universe_from_floor(vr::ETrackingUniverseOrigin u);
void reset_seated_zero();

// The controllers' device ("raw") pose relative to the platform grip pose.
const Pose &raw_from_grip(int hand);

// Fill one OpenVR pose from a platform sample, in universe `u`.
void fill_pose(vr::TrackedDevicePose_t *out, const klvr_tracked &t,
               vr::ETrackingUniverseOrigin u, const Pose *offset = nullptr);

// Poses of every device for one instant, in universe `u`. `count` may exceed
// kDeviceCount; the rest read as not connected.
void device_poses(double display_time, vr::ETrackingUniverseOrigin u,
                  vr::TrackedDevicePose_t *out, uint32_t count);

// ---- events ---------------------------------------------------------------
void push_event(vr::EVREventType type, vr::TrackedDeviceIndex_t device,
                const vr::VREvent_Data_t *data = nullptr);
bool pop_event(vr::VREvent_t *out, uint32_t size);

// ---- the frame ------------------------------------------------------------
// Set by IVRCompositor::WaitGetPoses and read by the rest of the frame.
struct Frame {
    uint32_t id = 0;
    double   display_time = 0;
    klvr_tracked head{};
};
Frame current_frame();
void  set_current_frame(const Frame &f);

// ---- cross-file hooks -----------------------------------------------------
// klvr_vk_submit.cpp. `fence`/`device`, if given, receive the fence the copy
// signals and the device it belongs to.
int  vk_copy_eye(int stage, int eye, const vr::VRVulkanTextureData_t *t, uint32_t array_index,
                 const vr::VRTextureBounds_t *bounds, uint64_t *fence = nullptr, void **device = nullptr,
                 const vr::VRTextureDepthInfo_t *depth = nullptr);
// Wait (up to `seconds`) for fences vk_copy_eye returned. False on timeout.
bool vk_wait_copies(void *device, const uint64_t *fences, uint32_t n, double seconds);
void input_frame_begin(const Frame &f);                        // klvr_input.cpp
void legacy_controller_state(int hand, vr::VRControllerState_t *out);
void legacy_haptic_pulse(int hand, uint32_t axis, uint16_t microseconds);

// ---- runtime --------------------------------------------------------------
bool runtime_initialized();
bool env_on(const char *name, bool def);
float env_float(const char *name, float def);

} // namespace klvr
