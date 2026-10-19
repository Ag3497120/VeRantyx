#include "vx_vulkan.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "vulkan bootstrap check failed at %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

typedef struct {
    VkQueue queue;
    VkFence fence;
    VkResult result;
} delayed_fence_submit;

static void *submit_empty_batch_after_delay(void *context) {
    delayed_fence_submit *submit = context;
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 20000000 };
    nanosleep(&delay, NULL);
    submit->result = vkQueueSubmit(submit->queue, 0, NULL, submit->fence);
    return NULL;
}

static uint64_t monotonic_time_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static vx_export registered[8];
static size_t registered_count;
static int allocations, frees;

void vx_register_module(const char *dll, const vx_export *exports, size_t count) {
    if (strcmp(dll, "vulkan-1.dll") != 0) abort();
    if (count > sizeof registered / sizeof registered[0]) abort();
    memcpy(registered, exports, count * sizeof *exports);
    registered_count = count;
}

static void *test_allocate(void *user, size_t size, size_t alignment, VkSystemAllocationScope scope) {
    if (user != (void *)0x1234 || alignment < 8 || scope != VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE) abort();
    allocations++;
    return malloc(size);
}

static void test_free(void *user, void *memory) {
    if (user != (void *)0x1234) abort();
    frees++;
    free(memory);
}

static uint8_t *read_fixture(const char *path, size_t *size) {
    *size = 0;
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || ftell(file) <= 0) { fclose(file); return NULL; }
    long length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return NULL; }
    uint8_t *bytes = malloc((size_t)length);
    if (!bytes) { fclose(file); return NULL; }
    if (fread(bytes, 1, (size_t)length, file) != (size_t)length || fclose(file)) {
        free(bytes);
        return NULL;
    }
    *size = (size_t)length;
    return bytes;
}

