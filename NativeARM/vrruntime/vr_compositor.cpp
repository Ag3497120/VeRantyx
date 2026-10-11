// IVRCompositor_029: pose hand-out paced to the virtual display, and Submit:
// the game's Vulkan eye images are read back and published to the Mac encoder
// through the shared frame file.
#include "gen_stubs.h"
#include "vr_runtime.h"
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>
#include <mutex>
#include <pthread.h>
#include <deque>
#include <condition_variable>
#include <thread>
#include <dlfcn.h>
#include <unistd.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace {

using namespace vr;

// ---- Vulkan readback of a submitted eye image ----
struct Readback {
    VkInstance inst = nullptr; VkDevice dev = nullptr; VkPhysicalDevice phys = nullptr; VkQueue queue = nullptr; uint32_t qfi = 0;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkCreateCommandPool pCreateCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers pAllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer pBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer pEndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer pResetCommandBuffer = nullptr;
    PFN_vkCmdCopyImageToBuffer pCmdCopyImageToBuffer = nullptr;
    PFN_vkCmdPipelineBarrier pCmdPipelineBarrier = nullptr;
    PFN_vkQueueSubmit pQueueSubmit = nullptr;
    PFN_vkCreateFence pCreateFence = nullptr;
    PFN_vkWaitForFences pWaitForFences = nullptr;
    PFN_vkResetFences pResetFences = nullptr;
    PFN_vkCreateBuffer pCreateBuffer = nullptr;
    PFN_vkDestroyBuffer pDestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements pGetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory pAllocateMemory = nullptr;
    PFN_vkFreeMemory pFreeMemory = nullptr;
    PFN_vkBindBufferMemory pBindBufferMemory = nullptr;
    PFN_vkMapMemory pMapMemory = nullptr;
    PFN_vkUnmapMemory pUnmapMemory = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties pGetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkInvalidateMappedMemoryRanges pInvalidateMappedMemoryRanges = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    struct Staging { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; void *map = nullptr; size_t cap = 0; uint32_t w = 0, h = 0; VkFormat fmt = VK_FORMAT_UNDEFINED; bool ready = false; bool coherent = true; };
    Staging eye[2];
    // Asynchronous readback: two slots used in turn. The game thread records
    // and submits the copies; a worker waits for the slot's fence, converts
    // and publishes, then frees the slot. The game never waits for its GPU.
    struct Slot {
        Staging eye[2];
        VkCommandBuffer cmd[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkFence fence = VK_NULL_HANDLE;
        bool flip[2] = { false, false };
        float head[16] = {};
        double visionTimestamp = 0;
        int state = 0;                       // 0 free, 1 eye 0 submitted, 2 in flight (worker's)
    };
    Slot slot[2];
    bool ok = false;
    unsigned failures = 0;

    static PFN_vkGetInstanceProcAddr gipa() {
        static PFN_vkGetInstanceProcAddr f;
        if (f) return f;
        const char *dir = getenv("KL_MVK_DIR");
        std::string path = std::string(dir && *dir ? dir : "vendor-moltenvk/out/macos") + "/libMoltenVK.dylib";
        void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) h = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
        if (h) f = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
        if (!f) vrrt::log("readback: cannot load MoltenVK's vkGetInstanceProcAddr (%s)", path.c_str());
        return f;
    }

