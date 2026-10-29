// IVRCompositor_029: pose hand-out paced to the virtual display, and Submit:
// the game's Vulkan eye images are read back and published to the Mac encoder
// through the shared frame file.
#include "gen_stubs.h"
#include "vr_runtime.h"
#include "json_mini.h"
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
#include <vulkan/vulkan_core.h>
#include <atomic>

extern "C" void kl_vulkan_async_begin_frame(void);

namespace {

using namespace vr;

// A small grid distinguishes a flat/black submitted picture from transport
// loss without saving screenshots or scanning every pixel on the render path.
struct PixelSummary {
    double mean = 0, deviation = 0;
    unsigned minimum = 255, maximum = 0;
    uint64_t hash = 14695981039346656037ull;
};
PixelSummary pixel_summary(const uint8_t *bgra, uint32_t width, uint32_t height, int eye) {
    PixelSummary out;
    double squares = 0;
    for (uint32_t y = 0; y < 16; y++) for (uint32_t x = 0; x < 16; x++) {
        const uint32_t px = (2 * x + 1) * (width / 2) / 32 + eye * (width / 2);
        const uint32_t py = (2 * y + 1) * height / 32;
        const uint8_t *p = bgra + ((size_t)py * width + px) * 4;
        unsigned value = (unsigned(p[0]) + p[1] + p[2]) / 3;
        out.minimum = std::min(out.minimum, value); out.maximum = std::max(out.maximum, value);
        out.mean += value; squares += value * value;
        for (int channel = 0; channel < 3; channel++) { out.hash ^= p[channel]; out.hash *= 1099511628211ull; }
    }
    out.mean /= 256;
    out.deviation = std::sqrt(std::max(0.0, squares / 256 - out.mean * out.mean));
    return out;
}

// Source 2 clears unrendered eyes to magenta while the first materials compile.
// Check only during startup, on the readback worker; never wait for a material
// or change magenta in an already running scene. Verify every pixel after the
// cheap grid check so even a small authored logo makes this a real picture.
bool startup_clear(const uint8_t *bgra, uint32_t width, uint32_t height, int eye) {
    auto pink = [](const uint8_t *p) { return p[0] >= 250 && p[1] <= 5 && p[2] >= 250; };
    uint32_t ew = width / 2;
    for (uint32_t y = 0; y < 16; y++) for (uint32_t x = 0; x < 16; x++) {
        uint32_t px = (2*x+1)*ew/32 + eye*ew, py = (2*y+1)*height/32;
        if (!pink(bgra + ((size_t)py*width + px)*4)) return false;
    }
    for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < ew; x++)
        if (!pink(bgra + ((size_t)y*width + eye*ew + x)*4)) return false;
    return true;
}

// ---- Vulkan readback of a submitted eye image ----
struct Readback {
    VkInstance inst = nullptr; VkDevice dev = nullptr; VkPhysicalDevice phys = nullptr; VkQueue queue = nullptr; uint32_t qfi = 0;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkCreateCommandPool pCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool pDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers pAllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer pBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer pEndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer pResetCommandBuffer = nullptr;
    PFN_vkCmdCopyImageToBuffer pCmdCopyImageToBuffer = nullptr;
    PFN_vkCmdPipelineBarrier pCmdPipelineBarrier = nullptr;
    PFN_vkQueueSubmit pQueueSubmit = nullptr;
    PFN_vkCreateFence pCreateFence = nullptr;
    PFN_vkDestroyFence pDestroyFence = nullptr;
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
        VkFence fence[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        unsigned submitted = 0;
        bool publish = false;
        bool flipX[2] = { false, false };
        bool flip[2] = { false, false };
        float head[16] = {};
        double visionTimestamp = 0;
        int state = 0;                       // 0 free, 1 recording, 2 worker owns, 3 quarantined
    };
    Slot slot[2];
    bool ok = false, syncUsable = true;
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

