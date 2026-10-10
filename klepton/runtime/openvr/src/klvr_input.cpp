// IVRInput: SteamVR Input, from the game's action manifest to PSVR2 Sense.
//
// A SteamVR Input game never names a button. It ships an action manifest
// (actions.json: "/actions/default/in/Teleport" is a boolean, and so on) and
// one binding file per controller type that maps actions onto controller
// components ("/user/hand/right/input/joystick" in "dpad" mode, north = the
// teleport). SteamVR evaluates those bindings every UpdateActionState. So
// does this file, against the Sense controllers, reading the binding file the
// game itself shipped for the controller type we present (Oculus Touch by
// default: see klvr_system.cpp).
#include "klvr_internal.h"
#include "klvr_json.h"
#include "klvr_skeleton.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" void kl_fault_print_frames(FILE *f, void *fp) __attribute__((weak));
namespace klvr {
namespace {

// ---- physical inputs ------------------------------------------------------
enum Comp { C_None, C_Trigger, C_Grip, C_Stick, C_FaceLower, C_FaceUpper, C_Menu, C_System };

struct Reading { bool click = false, touch = false; float value = 0, x = 0, y = 0; };

Reading read(const klvr_controller &c, Comp comp) {
    Reading r;
    switch (comp) {
    case C_Trigger:
        r.value = c.trigger;
        r.click = (c.buttons & KLVR_BTN_TRIGGER) || c.trigger >= 0.95f;
        r.touch = (c.touches & KLVR_TOUCH_TRIGGER) || c.trigger > 0.0f;
        break;
    case C_Grip:
        r.value = c.grip;
        r.click = (c.buttons & KLVR_BTN_GRIP) || c.grip >= 0.95f;
        r.touch = (c.touches & KLVR_TOUCH_GRIP) || c.grip > 0.0f;
        break;
    case C_Stick:
        r.x = c.stick[0]; r.y = c.stick[1];
        r.value = std::sqrt(r.x * r.x + r.y * r.y);
        r.click = c.buttons & KLVR_BTN_STICK;
        r.touch = (c.touches & KLVR_TOUCH_STICK) || r.click || r.value > 0.1f;
        break;
    case C_FaceLower:
        r.click = c.buttons & KLVR_BTN_FACE_LOWER; r.touch = r.click || (c.touches & KLVR_TOUCH_FACE_LOWER);
        break;
    case C_FaceUpper:
        r.click = c.buttons & KLVR_BTN_FACE_UPPER; r.touch = r.click || (c.touches & KLVR_TOUCH_FACE_UPPER);
        break;
    case C_Menu: r.click = r.touch = c.buttons & KLVR_BTN_MENU; break;
    case C_System: r.click = r.touch = c.buttons & KLVR_BTN_SYSTEM; break;
    default: break;
    }
    if (r.click && comp != C_Stick) r.value = std::max(r.value, 1.0f);
    return r;
}

// "/user/hand/left/input/trigger" -> (0, C_Trigger). Every controller type's
// names are accepted, so a binding file for Index or Vive wands still lands
// somewhere sensible if it is the only one the game ships.
bool parse_input_path(const std::string &path, int &hand, Comp &comp) {
    static const char L[] = "/user/hand/left/input/", R[] = "/user/hand/right/input/";
    std::string rest;
    if (path.compare(0, sizeof L - 1, L) == 0) { hand = 0; rest = path.substr(sizeof L - 1); }
    else if (path.compare(0, sizeof R - 1, R) == 0) { hand = 1; rest = path.substr(sizeof R - 1); }
    else return false;
    if (rest == "trigger") comp = C_Trigger;
    else if (rest == "grip") comp = C_Grip;
    else if (rest == "joystick" || rest == "thumbstick" || rest == "trackpad") comp = C_Stick;
    else if (rest == "a" || rest == "x") comp = C_FaceLower;
    else if (rest == "b" || rest == "y") comp = C_FaceUpper;
    else if (rest == "application_menu" || rest == "menu") comp = C_Menu;
    else if (rest == "system") comp = C_System;
    else return false;
    return true;
}

int parse_hand_path(const std::string &path) {
    if (path.compare(0, 15, "/user/hand/left") == 0) return 0;
    if (path.compare(0, 16, "/user/hand/right") == 0) return 1;
    return -1;
}

std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// ---- actions and bindings -------------------------------------------------
enum ActionType { A_Unknown, A_Boolean, A_Vector1, A_Vector2, A_Vector3, A_Pose, A_Skeleton, A_Vibration };

enum Sub {
    S_Click, S_Touch, S_Value, S_Position, S_Constant, S_Toggle, S_Grab,
    S_North, S_South, S_East, S_West, S_Center,
    S_Forced,                    // "button" mode with force_input: click made from position/force
};

struct Binding {
    int hand = 0;
    Comp comp = C_None;
    Sub sub = S_Click;
    bool dpad_click = false;     // dpad sub_mode "click": direction needs a press
    float constant = 0;
    float on_at = 0, off_at = 0; // S_Forced: click_activate/deactivate_threshold
    std::string pose;            // for pose actions: raw, tip, grip...
    // toggle/grab/forced state
    bool latch = false, prev = false;
};

struct State {
    bool active = false, bstate = false, changed = false;
    float v[3] = { 0, 0, 0 }, dv[3] = { 0, 0, 0 };
    double update_time = 0;
    vr::VRInputValueHandle_t origin = 0;
};

struct Action {
    std::string name;            // lower case
    ActionType type = A_Unknown;
    int set = -1;
    int skeleton_hand = -1;      // from the manifest's "skeleton" field
    std::vector<Binding> bindings;
    State state[3];              // [0] any hand, [1] left only, [2] right only
    bool read_logged = false;    // KLVR_TRACE_ACTIONS: this press was read by the game
    bool analog_polled = false;  // KLVR_TRACE_ACTIONS: the game has polled this analog action
    bool analog_on = false;      // ...and has read it deflected since it was last at rest
    int callers_shown = 0;       // KLVR_TRACE_CALLERS
};

struct ActionSet { std::string name; };

std::mutex g_mu;
std::vector<Action> g_actions;
std::vector<ActionSet> g_sets;
std::vector<std::string> g_sources = { "", "/user/hand/left", "/user/hand/right", "/user/head" };
vr::ETrackedControllerRole g_dominant = vr::TrackedControllerRole_RightHand;
std::string g_manifest_dir;
std::atomic<bool> g_uses_input{false};  // an action manifest is set: no legacy input (legacy_controller_state)
double g_last_update = 0;

// Handles: action = index + 1, set = index + 1 | kSetTag. Zero is invalid.
constexpr uint64_t kSetTag = 1ull << 32;
constexpr vr::VRInputValueHandle_t kLeft = 1, kRight = 2, kHead = 3;

Action *action_of(vr::VRActionHandle_t h) {
    return h && h <= g_actions.size() ? &g_actions[h - 1] : nullptr;
}

int set_index(const std::string &name) {
    std::string n = lower(name);
    for (size_t i = 0; i < g_sets.size(); i++) if (g_sets[i].name == n) return (int)i;
    g_sets.push_back({ n });
    return (int)g_sets.size() - 1;
}

std::string set_of_action(const std::string &action) {
    // "/actions/<set>/in/<name>" -> "/actions/<set>"
    size_t p = action.find('/', 9);
    return p == std::string::npos ? action : action.substr(0, p);
}

Action &action_named(const std::string &name) {
    std::string n = lower(name);
    for (auto &a : g_actions) if (a.name == n) return a;
    g_actions.push_back(Action());
    Action &a = g_actions.back();
    a.name = n;
    a.set = set_index(set_of_action(n));
    return a;
}

ActionType type_of(const std::string &t) {
    std::string s = lower(t);
    if (s == "boolean") return A_Boolean;
    if (s == "vector1") return A_Vector1;
    if (s == "vector2") return A_Vector2;
    if (s == "vector3") return A_Vector3;
    if (s == "pose") return A_Pose;
    if (s == "skeleton") return A_Skeleton;
    if (s == "vibration") return A_Vibration;
    return A_Unknown;
}

void add_source_binding(const Json &src) {
    std::string path = lower(src["path"].str()), mode = lower(src["mode"].str());
    int hand; Comp comp;
    if (!parse_input_path(path, hand, comp)) {
        logf("binding: input path %s is not a Sense component, skipped", path.c_str());
        return;
    }
    const Json &params = src["parameters"];
    bool dpad_click = lower(params["sub_mode"].str()) == "click";
    for (auto &kv : src["inputs"].o) {
        std::string in = lower(kv.first), out = kv.second["output"].str();
        if (out.empty()) continue;
        Binding b;
        b.hand = hand;
        b.comp = comp;
        b.dpad_click = dpad_click;
        if (in == "click" || in == "held" || in == "single" || in == "double" || in == "long") b.sub = S_Click;
        else if (in == "touch") b.sub = S_Touch;
        else if (in == "pull" || in == "force" || in == "value") b.sub = S_Value;
        else if (in == "position") b.sub = mode == "toggle_button" ? S_Toggle : S_Position;
        else if (in == "grab") b.sub = S_Grab;
        else if (in == "north") b.sub = S_North;
        else if (in == "south") b.sub = S_South;
        else if (in == "east") b.sub = S_East;
        else if (in == "west") b.sub = S_West;
        else if (in == "center") b.sub = S_Center;
        else {
            logf("binding: %s input '%s' (mode %s) not served", path.c_str(), in.c_str(), mode.c_str());
            continue;
        }
        // "button" mode with "force_input" (SteamVR's "generate click from"):
        // the click comes from the component's position (a stick's deflection)
        // or its force/value crossing click_activate_threshold, and it lets go
        // under click_deactivate_threshold, not from the physical click.
        // Alyx binds /actions/move/in/Walk this way on every controller
        // (joystick, button mode, force_input position), and its continuous
        // locomotion zeroes the stick unless Walk is on for the move hand
        // (libserver 0x137b750). Read as the stick's click, pushing the stick
        // never walked.
        std::string force = lower(params["force_input"].str());
        if (mode == "button" && b.sub == S_Click && (force == "position" || force == "force" || force == "value")) {
            auto threshold = [&](const char *k, float def) {
                const Json &j = params[k];
                return j.type == Json::Number ? (float)j.n : j.str().empty() ? def : (float)atof(j.str().c_str());
            };
            b.sub = S_Forced;
            b.on_at = threshold("click_activate_threshold", 0.25f);
            b.off_at = threshold("click_deactivate_threshold", 0.20f);
            logf("binding: %s from %s %s (on %.2f, off %.2f)", out.c_str(), path.c_str(), force.c_str(), b.on_at, b.off_at);
        }
        if (mode == "scalar_constant") {
            b.sub = S_Constant;
            b.constant = (float)atof(params["on/value"].str().c_str());
            if (b.constant == 0) b.constant = 1.0f;
        }
        // KLVR_UNBIND="/actions/set/in/name,...": drop these actions' bindings
        // (an A/B for bindings that fight each other on one component).
        // KLVR_WALK moves the player with the left stick, so the stick's
        // teleport (dpad) binding goes: both at once would start a teleport
        // arc on every step.
        // ...and the right stick's forward/back step actions (walkmantle,
        // moveback, shiftdashjump): with walking on the left stick, the right
        // stick only turns, like a continuous-locomotion layout.
        if (walk_enabled() && hand == 1 &&
            (lower(out) == "/actions/move/in/walkmantle" || lower(out) == "/actions/move/in/moveback" ||
             lower(out) == "/actions/move/in/shiftdashjump")) {
            static int said;
            if (said++ < 1) logf("binding: right-stick step/back actions dropped (KLVR_WALK: the right stick turns)");
            continue;
        }
        if (walk_enabled() && lower(out) == "/actions/move/in/teleport" && hand == 0) {
            static int said;
            if (said++ < 1) logf("binding: left-stick teleport dropped (KLVR_WALK walks with it)");
            continue;
        }
        if (const char *u = getenv("KLVR_UNBIND")) {
            std::string list = std::string(",") + lower(u) + ",";
            if (list.find("," + lower(out) + ",") != std::string::npos) {
                static int said;
                if (said++ < 8) logf("binding: %s from %s unbound (KLVR_UNBIND)", out.c_str(), path.c_str());
                continue;
            }
        }
        action_named(out).bindings.push_back(b);
    }
}

void load_bindings(const Json &doc) {
    const Json &bindings = doc["bindings"];
    for (auto &set : bindings.o) {
        const Json &s = set.second;
        for (auto &src : s["sources"].a) add_source_binding(src);
        for (auto &p : s["poses"].a) {
            int hand = parse_hand_path(p["path"].str());
            if (hand < 0) continue;
            Binding b;
            b.hand = hand;
            std::string path = lower(p["path"].str());
            size_t k = path.rfind('/');
            b.pose = k == std::string::npos ? "raw" : path.substr(k + 1);
            action_named(p["output"].str()).bindings.push_back(b);
        }
        for (auto &h : s["haptics"].a) {
            int hand = parse_hand_path(h["path"].str());
            if (hand < 0) continue;
            Binding b;
            b.hand = hand;
            action_named(h["output"].str()).bindings.push_back(b);
        }
        for (auto &k : s["skeleton"].a) {
            int hand = parse_hand_path(k["path"].str());
            if (hand < 0) {
                std::string p = lower(k["path"].str());
                hand = p.find("left") != std::string::npos ? 0 : p.find("right") != std::string::npos ? 1 : -1;
            }
            if (hand < 0) continue;
            Binding b;
            b.hand = hand;
            action_named(k["output"].str()).bindings.push_back(b);
        }
        if (s["chords"].size()) logf("binding: %zu chords in %s not served", s["chords"].size(), set.first.c_str());
    }
}

// Skeleton actions the binding file forgot still know their hand from the
// manifest ("skeleton": "/skeleton/hand/left"), which is how SteamVR binds them.
void default_skeleton_bindings() {
    for (auto &a : g_actions) {
        if (a.type != A_Skeleton || !a.bindings.empty() || a.skeleton_hand < 0) continue;
        Binding b;
        b.hand = a.skeleton_hand;
        a.bindings.push_back(b);
    }
}

// Alyx names its manifest "cfg\\actions.json": Windows separators, even in
// the Linux build. Darwin's fopen does not take them.
std::string slashes(std::string p) {
    for (auto &c : p) if (c == '\\') c = '/';
    return p;
}

std::string join_dir(const std::string &dir, const std::string &file) {
    std::string f = slashes(file);
    if (f.empty() || f[0] == '/') return f;
    return dir.empty() ? f : dir + "/" + f;
}

// ---- evaluation -----------------------------------------------------------
bool eval_digital(Binding &b, const klvr_controller &c) {
    Reading r = read(c, b.comp);
    const float dz = 0.5f;
    switch (b.sub) {
    case S_Click: return r.click;
    case S_Touch: return r.touch;
    case S_Value: return r.value >= 0.5f;
    case S_Position: return r.value >= dz;
    case S_Constant: return true;
    case S_Forced:
        // A stick's value is its deflection (read()), so "position" and
        // "force"/"value" both threshold r.value.
        b.latch = b.latch ? r.value > b.off_at : r.value >= b.on_at;
        return b.latch;
    case S_Toggle: {
        bool press = r.click;
        if (press && !b.prev) b.latch = !b.latch;
        b.prev = press;
        return b.latch;
    }
    case S_Grab: {
        // Index-style grab: close past 0.7, release under 0.5.
        float v = std::max(r.value, r.click ? 1.0f : 0.0f);
        b.latch = b.latch ? v > 0.5f : v >= 0.7f;
        return b.latch;
    }
    case S_North: case S_South: case S_East: case S_West: case S_Center: {
        if (b.dpad_click && !r.click) return false;
        float x = r.x, y = r.y, m = std::sqrt(x * x + y * y);
        if (b.sub == S_Center) return b.dpad_click ? m < dz : false;
        if (m < dz) return false;
        bool horiz = std::fabs(x) > std::fabs(y);
        if (b.sub == S_North) return !horiz && y > 0;
        if (b.sub == S_South) return !horiz && y < 0;
        if (b.sub == S_East) return horiz && x > 0;
        return horiz && x < 0;
    }
    }
    return false;
}

void eval_analog(Binding &b, const klvr_controller &c, float out[3]) {
    Reading r = read(c, b.comp);
    out[0] = out[1] = out[2] = 0;
    switch (b.sub) {
    case S_Position: out[0] = r.x; out[1] = r.y; break;
    case S_Value: out[0] = r.value; break;
    case S_Constant: out[0] = b.constant; break;
    default: out[0] = eval_digital(b, c) ? 1.0f : 0.0f; break;
    }
}

// KLVR_TRACE_ACTIONS=1: every change of an action the game can see, and the
// first time the game reads a pressed boolean. Proves the chain from raw bits
// to the game's own GetDigitalActionData, e.g. with KLVR_FAKE_INPUT.
bool trace_actions() {
    static int on = -1;
    if (on < 0) { const char *e = getenv("KLVR_TRACE_ACTIONS"); on = e && *e && *e != '0'; }
    return on;
}

// set_hands[set]: which hands the set is active for this update, bit 0 left
// and bit 1 right; 0 is inactive.
void update_locked(const uint8_t set_hands[]) {
    double now = klvr_plat_now();
    klvr_controller ctl[2];
    for (int h = 0; h < 2; h++) { memset(&ctl[h], 0, sizeof ctl[h]); klvr_plat_controller(h, &ctl[h]); }

    for (auto &a : g_actions) {
        if (a.type == A_Pose || a.type == A_Skeleton || a.type == A_Vibration) continue;
        uint8_t hands = a.set >= 0 ? set_hands[a.set] : 0;
        bool set_on = hands != 0;
        for (int slot = 0; slot < 3; slot++) {
            State &st = a.state[slot];
            State nw;
            nw.update_time = st.update_time;
            for (auto &b : a.bindings) {
                if (!set_on) break;
                if (slot && b.hand != slot - 1) continue;
                if (!(hands & (1u << b.hand))) continue;
                if (!ctl[b.hand].connected) continue;
                nw.active = true;
                if (a.type == A_Boolean) {
                    if (eval_digital(b, ctl[b.hand]) && !nw.bstate) {
                        nw.bstate = true;
                        nw.origin = b.hand ? kRight : kLeft;
                    }
                } else {
                    float v[3];
                    eval_analog(b, ctl[b.hand], v);
                    float mag = std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]);
                    float cur = std::fabs(nw.v[0]) + std::fabs(nw.v[1]) + std::fabs(nw.v[2]);
                    if (mag > cur || !nw.origin) {
                        memcpy(nw.v, v, sizeof v);
                        nw.origin = b.hand ? kRight : kLeft;
                    }
                }
            }
            if (a.type == A_Boolean) {
                nw.changed = nw.bstate != st.bstate;
            } else {
                for (int k = 0; k < 3; k++) nw.dv[k] = nw.v[k] - st.v[k];
                nw.changed = nw.dv[0] != 0 || nw.dv[1] != 0 || nw.dv[2] != 0;
            }
            if (nw.changed) nw.update_time = now;
            if (!nw.origin) nw.origin = st.origin;
            if (slot == 0 && trace_actions()) {
                if (a.type == A_Boolean && nw.changed)
                    logf("action %s = %d (%s)", a.name.c_str(), nw.bstate, nw.origin == kRight ? "right" : "left");
                else if (a.type != A_Boolean) {
                    bool was = std::fabs(st.v[0]) + std::fabs(st.v[1]) >= 0.5f;
                    bool is = std::fabs(nw.v[0]) + std::fabs(nw.v[1]) >= 0.5f;
                    if (was != is) logf("action %s = (%.2f %.2f) (%s)", a.name.c_str(), nw.v[0], nw.v[1],
                                        nw.origin == kRight ? "right" : "left");
                }
                if (a.type == A_Boolean && !nw.bstate) a.read_logged = false;
            }
            st = nw;
        }
    }
    g_last_update = now;
}