    bool init(const VRVulkanTextureData_t *d) {
        inst = d->m_pInstance; dev = d->m_pDevice; phys = d->m_pPhysicalDevice; queue = d->m_pQueue; qfi = d->m_nQueueFamilyIndex;
        PFN_vkGetInstanceProcAddr gi = gipa();
        if (!gi) return false;
        gdpa = (PFN_vkGetDeviceProcAddr)gi(inst, "vkGetDeviceProcAddr");
        pGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gi(inst, "vkGetPhysicalDeviceMemoryProperties");
        if (!gdpa || !pGetPhysicalDeviceMemoryProperties) { vrrt::log("readback: instance entry points missing"); return false; }
#define D(n) p##n = (PFN_vk##n)gdpa(dev, "vk" #n); if (!p##n) { vrrt::log("readback: vk" #n " missing"); return false; }
        D(CreateCommandPool) D(AllocateCommandBuffers) D(BeginCommandBuffer) D(EndCommandBuffer) D(ResetCommandBuffer)
        D(CmdCopyImageToBuffer) D(CmdPipelineBarrier) D(QueueSubmit) D(CreateFence) D(WaitForFences) D(ResetFences)
        D(CreateBuffer) D(DestroyBuffer) D(GetBufferMemoryRequirements) D(AllocateMemory) D(FreeMemory) D(BindBufferMemory)
        D(MapMemory) D(UnmapMemory) D(InvalidateMappedMemoryRanges)
#undef D
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfi };
        if (pCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) { vrrt::log("readback: command pool failed"); return false; }
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
        if (pAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS) { vrrt::log("readback: command buffer failed"); return false; }
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, 0 };
        if (pCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) { vrrt::log("readback: fence failed"); return false; }
        for (Slot &sl : slot) {
            VkCommandBufferAllocateInfo c2 = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2 };
            if (pAllocateCommandBuffers(dev, &c2, sl.cmd) != VK_SUCCESS) { vrrt::log("readback: slot command buffers failed"); return false; }
            if (pCreateFence(dev, &fci, nullptr, &sl.fence) != VK_SUCCESS) { vrrt::log("readback: slot fence failed"); return false; }
        }
        ok = true;
        vrrt::log("readback: Vulkan device %p queue %p (family %u) ready", (void *)dev, (void *)queue, qfi);
        return true;
    }

    bool ensure_staging(Staging &s, size_t bytes, uint32_t w, uint32_t h, VkFormat fmt) {
        if (s.ready && s.cap >= bytes) { s.w = w; s.h = h; s.fmt = fmt; return true; }
        if (s.buf) { pUnmapMemory(dev, s.mem); pDestroyBuffer(dev, s.buf, nullptr); pFreeMemory(dev, s.mem, nullptr); s = Staging(); }
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr };
        if (pCreateBuffer(dev, &bci, nullptr, &s.buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req; pGetBufferMemoryRequirements(dev, s.buf, &req);
        VkPhysicalDeviceMemoryProperties mp; pGetPhysicalDeviceMemoryProperties(phys, &mp);
        uint32_t type = UINT32_MAX; bool coherent = false;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            if (!(req.memoryTypeBits & (1u << i))) continue;
            VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
                bool coh = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
                if (type == UINT32_MAX || (coh && !coherent)) { type = i; coherent = coh; }
            }
        }
        if (type == UINT32_MAX) { vrrt::log("readback: no host-visible memory type"); return false; }
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type };
        if (pAllocateMemory(dev, &mai, nullptr, &s.mem) != VK_SUCCESS) return false;
        if (pBindBufferMemory(dev, s.buf, s.mem, 0) != VK_SUCCESS) return false;
        if (pMapMemory(dev, s.mem, 0, VK_WHOLE_SIZE, 0, &s.map) != VK_SUCCESS) return false;
        s.cap = bytes; s.w = w; s.h = h; s.fmt = fmt; s.ready = true; s.coherent = coherent;
        return true;
    }

    // Record and submit the copy of one eye into a slot; eye 1 signals the
    // slot's fence (which also covers eye 0, submitted earlier on the queue).
    bool record(Slot &sl, int eyeIndex, VkImage image, uint32_t x, uint32_t y, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp, uint32_t layer) {
        Staging &s = sl.eye[eyeIndex];
        if (!ensure_staging(s, (size_t)w * h * bpp, w, h, fmt)) return false;
        VkCommandBuffer c = sl.cmd[eyeIndex];
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr };
        pResetCommandBuffer(c, 0);
        if (pBeginCommandBuffer(c, &bi) != VK_SUCCESS) return false;
        VkImageMemoryBarrier bar = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 } };
        pCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy region = {};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
        region.imageOffset = { (int32_t)x, (int32_t)y, 0 };
        region.imageExtent = { w, h, 1 };
        pCmdCopyImageToBuffer(c, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buf, 1, &region);
        // The game's next frame renders into this image: it must wait for the
        // read (write-after-read), and the copy must be visible to the host.
        VkImageMemoryBarrier after = bar;
        after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        after.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        VkMemoryBarrier host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
        pCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                            0, 1, &host, 0, nullptr, 1, &after);
        if (pEndCommandBuffer(c) != VK_SUCCESS) return false;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &c, 0, nullptr };
        VkFence f = VK_NULL_HANDLE;
        if (eyeIndex == 1) { f = sl.fence; pResetFences(dev, 1, &f); }
        return pQueueSubmit(queue, 1, &si, f) == VK_SUCCESS;
    }
    // Worker side: wait for a slot's copies.
    bool wait(Slot &sl) {
        if (pWaitForFences(dev, 1, &sl.fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return false;
        for (Staging &s : sl.eye)
            if (!s.coherent) { VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, s.mem, 0, VK_WHOLE_SIZE }; pInvalidateMappedMemoryRanges(dev, 1, &r); }
        return true;
    }

    // Copy the eye image into the staging buffer and wait for it.
    bool copy(int eyeIndex, VkImage image, uint32_t x, uint32_t y, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp, uint32_t layer) {
        Staging &s = eye[eyeIndex];
        if (!ensure_staging(s, (size_t)w * h * bpp, w, h, fmt)) return false;
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr };
        pResetCommandBuffer(cmd, 0);
        if (pBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return false;
        // OpenVR's Vulkan contract: the application hands the image over in
        // TRANSFER_SRC_OPTIMAL; make the rendering visible to the transfer.
        VkImageMemoryBarrier bar = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 } };
        pCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy region = {};
        region.bufferOffset = 0; region.bufferRowLength = 0; region.bufferImageHeight = 0;
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
        region.imageOffset = { (int32_t)x, (int32_t)y, 0 };
        region.imageExtent = { w, h, 1 };
        pCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buf, 1, &region);
        if (pEndCommandBuffer(cmd) != VK_SUCCESS) return false;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd, 0, nullptr };
        pResetFences(dev, 1, &fence);
        if (pQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return false;
        if (pWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) return false;
        if (!s.coherent) { VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, s.mem, 0, VK_WHOLE_SIZE }; pInvalidateMappedMemoryRanges(dev, 1, &r); }
        return true;
    }
};

