// IVRInput_011: the SteamVR Input system — the game's action manifest and the
// default binding file for the emulated controller, evaluated against the
// Joy-Con/hand data the Vision Pro app sends.
#include "gen_stubs.h"
#include "vr_runtime.h"
#include "vr_skeleton.h"
#include "json_mini.h"
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <algorithm>
#include <climits>
#include <set>
#include <atomic>

namespace {

using namespace vr;

enum ActionType { AT_Boolean, AT_Vector1, AT_Vector2, AT_Vector3, AT_Pose, AT_Skeleton, AT_Vibration, AT_Unknown };

struct Binding {
    int hand;                 // 0 left, 1 right, -1 head/other
    std::string input;        // "trigger", "grip", "joystick", "a", "b", "x", "y", "system", "thumbrest"
    std::string mode;         // "button", "trigger", "joystick", "dpad", "grab", "scalar_constant", "toggle_button"
    std::string component;    // "click", "touch", "pull", "position", "north", ...
    std::string forceInput, subMode;
    std::string posePath;     // for pose actions
    double clickThreshold = 0.75, touchThreshold = 0.05, deadzone = 0.3, grabHold = 0.7, grabRelease = 0.5;
    double clickRelease = 0.75, maxzone = 1.0;
    bool clicked = false;
    bool grabbed = false;     // grab hysteresis state
    bool toggled = false, lastRaw = false;
};

struct ActionState {
    bool active = false;
    bool state = false, prevState = false;
    float x = 0, y = 0, z = 0, px = 0, py = 0, pz = 0;
    double updateTime = 0;
};

struct Action : ActionState {
    std::string name;         // "/actions/main/in/Teleport"
    std::string setName;      // "/actions/main"
    ActionType type = AT_Unknown;
    std::vector<Binding> bindings;
    ActionState hands[2];     // restrictions must preserve each hand's releases and analog deltas
    int hand = -1;            // origin device for the current state
};

struct ActionSet { std::string name; unsigned active = 0; int32_t priority[2] = { INT_MIN, INT_MIN }; };

static std::string lower(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }
static bool starts_with(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }
static double parameter(const json::Value &v, double fallback) {
    double n = v.number_or(fallback);
    // SteamVR's binding editor stores parameter values as JSON strings.
    if (v.is_string()) {
        char *end = nullptr; n = strtod(v.str.c_str(), &end);
        if (end == v.str.c_str() || *end) return fallback;
    }
    return std::isfinite(n) ? n : fallback;
}

class Input : public Stub_IVRInput {
public:
    std::mutex m_mu;
    std::string m_manifestPath;
    std::vector<ActionSet> m_sets;
    std::vector<Action> m_actions;
    std::map<std::string, VRInputValueHandle_t> m_sources;   // input source path -> handle
    std::vector<std::string> m_sourceNames;
    bool m_loaded = false;
    std::atomic<bool> m_actionInput{false};
    EVRInputError m_loadError = VRInputError_None;
    vrrt::PoseSet m_snapshot = {};
    ETrackedControllerRole m_dominant = TrackedControllerRole_RightHand;
    uint64_t m_updates = 0, m_updateErrors = 0, m_digitalReads = 0, m_analogReads = 0, m_poseReads = 0;
    double m_lastUpdate = 0, m_lastErrorReport = 0;
    uint32_t m_motionSignature = UINT32_MAX;
    double m_lastMotionReport = 0;
    bool m_motionReportPending = false;
    std::set<std::string> m_reportedLookups;

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
    const std::string *source_path(VRInputValueHandle_t handle) const {
        for (const auto &source : m_sources) if (source.second == handle) return &source.first;
        return nullptr;
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
        m_loadError = VRInputError_None;
        if (!json::parse_file(path, manifest, &err)) { vrrt::log("action manifest: %s", err.c_str()); m_loadError = VRInputError_InvalidParam; return; }
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
        if (bindingFile.empty()) { vrrt::log("action manifest has no default binding for controller type %s", ct.c_str()); m_loadError = VRInputError_MismatchedActionManifest; return; }
        std::string dir = path.substr(0, path.find_last_of('/') + 1);
        json::Value bindings;
        std::string bindingPath = bindingFile[0] == '/' ? bindingFile : dir + bindingFile;
        if (!json::parse_file(bindingPath, bindings, &err)) { vrrt::log("bindings: %s", err.c_str()); m_loadError = VRInputError_InvalidParam; return; }
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
                    b.forceInput = lower(params["force_input"].string_or(""));
                    b.subMode = lower(params["sub_mode"].string_or("touch"));
                    const bool positionClick = b.forceInput == "position";
                    // SteamVR's button filter defaults for analog position.
                    b.clickThreshold = std::clamp(parameter(params["click_activate_threshold"], positionClick ? 0.25 : 0.75), 0.0, 1.0);
                    b.clickRelease = std::clamp(parameter(params["click_deactivate_threshold"], positionClick ? 0.2 : b.clickThreshold), 0.0, b.clickThreshold);
                    b.touchThreshold = parameter(params["touch_activate_threshold"], 0.05);
                    b.deadzone = std::clamp(parameter(params["deadzone_pct"], mode == "joystick" ? 0.0 : 30.0) / 100.0, 0.0, 0.99);
                    b.maxzone = std::clamp(parameter(params["maxzone_pct"], 100.0) / 100.0, b.deadzone + 0.001, 1.0);
                    b.grabHold = parameter(params["value_hold_threshold"], 0.7);
                    b.grabRelease = parameter(params["value_release_threshold"], 0.5);
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
        // SteamVR Input replaces the legacy controller-state channel. Keep
        // this selected across failed reloads, so raw buttons cannot leak back.
        m_actionInput.store(true, std::memory_order_release);
        vrrt::log("action manifest %s: %zu sets, %zu actions; %s: %u bindings", path.c_str(), m_sets.size(), m_actions.size(), bindingFile.c_str(), nb);
    }

