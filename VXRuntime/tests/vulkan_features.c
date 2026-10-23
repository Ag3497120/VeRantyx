// Vulkan from a Windows program, the commands a game's frame uses besides drawing: clears of images, attachments
// inside a render pass and with dynamic rendering, a 4x multisampled clear resolved by its render pass, scaled
// blits, copies between buffers and images (mip levels, array layers, depth), 32-bit buffer fills and updates,
// timeline semaphores (host and queue), events, secondary command buffers, timestamps, and the feature/property
// queries of Vulkan 1.2/1.3. Every value is checked after reading it back.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "vulkan/vulkan_core.h"

static int bad;
static const char *test = "";
#define CHECK(x) do { if (!(x)) { printf("vulkan_features: %s: check failed (line %d): %s\n", test, __LINE__, #x); bad++; } } while (0)

static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkFence fence;

static uint32_t memory_type(uint32_t bits, VkFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

typedef struct { VkBuffer buffer; VkDeviceMemory memory; uint8_t *map; } buf;
static buf make_buffer(VkDeviceSize size) {
    buf b = { 0 };
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    vkCreateBuffer(device, &info, NULL, &b.buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    vkAllocateMemory(device, &alloc, NULL, &b.memory);
    vkBindBufferMemory(device, b.buffer, b.memory, 0);
    vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, (void **)&b.map);
    memset(b.map, 0, size);
    return b;
}
static void free_buffer(buf *b) { vkDestroyBuffer(device, b->buffer, NULL); vkFreeMemory(device, b->memory, NULL); }

typedef struct { VkImage image; VkDeviceMemory memory; } img;
static img make_image(VkFormat format, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, VkSampleCountFlagBits samples,
                      VkImageUsageFlags usage) {
    img i = { 0 };
    VkImageCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = { w, h, 1 }, .mipLevels = mips, .arrayLayers = layers, .samples = samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(vkCreateImage(device, &info, NULL, &i.image) == VK_SUCCESS);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, i.image, &req);
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                   .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHECK(vkAllocateMemory(device, &alloc, NULL, &i.memory) == VK_SUCCESS);
    CHECK(vkBindImageMemory(device, i.image, i.memory, 0) == VK_SUCCESS);
    return i;
}
static void free_image(img *i) { vkDestroyImage(device, i->image, NULL); vkFreeMemory(device, i->memory, NULL); }
static VkImageView make_view(VkImage image, VkFormat format, VkImageAspectFlags aspect, uint32_t mip, uint32_t layer) {
    VkImageViewCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format, .subresourceRange = { aspect, mip, 1, layer, 1 } };
    VkImageView v = 0;
    CHECK(vkCreateImageView(device, &info, NULL, &v) == VK_SUCCESS);
    return v;
}

static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                         .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb = 0;
    vkAllocateCommandBuffers(device, &info, &cb);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}
static void submit(VkCommandBuffer cb) {
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkResetFences(device, 1, &fence);
    CHECK(vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);
    vkFreeCommandBuffers(device, pool, 1, &cb);
}
static void barrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void to_buffer(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, uint32_t mip, uint32_t layer, uint32_t w, uint32_t h,
                      VkBuffer b, VkDeviceSize offset) {
    VkBufferImageCopy r = { .bufferOffset = offset, .imageSubresource = { aspect, mip, layer, 1 }, .imageExtent = { w, h, 1 } };
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &r);
}
static int near8(uint8_t a, int b) { return abs((int)a - b) <= 2; }
static int rgba(const uint8_t *p, int r, int g, int b, int a) { return near8(p[0], r) && near8(p[1], g) && near8(p[2], b) && near8(p[3], a); }

