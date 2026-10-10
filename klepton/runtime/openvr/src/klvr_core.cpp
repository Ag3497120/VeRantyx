// The loader half of libopenvr_api, plus the state every interface shares.
//
// Alyx ships its own libopenvr_api.so and calls VR_InitInternal2 /
// VR_GetGenericInterface on it. On Linux that library finds SteamVR through
// ~/.config/openvr/openvrpaths.vrpath and dlopens vrclient.so. Here the
// library is REPLACED rather than translated, for the reason Klepton replaces
// libOVRPlugin.so and the OpenXR loader: a loader's whole job is to find a
// runtime, and there is no SteamVR to find. Klepton's relinker resolves the
// game's imports of these symbols to this file.
#include "klvr_internal.h"
#include <array>
#include <atomic>
#include <utility>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <set>
#include <string>

namespace klvr {

// ---- logging --------------------------------------------------------------
void logf(const char *fmt, ...) {
    char line[1024];
    int n = snprintf(line, sizeof line, "[openvr] ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    klvr_plat_log(line);
}

uint32_t put_string(const char *s, char *buf, uint32_t size) {
    if (!s) s = "";
    uint32_t need = (uint32_t)strlen(s) + 1;
    if (buf && size) {
        uint32_t n = need < size ? need : size;
        memcpy(buf, s, n - 1);
        buf[n - 1] = 0;
    }
    return need;
}

bool env_on(const char *name, bool def) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

float env_float(const char *name, float def) {
    const char *v = getenv(name);
    return (v && *v) ? strtof(v, nullptr) : def;
}

// ---- math -----------------------------------------------------------------
Quat qmul(Quat a, Quat b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
Quat qconj(Quat a) { return { -a.x, -a.y, -a.z, a.w }; }
Quat qnormalize(Quat a) {
    float n = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    if (n < 1e-12f) return { 0, 0, 0, 1 };
    return { a.x / n, a.y / n, a.z / n, a.w / n };
}
Quat qslerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b = { -b.x, -b.y, -b.z, -b.w }; d = -d; }
    if (d > 0.9995f) {
        return qnormalize({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                            a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t });
    }
    float th = std::acos(d), s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return { a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb };
}
Vec3 qrot(Quat q, Vec3 v) {
    Quat p = qmul(qmul(q, { v.x, v.y, v.z, 0 }), qconj(q));
    return { p.x, p.y, p.z };
}
Quat qfrom_axis_angle(Vec3 a, float r) {
    float s = std::sin(r / 2);
    return { a.x * s, a.y * s, a.z * s, std::cos(r / 2) };
}

Pose to_pose(const klvr_pose &k) {
    return { { k.p[0], k.p[1], k.p[2] }, qnormalize({ k.q[0], k.q[1], k.q[2], k.q[3] }) };
}
Pose pmul(const Pose &a, const Pose &b) {
    return { vadd(a.p, qrot(a.q, b.p)), qnormalize(qmul(a.q, b.q)) };
}
Pose pinv(const Pose &a) {
    Quat qi = qconj(a.q);
    return { qrot(qi, vscale(a.p, -1)), qi };
}

vr::HmdMatrix34_t to_mat34(const Pose &p) {
    const Quat q = p.q;
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    vr::HmdMatrix34_t m;
    m.m[0][0] = 1 - 2 * (yy + zz); m.m[0][1] = 2 * (xy - wz);     m.m[0][2] = 2 * (xz + wy);     m.m[0][3] = p.p.x;
    m.m[1][0] = 2 * (xy + wz);     m.m[1][1] = 1 - 2 * (xx + zz); m.m[1][2] = 2 * (yz - wx);     m.m[1][3] = p.p.y;
    m.m[2][0] = 2 * (xz - wy);     m.m[2][1] = 2 * (yz + wx);     m.m[2][2] = 1 - 2 * (xx + yy); m.m[2][3] = p.p.z;
    return m;
}

Pose from_mat34(const vr::HmdMatrix34_t &m) {
    Pose p;
    p.p = { m.m[0][3], m.m[1][3], m.m[2][3] };
    float tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
    Quat q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2;
        q = { (m.m[2][1] - m.m[1][2]) / s, (m.m[0][2] - m.m[2][0]) / s, (m.m[1][0] - m.m[0][1]) / s, 0.25f * s };
    } else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
        float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
        q = { 0.25f * s, (m.m[0][1] + m.m[1][0]) / s, (m.m[0][2] + m.m[2][0]) / s, (m.m[2][1] - m.m[1][2]) / s };
    } else if (m.m[1][1] > m.m[2][2]) {
        float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
        q = { (m.m[0][1] + m.m[1][0]) / s, 0.25f * s, (m.m[1][2] + m.m[2][1]) / s, (m.m[0][2] - m.m[2][0]) / s };
    } else {
        float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
        q = { (m.m[0][2] + m.m[2][0]) / s, (m.m[1][2] + m.m[2][1]) / s, 0.25f * s, (m.m[1][0] - m.m[0][1]) / s };
    }
    p.q = qnormalize(q);
    return p;
}

