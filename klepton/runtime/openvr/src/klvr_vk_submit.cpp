// IVRCompositor::Submit's Vulkan half: copy the game's eye image into the
// platform's.
//
// SteamVR's contract for a Vulkan submit is that the image is in
// TRANSFER_SRC_OPTIMAL, its rendering has been submitted to the queue named in
// VRVulkanTextureData_t, and the runtime may record its own work on that queue
// to read it (SteamVR's vrclient does exactly that). So the copy is recorded
// here and submitted to the game's queue, which orders it after the game's
// rendering with no semaphore, and the platform then sees a finished picture
// once the queue drains (kl_vulkan_frame_done waits for that on Klepton).
//
// It is a blit rather than a copy because the three things that differ between
// the game's image and the compositor's all fall out of a blit for free: the
// texture bounds (Source 2 may render both eyes side by side into one image),
// a vertical flip (bounds with vMin > vMax), and format (a float or BGRA eye
// buffer into the compositor's RGBA8).
#include "klvr_internal.h"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cmath>
#include <map>

namespace klvr {

namespace {

struct Fns {
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdResolveImage CmdResolveImage;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkCmdClearColorImage CmdClearColorImage;   // optional: KLVR_DEBUG_EYE_COLOR
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer; // optional: KL_CP_DEPTH
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage; // optional: KL_CP_DEPTH
    bool ok = false;
};

Fns load_fns() {
    Fns f;
#define L(n) f.n = (PFN_vk##n)klvr_plat_vk_proc("vk" #n)
    L(CreateCommandPool); L(AllocateCommandBuffers); L(BeginCommandBuffer); L(EndCommandBuffer);
    L(CmdPipelineBarrier); L(CmdBlitImage); L(CmdResolveImage); L(QueueSubmit); L(CreateFence);
    L(WaitForFences); L(ResetFences); L(ResetCommandBuffer); L(CmdClearColorImage);
    L(CmdCopyImageToBuffer); L(CmdCopyBufferToImage);
#undef L
    f.ok = f.CreateCommandPool && f.AllocateCommandBuffers && f.BeginCommandBuffer && f.EndCommandBuffer &&
           f.CmdPipelineBarrier && f.CmdBlitImage && f.QueueSubmit && f.CreateFence && f.WaitForFences &&
           f.ResetFences && f.ResetCommandBuffer;
    return f;
}

struct Slot { VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE; bool used = false; };

struct DeviceState {
    VkCommandPool pool = VK_NULL_HANDLE;
    std::map<int, Slot> slots;   // key: stage * 2 + eye
};

std::mutex g_mu;
Fns g_fns;
bool g_fns_loaded = false;
std::map<std::pair<VkDevice, uint32_t>, DeviceState> g_devices;

bool is_srgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

VkImageMemoryBarrier barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return b;
}

// KL_CP_DEPTH's Mac check: KLVR_DEPTH_DUMP=<file.pfm> writes eye 0's copied depth (raw values, float PFM, row 0
// at the top) once, at depth copy KLVR_DEPTH_DUMP_AT (default 1500), read back when the copy's fence is next waited.
struct DepthDump { int stage = -1; uint32_t w = 0, h = 0, pitch = 0; void *mapped = nullptr, *cmapped = nullptr; bool done = true; int n = 0; };
DepthDump g_ddump;

void depth_dump_write() {
    const char *base = getenv("KLVR_DEPTH_DUMP");
    if (!base || !g_ddump.mapped) return;
    char path[600];
    snprintf(path, sizeof path, "%s.%d", base, g_ddump.n++);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "Pf\n%u %u\n-1.0\n", g_ddump.w, g_ddump.h);
    const float *px = (const float *)g_ddump.mapped;
    uint32_t pitch = g_ddump.pitch ? g_ddump.pitch : g_ddump.w;
    for (uint32_t y = g_ddump.h; y-- > 0;) fwrite(px + (size_t)y * pitch, 4, g_ddump.w, f);   // PFM is bottom-up
    fclose(f);
    logf("depth dump: %ux%u written to %s", g_ddump.w, g_ddump.h, path);
    if (g_ddump.cmapped) {                                   // the colour of the same submit, as a PPM (rows as stored)
        char cp[640]; snprintf(cp, sizeof cp, "%s.ppm", path);
        FILE *c = fopen(cp, "wb");
        if (c) {
            fprintf(c, "P6\n%u %u\n255\n", g_ddump.w, g_ddump.h);
            const uint8_t *q = (const uint8_t *)g_ddump.cmapped;
            for (size_t i = 0; i < (size_t)g_ddump.w * g_ddump.h; i++) fwrite(q + 4 * i, 1, 3, c);
            fclose(c);
        }
    }
    g_ddump.done = true;
}

} // namespace