// Whether a restriction names a source we actually have: none (any), or a
// hand. Alyx polls every action for /user/gamepad as well as both hands, and
// treating that unknown handle as "any" reported the Sense controllers a
// second time as a gamepad - with the game then reading gamepad movement and
// turning off our stick and face buttons.
// KLVR_GAMEPAD_ANY=1 restores the old "any" answer (the A/B for replays).
bool restrict_is_device(vr::VRInputValueHandle_t restrict_to) {
    static const bool any = env_on("KLVR_GAMEPAD_ANY", false);
    return any || restrict_to == 0 || restrict_to == kLeft || restrict_to == kRight;
}

int slot_of(vr::VRInputValueHandle_t restrict_to) {
    return restrict_to == kLeft ? 1 : restrict_to == kRight ? 2 : 0;
}

// Which hand a pose/skeleton/haptic action follows, honouring a restriction.
int bound_hand(const Action &a, vr::VRInputValueHandle_t restrict_to, const Binding **which = nullptr) {
    for (auto &b : a.bindings) {
        if (restrict_to == kLeft && b.hand != 0) continue;
        if (restrict_to == kRight && b.hand != 1) continue;
        if (which) *which = &b;
        return b.hand;
    }
    if (a.type == A_Skeleton && a.skeleton_hand >= 0) return a.skeleton_hand;
    return -1;
}

