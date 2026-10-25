// Vulkan from a Windows program, through shaders: GLSL written for this test (tests/vulkan_shaders/*, compiled to
// SPIR-V with glslang by `make vulkan-test-shaders`; the SPIR-V is committed, so the test needs no compiler) and
// made-up data, every result read back and compared with the value computed here.
//   compute:  storage buffers, push and specialization constants, shared memory, function calls, loops, a switch,
//             subgroup and buffer atomics; storage images, texel buffers (uniform, storage with atomics), a dynamic
//             uniform buffer, a partially bound runtime array of images indexed non-uniformly
//   sampling: BC1, sRGB, B10G11R11, a mip chain made with blits, a depth comparison (D24S8 where the device has it),
//             a repeating sampler
//   drawing:  two color targets (sRGB, B10G11R11) at 4x MSAA resolved by dynamic rendering, an input attachment read
//             in a second subpass, a geometry shader writing gl_Layer of a two-layer target, a two-component output
//             to an RGBA8 target with gl_FragDepth written and no depth attachment; triangle fans (Metal has none)
//             drawn, indexed (16- and 32-bit, with primitive restart) and indirectly; the viewport's orientation (NDC
//             y = -1 at the top row) with a positive and a negative (maintenance1) height; image copies between
//             RG32_UINT and BC1; tessellation (triangle and quad patches, levels 1 and more, both windings culled)
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "vulkan/vulkan_core.h"
#include "vulkan_shaders/compute_basic.comp.h"
#include "vulkan_shaders/compute_images.comp.h"
#include "vulkan_shaders/compute_sample.comp.h"
#include "vulkan_shaders/fullscreen.vert.h"
#include "vulkan_shaders/color.frag.h"
#include "vulkan_shaders/mrt.frag.h"
#include "vulkan_shaders/solid.frag.h"
#include "vulkan_shaders/subpass.frag.h"
#include "vulkan_shaders/layer.geom.h"
#include "vulkan_shaders/narrow.frag.h"
#include "vulkan_shaders/position.vert.h"
#include "vulkan_shaders/tess.vert.h"
#include "vulkan_shaders/tess_tri.tesc.h"
#include "vulkan_shaders/tess_quad.tesc.h"
#include "vulkan_shaders/tess_tri_ccw.tese.h"
#include "vulkan_shaders/tess_tri_cw.tese.h"
#include "vulkan_shaders/tess_quad_ccw.tese.h"
#include "vulkan_shaders/tess_quad_cw.tese.h"

static int bad;
static const char *test = "";
#define CHECK(x) do { if (!(x)) { printf("vulkan_shaders: %s: check failed (line %d): %s\n", test, __LINE__, #x); bad++; } } while (0)

static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkFence fence;
static VkDescriptorPool dpool;

static uint32_t memory_type(uint32_t bits, VkFlags want) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) return i;
    return 0;
}

typedef struct { VkBuffer buffer; VkDeviceMemory memory; uint8_t *map; VkDeviceSize size; } buf;
static buf make_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    buf b = { .size = size };
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    CHECK(vkCreateBuffer(device, &info, NULL, &b.buffer) == VK_SUCCESS);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHECK(vkAllocateMemory(device, &alloc, NULL, &b.memory) == VK_SUCCESS);
    vkBindBufferMemory(device, b.buffer, b.memory, 0);
    vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, (void **)&b.map);
    memset(b.map, 0, size);
    return b;
}

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
static VkImageView make_view(VkImage image, VkImageViewType type, VkFormat format, VkImageAspectFlags aspect, uint32_t mips, uint32_t layers) {
    VkImageViewCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image, .viewType = type,
        .format = format, .subresourceRange = { aspect, 0, mips, 0, layers } };
    VkImageView v = 0;
    CHECK(vkCreateImageView(device, &info, NULL, &v) == VK_SUCCESS);
    return v;
}
static VkSampler make_sampler(VkFilter filter, VkSamplerAddressMode address, int compare) {
    VkSamplerCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = filter, .minFilter = filter,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = address, .addressModeV = address, .addressModeW = address,
        .compareEnable = compare != 0, .compareOp = VK_COMPARE_OP_LESS, .maxLod = 16 };
    VkSampler s = 0;
    CHECK(vkCreateSampler(device, &info, NULL, &s) == VK_SUCCESS);
    return s;
}
static VkShaderModule module(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m = 0;
    CHECK(vkCreateShaderModule(device, &info, NULL, &m) == VK_SUCCESS);
    return m;
}
#define MODULE(name) module(name, sizeof name)

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
// (images stay in GENERAL: valid for every use here)
static void to_general(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image,
        .subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void to_buffer(VkCommandBuffer cb, VkImage image, uint32_t mip, uint32_t layer, uint32_t w, uint32_t h, VkBuffer b, VkDeviceSize offset) {
    VkBufferImageCopy r = { .bufferOffset = offset, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1 }, .imageExtent = { w, h, 1 } };
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
}
static int near8(uint8_t a, int b) { return abs((int)a - b) <= 2; }
static int rgba(const uint8_t *p, int r, int g, int b, int a) { return near8(p[0], r) && near8(p[1], g) && near8(p[2], b) && near8(p[3], a); }
static int nearf(float a, float b, float tolerance) { return fabsf(a - b) <= tolerance; }

static VkDescriptorSetLayout set_layout(const VkDescriptorSetLayoutBinding *b, uint32_t n, const void *next) {
    VkDescriptorSetLayoutCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = next,
                                             .bindingCount = n, .pBindings = b };
    VkDescriptorSetLayout l = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &info, NULL, &l) == VK_SUCCESS);
    return l;
}
static VkPipelineLayout pipeline_layout(const VkDescriptorSetLayout *sets, uint32_t n, uint32_t push) {
    VkPushConstantRange range = { VK_SHADER_STAGE_ALL, 0, push };
    VkPipelineLayoutCreateInfo info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = n, .pSetLayouts = sets,
                                        .pushConstantRangeCount = push ? 1 : 0, .pPushConstantRanges = &range };
    VkPipelineLayout l = 0;
    CHECK(vkCreatePipelineLayout(device, &info, NULL, &l) == VK_SUCCESS);
    return l;
}
static VkDescriptorSet alloc_set(VkDescriptorSetLayout l) {
    VkDescriptorSetAllocateInfo info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dpool,
                                         .descriptorSetCount = 1, .pSetLayouts = &l };
    VkDescriptorSet s = 0;
    CHECK(vkAllocateDescriptorSets(device, &info, &s) == VK_SUCCESS);
    return s;
}
static void write_buffer(VkDescriptorSet s, uint32_t binding, VkDescriptorType type, VkBuffer b, VkDeviceSize offset, VkDeviceSize range) {
    VkDescriptorBufferInfo bi = { b, offset, range };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s, .dstBinding = binding,
                               .descriptorCount = 1, .descriptorType = type, .pBufferInfo = &bi };
    vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
}
static void write_image(VkDescriptorSet s, uint32_t binding, uint32_t element, VkDescriptorType type, VkImageView v, VkSampler sampler) {
    VkDescriptorImageInfo ii = { sampler, v, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s, .dstBinding = binding, .dstArrayElement = element,
                               .descriptorCount = 1, .descriptorType = type, .pImageInfo = &ii };
    vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
}
static void write_texel(VkDescriptorSet s, uint32_t binding, VkDescriptorType type, VkBufferView v) {
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s, .dstBinding = binding,
                               .descriptorCount = 1, .descriptorType = type, .pTexelBufferView = &v };
    vkUpdateDescriptorSets(device, 1, &w, 0, NULL);
}