int main(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vulkan_features",
                              .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance instance = 0;
    uint32_t version = 0;
    test = "instance";
    CHECK(vkEnumerateInstanceVersion(&version) == VK_SUCCESS && VK_API_VERSION_MINOR(version) >= 3);
    CHECK(vkCreateInstance(&ii, NULL, &instance) == VK_SUCCESS);
    uint32_t n = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, &gpu) == VK_SUCCESS && n == 1);

    // Feature and property chains.
    test = "features";
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
    vkGetPhysicalDeviceFeatures2(gpu, &f2);
    CHECK(f12.timelineSemaphore && f12.descriptorIndexing && f12.runtimeDescriptorArray && f13.synchronization2 && f13.dynamicRendering);
    VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver };
    vkGetPhysicalDeviceProperties2(gpu, &p2);
    CHECK(VK_API_VERSION_MINOR(p2.properties.apiVersion) >= 3 && driver.driverName[0]);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_BC7_UNORM_BLOCK, &fp);
    CHECK(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
    CHECK(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);

    float priority = 1;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority };
    VkPhysicalDeviceVulkan13Features e13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .synchronization2 = 1, .dynamicRendering = 1 };
    VkPhysicalDeviceVulkan12Features e12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &e13, .timelineSemaphore = 1 };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &e12, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
    test = "device";
    CHECK(vkCreateDevice(gpu, &di, NULL, &device) == VK_SUCCESS);
    vkGetDeviceQueue(device, 0, 0, &queue);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    vkCreateCommandPool(device, &pi, NULL, &pool);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(device, &fi, NULL, &fence);
    const VkImageUsageFlags color_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    buf out = make_buffer(1 << 20);

    // 1. Clear an image, copy it to a buffer.
    {
        test = "clear color image";
        img a = make_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, VK_SAMPLE_COUNT_1_BIT, color_usage);
        VkCommandBuffer cb = begin();
        VkClearColorValue c = { .float32 = { 1.0f, 0.5f, 0.0f, 1.0f } };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        barrier(cb);
        to_buffer(cb, a.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 64, 64, out.buffer, 0);
        submit(cb);
        CHECK(rgba(out.map, 255, 128, 0, 255) && rgba(out.map + (64 * 64 - 1) * 4, 255, 128, 0, 255));
        free_image(&a);
    }
    // 2. Upload a pattern (left half red, right half blue), blit it down to half size (nearest and linear).
    {
        test = "blit";
        img a = make_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, VK_SAMPLE_COUNT_1_BIT, color_usage);
        img b = make_image(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 1, VK_SAMPLE_COUNT_1_BIT, color_usage);
        buf up = make_buffer(64 * 64 * 4);
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                uint8_t *p = up.map + (y * 64 + x) * 4;
                p[0] = x < 32 ? 255 : 0; p[1] = 0; p[2] = x < 32 ? 0 : 255; p[3] = 255;
            }
        VkCommandBuffer cb = begin();
        VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 64, 64, 1 } };
        vkCmdCopyBufferToImage(cb, up.buffer, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        barrier(cb);
        VkImageBlit blit = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { 64, 64, 1 } },
                             .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstOffsets = { { 0, 0, 0 }, { 32, 32, 1 } } };
        vkCmdBlitImage(cb, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barrier(cb);
        to_buffer(cb, b.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 32, 32, out.buffer, 0);
        // (and a flipped blit: left becomes right)
        VkImageBlit flip = blit;
        flip.dstOffsets[0].x = 32; flip.dstOffsets[1].x = 0;
        vkCmdBlitImage(cb, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &flip, VK_FILTER_NEAREST);
        barrier(cb);
        to_buffer(cb, b.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 32, 32, out.buffer, 32 * 32 * 4);
        submit(cb);
        CHECK(rgba(out.map + (10 * 32 + 2) * 4, 255, 0, 0, 255) && rgba(out.map + (10 * 32 + 29) * 4, 0, 0, 255, 255));
        uint8_t *f = out.map + 32 * 32 * 4;
        CHECK(rgba(f + (10 * 32 + 2) * 4, 0, 0, 255, 255) && rgba(f + (10 * 32 + 29) * 4, 255, 0, 0, 255));
        free_image(&a); free_image(&b); free_buffer(&up);
    }
    // 3. Fills with a 32-bit pattern and with a byte pattern, and an update.
    {
        test = "fill and update";
        buf b = make_buffer(256);
        VkCommandBuffer cb = begin();
        vkCmdFillBuffer(cb, b.buffer, 16, 32, 0x11223344u);
        vkCmdFillBuffer(cb, b.buffer, 64, 16, 0xababababu);
        uint32_t words[4] = { 7, 8, 9, 10 };
        vkCmdUpdateBuffer(cb, b.buffer, 128, sizeof words, words);
        submit(cb);
        uint32_t *w = (uint32_t *)b.map;
        CHECK(w[3] == 0 && w[4] == 0x11223344u && w[11] == 0x11223344u && w[12] == 0);
        CHECK(w[16] == 0xababababu && w[19] == 0xababababu && w[20] == 0);
        CHECK(w[32] == 7 && w[35] == 10);
        free_buffer(&b);
    }
    // 4. Mip levels and array layers: a buffer into level 1 of layer 2, copied to another image, read back.
    {
        test = "copy levels and layers";
        img a = make_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 3, 3, VK_SAMPLE_COUNT_1_BIT, color_usage);
        img b = make_image(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1, 1, VK_SAMPLE_COUNT_1_BIT, color_usage);
        buf up = make_buffer(32 * 32 * 4);
        for (int i = 0; i < 32 * 32; i++) { up.map[i * 4] = (uint8_t)i; up.map[i * 4 + 1] = (uint8_t)(i >> 8); up.map[i * 4 + 2] = 7; up.map[i * 4 + 3] = 9; }
        VkCommandBuffer cb = begin();
        VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 2, 1 }, .imageExtent = { 32, 32, 1 } };
        vkCmdCopyBufferToImage(cb, up.buffer, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        barrier(cb);
        VkImageCopy c = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 2, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                          .extent = { 32, 32, 1 } };
        vkCmdCopyImage(cb, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cb);
        to_buffer(cb, b.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 32, 32, out.buffer, 0);
        submit(cb);
        CHECK(memcmp(out.map, up.map, 32 * 32 * 4) == 0);
        free_image(&a); free_image(&b); free_buffer(&up);
    }
    // 5. A render pass with a 4x multisampled color attachment cleared and resolved, no draws.
    {
        test = "msaa resolve";
        img ms = make_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        img rs = make_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, VK_SAMPLE_COUNT_1_BIT, color_usage);
        VkAttachmentDescription att[2] = {
            { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
              VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
              VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL } };
        VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, rref = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                                    .pColorAttachments = &cref, .pResolveAttachments = &rref };
        VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = att,
                                       .subpassCount = 1, .pSubpasses = &sp };
        VkRenderPass rp = 0;
        CHECK(vkCreateRenderPass(device, &rpi, NULL, &rp) == VK_SUCCESS);
        VkImageView views[2] = { make_view(ms.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0),
                                 make_view(rs.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0) };
        VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2,
                                        .pAttachments = views, .width = 64, .height = 64, .layers = 1 };
        VkFramebuffer fb = 0;
        CHECK(vkCreateFramebuffer(device, &fbi, NULL, &fb) == VK_SUCCESS);
        VkClearValue clear[2] = { { .color = { .float32 = { 0, 1, 0, 1 } } } };
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                      .renderArea = { { 0, 0 }, { 64, 64 } }, .clearValueCount = 2, .pClearValues = clear };
        VkCommandBuffer cb = begin();
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cb);
        barrier(cb);
        to_buffer(cb, rs.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 64, 64, out.buffer, 0);
        submit(cb);
        CHECK(rgba(out.map + (20 * 64 + 20) * 4, 0, 255, 0, 255));
        vkDestroyFramebuffer(device, fb, NULL);
        vkDestroyImageView(device, views[0], NULL); vkDestroyImageView(device, views[1], NULL);
        vkDestroyRenderPass(device, rp, NULL);
        free_image(&ms); free_image(&rs);
    }
    // 6. Dynamic rendering: a color and a depth/stencil attachment cleared, then a rectangle of the color cleared again
    //    with vkCmdClearAttachments; the depth and stencil aspects read back.
    {
        test = "dynamic rendering";
        img c = make_image(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1, 1, VK_SAMPLE_COUNT_1_BIT, color_usage);
        img d = make_image(VK_FORMAT_D32_SFLOAT_S8_UINT, 64, 64, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkImageView cv = make_view(c.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);
        VkImageView dv = make_view(d.image, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0);
        VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = cv,
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue = { .color = { .float32 = { 0, 0, 1, 1 } } } };
        VkRenderingAttachmentInfo da = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dv,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .depthStencil = { 0.75f, 0x5a } } };
        VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { 64, 64 } }, .layerCount = 1,
                               .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &da, .pStencilAttachment = &da };
        VkCommandBuffer cb = begin();
        vkCmdBeginRendering(cb, &ri);
        VkClearAttachment clr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, { .color = { .float32 = { 1, 1, 0, 1 } } } };
        VkClearRect rect = { { { 16, 16 }, { 16, 16 } }, 0, 1 };
        vkCmdClearAttachments(cb, 1, &clr, 1, &rect);
        vkCmdEndRendering(cb);
        barrier(cb);
        to_buffer(cb, c.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 64, 64, out.buffer, 0);
        to_buffer(cb, d.image, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 64, 64, out.buffer, 65536);
        to_buffer(cb, d.image, VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 64, 64, out.buffer, 2 * 65536);
        submit(cb);
        CHECK(rgba(out.map + (2 * 64 + 2) * 4, 0, 0, 255, 255));
        CHECK(rgba(out.map + (20 * 64 + 20) * 4, 255, 255, 0, 255));
        CHECK(rgba(out.map + (40 * 64 + 40) * 4, 0, 0, 255, 255));
        float depth;
        memcpy(&depth, out.map + 65536 + (5 * 64 + 5) * 4, 4);
        CHECK(depth == 0.75f);
        CHECK(out.map[2 * 65536 + 5 * 64 + 5] == 0x5a);
        vkDestroyImageView(device, cv, NULL); vkDestroyImageView(device, dv, NULL);
        free_image(&c); free_image(&d);
    }
    // 7. Timeline semaphores: signaled from the host and from a submission that waits on the host's value.
    {
        test = "timeline semaphore";
        VkSemaphoreTypeCreateInfo ti = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                         .initialValue = 1 };
        VkSemaphoreCreateInfo si = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &ti };
        VkSemaphore t = 0;
        CHECK(vkCreateSemaphore(device, &si, NULL, &t) == VK_SUCCESS);
        uint64_t v = 0;
        CHECK(vkGetSemaphoreCounterValue(device, t, &v) == VK_SUCCESS && v == 1);
        uint64_t wait_value = 5, signal_value = 7;
        VkCommandBuffer cb = begin();
        vkEndCommandBuffer(cb);
        VkTimelineSemaphoreSubmitInfo tsi = { .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = &wait_value, .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &signal_value };
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo s = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tsi, .waitSemaphoreCount = 1, .pWaitSemaphores = &t,
                           .pWaitDstStageMask = &stage, .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1,
                           .pSignalSemaphores = &t };
        CHECK(vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE) == VK_SUCCESS);
        VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &t, .pValues = &signal_value };
        CHECK(vkWaitSemaphores(device, &wi, 0) == VK_TIMEOUT);              // (the submission waits for 5 first)
        VkSemaphoreSignalInfo sig = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = t, .value = 5 };
        CHECK(vkSignalSemaphore(device, &sig) == VK_SUCCESS);
        CHECK(vkWaitSemaphores(device, &wi, 5000000000ull) == VK_SUCCESS);
        CHECK(vkGetSemaphoreCounterValue(device, t, &v) == VK_SUCCESS && v == 7);
        vkQueueWaitIdle(queue);
        vkFreeCommandBuffers(device, pool, 1, &cb);
        vkDestroySemaphore(device, t, NULL);
    }
    // 8. A secondary command buffer clears an image; the primary runs it, sets an event, and writes timestamps.
    {
        test = "secondary, event, timestamps";
        img a = make_image(VK_FORMAT_R8G8B8A8_UNORM, 16, 16, 1, 1, VK_SAMPLE_COUNT_1_BIT, color_usage);
        VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                           .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY, .commandBufferCount = 1 };
        VkCommandBuffer sec = 0;
        vkAllocateCommandBuffers(device, &ai, &sec);
        VkCommandBufferInheritanceInfo inh = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pInheritanceInfo = &inh };
        vkBeginCommandBuffer(sec, &bi);
        VkClearColorValue c = { .float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(sec, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        vkEndCommandBuffer(sec);
        VkEventCreateInfo ei = { .sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
        VkEvent ev = 0;
        CHECK(vkCreateEvent(device, &ei, NULL, &ev) == VK_SUCCESS);
        CHECK(vkGetEventStatus(device, ev) == VK_EVENT_RESET);
        VkQueryPoolCreateInfo qpi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 };
        VkQueryPool qp = 0;
        CHECK(vkCreateQueryPool(device, &qpi, NULL, &qp) == VK_SUCCESS);
        VkCommandBuffer cb = begin();
        vkCmdResetQueryPool(cb, qp, 0, 2);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qp, 0);
        vkCmdExecuteCommands(cb, 1, &sec);
        barrier(cb);
        to_buffer(cb, a.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 16, 16, out.buffer, 0);
        vkCmdSetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        submit(cb);
        CHECK(rgba(out.map + (5 * 16 + 5) * 4, 64, 128, 191, 255));
        CHECK(vkGetEventStatus(device, ev) == VK_EVENT_SET);
        uint64_t ts[2] = { 0, 0 };
        CHECK(vkGetQueryPoolResults(device, qp, 0, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS);
        CHECK(ts[0] && ts[1] >= ts[0]);
        vkDestroyQueryPool(device, qp, NULL);
        vkDestroyEvent(device, ev, NULL);
        vkFreeCommandBuffers(device, pool, 1, &sec);
        free_image(&a);
    }

    free_buffer(&out);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("vulkan features from a Windows program: clears (image, render pass + resolve, dynamic rendering, attachment rectangle, "
           "depth/stencil), blits (scaled, flipped), copies (levels, layers, aspects), fills and updates, timeline semaphores, "
           "secondary command buffers, events, timestamps, 1.2/1.3 queries -> %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