static uint32_t bytes_per_pixel(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: return 8;
    default: return 0;
    }
}

static inline uint8_t half_to_u8(uint16_t h) {
    uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, mant = h & 0x3ff;
    float v;
    if (exp == 0) v = ldexpf((float)mant, -24);
    else if (exp == 31) v = 1.0f;
    else v = ldexpf((float)(mant | 0x400), (int)exp - 25);
    if (sign) v = 0;
    if (v < 0) v = 0; if (v > 1) v = 1;
    return (uint8_t)(v * 255.0f + 0.5f);
}

// Convert one eye from the staging buffer into the encoder's 8-bit layout
// (DXGI 28 RGBA or 87 BGRA), writing into the shared frame at column x0.
// The eye is scaled (nearest) to dw x dh: the two eyes' own frusta differ by a
// few percent, and the encoder expects two equal halves, each spanning its
// eye's frustum.
static void convert_eye(const Readback::Staging &s, uint8_t *dst, uint32_t dstRowBytes, uint32_t x0, bool flipY, uint32_t *outFormat,
                        uint32_t dw, uint32_t dh) {
    const uint32_t w = s.w, h = s.h;
    const uint8_t *src = (const uint8_t *)s.map;
    uint32_t bpp = bytes_per_pixel(s.fmt);
    const bool scale = dw != w || dh != h;
    std::vector<uint8_t> rowbuf(scale ? (size_t)w * 4 : 0);
    std::vector<uint32_t> xmap(scale ? dw : 0);
    for (uint32_t c = 0; scale && c < dw; c++) xmap[c] = std::min(w - 1, (uint32_t)(((uint64_t)c * w + w / 2) / dw));
    for (uint32_t orow = 0; orow < (scale ? dh : h); orow++) {
        const uint32_t row = scale ? std::min(h - 1, (uint32_t)(((uint64_t)orow * h + h / 2) / dh)) : orow;
        const uint32_t srow = flipY ? h - 1 - row : row;
        const uint8_t *sp = src + (size_t)srow * w * bpp;
        uint8_t *const out = dst + (size_t)orow * dstRowBytes + (size_t)x0 * 4;
        uint8_t *dp = scale ? rowbuf.data() : out;
        switch (s.fmt) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: memcpy(dp, sp, (size_t)w * 4); *outFormat = 28; break;
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: memcpy(dp, sp, (size_t)w * 4); *outFormat = 87; break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {   // R in bits 0-9 → BGRA8
            const uint32_t *p = (const uint32_t *)sp;
            for (uint32_t c = 0; c < w; c++) {
                uint32_t v = p[c];
                dp[c * 4 + 0] = (uint8_t)((((v >> 20) & 0x3ff) * 255u + 511u) / 1023u);
                dp[c * 4 + 1] = (uint8_t)((((v >> 10) & 0x3ff) * 255u + 511u) / 1023u);
                dp[c * 4 + 2] = (uint8_t)(((v & 0x3ff) * 255u + 511u) / 1023u);
                dp[c * 4 + 3] = 255;
            }
            *outFormat = 87; break;
        }
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {   // B in bits 0-9 → BGRA8
            const uint32_t *p = (const uint32_t *)sp;
            for (uint32_t c = 0; c < w; c++) {
                uint32_t v = p[c];
                dp[c * 4 + 0] = (uint8_t)(((v & 0x3ff) * 255u + 511u) / 1023u);
                dp[c * 4 + 1] = (uint8_t)((((v >> 10) & 0x3ff) * 255u + 511u) / 1023u);
                dp[c * 4 + 2] = (uint8_t)((((v >> 20) & 0x3ff) * 255u + 511u) / 1023u);
                dp[c * 4 + 3] = 255;
            }
            *outFormat = 87; break;
        }
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
            const uint16_t *p = (const uint16_t *)sp;
            for (uint32_t c = 0; c < w; c++) { dp[c * 4 + 0] = half_to_u8(p[c * 4 + 0]); dp[c * 4 + 1] = half_to_u8(p[c * 4 + 1]); dp[c * 4 + 2] = half_to_u8(p[c * 4 + 2]); dp[c * 4 + 3] = 255; }
            *outFormat = 28; break;
        }
        case VK_FORMAT_R16G16B16A16_UNORM: {
            const uint16_t *p = (const uint16_t *)sp;
            for (uint32_t c = 0; c < w; c++) { dp[c * 4 + 0] = p[c * 4 + 0] >> 8; dp[c * 4 + 1] = p[c * 4 + 1] >> 8; dp[c * 4 + 2] = p[c * 4 + 2] >> 8; dp[c * 4 + 3] = 255; }
            *outFormat = 28; break;
        }
        default: break;
        }
        if (scale) {
            const uint32_t *in = (const uint32_t *)rowbuf.data();
            uint32_t *o = (uint32_t *)out;
            for (uint32_t c = 0; c < dw; c++) o[c] = in[xmap[c]];
        }
    }
}

