// Drives the shim the way the game does: through VR_GetGenericInterface and
// the C++ interface classes in Valve's own openvr.h. So a vtable slot out of
// place, or a method that silently fell through to its stub, fails here.
//
// Run from tests/ (the fixtures are relative): make test.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include "openvr.h"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "klvr_platform_null.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b) CHECK(std::fabs((double)(a) - (double)(b)) < 1e-3)

static bool log_has(const char *s) { return strstr(klvr_null.last_log, s) != nullptr; }

template <class T> static T *iface(const char *version) {
    vr::EVRInitError e = vr::VRInitError_Unknown;
    void *p = vr::VR_GetGenericInterface(version, &e);
    CHECK(p != nullptr);
    CHECK(e == vr::VRInitError_None);
    return (T *)p;
}

static void connect_controllers() {
    for (int h = 0; h < 2; h++) {
        klvr_null.ctl[h] = {};
        klvr_null.ctl[h].connected = 1;
        klvr_null.hand[h] = {};
        klvr_null.hand[h].valid = 1;
        klvr_null.hand[h].pose = { { h ? 0.2f : -0.2f, 1.2f, -0.3f }, { 0, 0, 0, 1 } };
    }
}

// ---- loader ---------------------------------------------------------------
static void test_loader() {
    vr::EVRInitError e = vr::VRInitError_Unknown;
    uint32_t tok = vr::VR_InitInternal2(&e, vr::VRApplication_Scene, nullptr);
    CHECK(e == vr::VRInitError_None);
    CHECK(tok == vr::VR_GetInitToken());
    CHECK(vr::VR_IsHmdPresent());
    CHECK(vr::VR_IsInterfaceVersionValid(vr::IVRSystem_Version));
    CHECK(vr::VR_IsInterfaceVersionValid(vr::IVRCompositor_Version));
    CHECK(vr::VR_IsInterfaceVersionValid(vr::IVRInput_Version));
    // Older versions a game built against an older SDK would ask for.
    CHECK(vr::VR_IsInterfaceVersionValid("IVRSystem_019"));
    CHECK(vr::VR_IsInterfaceVersionValid("IVRCompositor_022"));
    CHECK(vr::VR_IsInterfaceVersionValid("IVRInput_007"));
    CHECK(!vr::VR_IsInterfaceVersionValid("IVRSystem_999"));
    CHECK(vr::VR_GetGenericInterface("IVRSystem_999", &e) == nullptr);
    CHECK(e == vr::VRInitError_Init_InterfaceNotFound);
    CHECK(vr::VR_GetGenericInterface("FnTable:IVRSystem_022", &e) == nullptr);
    // SteamVR-private interfaces libvr.so asks for are served as probes whose
    // slots log and return zero.
    CHECK(vr::VR_IsInterfaceVersionValid("IVRMailbox_001"));
    void *mb = vr::VR_GetGenericInterface("IVRMailbox_001", &e);
    CHECK(mb != nullptr && e == vr::VRInitError_None);
    typedef uint64_t (*slot_fn)(void *, uint64_t, uint64_t);
    klvr_null.quiet = 1;
    CHECK(((slot_fn)(*(void ***)mb)[5])(mb, 1, 2) == 0);
    CHECK(log_has("IVRMailbox_001 slot 5"));
    klvr_null.quiet = 0;
}