static VkPipeline compute(VkShaderModule m, VkPipelineLayout layout, const VkSpecializationInfo *spec) {
    VkComputePipelineCreateInfo info = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = layout,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m,
                   .pName = "main", .pSpecializationInfo = spec } };
    VkPipeline p = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &info, NULL, &p) == VK_SUCCESS);
    return p;
}
// A pipeline drawing the vertex shader's triangle, for dynamic rendering (colors) or a render pass's subpass.
// (the input assembly and vertex input of the next pipeline made by graphics(); a triangle list without inputs by default)
static VkPrimitiveTopology g_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
static VkBool32 g_restart;
static const VkPipelineVertexInputStateCreateInfo *g_vertex_input;
static VkPipeline graphics(VkShaderModule vs, VkShaderModule gs, VkShaderModule fs, VkPipelineLayout layout, const VkFormat *colors,
                           uint32_t ncolors, VkSampleCountFlagBits samples, VkRenderPass rp, uint32_t subpass) {
    VkPipelineShaderStageCreateInfo stages[3] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = gs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = g_topology, .primitiveRestartEnable = g_restart };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = samples };
    VkPipelineColorBlendAttachmentState blend[2] = { { .colorWriteMask = 0xf }, { .colorWriteMask = 0xf } };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = ncolors, .pAttachments = blend };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = ncolors,
                                         .pColorAttachmentFormats = colors };
    VkGraphicsPipelineCreateInfo info = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = rp ? NULL : &ri,
        .stageCount = gs ? 3 : 2, .pStages = stages, .pVertexInputState = g_vertex_input ? g_vertex_input : &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = layout,
        .renderPass = rp, .subpass = subpass };
    VkPipeline p = 0;
    CHECK(vkCreateGraphicsPipelines(device, 0, 1, &info, NULL, &p) == VK_SUCCESS);
    return p;
}
static void viewport(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    VkViewport v = { 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D s = { { 0, 0 }, { w, h } };
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
}

// Small unsigned floats of B10G11R11 (5-bit exponent, bias 15, 6- or 5-bit mantissa), normal values only.
static uint32_t uf_encode(float v, int mbits) {
    int e = 0;
    float m = frexpf(v, &e);   // v = m * 2^e, m in [0.5, 1)
    return ((uint32_t)(e - 1 + 15) << mbits) | (uint32_t)lroundf((m * 2 - 1) * (float)(1 << mbits));
}
static float uf_decode(uint32_t bits, int mbits) {
    uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1);
    return e ? ldexpf(1 + (float)m / (float)(1 << mbits), (int)e - 15) : ldexpf((float)m / (float)(1 << mbits), -14);
}

// ---- compute: buffers, constants, control flow --------------------------------------------------------------------
static void compute_basic(void) {
    test = "compute basic";
    enum { N = 200 };
    struct item { float v[2]; uint32_t id; float w; };
    buf in = make_buffer(N * sizeof(struct item), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    buf out = make_buffer(16 + 256 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    struct item *items = (struct item *)in.map;
    for (uint32_t i = 0; i < N; i++) items[i] = (struct item){ { i * 0.5f, 1.0f }, i, 0.5f + (float)(i % 4) };
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT } };
    VkDescriptorSetLayout sl = set_layout(b, 2, NULL);
    VkPipelineLayout pl = pipeline_layout(&sl, 1, 8);
    uint32_t spec_data[2] = { 64, 3 };
    VkSpecializationMapEntry entries[2] = { { 0, 0, 4 }, { 1, 4, 4 } };
    VkSpecializationInfo spec = { 2, entries, sizeof spec_data, spec_data };
    VkPipeline p = compute(MODULE(spv_compute_basic_comp), pl, &spec);
    VkDescriptorSet s = alloc_set(sl);
    write_buffer(s, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, in.buffer, 0, VK_WHOLE_SIZE);
    write_buffer(s, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out.buffer, 0, VK_WHOLE_SIZE);
    VkCommandBuffer cb = begin();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s, 0, NULL);
    struct { float bias; uint32_t count; } pc = { 0.25f, N };
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_ALL, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (N + 63) / 64, 1, 1);
    submit(cb);
    // The same computation here.
    float expect[N];
    uint32_t counter = 0;
    for (uint32_t i = 0; i < N; i++) {
        float s = 0;
        for (int k = 0; k < 4; k++) {
            if (k == 2) continue;
            s += items[i].v[0] * (float)k - items[i].v[1];
            if (s > 1000) break;
        }
        if (i % 3 == 0) { s += 1; counter++; } else if (i % 3 == 1) s *= 2; else s -= 0.5f;
        expect[i] = s * items[i].w * 3 + 0.25f;
    }
    const float *r = (const float *)(out.map + 16);
    int wrong = 0;
    for (uint32_t i = 0; i < N; i++)
        if (!nearf(r[i * 4], expect[i], 1e-3f) || !nearf(r[i * 4 + 1], expect[i ^ 1], 1e-3f) || r[i * 4 + 2] != 1.0f ||
            r[i * 4 + 3] != (i % 3 == 0 ? 1.0f : 0.0f)) {
            if (!wrong++) printf("vulkan_shaders: item %u: %g %g %g %g, expected %g %g 1 %d\n", i, r[i * 4], r[i * 4 + 1], r[i * 4 + 2],
                                 r[i * 4 + 3], expect[i], expect[i ^ 1], i % 3 == 0);
        }
    CHECK(!wrong);
    CHECK(*(uint32_t *)out.map == counter);
    printf("vulkan_shaders: compute: %d items, function/loop/switch/shared/subgroup results %s, atomic counter %u (of %u)\n",
           N, wrong ? "WRONG" : "right", *(uint32_t *)out.map, counter);
}