vr::HmdMatrix34_t identity34() {
    return to_mat34({ { 0, 0, 0 }, { 0, 0, 0, 1 } });
}

// ---- tracking universe ----------------------------------------------------
static std::mutex g_universe_mu;
static bool g_seated_set = false;
static Pose g_seated_zero = { { 0, 1.6f, 0 }, { 0, 0, 0, 1 } };

static Pose yaw_only(const Pose &p) {
    // Keep rotation about +Y only: project the forward vector onto the floor.
    Vec3 f = qrot(p.q, { 0, 0, -1 });
    float yaw = std::atan2(-f.x, -f.z);
    return { p.p, qfrom_axis_angle({ 0, 1, 0 }, yaw) };
}

void reset_seated_zero() {
    klvr_tracked h{};
    klvr_plat_head(klvr_plat_now(), &h);
    std::lock_guard<std::mutex> l(g_universe_mu);
    if (h.valid) g_seated_zero = yaw_only(to_pose(h.pose));
    g_seated_set = true;
}

// ---- KLVR_WALK: smooth locomotion by moving the play space ------------------
// Alyx's own continuous mode does not respond to our stick (see
// docs/OVERNIGHT.md). Room-scale walking always works, so the left stick
// walks the whole tracking space: every pose the game gets is shifted by
// g_walk, exactly as if the player had physically walked there, and the game
// resolves collision as it does for real walking. Updated once per frame (in
// WaitGetPoses) so the pose a frame was rendered from and the pose the shim
// converts back at Submit use the same offset.
static Vec3 g_walk = { 0, 0, 0 };
static std::mutex g_walk_mu;

bool walk_enabled() {
    static int on = -1;
    if (on < 0) on = env_on("KLVR_WALK", false);
    return on;
}

void walk_update(double dt) {
    if (!walk_enabled() || !(dt > 0) || dt > 0.1) return;
    static float speed = -1;
    if (speed < 0) {
        const char *e = getenv("KLVR_WALK_SPEED");
        speed = e && *e ? (float)atof(e) : 1.9f;    // m/s; Alyx's hlvr_continuous_normal_speed is 75 in/s
    }
    klvr_controller c{};
    klvr_plat_controller(0, &c);
    float sx = c.stick[0], sy = c.stick[1];
    float mag = std::sqrt(sx * sx + sy * sy);
    if (!c.connected || mag < 0.15f) return;
    // Heading from the head (the floor-space pose, before any walk offset).
    klvr_tracked h{};
    klvr_plat_head(klvr_plat_now(), &h);
    if (!h.valid) return;
    Quat q = { h.pose.q[0], h.pose.q[1], h.pose.q[2], h.pose.q[3] };
    Vec3 fwd = qrot(q, { 0, 0, -1 }), right = qrot(q, { 1, 0, 0 });
    fwd.y = 0; right.y = 0;
    float fl = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z), rl = std::sqrt(right.x * right.x + right.z * right.z);
    if (fl < 1e-3f || rl < 1e-3f) return;
    float k = speed * (float)dt * std::min(1.0f, (mag - 0.15f) / 0.85f) / mag;
    std::lock_guard<std::mutex> l(g_walk_mu);
    g_walk.x += k * (sy * fwd.x / fl + sx * right.x / rl);
    g_walk.z += k * (sy * fwd.z / fl + sx * right.z / rl);
}

static Pose walk_offset() {
    std::lock_guard<std::mutex> l(g_walk_mu);
    return { g_walk, { 0, 0, 0, 1 } };
}

Pose universe_from_floor(vr::ETrackingUniverseOrigin u) {
    if (walk_enabled()) {
        // The walk is a translation of the floor space itself, so it applies
        // under every universe: universe <- walked floor <- floor.
        Pose base = { { 0, 0, 0 }, { 0, 0, 0, 1 } };
        if (u == vr::TrackingUniverseSeated) {
            bool need;
            { std::lock_guard<std::mutex> l(g_universe_mu); need = !g_seated_set; }
            if (need) reset_seated_zero();
            std::lock_guard<std::mutex> l(g_universe_mu);
            base = pinv(g_seated_zero);
        }
        return pmul(base, walk_offset());
    }
    if (u != vr::TrackingUniverseSeated) return { { 0, 0, 0 }, { 0, 0, 0, 1 } };
    bool need;
    { std::lock_guard<std::mutex> l(g_universe_mu); need = !g_seated_set; }
    if (need) reset_seated_zero();
    std::lock_guard<std::mutex> l(g_universe_mu);
    return pinv(g_seated_zero);
}