static int run_compute(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer, VkFence fence,
    const char *shader_path) {
    enum { ELEMENTS = 64, BUFFER_BYTES = ELEMENTS * sizeof(uint32_t) };
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes != NULL && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    CHECK(vkGetDeviceProcAddr(device, "vkCreateShaderModule") == (PFN_vkVoidFunction)vkCreateShaderModule);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateDescriptorSetLayout") == (PFN_vkVoidFunction)vkCreateDescriptorSetLayout);
    CHECK(vkGetDeviceProcAddr(device, "vkCreatePipelineLayout") == (PFN_vkVoidFunction)vkCreatePipelineLayout);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateComputePipelines") == (PFN_vkVoidFunction)vkCreateComputePipelines);
    CHECK(vkGetDeviceProcAddr(device, "vkAllocateDescriptorSets") == (PFN_vkVoidFunction)vkAllocateDescriptorSets);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdBindDescriptorSets") == (PFN_vkVoidFunction)vkCmdBindDescriptorSets);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdClearColorImage") == (PFN_vkVoidFunction)vkCmdClearColorImage);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdDispatch") == (PFN_vkVoidFunction)vkCmdDispatch);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdDispatchIndirect") == (PFN_vkVoidFunction)vkCmdDispatchIndirect);
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader != 0);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding descriptor_binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &descriptor_binding
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout != 0);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout != 0);
    uint32_t specialized_group_size_x = 16;
    VkSpecializationMapEntry workgroup_size_map = { .constantID = 0, .offset = 0, .size = sizeof(uint32_t) };
    VkSpecializationInfo workgroup_specialization = {
        .mapEntryCount = 1, .pMapEntries = &workgroup_size_map,
        .dataSize = sizeof specialized_group_size_x, .pData = &specialized_group_size_x
    };
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main",
            .pSpecializationInfo = &workgroup_specialization
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkSpecializationMapEntry duplicate_maps[2] = {
        { .constantID = 0, .offset = 0, .size = sizeof(uint32_t) },
        { .constantID = 0, .offset = sizeof(uint32_t), .size = sizeof(uint32_t) }
    };
    uint32_t duplicate_data[2] = { 16, 16 };
    VkSpecializationInfo duplicate_specialization = {
        .mapEntryCount = 2, .pMapEntries = duplicate_maps,
        .dataSize = sizeof duplicate_data, .pData = duplicate_data
    };
    VkComputePipelineCreateInfo invalid_pipeline_info = pipeline_info;
    invalid_pipeline_info.stage.pSpecializationInfo = &duplicate_specialization;
    VkPipeline rejected_pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &invalid_pipeline_info, NULL, &rejected_pipeline) ==
          VK_ERROR_INITIALIZATION_FAILED && rejected_pipeline == 0);
    VkSpecializationMapEntry wrong_size_map = { .constantID = 0, .offset = 0, .size = 2 };
    VkSpecializationInfo wrong_size_specialization = {
        .mapEntryCount = 1, .pMapEntries = &wrong_size_map,
        .dataSize = sizeof specialized_group_size_x, .pData = &specialized_group_size_x
    };
    invalid_pipeline_info.stage.pSpecializationInfo = &wrong_size_specialization;
    CHECK(vkCreateComputePipelines(device, 0, 1, &invalid_pipeline_info, NULL, &rejected_pipeline) ==
          VK_ERROR_INITIALIZATION_FAILED && rejected_pipeline == 0);
    VkSpecializationMapEntry out_of_bounds_map = { .constantID = 0, .offset = sizeof(uint32_t),
                                                    .size = sizeof(uint32_t) };
    VkSpecializationInfo out_of_bounds_specialization = {
        .mapEntryCount = 1, .pMapEntries = &out_of_bounds_map,
        .dataSize = sizeof specialized_group_size_x, .pData = &specialized_group_size_x
    };
    invalid_pipeline_info.stage.pSpecializationInfo = &out_of_bounds_specialization;
    CHECK(vkCreateComputePipelines(device, 0, 1, &invalid_pipeline_info, NULL, &rejected_pipeline) ==
          VK_ERROR_INITIALIZATION_FAILED && rejected_pipeline == 0);
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline != 0);
    VkSpecializationMapEntry unused_map = { .constantID = 999, .offset = 0, .size = sizeof(uint32_t) };
    VkSpecializationInfo unused_specialization = {
        .mapEntryCount = 1, .pMapEntries = &unused_map,
        .dataSize = sizeof specialized_group_size_x, .pData = &specialized_group_size_x
    };
    VkComputePipelineCreateInfo unused_map_pipeline_info = pipeline_info;
    unused_map_pipeline_info.stage.pSpecializationInfo = &unused_specialization;
    VkPipeline unused_map_pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &unused_map_pipeline_info, NULL, &unused_map_pipeline) ==
          VK_SUCCESS && unused_map_pipeline != 0);
    vkDestroyPipeline(device, unused_map_pipeline, NULL);

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = BUFFER_BYTES,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer buffer = 0;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffer) == VK_SUCCESS && buffer != 0);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS && memory != 0);
    CHECK(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < ELEMENTS; i++) ((uint32_t *)mapped)[i] = 0x100u + i;
    vkUnmapMemory(device, memory);

    VkDescriptorPoolSize pool_size = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };
    VkDescriptorPool descriptor_pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool) == VK_SUCCESS && descriptor_pool != 0);
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout
    };
    VkDescriptorSet descriptor_set = 0;
    CHECK(vkAllocateDescriptorSets(device, &set_info, &descriptor_set) == VK_SUCCESS && descriptor_set != 0);
    VkDescriptorBufferInfo buffer_descriptor = { .buffer = buffer, .range = BUFFER_BYTES };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_descriptor
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    VkSubmitInfo stale_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &stale_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatch(command_buffer, 4, 1, 1);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .size = BUFFER_BYTES
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < ELEMENTS; i++)
        CHECK(((uint32_t *)mapped)[i] == (i == 7 ? 999u : i * 2u + 1u));
    vkUnmapMemory(device, memory);

    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkBufferCreateInfo indirect_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 12,
        .usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer indirect_buffer = 0;
    CHECK(vkCreateBuffer(device, &indirect_info, NULL, &indirect_buffer) == VK_SUCCESS && indirect_buffer != 0);
    VkMemoryRequirements indirect_requirements;
    vkGetBufferMemoryRequirements(device, indirect_buffer, &indirect_requirements);
    VkMemoryAllocateInfo indirect_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = indirect_requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory indirect_memory = 0;
    CHECK(vkAllocateMemory(device, &indirect_allocation, NULL, &indirect_memory) == VK_SUCCESS && indirect_memory != 0);
    CHECK(vkBindBufferMemory(device, indirect_buffer, indirect_memory, 0) == VK_SUCCESS);
    uint32_t dispatch_groups[3] = { 4, 1, 1 };
    CHECK(vkMapMemory(device, indirect_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    memcpy(mapped, dispatch_groups, sizeof dispatch_groups);
    vkUnmapMemory(device, indirect_memory);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatchIndirect(command_buffer, indirect_buffer, 0);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < ELEMENTS; i++)
        CHECK(((uint32_t *)mapped)[i] == (i == 7 ? 999u : i * 2u + 1u));
    vkUnmapMemory(device, memory);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    // A descriptor that exposes only element zero must drop later stores, even though the
    // underlying VkBuffer and Metal buffer remain larger.
    const uint32_t sentinel = 0x6a09e667u;
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < ELEMENTS; i++) ((uint32_t *)mapped)[i] = sentinel + i;
    vkUnmapMemory(device, memory);
    buffer_descriptor.offset = 4 * sizeof(uint32_t);
    buffer_descriptor.range = sizeof(uint32_t);
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatch(command_buffer, 4, 1, 1);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < ELEMENTS; i++)
        CHECK(((uint32_t *)mapped)[i] == (i == 4 ? 1u : sentinel + i));
    vkUnmapMemory(device, memory);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkFreeDescriptorSets(device, descriptor_pool, 1, &descriptor_set) == VK_SUCCESS);
    VkSubmitInfo invalid_descriptor_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_descriptor_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkAllocateDescriptorSets(device, &set_info, &descriptor_set) == VK_SUCCESS && descriptor_set);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetDescriptorPool(device, descriptor_pool, 0) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &invalid_descriptor_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkAllocateDescriptorSets(device, &set_info, &descriptor_set) == VK_SUCCESS && descriptor_set);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    CHECK(vkQueueSubmit(queue, 1, &invalid_descriptor_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    vkDestroyBuffer(device, indirect_buffer, NULL);
    vkFreeMemory(device, indirect_memory, NULL);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts("vulkan compute: SPIR-V storage-buffer pipeline, direct and indirect dispatch, stale descriptor-set command buffers rejected: ok");
    return 0;
}

static int run_storage_image_copy(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer,
                                 VkFence fence, const char *shader_path, int sampled_fetch) {
    const uint8_t expected[4] = { 37, 99, 171, 255 };
    int sampled_source = sampled_fetch != 0;
    int combined_sampler = sampled_fetch == 2;
    int separate_sampler = sampled_fetch == 3;
    uint32_t output_binding = separate_sampler ? 2u : 1u;
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding bindings[3] = {
        { .binding = 0, .descriptorType = combined_sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
              sampled_source ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = separate_sampler ? VK_DESCRIPTOR_TYPE_SAMPLER :
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = separate_sampler ? 3u : 2u,
        .pBindings = bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline);

    VkImage images[2] = { 0, 0 };
    VkDeviceMemory image_memories[2] = { 0, 0 };
    VkImageView views[2] = { 0, 0 };
    const VkImageUsageFlags image_usages[2] = {
        (sampled_source ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT) |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
    };
    for (uint32_t i = 0; i < 2; i++) {
        VkImageCreateInfo image_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { 1, 1, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = image_usages[i],
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
        };
        CHECK(vkCreateImage(device, &image_info, NULL, &images[i]) == VK_SUCCESS && images[i]);
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, images[i], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = 0
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &image_memories[i]) == VK_SUCCESS && image_memories[i]);
        CHECK(vkBindImageMemory(device, images[i], image_memories[i], 0) == VK_SUCCESS);
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        CHECK(vkCreateImageView(device, &view_info, NULL, &views[i]) == VK_SUCCESS && views[i]);
    }

    VkBuffer buffers[2] = { 0, 0 };
    VkDeviceMemory buffer_memories[2] = { 0, 0 };
    const VkBufferUsageFlags buffer_usages[2] = {
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT
    };
    for (uint32_t i = 0; i < 2; i++) {
        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof expected,
            .usage = buffer_usages[i],
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffers[i]) == VK_SUCCESS && buffers[i]);
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = 0
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &buffer_memories[i]) == VK_SUCCESS && buffer_memories[i]);
        CHECK(vkBindBufferMemory(device, buffers[i], buffer_memories[i], 0) == VK_SUCCESS);
    }
    void *mapped = NULL;
    CHECK(vkMapMemory(device, buffer_memories[0], 0, sizeof expected, 0, &mapped) == VK_SUCCESS && mapped);
    memcpy(mapped, expected, sizeof expected);
    vkUnmapMemory(device, buffer_memories[0]);

    VkDescriptorPoolSize pool_sizes[3] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sampled_fetch ? 1u : 2u },
        { combined_sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          sampled_source ? 1u : 0u },
        { VK_DESCRIPTOR_TYPE_SAMPLER, separate_sampler ? 1u : 0u }
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = sampled_source ? (separate_sampler ? 3u : 2u) : 1u,
        .pPoolSizes = pool_sizes
    };
    VkDescriptorPool pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool) == VK_SUCCESS && pool);
    VkDescriptorSetAllocateInfo allocate_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout
    };
    VkDescriptorSet descriptor_set = 0;
    CHECK(vkAllocateDescriptorSets(device, &allocate_info, &descriptor_set) == VK_SUCCESS && descriptor_set);
    VkSampler sampler = 0;
    if (combined_sampler || separate_sampler) {
        VkSamplerCreateInfo sampler_info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .maxAnisotropy = 1.0f,
            .compareOp = VK_COMPARE_OP_NEVER,
            .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
            .maxLod = 0.0f,
            .unnormalizedCoordinates = 0
        };
        CHECK(vkCreateSampler(device, &sampler_info, NULL, &sampler) == VK_SUCCESS && sampler);
    }
    VkDescriptorImageInfo image_infos[2] = {
        { .sampler = sampler, .imageView = views[0], .imageLayout = sampled_source ?
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = views[1], .imageLayout = VK_IMAGE_LAYOUT_GENERAL }
    };
    VkDescriptorImageInfo sampler_descriptor = { .sampler = sampler };
    VkWriteDescriptorSet writes[3] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor_set, .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = combined_sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
              sampled_source ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = &image_infos[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor_set,
          .dstBinding = separate_sampler ? 1u : 1u,
          .descriptorCount = 1, .descriptorType = separate_sampler ? VK_DESCRIPTOR_TYPE_SAMPLER :
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = separate_sampler ? &sampler_descriptor : &image_infos[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor_set,
          .dstBinding = output_binding,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .pImageInfo = &image_infos[1] }
    };
    vkUpdateDescriptorSets(device, separate_sampler ? 3u : 2u, writes, 0, NULL);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    VkImageMemoryBarrier initial_barriers[2] = {
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = images[0], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = images[1], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } }
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, 2, initial_barriers);
    VkBufferImageCopy copy_region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { 1, 1, 1 }
    };
    vkCmdCopyBufferToImage(command_buffer, buffers[0], images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &copy_region);
    VkImageMemoryBarrier source_to_general = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = sampled_source ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[0],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &source_to_general);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    VkImageMemoryBarrier destination_to_transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = images[1],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &destination_to_transfer);
    vkCmdCopyImageToBuffer(command_buffer, images[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           buffers[1], 1, &copy_region);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[1],
        .size = sizeof expected
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, buffer_memories[1], 0, sizeof expected, 0, &mapped) == VK_SUCCESS && mapped);
    int matches = memcmp(mapped, expected, sizeof expected) == 0;
    vkUnmapMemory(device, buffer_memories[1]);
    CHECK(matches);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    vkDestroyDescriptorPool(device, pool, NULL);
    if (sampler) vkDestroySampler(device, sampler, NULL);
    for (uint32_t i = 0; i < 2; i++) {
        vkDestroyImageView(device, views[i], NULL);
        vkDestroyImage(device, images[i], NULL);
        vkFreeMemory(device, image_memories[i], NULL);
        vkDestroyBuffer(device, buffers[i], NULL);
        vkFreeMemory(device, buffer_memories[i], NULL);
    }
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts(separate_sampler ?
         "vulkan compute: separate sampled image and sampler, OpSampledImage and explicit-LOD sampling: ok" :
         combined_sampler ?
         "vulkan compute: combined sampler, explicit-LOD SPIR-V sampling, storage-image write/readback: ok" :
         sampled_fetch ?
             "vulkan compute: RGBA8 sampled-image OpImageFetch and storage-image write/readback: ok" :
             "vulkan compute: RGBA8 storage-image OpImageRead/OpImageWrite dispatch and transfer readback: ok");
    return 0;
}

