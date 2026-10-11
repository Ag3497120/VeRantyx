// IVRClientCore: what openvr_api asks vrclient.so for first, and the door to
// every other interface.
#include "gen_stubs.h"
#include "vr_runtime.h"
#include <cstring>
#include <set>
#include <string>
#include <mutex>

vr::IVRSystem *vrrt_system();
vr::IVRCompositor *vrrt_compositor();
vr::IVRInput *vrrt_input();
void *vrrt_misc_interface(const char *name);

void vrrt_log_unimplemented(const char *iface, const char *method) {
    static std::set<std::string> said;
    static std::mutex mu;
    std::lock_guard<std::mutex> l(mu);
    std::string key = std::string(iface) + "::" + method;
    if (said.insert(key).second) vrrt::log("unimplemented %s called (default answer)", key.c_str());
}

namespace {

class ClientCore : public Stub_IVRClientCore {
public:
    vr::EVRInitError Init(vr::EVRApplicationType eApplicationType, const char *pStartupInfo) override {
        vrrt::log("IVRClientCore::Init(app type %d%s%s)", (int)eApplicationType,
                  pStartupInfo && *pStartupInfo ? ", startup info " : "", pStartupInfo ? pStartupInfo : "");
        // The devices exist from the start; tell the application so.
        for (uint32_t i = 0; i < 3; i++) {
            vr::VREvent_t e = {};
            e.eventType = vr::VREvent_TrackedDeviceActivated;
            e.trackedDeviceIndex = i;
            e.eventAgeSeconds = 0;
            vrrt::push_event(e);
        }
        return vr::VRInitError_None;
    }
    void Cleanup() override { vrrt::log("IVRClientCore::Cleanup"); }
    vr::EVRInitError IsInterfaceVersionValid(const char *v) override {
        vr::EVRInitError err = vr::VRInitError_None;
        return GetGenericInterface(v, &err) ? vr::VRInitError_None : vr::VRInitError_Init_InterfaceNotFound;
    }
    void *GetGenericInterface(const char *name, vr::EVRInitError *peError) override {
        void *p = nullptr;
        if (!strcmp(name, vr::IVRSystem_Version)) p = vrrt_system();
        else if (!strcmp(name, vr::IVRCompositor_Version)) p = vrrt_compositor();
        else if (!strcmp(name, vr::IVRInput_Version)) p = vrrt_input();
        else p = vrrt_misc_interface(name);
        static std::set<std::string> said;
        if (said.insert(name).second) vrrt::log("GetGenericInterface(%s) -> %s", name, p ? "served" : "NOT AVAILABLE");
        if (peError) *peError = p ? vr::VRInitError_None : vr::VRInitError_Init_InterfaceNotFound;
        return p;
    }
    bool BIsHmdPresent() override { return true; }
    const char *GetEnglishStringForHmdError(vr::EVRInitError e) override {
        static char buf[64]; snprintf(buf, sizeof buf, "VeRantyx runtime error %d", (int)e); return buf;
    }
    const char *GetIDForVRInitError(vr::EVRInitError e) override {
        static char buf[64]; snprintf(buf, sizeof buf, "VRInitError_%d", (int)e); return buf;
    }
};

ClientCore g_core;

} // namespace

extern "C" void *VRClientCoreFactory(const char *pInterfaceName, int *pReturnCode) {
    if (pInterfaceName && !strcmp(pInterfaceName, vr::IVRClientCore_Version)) {
        if (pReturnCode) *pReturnCode = vr::VRInitError_None;
        vrrt::log("VRClientCoreFactory(%s)", pInterfaceName);
        return &g_core;
    }
    vrrt::log("VRClientCoreFactory(%s): not available", pInterfaceName ? pInterfaceName : "?");
    if (pReturnCode) *pReturnCode = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}

extern "C" void *vrrt_dlsym(const char *name) {
    if (!strcmp(name, "VRClientCoreFactory")) return (void *)VRClientCoreFactory;
    vrrt::log("vrclient.so dlsym(%s): no such symbol", name);
    return nullptr;
}