void fill_pose(vr::TrackedDevicePose_t *out, const klvr_tracked &t,
               vr::ETrackingUniverseOrigin u, const Pose *offset) {
    memset(out, 0, sizeof *out);
    out->bDeviceIsConnected = true;
    if (!t.valid) {
        out->bPoseIsValid = false;
        out->eTrackingResult = vr::TrackingResult_Running_OutOfRange;
        out->mDeviceToAbsoluteTracking = identity34();
        return;
    }
    Pose uf = universe_from_floor(u);
    Pose p = pmul(uf, to_pose(t.pose));
    if (offset) p = pmul(p, *offset);
    out->mDeviceToAbsoluteTracking = to_mat34(p);
    Vec3 v = qrot(uf.q, { t.vel[0], t.vel[1], t.vel[2] });
    Vec3 a = qrot(uf.q, { t.ang[0], t.ang[1], t.ang[2] });
    // OpenVR has no "velocity unknown" flag; zero is the least wrong answer.
    // KLVR_POSE_VELOCITY=0 withholds velocities: an A/B for motion that looks
    // extrapolated the wrong way (a frame or axis mismatch in them).
    static bool vel = env_on("KLVR_POSE_VELOCITY", true);
    if (!t.motion_known || !vel) v = a = { 0, 0, 0 };
    out->vVelocity = { { v.x, v.y, v.z } };
    out->vAngularVelocity = { { a.x, a.y, a.z } };
    out->bPoseIsValid = true;
    out->eTrackingResult = vr::TrackingResult_Running_OK;
}

// The controllers' device ("raw") pose, expressed in the grip pose the
// platform reports (an OpenXR-convention grip; see klvr_platform.h).
//
// SteamVR reports a Touch controller at its device origin, not at the grip,
// and the skeleton and Alyx's per-controller hand offsets are built on that
// frame. SteamVR's own render models say where the grip sits in it: the
// "openxr_grip" component of oculus_quest2_controller_{left,right} is
// origin (-/+0.007, -0.0018, 0.1019), rotate_xyz (20.6, 0, 0). raw-from-grip
// is the inverse of that. KLVR_RAW_FROM_GRIP="px,py,pz,pitch_deg" replaces it
// for both hands.
static std::array<Pose, 2> make_raw_from_grip() {
    std::array<Pose, 2> r;
    float x = 0, y = 0, z = 0, pitch = 0;
    const char *v = getenv("KLVR_RAW_FROM_GRIP");
    if (v && sscanf(v, "%f,%f,%f,%f", &x, &y, &z, &pitch) >= 3) {
        r[0] = r[1] = { { x, y, z }, qfrom_axis_angle({ 1, 0, 0 }, pitch * 3.14159265f / 180) };
        return r;
    }
    for (int h = 0; h < 2; h++) {
        Pose grip_in_raw = { { h ? 0.007f : -0.007f, -0.00182941f, 0.1019482f },
                             qfrom_axis_angle({ 1, 0, 0 }, 20.6f * 3.14159265f / 180) };
        r[h] = pinv(grip_in_raw);
    }
    return r;
}

const Pose &raw_from_grip(int hand) {
    static const std::array<Pose, 2> p = make_raw_from_grip();
    return p[hand ? 1 : 0];
}

void device_poses(double t, vr::ETrackingUniverseOrigin u,
                  vr::TrackedDevicePose_t *out, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof out[i]);
        out[i].eTrackingResult = vr::TrackingResult_Uninitialized;
        out[i].mDeviceToAbsoluteTracking = identity34();
    }
    if (count > kHmd) {
        klvr_tracked h{};
        Frame f = current_frame();
        if (f.id && f.display_time == t) h = f.head;
        else klvr_plat_head(t, &h);
        fill_pose(&out[kHmd], h, u);
    }
    for (int hand = 0; hand < 2; hand++) {
        vr::TrackedDeviceIndex_t d = device_of_hand(hand);
        if (count <= d) continue;
        klvr_controller c{};
        klvr_plat_controller(hand, &c);
        if (!c.connected) continue;
        klvr_tracked g{};
        klvr_plat_hand(hand, t, &g);
        fill_pose(&out[d], g, u, &raw_from_grip(hand));
    }
}