// Returns 0 on success, or a vr::EVRCompositorError.
int vk_copy_eye(int stage, int eye, const vr::VRVulkanTextureData_t *t, uint32_t array_index,
                const vr::VRTextureBounds_t *bounds, uint64_t *fence_out, void **device_out,
                const vr::VRTextureDepthInfo_t *depth) {
    std::lock_guard<std::mutex> l(g_mu);
    // Retried until it works: the game can submit before its device exists
    // on our side of the loader, and that must not disable frames for good.
    if (!g_fns.ok) {
        g_fns = load_fns();
        if (!g_fns.ok && !g_fns_loaded) logf("Submit: Vulkan entry points unavailable, frames will not be shown");
        g_fns_loaded = true;
    }
    if (!g_fns.ok) return vr::VRCompositorError_TextureUsesUnsupportedFormat;
    if (!t || !t->m_nImage || !t->m_pDevice || !t->m_pQueue) return vr::VRCompositorError_InvalidTexture;

    VkDevice dev = (VkDevice)t->m_pDevice;
    VkQueue queue = (VkQueue)t->m_pQueue;
    VkImage src = (VkImage)t->m_nImage;

    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (bounds) { u0 = bounds->uMin; v0 = bounds->vMin; u1 = bounds->uMax; v1 = bounds->vMax; }
    // Source 2 sizes its bounds against the 1.0-scale render target. Started at a lower fidelity level, it allocates
    // the eye texture at that level's size and still submits e.g. u 0..1.28 v 0..1.25 (Mac, fixed L0 from boot): the
    // rendered picture is then the whole texture, and a copy that far past its edge reads nothing that was drawn.
    // Clamp to the texture.
    if (u0 < 0 || v0 < 0 || u1 > 1 || v1 > 1 || u0 > 1 || v0 > 1) {
        static int said;
        if (said < 2) {
            said++;
            logf("Submit: bounds u %.3f..%.3f v %.3f..%.3f run past the %ux%u eye texture; clamped to it", u0, u1, v0, v1,
                 t->m_nWidth, t->m_nHeight);
        }
        auto cl = [](float x) { return x < 0 ? 0.f : x > 1 ? 1.f : x; };
        u0 = cl(u0); v0 = cl(v0); u1 = cl(u1); v1 = cl(v1);
    }
    int32_t sx0 = (int32_t)std::lround(u0 * t->m_nWidth), sx1 = (int32_t)std::lround(u1 * t->m_nWidth);
    int32_t sy0 = (int32_t)std::lround(v0 * t->m_nHeight), sy1 = (int32_t)std::lround(v1 * t->m_nHeight);
    uint32_t w = (uint32_t)std::abs(sx1 - sx0), h = (uint32_t)std::abs(sy1 - sy0);
    if (!w || !h) return vr::VRCompositorError_InvalidBounds;

    VkImage dst = (VkImage)klvr_plat_eye_image(stage, eye, w, h, is_srgb((VkFormat)t->m_nFormat));
    if (!dst) return vr::VRCompositorError_TextureUsesUnsupportedFormat;

    DeviceState &ds = g_devices[{ dev, t->m_nQueueFamilyIndex }];
    if (!ds.pool) {
        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = t->m_nQueueFamilyIndex;
        if (g_fns.CreateCommandPool(dev, &ci, nullptr, &ds.pool) != VK_SUCCESS) return vr::VRCompositorError_InvalidTexture;
    }
    Slot &s = ds.slots[stage * 2 + eye];
    if (!s.cb) {
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = ds.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (g_fns.AllocateCommandBuffers(dev, &ai, &s.cb) != VK_SUCCESS ||
            g_fns.CreateFence(dev, &fi, nullptr, &s.fence) != VK_SUCCESS)
            return vr::VRCompositorError_InvalidTexture;
    }
    if (s.used) {
        g_fns.WaitForFences(dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
        g_fns.ResetFences(dev, 1, &s.fence);
        if (!g_ddump.done && g_ddump.stage == stage && eye == 0 && g_ddump.mapped) depth_dump_write();
    }
    g_fns.ResetCommandBuffer(s.cb, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_fns.BeginCommandBuffer(s.cb, &bi);

    VkImageMemoryBarrier pre[2] = {
        barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        barrier(dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    pre[0].subresourceRange.baseArrayLayer = array_index;
    g_fns.CmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, pre);

    if (t->m_nSampleCount > 1) {
        // A multisampled eye can only be resolved, and a resolve cannot crop,
        // flip or convert. Source 2 resolves its own MSAA before submitting,
        // so this is the path we expect never to take.
        static bool said = false;
        if (!said) { said = true; logf("Submit: %u-sample eye image; resolving without bounds or format conversion", t->m_nSampleCount); }
        VkImageResolve r{};
        r.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, array_index, 1 };
        r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        r.srcOffset = { std::min(sx0, sx1), std::min(sy0, sy1), 0 };
        r.extent = { w, h, 1 };
        if (g_fns.CmdResolveImage)
            g_fns.CmdResolveImage(s.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    } else if (env_on("KLVR_DEBUG_EYE_COLOR", false) && g_fns.CmdClearColorImage) {
        // The routing test: the left eye's image solid red, the right eye's solid
        // blue, instead of the game's picture. Closing one eye on the headset
        // then says whether each eye gets its own image end to end.
        VkClearColorValue c{};
        c.float32[0] = eye == 0 ? 1.0f : 0.0f;
        c.float32[2] = eye == 0 ? 0.0f : 1.0f;
        c.float32[3] = 1.0f;
        VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        g_fns.CmdClearColorImage(s.cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
    } else {
        VkImageBlit b{};
        b.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, array_index, 1 };
        b.srcOffsets[0] = { sx0, sy0, 0 };
        b.srcOffsets[1] = { sx1, sy1, 1 };
        b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        b.dstOffsets[0] = { 0, 0, 0 };
        b.dstOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
        g_fns.CmdBlitImage(s.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    }

    // KL_CP_DEPTH: the game's depth for this eye, same crop, into the platform's R32F depth image (depth aspect ->
    // host-visible staging -> image: a blit cannot read depth into a colour image). Not for a flipped or multisampled
    // eye: the colour blit flips, a copy cannot, and the depth would then disagree with the picture.
    static int want_depth = -1;
    if (want_depth < 0) want_depth = env_on("KL_CP_DEPTH", false) ? 1 : 0;
    // KL_CP_DEPTH_FROM=<s>: no depth copy (so no staging, and the compositor keeps plain rotation re-aim) until <s>
    // seconds after the first submit: a two-half run turns the depth re-aim on with its 30 fps half.
    static double depth_from = -2, first_submit;
    if (depth_from == -2) { const char *e = getenv("KL_CP_DEPTH_FROM"); depth_from = e && *e ? atof(e) : 0; }
    if (!first_submit) first_submit = klvr_plat_now();
    bool depth_now = want_depth && klvr_plat_now() - first_submit >= depth_from;
    if (want_depth && depth_from > 0 && depth_now) {
        static int said_from;
        if (!said_from++) logf("depth: re-aim with the game's depth from now on, %.1f s after the first submit (KL_CP_DEPTH_FROM)",
                               klvr_plat_now() - first_submit);
    }
    if (depth_now && depth && depth->handle && t->m_nSampleCount <= 1 && sx1 > sx0 && sy1 > sy0 &&
        g_fns.CmdCopyImageToBuffer && g_fns.CmdCopyBufferToImage) {
        const vr::VRVulkanTextureData_t *dt = (const vr::VRVulkanTextureData_t *)depth->handle;
        VkImage dsrc = (VkImage)dt->m_nImage;
        int32_t dx0 = (int32_t)std::lround(u0 * dt->m_nWidth), dx1 = (int32_t)std::lround(u1 * dt->m_nWidth);
        int32_t dy0 = (int32_t)std::lround(v0 * dt->m_nHeight), dy1 = (int32_t)std::lround(v1 * dt->m_nHeight);
        uint32_t dw = (uint32_t)(dx1 - dx0), dh = (uint32_t)(dy1 - dy0);
        void *mapped = nullptr;
        // By default the compositor samples the staging buffer itself (a texture view over it), so the rows are
        // padded to 64 texels for a linear texture's alignment; KL_CP_DEPTH_IMAGE=1 copies on into an R32F image.
        static int to_image = -1;
        if (to_image < 0) to_image = env_on("KL_CP_DEPTH_IMAGE", false) ? 1 : 0;
        // KL_CP_DEPTH_HALF (default on, staging path only): the game's depth is blitted (nearest) into a per-eye
        // half-size D32S8 image and that is copied to a quarter-size staging buffer. The re-aim samples it by
        // normalized coordinates, so nothing downstream changes; the host-visible staging (3 stages x 2 eyes)
        // drops from ~25 MB to ~6 MB per eye-stage at 2784x2208.
        static int half = -1;
        if (half < 0) half = env_on("KL_CP_DEPTH_HALF", true) ? 1 : 0;
        VkImage himg = VK_NULL_HANDLE;
        uint32_t fw = dw, fh = dh;
        if (half && !to_image && g_fns.CmdBlitImage && dw >= 2 && dh >= 2) {
            himg = (VkImage)klvr_plat_eye_depth_half(eye, dw / 2, dh / 2);
            if (himg) { dw /= 2; dh /= 2; }
        }
        uint32_t pitch = to_image ? dw : (dw + 63u) & ~63u;
        VkImage ddst = dsrc && dw && dh && to_image ? (VkImage)klvr_plat_eye_depth_image(stage, eye, dw, dh) : VK_NULL_HANDLE;
        VkBuffer sb = dsrc && dw && dh && (ddst || !to_image)
            ? (VkBuffer)klvr_plat_eye_depth_staging(stage, eye, (uint64_t)pitch * dh * 4, &mapped) : VK_NULL_HANDLE;
        if (sb) {
            VkImageMemoryBarrier db[2] = {
                barrier(dsrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
                barrier(ddst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
            };
            db[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            db[0].subresourceRange.baseArrayLayer = array_index;
            g_fns.CmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, ddst ? 2 : 1, db);
            VkBufferImageCopy r{};
            r.bufferRowLength = pitch;
            if (himg) {
                VkImageMemoryBarrier hb = barrier(himg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                  0, VK_ACCESS_TRANSFER_WRITE_BIT);
                hb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                g_fns.CmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &hb);
                VkImageBlit bl{};
                bl.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, array_index, 1 };
                bl.srcOffsets[0] = { dx0, dy0, 0 };
                bl.srcOffsets[1] = { dx0 + (int32_t)fw, dy0 + (int32_t)fh, 1 };
                bl.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
                bl.dstOffsets[0] = { 0, 0, 0 };
                bl.dstOffsets[1] = { (int32_t)dw, (int32_t)dh, 1 };
                g_fns.CmdBlitImage(s.cb, dsrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, himg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &bl, VK_FILTER_NEAREST);
                hb = barrier(himg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                hb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                g_fns.CmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &hb);
                r.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
                r.imageOffset = { 0, 0, 0 };
                r.imageExtent = { dw, dh, 1 };
                g_fns.CmdCopyImageToBuffer(s.cb, himg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sb, 1, &r);
            } else {
                r.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, array_index, 1 };
                r.imageOffset = { dx0, dy0, 0 };
                r.imageExtent = { dw, dh, 1 };
                g_fns.CmdCopyImageToBuffer(s.cb, dsrc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sb, 1, &r);
            }
            VkBufferMemoryBarrier bb{};
            bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
            bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = sb;
            bb.size = VK_WHOLE_SIZE;
            g_fns.CmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                                     0, 0, nullptr, 1, &bb, 0, nullptr);
            if (ddst) {
                VkBufferImageCopy w2{};
                w2.bufferRowLength = pitch;
                w2.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                w2.imageExtent = { dw, dh, 1 };
                g_fns.CmdCopyBufferToImage(s.cb, sb, ddst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &w2);
                VkImageMemoryBarrier dpost = barrier(ddst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
                g_fns.CmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &dpost);
            }
            // The game's depth mapping, as the compositor will need it (row-major, or its transpose).
            const vr::HmdMatrix44_t &m = depth->mProjection;
            bool rowmajor = std::fabs(m.m[3][2] + 1) < 1e-3f;
            klvr_plat_eye_depth_map(stage, eye, m.m[2][2], rowmajor ? m.m[2][3] : m.m[3][2], dw, dh, to_image ? 0 : pitch);
            static int said;
            if (!said++) logf("depth: copying the game's depth into the compositor (KL_CP_DEPTH): %ux%u per eye%s, m22 %.5f m23 %.5f",
                              dw, dh, himg ? " (half resolution)" : "", m.m[2][2], rowmajor ? m.m[2][3] : m.m[3][2]);
            // KLVR_DEPTH_DUMP_AT=<copy>[,<copy>...]: eye-0 copy numbers to dump (files <KLVR_DEPTH_DUMP>.0, .1, ...)
            static unsigned n_copies;
            if (eye == 0 && getenv("KLVR_DEPTH_DUMP")) {
                ++n_copies;
                const char *at = getenv("KLVR_DEPTH_DUMP_AT") ? getenv("KLVR_DEPTH_DUMP_AT") : "1500";
                for (const char *p = at; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : nullptr)
                    if ((unsigned)atoi(p) == n_copies) {
                        g_ddump.stage = stage; g_ddump.w = dw; g_ddump.h = dh; g_ddump.pitch = pitch; g_ddump.mapped = mapped; g_ddump.done = false;
                        // ...and the colour of this very submit, same crop, into a spare staging buffer (stage 3 is unused)
                        void *cm = nullptr;
                        VkBuffer cb2 = (VkBuffer)klvr_plat_eye_depth_staging(3, 0, (uint64_t)dw * dh * 4, &cm);
                        g_ddump.cmapped = nullptr;
                        if (cb2 && cm && dw == w && dh == h) {
                            VkBufferImageCopy rc{};
                            rc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, array_index, 1 };
                            rc.imageOffset = { dx0, dy0, 0 };
                            rc.imageExtent = { dw, dh, 1 };
                            g_fns.CmdCopyImageToBuffer(s.cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, cb2, 1, &rc);
                            g_ddump.cmapped = cm;
                        }
                        logf("depth dump armed at copy %u", n_copies);
                    }
            }
        }
    }

    VkImageMemoryBarrier post = barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    g_fns.CmdPipelineBarrier(s.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &post);
    g_fns.EndCommandBuffer(s.cb);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cb;
    if (g_fns.QueueSubmit(queue, 1, &si, s.fence) != VK_SUCCESS) return vr::VRCompositorError_InvalidTexture;
    s.used = true;
    if (fence_out) *fence_out = (uint64_t)s.fence;
    if (device_out) *device_out = (void *)dev;
    return vr::VRCompositorError_None;
}

bool vk_wait_copies(void *device, const uint64_t *fences, uint32_t n, double seconds) {
    if (!n || !device || !g_fns.ok) return true;
    VkFence f[2];
    for (uint32_t i = 0; i < n && i < 2; i++) f[i] = (VkFence)fences[i];
    return g_fns.WaitForFences((VkDevice)device, n < 2 ? n : 2, f, VK_TRUE, (uint64_t)(seconds * 1e9)) == VK_SUCCESS;
}

} // namespace klvr