    void release() {
        if (!dev || !pDestroyCommandPool || !syncUsable) return;
        // A timed-out queue can still be using every object reachable from
        // that command pool. Keep those allocations until process teardown.
        for (const Slot &sl : slot) if (sl.state == 3) return;
        if (pool) pDestroyCommandPool(dev, pool, nullptr);
        pool = VK_NULL_HANDLE; cmd = VK_NULL_HANDLE;
        if (fence) pDestroyFence(dev, fence, nullptr);
        fence = VK_NULL_HANDLE;
        auto free_staging = [this](Staging &s) {
            if (s.map) pUnmapMemory(dev, s.mem);
            if (s.buf) pDestroyBuffer(dev, s.buf, nullptr);
            if (s.mem) pFreeMemory(dev, s.mem, nullptr);
            s = Staging();
        };
        for (Staging &s : eye) free_staging(s);
        for (Slot &sl : slot) {
            for (VkFence f : sl.fence) if (f) pDestroyFence(dev, f, nullptr);
            for (Staging &s : sl.eye) free_staging(s);
            sl = Slot();
        }
        ok = false;
    }

    bool init(const VRVulkanTextureData_t *d) {
        release();
        inst = d->m_pInstance; dev = d->m_pDevice; phys = d->m_pPhysicalDevice; queue = d->m_pQueue; qfi = d->m_nQueueFamilyIndex;
        PFN_vkGetInstanceProcAddr gi = gipa();
        if (!gi) return false;
        gdpa = (PFN_vkGetDeviceProcAddr)gi(inst, "vkGetDeviceProcAddr");
        pGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)gi(inst, "vkGetPhysicalDeviceMemoryProperties");
        if (!gdpa || !pGetPhysicalDeviceMemoryProperties) { vrrt::log("readback: instance entry points missing"); return false; }
#define D(n) p##n = (PFN_vk##n)gdpa(dev, "vk" #n); if (!p##n) { vrrt::log("readback: vk" #n " missing"); return false; }
        D(CreateCommandPool) D(DestroyCommandPool) D(AllocateCommandBuffers) D(BeginCommandBuffer) D(EndCommandBuffer) D(ResetCommandBuffer)
        D(CmdCopyImageToBuffer) D(CmdPipelineBarrier) D(QueueSubmit) D(CreateFence) D(DestroyFence) D(WaitForFences) D(ResetFences)
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
            for (VkFence &f : sl.fence)
                if (pCreateFence(dev, &fci, nullptr, &f) != VK_SUCCESS) { vrrt::log("readback: slot fence failed"); return false; }
        }
        ok = true;
        vrrt::log("readback: Vulkan device %p queue %p (family %u) ready", (void *)dev, (void *)queue, qfi);
        return true;
    }

    bool ensure_staging(Staging &s, size_t bytes, uint32_t w, uint32_t h, VkFormat fmt) {
        if (s.ready && s.cap >= bytes) { s.w = w; s.h = h; s.fmt = fmt; return true; }
        if (s.map) pUnmapMemory(dev, s.mem);
        if (s.buf) pDestroyBuffer(dev, s.buf, nullptr);
        if (s.mem) pFreeMemory(dev, s.mem, nullptr);
        s = Staging();
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

    // Each eye has its own fence: an incomplete pair still has to finish
    // copying before its staging buffer and command buffer can be reused.
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
        VkFence f = sl.fence[eyeIndex];
        if (pResetFences(dev, 1, &f) != VK_SUCCESS) return false;
        if (pQueueSubmit(queue, 1, &si, f) != VK_SUCCESS) return false;
        sl.submitted |= 1u << eyeIndex;
        return true;
    }
    // Worker side: wait for a slot's copies.
    bool wait(Slot &sl) {
        for (int e = 0; e < 2; e++)
            if ((sl.submitted & (1u << e)) && pWaitForFences(dev, 1, &sl.fence[e], VK_TRUE, 2000000000ull) != VK_SUCCESS) return false;
        for (Staging &s : sl.eye)
            if (sl.publish && !s.coherent) { VkMappedMemoryRange r = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, s.mem, 0, VK_WHOLE_SIZE }; pInvalidateMappedMemoryRanges(dev, 1, &r); }
        return true;
    }

    // Copy the eye image into the staging buffer and wait for it.
    bool copy(int eyeIndex, VkImage image, uint32_t x, uint32_t y, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp, uint32_t layer) {
        if (!syncUsable) return false;
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
        VkImageMemoryBarrier after = bar;
        after.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        after.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        VkMemoryBarrier host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
        pCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                            0, 1, &host, 0, nullptr, 1, &after);
        if (pEndCommandBuffer(cmd) != VK_SUCCESS) return false;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd, 0, nullptr };
        pResetFences(dev, 1, &fence);
        if (pQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return false;
        if (pWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull) != VK_SUCCESS) { syncUsable = false; return false; }
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
                        uint32_t dw, uint32_t dh, bool flipX = false) {
    const uint32_t w = s.w, h = s.h;
    const uint8_t *src = (const uint8_t *)s.map;
    uint32_t bpp = bytes_per_pixel(s.fmt);
    const bool scale = dw != w || dh != h || flipX;
    std::vector<uint8_t> rowbuf(scale ? (size_t)w * 4 : 0);
    std::vector<uint32_t> xmap(scale ? dw : 0);
    for (uint32_t c = 0; scale && c < dw; c++) {
        uint32_t x = std::min(w - 1, (uint32_t)(((uint64_t)c * w + w / 2) / dw));
        xmap[c] = flipX ? w - 1 - x : x;
    }
    for (uint32_t orow = 0; orow < (scale ? dh : h); orow++) {
        const uint32_t row = scale ? std::min(h - 1, (uint32_t)(((uint64_t)orow * h + h / 2) / dh)) : orow;
        const uint32_t srow = flipY ? h - 1 - row : row;
        const uint8_t *sp = src + (size_t)srow * w * bpp;
        uint8_t *const out = dst + (size_t)orow * dstRowBytes + (size_t)x0 * 4;
        uint8_t *dp = scale ? rowbuf.data() : out;
        switch (s.fmt) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
            for (uint32_t c = 0; c < w; c++) { dp[c*4] = sp[c*4+2]; dp[c*4+1] = sp[c*4+1]; dp[c*4+2] = sp[c*4]; dp[c*4+3] = sp[c*4+3]; }
            *outFormat = 87; break;
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
            for (uint32_t c = 0; c < w; c++) { dp[c * 4 + 0] = half_to_u8(p[c * 4 + 2]); dp[c * 4 + 1] = half_to_u8(p[c * 4 + 1]); dp[c * 4 + 2] = half_to_u8(p[c * 4 + 0]); dp[c * 4 + 3] = 255; }
            *outFormat = 87; break;
        }
        case VK_FORMAT_R16G16B16A16_UNORM: {
            const uint16_t *p = (const uint16_t *)sp;
            for (uint32_t c = 0; c < w; c++) { dp[c * 4 + 0] = p[c * 4 + 2] >> 8; dp[c * 4 + 1] = p[c * 4 + 1] >> 8; dp[c * 4 + 2] = p[c * 4 + 0] >> 8; dp[c * 4 + 3] = 255; }
            *outFormat = 87; break;
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
    int m_slot = -1;
    unsigned m_submitted = 0;
    bool m_dropFrame = false;
    bool m_stopping = false;
    vrrt::PoseSet m_framePose = {};
    std::mutex m_wmu;
    std::condition_variable m_wcv;
    std::thread m_worker;
    std::deque<int> m_pending;               // slots in flight, in order
    bool m_async = getenv("VERANTYX_SYNC_READBACK") == nullptr || strcmp(getenv("VERANTYX_SYNC_READBACK"), "1") != 0;
    bool m_eyeDone[2] = { false, false };
    bool m_eyeFlip[2] = { false, false };
    bool m_eyeFlipX[2] = { false, false };
    std::atomic<unsigned> m_submits{0}, m_published{0}, m_skipped{0};
    bool m_startup = true;
    unsigned m_startupClears = 0;
    double m_lastReport = 0;
    double m_lastBoundsError = 0;
    uint32_t m_boundsErrors = 0;
    uint32_t m_posePredictionId = 0;
    std::mutex m_mu;
    std::mutex m_statsMu;

    ~Compositor() { shutdown(); }
    void resume() {
        std::lock_guard<std::mutex> l(m_mu);
        std::lock_guard<std::mutex> wl(m_wmu);
        if (!m_stopping || m_worker.joinable()) return;
        // A previous OpenVR lifetime may have ended with the application
        // destroying its VkDevice. Never carry its borrowed handles into Init.
        m_rb = Readback();
        m_stopping = false; m_submitted = 0; m_dropFrame = false;
        m_startup = true; m_startupClears = 0;
        m_eyeDone[0] = m_eyeDone[1] = false;
    }
    // A missing eye is retired without publishing. Its submitted copy still
    // belongs to the GPU, so it must go through the worker like a full pair.
    void retire_slot(bool publish) {
        if (m_slot < 0) return;
        std::lock_guard<std::mutex> l(m_wmu);
        Readback::Slot &sl = m_rb.slot[m_slot];
        if (sl.submitted) {
            sl.publish = publish; sl.state = 2;
            m_pending.push_back(m_slot);
            if (!m_worker.joinable()) m_worker = std::thread([this] { worker(); });
            m_wcv.notify_one();
        } else sl.state = 0;
        m_slot = -1;
    }
    void shutdown() {
        {
            std::lock_guard<std::mutex> l(m_mu);
            retire_slot(false);
            std::lock_guard<std::mutex> wl(m_wmu);
            m_stopping = true;
        }
        m_wcv.notify_one();
        if (m_worker.joinable()) m_worker.join();
        m_rb.release(); // OpenVR cleanup precedes the application's VkDevice destruction.
    }

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
        std::lock_guard<std::mutex> frameLock(m_mu);
        retire_slot(false);
        m_submitted = 0; m_dropFrame = false;
        m_eyeDone[0] = m_eyeDone[1] = false;
        const double t0 = vrrt::now_seconds();
        { std::lock_guard<std::mutex> l(m_statsMu);
          if (m_tWaitEnd > 0) { double wk = t0 - m_tWaitEnd; m_sumWork += wk; if (wk > m_maxWork) m_maxWork = wk; m_nFrames++; } }
        vrrt::wait_next_vsync();
        kl_vulkan_async_begin_frame();
        { std::lock_guard<std::mutex> l(m_statsMu);
          m_tWaitEnd = vrrt::now_seconds(); m_sumWait += m_tWaitEnd - t0; }
        vrrt::PoseSet set;
        vrrt::sample_poses(&set, 0.0);
        vrrt::set_last_poses(set);
        vrrt::report_inputs(set);
        m_framePose = set;
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
    EVRCompositorError invalid_bounds(EVREye eye, const VRVulkanTextureData_t *d, const VRTextureBounds_t *bounds, EVRSubmitFlags flags, const char *reason) {
        m_skipped++; m_boundsErrors++;
        const double now = vrrt::now_seconds();
        if (m_boundsErrors <= 4 || now - m_lastBoundsError >= 5) {
            m_lastBoundsError = now;
            vrrt::log("Submit rejected bounds: %s eye=%d image=%#llx %ux%u format=%u flags=%#x bounds=(%.9g %.9g %.9g %.9g) errors=%u",
                      reason, (int)eye, (unsigned long long)d->m_nImage, d->m_nWidth, d->m_nHeight, d->m_nFormat, (unsigned)flags,
                      bounds ? bounds->uMin : 0.f, bounds ? bounds->vMin : 0.f, bounds ? bounds->uMax : 1.f, bounds ? bounds->vMax : 1.f, m_boundsErrors);
        }
        return VRCompositorError_InvalidBounds;
    }
    EVRCompositorError submit(EVREye eye, const Texture_t *tex, uint32_t layer, const VRTextureBounds_t *bounds, EVRSubmitFlags flags) {
        std::lock_guard<std::mutex> l(m_mu);
        m_submits++;
        if (m_stopping) return VRCompositorError_RequestFailed;
        if (eye != Eye_Left && eye != Eye_Right) return VRCompositorError_IndexOutOfRange;
        if (!tex || !tex->handle) return VRCompositorError_InvalidTexture;
        if (tex->eType != TextureType_Vulkan) {
            if (m_submits <= 3) vrrt::log("Submit: texture type %d not supported (Vulkan only)", (int)tex->eType);
            return VRCompositorError_InvalidTexture;
        }
        const VRVulkanTextureData_t *d = (const VRVulkanTextureData_t *)tex->handle;
        if (!d->m_nImage || !d->m_nWidth || !d->m_nHeight || d->m_nWidth > 16384 || d->m_nHeight > 16384)
            return VRCompositorError_InvalidTexture;
        if (flags & Submit_VulkanTextureWithArrayData) layer = ((const VRVulkanTextureArrayData_t *)d)->m_unArrayIndex;
        if (m_submits <= 2) {
            vrrt::log("Submit eye %d: VkImage %#llx %ux%u format %u samples %u layer %u colorSpace %d flags %#x bounds (%.2f %.2f %.2f %.2f)",
                      (int)eye, (unsigned long long)d->m_nImage, d->m_nWidth, d->m_nHeight, d->m_nFormat, d->m_nSampleCount, layer,
                      (int)tex->eColorSpace, (unsigned)flags, bounds ? bounds->uMin : 0.f, bounds ? bounds->vMin : 0.f, bounds ? bounds->uMax : 1.f, bounds ? bounds->vMax : 1.f);
        }
        if (!vrrt::shm_ready()) { m_skipped++; return VRCompositorError_None; }
        if (m_rb.ok && (m_rb.dev != d->m_pDevice || m_rb.queue != d->m_pQueue)) return VRCompositorError_TextureIsOnWrongDevice;
        if (!m_rb.ok) {
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
        constexpr float edgeRoundoff = 1e-6f; // much less than a pixel at the largest supported eye size
        for (float value : {u0, u1, v0, v1}) if (!std::isfinite(value) || value < -edgeRoundoff || value > 1 + edgeRoundoff)
            return invalid_bounds(eye, d, bounds, flags, "outside normalized texture");
        u0 = std::clamp(u0, 0.f, 1.f); u1 = std::clamp(u1, 0.f, 1.f);
        v0 = std::clamp(v0, 0.f, 1.f); v1 = std::clamp(v1, 0.f, 1.f);
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
        bool flipX = u0 > u1;
        if (flip) std::swap(v0, v1);
        if (u0 > u1) std::swap(u0, u1);
        {   // the eye's own frustum inside the union the game rendered (vr_runtime eye_crop)
            float cu0, cu1, cv0, cv1;
            vrrt::eye_crop(eye == Eye_Left ? 0 : 1, &cu0, &cu1, &cv0, &cv1);
            if (flip) { float t0 = 1.0f - cv1, t1 = 1.0f - cv0; cv0 = t0; cv1 = t1; }   // row 0 is the bottom
            if (flipX) { float t0 = 1.0f - cu1, t1 = 1.0f - cu0; cu0 = t0; cu1 = t1; }
            const float bu = u1 - u0, bv = v1 - v0;
            u1 = u0 + cu1 * bu; u0 = u0 + cu0 * bu;
            v1 = v0 + cv1 * bv; v0 = v0 + cv0 * bv;
        }
        uint32_t x = (uint32_t)lroundf(u0 * d->m_nWidth), x1 = (uint32_t)lroundf(u1 * d->m_nWidth);
        uint32_t y = (uint32_t)lroundf(v0 * d->m_nHeight), y1 = (uint32_t)lroundf(v1 * d->m_nHeight);
        x1 = std::min(x1, d->m_nWidth); y1 = std::min(y1, d->m_nHeight);
        if (x1 <= x || y1 <= y) return invalid_bounds(eye, d, bounds, flags, "empty pixel rectangle after eye crop");
        const int e = eye == Eye_Left ? 0 : 1;
        if (m_submitted & (1u << e)) return VRCompositorError_AlreadySubmitted;
        m_submitted |= 1u << e;
        if (m_async) return submit_async(e, d, x, y, x1 - x, y1 - y, bpp, layer, flip, flipX);
        const double tr0 = vrrt::now_seconds();
        const bool copied = m_rb.copy(e, (VkImage)d->m_nImage, x, y, x1 - x, y1 - y, (VkFormat)d->m_nFormat, bpp, layer);
        { std::lock_guard<std::mutex> l(m_statsMu); m_sumReadback += vrrt::now_seconds() - tr0; m_nReadback++; }
        if (!copied) {
            m_rb.failures++; m_skipped++;
            if (m_rb.failures <= 3) vrrt::log("Submit: readback of eye %d failed", e);
            return VRCompositorError_None;
        }
        m_eyeDone[e] = true; m_eyeFlip[e] = flip; m_eyeFlipX[e] = flipX;
        if (m_eyeDone[0] && m_eyeDone[1]) publish();
        return VRCompositorError_None;
    }
    EVRCompositorError submit_async(int e, const VRVulkanTextureData_t *d, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                    uint32_t bpp, uint32_t layer, bool flip, bool flipX) {
        if (m_dropFrame) return VRCompositorError_None;
        if (m_slot < 0) {
            std::lock_guard<std::mutex> l(m_wmu);
            for (int i = 0; i < 2; i++) if (m_rb.slot[i].state == 0) {
                m_slot = i; m_rb.slot[i].state = 1; m_rb.slot[i].submitted = 0; break;
            }
            if (m_slot < 0) { m_skipped++; m_dropFrame = true; return VRCompositorError_None; }
        }
        Readback::Slot &sl = m_rb.slot[m_slot];
        const double tr0 = vrrt::now_seconds();
        if (!m_rb.record(sl, e, (VkImage)d->m_nImage, x, y, w, h, (VkFormat)d->m_nFormat, bpp, layer)) {
            m_rb.failures++; m_skipped++; m_dropFrame = true;
            retire_slot(false);
            if (m_rb.failures <= 3) vrrt::log("Submit: readback of eye %d failed", e);
            return VRCompositorError_None;
        }
        { std::lock_guard<std::mutex> l(m_statsMu); m_sumReadback += vrrt::now_seconds() - tr0; m_nReadback++; }
        sl.flip[e] = flip; sl.flipX[e] = flipX;
        memcpy(sl.head, m_framePose.headTransform, sizeof sl.head);
        sl.visionTimestamp = m_framePose.visionTimestamp;
        if (sl.submitted == 3) retire_slot(true);
        return VRCompositorError_None;
    }
    void worker() {
        pthread_setname_np("vrrt-readback");
        for (;;) {
            int idx;
            {
                std::unique_lock<std::mutex> l(m_wmu);
                m_wcv.wait(l, [this] { return m_stopping || !m_pending.empty(); });
                if (m_pending.empty()) return;
                idx = m_pending.front(); m_pending.pop_front();
            }
            Readback::Slot &sl = m_rb.slot[idx];
            bool completed = m_rb.wait(sl);
            if (completed && sl.publish) publish_from(sl.eye[0], sl.eye[1], sl.flip, sl.flipX, sl.head, sl.visionTimestamp);
            else if (!completed) { m_skipped++; vrrt::log("readback: copy did not finish in 2 s; slot quarantined until exit"); }
            std::lock_guard<std::mutex> l(m_wmu);
            sl.state = completed ? 0 : 3;
        }
    }
    void publish() {
        publish_from(m_rb.eye[0], m_rb.eye[1], m_eyeFlip, m_eyeFlipX, m_framePose.headTransform, m_framePose.visionTimestamp);
        m_eyeDone[0] = m_eyeDone[1] = false;
    }
    void publish_from(const Readback::Staging &L, const Readback::Staging &R, const bool *flips, const bool *flipX, const float *head, double visionTs) {
        const uint32_t ew = std::max(L.w, R.w), h = std::max(L.h, R.h), w = ew * 2;
        uint32_t idx;
        uint8_t *dst = vrrt::frame_begin(w, h, &idx);
        if (!dst) { m_skipped++; return; }
        uint32_t fmt = 87;
        const double tc0 = vrrt::now_seconds();
        convert_eye(L, dst, w * 4, 0, flips[0], &fmt, ew, h, flipX[0]);
        convert_eye(R, dst, w * 4, ew, flips[1], &fmt, ew, h, flipX[1]);
        if (m_startup) {
            if (startup_clear(dst, w, h, 0) || startup_clear(dst, w, h, 1)) {
                memset(dst, 0, (size_t)w*h*4);
                if (++m_startupClears == 1) vrrt::log("startup: clear-only eyes replaced with black while materials compile");
            } else if (pixel_summary(dst, w, h, 0).maximum > 1 && pixel_summary(dst, w, h, 1).maximum > 1) {
                m_startup = false;
                vrrt::log("startup: first scene pair; %u clear-only pairs replaced", m_startupClears);
            }
        }
        vrrt::frame_publish(idx, w, h, fmt, head, visionTs);
        m_published++;
        std::lock_guard<std::mutex> l(m_statsMu);
        m_sumConvert += vrrt::now_seconds() - tc0; m_nConvert++;
        const vrrt::PoseSet &set = vrrt::last_poses();
        double now = vrrt::now_seconds();
        if (now - m_lastReport > 5.0) {
            m_lastReport = now;
            vrrt::log("compositor: %u submits, %u eye pairs published (%ux%u, format %u), %u skipped, headset %s%s",
                      m_submits.load(), m_published.load(), w, h, fmt, m_skipped.load(), set.fresh ? "tracking" : "absent", m_async ? ", async readback" : "");
            vrrt::log("frame sample: t=%.6f pairs=%u skipped=%u width=%u height=%u tracked=%d",
                      now, m_published.load(), m_skipped.load(), w, h, (int)set.fresh);
            for (int eye = 0; eye < 2; eye++) {
                const auto pixels = pixel_summary(dst, w, h, eye);
                vrrt::log("frame pixels %s: mean=%.1f range=%u-%u deviation=%.1f hash=%016llx",
                          eye ? "right" : "left", pixels.mean, pixels.minimum, pixels.maximum, pixels.deviation,
                          (unsigned long long)pixels.hash);
            }
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
        if (size < sizeof(uint32_t)) return false;
        Compositor_FrameTiming out = {};
        out.m_nSize = size;
        out.m_nFrameIndex = (uint32_t)vrrt::frame_counter();
        out.m_nNumFramePresents = 1;
        out.m_flSystemTimeInSeconds = vrrt::now_seconds();
        out.m_flClientFrameIntervalMs = (float)(vrrt::display_period() * 1000.0);
        out.m_HmdPose = vrrt::last_poses().dev[0];
        memcpy(t, &out, std::min<uint32_t>(size, sizeof out));
        return true;
    }
    uint32_t GetFrameTimings(Compositor_FrameTiming *t, uint32_t n) override { if (n && t) { GetFrameTiming(t, 0); return 1; } return 0; }
    float GetFrameTimeRemaining() override { float r = (float)(vrrt::display_period() - vrrt::seconds_since_vsync()); return r > 0 ? r : 0.f; }
    void GetCumulativeStats(Compositor_CumulativeStats *s, uint32_t n) override { if (s) memset(s, 0, n); if (s && n >= sizeof(uint32_t) * 2) s->m_nNumFramePresents = m_published; }
    void FadeToColor(float seconds, float r, float g, float b, float a, bool background) override {
        std::lock_guard<std::mutex> l(m_mu);
        if (m_fade.r != r || m_fade.g != g || m_fade.b != b || m_fade.a != a)
            vrrt::log("compositor fade requested: %.2fs rgba=(%.2f,%.2f,%.2f,%.2f) background=%d", seconds, r, g, b, a, background);
        m_fade = { r, g, b, a };
    }
    HmdColor_t GetCurrentFadeColor(bool) override { std::lock_guard<std::mutex> l(m_mu); return m_fade; }
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
    EVRCompositorError SetStageOverride_Async(const char *path, const HmdMatrix34_t *transform,
            const Compositor_StageRenderSettings *settings, uint32_t size) override {
        if (!path || !*path || strlen(path) > 4096 || (settings && size != sizeof *settings))
            return VRCompositorError_RequestFailed;
        HmdMatrix34_t matrix; vrrt::identity34(&matrix);
        if (transform) matrix = *transform;
        auto style = settings ? *settings : DefaultStageRenderSettings();
        const float values[] = { style.m_PrimaryColor.r, style.m_PrimaryColor.g, style.m_PrimaryColor.b, style.m_PrimaryColor.a,
            style.m_SecondaryColor.r, style.m_SecondaryColor.g, style.m_SecondaryColor.b, style.m_SecondaryColor.a,
            style.m_flVignetteInnerRadius, style.m_flVignetteOuterRadius, style.m_flFresnelStrength };
        std::string data = "{\"model\":" + json::quote(path) + ",\"transform\":[";
        for (int i = 0; i < 12; i++) {
            float value = matrix.m[i / 4][i % 4];
            if (!std::isfinite(value)) return VRCompositorError_RequestFailed;
            data += (i ? "," : "") + std::to_string(value);
        }
        data += "],\"settings\":[";
        for (int i = 0; i < 11; i++) {
            if (!std::isfinite(values[i])) return VRCompositorError_RequestFailed;
            data += (i ? "," : "") + std::to_string(values[i]);
        }
        static std::atomic<uint64_t> serial{0};
        uint64_t token = ++serial;
        data += "],\"cull\":" + std::to_string(style.m_bBackfaceCulling) + ",\"greyscale\":" +
            std::to_string(style.m_bGreyscale) + ",\"wireframe\":" + std::to_string(style.m_bWireframe) +
            ",\"token\":" + std::to_string(token) + "}";
        if (!vrrt::publish_file("VERANTYX_NATIVE_STAGE_FILE", data)) return VRCompositorError_RequestFailed;
        vrrt::expect_stage_ready(token);
        vrrt::log("loading stage requested: %s (request %llu)", path, (unsigned long long)token);
        return VRCompositorError_None;
    }
    void ClearStageOverride() override {
        vrrt::expect_stage_ready(0);
        vrrt::publish_file("VERANTYX_NATIVE_STAGE_FILE", "");
    }
    bool GetCompositorBenchmarkResults(Compositor_BenchmarkResults *, uint32_t) override { return false; }
    EVRCompositorError GetLastPosePredictionIDs(uint32_t *r, uint32_t *g) override { if (r) *r = m_posePredictionId; if (g) *g = m_posePredictionId; return VRCompositorError_None; }
    EVRCompositorError GetPosesForFrame(uint32_t, TrackedDevicePose_t *arr, uint32_t n) override { copy_poses(vrrt::last_poses(), arr, n); return VRCompositorError_None; }
};

Compositor g_compositor;

} // namespace

vr::IVRCompositor *vrrt_compositor() { return &g_compositor; }
void vrrt_compositor_shutdown() { g_compositor.shutdown(); }
void vrrt_compositor_resume() { g_compositor.resume(); }