// ---- events ---------------------------------------------------------------
static std::mutex g_event_mu;
static std::deque<vr::VREvent_t> g_events;

void push_event(vr::EVREventType type, vr::TrackedDeviceIndex_t device,
                const vr::VREvent_Data_t *data) {
    vr::VREvent_t e;
    memset(&e, 0, sizeof e);
    e.eventType = type;
    e.trackedDeviceIndex = device;
    if (data) e.data = *data;
    std::lock_guard<std::mutex> l(g_event_mu);
    if (g_events.size() > 256) g_events.pop_front();
    g_events.push_back(e);
}

bool pop_event(vr::VREvent_t *out, uint32_t size) {
    std::lock_guard<std::mutex> l(g_event_mu);
    if (g_events.empty()) return false;
    if (out && size) memcpy(out, &g_events.front(), size < sizeof(vr::VREvent_t) ? size : sizeof(vr::VREvent_t));
    g_events.pop_front();
    return true;
}

// ---- the frame ------------------------------------------------------------
static std::mutex g_frame_mu;
static Frame g_frame;
Frame current_frame() { std::lock_guard<std::mutex> l(g_frame_mu); return g_frame; }
void set_current_frame(const Frame &f) { std::lock_guard<std::mutex> l(g_frame_mu); g_frame = f; }

// ---- interface registry ---------------------------------------------------
const InterfaceVersion *find_interface(const char *version) {
    if (!version) return nullptr;
    for (size_t i = 0; i < kInterfaceVersionCount; i++)
        if (strcmp(kInterfaceVersions[i].version, version) == 0) return &kInterfaceVersions[i];
    return nullptr;
}

static std::atomic<bool> g_initialized{false};
static std::atomic<uint32_t> g_init_token{0};
bool runtime_initialized() { return g_initialized.load(); }

} // namespace klvr

using namespace klvr;

// Every method nobody implemented lands here, once per name, so the first run
// on device prints the exact work list. KLVR_STRICT=1 aborts instead, which is
// Klepton's "calls are assertions" rule for when we want the trace to stop at
// the first gap.
extern "C" void klvr_unimplemented(const char *iface, const char *method) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::string key = std::string(iface) + "::" + method;
    {
        std::lock_guard<std::mutex> l(mu);
        if (!seen.insert(key).second) return;
    }
    logf("NOT IMPLEMENTED: %s (returning zero)", key.c_str());
    if (env_on("KLVR_STRICT", false)) abort();
}

extern "C" void klvr_unrepresentable_slot(void) {
    logf("NOT IMPLEMENTED: a method whose signature uses a type the current SDK no longer has");
    if (env_on("KLVR_STRICT", false)) abort();
}

// ---- the libopenvr_api exports --------------------------------------------
// Defined inside namespace vr, as Valve's openvr_api does, so they match the
// declarations in openvr.h (built with VR_API_EXPORT, which exports them).
namespace {
// One object per interface version: a vtable pointer and nothing else. The
// methods keep their state in globals, so `this` is never read.
struct Object { const void *const *vtable; };
Object g_objects[512];
std::once_flag g_objects_once;

void *object_for(const InterfaceVersion *iv) {
    std::call_once(g_objects_once, [] {
        for (size_t i = 0; i < kInterfaceVersionCount && i < 512; i++)
            g_objects[i].vtable = kInterfaceVersions[i].vtable;
    });
    return &g_objects[iv - kInterfaceVersions];
}
} // namespace