static int run_descriptor_array(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer, VkFence fence,
    const char *shader_path, VkDescriptorType descriptor_type) {
    int dynamic_storage = descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding descriptor_bindings[2] = {
        {
            .binding = 0,
            .descriptorType = descriptor_type,
            .descriptorCount = 3,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        },
        {
            .binding = 1,
            .descriptorType = descriptor_type,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        }
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = descriptor_bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline);

    VkBuffer buffers[4] = { 0 };
    VkDeviceMemory memories[4] = { 0 };
    uint32_t initial_values[4] = { 0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u };
    for (uint32_t i = 0; i < 4; i++) {
        VkBufferCreateInfo buffer_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof(uint32_t) * 8,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffers[i]) == VK_SUCCESS && buffers[i]);
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffers[i], &requirements);
        VkMemoryAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = 0
        };
        CHECK(vkAllocateMemory(device, &allocation, NULL, &memories[i]) == VK_SUCCESS && memories[i]);
        CHECK(vkBindBufferMemory(device, buffers[i], memories[i], 0) == VK_SUCCESS);
        void *mapped = NULL;
        CHECK(vkMapMemory(device, memories[i], 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
        uint32_t *words = mapped;
        for (uint32_t j = 0; j < 8; j++) words[j] = 0;
        words[0] = initial_values[i];
        vkUnmapMemory(device, memories[i]);
    }

    VkDescriptorPoolSize pool_size = {
        .type = descriptor_type,
        .descriptorCount = 8
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 2,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };
    VkDescriptorPool descriptor_pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool) == VK_SUCCESS && descriptor_pool);
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 2,
        .pSetLayouts = (VkDescriptorSetLayout[]){ set_layout, set_layout }
    };
    VkDescriptorSet descriptor_sets[2] = { 0 };
    CHECK(vkAllocateDescriptorSets(device, &set_info, descriptor_sets) == VK_SUCCESS &&
          descriptor_sets[0] && descriptor_sets[1]);
    VkDescriptorBufferInfo buffer_infos[4] = {
        { .buffer = buffers[0], .range = sizeof(uint32_t) },
        { .buffer = buffers[1], .range = sizeof(uint32_t) },
        { .buffer = buffers[2], .range = sizeof(uint32_t) },
        { .buffer = buffers[3], .range = sizeof(uint32_t) }
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_sets[0],
        .dstBinding = 0,
        .descriptorCount = 4,
        .descriptorType = descriptor_type,
        .pBufferInfo = buffer_infos
    };
    VkWriteDescriptorSet second_element_write = write;
    second_element_write.dstArrayElement = 1;
    second_element_write.descriptorCount = 1;
    second_element_write.pBufferInfo = &buffer_infos[1];
    VkWriteDescriptorSet destination_seed_write = write;
    destination_seed_write.dstSet = descriptor_sets[1];
    destination_seed_write.dstArrayElement = 1;
    destination_seed_write.descriptorCount = 1;
    destination_seed_write.pBufferInfo = &buffer_infos[3];
    VkWriteDescriptorSet descriptor_writes[3] = { write, second_element_write, destination_seed_write };
    VkCopyDescriptorSet descriptor_copy = {
        .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
        .srcSet = descriptor_sets[0],
        .srcBinding = 0,
        .dstSet = descriptor_sets[1],
        .dstBinding = 0,
        .descriptorCount = 4
    };
    vkUpdateDescriptorSets(device, 3, descriptor_writes, 1, &descriptor_copy);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    uint32_t dynamic_offsets[4] = { 0, 16, 0, 0 };
    if (dynamic_storage) {
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                0, 1, &descriptor_sets[1], 3, dynamic_offsets);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

        uint32_t misaligned_offsets[4] = { 0, 4, 0, 0 };
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                0, 1, &descriptor_sets[1], 4, misaligned_offsets);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

        uint32_t out_of_range_offsets[4] = { 0, 32, 0, 0 };
        CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
        vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                0, 1, &descriptor_sets[1], 4, out_of_range_offsets);
        CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
        CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    }

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_sets[1], dynamic_storage ? 4 : 0,
                            dynamic_storage ? dynamic_offsets : NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_sets[1], dynamic_storage ? 4 : 0,
                            dynamic_storage ? dynamic_offsets : NULL);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    VkBufferMemoryBarrier barriers[2] = { 0 };
    for (uint32_t i = 0; i < 2; i++) barriers[i] = (VkBufferMemoryBarrier){
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffers[i],
        .size = sizeof(uint32_t) * 8
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 2, barriers, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    for (uint32_t i = 0; i < 4; i++) {
        void *mapped = NULL;
        CHECK(vkMapMemory(device, memories[i], 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
        uint32_t *words = mapped;
        CHECK(words[0] == (i == 1 && !dynamic_storage ? 123u : initial_values[i]));
        CHECK(words[4] == (i == 1 && dynamic_storage ? 123u : 0u));
        vkUnmapMemory(device, memories[i]);
    }
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_sets[1], dynamic_storage ? 4 : 0,
                            dynamic_storage ? dynamic_offsets : NULL);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyPipeline(device, pipeline, NULL);
    pipeline = 0;
    VkSubmitInfo invalid_pipeline_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_pipeline_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    VkPipeline replacement_pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &replacement_pipeline) == VK_SUCCESS &&
          replacement_pipeline);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, replacement_pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_sets[1], dynamic_storage ? 4 : 0,
                            dynamic_storage ? dynamic_offsets : NULL);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyBuffer(device, buffers[1], NULL);
    buffers[1] = 0;
    VkSubmitInfo invalid_buffer_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_buffer_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, replacement_pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    pipeline_layout = 0;
    VkSubmitInfo invalid_pipeline_layout_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_pipeline_layout_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkFreeDescriptorSets(device, descriptor_pool, 2, descriptor_sets) == VK_SUCCESS);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    for (uint32_t i = 0; i < 4; i++) {
        vkDestroyBuffer(device, buffers[i], NULL);
        vkFreeMemory(device, memories[i], NULL);
    }
    vkDestroyPipeline(device, replacement_pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts(dynamic_storage ?
        "vulkan descriptors: dynamic storage-buffer arrays, ordered copies and offset dispatch: ok" :
        "vulkan descriptors: storage-buffer arrays and ordered cross-set copies: ok");
    return 0;
}

static int run_uniform_buffer(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer, VkFence fence,
    const char *shader_path) {
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline);

    VkBuffer output = 0, uniform = 0;
    VkDeviceMemory output_memory = 0, uniform_memory = 0;
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 16,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &output) == VK_SUCCESS && output);
    buffer_info.size = sizeof(uint32_t);
    buffer_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &uniform) == VK_SUCCESS && uniform);
    VkMemoryRequirements output_requirements, uniform_requirements;
    vkGetBufferMemoryRequirements(device, output, &output_requirements);
    vkGetBufferMemoryRequirements(device, uniform, &uniform_requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = output_requirements.size,
        .memoryTypeIndex = 0
    };
    CHECK(vkAllocateMemory(device, &allocation, NULL, &output_memory) == VK_SUCCESS && output_memory);
    allocation.allocationSize = uniform_requirements.size;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &uniform_memory) == VK_SUCCESS && uniform_memory);
    CHECK(vkBindBufferMemory(device, output, output_memory, 0) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(device, uniform, uniform_memory, 0) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, output_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    memset(mapped, 0, 16);
    vkUnmapMemory(device, output_memory);
    const uint32_t expected = 0xc001d00du;
    CHECK(vkMapMemory(device, uniform_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    *(uint32_t *)mapped = expected;
    vkUnmapMemory(device, uniform_memory);

    VkDescriptorPoolSize pool_sizes[2] = {
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 },
        { .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 2 }
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 2,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes
    };
    VkDescriptorPool descriptor_pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool) == VK_SUCCESS && descriptor_pool);
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 2,
        .pSetLayouts = (VkDescriptorSetLayout[]){ set_layout, set_layout }
    };
    VkDescriptorSet sets[2] = { 0 };
    CHECK(vkAllocateDescriptorSets(device, &set_info, sets) == VK_SUCCESS && sets[0] && sets[1]);
    VkDescriptorBufferInfo output_info = { .buffer = output, .range = 16 };
    VkDescriptorBufferInfo uniform_info = { .buffer = uniform, .range = sizeof(uint32_t) };
    VkWriteDescriptorSet writes[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0], .dstBinding = 1,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &uniform_info },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[1], .dstBinding = 0,
          .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &output_info }
    };
    VkCopyDescriptorSet copy_uniform = {
        .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
        .srcSet = sets[0], .srcBinding = 1,
        .dstSet = sets[1], .dstBinding = 1,
        .descriptorCount = 1
    };
    vkUpdateDescriptorSets(device, 2, writes, 1, &copy_uniform);

    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &sets[1], 0, NULL);
    vkCmdDispatch(command_buffer, 1, 1, 1);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = output,
        .size = 16
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, output_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    CHECK(*(uint32_t *)mapped == expected);
    vkUnmapMemory(device, output_memory);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    CHECK(vkFreeDescriptorSets(device, descriptor_pool, 2, sets) == VK_SUCCESS);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyBuffer(device, uniform, NULL);
    vkFreeMemory(device, uniform_memory, NULL);
    vkDestroyBuffer(device, output, NULL);
    vkFreeMemory(device, output_memory, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts("vulkan descriptors: uniform-buffer read, copied descriptors and compute dispatch: ok");
    return 0;
}