// ---- IVRSystem ------------------------------------------------------------
static void test_system() {
    auto *sys = iface<vr::IVRSystem>(vr::IVRSystem_Version);
    klvr_null.last_log[0] = 0;

    uint32_t w = 0, h = 0;
    sys->GetRecommendedRenderTargetSize(&w, &h);
    CHECK(w == 1920 && h == 1824);
    setenv("KLVR_RENDER_SCALE", "1.5", 1);
    sys->GetRecommendedRenderTargetSize(&w, &h);
    CHECK(w == 2880 && h == 2736);
    unsetenv("KLVR_RENDER_SCALE");

    float l, r, t, b;
    sys->GetProjectionRaw(vr::Eye_Left, &l, &r, &t, &b);
    NEAR(l, -1.0f); NEAR(r, 0.9f); NEAR(t, -1.2f); NEAR(b, 1.1f);   // up 1.1, down 1.2: top = -down

    // The matrix is built from those tangents as hellovr_vulkan does, so the
    // frustum's (left, top) and (right, bottom) edges land on NDC -1 and +1.
    vr::HmdMatrix44_t P = sys->GetProjectionMatrix(vr::Eye_Left, 0.1f, 100.0f);
    auto ndc = [&](float x, float y, float out[2]) {
        float p[4] = { x, y, -1.0f, 1.0f }, c[4];
        for (int i = 0; i < 4; i++) c[i] = P.m[i][0] * p[0] + P.m[i][1] * p[1] + P.m[i][2] * p[2] + P.m[i][3] * p[3];
        out[0] = c[0] / c[3]; out[1] = c[1] / c[3];
    };
    float c[2];
    ndc(l, t, c);
    NEAR(c[0], -1.0f); NEAR(c[1], -1.0f);
    ndc(r, b, c);
    NEAR(c[0], 1.0f); NEAR(c[1], 1.0f);
    // ...and geometrically: a view-space point 1.1 up (y+) at 1 m reaches the
    // top edge (NDC +1), and 1.2 down the bottom one.
    ndc(0, 1.1f, c);
    NEAR(c[1], 1.0f);
    ndc(0, -1.2f, c);
    NEAR(c[1], -1.0f);

    vr::HmdMatrix34_t e2h = sys->GetEyeToHeadTransform(vr::Eye_Right);
    NEAR(e2h.m[0][3], 0.032f);
    vr::ETrackedPropertyError pe;
    NEAR(sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &pe), 0.064f);
    NEAR(sys->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, &pe), 90.0f);

    CHECK(sys->GetTrackedDeviceClass(0) == vr::TrackedDeviceClass_HMD);
    CHECK(sys->GetTrackedDeviceClass(1) == vr::TrackedDeviceClass_Controller);
    CHECK(sys->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand) == 2);
    CHECK(sys->GetControllerRoleForTrackedDeviceIndex(1) == vr::TrackedControllerRole_LeftHand);
    vr::TrackedDeviceIndex_t idx[8];
    CHECK(sys->GetSortedTrackedDeviceIndicesOfClass(vr::TrackedDeviceClass_Controller, idx, 8) == 2);

    char buf[64];
    uint32_t n = sys->GetStringTrackedDeviceProperty(1, vr::Prop_ControllerType_String, buf, sizeof buf, &pe);
    CHECK(pe == vr::TrackedProp_Success && std::string(buf) == "oculus_touch" && n == 13);
    n = sys->GetStringTrackedDeviceProperty(1, vr::Prop_ControllerType_String, buf, 4, &pe);
    CHECK(pe == vr::TrackedProp_BufferTooSmall && n == 13 && std::string(buf) == "ocu");
    sys->GetStringTrackedDeviceProperty(0, vr::Prop_ManufacturerName_String, buf, sizeof buf, &pe);
    CHECK(std::string(buf) == "Apple");
    CHECK(sys->GetInt32TrackedDeviceProperty(2, vr::Prop_ControllerRoleHint_Int32, &pe) == vr::TrackedControllerRole_RightHand);
    sys->GetStringTrackedDeviceProperty(0, (vr::ETrackedDeviceProperty)1234567, buf, sizeof buf, &pe);
    CHECK(pe == vr::TrackedProp_UnknownProperty);

    // Events: the three devices announce themselves after init.
    vr::VREvent_t ev;
    int activated = 0;
    while (sys->PollNextEvent(&ev, sizeof ev))
        if (ev.eventType == vr::VREvent_TrackedDeviceActivated) activated++;
    CHECK(activated == 3);

    // Standing poses are the platform's floor poses.
    connect_controllers();
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
    CHECK(poses[0].bPoseIsValid);
    NEAR(poses[0].mDeviceToAbsoluteTracking.m[1][3], 1.6f);
    CHECK(poses[1].bPoseIsValid && poses[2].bPoseIsValid);
    // A controller is reported at SteamVR's Touch device origin, not at the
    // grip: 0.7 cm inboard, 3.4 cm below and 9.6 cm ahead of it, pitched
    // 20.6 degrees down (the grip is 20.6 up in the device's frame).
    NEAR(poses[2].mDeviceToAbsoluteTracking.m[0][3], 0.2f - 0.007f);
    NEAR(poses[2].mDeviceToAbsoluteTracking.m[1][3], 1.2f - 0.03415f);
    NEAR(poses[2].mDeviceToAbsoluteTracking.m[2][3], -0.3f - 0.09607f);
    NEAR(poses[1].mDeviceToAbsoluteTracking.m[0][3], -0.2f + 0.007f);
    NEAR(poses[2].mDeviceToAbsoluteTracking.m[1][2], std::sin(20.6f * 3.14159265f / 180));   // raw +Z tilts up
    CHECK(!poses[3].bDeviceIsConnected);

    // Seated: the head, turned 90 degrees left, becomes the origin facing -Z.
    float s = std::sqrt(0.5f);
    klvr_null.head.pose = { { 1.0f, 1.2f, 2.0f }, { 0, s, 0, s } };
    iface<vr::IVRChaperone>(vr::IVRChaperone_Version)->ResetZeroPose(vr::TrackingUniverseSeated);
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseSeated, 0, poses, 1);
    NEAR(poses[0].mDeviceToAbsoluteTracking.m[0][3], 0);
    NEAR(poses[0].mDeviceToAbsoluteTracking.m[1][3], 0);
    NEAR(poses[0].mDeviceToAbsoluteTracking.m[2][3], 0);
    NEAR(poses[0].mDeviceToAbsoluteTracking.m[0][0], 1);
    klvr_null.head.pose = { { 0, 1.6f, 0 }, { 0, 0, 0, 1 } };

    // Legacy input: trigger on axis 1, face button A.
    klvr_null.ctl[1].trigger = 0.5f;
    klvr_null.ctl[1].buttons = KLVR_BTN_FACE_LOWER;
    vr::VRControllerState_t st;
    CHECK(sys->GetControllerState(2, &st, sizeof st));
    NEAR(st.rAxis[1].x, 0.5f);
    CHECK(st.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_A));
    klvr_null.ctl[1] = {};
    klvr_null.ctl[1].connected = 1;

    CHECK(!log_has("NOT IMPLEMENTED"));
}

// The narrow-argument thunk: a Linux caller may leave garbage above bit 15 of
// an unsigned short. Call the slot as the raw ABI would see it.
static void test_abi_thunk() {
    auto *sys = iface<vr::IVRSystem>(vr::IVRSystem_Version);
    void **vt = *(void ***)sys;
    typedef void (*raw_fn)(void *, uint32_t, uint32_t, uint32_t);
    // TriggerHapticPulse's index among IVRSystem_026's methods in openvr.h.
    const int slot = 39;
    unsigned before = klvr_null.haptic_count[1];
    ((raw_fn)vt[slot])(sys, 2, 0, 0xABCD07D0u);   // 2000 us with junk above
    CHECK(klvr_null.haptic_count[1] == before + 1);
    NEAR(klvr_null.haptic_secs[1], 0.002f);
    // ...and the same call through the C++ class agrees.
    sys->TriggerHapticPulse(2, 0, 2000);
    NEAR(klvr_null.haptic_secs[1], 0.002f);
}