namespace vr {
// Two older entry points openvr.h no longer declares.
VR_INTERFACE uint32_t VR_CALLTYPE VR_InitInternal(EVRInitError *err, EVRApplicationType type);
VR_INTERFACE const char *VR_CALLTYPE VR_RuntimePath();

VR_INTERFACE uint32_t VR_InitInternal2(vr::EVRInitError *err, vr::EVRApplicationType type,
                                      const char *startup_info) {
    logf("VR_InitInternal2(type=%d%s%s)", (int)type, startup_info ? ", " : "",
         startup_info ? startup_info : "");
    if (!g_initialized.exchange(true)) {
        push_event(vr::VREvent_TrackedDeviceActivated, kHmd);
        push_event(vr::VREvent_TrackedDeviceActivated, kLeftHand);
        push_event(vr::VREvent_TrackedDeviceActivated, kRightHand);
    }
    if (err) *err = vr::VRInitError_None;
    return ++g_init_token;
}

VR_INTERFACE uint32_t VR_InitInternal(vr::EVRInitError *err, vr::EVRApplicationType type) {
    return VR_InitInternal2(err, type, nullptr);
}

VR_INTERFACE void VR_ShutdownInternal() {
    logf("VR_ShutdownInternal");
    g_initialized = false;
    ++g_init_token;
}

VR_INTERFACE bool VR_IsHmdPresent() { return true; }
VR_INTERFACE bool VR_IsRuntimeInstalled() { return true; }
VR_INTERFACE uint32_t VR_GetInitToken() { return g_init_token.load(); }

VR_INTERFACE const char *VR_RuntimePath() { return "/klepton/openvr"; }
VR_INTERFACE bool VR_GetRuntimePath(char *buf, uint32_t size, uint32_t *required) {
    uint32_t n = put_string(VR_RuntimePath(), buf, size);
    if (required) *required = n;
    return n <= size;
}

// ---- private interfaces ---------------------------------------------------
// libvr.so also asks for interfaces private to SteamVR, absent from every
// public SDK (klvr_private.cpp serves them). Slots whose meaning is unknown
// land on a probe that logs "IVRMailbox_001 slot 3 called" once and returns
// 0, so a run measures which slots the game really uses instead of handing it
// a null pointer it may not check.
} // namespace vr
namespace {
template <int N> uint64_t probe_slot(klvr::PrivateObject *self) {
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) logf("NOT IMPLEMENTED: %s slot %d called (private interface, returning 0)", self->name, N);
    if (env_on("KLVR_STRICT", false)) abort();
    return 0;
}
template <int... I> constexpr std::array<const void *, sizeof...(I)> probe_table(std::integer_sequence<int, I...>) {
    return { { (const void *)&probe_slot<I>... } };
}
const auto kProbeTable = probe_table(std::make_integer_sequence<int, klvr::kProbeSlotCount>{});
} // namespace
const void *const *const klvr::kProbeSlots = kProbeTable.data();
using klvr::trace_wrap;
namespace vr {

VR_INTERFACE bool VR_IsInterfaceVersionValid(const char *version) {
    bool ok = find_interface(version) != nullptr || private_interface(version) != nullptr;
    if (!ok) logf("VR_IsInterfaceVersionValid(%s) = false", version ? version : "(null)");
    return ok;
}

VR_INTERFACE void *VR_GetGenericInterface(const char *version, vr::EVRInitError *err) {
    if (version && strncmp(version, "FnTable:", 8) == 0) {
        // The C function-table flavour. Source 2 uses the C++ interfaces;
        // if this prints, it is the next thing to build.
        logf("VR_GetGenericInterface(%s): FnTable interfaces are not served", version);
        if (err) *err = vr::VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }
    const InterfaceVersion *iv = find_interface(version);
    if (!iv) {
        if (void *p = private_interface(version)) {
            logf("VR_GetGenericInterface(%s): private SteamVR interface", version);
            if (err) *err = vr::VRInitError_None;
            return p;
        }
        if (version && strcmp(version, "IXrProto_001") == 0) {
            // Private too, and SteamVR's public builds do not serve it either:
            // Alyx asks, gets InterfaceNotFound, and carries on.
            logf("VR_GetGenericInterface(IXrProto_001): not served, as in public SteamVR");
            if (err) *err = vr::VRInitError_Init_InterfaceNotFound;
            return nullptr;
        }
        logf("VR_GetGenericInterface(%s): unknown version", version ? version : "(null)");
        if (err) *err = vr::VRInitError_Init_InterfaceNotFound;
        return nullptr;
    }
    static std::mutex mu;
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> l(mu);
        if (seen.insert(version).second)
            logf("VR_GetGenericInterface(%s): %u slots, as of SDK %s", version, iv->slots, iv->sdk_tag);
    }
    if (err) *err = vr::VRInitError_None;
    return trace_wrap(object_for(iv), version, iv->slots);
}

VR_INTERFACE const char *VR_GetVRInitErrorAsSymbol(vr::EVRInitError e) {
    switch (e) {
    case vr::VRInitError_None: return "VRInitError_None";
    case vr::VRInitError_Init_InterfaceNotFound: return "VRInitError_Init_InterfaceNotFound";
    case vr::VRInitError_Init_HmdNotFound: return "VRInitError_Init_HmdNotFound";
    default: return "VRInitError_Unknown";
    }
}

VR_INTERFACE const char *VR_GetVRInitErrorAsEnglishDescription(vr::EVRInitError e) {
    switch (e) {
    case vr::VRInitError_None: return "No Error (0)";
    case vr::VRInitError_Init_InterfaceNotFound: return "Interface not found (105)";
    default: return "Error (see the klepton OpenVR shim log)";
    }
}

} // namespace vr