// Offsets from the device ("raw") pose to SteamVR's other pose components.
// KLVR_TIP_OFFSET="x,y,z" moves the aim point.
Pose component_offset(const std::string &pose) {
    Pose p = { { 0, 0, 0 }, { 0, 0, 0, 1 } };
    if (pose == "tip") {
        p.p = { 0, 0, -0.05f };
        if (const char *v = getenv("KLVR_TIP_OFFSET")) sscanf(v, "%f,%f,%f", &p.p.x, &p.p.y, &p.p.z);
    }
    return p;
}

// Curls from Sense contact. Thumb: resting on a face button or the stick
// half-curls it, a press closes it. Index: follows the trigger once touched.
// Middle, ring and pinky: follow the grip (digital on Sense, so touch alone
// half-closes them, which is where a relaxed hand around the grip sits).
void curls_from(const klvr_controller &c, float curl[5]) {
    bool thumb_press = c.buttons & (KLVR_BTN_FACE_LOWER | KLVR_BTN_FACE_UPPER | KLVR_BTN_STICK);
    bool thumb_touch = c.touches & (KLVR_TOUCH_FACE_LOWER | KLVR_TOUCH_FACE_UPPER | KLVR_TOUCH_STICK);
    curl[0] = thumb_press ? 1.0f : thumb_touch ? 0.6f : 0.0f;
    bool index_touch = (c.touches & KLVR_TOUCH_TRIGGER) || c.trigger > 0;
    curl[1] = index_touch ? std::max(0.25f, c.trigger) : 0.0f;
    bool grip_touch = (c.touches & KLVR_TOUCH_GRIP) || (c.buttons & KLVR_BTN_GRIP);
    float g = std::max(c.grip, (c.buttons & KLVR_BTN_GRIP) ? 1.0f : grip_touch ? 0.5f : 0.0f);
    curl[2] = curl[3] = curl[4] = g;
}

} // namespace

