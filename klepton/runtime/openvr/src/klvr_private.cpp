// SteamVR's private interfaces: the ones Alyx's libvr.so asks for that no
// public OpenVR SDK declares.
//
// What is known about them comes from how Alyx uses them, and from the
// interface shapes other OpenVR runtimes (Proton's vrclient, OpenComposite)
// have recorded for Half-Life: Alyx. Every slot not named here stays on the
// logging probe (klvr_core.cpp), so a run still says which ones matter.
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include "klvr_internal.h"

using namespace klvr;

namespace {

// A vtable that starts as the probe table, with `known` slots overridden.
struct PrivateTable {
    const void *slots[kProbeSlotCount];
    PrivateTable(std::initializer_list<std::pair<int, const void *>> known) {
        for (int i = 0; i < kProbeSlotCount; i++) slots[i] = kProbeSlots[i];
        for (auto &k : known) slots[k.first] = k.second;
    }
};

// ---- IVRMailbox_001 ---------------------------------------------------------
// A message pipe between the game and SteamVR's web-based UI. Four slots:
//   0 RegisterMailbox(const char *name, uint64_t *handle)
//   1 UnregisterMailbox(uint64_t handle)
//   2 SendMessage(uint64_t handle, const char *type, const char *json)
//   3 ReadMessage(uint64_t handle, char *buf, uint32_t size, uint32_t *length)
// returning 0 (ok), 1 (no message) or 2 (buffer too short). The one thing the
// game needs back is a {"type":"ready"} on its mailbox, which SteamVR's side
// sends once it is listening; everything the game sends is logged, since
// there is nobody on the other end.
enum : uint32_t { kMboxOk = 0, kMboxNoMessage = 1, kMboxTooShort = 2 };

std::mutex g_mbox_mu;
uint64_t g_mbox_next = 1;
std::map<uint64_t, std::deque<std::string>> g_mbox;

uint32_t mbox_register(PrivateObject *, const char *name, uint64_t *handle) {
    std::lock_guard<std::mutex> l(g_mbox_mu);
    uint64_t h = g_mbox_next++;
    g_mbox[h].push_back("{ \"type\": \"ready\" }");
    if (handle) *handle = h;
    logf("IVRMailbox: registered \"%s\" as %llu", name ? name : "", (unsigned long long)h);
    return kMboxOk;
}
uint32_t mbox_unregister(PrivateObject *, uint64_t handle) {
    std::lock_guard<std::mutex> l(g_mbox_mu);
    g_mbox.erase(handle);
    return kMboxOk;
}
uint32_t mbox_send(PrivateObject *, uint64_t handle, const char *type, const char *json) {
    static std::atomic<int> said{0};
    if (said++ < 50)
        logf("IVRMailbox: %llu sends %s: %.300s", (unsigned long long)handle, type ? type : "(null)", json ? json : "");
    return kMboxOk;
}
uint32_t mbox_read(PrivateObject *, uint64_t handle, char *buf, uint32_t size, uint32_t *length) {
    std::lock_guard<std::mutex> l(g_mbox_mu);
    static int reads = 0;
    auto it = g_mbox.find(handle);
    if (getenv("KLVR_TRACE_MAILBOX") && reads++ < 20)
        logf("IVRMailbox: read(%llu, size %u) -> %s", (unsigned long long)handle, size,
             (it == g_mbox.end() || it->second.empty()) ? "none" : it->second.front().c_str());
    if (it == g_mbox.end() || it->second.empty()) return kMboxNoMessage;
    const std::string &m = it->second.front();
    if (!buf || size < m.size() + 1) {
        if (length) *length = (uint32_t)m.size() + 1;
        return kMboxTooShort;
    }
    memcpy(buf, m.c_str(), m.size() + 1);
    if (length) *length = (uint32_t)m.size();
    it->second.pop_front();
    return kMboxOk;
}

// ---- IVRControlPanel (VRControlPanel()) ---------------------------------------
// libvr.so imports the VRControlPanel accessor itself and uses two slots:
//   15 SetDashboardUserToggleEnabled(bool)
//   26 RegisterExternalWebRoot(...) -> 0 on success
// Both are about SteamVR's dashboard, which does not exist here.
void cp_set_dashboard_toggle(PrivateObject *, uint32_t enabled) {   // bool, widened: see the ABI note in README.md
    logf("IVRControlPanel: dashboard toggle %s", (enabled & 0xff) ? "enabled" : "disabled");
}
uint64_t cp_register_web_root(PrivateObject *) { return 0; }

// Built on first use rather than at static-init time: the probe table lives in
// another translation unit, and static initialisation order across them is
// unspecified.
PrivateObject *mailbox() {
    static const PrivateTable t = { { 0, (const void *)&mbox_register }, { 1, (const void *)&mbox_unregister },
                                    { 2, (const void *)&mbox_send }, { 3, (const void *)&mbox_read } };
    static PrivateObject o = { t.slots, "IVRMailbox_001" };
    return &o;
}
PrivateObject *control_panel() {
    static const PrivateTable t = { { 15, (const void *)&cp_set_dashboard_toggle },
                                    { 26, (const void *)&cp_register_web_root } };
    static PrivateObject o = { t.slots, "IVRControlPanel" };
    return &o;
}
PrivateObject *probe_only(int i) {
    static PrivateObject o[2] = { { kProbeSlots, "IVRInputInternal_002" },
                                  { kProbeSlots, "IVRIPCResourceManagerClient_004" } };
    return &o[i];
}

} // namespace

void *klvr::private_interface(const char *version) {
    if (!version) return nullptr;
    // Serving IVRIPCResourceManagerClient (even as a probe) makes libvr pick
    // "zero copy presentation": swapchains allocated through SteamVR's shared
    // textures, never handed to IVRCompositor::Submit. Not serving it gives
    // "No IPCResourceManagerClient available. Using copies!", i.e. Submit.
    if (strcmp(version, "IVRIPCResourceManagerClient_004") == 0 && !env_on("KLVR_IPC_PROBE", false)) return nullptr;
    for (PrivateObject *o : { mailbox(), probe_only(0), probe_only(1) })
        if (strcmp(version, o->name) == 0) return o;
    return nullptr;
}

// Exported beside the VR_* entry points: libvr.so imports it by name.
// KLVR_CONTROL_PANEL=0 answers NULL instead, which Alyx also accepts.
KLVR_EXPORT void *VRControlPanel(void) {
    static std::atomic<bool> said{false};
    bool on = env_on("KLVR_CONTROL_PANEL", true);
    if (!said.exchange(true)) logf("VRControlPanel(): %s", on ? "served" : "NULL (KLVR_CONTROL_PANEL=0)");
    return on ? control_panel() : nullptr;
}
