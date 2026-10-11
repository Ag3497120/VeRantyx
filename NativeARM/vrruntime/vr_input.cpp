// IVRInput_011: the SteamVR Input system — the game's action manifest and the
// default binding file for the emulated controller, evaluated against the
// Joy-Con/hand data the Vision Pro app sends.
#include "gen_stubs.h"
#include "vr_runtime.h"
#include "json_mini.h"
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <mutex>

namespace {

using namespace vr;

enum ActionType { AT_Boolean, AT_Vector1, AT_Vector2, AT_Vector3, AT_Pose, AT_Skeleton, AT_Vibration, AT_Unknown };

struct Binding {
    int hand;                 // 0 left, 1 right, -1 head/other
    std::string input;        // "trigger", "grip", "joystick", "a", "b", "x", "y", "system", "thumbrest"
    std::string mode;         // "button", "trigger", "joystick", "dpad", "grab", "scalar_constant", "toggle_button"
    std::string component;    // "click", "touch", "pull", "position", "north", ...
    std::string posePath;     // for pose actions
    double clickThreshold = 0.75, touchThreshold = 0.05, deadzone = 0.3, grabHold = 0.7, grabRelease = 0.5;
    bool grabbed = false;     // grab hysteresis state
    bool toggled = false, lastRaw = false;
};

struct Action {
    std::string name;         // "/actions/main/in/Teleport"
    std::string setName;      // "/actions/main"
    ActionType type = AT_Unknown;
    std::vector<Binding> bindings;
    // Current and previous evaluated state.
    bool active = false;
    bool state = false, prevState = false;
    float x = 0, y = 0, z = 0, px = 0, py = 0, pz = 0;
    int hand = -1;            // origin device for the current state
    double updateTime = 0;
};

struct ActionSet { std::string name; bool active = false; };

static std::string lower(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }
static bool starts_with(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

class Input : public Stub_IVRInput {
public:
    std::mutex m_mu;
    std::string m_manifestPath;
    std::vector<ActionSet> m_sets;
    std::vector<Action> m_actions;
    std::map<std::string, VRInputValueHandle_t> m_sources;   // input source path -> handle
    std::vector<std::string> m_sourceNames;
    bool m_loaded = false;
    vrrt::PoseSet m_snapshot;
    ETrackedControllerRole m_dominant = TrackedControllerRole_RightHand;

    static constexpr VRActionSetHandle_t kSetBase = 0x1000;
    static constexpr VRActionHandle_t kActionBase = 0x10000;
    static constexpr VRInputValueHandle_t kLeftHand = 1, kRightHand = 2, kHead = 3, kSourceBase = 0x100;

    Input() {
        source_handle("/user/hand/left"); source_handle("/user/hand/right"); source_handle("/user/head");
    }
    VRInputValueHandle_t source_handle(const std::string &path) {
        auto it = m_sources.find(path);
        if (it != m_sources.end()) return it->second;
        VRInputValueHandle_t h;
        if (path == "/user/hand/left") h = kLeftHand;
        else if (path == "/user/hand/right") h = kRightHand;
        else if (path == "/user/head") h = kHead;
        else h = kSourceBase + m_sourceNames.size();
        m_sources[path] = h;
        m_sourceNames.push_back(path);
        return h;
    }
    static int hand_of(const std::string &path) {
        if (starts_with(path, "/user/hand/left")) return 0;
        if (starts_with(path, "/user/hand/right")) return 1;
        return -1;
    }
    // "/user/hand/left/input/trigger" -> "trigger"
    static std::string input_of(const std::string &path) {
        size_t p = path.find("/input/");
        if (p == std::string::npos) return "";
        std::string rest = path.substr(p + 7);
        size_t s = rest.find('/');
        return s == std::string::npos ? rest : rest.substr(0, s);
    }
    Action *find_action(const std::string &name) {
        std::string l = lower(name);
        for (auto &a : m_actions) if (lower(a.name) == l) return &a;
        return nullptr;
    }
    Action *action_from_handle(VRActionHandle_t h) {
        if (h < kActionBase || h - kActionBase >= m_actions.size()) return nullptr;
        return &m_actions[h - kActionBase];
    }

    void load_locked() {
        if (m_loaded) return;
        m_loaded = true;
        std::string path = m_manifestPath;
        if (path.empty()) path = vrrt::config().gameDir + "/cfg/steamvr_actions/actions.json";
        json::Value manifest; std::string err;
        if (!json::parse_file(path, manifest, &err)) { vrrt::log("action manifest: %s", err.c_str()); return; }
        for (auto &s : manifest["action_sets"].arr) m_sets.push_back({ s["name"].string_or(""), false });
        for (auto &a : manifest["actions"].arr) {
            Action act;
            act.name = a["name"].string_or("");
            std::string t = a["type"].string_or("");
            act.type = t == "boolean" ? AT_Boolean : t == "vector1" ? AT_Vector1 : t == "vector2" ? AT_Vector2 : t == "vector3" ? AT_Vector3
                     : t == "pose" ? AT_Pose : t == "skeleton" ? AT_Skeleton : t == "vibration" ? AT_Vibration : AT_Unknown;
            size_t in = act.name.find("/in/"), out = act.name.find("/out/");
            size_t cut = in != std::string::npos ? in : out;
            act.setName = cut != std::string::npos ? act.name.substr(0, cut) : "";
            m_actions.push_back(act);
        }
        // The binding file for the controller we emulate.
        std::string bindingFile;
        const std::string &ct = vrrt::config().controllerType;
        for (auto &b : manifest["default_bindings"].arr)
            if (bindingFile.empty() && b["controller_type"].string_or("") == ct) bindingFile = b["binding_url"].string_or("");
        const char *override_ = getenv("VERANTYX_BINDINGS");
        if (override_ && *override_) bindingFile = override_;
        if (bindingFile.empty()) { vrrt::log("action manifest has no default binding for controller type %s", ct.c_str()); return; }
        std::string dir = path.substr(0, path.find_last_of('/') + 1);
        json::Value bindings;
        if (!json::parse_file(dir + bindingFile, bindings, &err)) { vrrt::log("bindings: %s", err.c_str()); return; }
        unsigned nb = 0;
        const json::Value &sets = bindings["bindings"];
        for (auto &kv : sets.obj) {
            const json::Value &set = kv.second;
            for (auto &src : set["sources"].arr) {
                std::string path = src["path"].string_or(""), mode = lower(src["mode"].string_or("button"));
                const json::Value &params = src["parameters"];
                for (auto &inp : src["inputs"].obj) {
                    std::string output = inp.second["output"].string_or("");
                    Action *a = find_action(output);
                    if (!a) continue;
                    Binding b;
                    b.hand = hand_of(path); b.input = input_of(path); b.mode = mode; b.component = lower(inp.first);
                    b.clickThreshold = params["click_activate_threshold"].number_or(0.75);
                    b.touchThreshold = params["touch_activate_threshold"].number_or(0.05);
                    b.deadzone = params["deadzone_pct"].number_or(30.0) / 100.0;
                    b.grabHold = params["value_hold_threshold"].number_or(0.7);
                    b.grabRelease = params["value_release_threshold"].number_or(0.5);
                    a->bindings.push_back(b); nb++;
                }
            }
            for (auto &pose : set["poses"].arr) {
                Action *a = find_action(pose["output"].string_or(""));
                if (!a) continue;
                Binding b; b.posePath = pose["path"].string_or(""); b.hand = hand_of(b.posePath); b.mode = "pose";
                a->bindings.push_back(b); nb++;
            }
            for (auto &sk : set["skeleton"].arr) {
                Action *a = find_action(sk["output"].string_or(""));
                if (!a) continue;
                Binding b; b.posePath = sk["path"].string_or(""); b.hand = hand_of(b.posePath); b.mode = "skeleton";
                a->bindings.push_back(b); nb++;
            }
            for (auto &hp : set["haptics"].arr) {
                Action *a = find_action(hp["output"].string_or(""));
                if (!a) continue;
                Binding b; b.posePath = hp["path"].string_or(""); b.hand = hand_of(b.posePath); b.mode = "haptic";
                a->bindings.push_back(b); nb++;
            }
        }
        vrrt::log("action manifest %s: %zu sets, %zu actions; %s: %u bindings", path.c_str(), m_sets.size(), m_actions.size(), bindingFile.c_str(), nb);
    }

    // ---- evaluating one input source component against the controller data ----
    struct HandState { bool present; float trigger, grip, stickX, stickY; bool primary, secondary, stickClick, stickTouch, menu; };
    HandState hand_state(int hand) const {
        HandState s = {};
        const vrrt::SharedHands &h = m_snapshot.hands;
        const bool left = hand == 0;
        s.present = m_snapshot.fresh && (left ? h.leftTracked : h.rightTracked);
        if (!m_snapshot.fresh) return s;
        s.trigger = (left ? h.leftTrigger : h.rightTrigger) / 255.f;
        s.grip = (left ? h.leftGrip : h.rightGrip) / 255.f;
        s.stickX = left ? h.leftStickX : h.rightStickX; s.stickY = left ? h.leftStickY : h.rightStickY;
        if (!std::isfinite(s.stickX) || !std::isfinite(s.stickY)) s.stickX = s.stickY = 0;
        uint32_t b = (left ? h.leftButtons : h.rightButtons) >> (left ? 8 : 0);
        s.primary = b & vrrt::kBtnPrimary; s.secondary = b & vrrt::kBtnSecondary; s.stickClick = b & vrrt::kBtnStickClick;
        s.stickTouch = (b & vrrt::kBtnStickTouch) || s.stickClick || fabsf(s.stickX) >= 0.02f || fabsf(s.stickY) >= 0.02f;
        s.menu = b & vrrt::kBtnMenu;
        return s;
    }
    // Returns the source's scalar/boolean value; vec2 for joystick position.
    bool eval(Binding &b, bool *digital, float *x, float *y) {
        if (b.hand < 0) return false;
        HandState h = hand_state(b.hand);
        *digital = false; *x = *y = 0;
        const std::string &in = b.input, &c = b.component;
        float value = 0; bool click = false, touch = false;
        if (in == "trigger") { value = h.trigger; click = value >= b.clickThreshold; touch = value > b.touchThreshold; }
        else if (in == "grip") { value = h.grip; click = value >= b.clickThreshold; touch = value > b.touchThreshold; }
        else if (in == "joystick" || in == "thumbstick" || in == "trackpad") { *x = h.stickX; *y = h.stickY; click = h.stickClick; touch = h.stickTouch; value = sqrtf(h.stickX * h.stickX + h.stickY * h.stickY); }
        else if (in == "a" || in == "x") { click = touch = h.primary; value = click ? 1.f : 0.f; }
        else if (in == "b" || in == "y") { click = touch = h.secondary; value = click ? 1.f : 0.f; }
        else if (in == "system" || in == "menu" || in == "application_menu") { click = touch = h.menu; value = click ? 1.f : 0.f; }
        else if (in == "thumbrest") { touch = h.grip > 0.05f; click = false; }
        else return false;

        if (b.mode == "dpad") {
            bool engaged = c.find("touch") != std::string::npos ? touch : (click || touch);
            float dz = (float)b.deadzone;
            if (c == "north") *digital = engaged && *y > dz && fabsf(*y) >= fabsf(*x);
            else if (c == "south") *digital = engaged && *y < -dz && fabsf(*y) >= fabsf(*x);
            else if (c == "east") *digital = engaged && *x > dz && fabsf(*x) > fabsf(*y);
            else if (c == "west") *digital = engaged && *x < -dz && fabsf(*x) > fabsf(*y);
            else if (c == "center") *digital = engaged && fabsf(*x) <= dz && fabsf(*y) <= dz;
            return true;
        }
        if (b.mode == "grab") {
            if (!b.grabbed && value >= b.grabHold) b.grabbed = true;
            else if (b.grabbed && value < b.grabRelease) b.grabbed = false;
            *digital = b.grabbed; *x = value; return true;
        }
        if (b.mode == "toggle_button") {
            if (click && !b.lastRaw) b.toggled = !b.toggled;
            b.lastRaw = click; *digital = b.toggled; return true;
        }
        if (b.mode == "scalar_constant") { *digital = click || touch; *x = *digital ? 1.f : 0.f; return true; }
        if (c == "click" || c == "held" || c == "grab") *digital = click;
        else if (c == "touch") *digital = touch;
        else if (c == "pull" || c == "value" || c == "force" || c == "position" || c == "scroll") { *x = (in == "joystick" || in == "thumbstick" || in == "trackpad") ? *x : value; *digital = *x != 0 || *y != 0; }
        else if (c == "north") *digital = touch && *y > b.deadzone;
        else if (c == "south") *digital = touch && *y < -b.deadzone;
        else if (c == "east") *digital = touch && *x > b.deadzone;
        else if (c == "west") *digital = touch && *x < -b.deadzone;
        else *digital = click;
        return true;
    }

    void update_locked() {
        vrrt::PoseSet latest = vrrt::last_poses();
        m_snapshot = latest;
        double now = vrrt::now_seconds();
        for (auto &a : m_actions) {
            bool setActive = false;
            for (auto &s : m_sets) if (s.name == a.setName && s.active) setActive = true;
            a.prevState = a.state; a.px = a.x; a.py = a.y; a.pz = a.z;
            a.state = false; a.x = a.y = a.z = 0; a.hand = -1;
            a.active = setActive && !a.bindings.empty();
            if (!a.active) continue;
            float bestMag = -1;
            for (auto &b : a.bindings) {
                if (b.mode == "pose" || b.mode == "skeleton" || b.mode == "haptic") { a.hand = b.hand; continue; }
                bool d; float x, y;
                if (!eval(b, &d, &x, &y)) continue;
                if (a.type == AT_Boolean) { if (d) { a.state = true; a.hand = b.hand; } }
                else {
                    float mag = fabsf(x) + fabsf(y);
                    if (mag > bestMag) { bestMag = mag; a.x = x; a.y = y; a.hand = b.hand; }
                }
            }
            if (a.type != AT_Boolean) a.state = a.x != 0 || a.y != 0;
            if (a.state != a.prevState || a.x != a.px || a.y != a.py) a.updateTime = now;
        }
    }

    // ---- IVRInput ----
    EVRInputError SetActionManifestPath(const char *path) override {
        std::lock_guard<std::mutex> l(m_mu);
        m_manifestPath = path ? path : "";
        vrrt::log("SetActionManifestPath(%s)", m_manifestPath.c_str());
        m_loaded = false; m_sets.clear(); m_actions.clear();
        load_locked();
        return VRInputError_None;
    }
    EVRInputError GetActionSetHandle(const char *name, VRActionSetHandle_t *h) override {
        std::lock_guard<std::mutex> l(m_mu); load_locked();
        std::string n = lower(name ? name : "");
        for (size_t i = 0; i < m_sets.size(); i++) if (lower(m_sets[i].name) == n) { *h = kSetBase + i; return VRInputError_None; }
        // Unknown sets still get a handle so the game can carry on.
        m_sets.push_back({ name ? name : "", false }); *h = kSetBase + m_sets.size() - 1;
        return VRInputError_None;
    }
    EVRInputError GetActionHandle(const char *name, VRActionHandle_t *h) override {
        std::lock_guard<std::mutex> l(m_mu); load_locked();
        Action *a = find_action(name ? name : "");
        if (!a) { Action n; n.name = name ? name : ""; size_t in = n.name.find("/in/"); n.setName = in != std::string::npos ? n.name.substr(0, in) : ""; m_actions.push_back(n); a = &m_actions.back(); }
        *h = kActionBase + (VRActionHandle_t)(a - &m_actions[0]);
        return VRInputError_None;
    }
    EVRInputError GetInputSourceHandle(const char *path, VRInputValueHandle_t *h) override {
        std::lock_guard<std::mutex> l(m_mu);
        *h = source_handle(path ? path : "");
        return VRInputError_None;
    }
    EVRInputError UpdateActionState(VRActiveActionSet_t *sets, uint32_t size, uint32_t count) override {
        std::lock_guard<std::mutex> l(m_mu); load_locked();
        for (auto &s : m_sets) s.active = false;
        for (uint32_t i = 0; i < count; i++) {
            const VRActiveActionSet_t *s = (const VRActiveActionSet_t *)((const char *)sets + (size_t)i * size);
            if (s->ulActionSet >= kSetBase && s->ulActionSet - kSetBase < m_sets.size()) m_sets[s->ulActionSet - kSetBase].active = true;
        }
        update_locked();
        return VRInputError_None;
    }
    static VRInputValueHandle_t origin_for(int hand) { return hand == 0 ? kLeftHand : hand == 1 ? kRightHand : k_ulInvalidInputValueHandle; }
    EVRInputError GetDigitalActionData(VRActionHandle_t h, InputDigitalActionData_t *d, uint32_t size, VRInputValueHandle_t restrict_) override {
        std::lock_guard<std::mutex> l(m_mu);
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        InputDigitalActionData_t out = {};
        bool allow = restrict_ == k_ulInvalidInputValueHandle || origin_for(a->hand) == restrict_;
        out.bActive = a->active && allow;
        out.activeOrigin = out.bActive ? origin_for(a->hand) : k_ulInvalidInputValueHandle;
        out.bState = out.bActive && a->state;
        out.bChanged = out.bActive && a->state != a->prevState;
        out.fUpdateTime = (float)(a->updateTime - vrrt::now_seconds());
        memcpy(d, &out, std::min<uint32_t>(size, sizeof out));
        return VRInputError_None;
    }
    EVRInputError GetAnalogActionData(VRActionHandle_t h, InputAnalogActionData_t *d, uint32_t size, VRInputValueHandle_t restrict_) override {
        std::lock_guard<std::mutex> l(m_mu);
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        InputAnalogActionData_t out = {};
        bool allow = restrict_ == k_ulInvalidInputValueHandle || origin_for(a->hand) == restrict_;
        out.bActive = a->active && allow;
        out.activeOrigin = out.bActive ? origin_for(a->hand) : k_ulInvalidInputValueHandle;
        if (out.bActive) { out.x = a->x; out.y = a->y; out.z = a->z; out.deltaX = a->x - a->px; out.deltaY = a->y - a->py; out.deltaZ = a->z - a->pz; }
        out.fUpdateTime = (float)(a->updateTime - vrrt::now_seconds());
        memcpy(d, &out, std::min<uint32_t>(size, sizeof out));
        return VRInputError_None;
    }
    EVRInputError pose_data(VRActionHandle_t h, InputPoseActionData_t *d, uint32_t size, VRInputValueHandle_t restrict_) {
        std::lock_guard<std::mutex> l(m_mu);
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        InputPoseActionData_t out = {};
        int hand = a->hand;
        if (restrict_ == kLeftHand) hand = 0; else if (restrict_ == kRightHand) hand = 1;
        out.bActive = a->active && hand >= 0;
        out.activeOrigin = out.bActive ? origin_for(hand) : k_ulInvalidInputValueHandle;
        if (hand >= 0) out.pose = m_snapshot.dev[1 + hand];
        memcpy(d, &out, std::min<uint32_t>(size, sizeof out));
        return VRInputError_None;
    }
    EVRInputError GetPoseActionDataRelativeToNow(VRActionHandle_t h, ETrackingUniverseOrigin, float, InputPoseActionData_t *d, uint32_t size, VRInputValueHandle_t r) override { return pose_data(h, d, size, r); }
    EVRInputError GetPoseActionDataForNextFrame(VRActionHandle_t h, ETrackingUniverseOrigin, InputPoseActionData_t *d, uint32_t size, VRInputValueHandle_t r) override { return pose_data(h, d, size, r); }
    EVRInputError GetSkeletalActionData(VRActionHandle_t h, InputSkeletalActionData_t *d, uint32_t size) override {
        std::lock_guard<std::mutex> l(m_mu);
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        InputSkeletalActionData_t out = {}; out.bActive = false; out.activeOrigin = k_ulInvalidInputValueHandle;
        memcpy(d, &out, std::min<uint32_t>(size, sizeof out));
        return VRInputError_None;
    }
    EVRInputError GetDominantHand(ETrackedControllerRole *r) override { *r = m_dominant; return VRInputError_None; }
    EVRInputError SetDominantHand(ETrackedControllerRole r) override { m_dominant = r; return VRInputError_None; }
    EVRInputError GetBoneCount(VRActionHandle_t, uint32_t *n) override { *n = 0; return VRInputError_NoData; }
    EVRInputError GetBoneHierarchy(VRActionHandle_t, BoneIndex_t *, uint32_t) override { return VRInputError_NoData; }
    EVRInputError GetBoneName(VRActionHandle_t, BoneIndex_t, char *b, uint32_t n) override { if (b && n) b[0] = 0; return VRInputError_NoData; }
    EVRInputError GetSkeletalReferenceTransforms(VRActionHandle_t, EVRSkeletalTransformSpace, EVRSkeletalReferencePose, VRBoneTransform_t *, uint32_t) override { return VRInputError_NoData; }
    EVRInputError GetSkeletalTrackingLevel(VRActionHandle_t, EVRSkeletalTrackingLevel *lvl) override { *lvl = VRSkeletalTracking_Estimated; return VRInputError_NoData; }
    EVRInputError GetSkeletalBoneData(VRActionHandle_t, EVRSkeletalTransformSpace, EVRSkeletalMotionRange, VRBoneTransform_t *, uint32_t) override { return VRInputError_NoData; }
    EVRInputError GetSkeletalSummaryData(VRActionHandle_t, EVRSummaryType, VRSkeletalSummaryData_t *) override { return VRInputError_NoData; }
    EVRInputError GetSkeletalBoneDataCompressed(VRActionHandle_t, EVRSkeletalMotionRange, void *, uint32_t, uint32_t *req) override { if (req) *req = 0; return VRInputError_NoData; }
    EVRInputError DecompressSkeletalBoneData(const void *, uint32_t, EVRSkeletalTransformSpace, VRBoneTransform_t *, uint32_t) override { return VRInputError_NoData; }
    EVRInputError TriggerHapticVibrationAction(VRActionHandle_t, float, float, float, float, VRInputValueHandle_t) override { return VRInputError_None; }
    EVRInputError GetActionOrigins(VRActionSetHandle_t, VRActionHandle_t h, VRInputValueHandle_t *out, uint32_t cap) override {
        std::lock_guard<std::mutex> l(m_mu);
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        uint32_t n = 0;
        bool have[2] = { false, false };
        for (auto &b : a->bindings) if (b.hand >= 0 && !have[b.hand]) { have[b.hand] = true; if (n < cap) out[n++] = origin_for(b.hand); }
        for (uint32_t i = n; i < cap; i++) out[i] = k_ulInvalidInputValueHandle;
        return VRInputError_None;
    }
    EVRInputError GetOriginLocalizedName(VRInputValueHandle_t origin, char *buf, uint32_t cap, int32_t) override {
        const char *s = origin == kLeftHand ? "Left Hand" : origin == kRightHand ? "Right Hand" : origin == kHead ? "Head" : "";
        if (buf && cap) { strncpy(buf, s, cap); buf[cap - 1] = 0; }
        return VRInputError_None;
    }
    EVRInputError GetOriginTrackedDeviceInfo(VRInputValueHandle_t origin, InputOriginInfo_t *info, uint32_t size) override {
        InputOriginInfo_t out = {};
        out.devicePath = origin;
        out.trackedDeviceIndex = origin == kLeftHand ? 1 : origin == kRightHand ? 2 : origin == kHead ? 0 : k_unTrackedDeviceIndexInvalid;
        if (out.trackedDeviceIndex == k_unTrackedDeviceIndexInvalid) return VRInputError_InvalidHandle;
        memcpy(info, &out, std::min<uint32_t>(size, sizeof out));
        return VRInputError_None;
    }
    EVRInputError GetActionBindingInfo(VRActionHandle_t h, InputBindingInfo_t *info, uint32_t size, uint32_t cap, uint32_t *ret) override {
        std::lock_guard<std::mutex> l(m_mu);
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        uint32_t n = 0;
        for (auto &b : a->bindings) {
            if (n >= cap) break;
            InputBindingInfo_t *o = (InputBindingInfo_t *)((char *)info + (size_t)n * size);
            memset(o, 0, size);
            snprintf(o->rchDevicePathName, sizeof o->rchDevicePathName, "%s", b.hand == 0 ? "/user/hand/left" : b.hand == 1 ? "/user/hand/right" : "/user/head");
            snprintf(o->rchInputPathName, sizeof o->rchInputPathName, "/user/hand/%s/input/%s", b.hand == 0 ? "left" : "right", b.input.c_str());
            snprintf(o->rchModeName, sizeof o->rchModeName, "%s", b.mode.c_str());
            snprintf(o->rchSlotName, sizeof o->rchSlotName, "%s", b.component.c_str());
            snprintf(o->rchInputSourceType, sizeof o->rchInputSourceType, "%s", b.input.c_str());
            n++;
        }
        if (ret) *ret = n;
        return VRInputError_None;
    }
    EVRInputError ShowActionOrigins(VRActionSetHandle_t, VRActionHandle_t) override { return VRInputError_None; }
    EVRInputError ShowBindingsForActionSet(VRActiveActionSet_t *, uint32_t, uint32_t, VRInputValueHandle_t) override { return VRInputError_None; }
    EVRInputError GetComponentStateForBinding(const char *, const char *, const InputBindingInfo_t *, uint32_t, uint32_t, RenderModel_ComponentState_t *) override { return VRInputError_NoData; }
    bool IsUsingLegacyInput() override { return false; }
    EVRInputError OpenBindingUI(const char *, VRActionSetHandle_t, VRInputValueHandle_t, bool) override { return VRInputError_None; }
    EVRInputError GetBindingVariant(VRInputValueHandle_t, char *buf, uint32_t cap) override { if (buf && cap) buf[0] = 0; return VRInputError_None; }
};

Input g_input;

} // namespace

vr::IVRInput *vrrt_input() { return &g_input; }