// ---- compute: images, texel buffers, dynamic offsets, descriptor indexing ------------------------------------------
static void compute_images(void) {
    test = "compute images";
    enum { S = 16 };
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment < 16 ? 16 : props.limits.minUniformBufferOffsetAlignment;
    img out = make_image(VK_FORMAT_R8G8B8A8_UNORM, S, S, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    img accum = make_image(VK_FORMAT_R32_SFLOAT, S, S, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    img tex[3];
    VkImageView tex_view[3];
    for (int i = 0; i < 3; i++) {
        tex[i] = make_image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        tex_view[i] = make_view(tex[i].image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    }
    VkImageView out_view = make_view(out.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    VkImageView accum_view = make_view(accum.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    buf texels = make_buffer(S * 4, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT);
    for (int i = 0; i < S; i++) ((float *)texels.map)[i] = i * 0.25f;
    buf counts = make_buffer(16, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
    buf params = make_buffer(2 * align, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    const float tint[4] = { 1, 0.5f, 1, 1 };
    memcpy(params.map + align, tint, sizeof tint);
    buf readback = make_buffer(S * S * 8, 0);
    VkBufferView views[2];
    VkBufferViewCreateInfo bvi = { .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = texels.buffer, .format = VK_FORMAT_R32_SFLOAT,
                                   .range = VK_WHOLE_SIZE };
    CHECK(vkCreateBufferView(device, &bvi, NULL, &views[0]) == VK_SUCCESS);
    bvi.buffer = counts.buffer;
    bvi.format = VK_FORMAT_R32_UINT;
    CHECK(vkCreateBufferView(device, &bvi, NULL, &views[1]) == VK_SUCCESS);
    VkSampler sampler = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);

    VkDescriptorSetLayoutBinding b0[5] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        { 2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT },
        { 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT } };
    VkDescriptorSetLayoutBinding b1[2] = {
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4, VK_SHADER_STAGE_COMPUTE_BIT },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT } };
    VkDescriptorBindingFlags flags[2] = { VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT, 0 };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                       .bindingCount = 2, .pBindingFlags = flags };
    VkDescriptorSetLayout sl[2] = { set_layout(b0, 5, NULL), set_layout(b1, 2, &bf) };
    VkPipelineLayout pl = pipeline_layout(sl, 2, 0);
    VkPipeline p = compute(MODULE(spv_compute_images_comp), pl, NULL);
    VkDescriptorSet s0 = alloc_set(sl[0]), s1 = alloc_set(sl[1]);
    write_image(s0, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, out_view, 0);
    write_image(s0, 1, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, accum_view, 0);
    write_texel(s0, 2, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, views[0]);
    write_texel(s0, 3, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, views[1]);
    write_buffer(s0, 4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, params.buffer, 0, 16);
    for (int i = 0; i < 3; i++) write_image(s1, 0, (uint32_t)i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, tex_view[i], 0);   // (element 3 unbound)
    write_image(s1, 1, 0, VK_DESCRIPTOR_TYPE_SAMPLER, 0, sampler);

    VkCommandBuffer cb = begin();
    to_general(cb, out.image, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(cb, accum.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkClearColorValue one = { .float32 = { 1, 0, 0, 0 } };
    vkCmdClearColorImage(cb, accum.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
    const VkClearColorValue colors[3] = { { .float32 = { 1, 0, 0, 1 } }, { .float32 = { 0, 1, 0, 1 } }, { .float32 = { 0, 0, 1, 1 } } };
    for (int i = 0; i < 3; i++) {
        to_general(cb, tex[i].image, VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdClearColorImage(cb, tex[i].image, VK_IMAGE_LAYOUT_GENERAL, &colors[i], 1, &range);
    }
    barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    VkDescriptorSet sets[2] = { s0, s1 };
    uint32_t dyn = (uint32_t)align;
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, sets, 1, &dyn);
    vkCmdDispatch(cb, S / 8, S / 8, 1);
    barrier(cb);
    to_buffer(cb, out.image, 0, 0, S, S, readback.buffer, 0);
    to_buffer(cb, accum.image, 0, 0, S, S, readback.buffer, S * S * 4);
    submit(cb);
    int wrong_color = 0, wrong_accum = 0;
    uint32_t expect_counts[3] = { 0 };
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            int k = (x + y) % 3;
            expect_counts[k]++;
            const uint8_t *c = readback.map + (y * S + x) * 4;
            if (!rgba(c, k == 0 ? 255 : 0, k == 1 ? 128 : 0, k == 2 ? 255 : 0, 255)) {
                if (!wrong_color++) printf("vulkan_shaders: pixel %d,%d: %d %d %d %d\n", x, y, c[0], c[1], c[2], c[3]);
            }
            float a = ((const float *)(readback.map + S * S * 4))[y * S + x];
            if (!nearf(a, 1 + x * 0.25f, 1e-5f) && !wrong_accum++) printf("vulkan_shaders: accum %d,%d: %g\n", x, y, a);
        }
    const uint32_t *got = (const uint32_t *)counts.map;
    CHECK(!wrong_color);
    CHECK(!wrong_accum);
    CHECK(got[0] == expect_counts[0] && got[1] == expect_counts[1] && got[2] == expect_counts[2]);
    printf("vulkan_shaders: images: non-uniformly indexed textures x dynamic uniform tint %s, storage image read-modify-write %s, "
           "texel buffer atomics %u %u %u (expected %u %u %u)\n", wrong_color ? "WRONG" : "right", wrong_accum ? "WRONG" : "right",
           got[0], got[1], got[2], expect_counts[0], expect_counts[1], expect_counts[2]);
}

// ---- sampling: formats, mips, comparison, addressing ----------------------------------------------------------------
static void sampling(void) {
    test = "sampling";
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    int d24 = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
              (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    VkFormat depth_format = d24 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT;
    const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img bc1 = make_image(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    img srgb = make_image(VK_FORMAT_R8G8B8A8_SRGB, 2, 2, 1, 1, VK_SAMPLE_COUNT_1_BIT, u);
    img packedf = make_image(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 2, 2, 1, 1, VK_SAMPLE_COUNT_1_BIT, u);
    img mips = make_image(VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 4, 1, VK_SAMPLE_COUNT_1_BIT, u);
    img depth = make_image(depth_format, 4, 4, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    img rep = make_image(VK_FORMAT_R8G8B8A8_UNORM, 2, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, u);
    VkImageView v[6] = {
        make_view(bc1.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1),
        make_view(srgb.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1),
        make_view(packedf.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1),
        make_view(mips.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 4, 1),
        make_view(depth.image, VK_IMAGE_VIEW_TYPE_2D, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1),
        make_view(rep.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1) };
    VkSampler nearest = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);
    VkSampler linear = make_sampler(VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);
    VkSampler shadow = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1);
    VkSampler repeat = make_sampler(VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0);

    // The data: a BC1 block red on its left half and blue on its right (colors 0xf800 and 0x001f, indices 0 and 1),
    // sRGB texels (0x80, 0x40, 0xff, 0xff), B10G11R11 texels (2.5, 0.125, 64), an 8x8 black and white checkerboard,
    // and red, green.
    buf up = make_buffer(1024, 0);
    const uint8_t block[8] = { 0x00, 0xf8, 0x1f, 0x00, 0x50, 0x50, 0x50, 0x50 };
    memcpy(up.map, block, 8);
    for (int i = 0; i < 4; i++) { uint8_t t[4] = { 0x80, 0x40, 0xff, 0xff }; memcpy(up.map + 64 + i * 4, t, 4); }
    uint32_t packed = uf_encode(2.5f, 6) | uf_encode(0.125f, 6) << 11 | uf_encode(64, 5) << 22;
    for (int i = 0; i < 4; i++) memcpy(up.map + 128 + i * 4, &packed, 4);
    for (int i = 0; i < 64; i++) { uint8_t c = ((i % 8) + (i / 8)) % 2 ? 255 : 0; uint8_t t[4] = { c, c, c, 255 }; memcpy(up.map + 256 + i * 4, t, 4); }
    const uint8_t rg[8] = { 255, 0, 0, 255, 0, 255, 0, 255 };
    memcpy(up.map + 512, rg, 8);
    buf out = make_buffer(7 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    VkDescriptorSetLayoutBinding b[7];
    for (uint32_t i = 0; i < 6; i++) b[i] = (VkDescriptorSetLayoutBinding){ i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
    b[6] = (VkDescriptorSetLayoutBinding){ 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayout sl = set_layout(b, 7, NULL);
    VkPipelineLayout pl = pipeline_layout(&sl, 1, 0);
    VkPipeline p = compute(MODULE(spv_compute_sample_comp), pl, NULL);
    VkDescriptorSet s = alloc_set(sl);
    const VkSampler samplers[6] = { nearest, nearest, nearest, linear, shadow, repeat };
    for (uint32_t i = 0; i < 6; i++) write_image(s, i, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, v[i], samplers[i]);
    write_buffer(s, 6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, out.buffer, 0, VK_WHOLE_SIZE);

    VkCommandBuffer cb = begin();
    to_general(cb, bc1.image, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(cb, srgb.image, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(cb, packedf.image, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(cb, mips.image, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(cb, depth.image, VK_IMAGE_ASPECT_DEPTH_BIT | (d24 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0));
    to_general(cb, rep.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 4, 4, 1 } };
    vkCmdCopyBufferToImage(cb, up.buffer, bc1.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    r.bufferOffset = 64; r.imageExtent = (VkExtent3D){ 2, 2, 1 };
    vkCmdCopyBufferToImage(cb, up.buffer, srgb.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    r.bufferOffset = 128;
    vkCmdCopyBufferToImage(cb, up.buffer, packedf.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    r.bufferOffset = 256; r.imageExtent = (VkExtent3D){ 8, 8, 1 };
    vkCmdCopyBufferToImage(cb, up.buffer, mips.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    r.bufferOffset = 512; r.imageExtent = (VkExtent3D){ 2, 1, 1 };
    vkCmdCopyBufferToImage(cb, up.buffer, rep.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    VkClearDepthStencilValue half = { 0.5f, 0 };
    VkImageSubresourceRange dr = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    vkCmdClearDepthStencilImage(cb, depth.image, VK_IMAGE_LAYOUT_GENERAL, &half, 1, &dr);
    // (the mip chain, each level blitted from the one above with a linear filter)
    for (int32_t level = 1; level < 4; level++) {
        barrier(cb);
        int32_t sw = 8 >> (level - 1), dw = 8 >> level;
        VkImageBlit blit = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level - 1, 0, 1 }, .srcOffsets = { { 0, 0, 0 }, { sw, sw, 1 } },
                             .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1 }, .dstOffsets = { { 0, 0, 0 }, { dw, dw, 1 } } };
        vkCmdBlitImage(cb, mips.image, VK_IMAGE_LAYOUT_GENERAL, mips.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    }
    barrier(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &s, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    submit(cb);
    const float *o = (const float *)out.map;
    CHECK(nearf(o[0], 1, 0.01f) && nearf(o[1], 0, 0.01f) && nearf(o[2], 0, 0.01f) && nearf(o[3], 1, 0.01f));      // BC1 red
    CHECK(nearf(o[4], 0, 0.01f) && nearf(o[5], 0, 0.01f) && nearf(o[6], 1, 0.01f) && nearf(o[7], 1, 0.01f));      // BC1 blue
    CHECK(nearf(o[8], 0.2159f, 0.005f) && nearf(o[9], 0.0513f, 0.005f) && nearf(o[10], 1, 0.005f));            // sRGB decoded
    CHECK(nearf(o[12], 2.5f, 0.01f) && nearf(o[13], 0.125f, 0.001f) && nearf(o[14], 64, 0.01f));              // B10G11R11
    CHECK(nearf(o[16], 0.5f, 0.02f) && nearf(o[17], 0.5f, 0.02f) && nearf(o[19], 1, 0.01f));                   // mip 3 (1x1)
    CHECK(o[20] == 1.0f && o[21] == 0.0f);                                                                      // 0.25 < 0.5, 0.75 not
    CHECK(nearf(o[24], 1, 0.01f) && nearf(o[25], 0, 0.01f));                                                    // u 1.25 repeats to red
    printf("vulkan_shaders: sampling: BC1 %.2f %.2f %.2f | %.2f %.2f %.2f, sRGB %.4f %.4f, B10G11R11 %g %g %g, mip 3 of a blitted chain %.3f, "
           "%s compare %g %g, repeat %.2f %.2f\n", o[0], o[1], o[2], o[4], o[5], o[6], o[8], o[9], o[12], o[13], o[14], o[16],
           d24 ? "D24S8" : "D32 (no D24S8)", o[20], o[21], o[24], o[25]);
}

// ---- drawing: MRT with MSAA resolve (dynamic rendering) -------------------------------------------------------------
static void mrt_msaa(void) {
    test = "mrt msaa";
    enum { S = 8 };
    const VkFormat formats[2] = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B10G11R11_UFLOAT_PACK32 };
    img ms[2], res[2];
    VkImageView msv[2], resv[2];
    for (int i = 0; i < 2; i++) {
        ms[i] = make_image(formats[i], S, S, 1, 1, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        res[i] = make_image(formats[i], S, S, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        msv[i] = make_view(ms[i].image, VK_IMAGE_VIEW_TYPE_2D, formats[i], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
        resv[i] = make_view(res[i].image, VK_IMAGE_VIEW_TYPE_2D, formats[i], VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    }
    VkPipelineLayout pl = pipeline_layout(NULL, 0, 0);
    VkPipeline p = graphics(MODULE(spv_fullscreen_vert), 0, MODULE(spv_mrt_frag), pl, formats, 2, VK_SAMPLE_COUNT_4_BIT, 0, 0);
    buf readback = make_buffer(2 * S * S * 4, 0);
    VkCommandBuffer cb = begin();
    VkRenderingAttachmentInfo a[2];
    for (int i = 0; i < 2; i++) {
        to_general(cb, ms[i].image, VK_IMAGE_ASPECT_COLOR_BIT);
        to_general(cb, res[i].image, VK_IMAGE_ASPECT_COLOR_BIT);
        a[i] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = msv[i],
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT, .resolveImageView = resv[i],
            .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE };
    }
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { S, S } }, .layerCount = 1,
                           .colorAttachmentCount = 2, .pColorAttachments = a };
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    viewport(cb, S, S);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb);
    to_buffer(cb, res[0].image, 0, 0, S, S, readback.buffer, 0);
    to_buffer(cb, res[1].image, 0, 0, S, S, readback.buffer, S * S * 4);
    submit(cb);
    const uint8_t *c = readback.map + (3 * S + 5) * 4;
    uint32_t packed;
    memcpy(&packed, readback.map + S * S * 4 + (3 * S + 5) * 4, 4);
    float r = uf_decode(packed & 0x7ff, 6), g = uf_decode((packed >> 11) & 0x7ff, 6), b = uf_decode(packed >> 22, 5);
    // (0.5, 0.25, 1) stored as sRGB: 188, 137, 255)
    CHECK(rgba(c, 188, 137, 255, 255));
    CHECK(nearf(r, 2.5f, 0.05f) && nearf(g, 0.125f, 0.002f) && nearf(b, 64, 1));
    printf("vulkan_shaders: two targets at 4x resolved: sRGB %d %d %d %d, B10G11R11 %g %g %g\n", c[0], c[1], c[2], c[3], r, g, b);
}

// ---- drawing: an input attachment in a second subpass ---------------------------------------------------------------
static void subpasses(void) {
    test = "subpasses";
    enum { S = 8 };
    img a = make_image(VK_FORMAT_R8G8B8A8_UNORM, S, S, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    img b = make_image(VK_FORMAT_R8G8B8A8_UNORM, S, S, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageView views[2] = { make_view(a.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1),
                             make_view(b.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1) };
    VkAttachmentDescription att[2] = {
        { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE,
          VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL },
        { 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
          VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL } };
    VkAttachmentReference a_out = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, a_in = { 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                          b_out = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp[2] = {
        { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1, .pColorAttachments = &a_out },
        { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .inputAttachmentCount = 1, .pInputAttachments = &a_in,
          .colorAttachmentCount = 1, .pColorAttachments = &b_out } };
    VkSubpassDependency dep = { 0, 1, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT };
    VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 2, .pSubpasses = sp, .dependencyCount = 1, .pDependencies = &dep };
    VkRenderPass rp = 0;
    CHECK(vkCreateRenderPass(device, &rpi, NULL, &rp) == VK_SUCCESS);
    VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = 2,
                                    .pAttachments = views, .width = S, .height = S, .layers = 1 };
    VkFramebuffer fb = 0;
    CHECK(vkCreateFramebuffer(device, &fbi, NULL, &fb) == VK_SUCCESS);
    VkDescriptorSetLayoutBinding bind = { 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayout sl = set_layout(&bind, 1, NULL);
    VkPipelineLayout empty = pipeline_layout(NULL, 0, 0), pl = pipeline_layout(&sl, 1, 0);
    VkShaderModule vs = MODULE(spv_fullscreen_vert);
    VkPipeline p0 = graphics(vs, 0, MODULE(spv_solid_frag), empty, NULL, 1, VK_SAMPLE_COUNT_1_BIT, rp, 0);
    VkPipeline p1 = graphics(vs, 0, MODULE(spv_subpass_frag), pl, NULL, 1, VK_SAMPLE_COUNT_1_BIT, rp, 1);
    VkDescriptorSet s = alloc_set(sl);
    write_image(s, 0, 0, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, views[0], 0);
    buf readback = make_buffer(S * S * 4, 0);
    VkCommandBuffer cb = begin();
    VkClearValue clears[2] = { { .color = { .float32 = { 0, 0, 0, 1 } } }, { .color = { .float32 = { 0, 0, 0, 1 } } } };
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                 .renderArea = { { 0, 0 }, { S, S } }, .clearValueCount = 2, .pClearValues = clears };
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    viewport(cb, S, S);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p0);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p1);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &s, 0, NULL);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    barrier(cb);
    to_buffer(cb, b.image, 0, 0, S, S, readback.buffer, 0);
    submit(cb);
    const uint8_t *c = readback.map + (4 * S + 4) * 4;
    CHECK(rgba(c, 191, 128, 64, 255));   // (0.25, 0.5, 0.75) read back as .bgra
    printf("vulkan_shaders: input attachment in subpass 1: %d %d %d %d\n", c[0], c[1], c[2], c[3]);
}

// ---- drawing: a geometry shader choosing the layer ------------------------------------------------------------------
static void geometry_layer(void) {
    test = "geometry layer";
    enum { S = 8 };
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    img target = make_image(format, S, S, 1, 2, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageView view = make_view(target.image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 2);
    VkPipelineLayout pl = pipeline_layout(NULL, 0, 0);
    VkPipeline p = graphics(MODULE(spv_fullscreen_vert), MODULE(spv_layer_geom), MODULE(spv_color_frag), pl, &format, 1,
                            VK_SAMPLE_COUNT_1_BIT, 0, 0);
    buf readback = make_buffer(2 * S * S * 4, 0);
    VkCommandBuffer cb = begin();
    to_general(cb, target.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderingAttachmentInfo a = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 1, 0, 1 } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { S, S } }, .layerCount = 2,
                           .colorAttachmentCount = 1, .pColorAttachments = &a };
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    viewport(cb, S, S);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb);
    to_buffer(cb, target.image, 0, 0, S, S, readback.buffer, 0);
    to_buffer(cb, target.image, 0, 1, S, S, readback.buffer, S * S * 4);
    submit(cb);
    const uint8_t *l0 = readback.map + (4 * S + 4) * 4, *l1 = readback.map + S * S * 4 + (4 * S + 4) * 4;
    CHECK(rgba(l0, 0, 255, 0, 255));   // layer 0: the clear color
    CHECK(rgba(l1, 0, 0, 255, 255));   // layer 1: red as .bgra
    printf("vulkan_shaders: geometry shader to gl_Layer 1: layer 0 %d %d %d, layer 1 %d %d %d\n", l0[0], l0[1], l0[2], l1[0], l1[1], l1[2]);
}

// ---- drawing: fewer output components than the target has, FragDepth with no depth attachment ----------------------
static void narrow_output(void) {
    test = "narrow output";
    enum { S = 4 };
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    img target = make_image(format, S, S, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageView view = make_view(target.image, VK_IMAGE_VIEW_TYPE_2D, format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    VkPipelineLayout pl = pipeline_layout(NULL, 0, 0);
    VkPipeline p = graphics(MODULE(spv_fullscreen_vert), 0, MODULE(spv_narrow_frag), pl, &format, 1, VK_SAMPLE_COUNT_1_BIT, 0, 0);
    if (!p) return;
    buf readback = make_buffer(S * S * 4, 0);
    VkCommandBuffer cb = begin();
    to_general(cb, target.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderingAttachmentInfo a = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 0, 0, 0 } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { S, S } }, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &a };
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    viewport(cb, S, S);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb);
    to_buffer(cb, target.image, 0, 0, S, S, readback.buffer, 0);
    submit(cb);
    const uint8_t *c = readback.map + (2 * S + 1) * 4;
    CHECK(c[0] >= 63 && c[0] <= 64 && c[1] >= 191 && c[1] <= 192);   // red and green as written (blue, alpha undefined)
    printf("vulkan_shaders: vec2 to RGBA8 with FragDepth and no depth attachment: %d %d\n", c[0], c[1]);
}

// ---- drawing: triangle fans, made triangle lists for Metal ---------------------------------------------------------
// The fan's vertices: a centre and the target's four corners around it (the first again at the end), after two
// unused ones. Drawn as a fan it covers the whole target; as a list or a strip it would leave parts of it.
static void fan_case(VkPipeline p, VkImageView view, VkImage image, buf *readback, buf *vb, buf *ib, VkIndexType it, buf *args,
                     int indexed, uint32_t count, uint32_t first, int32_t offset) {
    enum { S = 8 };
    VkCommandBuffer cb = begin();
    to_general(cb, image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderingAttachmentInfo a = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 0, 0, 0 } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { S, S } }, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &a };
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    viewport(cb, S, S);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vb->buffer, &zero);
    if (indexed) vkCmdBindIndexBuffer(cb, ib->buffer, 0, it);
    if (args) vkCmdDrawIndirect(cb, args->buffer, 0, 1, 16);
    else if (indexed) vkCmdDrawIndexed(cb, count, 1, first, offset, 0);
    else vkCmdDraw(cb, count, 1, first, 0);
    vkCmdEndRendering(cb);
    barrier(cb);
    to_buffer(cb, image, 0, 0, S, S, readback->buffer, 0);
    submit(cb);
}
static int fan_covered(const buf *readback, int x, int y) { return rgba(readback->map + (y * 8 + x) * 4, 0, 255, 0, 255); }
static void triangle_fans(void) {
    test = "triangle fans";
    enum { S = 8 };
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    img target = make_image(format, S, S, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageView view = make_view(target.image, VK_IMAGE_VIEW_TYPE_2D, format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    VkPipelineLayout pl = pipeline_layout(NULL, 0, 0);
    VkVertexInputBindingDescription bind = { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attr = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind, .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attr };
    g_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; g_vertex_input = &vi;
    VkPipeline p = graphics(MODULE(spv_position_vert), 0, MODULE(spv_color_frag), pl, &format, 1, VK_SAMPLE_COUNT_1_BIT, 0, 0);
    g_restart = 1;
    VkPipeline pr = graphics(MODULE(spv_position_vert), 0, MODULE(spv_color_frag), pl, &format, 1, VK_SAMPLE_COUNT_1_BIT, 0, 0);
    g_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; g_restart = 0; g_vertex_input = NULL;
    if (!p || !pr) return;
    static const float verts[] = { 0.5f, 0.5f, 0.5f, 0.5f, 0, 0, -1, -1, 1, -1, 1, 1, -1, 1, -1, -1 };
    buf vb = make_buffer(sizeof verts, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(vb.map, verts, sizeof verts);
    buf readback = make_buffer(S * S * 4, 0);
    int all;

    // 1. Not indexed, from the third vertex.
    fan_case(p, view, target.image, &readback, &vb, NULL, 0, NULL, 0, 6, 2, 0);
    all = 1; for (int i = 0; i < S * S; i++) all &= fan_covered(&readback, i % S, i / S);
    CHECK(all);
    printf("vulkan_shaders: fan of 6, not indexed: %s\n", all ? "the whole target" : "not all of it");

    // 2. 16-bit indices from the third, with a vertex offset of 2.
    static const uint16_t i16[] = { 7, 7, 0, 1, 2, 3, 4, 5 };
    buf ib16 = make_buffer(sizeof i16, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    memcpy(ib16.map, i16, sizeof i16);
    fan_case(p, view, target.image, &readback, &vb, &ib16, VK_INDEX_TYPE_UINT16, NULL, 1, 6, 2, 2);
    all = 1; for (int i = 0; i < S * S; i++) all &= fan_covered(&readback, i % S, i / S);
    CHECK(all);

    // 3. 32-bit indices with primitive restart: two fans, the top and bottom quarters of the target only.
    static const uint32_t i32[] = { 2, 3, 4, 0xffffffffu, 2, 5, 6 };
    buf ib32 = make_buffer(sizeof i32, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    memcpy(ib32.map, i32, sizeof i32);
    fan_case(pr, view, target.image, &readback, &vb, &ib32, VK_INDEX_TYPE_UINT32, NULL, 1, 7, 0, 0);
    CHECK(fan_covered(&readback, 3, 0)); CHECK(fan_covered(&readback, 4, 7));        // top, bottom
    CHECK(!fan_covered(&readback, 0, 3)); CHECK(!fan_covered(&readback, 7, 4));      // left, right
    printf("vulkan_shaders: fans split by primitive restart: top %d bottom %d left %d right %d\n", fan_covered(&readback, 3, 0),
           fan_covered(&readback, 4, 7), fan_covered(&readback, 0, 3), fan_covered(&readback, 7, 4));

    // 4. Indirect: the arguments of draw 1.
    buf args = make_buffer(16, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    const uint32_t a[4] = { 6, 1, 2, 0 };
    memcpy(args.map, a, sizeof a);
    fan_case(p, view, target.image, &readback, &vb, NULL, 0, &args, 0, 0, 0, 0);
    all = 1; for (int i = 0; i < S * S; i++) all &= fan_covered(&readback, i % S, i / S);
    CHECK(all);
}

// ---- drawing: which way up -----------------------------------------------------------------------------------------
// Vulkan's viewport maps NDC y = -1 to its y and y = +1 to y + height (framebuffer rows grow downwards). The upper half
// of NDC (y from -1 to 0) lands in the top rows with a positive height; with a negative height (y = S, height = -S,
// VK_KHR_maintenance1 / Vulkan 1.1) the same geometry lands in the bottom rows.
static void orientation(void) {
    test = "orientation";
    enum { S = 8 };
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    img target = make_image(format, S, S, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageView view = make_view(target.image, VK_IMAGE_VIEW_TYPE_2D, format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    VkPipelineLayout pl = pipeline_layout(NULL, 0, 0);
    VkVertexInputBindingDescription bind = { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attr = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind, .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attr };
    g_vertex_input = &vi;
    VkPipeline p = graphics(MODULE(spv_position_vert), 0, MODULE(spv_color_frag), pl, &format, 1, VK_SAMPLE_COUNT_1_BIT, 0, 0);
    g_vertex_input = NULL;
    if (!p) return;
    static const float upper[] = { -1, -1, 1, -1, 1, 0, -1, -1, 1, 0, -1, 0 };   // NDC y from -1 to 0, both triangles
    buf vb = make_buffer(sizeof upper, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(vb.map, upper, sizeof upper);
    buf readback = make_buffer(S * S * 4, 0);
    for (int negative = 0; negative < 2; negative++) {
        VkCommandBuffer cb = begin();
        to_general(cb, target.image, VK_IMAGE_ASPECT_COLOR_BIT);
        VkRenderingAttachmentInfo a = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 0, 0, 0 } } } };
        VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { S, S } }, .layerCount = 1,
                               .colorAttachmentCount = 1, .pColorAttachments = &a };
        vkCmdBeginRendering(cb, &ri);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        VkViewport v = negative ? (VkViewport){ 0, S, S, -(float)S, 0, 1 } : (VkViewport){ 0, 0, S, S, 0, 1 };
        VkRect2D sc = { { 0, 0 }, { S, S } };
        vkCmdSetViewport(cb, 0, 1, &v);
        vkCmdSetScissor(cb, 0, 1, &sc);
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &vb.buffer, &zero);
        vkCmdDraw(cb, 6, 1, 0, 0);
        vkCmdEndRendering(cb);
        barrier(cb);
        to_buffer(cb, target.image, 0, 0, S, S, readback.buffer, 0);
        submit(cb);
        int top = fan_covered(&readback, 3, 1), bottom = fan_covered(&readback, 3, 6);
        CHECK(top == !negative); CHECK(bottom == negative);
        printf("vulkan_shaders: NDC's upper half, %s viewport height: top row %s, bottom row %s\n", negative ? "negative" : "positive",
               top ? "drawn" : "empty", bottom ? "drawn" : "empty");
    }
}

// ---- copies between a compressed and an uncompressed image ---------------------------------------------------------
// vkCmdCopyImage with size-compatible formats: one RG32_UINT texel is one BC1 block (8 bytes each). A 2x2 RG32_UINT
// image of made-up values becomes the top-left 2x2 blocks of an 8x8 BC1 image, read back to a buffer; then
// the BC1 blocks are copied into another RG32_UINT image (the extent in BC1 texels). No Man's Sky copies so.
static void compressed_copies(void) {
    test = "compressed copies";
    const VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img plain = make_image(VK_FORMAT_R32G32_UINT, 2, 2, 1, 1, VK_SAMPLE_COUNT_1_BIT, u);
    img bc = make_image(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 8, 1, 1, VK_SAMPLE_COUNT_1_BIT, u);
    img back = make_image(VK_FORMAT_R32G32_UINT, 2, 2, 1, 1, VK_SAMPLE_COUNT_1_BIT, u);
    buf data = make_buffer(32, 0), out_bc = make_buffer(4 * 8, 0), out_back = make_buffer(32, 0);
    uint32_t v[8];
    for (int i = 0; i < 8; i++) v[i] = 0x9e3779b9u * (uint32_t)(i + 1);
    memcpy(data.map, v, sizeof v);
    VkCommandBuffer cb = begin();
    to_general(cb, plain.image, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(cb, bc.image, VK_IMAGE_ASPECT_COLOR_BIT);
    to_general(cb, back.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy up = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 2, 2, 1 } };
    vkCmdCopyBufferToImage(cb, data.buffer, plain.image, VK_IMAGE_LAYOUT_GENERAL, 1, &up);
    barrier(cb);
    // RG32 texels (0..1, 0..1) -> BC1 texels (0..7, 0..7): one block per texel
    VkImageCopy to_bc = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                          .dstOffset = { 0, 0, 0 }, .extent = { 2, 2, 1 } };
    vkCmdCopyImage(cb, plain.image, VK_IMAGE_LAYOUT_GENERAL, bc.image, VK_IMAGE_LAYOUT_GENERAL, 1, &to_bc);
    barrier(cb);
    VkBufferImageCopy down_bc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 8, 8, 1 } };
    vkCmdCopyImageToBuffer(cb, bc.image, VK_IMAGE_LAYOUT_GENERAL, out_bc.buffer, 1, &down_bc);
    // BC1 texels (0..7, 0..7) -> RG32 texels (0..1, 0..1): the extent is the source's, 8x8
    VkImageCopy from_bc = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .extent = { 8, 8, 1 } };
    vkCmdCopyImage(cb, bc.image, VK_IMAGE_LAYOUT_GENERAL, back.image, VK_IMAGE_LAYOUT_GENERAL, 1, &from_bc);
    barrier(cb);
    VkBufferImageCopy down = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { 2, 2, 1 } };
    vkCmdCopyImageToBuffer(cb, back.image, VK_IMAGE_LAYOUT_GENERAL, out_back.buffer, 1, &down);
    submit(cb);
    CHECK(memcmp(out_bc.map, v, sizeof v) == 0);       // the four blocks, row by row, are the four texels
    CHECK(memcmp(out_back.map, v, sizeof v) == 0);
    printf("vulkan_shaders: RG32_UINT <-> BC1 image copies: %s, %s\n", memcmp(out_bc.map, v, sizeof v) ? "wrong" : "blocks as texels",
           memcmp(out_back.map, v, sizeof v) ? "wrong" : "back again");
}

// ---- drawing: tessellation -----------------------------------------------------------------------------------------
// Triangle patches (equal spacing) and quad patches (fractional odd), the level from a push constant, drawn with back
// faces culled (front: counter-clockwise). The evaluation shaders bow one edge in with a bump that only tessellated
// patches show: with level 1 the patch keeps its straight edges. Vulkan's tessellation domain has its origin at the
// upper left: "ccw" makes triangles counter-clockwise as seen there, so the triangle patch (corners top left, top
// right, bottom left on the screen; the generated triangles wind the other way to their corners u, v, w) and the quad
// patch (u to the right, v down) are front-facing with ccw and culled with cw.
static VkPipeline tess_pipeline(VkShaderModule vs, VkShaderModule tcs, VkShaderModule tes, VkShaderModule fs, VkPipelineLayout layout,
                                VkFormat format, uint32_t control_points) {
    VkPipelineShaderStageCreateInfo stages[4] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, .module = tcs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, .module = tes, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkVertexInputBindingDescription bind = { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attr = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &bind, .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attr };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST };
    VkPipelineTessellationStateCreateInfo ts = { .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, .patchControlPoints = control_points };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_BACK_BIT, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = 1 };
    VkPipelineColorBlendAttachmentState blend = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &blend };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &format };
    VkGraphicsPipelineCreateInfo info = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 4, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pTessellationState = &ts, .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = layout };
    VkPipeline p = 0;
    CHECK(vkCreateGraphicsPipelines(device, 0, 1, &info, NULL, &p) == VK_SUCCESS);
    return p;
}
static void tess_draw(VkPipeline p, VkPipelineLayout pl, VkImageView view, VkImage image, buf *vb, uint32_t vertices, float level, buf *readback) {
    enum { S = 16 };
    VkCommandBuffer cb = begin();
    to_general(cb, image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderingAttachmentInfo a = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 0, 0, 0 } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { S, S } }, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &a };
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    viewport(cb, S, S);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vb->buffer, &zero);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_ALL, 0, 4, &level);
    vkCmdDraw(cb, vertices, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb);
    to_buffer(cb, image, 0, 0, S, S, readback->buffer, 0);
    submit(cb);
}
static int tess_px(const buf *readback, int x, int y) { return rgba(readback->map + (y * 16 + x) * 4, 0, 255, 0, 255); }
static void tessellation(void) {
    test = "tessellation";
    enum { S = 16 };
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    img target = make_image(format, S, S, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageView view = make_view(target.image, VK_IMAGE_VIEW_TYPE_2D, format, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1);
    VkPipelineLayout pl = pipeline_layout(NULL, 0, 4);
    VkShaderModule vs = MODULE(spv_tess_vert), fs = MODULE(spv_color_frag);
    VkPipeline tri_ccw = tess_pipeline(vs, MODULE(spv_tess_tri_tesc), MODULE(spv_tess_tri_ccw_tese), fs, pl, format, 3);
    VkPipeline tri_cw = tess_pipeline(vs, MODULE(spv_tess_tri_tesc), MODULE(spv_tess_tri_cw_tese), fs, pl, format, 3);
    VkPipeline quad_ccw = tess_pipeline(vs, MODULE(spv_tess_quad_tesc), MODULE(spv_tess_quad_ccw_tese), fs, pl, format, 4);
    VkPipeline quad_cw = tess_pipeline(vs, MODULE(spv_tess_quad_tesc), MODULE(spv_tess_quad_cw_tese), fs, pl, format, 4);
    if (!tri_ccw || !tri_cw || !quad_ccw || !quad_cw) return;
    static const float tri[] = { -1, -1, 1, -1, -1, 1 }, quad[] = { -1, -1, 1, -1, 1, 1, -1, 1 };
    buf tvb = make_buffer(sizeof tri, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT), qvb = make_buffer(sizeof quad, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(tvb.map, tri, sizeof tri);
    memcpy(qvb.map, quad, sizeof quad);
    buf rb = make_buffer(S * S * 4, 0);
    // Triangles: (2, 6) inside in every case, (8, 1) near the middle of the top edge (bowed down past it when tessellated).
    tess_draw(tri_ccw, pl, view, target.image, &tvb, 3, 1.0f, &rb);
    int t1_in = tess_px(&rb, 2, 6), t1_edge = tess_px(&rb, 8, 1), t1_out = tess_px(&rb, 13, 13);
    tess_draw(tri_ccw, pl, view, target.image, &tvb, 3, 8.0f, &rb);
    int t8_in = tess_px(&rb, 2, 6), t8_edge = tess_px(&rb, 8, 1);
    tess_draw(tri_cw, pl, view, target.image, &tvb, 3, 8.0f, &rb);
    int tcw_in = tess_px(&rb, 2, 6);
    CHECK(t1_in && t1_edge && !t1_out); CHECK(t8_in && !t8_edge); CHECK(!tcw_in);
    printf("vulkan_shaders: tessellated triangle: level 1 inside %d edge %d outside %d, level 8 inside %d edge %d, cw culled %d\n",
           t1_in, t1_edge, t1_out, t8_in, t8_edge, !tcw_in);
    // Quads: (8, 12) inside, (8, 1) near the top edge's middle.
    tess_draw(quad_ccw, pl, view, target.image, &qvb, 4, 1.0f, &rb);
    int q1_in = tess_px(&rb, 8, 12), q1_edge = tess_px(&rb, 8, 1), q1_corner = tess_px(&rb, 0, 0) && tess_px(&rb, 15, 15);
    tess_draw(quad_ccw, pl, view, target.image, &qvb, 4, 7.0f, &rb);
    int q7_in = tess_px(&rb, 8, 12), q7_edge = tess_px(&rb, 8, 1);
    tess_draw(quad_cw, pl, view, target.image, &qvb, 4, 7.0f, &rb);
    int qcw_in = tess_px(&rb, 8, 12);
    CHECK(q1_in && q1_edge && q1_corner); CHECK(q7_in && !q7_edge); CHECK(!qcw_in);
    printf("vulkan_shaders: tessellated quad: level 1 inside %d edge %d corners %d, level 7 inside %d edge %d, cw culled %d\n",
           q1_in, q1_edge, q1_corner, q7_in, q7_edge, !qcw_in);

    // Several tessellation draws in one pass, with other state between them (the runtime runs their kernels before the
    // pass, each into its own place: vk_command.m, hoist_tessellation): the left half a triangle at level 8 (its top
    // edge bowed in), the right half a quad at level 1 (straight edges), each with its own vertex buffer, push
    // constant and viewport.
    VkCommandBuffer cb = begin();
    to_general(cb, target.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VkRenderingAttachmentInfo a = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue = { .color = { .float32 = { 0, 0, 0, 0 } } } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { S, S } }, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &a };
    vkCmdBeginRendering(cb, &ri);
    VkDeviceSize zero = 0;
    VkViewport left = { 0, 0, S / 2, S, 0, 1 }, right = { S / 2, 0, S / 2, S, 0, 1 };
    VkRect2D all = { { 0, 0 }, { S, S } };
    vkCmdSetScissor(cb, 0, 1, &all);
    float eight = 8.0f, one = 1.0f;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, tri_ccw);
    vkCmdSetViewport(cb, 0, 1, &left);
    vkCmdBindVertexBuffers(cb, 0, 1, &tvb.buffer, &zero);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_ALL, 0, 4, &eight);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, quad_ccw);
    vkCmdSetViewport(cb, 0, 1, &right);
    vkCmdBindVertexBuffers(cb, 0, 1, &qvb.buffer, &zero);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_ALL, 0, 4, &one);
    vkCmdDraw(cb, 4, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb);
    to_buffer(cb, target.image, 0, 0, S, S, rb.buffer, 0);
    submit(cb);
    // (left: the triangle's inside at (1, 6), its bowed top edge's middle (4, 1) empty, outside (6, 13) empty;
    //  right: the quad's top edge's middle (12, 1) and its corners (8, 0), (15, 15) drawn)
    int l_in = tess_px(&rb, 1, 6), l_edge = tess_px(&rb, 4, 1), l_out = tess_px(&rb, 6, 13);
    int r_edge = tess_px(&rb, 12, 1), r_corners = tess_px(&rb, 8, 0) && tess_px(&rb, 15, 15);
    CHECK(l_in && !l_edge && !l_out); CHECK(r_edge && r_corners);
    printf("vulkan_shaders: two tessellation draws in one pass: triangle inside %d edge %d outside %d, quad edge %d corners %d\n",
           l_in, l_edge, l_out, r_edge, r_corners);

    // Two patches in one draw (the control shader's kernel runs several patches in a threadgroup: VKMSL_TCS_PATCHES):
    // the left and the right half each a triangle, each patch's own top edge bowed at level 8, straight at level 1.
    static const float two[] = { -1, -1, 0, -1, -1, 1,   0, -1, 1, -1, 0, 1 };
    buf twovb = make_buffer(sizeof two, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    memcpy(twovb.map, two, sizeof two);
    tess_draw(tri_ccw, pl, view, target.image, &twovb, 6, 8.0f, &rb);
    int a8_in = tess_px(&rb, 1, 6), a8_edge = tess_px(&rb, 4, 1), b8_in = tess_px(&rb, 9, 6), b8_edge = tess_px(&rb, 12, 1);
    tess_draw(tri_ccw, pl, view, target.image, &twovb, 6, 1.0f, &rb);
    int a1_edge = tess_px(&rb, 4, 1), b1_edge = tess_px(&rb, 12, 1);
    CHECK(a8_in && b8_in && !a8_edge && !b8_edge); CHECK(a1_edge && b1_edge);
    printf("vulkan_shaders: two patches in one draw: level 8 inside %d %d edge %d %d, level 1 edge %d %d\n",
           a8_in, b8_in, a8_edge, b8_edge, a1_edge, b1_edge);
}

