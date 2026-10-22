// Vulkan from a Windows program: imports its commands from vulkan-1.dll by name (as a program linked against the
// Vulkan SDK's vulkan-1.lib does), draws a red triangle on blue into an offscreen image and reads its pixels back,
// then draws the same into a window's swapchain and presents three frames, and checks a depth test: with a D32 depth
// attachment cleared to 0.5 and LESS, a triangle at z 0.8 leaves the clear color and one at z 0.2 draws.
//   vulkan_win.exe <vertex .spv> <fragment .spv>     (build/vulkan_spirv_vertex_fixture.spv, ..._fragment_fixture.spv)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define VX_H        // (vx_vulkan.h's types only: windows.h gives what vx.h would)
#include "../src/vx_vulkan.h"

#define W 64
#define H 64
static int bad;
#define CHECK(x) do { if (!(x)) { printf("vulkan_win: check failed (line %d): %s\n", __LINE__, #x); bad++; goto done; } } while (0)

static uint32_t *read_spv(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *words = n > 0 ? malloc((size_t)n) : NULL;
    if (words && fread(words, 1, (size_t)n, f) != (size_t)n) { free(words); words = NULL; }
    fclose(f);
    *size = (size_t)n;
    return words;
}

static uint32_t memory_type(VkPhysicalDevice gpu, uint32_t bits, VkFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static VkRenderPass render_pass_depth(VkDevice device, VkFormat format, VkImageLayout final_layout, VkFormat depth_format);
static VkRenderPass render_pass(VkDevice device, VkFormat format, VkImageLayout final_layout) {
    return render_pass_depth(device, format, final_layout, 0);
}
// A pass with one color attachment and, with a depth_format, a depth attachment (index 1) cleared and not stored.
static VkRenderPass render_pass_depth(VkDevice device, VkFormat format, VkImageLayout final_layout, VkFormat depth_format) {
    VkAttachmentDescription color = {
        .format = format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = final_layout
    };
    VkAttachmentDescription attachments[2] = { color, {
        .format = depth_format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    } };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &ref,
        .pDepthStencilAttachment = depth_format ? &depth_ref : NULL
    };
    VkRenderPassCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = depth_format ? 2 : 1, .pAttachments = attachments, .subpassCount = 1, .pSubpasses = &subpass
    };
    VkRenderPass pass = 0;
    return vkCreateRenderPass(device, &info, NULL, &pass) == VK_SUCCESS ? pass : 0;
}

static VkPipeline pipeline_depth(VkDevice device, VkRenderPass pass, VkPipelineLayout layout,
                                 VkShaderModule vs, VkShaderModule fs, uint32_t width, uint32_t height, int depth_test);
static VkPipeline pipeline(VkDevice device, VkRenderPass pass, VkPipelineLayout layout,
                           VkShaderModule vs, VkShaderModule fs, uint32_t width, uint32_t height) {
    return pipeline_depth(device, pass, layout, vs, fs, width, height, 0);
}
static VkPipeline pipeline_depth(VkDevice device, VkRenderPass pass, VkPipelineLayout layout,
                                 VkShaderModule vs, VkShaderModule fs, uint32_t width, uint32_t height, int depth_test) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main" },
    };
    VkVertexInputBindingDescription binding = { 0, 4 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attribute = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attribute
    };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkViewport viewport = { 0, 0, (float)width, (float)height, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { width, height } };
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor
    };
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineColorBlendAttachmentState blend = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo blend_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend
    };
    VkPipelineDepthStencilStateCreateInfo depth_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS
    };
    VkGraphicsPipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vertex_input, .pInputAssemblyState = &assembly, .pViewportState = &viewport_state,
        .pRasterizationState = &raster, .pMultisampleState = &multisample,
        .pDepthStencilState = depth_test ? &depth_state : NULL, .pColorBlendState = &blend_state,
        .layout = layout, .renderPass = pass, .subpass = 0
    };
    VkPipeline p = 0;
    return vkCreateGraphicsPipelines(device, 0, 1, &info, NULL, &p) == VK_SUCCESS ? p : 0;
}

static void draw(VkCommandBuffer cmd, VkRenderPass pass, VkFramebuffer fb, VkPipeline p, VkBuffer vertices,
                 uint32_t width, uint32_t height) {
    VkClearValue blue = { .color = { .float32 = { 0, 0, 1, 1 } } };
    VkRenderPassBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = pass, .framebuffer = fb,
        .renderArea = { { 0, 0 }, { width, height } }, .clearValueCount = 1, .pClearValues = &blue
    };
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices, &offset);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