class Compositor : public Stub_IVRCompositor {
public:
    ETrackingUniverseOrigin m_origin = TrackingUniverseStanding;
    EVRCompositorTimingMode m_timing = VRCompositorTimingMode_Implicit;
    HmdColor_t m_fade = { 0, 0, 0, 0 };
    Readback m_rb;
    // Asynchronous publishing (Readback::Slot): VERANTYX_SYNC_READBACK=1 restores
    // the old copy-and-wait path.
    int m_slot = 0;
    std::mutex m_wmu;
    std::condition_variable m_wcv;
    std::thread m_worker;
    std::deque<int> m_pending;               // slots in flight, in order
    bool m_async = getenv("VERANTYX_SYNC_READBACK") == nullptr || strcmp(getenv("VERANTYX_SYNC_READBACK"), "1") != 0;
    bool m_eyeDone[2] = { false, false };
    bool m_eyeFlip[2] = { false, false };
    unsigned m_submits = 0, m_published = 0, m_skipped = 0;
    double m_lastReport = 0;
    uint32_t m_posePredictionId = 0;
    std::mutex m_mu;

    ETrackingUniverseOrigin GetTrackingSpace() override { return m_origin; }
    void SetTrackingSpace(ETrackingUniverseOrigin o) override { m_origin = o; }

    static void copy_poses(const vrrt::PoseSet &set, TrackedDevicePose_t *arr, uint32_t n) {
        for (uint32_t i = 0; i < n; i++) {
            if (i < 3) arr[i] = set.dev[i];
            else { memset(&arr[i], 0, sizeof arr[i]); arr[i].eTrackingResult = TrackingResult_Uninitialized; }
        }
    }
    // Frame timing, reported with the 5 s compositor line: how long the game
    // works between WaitGetPoses calls, how long it waits for the virtual
    // vsync, and what the readback and conversion cost.
    double m_tWaitEnd = 0, m_sumWork = 0, m_sumWait = 0, m_sumReadback = 0, m_sumConvert = 0, m_maxWork = 0;
    uint32_t m_nFrames = 0, m_nReadback = 0, m_nConvert = 0;