void input_frame_begin(const Frame &) {}

// Legacy input (IVRSystem::GetControllerState), laid out the way SteamVR
// reports a Touch controller: axis 0 stick, axis 1 trigger, axis 2 grip.
// Once the app has set an action manifest, SteamVR stops legacy input to it:
// the state is empty. Alyx sets one and still polls this, and it turns the
// player 90 degrees on k_EButton_A (X left, A right), so reporting the buttons
// here made X/square and A/cross turn. KLVR_LEGACY_INPUT=1 keeps them (A/B).
void legacy_controller_state(int hand, vr::VRControllerState_t *out) {
    klvr_controller c{};
    klvr_plat_controller(hand, &c);
    memset(out, 0, sizeof *out);
    static uint32_t packet = 0;
    out->unPacketNum = ++packet;
    static const bool keep_legacy = env_on("KLVR_LEGACY_INPUT", false);
    if (g_uses_input && !keep_legacy) return;
    auto bit = [](vr::EVRButtonId b) { return vr::ButtonMaskFromId(b); };
    if (c.buttons & KLVR_BTN_FACE_LOWER) out->ulButtonPressed |= bit(vr::k_EButton_A);
    if (c.buttons & KLVR_BTN_FACE_UPPER) out->ulButtonPressed |= bit(vr::k_EButton_ApplicationMenu);
    if (c.buttons & KLVR_BTN_MENU) out->ulButtonPressed |= bit(vr::k_EButton_ApplicationMenu);
    if (c.buttons & KLVR_BTN_SYSTEM) out->ulButtonPressed |= bit(vr::k_EButton_System);
    if (c.buttons & KLVR_BTN_STICK) out->ulButtonPressed |= bit(vr::k_EButton_Axis0);
    if ((c.buttons & KLVR_BTN_TRIGGER) || c.trigger >= 0.95f) out->ulButtonPressed |= bit(vr::k_EButton_Axis1);
    if ((c.buttons & KLVR_BTN_GRIP) || c.grip >= 0.95f) out->ulButtonPressed |= bit(vr::k_EButton_Grip) | bit(vr::k_EButton_Axis2);
    out->ulButtonTouched = out->ulButtonPressed;
    if (c.touches & KLVR_TOUCH_FACE_LOWER) out->ulButtonTouched |= bit(vr::k_EButton_A);
    if (c.touches & KLVR_TOUCH_FACE_UPPER) out->ulButtonTouched |= bit(vr::k_EButton_ApplicationMenu);
    if (c.touches & KLVR_TOUCH_STICK) out->ulButtonTouched |= bit(vr::k_EButton_Axis0);
    if ((c.touches & KLVR_TOUCH_TRIGGER) || c.trigger > 0) out->ulButtonTouched |= bit(vr::k_EButton_Axis1);
    if ((c.touches & KLVR_TOUCH_GRIP) || c.grip > 0) out->ulButtonTouched |= bit(vr::k_EButton_Axis2);
    out->rAxis[0] = { c.stick[0], c.stick[1] };
    out->rAxis[1] = { c.trigger, 0 };
    out->rAxis[2] = { c.grip, 0 };
}