int main(int argc, char **argv) {
    if (argc < 3) { printf("usage: vulkan_win.exe <vertex .spv> <fragment .spv>\n"); return 2; }
    VkInstance instance = NULL;
    VkDevice device = NULL;
    HWND window = NULL;
    int presented = 0;
    unsigned char center[4] = { 0 }, corner[4] = { 0 }, depth_center[2][4] = { { 0 } };

    const char *instance_extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vulkan_win",
                              .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo instance_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                           .enabledExtensionCount = 2, .ppEnabledExtensionNames = instance_extensions };
    CHECK(vkCreateInstance(&instance_info, NULL, &instance) == VK_SUCCESS);
    uint32_t gpus = 1;
    VkPhysicalDevice gpu = NULL;
    CHECK(vkEnumeratePhysicalDevices(instance, &gpus, &gpu) >= 0 && gpu);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                           .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority };
    const char *device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo device_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                                       .pQueueCreateInfos = &queue_info, .enabledExtensionCount = 1,
                                       .ppEnabledExtensionNames = device_extensions };
    CHECK(vkCreateDevice(gpu, &device_info, NULL, &device) == VK_SUCCESS);
    VkQueue queue = NULL;
    vkGetDeviceQueue(device, 0, 0, &queue);
    CHECK(queue);

    // Shaders and the triangle (clip space; the vertex shader passes the attribute through as the position).
    size_t vs_size = 0, fs_size = 0;
    uint32_t *vs_words = read_spv(argv[1], &vs_size), *fs_words = read_spv(argv[2], &fs_size);
    CHECK(vs_words && fs_words);
    VkShaderModuleCreateInfo vs_info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vs_size, .pCode = vs_words };
    VkShaderModuleCreateInfo fs_info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = fs_size, .pCode = fs_words };
    VkShaderModule vs = 0, fs = 0;
    CHECK(vkCreateShaderModule(device, &vs_info, NULL, &vs) == VK_SUCCESS);
    CHECK(vkCreateShaderModule(device, &fs_info, NULL, &fs) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout) == VK_SUCCESS);

    const float triangle[3][4] = { { -0.5f, -0.5f, 0, 1 }, { 0.5f, -0.5f, 0, 1 }, { 0.0f, 0.5f, 0, 1 } };
    VkBufferCreateInfo vb_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sizeof triangle,
                                   .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT };
    VkBuffer vertices = 0;
    CHECK(vkCreateBuffer(device, &vb_info, NULL, &vertices) == VK_SUCCESS);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, vertices, &req);
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory vb_memory = 0;
    CHECK(alloc.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(device, &alloc, NULL, &vb_memory) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(device, vertices, vb_memory, 0) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, vb_memory, 0, sizeof triangle, 0, &mapped) == VK_SUCCESS);
    memcpy(mapped, triangle, sizeof triangle);
    vkUnmapMemory(device, vb_memory);

    VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = 0 };
    VkCommandPool pool = 0;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cmd_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd = NULL;
    CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd) == VK_SUCCESS);
    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = 0;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    // 1. Offscreen: draw, copy to a host-visible buffer, check a pixel inside the triangle and one outside.
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VkImage image = 0;
    CHECK(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS);
    vkGetImageMemoryRequirements(device, image, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(gpu, req.memoryTypeBits, 0);
    VkDeviceMemory image_memory = 0;
    CHECK(vkAllocateMemory(device, &alloc, NULL, &image_memory) == VK_SUCCESS);
    CHECK(vkBindImageMemory(device, image, image_memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkImageView view = 0;
    CHECK(vkCreateImageView(device, &view_info, NULL, &view) == VK_SUCCESS);
    VkRenderPass offscreen_pass = render_pass(device, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CHECK(offscreen_pass);
    VkFramebufferCreateInfo fb_info = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = offscreen_pass,
                                        .attachmentCount = 1, .pAttachments = &view, .width = W, .height = H, .layers = 1 };
    VkFramebuffer offscreen_fb = 0;
    CHECK(vkCreateFramebuffer(device, &fb_info, NULL, &offscreen_fb) == VK_SUCCESS);
    VkPipeline offscreen_pipeline = pipeline(device, offscreen_pass, layout, vs, fs, W, H);
    CHECK(offscreen_pipeline);
    VkBufferCreateInfo rb_info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4,
                                   .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer readback = 0;
    CHECK(vkCreateBuffer(device, &rb_info, NULL, &readback) == VK_SUCCESS);
    vkGetBufferMemoryRequirements(device, readback, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory rb_memory = 0;
    CHECK(vkAllocateMemory(device, &alloc, NULL, &rb_memory) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(device, readback, rb_memory, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
    draw(cmd, offscreen_pass, offscreen_fb, offscreen_pipeline, vertices, W, H);
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);
    CHECK(vkMapMemory(device, rb_memory, 0, W * H * 4, 0, &mapped) == VK_SUCCESS);
    memcpy(center, (unsigned char *)mapped + ((H / 2) * W + W / 2) * 4, 4);
    memcpy(corner, (unsigned char *)mapped, 4);
    vkUnmapMemory(device, rb_memory);
    // (BGRA bytes: red is 00 00 ff ff, blue ff 00 00 ff)
    CHECK(center[0] == 0 && center[1] == 0 && center[2] == 255 && center[3] == 255);
    CHECK(corner[0] == 255 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255);

    // 2. Depth: a D32 attachment cleared to 0.5 with LESS. A: the triangle at z 0.8 alone fails the test (blue stays);
    //    B: z 0.8, then z 0.2, which passes (red).
    const float layered[6][4] = {
        { -0.5f, -0.5f, 0.8f, 1 }, { 0.5f, -0.5f, 0.8f, 1 }, { 0.0f, 0.5f, 0.8f, 1 },
        { -0.5f, -0.5f, 0.2f, 1 }, { 0.5f, -0.5f, 0.2f, 1 }, { 0.0f, 0.5f, 0.2f, 1 },
    };
    vb_info.size = sizeof layered;
    VkBuffer layered_vertices = 0;
    CHECK(vkCreateBuffer(device, &vb_info, NULL, &layered_vertices) == VK_SUCCESS);
    vkGetBufferMemoryRequirements(device, layered_vertices, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory layered_memory = 0;
    CHECK(vkAllocateMemory(device, &alloc, NULL, &layered_memory) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(device, layered_vertices, layered_memory, 0) == VK_SUCCESS);
    CHECK(vkMapMemory(device, layered_memory, 0, sizeof layered, 0, &mapped) == VK_SUCCESS);
    memcpy(mapped, layered, sizeof layered);
    vkUnmapMemory(device, layered_memory);
    VkImageCreateInfo depth_info = image_info;
    depth_info.format = VK_FORMAT_D32_SFLOAT;
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImage depth_image = 0;
    CHECK(vkCreateImage(device, &depth_info, NULL, &depth_image) == VK_SUCCESS);
    vkGetImageMemoryRequirements(device, depth_image, &req);
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type(gpu, req.memoryTypeBits, 0);
    VkDeviceMemory depth_memory = 0;
    CHECK(vkAllocateMemory(device, &alloc, NULL, &depth_memory) == VK_SUCCESS);
    CHECK(vkBindImageMemory(device, depth_image, depth_memory, 0) == VK_SUCCESS);
    VkImageViewCreateInfo depth_view_info = view_info;
    depth_view_info.image = depth_image;
    depth_view_info.format = VK_FORMAT_D32_SFLOAT;
    depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageView depth_view = 0;
    CHECK(vkCreateImageView(device, &depth_view_info, NULL, &depth_view) == VK_SUCCESS);
    VkRenderPass depth_pass = render_pass_depth(device, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                VK_FORMAT_D32_SFLOAT);
    CHECK(depth_pass);
    VkImageView depth_attachments[2] = { view, depth_view };
    VkFramebufferCreateInfo depth_fb_info = fb_info;
    depth_fb_info.renderPass = depth_pass;
    depth_fb_info.attachmentCount = 2;
    depth_fb_info.pAttachments = depth_attachments;
    VkFramebuffer depth_fb = 0;
    CHECK(vkCreateFramebuffer(device, &depth_fb_info, NULL, &depth_fb) == VK_SUCCESS);
    VkPipeline depth_pipeline = pipeline_depth(device, depth_pass, layout, vs, fs, W, H, 1);
    CHECK(depth_pipeline);
    for (int run = 0; run < 2; run++) {
        CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
        CHECK(vkResetCommandBuffer(cmd, 0) == VK_SUCCESS);
        CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
        VkClearValue clears[2] = { { .color = { .float32 = { 0, 0, 1, 1 } } },
                                   { .depthStencil = { 0.5f, 0 } } };
        VkRenderPassBeginInfo depth_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = depth_pass, .framebuffer = depth_fb,
            .renderArea = { { 0, 0 }, { W, H } }, .clearValueCount = 2, .pClearValues = clears
        };
        vkCmdBeginRenderPass(cmd, &depth_begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depth_pipeline);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &layered_vertices, &zero);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        if (run) vkCmdDraw(cmd, 3, 1, 3, 0);
        vkCmdEndRenderPass(cmd);
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
        CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);
        CHECK(vkMapMemory(device, rb_memory, 0, W * H * 4, 0, &mapped) == VK_SUCCESS);
        memcpy(depth_center[run], (unsigned char *)mapped + ((H / 2) * W + W / 2) * 4, 4);
        vkUnmapMemory(device, rb_memory);
    }
    CHECK(depth_center[0][0] == 255 && depth_center[0][2] == 0);    // A: blue, the far triangle was rejected
    CHECK(depth_center[1][0] == 0 && depth_center[1][2] == 255);    // B: red, the near one passed

    // 3. A window's swapchain: the same picture, presented three times.
    WNDCLASSA wc = { .lpfnWndProc = DefWindowProcA, .hInstance = GetModuleHandleA(NULL), .lpszClassName = "vulkan_win" };
    RegisterClassA(&wc);
    window = CreateWindowExA(0, "vulkan_win", "vulkan_win", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
    CHECK(window);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    VkWin32SurfaceCreateInfoKHR surface_info = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                                 .hinstance = wc.hInstance, .hwnd = window };
    VkSurfaceKHR surface = 0;
    CHECK(vkCreateWin32SurfaceKHR(instance, &surface_info, NULL, &surface) == VK_SUCCESS);
    VkBool32 supported = VK_FALSE;
    CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, 0, surface, &supported) == VK_SUCCESS && supported);
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps) == VK_SUCCESS);
    uint32_t sw_width = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent.width : 320;
    uint32_t sw_height = caps.currentExtent.height != UINT32_MAX ? caps.currentExtent.height : 240;
    VkSurfaceFormatKHR format;
    uint32_t formats = 1;
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &formats, &format) >= 0 && formats);
    VkSwapchainCreateInfoKHR sw_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = surface,
        .minImageCount = caps.minImageCount > 2 ? caps.minImageCount : 2,
        .imageFormat = format.format, .imageColorSpace = format.colorSpace, .imageExtent = { sw_width, sw_height },
        .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE
    };
    VkSwapchainKHR swapchain = 0;
    CHECK(vkCreateSwapchainKHR(device, &sw_info, NULL, &swapchain) == VK_SUCCESS);
    VkImage sw_images[4];
    uint32_t sw_count = 4;
    CHECK(vkGetSwapchainImagesKHR(device, swapchain, &sw_count, sw_images) >= 0 && sw_count >= 2);
    VkRenderPass present_pass = render_pass(device, format.format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    CHECK(present_pass);
    VkPipeline present_pipeline = pipeline(device, present_pass, layout, vs, fs, sw_width, sw_height);
    CHECK(present_pipeline);
    VkImageView sw_views[4] = { 0 };
    VkFramebuffer sw_fbs[4] = { 0 };
    for (uint32_t i = 0; i < sw_count; i++) {
        view_info.image = sw_images[i];
        view_info.format = format.format;
        CHECK(vkCreateImageView(device, &view_info, NULL, &sw_views[i]) == VK_SUCCESS);
        fb_info.renderPass = present_pass;
        fb_info.pAttachments = &sw_views[i];
        fb_info.width = sw_width;
        fb_info.height = sw_height;
        CHECK(vkCreateFramebuffer(device, &fb_info, NULL, &sw_fbs[i]) == VK_SUCCESS);
    }
    VkSemaphoreCreateInfo sem_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired = 0, rendered = 0;
    CHECK(vkCreateSemaphore(device, &sem_info, NULL, &acquired) == VK_SUCCESS);
    CHECK(vkCreateSemaphore(device, &sem_info, NULL, &rendered) == VK_SUCCESS);
    for (int frame = 0; frame < 3; frame++) {
        uint32_t index = 0;
        CHECK(vkAcquireNextImageKHR(device, swapchain, 5000000000ull, acquired, 0, &index) == VK_SUCCESS);
        CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
        CHECK(vkResetCommandBuffer(cmd, 0) == VK_SUCCESS);
        CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
        draw(cmd, present_pass, sw_fbs[index], present_pipeline, vertices, sw_width, sw_height);
        CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo frame_submit = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired,
            .pWaitDstStageMask = &wait_stage, .commandBufferCount = 1, .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered
        };
        CHECK(vkQueueSubmit(queue, 1, &frame_submit, fence) == VK_SUCCESS);
        VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                                     .pWaitSemaphores = &rendered, .swapchainCount = 1, .pSwapchains = &swapchain,
                                     .pImageIndices = &index };
        CHECK(vkQueuePresentKHR(queue, &present) == VK_SUCCESS);
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);
        presented++;
    }
    vkDeviceWaitIdle(device);

done:
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    if (window) DestroyWindow(window);
    printf("vulkan from a Windows program: commands imported from vulkan-1.dll, offscreen triangle "
           "(center %02x%02x%02x%02x, corner %02x%02x%02x%02x as BGRA), D32 depth test LESS against 0.5 "
           "(z 0.8 alone: %02x%02x%02x, then z 0.2: %02x%02x%02x), %d frames presented: %s\n",
           center[0], center[1], center[2], center[3], corner[0], corner[1], corner[2], corner[3],
           depth_center[0][0], depth_center[0][1], depth_center[0][2], depth_center[1][0], depth_center[1][1],
           depth_center[1][2], presented, bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
