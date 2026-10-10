// IVROverlay: a registry of overlays, none of them drawn.
//
// Alyx looks overlays up by key and creates its own. Without a registry,
// FindOverlay answered "found, handle 0" for keys nobody created, and every
// later call on handle 0 fell to a stub. Here overlays exist, remember what
// the game sets on them, and answer as SteamVR would; they are just not
// composited. If the game turns out to need one drawn (a loading screen or a
// menu), SetOverlayTexture is where that starts.
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include "klvr_internal.h"

using namespace klvr;

namespace {
struct Overlay {
    std::string key, name;
    bool visible = false;
    uint32_t flags = 0;
    float width = 1, alpha = 1, color[3] = { 1, 1, 1 }, curvature = 0, texel_aspect = 1;
    uint32_t sort_order = 0;
    vr::VROverlayInputMethod input = vr::VROverlayInputMethod_None;
    vr::VRTextureBounds_t bounds = { 0, 0, 1, 1 };
    bool has_texture = false;
};
std::mutex g_mu;
std::map<vr::VROverlayHandle_t, Overlay> g_overlays;
vr::VROverlayHandle_t g_next = 1;

Overlay *find(vr::VROverlayHandle_t h) {
    auto it = g_overlays.find(h);
    return it == g_overlays.end() ? nullptr : &it->second;
}
vr::EVROverlayError create(const char *key, const char *name, vr::VROverlayHandle_t *out) {
    if (!key || !out) return vr::VROverlayError_InvalidParameter;
    for (auto &o : g_overlays)
        if (o.second.key == key) return vr::VROverlayError_KeyInUse;
    vr::VROverlayHandle_t h = g_next++;
    Overlay &o = g_overlays[h];
    o.key = key;
    o.name = name ? name : "";
    *out = h;
    logf("IVROverlay: created \"%s\" (%s)", key, o.name.c_str());
    return vr::VROverlayError_None;
}
} // namespace

// Set one field on an overlay: the lock, the lookup and the error in one place.
#define WITH_OVERLAY(h, body)                                    \
    do {                                                         \
        std::lock_guard<std::mutex> l(g_mu);                     \
        Overlay *o = find(h);                                    \
        if (!o) return vr::VROverlayError_UnknownOverlay;        \
        body;                                                    \
        return vr::VROverlayError_None;                          \
    } while (0)