void legacy_haptic_pulse(int hand, uint32_t, uint16_t us) {
    // SteamVR's legacy pulse is a single tick of `us` microseconds, 3999 max,
    // and its length is also its strength.
    float amp = std::min(1.0f, us / 3999.0f);
    klvr_plat_haptic(hand, amp, 0, us / 1e6f);
}

} // namespace klvr

using namespace klvr;
namespace klvr { namespace impl {

vr::EVRInputError IVRInput__SetActionManifestPath(void *, const char *raw_path) {
    if (!raw_path) return vr::VRInputError_InvalidParam;
    std::string p = slashes(raw_path);
    Json m;
    std::string err;
    if (!Json::parse_file(p, m, err)) {
        logf("SetActionManifestPath(%s): %s", raw_path, err.c_str());
        return vr::VRInputError_MismatchedActionManifest;
    }
    std::lock_guard<std::mutex> l(g_mu);
    g_uses_input = true;
    size_t slash = p.find_last_of('/');
    g_manifest_dir = slash == std::string::npos ? "" : p.substr(0, slash);
    for (auto &s : m["action_sets"].a) set_index(s["name"].str());
    for (auto &a : m["actions"].a) {
        Action &act = action_named(a["name"].str());
        act.type = type_of(a["type"].str());
        std::string sk = lower(a["skeleton"].str());
        act.skeleton_hand = sk.find("left") != std::string::npos ? 0 : sk.find("right") != std::string::npos ? 1 : -1;
    }

    // Our controller type first, then anything the game shipped whose
    // component names we know.
    std::vector<std::string> tries;
    if (const char *f = getenv("KLVR_BINDING_FILE")) tries.push_back(f);
    char type[64] = "oculus_touch";
    if (const char *t = getenv("KLVR_CONTROLLER_TYPE")) snprintf(type, sizeof type, "%s", t);
    // frame_controller (Steam Frame) is the layout Alyx's ARM build was made for
    // and the closest to Sense, so it is the first fallback.
    const char *fallbacks[] = { type, "frame_controller", "oculus_touch", "knuckles", "holographic_controller", "vive_controller" };
    for (const char *want : fallbacks)
        for (auto &d : m["default_bindings"].a)
            if (d["controller_type"].str() == want) tries.push_back(join_dir(g_manifest_dir, d["binding_url"].str()));
    bool loaded = false;
    for (auto &f : tries) {
        Json b;
        if (!Json::parse_file(f, b, err)) { logf("binding %s: %s", f.c_str(), err.c_str()); continue; }
        load_bindings(b);
        logf("SetActionManifestPath: %zu actions, %zu sets, bindings from %s", m["actions"].size(), g_sets.size(), f.c_str());
        loaded = true;
        break;
    }
    if (!loaded) logf("SetActionManifestPath: no usable binding file; only skeleton actions will work");
    default_skeleton_bindings();
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetActionSetHandle(void *, const char *name, vr::VRActionSetHandle_t *out) {
    if (!name || !out) return vr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> l(g_mu);
    *out = kSetTag | (uint64_t)(set_index(name) + 1);
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetActionHandle(void *, const char *name, vr::VRActionHandle_t *out) {
    if (!name || !out) return vr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> l(g_mu);
    Action &a = action_named(name);
    *out = (vr::VRActionHandle_t)(&a - &g_actions[0] + 1);
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetInputSourceHandle(void *, const char *path, vr::VRInputValueHandle_t *out) {
    if (!path || !out) return vr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> l(g_mu);
    std::string p = lower(path);
    for (size_t i = 1; i < g_sources.size(); i++)
        if (g_sources[i] == p) { *out = i; return vr::VRInputError_None; }
    g_sources.push_back(p);
    *out = g_sources.size() - 1;
    logf("GetInputSourceHandle(%s) -> %llu (new)", path, (unsigned long long)*out);
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__UpdateActionState(void *, vr::VRActiveActionSet_t *sets, uint32_t size, uint32_t count) {
    if (count && (!sets || size < sizeof(vr::VRActionSetHandle_t))) return vr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> l(g_mu);
    // A set can be listed more than once (once per hand, say), and each entry
    // enables it for the hands that entry names, so they accumulate. A
    // restricted entry's secondary set takes the OTHER hand(s), which is how
    // SteamVR lets one hand hold a weapon set while the other keeps its own.
    std::vector<uint8_t> hands(g_sets.size() + 1, 0);
    auto index_of = [](vr::VRActionSetHandle_t h, uint64_t n, uint64_t &idx) {
        idx = (h & ~kSetTag) - 1;
        return (h & kSetTag) && idx < n;
    };
    for (uint32_t i = 0; i < count; i++) {
        const vr::VRActiveActionSet_t *s = (const vr::VRActiveActionSet_t *)((const char *)sets + (size_t)i * size);
        uint64_t idx;
        if (!index_of(s->ulActionSet, g_sets.size(), idx)) return vr::VRInputError_InvalidHandle;
        uint8_t mine = 3;
        if (size >= offsetof(vr::VRActiveActionSet_t, ulSecondaryActionSet)) {
            vr::VRInputValueHandle_t r = s->ulRestrictedToDevice;
            mine = r == kLeft ? 1 : r == kRight ? 2 : 3;
        }
        hands[idx] |= mine;
        uint64_t sec;
        if (mine != 3 && size >= offsetof(vr::VRActiveActionSet_t, unPadding) && s->ulSecondaryActionSet &&
            index_of(s->ulSecondaryActionSet, g_sets.size(), sec))
            hands[sec] |= (uint8_t)(3 & ~mine);
    }
    // Which sets are live for which hands, each time that changes (bounded):
    // a control that does nothing is most often a set the game never enabled.
    static std::vector<uint8_t> last;
    static int said;
    if (hands != last && said < 40) {
        said++;
        last = hands;
        std::string line;
        for (size_t i = 0; i < g_sets.size(); i++) {
            if (!hands[i]) continue;
            line += " " + g_sets[i].name + (hands[i] == 3 ? "" : hands[i] == 1 ? "(left)" : "(right)");
        }
        logf("active action sets:%s", line.empty() ? " none" : line.c_str());
    }
    update_locked(hands.data());
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetDigitalActionData(void *, vr::VRActionHandle_t h, vr::InputDigitalActionData_t *out,
                                                 uint32_t size, vr::VRInputValueHandle_t restrict_to) {
    if (!out || size < sizeof *out) return vr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> l(g_mu);
    Action *a = action_of(h);
    if (!a) return vr::VRInputError_InvalidHandle;
    if (a->type != A_Boolean && a->type != A_Unknown) return vr::VRInputError_WrongType;
    memset(out, 0, sizeof *out);
    // A source we have no device for (Alyx asks for /user/gamepad): inactive,
    // not "any" - see restrict_is_device.
    if (!restrict_is_device(restrict_to)) return vr::VRInputError_None;
    const State &s = a->state[slot_of(restrict_to)];
    out->bActive = s.active;
    out->activeOrigin = s.origin;
    out->bState = s.bstate;
    out->bChanged = s.changed;
    out->fUpdateTime = s.update_time ? (float)(s.update_time - klvr_plat_now()) : 0.0f;
    if (s.bstate && !a->read_logged && trace_actions()) {
        a->read_logged = true;
        logf("game read %s = 1 (restrict %llu)", a->name.c_str(), (unsigned long long)restrict_to);
    }
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetAnalogActionData(void *, vr::VRActionHandle_t h, vr::InputAnalogActionData_t *out,
                                                uint32_t size, vr::VRInputValueHandle_t restrict_to) {
    if (!out || size < sizeof *out) return vr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> l(g_mu);
    Action *a = action_of(h);
    if (!a) return vr::VRInputError_InvalidHandle;
    if (a->type != A_Vector1 && a->type != A_Vector2 && a->type != A_Vector3 && a->type != A_Unknown)
        return vr::VRInputError_WrongType;
    memset(out, 0, sizeof *out);
    if (!restrict_is_device(restrict_to)) return vr::VRInputError_None;
    const State &s = a->state[slot_of(restrict_to)];
    out->bActive = s.active;
    out->activeOrigin = s.origin;
    out->x = s.v[0]; out->y = s.v[1]; out->z = s.v[2];
    out->deltaX = s.dv[0]; out->deltaY = s.dv[1]; out->deltaZ = s.dv[2];
    out->fUpdateTime = s.update_time ? (float)(s.update_time - klvr_plat_now()) : 0.0f;
    // KLVR_ANALOG_FRESH=1: report every analog action as updated this frame
    // (A/B: a held stick otherwise reports an ever older update time).
    static int fresh = -1;
    if (fresh < 0) fresh = env_on("KLVR_ANALOG_FRESH", false);
    if (fresh) out->fUpdateTime = 0.0f;
    if (trace_actions()) {
        // Every analog action the game polls, once, and then each time it
        // reads a deflected value for the first time after a rest.
        bool on = std::fabs(s.v[0]) + std::fabs(s.v[1]) >= 0.5f;
        if (!a->analog_polled) { a->analog_polled = true; logf("game polls analog %s (restrict %llu)", a->name.c_str(), (unsigned long long)restrict_to); }
        {   // Each (action, restriction) the first time it reads deflected, and its value.
            static std::vector<std::pair<std::string, uint64_t>> seen;
            auto key = std::make_pair(a->name, (uint64_t)restrict_to);
            if (on && std::find(seen.begin(), seen.end(), key) == seen.end()) {
                seen.push_back(key);
                logf("analog %s restrict %llu -> (%.2f %.2f) active %d origin %llu", a->name.c_str(),
                     (unsigned long long)restrict_to, s.v[0], s.v[1], s.active, (unsigned long long)s.origin);
            }
            static std::vector<std::pair<std::string, uint64_t>> polled;
            if (std::find(polled.begin(), polled.end(), key) == polled.end()) {
                polled.push_back(key);
                logf("analog poll %s restrict %llu", a->name.c_str(), (unsigned long long)restrict_to);
            }
        }
        if (on && !a->analog_on) {
            a->analog_on = true;
            logf("game read analog %s = (%.2f %.2f) active %d", a->name.c_str(), s.v[0], s.v[1], s.active);
            // KLVR_TRACE_CALLERS=1: who in the game read it (first 6 times per action).
            static int callers = -1;
            if (callers < 0) callers = env_on("KLVR_TRACE_CALLERS", false);
            if (callers && kl_fault_print_frames && a->callers_shown < 6) {
                a->callers_shown++;
                kl_fault_print_frames(stderr, nullptr);
                void **fp = (void **)__builtin_frame_address(0);
                for (int i = 0; fp && i < 4; i++) { logf("  raw frame %d: %p (out=%p)", i, fp[1], (void *)out); fp = (void **)fp[0]; }
            }
        }
        if (!on) a->analog_on = false;
    }
    return vr::VRInputError_None;
}

static vr::EVRInputError pose_at(vr::VRActionHandle_t h, vr::ETrackingUniverseOrigin u, double t,
                                 vr::InputPoseActionData_t *out, uint32_t size, vr::VRInputValueHandle_t restrict_to) {
    if (!out || size < sizeof *out) return vr::VRInputError_InvalidParam;
    int hand;
    std::string pose;
    {
        std::lock_guard<std::mutex> l(g_mu);
        Action *a = action_of(h);
        if (!a) return vr::VRInputError_InvalidHandle;
        if (a->type != A_Pose && a->type != A_Unknown) return vr::VRInputError_WrongType;
        const Binding *b = nullptr;
        hand = bound_hand(*a, restrict_to, &b);
        pose = b ? b->pose : "raw";
    }
    memset(out, 0, sizeof *out);
    if (hand < 0) { out->pose.mDeviceToAbsoluteTracking = identity34(); return vr::VRInputError_None; }
    klvr_controller c{};
    klvr_plat_controller(hand, &c);
    klvr_tracked g{};
    klvr_plat_hand(hand, t, &g);
    Pose off;
    if (pose == "grip" || pose == "handgrip") off = { { 0, 0, 0 }, { 0, 0, 0, 1 } };
    else off = pmul(raw_from_grip(hand), component_offset(pose));
    fill_pose(&out->pose, g, u, &off);
    out->pose.bDeviceIsConnected = c.connected;
    out->bActive = c.connected;
    out->activeOrigin = hand ? kRight : kLeft;
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetPoseActionDataRelativeToNow(void *, vr::VRActionHandle_t h, vr::ETrackingUniverseOrigin u,
                                                           float seconds, vr::InputPoseActionData_t *out, uint32_t size,
                                                           vr::VRInputValueHandle_t restrict_to) {
    return pose_at(h, u, klvr_plat_now() + seconds, out, size, restrict_to);
}

vr::EVRInputError IVRInput__GetPoseActionDataForNextFrame(void *, vr::VRActionHandle_t h, vr::ETrackingUniverseOrigin u,
                                                          vr::InputPoseActionData_t *out, uint32_t size,
                                                          vr::VRInputValueHandle_t restrict_to) {
    Frame f = current_frame();
    return pose_at(h, u, f.id ? f.display_time : klvr_plat_now(), out, size, restrict_to);
}

vr::EVRInputError IVRInput__GetPoseActionData(void *self, vr::VRActionHandle_t h, vr::ETrackingUniverseOrigin u,
                                              float seconds, vr::InputPoseActionData_t *out, uint32_t size,
                                              vr::VRInputValueHandle_t restrict_to) {
    return IVRInput__GetPoseActionDataRelativeToNow(self, h, u, seconds, out, size, restrict_to);
}

// ---- skeleton -------------------------------------------------------------
static vr::EVRInputError skeleton_hand(vr::VRActionHandle_t h, int &hand) {
    std::lock_guard<std::mutex> l(g_mu);
    Action *a = action_of(h);
    if (!a) return vr::VRInputError_InvalidHandle;
    if (a->type != A_Skeleton && a->type != A_Unknown) return vr::VRInputError_WrongType;
    hand = bound_hand(*a, 0);
    return hand < 0 ? vr::VRInputError_NoData : vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetSkeletalActionData(void *, vr::VRActionHandle_t h, vr::InputSkeletalActionData_t *out,
                                                  uint32_t size) {
    if (!out || size < sizeof *out) return vr::VRInputError_InvalidParam;
    int hand;
    vr::EVRInputError e = skeleton_hand(h, hand);
    memset(out, 0, sizeof *out);
    if (e == vr::VRInputError_NoData) return vr::VRInputError_None;
    if (e) return e;
    klvr_controller c{};
    klvr_plat_controller(hand, &c);
    out->bActive = c.connected;
    out->activeOrigin = hand ? kRight : kLeft;
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetDominantHand(void *, vr::ETrackedControllerRole *out) {
    if (!out) return vr::VRInputError_InvalidParam;
    std::lock_guard<std::mutex> l(g_mu);
    *out = g_dominant;
    return vr::VRInputError_None;
}
vr::EVRInputError IVRInput__SetDominantHand(void *, vr::ETrackedControllerRole r) {
    std::lock_guard<std::mutex> l(g_mu);
    g_dominant = r;
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetBoneCount(void *, vr::VRActionHandle_t h, uint32_t *n) {
    int hand;
    vr::EVRInputError e = skeleton_hand(h, hand);
    if (e && e != vr::VRInputError_NoData) return e;
    if (n) *n = kBoneCount;
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetBoneHierarchy(void *, vr::VRActionHandle_t, vr::BoneIndex_t *out, uint32_t count) {
    if (!out || count < kBoneCount) return vr::VRInputError_BufferTooSmall;
    for (int i = 0; i < kBoneCount; i++) out[i] = (vr::BoneIndex_t)skel_parent(i);
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetBoneName(void *, vr::VRActionHandle_t, vr::BoneIndex_t i, char *buf, uint32_t size) {
    const char *n = skel_bone_name(i);
    if (!n) return vr::VRInputError_InvalidBoneIndex;
    return put_string(n, buf, size) > size ? vr::VRInputError_BufferTooSmall : vr::VRInputError_None;
}

static void to_space(vr::EVRSkeletalTransformSpace space, vr::VRBoneTransform_t *xf) {
    if (space != vr::VRSkeletalTransformSpace_Model) return;
    vr::VRBoneTransform_t m[kBoneCount];
    skel_to_model(xf, m);
    memcpy(xf, m, sizeof m);
}

vr::EVRInputError IVRInput__GetSkeletalReferenceTransforms(void *, vr::VRActionHandle_t h, vr::EVRSkeletalTransformSpace space,
                                                           vr::EVRSkeletalReferencePose which, vr::VRBoneTransform_t *out,
                                                           uint32_t count) {
    if (!out || count < kBoneCount) return vr::VRInputError_BufferTooSmall;
    int hand;
    vr::EVRInputError e = skeleton_hand(h, hand);
    if (e == vr::VRInputError_NoData) hand = 0;
    else if (e) return e;
    vr::VRBoneTransform_t xf[kBoneCount];
    skel_reference(hand, which, xf);
    to_space(space, xf);
    memcpy(out, xf, sizeof xf);
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetSkeletalTrackingLevel(void *, vr::VRActionHandle_t, vr::EVRSkeletalTrackingLevel *out) {
    if (out) *out = vr::VRSkeletalTracking_Partial;
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetSkeletalBoneData(void *, vr::VRActionHandle_t h, vr::EVRSkeletalTransformSpace space,
                                                vr::EVRSkeletalMotionRange range, vr::VRBoneTransform_t *out, uint32_t count) {
    if (!out || count < kBoneCount) return vr::VRInputError_BufferTooSmall;
    int hand;
    vr::EVRInputError e = skeleton_hand(h, hand);
    if (e) return e;
    klvr_controller c{};
    klvr_plat_controller(hand, &c);
    if (!c.connected) return vr::VRInputError_NoData;
    float curl[5];
    curls_from(c, curl);
    vr::VRBoneTransform_t xf[kBoneCount];
    skel_pose(hand, curl, range, xf);
    to_space(space, xf);
    memcpy(out, xf, sizeof xf);
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetSkeletalSummaryData(void *, vr::VRActionHandle_t h, vr::EVRSummaryType,
                                                   vr::VRSkeletalSummaryData_t *out) {
    if (!out) return vr::VRInputError_InvalidParam;
    int hand;
    vr::EVRInputError e = skeleton_hand(h, hand);
    if (e) return e;
    klvr_controller c{};
    klvr_plat_controller(hand, &c);
    memset(out, 0, sizeof *out);
    curls_from(c, out->flFingerCurl);
    for (int i = 0; i < 4; i++) out->flFingerSplay[i] = 0.2f;
    return vr::VRInputError_None;
}

// "Compressed" bone data is opaque to the game: it only ever hands it back to
// DecompressSkeletalBoneData (usually after sending it over the network). So
// the format is ours: a tag, then the 31 parent-space transforms.
namespace { struct Packed { uint32_t tag; uint32_t count; vr::VRBoneTransform_t xf[kBoneCount]; }; }

vr::EVRInputError IVRInput__GetSkeletalBoneDataCompressed(void *self, vr::VRActionHandle_t h, vr::EVRSkeletalMotionRange range,
                                                          void *buf, uint32_t size, uint32_t *required) {
    if (required) *required = sizeof(Packed);
    if (!buf || size < sizeof(Packed)) return vr::VRInputError_BufferTooSmall;
    Packed p;
    p.tag = 0x4b534c4b;   // "KLSK"
    p.count = kBoneCount;
    vr::EVRInputError e = IVRInput__GetSkeletalBoneData(self, h, vr::VRSkeletalTransformSpace_Parent, range, p.xf, kBoneCount);
    if (e) return e;
    memcpy(buf, &p, sizeof p);
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__DecompressSkeletalBoneData(void *, const void *buf, uint32_t size, vr::EVRSkeletalTransformSpace space,
                                                       vr::VRBoneTransform_t *out, uint32_t count) {
    if (!buf || size < sizeof(Packed)) return vr::VRInputError_InvalidParam;
    if (!out || count < kBoneCount) return vr::VRInputError_BufferTooSmall;
    Packed p;
    memcpy(&p, buf, sizeof p);
    if (p.tag != 0x4b534c4b || p.count != kBoneCount) return vr::VRInputError_InvalidCompressedData;
    to_space(space, p.xf);
    memcpy(out, p.xf, sizeof p.xf);
    return vr::VRInputError_None;
}

// ---- haptics --------------------------------------------------------------
vr::EVRInputError IVRInput__TriggerHapticVibrationAction(void *, vr::VRActionHandle_t h, float start, float duration,
                                                         float frequency, float amplitude, vr::VRInputValueHandle_t restrict_to) {
    std::vector<int> hands;
    {
        std::lock_guard<std::mutex> l(g_mu);
        Action *a = action_of(h);
        if (!a) return vr::VRInputError_InvalidHandle;
        if (a->type != A_Vibration && a->type != A_Unknown) return vr::VRInputError_WrongType;
        for (auto &b : a->bindings) {
            if (restrict_to == kLeft && b.hand != 0) continue;
            if (restrict_to == kRight && b.hand != 1) continue;
            if (std::find(hands.begin(), hands.end(), b.hand) == hands.end()) hands.push_back(b.hand);
        }
    }
    if (start > 0.001f) {
        static bool said = false;
        if (!said) { said = true; logf("haptics: delayed starts (%.3fs) are played immediately", start); }
    }
    for (int hand : hands) klvr_plat_haptic(hand, amplitude, frequency, duration);
    return vr::VRInputError_None;
}

// ---- origins and UI -------------------------------------------------------
vr::EVRInputError IVRInput__GetActionOrigins(void *, vr::VRActionSetHandle_t, vr::VRActionHandle_t h,
                                             vr::VRInputValueHandle_t *out, uint32_t count) {
    std::lock_guard<std::mutex> l(g_mu);
    Action *a = action_of(h);
    if (!a) return vr::VRInputError_InvalidHandle;
    if (out) memset(out, 0, sizeof *out * count);
    uint32_t n = 0;
    for (int hand = 0; hand < 2; hand++)
        for (auto &b : a->bindings)
            if (b.hand == hand) { if (out && n < count) out[n] = hand ? kRight : kLeft; n++; break; }
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetOriginLocalizedName(void *, vr::VRInputValueHandle_t origin, char *buf, uint32_t size, int32_t) {
    const char *n = origin == kLeft ? "Left Hand" : origin == kRight ? "Right Hand" : origin == kHead ? "Headset" : "";
    return put_string(n, buf, size) > size ? vr::VRInputError_BufferTooSmall : vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetOriginTrackedDeviceInfo(void *, vr::VRInputValueHandle_t origin, vr::InputOriginInfo_t *out,
                                                       uint32_t size) {
    if (!out || size < sizeof *out) return vr::VRInputError_InvalidParam;
    memset(out, 0, sizeof *out);
    out->devicePath = origin;
    out->trackedDeviceIndex = origin == kLeft ? kLeftHand : origin == kRight ? kRightHand : origin == kHead ? kHmd
                                                                                                        : vr::k_unTrackedDeviceIndexInvalid;
    return vr::VRInputError_None;
}

vr::EVRInputError IVRInput__GetActionBindingInfo(void *, vr::VRActionHandle_t, vr::InputBindingInfo_t *, uint32_t, uint32_t,
                                                 uint32_t *returned) {
    if (returned) *returned = 0;
    return vr::VRInputError_None;
}
vr::EVRInputError IVRInput__ShowActionOrigins(void *, vr::VRActionSetHandle_t, vr::VRActionHandle_t) { return vr::VRInputError_None; }
vr::EVRInputError IVRInput__ShowBindingsForActionSet(void *, vr::VRActiveActionSet_t *, uint32_t, uint32_t, vr::VRInputValueHandle_t) {
    return vr::VRInputError_None;
}
vr::EVRInputError IVRInput__OpenBindingUI(void *, const char *, vr::VRActionSetHandle_t, vr::VRInputValueHandle_t, bool) {
    return vr::VRInputError_None;
}
vr::EVRInputError IVRInput__GetBindingVariant(void *, vr::VRInputValueHandle_t, char *buf, uint32_t size) {
    put_string("", buf, size);
    return vr::VRInputError_None;
}
vr::EVRInputError IVRInput__GetComponentStateForBinding(void *, const char *, const char *, const vr::InputBindingInfo_t *,
                                                        uint32_t, uint32_t, vr::RenderModel_ComponentState_t *) {
    return vr::VRInputError_NoData;
}
bool IVRInput__IsUsingLegacyInput(void *) { return false; }

}} // namespace klvr::impl