int main(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vulkan_shaders", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance instance = 0;
    test = "device";
    CHECK(vkCreateInstance(&ii, NULL, &instance) == VK_SUCCESS);
    uint32_t n = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &n, &gpu) == VK_SUCCESS && n == 1);
    VkPhysicalDeviceFeatures2 have = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    vkGetPhysicalDeviceFeatures2(gpu, &have);
    CHECK(have.features.geometryShader);
    float priority = 1;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &priority };
    VkPhysicalDeviceVulkan13Features e13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .synchronization2 = 1, .dynamicRendering = 1 };
    VkPhysicalDeviceVulkan12Features e12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &e13,
        .runtimeDescriptorArray = 1, .descriptorBindingPartiallyBound = 1, .shaderSampledImageArrayNonUniformIndexing = 1 };
    VkPhysicalDeviceFeatures2 want = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &e12,
                                       .features = { .geometryShader = 1, .tessellationShader = 1, .textureCompressionBC = 1 } };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &want, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi };
    CHECK(vkCreateDevice(gpu, &di, NULL, &device) == VK_SUCCESS);
    if (bad) { printf("vulkan_shaders: FAILED (no device)\n"); return 1; }
    vkGetDeviceQueue(device, 0, 0, &queue);
    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    CHECK(vkCreateCommandPool(device, &pi, NULL, &pool) == VK_SUCCESS);
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK(vkCreateFence(device, &fi, NULL, &fence) == VK_SUCCESS);
    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 }, { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 4 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 4 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 4 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 }, { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 4 } };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 16,
                                       .poolSizeCount = sizeof sizes / sizeof sizes[0], .pPoolSizes = sizes };
    CHECK(vkCreateDescriptorPool(device, &dpi, NULL, &dpool) == VK_SUCCESS);

    compute_basic();
    compute_images();
    sampling();
    mrt_msaa();
    subpasses();
    geometry_layer();
    narrow_output();
    triangle_fans();
    orientation();
    compressed_copies();
    tessellation();

    vkDeviceWaitIdle(device);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    printf("vulkan through shaders (compute, images, sampling, MRT/MSAA, subpasses, geometry): %s\n", bad ? "FAILED" : "ok");
    return bad ? 1 : 0;
}