    EVRCompositorError WaitGetPoses(TrackedDevicePose_t *render, uint32_t nr, TrackedDevicePose_t *game, uint32_t ng) override {
        const double t0 = vrrt::now_seconds();
        if (m_tWaitEnd > 0) { double wk = t0 - m_tWaitEnd; m_sumWork += wk; if (wk > m_maxWork) m_maxWork = wk; m_nFrames++; }
        vrrt::wait_next_vsync();
        m_tWaitEnd = vrrt::now_seconds();
        m_sumWait += m_tWaitEnd - t0;
        vrrt::PoseSet set;
        vrrt::sample_poses(&set, 0.0);
        vrrt::set_last_poses(set);
        m_posePredictionId++;
        if (render) copy_poses(set, render, nr);
        if (game) copy_poses(set, game, ng);
        return VRCompositorError_None;
    }
    EVRCompositorError GetLastPoses(TrackedDevicePose_t *render, uint32_t nr, TrackedDevicePose_t *game, uint32_t ng) override {
        const vrrt::PoseSet &set = vrrt::last_poses();
        if (render) copy_poses(set, render, nr);
        if (game) copy_poses(set, game, ng);
        return VRCompositorError_None;
    }
    EVRCompositorError GetLastPoseForTrackedDeviceIndex(TrackedDeviceIndex_t i, TrackedDevicePose_t *out, TrackedDevicePose_t *game) override {
        if (i >= 3) return VRCompositorError_IndexOutOfRange;
        const vrrt::PoseSet &set = vrrt::last_poses();
        if (out) *out = set.dev[i];
        if (game) *game = set.dev[i];
        return VRCompositorError_None;
    }
    EVRCompositorError GetSubmitTexture(Texture_t *out, bool *flush, EVRCompositorTextureUsage, const Texture_t *in, const VRTextureBounds_t *, EVRSubmitFlags) override {
        if (out && in) *out = *in;
        if (flush) *flush = false;
        return VRCompositorError_None;
    }
    EVRCompositorError SubmitWithArrayIndex(EVREye eye, const Texture_t *tex, uint32_t layer, const VRTextureBounds_t *bounds, EVRSubmitFlags flags) override {
        return submit(eye, tex, layer, bounds, flags);
    }
    EVRCompositorError Submit(EVREye eye, const Texture_t *tex, const VRTextureBounds_t *bounds, EVRSubmitFlags flags) override {
        return submit(eye, tex, 0, bounds, flags);
    }
    EVRCompositorError submit(EVREye eye, const Texture_t *tex, uint32_t layer, const VRTextureBounds_t *bounds, EVRSubmitFlags flags) {
        std::lock_guard<std::mutex> l(m_mu);
        m_submits++;
        if (!tex || !tex->handle) return VRCompositorError_InvalidTexture;
        if (tex->eType != TextureType_Vulkan) {
            if (m_submits <= 3) vrrt::log("Submit: texture type %d not supported (Vulkan only)", (int)tex->eType);
            return VRCompositorError_InvalidTexture;
        }
        const VRVulkanTextureData_t *d = (const VRVulkanTextureData_t *)tex->handle;
        if (flags & Submit_VulkanTextureWithArrayData) layer = ((const VRVulkanTextureArrayData_t *)d)->m_unArrayIndex;
        if (m_submits <= 2) {
            vrrt::log("Submit eye %d: VkImage %#llx %ux%u format %u samples %u layer %u colorSpace %d flags %#x bounds (%.2f %.2f %.2f %.2f)",
                      (int)eye, (unsigned long long)d->m_nImage, d->m_nWidth, d->m_nHeight, d->m_nFormat, d->m_nSampleCount, layer,
                      (int)tex->eColorSpace, (unsigned)flags, bounds ? bounds->uMin : 0.f, bounds ? bounds->vMin : 0.f, bounds ? bounds->uMax : 1.f, bounds ? bounds->vMax : 1.f);
        }
        if (!vrrt::shm_ready()) { m_skipped++; return VRCompositorError_None; }
        if (!m_rb.ok || m_rb.dev != d->m_pDevice) {
            if (m_rb.failures > 3) { m_skipped++; return VRCompositorError_None; }
            if (!m_rb.init(d)) { m_rb.failures++; m_skipped++; return VRCompositorError_None; }
        }
        uint32_t bpp = bytes_per_pixel((VkFormat)d->m_nFormat);
        if (!bpp) {
            static bool said; if (!said) { said = true; vrrt::log("Submit: VkFormat %u is not converted; frames are not streamed", d->m_nFormat); }
            m_skipped++; return VRCompositorError_None;
        }
        if (d->m_nSampleCount > 1) {
            static bool said; if (!said) { said = true; vrrt::log("Submit: multisampled image (%u samples) cannot be read back", d->m_nSampleCount); }
            m_skipped++; return VRCompositorError_None;
        }
        // Bounds select a rectangle; vMin > vMax means the image is upside down.
        float u0 = 0, u1 = 1, v0 = 0, v1 = 1;
        if (bounds) { u0 = bounds->uMin; u1 = bounds->uMax; v0 = bounds->vMin; v1 = bounds->vMax; }
        {   // report every change of the submitted rectangle (dynamic resolution)
            static float last[2][4] = { { -1, -1, -1, -1 }, { -1, -1, -1, -1 } };
            static int changes;
            const int ei = eye == Eye_Left ? 0 : 1;
            if ((last[ei][0] != u0 || last[ei][1] != v0 || last[ei][2] != u1 || last[ei][3] != v1) && changes < 40) {
                changes++;
                vrrt::log("Submit eye %d bounds (%.3f %.3f %.3f %.3f) of %ux%u flags %#x", ei, u0, v0, u1, v1,
                          d->m_nWidth, d->m_nHeight, (unsigned)flags);
                last[ei][0] = u0; last[ei][1] = v0; last[ei][2] = u1; last[ei][3] = v1;
            }
        }
        bool flip = v0 > v1;
        if (flip) std::swap(v0, v1);
        if (u0 > u1) std::swap(u0, u1);
        {   // the eye's own frustum inside the union the game rendered (vr_runtime eye_crop)
            float cu0, cu1, cv0, cv1;
            vrrt::eye_crop(eye == Eye_Left ? 0 : 1, &cu0, &cu1, &cv0, &cv1);
            if (flip) { float t0 = 1.0f - cv1, t1 = 1.0f - cv0; cv0 = t0; cv1 = t1; }   // row 0 is the bottom
            const float bu = u1 - u0, bv = v1 - v0;
            u1 = u0 + cu1 * bu; u0 = u0 + cu0 * bu;
            v1 = v0 + cv1 * bv; v0 = v0 + cv0 * bv;
        }
        uint32_t x = (uint32_t)lroundf(u0 * d->m_nWidth), x1 = (uint32_t)lroundf(u1 * d->m_nWidth);
        uint32_t y = (uint32_t)lroundf(v0 * d->m_nHeight), y1 = (uint32_t)lroundf(v1 * d->m_nHeight);
        x1 = std::min(x1, d->m_nWidth); y1 = std::min(y1, d->m_nHeight);
        if (x1 <= x || y1 <= y) return VRCompositorError_InvalidBounds;
        const int e = eye == Eye_Left ? 0 : 1;
        if (m_async) return submit_async(e, d, x, y, x1 - x, y1 - y, bpp, layer, flip);
        const double tr0 = vrrt::now_seconds();
        const bool copied = m_rb.copy(e, (VkImage)d->m_nImage, x, y, x1 - x, y1 - y, (VkFormat)d->m_nFormat, bpp, layer);
        m_sumReadback += vrrt::now_seconds() - tr0; m_nReadback++;
        if (!copied) {
            m_rb.failures++; m_skipped++;
            if (m_rb.failures <= 3) vrrt::log("Submit: readback of eye %d failed", e);
            return VRCompositorError_None;
        }
        m_eyeDone[e] = true; m_eyeFlip[e] = flip;
        if (e == 1 && m_eyeDone[0]) publish();
        return VRCompositorError_None;
    }
    EVRCompositorError submit_async(int e, const VRVulkanTextureData_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    uint32_t bpp, uint32_t layer, bool flip) {
        Readback::Slot &sl = m_rb.slot[m_slot];
        const double tr0 = vrrt::now_seconds();
        if (e == 0) {
            int st; { std::lock_guard<std::mutex> l(m_wmu); st = sl.state; }
            if (st == 2) { m_skipped++; return VRCompositorError_None; }   // worker still busy with it: drop this frame
            sl.state = 0;
        } else if (sl.state != 1) {
            return VRCompositorError_None;   // eye 0 was dropped or failed
        }
        if (!m_rb.record(sl, e, (VkImage)d->m_nImage, x, y, w, h, (VkFormat)d->m_nFormat, bpp, layer)) {
            m_rb.failures++; m_skipped++; sl.state = 0;
            if (m_rb.failures <= 3) vrrt::log("Submit: readback of eye %d failed", e);
            return VRCompositorError_None;
        }
        m_sumReadback += vrrt::now_seconds() - tr0; m_nReadback++;
        sl.flip[e] = flip;
        if (e == 0) { sl.state = 1; return VRCompositorError_None; }
        const vrrt::PoseSet &set = vrrt::last_poses();
        memcpy(sl.head, set.headTransform, sizeof sl.head);
        sl.visionTimestamp = set.visionTimestamp;
        {
            std::lock_guard<std::mutex> l(m_wmu);
            sl.state = 2;
            m_pending.push_back(m_slot);
            if (!m_worker.joinable()) m_worker = std::thread([this] { worker(); });
        }
        m_wcv.notify_one();
        m_slot ^= 1;
        return VRCompositorError_None;
    }
    void worker() {
        pthread_setname_np("vrrt-readback");
        for (;;) {
            int idx;
            {
                std::unique_lock<std::mutex> l(m_wmu);
                m_wcv.wait(l, [this] { return !m_pending.empty(); });
                idx = m_pending.front(); m_pending.pop_front();
            }
            Readback::Slot &sl = m_rb.slot[idx];
            if (m_rb.wait(sl)) publish_from(sl.eye[0], sl.eye[1], sl.flip, sl.head, sl.visionTimestamp, true);
            else { m_skipped++; vrrt::log("readback: copy did not finish in 2 s"); }
            std::lock_guard<std::mutex> l(m_wmu);
            sl.state = 0;
        }
    }
    void publish() {
        const vrrt::PoseSet &set = vrrt::last_poses();
        publish_from(m_rb.eye[0], m_rb.eye[1], m_eyeFlip, set.headTransform, set.visionTimestamp, set.fresh);
        m_eyeDone[0] = m_eyeDone[1] = false;
    }
    void publish_from(const Readback::Staging &L, const Readback::Staging &R, const bool *flips, const float *head, double visionTs, bool) {
        const uint32_t ew = std::max(L.w, R.w), h = std::max(L.h, R.h), w = ew * 2;
        uint32_t idx;
        uint8_t *dst = vrrt::frame_begin(w, h, &idx);
        if (!dst) { m_skipped++; return; }
        uint32_t fmt = 28;
        const double tc0 = vrrt::now_seconds();
        convert_eye(L, dst, w * 4, 0, flips[0], &fmt, ew, h);
        convert_eye(R, dst, w * 4, ew, flips[1], &fmt, ew, h);
        m_sumConvert += vrrt::now_seconds() - tc0; m_nConvert++;
        vrrt::frame_publish(idx, w, h, fmt, head, visionTs);
        m_published++;
        const vrrt::PoseSet &set = vrrt::last_poses();
        double now = vrrt::now_seconds();
        if (now - m_lastReport > 5.0) {
            m_lastReport = now;
            vrrt::log("compositor: %u submits, %u eye pairs published (%ux%u, format %u), %u skipped, headset %s%s",
                      m_submits, m_published, w, h, fmt, m_skipped, set.fresh ? "tracking" : "absent", m_async ? ", async readback" : "");
            if (m_nFrames) {
                vrrt::log("frame timing: game work %.1f ms avg (max %.1f), vsync wait %.1f ms, readback %.2f ms/eye (game thread), convert %.2f ms/pair",
                          1000.0 * m_sumWork / m_nFrames, 1000.0 * m_maxWork, 1000.0 * m_sumWait / m_nFrames,
                          m_nReadback ? 1000.0 * m_sumReadback / m_nReadback : 0.0, m_nConvert ? 1000.0 * m_sumConvert / m_nConvert : 0.0);
                m_sumWork = m_sumWait = m_sumReadback = m_sumConvert = m_maxWork = 0; m_nFrames = m_nReadback = m_nConvert = 0;
            }
        }
    }
    void ClearLastSubmittedFrame() override {}
    void PostPresentHandoff() override {}
    bool GetFrameTiming(Compositor_FrameTiming *t, uint32_t) override {
        if (!t) return false;
        uint32_t size = t->m_nSize;
        memset(t, 0, std::min<uint32_t>(size, sizeof *t));
        t->m_nSize = size;
        t->m_nFrameIndex = (uint32_t)vrrt::frame_counter();
        t->m_nNumFramePresents = 1;
        t->m_flSystemTimeInSeconds = vrrt::now_seconds();
        t->m_flClientFrameIntervalMs = (float)(vrrt::display_period() * 1000.0);
        t->m_nReprojectionFlags = 0;
        t->m_HmdPose = vrrt::last_poses().dev[0];
        return true;
    }
    uint32_t GetFrameTimings(Compositor_FrameTiming *t, uint32_t n) override { if (n && t) { GetFrameTiming(t, 0); return 1; } return 0; }
    float GetFrameTimeRemaining() override { float r = (float)(vrrt::display_period() - vrrt::seconds_since_vsync()); return r > 0 ? r : 0.f; }
    void GetCumulativeStats(Compositor_CumulativeStats *s, uint32_t n) override { if (s) memset(s, 0, n); if (s && n >= sizeof(uint32_t) * 2) s->m_nNumFramePresents = m_published; }
    void FadeToColor(float, float r, float g, float b, float a, bool) override { m_fade = { r, g, b, a }; }
    HmdColor_t GetCurrentFadeColor(bool) override { return m_fade; }
    void FadeGrid(float, bool) override {}
    float GetCurrentGridAlpha() override { return 0.f; }
    EVRCompositorError SetSkyboxOverride(const Texture_t *, uint32_t) override { return VRCompositorError_None; }
    void ClearSkyboxOverride() override {}
    void CompositorBringToFront() override {}
    void CompositorGoToBack() override {}
    void CompositorQuit() override {}
    bool IsFullscreen() override { return true; }
    uint32_t GetCurrentSceneFocusProcess() override { return (uint32_t)getpid(); }
    uint32_t GetLastFrameRenderer() override { return (uint32_t)getpid(); }
    bool CanRenderScene() override { return true; }
    void ShowMirrorWindow() override {}
    void HideMirrorWindow() override {}
    bool IsMirrorWindowVisible() override { return false; }
    void CompositorDumpImages() override {}
    bool ShouldAppRenderWithLowResources() override { return false; }
    void ForceInterleavedReprojectionOn(bool) override {}
    void ForceReconnectProcess() override {}
    void SuspendRendering(bool) override {}
    uint32_t GetVulkanInstanceExtensionsRequired(char *buf, uint32_t cap) override {
        static const char ext[] = "VK_KHR_get_physical_device_properties2";
        if (buf && cap) { strncpy(buf, ext, cap); buf[cap - 1] = 0; }
        return sizeof ext;
    }
    uint32_t GetVulkanDeviceExtensionsRequired(VkPhysicalDevice_T *, char *buf, uint32_t cap) override {
        if (buf && cap) buf[0] = 0;
        return 1;
    }
    void SetExplicitTimingMode(EVRCompositorTimingMode m) override { m_timing = m; }
    EVRCompositorError SubmitExplicitTimingData() override { return VRCompositorError_None; }
    bool IsMotionSmoothingEnabled() override { return false; }
    bool IsMotionSmoothingSupported() override { return false; }
    bool IsCurrentSceneFocusAppLoading() override { return false; }
    EVRCompositorError SetStageOverride_Async(const char *, const HmdMatrix34_t *, const Compositor_StageRenderSettings *, uint32_t) override { return VRCompositorError_None; }
    void ClearStageOverride() override {}
    bool GetCompositorBenchmarkResults(Compositor_BenchmarkResults *, uint32_t) override { return false; }
    EVRCompositorError GetLastPosePredictionIDs(uint32_t *r, uint32_t *g) override { if (r) *r = m_posePredictionId; if (g) *g = m_posePredictionId; return VRCompositorError_None; }
    EVRCompositorError GetPosesForFrame(uint32_t, TrackedDevicePose_t *arr, uint32_t n) override { copy_poses(vrrt::last_poses(), arr, n); return VRCompositorError_None; }
};

Compositor g_compositor;

} // namespace

vr::IVRCompositor *vrrt_compositor() { return &g_compositor; }