// ---- IVRCompositor --------------------------------------------------------
struct FakeVk {
    int blits = 0, submits = 0;
    VkImageBlit last{};
    VkImage last_dst = VK_NULL_HANDLE;
} g_vk;

static VKAPI_ATTR VkResult VKAPI_CALL fake_CreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *p) { *p = (VkCommandPool)0x10; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL fake_AllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *c) { static char cb[8]; *c = (VkCommandBuffer)(void *)cb; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL fake_Begin(VkCommandBuffer, const VkCommandBufferBeginInfo *) { return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL fake_End(VkCommandBuffer) { return VK_SUCCESS; }
static VKAPI_ATTR void VKAPI_CALL fake_Barrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *, uint32_t, const VkImageMemoryBarrier *) {}
static VKAPI_ATTR void VKAPI_CALL fake_Blit(VkCommandBuffer, VkImage, VkImageLayout, VkImage dst, VkImageLayout, uint32_t, const VkImageBlit *r, VkFilter) { g_vk.blits++; g_vk.last = *r; g_vk.last_dst = dst; }
static VKAPI_ATTR VkResult VKAPI_CALL fake_Submit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence) { g_vk.submits++; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL fake_CreateFence(VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *f) { *f = (VkFence)0x20; return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL fake_Wait(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t) { return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL fake_ResetFences(VkDevice, uint32_t, const VkFence *) { return VK_SUCCESS; }
static VKAPI_ATTR VkResult VKAPI_CALL fake_ResetCB(VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; }

static void *fake_vk_proc(const char *n) {
    std::string s = n;
    if (s == "vkCreateCommandPool") return (void *)fake_CreateCommandPool;
    if (s == "vkAllocateCommandBuffers") return (void *)fake_AllocateCommandBuffers;
    if (s == "vkBeginCommandBuffer") return (void *)fake_Begin;
    if (s == "vkEndCommandBuffer") return (void *)fake_End;
    if (s == "vkCmdPipelineBarrier") return (void *)fake_Barrier;
    if (s == "vkCmdBlitImage") return (void *)fake_Blit;
    if (s == "vkQueueSubmit") return (void *)fake_Submit;
    if (s == "vkCreateFence") return (void *)fake_CreateFence;
    if (s == "vkWaitForFences") return (void *)fake_Wait;
    if (s == "vkResetFences") return (void *)fake_ResetFences;
    if (s == "vkResetCommandBuffer") return (void *)fake_ResetCB;
    return nullptr;
}

static void test_compositor() {
    auto *comp = iface<vr::IVRCompositor>(vr::IVRCompositor_Version);
    klvr_null.last_log[0] = 0;
    connect_controllers();

    vr::TrackedDevicePose_t render[vr::k_unMaxTrackedDeviceCount], game[vr::k_unMaxTrackedDeviceCount];
    unsigned waited = klvr_null.frames_waited;
    CHECK(comp->WaitGetPoses(render, vr::k_unMaxTrackedDeviceCount, game, vr::k_unMaxTrackedDeviceCount) == vr::VRCompositorError_None);
    CHECK(klvr_null.frames_waited == waited + 1);
    CHECK(render[0].bPoseIsValid && game[0].bPoseIsValid);
    NEAR(render[0].mDeviceToAbsoluteTracking.m[1][3], 1.6f);
    CHECK(render[1].bPoseIsValid);

    char ext[256];
    CHECK(comp->GetVulkanInstanceExtensionsRequired(ext, sizeof ext) == 1 && ext[0] == 0);

    // Side by side in one 4000x2000 image, both eyes.
    vr::VRVulkanTextureData_t vk{};
    vk.m_nImage = 0x777;
    vk.m_pDevice = (VkDevice_T *)0x1;
    vk.m_pPhysicalDevice = (VkPhysicalDevice_T *)0x2;
    vk.m_pInstance = (VkInstance_T *)0x3;
    vk.m_pQueue = (VkQueue_T *)0x4;
    vk.m_nWidth = 4000;
    vk.m_nHeight = 2000;
    vk.m_nFormat = VK_FORMAT_R8G8B8A8_SRGB;
    vk.m_nSampleCount = 1;
    vr::Texture_t tex = { &vk, vr::TextureType_Vulkan, vr::ColorSpace_Auto };
    vr::VRTextureBounds_t left = { 0, 0, 0.5f, 1 }, right = { 0.5f, 0, 1, 1 };

    // No Vulkan on the platform: refused, not crashed.
    CHECK(comp->Submit(vr::Eye_Left, &tex, &left) != vr::VRCompositorError_None);
    comp->PostPresentHandoff();

    klvr_null.vk_proc = fake_vk_proc;
    unsigned ended = klvr_null.stages_ended;
    comp->WaitGetPoses(render, 1, nullptr, 0);
    CHECK(comp->Submit(vr::Eye_Left, &tex, &left) == vr::VRCompositorError_None);
    CHECK(g_vk.blits == 1);
    CHECK(g_vk.last.srcOffsets[0].x == 0 && g_vk.last.srcOffsets[1].x == 2000);
    CHECK(klvr_null.eye_w[0] == 2000 && klvr_null.eye_h[0] == 2000 && klvr_null.eye_srgb == 1);
    CHECK(klvr_null.stages_ended == ended);                 // one eye is not a frame
    CHECK(comp->Submit(vr::Eye_Right, &tex, &right) == vr::VRCompositorError_None);
    CHECK(g_vk.last.srcOffsets[0].x == 2000 && g_vk.last.srcOffsets[1].x == 4000);
    CHECK(klvr_null.stages_ended == ended + 1);             // two eyes are
    NEAR(klvr_null.last_render_head.p[1], 1.6f);
    CHECK(g_vk.submits == 2);

    // A flipped v range blits flipped.
    vr::VRTextureBounds_t flip = { 0, 1, 1, 0 };
    comp->WaitGetPoses(render, 1, nullptr, 0);
    comp->Submit(vr::Eye_Left, &tex, &flip);
    CHECK(g_vk.last.srcOffsets[0].y == 2000 && g_vk.last.srcOffsets[1].y == 0);
    comp->Submit(vr::Eye_Right, &tex, &flip);

    // Alyx's shape: one array image per eye layer, with the pose it rendered
    // from and its depth projection attached (flags 0x258). The stage is
    // closed with the submitted pose, and the projection is read back.
    {
        auto *sys = iface<vr::IVRSystem>(vr::IVRSystem_Version);
        vr::VRVulkanTextureArrayData_t arr{};
        static_cast<vr::VRVulkanTextureData_t &>(arr) = vk;
        arr.m_unArraySize = 2;
        vr::VRTextureWithPoseAndDepth_t tp{};
        tp.handle = &arr;
        tp.eType = vr::TextureType_Vulkan;
        tp.mDeviceToAbsoluteTracking = render[0].mDeviceToAbsoluteTracking;
        tp.mDeviceToAbsoluteTracking.m[1][3] = 1.7f;       // rendered 10 cm higher than handed
        auto flags = (vr::EVRSubmitFlags)(vr::Submit_TextureWithMotion | vr::Submit_VulkanTextureWithArrayData);
        klvr_null.last_log[0] = 0;
        comp->WaitGetPoses(render, 1, nullptr, 0);
        for (int e = 0; e < 2; e++) {
            arr.m_unArrayIndex = e;
            tp.depth.mProjection = sys->GetProjectionMatrix((vr::EVREye)e, 0.1f, 100.0f);
            CHECK(comp->Submit((vr::EVREye)e, &tp, nullptr, flags) == vr::VRCompositorError_None);
            CHECK(g_vk.last.srcSubresource.baseArrayLayer == (uint32_t)e);
            CHECK(log_has("rendered with tan") && !log_has("MISMATCH"));
        }
        NEAR(klvr_null.last_render_head.p[1], 1.7f);
        // The game rendered with our frustum, so the stage carries it: up 1.1, down 1.2.
        NEAR(klvr_null.last_tan[0][0], 1.0f); NEAR(klvr_null.last_tan[0][1], 0.9f);
        NEAR(klvr_null.last_tan[0][2], 1.1f); NEAR(klvr_null.last_tan[0][3], 1.2f);

        // A game that renders with a frustum of its own (here symmetric 1.5 x
        // 1.25, Vulkan-flipped, and column-major for the right eye): the
        // stage is closed with THAT, so the picture is placed as drawn.
        auto proj = [](float l, float r, float u, float d, bool flip, bool transpose) {
            vr::HmdMatrix44_t m{};
            m.m[0][0] = 2 / (r + l); m.m[0][2] = (r - l) / (r + l);
            m.m[1][1] = 2 / (u + d); m.m[1][2] = (u - d) / (u + d);
            if (flip) { m.m[1][1] = -m.m[1][1]; m.m[1][2] = -m.m[1][2]; }
            m.m[2][2] = 0; m.m[2][3] = 0.1f; m.m[3][2] = -1;   // reversed-Z, infinite far
            if (transpose) {
                vr::HmdMatrix44_t t{};
                for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) t.m[i][j] = m.m[j][i];
                m = t;
            }
            return m;
        };
        comp->WaitGetPoses(render, 1, nullptr, 0);
        klvr_null.last_log[0] = 0;
        for (int e = 0; e < 2; e++) {
            arr.m_unArrayIndex = e;
            tp.depth.mProjection = proj(1.5f, 1.5f, 1.25f, 1.25f * (e ? 1.0f : 0.8f), true, e == 1);
            CHECK(comp->Submit((vr::EVREye)e, &tp, nullptr, flags) == vr::VRCompositorError_None);
            CHECK(log_has("MISMATCH"));
        }
        for (int e = 0; e < 2; e++) {
            NEAR(klvr_null.last_tan[e][0], 1.5f); NEAR(klvr_null.last_tan[e][1], 1.5f);
            NEAR(klvr_null.last_tan[e][2], 1.25f); NEAR(klvr_null.last_tan[e][3], e ? 1.25f : 1.0f);
        }
        // A frame without depth goes back to the platform's frustum.
        comp->WaitGetPoses(render, 1, nullptr, 0);
        CHECK(comp->Submit(vr::Eye_Left, &tex, &left) == vr::VRCompositorError_None);
        CHECK(comp->Submit(vr::Eye_Right, &tex, &right) == vr::VRCompositorError_None);
        NEAR(klvr_null.last_tan[0][2], 1.1f);
    }

    // KLVR_ASYNC_FRAMES: the second Submit returns before the stage is
    // published, and a worker publishes it once the copies are done.
    {
        setenv("KLVR_ASYNC_FRAMES", "1", 1);
        unsigned before = klvr_null.stages_ended;
        for (int f = 0; f < 5; f++) {
            comp->WaitGetPoses(render, 1, nullptr, 0);
            CHECK(comp->Submit(vr::Eye_Left, &tex, &left) == vr::VRCompositorError_None);
            CHECK(comp->Submit(vr::Eye_Right, &tex, &right) == vr::VRCompositorError_None);
        }
        for (int i = 0; i < 200 && klvr_null.stages_ended < before + 5; i++) usleep(5000);
        CHECK(klvr_null.stages_ended == before + 5 && klvr_null.async_frames == 1);
        CHECK(log_has("worker thread") || klvr_null.async_frames);
        unsetenv("KLVR_ASYNC_FRAMES");
    }

    // No display (a desktop run): both eyes accepted, nothing copied, the
    // frame still closes.
    klvr_null.no_display = 1;
    auto blits = g_vk.blits;
    ended = klvr_null.stages_ended;
    comp->WaitGetPoses(render, 1, nullptr, 0);
    CHECK(comp->Submit(vr::Eye_Left, &tex, &left) == vr::VRCompositorError_None);
    CHECK(comp->Submit(vr::Eye_Right, &tex, &right) == vr::VRCompositorError_None);
    CHECK(g_vk.blits == blits && klvr_null.stages_ended == ended + 1);
    klvr_null.no_display = 0;

    // A GL texture is refused by name.
    vr::Texture_t gl = { (void *)1, vr::TextureType_OpenGL, vr::ColorSpace_Auto };
    CHECK(comp->Submit(vr::Eye_Left, &gl, nullptr) == vr::VRCompositorError_TextureUsesUnsupportedFormat);
    klvr_null.vk_proc = nullptr;

    comp->SetTrackingSpace(vr::TrackingUniverseSeated);
    CHECK(comp->GetTrackingSpace() == vr::TrackingUniverseSeated);
    comp->SetTrackingSpace(vr::TrackingUniverseStanding);
    CHECK(!log_has("NOT IMPLEMENTED"));
}

// ---- IVRInput -------------------------------------------------------------
static void test_input() {
    auto *in = iface<vr::IVRInput>(vr::IVRInput_Version);
    connect_controllers();
    char cwd[1024];
    CHECK(getcwd(cwd, sizeof cwd) != nullptr);
    // Alyx passes Windows separators ("cfg\\actions.json").
    std::string manifest = std::string(cwd) + "/fixtures\\actions.json";
    CHECK(in->SetActionManifestPath(manifest.c_str()) == vr::VRInputError_None);

    vr::VRActionSetHandle_t set, menu;
    CHECK(in->GetActionSetHandle("/actions/default", &set) == vr::VRInputError_None);
    CHECK(in->GetActionSetHandle("/actions/menu", &menu) == vr::VRInputError_None);
    vr::VRActionHandle_t trig, pull, move, tele, grab, pose, skl, skr, hap, sel;
    in->GetActionHandle("/actions/default/in/Trigger", &trig);
    in->GetActionHandle("/actions/default/in/triggerpull", &pull);   // case-insensitive
    in->GetActionHandle("/actions/default/in/Move", &move);
    in->GetActionHandle("/actions/default/in/Teleport", &tele);
    in->GetActionHandle("/actions/default/in/Grab", &grab);
    in->GetActionHandle("/actions/default/in/HandPose", &pose);
    in->GetActionHandle("/actions/default/in/SkeletonLeft", &skl);
    in->GetActionHandle("/actions/default/in/SkeletonRight", &skr);
    in->GetActionHandle("/actions/default/out/Haptic", &hap);
    in->GetActionHandle("/actions/menu/in/Select", &sel);
    vr::VRInputValueHandle_t lh, rh;
    in->GetInputSourceHandle("/user/hand/left", &lh);
    in->GetInputSourceHandle("/user/hand/right", &rh);
    CHECK(lh != rh && lh && rh);

    vr::VRActiveActionSet_t active{};
    active.ulActionSet = set;
    klvr_null.ctl[1].trigger = 1.0f;
    klvr_null.ctl[1].buttons = KLVR_BTN_TRIGGER;
    klvr_null.ctl[0].stick[0] = 0.25f;
    klvr_null.ctl[0].stick[1] = -0.5f;
    klvr_null.ctl[1].stick[1] = 0.9f;
    CHECK(in->UpdateActionState(&active, sizeof active, 1) == vr::VRInputError_None);

    vr::InputDigitalActionData_t d;
    CHECK(in->GetDigitalActionData(trig, &d, sizeof d, vr::k_ulInvalidInputValueHandle) == vr::VRInputError_None);
    CHECK(d.bActive && d.bState && d.bChanged && d.activeOrigin == rh);
    CHECK(in->GetDigitalActionData(trig, &d, sizeof d, lh) == vr::VRInputError_None);
    CHECK(!d.bActive && !d.bState);                        // nothing bound on the left
    vr::InputAnalogActionData_t a;
    CHECK(in->GetAnalogActionData(pull, &a, sizeof a, 0) == vr::VRInputError_None);
    NEAR(a.x, 1.0f);
    CHECK(in->GetAnalogActionData(move, &a, sizeof a, 0) == vr::VRInputError_None);
    NEAR(a.x, 0.25f); NEAR(a.y, -0.5f);
    CHECK(in->GetDigitalActionData(tele, &d, sizeof d, 0) == vr::VRInputError_None);
    CHECK(d.bState);                                       // right stick pushed north
    CHECK(in->GetDigitalActionData(trig, &d, sizeof d, 0) == vr::VRInputError_None);
    CHECK(in->GetAnalogActionData(trig, &a, sizeof a, 0) == vr::VRInputError_WrongType);

    // Held a second frame: still pressed, no longer "changed".
    in->UpdateActionState(&active, sizeof active, 1);
    in->GetDigitalActionData(trig, &d, sizeof d, 0);
    CHECK(d.bState && !d.bChanged);

    // The menu set is inactive, so its action is too, even with A held.
    klvr_null.ctl[1].buttons |= KLVR_BTN_FACE_LOWER;
    in->UpdateActionState(&active, sizeof active, 1);
    in->GetDigitalActionData(sel, &d, sizeof d, 0);
    CHECK(!d.bActive && !d.bState);
    vr::VRActiveActionSet_t both[2] = { active, active };
    both[1].ulActionSet = menu;
    in->UpdateActionState(both, sizeof both[0], 2);
    in->GetDigitalActionData(sel, &d, sizeof d, 0);
    CHECK(d.bActive && d.bState);

    // A set restricted to the left hand ignores the right trigger.
    active.ulRestrictedToDevice = lh;
    in->UpdateActionState(&active, sizeof active, 1);
    in->GetDigitalActionData(trig, &d, sizeof d, 0);
    CHECK(!d.bState);
    // ...but the same set listed again for the right hand takes it back:
    // entries accumulate.
    vr::VRActiveActionSet_t per_hand[2] = { active, active };
    per_hand[1].ulRestrictedToDevice = rh;
    in->UpdateActionState(per_hand, sizeof per_hand[0], 2);
    in->GetDigitalActionData(trig, &d, sizeof d, 0);
    CHECK(d.bState);
    // A left-only entry's secondary set is live for the right hand.
    vr::VRActiveActionSet_t sec = active;
    sec.ulSecondaryActionSet = menu;
    in->UpdateActionState(&sec, sizeof sec, 1);
    in->GetDigitalActionData(sel, &d, sizeof d, 0);
    CHECK(d.bActive && d.bState);                        // right A, through the secondary set
    in->GetDigitalActionData(trig, &d, sizeof d, 0);
    CHECK(!d.bState);                                    // right trigger, primary set is left-only
    active.ulRestrictedToDevice = 0;

    // Grab is bound on both hands; restricting the query picks one.
    klvr_null.ctl[0].buttons = KLVR_BTN_GRIP;
    in->UpdateActionState(&active, sizeof active, 1);
    in->GetDigitalActionData(grab, &d, sizeof d, lh);
    CHECK(d.bState && d.activeOrigin == lh);
    in->GetDigitalActionData(grab, &d, sizeof d, rh);
    CHECK(!d.bState);

    // Poses follow the bound hand.
    vr::InputPoseActionData_t pd;
    CHECK(in->GetPoseActionDataForNextFrame(pose, vr::TrackingUniverseStanding, &pd, sizeof pd, rh) == vr::VRInputError_None);
    CHECK(pd.bActive && pd.pose.bPoseIsValid && pd.activeOrigin == rh);
    NEAR(pd.pose.mDeviceToAbsoluteTracking.m[0][3], 0.2f - 0.007f);
    in->GetPoseActionDataRelativeToNow(pose, vr::TrackingUniverseStanding, 0, &pd, sizeof pd, lh);
    NEAR(pd.pose.mDeviceToAbsoluteTracking.m[0][3], -0.2f + 0.007f);

    // Skeleton: 31 bones; grip closes the left hand's middle finger.
    uint32_t bones = 0;
    CHECK(in->GetBoneCount(skl, &bones) == vr::VRInputError_None && bones == 31);
    vr::BoneIndex_t parents[31];
    CHECK(in->GetBoneHierarchy(skl, parents, 31) == vr::VRInputError_None);
    CHECK(parents[0] == -1 && parents[1] == 0 && parents[7] == 6 && parents[27] == 0);
    char name[32];
    in->GetBoneName(skl, 6, name, sizeof name);
    CHECK(std::string(name) == "IndexFinger0");
    vr::InputSkeletalActionData_t sd;
    CHECK(in->GetSkeletalActionData(skl, &sd, sizeof sd) == vr::VRInputError_None && sd.bActive && sd.activeOrigin == lh);
    CHECK(in->GetSkeletalActionData(skr, &sd, sizeof sd) == vr::VRInputError_None && sd.bActive && sd.activeOrigin == rh);
    vr::VRBoneTransform_t open[31], fist[31], live[31];
    in->GetSkeletalReferenceTransforms(skl, vr::VRSkeletalTransformSpace_Parent, vr::VRSkeletalReferencePose_OpenHand, open, 31);
    in->GetSkeletalReferenceTransforms(skl, vr::VRSkeletalTransformSpace_Parent, vr::VRSkeletalReferencePose_Fist, fist, 31);
    CHECK(in->GetSkeletalBoneData(skl, vr::VRSkeletalTransformSpace_Parent, vr::VRSkeletalMotionRange_WithoutController, live, 31) == vr::VRInputError_None);
    NEAR(live[12].orientation.w, fist[12].orientation.w);   // middle finger: gripped
    NEAR(live[7].orientation.w, open[7].orientation.w);     // index: trigger untouched
    vr::VRSkeletalSummaryData_t sum;
    CHECK(in->GetSkeletalSummaryData(skl, vr::VRSummaryType_FromDevice, &sum) == vr::VRInputError_None);
    NEAR(sum.flFingerCurl[2], 1.0f);
    NEAR(sum.flFingerCurl[1], 0.0f);

    // Model space: the open hand's index tip sits in front of the wrist.
    vr::VRBoneTransform_t model[31];
    in->GetSkeletalReferenceTransforms(skl, vr::VRSkeletalTransformSpace_Model, vr::VRSkeletalReferencePose_OpenHand, model, 31);
    float dx = model[10].position.v[0] - model[27].position.v[0], dy = model[10].position.v[1] - model[27].position.v[1],
          dz = model[10].position.v[2] - model[27].position.v[2];
    CHECK(std::sqrt(dx * dx + dy * dy + dz * dz) < 0.03f);  // aux bone beside its tip

    // SteamVR's frame: the root does not rotate, fingers point forward (-Z)
    // from the wrist, and the left hand is the right hand's mirror image.
    vr::VRBoneTransform_t rmodel[31];
    in->GetSkeletalReferenceTransforms(skr, vr::VRSkeletalTransformSpace_Model, vr::VRSkeletalReferencePose_OpenHand, rmodel, 31);
    NEAR(model[0].orientation.w, 1.0f);
    NEAR(rmodel[0].orientation.w, 1.0f);
    CHECK(model[1].position.v[0] < -0.02f && rmodel[1].position.v[0] > 0.02f);   // wrists: left at -x, right at +x
    CHECK(model[10].position.v[2] < model[1].position.v[2] - 0.1f);               // index tip ahead of the wrist
    CHECK(rmodel[10].position.v[2] < rmodel[1].position.v[2] - 0.1f);
    // Mirrored to within Valve's own data, whose two hands differ by up to a centimetre.
    for (int b = 1; b < 31; b++)
        CHECK(std::fabs(model[b].position.v[0] + rmodel[b].position.v[0]) < 0.012f &&
              std::fabs(model[b].position.v[1] - rmodel[b].position.v[1]) < 0.012f &&
              std::fabs(model[b].position.v[2] - rmodel[b].position.v[2]) < 0.012f);

    // Compressed round trip.
    char packed[2048];
    uint32_t need = 0;
    CHECK(in->GetSkeletalBoneDataCompressed(skl, vr::VRSkeletalMotionRange_WithoutController, packed, sizeof packed, &need) == vr::VRInputError_None);
    vr::VRBoneTransform_t back[31];
    CHECK(in->DecompressSkeletalBoneData(packed, need, vr::VRSkeletalTransformSpace_Parent, back, 31) == vr::VRInputError_None);
    CHECK(memcmp(back, live, sizeof back) == 0);

    // Haptics reach both bound hands, or one when restricted.
    unsigned hl = klvr_null.haptic_count[0], hr = klvr_null.haptic_count[1];
    CHECK(in->TriggerHapticVibrationAction(hap, 0, 0.1f, 160, 0.7f, 0) == vr::VRInputError_None);
    CHECK(klvr_null.haptic_count[0] == hl + 1 && klvr_null.haptic_count[1] == hr + 1);
    NEAR(klvr_null.haptic_amp[0], 0.7f);
    NEAR(klvr_null.haptic_freq[0], 160.0f);
    in->TriggerHapticVibrationAction(hap, 0, 0.1f, 160, 0.7f, rh);
    CHECK(klvr_null.haptic_count[0] == hl + 1 && klvr_null.haptic_count[1] == hr + 2);

    klvr_null.ctl[0] = klvr_null.ctl[1] = {};
    CHECK(!log_has("NOT IMPLEMENTED"));
}

// ---- stubs ----------------------------------------------------------------
static void test_stubs() {
    // An interface nothing implements still has a full vtable, and its
    // methods report failure rather than success.
    auto *ov = iface<vr::IVROverlay>(vr::IVROverlay_Version);
    vr::VROverlayHandle_t hnd = 0;
    klvr_null.quiet = 1;
    uint32_t iw = 0, ih = 0;
    CHECK(ov->GetOverlayImageData(hnd, nullptr, 0, &iw, &ih) != vr::VROverlayError_None);
    CHECK(log_has("IVROverlay::GetOverlayImageData"));
    auto *rm = iface<vr::IVRRenderModels>(vr::IVRRenderModels_Version);
    vr::RenderModel_t *model = (vr::RenderModel_t *)0x1;
    CHECK(rm->LoadRenderModel_Async("oculus_quest2_controller_left", &model) == vr::VRRenderModelError_InvalidModel);
    CHECK(model == nullptr);
    klvr_null.quiet = 0;
}

extern "C" void *VRControlPanel(void);

// ---- SteamVR-private interfaces ---------------------------------------------
static void test_private() {
    vr::EVRInitError e;
    // IVRMailbox_001's four known slots, called through the vtable (a stand-in
    // abstract class would let the compiler devirtualise to its pure stubs).
    void *mb = vr::VR_GetGenericInterface("IVRMailbox_001", &e);
    void **vt = mb ? *(void ***)mb : nullptr;
    auto Register = [&](const char *n, uint64_t *h) { return ((uint32_t (*)(void *, const char *, uint64_t *))vt[0])(mb, n, h); };
    auto Unregister = [&](uint64_t h) { return ((uint32_t (*)(void *, uint64_t))vt[1])(mb, h); };
    auto Send = [&](uint64_t h, const char *t, const char *m) { return ((uint32_t (*)(void *, uint64_t, const char *, const char *))vt[2])(mb, h, t, m); };
    auto Read = [&](uint64_t h, char *b, uint32_t n, uint32_t *l) { return ((uint32_t (*)(void *, uint64_t, char *, uint32_t, uint32_t *))vt[3])(mb, h, b, n, l); };
    CHECK(mb && e == vr::VRInitError_None);
    uint64_t h = 0;
    CHECK(Register("steamvr.alyx", &h) == 0 && h != 0);
    char buf[64];
    uint32_t len = 0;
    CHECK(Read(h, buf, 4, &len) == 2 && len > 4);            // too short: says how much
    CHECK(Read(h, buf, sizeof buf, &len) == 0);              // then the ready message
    CHECK(strstr(buf, "\"ready\"") != nullptr && len == strlen(buf));
    CHECK(Read(h, buf, sizeof buf, &len) == 1);              // and nothing more
    CHECK(Send(h, "hello", "{}") == 0);
    CHECK(Unregister(h) == 0);

    // VRControlPanel: slots 15 and 26 answer, with junk above a bool.
    void *cp = VRControlPanel();
    CHECK(cp != nullptr);
    typedef void (*toggle_fn)(void *, uint64_t);
    typedef uint64_t (*root_fn)(void *);
    ((toggle_fn)(*(void ***)cp)[15])(cp, 0xabcdef00ull);
    CHECK(log_has("dashboard toggle disabled"));
    CHECK(((root_fn)(*(void ***)cp)[26])(cp) == 0);

    // IVRIPCResourceManagerClient_004 is withheld: served, it switches libvr
    // to zero-copy presentation and Submit never sees a frame.
    CHECK(vr::VR_GetGenericInterface("IVRIPCResourceManagerClient_004", &e) == nullptr);

    // IXrProto_001: not found, as in public SteamVR.
    CHECK(vr::VR_GetGenericInterface("IXrProto_001", &e) == nullptr && e == vr::VRInitError_Init_InterfaceNotFound);
}

// ---- IVROverlay -------------------------------------------------------------
static void test_overlay() {
    auto *ov = iface<vr::IVROverlay>(vr::IVROverlay_Version);
    vr::VROverlayHandle_t h = 123;
    CHECK(ov->FindOverlay("valve.steam.desktop", &h) == vr::VROverlayError_UnknownOverlay);
    CHECK(h == vr::k_ulOverlayHandleInvalid);
    CHECK(ov->CreateOverlay("hlvr.loading", "Loading", &h) == vr::VROverlayError_None && h != 0);
    vr::VROverlayHandle_t h2 = 0;
    CHECK(ov->CreateOverlay("hlvr.loading", "again", &h2) == vr::VROverlayError_KeyInUse);
    CHECK(ov->FindOverlay("hlvr.loading", &h2) == vr::VROverlayError_None && h2 == h);
    CHECK(!ov->IsOverlayVisible(h));
    CHECK(ov->ShowOverlay(h) == vr::VROverlayError_None && ov->IsOverlayVisible(h));
    CHECK(ov->SetOverlayWidthInMeters(h, 2.5f) == vr::VROverlayError_None);
    float w = 0;
    CHECK(ov->GetOverlayWidthInMeters(h, &w) == vr::VROverlayError_None && w == 2.5f);
    CHECK(ov->SetOverlayFlag(h, vr::VROverlayFlags_SideBySide_Parallel, true) == vr::VROverlayError_None);
    bool on = false;
    CHECK(ov->GetOverlayFlag(h, vr::VROverlayFlags_SideBySide_Parallel, &on) == vr::VROverlayError_None && on);
    char key[32];
    CHECK(ov->GetOverlayKey(h, key, sizeof key, nullptr) == strlen("hlvr.loading") + 1 && !strcmp(key, "hlvr.loading"));
    CHECK(ov->DestroyOverlay(h) == vr::VROverlayError_None);
    CHECK(ov->ShowOverlay(h) == vr::VROverlayError_UnknownOverlay);
    CHECK(!log_has("NOT IMPLEMENTED: IVROverlay"));
}

// ---- compositor fades -----------------------------------------------------------
static void test_fades() {
    auto *comp = iface<vr::IVRCompositor>(vr::IVRCompositor_Version);
    klvr_null.fixed_now = 100.0;
    CHECK(comp->GetCurrentGridAlpha() == 0.0f);
    comp->FadeGrid(1.0f, true);                  // Source 2 before a level load
    CHECK(comp->IsCurrentSceneFocusAppLoading());
    klvr_null.fixed_now = 100.5;
    NEAR(comp->GetCurrentGridAlpha(), 0.5f);
    klvr_null.fixed_now = 101.5;
    CHECK(comp->GetCurrentGridAlpha() == 1.0f);   // arrives, and the load can start
    comp->FadeGrid(0.0f, false);
    CHECK(comp->GetCurrentGridAlpha() == 0.0f && !comp->IsCurrentSceneFocusAppLoading());
    comp->FadeToColor(2.0f, 0, 0, 0, 1, false);
    klvr_null.fixed_now = 102.5;
    NEAR(comp->GetCurrentFadeColor(false).a, 0.5f);
    klvr_null.fixed_now = 110.0;
    CHECK(comp->GetCurrentFadeColor(false).a == 1.0f);
    comp->FadeToColor(0, 0, 0, 0, 0, false);

    // Frame timing reports how many display intervals each frame really took.
    vr::TrackedDevicePose_t poses[3];
    double period = 1.0 / klvr_null.display.refresh_hz;
    klvr_null.fixed_now = 200.0;
    comp->WaitGetPoses(poses, 3, nullptr, 0);
    klvr_null.fixed_now = 200.0 + 3 * period;   // a frame three vsyncs long
    comp->WaitGetPoses(poses, 3, nullptr, 0);
    vr::Compositor_FrameTiming ft{};
    ft.m_nSize = sizeof ft;
    CHECK(comp->GetFrameTiming(&ft, 0));
    CHECK(ft.m_nNumFramePresents == 3 && ft.m_nNumDroppedFrames == 2);
    NEAR(ft.m_flClientFrameIntervalMs, (float)(3 * period * 1e3));

    // The vsync counter counts display vsyncs, not rendered frames.
    auto *sys = iface<vr::IVRSystem>(vr::IVRSystem_Version);
    float since = 0;
    uint64_t v0 = 0, v1 = 0;
    CHECK(sys->GetTimeSinceLastVsync(&since, &v0));
    klvr_null.fixed_now += 3.5 * period;           // no frame rendered meanwhile
    CHECK(sys->GetTimeSinceLastVsync(&since, &v1));
    CHECK(v1 - v0 == 3 || v1 - v0 == 4);
    CHECK(since >= 0 && since < period);
    klvr_null.fixed_now = 0;
}

int main() {
    test_loader();
    test_system();
    test_abi_thunk();
    test_compositor();
    test_input();
    test_stubs();
    test_private();
    test_overlay();
    test_fades();
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