    // ---- evaluating one input source component against the controller data ----
    struct HandState { bool present; float trigger, grip, stickX, stickY; bool primary, secondary, stickClick, stickTouch, menu; };
    HandState hand_state(int hand, const vrrt::PoseSet *live = nullptr) const {
        HandState s = {};
        const auto &snapshot = live ? *live : m_snapshot;
        const vrrt::SharedHands &h = snapshot.hands;
        const bool left = hand == 0;
        s.present = snapshot.inputsFresh && (left ? h.leftTracked : h.rightTracked);
        if (!snapshot.inputsFresh) return s;
        s.trigger = (left ? h.leftTrigger : h.rightTrigger) / 255.f;
        s.grip = (left ? h.leftGrip : h.rightGrip) / 255.f;
        s.stickX = left ? h.leftStickX : h.rightStickX; s.stickY = left ? h.leftStickY : h.rightStickY;
        if (!std::isfinite(s.stickX) || !std::isfinite(s.stickY)) s.stickX = s.stickY = 0;
        s.stickX = std::clamp(s.stickX, -1.f, 1.f); s.stickY = std::clamp(s.stickY, -1.f, 1.f);
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
        if (!m_snapshot.inputsFresh) { b.grabbed = b.toggled = b.lastRaw = b.clicked = false; return true; }
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

        const bool stick = in == "joystick" || in == "thumbstick" || in == "trackpad";
        // Alyx's Walk is a boolean threshold of stick deflection, independent
        // of the physical stick click used for other actions on that source.
        if (stick && b.forceInput == "position") value = std::min(1.f, value);
        if (in == "trigger" || in == "grip" || (stick && b.forceInput == "position")) {
            if (!b.clicked && value >= b.clickThreshold) b.clicked = true;
            else if (b.clicked && value < b.clickRelease) b.clicked = false;
            click = b.clicked;
        }
        if (stick && b.mode == "joystick" && c == "position") {
            const float radius = value;
            const float output = (float)std::clamp((radius - b.deadzone) / (b.maxzone - b.deadzone), 0.0, 1.0);
            const float scale = radius > 0 ? output / radius : 0;
            *x *= scale; *y *= scale;
        }
        if (b.mode == "dpad") {
            bool engaged = b.subMode == "click" ? click : touch;
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
        std::map<std::pair<int, std::string>, int32_t> priorities;
        for (auto &a : m_actions) for (auto &s : m_sets) if (lower(s.name) == lower(a.setName))
            for (auto &b : a.bindings) if (b.hand >= 0 && (s.active & (1u << b.hand)) && !b.input.empty()) {
                auto key = std::make_pair(b.hand, b.input);
                auto it = priorities.find(key);
                if (it == priorities.end() || it->second < s.priority[b.hand]) priorities[key] = s.priority[b.hand];
            }
        for (auto &a : m_actions) {
            const ActionSet *set = nullptr;
            for (auto &s : m_sets) if (lower(s.name) == lower(a.setName)) set = &s;
            a.prevState = a.state; a.px = a.x; a.py = a.y; a.pz = a.z;
            a.state = false; a.x = a.y = a.z = 0; a.active = false;
            for (auto &h : a.hands) {
                h.prevState = h.state; h.px = h.x; h.py = h.y; h.pz = h.z;
                h.state = h.active = false; h.x = h.y = h.z = 0;
            }
            for (auto &b : a.bindings) {
                if (b.hand < 0 || !set || !(set->active & (1u << b.hand))) continue;
                if (!b.input.empty() && priorities[{b.hand, b.input}] > set->priority[b.hand]) continue;
                ActionState &h = a.hands[b.hand];
                h.active = true;
                if (b.mode == "pose" || b.mode == "skeleton" || b.mode == "haptic") continue;
                bool d; float x, y;
                if (!eval(b, &d, &x, &y)) continue;
                if (a.type == AT_Boolean) h.state |= d;
                else if (fabsf(x) + fabsf(y) >= fabsf(h.x) + fabsf(h.y)) { h.x = x; h.y = y; }
            }
            for (auto &h : a.hands) {
                if (a.type != AT_Boolean) h.state = h.x != 0 || h.y != 0;
                if (!h.updateTime || h.state != h.prevState || h.x != h.px || h.y != h.py) h.updateTime = now;
                a.active |= h.active;
            }
            int selected = a.hand >= 0 && a.hands[a.hand].active ? a.hand : a.hands[0].active ? 0 : a.hands[1].active ? 1 : -1;
            for (int hand = 0; hand < 2; hand++) if (a.hands[hand].active) {
                if (a.type == AT_Boolean ? a.hands[hand].state :
                    selected < 0 || fabsf(a.hands[hand].x) + fabsf(a.hands[hand].y) > fabsf(a.hands[selected].x) + fabsf(a.hands[selected].y)) selected = hand;
            }
            a.hand = selected;
            if (selected >= 0) { a.state = a.hands[selected].state; a.x = a.hands[selected].x; a.y = a.hands[selected].y; }
            if (!a.updateTime || a.state != a.prevState || a.x != a.px || a.y != a.py) a.updateTime = now;
        }
        report_locomotion(now);
    }

    void report_locomotion(double now) {
        const Action *walk = nullptr, *left = nullptr, *right = nullptr, *continuous = nullptr;
        for (auto &a : m_actions) {
            const auto n = lower(a.name);
            if (n == "/actions/move/in/walk") walk = &a;
            else if (n == "/actions/move/in/turnleft") left = &a;
            else if (n == "/actions/move/in/turnright") right = &a;
            else if (n == "/actions/move/in/continuousturn") continuous = &a;
        }
        if (!walk && !left && !right && !continuous) return;
        auto state = [](const Action *a) { return a && a->active && a->state; };
        auto label = [](const Action *a) { return !a || !a->active ? "inactive" : a->state ? "on" : "off"; };
        const auto l = hand_state(0), r = hand_state(1);
        const uint32_t lb = l.primary | (l.secondary << 1), rb = r.primary | (r.secondary << 1);
        const uint32_t signature = lb | (rb << 2) | (state(walk) << 4) | (state(left) << 5) | (state(right) << 6) |
            (state(continuous) << 7) | ((walk && walk->active) << 8) | ((continuous && continuous->active) << 9);
        m_motionReportPending |= signature != m_motionSignature;
        m_motionSignature = signature;
        if (!m_motionReportPending || now - m_lastMotionReport < 0.2) return;
        m_lastMotionReport = now; m_motionReportPending = false;
        vrrt::log("locomotion: walk=%s turnLeft=%s turnRight=%s continuous=(%.2f,%.2f) active=%d sticks L=(%.2f,%.2f) R=(%.2f,%.2f) faceButtons L=%u R=%u",
                  label(walk), label(left), label(right), continuous ? continuous->x : 0, continuous ? continuous->y : 0,
                  (int)(continuous && continuous->active), l.stickX, l.stickY, r.stickX, r.stickY, lb, rb);
    }

    // Every 5 s: what the headset sends per hand, which sets the game has
    // active, and the grab-related actions — enough to tell a missing input
    // from a binding that does not fire.
    double m_lastInputReport = 0;
    void report_inputs(const vrrt::PoseSet &latest) {
        const double now = vrrt::now_seconds();
        if (now - m_lastInputReport < 5.0) return;
        m_lastInputReport = now;
        vrrt::log("input API: updates=%llu errors=%llu digital=%llu analog=%llu pose=%llu lastUpdateAge=%.2fs",
                  (unsigned long long)m_updates, (unsigned long long)m_updateErrors, (unsigned long long)m_digitalReads,
                  (unsigned long long)m_analogReads, (unsigned long long)m_poseReads, m_lastUpdate ? now - m_lastUpdate : -1.0);
        std::string sets;
        for (auto &s : m_sets) if (s.active) { if (!sets.empty()) sets += ","; sets += s.name.substr(s.name.rfind('/') + 1); }
        for (int hand = 0; hand < 2; hand++) {
            HandState h = hand_state(hand, &latest);
            vrrt::log("input %s: fresh %d tracked %d trigger %.2f grip %.2f stick (%.2f,%.2f) A/X %d B/Y %d click %d touch %d menu %d",
                      hand ? "right" : "left", (int)latest.inputsFresh, (int)h.present, h.trigger, h.grip, h.stickX, h.stickY,
                      h.primary, h.secondary, h.stickClick, h.stickTouch, h.menu);
        }
        std::string acts;
        for (auto &a : m_actions) {
            const std::string n = lower(a.name);
            if (n.find("/use") != std::string::npos || n.find("handcurl") != std::string::npos || n.find("gglock") != std::string::npos ||
                n.find("teleport") != std::string::npos || n.find("continuousturn") != std::string::npos || n.find("walk") != std::string::npos ||
                n.find("/turnleft") != std::string::npos || n.find("/turnright") != std::string::npos ||
                n.find("triggerpull") != std::string::npos || n.find("menuinteract") != std::string::npos || n.find("menudismiss") != std::string::npos) {
                char buf[160];
                snprintf(buf, sizeof buf, " %s=%s%s", n.substr(n.find("/in/") + 4).c_str(),
                         a.active ? (a.type == AT_Boolean ? (a.state ? "on" : "off") : "") : "inactive",
                         a.active && a.type != AT_Boolean ? (std::to_string(a.x).substr(0, 4) + "," + std::to_string(a.y).substr(0, 4)).c_str() : "");
                acts += buf;
            }
        }
        vrrt::log("input sets [%s]%s", sets.c_str(), acts.c_str());
    }

    void report_lookup(const char *kind, const char *name, uint64_t handle, EVRInputError error) {
        std::string key = std::string(kind) + ":" + name + ":" + std::to_string(error);
        if (m_reportedLookups.size() < 256 && m_reportedLookups.insert(key).second)
            vrrt::log("input %s %s -> %#llx error=%d", kind, name, (unsigned long long)handle, error);
    }
    EVRInputError update_error(EVRInputError error, const char *reason, uint64_t value) {
        m_updateErrors++;
        const double now = vrrt::now_seconds();
        if (m_updateErrors <= 8 || now - m_lastErrorReport >= 5) {
            m_lastErrorReport = now;
            vrrt::log("UpdateActionState failed: %s=%#llx error=%d (errors=%llu)", reason,
                      (unsigned long long)value, error, (unsigned long long)m_updateErrors);
        }
        return error;
    }

    // ---- IVRInput ----
    EVRInputError SetActionManifestPath(const char *path) override {
        if (!path || !*path) return VRInputError_InvalidParam;
        std::lock_guard<std::mutex> l(m_mu);
        m_manifestPath = path ? path : "";
        vrrt::log("SetActionManifestPath(%s)", m_manifestPath.c_str());
        m_loaded = false; m_sets.clear(); m_actions.clear();
        load_locked();
        // Loading synchronously still needs OpenVR's asynchronous notification
        // so applications waiting for binding completion can enable input.
        VREvent_t event = {};
        event.trackedDeviceIndex = k_unTrackedDeviceIndexInvalid;
        event.eventType = m_loadError == VRInputError_None ? VREvent_Input_BindingLoadSuccessful : VREvent_Input_BindingLoadFailed;
        vrrt::push_event(event);
        if (m_loadError == VRInputError_None) {
            event.eventType = VREvent_Input_BindingsUpdated;
            vrrt::push_event(event);
        }
        vrrt::log("input binding notification: %s", m_loadError == VRInputError_None ? "loaded" : "failed");
        return m_loadError;
    }
    EVRInputError GetActionSetHandle(const char *name, VRActionSetHandle_t *h) override {
        if (!name || !h) return VRInputError_InvalidParam;
        std::lock_guard<std::mutex> l(m_mu); load_locked();
        std::string n = lower(name ? name : "");
        for (size_t i = 0; i < m_sets.size(); i++) if (lower(m_sets[i].name) == n) {
            *h = kSetBase + i; report_lookup("set", name, *h, VRInputError_None); return VRInputError_None;
        }
        *h = k_ulInvalidActionSetHandle;
        report_lookup("set", name, *h, VRInputError_NameNotFound);
        return VRInputError_NameNotFound;
    }
    EVRInputError GetActionHandle(const char *name, VRActionHandle_t *h) override {
        if (!name || !h) return VRInputError_InvalidParam;
        std::lock_guard<std::mutex> l(m_mu); load_locked();
        Action *a = find_action(name ? name : "");
        if (!a) { *h = k_ulInvalidActionHandle; report_lookup("action", name, *h, VRInputError_NameNotFound); return VRInputError_NameNotFound; }
        *h = kActionBase + (VRActionHandle_t)(a - &m_actions[0]);
        report_lookup("action", name, *h, VRInputError_None);
        return VRInputError_None;
    }
    EVRInputError GetInputSourceHandle(const char *path, VRInputValueHandle_t *h) override {
        if (!path || !h) return VRInputError_InvalidParam;
        std::lock_guard<std::mutex> l(m_mu);
        *h = source_handle(path ? path : "");
        report_lookup("source", path, *h, VRInputError_None);
        return VRInputError_None;
    }
    EVRInputError UpdateActionState(VRActiveActionSet_t *sets, uint32_t size, uint32_t count) override {
        std::lock_guard<std::mutex> l(m_mu);
        m_updates++;
        if (size < sizeof(VRActiveActionSet_t) || (count && !sets)) return update_error(VRInputError_InvalidParam, "elementSize", size);
        load_locked();
        if (m_loadError != VRInputError_None) return update_error(m_loadError, "manifest", 0);
        auto validSet = [this](VRActionSetHandle_t handle) { return handle >= kSetBase && handle - kSetBase < m_sets.size(); };
        // A path returned by GetInputSourceHandle is valid even when no local
        // device has bindings for it. Alyx includes /user/gamepad alongside its
        // hand sets; rejecting it discards every hand's update each frame.
        // Validate the whole request before replacing the previous snapshot.
        for (uint32_t i = 0; i < count; i++) {
            const auto *s = (const VRActiveActionSet_t *)((const char *)sets + (size_t)i * size);
            if (!validSet(s->ulActionSet)) return update_error(VRInputError_InvalidHandle, "actionSet", s->ulActionSet);
            if (s->ulRestrictedToDevice != k_ulInvalidInputValueHandle) {
                if (!source_path(s->ulRestrictedToDevice)) return update_error(VRInputError_InvalidHandle, "restrictedDevice", s->ulRestrictedToDevice);
                if (s->ulSecondaryActionSet != k_ulInvalidActionSetHandle && !validSet(s->ulSecondaryActionSet))
                    return update_error(VRInputError_InvalidHandle, "secondarySet", s->ulSecondaryActionSet);
            }
        }
        for (auto &s : m_sets) { s.active = 0; s.priority[0] = s.priority[1] = INT_MIN; }
        auto activate = [this](VRActionSetHandle_t handle, unsigned mask, int32_t priority) {
            ActionSet &s = m_sets[handle - kSetBase];
            s.active |= mask;
            for (int hand = 0; hand < 2; hand++) if (mask & (1u << hand)) s.priority[hand] = std::max(s.priority[hand], priority);
        };
        for (uint32_t i = 0; i < count; i++) {
            const VRActiveActionSet_t *s = (const VRActiveActionSet_t *)((const char *)sets + (size_t)i * size);
            const auto *path = source_path(s->ulRestrictedToDevice);
            const int hand = path ? hand_of(*path) : -1;
            const unsigned mask = s->ulRestrictedToDevice == k_ulInvalidInputValueHandle ? 3u : hand >= 0 ? 1u << hand : 0u;
            activate(s->ulActionSet, mask, s->nPriority);
            if (mask != 3 && s->ulSecondaryActionSet != k_ulInvalidActionSetHandle &&
                s->ulRestrictedToDevice != k_ulInvalidInputValueHandle) activate(s->ulSecondaryActionSet, 3u ^ mask, s->nPriority);
        }
        update_locked();
        m_lastUpdate = vrrt::now_seconds();
        return VRInputError_None;
    }
    static VRInputValueHandle_t origin_for(int hand) { return hand == 0 ? kLeftHand : hand == 1 ? kRightHand : k_ulInvalidInputValueHandle; }
    const ActionState *restricted_state(const Action &a, VRInputValueHandle_t restrict_, int *hand) const {
        if (restrict_ == k_ulInvalidInputValueHandle) { *hand = a.hand; return &a; }
        const auto *path = source_path(restrict_);
        *hand = path ? hand_of(*path) : -1;
        if (!path) return nullptr;
        static const ActionState unbound;
        return *hand >= 0 ? &a.hands[*hand] : &unbound;
    }
    EVRInputError GetDigitalActionData(VRActionHandle_t h, InputDigitalActionData_t *d, uint32_t size, VRInputValueHandle_t restrict_) override {
        if (!d || size < sizeof *d) return VRInputError_InvalidParam;
        std::lock_guard<std::mutex> l(m_mu);
        m_digitalReads++;
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        if (a->type != AT_Boolean) return VRInputError_WrongType;
        InputDigitalActionData_t out = {};
        int hand; const ActionState *s = restricted_state(*a, restrict_, &hand);
        if (!s) return VRInputError_InvalidHandle;
        out.bActive = s->active;
        out.activeOrigin = out.bActive ? origin_for(hand) : k_ulInvalidInputValueHandle;
        out.bState = out.bActive && s->state;
        out.bChanged = out.bActive && s->state != s->prevState;
        out.fUpdateTime = out.bActive ? (float)(s->updateTime - vrrt::now_seconds()) : 0.f;
        memcpy(d, &out, std::min<uint32_t>(size, sizeof out));
        return VRInputError_None;
    }
    EVRInputError GetAnalogActionData(VRActionHandle_t h, InputAnalogActionData_t *d, uint32_t size, VRInputValueHandle_t restrict_) override {
        if (!d || size < sizeof *d) return VRInputError_InvalidParam;
        std::lock_guard<std::mutex> l(m_mu);
        m_analogReads++;
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        if (a->type != AT_Vector1 && a->type != AT_Vector2 && a->type != AT_Vector3) return VRInputError_WrongType;
        InputAnalogActionData_t out = {};
        int hand; const ActionState *s = restricted_state(*a, restrict_, &hand);
        if (!s) return VRInputError_InvalidHandle;
        out.bActive = s->active;
        out.activeOrigin = out.bActive ? origin_for(hand) : k_ulInvalidInputValueHandle;
        if (out.bActive) { out.x = s->x; out.y = s->y; out.z = s->z; out.deltaX = s->x - s->px; out.deltaY = s->y - s->py; out.deltaZ = s->z - s->pz; }
        out.fUpdateTime = out.bActive ? (float)(s->updateTime - vrrt::now_seconds()) : 0.f;
        memcpy(d, &out, std::min<uint32_t>(size, sizeof out));
        return VRInputError_None;
    }
    EVRInputError pose_data(VRActionHandle_t h, InputPoseActionData_t *d, uint32_t size, VRInputValueHandle_t restrict_) {
        if (!d || size < sizeof *d) return VRInputError_InvalidParam;
        std::lock_guard<std::mutex> l(m_mu);
        m_poseReads++;
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        if (a->type != AT_Pose) return VRInputError_WrongType;
        InputPoseActionData_t out = {};
        int hand; const ActionState *s = restricted_state(*a, restrict_, &hand);
        if (!s) return VRInputError_InvalidHandle;
        out.bActive = s->active && hand >= 0;
        out.activeOrigin = out.bActive ? origin_for(hand) : k_ulInvalidInputValueHandle;
        if (hand >= 0) out.pose = m_snapshot.dev[1 + hand];
        memcpy(d, &out, std::min<uint32_t>(size, sizeof out));
        return VRInputError_None;
    }
    EVRInputError GetPoseActionDataRelativeToNow(VRActionHandle_t h, ETrackingUniverseOrigin, float, InputPoseActionData_t *d, uint32_t size, VRInputValueHandle_t r) override { return pose_data(h, d, size, r); }
    EVRInputError GetPoseActionDataForNextFrame(VRActionHandle_t h, ETrackingUniverseOrigin, InputPoseActionData_t *d, uint32_t size, VRInputValueHandle_t r) override { return pose_data(h, d, size, r); }
    // ---- skeletal input: fingers from pinch/fist/thumb (vr_skeleton.cpp) ----
    vrrt::SkeletonState m_skel[2];
    int skeleton_hand(VRActionHandle_t h) {
        Action *a = action_from_handle(h);
        if (!a || a->type != AT_Skeleton) return -1;
        for (auto &b : a->bindings) if (b.hand >= 0) return b.hand;
        return a->name.find("left") != std::string::npos || a->name.find("Left") != std::string::npos ? 0 : 1;
    }
    vrrt::FingerInputs finger_inputs(int hand) const {
        HandState st = hand_state(hand);
        vrrt::FingerInputs in;
        in.trigger = st.trigger;
        in.grip = st.grip;
        in.indexTouch = st.trigger > 0.05f;
        in.thumbTouch = st.primary || st.secondary || st.stickTouch || st.grip > 0.6f;
        return in;
    }
    EVRInputError GetSkeletalActionData(VRActionHandle_t h, InputSkeletalActionData_t *d, uint32_t size) override {
        if (!d || size < sizeof *d) return VRInputError_InvalidParam;
        std::lock_guard<std::mutex> l(m_mu);
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        if (a->type != AT_Skeleton) return VRInputError_WrongType;
        InputSkeletalActionData_t out = {};
        int hand = skeleton_hand(h);
        out.bActive = a->active && hand >= 0;
        out.activeOrigin = out.bActive ? origin_for(hand) : k_ulInvalidInputValueHandle;
        memcpy(d, &out, std::min<uint32_t>(size, sizeof out));
        return VRInputError_None;
    }
    EVRInputError GetDominantHand(ETrackedControllerRole *r) override { *r = m_dominant; return VRInputError_None; }
    EVRInputError SetDominantHand(ETrackedControllerRole r) override { m_dominant = r; return VRInputError_None; }
    EVRInputError GetBoneCount(VRActionHandle_t, uint32_t *n) override { if (n) *n = vrrt::kSkeletonBones; return VRInputError_None; }
    EVRInputError GetBoneHierarchy(VRActionHandle_t, BoneIndex_t *parents, uint32_t n) override {
        if (!parents || n < vrrt::kSkeletonBones) return VRInputError_BufferTooSmall;
        for (uint32_t i = 0; i < vrrt::kSkeletonBones; i++) parents[i] = (BoneIndex_t)vrrt::skeleton_parent(i);
        return VRInputError_None;
    }
    EVRInputError GetBoneName(VRActionHandle_t, BoneIndex_t i, char *b, uint32_t n) override {
        if (!b || !n || i < 0 || (uint32_t)i >= vrrt::kSkeletonBones) return VRInputError_InvalidParam;
        snprintf(b, n, "%s", vrrt::skeleton_bone_name((uint32_t)i));
        return VRInputError_None;
    }
    EVRInputError GetSkeletalReferenceTransforms(VRActionHandle_t h, EVRSkeletalTransformSpace space, EVRSkeletalReferencePose pose,
                                                 VRBoneTransform_t *out, uint32_t n) override {
        if (!out || n < vrrt::kSkeletonBones) return VRInputError_BufferTooSmall;
        int hand;
        { std::lock_guard<std::mutex> l(m_mu); hand = skeleton_hand(h); }
        vrrt::skeleton_reference(hand == 0, pose, out);
        if (space == VRSkeletalTransformSpace_Model) vrrt::skeleton_to_model(out);
        return VRInputError_None;
    }
    EVRInputError GetSkeletalTrackingLevel(VRActionHandle_t, EVRSkeletalTrackingLevel *lvl) override {
        if (lvl) *lvl = VRSkeletalTracking_Estimated;   // from controller-style values, like SteamVR's Touch skeleton
        return VRInputError_None;
    }
    EVRInputError GetSkeletalBoneData(VRActionHandle_t h, EVRSkeletalTransformSpace space, EVRSkeletalMotionRange range,
                                      VRBoneTransform_t *out, uint32_t n) override {
        if (!out || n < vrrt::kSkeletonBones) return VRInputError_BufferTooSmall;
        std::lock_guard<std::mutex> l(m_mu);
        int hand = skeleton_hand(h);
        if (hand < 0) return VRInputError_WrongType;
        vrrt::skeleton_bones(hand == 0, range == VRSkeletalMotionRange_WithController, finger_inputs(hand), m_skel[hand],
                             vrrt::now_seconds(), out);
        if (space == VRSkeletalTransformSpace_Model) vrrt::skeleton_to_model(out);
        static int said; if (!said++) vrrt::log("skeleton: bone data served (hand %d, %s, %s space)", hand,
                                                 range == VRSkeletalMotionRange_WithController ? "with controller" : "without controller",
                                                 space == VRSkeletalTransformSpace_Model ? "model" : "parent");
        return VRInputError_None;
    }
    EVRInputError GetSkeletalSummaryData(VRActionHandle_t h, EVRSummaryType, VRSkeletalSummaryData_t *d) override {
        if (!d) return VRInputError_InvalidParam;
        std::lock_guard<std::mutex> l(m_mu);
        int hand = skeleton_hand(h);
        if (hand < 0) return VRInputError_WrongType;
        vrrt::FingerInputs in = finger_inputs(hand);
        d->flFingerCurl[0] = in.thumbTouch ? 0.6f : 0.f;
        d->flFingerCurl[1] = in.trigger;
        d->flFingerCurl[2] = d->flFingerCurl[3] = d->flFingerCurl[4] = in.grip;
        for (int i = 0; i < 4; i++) d->flFingerSplay[i] = 0.2f;
        return VRInputError_None;
    }
    EVRInputError GetSkeletalBoneDataCompressed(VRActionHandle_t, EVRSkeletalMotionRange, void *, uint32_t, uint32_t *req) override { if (req) *req = 0; return VRInputError_NoData; }
    EVRInputError DecompressSkeletalBoneData(const void *, uint32_t, EVRSkeletalTransformSpace, VRBoneTransform_t *, uint32_t) override { return VRInputError_NoData; }
    EVRInputError TriggerHapticVibrationAction(VRActionHandle_t h, float start, float duration, float frequency, float amplitude, VRInputValueHandle_t restrict_) override {
        std::lock_guard<std::mutex> l(m_mu);
        Action *a = action_from_handle(h);
        if (!a) return VRInputError_InvalidHandle;
        if (a->type != AT_Vibration) return VRInputError_WrongType;
        if (restrict_ != k_ulInvalidInputValueHandle && restrict_ != kLeftHand && restrict_ != kRightHand) return VRInputError_InvalidHandle;
        bool sent[2] = { false, false };
        for (auto &b : a->bindings) if (b.mode == "haptic" && b.hand >= 0 && !sent[b.hand] &&
            (restrict_ == k_ulInvalidInputValueHandle || origin_for(b.hand) == restrict_)) {
            if (!vrrt::haptic(b.hand, start, duration, frequency, amplitude)) return VRInputError_InvalidParam;
            sent[b.hand] = true;
        }
        return VRInputError_None;
    }
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
        if (!info || size < sizeof *info) return VRInputError_InvalidParam;
        InputOriginInfo_t out = {};
        out.devicePath = origin;
        out.trackedDeviceIndex = origin == kLeftHand ? 1 : origin == kRightHand ? 2 : origin == kHead ? 0 : k_unTrackedDeviceIndexInvalid;
        if (out.trackedDeviceIndex == k_unTrackedDeviceIndexInvalid) return VRInputError_InvalidHandle;
        memcpy(info, &out, std::min<uint32_t>(size, sizeof out));
        return VRInputError_None;
    }
    EVRInputError GetActionBindingInfo(VRActionHandle_t h, InputBindingInfo_t *info, uint32_t size, uint32_t cap, uint32_t *ret) override {
        if (!ret || (cap && (!info || size < sizeof *info))) return VRInputError_InvalidParam;
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
    bool IsUsingLegacyInput() override { return !m_actionInput.load(std::memory_order_acquire); }
    EVRInputError OpenBindingUI(const char *, VRActionSetHandle_t, VRInputValueHandle_t, bool) override { return VRInputError_None; }
    EVRInputError GetBindingVariant(VRInputValueHandle_t, char *buf, uint32_t cap) override { if (buf && cap) buf[0] = 0; return VRInputError_None; }
};

Input g_input;

} // namespace

vr::IVRInput *vrrt_input() { return &g_input; }

void vrrt::report_inputs(const PoseSet &p) {
    std::lock_guard<std::mutex> l(g_input.m_mu);
    g_input.report_inputs(p);
}

bool vrrt::legacy_input_enabled() { return g_input.IsUsingLegacyInput(); }