static int run_push_constants(VkDevice device, VkQueue queue, VkCommandBuffer command_buffer, VkFence fence,
    const char *shader_path) {
    size_t shader_size = 0;
    uint8_t *shader_bytes = read_fixture(shader_path, &shader_size);
    CHECK(shader_bytes && !(shader_size & 3u));
    VkShaderModuleCreateInfo shader_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = shader_size,
        .pCode = (const uint32_t *)shader_bytes
    };
    VkShaderModule shader = 0;
    CHECK(vkCreateShaderModule(device, &shader_info, NULL, &shader) == VK_SUCCESS && shader);
    free(shader_bytes);

    VkDescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &binding
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout) == VK_SUCCESS && set_layout);

    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 8
    };
    VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range
    };
    VkPushConstantRange invalid_range = push_range;
    invalid_range.offset = 2;
    VkPipelineLayout invalid_layout = 0;
    VkPipelineLayoutCreateInfo invalid_layout_info = layout_info;
    invalid_layout_info.pPushConstantRanges = &invalid_range;
    CHECK(vkCreatePipelineLayout(device, &invalid_layout_info, NULL, &invalid_layout) ==
          VK_ERROR_INITIALIZATION_FAILED && invalid_layout == 0);
    invalid_range = push_range;
    invalid_range.stageFlags = 1u; // Vertex stage is not implemented by this Vulkan device.
    CHECK(vkCreatePipelineLayout(device, &invalid_layout_info, NULL, &invalid_layout) ==
          VK_ERROR_FEATURE_NOT_PRESENT && invalid_layout == 0);
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);

    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = shader,
            .pName = "main"
        },
        .layout = pipeline_layout,
        .basePipelineIndex = -1
    };
    VkPipeline pipeline = 0;
    CHECK(vkCreateComputePipelines(device, 0, 1, &pipeline_info, NULL, &pipeline) == VK_SUCCESS && pipeline);

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer output = 0;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &output) == VK_SUCCESS && output);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, output, &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &memory) == VK_SUCCESS && memory);
    CHECK(vkBindBufferMemory(device, output, memory, 0) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    *(uint32_t *)mapped = 0;
    vkUnmapMemory(device, memory);

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size
    };
    VkDescriptorPool descriptor_pool = 0;
    CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &descriptor_pool) == VK_SUCCESS && descriptor_pool);
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout
    };
    VkDescriptorSet descriptor_set = 0;
    CHECK(vkAllocateDescriptorSets(device, &set_info, &descriptor_set) == VK_SUCCESS && descriptor_set);
    VkDescriptorBufferInfo buffer_descriptor = { .buffer = output, .range = sizeof(uint32_t) };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptor_set,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_descriptor
    };
    vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

    CHECK(vkGetDeviceProcAddr(device, "vkCmdPushConstants") == (PFN_vkVoidFunction)vkCmdPushConstants);
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                            0, 1, &descriptor_set, 0, NULL);
    uint32_t first = 19, second = 23;
    vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof first, &first);
    first = 0; // The command must retain a copy, not this caller-owned address.
    vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                       sizeof first, sizeof second, &second);
    second = 0;
    vkCmdDispatch(command_buffer, 1, 1, 1);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = output,
        .size = sizeof(uint32_t)
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    CHECK(*(uint32_t *)mapped == 42u);
    vkUnmapMemory(device, memory);

    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkFreeDescriptorSets(device, descriptor_pool, 1, &descriptor_set) == VK_SUCCESS);
    vkDestroyDescriptorPool(device, descriptor_pool, NULL);
    vkDestroyBuffer(device, output, NULL);
    vkFreeMemory(device, memory, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    puts("vulkan compute: partial push-constant updates copied, translated and read back through Metal: ok");
    return 0;
}

static int check_buffer_descriptor_slot_limits(VkDevice device) {
    VkDescriptorSetLayoutBinding mixed_bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 16,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 15,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = mixed_bindings
    };
    VkDescriptorSetLayout rejected = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &rejected) == VK_ERROR_FEATURE_NOT_PRESENT &&
          !rejected);

    VkDescriptorSetLayoutBinding uniform_binding = mixed_bindings[0];
    VkDescriptorSetLayoutBinding storage_binding = mixed_bindings[1];
    VkDescriptorSetLayoutCreateInfo one_binding_info = set_info;
    one_binding_info.bindingCount = 1;
    one_binding_info.pBindings = &uniform_binding;
    VkDescriptorSetLayout uniform_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &one_binding_info, NULL, &uniform_layout) == VK_SUCCESS &&
          uniform_layout);
    one_binding_info.pBindings = &storage_binding;
    VkDescriptorSetLayout storage_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &one_binding_info, NULL, &storage_layout) == VK_SUCCESS &&
          storage_layout);
    VkDescriptorSetLayout layouts[2] = { uniform_layout, storage_layout };
    VkPipelineLayoutCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 2,
        .pSetLayouts = layouts
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &pipeline_info, NULL, &pipeline_layout) == VK_ERROR_FEATURE_NOT_PRESENT &&
          !pipeline_layout);
    vkDestroyDescriptorSetLayout(device, storage_layout, NULL);
    vkDestroyDescriptorSetLayout(device, uniform_layout, NULL);
    puts("vulkan descriptors: mixed uniform/storage buffer layouts respect the shared Metal slot limit: ok");
    return 0;
}

static int check_sampler_allocation_limit(VkDevice device, uint32_t limit) {
    VkSampler *samplers = calloc(limit, sizeof *samplers);
    if (!samplers) return 1;
    VkSamplerCreateInfo sampler_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxAnisotropy = 1.0f,
        .compareOp = VK_COMPARE_OP_NEVER,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
    };
    uint32_t created = 0;
    for (; created < limit; created++) {
        if (vkCreateSampler(device, &sampler_info, NULL, &samplers[created]) != VK_SUCCESS ||
            !samplers[created]) {
            for (uint32_t i = 0; i < created; i++) vkDestroySampler(device, samplers[i], NULL);
            free(samplers);
            return 1;
        }
    }
    VkSampler over_limit = 0;
    VkResult result = vkCreateSampler(device, &sampler_info, NULL, &over_limit);
    for (uint32_t i = 0; i < created; i++) vkDestroySampler(device, samplers[i], NULL);
    free(samplers);
    CHECK(result == VK_ERROR_TOO_MANY_OBJECTS && !over_limit);
    puts("vulkan sampler: allocation limit is enforced and slots are reusable after destruction: ok");
    return 0;
}