namespace klvr { namespace impl {

vr::EVROverlayError IVROverlay__FindOverlay(void *, const char *key, vr::VROverlayHandle_t *out) {
    std::lock_guard<std::mutex> l(g_mu);
    if (out) *out = vr::k_ulOverlayHandleInvalid;
    for (auto &o : g_overlays)
        if (key && o.second.key == key) {
            if (out) *out = o.first;
            return vr::VROverlayError_None;
        }
    return vr::VROverlayError_UnknownOverlay;
}
vr::EVROverlayError IVROverlay__CreateOverlay(void *, const char *key, const char *name, vr::VROverlayHandle_t *out) {
    std::lock_guard<std::mutex> l(g_mu);
    return create(key, name, out);
}
vr::EVROverlayError IVROverlay__CreateDashboardOverlay(void *, const char *key, const char *name,
                                                      vr::VROverlayHandle_t *main, vr::VROverlayHandle_t *thumb) {
    std::lock_guard<std::mutex> l(g_mu);
    vr::EVROverlayError e = create(key, name, main);
    if (e != vr::VROverlayError_None) return e;
    std::string tk = std::string(key) + ".thumbnail";
    vr::VROverlayHandle_t t;
    create(tk.c_str(), name, thumb ? thumb : &t);
    return vr::VROverlayError_None;
}
vr::EVROverlayError IVROverlay__DestroyOverlay(void *, vr::VROverlayHandle_t h) {
    std::lock_guard<std::mutex> l(g_mu);
    return g_overlays.erase(h) ? vr::VROverlayError_None : vr::VROverlayError_UnknownOverlay;
}
uint32_t IVROverlay__GetOverlayKey(void *, vr::VROverlayHandle_t h, char *buf, uint32_t size, vr::EVROverlayError *err) {
    std::lock_guard<std::mutex> l(g_mu);
    Overlay *o = find(h);
    if (err) *err = o ? vr::VROverlayError_None : vr::VROverlayError_UnknownOverlay;
    return o ? put_string(o->key.c_str(), buf, size) : 0;
}
uint32_t IVROverlay__GetOverlayName(void *, vr::VROverlayHandle_t h, char *buf, uint32_t size, vr::EVROverlayError *err) {
    std::lock_guard<std::mutex> l(g_mu);
    Overlay *o = find(h);
    if (err) *err = o ? vr::VROverlayError_None : vr::VROverlayError_UnknownOverlay;
    return o ? put_string(o->name.c_str(), buf, size) : 0;
}
vr::EVROverlayError IVROverlay__SetOverlayName(void *, vr::VROverlayHandle_t h, const char *name) {
    WITH_OVERLAY(h, o->name = name ? name : "");
}
vr::EVROverlayError IVROverlay__ShowOverlay(void *, vr::VROverlayHandle_t h) { WITH_OVERLAY(h, o->visible = true); }
vr::EVROverlayError IVROverlay__HideOverlay(void *, vr::VROverlayHandle_t h) { WITH_OVERLAY(h, o->visible = false); }
bool IVROverlay__IsOverlayVisible(void *, vr::VROverlayHandle_t h) {
    std::lock_guard<std::mutex> l(g_mu);
    Overlay *o = find(h);
    return o && o->visible;
}
vr::EVROverlayError IVROverlay__SetOverlayFlag(void *, vr::VROverlayHandle_t h, vr::VROverlayFlags f, bool on) {
    WITH_OVERLAY(h, o->flags = on ? (o->flags | (1u << f)) : (o->flags & ~(1u << f)));
}
vr::EVROverlayError IVROverlay__GetOverlayFlag(void *, vr::VROverlayHandle_t h, vr::VROverlayFlags f, bool *on) {
    WITH_OVERLAY(h, if (on) *on = (o->flags >> f) & 1);
}
vr::EVROverlayError IVROverlay__GetOverlayFlags(void *, vr::VROverlayHandle_t h, uint32_t *flags) {
    WITH_OVERLAY(h, if (flags) *flags = o->flags);
}
vr::EVROverlayError IVROverlay__SetOverlayColor(void *, vr::VROverlayHandle_t h, float r, float g, float b) {
    WITH_OVERLAY(h, o->color[0] = r; o->color[1] = g; o->color[2] = b);
}
vr::EVROverlayError IVROverlay__GetOverlayColor(void *, vr::VROverlayHandle_t h, float *r, float *g, float *b) {
    WITH_OVERLAY(h, if (r) *r = o->color[0]; if (g) *g = o->color[1]; if (b) *b = o->color[2]);
}
vr::EVROverlayError IVROverlay__SetOverlayAlpha(void *, vr::VROverlayHandle_t h, float a) { WITH_OVERLAY(h, o->alpha = a); }
vr::EVROverlayError IVROverlay__GetOverlayAlpha(void *, vr::VROverlayHandle_t h, float *a) {
    WITH_OVERLAY(h, if (a) *a = o->alpha);
}
vr::EVROverlayError IVROverlay__SetOverlayWidthInMeters(void *, vr::VROverlayHandle_t h, float w) { WITH_OVERLAY(h, o->width = w); }
vr::EVROverlayError IVROverlay__GetOverlayWidthInMeters(void *, vr::VROverlayHandle_t h, float *w) {
    WITH_OVERLAY(h, if (w) *w = o->width);
}
vr::EVROverlayError IVROverlay__SetOverlayCurvature(void *, vr::VROverlayHandle_t h, float c) { WITH_OVERLAY(h, o->curvature = c); }
vr::EVROverlayError IVROverlay__GetOverlayCurvature(void *, vr::VROverlayHandle_t h, float *c) {
    WITH_OVERLAY(h, if (c) *c = o->curvature);
}
vr::EVROverlayError IVROverlay__SetOverlayTexelAspect(void *, vr::VROverlayHandle_t h, float a) { WITH_OVERLAY(h, o->texel_aspect = a); }
vr::EVROverlayError IVROverlay__GetOverlayTexelAspect(void *, vr::VROverlayHandle_t h, float *a) {
    WITH_OVERLAY(h, if (a) *a = o->texel_aspect);
}
vr::EVROverlayError IVROverlay__SetOverlaySortOrder(void *, vr::VROverlayHandle_t h, uint32_t s) { WITH_OVERLAY(h, o->sort_order = s); }
vr::EVROverlayError IVROverlay__GetOverlaySortOrder(void *, vr::VROverlayHandle_t h, uint32_t *s) {
    WITH_OVERLAY(h, if (s) *s = o->sort_order);
}
vr::EVROverlayError IVROverlay__SetOverlayInputMethod(void *, vr::VROverlayHandle_t h, vr::VROverlayInputMethod m) {
    WITH_OVERLAY(h, o->input = m);
}
vr::EVROverlayError IVROverlay__GetOverlayInputMethod(void *, vr::VROverlayHandle_t h, vr::VROverlayInputMethod *m) {
    WITH_OVERLAY(h, if (m) *m = o->input);
}
vr::EVROverlayError IVROverlay__SetOverlayTextureBounds(void *, vr::VROverlayHandle_t h, const vr::VRTextureBounds_t *b) {
    WITH_OVERLAY(h, if (b) o->bounds = *b);
}
vr::EVROverlayError IVROverlay__GetOverlayTextureBounds(void *, vr::VROverlayHandle_t h, vr::VRTextureBounds_t *b) {
    WITH_OVERLAY(h, if (b) *b = o->bounds);
}
vr::EVROverlayError IVROverlay__SetOverlayTexture(void *, vr::VROverlayHandle_t h, const vr::Texture_t *t) {
    WITH_OVERLAY(h, o->has_texture = t && t->handle);
}
vr::EVROverlayError IVROverlay__ClearOverlayTexture(void *, vr::VROverlayHandle_t h) { WITH_OVERLAY(h, o->has_texture = false); }
vr::EVROverlayError IVROverlay__SetOverlayFromFile(void *, vr::VROverlayHandle_t h, const char *) { WITH_OVERLAY(h, o->has_texture = true); }
vr::EVROverlayError IVROverlay__SetOverlayRaw(void *, vr::VROverlayHandle_t h, void *, uint32_t, uint32_t, uint32_t) {
    WITH_OVERLAY(h, o->has_texture = true);
}
vr::EVROverlayError IVROverlay__SetOverlayTextureColorSpace(void *, vr::VROverlayHandle_t h, vr::EColorSpace) { WITH_OVERLAY(h, (void)o); }
vr::EVROverlayError IVROverlay__SetOverlayMouseScale(void *, vr::VROverlayHandle_t h, const vr::HmdVector2_t *) { WITH_OVERLAY(h, (void)o); }
vr::EVROverlayError IVROverlay__SetOverlayTransformAbsolute(void *, vr::VROverlayHandle_t h, vr::ETrackingUniverseOrigin,
                                                           const vr::HmdMatrix34_t *) {
    WITH_OVERLAY(h, (void)o);
}
vr::EVROverlayError IVROverlay__SetOverlayTransformTrackedDeviceRelative(void *, vr::VROverlayHandle_t h, vr::TrackedDeviceIndex_t,
                                                                        const vr::HmdMatrix34_t *) {
    WITH_OVERLAY(h, (void)o);
}
vr::EVROverlayError IVROverlay__SetOverlayTransformOverlayRelative(void *, vr::VROverlayHandle_t h, vr::VROverlayHandle_t,
                                                                  const vr::HmdMatrix34_t *) {
    WITH_OVERLAY(h, (void)o);
}
vr::EVROverlayError IVROverlay__SetHighQualityOverlay(void *, vr::VROverlayHandle_t h) { WITH_OVERLAY(h, (void)o); }
vr::VROverlayHandle_t IVROverlay__GetHighQualityOverlay(void *) { return vr::k_ulOverlayHandleInvalid; }
bool IVROverlay__PollNextOverlayEvent(void *, vr::VROverlayHandle_t, vr::VREvent_t *, uint32_t) { return false; }
bool IVROverlay__IsDashboardVisible(void *) { return false; }
bool IVROverlay__IsActiveDashboardOverlay(void *, vr::VROverlayHandle_t) { return false; }
vr::TrackedDeviceIndex_t IVROverlay__GetPrimaryDashboardDevice(void *) { return vr::k_unTrackedDeviceIndexInvalid; }
const char *IVROverlay__GetOverlayErrorNameFromEnum(void *, vr::EVROverlayError e) {
    switch (e) {
    case vr::VROverlayError_None: return "VROverlayError_None";
    case vr::VROverlayError_UnknownOverlay: return "VROverlayError_UnknownOverlay";
    case vr::VROverlayError_InvalidHandle: return "VROverlayError_InvalidHandle";
    case vr::VROverlayError_KeyInUse: return "VROverlayError_KeyInUse";
    case vr::VROverlayError_InvalidParameter: return "VROverlayError_InvalidParameter";
    default: return "VROverlayError_Unknown";
    }
}

}} // namespace klvr::impl