static int check_texture_descriptor_limits(VkDevice device, int storage_images_supported) {
    VkDescriptorSetLayoutBinding bindings[2] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 16,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 4,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT }
    };
    VkDescriptorSetLayoutCreateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = storage_images_supported ? 2u : 1u,
        .pBindings = bindings
    };
    VkDescriptorSetLayout set_layout = 0;
    CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout) == VK_SUCCESS && set_layout);
    VkPipelineLayoutCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &set_layout
    };
    VkPipelineLayout pipeline_layout = 0;
    CHECK(vkCreatePipelineLayout(device, &pipeline_info, NULL, &pipeline_layout) == VK_SUCCESS && pipeline_layout);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    puts(storage_images_supported ?
         "vulkan descriptors: 16 sampled and 4 storage images fit the shared Metal texture table: ok" :
         "vulkan descriptors: 16 sampled images fit the shared Metal texture table: ok");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 9) return 2;
    vx_vulkan_init();
    CHECK(registered_count == 6);
    CHECK(strcmp(registered[0].name, "vkGetInstanceProcAddr") == 0);
    CHECK(registered[0].fn == (void *)vkGetInstanceProcAddr);

    uint32_t version = 0;
    CHECK(vkEnumerateInstanceVersion(&version) == VK_SUCCESS);
    CHECK(version == VK_API_VERSION_1_0);
    CHECK(vkEnumerateInstanceVersion(NULL) == VK_ERROR_INITIALIZATION_FAILED);

    uint32_t count = 10;
    CHECK(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) == VK_SUCCESS && count == 0);
    count = 10;
    CHECK(vkEnumerateInstanceLayerProperties(&count, NULL) == VK_SUCCESS && count == 0);
    count = 10;
    CHECK(vkEnumerateInstanceExtensionProperties("missing-layer", &count, NULL) == VK_ERROR_LAYER_NOT_PRESENT);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .apiVersion = VK_API_VERSION_1_0 | 7 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                  .pApplicationInfo = &app };
    VkAllocationCallbacks allocator = { .pUserData = (void *)0x1234,
                                        .pfnAllocation = test_allocate,
                                        .pfnFree = test_free };
    VkInstance instance = NULL;
    CHECK(vkCreateInstance(&info, &allocator, &instance) == VK_SUCCESS);
    CHECK(instance != NULL && allocations == 1 && frees == 0);
    CHECK(vkGetInstanceProcAddr(NULL, "vkCreateInstance") != NULL);
    CHECK(vkGetInstanceProcAddr(NULL, "vkDestroyInstance") == NULL);
    CHECK(vkGetInstanceProcAddr(instance, "vkDestroyInstance") == (PFN_vkVoidFunction)vkDestroyInstance);
    CHECK(vkGetInstanceProcAddr(instance, "vkUnknownCommand") == NULL);

    uint32_t physical_count = 0;
    CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, NULL) == VK_SUCCESS && physical_count == 1);
    VkPhysicalDevice physical = NULL;
    physical_count = 1;
    CHECK(vkEnumeratePhysicalDevices(instance, &physical_count, &physical) == VK_SUCCESS && physical_count == 1);
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(physical, &features);
    CHECK(features.robustBufferAccess == 1 && features.geometryShader == 0 && features.shaderFloat64 == 0);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    CHECK(properties.apiVersion == VK_API_VERSION_1_0 &&
          properties.limits.maxImageDimension1D == 4096 && properties.limits.maxImageDimension2D == 4096 &&
          properties.limits.maxImageDimension3D == 256 && properties.limits.maxImageDimensionCube == 4096 &&
          properties.limits.maxImageArrayLayers == 256 &&
          properties.limits.maxPerStageDescriptorSampledImages == 16 &&
          properties.limits.maxDescriptorSetSampledImages == 16 &&
          properties.limits.maxPerStageDescriptorSamplers == 16 &&
          properties.limits.maxDescriptorSetSamplers == 16 &&
          properties.limits.maxSamplerAllocationCount == 4096 &&
          properties.limits.maxPerStageDescriptorUniformBuffers == 30 &&
          properties.limits.maxPerStageResources == 76 + properties.limits.maxPerStageDescriptorStorageImages &&
          properties.limits.maxDescriptorSetUniformBuffers == 30 &&
          properties.limits.maxDescriptorSetStorageBuffersDynamic == 30 &&
          properties.limits.maxPushConstantsSize == 128 &&
          properties.limits.maxComputeWorkGroupInvocations == 128 &&
          properties.limits.maxComputeSharedMemorySize > 0 &&
          properties.limits.maxComputeWorkGroupCount[0] == 65535 &&
          properties.limits.maxComputeWorkGroupCount[1] == 65535 &&
          properties.limits.maxComputeWorkGroupCount[2] == 65535);
    VkFormatProperties format_properties = { UINT32_MAX, UINT32_MAX, UINT32_MAX };
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &format_properties);
    CHECK(format_properties.linearTilingFeatures == 0 &&
          format_properties.optimalTilingFeatures == (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
              (properties.limits.maxPerStageDescriptorStorageImages ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : 0)) &&
          format_properties.bufferFeatures == 0);
    VkImageFormatProperties image_format_properties;
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        0, &image_format_properties) == VK_SUCCESS);
    CHECK(image_format_properties.maxExtent.width == 4096 && image_format_properties.maxExtent.height == 4096 &&
          image_format_properties.maxExtent.depth == 1 && image_format_properties.maxMipLevels == 1 &&
          image_format_properties.maxArrayLayers == 1 && image_format_properties.sampleCounts == VK_SAMPLE_COUNT_1_BIT);
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &image_format_properties) ==
        VK_SUCCESS);
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT, 0, &image_format_properties) ==
        (properties.limits.maxPerStageDescriptorStorageImages ? VK_SUCCESS : VK_ERROR_FORMAT_NOT_SUPPORTED));
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &image_format_properties) == VK_SUCCESS);
    CHECK(vkGetPhysicalDeviceImageFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_SAMPLED_BIT, 0, &image_format_properties) ==
        VK_ERROR_FORMAT_NOT_SUPPORTED);
    CHECK(properties.limits.maxComputeWorkGroupSize[0] >= 128 &&
          properties.limits.maxComputeWorkGroupSize[1] >= 128 &&
          properties.limits.maxComputeWorkGroupSize[2] >= 64);
    CHECK(properties.limits.minMemoryMapAlignment >= 64 &&
          !(properties.limits.minMemoryMapAlignment & (properties.limits.minMemoryMapAlignment - 1)));
    CHECK(properties.limits.nonCoherentAtomSize == 1);
    VkQueueFamilyProperties family;
    uint32_t family_count = 1;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, &family);
    CHECK(family_count == 1 && family.queueCount == 1 &&
          family.queueFlags == (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT));
    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    CHECK(memory_properties.memoryTypeCount == 1 && memory_properties.memoryHeapCount == 1 &&
          (memory_properties.memoryTypes[0].propertyFlags &
           (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
          (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_info
    };
    VkPhysicalDeviceFeatures unsupported_features = { .geometryShader = 1 };
    VkDeviceCreateInfo unsupported_device_info = device_info;
    unsupported_device_info.pEnabledFeatures = &unsupported_features;
    VkDevice rejected_device = NULL;
    CHECK(vkCreateDevice(physical, &unsupported_device_info, NULL, &rejected_device) ==
          VK_ERROR_FEATURE_NOT_PRESENT && rejected_device == NULL);
    device_info.pEnabledFeatures = &features;
    VkDevice device = NULL;
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device) == VK_SUCCESS && device != NULL);
    CHECK(check_buffer_descriptor_slot_limits(device) == 0);
    CHECK(check_texture_descriptor_limits(device, properties.limits.maxPerStageDescriptorStorageImages != 0) == 0);
    CHECK(check_sampler_allocation_limit(device, properties.limits.maxSamplerAllocationCount) == 0);
    CHECK(vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr") == (PFN_vkVoidFunction)vkGetDeviceProcAddr);
    CHECK(vkGetInstanceProcAddr(instance, "vkCmdPushConstants") == (PFN_vkVoidFunction)vkCmdPushConstants);
    PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)vkGetDeviceProcAddr(device, "vkGetDeviceQueue");
    CHECK(get_queue != NULL && vkGetDeviceProcAddr(device, "vkUnknownCommand") == NULL);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdUpdateBuffer") == (PFN_vkVoidFunction)vkCmdUpdateBuffer);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdFillBuffer") == (PFN_vkVoidFunction)vkCmdFillBuffer);
    CHECK(vkGetDeviceProcAddr(device, "vkFlushMappedMemoryRanges") ==
          (PFN_vkVoidFunction)vkFlushMappedMemoryRanges);
    CHECK(vkGetDeviceProcAddr(device, "vkInvalidateMappedMemoryRanges") ==
          (PFN_vkVoidFunction)vkInvalidateMappedMemoryRanges);
    CHECK(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceImageFormatProperties") ==
          (PFN_vkVoidFunction)vkGetPhysicalDeviceImageFormatProperties);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateImage") == (PFN_vkVoidFunction)vkCreateImage);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateImageView") == (PFN_vkVoidFunction)vkCreateImageView);
    CHECK(vkGetDeviceProcAddr(device, "vkDestroyImageView") == (PFN_vkVoidFunction)vkDestroyImageView);
    CHECK(vkGetDeviceProcAddr(device, "vkGetImageSubresourceLayout") ==
          (PFN_vkVoidFunction)vkGetImageSubresourceLayout);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdCopyBufferToImage") == (PFN_vkVoidFunction)vkCmdCopyBufferToImage);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdCopyImageToBuffer") == (PFN_vkVoidFunction)vkCmdCopyImageToBuffer);
    CHECK(vkGetDeviceProcAddr(device, "vkCreateSampler") == (PFN_vkVoidFunction)vkCreateSampler);
    CHECK(vkGetDeviceProcAddr(device, "vkDestroySampler") == (PFN_vkVoidFunction)vkDestroySampler);
    CHECK(vkGetDeviceProcAddr(device, "vkCmdCopyImage") == (PFN_vkVoidFunction)vkCmdCopyImage);
    VkQueue queue = NULL;
    get_queue(device, 0, 0, &queue);
    CHECK(queue != NULL);

    VkMemoryAllocateInfo mapped_range_allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = 128,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory mapped_range_memory = 0;
    CHECK(vkAllocateMemory(device, &mapped_range_allocation, NULL, &mapped_range_memory) == VK_SUCCESS);
    void *mapped_range_data = NULL;
    CHECK(vkMapMemory(device, mapped_range_memory, 16, 64, 0, &mapped_range_data) == VK_SUCCESS &&
          mapped_range_data != NULL);
    VkMappedMemoryRange mapped_range = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mapped_range_memory,
        .offset = 16,
        .size = 32
    };
    CHECK(vkFlushMappedMemoryRanges(device, 1, &mapped_range) == VK_SUCCESS);
    CHECK(vkInvalidateMappedMemoryRanges(device, 1, &mapped_range) == VK_SUCCESS);
    mapped_range.offset = 32;
    mapped_range.size = VK_WHOLE_SIZE;
    CHECK(vkFlushMappedMemoryRanges(device, 1, &mapped_range) == VK_SUCCESS);
    mapped_range.offset = 80;
    CHECK(vkInvalidateMappedMemoryRanges(device, 1, &mapped_range) == VK_ERROR_MEMORY_MAP_FAILED);
    mapped_range.offset = 16;
    mapped_range.size = 65;
    CHECK(vkFlushMappedMemoryRanges(device, 1, &mapped_range) == VK_ERROR_MEMORY_MAP_FAILED);
    mapped_range.size = VK_WHOLE_SIZE;
    vkUnmapMemory(device, mapped_range_memory);
    CHECK(vkFlushMappedMemoryRanges(device, 1, &mapped_range) == VK_ERROR_MEMORY_MAP_FAILED);
    CHECK(vkFlushMappedMemoryRanges(device, 0, &mapped_range) == VK_ERROR_INITIALIZATION_FAILED);
    vkFreeMemory(device, mapped_range_memory, NULL);

    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 66,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer src_buffer = 0, dst_buffer = 0;
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &src_buffer) == VK_SUCCESS && src_buffer != 0);
    CHECK(vkCreateBuffer(device, &buffer_info, NULL, &dst_buffer) == VK_SUCCESS && dst_buffer != 0);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, src_buffer, &requirements);
    CHECK(requirements.size == 66 && requirements.alignment == 16 && requirements.memoryTypeBits == 1);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = 0
    };
    VkDeviceMemory src_memory = 0, dst_memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &src_memory) == VK_SUCCESS && src_memory != 0);
    CHECK(vkAllocateMemory(device, &allocation, NULL, &dst_memory) == VK_SUCCESS && dst_memory != 0);
    CHECK(vkBindBufferMemory(device, src_buffer, src_memory, 0) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(device, dst_buffer, dst_memory, 0) == VK_SUCCESS);
    void *mapped = NULL;
    CHECK(vkMapMemory(device, src_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < 16; i++) ((uint32_t *)mapped)[i] = 0x4000u + i * 17u;
    vkUnmapMemory(device, src_memory);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    memset(mapped, 0, (size_t)requirements.size);
    vkUnmapMemory(device, dst_memory);

    VkCommandPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = 0
    };
    VkCommandPool pool = 0;
    CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool) == VK_SUCCESS && pool != 0);
    VkCommandBufferAllocateInfo command_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    VkCommandBuffer command_buffer = NULL;
    CHECK(vkAllocateCommandBuffers(device, &command_info, &command_buffer) == VK_SUCCESS && command_buffer != NULL);
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    uint32_t update_word = 0x12345678u;
    vkCmdUpdateBuffer(command_buffer, dst_buffer, 2, sizeof update_word, &update_word);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdUpdateBuffer(command_buffer, dst_buffer, 0, 3, &update_word);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    uint8_t out_of_range_data[68] = { 0 };
    vkCmdUpdateBuffer(command_buffer, dst_buffer, 0, sizeof out_of_range_data, out_of_range_data);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdFillBuffer(command_buffer, dst_buffer, 2, 4, 0xdeadbeefu);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdFillBuffer(command_buffer, dst_buffer, 0, 68, 0xdeadbeefu);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdFillBuffer(command_buffer, dst_buffer, 0, 0, 0xdeadbeefu);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    uint32_t recorded_data[16];
    for (uint32_t i = 0; i < 16; i++) recorded_data[i] = 0x8000u + i * 13u;
    vkCmdUpdateBuffer(command_buffer, src_buffer, 0, sizeof recorded_data, recorded_data);
    memset(recorded_data, 0, sizeof recorded_data);
    VkBufferMemoryBarrier update_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = src_buffer,
        .size = 64
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 1, &update_barrier, 0, NULL);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 0, NULL);
    VkBufferCopy copy = { .size = 64 };
    vkCmdCopyBuffer(command_buffer, src_buffer, dst_buffer, 1, &copy);
    vkCmdFillBuffer(command_buffer, dst_buffer, 48, VK_WHOLE_SIZE, 0xa1b2c3d4u);
    VkBufferMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst_buffer,
        .size = 64
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &host_barrier, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);

    VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = 0;
    CHECK(vkCreateFence(device, &fence_info, NULL, &fence) == VK_SUCCESS);
    CHECK(vkGetFenceStatus(device, fence) == VK_NOT_READY);
    CHECK(vkWaitForFences(device, 1, &fence, 1, 0) == VK_TIMEOUT);
    uint64_t wait_start = monotonic_time_ns();
    CHECK(wait_start && vkWaitForFences(device, 1, &fence, 1, 20000000ull) == VK_TIMEOUT);
    uint64_t wait_elapsed = monotonic_time_ns() - wait_start;
    CHECK(wait_elapsed >= 5000000ull);
    VkFenceCreateInfo signaled_fence_info = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT
    };
    VkFence signaled_fence = 0;
    CHECK(vkCreateFence(device, &signaled_fence_info, NULL, &signaled_fence) == VK_SUCCESS);
    VkFence any_fences[2] = { fence, signaled_fence };
    CHECK(vkWaitForFences(device, 2, any_fences, 0, 0) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 2, any_fences, 1, 0) == VK_TIMEOUT);
    vkDestroyFence(device, signaled_fence, NULL);
    delayed_fence_submit delayed_submit = { .queue = queue, .fence = fence, .result = VK_ERROR_UNKNOWN };
    pthread_t submit_thread;
    CHECK(pthread_create(&submit_thread, NULL, submit_empty_batch_after_delay, &delayed_submit) == 0);
    VkResult blocked_wait = vkWaitForFences(device, 1, &fence, 1, 2000000000ull);
    int thread_join = pthread_join(submit_thread, NULL);
    CHECK(blocked_wait == VK_SUCCESS && thread_join == 0 && delayed_submit.result == VK_SUCCESS);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS);
    VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore semaphore = 0;
    CHECK(vkCreateSemaphore(device, &semaphore_info, NULL, &semaphore) == VK_SUCCESS);
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &semaphore
    };
    CHECK(vkQueueSubmit(queue, 1, &submit, fence) == VK_SUCCESS);
    CHECK(vkGetFenceStatus(device, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkQueueWaitIdle(queue) == VK_SUCCESS && vkDeviceWaitIdle(device) == VK_SUCCESS);
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo wait_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &semaphore,
        .pWaitDstStageMask = &wait_stage
    };
    CHECK(vkQueueSubmit(queue, 1, &wait_submit, 0) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &wait_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < 12; i++) CHECK(((uint32_t *)mapped)[i] == 0x8000u + i * 13u);
    for (uint32_t i = 12; i < 16; i++) CHECK(((uint32_t *)mapped)[i] == 0xa1b2c3d4u);
    CHECK(((uint8_t *)mapped)[64] == 0 && ((uint8_t *)mapped)[65] == 0);
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkGetFenceStatus(device, fence) == VK_NOT_READY);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 4, 3, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VkImage image = 0;
    CHECK(vkCreateImage(device, &image_info, NULL, &image) == VK_SUCCESS && image != 0);
    VkImageViewCreateInfo image_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = image_info.format,
        .components = { VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
                        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkImageView image_view = 0;
    CHECK(vkCreateImageView(device, &image_view_info, NULL, &image_view) == VK_SUCCESS && image_view != 0);
    VkImageViewCreateInfo unsupported_image_view_info = image_view_info;
    unsupported_image_view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    VkImageView unsupported_image_view = UINT64_MAX;
    CHECK(vkCreateImageView(device, &unsupported_image_view_info, NULL, &unsupported_image_view) ==
          VK_ERROR_FORMAT_NOT_SUPPORTED && unsupported_image_view == 0);
    unsupported_image_view_info = image_view_info;
    unsupported_image_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    CHECK(vkCreateImageView(device, &unsupported_image_view_info, NULL, &unsupported_image_view) ==
          VK_ERROR_FORMAT_NOT_SUPPORTED && unsupported_image_view == 0);
    VkImageCreateInfo unsupported_image_info = image_info;
    unsupported_image_info.tiling = (VkImageTiling)UINT32_MAX;
    VkImage unsupported_image = UINT64_MAX;
    CHECK(vkCreateImage(device, &unsupported_image_info, NULL, &unsupported_image) ==
          VK_ERROR_FORMAT_NOT_SUPPORTED && unsupported_image == 0);
    VkMemoryRequirements image_requirements;
    vkGetImageMemoryRequirements(device, image, &image_requirements);
    CHECK(image_requirements.size >= 4u * 3u * 4u && image_requirements.alignment >= 16 &&
          (image_requirements.alignment & (image_requirements.alignment - 1)) == 0 &&
          image_requirements.memoryTypeBits == 1);
    VkImageSubresource color_subresource = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .arrayLayer = 0
    };
    VkSubresourceLayout image_layout;
    vkGetImageSubresourceLayout(device, image, &color_subresource, &image_layout);
    CHECK(image_layout.offset == 0 && image_layout.rowPitch >= 16 && image_layout.rowPitch % image_requirements.alignment == 0 &&
          image_layout.size == image_layout.rowPitch * 3 && image_layout.arrayPitch == image_layout.size &&
          image_layout.depthPitch == image_layout.size);
    allocation.allocationSize = image_requirements.size;
    VkDeviceMemory image_memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &image_memory) == VK_SUCCESS && image_memory != 0);
    CHECK(vkBindImageMemory(device, image, image_memory, 0) == VK_SUCCESS);
    CHECK(vkBindImageMemory(device, image, image_memory, 0) == VK_ERROR_INITIALIZATION_FAILED);

    VkImage copy_image = 0;
    CHECK(vkCreateImage(device, &image_info, NULL, &copy_image) == VK_SUCCESS && copy_image != 0);
    VkMemoryRequirements copy_image_requirements;
    vkGetImageMemoryRequirements(device, copy_image, &copy_image_requirements);
    allocation.allocationSize = copy_image_requirements.size;
    VkDeviceMemory copy_image_memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &copy_image_memory) == VK_SUCCESS && copy_image_memory != 0);
    CHECK(vkBindImageMemory(device, copy_image, copy_image_memory, 0) == VK_SUCCESS);
    CHECK(vkMapMemory(device, copy_image_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    memset(mapped, 0, copy_image_requirements.size);
    vkUnmapMemory(device, copy_image_memory);

    CHECK(vkMapMemory(device, src_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t i = 0; i < 48; i++) ((uint8_t *)mapped)[i] = (uint8_t)(i * 3u + 7u);
    vkUnmapMemory(device, src_memory);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    memset(mapped, 0, 64);
    vkUnmapMemory(device, dst_memory);

    VkImageMemoryBarrier image_to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
    };
    VkImageMemoryBarrier image_to_src = image_to_dst;
    image_to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    image_to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image_to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkImageMemoryBarrier copy_image_to_dst = image_to_dst;
    copy_image_to_dst.image = copy_image;
    VkImageMemoryBarrier copy_image_to_src = image_to_src;
    copy_image_to_src.image = copy_image;
    VkBufferImageCopy image_copy = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { 4, 3, 1 }
    };
    VkImageCopy image_to_image = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffset = { 1, 1, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset = { 2, 0, 0 },
        .extent = { 2, 2, 1 }
    };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &image_to_dst);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &copy_image_to_dst);
    vkCmdCopyBufferToImage(command_buffer, src_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &image_to_src);
    vkCmdCopyImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        copy_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &image_to_image);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &copy_image_to_src);
    vkCmdCopyImageToBuffer(command_buffer, copy_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_buffer, 1, &image_copy);
    VkBufferMemoryBarrier copy_to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst_buffer,
        .size = 64
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &copy_to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    VkSubmitInfo image_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, image_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    uint8_t *image_bytes = mapped;
    CHECK(image_layout.rowPitch >= 16);
    for (uint32_t y = 0; y < 3; y++)
        for (uint32_t x = 0; x < 16; x++) CHECK(image_bytes[y * image_layout.rowPitch + x] == (uint8_t)((y * 16 + x) * 3u + 7u));
    vkUnmapMemory(device, image_memory);
    CHECK(vkMapMemory(device, copy_image_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    uint8_t *copy_image_bytes = mapped;
    for (uint32_t y = 0; y < 3; y++) {
        for (uint32_t x = 0; x < 16; x++) {
            uint8_t expected = 0;
            if (y < 2 && x >= 8) expected = (uint8_t)(((y + 1u) * 16u + x - 4u) * 3u + 7u);
            CHECK(copy_image_bytes[y * image_layout.rowPitch + x] == expected);
        }
    }
    vkUnmapMemory(device, copy_image_memory);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    for (uint32_t y = 0; y < 3; y++) {
        for (uint32_t x = 0; x < 16; x++) {
            uint8_t expected = 0;
            if (y < 2 && x >= 8) expected = (uint8_t)(((y + 1u) * 16u + x - 4u) * 3u + 7u);
            CHECK(((uint8_t *)mapped)[y * 16u + x] == expected);
        }
    }
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageMemoryBarrier clear_image_to_dst = image_to_dst;
    clear_image_to_dst.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    clear_image_to_dst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkClearColorValue clear_color = { .float32 = { 1.0f, 0.5f, 0.25f, 1.0f } };
    VkImageSubresourceRange clear_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkBufferMemoryBarrier clear_readback_to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = dst_buffer,
        .size = 64
    };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &clear_image_to_dst);
    vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1,
                         &clear_range);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &image_to_src);
    vkCmdCopyImageToBuffer(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst_buffer, 1, &image_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &clear_readback_to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped != NULL);
    const uint8_t expected_clear[4] = { 255, 128, 64, 255 };
    for (uint32_t i = 0; i < 12; i++)
        CHECK(memcmp((const uint8_t *)mapped + i * sizeof expected_clear, expected_clear, sizeof expected_clear) == 0);
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageMemoryBarrier stale_transition = clear_image_to_dst;
    stale_transition.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &stale_transition);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkGetFenceStatus(device, fence) == VK_NOT_READY);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1,
                         &clear_range);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkGetFenceStatus(device, fence) == VK_NOT_READY);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageSubresourceRange invalid_clear_range = clear_range;
    invalid_clear_range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1,
                         &invalid_clear_range);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &clear_color, 1,
                         &clear_range);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdCopyImage(command_buffer, image, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_GENERAL,
                   1, &image_to_image);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageCopy nonoverlapping_self_copy = image_to_image;
    nonoverlapping_self_copy.srcOffset = (VkOffset3D){ 0, 0, 0 };
    nonoverlapping_self_copy.dstOffset = (VkOffset3D){ 1, 0, 0 };
    nonoverlapping_self_copy.extent = (VkExtent3D){ 1, 1, 1 };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdCopyImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &nonoverlapping_self_copy);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageCopy repeated_destination_regions[2] = { image_to_image, image_to_image };
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdCopyImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        copy_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, repeated_destination_regions);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    VkImageCreateInfo optimal_image_info = image_info;
    optimal_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    VkImage optimal_image = 0;
    CHECK(vkCreateImage(device, &optimal_image_info, NULL, &optimal_image) == VK_SUCCESS && optimal_image);
    VkMemoryRequirements optimal_requirements;
    vkGetImageMemoryRequirements(device, optimal_image, &optimal_requirements);
    allocation.allocationSize = optimal_requirements.size;
    VkDeviceMemory optimal_memory = 0;
    CHECK(vkAllocateMemory(device, &allocation, NULL, &optimal_memory) == VK_SUCCESS && optimal_memory);
    CHECK(vkBindImageMemory(device, optimal_image, optimal_memory, 0) == VK_SUCCESS);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    memset(mapped, 0, 64);
    vkUnmapMemory(device, dst_memory);
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    VkImageMemoryBarrier optimal_to_dst = image_to_dst;
    optimal_to_dst.image = optimal_image;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &optimal_to_dst);
    vkCmdCopyBufferToImage(command_buffer, src_buffer, optimal_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &image_copy);
    VkImageMemoryBarrier optimal_to_src = image_to_src;
    optimal_to_src.image = optimal_image;
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &optimal_to_src);
    vkCmdCopyImageToBuffer(command_buffer, optimal_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        dst_buffer, 1, &image_copy);
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &copy_to_host, 0, NULL);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    CHECK(vkQueueSubmit(queue, 1, &image_submit, fence) == VK_SUCCESS);
    CHECK(vkWaitForFences(device, 1, &fence, 1, UINT64_MAX) == VK_SUCCESS);
    CHECK(vkMapMemory(device, dst_memory, 0, UINT64_MAX, 0, &mapped) == VK_SUCCESS && mapped);
    for (uint32_t i = 0; i < 48; i++) CHECK(((uint8_t *)mapped)[i] == (uint8_t)(i * 3u + 7u));
    vkUnmapMemory(device, dst_memory);
    CHECK(vkResetFences(device, 1, &fence) == VK_SUCCESS && vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    vkDestroyImage(device, optimal_image, NULL);
    vkFreeMemory(device, optimal_memory, NULL);

    if (properties.limits.maxPerStageDescriptorStorageImages) {
        VkImageCreateInfo storage_image_info = image_info;
        storage_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        storage_image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        VkImage storage_image = 0;
        CHECK(vkCreateImage(device, &storage_image_info, NULL, &storage_image) == VK_SUCCESS && storage_image);
        VkMemoryRequirements storage_requirements;
        vkGetImageMemoryRequirements(device, storage_image, &storage_requirements);
        allocation.allocationSize = storage_requirements.size;
        VkDeviceMemory storage_memory = 0;
        CHECK(vkAllocateMemory(device, &allocation, NULL, &storage_memory) == VK_SUCCESS && storage_memory);
        CHECK(vkBindImageMemory(device, storage_image, storage_memory, 0) == VK_SUCCESS);
        VkImageViewCreateInfo storage_view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = storage_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = storage_image_info.format,
            .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        VkImageView storage_view = 0;
        CHECK(vkCreateImageView(device, &storage_view_info, NULL, &storage_view) == VK_SUCCESS && storage_view);
        VkDescriptorSetLayoutBinding storage_binding = {
            .binding = 3,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
        };
        VkDescriptorSetLayoutCreateInfo storage_set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &storage_binding
        };
        VkDescriptorSetLayout storage_set_layout = 0;
        CHECK(vkCreateDescriptorSetLayout(device, &storage_set_info, NULL, &storage_set_layout) == VK_SUCCESS &&
              storage_set_layout);
        VkPipelineLayoutCreateInfo storage_pipeline_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &storage_set_layout
        };
        VkPipelineLayout storage_pipeline_layout = 0;
        CHECK(vkCreatePipelineLayout(device, &storage_pipeline_info, NULL, &storage_pipeline_layout) == VK_SUCCESS &&
              storage_pipeline_layout);
        VkDescriptorPoolSize storage_pool_size = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 };
        VkDescriptorPoolCreateInfo storage_pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 2,
            .poolSizeCount = 1,
            .pPoolSizes = &storage_pool_size
        };
        VkDescriptorPool storage_pool = 0;
        CHECK(vkCreateDescriptorPool(device, &storage_pool_info, NULL, &storage_pool) == VK_SUCCESS && storage_pool);
        VkDescriptorSetAllocateInfo storage_allocate_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = storage_pool,
            .descriptorSetCount = 2,
            .pSetLayouts = (VkDescriptorSet[2]){ storage_set_layout, storage_set_layout }
        };
        VkDescriptorSet storage_sets[2] = { 0 };
        CHECK(vkAllocateDescriptorSets(device, &storage_allocate_info, storage_sets) == VK_SUCCESS &&
              storage_sets[0] && storage_sets[1]);
        VkDescriptorImageInfo storage_descriptor_image = {
            .imageView = storage_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL
        };
        VkWriteDescriptorSet storage_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = storage_sets[0],
            .dstBinding = 3,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &storage_descriptor_image
        };
        vkUpdateDescriptorSets(device, 1, &storage_write, 0, NULL);
        VkCopyDescriptorSet storage_copy = {
            .sType = VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,
            .srcSet = storage_sets[0],
            .srcBinding = 3,
            .dstSet = storage_sets[1],
            .dstBinding = 3,
            .descriptorCount = 1
        };
        vkUpdateDescriptorSets(device, 0, NULL, 1, &storage_copy);
        vkDestroyImageView(device, storage_view, NULL);
        vkDestroyImage(device, storage_image, NULL);
        vkDestroyDescriptorPool(device, storage_pool, NULL);
        vkDestroyPipelineLayout(device, storage_pipeline_layout, NULL);
        vkDestroyDescriptorSetLayout(device, storage_set_layout, NULL);
        vkFreeMemory(device, storage_memory, NULL);
    }

    vkDestroyImage(device, copy_image, NULL);
    vkFreeMemory(device, copy_image_memory, NULL);

    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    vkCmdClearColorImage(command_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1,
                         &clear_range);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyImageView(device, image_view, NULL);
    vkDestroyImage(device, image, NULL);
    image = 0;
    VkSubmitInfo invalid_image_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_image_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);
    vkFreeMemory(device, image_memory, NULL);

    const VkFormat transfer_formats[] = {
        VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT
    };
    for (size_t i = 0; i < sizeof transfer_formats / sizeof transfer_formats[0]; i++) {
        VkImageCreateInfo format_info = image_info;
        format_info.format = transfer_formats[i];
        VkImage format_image = 0;
        CHECK(vkCreateImage(device, &format_info, NULL, &format_image) == VK_SUCCESS && format_image != 0);
        VkMemoryRequirements format_requirements;
        vkGetImageMemoryRequirements(device, format_image, &format_requirements);
        allocation.allocationSize = format_requirements.size;
        VkDeviceMemory format_memory = 0;
        CHECK(vkAllocateMemory(device, &allocation, NULL, &format_memory) == VK_SUCCESS && format_memory != 0);
        CHECK(vkBindImageMemory(device, format_image, format_memory, 0) == VK_SUCCESS);
        vkDestroyImage(device, format_image, NULL);
        vkFreeMemory(device, format_memory, NULL);
    }
    CHECK(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS);
    uint32_t recorded_value = 0x1234abcdu;
    vkCmdUpdateBuffer(command_buffer, src_buffer, 0, sizeof recorded_value, &recorded_value);
    CHECK(vkEndCommandBuffer(command_buffer) == VK_SUCCESS);
    vkDestroyBuffer(device, src_buffer, NULL);
    src_buffer = 0;
    VkSubmitInfo invalid_buffer_submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer
    };
    CHECK(vkQueueSubmit(queue, 1, &invalid_buffer_submit, 0) == VK_ERROR_INITIALIZATION_FAILED);
    CHECK(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS);

    if (argc >= 2) CHECK(run_compute(device, queue, command_buffer, fence, argv[1]) == 0);
    if (argc >= 3) {
        CHECK(run_descriptor_array(device, queue, command_buffer, fence, argv[2],
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) == 0);
        CHECK(run_descriptor_array(device, queue, command_buffer, fence, argv[2],
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) == 0);
    }
    if (argc >= 4) CHECK(run_uniform_buffer(device, queue, command_buffer, fence, argv[3]) == 0);
    if (argc >= 5) CHECK(run_push_constants(device, queue, command_buffer, fence, argv[4]) == 0);
    if (argc >= 6) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[5], 0) == 0);
    if (argc >= 7) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[6], 1) == 0);
    if (argc >= 8) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[7], 2) == 0);
    if (argc >= 9) CHECK(run_storage_image_copy(device, queue, command_buffer, fence, argv[8], 3) == 0);
    puts(properties.limits.maxPerStageDescriptorStorageImages ?
         "vulkan descriptors: optimal RGBA8 storage-image write/copy and retained lifetime: ok" :
         "vulkan descriptors: storage-image path unavailable on this Metal device: ok");
    puts("vulkan transfer: buffer/image alias, image views, linear and opaque optimal layouts, image barriers/layout validation, clears and roundtrips: ok");
    vkDestroySemaphore(device, semaphore, NULL);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, pool, NULL);
    vkDestroyBuffer(device, src_buffer, NULL);
    vkDestroyBuffer(device, dst_buffer, NULL);
    vkFreeMemory(device, src_memory, NULL);
    vkFreeMemory(device, dst_memory, NULL);
    vkDestroyDevice(device, NULL);

    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance, "vkDestroyInstance");
    destroy(instance, &allocator);
    CHECK(allocations == 1 && frees == 1);

    const char *unsupported_extension[] = { "VK_EXT_not_implemented" };
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = unsupported_extension;
    instance = (VkInstance)(uintptr_t)1;
    CHECK(vkCreateInstance(&info, NULL, &instance) == VK_ERROR_EXTENSION_NOT_PRESENT);
    CHECK(instance == NULL);

    const char *unsupported_layer[] = { "VK_LAYER_not_implemented" };
    info.enabledExtensionCount = 0;
    info.ppEnabledExtensionNames = NULL;
    info.enabledLayerCount = 1;
    info.ppEnabledLayerNames = unsupported_layer;
    CHECK(vkCreateInstance(&info, NULL, &instance) == VK_ERROR_LAYER_NOT_PRESENT);

    info.enabledLayerCount = 0;
    info.ppEnabledLayerNames = NULL;
    app.apiVersion = (1u << 22) | (1u << 12);
    CHECK(vkCreateInstance(&info, NULL, &instance) == VK_ERROR_INCOMPATIBLE_DRIVER);
    app.apiVersion = VK_API_VERSION_1_0;
    info.flags = 1;
    CHECK(vkCreateInstance(&info, NULL, &instance) == VK_ERROR_INITIALIZATION_FAILED);
    return 0;
}
